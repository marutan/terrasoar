// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The application's main window.
 */
#ifndef TS_APP_MAINWINDOW_H
#define TS_APP_MAINWINDOW_H

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <QString>

#include "ts/packs/packmanifest.h"
#include "ts/providers/tileprovider.h"

class QAction;
class QActionGroup;
class QModelIndex;
class QEvent;
class QLabel;
class QMenu;
class QTimer;

namespace ts
{

class CountryAreas;
class GlobeView;
class ImageryProvider;
class LayerTreeDock;
class LayerTreeModel;
class NewRegionDialog;
class OfflineRegionManager;
class OfflineRegionsDialog;
class PackManager;
class PackManagerDialog;
class ProviderRegistry;
struct LayerStack;

/**
 * The top-level window: the globe view, the layer tree dock, menus and
 * the status bar, which shows the position under the pointer and the eye
 * height. Projects (layer tree, camera and view) are opened and saved
 * from the File menu, which also opens the offline regions (areas of WMS
 * layers kept for use without a network). The View menu changes between the 3D
 * globe and the 2D map and picks the map's projection.
 *
 * All text is set in retranslateUi(), which runs again when the language
 * changes, so installing a translator at run time updates the window.
 *
 * @guithread
 */
class MainWindow : public QMainWindow
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(MainWindow)

public:
  /** Creates the window. @param parent Usual QWidget parent. */
  explicit MainWindow(QWidget* parent = nullptr);

  /** Destroys the window and its children. */
  ~MainWindow() override;

  /** Returns the layer tree. */
  LayerTreeModel* layerModel() const { return m_layers; }

  /**
   * Opens the project at @p path: replaces the layer tree and moves the
   * camera. Returns false and sets @p errorString on failure (the current
   * project stays).
   */
  bool openProject(const QString& path, QString* errorString);

  /**
   * Saves the layer tree and camera to @p path. Returns false and sets
   * @p errorString on failure.
   */
  bool saveProject(const QString& path, QString* errorString);

  /**
   * Opens the Add WMS Layer dialog and adds what the user chooses. With
   * @p address, connects to it at once (tests).
   */
  void addWmsLayer(const QString& address = QString());

  /**
   * Asks for a DEM file and what its heights are measured from, and adds
   * it as an elevation layer.
   */
  void addElevationFile();

  /**
   * Adds the DEM file @p path, its heights above @p datum ("egm2008",
   * "egm96" or "ellipsoid"), as a visible elevation layer above the
   * others, with the terrain's current exaggeration.
   */
  // The file, then its datum.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  void addElevationLayer(const QString& path, const QString& datum);

  /**
   * Shows the Offline Regions window (not modal) and returns it.
   */
  OfflineRegionsDialog* showOfflineRegions();

  /**
   * Opens the New Offline Region dialog (not modal) for the WMS layers in
   * the project and returns it; null (after telling the user) if there
   * are none. Accepting it adds the region and starts its download.
   */
  NewRegionDialog* newOfflineRegion();

  /** Returns the offline regions. */
  OfflineRegionManager& offlineRegions() const { return *m_offlineRegions; }

  /** Shows the Data Packs window (not modal) and returns it. */
  PackManagerDialog* showDataPacks();

  /** Returns the extra data packs. */
  PackManager& packs() const { return *m_packs; }

  /** Returns the globe view, or null without OpenGL (tests). */
  GlobeView* globeView() const { return m_globeView; }

  /**
   * Returns true while the globe view is loading tiles (false without
   * one).
   */
  bool isLoadingTiles() const;

  /** Returns the provider registry (tests). */
  ProviderRegistry& registry() const { return *m_registry; }

  /** Returns the path of the current project, empty if never saved. */
  const QString& projectPath() const { return m_projectPath; }

  /** What is known about the base pack. */
  enum class BasePackState {
    Missing,  ///< No base pack was found.
    Checking, ///< Found; its checksums are being verified.
    Ok,       ///< Found and intact.
    Damaged,  ///< Found, but a file is missing or changed.
  };

  /** Returns the base pack's state. */
  BasePackState basePackState() const { return m_basePackState; }

  /** Returns the base pack's directory, empty if there is none. */
  const QString& basePackDirectory() const { return m_basePackDirectory; }

  /**
   * Returns what the window shows, as JSON for `--measure`: the base
   * pack's state, the layers drawn (top first), layers whose source is
   * offline or failing, the attributions, the
   * view (globe or map, the projection, the scale bar's text) and the
   * last frame's tile counts.
   */
  QJsonObject stateReport() const;

signals:
  /** The base pack's checksums have been verified (see basePackState()). */
  void basePackVerified();

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

private:
  void createActions();
  QModelIndex topmostElevationLayer() const;
  void syncTerrainActions();
  void createMenus();
  void retranslateUi();
  void showAbout();
  void showPointerPosition(bool onGlobe, double latDeg, double lonDeg);
  void showEyeHeight(double eyeHeightM);
  void refreshLayers();
  void watchLayerStatus(const LayerStack& stack);
  void updateLayerStatus();
  void syncViewActions();
  void openProjectDialog();
  void saveProjectDialog(bool askForPath);
  void findBasePack();
  void pollBasePackCheck();
  void showBasePackResult(bool ok, const QString& problem);

  /** The globe, or null where the platform has no OpenGL. */
  GlobeView* m_globeView = nullptr;
  /** Shown instead of the globe without OpenGL. */
  QLabel* m_noGlobeLabel = nullptr;
  QMenu* m_fileMenu = nullptr;
  QMenu* m_viewMenu = nullptr;
  QMenu* m_helpMenu = nullptr;
  QAction* m_quitAction = nullptr;
  QAction* m_debugOverlayAction = nullptr;
  QAction* m_resetViewAction = nullptr;
  QAction* m_mapViewAction = nullptr;
  QAction* m_scaleBarAction = nullptr;
  QAction* m_ellipsoidalHeightsAction = nullptr;
  QAction* m_addElevationAction = nullptr;
  QMenu* m_projectionMenu = nullptr;
  QActionGroup* m_projectionGroup = nullptr;
  QMenu* m_exaggerationMenu = nullptr;
  QActionGroup* m_exaggerationGroup = nullptr;
  QAction* m_openAction = nullptr;
  QAction* m_saveAction = nullptr;
  QAction* m_saveAsAction = nullptr;
  LayerTreeDock* m_layerDock = nullptr;
  QString m_projectPath;
  QAction* m_aboutAction = nullptr;
  QAction* m_aboutQtAction = nullptr;
  QLabel* m_statusLabel = nullptr;
  QLabel* m_pointerLabel = nullptr;
  QLabel* m_eyeLabel = nullptr;
  LayerTreeModel* m_layers = nullptr;
  std::unique_ptr<ProviderRegistry> m_registry;
  QAction* m_addWmsAction = nullptr;
  QAction* m_offlineRegionsAction = nullptr;
  /** Destroyed before the registry. */
  std::unique_ptr<OfflineRegionManager> m_offlineRegions;
  QPointer<OfflineRegionsDialog> m_offlineRegionsDialog;
  QPointer<NewRegionDialog> m_newRegionDialog;
  QAction* m_dataPacksAction = nullptr;
  /** Destroyed before the registry. */
  std::unique_ptr<PackManager> m_packs;
  QPointer<PackManagerDialog> m_packsDialog;
  QLabel* m_downloadLabel = nullptr;
  /** The imagery layers' providers, by node id, for their status. */
  std::vector<std::pair<QString, std::shared_ptr<ImageryProvider>>>
      m_layerProviders;
  /** Layers whose source can't be opened, by node id, with the reason. */
  std::vector<std::pair<QString, QString>> m_brokenLayers;
  /** Passes status changes from loader threads to this window. */
  std::shared_ptr<QTimer> m_statusRelay;
  QString m_basePackDirectory;
  /** The base pack's country outlines, if it has them (read when needed). */
  QString m_countriesFile;
  std::shared_ptr<const CountryAreas> m_countries;
  BasePackState m_basePackState = BasePackState::Missing;
  QString m_basePackProblem;
  /** Verifies the base pack off the GUI thread; joined on destruction. */
  std::thread m_basePackCheck;
  std::atomic<bool> m_cancelBasePackCheck{false};
  /** The check's result, set by its thread, taken by pollBasePackCheck(). */
  std::optional<PackVerification> m_basePackResult;
  std::mutex m_basePackMutex;
  QTimer* m_basePackPoll = nullptr;
  bool m_pointerOnGlobe = false;
  double m_pointerLatDeg = 0.0;
  double m_pointerLonDeg = 0.0;
  double m_eyeHeightM = 0.0;
};

} // namespace ts

#endif // TS_APP_MAINWINDOW_H
