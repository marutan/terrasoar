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

class FeatureProvider;
class ImageryProvider;
class NetworkClient;
class PackManifest;
struct InstalledPack;
struct WmsProviderSettings;

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
                     const WmsProviderSettings& settings);

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

  mutable std::mutex m_mutex;
  std::unordered_map<QString, std::weak_ptr<ImageryProvider>> m_imagery;
  QString m_basePackDirectory;
  /** Dataset id → file name, of the base pack's imagery datasets. */
  std::unordered_map<QString, QString> m_basePackImagery;
  /** Dataset id → file name, of the base pack's features datasets. */
  std::unordered_map<QString, QString> m_basePackFeatures;
  /** Dataset id → attribution. */
  std::unordered_map<QString, QString> m_basePackAttribution;
  std::unordered_map<QString, std::weak_ptr<FeatureProvider>> m_features;
  /** A dataset of an installed pack in place of a base pack one. */
  struct Replacement {
    QString path;
    bool features = false;
    QString attribution;
  };
  /** Base pack dataset id → what replaces it. */
  std::unordered_map<QString, Replacement> m_replacements;
  std::shared_ptr<NetworkClient> m_network;
  std::unique_ptr<WmsProviderSettings> m_wmsSettings;
};

} // namespace ts

#endif // TS_PROVIDERS_PROVIDERREGISTRY_H
