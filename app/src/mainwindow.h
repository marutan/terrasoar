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

/**
 * @file
 * The application's main window.
 */
#ifndef TS_APP_MAINWINDOW_H
#define TS_APP_MAINWINDOW_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <QString>

#include "ts/layers/layerdefinition.h"
#include "ts/packs/packmanifest.h"
#include "ts/providers/tileprovider.h"
#include "ts/regions/regiongeometry.h"

class QAction;
class QActionGroup;
class QModelIndex;
class QEvent;
class QLabel;
class QMenu;
class QTimer;

namespace ts
{

class ElevationProfileDialog;
class FeatureInfoWindow;
struct AboutDataRow;

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
class ProgramClock;
class ProviderRegistry;
class TimeBar;
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
   * Returns the datasets for the About window: the base pack's and the
   * installed packs', with their sources, licences and attributions.
   */
  std::vector<AboutDataRow> aboutData() const;

  /**
   * Opens the Add WMS Layer dialog and adds what the user chooses. With
   * @p address, connects to it at once (tests).
   */
  void addWmsLayer(const QString& address = QString());

  /**
   * Opens the Add WMTS Layer dialog and adds what the user chooses. With
   * @p address, connects to it at once (tests).
   */
  void addWmtsLayer(const QString& address = QString());

  /** Opens the Add XYZ Tile Layer dialog and adds what the user chooses. */
  void addXyzLayer();

  /**
   * Opens the Add WFS Layer dialog and adds what the user chooses. With
   * @p address, connects to it at once (tests).
   */
  void addWfsLayer(const QString& address = QString());

  /**
   * Asks for a raster file (a GeoTIFF, an RPF A.TOC, …) and adds it as an
   * imagery layer.
   */
  void addImageryFile();

  /**
   * Adds the raster file @p path as a visible imagery layer at the top,
   * or returns false (and the reason in @p errorString) if it can't be
   * used.
   */
  bool addImageryLayer(const QString& path, QString* errorString = nullptr);

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
   * Opens the Add Web Terrain dialog and adds what the user chooses.
   */
  void addWebTerrain();

  /**
   * Adds the elevation layer @p layer, visible, above the other
   * elevation layers (so it takes priority over them), with the terrain's
   * current exaggeration.
   */
  void addElevationDefinition(const LayerDefinition& layer);

  /**
   * Shows the Elevation Profile window (not modal) and starts drawing a
   * line on the globe; returns the window (null without an OpenGL view).
   */
  ElevationProfileDialog* showElevationProfile();

  /**
   * Computes the profile along @p points (longitude, latitude) on a worker
   * thread and shows it in the Elevation Profile window (lines drawn on
   * the globe come here; tests call it directly).
   */
  void profileLine(const std::vector<LonLat>& points);

  /** Points of an elevation profile. */
  static constexpr int32_t profileSamples = 512;

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

  /** Returns the program's clock, which layers with times follow. */
  ProgramClock* clock() const { return m_clock; }

  /** Returns the time slider (for tests). */
  TimeBar* timeBar() const { return m_timeBar; }

  /**
   * Asks the visible WMS layers that can be queried what they have at
   * @p point (GetFeatureInfo, at the scale of @p degreesPerPixel) and shows
   * the answers in the What Is Here window. Returns the number of layers
   * asked; none if View → What Is Here on Click is off.
   */
  int32_t identifyAt(LonLat point, double degreesPerPixel);

  /** Returns the What Is Here window, or null before the first click. */
  FeatureInfoWindow* featureInfoWindow() const { return m_featureInfoWindow; }

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
  void clockMoved();
  LayerStack timedStack() const;
  void updateTimeBar(const LayerStack& stack);
  void applyImageryDraws(const LayerStack& stack, QStringList* errors);
  void updateLayerStatus();
  void syncViewActions();
  void showProjectionSettings();
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
  QAction* m_attributionsAction = nullptr;
  QAction* m_ellipsoidalHeightsAction = nullptr;
  QAction* m_hillshadeAction = nullptr;
  QAction* m_profileAction = nullptr;
  QPointer<ElevationProfileDialog> m_profileDialog;
  /** Cancels the profile being computed, if any. */
  std::shared_ptr<std::atomic<bool>> m_profileCancel;
  /** The line of the profile shown. */
  std::vector<LonLat> m_profileLine;
  QAction* m_addElevationAction = nullptr;
  QAction* m_addImageryAction = nullptr;
  QAction* m_addWebTerrainAction = nullptr;
  QMenu* m_projectionMenu = nullptr;
  QActionGroup* m_projectionGroup = nullptr;
  QAction* m_projectionSettingsAction = nullptr;
  QMenu* m_exaggerationMenu = nullptr;
  QActionGroup* m_exaggerationGroup = nullptr;
  QAction* m_openAction = nullptr;
  QAction* m_saveAction = nullptr;
  QAction* m_saveAsAction = nullptr;
  LayerTreeDock* m_layerDock = nullptr;
  QString m_projectPath;
  QAction* m_userGuideAction = nullptr;
  QAction* m_aboutAction = nullptr;
  QLabel* m_statusLabel = nullptr;
  QLabel* m_pointerLabel = nullptr;
  QLabel* m_eyeLabel = nullptr;
  LayerTreeModel* m_layers = nullptr;
  std::unique_ptr<ProviderRegistry> m_registry;
  QAction* m_addWmsAction = nullptr;
  QAction* m_addWmtsAction = nullptr;
  QAction* m_addXyzAction = nullptr;
  QAction* m_addWfsAction = nullptr;
  QAction* m_featureInfoAction = nullptr;
  FeatureInfoWindow* m_featureInfoWindow = nullptr;
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
  ProgramClock* m_clock = nullptr;
  TimeBar* m_timeBar = nullptr;
  /** The imagery sources last drawn, as drawn at the clock's time. */
  QJsonArray m_drawnImagery;
  /** True once the time slider was shown for a layer with times. */
  bool m_timeBarOffered = false;
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
