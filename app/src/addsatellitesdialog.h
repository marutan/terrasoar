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
 * The Add Satellites dialog: a CelesTrak group, a TLE or OMM file, or
 * element sets pasted in.
 */
#ifndef TS_APP_ADDSATELLITESDIALOG_H
#define TS_APP_ADDSATELLITESDIALOG_H

#include <optional>

#include <QDialog>
#include <QUrl>

#include "ts/layers/layerdefinition.h"

class QEvent;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTabWidget;
class QTreeWidget;

namespace ts
{

/**
 * Asks where satellites come from: one of CelesTrak's groups (by
 * category, with a filter), a TLE or OMM JSON file, or TLEs pasted in
 * (two-line or three-line, checksums checked). Files and text are read
 * before the dialog closes, so what can't be read is said at once.
 *
 * CelesTrak's address can be replaced for tests (TS_CELESTRAK_URL).
 *
 * @guithread
 */
class AddSatellitesDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(AddSatellitesDialog)

public:
  /** Creates the dialog. */
  explicit AddSatellitesDialog(QWidget* parent);

  /** Destroys it. */
  ~AddSatellitesDialog() override;

  /** Returns the layer chosen, once accepted. */
  const std::optional<LayerDefinition>& layerDefinition() const
  {
    return m_definition;
  }

  /** Shows the CelesTrak tab with @p group chosen (for tests). */
  void chooseGroup(const QString& group);

  /** Shows the File tab with @p path (for tests). */
  void chooseFile(const QString& path);

  /** Shows the Text tab with @p text (for tests). */
  void chooseText(const QString& text);

  /** Returns the message shown under the tabs (for tests). */
  QString message() const;

  /** Returns CelesTrak's address (or the tests'). */
  static QUrl celestrakUrl();

  /** Makes a satellite layer's definition of @p source named @p name. */
  static LayerDefinition definitionFor(const QJsonObject& source,
                                       const QString& name);

  /** Accepts what is chosen if it can be read. */
  void accept() override;

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

private:
  void retranslateUi();
  void fillGroups();
  void filterGroups(const QString& text);
  void browse();

  QTabWidget* m_tabs = nullptr;
  QLineEdit* m_groupFilter = nullptr;
  QTreeWidget* m_groups = nullptr;
  QLineEdit* m_path = nullptr;
  QPushButton* m_browse = nullptr;
  QLabel* m_fileLabel = nullptr;
  QPlainTextEdit* m_text = nullptr;
  QLineEdit* m_textName = nullptr;
  QLabel* m_textNameLabel = nullptr;
  QLabel* m_message = nullptr;
  std::optional<LayerDefinition> m_definition;
};

} // namespace ts

#endif // TS_APP_ADDSATELLITESDIALOG_H
