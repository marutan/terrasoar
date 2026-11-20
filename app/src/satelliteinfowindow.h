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
 * The window of a satellite's details, shown when it is clicked.
 */
#ifndef TS_APP_SATELLITEINFOWINDOW_H
#define TS_APP_SATELLITEINFOWINDOW_H

#include <cstdint>
#include <optional>

#include <QDateTime>
#include <QDialog>

#include "satellitelayers.h"
#include "ts/geo/geodetic.h"
#include "ts/orbits/satellitepasses.h"

class QEvent;
class QPushButton;
class QTextBrowser;

namespace ts
{

/**
 * A satellite's details (not modal): its name, catalogue number and
 * international designator, its elements' epoch and age (marked when old,
 * with Refresh Elements offered), where it is (latitude, longitude,
 * altitude), its speed, period and inclination, and its next pass over
 * the point under the pointer. Kept up to date as the clock moves.
 *
 * @guithread
 */
class SatelliteInfoWindow : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(SatelliteInfoWindow)

public:
  /** Creates the window. */
  explicit SatelliteInfoWindow(QWidget* parent);

  /** Destroys it. */
  ~SatelliteInfoWindow() override;

  /**
   * Shows @p satellite, of the layer named @p layerName drawn with
   * @p style, at @p time; @p canRefresh offers Refresh Elements.
   */
  void showSatellite(const DrawnSatellite& satellite,
                     const SatelliteStyle& style, const QString& layerName,
                     const QDateTime& time, bool canRefresh);

  /**
   * Sets the point passes are found for (the ground under the pointer);
   * nothing for none.
   */
  void setObserver(const std::optional<Geodetic>& observer);

  /** Returns the text shown (for tests). */
  QString text() const;

  /** Returns the pass shown, if any (for tests). */
  const std::optional<SatellitePass>& pass() const { return m_pass; }

  /** Returns the catalogue number shown, −1 for none. */
  int32_t catalogueNumber() const;

signals:
  /** The user asked for the satellite's elements to be refreshed. */
  void refreshRequested();

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

private:
  void render();
  void updatePass();

  QTextBrowser* m_browser = nullptr;
  QPushButton* m_refresh = nullptr;
  std::optional<DrawnSatellite> m_satellite;
  SatelliteStyle m_style;
  QString m_layerName;
  QDateTime m_time;
  bool m_canRefresh = false;
  std::optional<Geodetic> m_observer;
  std::optional<SatellitePass> m_pass;
  /** What m_pass was found for: satellite, observer and from when. */
  int32_t m_passSatellite = -1;
  std::optional<Geodetic> m_passObserver;
  JulianDate m_passFrom;
  bool m_passSearched = false;
};

} // namespace ts

#endif // TS_APP_SATELLITEINFOWINDOW_H
