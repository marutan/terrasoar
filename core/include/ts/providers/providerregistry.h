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

#include <QJsonObject>
#include <QString>

namespace ts
{

class ImageryProvider;
class PackManifest;

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
   * Makes the datasets of the base pack in @p directory available to
   * sources `{"kind": "basepack", "dataset": "<id>"}`. An empty
   * @p directory means there is no base pack: such sources then fail.
   */
  void setBasePack(const QString& directory, const PackManifest& manifest);

  /** Returns the base pack's directory, empty if there is none. */
  QString basePackDirectory() const;

private:
  std::shared_ptr<ImageryProvider>
  createBasePackImagery(const QJsonObject& source, QString* errorString) const;

  mutable std::mutex m_mutex;
  std::unordered_map<QString, std::weak_ptr<ImageryProvider>> m_imagery;
  QString m_basePackDirectory;
  /** Dataset id → file name, of the base pack's imagery datasets. */
  std::unordered_map<QString, QString> m_basePackImagery;
};

} // namespace ts

#endif // TS_PROVIDERS_PROVIDERREGISTRY_H
