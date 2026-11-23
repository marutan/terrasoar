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

#include "ts/providers/providerregistry.h"

#include <algorithm>
#include <cstdint>
#include <map>

#include <QColor>
#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>

#include "ts/elevation/compositeelevationprovider.h"
#include "ts/elevation/gdalelevationprovider.h"
#include "ts/elevation/geoidmodel.h"
#include "ts/elevation/sqliteelevationprovider.h"
#include "ts/elevation/webterrainprovider.h"
#include "ts/kml/kmlfeatureprovider.h"
#include "ts/kml/kmloverlays.h"
#include "ts/network/networkclient.h"
#include "ts/packs/packlibrary.h"
#include "ts/packs/packmanifest.h"
#include "ts/projection/datumshift.h"
#include "ts/providers/compositefeatureprovider.h"
#include "ts/providers/gdalimageryprovider.h"
#include "ts/providers/geopackagefeatureprovider.h"
#include "ts/providers/sqlitetileimageryprovider.h"
#include "ts/providers/testproviders.h"
#include "ts/tiling/tilekey.h"
#include "ts/wfs/wfsfeatureprovider.h"
#include "ts/wfs/wfssource.h"
#include "ts/wms/wmsimageryprovider.h"
#include "ts/wms/wmssource.h"
#include "ts/wmts/wmtsimageryprovider.h"
#include "ts/wmts/wmtssource.h"
#include "ts/xyz/xyzimageryprovider.h"
#include "ts/xyz/xyzsource.h"

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

int32_t intMember(const QJsonObject& source, const char* name, int32_t fallback)
{
  return source.value(QLatin1String(name)).toInt(fallback);
}

} // namespace

std::shared_ptr<ImageryProvider>
createImageryProvider(const QJsonObject& source, QString* errorString)
{
  const QString kind = source.value(QStringLiteral("kind")).toString();
  const int32_t maximumLevel =
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

/**
 * How a pack layer reads its dataset: some of the dataset's tables, with
 * its own style rules (see docs/design/packs.md#pack-layers).
 */
GeoPackageFeatureProvider::Options packLayerOptions(const QJsonObject& source)
{
  GeoPackageFeatureProvider::Options options;
  const QJsonArray tables = source.value(QStringLiteral("tables")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < tables.size(); ++i) {
    options.tables.push_back(tables.at(i).toString());
  }
  options.styleRules = source.value(QStringLiteral("styleRules")).toArray();
  return options;
}

} // namespace

QString ProviderRegistry::basePackFile(const QJsonObject& source, bool features,
                                       QString* errorString) const
{
  const QString dataset = source.value(QStringLiteral("dataset")).toString();
  const Replacement* variant = basePackVariant(source);
  if (variant != nullptr && variant->features == features) {
    return variant->path;
  }
  const std::unordered_map<QString, Replacement>::const_iterator replacement =
      m_replacements.find(dataset);
  if (replacement != m_replacements.end() &&
      replacement->second.features == features &&
      !replacement->second.elevation) {
    return replacement->second.path;
  }
  const std::unordered_map<QString, QString>& files =
      features ? m_basePackFeatures : m_basePackImagery;
  const std::unordered_map<QString, QString>::const_iterator file =
      files.find(dataset);
  if (m_basePackDirectory.isEmpty() || file == files.end()) {
    if (errorString) {
      *errorString =
          file == files.end() && !m_basePackDirectory.isEmpty()
              ? QCoreApplication::translate(
                    "ts::ProviderRegistry",
                    "\"%1\" is in no installed data pack. Install its pack "
                    "from the Data Packs window.")
                    .arg(dataset)
              : QCoreApplication::translate(
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
ProviderRegistry::createWmtsImagery(const QJsonObject& source,
                                    QString* errorString)
{
  const std::optional<WmtsSource> wmts =
      WmtsSource::fromJson(source, errorString);
  if (!wmts) {
    return nullptr;
  }
  // The same network client and cache settings as WMS: one limit on
  // concurrent requests, one cache directory.
  return std::make_shared<WmtsImageryProvider>(*wmts, networkLocked(),
                                               *m_wmsSettings);
}

std::shared_ptr<ImageryProvider>
ProviderRegistry::createXyzImagery(const QJsonObject& source,
                                   QString* errorString)
{
  const std::optional<XyzSource> xyz = XyzSource::fromJson(source, errorString);
  if (!xyz) {
    return nullptr;
  }
  return std::make_shared<XyzImageryProvider>(*xyz, networkLocked(),
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
  m_variants.clear();
  m_packGeoids.clear();
  // Providers created from now on use the packs' geoids.
  m_geoids.clear();
  // Datum shift grids (OSTN15) go to the projections; the first pack
  // with one wins.
  std::map<QString, QString> shiftGrids;
  for (const InstalledPack& pack : packs) {
    if (pack.damaged) {
      continue;
    }
    for (const PackDataset& shift : pack.manifest.datumShifts) {
      shiftGrids.try_emplace(shift.id,
                             QDir(pack.directory).filePath(shift.file));
    }
    for (const PackDataset& dataset : pack.manifest.datasets) {
      addPackDatasetLocked(pack.directory, dataset);
    }
  }
  setDatumShiftGrids(shiftGrids);
}

void ProviderRegistry::addPackDatasetLocked(const QString& directory,
                                            const PackDataset& dataset)
{
  const QString path = QDir(directory).filePath(dataset.file);
  if (dataset.kind == PackDataset::Kind::Geoid) {
    // A finer grid of a base pack datum, or a datum of its own.
    const QString id =
        dataset.replaces.isEmpty() ? dataset.id : dataset.replaces;
    // The first pack with a grid for it wins.
    m_packGeoids.try_emplace(id, PackGeoid{directory, dataset.file,
                                           dataset.name, dataset.attribution});
    return;
  }
  Replacement replacement;
  replacement.path = path;
  replacement.features = dataset.kind == PackDataset::Kind::Features;
  replacement.elevation = dataset.kind == PackDataset::Kind::Elevation;
  replacement.attribution = dataset.attribution;
  if (!dataset.variantOf.isEmpty() && !dataset.variant.isEmpty()) {
    // The first pack with this variant wins.
    m_variants[dataset.variantOf].try_emplace(dataset.variant, replacement);
    return;
  }
  // A dataset replacing none of the base pack's is one of its own
  // (VMAP0's): layer sources name it by its id.
  const bool own = dataset.replaces.isEmpty();
  const QString id = own ? dataset.id : dataset.replaces;
  const std::unordered_map<QString, Replacement>::iterator known =
      m_replacements.find(id);
  if (known != m_replacements.end()) {
    // Regional packs of one dataset of its own (VMAP0's hydrography by
    // library, GEBCO by quarter of the world) are read together; otherwise
    // the first pack wins.
    if (own && ((replacement.features && known->second.features) ||
                (replacement.elevation && known->second.elevation))) {
      known->second.morePaths.push_back(path);
    }
    return;
  }
  if (own && dataset.kind != PackDataset::Kind::Features &&
      dataset.kind != PackDataset::Kind::Imagery &&
      dataset.kind != PackDataset::Kind::Elevation) {
    return;
  }
  m_replacements.emplace(id, replacement);
}

const ProviderRegistry::Replacement*
ProviderRegistry::basePackVariant(const QJsonObject& source) const
{
  const std::unordered_map<
      QString, std::map<QString, Replacement>>::const_iterator variants =
      m_variants.find(source.value(QStringLiteral("dataset")).toString());
  if (variants == m_variants.end()) {
    return nullptr;
  }
  const QJsonArray dimensions =
      source.value(QStringLiteral("dimensions")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < dimensions.size(); ++i) {
    const QJsonArray pair = dimensions.at(i).toArray();
    if (pair.at(0).toString() == QLatin1String("month")) {
      const std::map<QString, Replacement>::const_iterator found =
          variants->second.find(pair.at(1).toString());
      return found == variants->second.end() ? nullptr : &found->second;
    }
  }
  return nullptr;
}

QString ProviderRegistry::basePackVariants(const QString& dataset) const
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  const std::unordered_map<
      QString, std::map<QString, Replacement>>::const_iterator variants =
      m_variants.find(dataset);
  if (variants == m_variants.end()) {
    return QString();
  }
  QStringList values;
  for (const std::pair<const QString, Replacement>& variant :
       variants->second) {
    values.append(variant.first);
  }
  return values.join(QLatin1Char(','));
}

std::optional<LayerTime>
ProviderRegistry::basePackTime(const QJsonObject& source) const
{
  if (!isBasePackSource(source) || source.contains(QStringLiteral("time"))) {
    return std::nullopt;
  }
  const QString extent =
      basePackVariants(source.value(QStringLiteral("dataset")).toString());
  if (extent.isEmpty()) {
    return std::nullopt;
  }
  LayerTime time;
  time.dimension = QStringLiteral("month");
  time.extent = extent;
  time.mode = LayerTime::Mode::MonthOfYear;
  return time;
}

namespace
{

/** A KML layer's placemarks ("kind": "kml"). */
std::shared_ptr<FeatureProvider> createKmlFeatures(const QJsonObject& source,
                                                   QString* errorString)
{
  std::shared_ptr<const KmlDocument> document = sharedKmlDocument(
      source.value(QStringLiteral("path")).toString(), errorString);
  if (!document) {
    return nullptr;
  }
  // The clock's moment for a document with times (kmlTimeKey()).
  std::optional<QDateTime> at;
  if (source.contains(QStringLiteral("at"))) {
    at = QDateTime::fromString(source.value(QStringLiteral("at")).toString(),
                               Qt::ISODate);
  }
  return std::make_shared<KmlFeatureProvider>(
      std::move(document), source.value(QStringLiteral("container")).toString(),
      at, source.value(QStringLiteral("deep")).toBool());
}

} // namespace

std::shared_ptr<FeatureProvider>
ProviderRegistry::features(const QJsonObject& source, QString* errorString)
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  if (source.value(QStringLiteral("kind")).toString() == QLatin1String("kml")) {
    const QString key = canonical(source);
    const std::unordered_map<QString, std::weak_ptr<FeatureProvider>>::iterator
        found = m_features.find(key);
    std::shared_ptr<FeatureProvider> provider =
        found != m_features.end() ? found->second.lock() : nullptr;
    if (!provider) {
      provider = createKmlFeatures(source, errorString);
    }
    if (provider) {
      m_features[key] = provider;
    }
    return provider;
  }
  if (source.value(QStringLiteral("kind")).toString() == QLatin1String("wfs")) {
    const QString key = canonical(source);
    const std::unordered_map<QString, std::weak_ptr<FeatureProvider>>::iterator
        found = m_features.find(key);
    if (found != m_features.end()) {
      std::shared_ptr<FeatureProvider> live = found->second.lock();
      if (live) {
        return live;
      }
    }
    const std::optional<WfsSource> wfs =
        WfsSource::fromJson(source, errorString);
    if (!wfs) {
      return nullptr;
    }
    // The network client and cache settings of WMS: one limit on
    // concurrent requests, one cache directory.
    std::shared_ptr<WfsFeatureProvider> provider =
        std::make_shared<WfsFeatureProvider>(*wfs, networkLocked(),
                                             *m_wmsSettings);
    m_features[key] = provider;
    return provider;
  }
  if (!isBasePackSource(source)) {
    if (errorString) {
      *errorString =
          QCoreApplication::translate("ts::ProviderRegistry",
                                      "Unknown or invalid feature source: %1")
              .arg(canonical(source));
    }
    return nullptr;
  }
  return packFeaturesLocked(source, errorString);
}

std::shared_ptr<FeatureProvider>
ProviderRegistry::packFeaturesLocked(const QJsonObject& source,
                                     QString* errorString)
{
  // A pack source depends on which files it resolves to.
  const QString dataset = source.value(QStringLiteral("dataset")).toString();
  const std::unordered_map<QString, Replacement>::const_iterator regional =
      m_replacements.find(dataset);
  std::vector<QString> morePaths;
  if (regional != m_replacements.end() && regional->second.features) {
    morePaths = regional->second.morePaths;
  }
  QString key = canonical(source) + basePackFile(source, true, nullptr);
  for (const QString& more : morePaths) {
    key += QLatin1Char('|') + more;
  }
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
  const GeoPackageFeatureProvider::Options options = packLayerOptions(source);
  std::vector<QString> paths = {path};
  paths.insert(paths.end(), morePaths.begin(), morePaths.end());
  std::vector<std::shared_ptr<FeatureProvider>> providers;
  for (const QString& file : paths) {
    std::shared_ptr<GeoPackageFeatureProvider> provider =
        std::make_shared<GeoPackageFeatureProvider>(file, options);
    if (!provider->isValid()) {
      if (errorString) {
        *errorString = QCoreApplication::translate(
                           "ts::ProviderRegistry",
                           "Cannot read \"%1\" from the base pack: %2")
                           .arg(dataset, provider->errorString());
      }
      return nullptr;
    }
    providers.push_back(provider);
  }
  std::shared_ptr<FeatureProvider> provider =
      providers.size() == 1
          ? providers.front()
          : std::make_shared<CompositeFeatureProvider>(std::move(providers));
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
  // A pack's finer DEM in place of the base pack's, or a DEM of a pack's
  // own; regional packs of one (GEBCO's quarters) read together.
  std::vector<QString> paths;
  const std::unordered_map<QString, Replacement>::const_iterator replacement =
      m_replacements.find(dataset);
  const std::unordered_map<QString, QString>::const_iterator file =
      m_basePackElevation.find(dataset);
  if (isBasePackSource(source) && replacement != m_replacements.end() &&
      replacement->second.elevation) {
    paths.push_back(replacement->second.path);
    paths.insert(paths.end(), replacement->second.morePaths.begin(),
                 replacement->second.morePaths.end());
  } else if (isBasePackSource(source) && !m_basePackDirectory.isEmpty() &&
             file != m_basePackElevation.end()) {
    paths.push_back(QDir(m_basePackDirectory).filePath(file->second));
  } else {
    if (errorString) {
      *errorString =
          QCoreApplication::translate("ts::ProviderRegistry",
                                      "Unknown or invalid elevation source: %1")
              .arg(canonical(source));
    }
    return nullptr;
  }
  std::vector<std::shared_ptr<ElevationProvider>> stores;
  stores.reserve(paths.size());
  QStringList ids;
  for (const QString& path : paths) {
    std::shared_ptr<ElevationProvider> store =
        elevationStoreLocked(path, dataset, errorString);
    if (!store) {
      return nullptr;
    }
    ids << store->id();
    stores.push_back(std::move(store));
  }
  if (stores.size() == 1) {
    return stores.front();
  }
  // Each region's tiles are its own: the composite takes each from the
  // store that has it.
  const QString key = ids.join(QLatin1Char('\n'));
  const std::unordered_map<QString, std::weak_ptr<ElevationProvider>>::iterator
      it = m_composites.find(key);
  if (it != m_composites.end()) {
    std::shared_ptr<ElevationProvider> live = it->second.lock();
    if (live) {
      return live;
    }
  }
  std::shared_ptr<ElevationProvider> composite =
      std::make_shared<CompositeElevationProvider>(std::move(stores));
  m_composites[key] = composite;
  return composite;
}

std::shared_ptr<ElevationProvider> ProviderRegistry::elevationStoreLocked(
    const QString& path, const QString& dataset, QString* errorString)
{
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
  if (kind == QLatin1String("wms") || kind == QLatin1String("wmts") ||
      kind == QLatin1String("xyz") || kind == QLatin1String("wfs") ||
      kind == QLatin1String("web") || kind == QLatin1String("file") ||
      kind == QLatin1String("satellites") || kind == QLatin1String("vessels")) {
    return source.value(QStringLiteral("attribution")).toString();
  }
  const std::lock_guard<std::mutex> lock(m_mutex);
  if (!isBasePackSource(source)) {
    return QString();
  }
  const QString dataset = source.value(QStringLiteral("dataset")).toString();
  const Replacement* variant = basePackVariant(source);
  if (variant != nullptr) {
    return variant->attribution;
  }
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

namespace
{

/** A local raster file read by GDAL ("kind": "file", "path"). */
std::shared_ptr<ImageryProvider> createFileImagery(const QJsonObject& source,
                                                   QString* errorString)
{
  const QString path = source.value(QStringLiteral("path")).toString();
  std::shared_ptr<GdalImageryProvider> provider =
      std::make_shared<GdalImageryProvider>(path);
  if (provider->isValid()) {
    return provider;
  }
  if (errorString != nullptr) {
    *errorString =
        QCoreApplication::translate("ts::ProviderRegistry",
                                    "Cannot use \"%1\" as imagery: %2")
            .arg(QDir::toNativeSeparators(path), provider->errorString());
  }
  return nullptr;
}

} // namespace

std::shared_ptr<ImageryProvider>
ProviderRegistry::createKmlOverlayLocked(const QJsonObject& source,
                                         QString* errorString)
{
  const std::shared_ptr<const KmlDocument> document = sharedKmlDocument(
      source.value(QStringLiteral("path")).toString(), errorString);
  if (!document) {
    return nullptr;
  }
  std::shared_ptr<KmlGroundOverlayProvider> provider =
      std::make_shared<KmlGroundOverlayProvider>(
          document, source.value(QStringLiteral("feature")).toString(),
          networkLocked());
  if (!provider->isValid()) {
    if (errorString != nullptr) {
      *errorString =
          QCoreApplication::translate("ts::ProviderRegistry",
                                      "Unknown or invalid imagery source: %1")
              .arg(canonical(source));
    }
    return nullptr;
  }
  return provider;
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
  } else if (source.value(QStringLiteral("kind")).toString() ==
             QLatin1String("wmts")) {
    provider = createWmtsImagery(source, errorString);
  } else if (source.value(QStringLiteral("kind")).toString() ==
             QLatin1String("xyz")) {
    provider = createXyzImagery(source, errorString);
  } else if (source.value(QStringLiteral("kind")).toString() ==
             QLatin1String("file")) {
    provider = createFileImagery(source, errorString);
  } else if (source.value(QStringLiteral("kind")).toString() ==
             QLatin1String("kmloverlay")) {
    provider = createKmlOverlayLocked(source, errorString);
  } else {
    provider = createImageryProvider(source, errorString);
  }
  if (provider) {
    m_imagery[key] = provider;
  }
  return provider;
}

int32_t ProviderRegistry::liveImageryProviders() const
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  int32_t count = 0;
  for (const std::pair<const QString, std::weak_ptr<ImageryProvider>>& entry :
       m_imagery) {
    count += entry.second.expired() ? 0 : 1;
  }
  return count;
}

} // namespace ts
