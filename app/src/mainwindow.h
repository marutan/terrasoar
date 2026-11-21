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
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <utility>
#include <vector>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QMainWindow>
#include <QPointer>
#include <QString>

#include "ts/camera/camera.h"
#include "ts/kml/kmledit.h"
#include "ts/layers/layerdefinition.h"
#include "ts/packs/packmanifest.h"
#include "ts/providers/tileprovider.h"
#include "ts/regions/regiongeometry.h"

class QAction;
class QActionGroup;
class QModelIndex;
class QDragEnterEvent;
class QDropEvent;
class QEvent;
class QLabel;
class QMenu;
class QTimer;

namespace ts
{

class BalloonWindow;
class ClockWidget;
class AircraftInfoWindow;
class AircraftLayers;
class AircraftListDialog;
class SatelliteInfoWindow;
class VesselInfoWindow;
class VesselLayers;
class VesselListDialog;
class SatelliteLayers;
class SatelliteListDialog;
class IconCache;
class KmlLinks;
struct KmlViewParameters;
struct KmlBillboard;
class ElevationProfileDialog;
class FeatureInfoWindow;
struct AboutDataRow;

class CountryAreas;
struct BillboardDraw;
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
   * Opens @p path: a KML or KMZ file (openKml()), else a project. Returns
   * false and sets @p errorString if it can't be read.
   */
  bool openFile(const QString& path, QString* errorString);

  /**
   * Adds the KML or KMZ file at @p path to the layer tree: a collection
   * for its document with one for each folder, the placemarks of each in
   * a vector layer, visibility as the file has it; flies to the
   * document's view if it has one. Returns false and sets @p errorString
   * if it can't be read.
   */
  bool openKml(const QString& path, QString* errorString);

  /** Returns the NetworkLinks of the layer tree. */
  KmlLinks* kmlLinks() const { return m_kmlLinks; }

  /**
   * Checks the Regions of the KML in the layer tree against the view now
   * (it is done when the view stops moving): layers out of sight are left
   * out, links that come into sight are fetched.
   */
  void updateKmlRegions();

  /**
   * Returns the KML of the layer tree's entry @p nodeId as it would be
   * saved: the file read again with its text kept, the whole document for
   * its top entry, else a document holding just that folder, overlay or
   * link. Nothing (with @p errorString) if it isn't KML or can't be read.
   */
  std::optional<KmlDocument> kmlNodeDocument(const QString& nodeId,
                                             QString* errorString) const;

  /**
   * Starts a new KML document to draw in, at the top of the layer tree;
   * returns its key (KmlEditDocument::key()).
   */
  QString newKmlDocument();

  /**
   * Makes the KML document holding the layer tree's entry @p nodeId
   * editable (read again with its text kept); returns its key, empty if
   * it isn't KML or can't be read. One already editable keeps its key.
   */
  QString editKmlDocument(const QString& nodeId);

  /** Returns the document being edited with @p key, or null. */
  KmlEditDocument* kmlEditDocument(const QString& key) const;

  /**
   * Returns the key of the document edits go to: the selected entry's, or
   * the last one edited; empty if none.
   */
  QString activeKmlDocument() const;

  /**
   * Selects the feature at @p path of the document @p key (its handles
   * drawn to drag); an empty @p key selects nothing.
   */
  void selectKmlFeature(const QString& key, const QString& path);

  /** Returns the path of the selected feature; nothing if none. */
  std::optional<QString> selectedKmlFeature() const;

  /** Adds a placemark at @p at to the active document (as drawing does). */
  bool addKmlPlacemark(LonLat at);

  /** Adds a path through @p points to the active document. */
  bool addKmlPath(const std::vector<LonLat>& points);

  /** Adds a polygon with the corners @p corners to the active document. */
  bool addKmlPolygon(const std::vector<LonLat>& corners);

  /** Adds a hole with the corners @p corners to the selected polygon. */
  bool addKmlHole(const std::vector<LonLat>& corners);

  /** Adds the picture @p image over @p area to the active document. */
  bool addKmlGroundOverlay(const QString& image, const RegionGeometry& area);

  /** Adds a folder to the active document. */
  bool addKmlFolder();

  /**
   * Gives the feature at @p path of document @p key the properties
   * @p feature, and the shared style @p shared its values (as the
   * Properties dialog does).
   */
  bool setKmlFeature(const QString& key, const QString& path,
                     const KmlFeature& feature,
                     const std::optional<KmlStyle>& shared);

  /** Undoes the active document's last change. */
  bool undoKml();

  /** Redoes the active document's last undone change. */
  bool redoKml();

  /** Copies the selected feature (and to the clipboard as KML). */
  bool copyKml();

  /** Copies and deletes the selected feature. */
  bool cutKml();

  /** Pastes what was copied into the active document. */
  bool pasteKml();

  /** Deletes the selected feature. */
  bool deleteKml();

  /**
   * Saves the document @p key to @p path (KML or KMZ), noting it saved
   * there; false (with a message box) if it can't be.
   */
  bool saveKmlDocument(const QString& key, const QString& path);

  /** Returns the KML menu. */
  QMenu* kmlMenu() const { return m_kmlMenu; }

  /** Returns the KML nodes out of their Regions' sight (for tests). */
  QStringList kmlHiddenNodes() const
  {
    return QStringList(m_kmlInactive.begin(), m_kmlInactive.end());
  }

  /** Returns the balloon window, or null before the first balloon. */
  BalloonWindow* balloonWindow() const { return m_balloon; }

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

  /** Returns the clock in the status bar. */
  ClockWidget* clockWidget() const { return m_clockWidget; }

  /** Returns the satellites of the layer tree. */
  SatelliteLayers* satellites() const { return m_satellites; }

  /** Opens the Add Satellites dialog and adds what the user chooses. */
  void addSatellites();

  /**
   * Adds the satellite layer @p definition, visible, at the top of the
   * layer tree; returns its node id.
   */
  QString addSatelliteLayer(const LayerDefinition& definition);

  /**
   * Adds the TLE or OMM file @p path as a satellite layer; false (with
   * @p errorString) if it has no element sets.
   */
  bool addSatelliteFile(const QString& path, QString* errorString);

  /**
   * Shows the list of the satellites of layer @p nodeId (not modal) and
   * returns it; null if it isn't a satellite layer.
   */
  SatelliteListDialog* showSatelliteList(const QString& nodeId);

  /** Selects satellite @p catalogueNumber of layer @p nodeId, its details
   * shown. */
  void selectSatellite(const QString& nodeId, int32_t catalogueNumber);

  /** Downloads (or reads) layer @p nodeId's element sets again. */
  void refreshSatellites(const QString& nodeId);

  /** Returns the satellite details window, or null before the first. */
  SatelliteInfoWindow* satelliteInfoWindow() const { return m_satelliteInfo; }

  /** Returns the aircraft of the layer tree. */
  AircraftLayers* aircraft() const { return m_aircraft; }

  /** Opens the Import ADS-B Data dialog (for @p path, if given). */
  void importAdsbData(const QString& path = QString());

  /** Opens the Connect to ADS-B Receiver dialog. */
  void connectAdsbReceiver();

  /**
   * Adds the aircraft layer @p definition, visible, at the top of the
   * layer tree; returns its node id.
   */
  QString addAircraftLayer(const LayerDefinition& definition);

  /**
   * Adds the recording @p path as an aircraft layer if its format can be
   * told and read without asking (SBS, timed raw messages, readsb JSON,
   * traces); otherwise opens the import dialog for it. False (with
   * @p errorString) if it isn't ADS-B data.
   */
  bool openAdsbFile(const QString& path, QString* errorString);

  /**
   * Shows the list of the aircraft of layer @p nodeId (not modal) and
   * returns it; null if it isn't an aircraft layer.
   */
  AircraftListDialog* showAircraftList(const QString& nodeId);

  /** Selects aircraft @p address of layer @p nodeId, its details shown. */
  void selectAircraft(const QString& nodeId, std::optional<uint32_t> address);

  /** Returns the aircraft details window, or null before the first. */
  AircraftInfoWindow* aircraftInfoWindow() const { return m_aircraftInfo; }

  /** Makes the camera follow the selected aircraft (or stop). */
  void setFollowingAircraft(bool follow);

  /** Returns true while the camera follows the selected aircraft. */
  bool isFollowingAircraft() const { return m_followAircraft; }

  /** Returns the ships of the layer tree. */
  VesselLayers* vessels() const { return m_vessels; }

  /** Opens the Import AIS Data dialog (for @p path, if given). */
  void importAisData(const QString& path = QString());

  /** Opens the Add AIS Source dialog (a receiver or an open feed). */
  void addAisSource();

  /** Adds the ship layer @p definition; returns its node id. */
  QString addVesselLayer(const LayerDefinition& definition);

  /**
   * Adds the AIS recording @p path as a ship layer if it can be read
   * without asking (NMEA with times, CSV with NOAA's columns); otherwise
   * opens the import dialog. False (with @p errorString) if it isn't AIS.
   */
  bool openAisFile(const QString& path, QString* errorString);

  /** Shows the list of layer @p nodeId's stations; null if not ships. */
  VesselListDialog* showVesselList(const QString& nodeId);

  /** Selects station @p mmsi of layer @p nodeId, its details shown. */
  void selectVessel(const QString& nodeId, std::optional<uint32_t> mmsi);

  /** Returns the ship details window, or null before the first. */
  VesselInfoWindow* vesselInfoWindow() const { return m_vesselInfo; }

  /** Makes the camera follow the selected station (or stop). */
  void setFollowingVessel(bool follow);

  /** Returns true while the camera follows the selected station. */
  bool isFollowingVessel() const { return m_followVessel; }

  /** Opens layer @p nodeId's ship properties (modal). */
  void showVesselProperties(const QString& nodeId);

  /**
   * Exports station @p mmsi of layer @p nodeId's track to @p path (KML,
   * GPX or CSV by its suffix); false with @p errorString if not.
   */
  bool exportVesselTrack(const QString& nodeId, uint32_t mmsi,
                         const QString& path, QString* errorString);

  /** Opens layer @p nodeId's aircraft properties (modal). */
  void showAircraftProperties(const QString& nodeId);

  /**
   * Exports aircraft @p address of layer @p nodeId's track to @p path
   * (KML, GPX or CSV by its suffix); false with @p errorString if not.
   */
  bool exportAircraftTrack(const QString& nodeId, uint32_t address,
                           const QString& path, QString* errorString);

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
  /** Accepts KML, KMZ and project files dragged onto the window. */
  void dragEnterEvent(QDragEnterEvent* event) override;
  /** Opens the KML, KMZ or project files dropped on the window. */
  void dropEvent(QDropEvent* event) override;

private:
  void createActions();
  QModelIndex topmostElevationLayer() const;
  void syncTerrainActions();
  void createMenus();
  void createKmlEditing();
  void retranslateKmlEditing();
  void updateKmlActions();
  void refreshKmlDocument(const QString& key);
  QString kmlKeyOf(const QString& nodeId) const;
  QString kmlTargetFolder(const QString& key) const;
  bool addKmlFeatureToActive(const QString& description, KmlFeature feature);
  bool kmlClicked(LonLat point, double degreesPerPixel);
  void startKmlDrawing(int32_t kind);
  void kmlLineDrawn(const std::vector<LonLat>& points);
  void kmlRegionDrawn(const RegionGeometry& region);
  void kmlHandleMoved(int32_t index, LonLat to);
  void kmlHandleDeleted(int32_t index);
  void showKmlHandles();
  void showKmlProperties();
  bool saveKmlActive(bool as);
  void kmlNodesMoved();
  QJsonArray kmlProjectEntries(bool* cancelled);
  void openKmlProjectEntries(const QJsonArray& entries);
  QJsonArray kmlProjectLayers(const QJsonArray& entries) const;
  void syncKmlEntry(const QModelIndex& index);
  std::optional<KmlDocument> kmlToPaste() const;
  std::shared_ptr<const KmlDocument>
  kmlProjectDocument(const QJsonObject& entry, QString* path);
  void fillKmlEntry(const QModelIndex& node, const KmlDocument& document);
  QJsonObject withKmlRevision(QJsonObject source) const;
  void retranslateUi();
  void showAbout();
  void showPointerPosition(bool onGlobe, double latDeg, double lonDeg);
  void showEyeHeight(double eyeHeightM);
  void refreshLayers();
  void watchLayerStatus(const LayerStack& stack);
  void clockMoved();
  void createSatellites();
  void refreshSatelliteLayers(const LayerStack& stack);
  void satellitesDrawn();
  void satelliteSetLoaded(const QJsonObject& source);
  void satelliteMarkerClicked(int32_t index);
  void updateSatelliteInfo();
  void satellitePointerSettled();
  std::vector<QAction*> satelliteNodeActions(const QString& id,
                                             QObject* parent);
  void showSatelliteProperties(const QString& nodeId);
  void createShips();
  void refreshVesselLayers(const LayerStack& stack);
  void vesselsDrawn();
  void vesselTrailsChanged();
  void vesselSetChanged(const QJsonObject& source);
  void vesselMarkerClicked(int32_t index);
  void updateVesselInfo();
  std::vector<QAction*> vesselNodeActions(const QString& id, QObject* parent);
  void createAircraft();
  void refreshAircraftLayers(const LayerStack& stack);
  void aircraftDrawn();
  void aircraftTrailsChanged();
  void aircraftSetChanged(const QJsonObject& source);
  void aircraftMarkerClicked(int32_t index);
  void updateAircraftInfo();
  std::vector<QAction*> aircraftNodeActions(const QString& id, QObject* parent);
  void openKmlDialog();
  void addKmlFeature(const KmlDocument& document, const KmlFeature& container,
                     const QString& featurePath, const QModelIndex& parent,
                     int32_t row);
  std::vector<QAction*> nodeActions(const QModelIndex& index, QObject* parent);
  bool showBalloonAt(LonLat point, double degreesPerPixel);
  void refreshBillboards(const LayerStack& stack);
  BillboardDraw billboardDraw(const KmlBillboard& billboard);
  QImage tintedIcon(const QString& where, const QColor& tint,
                    bool pushpin = true);
  std::shared_ptr<const KmlDocument> readKmlInBackground(const QString& path,
                                                         QString* errorString);
  void addKmlChildren(const KmlDocument& document, const KmlFeature& container,
                      const QString& featurePath,
                      const QModelIndex& collection);
  void addKmlPlaces(const KmlDocument& document, const QString& featurePath,
                    const QModelIndex& collection, int32_t row, bool deep);
  void addKmlLink(const KmlDocument& document, const KmlFeature& link,
                  const QString& featurePath, const QModelIndex& parent,
                  int32_t row);
  KmlViewParameters currentViewParameters() const;
  void kmlLinkLoaded(const QString& nodeId, const QString& path,
                     bool flyToView);
  void forgetRemovedKmlNodes();
  void kmlCameraMoved();
  void kmlCheckChanged(const QModelIndex& topLeft);
  const std::vector<QDateTime>& kmlBoundaries(const QJsonObject& source) const;
  std::vector<QAction*> flyThereActions(const QString& id, QObject* parent);
  std::optional<CameraPose> kmlNodePose(const QString& id) const;
  void addKmlOverlay(const KmlDocument& document, const KmlFeature& overlay,
                     const QString& featurePath, const QModelIndex& parent,
                     int32_t row);
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
  QAction* m_openKmlAction = nullptr;
  BalloonWindow* m_balloon = nullptr;
  /** The KML documents being edited, by key. */
  std::map<QString, std::unique_ptr<KmlEditDocument>> m_kmlEdits;
  /** Each KML document's entry in the layer tree: node id → path. */
  std::map<QString, QString> m_kmlRoots;
  /** The document edited last (its key). */
  QString m_kmlActive;
  /** The selected feature: document key and path. */
  std::optional<std::pair<QString, QString>> m_kmlSelection;
  /** The handles shown for it. */
  std::vector<KmlHandle> m_kmlHandles;
  /** What is being drawn for KML (KmlDrawing in mainwindowkml.cpp). */
  int32_t m_kmlDrawing = 0;
  /** The picture of the ground overlay being drawn. */
  QString m_kmlOverlayImage;
  /** What was copied: a document holding it and the styles it uses. */
  std::optional<KmlDocument> m_kmlClipboard;
  QMenu* m_kmlMenu = nullptr;
  QAction* m_newKmlAction = nullptr;
  QAction* m_editKmlAction = nullptr;
  QAction* m_addPlacemarkAction = nullptr;
  QAction* m_addPathAction = nullptr;
  QAction* m_addPolygonAction = nullptr;
  QAction* m_addHoleAction = nullptr;
  QAction* m_addOverlayAction = nullptr;
  QAction* m_addFolderAction = nullptr;
  QAction* m_kmlPropertiesAction = nullptr;
  QAction* m_kmlCutAction = nullptr;
  QAction* m_kmlCopyAction = nullptr;
  QAction* m_kmlPasteAction = nullptr;
  QAction* m_kmlDeleteAction = nullptr;
  QAction* m_kmlUndoAction = nullptr;
  QAction* m_kmlRedoAction = nullptr;
  QAction* m_kmlSaveAction = nullptr;
  QAction* m_kmlSaveAsAction = nullptr;
  IconCache* m_icons = nullptr;
  KmlLinks* m_kmlLinks = nullptr;
  /** Waits for the view to stop before checking KML Regions. */
  QTimer* m_kmlViewTimer = nullptr;
  std::optional<CameraPose> m_lastKmlPose;
  /** KML layers out of their Regions' sight: left out of the stack. */
  std::set<QString> m_kmlInactive;
  /** KML radio folders (ListStyle radioFolder) by node id. */
  std::set<QString> m_kmlRadio;
  /** Set while the main window changes KML nodes itself. */
  bool m_refreshingKml = false;
  /** The vector sources drawn (KML's change with the clock). */
  QJsonArray m_drawnVectors;
  /** Each KML layer's time boundaries, by path, container and depth. */
  mutable std::map<QString, std::vector<QDateTime>> m_kmlBoundaries;
  /** Icons tinted by their IconStyle colour: address and colour → image. */
  std::map<std::pair<QString, QRgb>, QImage> m_tintedIcons;
  /** KML nodes of the layer tree: node id → file and feature path. */
  std::map<QString, std::pair<QString, QString>> m_kmlNodes;
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
  ClockWidget* m_clockWidget = nullptr;
  QMenu* m_clockMenu = nullptr;
  SatelliteLayers* m_satellites = nullptr;
  SatelliteInfoWindow* m_satelliteInfo = nullptr;
  QPointer<SatelliteListDialog> m_satelliteList;
  QAction* m_addSatellitesAction = nullptr;
  /** Waits for the pointer to stop before finding passes under it. */
  QTimer* m_satellitePointerTimer = nullptr;
  /** Old elements already pointed out, by source. */
  std::set<QString> m_oldSatelliteNotes;
  VesselLayers* m_vessels = nullptr;
  VesselInfoWindow* m_vesselInfo = nullptr;
  QPointer<VesselListDialog> m_vesselList;
  QAction* m_importAisAction = nullptr;
  QAction* m_addAisSourceAction = nullptr;
  bool m_followVessel = false;
  /** Ship recordings whose start the clock was moved to, by source. */
  std::set<QString> m_vesselsStarted;
  AircraftLayers* m_aircraft = nullptr;
  AircraftInfoWindow* m_aircraftInfo = nullptr;
  QPointer<AircraftListDialog> m_aircraftList;
  QAction* m_importAdsbAction = nullptr;
  QAction* m_connectReceiverAction = nullptr;
  bool m_followAircraft = false;
  /** Recordings whose start the clock was moved to, by source. */
  std::set<QString> m_aircraftStarted;
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
