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
 * The aircraft of an aircraft layer listed, and one aircraft's details.
 */
#ifndef TS_APP_AIRCRAFTWINDOWS_H
#define TS_APP_AIRCRAFTWINDOWS_H

#include <cstdint>
#include <optional>

#include <QDialog>
#include <QString>

#include "aircraftlayers.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace ts
{

class LayerTreeModel;

/**
 * Lists an aircraft layer's aircraft with what they are and where, to
 * find one, show or hide each, and give one its own trail.
 */
class AircraftListDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(AircraftListDialog)

public:
  /** Lists layer @p nodeId of @p layers (both must outlive it). */
  AircraftListDialog(QString nodeId, LayerTreeModel* layers,
                     AircraftLayers* aircraft, QWidget* parent);
  ~AircraftListDialog() override;

  /** Returns the layer listed. */
  const QString& nodeId() const { return m_nodeId; }

  /** Selects the row of aircraft @p address. */
  void selectAircraft(uint32_t address);

  /** Returns the number of rows shown (for tests). */
  int32_t rowCount() const;

  /** Sets the filter text (for tests). */
  void setFilter(const QString& text);

signals:
  /** The user chose aircraft @p address (double click). */
  void aircraftChosen(const QString& nodeId, uint32_t address);

private:
  void refill();
  void itemChanged(QTreeWidgetItem* item, int32_t column);
  void setAllShown(bool shown);
  void setTrail(uint32_t address, const std::optional<TrailSettings>& trail);
  void showTrailMenu(QPoint at);
  void addRow(const AircraftTrack& track, const AircraftStyle& style,
              double timeS);

  QString m_nodeId;
  LayerTreeModel* m_layers;
  AircraftLayers* m_aircraft;
  QLineEdit* m_filter;
  QTreeWidget* m_list;
  QLabel* m_summary;
  bool m_filling = false;
};

/** One aircraft's details, following the clock. */
class AircraftInfoWindow : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(AircraftInfoWindow)

public:
  /** Creates the window. */
  explicit AircraftInfoWindow(QWidget* parent);
  ~AircraftInfoWindow() override;

  /** Shows @p aircraft of layer @p layerName at @p timeS. */
  void showAircraft(const DrawnAircraft& aircraft, const QString& layerName,
                    double timeS);

  /** Sets the Follow button without signalling. */
  void setFollowing(bool following);

  /** Returns the text shown for a field (for tests), by object name. */
  QString fieldText(const QString& name) const;

signals:
  /** The Follow button was toggled. */
  void followToggled(bool follow);

  /** Export Track was pressed. */
  void exportRequested();

private:
  QLabel* field(const QString& name);

  QLabel* m_title;
  QPushButton* m_follow;
  QPushButton* m_export;
};

} // namespace ts

#endif // TS_APP_AIRCRAFTWINDOWS_H
