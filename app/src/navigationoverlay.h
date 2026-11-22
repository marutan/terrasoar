// SPDX-License-Identifier: GPL-2.0-or-later
/*
  Terra Soar - A virtual globe

  Copyright (C) 2026 Peter Howkins

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 * The on-screen navigation controls over the globe and the map: a compass,
 * pan arrows, zoom and tilt, and Home (Phase 20).
 */
#ifndef TS_APP_NAVIGATIONOVERLAY_H
#define TS_APP_NAVIGATIONOVERLAY_H

#include <cstdint>
#include <optional>
#include <vector>

#include <QColor>
#include <QString>
#include <QWidget>

namespace ts
{

/** What a navigation control does, held or clicked. */
enum class NavigationAction {
  PanUp,
  PanDown,
  PanLeft,
  PanRight,
  ZoomIn,
  ZoomOut,
  TurnLeft,
  TurnRight,
  TiltUp,
  TiltDown,
};

/** The corner of the view the controls sit in. */
enum class OverlayCorner {
  TopRight,
  TopLeft,
  BottomLeft,
  BottomRight,
};

/** Returns the settings name of @p corner ("top-right" and so on). */
QString overlayCornerName(OverlayCorner corner);

/** Returns the corner named @p name, top right if it isn't one. */
OverlayCorner overlayCornerFromName(const QString& name);

/** What the controls show of the camera. */
struct NavigationState {
  /** Heading, degrees clockwise from north. */
  double headingDeg = 0.0;
  /** Tilt, degrees from straight down. */
  double tiltDeg = 0.0;
  /** The largest tilt now (the globe's or the map's), degrees. */
  double maximumTiltDeg = 180.0;
  /** Where the zoom slider stands: 0 farthest out, 1 nearest in. */
  double zoomFraction = 0.0;
  /** The height (globe) or scale (map) the slider shows, as text. */
  QString zoomText;

  /** Equality. */
  bool operator==(const NavigationState& other) const;
  /** Inequality. */
  bool operator!=(const NavigationState& other) const
  {
    return !(*this == other);
  }
};

/** The colours of the controls. */
struct NavigationColours {
  /** The plate behind each control (premultiplied alpha not applied). */
  QColor plate;
  /** The plate's thin edge. */
  QColor edge;
  /** The symbols. */
  QColor symbol;
  /** The symbols' outline (halo). */
  QColor halo;
  /** Marks: north, the sliders' knobs. */
  QColor accent;
};

/**
 * The navigation controls: a group of small controls on plates, laid
 * over the view in one of its corners.
 *
 * Every control is a widget of its own, so it takes the mouse, touch
 * (through Qt's synthesised mouse events), keyboard focus (Tab into the
 * view, then between the controls; arrows and Enter work them), and has
 * a tooltip and an accessible name. Clicks on the controls' area never
 * reach the view below. The controls say what the user wants through
 * signals; GlobeView turns them into CameraController motions, so they
 * share the keys' speeds, inertia and limits.
 *
 * Plates are translucent and dark with a light edge, symbols light with a
 * dark halo, so one of them stands out on any background; the pointer
 * over the controls makes the plates opaque, and hover and press change
 * their brightness. In high-contrast mode the plates are opaque in the
 * system's colours.
 */
class NavigationOverlay : public QWidget
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(NavigationOverlay)

public:
  /** Lays the controls over @p view (its parent), in the top right. */
  explicit NavigationOverlay(QWidget* view);
  ~NavigationOverlay() override;

  /** Shows the camera's state (repaints only what changed). */
  void setState(const NavigationState& state);

  /** Returns the state shown. */
  const NavigationState& state() const { return m_state; }

  /** Puts the controls in @p corner of the view. */
  void setCorner(OverlayCorner corner);

  /** Returns the corner the controls are in. */
  OverlayCorner corner() const { return m_corner; }

  /** Uses opaque plates in the system's colours (or not). */
  void setHighContrast(bool highContrast);

  /** Returns true in high-contrast mode. */
  bool isHighContrast() const { return m_highContrast; }

  /** Returns true if the system asks for high contrast. */
  static bool systemHighContrast();

  /** Returns the colours used now (hover, press and contrast mode). */
  NavigationColours colours(bool hovered, bool pressed) const;

  /** Returns true while the pointer is over the controls. */
  bool isActive() const { return m_active; }

  /**
   * Says whether the pointer is over the controls (the overlay follows
   * the pointer itself; for tests and touch).
   */
  void setPointerOver(bool over);

  /**
   * Returns how opaque the plates are now, 0 to 1: lower until the pointer
   * is over the controls, never below the least that keeps them readable.
   */
  double plateOpacity() const;

  /** The plates' opacity when the pointer isn't over the controls. */
  static constexpr double idlePlateOpacity = 0.75;

  /** Returns the size of one control (a button), logical pixels. */
  int32_t unitPx() const { return m_unitPx; }

  /** Returns the control named @p objectName (for tests). */
  QWidget* control(const QString& objectName) const;

  /** Returns every control (for tests). */
  std::vector<QWidget*> controls() const;

  /** Places the controls in their corner of the view again. */
  void place();

  /** Lays the controls out again for the screen's DPI and font size. */
  void relayout();

  /** Emits actionHeld(); for the controls. */
  void notifyHeld(NavigationAction action, bool held);

  /** Emits actionClicked(); for the controls. */
  void notifyClicked(NavigationAction action);

  /** Emits headingRequested(); for the compass. */
  void notifyHeading(double headingDeg);

  /** Emits northUpRequested(); for the compass. */
  void notifyNorthUp();

  /** Emits homeRequested(); for the Home button. */
  void notifyHome();

  /** Emits zoomRequested(); for the zoom slider. */
  void notifyZoom(double fraction);

  /** Emits tiltRequested(); for the tilt arc. */
  void notifyTilt(double tiltDeg);

signals:
  /** A control that moves while held was pressed (or released). */
  void actionHeld(ts::NavigationAction action, bool held);

  /** A control was clicked: a step of its motion. */
  void actionClicked(ts::NavigationAction action);

  /** The compass ring was dragged: the heading asked for. */
  void headingRequested(double headingDeg);

  /** "N" was clicked: back to north-up. */
  void northUpRequested();

  /** Home was clicked: the start view (View → Reset View). */
  void homeRequested();

  /** The zoom slider was clicked or dragged to @p fraction. */
  void zoomRequested(double fraction);

  /** The tilt arc was clicked or dragged (or double-clicked: 0). */
  void tiltRequested(double tiltDeg);

protected:
  /** Follows the view's size and the pointer entering a control. */
  bool eventFilter(QObject* watched, QEvent* event) override;
  /** Hover, language, font and screen changes. */
  bool event(QEvent* event) override;
  /** Keeps a press between the controls from the view. */
  void mousePressEvent(QMouseEvent* event) override;
  /** Keeps a release between the controls from the view. */
  void mouseReleaseEvent(QMouseEvent* event) override;
  /** Keeps a double click between the controls from the view. */
  void mouseDoubleClickEvent(QMouseEvent* event) override;
  /** Keeps the wheel over the controls from the view. */
  void wheelEvent(QWheelEvent* event) override;

private:
  void retranslate();

  NavigationState m_state;
  OverlayCorner m_corner = OverlayCorner::TopRight;
  bool m_highContrast = false;
  bool m_active = false;
  int32_t m_unitPx = 36;
  std::vector<QWidget*> m_controls;
};

/** WCAG 2.1's contrast ratio between two opaque colours (1 to 21). */
double navigationContrastRatio(const QColor& a, const QColor& b);

/** @p over (with its alpha) composited over the opaque @p under. */
QColor navigationComposite(const QColor& over, const QColor& under);

} // namespace ts

#endif // TS_APP_NAVIGATIONOVERLAY_H
