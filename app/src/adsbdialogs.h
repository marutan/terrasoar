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
 * The Import Aircraft ADS-B Data dialog (a recording) and the Connect to
 * Aircraft ADS-B Receiver dialog.
 */
#ifndef TS_APP_ADSBDIALOGS_H
#define TS_APP_ADSBDIALOGS_H

#include <array>
#include <optional>

#include <QDialog>
#include <QString>

#include "ts/adsb/adsbreaders.h"
#include "ts/adsb/aircraftlayer.h"
#include "ts/layers/layerdefinition.h"

class QCheckBox;
class QComboBox;
class QDateTimeEdit;
class QDoubleSpinBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace ts
{

/**
 * Chooses a recording of ADS-B data and how to read it: the format
 * (detected), the start time of formats timed by a receiver's counter,
 * whether SBS times are local, the receiver's position, and a CSV file's
 * columns.
 */
class ImportAdsbDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(ImportAdsbDialog)

public:
  /** Creates the dialog, for @p path if given. */
  explicit ImportAdsbDialog(const QString& path, QWidget* parent);
  ~ImportAdsbDialog() override;

  /** Returns the source chosen (after accept). */
  const AircraftSource& source() const { return m_source; }

  /** Returns the layer's name. */
  QString layerName() const;

  /** Returns the layer to add for the source chosen. */
  LayerDefinition layerDefinition() const;

  /** Sets the file (for tests). */
  void setPath(const QString& path);

  /** Returns the format detected or chosen. */
  AdsbFormat format() const;

  /** Returns the CSV columns as chosen. */
  CsvColumns csvColumns() const;

  /** Returns a new aircraft layer's definition for @p source. */
  static LayerDefinition definitionFor(const QJsonObject& source,
                                       const QString& name);

  /** Returns the file dialog's filter for recordings. */
  static QString fileFilter();

  /** Checks what was chosen and accepts it. */
  void accept() override;

private:
  void browse();
  void examine();
  void updateVisibility();

  QLineEdit* m_path;
  QLineEdit* m_name;
  QComboBox* m_format;
  QLabel* m_detected;
  QDateTimeEdit* m_start;
  QLabel* m_startLabel;
  QComboBox* m_sbsTime;
  QLabel* m_sbsTimeLabel;
  QCheckBox* m_hasReceiver;
  QDoubleSpinBox* m_receiverLat;
  QDoubleSpinBox* m_receiverLon;
  QGroupBox* m_csvBox;
  QComboBox* m_separator;
  QCheckBox* m_header;
  std::array<QComboBox*, 9> m_columns{};
  QComboBox* m_timeFormat;
  QComboBox* m_altitudeKind;
  QComboBox* m_speedUnit;
  QLabel* m_message;
  AdsbFormat m_detectedFormat = AdsbFormat::Unknown;
  QByteArray m_head;
  AircraftSource m_source;
};

/** Chooses a receiver to connect to and how. */
class ReceiverDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(ReceiverDialog)

public:
  /** Creates the dialog. */
  explicit ReceiverDialog(QWidget* parent);
  ~ReceiverDialog() override;

  /** Returns the source chosen (after accept). */
  const AircraftSource& source() const { return m_source; }

  /** Returns the layer to add for the source chosen. */
  LayerDefinition layerDefinition() const;

  /** Sets the fields (for tests). */
  void setReceiver(ReceiverProtocol protocol, const QString& host,
                   uint16_t port);

  /** Checks what was chosen and accepts it. */
  void accept() override;

private:
  void protocolChanged();

  QComboBox* m_protocol;
  QLineEdit* m_host;
  QSpinBox* m_port;
  QLineEdit* m_url;
  QDoubleSpinBox* m_poll;
  QLabel* m_hostLabel;
  QLabel* m_portLabel;
  QLabel* m_urlLabel;
  QLabel* m_pollLabel;
  QLineEdit* m_record;
  QCheckBox* m_hasReceiver;
  QDoubleSpinBox* m_receiverLat;
  QDoubleSpinBox* m_receiverLon;
  QLineEdit* m_name;
  QLabel* m_message;
  AircraftSource m_source;
};

} // namespace ts

#endif // TS_APP_ADSBDIALOGS_H
