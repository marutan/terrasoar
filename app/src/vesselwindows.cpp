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
#include "vesselwindows.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

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
#include <QTreeWidget>
#include <QVBoxLayout>

#include "aisdialogs.h"
#include "ts/geo/geoformat.h"
#include "ts/layers/layertreemodel.h"

namespace ts
{

namespace
{

constexpr int32_t mmsiRole = Qt::UserRole + 1;

/** Knots in metres a second. */
constexpr double knot = 1852.0 / 3600.0;

enum Column : int32_t {
  NameColumn,
  MmsiColumn,
  TypeColumn,
  StatusColumn,
  SpeedColumn,
  DestinationColumn,
  TrailColumn,
};

QString trailText(const std::optional<TrailSettings>& trail)
{
  if (!trail) {
    return VesselListDialog::tr("Layer's");
  }
  switch (trail->mode) {
  case TrailMode::Off:
    return VesselListDialog::tr("Off");
  case TrailMode::Span:
    return VesselListDialog::tr(
        "%n min", nullptr,
        static_cast<int32_t>(std::lround(trail->spanS / 60.0)));
  case TrailMode::All:
    break;
  }
  return VesselListDialog::tr("All");
}

QString timeText(double timeS)
{
  return QDateTime::fromMSecsSinceEpoch(std::llround(timeS * 1000.0))
      .toUTC()
      .toString(Qt::ISODate);
}

} // namespace

VesselListDialog::VesselListDialog(QString nodeId, LayerTreeModel* layers,
                                   VesselLayers* vessels, QWidget* parent)
    : QDialog(parent), m_nodeId(std::move(nodeId)), m_layers(layers),
      m_vessels(vessels), m_filter(new QLineEdit(this)),
      m_list(new QTreeWidget(this)), m_summary(new QLabel(this))
{
  setObjectName(QStringLiteral("vesselListDialog"));
  m_filter->setObjectName(QStringLiteral("vesselFilter"));
  m_list->setObjectName(QStringLiteral("vesselList"));
  m_filter->setClearButtonEnabled(true);
  m_filter->setPlaceholderText(tr("Find by name, MMSI or destination"));
  m_list->setRootIsDecorated(false);
  m_list->setSortingEnabled(true);
  m_list->setContextMenuPolicy(Qt::CustomContextMenu);
  m_list->setHeaderLabels({tr("Name"), tr("MMSI"), tr("Type"), tr("Status"),
                           tr("Speed"), tr("Destination"), tr("Trail")});
  m_list->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
  QPushButton* showAll = new QPushButton(tr("Show All"), this);
  QPushButton* hideAll = new QPushButton(tr("Hide All"), this);
  QDialogButtonBox* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
  QHBoxLayout* buttons = new QHBoxLayout;
  buttons->addWidget(showAll);
  buttons->addWidget(hideAll);
  buttons->addStretch(1);
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
  connect(m_filter, &QLineEdit::textChanged, this, &VesselListDialog::refill);
  connect(m_list, &QTreeWidget::itemChanged, this,
          &VesselListDialog::itemChanged);
  connect(m_list, &QTreeWidget::itemDoubleClicked, this,
          [this](QTreeWidgetItem* item) {
            emit vesselChosen(m_nodeId, item->data(0, mmsiRole).toUInt());
          });
  connect(m_list, &QWidget::customContextMenuRequested, this,
          &VesselListDialog::showTrailMenu);
  connect(m_vessels, &VesselLayers::setChanged, this,
          &VesselListDialog::refill);
  resize(760, 520);
  refill();
}

VesselListDialog::~VesselListDialog() = default;

int32_t VesselListDialog::rowCount() const
{
  return m_list->topLevelItemCount();
}

void VesselListDialog::setFilter(const QString& text)
{
  m_filter->setText(text);
}

void VesselListDialog::refill()
{
  const QModelIndex index = m_layers->indexOfId(m_nodeId);
  if (!index.isValid()) {
    return;
  }
  const LayerDefinition definition = m_layers->layerDefinition(index);
  setWindowTitle(tr("%1 – Ships").arg(definition.name));
  const std::shared_ptr<const VesselSet> set =
      m_vessels->set(definition.source);
  const VesselStyle style = VesselStyle::fromJson(definition.style);
  const QString filter = m_filter->text().trimmed();
  const double time = m_vessels->timeS();
  m_filling = true;
  m_list->setSortingEnabled(false);
  m_list->clear();
  int32_t total = 0;
  if (set) {
    for (const std::shared_ptr<const VesselTrack>& track : set->tracks) {
      ++total;
      const VesselInfo* info = track->infoAt(time);
      const VesselInfo known = info != nullptr ? *info : VesselInfo();
      const QString mmsi = QString::number(track->mmsi);
      const std::array<const QString*, 3> texts = {&known.name, &mmsi,
                                                   &known.destination};
      if (filter.isEmpty() ||
          std::any_of(texts.begin(), texts.end(),
                      [&filter](const QString* text) {
                        return text->contains(filter, Qt::CaseInsensitive);
                      })) {
        addRow(*track, style, time);
      }
    }
  }
  m_list->setSortingEnabled(true);
  m_filling = false;
  m_summary->setText(!set || set->state == VesselSet::State::Loading
                         ? tr("Reading…")
                         : tr("%1 of %n station(s) listed", nullptr, total)
                               .arg(m_list->topLevelItemCount()));
}

void VesselListDialog::addRow(const VesselTrack& track,
                              const VesselStyle& style, double timeS)
{
  const VesselInfo* info = track.infoAt(timeS);
  const VesselInfo known = info != nullptr ? *info : VesselInfo();
  QTreeWidgetItem* item = new QTreeWidgetItem(m_list);
  item->setData(0, mmsiRole, track.mmsi);
  item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
  item->setCheckState(0, style.hidden.count(track.mmsi) == 0 ? Qt::Checked
                                                             : Qt::Unchecked);
  item->setText(NameColumn, known.name);
  item->setText(MmsiColumn, QString::number(track.mmsi));
  item->setText(TypeColumn, shipTypeGroupName(VesselStyle::groupOf(known)));
  item->setText(StatusColumn, navStatusName(known.status));
  const std::size_t at = track.track.indexAtOrBefore(timeS).value_or(0);
  const TrackSample& sample = track.track.points().at(at);
  if (!std::isnan(sample.speedMS)) {
    item->setText(SpeedColumn,
                  tr("%1 kn").arg(QLocale().toString(
                      static_cast<double>(sample.speedMS) / knot, 'f', 1)));
  }
  item->setText(DestinationColumn, known.destination);
  const std::map<uint32_t, TrailSettings>::const_iterator own =
      style.trails.find(track.mmsi);
  item->setText(TrailColumn,
                trailText(own != style.trails.end()
                              ? std::optional<TrailSettings>(own->second)
                              : std::nullopt));
}

void VesselListDialog::selectVessel(uint32_t mmsi)
{
  for (int32_t i = 0; i < m_list->topLevelItemCount(); ++i) {
    QTreeWidgetItem* item = m_list->topLevelItem(i);
    if (item->data(0, mmsiRole).toUInt() == mmsi) {
      m_list->setCurrentItem(item);
      m_list->scrollToItem(item);
      return;
    }
  }
}

void VesselListDialog::itemChanged(QTreeWidgetItem* item, int32_t column)
{
  if (m_filling || column != 0) {
    return;
  }
  const QModelIndex index = m_layers->indexOfId(m_nodeId);
  LayerDefinition definition = m_layers->layerDefinition(index);
  VesselStyle style = VesselStyle::fromJson(definition.style);
  const uint32_t mmsi = item->data(0, mmsiRole).toUInt();
  if (item->checkState(0) == Qt::Checked) {
    style.hidden.erase(mmsi);
  } else {
    style.hidden.insert(mmsi);
  }
  definition.style = style.toJson();
  m_layers->setLayerDefinition(index, definition);
}

void VesselListDialog::setAllShown(bool shown)
{
  const QModelIndex index = m_layers->indexOfId(m_nodeId);
  LayerDefinition definition = m_layers->layerDefinition(index);
  VesselStyle style = VesselStyle::fromJson(definition.style);
  style.hidden.clear();
  const std::shared_ptr<const VesselSet> set =
      m_vessels->set(definition.source);
  if (!shown && set) {
    for (const std::shared_ptr<const VesselTrack>& track : set->tracks) {
      style.hidden.insert(track->mmsi);
    }
  }
  definition.style = style.toJson();
  m_layers->setLayerDefinition(index, definition);
  refill();
}

void VesselListDialog::setTrail(uint32_t mmsi,
                                const std::optional<TrailSettings>& trail)
{
  const QModelIndex index = m_layers->indexOfId(m_nodeId);
  LayerDefinition definition = m_layers->layerDefinition(index);
  VesselStyle style = VesselStyle::fromJson(definition.style);
  if (trail) {
    style.trails[mmsi] = *trail;
  } else {
    style.trails.erase(mmsi);
  }
  definition.style = style.toJson();
  m_layers->setLayerDefinition(index, definition);
  refill();
  selectVessel(mmsi);
}

void VesselListDialog::showTrailMenu(QPoint at)
{
  const QTreeWidgetItem* item = m_list->itemAt(at);
  if (item == nullptr) {
    return;
  }
  const uint32_t mmsi = item->data(0, mmsiRole).toUInt();
  QMenu menu(this);
  menu.addSection(tr("This station's trail"));
  menu.addAction(tr("The layer's"),
                 [this, mmsi]() { setTrail(mmsi, std::nullopt); });
  menu.addAction(tr("Off"), [this, mmsi]() {
    setTrail(mmsi, TrailSettings{TrailMode::Off, 0.0});
  });
  for (const int32_t minutes : {15, 60, 240}) {
    menu.addAction(
        tr("The last %n min", nullptr, minutes), [this, mmsi, minutes]() {
          setTrail(mmsi, TrailSettings{TrailMode::Span, 60.0 * minutes});
        });
  }
  menu.addAction(tr("All the data"), [this, mmsi]() {
    setTrail(mmsi, TrailSettings{TrailMode::All, 0.0});
  });
  menu.exec(m_list->viewport()->mapToGlobal(at));
}

VesselInfoWindow::VesselInfoWindow(QWidget* parent)
    : QDialog(parent), m_title(new QLabel(this)),
      m_follow(new QPushButton(tr("Follow"), this)),
      m_export(new QPushButton(tr("Export Track…"), this))
{
  setObjectName(QStringLiteral("vesselInfoWindow"));
  setWindowTitle(tr("Ship"));
  m_follow->setObjectName(QStringLiteral("followVessel"));
  m_follow->setCheckable(true);
  m_export->setObjectName(QStringLiteral("exportVesselTrack"));
  QFont bold = m_title->font();
  bold.setBold(true);
  bold.setPointSizeF(bold.pointSizeF() * 1.2);
  m_title->setFont(bold);
  QFormLayout* form = new QFormLayout;
  const std::vector<std::pair<QString, QString>> rows = {
      {QStringLiteral("mmsi"), tr("MMSI:")},
      {QStringLiteral("name"), tr("Name:")},
      {QStringLiteral("callsign"), tr("Callsign:")},
      {QStringLiteral("imo"), tr("IMO number:")},
      {QStringLiteral("type"), tr("Type:")},
      {QStringLiteral("dimensions"), tr("Length and beam:")},
      {QStringLiteral("draught"), tr("Draught:")},
      {QStringLiteral("destination"), tr("Destination:")},
      {QStringLiteral("eta"), tr("ETA:")},
      {QStringLiteral("status"), tr("Status:")},
      {QStringLiteral("speed"), tr("Speed:")},
      {QStringLiteral("course"), tr("Course:")},
      {QStringLiteral("heading"), tr("Heading:")},
      {QStringLiteral("turn"), tr("Rate of turn:")},
      {QStringLiteral("position"), tr("Position:")},
      {QStringLiteral("lastPosition"), tr("Last position report:")},
      {QStringLiteral("lastStatic"), tr("Last static report:")},
      {QStringLiteral("layer"), tr("Layer:")},
  };
  for (const std::pair<QString, QString>& row : rows) {
    QLabel* value = new QLabel(this);
    value->setObjectName(row.first);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(row.second, value);
  }
  QDialogButtonBox* close = new QDialogButtonBox(QDialogButtonBox::Close, this);
  QHBoxLayout* buttons = new QHBoxLayout;
  buttons->addWidget(m_follow);
  buttons->addWidget(m_export);
  buttons->addStretch(1);
  buttons->addWidget(close);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(m_title);
  layout->addLayout(form);
  layout->addLayout(buttons);
  connect(close, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_follow, &QPushButton::toggled, this,
          &VesselInfoWindow::followToggled);
  connect(m_export, &QPushButton::clicked, this,
          &VesselInfoWindow::exportRequested);
}

VesselInfoWindow::~VesselInfoWindow() = default;

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void VesselInfoWindow::setField(const QString& name, const QString& text)
{
  QLabel* label = findChild<QLabel*>(name);
  if (label != nullptr) {
    label->setText(text.isEmpty() ? QStringLiteral("–") : text);
  }
}

QString VesselInfoWindow::fieldText(const QString& name) const
{
  const QLabel* label = findChild<QLabel*>(name);
  return label != nullptr ? label->text() : QString();
}

void VesselInfoWindow::setFollowing(bool following)
{
  const QSignalBlocker blocker(m_follow);
  m_follow->setChecked(following);
}

void VesselInfoWindow::showVessel(const DrawnVessel& vessel,
                                  const QString& layerName, double timeS)
{
  const QLocale locale;
  const VesselInfo& info = vessel.info;
  const TrackSample& at = vessel.at;
  const QString mmsi = QString::number(vessel.track->mmsi);
  m_title->setText(info.name.isEmpty() ? mmsi : info.name);
  setField(QStringLiteral("mmsi"), mmsi);
  setField(QStringLiteral("name"), info.name);
  setField(QStringLiteral("callsign"), info.callsign);
  setField(QStringLiteral("imo"),
           info.imo != 0 ? QString::number(info.imo) : QString());
  setField(QStringLiteral("type"),
           info.shipType != 0
               ? tr("%1 (%2)")
                     .arg(shipTypeGroupName(VesselStyle::groupOf(info)))
                     .arg(info.shipType)
               : shipTypeGroupName(VesselStyle::groupOf(info)));
  const AisDimensions& d = info.dimensions;
  setField(QStringLiteral("dimensions"),
           d.isKnown() ? tr("%1 m by %2 m")
                             .arg(d.toBow + d.toStern)
                             .arg(d.toPort + d.toStarboard)
                       : QString());
  setField(QStringLiteral("draught"),
           info.draughtM > 0.0
               ? tr("%1 m").arg(locale.toString(info.draughtM, 'f', 1))
               : QString());
  setField(QStringLiteral("destination"), info.destination);
  setField(QStringLiteral("eta"),
           info.eta ? tr("%1-%2 %3:%4 UTC")
                          .arg(info.eta->month, 2, 10, QLatin1Char('0'))
                          .arg(info.eta->day, 2, 10, QLatin1Char('0'))
                          .arg(info.eta->hour, 2, 10, QLatin1Char('0'))
                          .arg(info.eta->minute, 2, 10, QLatin1Char('0'))
                    : QString());
  setField(QStringLiteral("status"), navStatusName(info.status));
  const double speed = static_cast<double>(at.speedMS);
  const double course = static_cast<double>(at.trackDeg);
  const double heading = static_cast<double>(vessel.motion.headingDeg);
  const double turn = static_cast<double>(vessel.motion.turnDegMin);
  setField(QStringLiteral("speed"),
           std::isnan(speed)
               ? QString()
               : tr("%1 kn").arg(locale.toString(speed / knot, 'f', 1)));
  setField(QStringLiteral("course"),
           std::isnan(course) ? QString()
                              : tr("%1°").arg(locale.toString(course, 'f', 1)));
  setField(QStringLiteral("heading"),
           std::isnan(heading) ? QString()
                               : tr("%1°").arg(std::lround(heading)));
  setField(QStringLiteral("turn"),
           std::isnan(turn) ? QString()
                            : tr("%1°/min").arg(locale.toString(turn, 'f', 0)));
  setField(QStringLiteral("position"),
           QStringLiteral("%1 %2").arg(formatLatitude(at.latDeg, locale),
                                       formatLongitude(at.lonDeg, locale)));
  setField(QStringLiteral("lastPosition"),
           tr("%1 (%2 s before the clock)")
               .arg(timeText(vessel.lastReportS))
               .arg(std::lround(std::max(timeS - vessel.lastReportS, 0.0))));
  setField(QStringLiteral("lastStatic"),
           info.staticTimeS > 0.0 ? timeText(info.staticTimeS) : QString());
  setField(QStringLiteral("layer"), layerName);
}

} // namespace ts
