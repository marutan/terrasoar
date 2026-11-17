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
 * The Data Packs window.
 */
#ifndef TS_APP_PACKMANAGERDIALOG_H
#define TS_APP_PACKMANAGERDIALOG_H

#include <QDialog>
#include <QString>

#include "ts/packs/packinstall.h"

class QLabel;
class QPushButton;
class QTreeWidget;

namespace ts
{

class PackManager;

/**
 * Lists the extra data packs: those in the online index, installed ones
 * and downloads, with version, size and state (not installed, installed,
 * update available, downloading with progress, paused, failed, damaged).
 * Installs, updates, pauses, resumes, cancels and removes them. Reads the
 * index when opened. Not modal: installs go on whether it is open or not.
 *
 * @guithread
 */
class PackManagerDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(PackManagerDialog)

public:
  /** What the main button does for a pack. */
  enum class Action {
    None,      ///< Nothing to do (disabled).
    Install,   ///< Install it.
    Update,    ///< Install the index's newer (or other) version.
    Reinstall, ///< Install again (it is damaged).
    Resume,    ///< Carry on with its download.
    Retry,     ///< Try its failed download again.
    Pause,     ///< Pause its download.
  };

  /** What the second button does for a pack. */
  enum class Removal {
    None,           ///< Nothing (disabled).
    Remove,         ///< Remove the installed pack.
    CancelDownload, ///< Cancel the download and delete it.
  };

  /** A pack as the list shows it. */
  struct Row {
    /** Name for the user. */
    QString name;
    /** The installed version, else the index's. */
    QString version;
    /** Size, bytes (0: unknown). */
    qint64 sizeBytes = 0;
    /** Its state, a phrase for the user. */
    QString state;
    /** Description and licence from the index. */
    QString details;
    /** What the main button does. */
    Action action = Action::None;
    /** What the second button does. */
    Removal removal = Removal::None;
  };

  /**
   * Shows @p manager's packs.
   * @param manager The packs.
   * @param parent Usual QWidget parent.
   */
  explicit PackManagerDialog(PackManager& manager, QWidget* parent = nullptr);

  /** Destroys the dialog. */
  ~PackManagerDialog() override;

  /** Returns the list (tests). */
  QTreeWidget* list() const { return m_list; }

  /** Returns the id of the selected pack, or an empty string. */
  QString selectedPack() const;

  /** Selects the pack @p id. */
  void selectPack(const QString& id);

  /** Returns how pack @p id is shown. */
  Row row(const QString& id) const;

  /**
   * Fills the state, action and removal of @p row from a download's
   * @p progress if it runs, is paused or failed; false otherwise.
   * @p installed: an earlier version is installed.
   */
  static bool
  describeDownload(Row* row, const std::optional<PackInstallProgress>& progress,
                   bool installed);

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

private:
  void retranslateUi();
  void fill();
  void updateRows();
  void updateButtons();
  void act();
  void removeOrCancel();

  PackManager& m_manager;
  QTreeWidget* m_list = nullptr;
  QLabel* m_detailsLabel = nullptr;
  QLabel* m_indexLabel = nullptr;
  QPushButton* m_actionButton = nullptr;
  QPushButton* m_removeButton = nullptr;
  QPushButton* m_refreshButton = nullptr;
  QPushButton* m_closeButton = nullptr;
};

} // namespace ts

#endif // TS_APP_PACKMANAGERDIALOG_H
