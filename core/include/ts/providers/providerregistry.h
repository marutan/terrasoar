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

/**
 * Creates the imagery provider a layer source describes ("kind":
 * "checkerboard" or "solid"), or returns null and sets @p errorString.
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

private:
  mutable std::mutex m_mutex;
  std::unordered_map<QString, std::weak_ptr<ImageryProvider>> m_imagery;
};

} // namespace ts

#endif // TS_PROVIDERS_PROVIDERREGISTRY_H
