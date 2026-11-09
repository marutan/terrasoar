// SPDX-License-Identifier: GPL-2.0-or-later
#include "globeview.h"

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

#include "ts/geo/geoformat.h"
#include "ts/globe/globerenderer.h"
#include "ts/globe/tilerenderer.h"
#include "ts/providers/tileprovider.h"
#include "ts/render/glrenderdevice.h"

namespace ts
{

namespace
{

/** Range factor for a double click: fly three times closer. */
constexpr double doubleClickZoom = 1.0 / 3.0;

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
