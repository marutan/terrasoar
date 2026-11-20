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
 * The properties of a KML element being edited.
 */
#ifndef TS_APP_KMLPROPERTIESDIALOG_H
#define TS_APP_KMLPROPERTIESDIALOG_H

#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

#include <QColor>
#include <QDialog>
#include <QString>

#include "ts/kml/kmldocument.h"

class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTextBrowser;

namespace ts
{

/**
 * Edits a KML element's name, description (with its balloon shown as it
 * will look), style, height, time, extended data and view. The style is
 * the element's own or one of the document's shared styles, which then
 * changes every element using it.
 *
 * @guithread
 */
class KmlPropertiesDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(KmlPropertiesDialog)

public:
  /**
   * Edits @p feature of @p document; @p currentView gives the camera's
   * view for "Set from the view".
   */
  KmlPropertiesDialog(const KmlFeature& feature, const KmlDocument& document,
                      std::function<KmlView()> currentView, QWidget* parent);
  ~KmlPropertiesDialog() override;

  /** Returns the element as edited. */
  KmlFeature feature() const;

  /**
   * Returns the shared style chosen and its edited values (its id first),
   * or nothing if the element has its own.
   */
  std::optional<KmlStyle> sharedStyle() const;

  /** The user guide's section on this dialog. */
  static QString helpTarget();

private:
  QWidget* makeGeneral();
  QWidget* makeStyle();
  QWidget* makePosition();
  QWidget* makeTime();
  QWidget* makeData();
  void chooseColor(QPushButton* button, QColor* color);
  static void showColor(QPushButton* button, const QColor& color);
  void loadStyle(const KmlStyle& style);
  KmlStyle editedStyle() const;
  void styleChosen();
  void updatePreview();
  void updateTime();
  void updateView();
  int32_t kmlStyleUsersOf(const QString& id) const;

  KmlFeature m_feature;
  const KmlDocument* m_document;
  std::function<KmlView()> m_currentView;
  std::optional<KmlView> m_view;
  KmlStyle m_loaded;

  QLineEdit* m_name = nullptr;
  QPlainTextEdit* m_description = nullptr;
  QTextBrowser* m_preview = nullptr;
  QComboBox* m_style = nullptr;
  QLineEdit* m_icon = nullptr;
  QPushButton* m_iconColorButton = nullptr;
  QColor m_iconColor;
  QDoubleSpinBox* m_iconScale = nullptr;
  QPushButton* m_labelColorButton = nullptr;
  QColor m_labelColor;
  QDoubleSpinBox* m_labelScale = nullptr;
  QPushButton* m_lineColorButton = nullptr;
  QColor m_lineColor;
  QDoubleSpinBox* m_lineWidth = nullptr;
  QPushButton* m_fillColorButton = nullptr;
  QColor m_fillColor;
  QCheckBox* m_fill = nullptr;
  QCheckBox* m_outline = nullptr;
  QDoubleSpinBox* m_altitude = nullptr;
  QComboBox* m_altitudeMode = nullptr;
  QCheckBox* m_extrude = nullptr;
  QComboBox* m_timeKind = nullptr;
  QDateTimeEdit* m_begin = nullptr;
  QDateTimeEdit* m_end = nullptr;
  QTableWidget* m_data = nullptr;
  QLabel* m_viewLabel = nullptr;
};

} // namespace ts

#endif // TS_APP_KMLPROPERTIESDIALOG_H
