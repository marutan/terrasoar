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
 * The stations of a ship layer listed, and one station's details.
 */
#ifndef TS_APP_VESSELWINDOWS_H
#define TS_APP_VESSELWINDOWS_H

#include <cstdint>
#include <optional>

#include <QDialog>
#include <QPoint>
#include <QString>

#include "vessellayers.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace ts
{

class LayerTreeModel;

/**
 * Lists a ship layer's stations: to find one, show or hide each, and give
 * one its own trail.
 */
class VesselListDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(VesselListDialog)

public:
  /** Lists layer @p nodeId (the models must outlive it). */
  VesselListDialog(QString nodeId, LayerTreeModel* layers,
                   VesselLayers* vessels, QWidget* parent);
  ~VesselListDialog() override;

  /** Returns the layer listed. */
  const QString& nodeId() const { return m_nodeId; }

  /** Selects the row of station @p mmsi. */
  void selectVessel(uint32_t mmsi);

  /** Returns the number of rows shown (for tests). */
  int32_t rowCount() const;

  /** Sets the filter text (for tests). */
  void setFilter(const QString& text);

signals:
  /** The user chose station @p mmsi (double click). */
  void vesselChosen(const QString& nodeId, uint32_t mmsi);

private:
  void refill();
  void addRow(const VesselTrack& track, const VesselStyle& style, double timeS);
  void itemChanged(QTreeWidgetItem* item, int32_t column);
  void setAllShown(bool shown);
  void setTrail(uint32_t mmsi, const std::optional<TrailSettings>& trail);
  void showTrailMenu(QPoint at);

  QString m_nodeId;
  LayerTreeModel* m_layers;
  VesselLayers* m_vessels;
  QLineEdit* m_filter;
  QTreeWidget* m_list;
  QLabel* m_summary;
  bool m_filling = false;
};

/** One station's details, following the clock. */
class VesselInfoWindow : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(VesselInfoWindow)

public:
  /** Creates the window. */
  explicit VesselInfoWindow(QWidget* parent);
  ~VesselInfoWindow() override;

  /** Shows @p vessel of layer @p layerName at @p timeS. */
  void showVessel(const DrawnVessel& vessel, const QString& layerName,
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
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  void setField(const QString& name, const QString& text);

  QLabel* m_title;
  QPushButton* m_follow;
  QPushButton* m_export;
};

} // namespace ts

#endif // TS_APP_VESSELWINDOWS_H
