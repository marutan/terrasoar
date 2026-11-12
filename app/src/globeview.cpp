// SPDX-License-Identifier: GPL-2.0-or-later
#include "globeview.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include <QFocusEvent>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QPainter>
#include <QScreen>
#include <QStringList>
#include <QSurfaceFormat>
#include <QWheelEvent>

#include "ts/camera/scalebar.h"
#include "ts/geo/geodetic.h"
#include "ts/geo/geoformat.h"
#include "ts/globe/globerenderer.h"
#include "ts/globe/tilerenderer.h"
#include "ts/projection/projection.h"
#include "ts/providers/tileprovider.h"
#include "ts/render/glrenderdevice.h"

namespace ts
{

namespace
{

/** Range factor for a double click: fly three times closer. */
constexpr double doubleClickZoom = 1.0 / 3.0;

/** The change between globe and map on a software renderer, seconds:
 *  shorter, since every frame of it is expensive there. */
constexpr double softwareMorphDurationS = 0.5;

/** Longest scale bar, logical pixels. */
constexpr double scaleBarMaximumWidthPx = 140.0;

/** Wheel angle of one notch, in eighths of a degree (Qt's unit). */
constexpr double wheelNotch = 120.0;

QPointF eventPosition(const QMouseEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  return event->position();
#else
  return event->localPos();
#endif
}

/** Returns the CameraMotion flag a key drives, or NoMotion. */
unsigned int keyMotion(int key)
{
  switch (key) {
  case Qt::Key_Up:
  case Qt::Key_W:
    return MoveForward;
  case Qt::Key_Down:
  case Qt::Key_S:
    return MoveBackward;
  case Qt::Key_Left:
  case Qt::Key_A:
    return MoveLeft;
  case Qt::Key_Right:
  case Qt::Key_D:
    return MoveRight;
  case Qt::Key_PageUp:
  case Qt::Key_Plus:
  case Qt::Key_Equal:
    return ZoomIn;
  case Qt::Key_PageDown:
  case Qt::Key_Minus:
    return ZoomOut;
  case Qt::Key_Q:
    return TurnLeft;
  case Qt::Key_E:
    return TurnRight;
  case Qt::Key_R:
    return TiltUp;
  case Qt::Key_F:
    return TiltDown;
  default:
    return NoMotion;
  }
}

} // namespace

GlobeView::GlobeView(QWidget* parent) : QOpenGLWidget(parent)
{
  setObjectName(QStringLiteral("globeView"));
  // Qt 5 needs the type registered by name for queued connections and
  // QSignalSpy.
  qRegisterMetaType<ts::RegionGeometry>("ts::RegionGeometry");
  QSurfaceFormat format = QSurfaceFormat::defaultFormat();
  format.setRenderableType(QSurfaceFormat::OpenGL);
  format.setVersion(3, 3);
  format.setProfile(QSurfaceFormat::CoreProfile);
  format.setDepthBufferSize(24);
  // The globe, graticule and markers are antialiased in their shaders, so
  // multisampling isn't needed yet (it would cost software renderers most).
  format.setSamples(0);
  format.setSwapInterval(1);
  setFormat(format);

  setFocusPolicy(Qt::StrongFocus);
  setMouseTracking(true);
  setMinimumSize(64, 64);

  m_clock.start();
  m_frameTimer.setSingleShot(true);
  m_frameTimer.setTimerType(Qt::PreciseTimer);
  connect(&m_frameTimer, &QTimer::timeout, this, &GlobeView::onFrameTimer);

  m_controller.setPose(homePose());
}

GlobeView::~GlobeView()
{
  // ~QOpenGLWidget destroys the context after this destructor; its
  // aboutToBeDestroyed must not call back into the half-destroyed view.
  disconnect(m_contextConnection);
  releaseGl();
}

CameraPose GlobeView::homePose()
{
  return CameraPose{20.0, 10.0, 2.0e7, 0.0, 0.0};
}

bool GlobeView::isOpenGlAvailable()
{
  // These platforms may create bare contexts (offscreen does through GLX
  // when a display is set) but can't compose a QOpenGLWidget into a
  // window; Qt 6.4 crashes trying. Qt checks that capability only through
  // private API.
  const QString platform = QGuiApplication::platformName();
  if (platform == QLatin1String("offscreen") ||
      platform == QLatin1String("minimal")) {
    return false;
  }
  if (QOpenGLContext::globalShareContext() != nullptr) {
    return true;
  }
  QOpenGLContext context;
  return context.create();
}

double GlobeView::nowS() const
{
  return static_cast<double>(m_clock.nsecsElapsed()) * 1e-9;
}

void GlobeView::setDebugOverlayVisible(bool visible)
{
  if (m_overlayVisible != visible) {
    m_overlayVisible = visible;
    update();
  }
}

void GlobeView::goHome()
{
  flyTo(homePose(), 0.0);
}

void GlobeView::setImageryDraws(const ImageryDraws& draws)
{
  m_draws = draws;
  if (m_globe) {
    makeCurrent();
    m_globe->setImageryLayers(m_draws.layers, m_draws.groupAlphas);
    doneCurrent();
  }
  update();
}

void GlobeView::setVectorDraws(const std::vector<VectorLayerDraw>& draws)
{
  m_vectorDraws = draws;
  if (m_globe) {
    makeCurrent();
    m_globe->setVectorLayers(m_vectorDraws);
    doneCurrent();
  }
  update();
}

void GlobeView::setAttributions(const QStringList& attributions)
{
  if (m_attributions != attributions) {
    m_attributions = attributions;
    update();
  }
}

TileStatistics GlobeView::tileStatistics() const
{
  return m_globe ? m_globe->tileStatistics() : TileStatistics();
}

VectorStatistics GlobeView::vectorStatistics() const
{
  return m_globe ? m_globe->vectorStatistics() : VectorStatistics();
}

bool GlobeView::isGroupCompositingAvailable() const
{
  return !m_deviceInfo.softwareRenderer;
}

void GlobeView::setViewMode(ViewMode mode, bool animate)
{
  if (mode == m_controller.viewMode()) {
    return;
  }
  m_controller.setViewMode(mode, animate ? morphDurationS() : 0.0);
  emit viewModeChanged(mode);
  requestFrame();
  update();
}

void GlobeView::setMapProjection(const Projection* projection)
{
  if (projection == nullptr || projection == mapProjection()) {
    return;
  }
  m_controller.setMapProjection(projection);
  emit mapProjectionChanged(projection);
  update();
}

const Projection* GlobeView::mapProjection() const
{
  return m_controller.camera().mapProjection();
}

double GlobeView::morphDurationS() const
{
  return m_deviceInfo.softwareRenderer
             ? softwareMorphDurationS
             : CameraController::defaultMorphDurationS;
}

void GlobeView::setScaleBarVisible(bool visible)
{
  if (m_scaleBarVisible != visible) {
    m_scaleBarVisible = visible;
    update();
  }
}

void GlobeView::setPose(const CameraPose& pose)
{
  m_controller.setPose(pose);
  update();
}

bool GlobeView::isLoadingTiles() const
{
  return m_globe && m_globe->isBusy();
}

void GlobeView::flyTo(const CameraPose& pose, double durationS)
{
  m_controller.flyTo(pose, durationS);
  requestFrame();
}

// --- OpenGL
// -------------------------------------------------------------------

void GlobeView::initializeGL()
{
  // The context is replaced when the widget moves to another window; free
  // everything made on the old one first.
  disconnect(m_contextConnection);
  m_contextConnection = connect(context(), &QOpenGLContext::aboutToBeDestroyed,
                                this, &GlobeView::releaseGl);
  m_error.clear();
  m_device = GlRenderDevice::create(context(), &m_error);
  if (!m_device) {
    qWarning("Globe view: %s", qPrintable(m_error));
    return;
  }
  m_deviceInfo = m_device->info();
  m_globe = std::make_unique<GlobeRenderer>(*m_device);
  if (!m_globe->initialise(&m_error)) {
    qWarning("Globe view: %s", qPrintable(m_error));
    m_globe.reset();
    m_device.reset();
    return;
  }
  // Called on a loader thread: hand over to the GUI thread. With the view
  // as context, the call is dropped if the view is gone by then (the
  // loader is stopped before that anyway, in releaseGl()).
  m_globe->setTilesReadyCallback([this]() {
    QMetaObject::invokeMethod(
        this, [this]() { requestFrame(); }, Qt::QueuedConnection);
  });
  // Compositing a group costs a full-screen buffer and pass; software
  // rasterisers multiply the group's opacity into its layers instead.
  m_globe->setGroupCompositing(isGroupCompositingAvailable());
  m_globe->setImageryLayers(m_draws.layers, m_draws.groupAlphas);
  // The graticule is a layer in the tree, drawn in its order.
  GlobeStyle style = m_globe->style();
  style.graticule = false;
  m_globe->setStyle(style);
  m_globe->setVectorLayers(m_vectorDraws);
}

void GlobeView::releaseGl()
{
  if (!m_device && !m_globe) {
    return;
  }
  makeCurrent();
  m_globe.reset();
  m_device.reset();
  doneCurrent();
}

void GlobeView::resizeGL(int widthPx, int heightPx)
{
  // QOpenGLWidget passes logical (device-independent) pixels.
  m_controller.setViewportSize(std::max(widthPx, 1), std::max(heightPx, 1));
}

void GlobeView::paintGL()
{
  const double startS = nowS();
  if (m_animationClockRunning && m_lastFrameStartS >= 0.0) {
    m_intervalStats.addFrame(startS - m_lastFrameStartS);
  }
  m_lastFrameStartS = startS;
  m_pacer.frameStarted(startS);

  if (!m_globe) {
    drawError();
    return;
  }
  const double ratio = devicePixelRatioF();
  const QSize devicePx(qRound(width() * ratio), qRound(height() * ratio));
  m_device->setDefaultFramebuffer(defaultFramebufferObject(), devicePx);
  PassDesc pass;
  pass.viewportPx = QRect(QPoint(0, 0), devicePx);
  m_globe->render(m_controller.camera(), pass, ratio);
  m_cpuFrameStats.addFrame(nowS() - startS);
  ++m_framesRendered;

  drawAttributions();
  drawScaleBar();
  drawRegions();
  if (m_overlayVisible) {
    drawOverlay();
  }
  emit cameraChanged(m_controller.camera().eyeGeodetic().heightM);
  updatePointerReadout();
  if (m_controller.isAnimating() || m_globe->isBusy()) {
    requestFrame();
  }
}

// --- Frame scheduling
// ---------------------------------------------------------

void GlobeView::requestFrame()
{
  const double now = nowS();
  if (m_controller.isAnimating() && !m_animationClockRunning) {
    m_lastAdvanceS = now;
    m_animationClockRunning = true;
  }
  if (const QScreen* s = screen()) {
    m_pacer.setRefreshRateHz(s->refreshRate());
  }
  if (!m_frameTimer.isActive()) {
    m_frameTimer.start(
        static_cast<int>(std::ceil(m_pacer.delayS(now) * 1000.0)));
  }
}

void GlobeView::onFrameTimer()
{
  if (m_animationClockRunning) {
    const double now = nowS();
    m_controller.advance(now - m_lastAdvanceS);
    m_lastAdvanceS = now;
    if (!m_controller.isAnimating()) {
      m_animationClockRunning = false;
    }
  }
  update();
}

// --- Input
// --------------------------------------------------------------------

void GlobeView::mousePressEvent(QMouseEvent* event)
{
  setFocus(Qt::MouseFocusReason);
  const QPointF position = eventPosition(event);
  if (m_drawing != RegionDrawing::None && event->button() == Qt::LeftButton &&
      (event->modifiers() & Qt::ControlModifier) == 0) {
    const std::optional<LonLat> point = pickLonLat(position);
    if (point) {
      if (m_drawing == RegionDrawing::Rectangle) {
        m_drawnPoints = {*point};
        m_drawingDrag = true;
      } else {
        m_drawnPoints.push_back(*point);
      }
    }
    m_pointerPx = position;
    update();
    event->accept();
    return;
  }
  const bool rotate = event->button() == Qt::RightButton ||
                      event->button() == Qt::MiddleButton ||
                      (event->button() == Qt::LeftButton &&
                       (event->modifiers() & Qt::ControlModifier) != 0);
  if (rotate) {
    m_controller.beginRotate(position);
  } else if (event->button() == Qt::LeftButton) {
    m_controller.beginPan(position, nowS());
  }
  requestFrame();
  event->accept();
}

void GlobeView::mouseMoveEvent(QMouseEvent* event)
{
  m_pointerInside = true;
  m_pointerPx = eventPosition(event);
  if (m_drawing != RegionDrawing::None && !m_drawnPoints.empty()) {
    update(); // the rubber band follows the pointer
  }
  if (m_controller.isDragging()) {
    m_controller.dragTo(m_pointerPx, nowS());
    requestFrame();
  } else {
    updatePointerReadout();
  }
  event->accept();
}

void GlobeView::mouseReleaseEvent(QMouseEvent* event)
{
  if (m_drawing == RegionDrawing::Rectangle && m_drawingDrag &&
      event->button() == Qt::LeftButton) {
    m_drawingDrag = false;
    const std::optional<LonLat> picked = pickLonLat(eventPosition(event));
    if (picked && !m_drawnPoints.empty()) {
      const LonLat start = m_drawnPoints.front();
      const LonLat end = *picked;
      // The shorter way round between the two corners.
      double delta = std::fmod(end.lonDeg - start.lonDeg, 360.0);
      if (delta > 180.0) {
        delta -= 360.0;
      } else if (delta < -180.0) {
        delta += 360.0;
      }
      const double west = delta >= 0.0 ? start.lonDeg : end.lonDeg;
      const RegionGeometry rectangle = RegionGeometry::rectangle(
          west, std::min(start.latDeg, end.latDeg), west + std::abs(delta),
          std::max(start.latDeg, end.latDeg));
      if (rectangle.isValid()) {
        m_drawing = RegionDrawing::None;
        m_drawnPoints.clear();
        update();
        emit regionDrawn(rectangle);
        event->accept();
        return;
      }
    }
    m_drawnPoints.clear();
    update();
    event->accept();
    return;
  }
  if (m_controller.isDragging() && event->buttons() == Qt::NoButton) {
    m_controller.endDrag(nowS());
    requestFrame();
  }
  event->accept();
}

void GlobeView::mouseDoubleClickEvent(QMouseEvent* event)
{
  if (event->button() != Qt::LeftButton) {
    QOpenGLWidget::mouseDoubleClickEvent(event);
    return;
  }
  if (m_drawing == RegionDrawing::Polygon) {
    // The first click of the two added the last corner.
    finishPolygon();
    event->accept();
    return;
  }
  if (m_drawing != RegionDrawing::None) {
    event->accept();
    return;
  }
  const QPointF position = eventPosition(event);
  const std::optional<Geodetic> hit =
      m_controller.camera().pickGeodetic(position.x(), position.y());
  if (hit) {
    flyTo(m_controller.poseLookingAt(*hit, m_controller.camera().pose().rangeM *
                                               doubleClickZoom),
          0.0);
  }
  event->accept();
}

void GlobeView::wheelEvent(QWheelEvent* event)
{
  const double steps = event->angleDelta().y() / wheelNotch;
  if (steps != 0.0) {
    m_controller.zoomAt(event->position(), steps);
    requestFrame();
  }
  event->accept();
}

void GlobeView::keyPressEvent(QKeyEvent* event)
{
  if (m_drawing != RegionDrawing::None) {
    if (event->key() == Qt::Key_Escape) {
      startRegionDrawing(RegionDrawing::None);
      event->accept();
      return;
    }
    if (m_drawing == RegionDrawing::Polygon &&
        (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)) {
      finishPolygon();
      event->accept();
      return;
    }
    if (m_drawing == RegionDrawing::Polygon &&
        event->key() == Qt::Key_Backspace) {
      if (!m_drawnPoints.empty()) {
        m_drawnPoints.pop_back();
        update();
      }
      event->accept();
      return;
    }
  }
  const unsigned int motion = keyMotion(event->key());
  if (motion != NoMotion) {
    if (!event->isAutoRepeat()) {
      m_heldKeys |= motion;
      m_controller.setMotion(m_heldKeys);
      requestFrame();
    }
    event->accept();
    return;
  }
  if (event->key() == Qt::Key_Home) {
    goHome();
    event->accept();
    return;
  }
  QOpenGLWidget::keyPressEvent(event);
}

void GlobeView::keyReleaseEvent(QKeyEvent* event)
{
  const unsigned int motion = keyMotion(event->key());
  if (motion != NoMotion) {
    if (!event->isAutoRepeat()) {
      m_heldKeys &= ~motion;
      m_controller.setMotion(m_heldKeys);
    }
    event->accept();
    return;
  }
  QOpenGLWidget::keyReleaseEvent(event);
}

void GlobeView::focusOutEvent(QFocusEvent* event)
{
  m_heldKeys = NoMotion;
  m_controller.setMotion(NoMotion);
  QOpenGLWidget::focusOutEvent(event);
}

void GlobeView::leaveEvent(QEvent* event)
{
  m_pointerInside = false;
  emit pointerPositionChanged(false, 0.0, 0.0);
  QOpenGLWidget::leaveEvent(event);
}

// --- Offline regions
// ------------------------------------------------------------

void GlobeView::startRegionDrawing(RegionDrawing mode)
{
  const bool wasDrawing = m_drawing != RegionDrawing::None;
  m_drawing = mode;
  m_drawnPoints.clear();
  m_drawingDrag = false;
  if (mode != RegionDrawing::None) {
    setFocus(Qt::OtherFocusReason);
  }
  update();
  if (wasDrawing && mode == RegionDrawing::None) {
    emit regionDrawingCancelled();
  }
}

void GlobeView::finishPolygon()
{
  QString error;
  std::optional<RegionGeometry> polygon =
      RegionGeometry::polygon(m_drawnPoints, &error);
  if (!polygon) {
    return; // too few corners yet: carry on drawing
  }
  m_drawing = RegionDrawing::None;
  m_drawnPoints.clear();
  update();
  emit regionDrawn(*polygon);
}

void GlobeView::setRegionOutlines(const std::vector<RegionGeometry>& regions)
{
  m_outlines = regions;
  update();
}

std::optional<LonLat> GlobeView::pickLonLat(QPointF positionPx) const
{
  const std::optional<Geodetic> hit =
      m_controller.camera().pickGeodetic(positionPx.x(), positionPx.y());
  if (!hit) {
    return std::nullopt;
  }
  return LonLat{hit->lonDeg, hit->latDeg};
}

std::optional<RegionGeometry> GlobeView::visibleArea() const
{
  // Sample the view on a grid; longitudes relative to the first hit so
  // that a view across the antimeridian stays in one piece.
  const int steps = 16;
  bool any = false;
  double centreLon = 0.0;
  GeoRect box{0.0, 0.0, 0.0, 0.0};
  for (int row = 0; row <= steps; ++row) {
    for (int column = 0; column <= steps; ++column) {
      const std::optional<LonLat> hit = pickLonLat(QPointF(
          width() * column / double(steps), height() * row / double(steps)));
      if (!hit) {
        continue;
      }
      if (!any) {
        any = true;
        centreLon = hit->lonDeg;
        box = GeoRect{hit->lonDeg, hit->latDeg, hit->lonDeg, hit->latDeg};
        continue;
      }
      double delta = std::fmod(hit->lonDeg - centreLon, 360.0);
      delta = delta > 180.0 ? delta - 360.0
                            : (delta < -180.0 ? delta + 360.0 : delta);
      const double lon = centreLon + delta;
      box.westDeg = std::min(box.westDeg, lon);
      box.eastDeg = std::max(box.eastDeg, lon);
      box.southDeg = std::min(box.southDeg, hit->latDeg);
      box.northDeg = std::max(box.northDeg, hit->latDeg);
    }
  }
  if (!any) {
    return std::nullopt;
  }
  const RegionGeometry area = RegionGeometry::rectangle(
      box.westDeg, box.southDeg, box.eastDeg, box.northDeg);
  return area.isValid() ? std::optional<RegionGeometry>(area) : std::nullopt;
}

std::vector<QPolygonF>
GlobeView::outlineOnScreen(const std::vector<LonLat>& points, bool closed) const
{
  // Each edge straight in longitude and latitude, in steps of at most a
  // degree; broken where it goes behind the globe or jumps across the
  // map's wrap.
  const Camera& camera = m_controller.camera();
  const bool onGlobe = camera.morph() == 0.0;
  const Vec3 eye = camera.eyeEcef();
  std::vector<QPolygonF> pieces(1);
  const std::size_t n = points.size();
  const std::size_t edges = closed ? n : (n > 0 ? n - 1 : 0);
  for (std::size_t i = 0; i < edges; ++i) {
    const LonLat a = points[i];
    const LonLat b = points[(i + 1) % n];
    const double length =
        std::max(std::abs(b.lonDeg - a.lonDeg), std::abs(b.latDeg - a.latDeg));
    const int samples = std::max(1, static_cast<int>(std::ceil(length)));
    for (int step = i == 0 ? 0 : 1; step <= samples; ++step) {
      const double t = step / static_cast<double>(samples);
      const double lat = a.latDeg + t * (b.latDeg - a.latDeg);
      const double lon = a.lonDeg + t * (b.lonDeg - a.lonDeg);
      bool visible = true;
      if (onGlobe) {
        const Vec3 point = geodeticToEcef(Geodetic{lat, lon, 0.0});
        visible = dot(point - eye, point) < 0.0;
      }
      const std::optional<ScreenPoint> screen =
          visible ? camera.projectGeodetic(lat, lon) : std::nullopt;
      QPolygonF& piece = pieces.back();
      if (!screen) {
        if (!piece.isEmpty()) {
          pieces.emplace_back();
        }
        continue;
      }
      const QPointF here(screen->xPx, screen->yPx);
      if (!piece.isEmpty() &&
          std::abs(here.x() - piece.last().x()) > 0.5 * width()) {
        pieces.emplace_back();
      }
      pieces.back().append(here);
    }
  }
  pieces.erase(
      std::remove_if(pieces.begin(), pieces.end(),
                     [](const QPolygonF& piece) { return piece.size() < 2; }),
      pieces.end());
  return pieces;
}

void GlobeView::drawRegions()
{
  if (m_outlines.empty() && m_drawing == RegionDrawing::None) {
    return;
  }
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen(QColor(255, 200, 40), 2.0));
  for (const RegionGeometry& region : m_outlines) {
    for (const QPolygonF& piece : outlineOnScreen(region.vertices(), true)) {
      painter.drawPolyline(piece);
    }
  }
  if (m_drawing == RegionDrawing::None) {
    return;
  }
  // What is being drawn, with a rubber band to the pointer.
  std::vector<LonLat> drawn = m_drawnPoints;
  const std::optional<LonLat> pointer =
      m_pointerInside ? pickLonLat(m_pointerPx) : std::nullopt;
  if (m_drawing == RegionDrawing::Rectangle && m_drawingDrag && pointer &&
      !drawn.empty()) {
    const LonLat start = drawn.front();
    drawn = {start, LonLat{pointer->lonDeg, start.latDeg}, *pointer,
             LonLat{start.lonDeg, pointer->latDeg}};
  } else if (m_drawing == RegionDrawing::Polygon && pointer && !drawn.empty()) {
    drawn.push_back(*pointer);
  }
  QPen pen(QColor(80, 220, 255), 2.0, Qt::DashLine);
  painter.setPen(pen);
  const bool closed = m_drawing == RegionDrawing::Rectangle || drawn.size() > 2;
  if (drawn.size() >= 2) {
    // Rectangle corners may lie either way round the antimeridian: keep
    // each edge the short way.
    for (std::size_t i = 1; i < drawn.size(); ++i) {
      double delta = std::fmod(drawn[i].lonDeg - drawn[i - 1].lonDeg, 360.0);
      delta = delta > 180.0 ? delta - 360.0
                            : (delta < -180.0 ? delta + 360.0 : delta);
      drawn[i].lonDeg = drawn[i - 1].lonDeg + delta;
    }
    for (const QPolygonF& piece : outlineOnScreen(drawn, closed)) {
      painter.drawPolyline(piece);
    }
  }
  for (const LonLat& corner : m_drawnPoints) {
    const std::optional<ScreenPoint> screen =
        m_controller.camera().projectGeodetic(corner.latDeg, corner.lonDeg);
    if (screen) {
      painter.drawEllipse(QPointF(screen->xPx, screen->yPx), 3.0, 3.0);
    }
  }
  // How to draw, at the top.
  const QString text =
      m_drawing == RegionDrawing::Rectangle
          ? tr("Drag across the area to keep for offline use. Esc cancels.")
          : tr("Click each corner of the area; double-click or press Enter "
               "to finish. Backspace removes a corner, Esc cancels.");
  const QFontMetrics metrics(painter.font());
  const int margin = 6;
  const int textWidth =
      std::min(width() - 4 * margin, metrics.horizontalAdvance(text));
  const QRect box((width() - textWidth) / 2 - margin, margin,
                  textWidth + 2 * margin, metrics.height() + 2 * margin);
  painter.fillRect(box, QColor(0, 0, 0, 160));
  painter.setPen(QColor(235, 240, 250));
  painter.drawText(box, Qt::AlignCenter,
                   metrics.elidedText(text, Qt::ElideRight, textWidth));
}

void GlobeView::updatePointerReadout()
{
  if (!m_pointerInside) {
    return;
  }
  const std::optional<Geodetic> hit =
      m_controller.camera().pickGeodetic(m_pointerPx.x(), m_pointerPx.y());
  emit pointerPositionChanged(hit.has_value(), hit ? hit->latDeg : 0.0,
                              hit ? hit->lonDeg : 0.0);
}

// --- Overlays
// -----------------------------------------------------------------

void GlobeView::drawOverlay()
{
  const QLocale locale;
  const Camera& camera = m_controller.camera();
  const CameraPose& pose = camera.pose();
  QStringList lines;
  lines << tr("Renderer: %1").arg(m_deviceInfo.renderer);
  lines << tr("CPU per frame: %1 ms mean, %2 ms max")
               .arg(locale.toString(m_cpuFrameStats.meanS() * 1000.0, 'f', 2),
                    locale.toString(m_cpuFrameStats.maxS() * 1000.0, 'f', 2));
  if (m_intervalStats.count() > 0) {
    lines << tr("While moving: %1 frames per second")
                 .arg(locale.toString(m_intervalStats.meanFps(), 'f', 1));
  }
  lines << tr("Frames drawn: %1").arg(locale.toString(m_framesRendered));
  if (m_globe) {
    const TileStatistics tiles = m_globe->tileStatistics();
    lines << tr("Tiles: %1 drawn (deepest level %2), %3 on the GPU (%4 MB), "
                "%5 wanted")
                 .arg(locale.toString(tiles.rendered),
                      locale.toString(tiles.deepestLevel),
                      locale.toString(tiles.resident),
                      locale.toString(static_cast<double>(tiles.gpuBytes) /
                                          (1024.0 * 1024.0),
                                      'f', 1),
                      locale.toString(tiles.requested));
    const VectorStatistics vectors = m_globe->vectorStatistics();
    lines << tr("Vectors: %1 tiles drawn, %2 triangles, %3 segments, %4 on "
                "the GPU (%5 MB)")
                 .arg(locale.toString(vectors.tilesDrawn),
                      locale.toString(vectors.triangles),
                      locale.toString(vectors.segments),
                      locale.toString(vectors.resident),
                      locale.toString(static_cast<double>(vectors.gpuBytes) /
                                          (1024.0 * 1024.0),
                                      'f', 1));
  }
  lines << tr("Target: %1, %2")
               .arg(formatLatitude(pose.targetLatDeg, locale),
                    formatLongitude(pose.targetLonDeg, locale));
  lines << tr("Range: %1, heading %2°, tilt %3°")
               .arg(formatDistance(pose.rangeM, locale),
                    locale.toString(pose.headingDeg, 'f', 1),
                    locale.toString(pose.tiltDeg, 'f', 1));
  lines << tr("Eye height: %1")
               .arg(formatDistance(camera.eyeGeodetic().heightM, locale));
  if (camera.morph() > 0.0) {
    lines << tr("Map: %1, %2 % of the way from the globe")
                 .arg(camera.mapProjection()->displayName(),
                      locale.toString(camera.morph() * 100.0, 'f', 0));
  }

  QPainter painter(this);
  painter.setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  const QFontMetrics metrics = painter.fontMetrics();
  int textWidth = 0;
  for (const QString& line : lines) {
    textWidth = std::max(textWidth, metrics.horizontalAdvance(line));
  }
  const int margin = 6;
  const int lineHeight = metrics.lineSpacing();
  const QRect box(margin, margin, textWidth + 2 * margin,
                  lineHeight * static_cast<int>(lines.size()) + 2 * margin);
  painter.fillRect(box, QColor(0, 0, 0, 160));
  painter.setPen(QColor(230, 235, 245));
  int y = box.top() + margin + metrics.ascent();
  for (const QString& line : lines) {
    painter.drawText(box.left() + margin, y, line);
    y += lineHeight;
  }
}

void GlobeView::drawAttributions()
{
  if (m_attributions.isEmpty()) {
    return;
  }
  // Small text on a translucent band in the bottom-right corner, one
  // line per source.
  QPainter painter(this);
  QFont font = painter.font();
  font.setPointSizeF(font.pointSizeF() * 0.85);
  painter.setFont(font);
  const QFontMetrics metrics(font);
  const int margin = 4;
  int textWidth = 0;
  for (const QString& line : m_attributions) {
    textWidth = std::max(textWidth, metrics.horizontalAdvance(line));
  }
  const int lineHeight = metrics.lineSpacing();
  const int boxHeight =
      lineHeight * static_cast<int>(m_attributions.size()) + 2 * margin;
  const QRect box(width() - textWidth - 2 * margin, height() - boxHeight,
                  textWidth + 2 * margin, boxHeight);
  painter.fillRect(box, QColor(0, 0, 0, 120));
  painter.setPen(QColor(225, 230, 240));
  int y = box.top() + margin + metrics.ascent();
  for (const QString& line : m_attributions) {
    painter.drawText(box.right() - margin - metrics.horizontalAdvance(line), y,
                     line);
    y += lineHeight;
  }
}

void GlobeView::drawScaleBar()
{
  m_scaleBarText.clear();
  if (!m_scaleBarVisible || m_controller.isMorphing()) {
    return;
  }
  const std::optional<ScaleBar> bar =
      scaleBarFor(m_controller.camera(), scaleBarMaximumWidthPx);
  if (!bar) {
    return;
  }
  const QLocale locale;
  // Where the scale changes over the view, say where the bar holds.
  m_scaleBarText = bar->variesAcrossView
                       ? tr("%1 at %2")
                             .arg(formatRoundDistance(bar->lengthM, locale),
                                  formatWholeLatitude(bar->latDeg, locale))
                       : formatRoundDistance(bar->lengthM, locale);
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, false);
  const QFontMetrics metrics = painter.fontMetrics();
  const int margin = 6;
  const int tick = 5;
  const int barWidth = qRound(bar->widthPx);
  const int boxWidth =
      std::max(barWidth, metrics.horizontalAdvance(m_scaleBarText)) +
      2 * margin;
  const int boxHeight = metrics.lineSpacing() + tick + 2 * margin;
  const QRect box(margin, height() - boxHeight - margin, boxWidth, boxHeight);
  painter.fillRect(box, QColor(0, 0, 0, 120));
  painter.setPen(QColor(235, 238, 245));
  painter.drawText(box.left() + margin, box.top() + margin + metrics.ascent(),
                   m_scaleBarText);
  const int left = box.left() + margin;
  const int bottom = box.bottom() - margin;
  painter.drawLine(left, bottom, left + barWidth, bottom);
  painter.drawLine(left, bottom - tick, left, bottom);
  painter.drawLine(left + barWidth, bottom - tick, left + barWidth, bottom);
}

void GlobeView::drawError()
{
  QPainter painter(this);
  painter.fillRect(rect(), QColor(16, 18, 24));
  painter.setPen(QColor(230, 235, 245));
  const QString message =
      m_error.isEmpty()
          ? tr("The globe could not be drawn.")
          : tr("The globe needs OpenGL 3.3, which is not available:\n%1")
                .arg(m_error);
  painter.drawText(rect().adjusted(16, 16, -16, -16),
                   Qt::AlignCenter | Qt::TextWordWrap, message);
}

} // namespace ts
