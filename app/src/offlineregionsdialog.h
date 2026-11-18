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
 * The window that lists the offline regions.
 */
#ifndef TS_APP_OFFLINEREGIONSDIALOG_H
#define TS_APP_OFFLINEREGIONSDIALOG_H

#include <cstdint>

#include <QDialog>
#include <QPointer>

class QEvent;
class QLabel;
class QPushButton;
class QTreeWidget;

namespace ts
{

class GlobeView;
class OfflineRegionManager;

/**
 * Lists the offline regions with their layer, levels, tiles, size and
 * state (downloading with progress, paused and why, complete, or not
 * finished), and lets the user start a new one, pause and resume a
 * download, update a region (fetching what is missing or stale) and
 * delete one. The selected region's outline is drawn on the globe.
 * Not modal: downloads go on whether it is open or not.
 *
 * @guithread
 */
class OfflineRegionsDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(OfflineRegionsDialog)

public:
  /**
   * The user guide's section on this window
   * (`offline-use.md#managing-regions`), opened by F1.
   */
  static QString helpTarget();

  /**
   * Shows @p manager's regions.
   * @param manager The regions.
   * @param view Where the selected region's outline is drawn; may be null.
   * @param parent Usual QWidget parent.
   */
  OfflineRegionsDialog(OfflineRegionManager& manager, GlobeView* view,
                       QWidget* parent = nullptr);

  /** Destroys the dialog. */
  ~OfflineRegionsDialog() override;

  /** Returns the list (tests). */
  QTreeWidget* list() const { return m_list; }

  /** Returns the id of the selected region, or 0. */
  int64_t selectedRegion() const;

  /** Selects the region @p id. */
  void selectRegion(int64_t id);

signals:
  /** The user asked for a new region. */
  void newRegionRequested();

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

  /** Hides the outline on the globe. */
  void done(int32_t result) override;

private:
  void retranslateUi();
  void fill();
  void updateProgress();
  void updateButtons();
  void showOutline();
  void pauseOrResume();
  void deleteRegion();

  OfflineRegionManager& m_manager;
  QPointer<GlobeView> m_view;
  QTreeWidget* m_list = nullptr;
  QLabel* m_emptyLabel = nullptr;
  QPushButton* m_newButton = nullptr;
  QPushButton* m_pauseButton = nullptr;
  QPushButton* m_deleteButton = nullptr;
  QPushButton* m_closeButton = nullptr;
};

} // namespace ts

#endif // TS_APP_OFFLINEREGIONSDIALOG_H
