// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Creates providers from layer sources and shares them.
 */
#ifndef TS_PROVIDERS_PROVIDERREGISTRY_H
#define TS_PROVIDERS_PROVIDERREGISTRY_H

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <QJsonObject>
#include <QString>

namespace ts
{

class ElevationProvider;
class FeatureProvider;
class GeoidModel;
class ImageryProvider;
class NetworkClient;
class PackManifest;
struct InstalledPack;
struct WebTileSettings;

/**
 * Creates the imagery provider a layer source describes ("kind":
 * "checkerboard" or "solid"), or returns null and sets @p errorString.
 * Sources in a pack ("kind": "basepack") need a ProviderRegistry that
 * knows the pack.
 *
 * @threadsafe
 */
std::shared_ptr<ImageryProvider>
createImageryProvider(const QJsonObject& source, QString* errorString);

/** A vertical datum elevation sources can be measured from. */
struct VerticalDatum {
  /** Its id in layer sources ("egm2008", "egm96", "osgm15", "ellipsoid"). */
  QString id;
  /** A name for the user from the pack that brings it; empty for the
   *  built-in ones (the application names those). */
  QString name;
};

/**
 * Hands out one provider per source: layers whose sources are equal get
 * the same provider object, so they also share its loader and caches.
 *
 * Holds the providers weakly; a provider goes when the last layer using
 * it does.
 *
 * @threadsafe
 */
class ProviderRegistry
{
public:
  /**
   * Returns the provider for @p source, creating it if no live one
   * exists, or null (and @p errorString) if it can't be created.
   */
  std::shared_ptr<ImageryProvider> imagery(const QJsonObject& source,
                                           QString* errorString);

  /** Returns the number of providers still in use. */
  int liveImageryProviders() const;

  /**
   * Returns the feature provider for @p source (`{"kind": "basepack",
   * "dataset": "<id>"}` for a features dataset of the base pack), shared
   * like imagery(), or null and @p errorString.
   */
  std::shared_ptr<FeatureProvider> features(const QJsonObject& source,
                                            QString* errorString);

  /**
   * Returns the elevation provider for @p source, with the geoid model of
   * its vertical datum, shared like imagery(), or null and @p errorString:
   * - `{"kind": "basepack", "dataset": "<id>"}`: an elevation dataset of
   *   the base pack;
   * - `{"kind": "file", "path": "<raster>", "verticalDatum": "<datum>"}`:
   *   a DEM file GDAL reads (GdalElevationProvider), its heights above
   *   `"egm2008"`, `"egm96"` (the base pack's geoids) or `"ellipsoid"`;
   *   a path of `/vsicurl/<url>` reads a Cloud Optimized GeoTIFF over
   *   HTTP;
   * - `{"kind": "web", "url": …, "encoding": …}`: web terrain tiles
   *   (WebTerrainSource), through the WMS network and disk cache settings
   *   (setWmsNetwork()).
   */
  std::shared_ptr<ElevationProvider> elevation(const QJsonObject& source,
                                               QString* errorString);

  /**
   * Returns @p sources (most important first) as one provider
   * (CompositeElevationProvider), shared: the same sources give the same
   * provider while it is in use, so the renderer keeps its heights.
   */
  std::shared_ptr<ElevationProvider>
  elevationComposite(std::vector<std::shared_ptr<ElevationProvider>> sources);

  /**
   * Returns the geoid model of the base pack's geoid dataset @p dataset
   * (e.g. "egm2008"), shared, or null and @p errorString.
   */
  std::shared_ptr<const GeoidModel> geoid(const QString& dataset,
                                          QString* errorString);

  /**
   * Returns the vertical datums elevation sources can be measured from
   * now: "egm2008" and "egm96" (the base pack's, or a pack's finer
   * grids), the datums of installed geoid packs (e.g. "osgm15", with the
   * pack's name for it), then "ellipsoid".
   */
  std::vector<VerticalDatum> verticalDatums() const;

  /**
   * Returns the attribution to show while a layer with @p source is on
   * screen (from the pack's manifest), or an empty string.
   */
  QString attribution(const QJsonObject& source) const;

  /**
   * Makes the datasets of the base pack in @p directory available to
   * sources `{"kind": "basepack", "dataset": "<id>"}`. An empty
   * @p directory means there is no base pack: such sources then fail.
   */
  void setBasePack(const QString& directory, const PackManifest& manifest);

  /** Returns the base pack's directory, empty if there is none. */
  QString basePackDirectory() const;

  /**
   * Makes the datasets of @p packs that replace a base pack dataset
   * (PackDataset::replaces) take its place for sources `{"kind":
   * "basepack", "dataset": "<id>"}`, with their own attribution. Damaged
   * packs are left out; if two packs replace one dataset, the first in
   * @p packs wins. Replaces the previous set; affects providers handed out
   * afterwards (refresh the layers). See docs/design/packs.md.
   */
  void setInstalledPacks(const std::vector<InstalledPack>& packs);

  /**
   * Sets the network client and settings for WMS sources (`{"kind":
   * "wms", …}`, see WmsSource). A null @p network is created with default
   * settings when the first WMS source needs it (so that a program without
   * WMS layers has no network thread). Without this call there is also no
   * disk cache. Affects providers created afterwards.
   */
  void setWmsNetwork(std::shared_ptr<NetworkClient> network,
                     const WebTileSettings& settings);

  /** Returns the network client for WMS sources (creating it if needed). */
  std::shared_ptr<NetworkClient> network();

  /** Constructs an empty registry. */
  ProviderRegistry();

  /** Destroys the registry; providers still in use live on. */
  ~ProviderRegistry();

  ProviderRegistry(const ProviderRegistry&) = delete;
  ProviderRegistry& operator=(const ProviderRegistry&) = delete;
  ProviderRegistry(ProviderRegistry&&) = delete;
  ProviderRegistry& operator=(ProviderRegistry&&) = delete;

private:
  std::shared_ptr<ImageryProvider>
  createBasePackImagery(const QJsonObject& source, QString* errorString) const;
  QString basePackFile(const QJsonObject& source, bool features,
                       QString* errorString) const;
  std::shared_ptr<ImageryProvider> createWmsImagery(const QJsonObject& source,
                                                    QString* errorString);
  std::shared_ptr<NetworkClient> networkLocked();
  std::shared_ptr<const GeoidModel> geoidLocked(const QString& dataset,
                                                QString* errorString);
  std::shared_ptr<ElevationProvider>
  basePackElevationLocked(const QJsonObject& source, QString* errorString);
  std::shared_ptr<ElevationProvider>
  fileElevationLocked(const QJsonObject& source, QString* errorString);
  std::shared_ptr<ElevationProvider>
  webElevationLocked(const QJsonObject& source, QString* errorString);
  /**
   * Sets @p geoid to the model of @p datum (null for "ellipsoid"); false
   * and @p errorString for an unknown datum or a missing grid.
   */
  bool datumGeoidLocked(const QString& datum,
                        std::shared_ptr<const GeoidModel>* geoid,
                        const QJsonObject& source, QString* errorString);

  mutable std::mutex m_mutex;
  std::unordered_map<QString, std::weak_ptr<ImageryProvider>> m_imagery;
  QString m_basePackDirectory;
  /** Dataset id → file name, of the base pack's imagery datasets. */
  std::unordered_map<QString, QString> m_basePackImagery;
  /** Dataset id → file name, of the base pack's features datasets. */
  std::unordered_map<QString, QString> m_basePackFeatures;
  /** Dataset id → file name, of the base pack's elevation datasets. */
  std::unordered_map<QString, QString> m_basePackElevation;
  /** Dataset id → file name, of the base pack's geoid grids. */
  std::unordered_map<QString, QString> m_basePackGeoids;
  std::unordered_map<QString, std::weak_ptr<ElevationProvider>> m_elevation;
  /** Composite id → composite. */
  std::unordered_map<QString, std::weak_ptr<ElevationProvider>> m_composites;
  /** Geoid models stay loaded once read (a few MB each). */
  std::unordered_map<QString, std::shared_ptr<const GeoidModel>> m_geoids;
  /** Dataset id → attribution. */
  std::unordered_map<QString, QString> m_basePackAttribution;
  std::unordered_map<QString, std::weak_ptr<FeatureProvider>> m_features;
  /** A dataset of an installed pack in place of a base pack one. */
  struct Replacement {
    QString path;
    bool features = false;
    QString attribution;
  };
  /** A geoid grid of an installed pack. */
  struct PackGeoid {
    QString directory;
    QString file;
    QString name;
    QString attribution;
  };
  /** Datum id → an installed pack's grid for it. */
  std::unordered_map<QString, PackGeoid> m_packGeoids;
  /** Base pack dataset id → what replaces it. */
  std::unordered_map<QString, Replacement> m_replacements;
  std::shared_ptr<NetworkClient> m_network;
  std::unique_ptr<WebTileSettings> m_wmsSettings;
};

} // namespace ts

#endif // TS_PROVIDERS_PROVIDERREGISTRY_H
