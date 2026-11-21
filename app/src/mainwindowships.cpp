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

// The main window's ships: ship layers from AIS recordings, receivers and
// open feeds, their drawing, details on click, following and export
// (Phase 18). See docs/design/ais.md.

#include <algorithm>
#include <cmath>
#include <memory>

#include <QAction>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QMessageBox>
#include <QStatusBar>
#include <QTimeZone>

#include "aisdialogs.h"
#include "globeview.h"
#include "layertreedock.h"
#include "mainwindow.h"
#include "timebar.h"
#include "ts/adsb/adsbreaders.h"
#include "ts/ais/aisreaders.h"
#include "ts/ais/vesselexport.h"
#include "ts/ais/vessellayer.h"
#include "ts/layers/layertreemodel.h"
#include "ts/providers/providerregistry.h"
#include "ts/time/programclock.h"
#include "vessellayers.h"
#include "vesselwindows.h"

namespace ts
{

void MainWindow::createShips()
{
  m_vessels = new VesselLayers(
      m_clock, [this]() { return m_registry->network(); },
      [this]() {
        return m_registry->geoid(QStringLiteral("egm2008"), nullptr);
      },
      this);
  connect(m_vessels, &VesselLayers::drawsChanged, this,
          &MainWindow::vesselsDrawn);
  connect(m_vessels, &VesselLayers::trailsChanged, this,
          &MainWindow::vesselTrailsChanged);
  connect(m_vessels, &VesselLayers::setChanged, this,
          &MainWindow::vesselSetChanged);
  m_importAisAction = new QAction(this);
  m_importAisAction->setObjectName(QStringLiteral("importAisAction"));
  connect(m_importAisAction, &QAction::triggered, this,
          [this]() { importAisData(); });
  m_addAisSourceAction = new QAction(this);
  m_addAisSourceAction->setObjectName(QStringLiteral("addAisSourceAction"));
  connect(m_addAisSourceAction, &QAction::triggered, this,
          &MainWindow::addAisSource);
}

void MainWindow::refreshVesselLayers(const LayerStack& stack)
{
  std::vector<VesselLayerEntry> layers;
  for (const VectorStackEntry& entry : stack.vectors) {
    if (VesselSource::isVessels(entry.definition.source)) {
      layers.push_back(
          VesselLayerEntry{entry.nodeId, entry.definition, entry.alpha});
    }
  }
  m_vessels->setLayers(std::move(layers));
}

void MainWindow::vesselsDrawn()
{
  if (m_globeView != nullptr) {
    m_globeView->setVessels(m_vessels->markers(), m_vessels->labels());
    m_globeView->setSpaceLines(m_vessels->courseVectors(), 4);
    m_globeView->setSpaceLineTime(m_vessels->timeS());
    if (m_followVessel) {
      const std::optional<DrawnVessel> selected = m_vessels->selected();
      if (selected) {
        CameraPose pose = m_globeView->controller().camera().pose();
        pose.targetLatDeg = selected->at.latDeg;
        pose.targetLonDeg = selected->at.lonDeg;
        m_globeView->setPose(pose);
      }
    }
  }
  updateVesselInfo();
}

void MainWindow::vesselTrailsChanged()
{
  if (m_globeView != nullptr) {
    m_globeView->setSpaceLines(m_vessels->trails(), 3);
  }
}

void MainWindow::vesselSetChanged(const QJsonObject& source)
{
  const std::shared_ptr<const VesselSet> set = m_vessels->set(source);
  if (!set) {
    return;
  }
  const QString key = VesselLayers::keyOf(source);
  // A copy: moving the clock below refreshes the layers, which replaces
  // the list being walked.
  const std::vector<VesselLayerEntry> layers = m_vessels->layers();
  for (const VesselLayerEntry& layer : layers) {
    if (layer.definition.source != source) {
      continue;
    }
    ProviderStatus status;
    if (set->state == VesselSet::State::Failed) {
      status = ProviderStatus{ProviderStatus::State::Error, set->error};
    } else if (set->live && set->feed == AisFeed::State::Reconnecting) {
      status = ProviderStatus{
          ProviderStatus::State::Offline,
          tr("Can't reach the source (%1); trying again.").arg(set->error)};
    } else if (set->read.problemCount > 0) {
      const AisReadProblem& first = set->read.problems.front();
      status = ProviderStatus{
          ProviderStatus::State::Offline,
          tr("%n line(s) couldn't be read; the first, line %1: %2", nullptr,
             static_cast<int32_t>(set->read.problemCount))
              .arg(first.line)
              .arg(first.message)};
    } else if (set->read.untimed) {
      status = ProviderStatus{
          ProviderStatus::State::Offline,
          tr("The sentences have no times: they are a second apart from the "
             "start.")};
    }
    m_layers->setLayerStatus(layer.nodeId, status);
    if (!set->live && set->state == VesselSet::State::Ready &&
        set->firstS > 0.0 && m_vesselsStarted.insert(key).second) {
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
    } else if (set->state == VesselSet::State::Failed) {
      statusBar()->showMessage(
          tr("%1: %2").arg(layer.definition.name, set->error), 10000);
    }
  }
  updateTimeBar(timedStack());
}

void MainWindow::importAisData(const QString& path)
{
  ImportAisDialog dialog(path, this);
  if (dialog.exec() == QDialog::Accepted) {
    addVesselLayer(dialog.layerDefinition());
  }
}

void MainWindow::addAisSource()
{
  AisSourceDialog dialog(this);
  if (dialog.exec() == QDialog::Accepted) {
    addVesselLayer(dialog.layerDefinition());
  }
}

QString MainWindow::addVesselLayer(const LayerDefinition& definition)
{
  const QModelIndex index = m_layers->addLayer(definition);
  return m_layers->data(index, LayerTreeModel::NodeIdRole).toString();
}

bool MainWindow::openAisFile(const QString& path, QString* errorString)
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
  const AisFormat format = detectAisFormat(head);
  bool timed = false;
  if (format == AisFormat::Nmea) {
    NmeaAisParser parser;
    for (const QByteArray& line : head.split('\n')) {
      timed = timed || parser.parse(line, 0.0).timeS.has_value();
    }
  }
  const bool noaa =
      format == AisFormat::Csv && guessAisCsvColumns(head).isComplete();
  if (format == AisFormat::Unknown) {
    if (errorString != nullptr) {
      *errorString = tr("%1 isn't a file Terra Soar reads.")
                         .arg(QFileInfo(path).fileName());
    }
    return false;
  }
  if (!timed && !noaa) {
    importAisData(path);
    return true;
  }
  VesselSource source;
  source.path = QFileInfo(path).absoluteFilePath();
  source.format = format;
  if (noaa) {
    source.csv = guessAisCsvColumns(head);
  }
  addVesselLayer(vesselLayerDefinition(source.toJson(),
                                       QFileInfo(path).completeBaseName()));
  return true;
}

VesselListDialog* MainWindow::showVesselList(const QString& nodeId)
{
  const QModelIndex index = m_layers->indexOfId(nodeId);
  if (!index.isValid() ||
      !VesselSource::isVessels(m_layers->layerDefinition(index).source)) {
    return nullptr;
  }
  if (m_vesselList && m_vesselList->nodeId() != nodeId) {
    m_vesselList->close();
    m_vesselList->deleteLater();
    m_vesselList = nullptr;
  }
  if (!m_vesselList) {
    m_vesselList = new VesselListDialog(nodeId, m_layers, m_vessels, this);
    m_vesselList->setAttribute(Qt::WA_DeleteOnClose);
    connect(m_vesselList, &VesselListDialog::vesselChosen, this,
            [this](const QString& layer, uint32_t mmsi) {
              selectVessel(layer, mmsi);
            });
  }
  m_vesselList->show();
  m_vesselList->raise();
  return m_vesselList;
}

void MainWindow::showVesselProperties(const QString& nodeId)
{
  const QModelIndex index = m_layers->indexOfId(nodeId);
  if (!index.isValid()) {
    return;
  }
  LayerDefinition definition = m_layers->layerDefinition(index);
  VesselPropertiesDialog dialog(
      VesselStyle::fromJson(definition.style), m_layers->data(index).toString(),
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

void MainWindow::selectVessel(const QString& nodeId,
                              std::optional<uint32_t> mmsi)
{
  m_vessels->select(nodeId, mmsi);
  if (!mmsi) {
    setFollowingVessel(false);
    if (m_vesselInfo != nullptr) {
      m_vesselInfo->hide();
    }
    return;
  }
  if (m_vesselInfo == nullptr) {
    m_vesselInfo = new VesselInfoWindow(this);
    connect(m_vesselInfo, &VesselInfoWindow::followToggled, this,
            &MainWindow::setFollowingVessel);
    connect(m_vesselInfo, &VesselInfoWindow::exportRequested, this, [this]() {
      const std::optional<uint32_t> selected = m_vessels->selectedMmsi();
      if (!selected) {
        return;
      }
      const QString suggested =
          QString::number(*selected) + QStringLiteral(".kml");
      const QString path = QFileDialog::getSaveFileName(
          this, tr("Export Track"), suggested,
          tr("KML (*.kml);;GPX (*.gpx);;CSV (*.csv)"));
      QString error;
      if (!path.isEmpty() && !exportVesselTrack(m_vessels->selectedLayer(),
                                                *selected, path, &error)) {
        QMessageBox::warning(this, tr("Export Track"), error);
      }
    });
    connect(m_vesselInfo, &QDialog::finished, this,
            [this]() { selectVessel(QString(), std::nullopt); });
  }
  m_vesselInfo->setFollowing(m_followVessel);
  m_vesselInfo->show();
  m_vesselInfo->raise();
  updateVesselInfo();
}

void MainWindow::setFollowingVessel(bool follow)
{
  m_followVessel = follow && m_vessels->selectedMmsi().has_value();
  if (m_vesselInfo != nullptr) {
    m_vesselInfo->setFollowing(m_followVessel);
  }
  if (m_followVessel) {
    vesselsDrawn();
  }
}

bool MainWindow::exportVesselTrack(const QString& nodeId, uint32_t mmsi,
                                   const QString& path, QString* errorString)
{
  const QModelIndex index = m_layers->indexOfId(nodeId);
  const std::shared_ptr<const VesselSet> set =
      index.isValid() ? m_vessels->set(m_layers->layerDefinition(index).source)
                      : nullptr;
  const std::shared_ptr<const VesselTrack> track =
      set ? set->find(mmsi) : nullptr;
  if (!track) {
    if (errorString != nullptr) {
      *errorString = tr("That station isn't there any more.");
    }
    return false;
  }
  const QString name = track->lastName().isEmpty()
                           ? QString::number(mmsi)
                           : tr("%1 (%2)").arg(track->lastName()).arg(mmsi);
  const QByteArray exported =
      ts::exportVesselTrack(*track, name, trackExportFormatFor(path));
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

std::vector<QAction*> MainWindow::vesselNodeActions(const QString& id,
                                                    QObject* parent)
{
  std::vector<QAction*> actions;
  const QModelIndex index = m_layers->indexOfId(id);
  const std::optional<VesselSource> source =
      index.isValid()
          ? VesselSource::fromJson(m_layers->layerDefinition(index).source)
          : std::nullopt;
  if (!source) {
    return actions;
  }
  QAction* list = new QAction(tr("Ships…"), parent);
  list->setObjectName(QStringLiteral("vesselListAction"));
  connect(list, &QAction::triggered, this,
          [this, id]() { showVesselList(id); });
  QAction* properties = new QAction(tr("Ship Layer Properties…"), parent);
  properties->setObjectName(QStringLiteral("vesselPropertiesAction"));
  connect(properties, &QAction::triggered, this,
          [this, id]() { showVesselProperties(id); });
  actions = {list, properties};
  const bool file = source->origin == VesselSource::Origin::File;
  if (file) {
    QAction* start =
        new QAction(tr("Go to the Start of the Recording"), parent);
    start->setObjectName(QStringLiteral("vesselStartAction"));
    connect(start, &QAction::triggered, this, [this, id]() {
      const QModelIndex at = m_layers->indexOfId(id);
      const std::shared_ptr<const VesselSet> set =
          at.isValid() ? m_vessels->set(m_layers->layerDefinition(at).source)
                       : nullptr;
      if (set && set->firstS > 0.0) {
        m_clock->setTime(QDateTime::fromMSecsSinceEpoch(
            std::llround(set->firstS * 1000.0), QTimeZone::utc()));
      }
    });
    actions.push_back(start);
  }
  QAction* reload =
      new QAction(file ? tr("Read Again") : tr("Connect Again"), parent);
  reload->setObjectName(file ? QStringLiteral("vesselReloadAction")
                             : QStringLiteral("vesselReconnectAction"));
  connect(reload, &QAction::triggered, this, [this, id]() {
    const QModelIndex at = m_layers->indexOfId(id);
    if (at.isValid()) {
      m_vessels->reload(m_layers->layerDefinition(at).source);
    }
  });
  actions.push_back(reload);
  return actions;
}

void MainWindow::vesselMarkerClicked(int32_t index)
{
  const std::vector<DrawnVessel>& drawn = m_vessels->drawn();
  if (index < 0 || static_cast<std::size_t>(index) >= drawn.size()) {
    return;
  }
  const DrawnVessel& vessel = drawn[static_cast<std::size_t>(index)];
  const QString nodeId = vessel.nodeId;
  const uint32_t mmsi = vessel.track->mmsi;
  selectVessel(nodeId, mmsi);
  if (m_vesselList && m_vesselList->nodeId() == nodeId) {
    m_vesselList->selectVessel(mmsi);
  }
}

void MainWindow::updateVesselInfo()
{
  if (m_vesselInfo == nullptr || !m_vesselInfo->isVisible()) {
    return;
  }
  const std::optional<DrawnVessel> selected = m_vessels->selected();
  if (!selected) {
    return;
  }
  const QModelIndex index = m_layers->indexOfId(selected->nodeId);
  m_vesselInfo->showVessel(*selected, m_layers->data(index).toString(),
                           m_vessels->timeS());
}

} // namespace ts
