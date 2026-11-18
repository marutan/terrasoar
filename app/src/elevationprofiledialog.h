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
 * The Elevation Profile window: a chart of the heights along a line drawn
 * on the globe.
 */
#ifndef TS_APP_ELEVATIONPROFILEDIALOG_H
#define TS_APP_ELEVATIONPROFILEDIALOG_H

#include <cstdint>
#include <optional>

#include <QDialog>
#include <QWidget>

#include "ts/elevation/elevationprofile.h"

class QEvent;
class QLabel;
class QMouseEvent;
class QPaintEvent;
class QPushButton;

namespace ts
{

/**
 * Draws an elevation profile: height above sea level against distance,
 * the ground filled below the line, with the scale's ends labelled. The
 * pointer's position on it is marked and reported (hovered()).
 */
class ElevationProfileChart : public QWidget
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(ElevationProfileChart)

public:
  /** Creates an empty chart. */
  explicit ElevationProfileChart(QWidget* parent);
  ~ElevationProfileChart() override;

  /** Shows @p profile (nothing: an empty chart). */
  void setProfile(std::optional<ElevationProfile> profile);

  /** Returns the profile shown. */
  const std::optional<ElevationProfile>& profile() const { return m_profile; }

  /**
   * Returns the index of the profile point at horizontal position @p xPx,
   * or nothing outside the chart or without a profile.
   */
  std::optional<std::size_t> pointAt(double xPx) const;

  /** A size good for a chart. */
  QSize sizeHint() const override;

signals:
  /** The pointer is over point @p index of the profile (or left it: −1). */
  void hovered(int32_t index);

protected:
  /** Draws the chart. */
  void paintEvent(QPaintEvent* event) override;
  /** Marks the point under the pointer and reports it (hovered()). */
  void mouseMoveEvent(QMouseEvent* event) override;
  /** Clears the mark when the pointer leaves. */
  void leaveEvent(QEvent* event) override;

private:
  /** The plotting area, inside the axis labels. */
  QRectF plotArea() const;

  std::optional<ElevationProfile> m_profile;
  int32_t m_hovered = -1;
};

/**
 * The Elevation Profile window (not modal): the chart of the last line
 * drawn, its length, lowest and highest point, climb and descent, and
 * buttons to draw another line and save the profile as CSV. The window
 * only shows; the main window draws lines and computes profiles.
 */
class ElevationProfileDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(ElevationProfileDialog)

public:
  /**
   * The user guide's section on this window (`terrain.md#elevation-profiles`),
   * opened by F1.
   */
  static QString helpTarget();

  /** Creates the window, waiting for a line. */
  explicit ElevationProfileDialog(QWidget* parent);
  ~ElevationProfileDialog() override;

  /** Says a line is being drawn. */
  void setDrawing();
  /** Says the profile of a line is being computed. */
  void setComputing();
  /** Shows @p profile. */
  void setProfile(const ElevationProfile& profile);
  /** Says why there is no profile (translated). */
  void setFailed(const QString& message);

  /** Returns the chart. */
  ElevationProfileChart* chart() const { return m_chart; }

  /** Returns the summary line (length, heights, climb). */
  QString summaryText() const;

  /**
   * Returns @p profile as CSV: a header, then distance (m), latitude,
   * longitude, height above sea level and above the ellipsoid (m) per
   * point, with a full stop for decimals whatever the locale.
   */
  static QByteArray toCsv(const ElevationProfile& profile);

signals:
  /** The user asked to draw another line. */
  void drawAnotherRequested();
  /** The pointer is over this point of the profile (or none). */
  void pointHovered(std::optional<ts::GeoPoint> point);

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

private:
  void retranslateUi();
  void saveCsv();

  ElevationProfileChart* m_chart = nullptr;
  QLabel* m_summaryLabel = nullptr;
  QLabel* m_statusLabel = nullptr;
  QPushButton* m_drawButton = nullptr;
  QPushButton* m_saveButton = nullptr;
  QPushButton* m_closeButton = nullptr;
  /** What the status label says, so it can be retranslated. */
  enum class State { Waiting, Drawing, Computing, Ready, Failed };
  State m_state = State::Waiting;
  QString m_failure;
};

} // namespace ts

Q_DECLARE_METATYPE(std::optional<ts::GeoPoint>)

#endif // TS_APP_ELEVATIONPROFILEDIALOG_H
