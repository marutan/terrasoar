// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The 3D globe view widget.
 */
#ifndef TS_APP_GLOBEVIEW_H
#define TS_APP_GLOBEVIEW_H

#include <memory>
#include <optional>
#include <vector>

#include <QElapsedTimer>
#include <QMetaType>
#include <QOpenGLWidget>
#include <QPointF>
#include <QPolygonF>
#include <QString>
#include <QStringList>
#include <QTimer>

#include "ts/camera/cameracontroller.h"
#include "ts/globe/layerstackdraws.h"
#include "ts/perf/frametimestats.h"
#include "ts/regions/regiongeometry.h"
#include "ts/render/framepacer.h"
#include "ts/render/renderdevice.h"

class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

namespace ts
{

class GlRenderDevice;
class GlobeRenderer;
class Projection;

/**
 * Shows the globe and turns mouse and keyboard input into camera motion.
 *
 * Renders only when something changes: input, or camera motion that
 * continues on its own (inertia, fly-to, held keys). While moving, frames
 * are paced to the screen's refresh rate (FramePacer); when still, no
 * timer runs and the view uses no CPU.
 *
 * Mouse: left drag pans (the grabbed point stays under the pointer); right
 * or middle drag, or Ctrl + left drag, turns and tilts; the wheel zooms at
 * the pointer; a double click flies to the point, closer. Keys: arrows or
 * W/A/S/D move, Page Up/Down or +/− zoom, Q/E turn, R/F tilt, Home goes
 * back to the start view.
 *
 * The view is the 3D globe or a flat map in one of the projections()
 * (setViewMode(), setMapProjection()); changing between them is animated.
 * The map looks straight down with north up, so there dragging and the
 * keys slide it and turning and tilting do nothing. A scale bar in the
 * bottom-left corner shows a ground distance at the centre of the view.
 *
 * For offline regions the view can draw a region's outline
 * (setRegionOutlines()) and let the user draw a new one
 * (startRegionDrawing()): a rectangle by dragging, or a polygon by
 * clicking its corners (double-click or Enter finishes, Backspace takes
 * the last corner back); Esc cancels. Right drags and the wheel still
 * move the camera meanwhile.
 *
 * @guithread
 */
class GlobeView : public QOpenGLWidget
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(GlobeView)

public:
  /** Creates the view. @param parent Usual QWidget parent. */
  explicit GlobeView(QWidget* parent = nullptr);

  /** Releases the GPU resources. */
  ~GlobeView() override;

  /** Returns the camera controller. */
  const CameraController& controller() const { return m_controller; }

  /** Returns the pose the view starts at and Home returns to. */
  static CameraPose homePose();

  /**
   * Returns true if the platform can show an OpenGL view: not the
   * offscreen or minimal platforms (headless runs), and OpenGL contexts
   * can be created. Free when Qt already made its global share context
   * (Qt::AA_ShareOpenGLContexts, as main() sets); otherwise creates and
   * discards a context.
   */
  static bool isOpenGlAvailable();

  /** Shows or hides the debug overlay (frame times, camera). */
  void setDebugOverlayVisible(bool visible);

  /** Returns true if the debug overlay is shown. */
  bool isDebugOverlayVisible() const { return m_overlayVisible; }

  /**
   * Sets the imagery layers drawn on the globe (from the layer tree, see
   * imageryDrawsForStack()). Shown in the next frame.
   */
  void setImageryDraws(const ImageryDraws& draws);

  /** Returns the number of imagery layers drawn. */
  int imageryLayerCount() const
  {
    return static_cast<int>(m_draws.layers.size());
  }

  /** Sets the vector layers (and the graticule), bottom to top. */
  void setVectorDraws(const std::vector<VectorLayerDraw>& draws);

  /** Returns the number of vector layers drawn (the graticule included). */
  int vectorLayerCount() const
  {
    return static_cast<int>(m_vectorDraws.size());
  }

  /**
   * Sets the attributions shown in the bottom corner of the view while
   * their layers are on screen.
   */
  void setAttributions(const QStringList& attributions);

  /** Returns the attributions shown. */
  const QStringList& attributions() const { return m_attributions; }

  /** Returns what the tile renderer did in the last frame. */
  TileStatistics tileStatistics() const;

  /** Returns what the vector renderer did in the last frame. */
  VectorStatistics vectorStatistics() const;

  /**
   * Returns false where composite groups fall back to multiplied opacity
   * (software rendering), true otherwise.
   */
  bool isGroupCompositingAvailable() const;

  /**
   * Shows the globe or the flat map. @p animate true changes over about a
   * second (less on software renderers), false at once.
   */
  void setViewMode(ViewMode mode, bool animate);

  /** Returns what is shown (what is being changed to, during a change). */
  ViewMode viewMode() const { return m_controller.viewMode(); }

  /** Sets the projection of the flat map; null is ignored. */
  void setMapProjection(const Projection* projection);

  /** Returns the projection of the flat map. */
  const Projection* mapProjection() const;

  /** Returns how long an animated change of view mode takes, seconds. */
  double morphDurationS() const;

  /** Shows or hides the scale bar. */
  void setScaleBarVisible(bool visible);

  /** Returns true if the scale bar is shown. */
  bool isScaleBarVisible() const { return m_scaleBarVisible; }

  /**
   * Returns the text of the scale bar as last drawn, e.g. "500 km"; empty
   * if it wasn't drawn.
   */
  const QString& scaleBarText() const { return m_scaleBarText; }

  /** Moves the camera to @p pose at once, stopping any motion. */
  void setPose(const CameraPose& pose);

  /**
   * Returns true while tiles are loading or waiting for upload (frames
   * keep coming until they are done).
   */
  bool isLoadingTiles() const;

  /** Flies back to homePose(). */
  void goHome();

  /** Flies to @p pose; @p durationS ≤ 0 picks the duration. */
  void flyTo(const CameraPose& pose, double durationS);

  /**
   * Returns why OpenGL could not be used, or an empty string if it
   * works (or hasn't been initialised yet).
   */
  const QString& errorString() const { return m_error; }

  /** Returns what the renderer runs on (empty before initialisation). */
  const RenderDeviceInfo& deviceInfo() const { return m_deviceInfo; }

  /** Returns the number of frames drawn so far. */
  int framesRendered() const { return m_framesRendered; }

  /** How the user draws a region, see startRegionDrawing(). */
  enum class RegionDrawing {
    None,      ///< Not drawing.
    Rectangle, ///< Drag from corner to corner.
    Polygon,   ///< Click each corner; double-click or Enter finishes.
  };

  /**
   * Lets the user draw a region with the left button (see the class
   * description); regionDrawn() or regionDrawingCancelled() follows.
   * RegionDrawing::None cancels drawing.
   */
  void startRegionDrawing(RegionDrawing mode);

  /** Returns how a region is being drawn (None when not). */
  RegionDrawing regionDrawing() const { return m_drawing; }

  /** Draws the outlines of @p regions over the globe (none: nothing). */
  void setRegionOutlines(const std::vector<RegionGeometry>& regions);

  /** Returns the outlines being drawn. */
  const std::vector<RegionGeometry>& regionOutlines() const
  {
    return m_outlines;
  }

  /**
   * Returns the latitude/longitude rectangle around what the view shows
   * of the Earth, or nothing if the Earth isn't in view.
   */
  std::optional<RegionGeometry> visibleArea() const;

signals:
  /**
   * The point under the pointer changed. @p onGlobe is false when the
   * pointer is off the globe or outside the view; the angles are then
   * meaningless.
   */
  void pointerPositionChanged(bool onGlobe, double latDeg, double lonDeg);

  /** The camera moved; @p eyeHeightM is its height above the ellipsoid. */
  void cameraChanged(double eyeHeightM);

  /** setViewMode() changed what the view shows (or is changing to). */
  void viewModeChanged(ts::ViewMode mode);

  /** setMapProjection() changed the flat map's projection. */
  void mapProjectionChanged(const ts::Projection* projection);

  /** The user finished drawing a region (startRegionDrawing()). */
  void regionDrawn(const ts::RegionGeometry& region);

  /** The user cancelled drawing a region, or it was cancelled. */
  void regionDrawingCancelled();

protected:
  /** Creates the renderer. */
  void initializeGL() override;
  /** Updates the camera's viewport. */
  void resizeGL(int widthPx, int heightPx) override;
  /** Draws the globe and, if shown, the debug overlay. */
  void paintGL() override;
  /** Starts a pan or rotate drag. */
  void mousePressEvent(QMouseEvent* event) override;
  /** Continues a drag; updates the pointer readout. */
  void mouseMoveEvent(QMouseEvent* event) override;
  /** Ends a drag. */
  void mouseReleaseEvent(QMouseEvent* event) override;
  /** Flies to the point under the pointer. */
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  /** Zooms at the pointer. */
  void wheelEvent(QWheelEvent* event) override;
  /** Starts held-key motion. */
  void keyPressEvent(QKeyEvent* event) override;
  /** Stops held-key motion. */
  void keyReleaseEvent(QKeyEvent* event) override;
  /** Stops held-key motion when focus goes elsewhere. */
  void focusOutEvent(QFocusEvent* event) override;
  /** Clears the pointer readout. */
  void leaveEvent(QEvent* event) override;

private:
  double nowS() const;
  void requestFrame();
  void onFrameTimer();
  void releaseGl();
  void updatePointerReadout();
  void drawOverlay();
  void drawAttributions();
  void drawScaleBar();
  void drawError();
  void drawRegions();
  void finishPolygon();
  std::optional<LonLat> pickLonLat(QPointF positionPx) const;
  std::vector<QPolygonF> outlineOnScreen(const std::vector<LonLat>& points,
                                         bool closed) const;

  CameraController m_controller;
  QMetaObject::Connection m_contextConnection;
  std::unique_ptr<GlRenderDevice> m_device;
  std::unique_ptr<GlobeRenderer> m_globe;
  ImageryDraws m_draws;
  std::vector<VectorLayerDraw> m_vectorDraws;
  QStringList m_attributions;
  RenderDeviceInfo m_deviceInfo;
  QString m_error;

  QElapsedTimer m_clock;
  QTimer m_frameTimer;
  FramePacer m_pacer;
  double m_lastAdvanceS = 0.0;
  bool m_animationClockRunning = false;
  unsigned int m_heldKeys = NoMotion;

  bool m_pointerInside = false;
  QPointF m_pointerPx;

  bool m_overlayVisible = false;
  bool m_scaleBarVisible = true;
  QString m_scaleBarText;
  FrameTimeStats m_cpuFrameStats;
  FrameTimeStats m_intervalStats;
  double m_lastFrameStartS = -1.0;
  int m_framesRendered = 0;

  RegionDrawing m_drawing = RegionDrawing::None;
  /** Corners drawn so far (a rectangle's first corner, a polygon's). */
  std::vector<LonLat> m_drawnPoints;
  /** True while dragging out a rectangle. */
  bool m_drawingDrag = false;
  std::vector<RegionGeometry> m_outlines;
};

} // namespace ts

// For queued connections and QSignalSpy (Qt 5 needs it declared).
Q_DECLARE_METATYPE(ts::RegionGeometry)

#endif // TS_APP_GLOBEVIEW_H
