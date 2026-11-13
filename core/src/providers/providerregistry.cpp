// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/providers/providerregistry.h"

#include <algorithm>

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QStringList>

#include "ts/elevation/compositeelevationprovider.h"
#include "ts/elevation/gdalelevationprovider.h"
#include "ts/elevation/geoidmodel.h"
#include "ts/elevation/sqliteelevationprovider.h"
#include "ts/elevation/webterrainprovider.h"
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
    : m_wmsSettings(std::make_unique<WebTileSettings>())
{
}

ProviderRegistry::~ProviderRegistry() = default;

void ProviderRegistry::setWmsNetwork(std::shared_ptr<NetworkClient> network,
                                     const WebTileSettings& settings)
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
  m_basePackElevation.clear();
  m_basePackGeoids.clear();
  m_basePackAttribution.clear();
  m_geoids.clear();
  for (const PackDataset& dataset : manifest.datasets) {
    switch (dataset.kind) {
    case PackDataset::Kind::Imagery:
      m_basePackImagery[dataset.id] = dataset.file;
      break;
    case PackDataset::Kind::Features:
      m_basePackFeatures[dataset.id] = dataset.file;
      break;
    case PackDataset::Kind::Elevation:
      m_basePackElevation[dataset.id] = dataset.file;
      break;
    case PackDataset::Kind::Geoid:
      m_basePackGeoids[dataset.id] = dataset.file;
      break;
    case PackDataset::Kind::Areas:
      break;
    }
    m_basePackAttribution[dataset.id] = dataset.attribution;
  }
}

void ProviderRegistry::setInstalledPacks(
    const std::vector<InstalledPack>& packs)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  m_replacements.clear();
  m_packGeoids.clear();
  // Providers created from now on use the packs' geoids.
  m_geoids.clear();
  for (const InstalledPack& pack : packs) {
    if (pack.damaged) {
      continue;
    }
    for (const PackDataset& dataset : pack.manifest.datasets) {
      if (dataset.kind == PackDataset::Kind::Geoid) {
        // A finer grid of a base pack datum, or a datum of its own.
        const QString id =
            dataset.replaces.isEmpty() ? dataset.id : dataset.replaces;
        // The first pack with a grid for it wins.
        m_packGeoids.try_emplace(id,
                                 PackGeoid{pack.directory, dataset.file,
                                           dataset.name, dataset.attribution});
        continue;
      }
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

std::shared_ptr<const GeoidModel>
ProviderRegistry::geoid(const QString& dataset, QString* errorString)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  return geoidLocked(dataset, errorString);
}

std::shared_ptr<const GeoidModel>
ProviderRegistry::geoidLocked(const QString& dataset, QString* errorString)
{
  const std::unordered_map<
      QString, std::shared_ptr<const GeoidModel>>::const_iterator loaded =
      m_geoids.find(dataset);
  if (loaded != m_geoids.end()) {
    return loaded->second;
  }
  // An installed pack's grid first (finer, or a datum of its own).
  const std::unordered_map<QString, PackGeoid>::const_iterator packed =
      m_packGeoids.find(dataset);
  if (packed != m_packGeoids.end()) {
    std::shared_ptr<const GeoidModel> model =
        std::make_shared<const GeoidModel>(packed->second.directory,
                                           packed->second.file);
    if (!model->isValid()) {
      if (errorString) {
        *errorString =
            QCoreApplication::translate("ts::ProviderRegistry",
                                        "Cannot read the geoid \"%1\": %2")
                .arg(dataset, model->errorString());
      }
      return nullptr;
    }
    m_geoids[dataset] = model;
    return model;
  }
  const std::unordered_map<QString, QString>::const_iterator file =
      m_basePackGeoids.find(dataset);
  if (m_basePackDirectory.isEmpty() || file == m_basePackGeoids.end()) {
    if (errorString) {
      *errorString = QCoreApplication::translate(
                         "ts::ProviderRegistry",
                         "The base pack, which holds \"%1\", is not installed.")
                         .arg(dataset);
    }
    return nullptr;
  }
  std::shared_ptr<const GeoidModel> model =
      std::make_shared<const GeoidModel>(m_basePackDirectory, file->second);
  if (!model->isValid()) {
    if (errorString) {
      *errorString = QCoreApplication::translate(
                         "ts::ProviderRegistry",
                         "Cannot read \"%1\" from the base pack: %2")
                         .arg(dataset, model->errorString());
    }
    return nullptr;
  }
  m_geoids[dataset] = model;
  return model;
}

std::shared_ptr<ElevationProvider>
ProviderRegistry::elevation(const QJsonObject& source, QString* errorString)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  const QString kind = source.value(QStringLiteral("kind")).toString();
  if (kind == QLatin1String("file")) {
    return fileElevationLocked(source, errorString);
  }
  if (kind == QLatin1String("web")) {
    return webElevationLocked(source, errorString);
  }
  return basePackElevationLocked(source, errorString);
}

std::shared_ptr<ElevationProvider>
ProviderRegistry::webElevationLocked(const QJsonObject& source,
                                     QString* errorString)
{
  const std::optional<WebTerrainSource> web =
      WebTerrainSource::fromJson(source, errorString);
  if (!web) {
    return nullptr;
  }
  std::shared_ptr<const GeoidModel> datumGeoid;
  if (!datumGeoidLocked(web->verticalDatum, &datumGeoid, source, errorString)) {
    return nullptr;
  }
  const QString key = web->id() + QLatin1Char('|') +
                      (datumGeoid ? datumGeoid->id() : QString());
  const std::unordered_map<QString, std::weak_ptr<ElevationProvider>>::iterator
      it = m_elevation.find(key);
  if (it != m_elevation.end()) {
    std::shared_ptr<ElevationProvider> live = it->second.lock();
    if (live) {
      return live;
    }
  }
  QString ignored;
  std::shared_ptr<const GeoidModel> seaLevel =
      geoidLocked(QStringLiteral("egm2008"), &ignored);
  std::shared_ptr<ElevationProvider> provider =
      std::make_shared<WebTerrainProvider>(
          *web, networkLocked(), *m_wmsSettings, datumGeoid, seaLevel);
  m_elevation[key] = provider;
  return provider;
}

bool ProviderRegistry::datumGeoidLocked(
    const QString& datum, std::shared_ptr<const GeoidModel>* geoid,
    const QJsonObject& source, QString* errorString)
{
  if (datum == QLatin1String("ellipsoid")) {
    geoid->reset();
    return true;
  }
  if (datum.isEmpty() ||
      (m_basePackGeoids.count(datum) == 0 && m_packGeoids.count(datum) == 0)) {
    if (errorString) {
      *errorString =
          QCoreApplication::translate("ts::ProviderRegistry",
                                      "Unknown or invalid elevation source: %1")
              .arg(canonical(source));
    }
    return false;
  }
  *geoid = geoidLocked(datum, errorString);
  return *geoid != nullptr;
}

std::shared_ptr<ElevationProvider>
ProviderRegistry::fileElevationLocked(const QJsonObject& source,
                                      QString* errorString)
{
  const QString path = source.value(QStringLiteral("path")).toString();
  const QString datum =
      source.value(QStringLiteral("verticalDatum")).toString();
  std::shared_ptr<const GeoidModel> datumGeoid;
  if (path.isEmpty() ||
      !datumGeoidLocked(datum, &datumGeoid, source, errorString)) {
    if (path.isEmpty() && errorString) {
      *errorString =
          QCoreApplication::translate("ts::ProviderRegistry",
                                      "Unknown or invalid elevation source: %1")
              .arg(canonical(source));
    }
    return nullptr;
  }
  // Shared while the same grid converts its heights.
  const QString key = path + QLatin1Char('|') + datum + QLatin1Char('|') +
                      (datumGeoid ? datumGeoid->id() : QString());
  const std::unordered_map<QString, std::weak_ptr<ElevationProvider>>::iterator
      it = m_elevation.find(key);
  if (it != m_elevation.end()) {
    std::shared_ptr<ElevationProvider> live = it->second.lock();
    if (live) {
      return live;
    }
  }
  // Sea level for heights above the ellipsoid; without the base pack the
  // ellipsoid stands in for it.
  QString ignored;
  std::shared_ptr<const GeoidModel> seaLevel =
      geoidLocked(QStringLiteral("egm2008"), &ignored);
  std::shared_ptr<GdalElevationProvider> provider =
      std::make_shared<GdalElevationProvider>(path, datum, datumGeoid,
                                              seaLevel);
  if (!provider->isValid()) {
    if (errorString) {
      *errorString =
          QCoreApplication::translate("ts::ProviderRegistry",
                                      "Cannot read \"%1\": %2")
              .arg(QDir::toNativeSeparators(path), provider->errorString());
    }
    return nullptr;
  }
  m_elevation[key] = provider;
  return provider;
}

std::shared_ptr<ElevationProvider> ProviderRegistry::elevationComposite(
    std::vector<std::shared_ptr<ElevationProvider>> sources)
{
  QStringList ids;
  for (const std::shared_ptr<ElevationProvider>& source : sources) {
    ids << source->id();
  }
  const QString key = ids.join(QLatin1Char('\n'));
  const std::lock_guard<std::mutex> lock(m_mutex);
  const std::unordered_map<QString, std::weak_ptr<ElevationProvider>>::iterator
      it = m_composites.find(key);
  if (it != m_composites.end()) {
    std::shared_ptr<ElevationProvider> live = it->second.lock();
    if (live) {
      return live;
    }
  }
  std::shared_ptr<ElevationProvider> composite =
      std::make_shared<CompositeElevationProvider>(std::move(sources));
  m_composites[key] = composite;
  return composite;
}

std::shared_ptr<ElevationProvider>
ProviderRegistry::basePackElevationLocked(const QJsonObject& source,
                                          QString* errorString)
{
  const QString dataset = source.value(QStringLiteral("dataset")).toString();
  const std::unordered_map<QString, QString>::const_iterator file =
      m_basePackElevation.find(dataset);
  if (!isBasePackSource(source) || m_basePackDirectory.isEmpty() ||
      file == m_basePackElevation.end()) {
    if (errorString) {
      *errorString =
          QCoreApplication::translate("ts::ProviderRegistry",
                                      "Unknown or invalid elevation source: %1")
              .arg(canonical(source));
    }
    return nullptr;
  }
  const QString path = QDir(m_basePackDirectory).filePath(file->second);
  // The store names its vertical datum: a geoid dataset of the pack (or a
  // finer one from an installed pack).
  const QString datum = SqliteTileImageryProvider(path).info().verticalDatum;
  std::shared_ptr<const GeoidModel> geoid;
  if (!datum.isEmpty()) {
    geoid = geoidLocked(datum, errorString);
    if (!geoid) {
      return nullptr;
    }
  }
  // Shared while the same grid converts its heights.
  const QString key =
      path + QLatin1Char('|') + (geoid ? geoid->id() : QString());
  const std::unordered_map<QString, std::weak_ptr<ElevationProvider>>::iterator
      it = m_elevation.find(key);
  if (it != m_elevation.end()) {
    std::shared_ptr<ElevationProvider> live = it->second.lock();
    if (live) {
      return live;
    }
  }
  std::shared_ptr<SqliteElevationProvider> provider =
      std::make_shared<SqliteElevationProvider>(path, geoid);
  if (!provider->isValid()) {
    if (errorString) {
      *errorString = QCoreApplication::translate(
                         "ts::ProviderRegistry",
                         "Cannot read \"%1\" from the base pack: %2")
                         .arg(dataset, provider->errorString());
    }
    return nullptr;
  }
  m_elevation[key] = provider;
  return provider;
}

std::vector<VerticalDatum> ProviderRegistry::verticalDatums() const
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  std::vector<VerticalDatum> datums;
  for (const QString& id :
       {QStringLiteral("egm2008"), QStringLiteral("egm96")}) {
    if (m_basePackGeoids.count(id) != 0 || m_packGeoids.count(id) != 0) {
      datums.push_back(VerticalDatum{id, QString()});
    }
  }
  for (const std::pair<const QString, PackGeoid>& geoid : m_packGeoids) {
    if (geoid.first != QLatin1String("egm2008") &&
        geoid.first != QLatin1String("egm96")) {
      datums.push_back(VerticalDatum{geoid.first, geoid.second.name});
    }
  }
  std::sort(datums.begin() + std::min<std::ptrdiff_t>(
                                 static_cast<std::ptrdiff_t>(datums.size()), 2),
            datums.end(), [](const VerticalDatum& a, const VerticalDatum& b) {
              return a.id < b.id;
            });
  datums.push_back(VerticalDatum{QStringLiteral("ellipsoid"), QString()});
  return datums;
}

QString ProviderRegistry::attribution(const QJsonObject& source) const
{
  const QString kind = source.value(QStringLiteral("kind")).toString();
  if (kind == QLatin1String("wms") || kind == QLatin1String("web") ||
      kind == QLatin1String("file")) {
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
