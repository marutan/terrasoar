// SPDX-License-Identifier: GPL-2.0-or-later
#include "mainwindow.h"

#include <memory>

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QFileDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QWidget>

#include "globeview.h"
#include "layertreedock.h"
#include "ts/base/buildinfo.h"
#include "ts/geo/geoformat.h"
#include "ts/globe/layerstackdraws.h"
#include "ts/layers/layertreemodel.h"
#include "ts/layers/projectfile.h"
#include "ts/providers/providerregistry.h"

namespace ts
{

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent), m_statusLabel(new QLabel(this)),
      m_pointerLabel(new QLabel(this)), m_eyeLabel(new QLabel(this)),
      m_layers(new LayerTreeModel(this)),
      m_registry(std::make_unique<ProviderRegistry>())
{
  setObjectName(QStringLiteral("mainWindow"));
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

  // A new project starts with the test imagery; real sources come with
  // the base pack (Phase 3).
  for (const LayerDefinition& definition : builtinLayerDefinitions()) {
    if (definition.id == QLatin1String("builtin:checkerboard")) {
      m_layers->addLayer(definition);
    }
  }
  m_layerDock = new LayerTreeDock(m_layers, this);
  addDockWidget(Qt::LeftDockWidgetArea, m_layerDock);
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

  createActions();
  createMenus();
  retranslateUi();

  resize(1024, 768);
}

MainWindow::~MainWindow() = default;

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

void MainWindow::createMenus()
{
  m_fileMenu = menuBar()->addMenu(QString());
  m_fileMenu->setObjectName(QStringLiteral("fileMenu"));
  m_fileMenu->addAction(m_openAction);
  m_fileMenu->addAction(m_saveAction);
  m_fileMenu->addAction(m_saveAsAction);
  m_fileMenu->addSeparator();
  m_fileMenu->addAction(m_quitAction);

  m_viewMenu = menuBar()->addMenu(QString());
  m_viewMenu->setObjectName(QStringLiteral("viewMenu"));
  m_viewMenu->addAction(m_resetViewAction);
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
  m_statusLabel->setText(tr("Ready"));
  if (m_noGlobeLabel) {
    m_noGlobeLabel->setText(
        tr("The globe needs OpenGL 3.3, which this display does not offer. "
           "Start %1 with --software-gl to use software rendering.")
            .arg(programName()));
  }
  showPointerPosition(m_pointerOnGlobe, m_pointerLatDeg, m_pointerLonDeg);
  showEyeHeight(m_eyeHeightM);
}

void MainWindow::showPointerPosition(bool onGlobe, double latDeg, double lonDeg)
{
  m_pointerOnGlobe = onGlobe;
  m_pointerLatDeg = latDeg;
  m_pointerLonDeg = lonDeg;
  const QLocale locale;
  m_pointerLabel->setText(
      onGlobe ? tr("%1  %2").arg(formatLatitude(latDeg, locale),
                                 formatLongitude(lonDeg, locale))
              : tr("Pointer not on the globe"));
}

void MainWindow::refreshLayers()
{
  if (!m_globeView) {
    return;
  }
  QStringList errors;
  m_globeView->setImageryDraws(
      imageryDrawsForStack(m_layers->layerStack(), *m_registry, &errors));
  if (!errors.isEmpty()) {
    statusBar()->showMessage(errors.join(QLatin1Char(' ')), 10000);
  }
}

bool MainWindow::openProject(const QString& path, QString* errorString)
{
  const std::optional<Project> project = loadProject(path, errorString);
  if (!project || !m_layers->fromJson(project->layers, errorString)) {
    return false;
  }
  if (m_globeView) {
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
