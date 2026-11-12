// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/providers/providerregistry.h"

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>

#include "ts/network/networkclient.h"
#include "ts/packs/packlibrary.h"
#include "ts/packs/packmanifest.h"
#include "ts/providers/geopackagefeatureprovider.h"
#include "ts/providers/sqlitetileimageryprovider.h"
#include "ts/providers/testproviders.h"
#include "ts/tiling/tilekey.h"
#include "ts/wms/wmsimageryprovider.h"
#include "ts/wms/wmssource.h"

namespace ts
{

namespace
{

/** The source as compact JSON with sorted keys: equal sources, equal keys. */
QString canonical(const QJsonObject& source)
{
  return QString::fromUtf8(
      QJsonDocument(source).toJson(QJsonDocument::Compact));
}

int intMember(const QJsonObject& source, const char* name, int fallback)
{
  return source.value(QLatin1String(name)).toInt(fallback);
}

} // namespace

std::shared_ptr<ImageryProvider>
createImageryProvider(const QJsonObject& source, QString* errorString)
{
  const QString kind = source.value(QStringLiteral("kind")).toString();
  const int maximumLevel =
      intMember(source, "maximumLevel", TileKey::maximumLevel);
  if (kind == QLatin1String("checkerboard")) {
    return std::make_shared<CheckerboardImageryProvider>(
        intMember(source, "tileSize", 256), maximumLevel);
  }
  if (kind == QLatin1String("solid")) {
    const QColor color(source.value(QStringLiteral("color")).toString());
    if (color.isValid()) {
      return std::make_shared<SolidColorImageryProvider>(
          color, intMember(source, "tileSize", 64), maximumLevel);
    }
  }
  if (errorString) {
    *errorString =
        QCoreApplication::translate("ts::ProviderRegistry",
                                    "Unknown or invalid imagery source: %1")
            .arg(canonical(source));
  }
  return nullptr;
}

namespace
{

bool isBasePackSource(const QJsonObject& source)
{
  return source.value(QStringLiteral("kind")).toString() ==
         QLatin1String("basepack");
}

} // namespace

QString ProviderRegistry::basePackFile(const QJsonObject& source, bool features,
                                       QString* errorString) const
{
  const QString dataset = source.value(QStringLiteral("dataset")).toString();
  const std::unordered_map<QString, Replacement>::const_iterator replacement =
      m_replacements.find(dataset);
  if (replacement != m_replacements.end() &&
      replacement->second.features == features) {
    return replacement->second.path;
  }
  const std::unordered_map<QString, QString>& files =
      features ? m_basePackFeatures : m_basePackImagery;
  const std::unordered_map<QString, QString>::const_iterator file =
      files.find(dataset);
  if (m_basePackDirectory.isEmpty() || file == files.end()) {
    if (errorString) {
      *errorString = QCoreApplication::translate(
                         "ts::ProviderRegistry",
                         "The base pack, which holds \"%1\", is not installed.")
                         .arg(dataset);
    }
    return QString();
  }
  return QDir(m_basePackDirectory).filePath(file->second);
}

ProviderRegistry::ProviderRegistry()
    : m_wmsSettings(std::make_unique<WmsProviderSettings>())
{
}

ProviderRegistry::~ProviderRegistry() = default;

void ProviderRegistry::setWmsNetwork(std::shared_ptr<NetworkClient> network,
                                     const WmsProviderSettings& settings)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  m_network = std::move(network);
  *m_wmsSettings = settings;
}

std::shared_ptr<NetworkClient> ProviderRegistry::network()
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  return networkLocked();
}

std::shared_ptr<NetworkClient> ProviderRegistry::networkLocked()
{
  if (!m_network) {
    m_network = std::make_shared<NetworkClient>();
  }
  return m_network;
}

std::shared_ptr<ImageryProvider>
ProviderRegistry::createWmsImagery(const QJsonObject& source,
                                   QString* errorString)
{
  const std::optional<WmsSource> wms = WmsSource::fromJson(source, errorString);
  if (!wms) {
    return nullptr;
  }
  return std::make_shared<WmsImageryProvider>(*wms, networkLocked(),
                                              *m_wmsSettings);
}

std::shared_ptr<ImageryProvider>
ProviderRegistry::createBasePackImagery(const QJsonObject& source,
                                        QString* errorString) const
{
  const QString dataset = source.value(QStringLiteral("dataset")).toString();
  const QString path = basePackFile(source, false, errorString);
  if (path.isEmpty()) {
    return nullptr;
  }
  std::shared_ptr<SqliteTileImageryProvider> provider =
      std::make_shared<SqliteTileImageryProvider>(path);
  if (!provider->isValid()) {
    if (errorString) {
      *errorString = QCoreApplication::translate(
                         "ts::ProviderRegistry",
                         "Cannot read \"%1\" from the base pack: %2")
                         .arg(dataset, provider->errorString());
    }
    return nullptr;
  }
  return provider;
}

void ProviderRegistry::setBasePack(const QString& directory,
                                   const PackManifest& manifest)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  m_basePackDirectory = directory;
  m_basePackImagery.clear();
  m_basePackFeatures.clear();
  m_basePackAttribution.clear();
  for (const PackDataset& dataset : manifest.datasets) {
    if (dataset.kind == PackDataset::Kind::Imagery) {
      m_basePackImagery[dataset.id] = dataset.file;
    } else {
      m_basePackFeatures[dataset.id] = dataset.file;
    }
    m_basePackAttribution[dataset.id] = dataset.attribution;
  }
}

void ProviderRegistry::setInstalledPacks(
    const std::vector<InstalledPack>& packs)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  m_replacements.clear();
  for (const InstalledPack& pack : packs) {
    if (pack.damaged) {
      continue;
    }
    for (const PackDataset& dataset : pack.manifest.datasets) {
      if (dataset.replaces.isEmpty() ||
          m_replacements.count(dataset.replaces) != 0) {
        continue;
      }
      Replacement replacement;
      replacement.path = QDir(pack.directory).filePath(dataset.file);
      replacement.features = dataset.kind == PackDataset::Kind::Features;
      replacement.attribution = dataset.attribution;
      m_replacements[dataset.replaces] = replacement;
    }
  }
}

std::shared_ptr<FeatureProvider>
ProviderRegistry::features(const QJsonObject& source, QString* errorString)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  if (!isBasePackSource(source)) {
    if (errorString) {
      *errorString =
          QCoreApplication::translate("ts::ProviderRegistry",
                                      "Unknown or invalid feature source: %1")
              .arg(canonical(source));
    }
    return nullptr;
  }
  // A pack source depends on which file it resolves to.
  const QString key = canonical(source) + basePackFile(source, true, nullptr);
  const std::unordered_map<QString, std::weak_ptr<FeatureProvider>>::iterator
      it = m_features.find(key);
  if (it != m_features.end()) {
    std::shared_ptr<FeatureProvider> live = it->second.lock();
    if (live) {
      return live;
    }
  }
  const QString path = basePackFile(source, true, errorString);
  if (path.isEmpty()) {
    return nullptr;
  }
  std::shared_ptr<GeoPackageFeatureProvider> provider =
      std::make_shared<GeoPackageFeatureProvider>(path);
  if (!provider->isValid()) {
    if (errorString) {
      *errorString =
          QCoreApplication::translate(
              "ts::ProviderRegistry",
              "Cannot read \"%1\" from the base pack: %2")
              .arg(source.value(QStringLiteral("dataset")).toString(),
                   provider->errorString());
    }
    return nullptr;
  }
  m_features[key] = provider;
  return provider;
}

QString ProviderRegistry::attribution(const QJsonObject& source) const
{
  if (source.value(QStringLiteral("kind")).toString() == QLatin1String("wms")) {
    return source.value(QStringLiteral("attribution")).toString();
  }
  const std::lock_guard<std::mutex> lock(m_mutex);
  if (!isBasePackSource(source)) {
    return QString();
  }
  const QString dataset = source.value(QStringLiteral("dataset")).toString();
  const std::unordered_map<QString, Replacement>::const_iterator replacement =
      m_replacements.find(dataset);
  if (replacement != m_replacements.end()) {
    return replacement->second.attribution;
  }
  if (m_basePackDirectory.isEmpty()) {
    return QString();
  }
  const std::unordered_map<QString, QString>::const_iterator found =
      m_basePackAttribution.find(dataset);
  return found == m_basePackAttribution.end() ? QString() : found->second;
}

QString ProviderRegistry::basePackDirectory() const
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  return m_basePackDirectory;
}

std::shared_ptr<ImageryProvider>
ProviderRegistry::imagery(const QJsonObject& source, QString* errorString)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  // A pack source depends on which file it resolves to.
  const QString key =
      isBasePackSource(source)
          ? canonical(source) + basePackFile(source, false, nullptr)
          : canonical(source);
  const std::unordered_map<QString, std::weak_ptr<ImageryProvider>>::iterator
      it = m_imagery.find(key);
  if (it != m_imagery.end()) {
    std::shared_ptr<ImageryProvider> live = it->second.lock();
    if (live) {
      return live;
    }
  }
  std::shared_ptr<ImageryProvider> provider;
  if (isBasePackSource(source)) {
    provider = createBasePackImagery(source, errorString);
  } else if (source.value(QStringLiteral("kind")).toString() ==
             QLatin1String("wms")) {
    provider = createWmsImagery(source, errorString);
  } else {
    provider = createImageryProvider(source, errorString);
  }
  if (provider) {
    m_imagery[key] = provider;
  }
  return provider;
}

int ProviderRegistry::liveImageryProviders() const
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  int count = 0;
  for (const std::pair<const QString, std::weak_ptr<ImageryProvider>>& entry :
       m_imagery) {
    count += entry.second.expired() ? 0 : 1;
  }
  return count;
}

} // namespace ts
