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
#include "aircraftwindows.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

#include <QAction>
#include <QCheckBox>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QTimeZone>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "ts/geo/geoformat.h"
#include "ts/layers/layertreemodel.h"

namespace ts
{

namespace
{

constexpr int32_t addressRole = Qt::UserRole + 1;

enum Column : int32_t {
  CallsignColumn,
  AddressColumn,
  RegistrationColumn,
  TypeColumn,
  AltitudeColumn,
  SpeedColumn,
  TrailColumn,
  ColumnCount,
};

QString feet(double value)
{
  return std::isnan(value)
             ? QString()
             : AircraftListDialog::tr("%1 ft").arg(QLocale().toString(
                   static_cast<int32_t>(std::lround(value))));
}

QString trailText(const std::optional<TrailSettings>& trail)
{
  if (!trail) {
    return AircraftListDialog::tr("Layer's");
  }
  switch (trail->mode) {
  case TrailMode::Off:
    return AircraftListDialog::tr("Off");
  case TrailMode::Span:
    return AircraftListDialog::tr(
        "%n min", nullptr,
        static_cast<int32_t>(std::lround(trail->spanS / 60.0)));
  case TrailMode::All:
    break;
  }
  return AircraftListDialog::tr("All");
}

QString emergencyText(AdsbEmergency emergency)
{
  switch (emergency) {
  case AdsbEmergency::None:
    return AircraftInfoWindow::tr("None");
  case AdsbEmergency::General:
    return AircraftInfoWindow::tr("Emergency");
  case AdsbEmergency::Medical:
    return AircraftInfoWindow::tr("Medical");
  case AdsbEmergency::MinimumFuel:
    return AircraftInfoWindow::tr("Minimum fuel");
  case AdsbEmergency::NoCommunications:
    return AircraftInfoWindow::tr("No communications");
  case AdsbEmergency::UnlawfulInterference:
    return AircraftInfoWindow::tr("Unlawful interference");
  case AdsbEmergency::Downed:
    return AircraftInfoWindow::tr("Downed aircraft");
  case AdsbEmergency::Reserved:
    break;
  }
  return AircraftInfoWindow::tr("Reserved");
}

} // namespace

AircraftListDialog::AircraftListDialog(QString nodeId, LayerTreeModel* layers,
                                       AircraftLayers* aircraft,
                                       QWidget* parent)
    : QDialog(parent), m_nodeId(std::move(nodeId)), m_layers(layers),
      m_aircraft(aircraft), m_filter(new QLineEdit(this)),
      m_list(new QTreeWidget(this)), m_summary(new QLabel(this))
{
  setObjectName(QStringLiteral("aircraftListDialog"));
  m_filter->setObjectName(QStringLiteral("aircraftFilter"));
  m_list->setObjectName(QStringLiteral("aircraftList"));
  m_filter->setClearButtonEnabled(true);
  m_filter->setPlaceholderText(tr("Find by callsign, address, registration "
                                  "or type"));
  m_list->setRootIsDecorated(false);
  m_list->setSortingEnabled(true);
  m_list->setContextMenuPolicy(Qt::CustomContextMenu);
  m_list->setHeaderLabels({tr("Callsign"), tr("ICAO address"),
                           tr("Registration"), tr("Type"), tr("Altitude"),
                           tr("Speed"), tr("Trail")});
  m_list->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
  QPushButton* showAll = new QPushButton(tr("Show All"), this);
  QPushButton* hideAll = new QPushButton(tr("Hide All"), this);
  QHBoxLayout* buttons = new QHBoxLayout;
  buttons->addWidget(showAll);
  buttons->addWidget(hideAll);
  buttons->addStretch(1);
  QDialogButtonBox* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
  buttons->addWidget(close);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(m_filter);
  layout->addWidget(m_list);
  layout->addWidget(m_summary);
  layout->addLayout(buttons);
  connect(close, &QDialogButtonBox::rejected, this, &QDialog::close);
  connect(showAll, &QPushButton::clicked, this,
          [this]() { setAllShown(true); });
  connect(hideAll, &QPushButton::clicked, this,
          [this]() { setAllShown(false); });
  connect(m_filter, &QLineEdit::textChanged, this, &AircraftListDialog::refill);
  connect(m_list, &QTreeWidget::itemChanged, this,
          &AircraftListDialog::itemChanged);
  connect(m_list, &QTreeWidget::itemDoubleClicked, this,
          [this](QTreeWidgetItem* item) {
            emit aircraftChosen(m_nodeId, item->data(0, addressRole).toUInt());
          });
  connect(m_list, &QWidget::customContextMenuRequested, this,
          &AircraftListDialog::showTrailMenu);
  connect(m_aircraft, &AircraftLayers::setChanged, this,
          &AircraftListDialog::refill);
  resize(720, 520);
  refill();
}

AircraftListDialog::~AircraftListDialog() = default;

int32_t AircraftListDialog::rowCount() const
{
  return m_list->topLevelItemCount();
}

void AircraftListDialog::setFilter(const QString& text)
{
  m_filter->setText(text);
}

void AircraftListDialog::refill()
{
  const QModelIndex index = m_layers->indexOfId(m_nodeId);
  if (!index.isValid()) {
    return;
  }
  const LayerDefinition definition = m_layers->layerDefinition(index);
  setWindowTitle(tr("%1 – Aircraft").arg(definition.name));
  const std::shared_ptr<const AircraftSet> set =
      m_aircraft->set(definition.source);
  const AircraftStyle style = AircraftStyle::fromJson(definition.style);
  const QString filter = m_filter->text().trimmed();
  const double time = m_aircraft->timeS();
  m_filling = true;
  m_list->setSortingEnabled(false);
  m_list->clear();
  int32_t total = 0;
  if (set) {
    for (const std::shared_ptr<const AircraftTrack>& track : set->tracks) {
      ++total;
      const AircraftInfo* info = track->infoAt(time);
      const AircraftInfo known = info != nullptr ? *info : AircraftInfo();
      const QString address = formatIcaoAddress(track->address);
      const std::array<const QString*, 4> texts = {
          &known.callsign, &address, &known.registration, &known.aircraftType};
      const bool found =
          filter.isEmpty() ||
          std::any_of(texts.begin(), texts.end(),
                      [&filter](const QString* text) {
                        return text->contains(filter, Qt::CaseInsensitive);
                      });
      if (found) {
        addRow(*track, style, time);
      }
    }
  }
  m_list->setSortingEnabled(true);
  m_filling = false;
  if (!set || set->state == AircraftSet::State::Loading) {
    m_summary->setText(tr("Reading…"));
  } else {
    m_summary->setText(tr("%1 of %n aircraft listed", nullptr, total)
                           .arg(m_list->topLevelItemCount()));
  }
}

void AircraftListDialog::addRow(const AircraftTrack& track,
                                const AircraftStyle& style, double timeS)
{
  const AircraftInfo* info = track.infoAt(timeS);
  const AircraftInfo known = info != nullptr ? *info : AircraftInfo();
  QTreeWidgetItem* item = new QTreeWidgetItem(m_list);
  item->setData(0, addressRole, track.address);
  item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
  item->setCheckState(
      0, style.hidden.count(track.address) == 0 ? Qt::Checked : Qt::Unchecked);
  item->setText(CallsignColumn, known.callsign);
  item->setText(AddressColumn, formatIcaoAddress(track.address));
  item->setText(RegistrationColumn, known.registration);
  item->setText(TypeColumn, known.aircraftType);
  const std::size_t at = track.track.indexAtOrBefore(timeS).value_or(0);
  if (!track.altitudes.empty()) {
    item->setText(AltitudeColumn,
                  feet(static_cast<double>(track.altitudes.at(at).baroFt)));
  }
  const TrackSample& sample = track.track.points().at(at);
  if (!std::isnan(sample.speedMS)) {
    item->setText(SpeedColumn, tr("%1 kt").arg(std::lround(
                                   static_cast<double>(sample.speedMS) /
                                   knotsToMetresPerSecond)));
  }
  const std::map<uint32_t, AircraftOverride>::const_iterator own =
      style.overrides.find(track.address);
  item->setText(TrailColumn, trailText(own != style.overrides.end()
                                           ? own->second.trail
                                           : std::optional<TrailSettings>()));
}

void AircraftListDialog::selectAircraft(uint32_t address)
{
  for (int32_t i = 0; i < m_list->topLevelItemCount(); ++i) {
    QTreeWidgetItem* item = m_list->topLevelItem(i);
    if (item->data(0, addressRole).toUInt() == address) {
      m_list->setCurrentItem(item);
      m_list->scrollToItem(item);
      return;
    }
  }
}

void AircraftListDialog::itemChanged(QTreeWidgetItem* item, int32_t column)
{
  if (m_filling || column != 0) {
    return;
  }
  const QModelIndex index = m_layers->indexOfId(m_nodeId);
  LayerDefinition definition = m_layers->layerDefinition(index);
  AircraftStyle style = AircraftStyle::fromJson(definition.style);
  const uint32_t address = item->data(0, addressRole).toUInt();
  if (item->checkState(0) == Qt::Checked) {
    style.hidden.erase(address);
  } else {
    style.hidden.insert(address);
  }
  definition.style = style.toJson();
  m_layers->setLayerDefinition(index, definition);
}

void AircraftListDialog::setAllShown(bool shown)
{
  const QModelIndex index = m_layers->indexOfId(m_nodeId);
  LayerDefinition definition = m_layers->layerDefinition(index);
  AircraftStyle style = AircraftStyle::fromJson(definition.style);
  style.hidden.clear();
  const std::shared_ptr<const AircraftSet> set =
      m_aircraft->set(definition.source);
  if (!shown && set) {
    for (const std::shared_ptr<const AircraftTrack>& track : set->tracks) {
      style.hidden.insert(track->address);
    }
  }
  definition.style = style.toJson();
  m_layers->setLayerDefinition(index, definition);
  refill();
}

void AircraftListDialog::setTrail(uint32_t address,
                                  const std::optional<TrailSettings>& trail)
{
  const QModelIndex index = m_layers->indexOfId(m_nodeId);
  LayerDefinition definition = m_layers->layerDefinition(index);
  AircraftStyle style = AircraftStyle::fromJson(definition.style);
  if (trail) {
    style.overrides[address].trail = trail;
  } else {
    style.overrides.erase(address);
  }
  definition.style = style.toJson();
  m_layers->setLayerDefinition(index, definition);
  refill();
  selectAircraft(address);
}

void AircraftListDialog::showTrailMenu(QPoint at)
{
  QTreeWidgetItem* item = m_list->itemAt(at);
  if (item == nullptr) {
    return;
  }
  const uint32_t address = item->data(0, addressRole).toUInt();
  QMenu menu(this);
  menu.addSection(tr("This aircraft's trail"));
  menu.addAction(tr("The layer's"),
                 [this, address]() { setTrail(address, std::nullopt); });
  menu.addAction(tr("Off"), [this, address]() {
    TrailSettings off;
    off.mode = TrailMode::Off;
    setTrail(address, off);
  });
  for (const int32_t minutes : {5, 15, 60}) {
    menu.addAction(tr("The last %n min", nullptr, minutes),
                   [this, address, minutes]() {
                     TrailSettings span;
                     span.mode = TrailMode::Span;
                     span.spanS = 60.0 * minutes;
                     setTrail(address, span);
                   });
  }
  menu.addAction(tr("All the data"), [this, address]() {
    TrailSettings all;
    all.mode = TrailMode::All;
    setTrail(address, all);
  });
  menu.exec(m_list->viewport()->mapToGlobal(at));
}

AircraftInfoWindow::AircraftInfoWindow(QWidget* parent)
    : QDialog(parent), m_title(new QLabel(this)),
      m_follow(new QPushButton(tr("Follow"), this)),
      m_export(new QPushButton(tr("Export Track…"), this))
{
  setObjectName(QStringLiteral("aircraftInfoWindow"));
  setWindowTitle(tr("Aircraft"));
  m_follow->setObjectName(QStringLiteral("followAircraft"));
  m_follow->setCheckable(true);
  m_export->setObjectName(QStringLiteral("exportAircraftTrack"));
  QFont bold = m_title->font();
  bold.setBold(true);
  bold.setPointSizeF(bold.pointSizeF() * 1.2);
  m_title->setFont(bold);
  QFormLayout* form = new QFormLayout;
  const std::vector<std::pair<QString, QString>> rows = {
      {QStringLiteral("address"), tr("ICAO address:")},
      {QStringLiteral("callsign"), tr("Callsign:")},
      {QStringLiteral("registration"), tr("Registration:")},
      {QStringLiteral("type"), tr("Type:")},
      {QStringLiteral("category"), tr("Category:")},
      {QStringLiteral("squawk"), tr("Squawk:")},
      {QStringLiteral("emergency"), tr("Emergency:")},
      {QStringLiteral("baroAltitude"), tr("Barometric altitude:")},
      {QStringLiteral("geometricAltitude"), tr("Geometric altitude:")},
      {QStringLiteral("height"), tr("Height above the ellipsoid:")},
      {QStringLiteral("speed"), tr("Ground speed:")},
      {QStringLiteral("track"), tr("Track:")},
      {QStringLiteral("verticalRate"), tr("Vertical rate:")},
      {QStringLiteral("position"), tr("Position:")},
      {QStringLiteral("lastReport"), tr("Last report:")},
      {QStringLiteral("layer"), tr("Layer:")},
  };
  for (const std::pair<QString, QString>& row : rows) {
    QLabel* value = new QLabel(this);
    value->setObjectName(row.first);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(row.second, value);
  }
  QHBoxLayout* buttons = new QHBoxLayout;
  buttons->addWidget(m_follow);
  buttons->addWidget(m_export);
  buttons->addStretch(1);
  QDialogButtonBox* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
  buttons->addWidget(close);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(m_title);
  layout->addLayout(form);
  layout->addLayout(buttons);
  connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_follow, &QPushButton::toggled, this,
          &AircraftInfoWindow::followToggled);
  connect(m_export, &QPushButton::clicked, this,
          &AircraftInfoWindow::exportRequested);
}

AircraftInfoWindow::~AircraftInfoWindow() = default;

QLabel* AircraftInfoWindow::field(const QString& name)
{
  return findChild<QLabel*>(name);
}

QString AircraftInfoWindow::fieldText(const QString& name) const
{
  const QLabel* label = findChild<QLabel*>(name);
  return label != nullptr ? label->text() : QString();
}

void AircraftInfoWindow::setFollowing(bool following)
{
  const QSignalBlocker blocker(m_follow);
  m_follow->setChecked(following);
}

void AircraftInfoWindow::showAircraft(const DrawnAircraft& aircraft,
                                      const QString& layerName, double timeS)
{
  const QLocale locale;
  const AircraftInfo& info = aircraft.info;
  const TrackSample& at = aircraft.at;
  const QString address = formatIcaoAddress(aircraft.track->address);
  m_title->setText(info.callsign.isEmpty() ? address : info.callsign);
  const std::function<QString(double, const QString&)> orDash =
      [](double value, const QString& text) {
        return std::isnan(value) ? QStringLiteral("–") : text;
      };
  const double baro = static_cast<double>(aircraft.altitudes.baroFt);
  const double geometric = static_cast<double>(aircraft.altitudes.geometricFt);
  const double speed = static_cast<double>(at.speedMS);
  const double track = static_cast<double>(at.trackDeg);
  const double rate = static_cast<double>(at.verticalRateMS);
  field(QStringLiteral("address"))->setText(address);
  field(QStringLiteral("callsign"))->setText(info.callsign);
  field(QStringLiteral("registration"))->setText(info.registration);
  field(QStringLiteral("type"))->setText(info.aircraftType);
  field(QStringLiteral("category"))->setText(info.category);
  field(QStringLiteral("squawk"))->setText(info.squawk);
  field(QStringLiteral("emergency"))->setText(emergencyText(info.emergency));
  field(QStringLiteral("baroAltitude"))
      ->setText(
          info.onGround.value_or(false) && std::isnan(baro)
              ? tr("On the ground")
              : orDash(baro, tr("%1 ft").arg(locale.toString(
                                 static_cast<int32_t>(std::lround(baro))))));
  field(QStringLiteral("geometricAltitude"))
      ->setText(orDash(geometric,
                       tr("%1 ft").arg(locale.toString(
                           static_cast<int32_t>(std::lround(geometric))))));
  field(QStringLiteral("height"))
      ->setText(orDash(
          at.heightM, (at.heightApproximate ? tr("about %1 m") : tr("%1 m"))
                          .arg(locale.toString(
                              static_cast<int32_t>(std::lround(at.heightM))))));
  field(QStringLiteral("speed"))
      ->setText(orDash(
          speed, tr("%1 kt").arg(std::lround(speed / knotsToMetresPerSecond))));
  field(QStringLiteral("track"))
      ->setText(orDash(track, tr("%1°").arg(std::lround(track))));
  field(QStringLiteral("verticalRate"))
      ->setText(
          orDash(rate, tr("%1 ft/min")
                           .arg(locale.toString(static_cast<int32_t>(
                               std::lround(rate / (feetToMetres / 60.0)))))));
  field(QStringLiteral("position"))
      ->setText(
          QStringLiteral("%1 %2").arg(formatLatitude(at.latDeg, locale),
                                      formatLongitude(at.lonDeg, locale)));
  const double ago = timeS - aircraft.lastReportS;
  field(QStringLiteral("lastReport"))
      ->setText(tr("%1 (%2 s before the clock)")
                    .arg(QDateTime::fromMSecsSinceEpoch(
                             std::llround(aircraft.lastReportS * 1000.0),
                             QTimeZone::utc())
                             .toString(Qt::ISODate))
                    .arg(std::lround(std::max(ago, 0.0))));
  field(QStringLiteral("layer"))->setText(layerName);
}

} // namespace ts
