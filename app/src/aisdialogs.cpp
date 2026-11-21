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
#include "aisdialogs.h"

#include <cmath>
#include <cstddef>
#include <utility>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCoreApplication>
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
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QRadioButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QTimeZone>
#include <QUuid>
#include <QVBoxLayout>

#include "ts/adsb/adsbreaders.h"

namespace ts
{

namespace
{

/** A swatch of @p rgba for lists and legends. */
QIcon swatch(std::array<std::uint8_t, 4> rgba)
{
  QPixmap pixmap(14, 14);
  pixmap.fill(QColor(rgba[0], rgba[1], rgba[2]));
  return QIcon(pixmap);
}

/** The columns asked for, with their labels and members. */
struct ColumnChoice {
  const char* label;
  int32_t AisCsvColumns::*column;
};

const std::array<ColumnChoice, 9>& columnChoices()
{
  static const std::array<ColumnChoice, 9> choices = {{
      {QT_TRANSLATE_NOOP("ts::ImportAisDialog", "Time:"), &AisCsvColumns::time},
      {QT_TRANSLATE_NOOP("ts::ImportAisDialog", "MMSI:"), &AisCsvColumns::mmsi},
      {QT_TRANSLATE_NOOP("ts::ImportAisDialog", "Latitude:"),
       &AisCsvColumns::latitude},
      {QT_TRANSLATE_NOOP("ts::ImportAisDialog", "Longitude:"),
       &AisCsvColumns::longitude},
      {QT_TRANSLATE_NOOP("ts::ImportAisDialog", "Speed (knots):"),
       &AisCsvColumns::speed},
      {QT_TRANSLATE_NOOP("ts::ImportAisDialog", "Course:"),
       &AisCsvColumns::course},
      {QT_TRANSLATE_NOOP("ts::ImportAisDialog", "Heading:"),
       &AisCsvColumns::heading},
      {QT_TRANSLATE_NOOP("ts::ImportAisDialog", "Name:"), &AisCsvColumns::name},
      {QT_TRANSLATE_NOOP("ts::ImportAisDialog", "Ship type:"),
       &AisCsvColumns::shipType},
  }};
  return choices;
}

} // namespace

QString shipTypeGroupName(ShipTypeGroup group)
{
  switch (group) {
  case ShipTypeGroup::Unknown:
    break;
  case ShipTypeGroup::Cargo:
    return QCoreApplication::translate("ts::Ships", "Cargo");
  case ShipTypeGroup::Tanker:
    return QCoreApplication::translate("ts::Ships", "Tanker");
  case ShipTypeGroup::Passenger:
    return QCoreApplication::translate("ts::Ships", "Passenger");
  case ShipTypeGroup::HighSpeed:
    return QCoreApplication::translate("ts::Ships", "High-speed craft");
  case ShipTypeGroup::TugSpecial:
    return QCoreApplication::translate("ts::Ships", "Tug, pilot or special");
  case ShipTypeGroup::Fishing:
    return QCoreApplication::translate("ts::Ships", "Fishing");
  case ShipTypeGroup::Pleasure:
    return QCoreApplication::translate("ts::Ships", "Sailing or pleasure");
  case ShipTypeGroup::Other:
    return QCoreApplication::translate("ts::Ships", "Other");
  case ShipTypeGroup::Navigation:
    return QCoreApplication::translate("ts::Ships",
                                       "Aids to navigation and base stations");
  }
  return QCoreApplication::translate("ts::Ships", "Type not known");
}

QString navStatusName(AisNavStatus status)
{
  switch (status) {
  case AisNavStatus::UnderWayEngine:
    return QCoreApplication::translate("ts::Ships", "Under way using engine");
  case AisNavStatus::AtAnchor:
    return QCoreApplication::translate("ts::Ships", "At anchor");
  case AisNavStatus::NotUnderCommand:
    return QCoreApplication::translate("ts::Ships", "Not under command");
  case AisNavStatus::RestrictedManoeuvre:
    return QCoreApplication::translate("ts::Ships",
                                       "Restricted manoeuvrability");
  case AisNavStatus::ConstrainedByDraught:
    return QCoreApplication::translate("ts::Ships",
                                       "Constrained by her draught");
  case AisNavStatus::Moored:
    return QCoreApplication::translate("ts::Ships", "Moored");
  case AisNavStatus::Aground:
    return QCoreApplication::translate("ts::Ships", "Aground");
  case AisNavStatus::Fishing:
    return QCoreApplication::translate("ts::Ships", "Engaged in fishing");
  case AisNavStatus::UnderWaySailing:
    return QCoreApplication::translate("ts::Ships", "Under way sailing");
  case AisNavStatus::AisSart:
    return QCoreApplication::translate("ts::Ships", "AIS-SART");
  case AisNavStatus::NotDefined:
    break;
  }
  return QCoreApplication::translate("ts::Ships", "Not given");
}

LayerDefinition vesselLayerDefinition(const QJsonObject& source,
                                      const QString& name)
{
  LayerDefinition definition;
  definition.id = QStringLiteral("user:vessels:") +
                  QUuid::createUuid().toString(QUuid::WithoutBraces);
  definition.origin = LayerOrigin::User;
  definition.type = LayerType::Vector;
  definition.name = name;
  definition.source = source;
  definition.style = VesselStyle().toJson();
  return definition;
}

ImportAisDialog::ImportAisDialog(const QString& path, QWidget* parent)
    : QDialog(parent), m_path(new QLineEdit(this)), m_name(new QLineEdit(this)),
      m_format(new QComboBox(this)), m_detected(new QLabel(this)),
      m_start(new QDateTimeEdit(this)),
      m_startLabel(new QLabel(tr("First sentence at:"), this)),
      m_csvBox(new QGroupBox(tr("CSV columns"), this)),
      m_separator(new QComboBox(this)),
      m_header(new QCheckBox(tr("The first line names the columns"), this)),
      m_message(new QLabel(this))
{
  setObjectName(QStringLiteral("importAisDialog"));
  setWindowTitle(tr("Import AIS Data"));
  m_path->setObjectName(QStringLiteral("aisPath"));
  m_format->setObjectName(QStringLiteral("aisFormat"));
  m_message->setWordWrap(true);
  m_format->addItem(tr("Detect automatically"),
                    static_cast<int32_t>(AisFormat::Unknown));
  m_format->addItem(tr("NMEA sentences (!AIVDM)"),
                    static_cast<int32_t>(AisFormat::Nmea));
  m_format->addItem(tr("CSV (NOAA MarineCadastre's or other columns)"),
                    static_cast<int32_t>(AisFormat::Csv));
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  m_start->setTimeZone(QTimeZone::utc());
#else
  m_start->setTimeSpec(Qt::UTC);
#endif
  m_start->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'"));
  m_separator->addItem(tr("Comma"), QString(QLatin1Char(',')));
  m_separator->addItem(tr("Semicolon"), QString(QLatin1Char(';')));
  m_separator->addItem(tr("Tab"), QString(QLatin1Char('\t')));
  QPushButton* browseButton = new QPushButton(tr("Browse…"), this);
  QHBoxLayout* pathRow = new QHBoxLayout;
  pathRow->addWidget(m_path);
  pathRow->addWidget(browseButton);
  QFormLayout* form = new QFormLayout;
  form->addRow(tr("File:"), pathRow);
  form->addRow(tr("Format:"), m_format);
  form->addRow(QString(), m_detected);
  form->addRow(m_startLabel, m_start);
  form->addRow(tr("Layer name:"), m_name);
  QGridLayout* grid = new QGridLayout(m_csvBox);
  grid->addWidget(new QLabel(tr("Separator:"), m_csvBox), 0, 0);
  grid->addWidget(m_separator, 0, 1);
  grid->addWidget(m_header, 0, 2, 1, 2);
  for (std::size_t i = 0; i < m_columns.size(); ++i) {
    m_columns.at(i) = new QComboBox(m_csvBox);
    m_columns.at(i)->setObjectName(QStringLiteral("aisColumn%1").arg(i));
    const int32_t row = 1 + static_cast<int32_t>(i / 2);
    const int32_t column = 2 * static_cast<int32_t>(i % 2);
    grid->addWidget(new QLabel(tr(columnChoices().at(i).label), m_csvBox), row,
                    column);
    grid->addWidget(m_columns.at(i), row, column + 1);
  }
  QDialogButtonBox* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &ImportAisDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(browseButton, &QPushButton::clicked, this, &ImportAisDialog::browse);
  connect(m_path, &QLineEdit::editingFinished, this, &ImportAisDialog::examine);
  connect(m_format, qOverload<int32_t>(&QComboBox::currentIndexChanged), this,
          &ImportAisDialog::updateVisibility);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addLayout(form);
  layout->addWidget(m_csvBox);
  layout->addWidget(m_message);
  layout->addStretch(1);
  layout->addWidget(buttons);
  resize(600, 480);
  setPath(path);
}

ImportAisDialog::~ImportAisDialog() = default;

QString ImportAisDialog::fileFilter()
{
  return tr("AIS recordings (*.nmea *.ais *.txt *.log *.csv *.gz);;All files "
            "(*)");
}

void ImportAisDialog::browse()
{
  const QString path = QFileDialog::getOpenFileName(
      this, tr("Import AIS Data"), m_path->text(), fileFilter());
  if (!path.isEmpty()) {
    setPath(path);
  }
}

void ImportAisDialog::setPath(const QString& path)
{
  m_path->setText(path);
  examine();
}

void ImportAisDialog::examine()
{
  const QString path = m_path->text().trimmed();
  m_head.clear();
  m_detectedFormat = AisFormat::Unknown;
  m_untimed = false;
  QFile file(path);
  if (path.isEmpty() || !file.open(QIODevice::ReadOnly)) {
    m_detected->setText(path.isEmpty() ? QString() : file.errorString());
    updateVisibility();
    return;
  }
  QByteArray head = file.read(8192);
  if (head.startsWith("\x1f\x8b")) {
    file.seek(0);
    head = gunzipIfCompressed(file.readAll()).left(8192);
  }
  m_head = head;
  m_detectedFormat = detectAisFormat(m_head);
  if (m_detectedFormat == AisFormat::Nmea) {
    // Untimed if no line of the head has a time.
    NmeaAisParser parser;
    m_untimed = true;
    for (const QByteArray& line : m_head.split('\n')) {
      if (parser.parse(line, 0.0).timeS) {
        m_untimed = false;
        break;
      }
    }
  }
  m_detected->setText(m_detectedFormat == AisFormat::Unknown
                          ? tr("The format can't be told from the file: "
                               "choose it.")
                          : tr("Looks like: %1")
                                .arg(m_format->itemText(m_format->findData(
                                    static_cast<int32_t>(m_detectedFormat)))));
  if (m_name->text().isEmpty() || m_name->property("automatic").toBool()) {
    m_name->setText(QFileInfo(path).completeBaseName());
    m_name->setProperty("automatic", true);
  }
  m_start->setDateTime(QFileInfo(path).lastModified().toUTC());
  const AisCsvColumns guess = guessAisCsvColumns(m_head);
  m_separator->setCurrentIndex(
      std::max(m_separator->findData(QString(guess.separator)), 0));
  m_header->setChecked(guess.header);
  const int32_t end = static_cast<int32_t>(m_head.indexOf('\n'));
  const QStringList fields = splitCsvLine(
      QString::fromUtf8(end < 0 ? m_head : m_head.left(end)), guess.separator);
  for (std::size_t i = 0; i < m_columns.size(); ++i) {
    QComboBox* box = m_columns.at(i);
    box->clear();
    box->addItem(tr("(none)"), -1);
    for (int32_t f = 0; f < fields.size(); ++f) {
      box->addItem(QStringLiteral("%1: %2").arg(f + 1).arg(
                       fields.at(f).trimmed().left(24)),
                   f);
    }
    box->setCurrentIndex(
        std::max(box->findData(guess.*columnChoices().at(i).column), 0));
  }
  updateVisibility();
}

AisFormat ImportAisDialog::format() const
{
  const AisFormat chosen =
      static_cast<AisFormat>(m_format->currentData().toInt());
  return chosen == AisFormat::Unknown ? m_detectedFormat : chosen;
}

void ImportAisDialog::updateVisibility()
{
  const AisFormat f = format();
  m_start->setVisible(f == AisFormat::Nmea && m_untimed);
  m_startLabel->setVisible(f == AisFormat::Nmea && m_untimed);
  m_csvBox->setVisible(f == AisFormat::Csv);
}

AisCsvColumns ImportAisDialog::csvColumns() const
{
  AisCsvColumns c = guessAisCsvColumns(m_head);
  c.separator = m_separator->currentData().toString().at(0);
  c.header = m_header->isChecked();
  for (std::size_t i = 0; i < m_columns.size(); ++i) {
    c.*columnChoices().at(i).column = m_columns.at(i)->currentData().toInt();
  }
  return c;
}

LayerDefinition ImportAisDialog::layerDefinition() const
{
  const QString name = m_name->text().trimmed();
  return vesselLayerDefinition(
      m_source.toJson(),
      name.isEmpty() ? QFileInfo(m_source.path).completeBaseName() : name);
}

void ImportAisDialog::accept()
{
  const QString path = m_path->text().trimmed();
  if (path.isEmpty() || !QFileInfo(path).isFile()) {
    m_message->setText(tr("Choose a recording."));
    return;
  }
  const AisFormat f = format();
  if (f == AisFormat::Unknown) {
    m_message->setText(tr("Choose the recording's format."));
    return;
  }
  VesselSource source;
  source.path = QFileInfo(path).absoluteFilePath();
  source.format = f;
  QDateTime start = m_start->dateTime();
  start.setTimeZone(QTimeZone::utc());
  if (f == AisFormat::Nmea && m_untimed) {
    source.startTimeS = static_cast<double>(start.toMSecsSinceEpoch()) / 1000.0;
  }
  if (f == AisFormat::Csv) {
    source.csv = csvColumns();
    if (!source.csv.isComplete()) {
      m_message->setText(
          tr("Choose the time, MMSI, latitude and longitude columns."));
      return;
    }
  }
  m_source = source;
  QDialog::accept();
}

AisSourceDialog::AisSourceDialog(QWidget* parent)
    : QDialog(parent), m_tabs(new QTabWidget(this)),
      m_tcp(new QRadioButton(tr("Connect over TCP"), this)),
      m_udp(new QRadioButton(tr("Listen for UDP"), this)),
      m_host(new QLineEdit(this)), m_port(new QSpinBox(this)),
      m_record(new QLineEdit(this)), m_feeds(new QListWidget(this)),
      m_feedTerms(new QLabel(this)), m_name(new QLineEdit(this)),
      m_message(new QLabel(this))
{
  setObjectName(QStringLiteral("aisSourceDialog"));
  setWindowTitle(tr("Add AIS Source"));
  m_tabs->setObjectName(QStringLiteral("aisSourceTabs"));
  m_host->setObjectName(QStringLiteral("aisHost"));
  m_port->setObjectName(QStringLiteral("aisPort"));
  m_record->setObjectName(QStringLiteral("aisRecord"));
  m_feeds->setObjectName(QStringLiteral("aisFeeds"));
  m_feedTerms->setWordWrap(true);
  m_message->setWordWrap(true);
  m_tcp->setChecked(true);
  const QString localHost = QStringLiteral("localhost");
  m_host->setText(localHost);
  m_port->setRange(1, 65535);
  m_port->setValue(10110);
  m_record->setPlaceholderText(tr("Not recorded"));
  QWidget* receiver = new QWidget(this);
  QFormLayout* receiverForm = new QFormLayout(receiver);
  receiverForm->addRow(QString(), m_tcp);
  receiverForm->addRow(QString(), m_udp);
  receiverForm->addRow(tr("Host:"), m_host);
  receiverForm->addRow(tr("Port:"), m_port);
  QPushButton* recordBrowse = new QPushButton(tr("Choose…"), this);
  QHBoxLayout* recordRow = new QHBoxLayout;
  recordRow->addWidget(m_record);
  recordRow->addWidget(recordBrowse);
  receiverForm->addRow(tr("Record to:"), recordRow);
  receiverForm->addRow(
      QString(),
      new QLabel(tr("AIS-catcher, rtl-ais, a ship's AIS: NMEA sentences."),
                 this));
  QWidget* feeds = new QWidget(this);
  QVBoxLayout* feedsLayout = new QVBoxLayout(feeds);
  for (const AisFeedEntry& entry : aisFeedCatalogue()) {
    QListWidgetItem* item = new QListWidgetItem(entry.name, m_feeds);
    item->setData(Qt::UserRole, entry.id);
  }
  m_feeds->setCurrentRow(0);
  feedsLayout->addWidget(m_feeds);
  feedsLayout->addWidget(m_feedTerms);
  m_tabs->addTab(receiver, tr("Receiver"));
  m_tabs->addTab(feeds, tr("Open Feeds"));
  QFormLayout* nameForm = new QFormLayout;
  nameForm->addRow(tr("Layer name:"), m_name);
  QDialogButtonBox* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &AisSourceDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_udp, &QRadioButton::toggled, this, &AisSourceDialog::updateEnabled);
  connect(m_feeds, &QListWidget::currentRowChanged, this,
          &AisSourceDialog::showFeed);
  connect(recordBrowse, &QPushButton::clicked, this, [this]() {
    const QString path =
        QFileDialog::getSaveFileName(this, tr("Record To"), m_record->text(),
                                     tr("NMEA logs (*.nmea);;All files (*)"));
    if (!path.isEmpty()) {
      m_record->setText(path);
    }
  });
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(m_tabs);
  layout->addLayout(nameForm);
  layout->addWidget(m_message);
  layout->addWidget(buttons);
  resize(520, 420);
  updateEnabled();
  showFeed();
}

AisSourceDialog::~AisSourceDialog() = default;

void AisSourceDialog::updateEnabled()
{
  m_host->setEnabled(!m_udp->isChecked());
}

void AisSourceDialog::showFeed()
{
  const QListWidgetItem* item = m_feeds->currentItem();
  const std::optional<AisFeedEntry> entry =
      item != nullptr ? aisFeedEntry(item->data(Qt::UserRole).toString())
                      : std::nullopt;
  if (!entry) {
    m_feedTerms->clear();
    return;
  }
  m_feedTerms->setText(
      tr("%1\nLicence: %2. Credited as \"%3\" while the "
         "layer is shown.")
          .arg(entry->coverage, entry->licence, entry->attribution));
}

void AisSourceDialog::setReceiver(bool udp, const QString& host, uint16_t port)
{
  m_tabs->setCurrentIndex(0);
  (udp ? m_udp : m_tcp)->setChecked(true);
  m_host->setText(host);
  m_port->setValue(port);
}

void AisSourceDialog::setFeed(const QString& id)
{
  m_tabs->setCurrentIndex(1);
  for (int32_t i = 0; i < m_feeds->count(); ++i) {
    if (m_feeds->item(i)->data(Qt::UserRole).toString() == id) {
      m_feeds->setCurrentRow(i);
    }
  }
}

LayerDefinition AisSourceDialog::layerDefinition() const
{
  return vesselLayerDefinition(m_source.toJson(), m_layerName);
}

void AisSourceDialog::accept()
{
  VesselSource source;
  QString name = m_name->text().trimmed();
  if (m_tabs->currentIndex() == 0) {
    source.origin = VesselSource::Origin::Receiver;
    source.udp = m_udp->isChecked();
    source.host = m_host->text().trimmed();
    source.port = static_cast<uint16_t>(m_port->value());
    source.recordPath = m_record->text().trimmed();
    if (!source.udp && source.host.isEmpty()) {
      m_message->setText(tr("Enter the receiver's host name or address."));
      return;
    }
    if (name.isEmpty()) {
      name = source.udp
                 ? tr("AIS on UDP port %1").arg(source.port)
                 : tr("AIS from %1:%2").arg(source.host).arg(source.port);
    }
  } else {
    const QListWidgetItem* item = m_feeds->currentItem();
    if (item == nullptr) {
      m_message->setText(tr("Choose a feed."));
      return;
    }
    source.origin = VesselSource::Origin::Feed;
    source.feed = item->data(Qt::UserRole).toString();
    if (name.isEmpty()) {
      name = item->text();
    }
  }
  m_source = source;
  m_layerName = name;
  QDialog::accept();
}

VesselPropertiesDialog::VesselPropertiesDialog(const VesselStyle& style,
                                               const QString& name,
                                               ViewArea viewArea,
                                               QWidget* parent)
    : QDialog(parent), m_style(style), m_name(name),
      m_viewArea(std::move(viewArea)), m_color(style.color),
      m_colorButton(new QPushButton(this)),
      m_byType(new QCheckBox(tr("Coloured by ship type"), this)),
      m_markerSize(new QSpinBox(this)),
      m_labels(new QCheckBox(tr("Names"), this)),
      m_labelSpeed(new QCheckBox(tr("with speeds"), this)),
      m_labelDestination(new QCheckBox(tr("with destinations"), this)),
      m_courseVector(new QSpinBox(this)), m_trailMode(new QComboBox(this)),
      m_trailSpan(new QDoubleSpinBox(this)),
      m_trailColouring(new QComboBox(this)), m_stale(new QDoubleSpinBox(this)),
      m_gap(new QDoubleSpinBox(this)), m_nameFilter(new QLineEdit(this)),
      m_groups(new QListWidget(this)), m_statuses(new QListWidget(this)),
      m_hasMinimum(new QCheckBox(tr("At least"), this)),
      m_minimum(new QDoubleSpinBox(this)),
      m_hasMaximum(new QCheckBox(tr("At most"), this)),
      m_maximum(new QDoubleSpinBox(this)),
      m_hasArea(new QCheckBox(tr("Only within"), this)),
      m_useView(new QPushButton(tr("Use the View"), this))
{
  setObjectName(QStringLiteral("vesselPropertiesDialog"));
  setWindowTitle(tr("%1 – Ship Layer Properties").arg(name));
  m_trailMode->setObjectName(QStringLiteral("vesselTrailMode"));
  m_courseVector->setObjectName(QStringLiteral("courseVector"));
  m_groups->setObjectName(QStringLiteral("vesselTypeGroups"));
  m_byType->setChecked(style.colourByType);
  m_markerSize->setRange(4, 64);
  m_markerSize->setSuffix(tr(" px"));
  m_markerSize->setValue(static_cast<int32_t>(std::lround(style.markerSizePx)));
  m_labels->setChecked(style.labels);
  m_labelSpeed->setChecked(style.labelSpeed);
  m_labelDestination->setChecked(style.labelDestination);
  m_courseVector->setRange(0, 120);
  m_courseVector->setSuffix(tr(" min"));
  m_courseVector->setSpecialValueText(tr("None"));
  m_courseVector->setValue(
      static_cast<int32_t>(std::lround(style.courseVectorMinutes)));
  m_trailMode->addItem(tr("Off"), static_cast<int32_t>(TrailMode::Off));
  m_trailMode->addItem(tr("A time span before the clock"),
                       static_cast<int32_t>(TrailMode::Span));
  m_trailMode->addItem(tr("All the data up to the clock"),
                       static_cast<int32_t>(TrailMode::All));
  m_trailMode->setCurrentIndex(
      m_trailMode->findData(static_cast<int32_t>(style.trail.mode)));
  m_trailSpan->setRange(0.1, 24.0 * 60.0);
  m_trailSpan->setSuffix(tr(" min"));
  m_trailSpan->setValue(style.trail.spanS / 60.0);
  m_trailColouring->addItem(tr("The ship type"),
                            static_cast<int32_t>(VesselColouring::Type));
  m_trailColouring->addItem(tr("The layer's colour"),
                            static_cast<int32_t>(VesselColouring::Vessel));
  m_trailColouring->addItem(tr("Speed"),
                            static_cast<int32_t>(VesselColouring::Speed));
  m_trailColouring->setCurrentIndex(
      m_trailColouring->findData(static_cast<int32_t>(style.trailColouring)));
  for (QDoubleSpinBox* box : {m_stale, m_gap}) {
    box->setRange(0.5, 24.0 * 60.0);
    box->setSuffix(tr(" min"));
  }
  m_stale->setValue(style.staleMinimumS / 60.0);
  m_gap->setValue(style.gapS / 60.0);
  m_nameFilter->setText(style.nameFilter);
  m_nameFilter->setPlaceholderText(tr("Any name or MMSI"));
  // The type groups, with their colours: the legend.
  for (const ShipTypeGroup group : shipTypeLegend()) {
    QListWidgetItem* item = new QListWidgetItem(
        swatch(shipTypeColour(group)), shipTypeGroupName(group), m_groups);
    item->setData(Qt::UserRole, static_cast<int32_t>(group));
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(style.typeGroups.empty() ||
                                style.typeGroups.count(group) != 0
                            ? Qt::Checked
                            : Qt::Unchecked);
  }
  for (const AisNavStatus status :
       {AisNavStatus::UnderWayEngine, AisNavStatus::UnderWaySailing,
        AisNavStatus::AtAnchor, AisNavStatus::Moored, AisNavStatus::Fishing,
        AisNavStatus::RestrictedManoeuvre, AisNavStatus::NotUnderCommand,
        AisNavStatus::ConstrainedByDraught, AisNavStatus::Aground,
        AisNavStatus::AisSart, AisNavStatus::NotDefined}) {
    QListWidgetItem* item =
        new QListWidgetItem(navStatusName(status), m_statuses);
    item->setData(Qt::UserRole, static_cast<int32_t>(status));
    item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
    item->setCheckState(style.statuses.empty() ||
                                style.statuses.count(status) != 0
                            ? Qt::Checked
                            : Qt::Unchecked);
  }
  for (QDoubleSpinBox* box : {m_minimum, m_maximum}) {
    box->setRange(0.0, 100.0);
    box->setSuffix(tr(" kn"));
  }
  m_hasMinimum->setChecked(style.minimumSpeedKn.has_value());
  m_minimum->setValue(style.minimumSpeedKn.value_or(0.5));
  m_hasMaximum->setChecked(style.maximumSpeedKn.has_value());
  m_maximum->setValue(style.maximumSpeedKn.value_or(30.0));
  const LatLonBox area = style.area.value_or(LatLonBox());
  const std::array<double, 4> values = {area.southDeg, area.westDeg,
                                        area.northDeg, area.eastDeg};
  for (std::size_t i = 0; i < m_area.size(); ++i) {
    m_area.at(i) = new QDoubleSpinBox(this);
    m_area.at(i)->setDecimals(3);
    m_area.at(i)->setRange(i % 2 == 0 ? -90.0 : -180.0,
                           i % 2 == 0 ? 90.0 : 180.0);
    m_area.at(i)->setValue(values.at(i));
  }
  m_hasArea->setChecked(style.area.has_value());
  m_useView->setEnabled(static_cast<bool>(m_viewArea));

  QGroupBox* look = new QGroupBox(tr("Ships"), this);
  QFormLayout* lookForm = new QFormLayout(look);
  QHBoxLayout* colourRow = new QHBoxLayout;
  colourRow->addWidget(m_byType);
  colourRow->addWidget(m_colorButton);
  lookForm->addRow(tr("Colour:"), colourRow);
  lookForm->addRow(tr("Size:"), m_markerSize);
  QHBoxLayout* labelRow = new QHBoxLayout;
  labelRow->addWidget(m_labels);
  labelRow->addWidget(m_labelSpeed);
  labelRow->addWidget(m_labelDestination);
  lookForm->addRow(tr("Labels:"), labelRow);
  lookForm->addRow(tr("Course vectors:"), m_courseVector);
  lookForm->addRow(tr("Shown at least for:"), m_stale);
  QGroupBox* trails = new QGroupBox(tr("Trails"), this);
  QFormLayout* trailForm = new QFormLayout(trails);
  trailForm->addRow(tr("Show:"), m_trailMode);
  trailForm->addRow(tr("Span:"), m_trailSpan);
  trailForm->addRow(tr("Coloured by:"), m_trailColouring);
  trailForm->addRow(tr("Broken at gaps over:"), m_gap);
  QGroupBox* filters = new QGroupBox(tr("Show Only"), this);
  QGridLayout* grid = new QGridLayout(filters);
  grid->addWidget(new QLabel(tr("Name or MMSI:"), filters), 0, 0);
  grid->addWidget(m_nameFilter, 0, 1, 1, 3);
  grid->addWidget(new QLabel(tr("Ship types:"), filters), 1, 0, 1, 2);
  grid->addWidget(new QLabel(tr("Statuses:"), filters), 1, 2, 1, 2);
  grid->addWidget(m_groups, 2, 0, 1, 2);
  grid->addWidget(m_statuses, 2, 2, 1, 2);
  grid->addWidget(m_hasMinimum, 3, 0);
  grid->addWidget(m_minimum, 3, 1);
  grid->addWidget(m_hasMaximum, 3, 2);
  grid->addWidget(m_maximum, 3, 3);
  grid->addWidget(m_hasArea, 4, 0);
  grid->addWidget(m_useView, 4, 1);
  for (std::size_t i = 0; i < m_area.size(); ++i) {
    grid->addWidget(m_area.at(i), 5, static_cast<int32_t>(i));
  }
  QDialogButtonBox* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_colorButton, &QPushButton::clicked, this,
          &VesselPropertiesDialog::chooseColor);
  for (QCheckBox* box :
       {m_byType, m_labels, m_hasMinimum, m_hasMaximum, m_hasArea}) {
    connect(box, &QCheckBox::toggled, this,
            &VesselPropertiesDialog::updateEnabled);
  }
  connect(m_trailMode, qOverload<int32_t>(&QComboBox::currentIndexChanged),
          this, &VesselPropertiesDialog::updateEnabled);
  connect(m_useView, &QPushButton::clicked, this, [this]() {
    const std::optional<LatLonBox> view = m_viewArea();
    if (view) {
      m_hasArea->setChecked(true);
      m_area.at(0)->setValue(view->southDeg);
      m_area.at(1)->setValue(view->westDeg);
      m_area.at(2)->setValue(view->northDeg);
      m_area.at(3)->setValue(view->eastDeg);
    }
  });
  QHBoxLayout* top = new QHBoxLayout;
  top->addWidget(look);
  top->addWidget(trails);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addLayout(top);
  layout->addWidget(filters);
  layout->addWidget(buttons);
  updateSwatch();
  updateEnabled();
}

VesselPropertiesDialog::~VesselPropertiesDialog() = default;

void VesselPropertiesDialog::chooseColor()
{
  const QColor chosen =
      QColorDialog::getColor(m_color, this, tr("Colour of %1").arg(m_name),
                             QColorDialog::ShowAlphaChannel);
  if (chosen.isValid()) {
    m_color = chosen;
    updateSwatch();
  }
}

void VesselPropertiesDialog::updateSwatch()
{
  QPixmap pixmap(32, 16);
  pixmap.fill(m_color);
  m_colorButton->setIcon(QIcon(pixmap));
  m_colorButton->setText(m_color.name());
}

void VesselPropertiesDialog::updateEnabled()
{
  m_labelSpeed->setEnabled(m_labels->isChecked());
  m_labelDestination->setEnabled(m_labels->isChecked());
  m_trailSpan->setEnabled(
      static_cast<TrailMode>(m_trailMode->currentData().toInt()) ==
      TrailMode::Span);
  m_minimum->setEnabled(m_hasMinimum->isChecked());
  m_maximum->setEnabled(m_hasMaximum->isChecked());
  for (QDoubleSpinBox* box : m_area) {
    box->setEnabled(m_hasArea->isChecked());
  }
}

VesselStyle VesselPropertiesDialog::style() const
{
  VesselStyle s = m_style;
  s.color = m_color;
  s.colourByType = m_byType->isChecked();
  s.markerSizePx = m_markerSize->value();
  s.labels = m_labels->isChecked();
  s.labelSpeed = m_labelSpeed->isChecked();
  s.labelDestination = m_labelDestination->isChecked();
  s.courseVectorMinutes = m_courseVector->value();
  s.trail.mode = static_cast<TrailMode>(m_trailMode->currentData().toInt());
  s.trail.spanS = m_trailSpan->value() * 60.0;
  s.trailColouring =
      static_cast<VesselColouring>(m_trailColouring->currentData().toInt());
  s.staleMinimumS = m_stale->value() * 60.0;
  s.gapS = m_gap->value() * 60.0;
  s.nameFilter = m_nameFilter->text().trimmed();
  s.typeGroups.clear();
  bool allGroups = true;
  for (int32_t i = 0; i < m_groups->count(); ++i) {
    const QListWidgetItem* item = m_groups->item(i);
    if (item->checkState() == Qt::Checked) {
      s.typeGroups.insert(
          static_cast<ShipTypeGroup>(item->data(Qt::UserRole).toInt()));
    } else {
      allGroups = false;
    }
  }
  if (allGroups) {
    s.typeGroups.clear();
  }
  s.statuses.clear();
  bool allStatuses = true;
  for (int32_t i = 0; i < m_statuses->count(); ++i) {
    const QListWidgetItem* item = m_statuses->item(i);
    if (item->checkState() == Qt::Checked) {
      s.statuses.insert(
          static_cast<AisNavStatus>(item->data(Qt::UserRole).toInt()));
    } else {
      allStatuses = false;
    }
  }
  if (allStatuses) {
    s.statuses.clear();
  }
  s.minimumSpeedKn = m_hasMinimum->isChecked()
                         ? std::optional<double>(m_minimum->value())
                         : std::nullopt;
  s.maximumSpeedKn = m_hasMaximum->isChecked()
                         ? std::optional<double>(m_maximum->value())
                         : std::nullopt;
  s.area = m_hasArea->isChecked()
               ? std::optional<LatLonBox>(
                     LatLonBox{m_area.at(0)->value(), m_area.at(1)->value(),
                               m_area.at(2)->value(), m_area.at(3)->value()})
               : std::nullopt;
  return s;
}

} // namespace ts
