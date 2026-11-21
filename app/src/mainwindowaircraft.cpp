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

// The main window's aircraft: aircraft layers from ADS-B recordings and
// receivers, their drawing, details on click, following and export
// (Phase 17). See docs/design/adsb.md.

#include <algorithm>
#include <memory>

#include <QAction>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QStatusBar>
#include <QTimeZone>

#include "adsbdialogs.h"
#include "aircraftlayers.h"
#include "aircraftpropertiesdialog.h"
#include "aircraftwindows.h"
#include "globeview.h"
#include "layertreedock.h"
#include "mainwindow.h"
#include "satellitelayers.h"
#include "timebar.h"
#include "ts/adsb/aircraftexport.h"
#include "ts/adsb/aircraftlayer.h"
#include "ts/elevation/geoidmodel.h"
#include "ts/layers/layertreemodel.h"
#include "ts/providers/providerregistry.h"
#include "ts/time/programclock.h"

namespace ts
{

void MainWindow::createAircraft()
{
  m_aircraft = new AircraftLayers(
      m_clock, [this]() { return m_registry->network(); },
      [this]() {
        return m_registry->geoid(QStringLiteral("egm2008"), nullptr);
      },
      this);
  connect(m_aircraft, &AircraftLayers::drawsChanged, this,
          &MainWindow::aircraftDrawn);
  connect(m_aircraft, &AircraftLayers::trailsChanged, this,
          &MainWindow::aircraftTrailsChanged);
  connect(m_aircraft, &AircraftLayers::setChanged, this,
          &MainWindow::aircraftSetChanged);
  m_importAdsbAction = new QAction(this);
  m_importAdsbAction->setObjectName(QStringLiteral("importAdsbAction"));
  connect(m_importAdsbAction, &QAction::triggered, this,
          [this]() { importAdsbData(); });
  m_connectReceiverAction = new QAction(this);
  m_connectReceiverAction->setObjectName(
      QStringLiteral("connectReceiverAction"));
  connect(m_connectReceiverAction, &QAction::triggered, this,
          &MainWindow::connectAdsbReceiver);
}

void MainWindow::refreshAircraftLayers(const LayerStack& stack)
{
  std::vector<AircraftLayerEntry> layers;
  for (const VectorStackEntry& entry : stack.vectors) {
    if (AircraftSource::isAircraft(entry.definition.source)) {
      layers.push_back(
          AircraftLayerEntry{entry.nodeId, entry.definition, entry.alpha});
    }
  }
  m_aircraft->setLayers(std::move(layers));
}

void MainWindow::aircraftDrawn()
{
  if (m_globeView != nullptr) {
    m_globeView->setAircraft(m_aircraft->markers(), m_aircraft->labels());
    m_globeView->setSpaceLines(m_aircraft->dropLines(), 2);
    m_globeView->setSpaceLineTime(m_aircraft->timeS());
    if (m_followAircraft) {
      const std::optional<DrawnAircraft> selected = m_aircraft->selected();
      if (selected) {
        CameraPose pose = m_globeView->controller().camera().pose();
        pose.targetLatDeg = selected->at.latDeg;
        pose.targetLonDeg = selected->at.lonDeg;
        m_globeView->setPose(pose);
      }
    }
  }
  updateAircraftInfo();
}

void MainWindow::aircraftTrailsChanged()
{
  if (m_globeView != nullptr) {
    m_globeView->setSpaceLines(m_aircraft->trails(), 1);
  }
}

void MainWindow::aircraftSetChanged(const QJsonObject& source)
{
  const std::shared_ptr<const AircraftSet> set = m_aircraft->set(source);
  if (!set) {
    return;
  }
  const QString key = AircraftLayers::keyOf(source);
  for (const AircraftLayerEntry& layer : m_aircraft->layers()) {
    if (layer.definition.source != source) {
      continue;
    }
    ProviderStatus status;
    if (set->state == AircraftSet::State::Failed) {
      status = ProviderStatus{ProviderStatus::State::Error, set->error};
    } else if (set->live && set->feed == ReceiverFeed::State::Reconnecting) {
      status = ProviderStatus{
          ProviderStatus::State::Offline,
          tr("Can't reach the receiver (%1); trying again.").arg(set->error)};
    } else if (set->read.problemCount > 0) {
      const AdsbReadProblem& first = set->read.problems.front();
      status = ProviderStatus{
          ProviderStatus::State::Offline,
          tr("%n line(s) couldn't be read; the first, line %1: %2", nullptr,
             static_cast<int32_t>(set->read.problemCount))
              .arg(first.line)
              .arg(first.message)};
    } else if (set->read.untimed) {
      status = ProviderStatus{
          ProviderStatus::State::Offline,
          tr("The messages have no times: all are placed at the start.")};
    }
    m_layers->setLayerStatus(layer.nodeId, status);
    // A recording, the first time it is read: the clock goes to its start
    // if it is outside it, paused, so its aircraft are there to play.
    if (!set->live && set->state == AircraftSet::State::Ready &&
        set->firstS > 0.0 && m_aircraftStarted.insert(key).second) {
      const double now =
          static_cast<double>(m_clock->time().toMSecsSinceEpoch()) / 1000.0;
      if (now < set->firstS || now > set->lastS) {
        m_clock->setTime(QDateTime::fromMSecsSinceEpoch(
            std::llround(set->firstS * 1000.0), QTimeZone::utc()));
        statusBar()->showMessage(
            tr("%1: the clock is at the start of the recording; press Play "
               "to play it.")
                .arg(layer.definition.name),
            15000);
      }
    } else if (set->state == AircraftSet::State::Failed) {
      statusBar()->showMessage(
          tr("%1: %2").arg(layer.definition.name, set->error), 10000);
    }
  }
  // The time slider spans the recordings.
  updateTimeBar(timedStack());
}

void MainWindow::importAdsbData(const QString& path)
{
  ImportAdsbDialog dialog(path, this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  addAircraftLayer(dialog.layerDefinition());
}

void MainWindow::connectAdsbReceiver()
{
  ReceiverDialog dialog(this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  addAircraftLayer(dialog.layerDefinition());
}

QString MainWindow::addAircraftLayer(const LayerDefinition& definition)
{
  const QModelIndex index = m_layers->addLayer(definition);
  return m_layers->data(index, LayerTreeModel::NodeIdRole).toString();
}

bool MainWindow::openAdsbFile(const QString& path, QString* errorString)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    if (errorString != nullptr) {
      *errorString = file.errorString();
    }
    return false;
  }
  QByteArray head = file.read(8192);
  if (head.startsWith("\x1f\x8b")) {
    file.seek(0);
    head = gunzipIfCompressed(file.readAll()).left(8192);
  }
  const AdsbFormat format = detectAdsbFormat(head);
  const bool timedRaw = format == AdsbFormat::Raw &&
                        parseRawLine(head.left(head.indexOf('\n')).trimmed())
                            .value_or(TimedMessage())
                            .timeS.has_value();
  if (format == AdsbFormat::Sbs || format == AdsbFormat::ReadsbJson ||
      format == AdsbFormat::Trace || timedRaw) {
    AircraftSource source;
    source.path = QFileInfo(path).absoluteFilePath();
    source.format = format;
    addAircraftLayer(ImportAdsbDialog::definitionFor(
        source.toJson(), QFileInfo(path).completeBaseName()));
    return true;
  }
  if (format == AdsbFormat::Unknown) {
    if (errorString != nullptr) {
      *errorString = tr("%1 isn't a file Terra Soar reads.")
                         .arg(QFileInfo(path).fileName());
    }
    return false;
  }
  // Beast, counter-timed AVR and CSV need questions answered.
  importAdsbData(path);
  return true;
}

AircraftListDialog* MainWindow::showAircraftList(const QString& nodeId)
{
  const QModelIndex index = m_layers->indexOfId(nodeId);
  if (!index.isValid() ||
      !AircraftSource::isAircraft(m_layers->layerDefinition(index).source)) {
    return nullptr;
  }
  if (m_aircraftList && m_aircraftList->nodeId() != nodeId) {
    m_aircraftList->close();
    m_aircraftList->deleteLater();
    m_aircraftList = nullptr;
  }
  if (!m_aircraftList) {
    m_aircraftList = new AircraftListDialog(nodeId, m_layers, m_aircraft, this);
    m_aircraftList->setAttribute(Qt::WA_DeleteOnClose);
    connect(m_aircraftList, &AircraftListDialog::aircraftChosen, this,
            [this](const QString& layer, uint32_t address) {
              selectAircraft(layer, address);
            });
  }
  m_aircraftList->show();
  m_aircraftList->raise();
  return m_aircraftList;
}

void MainWindow::showAircraftProperties(const QString& nodeId)
{
  const QModelIndex index = m_layers->indexOfId(nodeId);
  if (!index.isValid()) {
    return;
  }
  LayerDefinition definition = m_layers->layerDefinition(index);
  AircraftPropertiesDialog dialog(
      AircraftStyle::fromJson(definition.style),
      m_layers->data(index).toString(),
      [this]() -> std::optional<LatLonBox> {
        if (m_globeView == nullptr) {
          return std::nullopt;
        }
        const std::optional<RegionGeometry> area = m_globeView->visibleArea();
        if (!area) {
          return std::nullopt;
        }
        const GeoRect& rect = area->extent();
        return LatLonBox{rect.southDeg, normaliseLonDeg(rect.westDeg),
                         rect.northDeg, normaliseLonDeg(rect.eastDeg)};
      },
      this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  definition.style = dialog.style().toJson();
  m_layers->setLayerDefinition(index, definition);
}

void MainWindow::selectAircraft(const QString& nodeId,
                                std::optional<uint32_t> address)
{
  m_aircraft->select(nodeId, address);
  if (!address) {
    setFollowingAircraft(false);
    if (m_aircraftInfo != nullptr) {
      m_aircraftInfo->hide();
    }
    return;
  }
  if (m_aircraftInfo == nullptr) {
    m_aircraftInfo = new AircraftInfoWindow(this);
    connect(m_aircraftInfo, &AircraftInfoWindow::followToggled, this,
            &MainWindow::setFollowingAircraft);
    connect(m_aircraftInfo, &AircraftInfoWindow::exportRequested, this,
            [this]() {
              const std::optional<uint32_t> selected =
                  m_aircraft->selectedAddress();
              if (!selected) {
                return;
              }
              const QString suggested =
                  formatIcaoAddress(*selected) + QStringLiteral(".kml");
              const QString path = QFileDialog::getSaveFileName(
                  this, tr("Export Track"), suggested,
                  tr("KML (*.kml);;GPX (*.gpx);;CSV (*.csv)"));
              if (path.isEmpty()) {
                return;
              }
              QString error;
              if (!exportAircraftTrack(m_aircraft->selectedLayer(), *selected,
                                       path, &error)) {
                QMessageBox::warning(this, tr("Export Track"), error);
              }
            });
    connect(m_aircraftInfo, &QDialog::finished, this,
            [this]() { selectAircraft(QString(), std::nullopt); });
  }
  m_aircraftInfo->setFollowing(m_followAircraft);
  m_aircraftInfo->show();
  m_aircraftInfo->raise();
  updateAircraftInfo();
}

void MainWindow::setFollowingAircraft(bool follow)
{
  m_followAircraft = follow && m_aircraft->selectedAddress().has_value();
  if (m_aircraftInfo != nullptr) {
    m_aircraftInfo->setFollowing(m_followAircraft);
  }
  if (m_followAircraft) {
    aircraftDrawn();
  }
}

bool MainWindow::exportAircraftTrack(const QString& nodeId, uint32_t address,
                                     const QString& path, QString* errorString)
{
  const QModelIndex index = m_layers->indexOfId(nodeId);
  const std::shared_ptr<const AircraftSet> set =
      index.isValid() ? m_aircraft->set(m_layers->layerDefinition(index).source)
                      : nullptr;
  const std::shared_ptr<const AircraftTrack> track =
      set ? set->find(address) : nullptr;
  if (!track) {
    if (errorString != nullptr) {
      *errorString = tr("That aircraft isn't there any more.");
    }
    return false;
  }
  const QString callsign = track->lastCallsign();
  const QString name =
      callsign.isEmpty()
          ? formatIcaoAddress(address)
          : tr("%1 (%2)").arg(callsign, formatIcaoAddress(address));
  const std::shared_ptr<const GeoidModel> geoid =
      m_registry->geoid(QStringLiteral("egm2008"), nullptr);
  UndulationFunction undulation;
  if (geoid && geoid->isValid()) {
    undulation = [geoid](double latDeg, double lonDeg) {
      return geoid->undulationM(latDeg, lonDeg);
    };
  }
  const QByteArray exported = ts::exportAircraftTrack(
      *track, name, trackExportFormatFor(path), undulation);
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
      file.write(exported) != exported.size()) {
    if (errorString != nullptr) {
      *errorString = file.errorString();
    }
    return false;
  }
  return true;
}

std::vector<QAction*> MainWindow::aircraftNodeActions(const QString& id,
                                                      QObject* parent)
{
  std::vector<QAction*> actions;
  const QModelIndex index = m_layers->indexOfId(id);
  if (!index.isValid()) {
    return actions;
  }
  const LayerDefinition definition = m_layers->layerDefinition(index);
  const std::optional<AircraftSource> source =
      AircraftSource::fromJson(definition.source);
  if (!source) {
    return actions;
  }
  QAction* list = new QAction(tr("Aircraft…"), parent);
  list->setObjectName(QStringLiteral("aircraftListAction"));
  connect(list, &QAction::triggered, this,
          [this, id]() { showAircraftList(id); });
  QAction* properties = new QAction(tr("Aircraft Layer Properties…"), parent);
  properties->setObjectName(QStringLiteral("aircraftPropertiesAction"));
  connect(properties, &QAction::triggered, this,
          [this, id]() { showAircraftProperties(id); });
  actions = {list, properties};
  if (source->origin == AircraftSource::Origin::File) {
    QAction* start =
        new QAction(tr("Go to the Start of the Recording"), parent);
    start->setObjectName(QStringLiteral("aircraftStartAction"));
    connect(start, &QAction::triggered, this, [this, id]() {
      const QModelIndex at = m_layers->indexOfId(id);
      const std::shared_ptr<const AircraftSet> set =
          at.isValid() ? m_aircraft->set(m_layers->layerDefinition(at).source)
                       : nullptr;
      if (set && set->firstS > 0.0) {
        m_clock->setTime(QDateTime::fromMSecsSinceEpoch(
            std::llround(set->firstS * 1000.0), QTimeZone::utc()));
      }
    });
    QAction* reload = new QAction(tr("Read Again"), parent);
    reload->setObjectName(QStringLiteral("aircraftReloadAction"));
    connect(reload, &QAction::triggered, this, [this, id]() {
      const QModelIndex at = m_layers->indexOfId(id);
      if (at.isValid()) {
        m_aircraft->reload(m_layers->layerDefinition(at).source);
      }
    });
    actions.push_back(start);
    actions.push_back(reload);
  } else {
    QAction* reconnect = new QAction(tr("Connect Again"), parent);
    reconnect->setObjectName(QStringLiteral("aircraftReconnectAction"));
    connect(reconnect, &QAction::triggered, this, [this, id]() {
      const QModelIndex at = m_layers->indexOfId(id);
      if (at.isValid()) {
        m_aircraft->reload(m_layers->layerDefinition(at).source);
      }
    });
    actions.push_back(reconnect);
  }
  return actions;
}

void MainWindow::aircraftMarkerClicked(int32_t index)
{
  const std::vector<DrawnAircraft>& drawn = m_aircraft->drawn();
  if (index < 0 || static_cast<std::size_t>(index) >= drawn.size()) {
    return;
  }
  const DrawnAircraft& aircraft = drawn[static_cast<std::size_t>(index)];
  const QString nodeId = aircraft.nodeId;
  const uint32_t address = aircraft.track->address;
  selectAircraft(nodeId, address);
  if (m_aircraftList && m_aircraftList->nodeId() == nodeId) {
    m_aircraftList->selectAircraft(address);
  }
}

void MainWindow::updateAircraftInfo()
{
  if (m_aircraftInfo == nullptr || !m_aircraftInfo->isVisible()) {
    return;
  }
  const std::optional<DrawnAircraft> selected = m_aircraft->selected();
  if (!selected) {
    return;
  }
  const QModelIndex index = m_layers->indexOfId(selected->nodeId);
  m_aircraftInfo->showAircraft(*selected, m_layers->data(index).toString(),
                               m_aircraft->timeS());
}

} // namespace ts
