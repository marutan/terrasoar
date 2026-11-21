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
#include "navigationoverlay.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <optional>
#include <utility>

#include <QApplication>
#include <QBasicTimer>
#include <QCoreApplication>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QResizeEvent>
#include <QTimerEvent>
#include <QWheelEvent>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace ts
{

namespace
{

/** QColor's channels: qreal in Qt 5, float in Qt 6. */
using ColourChannel = decltype(QColor().redF());

/** How long a press must last to be a hold rather than a click, ms. */
constexpr int32_t holdDelayMs = 300;

/** The kinds of symbol a button shows. */
enum class Symbol {
  ArrowUp,
  ArrowDown,
  ArrowLeft,
  ArrowRight,
  Plus,
  Minus,
  TurnLeft,
  TurnRight,
  TiltUp,
  TiltDown,
  Home,
};

double pi()
{
  return 3.14159265358979323846;
}

/** Draws @p path as a light stroke with a dark halo around it. */
void drawSymbol(QPainter& painter, const QPainterPath& path,
                const NavigationColours& colours, double widthPx)
{
  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen(colours.halo, widthPx + 3.0, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
  painter.drawPath(path);
  painter.setPen(QPen(colours.symbol, widthPx, Qt::SolidLine, Qt::RoundCap,
                      Qt::RoundJoin));
  painter.drawPath(path);
}

/** Fills @p path in @p fill with a dark halo around it. */
void fillSymbol(QPainter& painter, const QPainterPath& path, const QColor& fill,
                const NavigationColours& colours)
{
  painter.setPen(
      QPen(colours.halo, 3.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.setBrush(Qt::NoBrush);
  painter.drawPath(path);
  painter.setPen(Qt::NoPen);
  painter.setBrush(fill);
  painter.drawPath(path);
}

/** Paints a round plate filling @p rect, and the focus ring if focused. */
void paintPlate(QPainter& painter, const QRectF& rect,
                const NavigationColours& colours, bool focused, bool round)
{
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(QPen(colours.edge, 1.2));
  painter.setBrush(colours.plate);
  const QRectF plate = rect.adjusted(1.0, 1.0, -1.0, -1.0);
  const double radius = 0.5 * std::min(plate.width(), plate.height());
  if (round) {
    painter.drawEllipse(plate);
  } else {
    painter.drawRoundedRect(plate, radius, radius);
  }
  if (focused) {
    painter.setPen(QPen(colours.accent, 2.0, Qt::DashLine));
    painter.setBrush(Qt::NoBrush);
    const QRectF ring = plate.adjusted(2.5, 2.5, -2.5, -2.5);
    if (round) {
      painter.drawEllipse(ring);
    } else {
      painter.drawRoundedRect(ring, radius - 2.5, radius - 2.5);
    }
  }
}

/** The path of @p symbol in a box @p sizePx across, centred at 0. */
QPainterPath symbolPath(Symbol symbol, double sizePx)
{
  const double s = 0.22 * sizePx;
  QPainterPath path;
  switch (symbol) {
  case Symbol::ArrowUp:
    path.moveTo(-s, 0.5 * s);
    path.lineTo(0.0, -0.5 * s);
    path.lineTo(s, 0.5 * s);
    break;
  case Symbol::ArrowDown:
    path.moveTo(-s, -0.5 * s);
    path.lineTo(0.0, 0.5 * s);
    path.lineTo(s, -0.5 * s);
    break;
  case Symbol::ArrowLeft:
    path.moveTo(0.5 * s, -s);
    path.lineTo(-0.5 * s, 0.0);
    path.lineTo(0.5 * s, s);
    break;
  case Symbol::ArrowRight:
    path.moveTo(-0.5 * s, -s);
    path.lineTo(0.5 * s, 0.0);
    path.lineTo(-0.5 * s, s);
    break;
  case Symbol::Plus:
    path.moveTo(-s, 0.0);
    path.lineTo(s, 0.0);
    path.moveTo(0.0, -s);
    path.lineTo(0.0, s);
    break;
  case Symbol::Minus:
    path.moveTo(-s, 0.0);
    path.lineTo(s, 0.0);
    break;
  case Symbol::TurnLeft:
  case Symbol::TurnRight: {
    // Most of a circle, with an arrowhead at its end.
    const bool left = symbol == Symbol::TurnLeft;
    const QRectF circle(-s, -s, 2.0 * s, 2.0 * s);
    path.arcMoveTo(circle, left ? 60.0 : 120.0);
    path.arcTo(circle, left ? 60.0 : 120.0, left ? 250.0 : -250.0);
    const QPointF end = path.currentPosition();
    const double side = left ? 1.0 : -1.0;
    path.moveTo(end + QPointF(-0.55 * s * side, -0.15 * s));
    path.lineTo(end);
    path.lineTo(end + QPointF(0.05 * s * side, -0.6 * s));
    break;
  }
  case Symbol::TiltUp:
  case Symbol::TiltDown: {
    // A plane seen flat (down) or on edge (up), with an arrow.
    const double tilt = symbol == Symbol::TiltUp ? 0.35 : 0.9;
    path.moveTo(-s, 0.6 * s);
    path.lineTo(s, 0.6 * s);
    path.lineTo(0.7 * s, 0.6 * s - tilt * s);
    path.lineTo(-0.7 * s, 0.6 * s - tilt * s);
    path.closeSubpath();
    const double y = symbol == Symbol::TiltUp ? -0.55 * s : -0.15 * s;
    path.moveTo(-0.35 * s, y + 0.2 * s * (symbol == Symbol::TiltUp ? 1 : -1));
    path.lineTo(0.0, y);
    path.lineTo(0.35 * s, y + 0.2 * s * (symbol == Symbol::TiltUp ? 1 : -1));
    break;
  }
  case Symbol::Home:
    path.moveTo(-s, 0.05 * s);
    path.lineTo(0.0, -0.95 * s);
    path.lineTo(s, 0.05 * s);
    path.moveTo(-0.7 * s, -0.2 * s);
    path.lineTo(-0.7 * s, s);
    path.lineTo(0.7 * s, s);
    path.lineTo(0.7 * s, -0.2 * s);
    break;
  }
  return path;
}

/** A control of the overlay: hover, press and focus, painted on a plate. */
// No signals or slots of its own: it calls the overlay.
class NavControl : public QWidget // clazy:exclude=missing-qobject-macro
{
public:
  NavControl(NavigationOverlay* overlay, const QString& objectName)
      : QWidget(overlay), m_overlay(overlay)
  {
    setObjectName(objectName);
    setFocusPolicy(Qt::TabFocus);
    setAttribute(Qt::WA_Hover, true);
    setCursor(Qt::PointingHandCursor);
  }

protected:
  NavigationColours plateColours() const
  {
    return m_overlay->colours(m_hovered, m_pressed);
  }

  bool event(QEvent* event) override
  {
    switch (event->type()) {
    case QEvent::Enter:
    case QEvent::HoverEnter:
      m_hovered = true;
      update();
      break;
    case QEvent::Leave:
    case QEvent::HoverLeave:
      m_hovered = false;
      update();
      break;
    default:
      break;
    }
    return QWidget::event(event);
  }

  void focusInEvent(QFocusEvent* event) override
  {
    update();
    QWidget::focusInEvent(event);
  }

  void focusOutEvent(QFocusEvent* event) override
  {
    update();
    QWidget::focusOutEvent(event);
  }

  void setPressed(bool pressed)
  {
    m_pressed = pressed;
    update();
  }

  bool isPressed() const { return m_pressed; }

  NavigationOverlay* overlay() const { return m_overlay; }

private:
  NavigationOverlay* m_overlay;
  bool m_hovered = false;
  bool m_pressed = false;
};

/**
 * A round button: a click is one step of its action, a press held longer
 * moves until released (the same as holding the matching key).
 */
// No signals or slots of its own: it calls the overlay.
class NavButton : public NavControl // clazy:exclude=missing-qobject-macro
{
public:
  NavButton(NavigationOverlay* overlay, const QString& objectName,
            Symbol symbol, std::optional<NavigationAction> action)
      : NavControl(overlay, objectName), m_symbol(symbol), m_action(action)
  {
  }

protected:
  void paintEvent(QPaintEvent* /*event*/) override
  {
    QPainter painter(this);
    const NavigationColours c = plateColours();
    paintPlate(painter, QRectF(rect()), c, hasFocus(), true);
    painter.translate(QRectF(rect()).center());
    drawSymbol(painter, symbolPath(m_symbol, width()), c,
               std::max(2.0, 0.07 * width()));
  }

  void mousePressEvent(QMouseEvent* event) override
  {
    if (event->button() != Qt::LeftButton) {
      event->accept();
      return;
    }
    setPressed(true);
    m_holding = false;
    if (m_action) {
      m_holdTimer.start(holdDelayMs, this);
    }
    event->accept();
  }

  void mouseReleaseEvent(QMouseEvent* event) override
  {
    if (event->button() != Qt::LeftButton || !isPressed()) {
      event->accept();
      return;
    }
    const bool inside = rect().contains(event->pos());
    finish(inside);
    event->accept();
  }

  void mouseDoubleClickEvent(QMouseEvent* event) override
  {
    // A second quick click is a second step.
    mousePressEvent(event);
  }

  void timerEvent(QTimerEvent* event) override
  {
    if (event->timerId() != m_holdTimer.timerId()) {
      NavControl::timerEvent(event);
      return;
    }
    m_holdTimer.stop();
    if (isPressed() && m_action) {
      m_holding = true;
      overlay()->notifyHeld(*m_action, true);
    }
  }

  void keyPressEvent(QKeyEvent* event) override
  {
    switch (event->key()) {
    case Qt::Key_Space:
    case Qt::Key_Return:
    case Qt::Key_Enter:
      click();
      event->accept();
      return;
    default:
      NavControl::keyPressEvent(event);
    }
  }

  void focusOutEvent(QFocusEvent* event) override
  {
    if (isPressed()) {
      finish(false);
    }
    NavControl::focusOutEvent(event);
  }

  void hideEvent(QHideEvent* event) override
  {
    if (isPressed()) {
      finish(false);
    }
    NavControl::hideEvent(event);
  }

private:
  void finish(bool clicked)
  {
    m_holdTimer.stop();
    setPressed(false);
    if (m_holding && m_action) {
      m_holding = false;
      overlay()->notifyHeld(*m_action, false);
      return;
    }
    if (clicked) {
      click();
    }
  }

  void click()
  {
    if (m_action) {
      overlay()->notifyClicked(*m_action);
    } else {
      overlay()->notifyHome();
    }
  }

  Symbol m_symbol;
  std::optional<NavigationAction> m_action;
  QBasicTimer m_holdTimer;
  bool m_holding = false;
};

/**
 * The compass: a ring showing north. Dragging the ring turns the view;
 * clicking the "N" turns it back to north-up.
 */
// No signals or slots of its own: it calls the overlay.
class CompassControl : public NavControl // clazy:exclude=missing-qobject-macro
{
public:
  explicit CompassControl(NavigationOverlay* overlay)
      : NavControl(overlay, QStringLiteral("navigationCompass"))
  {
  }

  /** Where "N" is drawn, relative to the centre. */
  QPointF northAt() const
  {
    const double r = 0.36 * width();
    const double a = -overlay()->state().headingDeg * pi() / 180.0;
    return QPointF(r * std::sin(a), -r * std::cos(a));
  }

protected:
  void paintEvent(QPaintEvent* /*event*/) override
  {
    QPainter painter(this);
    const NavigationColours c = plateColours();
    paintPlate(painter, QRectF(rect()), c, hasFocus(), true);
    painter.translate(QRectF(rect()).center());
    const double r = 0.5 * width();
    // The ring and its ticks every 45°, turned with the view.
    const double heading = overlay()->state().headingDeg;
    QPainterPath ticks;
    for (int32_t i = 0; i < 8; ++i) {
      const double a = (45.0 * i - heading) * pi() / 180.0;
      const double inner = i % 2 == 0 ? 0.62 : 0.70;
      ticks.moveTo(inner * r * std::sin(a), -inner * r * std::cos(a));
      ticks.lineTo(0.80 * r * std::sin(a), -0.80 * r * std::cos(a));
    }
    drawSymbol(painter, ticks, c, std::max(1.5, 0.03 * width()));
    // North: a pointer and the letter.
    const QPointF n = northAt();
    QFont font = this->font();
    font.setBold(true);
    font.setPixelSize(std::max(9, static_cast<int32_t>(0.24 * width())));
    QPainterPath letter;
    // ts-notr: the compass's north mark is a symbol, the same everywhere
    letter.addText(0.0, 0.0, font, QStringLiteral("N"));
    const QRectF box = letter.boundingRect();
    letter.translate(n - box.center());
    fillSymbol(painter, letter, c.accent, c);
  }

  void mousePressEvent(QMouseEvent* event) override
  {
    if (event->button() != Qt::LeftButton) {
      event->accept();
      return;
    }
    setPressed(true);
    const QPointF p = event->pos() - QRectF(rect()).center();
    m_onNorth = QLineF(p, northAt()).length() < 0.2 * width();
    m_startAngleDeg = angleOf(p);
    m_startHeadingDeg = overlay()->state().headingDeg;
    m_dragged = false;
    event->accept();
  }

  void mouseMoveEvent(QMouseEvent* event) override
  {
    if (!isPressed()) {
      event->ignore();
      return;
    }
    const QPointF p = event->pos() - QRectF(rect()).center();
    const double delta = angleOf(p) - m_startAngleDeg;
    if (!m_dragged && std::abs(delta) < 3.0) {
      event->accept();
      return;
    }
    m_dragged = true;
    // The ring turns with the pointer, and north with it.
    overlay()->notifyHeading(m_startHeadingDeg - delta);
    event->accept();
  }

  void mouseReleaseEvent(QMouseEvent* event) override
  {
    if (isPressed() && !m_dragged && m_onNorth &&
        rect().contains(event->pos())) {
      overlay()->notifyNorthUp();
    }
    setPressed(false);
    event->accept();
  }

  void keyPressEvent(QKeyEvent* event) override
  {
    switch (event->key()) {
    case Qt::Key_Left:
      overlay()->notifyClicked(NavigationAction::TurnLeft);
      break;
    case Qt::Key_Right:
      overlay()->notifyClicked(NavigationAction::TurnRight);
      break;
    case Qt::Key_N:
    case Qt::Key_Space:
    case Qt::Key_Return:
    case Qt::Key_Enter:
      overlay()->notifyNorthUp();
      break;
    default:
      NavControl::keyPressEvent(event);
      return;
    }
    event->accept();
  }

private:
  static double angleOf(QPointF p)
  {
    return std::atan2(p.x(), -p.y()) * 180.0 / pi();
  }

  bool m_onNorth = false;
  bool m_dragged = false;
  double m_startAngleDeg = 0.0;
  double m_startHeadingDeg = 0.0;
};

/** The zoom slider: nearest in at the top; click or drag flies there. */
// No signals or slots of its own: it calls the overlay.
class ZoomSlider : public NavControl // clazy:exclude=missing-qobject-macro
{
public:
  explicit ZoomSlider(NavigationOverlay* overlay)
      : NavControl(overlay, QStringLiteral("navigationZoomSlider"))
  {
  }

  /** The knob's centre for @p fraction. */
  QPointF knobAt(double fraction) const
  {
    const double margin = 0.5 * width();
    return QPointF(0.5 * width(),
                   margin + (1.0 - fraction) * (height() - 2.0 * margin));
  }

protected:
  void paintEvent(QPaintEvent* /*event*/) override
  {
    QPainter painter(this);
    const NavigationColours c = plateColours();
    paintPlate(painter, QRectF(rect()), c, hasFocus(), false);
    QPainterPath track;
    track.moveTo(knobAt(0.0));
    track.lineTo(knobAt(1.0));
    drawSymbol(painter, track, c, std::max(2.0, 0.08 * width()));
    const QPointF knob = knobAt(overlay()->state().zoomFraction);
    const double r = 0.24 * width();
    QPainterPath disc;
    disc.addEllipse(knob, r, r);
    fillSymbol(painter, disc, c.accent, c);
  }

  void mousePressEvent(QMouseEvent* event) override
  {
    if (event->button() == Qt::LeftButton) {
      setPressed(true);
      overlay()->notifyZoom(fractionAt(event->pos().y()));
    }
    event->accept();
  }

  void mouseMoveEvent(QMouseEvent* event) override
  {
    if (isPressed()) {
      overlay()->notifyZoom(fractionAt(event->pos().y()));
    }
    event->accept();
  }

  void mouseReleaseEvent(QMouseEvent* event) override
  {
    setPressed(false);
    event->accept();
  }

  void keyPressEvent(QKeyEvent* event) override
  {
    if (event->key() == Qt::Key_Up) {
      overlay()->notifyClicked(NavigationAction::ZoomIn);
    } else if (event->key() == Qt::Key_Down) {
      overlay()->notifyClicked(NavigationAction::ZoomOut);
    } else {
      NavControl::keyPressEvent(event);
      return;
    }
    event->accept();
  }

private:
  double fractionAt(double yPx) const
  {
    const double margin = 0.5 * width();
    const double span = std::max(height() - 2.0 * margin, 1.0);
    return std::clamp(1.0 - (yPx - margin) / span, 0.0, 1.0);
  }
};

/**
 * The tilt arc: a quarter circle from straight down (its bottom) to the
 * horizon limit; the knob shows the tilt. Click or drag sets it; a double
 * click returns to straight down.
 */
// No signals or slots of its own: it calls the overlay.
class TiltArc : public NavControl // clazy:exclude=missing-qobject-macro
{
public:
  explicit TiltArc(NavigationOverlay* overlay)
      : NavControl(overlay, QStringLiteral("navigationTiltArc"))
  {
  }

  /** The pivot (the eye) and the arc's radius. */
  QPointF pivot() const { return QPointF(0.25 * width(), 0.22 * height()); }
  double radius() const { return 0.62 * std::min(width(), height()); }

  /** The point of the arc for @p tiltDeg. */
  QPointF pointAt(double tiltDeg) const
  {
    const double a = tiltDeg * pi() / 180.0;
    return pivot() + QPointF(radius() * std::sin(a), radius() * std::cos(a));
  }

protected:
  void paintEvent(QPaintEvent* /*event*/) override
  {
    QPainter painter(this);
    const NavigationColours c = plateColours();
    paintPlate(painter, QRectF(rect()), c, hasFocus(), false);
    const double maximum = overlay()->state().maximumTiltDeg;
    QPainterPath arc;
    arc.moveTo(pointAt(0.0));
    for (int32_t i = 1; i <= 24; ++i) {
      arc.lineTo(pointAt(maximum * i / 24.0));
    }
    drawSymbol(painter, arc, c, std::max(2.0, 0.05 * width()));
    const double tilt = overlay()->state().tiltDeg;
    QPainterPath line;
    line.moveTo(pivot());
    line.lineTo(pointAt(tilt));
    drawSymbol(painter, line, c, std::max(1.5, 0.03 * width()));
    const double r = 0.12 * width();
    QPainterPath knob;
    knob.addEllipse(pointAt(tilt), r, r);
    fillSymbol(painter, knob, c.accent, c);
  }

  void mousePressEvent(QMouseEvent* event) override
  {
    if (event->button() == Qt::LeftButton) {
      setPressed(true);
      overlay()->notifyTilt(tiltAt(event->pos()));
    }
    event->accept();
  }

  void mouseMoveEvent(QMouseEvent* event) override
  {
    if (isPressed()) {
      overlay()->notifyTilt(tiltAt(event->pos()));
    }
    event->accept();
  }

  void mouseReleaseEvent(QMouseEvent* event) override
  {
    setPressed(false);
    event->accept();
  }

  void mouseDoubleClickEvent(QMouseEvent* event) override
  {
    setPressed(false);
    overlay()->notifyTilt(0.0);
    event->accept();
  }

  void keyPressEvent(QKeyEvent* event) override
  {
    switch (event->key()) {
    case Qt::Key_Up:
      overlay()->notifyClicked(NavigationAction::TiltUp);
      break;
    case Qt::Key_Down:
      overlay()->notifyClicked(NavigationAction::TiltDown);
      break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
      overlay()->notifyTilt(0.0);
      break;
    default:
      NavControl::keyPressEvent(event);
      return;
    }
    event->accept();
  }

private:
  double tiltAt(QPointF p) const
  {
    const QPointF d = p - pivot();
    const double tilt = std::atan2(d.x(), d.y()) * 180.0 / pi();
    return std::clamp(tilt, 0.0, overlay()->state().maximumTiltDeg);
  }
};

/** The relative luminance of @p c (WCAG 2.1). */
double luminance(const QColor& c)
{
  const std::array<double, 3> channels = {static_cast<double>(c.redF()),
                                          static_cast<double>(c.greenF()),
                                          static_cast<double>(c.blueF())};
  std::array<double, 3> linear{};
  for (std::size_t i = 0; i < 3; ++i) {
    const double v = channels.at(i);
    linear.at(i) =
        v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
  }
  return 0.2126 * linear[0] + 0.7152 * linear[1] + 0.0722 * linear[2];
}

} // namespace

QString overlayCornerName(OverlayCorner corner)
{
  switch (corner) {
  case OverlayCorner::TopLeft:
    return QStringLiteral("top-left");
  case OverlayCorner::BottomLeft:
    return QStringLiteral("bottom-left");
  case OverlayCorner::BottomRight:
    return QStringLiteral("bottom-right");
  case OverlayCorner::TopRight:
    break;
  }
  return QStringLiteral("top-right");
}

OverlayCorner overlayCornerFromName(const QString& name)
{
  for (const OverlayCorner corner :
       {OverlayCorner::TopLeft, OverlayCorner::BottomLeft,
        OverlayCorner::BottomRight}) {
    if (overlayCornerName(corner) == name) {
      return corner;
    }
  }
  return OverlayCorner::TopRight;
}

bool NavigationState::operator==(const NavigationState& other) const
{
  return headingDeg == other.headingDeg && tiltDeg == other.tiltDeg &&
         maximumTiltDeg == other.maximumTiltDeg &&
         zoomFraction == other.zoomFraction && zoomText == other.zoomText;
}

NavigationOverlay::NavigationOverlay(QWidget* view)
    : QWidget(view), m_highContrast(systemHighContrast())
{
  setObjectName(QStringLiteral("navigationOverlay"));
  setAttribute(Qt::WA_TranslucentBackground, true);
  setAttribute(Qt::WA_NoSystemBackground, true);
  setAttribute(Qt::WA_Hover, true);
  setFocusPolicy(Qt::NoFocus);
  // In tab order: the compass, turning, panning and Home, zoom, tilt.
  m_controls = {
      new CompassControl(this),
      new NavButton(this, QStringLiteral("navigationTurnLeft"),
                    Symbol::TurnLeft, NavigationAction::TurnLeft),
      new NavButton(this, QStringLiteral("navigationTurnRight"),
                    Symbol::TurnRight, NavigationAction::TurnRight),
      new NavButton(this, QStringLiteral("navigationPanUp"), Symbol::ArrowUp,
                    NavigationAction::PanUp),
      new NavButton(this, QStringLiteral("navigationPanLeft"),
                    Symbol::ArrowLeft, NavigationAction::PanLeft),
      new NavButton(this, QStringLiteral("navigationHome"), Symbol::Home,
                    std::nullopt),
      new NavButton(this, QStringLiteral("navigationPanRight"),
                    Symbol::ArrowRight, NavigationAction::PanRight),
      new NavButton(this, QStringLiteral("navigationPanDown"),
                    Symbol::ArrowDown, NavigationAction::PanDown),
      new NavButton(this, QStringLiteral("navigationZoomIn"), Symbol::Plus,
                    NavigationAction::ZoomIn),
      new ZoomSlider(this),
      new NavButton(this, QStringLiteral("navigationZoomOut"), Symbol::Minus,
                    NavigationAction::ZoomOut),
      new NavButton(this, QStringLiteral("navigationTiltUp"), Symbol::TiltUp,
                    NavigationAction::TiltUp),
      new NavButton(this, QStringLiteral("navigationTiltDown"),
                    Symbol::TiltDown, NavigationAction::TiltDown),
      new TiltArc(this),
  };
  for (QWidget* c : m_controls) {
    c->installEventFilter(this);
  }
  view->installEventFilter(this);
  retranslate();
  relayout();
}

NavigationOverlay::~NavigationOverlay() = default;

bool NavigationOverlay::systemHighContrast()
{
#ifdef Q_OS_WIN
  HIGHCONTRASTW contrast{};
  contrast.cbSize = sizeof(contrast);
  if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast,
                            0) != 0) {
    return (contrast.dwFlags & HCF_HIGHCONTRASTON) != 0;
  }
  return false;
#else
  const QString theme = qEnvironmentVariable("GTK_THEME");
  return theme.contains(QLatin1String("HighContrast"), Qt::CaseInsensitive);
#endif
}

NavigationColours NavigationOverlay::colours(bool hovered, bool pressed) const
{
  NavigationColours c;
  if (m_highContrast) {
    const QPalette palette = QApplication::palette();
    c.plate = pressed || hovered ? palette.color(QPalette::Highlight)
                                 : palette.color(QPalette::Window);
    c.edge = palette.color(QPalette::WindowText);
    c.symbol = pressed || hovered ? palette.color(QPalette::HighlightedText)
                                  : palette.color(QPalette::WindowText);
    c.halo = c.plate;
    c.accent = c.symbol;
    return c;
  }
  // Brightness, not only transparency, marks hover (lighter) and press
  // (darker). The plates stay dark enough, and the edge white, for one of
  // them to stand 3:1 against any background (docs/design/
  // camera-and-precision.md).
  QColor plate(22, 26, 32);
  if (pressed) {
    plate = QColor(6, 8, 12);
  } else if (hovered) {
    plate = QColor(50, 56, 66);
  }
  plate.setAlphaF(static_cast<ColourChannel>(plateOpacity()));
  c.plate = plate;
  c.edge = QColor(255, 255, 255);
  c.symbol = QColor(242, 244, 247);
  c.halo = QColor(0, 0, 0, 220);
  c.accent = QColor(255, 120, 100);
  return c;
}

double NavigationOverlay::plateOpacity() const
{
  if (m_highContrast) {
    return 1.0;
  }
  return m_active ? 0.92 : idlePlateOpacity;
}

void NavigationOverlay::setState(const NavigationState& state)
{
  if (state == m_state) {
    return;
  }
  const bool zoomTextChanged = state.zoomText != m_state.zoomText;
  m_state = state;
  if (zoomTextChanged) {
    retranslate();
  }
  for (QWidget* c : m_controls) {
    c->update();
  }
}

void NavigationOverlay::setCorner(OverlayCorner corner)
{
  m_corner = corner;
  place();
}

void NavigationOverlay::setHighContrast(bool highContrast)
{
  m_highContrast = highContrast;
  for (QWidget* c : m_controls) {
    c->update();
  }
}

QWidget* NavigationOverlay::control(const QString& objectName) const
{
  for (QWidget* c : m_controls) {
    if (c->objectName() == objectName) {
      return c;
    }
  }
  return nullptr;
}

std::vector<QWidget*> NavigationOverlay::controls() const
{
  return m_controls;
}

void NavigationOverlay::relayout()
{
  // At least 9 mm a target (touch), and larger with a larger font.
  const double mmPx = logicalDpiY() / 25.4;
  const int32_t fontPx = QFontMetrics(font()).height();
  m_unitPx = std::max(static_cast<int32_t>(std::lround(9.0 * mmPx)),
                      static_cast<int32_t>(std::lround(2.1 * fontPx)));
  const int32_t u = m_unitPx;
  const int32_t g = std::max(3, u / 6);
  const int32_t width = 4 * u + 2 * g;
  const std::function<void(const QString&, int32_t, int32_t, int32_t, int32_t)>
      put = [this](const QString& name, int32_t x, int32_t y, int32_t w,
                   int32_t h) {
        QWidget* c = control(name);
        if (c != nullptr) {
          c->setGeometry(x, y, w, h);
        }
      };
  // The compass between the turn buttons.
  put(QStringLiteral("navigationTurnLeft"), 0, u / 2, u, u);
  put(QStringLiteral("navigationCompass"), u + g, 0, 2 * u, 2 * u);
  put(QStringLiteral("navigationTurnRight"), 3 * u + 2 * g, u / 2, u, u);
  // The pan pad, Home in its middle.
  const int32_t padX = (width - 3 * u - 2 * g) / 2;
  const int32_t padY = 2 * u + 2 * g;
  put(QStringLiteral("navigationPanUp"), padX + u + g, padY, u, u);
  put(QStringLiteral("navigationPanLeft"), padX, padY + u + g, u, u);
  put(QStringLiteral("navigationHome"), padX + u + g, padY + u + g, u, u);
  put(QStringLiteral("navigationPanRight"), padX + 2 * (u + g), padY + u + g, u,
      u);
  put(QStringLiteral("navigationPanDown"), padX + u + g, padY + 2 * (u + g), u,
      u);
  // Zoom on the left, tilt on the right.
  const int32_t lowY = padY + 3 * u + 4 * g;
  const int32_t zoomX = padX;
  put(QStringLiteral("navigationZoomIn"), zoomX, lowY, u, u);
  put(QStringLiteral("navigationZoomSlider"), zoomX, lowY + u + g, u, 2 * u);
  put(QStringLiteral("navigationZoomOut"), zoomX, lowY + 3 * u + 2 * g, u, u);
  const int32_t tiltX = zoomX + u + 2 * g;
  put(QStringLiteral("navigationTiltUp"), tiltX, lowY, u, u);
  put(QStringLiteral("navigationTiltDown"), tiltX + u + g, lowY, u, u);
  put(QStringLiteral("navigationTiltArc"), tiltX, lowY + u + g, 2 * u + g,
      2 * u + g);
  setFixedSize(width, lowY + 4 * u + 2 * g);
  place();
}

void NavigationOverlay::place()
{
  const QWidget* view = parentWidget();
  if (view == nullptr) {
    return;
  }
  const int32_t margin = std::max(6, m_unitPx / 3);
  // The bottom corners keep clear of the scale bar and the attributions.
  const int32_t bottomMargin = margin + 2 * m_unitPx;
  const bool left = m_corner == OverlayCorner::TopLeft ||
                    m_corner == OverlayCorner::BottomLeft;
  const bool top =
      m_corner == OverlayCorner::TopLeft || m_corner == OverlayCorner::TopRight;
  const int32_t x = left ? margin : view->width() - width() - margin;
  const int32_t y = top ? margin : view->height() - height() - bottomMargin;
  move(std::max(0, x), std::max(0, y));
}

void NavigationOverlay::retranslate()
{
  // NOLINTBEGIN(bugprone-easily-swappable-parameters): name, then texts
  const std::function<void(const QString&, const QString&, const QString&)>
      describe = [this](const QString& name, const QString& accessible,
                        const QString& tip) {
        QWidget* c = control(name);
        if (c != nullptr) {
          c->setAccessibleName(accessible);
          c->setToolTip(tip);
        }
      };
  // NOLINTEND(bugprone-easily-swappable-parameters)
  describe(QStringLiteral("navigationCompass"), tr("Compass"),
           tr("Drag the ring to turn the view; click N to turn north up "
              "(N when focused)"));
  describe(QStringLiteral("navigationTurnLeft"), tr("Turn left"),
           tr("Turn left; hold to keep turning (Q)"));
  describe(QStringLiteral("navigationTurnRight"), tr("Turn right"),
           tr("Turn right; hold to keep turning (E)"));
  describe(QStringLiteral("navigationPanUp"), tr("Move forward"),
           tr("Move forward; hold to keep moving (W or Up)"));
  describe(QStringLiteral("navigationPanDown"), tr("Move back"),
           tr("Move back; hold to keep moving (S or Down)"));
  describe(QStringLiteral("navigationPanLeft"), tr("Move left"),
           tr("Move left; hold to keep moving (A or Left)"));
  describe(QStringLiteral("navigationPanRight"), tr("Move right"),
           tr("Move right; hold to keep moving (D or Right)"));
  describe(QStringLiteral("navigationHome"), tr("Reset view"),
           tr("Back to the start view (Home)"));
  describe(QStringLiteral("navigationZoomIn"), tr("Zoom in"),
           tr("Zoom in; hold to keep zooming (+ or Page Up)"));
  describe(QStringLiteral("navigationZoomOut"), tr("Zoom out"),
           tr("Zoom out; hold to keep zooming (− or Page Down)"));
  describe(
      QStringLiteral("navigationZoomSlider"), tr("Zoom"),
      m_state.zoomText.isEmpty()
          ? tr("Click or drag to fly to a height")
          : tr("%1. Click or drag to fly to a height").arg(m_state.zoomText));
  describe(QStringLiteral("navigationTiltUp"), tr("Tilt up"),
           tr("Tilt towards the horizon; hold to keep tilting (R)"));
  describe(QStringLiteral("navigationTiltDown"), tr("Tilt down"),
           tr("Tilt towards straight down; hold to keep tilting (F)"));
  describe(QStringLiteral("navigationTiltArc"), tr("Tilt"),
           tr("Click or drag to tilt; double-click for straight down"));
  QWidget* slider = control(QStringLiteral("navigationZoomSlider"));
  if (slider != nullptr) {
    slider->setAccessibleDescription(m_state.zoomText);
  }
}

void NavigationOverlay::setPointerOver(bool over)
{
  if (m_active != over) {
    m_active = over;
    for (QWidget* c : m_controls) {
      c->update();
    }
  }
}

bool NavigationOverlay::eventFilter(QObject* watched, QEvent* event)
{
  if (watched == parentWidget() && event->type() == QEvent::Resize) {
    place();
  }
  if (watched != parentWidget() &&
      (event->type() == QEvent::Enter || event->type() == QEvent::HoverEnter)) {
    setPointerOver(true);
  }
  return QWidget::eventFilter(watched, event);
}

bool NavigationOverlay::event(QEvent* event)
{
  switch (event->type()) {
  case QEvent::Enter:
  case QEvent::HoverEnter:
    setPointerOver(true);
    break;
  case QEvent::Leave:
  case QEvent::HoverLeave:
    setPointerOver(underMouse());
    break;
  case QEvent::LanguageChange:
    retranslate();
    break;
  case QEvent::FontChange:
  case QEvent::ScreenChangeInternal:
    relayout();
    break;
  default:
    break;
  }
  return QWidget::event(event);
}

// The overlay's own area (between the controls) keeps clicks from the view.
void NavigationOverlay::mousePressEvent(QMouseEvent* event)
{
  event->accept();
}

void NavigationOverlay::mouseReleaseEvent(QMouseEvent* event)
{
  event->accept();
}

void NavigationOverlay::mouseDoubleClickEvent(QMouseEvent* event)
{
  event->accept();
}

void NavigationOverlay::wheelEvent(QWheelEvent* event)
{
  event->accept();
}

void NavigationOverlay::notifyHeld(NavigationAction action, bool held)
{
  emit actionHeld(action, held);
}

void NavigationOverlay::notifyClicked(NavigationAction action)
{
  emit actionClicked(action);
}

void NavigationOverlay::notifyHeading(double headingDeg)
{
  emit headingRequested(headingDeg);
}

void NavigationOverlay::notifyNorthUp()
{
  emit northUpRequested();
}

void NavigationOverlay::notifyHome()
{
  emit homeRequested();
}

void NavigationOverlay::notifyZoom(double fraction)
{
  emit zoomRequested(fraction);
}

void NavigationOverlay::notifyTilt(double tiltDeg)
{
  emit tiltRequested(tiltDeg);
}

double navigationContrastRatio(const QColor& a, const QColor& b)
{
  const double la = luminance(a);
  const double lb = luminance(b);
  return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

QColor navigationComposite(const QColor& over, const QColor& under)
{
  const double a = static_cast<double>(over.alphaF());
  const std::function<ColourChannel(ColourChannel, ColourChannel)> mix =
      [a](ColourChannel top, ColourChannel bottom) {
        return static_cast<ColourChannel>(a * static_cast<double>(top) +
                                          (1.0 - a) *
                                              static_cast<double>(bottom));
      };
  return QColor::fromRgbF(mix(over.redF(), under.redF()),
                          mix(over.greenF(), under.greenF()),
                          mix(over.blueF(), under.blueF()));
}

} // namespace ts
