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

#include "mainwindow.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeySequence>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressDialog>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QWidget>

#include "aboutdialog.h"
#include "addwebterraindialog.h"
#include "addwfsdialog.h"
#include "addwmsdialog.h"
#include "addwmtsdialog.h"
#include "addxyzdialog.h"
#include "aircraftlayers.h"
#include "appsettings.h"
#include "balloonwindow.h"
#include "clockwidget.h"
#include "elevationprofiledialog.h"
#include "featureinfowindow.h"
#include "globeview.h"
#include "helpwindow.h"
#include "iconcache.h"
#include "kmlexport.h"
#include "kmllinks.h"
#include "layertreedock.h"
#include "newregiondialog.h"
#include "offlineregionmanager.h"
#include "offlineregionsdialog.h"
#include "packmanager.h"
#include "packmanagerdialog.h"
#include "projectionsettingsdialog.h"
#include "satellitelayers.h"
#include "timebar.h"
#include "ts/adsb/adsbreaders.h"
#include "ts/ais/aisreaders.h"
#include "ts/base/buildinfo.h"
#include "ts/deps/dependencies.h"
#include "ts/elevation/elevationprofile.h"
#include "ts/elevation/gdalelevationprovider.h"
#include "ts/geo/geodetic.h"
#include "ts/geo/geoformat.h"
#include "ts/globe/layerstackdraws.h"
#include "ts/kml/kmlactivity.h"
#include "ts/kml/kmldocument.h"
#include "ts/kml/kmlfeatureprovider.h"
#include "ts/kml/kmloverlays.h"
#include "ts/kml/kmlpresentation.h"
#include "ts/kml/kmlreader.h"
#include "ts/kml/kmlwriter.h"
#include "ts/layers/layertreemodel.h"
#include "ts/layers/projectfile.h"
#include "ts/packs/basepack.h"
#include "ts/packs/packmanifest.h"
#include "ts/projection/projection.h"
#include "ts/providers/providerregistry.h"
#include "ts/regions/countryareas.h"
#include "ts/tiling/tilekey.h"
#include "ts/time/layertime.h"
#include "ts/time/programclock.h"
#include "ts/time/timeextent.h"
#include "ts/vector/vectorstyle.h"
#include "ts/wms/wmsimageryprovider.h"
#include "ts/wms/wmssource.h"
#include "vessellayers.h"

namespace ts
{

namespace
{

/** How long the view must stay still before KML Regions are checked. */
constexpr int32_t kmlViewSettleMs = 300;

/** How many NetworkLinks may sit inside each other. */
constexpr int32_t kmlMostLinkDepth = 8;

/**
 * Returns true if @p path is named like an ADS-B recording: .sbs, .avr,
 * .beast, .jsonl, a tar1090 trace, or the same compressed.
 */
bool isAdsbFileName(const QString& path)
{
  QString name = QFileInfo(path).fileName().toLower();
  if (name.endsWith(QLatin1String(".gz"))) {
    name.chop(3);
  }
  for (const char* suffix :
       {".sbs", ".basestation", ".avr", ".beast", ".jsonl"}) {
    if (name.endsWith(QLatin1String(suffix))) {
      return true;
    }
  }
  return name.startsWith(QLatin1String("trace_full_")) ||
         name.startsWith(QLatin1String("trace_recent_"));
}

/** Returns true if a layer of the tree @p nodes has satellites from CelesTrak.
 */
bool usesCelestrak(const QJsonArray& nodes)
{
  // NOLINTNEXTLINE(modernize-loop-convert): the same on Qt 5 and Qt 6
  for (int32_t i = 0; i < static_cast<int32_t>(nodes.size()); ++i) {
    const QJsonObject node = nodes.at(i).toObject();
    const QJsonObject source = node.value(QStringLiteral("definition"))
                                   .toObject()
                                   .value(QStringLiteral("source"))
                                   .toObject();
    if ((source.value(QStringLiteral("kind")).toString() ==
             QLatin1String("satellites") &&
         source.value(QStringLiteral("origin")).toString() ==
             QLatin1String("celestrak")) ||
        usesCelestrak(node.value(QStringLiteral("children")).toArray())) {
      return true;
    }
  }
  return false;
}

} // namespace

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), m_statusLabel(new QLabel(this)),
      m_pointerLabel(new QLabel(this)), m_eyeLabel(new QLabel(this)),
      m_layers(new LayerTreeModel(this)),
      m_registry(std::make_unique<ProviderRegistry>())
{
  setObjectName(QStringLiteral("mainWindow"));
  // WMS tiles are kept on disk between runs; the network client starts
  // with the first WMS layer.
  WmsProviderSettings wms;
  wms.cacheDirectory =
      QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
          .filePath(QStringLiteral("wms"));
  m_registry->setWmsNetwork(nullptr, wms);
  m_offlineRegions = std::make_unique<OfflineRegionManager>(
      *m_registry,
      QDir(wms.cacheDirectory).filePath(QStringLiteral("regions.json")));
  // Extra packs are data, not cache: they live with the application's
  // data, or in TS_PACKS_DIR (tests, portable installs).
  QString packsDirectory = qEnvironmentVariable("TS_PACKS_DIR");
  if (packsDirectory.isEmpty()) {
    packsDirectory = QDir(QStandardPaths::writableLocation(
                              QStandardPaths::AppLocalDataLocation))
                         .filePath(QStringLiteral("packs"));
  }
  m_packs = std::make_unique<PackManager>(*m_registry, packsDirectory);
  // Directly: an update closes the old version's files before replacing it.
  connect(m_packs.get(), &PackManager::layersChanged, this,
          &MainWindow::refreshLayers, Qt::DirectConnection);
  m_layers->setStatusIcons(
      style()->standardIcon(QStyle::SP_MessageBoxWarning),
      style()->standardIcon(QStyle::SP_MessageBoxCritical));
  // Status changes arrive on loader threads; a single-shot timer on this
  // thread hands them over (and several at once become one update). Loader
  // threads hold it only through a weak pointer. When the last owner lets go
  // on this thread (normally the window, often after the event loop has
  // ended) it is deleted at once, which also drops queued calls to it; on a
  // loader thread, it is deleted later on this one.
  QTimer* relay = new QTimer;
  relay->setSingleShot(true);
  relay->setInterval(0);
  connect(relay, &QTimer::timeout, this, &MainWindow::updateLayerStatus);
  m_statusRelay = std::shared_ptr<QTimer>(relay, [](QTimer* timer) {
    if (QThread::currentThread() == timer->thread()) {
      delete timer;
    } else {
      timer->deleteLater();
    }
  });
  if (GlobeView::isOpenGlAvailable()) {
    m_globeView = new GlobeView(this);
    setCentralWidget(m_globeView);
    m_eyeHeightM = m_globeView->controller().camera().eyeGeodetic().heightM;
    connect(m_globeView, &GlobeView::lineDrawn, this, &MainWindow::profileLine);
    connect(m_globeView, &GlobeView::pointerPositionChanged, this,
            &MainWindow::showPointerPosition);
    connect(m_globeView, &GlobeView::cameraChanged, this,
            &MainWindow::showEyeHeight);
    connect(m_globeView, &GlobeView::cameraChanged, this,
            &MainWindow::kmlCameraMoved);
    connect(m_globeView, &GlobeView::pointClicked, this,
            [this](LonLat point, double degreesPerPixel) {
              identifyAt(point, degreesPerPixel);
            });
  } else {
    // Headless runs (the offscreen platform) and broken drivers: the rest
    // of the window still works.
    m_noGlobeLabel = new QLabel(this);
    m_noGlobeLabel->setObjectName(QStringLiteral("noGlobeLabel"));
    m_noGlobeLabel->setAlignment(Qt::AlignCenter);
    m_noGlobeLabel->setWordWrap(true);
    setCentralWidget(m_noGlobeLabel);
    m_eyeHeightM = GlobeView::homePose().rangeM;
  }

  // A new project starts with Blue Marble from the base pack, or the test
  // imagery when the pack is not installed (a development build before
  // `make basepack`).
  findBasePack();
  // Added bottom first; each new layer goes on top.
  const QStringList defaultLayers =
      m_basePackState == BasePackState::Missing
          ? QStringList{QStringLiteral("builtin:checkerboard"),
                        QStringLiteral("builtin:graticule")}
          : QStringList{QStringLiteral("builtin:bluemarble"),
                        QStringLiteral("builtin:shoreline"),
                        QStringLiteral("builtin:graticule")};
  const std::vector<LayerDefinition> builtins = builtinLayerDefinitions();
  for (const QString& id : defaultLayers) {
    for (const LayerDefinition& definition : builtins) {
      if (definition.id == id) {
        m_layers->addLayer(definition);
      }
    }
  }
  // The terrain, off: its heights load meanwhile, so ticking it shows the
  // terrain at once.
  if (m_basePackState != BasePackState::Missing) {
    for (const LayerDefinition& definition : builtins) {
      if (definition.id == QLatin1String("builtin:terrain")) {
        // At the bottom: it is drawn as the surface, under everything.
        const QModelIndex terrain =
            m_layers->addLayer(definition, QModelIndex(), m_layers->rowCount());
        m_layers->setData(terrain, Qt::Unchecked, Qt::CheckStateRole);
      }
    }
  }
  // The clock layers with times follow, and its slider under the view
  // (shown when a layer that follows it is added).
  m_clock = new ProgramClock(this);
  m_timeBar = new TimeBar(m_clock, this);
  addToolBar(Qt::BottomToolBarArea, m_timeBar);
  m_timeBar->hide();
  m_timeBar->toggleViewAction()->setObjectName(
      QStringLiteral("timeSliderAction"));
  connect(m_clock, &ProgramClock::timeChanged, this, &MainWindow::clockMoved);
  m_layerDock = new LayerTreeDock(m_layers, this);
  addDockWidget(Qt::LeftDockWidgetArea, m_layerDock);
  m_layerDock->setTimeLookup([this](const QJsonObject& source) {
    return m_registry->basePackTime(source);
  });
  m_layerDock->setNodeActions([this](const QModelIndex& index, QObject* owner) {
    return nodeActions(index, owner);
  });
  // Double-clicking a KML entry flies there, as in Google Earth.
  m_layerDock->setDoubleClickAction([this](const QModelIndex& index) {
    const std::optional<CameraPose> pose = kmlNodePose(
        m_layers->data(index, LayerTreeModel::NodeIdRole).toString());
    if (!pose || m_globeView == nullptr) {
      return false;
    }
    m_globeView->flyTo(*pose, 0.0);
    return true;
  });
  setAcceptDrops(true);
  m_icons = new IconCache(m_registry->network(), this);
  m_kmlLinks = new KmlLinks(m_registry->network(), this);
  connect(m_kmlLinks, &KmlLinks::loaded, this, &MainWindow::kmlLinkLoaded);
  connect(m_kmlLinks, &KmlLinks::failed, this,
          [this](const QString&, const QString& error) {
            statusBar()->showMessage(error, 10000);
          });
  m_kmlViewTimer = new QTimer(this);
  m_kmlViewTimer->setSingleShot(true);
  m_kmlViewTimer->setInterval(kmlViewSettleMs);
  connect(m_kmlViewTimer, &QTimer::timeout, this,
          &MainWindow::updateKmlRegions);
  connect(m_layers, &QAbstractItemModel::dataChanged, this,
          [this](const QModelIndex& topLeft) { kmlCheckChanged(topLeft); });
  connect(m_icons, &IconCache::iconsChanged, this, [this]() {
    m_tintedIcons.clear();
    refreshBillboards(m_layers->layerStack());
  });
  connect(m_layerDock, &LayerTreeDock::addWmsLayerRequested, this,
          [this]() { addWmsLayer(); });
  connect(m_layerDock, &LayerTreeDock::addWmtsLayerRequested, this,
          [this]() { addWmtsLayer(); });
  connect(m_layerDock, &LayerTreeDock::addXyzLayerRequested, this,
          [this]() { addXyzLayer(); });
  connect(m_layerDock, &LayerTreeDock::addWfsLayerRequested, this,
          [this]() { addWfsLayer(); });
  connect(m_layerDock, &LayerTreeDock::addImageryFileRequested, this,
          [this]() { addImageryFile(); });
  connect(m_layerDock, &LayerTreeDock::addElevationFileRequested, this,
          [this]() { addElevationFile(); });
  connect(m_layerDock, &LayerTreeDock::addWebTerrainRequested, this,
          [this]() { addWebTerrain(); });
  connect(m_layers, &LayerTreeModel::layerStackChanged, this,
          &MainWindow::refreshLayers);
  if (m_globeView) {
    m_layerDock->setGroupCompositingAvailable(
        m_globeView->isGroupCompositingAvailable());
  }
  refreshLayers();

  m_statusLabel->setObjectName(QStringLiteral("statusLabel"));
  statusBar()->addWidget(m_statusLabel, 1);
  m_pointerLabel->setObjectName(QStringLiteral("pointerLabel"));
  statusBar()->addPermanentWidget(m_pointerLabel);
  m_eyeLabel->setObjectName(QStringLiteral("eyeLabel"));
  statusBar()->addPermanentWidget(m_eyeLabel);
  m_downloadLabel = new QLabel(this);
  m_downloadLabel->setObjectName(QStringLiteral("downloadLabel"));
  statusBar()->addPermanentWidget(m_downloadLabel);
  const std::function<void()> showDownloads = [this]() {
    QStringList parts{m_offlineRegions->statusText(), m_packs->statusText()};
    parts.removeAll(QString());
    const QString text = parts.join(QStringLiteral(" · "));
    m_downloadLabel->setText(text);
    m_downloadLabel->setVisible(!text.isEmpty());
  };
  connect(m_offlineRegions.get(), &OfflineRegionManager::progressChanged, this,
          showDownloads);
  connect(m_packs.get(), &PackManager::progressChanged, this, showDownloads);
  m_downloadLabel->hide();

  createSatellites();
  createAircraft();
  createShips();
  createActions();
  createMenus();
  createKmlEditing();
  retranslateUi();

  resize(1024, 768);
}

MainWindow::~MainWindow()
{
  if (m_profileCancel) {
    m_profileCancel->store(true);
  }
  // Dialogs first: they refer to the regions, which go before the registry.
  delete m_profileDialog;
  delete m_newRegionDialog;
  delete m_offlineRegionsDialog;
  delete m_packsDialog;
  m_offlineRegions.reset();
  m_packs.reset();
  for (const std::pair<QString, std::shared_ptr<ImageryProvider>>& layer :
       m_layerProviders) {
    layer.second->setStatusCallback({});
  }
  m_cancelBasePackCheck.store(true);
  if (m_basePackCheck.joinable()) {
    m_basePackCheck.join();
  }
}

void MainWindow::findBasePack()
{
  m_basePackDirectory =
      ts::findBasePack(QCoreApplication::applicationDirPath());
  QString error;
  const std::optional<PackManifest> manifest =
      m_basePackDirectory.isEmpty()
          ? std::nullopt
          : PackManifest::read(m_basePackDirectory, &error);
  if (!manifest) {
    m_basePackDirectory.clear();
    m_basePackState = BasePackState::Missing;
    return;
  }
  m_registry->setBasePack(m_basePackDirectory, *manifest);
  m_countriesFile = CountryAreas::fileInPack(*manifest, m_basePackDirectory);
  m_basePackState = BasePackState::Checking;
  // Reading every byte takes a moment, so the check runs on its own
  // thread; tiles load meanwhile, and a damaged file only means missing
  // or wrong tiles, never a crash.
  //
  // The result comes back through a std::mutex and a timer on the GUI
  // thread, not a queued call: Qt's event queue isn't instrumented for
  // ThreadSanitizer, which then can't see that handing data over that way
  // is safe.
  m_basePackCheck = std::thread([this, directory = m_basePackDirectory,
                                 checked = *manifest]() {
    PackVerification result = checked.verify(directory, m_cancelBasePackCheck);
    const std::lock_guard<std::mutex> lock(m_basePackMutex);
    m_basePackResult = std::move(result);
  });
  m_basePackPoll = new QTimer(this);
  m_basePackPoll->setInterval(50);
  connect(m_basePackPoll, &QTimer::timeout, this,
          &MainWindow::pollBasePackCheck);
  m_basePackPoll->start();
}

void MainWindow::pollBasePackCheck()
{
  std::optional<PackVerification> result;
  {
    const std::lock_guard<std::mutex> lock(m_basePackMutex);
    result.swap(m_basePackResult);
  }
  if (!result) {
    return;
  }
  m_basePackPoll->stop();
  m_basePackCheck.join();
  for (const QString& problem : result->problems) {
    qWarning("Base pack %s: %s", qUtf8Printable(m_basePackDirectory),
             qUtf8Printable(problem));
  }
  showBasePackResult(result->ok, result->problems.empty()
                                     ? QString()
                                     : result->problems.front());
}

QJsonObject MainWindow::stateReport() const
{
  QString basePack;
  switch (m_basePackState) {
  case BasePackState::Missing:
    basePack = QStringLiteral("missing");
    break;
  case BasePackState::Checking:
    basePack = QStringLiteral("checking");
    break;
  case BasePackState::Ok:
    basePack = QStringLiteral("ok");
    break;
  case BasePackState::Damaged:
    basePack = QStringLiteral("damaged");
    break;
  }
  QJsonObject state;
  state.insert(QStringLiteral("base_pack"), basePack);
  // PROJ's database, which GDAL needs to read coordinate systems by code
  // (the packages ship it; see useProjData()).
  state.insert(QStringLiteral("crs_database"), projDatabaseUsable()
                                                   ? QStringLiteral("ok")
                                                   : QStringLiteral("missing"));
  const LayerStack stack = m_layers->layerStack();
  QJsonArray layers;
  for (std::size_t i = stack.vectors.size(); i-- > 0;) {
    layers.append(stack.vectors[i].definition.name);
  }
  for (std::size_t i = stack.imagery.size(); i-- > 0;) {
    layers.append(stack.imagery[i].definition.name);
  }
  state.insert(QStringLiteral("layers"), layers);
  // Layers whose source isn't Ok: name → "offline" or "error".
  QJsonObject problems;
  for (const ImageryStackEntry& entry : stack.imagery) {
    const QModelIndex index = m_layers->indexOfId(entry.nodeId);
    const int32_t status =
        m_layers->data(index, LayerTreeModel::StatusRole).toInt();
    if (status == static_cast<int32_t>(ProviderStatus::State::Offline)) {
      problems.insert(entry.definition.name, QStringLiteral("offline"));
    } else if (status == static_cast<int32_t>(ProviderStatus::State::Error)) {
      problems.insert(entry.definition.name, QStringLiteral("error"));
    }
  }
  state.insert(QStringLiteral("layer_problems"), problems);
  state.insert(
      QStringLiteral("attributions"),
      QJsonArray::fromStringList(attributionsForStack(stack, *m_registry)));
  if (m_globeView) {
    state.insert(QStringLiteral("view"),
                 m_globeView->viewMode() == ViewMode::Map
                     ? QStringLiteral("map")
                     : QStringLiteral("globe"));
    state.insert(QStringLiteral("projection"),
                 m_globeView->mapProjection()->id());
    state.insert(QStringLiteral("scale_bar"), m_globeView->scaleBarText());
    state.insert(QStringLiteral("imagery_layers"),
                 m_globeView->imageryLayerCount());
    state.insert(QStringLiteral("vector_layers"),
                 m_globeView->vectorLayerCount());
    state.insert(QStringLiteral("imagery_tiles_drawn"),
                 m_globeView->tileStatistics().rendered);
    state.insert(QStringLiteral("vector_tiles_drawn"),
                 m_globeView->vectorStatistics().tilesDrawn);
  }
  return state;
}

void MainWindow::showBasePackResult(bool ok, const QString& problem)
{
  m_basePackState = ok ? BasePackState::Ok : BasePackState::Damaged;
  // The file name is the part before the colon of "file: what is wrong".
  m_basePackProblem = problem.section(QLatin1Char(':'), 0, 0);
  retranslateUi();
  emit basePackVerified();
}

void MainWindow::createActions()
{
  m_quitAction = new QAction(this);
  m_quitAction->setObjectName(QStringLiteral("quitAction"));
  m_quitAction->setShortcut(QKeySequence::Quit);
  m_quitAction->setMenuRole(QAction::QuitRole);
  connect(m_quitAction, &QAction::triggered, this, &QWidget::close);

  m_debugOverlayAction = new QAction(this);
  m_debugOverlayAction->setObjectName(QStringLiteral("debugOverlayAction"));
  m_debugOverlayAction->setCheckable(true);
  m_debugOverlayAction->setShortcut(QKeySequence(Qt::Key_F3));
  if (m_globeView) {
    connect(m_debugOverlayAction, &QAction::toggled, m_globeView,
            &GlobeView::setDebugOverlayVisible);
  } else {
    m_debugOverlayAction->setEnabled(false);
  }

  m_resetViewAction = new QAction(this);
  m_resetViewAction->setObjectName(QStringLiteral("resetViewAction"));
  m_resetViewAction->setShortcut(QKeySequence(Qt::Key_Home));
  if (m_globeView) {
    connect(m_resetViewAction, &QAction::triggered, m_globeView,
            &GlobeView::goHome);
  } else {
    m_resetViewAction->setEnabled(false);
  }

  // 3D globe or 2D map, and the map's projection.
  m_mapViewAction = new QAction(this);
  m_mapViewAction->setObjectName(QStringLiteral("mapViewAction"));
  m_mapViewAction->setCheckable(true);
  // As integers: Qt 5 has no operator for a modifier and a key, and
  // Clang rejects the plain enums' bitwise or.
  m_mapViewAction->setShortcut(QKeySequence(static_cast<int32_t>(Qt::CTRL) |
                                            static_cast<int32_t>(Qt::Key_M)));
  m_scaleBarAction = new QAction(this);
  m_scaleBarAction->setObjectName(QStringLiteral("scaleBarAction"));
  m_scaleBarAction->setCheckable(true);
  m_scaleBarAction->setChecked(true);
  m_attributionsAction = new QAction(this);
  m_attributionsAction->setObjectName(QStringLiteral("attributionsAction"));
  m_attributionsAction->setCheckable(true);
  m_attributionsAction->setChecked(true);
  // The navigation controls: shown unless the user hid them, in the
  // corner they chose (both kept in the settings).
  const std::unique_ptr<QSettings> settings = openSettings();
  m_navigationControlsAction = new QAction(this);
  m_navigationControlsAction->setObjectName(
      QStringLiteral("navigationControlsAction"));
  m_navigationControlsAction->setCheckable(true);
  m_navigationControlsAction->setChecked(
      settings->value(QStringLiteral("view/navigationControls"), true)
          .toBool());
  const OverlayCorner savedCorner = overlayCornerFromName(
      settings->value(QStringLiteral("view/navigationControlsCorner"))
          .toString());
  m_navigationCornerGroup = new QActionGroup(this);
  for (const OverlayCorner corner :
       {OverlayCorner::TopRight, OverlayCorner::TopLeft,
        OverlayCorner::BottomLeft, OverlayCorner::BottomRight}) {
    QAction* action = new QAction(this);
    action->setObjectName(QStringLiteral("navigationCorner_") +
                          overlayCornerName(corner));
    action->setCheckable(true);
    action->setChecked(corner == savedCorner);
    action->setData(overlayCornerName(corner));
    m_navigationCornerGroup->addAction(action);
  }
  m_projectionGroup = new QActionGroup(this);
  for (const Projection* projection : projections()) {
    QAction* action = new QAction(this);
    action->setObjectName(QStringLiteral("projectionAction_") +
                          projection->id());
    action->setCheckable(true);
    action->setData(projection->id());
    m_projectionGroup->addAction(action);
  }
  m_projectionSettingsAction = new QAction(this);
  m_projectionSettingsAction->setObjectName(
      QStringLiteral("projectionSettingsAction"));
  connect(m_projectionSettingsAction, &QAction::triggered, this,
          &MainWindow::showProjectionSettings);
  if (m_globeView) {
    connect(m_mapViewAction, &QAction::toggled, m_globeView,
            [this](bool checked) {
              m_globeView->setViewMode(
                  checked ? ViewMode::Map : ViewMode::Globe, true);
            });
    connect(m_scaleBarAction, &QAction::toggled, m_globeView,
            &GlobeView::setScaleBarVisible);
    connect(m_attributionsAction, &QAction::toggled, m_globeView,
            &GlobeView::setAttributionsVisible);
    m_globeView->setNavigationControlsVisible(
        m_navigationControlsAction->isChecked());
    m_globeView->setNavigationControlsCorner(savedCorner);
    connect(m_navigationControlsAction, &QAction::toggled, m_globeView,
            [this](bool checked) {
              m_globeView->setNavigationControlsVisible(checked);
              openSettings()->setValue(
                  QStringLiteral("view/navigationControls"), checked);
            });
    connect(m_navigationCornerGroup, &QActionGroup::triggered, m_globeView,
            [this](const QAction* action) {
              const QString name = action->data().toString();
              m_globeView->setNavigationControlsCorner(
                  overlayCornerFromName(name));
              openSettings()->setValue(
                  QStringLiteral("view/navigationControlsCorner"), name);
            });
    connect(m_projectionGroup, &QActionGroup::triggered, m_globeView,
            [this](const QAction* action) {
              // Choosing the projection already shown keeps its settings.
              // projectionWith(): with an installed pack's datum grid.
              const QString id = action->data().toString();
              if (m_globeView->mapProjection()->id() != id) {
                m_globeView->setMapProjection(projectionWith(id, {}));
              }
            });
    connect(m_globeView, &GlobeView::viewModeChanged, this,
            &MainWindow::syncViewActions);
    connect(m_globeView, &GlobeView::mapProjectionChanged, this,
            &MainWindow::syncViewActions);
  } else {
    m_mapViewAction->setEnabled(false);
    m_scaleBarAction->setEnabled(false);
    m_attributionsAction->setEnabled(false);
    m_navigationControlsAction->setEnabled(false);
    m_navigationCornerGroup->setEnabled(false);
    m_projectionGroup->setEnabled(false);
  }
  syncViewActions();

  // Terrain exaggeration: the style of the topmost elevation layer.
  m_exaggerationGroup = new QActionGroup(this);
  for (const double factor : {1.0, 1.5, 2.0, 3.0, 5.0}) {
    QAction* action = new QAction(this);
    action->setCheckable(true);
    action->setData(factor);
    action->setObjectName(QStringLiteral("exaggeration%1Action")
                              .arg(QString::number(factor * 10.0)));
    m_exaggerationGroup->addAction(action);
  }
  connect(m_exaggerationGroup, &QActionGroup::triggered, this,
          [this](const QAction* action) {
            const QModelIndex index = topmostElevationLayer();
            LayerDefinition definition = m_layers->layerDefinition(index);
            definition.style.insert(QStringLiteral("exaggeration"),
                                    action->data().toDouble());
            m_layers->setLayerDefinition(index, definition);
          });
  m_profileAction = new QAction(this);
  m_profileAction->setObjectName(QStringLiteral("profileAction"));
  connect(m_profileAction, &QAction::triggered, this,
          [this]() { showElevationProfile(); });

  m_hillshadeAction = new QAction(this);
  m_hillshadeAction->setObjectName(QStringLiteral("hillshadeAction"));
  m_hillshadeAction->setCheckable(true);
  connect(m_hillshadeAction, &QAction::triggered, this, [this](bool checked) {
    const QModelIndex index = topmostElevationLayer();
    LayerDefinition definition = m_layers->layerDefinition(index);
    definition.style.insert(QStringLiteral("hillshade"), checked);
    m_layers->setLayerDefinition(index, definition);
  });
  connect(m_layers, &LayerTreeModel::layerStackChanged, this,
          &MainWindow::syncTerrainActions);

  m_addWmsAction = new QAction(this);
  m_addWmsAction->setObjectName(QStringLiteral("addWmsAction"));
  connect(m_addWmsAction, &QAction::triggered, this,
          [this]() { addWmsLayer(); });

  m_addWmtsAction = new QAction(this);
  m_addWmtsAction->setObjectName(QStringLiteral("addWmtsAction"));
  connect(m_addWmtsAction, &QAction::triggered, this,
          [this]() { addWmtsLayer(); });
  m_featureInfoAction = new QAction(this);
  m_featureInfoAction->setObjectName(QStringLiteral("featureInfoAction"));
  m_featureInfoAction->setCheckable(true);
  m_featureInfoAction->setChecked(true);
  m_openKmlAction = new QAction(this);
  m_openKmlAction->setObjectName(QStringLiteral("openKmlAction"));
  connect(m_openKmlAction, &QAction::triggered, this,
          &MainWindow::openKmlDialog);
  m_addWfsAction = new QAction(this);
  m_addWfsAction->setObjectName(QStringLiteral("addWfsAction"));
  connect(m_addWfsAction, &QAction::triggered, this,
          [this]() { addWfsLayer(); });
  m_addXyzAction = new QAction(this);
  m_addXyzAction->setObjectName(QStringLiteral("addXyzAction"));
  connect(m_addXyzAction, &QAction::triggered, this,
          [this]() { addXyzLayer(); });

  m_addWebTerrainAction = new QAction(this);
  m_addWebTerrainAction->setObjectName(QStringLiteral("addWebTerrainAction"));
  connect(m_addWebTerrainAction, &QAction::triggered, this,
          [this]() { addWebTerrain(); });

  m_addImageryAction = new QAction(this);
  m_addImageryAction->setObjectName(QStringLiteral("addImageryAction"));
  connect(m_addImageryAction, &QAction::triggered, this,
          [this]() { addImageryFile(); });

  m_addElevationAction = new QAction(this);
  m_addElevationAction->setObjectName(QStringLiteral("addElevationAction"));
  connect(m_addElevationAction, &QAction::triggered, this,
          [this]() { addElevationFile(); });

  m_ellipsoidalHeightsAction = new QAction(this);
  m_ellipsoidalHeightsAction->setObjectName(
      QStringLiteral("ellipsoidalHeightsAction"));
  m_ellipsoidalHeightsAction->setCheckable(true);
  connect(m_ellipsoidalHeightsAction, &QAction::toggled, this, [this]() {
    showPointerPosition(m_pointerOnGlobe, m_pointerLatDeg, m_pointerLonDeg);
  });

  m_offlineRegionsAction = new QAction(this);
  m_offlineRegionsAction->setObjectName(QStringLiteral("offlineRegionsAction"));
  connect(m_offlineRegionsAction, &QAction::triggered, this,
          [this]() { showOfflineRegions(); });

  m_dataPacksAction = new QAction(this);
  m_dataPacksAction->setObjectName(QStringLiteral("dataPacksAction"));
  connect(m_dataPacksAction, &QAction::triggered, this,
          [this]() { showDataPacks(); });

  m_openAction = new QAction(this);
  m_openAction->setObjectName(QStringLiteral("openProjectAction"));
  m_openAction->setShortcut(QKeySequence::Open);
  connect(m_openAction, &QAction::triggered, this,
          &MainWindow::openProjectDialog);
  m_saveAction = new QAction(this);
  m_saveAction->setObjectName(QStringLiteral("saveProjectAction"));
  m_saveAction->setShortcut(QKeySequence::Save);
  connect(m_saveAction, &QAction::triggered, this,
          [this]() { saveProjectDialog(false); });
  m_saveAsAction = new QAction(this);
  m_saveAsAction->setObjectName(QStringLiteral("saveProjectAsAction"));
  m_saveAsAction->setShortcut(QKeySequence::SaveAs);
  connect(m_saveAsAction, &QAction::triggered, this,
          [this]() { saveProjectDialog(true); });

  m_userGuideAction = new QAction(this);
  m_userGuideAction->setObjectName(QStringLiteral("userGuideAction"));
  m_userGuideAction->setShortcut(QKeySequence::HelpContents);
  connect(m_userGuideAction, &QAction::triggered, this,
          [this]() { showUserGuide(this); });

  m_aboutAction = new QAction(this);
  m_aboutAction->setObjectName(QStringLiteral("aboutAction"));
  m_aboutAction->setMenuRole(QAction::AboutRole);
  connect(m_aboutAction, &QAction::triggered, this, &MainWindow::showAbout);
}

namespace
{

/**
 * A layer's name for the raster @p path: its name, but for an RPF table
 * of contents, A.TOC, the name of the disc or folder it is on.
 */
QString imageryLayerName(const QString& path)
{
  const QFileInfo file(path);
  if (file.fileName().compare(QLatin1String("A.TOC"), Qt::CaseInsensitive) !=
      0) {
    return file.completeBaseName();
  }
  // Usually <disc>/RPF/A.TOC.
  QDir folder = file.absoluteDir();
  if (folder.dirName().compare(QLatin1String("RPF"), Qt::CaseInsensitive) ==
          0 &&
      folder.cdUp() && !folder.dirName().isEmpty()) {
    return folder.dirName();
  }
  return file.absoluteDir().dirName();
}

} // namespace

void MainWindow::addImageryFile()
{
  const QString path = QFileDialog::getOpenFileName(
      this, tr("Add Imagery File"), QString(),
      tr("Images (*.tif *.tiff *.jp2 *.vrt *.img *.ntf *.nitf *.toc);;"
         "All files (*)"));
  if (path.isEmpty()) {
    return;
  }
  QString error;
  if (!addImageryLayer(path, &error)) {
    QMessageBox::warning(this, tr("Add Imagery File"), error);
  }
}

bool MainWindow::addImageryLayer(const QString& path, QString* errorString)
{
  LayerDefinition definition;
  definition.id = QStringLiteral("user:imagery-") +
                  QUuid::createUuid().toString(QUuid::WithoutBraces);
  definition.origin = LayerOrigin::User;
  definition.type = LayerType::Imagery;
  definition.name = imageryLayerName(path);
  QJsonObject source;
  source.insert(QStringLiteral("kind"), QStringLiteral("file"));
  source.insert(QStringLiteral("path"), QDir::cleanPath(path));
  definition.source = source;
  // Opened once now, so that an unreadable file is said at once.
  QString error;
  if (!m_registry->imagery(source, &error)) {
    if (errorString != nullptr) {
      *errorString = error;
    }
    return false;
  }
  m_layerDock->addLayer(definition);
  return true;
}

void MainWindow::addElevationFile()
{
  const QString path = QFileDialog::getOpenFileName(
      this, tr("Add Elevation File"), QString(),
      tr("Elevation models (*.tif *.tiff *.vrt *.hgt *.dem *.asc *.img "
         "*.bil *.nc);;All files (*)"));
  if (path.isEmpty()) {
    return;
  }
  // Opened once now, so that an unreadable file is said at once.
  const GdalElevationProvider probe(path, QStringLiteral("ellipsoid"), nullptr,
                                    nullptr);
  if (!probe.isValid()) {
    QMessageBox::warning(
        this, tr("Add Elevation File"),
        tr("Cannot use \"%1\" as an elevation model: %2")
            .arg(QDir::toNativeSeparators(path), probe.errorString()));
    return;
  }
  const DemVerticalDatum detected = detectDemVerticalDatum(path);
  // The datums there are now: the base pack's, installed geoid packs'.
  QStringList ids;
  QStringList choices;
  for (const VerticalDatum& datum : m_registry->verticalDatums()) {
    ids << datum.id;
    choices << AddWebTerrainDialog::datumLabel(datum);
  }
  int32_t current =
      static_cast<int32_t>(ids.indexOf(demVerticalDatumId(detected)));
  const QString file = QFileInfo(path).fileName();
  QString label;
  if (detected == DemVerticalDatum::Unknown) {
    label = tr("The file doesn't say what its heights are measured from. "
               "Heights of \"%1\" are above:")
                .arg(file);
  } else if (current < 0 && detected == DemVerticalDatum::Odn) {
    label = tr("Heights of \"%1\" are above Ordnance Datum Newlyn. Install "
               "the OSGM15 geoid pack (File → Data Packs) to convert them "
               "exactly; until then EGM2008 is within about a metre:")
                .arg(file);
  } else {
    label = tr("Heights of \"%1\" are above (as the file says):").arg(file);
  }
  bool ok = false;
  const QString chosen =
      QInputDialog::getItem(this, tr("Add Elevation File"), label, choices,
                            std::max(current, 0), false, &ok);
  if (!ok) {
    return;
  }
  addElevationLayer(path,
                    ids.value(static_cast<int32_t>(choices.indexOf(chosen))));
}

// The file, then its datum.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void MainWindow::addElevationLayer(const QString& path, const QString& datum)
{
  LayerDefinition definition;
  definition.id = QStringLiteral("user:elevation-") +
                  QUuid::createUuid().toString(QUuid::WithoutBraces);
  definition.origin = LayerOrigin::User;
  definition.type = LayerType::Elevation;
  definition.name = QFileInfo(path).completeBaseName();
  QJsonObject source;
  source.insert(QStringLiteral("kind"), QStringLiteral("file"));
  source.insert(QStringLiteral("path"), QDir::cleanPath(path));
  source.insert(QStringLiteral("verticalDatum"), datum);
  definition.source = source;
  addElevationDefinition(definition);
}

void MainWindow::addWebTerrain()
{
  AddWebTerrainDialog dialog(this, m_registry->verticalDatums());
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  const std::optional<LayerDefinition>& chosen = dialog.layerDefinition();
  if (chosen.has_value()) {
    addElevationDefinition(chosen.value());
  }
}

void MainWindow::addElevationDefinition(const LayerDefinition& layer)
{
  LayerDefinition definition = layer;
  // The terrain keeps its exaggeration.
  const QModelIndex topmost = topmostElevationLayer();
  const double exaggeration =
      topmost.isValid()
          ? terrainExaggeration(m_layers->layerDefinition(topmost))
          : 1.0;
  definition.style.insert(QStringLiteral("exaggeration"), exaggeration);
  // Above the other elevation layers, so it takes priority over them.
  const QModelIndex added = m_layers->addLayer(
      definition, topmost.isValid() ? topmost.parent() : QModelIndex(),
      topmost.isValid() ? topmost.row() : 0);
  m_layers->setData(added, Qt::Checked, Qt::CheckStateRole);
}

QModelIndex MainWindow::topmostElevationLayer() const
{
  // Depth first from the top row: the first elevation layer met.
  std::vector<QModelIndex> pending;
  for (int32_t row = m_layers->rowCount() - 1; row >= 0; --row) {
    pending.push_back(m_layers->index(row, 0));
  }
  while (!pending.empty()) {
    const QModelIndex index = pending.back();
    pending.pop_back();
    if (m_layers->data(index, LayerTreeModel::IsCollectionRole).toBool()) {
      for (int32_t row = m_layers->rowCount(index) - 1; row >= 0; --row) {
        pending.push_back(m_layers->index(row, 0, index));
      }
    } else if (m_layers->layerDefinition(index).type == LayerType::Elevation) {
      return index;
    }
  }
  return QModelIndex();
}

void MainWindow::syncTerrainActions()
{
  const QModelIndex index = topmostElevationLayer();
  m_exaggerationGroup->setEnabled(index.isValid());
  const double current =
      index.isValid() ? terrainExaggeration(m_layers->layerDefinition(index))
                      : 1.0;
  for (QAction* action : m_exaggerationGroup->actions()) {
    action->setChecked(std::abs(action->data().toDouble() - current) < 1e-9);
  }
  m_hillshadeAction->setEnabled(index.isValid());
  m_profileAction->setEnabled(index.isValid() && m_globeView != nullptr);
  m_hillshadeAction->setChecked(
      !index.isValid() || terrainHillshade(m_layers->layerDefinition(index)));
}

void MainWindow::createMenus()
{
  m_fileMenu = menuBar()->addMenu(QString());
  m_fileMenu->setObjectName(QStringLiteral("fileMenu"));
  m_fileMenu->addAction(m_openAction);
  m_fileMenu->addAction(m_saveAction);
  m_fileMenu->addAction(m_saveAsAction);
  m_fileMenu->addSeparator();
  m_fileMenu->addAction(m_addWmsAction);
  m_fileMenu->addAction(m_addWmtsAction);
  m_fileMenu->addAction(m_addXyzAction);
  m_fileMenu->addAction(m_openKmlAction);
  m_fileMenu->addAction(m_addWfsAction);
  m_fileMenu->addAction(m_addSatellitesAction);
  m_fileMenu->addAction(m_importAdsbAction);
  m_fileMenu->addAction(m_connectReceiverAction);
  m_fileMenu->addAction(m_importAisAction);
  m_fileMenu->addAction(m_addAisSourceAction);
  m_fileMenu->addAction(m_addImageryAction);
  m_fileMenu->addAction(m_addElevationAction);
  m_fileMenu->addAction(m_addWebTerrainAction);
  m_fileMenu->addAction(m_offlineRegionsAction);
  m_fileMenu->addAction(m_dataPacksAction);
  m_fileMenu->addSeparator();
  m_fileMenu->addAction(m_quitAction);

  m_viewMenu = menuBar()->addMenu(QString());
  m_viewMenu->setObjectName(QStringLiteral("viewMenu"));
  m_viewMenu->addAction(m_resetViewAction);
  m_viewMenu->addSeparator();
  m_viewMenu->addAction(m_mapViewAction);
  m_projectionMenu = m_viewMenu->addMenu(QString());
  m_projectionMenu->setObjectName(QStringLiteral("projectionMenu"));
  m_projectionMenu->addActions(m_projectionGroup->actions());
  m_projectionMenu->addSeparator();
  m_projectionMenu->addAction(m_projectionSettingsAction);
  m_viewMenu->addAction(m_scaleBarAction);
  m_viewMenu->addAction(m_attributionsAction);
  m_viewMenu->addAction(m_navigationControlsAction);
  m_navigationCornerMenu = m_viewMenu->addMenu(QString());
  m_navigationCornerMenu->setObjectName(QStringLiteral("navigationCornerMenu"));
  m_navigationCornerMenu->addActions(m_navigationCornerGroup->actions());
  m_viewMenu->addAction(m_timeBar->toggleViewAction());
  m_clockMenu = m_viewMenu->addMenu(QString());
  m_clockMenu->setObjectName(QStringLiteral("clockMenu"));
  m_clockMenu->addAction(m_clockWidget->playAction());
  m_clockMenu->addAction(m_clockWidget->realTimeAction());
  m_clockMenu->addSeparator();
  m_clockMenu->addAction(m_clockWidget->fasterAction());
  m_clockMenu->addAction(m_clockWidget->slowerAction());
  m_clockMenu->addSeparator();
  m_clockMenu->addAction(m_clockWidget->stepForwardAction());
  m_clockMenu->addAction(m_clockWidget->stepBackAction());
  m_clockMenu->addAction(m_clockWidget->jumpAction());
  // The shortcuts work anywhere in the window.
  addActions(m_clockMenu->actions());
  m_viewMenu->addAction(m_featureInfoAction);
  m_exaggerationMenu = m_viewMenu->addMenu(QString());
  m_exaggerationMenu->setObjectName(QStringLiteral("exaggerationMenu"));
  m_exaggerationMenu->addActions(m_exaggerationGroup->actions());
  m_viewMenu->addAction(m_hillshadeAction);
  m_viewMenu->addAction(m_profileAction);
  syncTerrainActions();
  m_viewMenu->addAction(m_ellipsoidalHeightsAction);
  m_viewMenu->addSeparator();
  QAction* layers = m_layerDock->toggleViewAction();
  layers->setObjectName(QStringLiteral("layersDockAction"));
  m_viewMenu->addAction(layers);
  m_viewMenu->addSeparator();
  m_viewMenu->addAction(m_debugOverlayAction);

  m_helpMenu = menuBar()->addMenu(QString());
  m_helpMenu->setObjectName(QStringLiteral("helpMenu"));
  m_helpMenu->addAction(m_userGuideAction);
  m_helpMenu->addSeparator();
  m_helpMenu->addAction(m_aboutAction);
}

void MainWindow::retranslateUi()
{
  setWindowTitle(programName());
  m_fileMenu->setTitle(tr("&File"));
  if (m_kmlMenu != nullptr) {
    retranslateKmlEditing();
  }
  m_viewMenu->setTitle(tr("&View"));
  m_clockMenu->setTitle(tr("&Clock"));
  m_addSatellitesAction->setText(tr("Add &Satellites…"));
  m_addSatellitesAction->setStatusTip(
      tr("Add satellites from CelesTrak, a TLE or OMM file, or TLEs pasted "
         "in"));
  m_importAdsbAction->setText(tr("Import ADS-&B Data…"));
  m_importAdsbAction->setStatusTip(
      tr("Show aircraft from a recording of ADS-B data: SBS, raw messages, "
         "Beast, readsb JSON, traces or CSV"));
  m_connectReceiverAction->setText(tr("Connect to ADS-B &Receiver…"));
  m_connectReceiverAction->setStatusTip(
      tr("Show aircraft live from a dump1090 or readsb receiver"));
  m_importAisAction->setText(tr("Import AIS Data…"));
  m_importAisAction->setStatusTip(
      tr("Show ships from a recording of AIS data: NMEA logs or CSV (NOAA "
         "MarineCadastre's and others)"));
  m_addAisSourceAction->setText(tr("Add AIS Source…"));
  m_addAisSourceAction->setStatusTip(
      tr("Show ships live from an AIS receiver or an open online feed"));
  m_helpMenu->setTitle(tr("&Help"));
  m_quitAction->setText(tr("&Quit"));
  m_quitAction->setStatusTip(tr("Close the program"));
  m_resetViewAction->setText(tr("&Reset View"));
  m_resetViewAction->setStatusTip(tr("Fly back to the start view"));
  m_mapViewAction->setText(tr("2D &Map"));
  m_mapViewAction->setStatusTip(
      tr("Change between the 3D globe and a flat map"));
  m_projectionMenu->setTitle(tr("Map &Projection"));
  m_projectionSettingsAction->setText(tr("Projection &Settings…"));
  m_projectionSettingsAction->setStatusTip(
      tr("Change the settings of the flat map's projection, such as its "
         "standard parallel"));
  for (QAction* action : m_projectionGroup->actions()) {
    if (const Projection* projection =
            projectionById(action->data().toString())) {
      action->setText(projection->displayName());
      action->setStatusTip(tr("Draw the flat map in the %1 projection")
                               .arg(projection->displayName()));
    }
  }
  m_profileAction->setText(tr("Elevation &Profile…"));
  m_profileAction->setStatusTip(
      tr("Draw a line on the globe and see the heights along it"));
  m_hillshadeAction->setText(tr("&Hillshade on the Map"));
  m_hillshadeAction->setStatusTip(
      tr("Shade the terrain's slopes on the flat map, as if lit from the "
         "north-west"));
  m_exaggerationMenu->setTitle(tr("Terrain &Exaggeration"));
  for (QAction* action : m_exaggerationGroup->actions()) {
    const QString factor = QLocale().toString(action->data().toDouble());
    action->setText(tr("%1×").arg(factor));
    action->setStatusTip(
        tr("Draw the terrain %1 times as high as it is").arg(factor));
  }
  m_scaleBarAction->setText(tr("&Scale Bar"));
  m_scaleBarAction->setStatusTip(
      tr("Show a ground distance at the centre of the view"));
  m_featureInfoAction->setText(tr("What Is &Here on Click"));
  m_featureInfoAction->setStatusTip(
      tr("Clicking the globe or the map asks WMS layers what they show "
         "there"));
  m_timeBar->toggleViewAction()->setText(tr("&Time Slider"));
  m_timeBar->toggleViewAction()->setStatusTip(
      tr("Show the slider that sets the time shown by layers with dates"));
  m_attributionsAction->setText(tr("&Attributions"));
  m_attributionsAction->setStatusTip(
      tr("Show the sources of the layers drawn in the corner of the view"));
  m_navigationControlsAction->setText(tr("&Navigation Controls"));
  m_navigationControlsAction->setStatusTip(
      tr("Show controls on the view to turn, move, zoom and tilt it"));
  m_navigationCornerMenu->setTitle(tr("Navigation Controls &Corner"));
  for (QAction* action : m_navigationCornerGroup->actions()) {
    const OverlayCorner corner =
        overlayCornerFromName(action->data().toString());
    switch (corner) {
    case OverlayCorner::TopRight:
      action->setText(tr("Top &Right"));
      break;
    case OverlayCorner::TopLeft:
      action->setText(tr("Top &Left"));
      break;
    case OverlayCorner::BottomLeft:
      action->setText(tr("&Bottom Left"));
      break;
    case OverlayCorner::BottomRight:
      action->setText(tr("Bottom Ri&ght"));
      break;
    }
  }
  m_addWebTerrainAction->setText(tr("Add Web &Terrain…"));
  m_addWebTerrainAction->setStatusTip(
      tr("Add elevation from a web terrain tile service or a Cloud "
         "Optimized GeoTIFF on a server"));
  m_addImageryAction->setText(tr("Add &Imagery File…"));
  m_addImageryAction->setStatusTip(
      tr("Add a georeferenced image (GeoTIFF, JPEG 2000, or RPF charts and "
         "imagery through their A.TOC) as a layer"));
  m_addElevationAction->setText(tr("Add &Elevation File…"));
  m_addElevationAction->setStatusTip(
      tr("Add a digital elevation model (GeoTIFF or another raster) as "
         "terrain"));
  m_ellipsoidalHeightsAction->setText(tr("Heights Above the &Ellipsoid"));
  m_ellipsoidalHeightsAction->setStatusTip(
      tr("Show heights above the WGS 84 ellipsoid (as GNSS gives them) "
         "instead of above sea level"));
  m_addWmsAction->setText(tr("Add &WMS Layer…"));
  m_addWmsAction->setStatusTip(
      tr("Add a layer from a web map server (WMS) to the layer tree"));
  m_addWmtsAction->setText(tr("Add W&MTS Layer…"));
  m_addWmtsAction->setStatusTip(
      tr("Add a layer from a web map tile server (WMTS) to the layer tree"));
  m_openKmlAction->setText(tr("Open &KML or KMZ…"));
  m_openKmlAction->setStatusTip(
      tr("Add a KML or KMZ file (from Google Earth and other programs) to "
         "the layer tree"));
  m_addWfsAction->setText(tr("Add W&FS Layer…"));
  m_addWfsAction->setStatusTip(
      tr("Add features (points, lines and areas) from a web feature service "
         "(WFS) to the layer tree"));
  m_addXyzAction->setText(tr("Add &XYZ Tile Layer…"));
  m_addXyzAction->setStatusTip(
      tr("Add a layer from an XYZ or TMS tile service, such as "
         "OpenStreetMap, to the layer tree"));
  m_offlineRegionsAction->setText(tr("Offline &Regions…"));
  m_offlineRegionsAction->setStatusTip(
      tr("Keep areas of WMS, WMTS, XYZ and WFS layers on this computer for "
         "use without a network"));
  m_dataPacksAction->setText(tr("&Data Packs…"));
  m_dataPacksAction->setStatusTip(
      tr("Install more detailed imagery and shorelines, or remove them"));
  m_openAction->setText(tr("&Open Project…"));
  m_openAction->setStatusTip(
      tr("Replace the layers and view with a saved project"));
  m_saveAction->setText(tr("&Save Project"));
  m_saveAction->setStatusTip(tr("Save the layers and view"));
  m_saveAsAction->setText(tr("Save Project &As…"));
  m_saveAsAction->setStatusTip(tr("Save the layers and view to a new file"));
  m_debugOverlayAction->setText(tr("&Debug Overlay"));
  m_debugOverlayAction->setStatusTip(
      tr("Show frame times and the camera position over the globe"));
  m_userGuideAction->setText(tr("&User Guide"));
  m_userGuideAction->setStatusTip(
      tr("Read the user guide, built into the program"));
  m_aboutAction->setText(tr("&About %1").arg(programName()));
  m_aboutAction->setStatusTip(tr("Show the program version and licence"));
  switch (m_basePackState) {
  case BasePackState::Missing:
    m_statusLabel->setText(
        tr("The base pack is not installed; showing test imagery."));
    break;
  case BasePackState::Damaged:
    m_statusLabel->setText(
        tr("The base pack is damaged (%1). Reinstall %2 to repair it.")
            .arg(m_basePackProblem, programName()));
    break;
  case BasePackState::Checking:
  case BasePackState::Ok:
    m_statusLabel->setText(tr("Ready"));
    break;
  }
  if (m_noGlobeLabel) {
    m_noGlobeLabel->setText(
        tr("The globe needs OpenGL 3.3, which this display does not offer. "
           "Start %1 with --software-gl to use software rendering.")
            .arg(programName()));
  }
  showPointerPosition(m_pointerOnGlobe, m_pointerLatDeg, m_pointerLonDeg);
  showEyeHeight(m_eyeHeightM);
}

void MainWindow::showProjectionSettings()
{
  if (m_globeView == nullptr ||
      m_globeView->mapProjection()->parameters().empty()) {
    return;
  }
  ProjectionSettingsDialog dialog(this, m_globeView->mapProjection());
  if (dialog.exec() == QDialog::Accepted) {
    m_globeView->setMapProjection(dialog.projection());
  }
}

void MainWindow::syncViewActions()
{
  // The actions follow the view (a project just opened, for instance).
  const bool onMap =
      m_globeView != nullptr && m_globeView->viewMode() == ViewMode::Map;
  const QString projection = m_globeView != nullptr
                                 ? m_globeView->mapProjection()->id()
                                 : projections().front()->id();
  if (m_mapViewAction->isChecked() != onMap) {
    m_mapViewAction->setChecked(onMap);
  }
  for (QAction* action : m_projectionGroup->actions()) {
    if (action->data().toString() == projection && !action->isChecked()) {
      action->setChecked(true);
    }
  }
  m_projectionSettingsAction->setEnabled(
      m_globeView != nullptr &&
      !m_globeView->mapProjection()->parameters().empty());
}

void MainWindow::showPointerPosition(bool onGlobe, double latDeg, double lonDeg)
{
  m_pointerOnGlobe = onGlobe;
  m_pointerLatDeg = latDeg;
  m_pointerLonDeg = lonDeg;
  if (m_satellitePointerTimer != nullptr) {
    m_satellitePointerTimer->start();
  }
  const QLocale locale;
  if (onGlobe) {
    QString text = tr("%1  %2").arg(formatLatitude(latDeg, locale),
                                    formatLongitude(lonDeg, locale));
    // On the map, the position in its own grid too (a grid reference).
    if (m_globeView != nullptr && m_globeView->viewMode() == ViewMode::Map) {
      const QString grid =
          m_globeView->mapProjection()->gridReference(latDeg, lonDeg);
      if (!grid.isEmpty()) {
        text = tr("%1  %2").arg(text, grid);
      }
    }
    const std::optional<GroundHeight> ground =
        m_globeView != nullptr ? m_globeView->pointerGround() : std::nullopt;
    if (ground) {
      const bool ellipsoidal = m_ellipsoidalHeightsAction->isChecked();
      const QString metres = locale.toString(std::lround(
          ellipsoidal ? ground->ellipsoidalM : ground->aboveSeaLevelM()));
      text = ellipsoidal ? tr("%1  %2 m ellipsoidal").arg(text, metres)
                         : tr("%1  %2 m").arg(text, metres);
    }
    m_pointerLabel->setText(text);
  } else if (m_globeView != nullptr &&
             m_globeView->viewMode() == ViewMode::Map) {
    m_pointerLabel->setText(tr("Pointer not on the map"));
  } else {
    m_pointerLabel->setText(tr("Pointer not on the globe"));
  }
}

void MainWindow::refreshLayers()
{
  const LayerStack tree = timedStack();
  updateTimeBar(tree);
  // Layers with times draw the clock's.
  const LayerStack stack = stackAtTime(tree, m_clock->time());
  if (m_satellites != nullptr) {
    refreshSatelliteLayers(stack);
  }
  if (m_aircraft != nullptr) {
    refreshAircraftLayers(stack);
  }
  if (m_vessels != nullptr) {
    refreshVesselLayers(stack);
  }
  if (!m_globeView) {
    // No OpenGL view: nothing is drawn, but layers that can't be opened are
    // still marked in the dock.
    watchLayerStatus(stack);
    return;
  }
  // A pack may have brought or taken away the projection's datum grid
  // (OSTN15 for the National Grid).
  const Projection* projection = m_globeView->mapProjection();
  m_globeView->setMapProjection(
      projectionWith(projection->id(), projection->parameterValues()));
  QStringList errors;
  applyImageryDraws(stack, &errors);
  refreshBillboards(stack);
  m_drawnVectors = QJsonArray();
  for (const VectorStackEntry& entry : stack.vectors) {
    m_drawnVectors.append(entry.definition.source);
  }
  m_globeView->setVectorDraws(vectorDrawsForStack(stack, *m_registry, &errors));
  m_globeView->setElevationDraw(
      elevationDrawForStack(stack, *m_registry, &errors));
  m_globeView->setAttributions(attributionsForStack(stack, *m_registry));
  if (!errors.isEmpty()) {
    statusBar()->showMessage(errors.join(QLatin1Char(' ')), 10000);
  }
  watchLayerStatus(stack);
}

void MainWindow::applyImageryDraws(const LayerStack& stack, QStringList* errors)
{
  QJsonArray sources;
  for (const ImageryStackEntry& entry : stack.imagery) {
    sources.append(entry.definition.source);
  }
  m_drawnImagery = sources;
  if (m_globeView) {
    m_globeView->setImageryDraws(
        imageryDrawsForStack(stack, *m_registry, errors));
  }
}

void MainWindow::clockMoved()
{
  // Only the imagery whose time changed is drawn anew: the other layers'
  // providers (and their tiles) stay.
  const LayerStack stack = stackAtTime(timedStack(), m_clock->time());
  QJsonArray sources;
  for (const ImageryStackEntry& entry : stack.imagery) {
    sources.append(entry.definition.source);
  }
  QJsonArray vectors;
  for (const VectorStackEntry& entry : stack.vectors) {
    vectors.append(entry.definition.source);
  }
  if (vectors != m_drawnVectors) {
    // KML whose times show something else now.
    refreshLayers();
    return;
  }
  // Markers moving along tracks.
  refreshBillboards(stack);
  if (sources == m_drawnImagery) {
    return;
  }
  QStringList errors;
  applyImageryDraws(stack, &errors);
  watchLayerStatus(stack);
}

const std::vector<QDateTime>&
MainWindow::kmlBoundaries(const QJsonObject& source) const
{
  const QString key =
      source.value(QStringLiteral("path")).toString() + QLatin1Char('|') +
      source.value(QStringLiteral("container")).toString() + QLatin1Char('|') +
      (source.value(QStringLiteral("deep")).toBool() ? QStringLiteral("deep")
                                                     : QString()) +
      QLatin1Char('|') +
      QString::number(source.value(QStringLiteral("rev")).toDouble());
  const std::map<QString, std::vector<QDateTime>>::const_iterator found =
      m_kmlBoundaries.find(key);
  if (found != m_kmlBoundaries.end()) {
    return found->second;
  }
  std::vector<QDateTime> boundaries;
  const std::shared_ptr<const KmlDocument> document = sharedKmlDocument(
      source.value(QStringLiteral("path")).toString(), nullptr);
  if (document) {
    boundaries = kmlTimeBoundaries(
        *document, source.value(QStringLiteral("container")).toString(),
        source.value(QStringLiteral("deep")).toBool());
  }
  return m_kmlBoundaries[key] = std::move(boundaries);
}

LayerStack MainWindow::timedStack() const
{
  // Base pack layers with month variants installed (Blue Marble's months)
  // follow the clock's month unless told otherwise.
  LayerStack stack = m_layers->layerStack();
  for (ImageryStackEntry& entry : stack.imagery) {
    const std::optional<LayerTime> time =
        m_registry->basePackTime(entry.definition.source);
    if (time) {
      entry.definition.source.insert(QStringLiteral("time"), time->toJson());
    }
  }
  // KML layers whose Regions are out of sight are left out; those with
  // times show the clock's.
  // A Region hides what is inside its feature too (a link's document).
  const std::function<bool(const QString&)> hidden =
      [this](const QString& nodeId) {
        if (m_kmlInactive.empty()) {
          return false;
        }
        for (QModelIndex up = m_layers->indexOfId(nodeId); up.isValid();
             up = up.parent()) {
          if (m_kmlInactive.count(
                  m_layers->data(up, LayerTreeModel::NodeIdRole).toString()) !=
              0) {
            return true;
          }
        }
        return false;
      };
  stack.imagery.erase(std::remove_if(stack.imagery.begin(), stack.imagery.end(),
                                     [&](const ImageryStackEntry& entry) {
                                       return hidden(entry.nodeId);
                                     }),
                      stack.imagery.end());
  stack.vectors.erase(std::remove_if(stack.vectors.begin(), stack.vectors.end(),
                                     [&](const VectorStackEntry& entry) {
                                       return hidden(entry.nodeId);
                                     }),
                      stack.vectors.end());
  for (VectorStackEntry& entry : stack.vectors) {
    const QJsonObject& source = entry.definition.source;
    if (source.value(QStringLiteral("kind")).toString() !=
        QLatin1String("kml")) {
      continue;
    }
    const std::vector<QDateTime>& boundaries = kmlBoundaries(source);
    if (!boundaries.empty()) {
      entry.definition.source.insert(
          QStringLiteral("at"),
          kmlTimeKey(boundaries, m_clock->time()).toString(Qt::ISODate));
    }
  }
  return stack;
}

bool MainWindow::openFile(const QString& path, QString* errorString)
{
  if (path.endsWith(QLatin1String(".kml"), Qt::CaseInsensitive) ||
      path.endsWith(QLatin1String(".kmz"), Qt::CaseInsensitive)) {
    return openKml(path, errorString);
  }
  const QString suffix = QFileInfo(path).suffix().toLower();
  if (isAdsbFileName(path)) {
    return openAdsbFile(path, errorString);
  }
  if (suffix == QLatin1String("nmea") || suffix == QLatin1String("ais")) {
    return openAisFile(path, errorString);
  }
  if (suffix == QLatin1String("txt") || suffix == QLatin1String("log") ||
      suffix == QLatin1String("csv")) {
    // AIS sentences or CSV, raw ADS-B messages, or element sets.
    QFile file(path);
    const QByteArray head =
        file.open(QIODevice::ReadOnly) ? file.read(4096) : QByteArray();
    if (detectAisFormat(head) != AisFormat::Unknown) {
      return openAisFile(path, errorString);
    }
    const AdsbFormat adsb = detectAdsbFormat(head);
    if (adsb == AdsbFormat::Raw || adsb == AdsbFormat::Sbs ||
        (adsb == AdsbFormat::Csv && suffix == QLatin1String("csv"))) {
      return openAdsbFile(path, errorString);
    }
  }
  if (suffix == QLatin1String("tle") || suffix == QLatin1String("3le") ||
      suffix == QLatin1String("2le") || suffix == QLatin1String("txt")) {
    return addSatelliteFile(path, errorString);
  }
  return openProject(path, errorString);
}

void MainWindow::openKmlDialog()
{
  const QString path = QFileDialog::getOpenFileName(
      this, tr("Open KML or KMZ"), QString(),
      tr("KML and KMZ files (*.kml *.kmz);;All files (*)"));
  if (path.isEmpty()) {
    return;
  }
  QString error;
  if (!openKml(path, &error)) {
    QMessageBox::warning(this, tr("Open KML or KMZ"), error);
  }
}

bool MainWindow::openKml(const QString& path, QString* errorString)
{
  const QString absolute = QFileInfo(path).absoluteFilePath();
  const std::shared_ptr<const KmlDocument> document =
      readKmlInBackground(absolute, errorString);
  if (!document) {
    return false;
  }
  addKmlFeature(*document, document->root, QString(), QModelIndex(), 0);
  m_kmlRoots[m_layers->data(m_layers->index(0, 0), LayerTreeModel::NodeIdRole)
                 .toString()] = absolute;
  updateKmlActions();
  if (!document->skipped.isEmpty()) {
    statusBar()->showMessage(
        tr("%1: %n element(s) Terra Soar doesn't show were skipped.", nullptr,
           static_cast<int32_t>(document->skipped.size()))
            .arg(QFileInfo(path).fileName()),
        10000);
  }
  if (m_globeView != nullptr && document->root.view) {
    m_globeView->flyTo(kmlViewPose(document->root.view.value_or(KmlView{})),
                       0.0);
  }
  return true;
}

std::shared_ptr<const KmlDocument>
MainWindow::readKmlInBackground(const QString& path, QString* errorString)
{
  // Large files take seconds to read: off the GUI thread, which keeps
  // drawing, with a busy dialog if it takes a while.
  constexpr int32_t pollMs = 20;
  constexpr int32_t dialogAfterMs = 500;
  std::future<std::pair<std::shared_ptr<const KmlDocument>, QString>> reading =
      std::async(std::launch::async, [path]() {
        QString error;
        std::shared_ptr<const KmlDocument> document =
            sharedKmlDocument(path, &error);
        return std::make_pair(document, error);
      });
  QElapsedTimer waited;
  waited.start();
  std::unique_ptr<QProgressDialog> busy;
  while (reading.wait_for(std::chrono::milliseconds(pollMs)) !=
         std::future_status::ready) {
    if (!busy && waited.elapsed() > dialogAfterMs) {
      busy = std::make_unique<QProgressDialog>(
          tr("Reading %1…").arg(QFileInfo(path).fileName()), QString(), 0, 0,
          this);
      busy->setWindowModality(Qt::WindowModal);
      busy->setMinimumDuration(0);
      busy->show();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, pollMs);
  }
  std::pair<std::shared_ptr<const KmlDocument>, QString> read = reading.get();
  if (!read.first && errorString != nullptr) {
    *errorString = read.second;
  }
  return read.first;
}

void MainWindow::addKmlFeature(const KmlDocument& document,
                               const KmlFeature& container,
                               const QString& featurePath,
                               const QModelIndex& parent, int32_t row)
{
  // A file whose feature is a link or an overlay: that alone.
  if (container.kind == KmlFeature::Kind::NetworkLink) {
    addKmlLink(document, container, featurePath, parent, row);
    return;
  }
  if (container.kind == KmlFeature::Kind::GroundOverlay ||
      container.kind == KmlFeature::Kind::ScreenOverlay) {
    addKmlOverlay(document, container, featurePath, parent, row);
    return;
  }
  QString name = container.name;
  if (name.isEmpty() && featurePath.isEmpty()) {
    name = QFileInfo(document.path).completeBaseName();
  }
  const QModelIndex collection = m_layers->addCollection(
      name.isEmpty() ? tr("Folder") : name, parent, row);
  const QString collectionId =
      m_layers->data(collection, LayerTreeModel::NodeIdRole).toString();
  m_kmlNodes[collectionId] = {document.path, featurePath};
  const KmlListStyle list =
      KmlStyleResolver(document).resolve(container).list.value_or(
          KmlListStyle{});
  if (container.kind == KmlFeature::Kind::Placemark) {
    // A file of one placemark.
    addKmlPlaces(document, featurePath, collection, 0, false);
  } else if (list.itemType == KmlListStyle::ItemType::CheckHideChildren) {
    // Listed as one entry: everything inside in one layer.
    addKmlPlaces(document, featurePath, collection, 0, true);
  } else {
    addKmlChildren(document, container, featurePath, collection);
  }
  if (list.itemType == KmlListStyle::ItemType::RadioFolder) {
    // At most one child on: the first the file shows.
    m_kmlRadio.insert(collectionId);
    bool on = false;
    for (int32_t i = 0; i < m_layers->rowCount(collection); ++i) {
      const QModelIndex child = m_layers->index(i, 0, collection);
      if (m_layers->data(child, Qt::CheckStateRole).toInt() == Qt::Checked) {
        if (on) {
          m_layers->setData(child, Qt::Unchecked, Qt::CheckStateRole);
        }
        on = true;
      }
    }
  }
  if (!container.visibility) {
    m_layers->setData(collection, Qt::Unchecked, Qt::CheckStateRole);
  }
}

void MainWindow::addKmlChildren(const KmlDocument& document,
                                const KmlFeature& container,
                                const QString& featurePath,
                                const QModelIndex& collection)
{
  int32_t next = 0;
  bool placemarks = false;
  for (std::size_t i = 0; i < container.children.size(); ++i) {
    const KmlFeature& child = container.children[i];
    const QString childPath =
        featurePath.isEmpty()
            ? QString::number(i)
            : featurePath + QLatin1Char('.') + QString::number(i);
    switch (child.kind) {
    case KmlFeature::Kind::Document:
    case KmlFeature::Kind::Folder:
      addKmlFeature(document, child, childPath, collection, next++);
      break;
    case KmlFeature::Kind::GroundOverlay:
    case KmlFeature::Kind::ScreenOverlay:
      addKmlOverlay(document, child, childPath, collection, next++);
      break;
    case KmlFeature::Kind::NetworkLink:
      addKmlLink(document, child, childPath, collection, next++);
      break;
    case KmlFeature::Kind::Placemark:
      // The container's placemarks, in one layer where the first is.
      if (!placemarks) {
        placemarks = true;
        addKmlPlaces(document, featurePath, collection, next++, false);
      }
      break;
    }
  }
}

void MainWindow::addKmlPlaces(const KmlDocument& document,
                              const QString& featurePath,
                              const QModelIndex& collection, int32_t row,
                              bool deep)
{
  LayerDefinition definition;
  definition.id = QStringLiteral("kml:") +
                  QUuid::createUuid().toString(QUuid::WithoutBraces);
  definition.origin = LayerOrigin::User;
  definition.type = LayerType::Vector;
  definition.name = tr("Places");
  definition.source =
      withKmlRevision(kmlLayerSource(document.path, featurePath, deep));
  VectorStyle style;
  style.fill = QColor(255, 255, 255);
  style.outline = QColor(255, 255, 255);
  definition.style = style.toJson();
  const QModelIndex layer = m_layers->addLayer(definition, collection, row);
  m_kmlNodes[m_layers->data(layer, LayerTreeModel::NodeIdRole).toString()] = {
      document.path, featurePath};
}

void MainWindow::addKmlLink(const KmlDocument& document, const KmlFeature& link,
                            const QString& featurePath,
                            const QModelIndex& parent, int32_t row)
{
  const QModelIndex collection = m_layers->addCollection(
      link.name.isEmpty() ? tr("Network link") : link.name, parent, row);
  const QString id =
      m_layers->data(collection, LayerTreeModel::NodeIdRole).toString();
  m_kmlNodes[id] = {document.path, featurePath};
  m_kmlLinks->add(id, sharedKmlDocument(document.path, nullptr), featurePath);
  // Fetched now, unless a Region waits for the view to come near.
  if (!kmlHasRegion(document, featurePath)) {
    m_kmlLinks->load(id, currentViewParameters());
  }
  if (!link.visibility) {
    m_layers->setData(collection, Qt::Unchecked, Qt::CheckStateRole);
  }
}

KmlViewParameters MainWindow::currentViewParameters() const
{
  if (m_globeView == nullptr) {
    return KmlViewParameters{};
  }
  return kmlViewParameters(m_globeView->controller().camera(),
                           m_globeView->terrainShown());
}

// The link's node, then the file it brought.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void MainWindow::kmlLinkLoaded(const QString& nodeId, const QString& path,
                               bool flyToView)
{
  const QModelIndex node = m_layers->indexOfId(nodeId);
  if (!node.isValid()) {
    m_kmlLinks->remove(nodeId);
    return;
  }
  QString error;
  const std::shared_ptr<const KmlDocument> document =
      sharedKmlDocument(path, &error);
  if (!document) {
    statusBar()->showMessage(error, 10000);
    return;
  }
  // Links inside links (a superoverlay's levels): not without end.
  int32_t depth = 0;
  for (QModelIndex up = node; up.isValid(); up = up.parent()) {
    depth += m_kmlLinks->contains(
                 m_layers->data(up, LayerTreeModel::NodeIdRole).toString())
                 ? 1
                 : 0;
  }
  if (depth > kmlMostLinkDepth) {
    statusBar()->showMessage(tr("%1: too many network links inside each other.")
                                 .arg(m_layers->data(node).toString()),
                             10000);
    return;
  }
  // What it linked to before goes; what it links to now comes in its place.
  const bool wasRefreshing = m_refreshingKml;
  m_refreshingKml = true;
  while (m_layers->rowCount(node) > 0) {
    m_layers->removeNode(
        m_layers->index(m_layers->rowCount(node) - 1, 0, node));
  }
  forgetRemovedKmlNodes();
  addKmlFeature(*document, document->root, QString(), node, 0);
  m_refreshingKml = wasRefreshing;
  if (flyToView && m_globeView != nullptr && document->root.view) {
    m_globeView->flyTo(kmlViewPose(document->root.view.value_or(KmlView{})),
                       0.0);
  }
}

void MainWindow::forgetRemovedKmlNodes()
{
  for (std::map<QString, std::pair<QString, QString>>::iterator it =
           m_kmlNodes.begin();
       it != m_kmlNodes.end();) {
    if (m_layers->indexOfId(it->first).isValid()) {
      ++it;
    } else {
      m_kmlRadio.erase(it->first);
      m_kmlLinks->remove(it->first);
      it = m_kmlNodes.erase(it);
    }
  }
}

void MainWindow::kmlCameraMoved()
{
  if (m_globeView == nullptr) {
    return;
  }
  const CameraPose& pose = m_globeView->controller().camera().pose();
  const bool moved = !m_lastKmlPose ||
                     m_lastKmlPose->targetLatDeg != pose.targetLatDeg ||
                     m_lastKmlPose->targetLonDeg != pose.targetLonDeg ||
                     m_lastKmlPose->rangeM != pose.rangeM ||
                     m_lastKmlPose->headingDeg != pose.headingDeg ||
                     m_lastKmlPose->tiltDeg != pose.tiltDeg;
  if (moved && !m_kmlNodes.empty()) {
    m_lastKmlPose = pose;
    m_kmlViewTimer->start();
  }
}

void MainWindow::updateKmlRegions()
{
  if (m_globeView == nullptr) {
    return;
  }
  forgetRemovedKmlNodes();
  const Camera& camera = m_globeView->controller().camera();
  const KmlViewParameters view = currentViewParameters();
  std::set<QString> inactive;
  for (const std::pair<const QString, std::pair<QString, QString>>& node :
       m_kmlNodes) {
    const std::shared_ptr<const KmlDocument> document =
        sharedKmlDocument(node.second.first, nullptr);
    if (!document || !kmlHasRegion(*document, node.second.second)) {
      continue;
    }
    if (!kmlRegionsActive(*document, node.second.second, camera)) {
      inactive.insert(node.first);
    } else if (m_kmlLinks->contains(node.first) &&
               !m_kmlLinks->isLoaded(node.first)) {
      // A link waiting for its Region: now.
      m_kmlLinks->load(node.first, view);
    }
  }
  m_kmlLinks->viewStopped(view);
  if (inactive != m_kmlInactive) {
    m_kmlInactive = inactive;
    refreshLayers();
  }
}

void MainWindow::kmlCheckChanged(const QModelIndex& topLeft)
{
  syncKmlEntry(topLeft);
  // A radio folder's child switched on: its siblings off.
  if (m_refreshingKml || !topLeft.isValid() ||
      m_layers->data(topLeft, Qt::CheckStateRole).toInt() != Qt::Checked) {
    return;
  }
  const QModelIndex parent = topLeft.parent();
  if (m_kmlRadio.count(
          m_layers->data(parent, LayerTreeModel::NodeIdRole).toString()) == 0) {
    return;
  }
  m_refreshingKml = true;
  for (int32_t i = 0; i < m_layers->rowCount(parent); ++i) {
    const QModelIndex sibling = m_layers->index(i, 0, parent);
    if (sibling != topLeft &&
        m_layers->data(sibling, Qt::CheckStateRole).toInt() != Qt::Unchecked) {
      m_layers->setData(sibling, Qt::Unchecked, Qt::CheckStateRole);
    }
  }
  m_refreshingKml = false;
}

void MainWindow::addKmlOverlay(const KmlDocument& document,
                               const KmlFeature& overlay,
                               const QString& featurePath,
                               const QModelIndex& parent, int32_t row)
{
  LayerDefinition definition;
  definition.id = QStringLiteral("kml:") +
                  QUuid::createUuid().toString(QUuid::WithoutBraces);
  definition.origin = LayerOrigin::User;
  if (overlay.kind == KmlFeature::Kind::GroundOverlay) {
    // A picture on the ground: imagery, in the file's draw order.
    definition.type = LayerType::Imagery;
    definition.name =
        overlay.name.isEmpty() ? tr("Ground overlay") : overlay.name;
    definition.source =
        withKmlRevision(kmlOverlaySource(document.path, featurePath));
  } else {
    // A picture on the screen: drawn by the view, from its own layer.
    definition.type = LayerType::Vector;
    definition.name =
        overlay.name.isEmpty() ? tr("Screen overlay") : overlay.name;
    definition.source =
        withKmlRevision(kmlLayerSource(document.path, featurePath));
    VectorStyle style;
    definition.style = style.toJson();
  }
  const QModelIndex layer = m_layers->addLayer(definition, parent, row);
  m_kmlNodes[m_layers->data(layer, LayerTreeModel::NodeIdRole).toString()] = {
      document.path, featurePath};
  if (!overlay.visibility) {
    m_layers->setData(layer, Qt::Unchecked, Qt::CheckStateRole);
  }
}

std::vector<QAction*> MainWindow::nodeActions(const QModelIndex& index,
                                              QObject* parent)
{
  const QString id =
      m_layers->data(index, LayerTreeModel::NodeIdRole).toString();
  std::vector<QAction*> actions;
  if (m_kmlLinks->contains(id)) {
    QAction* refresh = new QAction(tr("Refresh"), parent);
    refresh->setObjectName(QStringLiteral("refreshLinkAction"));
    connect(refresh, &QAction::triggered, this,
            [this, id]() { m_kmlLinks->load(id, currentViewParameters()); });
    actions.push_back(refresh);
  }
  const std::vector<QAction*> fly = flyThereActions(id, parent);
  actions.insert(actions.end(), fly.begin(), fly.end());
  const std::vector<QAction*> satellites = satelliteNodeActions(id, parent);
  actions.insert(actions.end(), satellites.begin(), satellites.end());
  const std::vector<QAction*> aircraft = aircraftNodeActions(id, parent);
  actions.insert(actions.end(), aircraft.begin(), aircraft.end());
  const std::vector<QAction*> vessels = vesselNodeActions(id, parent);
  actions.insert(actions.end(), vessels.begin(), vessels.end());
  if (m_kmlNodes.count(id) != 0 && kmlKeyOf(id).isEmpty()) {
    QAction* edit = new QAction(tr("Edit This Document"), parent);
    edit->setObjectName(QStringLiteral("editKmlNodeAction"));
    connect(edit, &QAction::triggered, this,
            [this, id]() { editKmlDocument(id); });
    actions.push_back(edit);
  }
  if (m_kmlNodes.count(id) != 0) {
    QAction* save = new QAction(tr("Save As KML or KMZ…"), parent);
    save->setObjectName(QStringLiteral("saveKmlAction"));
    connect(save, &QAction::triggered, this, [this, id]() {
      QString error;
      const std::optional<KmlDocument> document = kmlNodeDocument(id, &error);
      if (!document) {
        QMessageBox::warning(this, tr("Save As KML or KMZ"), error);
        return;
      }
      const QString name =
          document->root.children.size() == 1 &&
                  !document->root.children.front().name.isEmpty()
              ? document->root.children.front().name
              : QFileInfo(document->path).completeBaseName();
      saveKmlAs(this, *document, tr("Save As KML or KMZ"),
                name + QStringLiteral(".kml"));
    });
    actions.push_back(save);
  }
  return actions;
}

std::optional<KmlDocument>
MainWindow::kmlNodeDocument(const QString& nodeId, QString* errorString) const
{
  const std::map<QString, std::pair<QString, QString>>::const_iterator found =
      m_kmlNodes.find(nodeId);
  if (found == m_kmlNodes.end()) {
    return std::nullopt;
  }
  // Read again keeping the text, so what is saved is the file as it was.
  KmlReadOptions options;
  options.keepSource = true;
  std::optional<KmlDocument> document =
      readKmlFile(found->second.first, errorString, options);
  if (!document) {
    return std::nullopt;
  }
  const std::shared_ptr<const KmlDocument> shown =
      sharedKmlDocument(found->second.first, nullptr);
  document->webBase = shown ? shown->webBase : QString();
  if (found->second.second.isEmpty()) {
    return document;
  }
  return kmlExtract(*document, found->second.second);
}

std::optional<CameraPose> MainWindow::kmlNodePose(const QString& id) const
{
  const std::map<QString, std::pair<QString, QString>>::const_iterator found =
      m_kmlNodes.find(id);
  if (found == m_kmlNodes.end()) {
    return std::nullopt;
  }
  const std::shared_ptr<const KmlDocument> document =
      sharedKmlDocument(found->second.first, nullptr);
  const KmlFeature* feature =
      document ? kmlContainer(*document, found->second.second) : nullptr;
  if (feature == nullptr) {
    return std::nullopt;
  }
  // Its own view, else one framing its first placemark with a geometry.
  std::optional<CameraPose> pose = kmlFeaturePose(*feature);
  for (std::size_t i = 0; !pose && i < feature->children.size(); ++i) {
    pose = kmlFeaturePose(feature->children[i]);
  }
  return pose;
}

std::vector<QAction*> MainWindow::flyThereActions(const QString& id,
                                                  QObject* parent)
{
  const std::optional<CameraPose> pose = kmlNodePose(id);
  if (!pose || m_globeView == nullptr) {
    return {};
  }
  QAction* fly = new QAction(tr("Fly There"), parent);
  fly->setObjectName(QStringLiteral("flyThereAction"));
  const CameraPose target = *pose;
  connect(fly, &QAction::triggered, this,
          [this, target]() { m_globeView->flyTo(target, 0.0); });
  return {fly};
}

namespace
{

/** A hot spot's coordinate as a fraction of the icon's @p extent. */
double hotSpotFraction(double value, KmlVec2::Units units, double extent)
{
  switch (units) {
  case KmlVec2::Units::Fraction:
    return value;
  case KmlVec2::Units::Pixels:
    return value / std::max(extent, 1.0);
  case KmlVec2::Units::InsetPixels:
    break;
  }
  return 1.0 - value / std::max(extent, 1.0);
}

/** A raised KML shape as the view draws it. */
RaisedShapeDraw raisedShapeDraw(const KmlRaisedShape& shape)
{
  RaisedShapeDraw draw;
  draw.height =
      shape.relative ? OverlayHeight::Relative : OverlayHeight::Absolute;
  draw.closed = shape.closed;
  draw.surface = shape.surface;
  draw.walls = shape.extrude;
  draw.line = shape.line;
  draw.widthPx = shape.widthPx;
  draw.fill = shape.fill;
  for (const std::vector<KmlCoordinate>& ring : shape.rings) {
    std::vector<Geodetic> points;
    points.reserve(ring.size());
    for (const KmlCoordinate& c : ring) {
      points.push_back(Geodetic{c.latDeg, c.lonDeg, c.altM});
    }
    draw.rings.push_back(std::move(points));
  }
  return draw;
}

OverlayHeight overlayHeight(KmlAltitudeMode mode)
{
  switch (mode) {
  case KmlAltitudeMode::ClampToGround:
  case KmlAltitudeMode::ClampToSeaFloor:
    return OverlayHeight::Ground;
  case KmlAltitudeMode::RelativeToGround:
  case KmlAltitudeMode::RelativeToSeaFloor:
    return OverlayHeight::Relative;
  case KmlAltitudeMode::Absolute:
    break;
  }
  return OverlayHeight::Absolute;
}

} // namespace

QImage MainWindow::tintedIcon(const QString& where, const QColor& tint,
                              bool pushpin)
{
  const std::pair<QString, QRgb> key{where, tint.rgba()};
  const std::map<std::pair<QString, QRgb>, QImage>::const_iterator known =
      m_tintedIcons.find(key);
  if (known != m_tintedIcons.end()) {
    return known->second;
  }
  // A missing picture is nothing; a missing icon the pushpin.
  QImage icon = pushpin ? m_icons->icon(where) : m_icons->image(where);
  if (icon.isNull()) {
    return icon;
  }
  icon = icon.convertToFormat(QImage::Format_ARGB32);
  if (tint != QColor(255, 255, 255)) {
    // KML multiplies the icon's colours (and opacity) by the tint.
    for (int32_t y = 0; y < icon.height(); ++y) {
      QRgb* row = reinterpret_cast<QRgb*>(icon.scanLine(y));
      for (int32_t x = 0; x < icon.width(); ++x) {
        const QRgb p = row[x];
        row[x] =
            qRgba(qRed(p) * tint.red() / 255, qGreen(p) * tint.green() / 255,
                  qBlue(p) * tint.blue() / 255, qAlpha(p) * tint.alpha() / 255);
      }
    }
  }
  m_tintedIcons[key] = icon;
  return icon;
}

void MainWindow::refreshBillboards(const LayerStack& stack)
{
  if (m_globeView == nullptr || m_icons == nullptr) {
    return;
  }
  std::vector<BillboardDraw> draws;
  std::vector<RaisedShapeDraw> raised;
  std::vector<ScreenOverlayDraw> screens;
  for (const VectorStackEntry& entry : stack.vectors) {
    if (entry.definition.source.value(QStringLiteral("kind")).toString() !=
        QLatin1String("kml")) {
      continue;
    }
    const std::shared_ptr<KmlFeatureProvider> provider =
        std::dynamic_pointer_cast<KmlFeatureProvider>(
            m_registry->features(entry.definition.source, nullptr));
    if (!provider) {
      continue;
    }
    std::vector<KmlBillboard> points = provider->billboards();
    if (provider->hasTracks()) {
      const std::vector<KmlBillboard> markers =
          provider->trackMarkersAt(m_clock->time());
      points.insert(points.end(), markers.begin(), markers.end());
    }
    for (const KmlBillboard& billboard : points) {
      draws.push_back(billboardDraw(billboard));
    }
    for (const KmlFeature* screen : provider->screenOverlays()) {
      ScreenOverlayDraw draw;
      draw.image =
          tintedIcon(resolveKmlHref(*provider->document(), screen->iconHref),
                     screen->color, false);
      draw.placement = screen->screen.value_or(KmlScreenPlacement{});
      draw.drawOrder = screen->drawOrder;
      if (!draw.image.isNull()) {
        screens.push_back(std::move(draw));
      }
    }
    for (const KmlRaisedShape& shape : provider->raisedShapes()) {
      raised.push_back(raisedShapeDraw(shape));
    }
  }
  m_globeView->setBillboards(std::move(draws));
  m_globeView->setRaisedShapes(std::move(raised));
  std::stable_sort(screens.begin(), screens.end(),
                   [](const ScreenOverlayDraw& a, const ScreenOverlayDraw& b) {
                     return a.drawOrder < b.drawOrder;
                   });
  m_globeView->setScreenOverlays(std::move(screens));
}

BillboardDraw MainWindow::billboardDraw(const KmlBillboard& billboard)
{
  constexpr double iconPx = 32.0;
  BillboardDraw draw;
  draw.latDeg = billboard.latDeg;
  draw.lonDeg = billboard.lonDeg;
  draw.height = overlayHeight(billboard.altitudeMode);
  draw.heightM = billboard.altM;
  draw.stalk = billboard.extrude;
  draw.stalkColor = billboard.stalkColor;
  draw.icon = tintedIcon(billboard.icon, billboard.tint);
  draw.sizePx = iconPx * billboard.scale;
  draw.headingDeg = billboard.headingDeg;
  // KML's hot spot counts y from the bottom; the default pushpin's needle
  // is at its bottom centre.
  if (billboard.icon.isEmpty()) {
    draw.hotSpot = QPointF(0.5, 1.0);
  } else {
    const QSizeF size(draw.icon.size());
    const KmlVec2& h = billboard.hotSpot;
    draw.hotSpot = QPointF(hotSpotFraction(h.x, h.xUnits, size.width()),
                           1.0 - hotSpotFraction(h.y, h.yUnits, size.height()));
  }
  draw.label = billboard.label;
  draw.labelColor = billboard.labelColor;
  draw.labelScale = billboard.labelScale;
  return draw;
}

bool MainWindow::showBalloonAt(LonLat point, double degreesPerPixel)
{
  const LayerStack stack = m_layers->layerStack();
  for (std::size_t i = stack.vectors.size(); i-- > 0;) {
    const LayerDefinition& definition = stack.vectors[i].definition;
    if (definition.source.value(QStringLiteral("kind")).toString() !=
        QLatin1String("kml")) {
      continue;
    }
    const std::shared_ptr<KmlFeatureProvider> provider =
        std::dynamic_pointer_cast<KmlFeatureProvider>(
            m_registry->features(definition.source, nullptr));
    if (!provider) {
      continue;
    }
    const std::vector<const KmlFeature*> found = provider->placemarksAt(
        GeoPoint{point.latDeg, point.lonDeg}, 4.0 * degreesPerPixel);
    if (found.empty()) {
      continue;
    }
    const KmlFeature& placemark = *found.front();
    KmlStyleResolver resolver(*provider->document());
    const KmlStyle style = resolver.resolve(placemark);
    if (style.balloon && !style.balloon->display) {
      return true; // hidden on purpose
    }
    if (m_balloon == nullptr) {
      m_balloon = new BalloonWindow(this);
      connect(m_balloon, &BalloonWindow::flyToRequested, this,
              [this](const CameraPose& pose) {
                if (m_globeView != nullptr) {
                  m_globeView->flyTo(pose, 0.0);
                }
              });
    }
    const QString base =
        provider->document()->kmz ? QString() : provider->document()->base;
    m_balloon->showBalloon(placemark.name, kmlBalloonHtml(placemark, style),
                           style.balloon ? style.balloon->backgroundColor
                                         : QColor(Qt::white),
                           kmlFeaturePose(placemark), base);
    return true;
  }
  return false;
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event)
{
  const QList<QUrl> urls = event->mimeData()->urls();
  const bool any = std::any_of(urls.begin(), urls.end(), [](const QUrl& url) {
    const QString path = url.toLocalFile().toLower();
    return path.endsWith(QLatin1String(".kml")) ||
           path.endsWith(QLatin1String(".kmz")) ||
           path.endsWith(QLatin1String(".terrasoar")) ||
           path.endsWith(QLatin1String(".tle")) ||
           path.endsWith(QLatin1String(".3le")) ||
           path.endsWith(QLatin1String(".2le")) ||
           path.endsWith(QLatin1String(".txt")) ||
           path.endsWith(QLatin1String(".nmea")) ||
           path.endsWith(QLatin1String(".ais")) ||
           path.endsWith(QLatin1String(".log")) ||
           path.endsWith(QLatin1String(".csv")) || isAdsbFileName(path);
  });
  if (any) {
    event->acceptProposedAction();
  }
}

void MainWindow::dropEvent(QDropEvent* event)
{
  const QList<QUrl> urls = event->mimeData()->urls();
  for (const QUrl& url : urls) {
    const QString path = url.toLocalFile();
    if (path.isEmpty()) {
      continue;
    }
    QString error;
    if (!openFile(path, &error)) {
      QMessageBox::warning(this, tr("Open"), error);
    }
  }
  event->acceptProposedAction();
}

int32_t MainWindow::identifyAt(LonLat point, double degreesPerPixel)
{
  // KML being drawn or edited takes the click first.
  if (kmlClicked(point, degreesPerPixel)) {
    return 1;
  }
  if (!m_featureInfoAction->isChecked()) {
    return 0;
  }
  // A KML placemark there: its balloon.
  if (showBalloonAt(point, degreesPerPixel)) {
    return 1;
  }
  // The visible WMS layers that can be asked, topmost first, as drawn now;
  // then the WFS layers, whose features are here already.
  const LayerStack stack = stackAtTime(timedStack(), m_clock->time());
  std::vector<FeatureInfoQuery> queries;
  for (std::size_t i = stack.imagery.size(); i-- > 0;) {
    const LayerDefinition& definition = stack.imagery[i].definition;
    if (definition.source.value(QStringLiteral("kind")).toString() !=
        QLatin1String("wms")) {
      continue;
    }
    const std::optional<WmsSource> source =
        WmsSource::fromJson(definition.source, nullptr);
    if (!source || source->featureInfoUrl.isEmpty()) {
      continue;
    }
    // The tile at the view's scale (its pixels about the screen's).
    const double perPixel = std::max(degreesPerPixel, 1e-9);
    const int32_t level = std::clamp(
        static_cast<int32_t>(std::lround(std::log2(
            180.0 / (static_cast<double>(source->tileSizePx) * perPixel)))),
        0, std::min(source->maximumLevel, TileKey::maximumLevel));
    const TileKey key =
        TileKey::containing(level, Geodetic{point.latDeg, point.lonDeg, 0.0});
    const QUrl url =
        getFeatureInfoUrl(*source, key, point.lonDeg, point.latDeg);
    if (!url.isEmpty()) {
      queries.push_back(
          FeatureInfoQuery{definition.name, url, source->infoFormat});
    }
  }
  // Vector layers whose features are at hand (WFS), topmost first: within
  // a few pixels of the click.
  std::vector<FeatureInfoLocal> local;
  for (std::size_t i = stack.vectors.size(); i-- > 0;) {
    const LayerDefinition& definition = stack.vectors[i].definition;
    if (definition.source.value(QStringLiteral("kind")).toString() !=
        QLatin1String("wfs")) {
      continue;
    }
    const std::shared_ptr<FeatureProvider> provider =
        m_registry->features(definition.source, nullptr);
    if (!provider) {
      continue;
    }
    const std::vector<Feature> found = provider->featuresAt(
        GeoPoint{point.latDeg, point.lonDeg}, 4.0 * degreesPerPixel);
    if (!found.empty()) {
      local.push_back(FeatureInfoLocal{definition.name, found});
    }
  }
  if (queries.empty() && local.empty()) {
    return 0;
  }
  if (m_featureInfoWindow == nullptr) {
    m_featureInfoWindow = new FeatureInfoWindow(m_registry->network(), this);
  }
  m_featureInfoWindow->query(point, queries, local);
  m_featureInfoWindow->show();
  m_featureInfoWindow->raise();
  return static_cast<int32_t>(queries.size() + local.size());
}

void MainWindow::updateTimeBar(const LayerStack& stack)
{
  // The times of the visible layers that follow the clock, joined; those
  // following the month of the year step through the clock's year.
  QStringList extents;
  for (const ImageryStackEntry& entry : stack.imagery) {
    const std::optional<LayerTime> time =
        LayerTime::fromSource(entry.definition.source);
    if (!time || !time->followsClock()) {
      continue;
    }
    if (time->mode == LayerTime::Mode::MonthOfYear) {
      const int32_t year = m_clock->time().date().year();
      extents.append(QStringLiteral("%1-01-01/%1-12-01/P1M").arg(year));
    } else {
      extents.append(time->extent);
    }
  }
  for (const VectorStackEntry& entry : stack.vectors) {
    const QJsonObject& source = entry.definition.source;
    if (source.value(QStringLiteral("kind")).toString() !=
        QLatin1String("kml")) {
      continue;
    }
    const std::shared_ptr<const KmlDocument> document = sharedKmlDocument(
        source.value(QStringLiteral("path")).toString(), nullptr);
    const QString extent =
        document ? kmlTimeExtent(
                       *document,
                       source.value(QStringLiteral("container")).toString(),
                       source.value(QStringLiteral("deep")).toBool())
                 : QString();
    if (!extent.isEmpty()) {
      extents.append(extent);
    }
  }
  if (m_aircraft != nullptr) {
    const QString recordings = m_aircraft->timeExtent();
    if (!recordings.isEmpty()) {
      extents.append(recordings);
    }
  }
  if (m_vessels != nullptr) {
    const QString recordings = m_vessels->timeExtent();
    if (!recordings.isEmpty()) {
      extents.append(recordings);
    }
  }
  m_timeBar->setExtent(TimeExtent::parse(extents));
  if (!extents.isEmpty() && !m_timeBarOffered) {
    m_timeBarOffered = true;
    m_timeBar->show();
  }
}

void MainWindow::watchLayerStatus(const LayerStack& stack)
{
  // Each imagery layer's provider (shared through the registry) tells the
  // relay when its status changes; the relay calls updateLayerStatus().
  std::vector<std::pair<QString, std::shared_ptr<ImageryProvider>>> providers;
  std::vector<std::pair<QString, QString>> broken;
  const std::weak_ptr<QTimer> relay = m_statusRelay;
  for (const ImageryStackEntry& entry : stack.imagery) {
    QString error;
    std::shared_ptr<ImageryProvider> provider =
        m_registry->imagery(entry.definition.source, &error);
    if (!provider) {
      broken.emplace_back(entry.nodeId, error);
      continue;
    }
    provider->setStatusCallback([relay]() {
      if (const std::shared_ptr<QTimer> timer = relay.lock()) {
        QMetaObject::invokeMethod(timer.get(), qOverload<>(&QTimer::start),
                                  Qt::QueuedConnection);
      }
    });
    providers.emplace_back(entry.nodeId, std::move(provider));
  }
  m_layerProviders = std::move(providers);
  m_brokenLayers = std::move(broken);
  updateLayerStatus();
}

void MainWindow::updateLayerStatus()
{
  for (const std::pair<QString, std::shared_ptr<ImageryProvider>>& layer :
       m_layerProviders) {
    const ProviderStatus status = layer.second->status();
    const QModelIndex index = m_layers->indexOfId(layer.first);
    const int32_t before =
        m_layers->data(index, LayerTreeModel::StatusRole).toInt();
    m_layers->setLayerStatus(layer.first, status);
    if (status.state == ProviderStatus::State::Error &&
        before != static_cast<int32_t>(ProviderStatus::State::Error)) {
      statusBar()->showMessage(
          tr("%1: %2").arg(m_layers->data(index).toString(), status.message),
          10000);
    }
  }
  for (const std::pair<QString, QString>& layer : m_brokenLayers) {
    m_layers->setLayerStatus(
        layer.first,
        ProviderStatus{ProviderStatus::State::Error, layer.second});
  }
}

bool MainWindow::isLoadingTiles() const
{
  return m_globeView != nullptr && m_globeView->isLoadingTiles();
}

void MainWindow::addWmsLayer(const QString& address)
{
  AddWmsDialog dialog(m_registry->network(), this);
  if (!address.isEmpty()) {
    dialog.connectTo(address);
  }
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  const std::optional<LayerDefinition>& definition = dialog.layerDefinition();
  if (definition) {
    m_layerDock->addLayer(*definition);
  }
}

void MainWindow::addWmtsLayer(const QString& address)
{
  AddWmtsDialog dialog(m_registry->network(), this);
  if (!address.isEmpty()) {
    dialog.connectTo(address);
  }
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  const std::optional<LayerDefinition>& definition = dialog.layerDefinition();
  if (definition) {
    m_layerDock->addLayer(*definition);
  }
}

void MainWindow::addWfsLayer(const QString& address)
{
  AddWfsDialog dialog(m_registry->network(), this);
  if (!address.isEmpty()) {
    dialog.connectTo(address);
  }
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  const std::optional<LayerDefinition>& definition = dialog.layerDefinition();
  if (definition) {
    m_layerDock->addLayer(*definition);
  }
}

void MainWindow::addXyzLayer()
{
  AddXyzDialog dialog(m_registry->network(), this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  const std::optional<LayerDefinition>& definition = dialog.layerDefinition();
  if (definition) {
    m_layerDock->addLayer(*definition);
  }
}

ElevationProfileDialog* MainWindow::showElevationProfile()
{
  if (m_globeView == nullptr) {
    return nullptr;
  }
  if (!m_profileDialog) {
    m_profileDialog = new ElevationProfileDialog(this);
    m_profileDialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(m_profileDialog, &ElevationProfileDialog::drawAnotherRequested,
            this, [this]() {
              m_profileDialog->setDrawing();
              m_globeView->startRegionDrawing(GlobeView::RegionDrawing::Line);
            });
    connect(m_profileDialog, &ElevationProfileDialog::pointHovered, this,
            [this](std::optional<GeoPoint> point) {
              m_globeView->setProfileLine(
                  m_profileLine, point ? std::optional<LonLat>(LonLat{
                                             point->lonDeg, point->latDeg})
                                       : std::nullopt);
            });
    connect(m_profileDialog, &QObject::destroyed, this, [this]() {
      if (m_profileCancel) {
        m_profileCancel->store(true);
      }
      m_profileLine.clear();
      if (m_globeView != nullptr) {
        m_globeView->setProfileLine({});
        if (m_globeView->regionDrawing() == GlobeView::RegionDrawing::Line) {
          m_globeView->startRegionDrawing(GlobeView::RegionDrawing::None);
        }
      }
    });
  }
  m_profileDialog->show();
  m_profileDialog->raise();
  m_profileDialog->setDrawing();
  m_globeView->startRegionDrawing(GlobeView::RegionDrawing::Line);
  return m_profileDialog;
}

void MainWindow::profileLine(const std::vector<LonLat>& points)
{
  if (m_kmlDrawing != 0) {
    return; // a KML path, not a profile
  }
  if (m_globeView == nullptr || points.size() < 2) {
    return;
  }
  if (!m_profileDialog) {
    showElevationProfile();
  }
  if (m_globeView->regionDrawing() == GlobeView::RegionDrawing::Line) {
    m_globeView->startRegionDrawing(GlobeView::RegionDrawing::None);
  }
  m_profileLine = points;
  m_globeView->setProfileLine(points);
  const ElevationDraw draw =
      elevationDrawForStack(m_layers->layerStack(), *m_registry, nullptr);
  if (!draw.provider) {
    m_profileDialog->setFailed(
        tr("There is no elevation layer to read heights from."));
    return;
  }
  if (m_profileCancel) {
    m_profileCancel->store(true);
  }
  std::shared_ptr<std::atomic<bool>> cancel =
      std::make_shared<std::atomic<bool>>(false);
  m_profileCancel = cancel;
  m_profileDialog->setComputing();
  std::vector<GeoPoint> vertices;
  vertices.reserve(points.size());
  for (const LonLat& point : points) {
    vertices.push_back(GeoPoint{point.latDeg, normaliseLonDeg(point.lonDeg)});
  }
  // Only the GUI thread touches the window (through this guard).
  std::shared_ptr<QPointer<ElevationProfileDialog>> target =
      std::make_shared<QPointer<ElevationProfileDialog>>(m_profileDialog);
  std::shared_ptr<ElevationProvider> provider = draw.provider;
  std::thread([provider, vertices, cancel, target]() mutable {
    std::optional<ElevationProfile> profile =
        computeElevationProfile(*provider, vertices, profileSamples, *cancel);
    QMetaObject::invokeMethod(
        qApp,
        [profile = std::move(profile), cancel = std::move(cancel),
         target = std::move(target)]() {
          if (cancel->load() || !*target) {
            return;
          }
          if (profile) {
            (*target)->setProfile(*profile);
          } else {
            (*target)->setFailed(
                MainWindow::tr("There are no heights along the line."));
          }
        },
        Qt::QueuedConnection);
  }).detach();
}

OfflineRegionsDialog* MainWindow::showOfflineRegions()
{
  if (!m_offlineRegionsDialog) {
    m_offlineRegionsDialog =
        new OfflineRegionsDialog(*m_offlineRegions, m_globeView, this);
    m_offlineRegionsDialog->setAttribute(Qt::WA_DeleteOnClose);
    connect(m_offlineRegionsDialog, &OfflineRegionsDialog::newRegionRequested,
            this, [this]() { newOfflineRegion(); });
  }
  m_offlineRegionsDialog->show();
  m_offlineRegionsDialog->raise();
  m_offlineRegionsDialog->activateWindow();
  return m_offlineRegionsDialog;
}

PackManagerDialog* MainWindow::showDataPacks()
{
  if (!m_packsDialog) {
    m_packsDialog = new PackManagerDialog(*m_packs, this);
    m_packsDialog->setAttribute(Qt::WA_DeleteOnClose);
  }
  m_packsDialog->show();
  m_packsDialog->raise();
  m_packsDialog->activateWindow();
  return m_packsDialog;
}

NewRegionDialog* MainWindow::newOfflineRegion()
{
  if (m_newRegionDialog) {
    m_newRegionDialog->show();
    m_newRegionDialog->raise();
    return m_newRegionDialog;
  }
  // The WMS, WMTS and XYZ layers of the project, each source once, top
  // first; layers with times as drawn now.
  std::vector<RegionLayerChoice> layers;
  const LayerStack stack = stackAtTime(timedStack(), m_clock->time());
  for (std::size_t i = stack.imagery.size(); i-- > 0;) {
    const LayerDefinition& definition = stack.imagery[i].definition;
    const QString kind =
        definition.source.value(QStringLiteral("kind")).toString();
    if (kind != QLatin1String("wms") && kind != QLatin1String("wmts") &&
        kind != QLatin1String("xyz")) {
      continue;
    }
    const bool seen = std::any_of(layers.begin(), layers.end(),
                                  [&](const RegionLayerChoice& layer) {
                                    return layer.source == definition.source;
                                  });
    if (!seen) {
      layers.push_back(RegionLayerChoice{definition.name, definition.source});
    }
  }
  for (std::size_t i = stack.vectors.size(); i-- > 0;) {
    const LayerDefinition& definition = stack.vectors[i].definition;
    if (definition.source.value(QStringLiteral("kind")).toString() !=
        QLatin1String("wfs")) {
      continue;
    }
    const bool seen = std::any_of(layers.begin(), layers.end(),
                                  [&](const RegionLayerChoice& layer) {
                                    return layer.source == definition.source;
                                  });
    if (!seen) {
      layers.push_back(RegionLayerChoice{definition.name, definition.source});
    }
  }
  if (layers.empty()) {
    QMessageBox::information(
        this, tr("New Offline Region"),
        tr("Only layers from WMS, WMTS, XYZ tile and WFS servers can be kept "
           "for offline use. Add one first (File → Add WMS Layer…, Add WMTS "
           "Layer…, Add XYZ Tile Layer… or Add WFS Layer…)."));
    return nullptr;
  }
  if (!m_countries && !m_countriesFile.isEmpty()) {
    m_countries = std::make_shared<const CountryAreas>(m_countriesFile);
    if (!m_countries->isValid()) {
      qWarning("Countries: %s", qUtf8Printable(m_countries->errorString()));
    }
  }
  m_newRegionDialog = new NewRegionDialog(*m_offlineRegions, std::move(layers),
                                          m_globeView, m_countries, this);
  m_newRegionDialog->setAttribute(Qt::WA_DeleteOnClose);
  connect(m_newRegionDialog, &QDialog::accepted, this, [this]() {
    QString error;
    const int64_t id =
        m_offlineRegions->addRegion(m_newRegionDialog->region(), &error);
    if (id == 0) {
      QMessageBox::warning(this, tr("New Offline Region"), error);
      return;
    }
    showOfflineRegions()->selectRegion(id);
  });
  m_newRegionDialog->show();
  return m_newRegionDialog;
}

bool MainWindow::openProject(const QString& path, QString* errorString)
{
  const std::optional<Project> project = loadProject(path, errorString);
  if (!project) {
    return false;
  }
  // The KML of the project before is gone with its entries.
  m_kmlEdits.clear();
  m_kmlRoots.clear();
  m_kmlNodes.clear();
  m_kmlRadio.clear();
  m_kmlSelection.reset();
  showKmlHandles();
  if (!m_layers->fromJson(project->layers, errorString)) {
    return false;
  }
  openKmlProjectEntries(project->kml);
  if (m_globeView) {
    // An unknown projection (a newer program's) keeps the current one.
    m_globeView->setMapProjection(
        projectionWith(project->projection, project->projectionParameters));
    m_globeView->setViewMode(project->mapView ? ViewMode::Map : ViewMode::Globe,
                             false);
    m_globeView->setPose(project->camera);
  }
  if (project->clock && project->clock->realTime) {
    m_clock->setRealTime();
  } else {
    if (project->time.isValid()) {
      m_clock->setTime(project->time);
    }
    if (project->clock) {
      m_clock->setRate(project->clock->rate);
      if (project->clock->running) {
        m_clock->play();
      }
    }
  }
  m_projectPath = path;
  return true;
}

bool MainWindow::saveProject(const QString& path, QString* errorString)
{
  Project project;
  project.camera = m_globeView ? m_globeView->controller().camera().pose()
                               : GlobeView::homePose();
  if (m_globeView) {
    project.mapView = m_globeView->viewMode() == ViewMode::Map;
    project.projection = m_globeView->mapProjection()->id();
    project.projectionParameters =
        m_globeView->mapProjection()->parameterValues();
  }
  project.time = m_clock->time();
  project.clock =
      ClockState{m_clock->isRealTime(), m_clock->isRunning(), m_clock->rate()};
  // KML documents: their files (or text); their entries are made again
  // from them when the project opens.
  bool cancelled = false;
  project.kml = kmlProjectEntries(&cancelled);
  if (cancelled) {
    if (errorString != nullptr) {
      *errorString = tr("The project wasn't saved.");
    }
    return false;
  }
  project.layers = kmlProjectLayers(project.kml);
  if (!ts::saveProject(path, project, errorString)) {
    return false;
  }
  m_projectPath = path;
  return true;
}

void MainWindow::openProjectDialog()
{
  const QString path = QFileDialog::getOpenFileName(
      this, tr("Open Project"), m_projectPath,
      tr("Terra Soar projects (*.terrasoar);;All files (*)"));
  if (path.isEmpty()) {
    return;
  }
  QString error;
  if (!openProject(path, &error)) {
    QMessageBox::warning(this, tr("Open Project"), error);
  }
}

void MainWindow::saveProjectDialog(bool askForPath)
{
  QString path = m_projectPath;
  if (askForPath || path.isEmpty()) {
    path =
        QFileDialog::getSaveFileName(this, tr("Save Project"), m_projectPath,
                                     tr("Terra Soar projects (*.terrasoar)"));
    if (path.isEmpty()) {
      return;
    }
    if (!path.endsWith(QLatin1String(".terrasoar"))) {
      path += QStringLiteral(".terrasoar");
    }
  }
  QString error;
  if (!saveProject(path, &error)) {
    QMessageBox::warning(this, tr("Save Project"), error);
  }
}

void MainWindow::showEyeHeight(double eyeHeightM)
{
  m_eyeHeightM = eyeHeightM;
  m_eyeLabel->setText(
      tr("Eye height %1").arg(formatDistance(eyeHeightM, QLocale())));
}

void MainWindow::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QMainWindow::changeEvent(event);
}

std::vector<AboutDataRow> MainWindow::aboutData() const
{
  // Each pack shown with its name for the user: the base pack, then the
  // installed packs.
  std::vector<std::pair<QString, PackManifest>> packs;
  if (!m_basePackDirectory.isEmpty()) {
    QString error;
    std::optional<PackManifest> base =
        PackManifest::read(m_basePackDirectory, &error);
    if (base) {
      packs.emplace_back(tr("Base pack"), std::move(*base));
    }
  }
  if (m_packs) {
    for (const InstalledPack& pack : m_packs->installed()) {
      if (!pack.damaged) {
        packs.emplace_back(pack.manifest.id, pack.manifest);
      }
    }
  }
  std::vector<AboutDataRow> rows;
  for (const std::pair<QString, PackManifest>& pack : packs) {
    for (const PackDataset& dataset : pack.second.datasets) {
      AboutDataRow row;
      row.pack = pack.first;
      row.name = dataset.name.isEmpty() ? dataset.id : dataset.name;
      row.source =
          dataset.source.version.isEmpty()
              ? dataset.source.name
              : tr("%1, %2").arg(dataset.source.name, dataset.source.version);
      row.licence = dataset.licence;
      row.attribution = dataset.attribution;
      rows.push_back(row);
    }
  }
  // Online sources in use: CelesTrak, when a layer has satellites from it.
  if (!usesCelestrak(m_layers->toJson())) {
    return rows;
  }
  AboutDataRow celestrak;
  celestrak.pack = tr("Online");
  celestrak.name = tr("Satellite orbital elements");
  celestrak.source = tr("CelesTrak (celestrak.org), from the US Space "
                        "Force's catalogue");
  celestrak.licence = tr("Free to use, with credit to CelesTrak");
  celestrak.attribution = QStringLiteral("Orbital elements: CelesTrak");
  rows.push_back(celestrak);
  return rows;
}

void MainWindow::showAbout()
{
  AboutDialog dialog(this, aboutData());
  dialog.exec();
  if (dialog.userGuideRequested()) {
    showUserGuide(this);
  }
}

} // namespace ts
