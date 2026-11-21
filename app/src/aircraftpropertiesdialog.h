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
 * An aircraft layer's properties: markers, labels, trails, colours and
 * filters.
 */
#ifndef TS_APP_AIRCRAFTPROPERTIESDIALOG_H
#define TS_APP_AIRCRAFTPROPERTIESDIALOG_H

#include <functional>
#include <optional>

#include <QColor>
#include <QDialog>
#include <QPixmap>

#include "ts/adsb/aircraftlayer.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace ts
{

/**
 * Returns a legend of the altitude colours (altitudeStops()), @p widthPx
 * wide: the scale with its heights in feet.
 */
QPixmap altitudeLegend(int32_t widthPx);

/** Edits an aircraft layer's style. */
class AircraftPropertiesDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(AircraftPropertiesDialog)

public:
  /** Returns the area in view, if there is one (for the area filter). */
  using ViewArea = std::function<std::optional<LatLonBox>()>;

  /** Edits @p style of layer @p name. */
  AircraftPropertiesDialog(const AircraftStyle& style, const QString& name,
                           ViewArea viewArea, QWidget* parent);
  ~AircraftPropertiesDialog() override;

  /** Returns the style as edited. */
  AircraftStyle style() const;

private:
  void chooseColor();
  void updateSwatch();
  void updateEnabled();

  AircraftStyle m_style;
  QString m_name;
  ViewArea m_viewArea;
  QColor m_color;
  QPushButton* m_colorButton;
  QSpinBox* m_markerSize;
  QCheckBox* m_labels;
  QCheckBox* m_labelAltitude;
  QCheckBox* m_labelSpeed;
  QCheckBox* m_dropLines;
  QComboBox* m_trailMode;
  QDoubleSpinBox* m_trailSpan;
  QComboBox* m_colouring;
  QLabel* m_legend;
  QDoubleSpinBox* m_stale;
  QDoubleSpinBox* m_gap;
  QLineEdit* m_callsignFilter;
  QLineEdit* m_addressFilter;
  QCheckBox* m_hasMinimum;
  QSpinBox* m_minimum;
  QCheckBox* m_hasMaximum;
  QSpinBox* m_maximum;
  QCheckBox* m_hasArea;
  QDoubleSpinBox* m_south;
  QDoubleSpinBox* m_west;
  QDoubleSpinBox* m_north;
  QDoubleSpinBox* m_east;
  QPushButton* m_useView;
};

} // namespace ts

#endif // TS_APP_AIRCRAFTPROPERTIESDIALOG_H
