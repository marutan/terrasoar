// SPDX-License-Identifier: GPL-2.0-or-later
#include "mainwindow.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QJsonArray>
#include <QKeySequence>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QWidget>

#include "addwebterraindialog.h"
#include "addwmsdialog.h"
#include "globeview.h"
#include "layertreedock.h"
#include "newregiondialog.h"
#include "offlineregionmanager.h"
#include "offlineregionsdialog.h"
#include "packmanager.h"
#include "packmanagerdialog.h"
#include "ts/base/buildinfo.h"
#include "ts/elevation/gdalelevationprovider.h"
#include "ts/geo/geoformat.h"
#include "ts/globe/layerstackdraws.h"
#include "ts/layers/layertreemodel.h"
#include "ts/layers/projectfile.h"
#include "ts/packs/basepack.h"
#include "ts/packs/packmanifest.h"
#include "ts/projection/projection.h"
#include "ts/providers/providerregistry.h"
#include "ts/regions/countryareas.h"
#include "ts/wms/wmsimageryprovider.h"

namespace ts
{

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
    connect(m_globeView, &GlobeView::pointerPositionChanged, this,
            &MainWindow::showPointerPosition);
    connect(m_globeView, &GlobeView::cameraChanged, this,
            &MainWindow::showEyeHeight);
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
  m_layerDock = new LayerTreeDock(m_layers, this);
  addDockWidget(Qt::LeftDockWidgetArea, m_layerDock);
  connect(m_layerDock, &LayerTreeDock::addWmsLayerRequested, this,
          [this]() { addWmsLayer(); });
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

  createActions();
  createMenus();
  retranslateUi();

  resize(1024, 768);
}

MainWindow::~MainWindow()
{
  // Dialogs first: they refer to the regions, which go before the registry.
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
    const int status =
        m_layers->data(index, LayerTreeModel::StatusRole).toInt();
    if (status == static_cast<int>(ProviderStatus::State::Offline)) {
      problems.insert(entry.definition.name, QStringLiteral("offline"));
    } else if (status == static_cast<int>(ProviderStatus::State::Error)) {
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
  m_mapViewAction->setShortcut(
      QKeySequence(static_cast<int>(Qt::CTRL) | static_cast<int>(Qt::Key_M)));
  m_scaleBarAction = new QAction(this);
  m_scaleBarAction->setObjectName(QStringLiteral("scaleBarAction"));
  m_scaleBarAction->setCheckable(true);
  m_scaleBarAction->setChecked(true);
  m_projectionGroup = new QActionGroup(this);
  for (const Projection* projection : projections()) {
    QAction* action = new QAction(this);
    action->setObjectName(QStringLiteral("projectionAction_") +
                          projection->id());
    action->setCheckable(true);
    action->setData(projection->id());
    m_projectionGroup->addAction(action);
  }
  if (m_globeView) {
    connect(m_mapViewAction, &QAction::toggled, m_globeView,
            [this](bool checked) {
              m_globeView->setViewMode(
                  checked ? ViewMode::Map : ViewMode::Globe, true);
            });
    connect(m_scaleBarAction, &QAction::toggled, m_globeView,
            &GlobeView::setScaleBarVisible);
    connect(m_projectionGroup, &QActionGroup::triggered, m_globeView,
            [this](const QAction* action) {
              m_globeView->setMapProjection(
                  projectionById(action->data().toString()));
            });
    connect(m_globeView, &GlobeView::viewModeChanged, this,
            &MainWindow::syncViewActions);
    connect(m_globeView, &GlobeView::mapProjectionChanged, this,
            &MainWindow::syncViewActions);
  } else {
    m_mapViewAction->setEnabled(false);
    m_scaleBarAction->setEnabled(false);
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

  m_addWebTerrainAction = new QAction(this);
  m_addWebTerrainAction->setObjectName(QStringLiteral("addWebTerrainAction"));
  connect(m_addWebTerrainAction, &QAction::triggered, this,
          [this]() { addWebTerrain(); });

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

  m_aboutAction = new QAction(this);
  m_aboutAction->setObjectName(QStringLiteral("aboutAction"));
  m_aboutAction->setMenuRole(QAction::AboutRole);
  connect(m_aboutAction, &QAction::triggered, this, &MainWindow::showAbout);

  m_aboutQtAction = new QAction(this);
  m_aboutQtAction->setObjectName(QStringLiteral("aboutQtAction"));
  m_aboutQtAction->setMenuRole(QAction::AboutQtRole);
  connect(m_aboutQtAction, &QAction::triggered, qApp, &QApplication::aboutQt);
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
  const QStringList ids = {QStringLiteral("egm2008"), QStringLiteral("egm96"),
                           QStringLiteral("ellipsoid")};
  const QStringList choices = {
      tr("Sea level, EGM2008 (most recent DEMs, e.g. Copernicus)"),
      tr("Sea level, EGM96 (e.g. SRTM)"),
      tr("WGS 84 ellipsoid (GNSS heights)")};
  int current = static_cast<int>(ids.indexOf(demVerticalDatumId(detected)));
  const QString label =
      detected == DemVerticalDatum::Unknown
          ? tr("The file doesn't say what its heights are measured from. "
               "Heights of \"%1\" are above:")
                .arg(QFileInfo(path).fileName())
          : tr("Heights of \"%1\" are above (as the file says):")
                .arg(QFileInfo(path).fileName());
  bool ok = false;
  const QString chosen =
      QInputDialog::getItem(this, tr("Add Elevation File"), label, choices,
                            std::max(current, 0), false, &ok);
  if (!ok) {
    return;
  }
  addElevationLayer(path, ids.value(static_cast<int>(choices.indexOf(chosen))));
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
  AddWebTerrainDialog dialog(this);
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
  for (int row = m_layers->rowCount() - 1; row >= 0; --row) {
    pending.push_back(m_layers->index(row, 0));
  }
  while (!pending.empty()) {
    const QModelIndex index = pending.back();
    pending.pop_back();
    if (m_layers->data(index, LayerTreeModel::IsCollectionRole).toBool()) {
      for (int row = m_layers->rowCount(index) - 1; row >= 0; --row) {
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
  m_viewMenu->addAction(m_scaleBarAction);
  m_exaggerationMenu = m_viewMenu->addMenu(QString());
  m_exaggerationMenu->setObjectName(QStringLiteral("exaggerationMenu"));
  m_exaggerationMenu->addActions(m_exaggerationGroup->actions());
  m_viewMenu->addAction(m_hillshadeAction);
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
  m_helpMenu->addAction(m_aboutAction);
  m_helpMenu->addAction(m_aboutQtAction);
}

void MainWindow::retranslateUi()
{
  setWindowTitle(programName());
  m_fileMenu->setTitle(tr("&File"));
  m_viewMenu->setTitle(tr("&View"));
  m_helpMenu->setTitle(tr("&Help"));
  m_quitAction->setText(tr("&Quit"));
  m_quitAction->setStatusTip(tr("Close the program"));
  m_resetViewAction->setText(tr("&Reset View"));
  m_resetViewAction->setStatusTip(tr("Fly back to the start view"));
  m_mapViewAction->setText(tr("2D &Map"));
  m_mapViewAction->setStatusTip(
      tr("Change between the 3D globe and a flat map"));
  m_projectionMenu->setTitle(tr("Map &Projection"));
  for (QAction* action : m_projectionGroup->actions()) {
    if (const Projection* projection =
            projectionById(action->data().toString())) {
      action->setText(projection->displayName());
      action->setStatusTip(tr("Draw the flat map in the %1 projection")
                               .arg(projection->displayName()));
    }
  }
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
  m_addWebTerrainAction->setText(tr("Add Web &Terrain…"));
  m_addWebTerrainAction->setStatusTip(
      tr("Add elevation from a web terrain tile service or a Cloud "
         "Optimized GeoTIFF on a server"));
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
  m_offlineRegionsAction->setText(tr("Offline &Regions…"));
  m_offlineRegionsAction->setStatusTip(
      tr("Keep areas of WMS layers on this computer for use without a "
         "network"));
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
  m_aboutAction->setText(tr("&About %1").arg(programName()));
  m_aboutAction->setStatusTip(tr("Show the program version and licence"));
  m_aboutQtAction->setText(tr("About &Qt"));
  m_aboutQtAction->setStatusTip(tr("Show the Qt version in use"));
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
}

void MainWindow::showPointerPosition(bool onGlobe, double latDeg, double lonDeg)
{
  m_pointerOnGlobe = onGlobe;
  m_pointerLatDeg = latDeg;
  m_pointerLonDeg = lonDeg;
  const QLocale locale;
  if (onGlobe) {
    QString text = tr("%1  %2").arg(formatLatitude(latDeg, locale),
                                    formatLongitude(lonDeg, locale));
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
  const LayerStack stack = m_layers->layerStack();
  if (!m_globeView) {
    // No OpenGL view: nothing is drawn, but layers that can't be opened are
    // still marked in the dock.
    watchLayerStatus(stack);
    return;
  }
  QStringList errors;
  m_globeView->setImageryDraws(
      imageryDrawsForStack(stack, *m_registry, &errors));
  m_globeView->setVectorDraws(vectorDrawsForStack(stack, *m_registry, &errors));
  m_globeView->setElevationDraw(
      elevationDrawForStack(stack, *m_registry, &errors));
  m_globeView->setAttributions(attributionsForStack(stack, *m_registry));
  if (!errors.isEmpty()) {
    statusBar()->showMessage(errors.join(QLatin1Char(' ')), 10000);
  }
  watchLayerStatus(stack);
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
    const int before =
        m_layers->data(index, LayerTreeModel::StatusRole).toInt();
    m_layers->setLayerStatus(layer.first, status);
    if (status.state == ProviderStatus::State::Error &&
        before != static_cast<int>(ProviderStatus::State::Error)) {
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
  // The WMS layers of the project, each source once, top first.
  std::vector<RegionLayerChoice> layers;
  const LayerStack stack = m_layers->layerStack();
  for (std::size_t i = stack.imagery.size(); i-- > 0;) {
    const LayerDefinition& definition = stack.imagery[i].definition;
    if (definition.source.value(QStringLiteral("kind")).toString() !=
        QLatin1String("wms")) {
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
        tr("Only layers from WMS servers can be kept for offline use. Add a "
           "WMS layer first (File → Add WMS Layer…)."));
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
    const long long id =
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
  if (!project || !m_layers->fromJson(project->layers, errorString)) {
    return false;
  }
  if (m_globeView) {
    // An unknown projection (a newer program's) keeps the current one.
    m_globeView->setMapProjection(projectionById(project->projection));
    m_globeView->setViewMode(project->mapView ? ViewMode::Map : ViewMode::Globe,
                             false);
    m_globeView->setPose(project->camera);
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
  }
  project.layers = m_layers->toJson();
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

void MainWindow::showAbout()
{
  const BuildInfo info = buildInfo();
  QMessageBox::about(
      this, tr("About %1").arg(programName()),
      tr("<h3>%1 %2</h3>"
         "<p>A 3D globe and 2D map viewer.</p>"
         "<p>Built with Qt %3 (running on Qt %4) and %5.</p>"
         "<p>This program is free software, licensed under the GNU General "
         "Public License, version 2 or later.</p>")
          .arg(programName(), info.version, info.qtCompileVersion,
               info.qtRuntimeVersion, info.compiler));
}

} // namespace ts
