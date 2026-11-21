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
 * The Import AIS Data dialog, the Add AIS Source dialog (a receiver or an
 * open feed) and a ship layer's properties.
 */
#ifndef TS_APP_AISDIALOGS_H
#define TS_APP_AISDIALOGS_H

#include <array>
#include <functional>
#include <map>
#include <optional>

#include <QColor>
#include <QDialog>
#include <QString>

#include "ts/ais/vessellayer.h"
#include "ts/layers/layerdefinition.h"

class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTabWidget;

namespace ts
{

/** Returns the name of ship type group @p group (translated). */
QString shipTypeGroupName(ShipTypeGroup group);

/** Returns the name of navigational status @p status (translated). */
QString navStatusName(AisNavStatus status);

/** Returns a new ship layer's definition for @p source. */
LayerDefinition vesselLayerDefinition(const QJsonObject& source,
                                      const QString& name);

/**
 * Chooses a recording of AIS data and how to read it: the format
 * (detected), the start time of a log without times, a CSV's columns.
 */
class ImportAisDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(ImportAisDialog)

public:
  /** Creates the dialog, for @p path if given. */
  ImportAisDialog(const QString& path, QWidget* parent);
  ~ImportAisDialog() override;

  /** Returns the layer to add (after accept). */
  LayerDefinition layerDefinition() const;

  /** Sets the file (for tests). */
  void setPath(const QString& path);

  /** Returns the format detected or chosen. */
  AisFormat format() const;

  /** Returns true if the file's sentences have no times. */
  bool untimed() const { return m_untimed; }

  /** Returns the file dialog's filter for recordings. */
  static QString fileFilter();

  /** Checks what was chosen and accepts it. */
  void accept() override;

private:
  void browse();
  void examine();
  void updateVisibility();
  AisCsvColumns csvColumns() const;

  QLineEdit* m_path;
  QLineEdit* m_name;
  QComboBox* m_format;
  QLabel* m_detected;
  QDateTimeEdit* m_start;
  QLabel* m_startLabel;
  QGroupBox* m_csvBox;
  QComboBox* m_separator;
  QCheckBox* m_header;
  std::array<QComboBox*, 9> m_columns{};
  QLabel* m_message;
  QByteArray m_head;
  AisFormat m_detectedFormat = AisFormat::Unknown;
  bool m_untimed = false;
  VesselSource m_source;
};

/** Chooses a live AIS source: a receiver, or an open online feed. */
class AisSourceDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(AisSourceDialog)

public:
  /** Creates the dialog. */
  explicit AisSourceDialog(QWidget* parent);
  ~AisSourceDialog() override;

  /** Returns the layer to add (after accept). */
  LayerDefinition layerDefinition() const;

  /** Sets a TCP or UDP receiver (for tests). */
  void setReceiver(bool udp, const QString& host, uint16_t port);

  /** Chooses feed @p id (for tests). */
  void setFeed(const QString& id);

  /** Checks what was chosen and accepts it. */
  void accept() override;

private:
  void updateEnabled();
  void showFeed();

  QTabWidget* m_tabs;
  QRadioButton* m_tcp;
  QRadioButton* m_udp;
  QLineEdit* m_host;
  QSpinBox* m_port;
  QLineEdit* m_record;
  QListWidget* m_feeds;
  QLabel* m_feedTerms;
  QLineEdit* m_name;
  QLabel* m_message;
  VesselSource m_source;
  QString m_layerName;
};

/** Edits a ship layer's style. */
class VesselPropertiesDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(VesselPropertiesDialog)

public:
  /** Returns the area in view, if there is one (for the area filter). */
  using ViewArea = std::function<std::optional<LatLonBox>()>;

  /** Edits @p style of layer @p name. */
  VesselPropertiesDialog(const VesselStyle& style, const QString& name,
                         ViewArea viewArea, QWidget* parent);
  ~VesselPropertiesDialog() override;

  /** Returns the style as edited. */
  VesselStyle style() const;

private:
  void chooseColor();
  void updateSwatch();
  void updateEnabled();

  VesselStyle m_style;
  QString m_name;
  ViewArea m_viewArea;
  QColor m_color;
  QPushButton* m_colorButton;
  QCheckBox* m_byType;
  QSpinBox* m_markerSize;
  QCheckBox* m_labels;
  QCheckBox* m_labelSpeed;
  QCheckBox* m_labelDestination;
  QSpinBox* m_courseVector;
  QComboBox* m_trailMode;
  QDoubleSpinBox* m_trailSpan;
  QComboBox* m_trailColouring;
  QDoubleSpinBox* m_stale;
  QDoubleSpinBox* m_gap;
  QLineEdit* m_nameFilter;
  QListWidget* m_groups;
  QListWidget* m_statuses;
  QCheckBox* m_hasMinimum;
  QDoubleSpinBox* m_minimum;
  QCheckBox* m_hasMaximum;
  QDoubleSpinBox* m_maximum;
  QCheckBox* m_hasArea;
  std::array<QDoubleSpinBox*, 4> m_area{};
  QPushButton* m_useView;
};

} // namespace ts

#endif // TS_APP_AISDIALOGS_H
