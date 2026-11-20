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
 * The list of a satellite layer's satellites: which are shown, a filter,
 * their elements' age and own track spans.
 */
#ifndef TS_APP_SATELLITELISTDIALOG_H
#define TS_APP_SATELLITELISTDIALOG_H

#include <cstdint>
#include <vector>

#include <QDialog>
#include <QString>

class QEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QSortFilterProxyModel;
class QStandardItem;
class QStandardItemModel;
class QTableView;

namespace ts
{

class LayerTreeModel;
class ProgramClock;
class SatelliteLayers;

/**
 * Lists the satellites of one satellite layer: a check box for each
 * (shown or hidden), its name and catalogue number, how old its elements
 * are at the clock's time (marked when older than the layer allows) and
 * whether it has its own track spans. The filter matches names and
 * numbers. Show All, Hide All and Show Only These (those the filter
 * leaves) change many at once; Track Spans sets the selected satellites'
 * own spans. Selecting a row selects the satellite on the globe.
 *
 * Changes go straight into the layer's style (in the project).
 *
 * @guithread
 */
class SatelliteListDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(SatelliteListDialog)

public:
  /** Creates the list of layer @p nodeId of @p layers. */
  SatelliteListDialog(QString nodeId, LayerTreeModel* layers,
                      SatelliteLayers* satellites, ProgramClock* clock,
                      QWidget* parent);

  /** Destroys it. */
  ~SatelliteListDialog() override;

  /** Returns the layer listed. */
  const QString& nodeId() const { return m_nodeId; }

  /** Sets the filter (as typing it). */
  void setFilter(const QString& text);

  /** Returns how many rows the filter leaves. */
  int32_t visibleRows() const;

  /** Returns how many satellites are listed. */
  int32_t rows() const;

  /** Checks or unchecks satellite @p catalogueNumber (as clicking it). */
  bool setShown(int32_t catalogueNumber, bool shown);

  /** Shows only the satellites the filter leaves. */
  void showOnlyFiltered();

  /** Shows (@p shown) or hides every satellite. */
  void setAllShown(bool shown);

  /** Selects the row of @p catalogueNumber (and the satellite). */
  bool selectSatellite(int32_t catalogueNumber);

  /** Fills the list again (the layer's satellites or style changed). */
  void reload();

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

private:
  void retranslateUi();
  void itemChanged(QStandardItem* item);
  void rowSelected();
  void editSpans();
  void writeStyle(const QJsonObject& style);
  std::vector<int32_t> selectedNumbers() const;

  QString m_nodeId;
  LayerTreeModel* m_layers;
  SatelliteLayers* m_satellites;
  ProgramClock* m_clock;
  QStandardItemModel* m_model = nullptr;
  QSortFilterProxyModel* m_filterModel = nullptr;
  QTableView* m_table = nullptr;
  QLineEdit* m_filter = nullptr;
  QLabel* m_count = nullptr;
  QPushButton* m_showAll = nullptr;
  QPushButton* m_hideAll = nullptr;
  QPushButton* m_showOnly = nullptr;
  QPushButton* m_spans = nullptr;
  /** Set while filling the list (item changes aren't the user's). */
  bool m_filling = false;
};

} // namespace ts

#endif // TS_APP_SATELLITELISTDIALOG_H
