// SPDX-License-Identifier: GPL-2.0-or-later
#include "packbuilder.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <utility>
#include <vector>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QStringList>

#include <cpl_vsi.h>

#include "ts/packs/packmanifest.h"
#include "ts/providers/geopackagefeatureprovider.h"
#include "ts/providers/sqlitetileimageryprovider.h"
#include "ts/tiling/tilekey.h"

namespace ts::packager
{

namespace
{

bool fail(QString* errorString, const QString& message)
{
  *errorString = message;
  return false;
}

QString text(const QJsonObject& json, const char* name)
{
  return json.value(QLatin1String(name)).toString();
}

int integer(const QJsonObject& json, const char* name, int fallback)
{
  return json.value(QLatin1String(name)).toInt(fallback);
}

const SourceSpec* findSource(const PackConfig& config, const QString& id)
{
  for (const SourceSpec& source : config.sources) {
    if (source.id == id) {
      return &source;
    }
  }
  return nullptr;
}

/**
 * The source as a path GDAL can read: a zip as /vsizip/<path>, anything
 * else (a file or a directory) as its absolute path. Append "/<inside>"
 * for a file in a zip or directory.
 */
QString gdalPath(const QString& file)
{
  const QFileInfo info(file);
  const QString path = info.absoluteFilePath();
  return info.suffix().compare(QLatin1String("zip"), Qt::CaseInsensitive) == 0
             ? QStringLiteral("/vsizip/") + path
             : path;
}

/** Copies the file @p from (a GDAL path, maybe in a zip) to @p target. */
bool copyGdalFile(const QByteArray& from, const QString& target,
                  QString* errorString)
{
  VSILFILE* in = VSIFOpenL(from.constData(), "rb");
  if (in == nullptr) {
    return fail(errorString,
                QStringLiteral("Cannot read %1").arg(QString::fromUtf8(from)));
  }
  QByteArray contents;
  std::vector<char> buffer(std::size_t{1} << 16);
  std::size_t read = 0;
  while ((read = VSIFReadL(buffer.data(), 1, buffer.size(), in)) > 0) {
    contents.append(buffer.data(), static_cast<int>(read));
  }
  VSIFCloseL(in);
  QFile out(target);
  if (!out.open(QIODevice::WriteOnly) ||
      out.write(contents) != contents.size()) {
    return fail(errorString, QStringLiteral("Cannot write %1").arg(target));
  }
  return true;
}

PackSource packSource(const SourceSpec& source)
{
  PackSource result;
  result.name = source.name;
  result.version = source.version;
  result.url = source.url;
  result.sha256 = source.sha256;
  return result;
}

/** One entry of "sources", or nothing if it is unusable. */
std::optional<SourceSpec> readSource(const QJsonObject& entry)
{
  SourceSpec source;
  source.id = text(entry, "id");
  source.file = text(entry, "file");
  source.url = text(entry, "url");
  source.sha256 = text(entry, "sha256").toLower();
  source.name = text(entry, "name");
  source.version = text(entry, "version");
  const QJsonArray notices = entry.value(QStringLiteral("notices")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int j = 0; j < notices.size(); ++j) {
    source.notices.push_back(notices.at(j).toString());
  }
  const QJsonArray local =
      entry.value(QStringLiteral("localNotices")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int j = 0; j < local.size(); ++j) {
    source.localNotices.push_back(local.at(j).toString());
  }
  if (source.id.isEmpty() || source.file.isEmpty() ||
      source.file.contains(QLatin1Char('/')) || source.sha256.size() != 64) {
    return std::nullopt;
  }
  return source;
}

/** Reads the optional "geoids" and "elevation" entries into @p config. */
bool readElevation(const QJsonObject& json, PackConfig* config,
                   QString* errorString)
{
  const QJsonArray geoids = json.value(QStringLiteral("geoids")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int i = 0; i < geoids.size(); ++i) {
    const QJsonObject entry = geoids.at(i).toObject();
    GeoidSpec spec;
    spec.dataset = text(entry, "dataset");
    spec.source = text(entry, "source");
    spec.name = text(entry, "name");
    spec.licence = text(entry, "licence");
    spec.attribution = text(entry, "attribution");
    spec.step = integer(entry, "step", 1);
    if (findSource(*config, spec.source) == nullptr || spec.dataset.isEmpty() ||
        spec.step < 1) {
      return fail(errorString, QStringLiteral("Bad geoid entry %1").arg(i));
    }
    config->geoids.push_back(spec);
  }
  const QJsonObject elevation =
      json.value(QStringLiteral("elevation")).toObject();
  if (elevation.isEmpty()) {
    return true;
  }
  ElevationSpec spec;
  spec.dataset = text(elevation, "dataset");
  spec.source = text(elevation, "source");
  spec.name = text(elevation, "name");
  spec.licence = text(elevation, "licence");
  spec.attribution = text(elevation, "attribution");
  spec.maxLevel = integer(elevation, "maxLevel", 6);
  spec.samples = integer(elevation, "samples", 65);
  spec.verticalDatum = text(elevation, "verticalDatum");
  const bool datumKnown =
      spec.verticalDatum.isEmpty() ||
      std::any_of(config->geoids.begin(), config->geoids.end(),
                  [&spec](const GeoidSpec& geoid) {
                    return geoid.dataset == spec.verticalDatum;
                  });
  if (findSource(*config, spec.source) == nullptr || spec.dataset.isEmpty() ||
      spec.maxLevel < 0 || spec.maxLevel > 10 || spec.samples < 3 ||
      !datumKnown) {
    return fail(errorString, QStringLiteral("Bad elevation entry"));
  }
  config->elevation = spec;
  return true;
}

/** Reads the optional "countries" entry into @p config. */
bool readCountries(const QJsonObject& json, PackConfig* config,
                   QString* errorString)
{
  const QJsonObject countries =
      json.value(QStringLiteral("countries")).toObject();
  if (countries.isEmpty()) {
    return true;
  }
  CountriesSpec spec;
  spec.dataset = text(countries, "dataset");
  spec.source = text(countries, "source");
  spec.name = text(countries, "name");
  spec.licence = text(countries, "licence");
  spec.attribution = text(countries, "attribution");
  spec.bufferDeg =
      countries.value(QStringLiteral("bufferDeg")).toDouble(spec.bufferDeg);
  spec.simplifyDeg =
      countries.value(QStringLiteral("simplifyDeg")).toDouble(spec.simplifyDeg);
  const QJsonArray exclude =
      countries.value(QStringLiteral("exclude")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int i = 0; i < exclude.size(); ++i) {
    spec.exclude.push_back(exclude.at(i).toString());
  }
  if (findSource(*config, spec.source) == nullptr || spec.dataset.isEmpty() ||
      !(spec.bufferDeg > spec.simplifyDeg) || spec.simplifyDeg < 0.0) {
    return fail(errorString, QStringLiteral("Bad countries entry"));
  }
  config->countries = spec;
  return true;
}

} // namespace

std::optional<PackConfig> PackConfig::fromJson(const QJsonObject& json,
                                               QString* errorString)
{
  PackConfig config;
  config.id = text(json, "id");
  config.version = text(json, "version");
  config.sizeLimitBytes = static_cast<qint64>(
      json.value(QStringLiteral("sizeLimitBytes")).toDouble());
  config.reservedBytes = static_cast<qint64>(
      json.value(QStringLiteral("reservedBytes")).toDouble());
  const QJsonArray sources = json.value(QStringLiteral("sources")).toArray();
  // An index loop: QJsonArray's iterator differs between Qt 5 and Qt 6.
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int i = 0; i < sources.size(); ++i) {
    std::optional<SourceSpec> source = readSource(sources.at(i).toObject());
    if (!source) {
      fail(errorString, QStringLiteral("Bad source entry %1").arg(i));
      return std::nullopt;
    }
    config.sources.push_back(std::move(*source));
  }

  const QJsonObject imagery = json.value(QStringLiteral("imagery")).toObject();
  if (!imagery.isEmpty()) {
    ImagerySpec spec;
    spec.dataset = text(imagery, "dataset");
    spec.source = text(imagery, "source");
    spec.name = text(imagery, "name");
    spec.licence = text(imagery, "licence");
    spec.attribution = text(imagery, "attribution");
    spec.replaces = text(imagery, "replaces");
    spec.tiling.maxLevel = integer(imagery, "maxLevel", 4);
    spec.tiling.tileSizePx = integer(imagery, "tileSize", 256);
    spec.tiling.jpegQuality = integer(imagery, "jpegQuality", 85);
    if (findSource(config, spec.source) == nullptr || spec.dataset.isEmpty() ||
        spec.tiling.maxLevel < 0 || spec.tiling.maxLevel > 8 ||
        spec.tiling.tileSizePx < 16) {
      fail(errorString, QStringLiteral("Bad imagery entry"));
      return std::nullopt;
    }
    config.imagery = spec;
  }

  const QJsonObject shoreline =
      json.value(QStringLiteral("shoreline")).toObject();
  if (!shoreline.isEmpty()) {
    ShorelineSpec spec;
    spec.dataset = text(shoreline, "dataset");
    spec.source = text(shoreline, "source");
    spec.name = text(shoreline, "name");
    spec.licence = text(shoreline, "licence");
    spec.attribution = text(shoreline, "attribution");
    spec.replaces = text(shoreline, "replaces");
    spec.minimumAreaKm2 =
        shoreline.value(QStringLiteral("minimumAreaKm2")).toDouble(0.0);
    const QJsonArray lods = shoreline.value(QStringLiteral("lods")).toArray();
    // NOLINTNEXTLINE(modernize-loop-convert)
    for (int i = 0; i < lods.size(); ++i) {
      const QJsonObject entry = lods.at(i).toObject();
      ShorelineLod lod;
      lod.resolution = text(entry, "resolution");
      lod.storageLevel = integer(entry, "storageLevel", 0);
      lod.minLevel = integer(entry, "minLevel", lod.storageLevel);
      lod.maxLevel = integer(entry, "maxLevel", -1);
      if (lod.resolution.size() != 1 ||
          !QStringLiteral("clihf").contains(lod.resolution) ||
          lod.storageLevel < 0 || lod.storageLevel > 10 ||
          lod.minLevel < lod.storageLevel) {
        fail(errorString, QStringLiteral("Bad shoreline level %1").arg(i));
        return std::nullopt;
      }
      spec.lods.push_back(lod);
    }
    if (findSource(config, spec.source) == nullptr || spec.dataset.isEmpty() ||
        spec.lods.empty()) {
      fail(errorString, QStringLiteral("Bad shoreline entry"));
      return std::nullopt;
    }
    config.shoreline = spec;
  }
  if (!readCountries(json, &config, errorString) ||
      !readElevation(json, &config, errorString)) {
    return std::nullopt;
  }
  if (config.id.isEmpty() || config.sizeLimitBytes <= 0) {
    fail(errorString, QStringLiteral("The pack has no id or size limit"));
    return std::nullopt;
  }
  return config;
}

std::optional<PackConfig> PackConfig::read(const QString& path,
                                           QString* errorString)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    fail(errorString, QStringLiteral("Cannot read %1").arg(path));
    return std::nullopt;
  }
  QJsonParseError parseError{};
  const QJsonDocument document =
      QJsonDocument::fromJson(file.readAll(), &parseError);
  if (!document.isObject()) {
    fail(errorString,
         QStringLiteral("%1: %2").arg(path, parseError.errorString()));
    return std::nullopt;
  }
  return fromJson(document.object(), errorString);
}

QJsonObject BuildReport::toJson() const
{
  QJsonArray lods;
  for (const ShorelineLodStats& lod : shoreline) {
    QJsonObject entry;
    entry.insert(QStringLiteral("resolution"), lod.resolution);
    entry.insert(QStringLiteral("sourcePolygons"), lod.sourcePolygons);
    entry.insert(QStringLiteral("repairedPolygons"), lod.repairedPolygons);
    entry.insert(QStringLiteral("landTiles"), lod.landTiles);
    entry.insert(QStringLiteral("vertices"), static_cast<double>(lod.vertices));
    entry.insert(QStringLiteral("landAreaKm2"), lod.landAreaKm2);
    entry.insert(QStringLiteral("sourceAreaKm2"), lod.sourceAreaKm2);
    entry.insert(QStringLiteral("gshhgAreaKm2"), lod.gshhgAreaKm2);
    lods.append(entry);
  }
  QJsonObject imageryJson;
  imageryJson.insert(QStringLiteral("tiles"), imagery.tiles);
  imageryJson.insert(QStringLiteral("bytes"),
                     static_cast<double>(imagery.bytes));
  QJsonObject json;
  json.insert(QStringLiteral("totalBytes"), static_cast<double>(totalBytes));
  json.insert(QStringLiteral("imagery"), imageryJson);
  json.insert(QStringLiteral("shoreline"), lods);
  QJsonObject countriesJson;
  countriesJson.insert(QStringLiteral("countries"), countries.countries);
  countriesJson.insert(QStringLiteral("excluded"), countries.excluded);
  countriesJson.insert(QStringLiteral("sourceVertices"),
                       static_cast<double>(countries.sourceVertices));
  countriesJson.insert(QStringLiteral("rings"),
                       static_cast<double>(countries.rings));
  countriesJson.insert(QStringLiteral("vertices"),
                       static_cast<double>(countries.vertices));
  json.insert(QStringLiteral("countries"), countriesJson);
  QJsonObject elevationJson;
  elevationJson.insert(QStringLiteral("tiles"), elevation.tiles);
  elevationJson.insert(QStringLiteral("minHeightM"), elevation.minHeightM);
  elevationJson.insert(QStringLiteral("maxHeightM"), elevation.maxHeightM);
  elevationJson.insert(QStringLiteral("maxQuantizationErrorM"),
                       elevation.maxQuantizationErrorM);
  json.insert(QStringLiteral("elevation"), elevationJson);
  return json;
}

namespace
{

/** One run of buildPack(), step by step. */
class PackBuild
{
public:
  PackBuild(const PackConfig& config, const BuildOptions& options,
            BuildReport* report, QString* errorString)
      : m_config(&config), m_options(&options), m_report(report),
        m_error(errorString), m_sources(options.sourcesDirectory),
        m_output(options.outputDirectory),
        m_progress(options.progress ? options.progress : [](const QString&) {})
  {
    m_manifest.id = config.id;
    m_manifest.version = config.version;
  }

  bool run()
  {
    return checkSources() && prepareOutput() && buildImagery() &&
           buildShoreline() && buildCountries() && buildGeoids() &&
           buildElevation() && copyNotices() && writeManifest() &&
           checkBudget();
  }

private:
  /** The sources must be the ones the configuration names. */
  bool checkSources()
  {
    for (const SourceSpec& source : m_config->sources) {
      const QString path = m_sources.filePath(source.file);
      if (!QFileInfo::exists(path)) {
        return fail(m_error, QStringLiteral("Missing source %1 (run "
                                            "fetch_sources.py)")
                                 .arg(path));
      }
      if (m_options->verifySources) {
        m_progress(QStringLiteral("Checking %1").arg(source.file));
        if (fileSha256(path, m_notCancelled) != source.sha256) {
          return fail(m_error,
                      QStringLiteral("%1 does not have the expected SHA-256")
                          .arg(path));
        }
      }
    }
    return true;
  }

  /** A fresh output directory. */
  bool prepareOutput()
  {
    if (m_output.exists() &&
        !m_output.entryList(QDir::AllEntries | QDir::NoDotAndDotDot)
             .isEmpty()) {
      return fail(m_error, QStringLiteral("%1 exists and is not empty")
                               .arg(m_options->outputDirectory));
    }
    if (!QDir().mkpath(m_options->outputDirectory)) {
      return fail(
          m_error,
          QStringLiteral("Cannot create %1").arg(m_options->outputDirectory));
    }
    return true;
  }

  void addDataset(const QString& id, PackDataset::Kind kind,
                  const QString& file, const SourceSpec& source)
  {
    PackDataset dataset;
    dataset.id = id;
    dataset.kind = kind;
    dataset.file = file;
    dataset.source = packSource(source);
    m_manifest.datasets.push_back(dataset);
  }

  bool buildImagery()
  {
    if (!m_config->imagery) {
      return true;
    }
    const ImagerySpec& spec = *m_config->imagery;
    const SourceSpec& source = *findSource(*m_config, spec.source);
    const QString file = spec.dataset + QStringLiteral(".sqlite");
    const int width = spec.tiling.tileSizePx *
                      static_cast<int>(TileKey::columns(spec.tiling.maxLevel));
    m_progress(QStringLiteral("Reading %1 into a %2 × %3 mosaic")
                   .arg(source.file)
                   .arg(width)
                   .arg(width / 2));
    const QImage mosaic = readEquirectangular(
        gdalPath(m_sources.filePath(source.file)), width, width / 2, m_error);
    if (mosaic.isNull()) {
      return false;
    }
    SqliteTileStoreInfo info;
    info.format = QStringLiteral("jpeg");
    info.tileSizePx = spec.tiling.tileSizePx;
    info.maxLevel = spec.tiling.maxLevel;
    info.name = spec.name;
    SqliteTileStoreWriter writer(m_output.filePath(file), info);
    ImageryTilerOptions tiling = spec.tiling;
    tiling.progress = m_progress;
    if (!writer.errorString().isEmpty() ||
        !tileMosaic(mosaic, tiling, &writer, &m_report->imagery, m_error) ||
        !writer.finish()) {
      return fail(m_error,
                  m_error->isEmpty() ? writer.errorString() : *m_error);
    }
    addDataset(spec.dataset, PackDataset::Kind::Imagery, file, source);
    PackDataset& dataset = m_manifest.datasets.back();
    dataset.name = spec.name;
    dataset.licence = spec.licence;
    dataset.attribution = spec.attribution;
    dataset.replaces = spec.replaces;
    return true;
  }

  bool buildShoreline()
  {
    if (!m_config->shoreline) {
      return true;
    }
    const ShorelineSpec& spec = *m_config->shoreline;
    const SourceSpec& source = *findSource(*m_config, spec.source);
    const QString file = spec.dataset + QStringLiteral(".gpkg");
    GeoPackageFeatureWriter writer(m_output.filePath(file));
    ShorelineOptions options;
    options.shapefileDirectory = gdalPath(m_sources.filePath(source.file)) +
                                 QStringLiteral("/GSHHS_shp");
    options.lods = spec.lods;
    options.minimumAreaKm2 = spec.minimumAreaKm2;
    options.progress = m_progress;
    if (!writer.errorString().isEmpty() ||
        !ts::packager::buildShoreline(options, &writer, &m_report->shoreline,
                                      m_error) ||
        !writer.finish()) {
      return fail(m_error,
                  m_error->isEmpty() ? writer.errorString() : *m_error);
    }
    for (const ShorelineLodStats& lod : m_report->shoreline) {
      const double error =
          std::abs(lod.landAreaKm2 - lod.sourceAreaKm2) / lod.sourceAreaKm2;
      if (!(error <= areaTolerance)) {
        return fail(m_error,
                    QStringLiteral("Land area of resolution %1 is %2 km², "
                                   "%3 % off the source polygons' %4 km²")
                        .arg(lod.resolution)
                        .arg(lod.landAreaKm2, 0, 'f', 0)
                        .arg(100.0 * error, 0, 'f', 3)
                        .arg(lod.sourceAreaKm2, 0, 'f', 0));
      }
    }
    addDataset(spec.dataset, PackDataset::Kind::Features, file, source);
    PackDataset& dataset = m_manifest.datasets.back();
    dataset.name = spec.name;
    dataset.licence = spec.licence;
    dataset.attribution = spec.attribution;
    dataset.replaces = spec.replaces;
    return true;
  }

  bool buildGeoids()
  {
    for (const GeoidSpec& spec : m_config->geoids) {
      const SourceSpec& source = *findSource(*m_config, spec.source);
      const QString file = spec.dataset + QStringLiteral(".tif");
      m_progress(QStringLiteral("Writing the geoid grid %1").arg(file));
      GeoidGridOptions options;
      options.sourcePath = m_sources.filePath(source.file);
      options.outputPath = m_output.filePath(file);
      options.step = spec.step;
      GeoidGridStats stats;
      if (!buildGeoidGrid(options, &stats, m_error)) {
        return false;
      }
      addDataset(spec.dataset, PackDataset::Kind::Geoid, file, source);
      PackDataset& dataset = m_manifest.datasets.back();
      dataset.name = spec.name;
      dataset.licence = spec.licence;
      dataset.attribution = spec.attribution;
    }
    return true;
  }

  bool buildElevation()
  {
    if (!m_config->elevation) {
      return true;
    }
    const ElevationSpec& spec = *m_config->elevation;
    const SourceSpec& source = *findSource(*m_config, spec.source);
    const QString file = spec.dataset + QStringLiteral(".sqlite");
    ElevationOptions options;
    options.sourcePath = m_sources.filePath(source.file);
    options.outputPath = m_output.filePath(file);
    options.maxLevel = spec.maxLevel;
    options.samples = spec.samples;
    options.verticalDatum = spec.verticalDatum;
    options.name = spec.name;
    options.progress = m_progress;
    if (!ts::packager::buildElevation(options, &m_report->elevation, m_error)) {
      return false;
    }
    addDataset(spec.dataset, PackDataset::Kind::Elevation, file, source);
    PackDataset& dataset = m_manifest.datasets.back();
    dataset.name = spec.name;
    dataset.licence = spec.licence;
    dataset.attribution = spec.attribution;
    return true;
  }

  bool buildCountries()
  {
    if (!m_config->countries) {
      return true;
    }
    const CountriesSpec& spec = *m_config->countries;
    const SourceSpec& source = *findSource(*m_config, spec.source);
    const QString file = spec.dataset + QStringLiteral(".gpkg");
    CountryOptions options;
    options.sourcePath = gdalPath(m_sources.filePath(source.file));
    options.outputPath = m_output.filePath(file);
    options.bufferDeg = spec.bufferDeg;
    options.simplifyDeg = spec.simplifyDeg;
    options.exclude = spec.exclude;
    options.progress = m_progress;
    if (!ts::packager::buildCountries(options, &m_report->countries, m_error)) {
      return false;
    }
    addDataset(spec.dataset, PackDataset::Kind::Areas, file, source);
    PackDataset& dataset = m_manifest.datasets.back();
    dataset.name = spec.name;
    dataset.licence = spec.licence;
    dataset.attribution = spec.attribution;
    return true;
  }

  /** Licence notices from the sources (inside them) and the repository. */
  bool copyNotices()
  {
    for (const SourceSpec& source : m_config->sources) {
      for (const QString& notice : source.notices) {
        const QByteArray from = (gdalPath(m_sources.filePath(source.file)) +
                                 QLatin1Char('/') + notice)
                                    .toUtf8();
        if (!copyNotice(source, notice, [&](const QString& target) {
              return copyGdalFile(from, target, m_error);
            })) {
          return false;
        }
      }
      for (const QString& notice : source.localNotices) {
        const QString from = QDir(m_options->configDirectory).filePath(notice);
        if (!copyNotice(source, notice, [&](const QString& target) {
              return QFile::copy(from, target) ||
                     fail(m_error, QStringLiteral("Cannot copy %1 into the "
                                                  "pack")
                                       .arg(from));
            })) {
          return false;
        }
      }
    }
    return true;
  }

  /** Copies @p notice of @p source to licences/<source id>/ with @p copy. */
  bool copyNotice(const SourceSpec& source, const QString& notice,
                  const std::function<bool(const QString&)>& copy)
  {
    const QString directory = QStringLiteral("licences/") + source.id;
    m_output.mkpath(directory);
    const QString target =
        directory + QLatin1Char('/') + QFileInfo(notice).fileName();
    if (!copy(m_output.filePath(target))) {
      return false;
    }
    m_manifest.notices.push_back(target);
    return true;
  }

  /** The manifest, listing every other file. */
  bool writeManifest()
  {
    m_progress(QStringLiteral("Writing the manifest"));
    QStringList files;
    QDirIterator iterator(m_options->outputDirectory, QDir::Files,
                          QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
      files.append(m_output.relativeFilePath(iterator.next()));
    }
    files.sort();
    for (const QString& file : std::as_const(files)) {
      PackFile entry;
      entry.path = file;
      entry.sizeBytes = QFileInfo(m_output.filePath(file)).size();
      entry.sha256 = fileSha256(m_output.filePath(file), m_notCancelled);
      m_report->totalBytes += entry.sizeBytes;
      m_manifest.files.push_back(entry);
    }
    const QByteArray json =
        QJsonDocument(m_manifest.toJson()).toJson(QJsonDocument::Indented);
    QFile manifestFile(m_output.filePath(PackManifest::fileName()));
    if (!manifestFile.open(QIODevice::WriteOnly) ||
        manifestFile.write(json) != json.size()) {
      return fail(m_error, QStringLiteral("Cannot write the manifest"));
    }
    manifestFile.close();
    m_report->totalBytes += manifestFile.size();
    return true;
  }

  bool checkBudget()
  {
    const qint64 budget = m_config->sizeLimitBytes - m_config->reservedBytes;
    if (m_report->totalBytes <= budget) {
      return true;
    }
    return fail(m_error,
                QStringLiteral("The pack is %1 bytes, over its budget of %2 "
                               "(limit %3 minus %4 reserved)")
                    .arg(m_report->totalBytes)
                    .arg(budget)
                    .arg(m_config->sizeLimitBytes)
                    .arg(m_config->reservedBytes));
  }

  const PackConfig* m_config;
  const BuildOptions* m_options;
  BuildReport* m_report;
  QString* m_error;
  QDir m_sources;
  QDir m_output;
  std::function<void(const QString&)> m_progress;
  PackManifest m_manifest;
  const std::atomic<bool> m_notCancelled{false};
};

} // namespace

bool buildPack(const PackConfig& config, const BuildOptions& options,
               BuildReport* report, QString* errorString)
{
  return PackBuild(config, options, report, errorString).run();
}

} // namespace ts::packager
