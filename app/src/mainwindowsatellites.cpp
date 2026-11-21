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
// The main window's satellites: satellite layers, their drawing, the
// details on click and the clock in the status bar (Phase 15). See
// docs/design/satellites.md.

#include <algorithm>
#include <memory>

#include <QAction>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>

#include "addsatellitesdialog.h"
#include "clockwidget.h"
#include "globeview.h"
#include "layertreedock.h"
#include "mainwindow.h"
#include "satelliteinfowindow.h"
#include "satellitelayers.h"
#include "satellitelistdialog.h"
#include "satellitepropertiesdialog.h"
#include "ts/layers/layertreemodel.h"
#include "ts/orbits/satellitelayer.h"
#include "ts/providers/providerregistry.h"
#include "ts/time/programclock.h"

namespace ts
{

namespace
{

/** How long the pointer must rest before passes under it are found, ms. */
constexpr int32_t pointerSettleMs = 300;

} // namespace

void MainWindow::createSatellites()
{
  m_satellites = new SatelliteLayers(
      m_clock, [this]() { return m_registry->network(); },
      QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
          .filePath(QStringLiteral("satellites")),
      this);
  connect(m_satellites, &SatelliteLayers::drawsChanged, this,
          &MainWindow::satellitesDrawn);
  connect(m_satellites, &SatelliteLayers::setLoaded, this,
          &MainWindow::satelliteSetLoaded);
  m_clockWidget = new ClockWidget(m_clock, this);
  statusBar()->addPermanentWidget(m_clockWidget);
  m_satellitePointerTimer = new QTimer(this);
  m_satellitePointerTimer->setSingleShot(true);
  m_satellitePointerTimer->setInterval(pointerSettleMs);
  connect(m_satellitePointerTimer, &QTimer::timeout, this,
          &MainWindow::satellitePointerSettled);
  if (m_globeView != nullptr) {
    connect(m_globeView, &GlobeView::markerClicked, this,
            &MainWindow::satelliteMarkerClicked);
  }
  m_addSatellitesAction = new QAction(this);
  m_addSatellitesAction->setObjectName(QStringLiteral("addSatellitesAction"));
  connect(m_addSatellitesAction, &QAction::triggered, this,
          &MainWindow::addSatellites);
}

void MainWindow::refreshSatelliteLayers(const LayerStack& stack)
{
  std::vector<SatelliteLayerEntry> layers;
  for (const VectorStackEntry& entry : stack.vectors) {
    if (SatelliteSource::isSatellites(entry.definition.source)) {
      layers.push_back(
          SatelliteLayerEntry{entry.nodeId, entry.definition, entry.alpha});
    }
  }
  m_satellites->setLayers(std::move(layers));
}

void MainWindow::satellitesDrawn()
{
  if (m_globeView != nullptr) {
    m_globeView->setMarkers(m_satellites->markers());
    m_globeView->setSpaceLines(m_satellites->lines());
    m_globeView->setSatelliteLabels(m_satellites->labels());
  }
  updateSatelliteInfo();
}

void MainWindow::satelliteSetLoaded(const QJsonObject& source)
{
  const std::shared_ptr<const SatelliteSet> set = m_satellites->set(source);
  if (!set) {
    return;
  }
  // Each layer of the source: its status in the tree.
  const QDateTime now = QDateTime::currentDateTimeUtc();
  // A copy: what is done for each may refresh the layers.
  const std::vector<SatelliteLayerEntry> layers = m_satellites->layers();
  for (const SatelliteLayerEntry& layer : layers) {
    if (layer.definition.source != source) {
      continue;
    }
    const SatelliteStyle style =
        SatelliteStyle::fromJson(layer.definition.style);
    const int32_t old = m_satellites->oldCount(source, style, now);
    ProviderStatus status;
    if (set->state == SatelliteSet::State::Failed) {
      status = ProviderStatus{ProviderStatus::State::Error, set->error};
    } else if (set->origin == ElementOrigin::StaleCache) {
      status =
          ProviderStatus{ProviderStatus::State::Offline,
                         tr("Using the last download: %1").arg(set->error)};
    } else if (old > 0) {
      status = ProviderStatus{
          ProviderStatus::State::Offline,
          tr("%n satellite(s) with elements older than %1 days: Refresh "
             "Elements in this layer's menu.",
             nullptr, old)
              .arg(style.maximumAgeDays)};
    }
    m_layers->setLayerStatus(layer.nodeId, status);
    // Offered once a source, when it loads with old elements.
    const QString key = SatelliteLayers::keyOf(source);
    if (old > 0 && m_oldSatelliteNotes.insert(key).second) {
      statusBar()->showMessage(
          tr("%1: %n satellite(s) with elements older than %2 days; "
             "right-click the layer and choose Refresh Elements.",
             nullptr, old)
              .arg(layer.definition.name)
              .arg(style.maximumAgeDays),
          15000);
    } else if (set->state == SatelliteSet::State::Failed) {
      statusBar()->showMessage(
          tr("%1: %2").arg(layer.definition.name, set->error), 10000);
    }
  }
}

void MainWindow::addSatellites()
{
  AddSatellitesDialog dialog(this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  const std::optional<LayerDefinition>& chosen = dialog.layerDefinition();
  if (chosen) {
    addSatelliteLayer(*chosen);
  }
}

QString MainWindow::addSatelliteLayer(const LayerDefinition& definition)
{
  const QModelIndex index = m_layers->addLayer(definition);
  return m_layers->data(index, LayerTreeModel::NodeIdRole).toString();
}

bool MainWindow::addSatelliteFile(const QString& path, QString* errorString)
{
  SatelliteSource source;
  source.origin = SatelliteSource::Origin::File;
  source.path = QFileInfo(path).absoluteFilePath();
  const SatelliteSet set = SatelliteLayers::readLocal(source);
  if (set.satellites.empty()) {
    if (errorString != nullptr) {
      *errorString = set.error;
    }
    return false;
  }
  addSatelliteLayer(AddSatellitesDialog::definitionFor(
      source.toJson(), QFileInfo(path).completeBaseName()));
  return true;
}

SatelliteListDialog* MainWindow::showSatelliteList(const QString& nodeId)
{
  const QModelIndex index = m_layers->indexOfId(nodeId);
  if (!index.isValid() ||
      !SatelliteSource::isSatellites(m_layers->layerDefinition(index).source)) {
    return nullptr;
  }
  if (m_satelliteList && m_satelliteList->nodeId() != nodeId) {
    m_satelliteList->close();
    m_satelliteList->deleteLater();
    m_satelliteList = nullptr;
  }
  if (!m_satelliteList) {
    m_satelliteList =
        new SatelliteListDialog(nodeId, m_layers, m_satellites, m_clock, this);
    m_satelliteList->setAttribute(Qt::WA_DeleteOnClose);
  }
  m_satelliteList->show();
  m_satelliteList->raise();
  return m_satelliteList;
}

void MainWindow::showSatelliteProperties(const QString& nodeId)
{
  const QModelIndex index = m_layers->indexOfId(nodeId);
  if (!index.isValid()) {
    return;
  }
  LayerDefinition definition = m_layers->layerDefinition(index);
  SatellitePropertiesDialog dialog(SatelliteStyle::fromJson(definition.style),
                                   m_layers->data(index).toString(), this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  definition.style = dialog.style().toJson();
  m_layers->setLayerDefinition(index, definition);
}

void MainWindow::refreshSatellites(const QString& nodeId)
{
  const QModelIndex index = m_layers->indexOfId(nodeId);
  if (!index.isValid()) {
    return;
  }
  const QJsonObject source = m_layers->layerDefinition(index).source;
  m_oldSatelliteNotes.erase(SatelliteLayers::keyOf(source));
  m_satellites->refresh(source);
}

std::vector<QAction*> MainWindow::satelliteNodeActions(const QString& id,
                                                       QObject* parent)
{
  std::vector<QAction*> actions;
  const QModelIndex index = m_layers->indexOfId(id);
  if (!index.isValid() ||
      !SatelliteSource::isSatellites(m_layers->layerDefinition(index).source)) {
    return actions;
  }
  QAction* list = new QAction(tr("Satellites…"), parent);
  list->setObjectName(QStringLiteral("satelliteListAction"));
  connect(list, &QAction::triggered, this,
          [this, id]() { showSatelliteList(id); });
  QAction* properties = new QAction(tr("Satellite Layer Properties…"), parent);
  properties->setObjectName(QStringLiteral("satellitePropertiesAction"));
  connect(properties, &QAction::triggered, this,
          [this, id]() { showSatelliteProperties(id); });
  QAction* refresh = new QAction(tr("Refresh Elements"), parent);
  refresh->setObjectName(QStringLiteral("refreshSatellitesAction"));
  connect(refresh, &QAction::triggered, this,
          [this, id]() { refreshSatellites(id); });
  actions = {list, properties, refresh};
  return actions;
}

void MainWindow::selectSatellite(const QString& nodeId, int32_t catalogueNumber)
{
  m_satellites->select(nodeId, catalogueNumber);
  if (catalogueNumber < 0) {
    if (m_satelliteInfo != nullptr) {
      m_satelliteInfo->hide();
    }
    return;
  }
  if (m_satelliteInfo == nullptr) {
    m_satelliteInfo = new SatelliteInfoWindow(this);
    connect(m_satelliteInfo, &SatelliteInfoWindow::refreshRequested, this,
            [this]() { refreshSatellites(m_satellites->selectedLayer()); });
    connect(m_satelliteInfo, &QDialog::finished, this,
            [this]() { m_satellites->select(QString(), -1); });
  }
  m_satelliteInfo->show();
  m_satelliteInfo->raise();
  satellitePointerSettled();
  updateSatelliteInfo();
}

void MainWindow::satelliteMarkerClicked(int32_t index)
{
  const std::vector<DrawnSatellite>& drawn = m_satellites->drawn();
  if (index < 0) {
    return;
  }
  // Aircraft's markers come after the satellites'.
  if (static_cast<std::size_t>(index) >= drawn.size()) {
    aircraftMarkerClicked(index - static_cast<int32_t>(drawn.size()));
    return;
  }
  const DrawnSatellite& satellite = drawn[static_cast<std::size_t>(index)];
  selectSatellite(satellite.nodeId, satellite.sgp4->elements().catalogueNumber);
  if (m_satelliteList && m_satelliteList->nodeId() == satellite.nodeId) {
    m_satelliteList->selectSatellite(
        satellite.sgp4->elements().catalogueNumber);
  }
}

void MainWindow::updateSatelliteInfo()
{
  if (m_satelliteInfo == nullptr || !m_satelliteInfo->isVisible()) {
    return;
  }
  const std::optional<DrawnSatellite> selected = m_satellites->selected();
  if (!selected) {
    return;
  }
  const QModelIndex index = m_layers->indexOfId(selected->nodeId);
  const LayerDefinition definition = m_layers->layerDefinition(index);
  const std::optional<SatelliteSource> source =
      SatelliteSource::fromJson(definition.source);
  m_satelliteInfo->showSatellite(
      *selected, SatelliteStyle::fromJson(definition.style),
      m_layers->data(index).toString(), m_clock->time(),
      source.has_value() && (source->isDownloaded() ||
                             source->origin == SatelliteSource::Origin::File));
}

void MainWindow::satellitePointerSettled()
{
  if (m_satelliteInfo == nullptr || !m_satelliteInfo->isVisible()) {
    return;
  }
  m_satelliteInfo->setObserver(m_pointerOnGlobe
                                   ? std::optional<Geodetic>(Geodetic{
                                         m_pointerLatDeg, m_pointerLonDeg, 0.0})
                                   : std::nullopt);
}

} // namespace ts
