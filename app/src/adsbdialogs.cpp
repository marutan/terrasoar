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
#include "adsbdialogs.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimeZone>
#include <QUuid>
#include <QVBoxLayout>

namespace ts
{

namespace
{

/** A receiver position row: a check box and latitude, longitude. */
QWidget* receiverRow(QWidget* parent, QCheckBox* has, QDoubleSpinBox* lat,
                     QDoubleSpinBox* lon)
{
  lat->setRange(-90.0, 90.0);
  lat->setDecimals(5);
  lon->setRange(-180.0, 180.0);
  lon->setDecimals(5);
  lat->setEnabled(false);
  lon->setEnabled(false);
  QObject::connect(has, &QCheckBox::toggled, lat, &QWidget::setEnabled);
  QObject::connect(has, &QCheckBox::toggled, lon, &QWidget::setEnabled);
  QWidget* row = new QWidget(parent);
  QHBoxLayout* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(has);
  layout->addWidget(lat);
  layout->addWidget(lon);
  return row;
}

std::optional<CprPosition> receiverOf(const QCheckBox* has,
                                      const QDoubleSpinBox* lat,
                                      const QDoubleSpinBox* lon)
{
  if (!has->isChecked()) {
    return std::nullopt;
  }
  return CprPosition{lat->value(), lon->value()};
}

} // namespace

ImportAdsbDialog::ImportAdsbDialog(const QString& path, QWidget* parent)
    : QDialog(parent), m_path(new QLineEdit(this)), m_name(new QLineEdit(this)),
      m_format(new QComboBox(this)), m_detected(new QLabel(this)),
      m_start(new QDateTimeEdit(this)), m_startLabel(new QLabel(this)),
      m_sbsTime(new QComboBox(this)), m_sbsTimeLabel(new QLabel(this)),
      m_hasReceiver(new QCheckBox(this)),
      m_receiverLat(new QDoubleSpinBox(this)),
      m_receiverLon(new QDoubleSpinBox(this)), m_csvBox(new QGroupBox(this)),
      m_separator(new QComboBox(this)), m_header(new QCheckBox(this)),
      m_timeFormat(new QComboBox(this)), m_altitudeKind(new QComboBox(this)),
      m_speedUnit(new QComboBox(this)), m_message(new QLabel(this))
{
  setObjectName(QStringLiteral("importAdsbDialog"));
  setWindowTitle(tr("Import Aircraft ADS-B Data"));
  m_path->setObjectName(QStringLiteral("adsbPath"));
  m_format->setObjectName(QStringLiteral("adsbFormat"));
  m_detected->setObjectName(QStringLiteral("adsbDetected"));
  m_start->setObjectName(QStringLiteral("adsbStart"));
  m_message->setObjectName(QStringLiteral("adsbMessage"));
  m_message->setWordWrap(true);
  m_format->addItem(tr("Detect automatically"),
                    static_cast<int32_t>(AdsbFormat::Unknown));
  m_format->addItem(tr("SBS-1 / BaseStation (port 30003)"),
                    static_cast<int32_t>(AdsbFormat::Sbs));
  m_format->addItem(tr("Raw Mode S messages (AVR, time and hex)"),
                    static_cast<int32_t>(AdsbFormat::Raw));
  m_format->addItem(tr("Beast binary (port 30005)"),
                    static_cast<int32_t>(AdsbFormat::Beast));
  m_format->addItem(tr("dump1090/readsb aircraft.json snapshots"),
                    static_cast<int32_t>(AdsbFormat::ReadsbJson));
  m_format->addItem(tr("tar1090/readsb trace"),
                    static_cast<int32_t>(AdsbFormat::Trace));
  m_format->addItem(tr("CSV with chosen columns"),
                    static_cast<int32_t>(AdsbFormat::Csv));
  // UTC, shown as UTC.
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  m_start->setTimeZone(QTimeZone::utc());
#else
  m_start->setTimeSpec(Qt::UTC);
#endif
  m_start->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'"));
  m_start->setDateTime(QDateTime::currentDateTimeUtc());
  m_sbsTime->addItem(tr("This computer's local time"), true);
  m_sbsTime->addItem(tr("UTC"), false);
  m_hasReceiver->setText(tr("Receiver at"));

  QPushButton* browseButton = new QPushButton(tr("Browse…"), this);
  QHBoxLayout* pathRow = new QHBoxLayout;
  pathRow->addWidget(m_path);
  pathRow->addWidget(browseButton);
  QFormLayout* form = new QFormLayout;
  form->addRow(tr("File:"), pathRow);
  form->addRow(tr("Format:"), m_format);
  form->addRow(QString(), m_detected);
  form->addRow(m_startLabel, m_start);
  form->addRow(m_sbsTimeLabel, m_sbsTime);
  form->addRow(tr("Receiver:"),
               receiverRow(this, m_hasReceiver, m_receiverLat, m_receiverLon));
  form->addRow(tr("Layer name:"), m_name);
  m_startLabel->setText(tr("First message at:"));
  m_sbsTimeLabel->setText(tr("SBS times are in:"));

  m_csvBox->setTitle(tr("CSV columns"));
  m_csvBox->setObjectName(QStringLiteral("adsbCsvColumns"));
  m_separator->addItem(tr("Comma"), QString(QLatin1Char(',')));
  m_separator->addItem(tr("Semicolon"), QString(QLatin1Char(';')));
  m_separator->addItem(tr("Tab"), QString(QLatin1Char('\t')));
  m_separator->addItem(tr("Bar"), QString(QLatin1Char('|')));
  m_header->setText(tr("The first line names the columns"));
  m_timeFormat->addItem(tr("Unix seconds"),
                        static_cast<int32_t>(CsvTime::UnixSeconds));
  m_timeFormat->addItem(tr("Unix milliseconds"),
                        static_cast<int32_t>(CsvTime::UnixMilliseconds));
  m_timeFormat->addItem(tr("ISO 8601"), static_cast<int32_t>(CsvTime::Iso8601));
  m_altitudeKind->addItem(tr("Barometric, feet"),
                          static_cast<int32_t>(CsvAltitude::BaroFeet));
  m_altitudeKind->addItem(tr("Barometric, metres"),
                          static_cast<int32_t>(CsvAltitude::BaroMetres));
  m_altitudeKind->addItem(tr("Above the ellipsoid, feet"),
                          static_cast<int32_t>(CsvAltitude::GeometricFeet));
  m_altitudeKind->addItem(tr("Above the ellipsoid, metres"),
                          static_cast<int32_t>(CsvAltitude::GeometricMetres));
  m_speedUnit->addItem(tr("Knots"), static_cast<int32_t>(CsvSpeed::Knots));
  m_speedUnit->addItem(tr("Metres a second"),
                       static_cast<int32_t>(CsvSpeed::MetresPerSecond));
  m_speedUnit->addItem(tr("Kilometres an hour"),
                       static_cast<int32_t>(CsvSpeed::KilometresPerHour));
  const std::array<QString, 9> names = {
      tr("Time:"),      tr("ICAO address:"), tr("Latitude:"),
      tr("Longitude:"), tr("Altitude:"),     tr("Track or heading:"),
      tr("Speed:"),     tr("Callsign:"),     tr("Vertical rate (ft/min):")};
  QGridLayout* csvLayout = new QGridLayout(m_csvBox);
  csvLayout->addWidget(new QLabel(tr("Separator:"), m_csvBox), 0, 0);
  csvLayout->addWidget(m_separator, 0, 1);
  csvLayout->addWidget(m_header, 0, 2, 1, 2);
  for (std::size_t i = 0; i < m_columns.size(); ++i) {
    m_columns.at(i) = new QComboBox(m_csvBox);
    m_columns.at(i)->setObjectName(QStringLiteral("adsbColumn%1").arg(i));
    const int32_t row = 1 + static_cast<int32_t>(i / 2);
    const int32_t column = 2 * static_cast<int32_t>(i % 2);
    csvLayout->addWidget(new QLabel(names.at(i), m_csvBox), row, column);
    csvLayout->addWidget(m_columns.at(i), row, column + 1);
  }
  csvLayout->addWidget(new QLabel(tr("Times are:"), m_csvBox), 6, 0);
  csvLayout->addWidget(m_timeFormat, 6, 1);
  csvLayout->addWidget(new QLabel(tr("Altitudes are:"), m_csvBox), 6, 2);
  csvLayout->addWidget(m_altitudeKind, 6, 3);
  csvLayout->addWidget(new QLabel(tr("Speeds are in:"), m_csvBox), 7, 0);
  csvLayout->addWidget(m_speedUnit, 7, 1);

  QDialogButtonBox* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this,
          &ImportAdsbDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(browseButton, &QPushButton::clicked, this, &ImportAdsbDialog::browse);
  connect(m_path, &QLineEdit::editingFinished, this,
          &ImportAdsbDialog::examine);
  connect(m_format, qOverload<int32_t>(&QComboBox::currentIndexChanged), this,
          &ImportAdsbDialog::updateVisibility);
  connect(m_separator, qOverload<int32_t>(&QComboBox::currentIndexChanged),
          this, [this]() {
            // The columns again, split with the separator chosen.
            CsvColumns columns = guessCsvColumns(m_head);
            columns.separator = m_separator->currentData().toString().at(0);
            const QStringList fields = splitCsvLine(
                QString::fromUtf8(m_head.left(m_head.indexOf('\n'))),
                columns.separator);
            for (QComboBox* box : m_columns) {
              const QVariant kept = box->currentData();
              box->clear();
              box->addItem(tr("(none)"), -1);
              for (int32_t i = 0; i < fields.size(); ++i) {
                box->addItem(QStringLiteral("%1: %2").arg(i + 1).arg(
                                 fields.at(i).trimmed().left(24)),
                             i);
              }
              box->setCurrentIndex(std::max(box->findData(kept), 0));
            }
          });
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addLayout(form);
  layout->addWidget(m_csvBox);
  layout->addWidget(m_message);
  layout->addStretch(1);
  layout->addWidget(buttons);
  resize(620, 520);
  setPath(path);
}

ImportAdsbDialog::~ImportAdsbDialog() = default;

QString ImportAdsbDialog::fileFilter()
{
  return tr("ADS-B recordings (*.sbs *.basestation *.avr *.raw *.beast *.bin "
            "*.json *.jsonl *.csv *.txt *.gz);;All files (*)");
}

void ImportAdsbDialog::browse()
{
  const QString path = QFileDialog::getOpenFileName(
      this, tr("Import Aircraft ADS-B Data"), m_path->text(), fileFilter());
  if (!path.isEmpty()) {
    setPath(path);
  }
}

void ImportAdsbDialog::setPath(const QString& path)
{
  m_path->setText(path);
  examine();
}

void ImportAdsbDialog::examine()
{
  const QString path = m_path->text().trimmed();
  m_head.clear();
  m_detectedFormat = AdsbFormat::Unknown;
  if (path.isEmpty()) {
    m_detected->clear();
    updateVisibility();
    return;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    m_detected->setText(file.errorString());
    updateVisibility();
    return;
  }
  bool ok = false;
  // A compressed file: uncompressed whole (traces are small).
  QByteArray head = file.read(8192);
  if (head.startsWith("\x1f\x8b")) {
    file.seek(0);
    head = gunzipIfCompressed(file.readAll(), &ok).left(8192);
  }
  m_head = head;
  m_detectedFormat = detectAdsbFormat(m_head);
  m_detected->setText(
      m_detectedFormat == AdsbFormat::Unknown
          ? tr("The format can't be told from the file: choose it.")
          : tr("Looks like: %1")
                .arg(m_format->itemText(m_format->findData(
                    static_cast<int32_t>(m_detectedFormat)))));
  if (m_name->text().isEmpty() || m_name->property("automatic").toBool()) {
    m_name->setText(QFileInfo(path).completeBaseName());
    m_name->setProperty("automatic", true);
  }
  // A counter-timed recording starts, by default, as long before the
  // file was last written as it lasts: unknown, so at its time.
  m_start->setDateTime(QFileInfo(path).lastModified().toUTC());
  // The CSV columns guessed from the head.
  const CsvColumns guess = guessCsvColumns(m_head);
  m_separator->blockSignals(true);
  m_separator->setCurrentIndex(
      std::max(m_separator->findData(QString(guess.separator)), 0));
  m_separator->blockSignals(false);
  m_header->setChecked(guess.header);
  const QStringList fields = splitCsvLine(
      QString::fromUtf8(m_head.left(m_head.indexOf('\n'))), guess.separator);
  const std::array<int32_t, 9> chosen = {
      guess.time,      guess.address,  guess.latitude,
      guess.longitude, guess.altitude, guess.heading,
      guess.speed,     guess.callsign, guess.verticalRate};
  for (std::size_t i = 0; i < m_columns.size(); ++i) {
    QComboBox* box = m_columns.at(i);
    box->clear();
    box->addItem(tr("(none)"), -1);
    for (int32_t f = 0; f < fields.size(); ++f) {
      box->addItem(QStringLiteral("%1: %2").arg(f + 1).arg(
                       fields.at(f).trimmed().left(24)),
                   f);
    }
    box->setCurrentIndex(std::max(box->findData(chosen.at(i)), 0));
  }
  m_timeFormat->setCurrentIndex(
      m_timeFormat->findData(static_cast<int32_t>(guess.timeFormat)));
  m_altitudeKind->setCurrentIndex(
      m_altitudeKind->findData(static_cast<int32_t>(guess.altitudeKind)));
  m_speedUnit->setCurrentIndex(
      m_speedUnit->findData(static_cast<int32_t>(guess.speedUnit)));
  updateVisibility();
}

AdsbFormat ImportAdsbDialog::format() const
{
  const AdsbFormat chosen =
      static_cast<AdsbFormat>(m_format->currentData().toInt());
  return chosen == AdsbFormat::Unknown ? m_detectedFormat : chosen;
}

void ImportAdsbDialog::updateVisibility()
{
  const AdsbFormat f = format();
  // Raw messages are timed by a counter (or not at all) unless each line
  // has a Unix time.
  const bool counter =
      f == AdsbFormat::Beast ||
      (f == AdsbFormat::Raw && !m_head.isEmpty() &&
       !parseRawLine(m_head.left(m_head.indexOf('\n')).trimmed())
            .value_or(TimedMessage())
            .timeS);
  m_start->setVisible(counter);
  m_startLabel->setVisible(counter);
  m_sbsTime->setVisible(f == AdsbFormat::Sbs);
  m_sbsTimeLabel->setVisible(f == AdsbFormat::Sbs);
  m_csvBox->setVisible(f == AdsbFormat::Csv);
}

CsvColumns ImportAdsbDialog::csvColumns() const
{
  CsvColumns c;
  c.separator = m_separator->currentData().toString().at(0);
  c.header = m_header->isChecked();
  std::array<int32_t*, 9> targets = {
      &c.time,    &c.address, &c.latitude, &c.longitude,   &c.altitude,
      &c.heading, &c.speed,   &c.callsign, &c.verticalRate};
  for (std::size_t i = 0; i < m_columns.size(); ++i) {
    *targets.at(i) = m_columns.at(i)->currentData().toInt();
  }
  c.timeFormat = static_cast<CsvTime>(m_timeFormat->currentData().toInt());
  c.altitudeKind =
      static_cast<CsvAltitude>(m_altitudeKind->currentData().toInt());
  c.speedUnit = static_cast<CsvSpeed>(m_speedUnit->currentData().toInt());
  return c;
}

QString ImportAdsbDialog::layerName() const
{
  const QString name = m_name->text().trimmed();
  return name.isEmpty() ? QFileInfo(m_path->text()).completeBaseName() : name;
}

LayerDefinition ImportAdsbDialog::definitionFor(const QJsonObject& source,
                                                const QString& name)
{
  LayerDefinition definition;
  definition.id = QStringLiteral("user:aircraft:") +
                  QUuid::createUuid().toString(QUuid::WithoutBraces);
  definition.origin = LayerOrigin::User;
  definition.type = LayerType::Vector;
  definition.name = name;
  definition.source = source;
  definition.style = AircraftStyle().toJson();
  return definition;
}

LayerDefinition ImportAdsbDialog::layerDefinition() const
{
  return definitionFor(m_source.toJson(), layerName());
}

void ImportAdsbDialog::accept()
{
  const QString path = m_path->text().trimmed();
  if (path.isEmpty() || !QFileInfo(path).isFile()) {
    m_message->setText(tr("Choose a recording."));
    return;
  }
  const AdsbFormat f = format();
  if (f == AdsbFormat::Unknown) {
    m_message->setText(tr("Choose the recording's format."));
    return;
  }
  AircraftSource source;
  source.origin = AircraftSource::Origin::File;
  source.path = QFileInfo(path).absoluteFilePath();
  source.format = f;
  source.sbsLocalTime = m_sbsTime->currentData().toBool();
  QDateTime start = m_start->dateTime();
  start.setTimeZone(QTimeZone::utc());
  source.startTimeS = static_cast<double>(start.toMSecsSinceEpoch()) / 1000.0;
  source.receiverPosition =
      receiverOf(m_hasReceiver, m_receiverLat, m_receiverLon);
  if (f == AdsbFormat::Csv) {
    source.csv = csvColumns();
    if (!source.csv.isComplete()) {
      m_message->setText(tr("Choose the time, address, latitude and longitude "
                            "columns."));
      return;
    }
  }
  m_source = source;
  QDialog::accept();
}

ReceiverDialog::ReceiverDialog(QWidget* parent)
    : QDialog(parent), m_protocol(new QComboBox(this)),
      m_host(new QLineEdit(this)), m_port(new QSpinBox(this)),
      m_url(new QLineEdit(this)), m_poll(new QDoubleSpinBox(this)),
      m_hostLabel(new QLabel(tr("Host:"), this)),
      m_portLabel(new QLabel(tr("Port:"), this)),
      m_urlLabel(new QLabel(tr("aircraft.json address:"), this)),
      m_pollLabel(new QLabel(tr("Fetch every:"), this)),
      m_record(new QLineEdit(this)), m_hasReceiver(new QCheckBox(this)),
      m_receiverLat(new QDoubleSpinBox(this)),
      m_receiverLon(new QDoubleSpinBox(this)), m_name(new QLineEdit(this)),
      m_message(new QLabel(this))
{
  setObjectName(QStringLiteral("receiverDialog"));
  setWindowTitle(tr("Connect to Aircraft ADS-B Receiver"));
  m_protocol->setObjectName(QStringLiteral("receiverProtocol"));
  m_host->setObjectName(QStringLiteral("receiverHost"));
  m_port->setObjectName(QStringLiteral("receiverPort"));
  m_url->setObjectName(QStringLiteral("receiverUrl"));
  m_record->setObjectName(QStringLiteral("receiverRecord"));
  m_message->setWordWrap(true);
  m_protocol->addItem(tr("SBS-1 / BaseStation (port 30003)"),
                      static_cast<int32_t>(ReceiverProtocol::Sbs));
  m_protocol->addItem(tr("Raw AVR (port 30002)"),
                      static_cast<int32_t>(ReceiverProtocol::Avr));
  m_protocol->addItem(tr("Beast binary (port 30005)"),
                      static_cast<int32_t>(ReceiverProtocol::Beast));
  m_protocol->addItem(tr("aircraft.json over HTTP"),
                      static_cast<int32_t>(ReceiverProtocol::Json));
  const QString localHost = QStringLiteral("localhost");
  m_host->setText(localHost);
  m_port->setRange(1, 65535);
  m_port->setValue(30003);
  const QString jsonAddress =
      QStringLiteral("http://localhost/tar1090/data/aircraft.json");
  m_url->setText(jsonAddress);
  m_poll->setRange(1.0, 60.0);
  m_poll->setDecimals(0);
  m_poll->setSuffix(tr(" s"));
  m_poll->setValue(1.0);
  m_hasReceiver->setText(tr("Receiver at"));
  m_record->setPlaceholderText(tr("Not recorded"));
  QPushButton* recordBrowse = new QPushButton(tr("Choose…"), this);
  QHBoxLayout* recordRow = new QHBoxLayout;
  recordRow->addWidget(m_record);
  recordRow->addWidget(recordBrowse);
  QFormLayout* form = new QFormLayout;
  form->addRow(tr("Receiver sends:"), m_protocol);
  form->addRow(m_hostLabel, m_host);
  form->addRow(m_portLabel, m_port);
  form->addRow(m_urlLabel, m_url);
  form->addRow(m_pollLabel, m_poll);
  form->addRow(tr("Position:"),
               receiverRow(this, m_hasReceiver, m_receiverLat, m_receiverLon));
  form->addRow(tr("Record to:"), recordRow);
  form->addRow(tr("Layer name:"), m_name);
  QDialogButtonBox* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &ReceiverDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_protocol, qOverload<int32_t>(&QComboBox::currentIndexChanged), this,
          &ReceiverDialog::protocolChanged);
  connect(recordBrowse, &QPushButton::clicked, this, [this]() {
    const QString path = QFileDialog::getSaveFileName(
        this, tr("Record To"), m_record->text(),
        tr("Recordings (*.sbs *.txt *.jsonl);;All files (*)"));
    if (!path.isEmpty()) {
      m_record->setText(path);
    }
  });
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addLayout(form);
  layout->addWidget(m_message);
  layout->addWidget(buttons);
  protocolChanged();
}

ReceiverDialog::~ReceiverDialog() = default;

void ReceiverDialog::protocolChanged()
{
  const ReceiverProtocol protocol =
      static_cast<ReceiverProtocol>(m_protocol->currentData().toInt());
  const bool json = protocol == ReceiverProtocol::Json;
  for (QWidget* w :
       std::array<QWidget*, 4>{m_host, m_port, m_hostLabel, m_portLabel}) {
    w->setVisible(!json);
  }
  for (QWidget* w :
       std::array<QWidget*, 4>{m_url, m_poll, m_urlLabel, m_pollLabel}) {
    w->setVisible(json);
  }
  m_port->setValue(defaultReceiverPort(protocol));
}

void ReceiverDialog::setReceiver(ReceiverProtocol protocol, const QString& host,
                                 uint16_t port)
{
  m_protocol->setCurrentIndex(
      m_protocol->findData(static_cast<int32_t>(protocol)));
  m_host->setText(host);
  m_port->setValue(port);
}

LayerDefinition ReceiverDialog::layerDefinition() const
{
  const QString name = m_name->text().trimmed();
  return ImportAdsbDialog::definitionFor(
      m_source.toJson(), name.isEmpty() ? m_source.describe() : name);
}

void ReceiverDialog::accept()
{
  AircraftSource source;
  source.origin = AircraftSource::Origin::Receiver;
  source.protocol =
      static_cast<ReceiverProtocol>(m_protocol->currentData().toInt());
  if (source.protocol == ReceiverProtocol::Json) {
    source.url = QUrl::fromUserInput(m_url->text().trimmed());
    source.pollS = m_poll->value();
    if (!source.url.isValid() || source.url.scheme().isEmpty()) {
      m_message->setText(tr("Enter the address of the receiver's "
                            "aircraft.json."));
      return;
    }
  } else {
    source.host = m_host->text().trimmed();
    source.port = static_cast<uint16_t>(m_port->value());
    if (source.host.isEmpty()) {
      m_message->setText(tr("Enter the receiver's host name or address."));
      return;
    }
  }
  source.recordPath = m_record->text().trimmed();
  source.receiverPosition =
      receiverOf(m_hasReceiver, m_receiverLat, m_receiverLon);
  m_source = source;
  QDialog::accept();
}

} // namespace ts
