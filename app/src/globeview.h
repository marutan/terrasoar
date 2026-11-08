// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The 3D globe view widget.
 */
#ifndef TS_APP_GLOBEVIEW_H
#define TS_APP_GLOBEVIEW_H

#include <memory>

#include <QElapsedTimer>
#include <QOpenGLWidget>
#include <QPointF>
#include <QString>
#include <QTimer>

#include "ts/camera/cameracontroller.h"
#include "ts/globe/layerstackdraws.h"
#include "ts/perf/frametimestats.h"
#include "ts/render/framepacer.h"
#include "ts/render/renderdevice.h"

class QKeyEvent;
class QMouseEvent;
class QWheelEvent;

namespace ts
{

class GlRenderDevice;
class GlobeRenderer;

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

  /**
   * Returns false where composite groups fall back to multiplied opacity
   * (software rendering), true otherwise.
   */
  bool isGroupCompositingAvailable() const;

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

signals:
  /**
   * The point under the pointer changed. @p onGlobe is false when the
   * pointer is off the globe or outside the view; the angles are then
   * meaningless.
   */
  void pointerPositionChanged(bool onGlobe, double latDeg, double lonDeg);

  /** The camera moved; @p eyeHeightM is its height above the ellipsoid. */
  void cameraChanged(double eyeHeightM);

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
  void drawError();

  CameraController m_controller;
  QMetaObject::Connection m_contextConnection;
  std::unique_ptr<GlRenderDevice> m_device;
  std::unique_ptr<GlobeRenderer> m_globe;
  ImageryDraws m_draws;
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
  FrameTimeStats m_cpuFrameStats;
  FrameTimeStats m_intervalStats;
  double m_lastFrameStartS = -1.0;
  int m_framesRendered = 0;
};

} // namespace ts

#endif // TS_APP_GLOBEVIEW_H
