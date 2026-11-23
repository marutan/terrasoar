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

#include "packbuilder.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
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

#include <cpl_error.h>
#include <cpl_vsi.h>
#include <gdal.h>
#include <gdal_utils.h>

#include "ts/packs/packmanifest.h"
#include "ts/providers/geopackagefeatureprovider.h"
#include "ts/providers/sqlitetileimageryprovider.h"
#include "ts/tiling/tilekey.h"
#include "vpf/vpftable.h"

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

int32_t integer(const QJsonObject& json, const char* name, int32_t fallback)
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
    contents.append(buffer.data(), static_cast<int32_t>(read));
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
  for (int32_t j = 0; j < notices.size(); ++j) {
    source.notices.push_back(notices.at(j).toString());
  }
  const QJsonArray local =
      entry.value(QStringLiteral("localNotices")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t j = 0; j < local.size(); ++j) {
    source.localNotices.push_back(local.at(j).toString());
  }
  if (source.id.isEmpty() || source.file.isEmpty() ||
      source.file.contains(QLatin1Char('/')) || source.sha256.size() != 64) {
    return std::nullopt;
  }
  return source;
}

std::vector<QString> strings(const QJsonValue& value);

/** Reads the optional "geoids", "datumShifts" and "elevation" entries into
 *  @p config. */
bool readElevation(const QJsonObject& json, PackConfig* config,
                   QString* errorString)
{
  const QJsonArray geoids = json.value(QStringLiteral("geoids")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < geoids.size(); ++i) {
    const QJsonObject entry = geoids.at(i).toObject();
    GeoidSpec spec;
    spec.dataset = text(entry, "dataset");
    spec.source = text(entry, "source");
    spec.name = text(entry, "name");
    spec.licence = text(entry, "licence");
    spec.attribution = text(entry, "attribution");
    spec.step = integer(entry, "step", 1);
    spec.replaces = text(entry, "replaces");
    spec.copy = entry.value(QStringLiteral("copy")).toBool(false);
    if (findSource(*config, spec.source) == nullptr || spec.dataset.isEmpty() ||
        spec.step < 1) {
      return fail(errorString, QStringLiteral("Bad geoid entry %1").arg(i));
    }
    config->geoids.push_back(spec);
  }
  const QJsonArray shifts = json.value(QStringLiteral("datumShifts")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < shifts.size(); ++i) {
    const QJsonObject entry = shifts.at(i).toObject();
    DatumShiftSpec spec;
    spec.dataset = text(entry, "dataset");
    spec.source = text(entry, "source");
    spec.name = text(entry, "name");
    spec.licence = text(entry, "licence");
    spec.attribution = text(entry, "attribution");
    if (findSource(*config, spec.source) == nullptr || spec.dataset.isEmpty()) {
      return fail(errorString,
                  QStringLiteral("Bad datum shift entry %1").arg(i));
    }
    config->datumShifts.push_back(spec);
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
  spec.replaces = text(elevation, "replaces");
  spec.layerName = text(elevation, "layer");
  spec.minLevel = integer(elevation, "minLevel", 0);
  const QJsonArray region = elevation.value(QStringLiteral("region")).toArray();
  if (region.size() == 2) {
    spec.westDeg = region.at(0).toDouble();
    spec.eastDeg = region.at(1).toDouble();
  }
  spec.members = strings(elevation.value(QStringLiteral("members")));
  // A pack's own geoid, or the base pack's (a finer DEM or a regional one
  // installed with it).
  const bool datumKnown =
      spec.verticalDatum.isEmpty() ||
      spec.verticalDatum == QLatin1String("egm2008") ||
      spec.verticalDatum == QLatin1String("egm96") ||
      std::any_of(config->geoids.begin(), config->geoids.end(),
                  [&spec](const GeoidSpec& geoid) {
                    return geoid.dataset == spec.verticalDatum;
                  });
  if (findSource(*config, spec.source) == nullptr || spec.dataset.isEmpty() ||
      spec.maxLevel < 0 || spec.maxLevel > 10 || spec.samples < 3 ||
      spec.minLevel < 0 || spec.minLevel > spec.maxLevel ||
      (!region.isEmpty() && region.size() != 2) ||
      (!spec.replaces.isEmpty() && !spec.layerName.isEmpty()) || !datumKnown) {
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
  for (int32_t i = 0; i < exclude.size(); ++i) {
    spec.exclude.push_back(exclude.at(i).toString());
  }
  if (findSource(*config, spec.source) == nullptr || spec.dataset.isEmpty() ||
      !(spec.bufferDeg > spec.simplifyDeg) || spec.simplifyDeg < 0.0) {
    return fail(errorString, QStringLiteral("Bad countries entry"));
  }
  config->countries = spec;
  return true;
}

/** An imagery entry of @p config's; nothing if it can't be used. */
std::optional<ImagerySpec> readImagery(const PackConfig& config,
                                       const QJsonObject& entry)
{
  ImagerySpec spec;
  spec.dataset = text(entry, "dataset");
  spec.source = text(entry, "source");
  spec.name = text(entry, "name");
  spec.licence = text(entry, "licence");
  spec.attribution = text(entry, "attribution");
  spec.replaces = text(entry, "replaces");
  spec.variantOf = text(entry, "variantOf");
  spec.variant = text(entry, "variant");
  spec.member = text(entry, "member");
  spec.layerName = text(entry, "layer");
  spec.tiling.maxLevel = integer(entry, "maxLevel", 4);
  spec.tiling.tileSizePx = integer(entry, "tileSize", 256);
  spec.tiling.jpegQuality = integer(entry, "jpegQuality", 85);
  if (findSource(config, spec.source) == nullptr || spec.dataset.isEmpty() ||
      spec.tiling.maxLevel < 0 || spec.tiling.maxLevel > 8 ||
      spec.tiling.tileSizePx < 16 ||
      spec.variantOf.isEmpty() != spec.variant.isEmpty()) {
    return std::nullopt;
  }
  return spec;
}

std::vector<QString> strings(const QJsonValue& value)
{
  std::vector<QString> result;
  const QJsonArray array = value.toArray();
  result.reserve(static_cast<std::size_t>(array.size()));
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < array.size(); ++i) {
    result.push_back(array.at(i).toString());
  }
  return result;
}

/** Reads one VMAP0 feature class; nothing if it is malformed. */
std::optional<Vmap0Class> readVmap0Class(const QJsonObject& entry)
{
  Vmap0Class cls;
  cls.coverage = text(entry, "coverage");
  cls.featureClass = text(entry, "class");
  cls.table = text(entry, "table");
  cls.minLevel = integer(entry, "minLevel", 0);
  cls.maxLevel = integer(entry, "maxLevel", -1);
  const QJsonArray attributes =
      entry.value(QStringLiteral("attributes")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < attributes.size(); ++i) {
    const QJsonObject item = attributes.at(i).toObject();
    Vmap0Attribute attribute;
    attribute.column = text(item, "column");
    attribute.name = text(item, "name");
    const std::optional<Vmap0Attribute::Kind> kind =
        vmap0AttributeKind(text(item, "kind"));
    if (attribute.column.isEmpty() || attribute.name.isEmpty() || !kind) {
      return std::nullopt;
    }
    attribute.kind = *kind;
    cls.attributes.push_back(attribute);
  }
  const QJsonArray filters = entry.value(QStringLiteral("filters")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < filters.size(); ++i) {
    const QJsonObject item = filters.at(i).toObject();
    Vmap0Filter filter;
    filter.column = text(item, "column");
    filter.values = strings(item.value(QStringLiteral("values")));
    filter.exclude = item.value(QStringLiteral("exclude")).toBool(false);
    if (filter.column.isEmpty() || filter.values.empty()) {
      return std::nullopt;
    }
    cls.filters.push_back(filter);
  }
  if (cls.coverage.isEmpty() || cls.featureClass.isEmpty() ||
      cls.table.isEmpty() || cls.minLevel < 0) {
    return std::nullopt;
  }
  return cls;
}

bool readLods(const QJsonObject& entry, std::vector<FeatureLod>* result,
              QString* errorString);

/** Reads the optional "vmap0" entry into @p config. */
bool readVmap0(const QJsonObject& json, PackConfig* config,
               QString* errorString)
{
  const QJsonObject entry = json.value(QStringLiteral("vmap0")).toObject();
  if (entry.isEmpty()) {
    return true;
  }
  Vmap0Spec spec;
  spec.dataset = text(entry, "dataset");
  spec.sources = strings(entry.value(QStringLiteral("sources")));
  spec.name = text(entry, "name");
  spec.licence = text(entry, "licence");
  spec.attribution = text(entry, "attribution");
  spec.sourceName = text(entry, "sourceName");
  spec.sourceVersion = text(entry, "sourceVersion");
  spec.sourceUrl = text(entry, "sourceUrl");
  spec.libraries = strings(entry.value(QStringLiteral("libraries")));
  spec.layers = entry.value(QStringLiteral("layers")).toArray();
  if (!readLods(entry, &spec.lods, errorString)) {
    return false;
  }
  const QJsonArray classes = entry.value(QStringLiteral("classes")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < classes.size(); ++i) {
    const std::optional<Vmap0Class> cls =
        readVmap0Class(classes.at(i).toObject());
    if (!cls) {
      return fail(errorString, QStringLiteral("Bad VMAP0 class %1").arg(i));
    }
    spec.classes.push_back(*cls);
  }
  bool sourcesKnown = !spec.sources.empty();
  for (const QString& source : spec.sources) {
    sourcesKnown = sourcesKnown && findSource(*config, source) != nullptr;
  }
  if (!sourcesKnown || spec.dataset.isEmpty() || spec.lods.empty() ||
      spec.classes.empty()) {
    return fail(errorString, QStringLiteral("Bad vmap0 entry"));
  }
  config->vmap0 = spec;
  return true;
}

/** Reads the levels of detail of a "vmap0" or "vector" entry. */
bool readLods(const QJsonObject& entry, std::vector<FeatureLod>* result,
              QString* errorString)
{
  const QJsonArray lods = entry.value(QStringLiteral("lods")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < lods.size(); ++i) {
    const QJsonObject item = lods.at(i).toObject();
    FeatureLod lod;
    lod.storageLevel = integer(item, "storageLevel", 0);
    lod.minLevel = integer(item, "minLevel", lod.storageLevel);
    lod.maxLevel = integer(item, "maxLevel", -1);
    lod.toleranceDeg = item.value(QStringLiteral("toleranceDeg")).toDouble(0.0);
    if (lod.storageLevel < 0 || lod.storageLevel > 12 ||
        lod.minLevel < lod.storageLevel || lod.toleranceDeg < 0.0 ||
        (lod.maxLevel >= 0 && lod.maxLevel < lod.minLevel)) {
      return fail(errorString, QStringLiteral("Bad level of detail %1").arg(i));
    }
    result->push_back(lod);
  }
  return !result->empty() ||
         fail(errorString, QStringLiteral("No levels of detail"));
}

/** True if @p text is a number. */
bool numeric(const QString& text)
{
  bool ok = false;
  text.toDouble(&ok);
  return ok;
}

/** Reads one attribute of a "vector" class; nothing if it is malformed. */
std::optional<VectorAttribute> readVectorAttribute(const QJsonObject& item)
{
  VectorAttribute attribute;
  attribute.column = text(item, "column");
  attribute.name = text(item, "name");
  attribute.value = text(item, "value");
  const QString kind = text(item, "kind");
  attribute.number = kind == QLatin1String("number");
  if (attribute.name.isEmpty() ||
      (attribute.column.isEmpty() == attribute.value.isEmpty()) ||
      (!kind.isEmpty() && kind != QLatin1String("text") && !attribute.number) ||
      (attribute.number && attribute.column.isEmpty() &&
       !numeric(attribute.value))) {
    return std::nullopt;
  }
  return attribute;
}

/** Reads one "vector" class into @p spec; false if it is malformed. */
bool readVectorClass(const PackConfig& config, const QJsonObject& entry,
                     std::size_t lodCount, VectorSpec* spec)
{
  VectorClass cls;
  const QString source = text(entry, "source");
  cls.layer = text(entry, "layer");
  cls.where = text(entry, "where");
  cls.table = text(entry, "table");
  cls.minLevel = integer(entry, "minLevel", 0);
  cls.maxLevel = integer(entry, "maxLevel", -1);
  cls.dimension = vectorDimension(text(entry, "geometry"));
  cls.dissolve = entry.value(QStringLiteral("dissolve")).toBool(false);
  const std::vector<QString> open =
      strings(entry.value(QStringLiteral("open")));
  cls.openOptions = QStringList(open.begin(), open.end());
  const std::vector<QString> line =
      strings(entry.value(QStringLiteral("lineColumns")));
  cls.lineColumns = QStringList(line.begin(), line.end());
  const QJsonArray lods = entry.value(QStringLiteral("lods")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < lods.size(); ++i) {
    const int32_t index = lods.at(i).toInt(-1);
    if (index < 0 || static_cast<std::size_t>(index) >= lodCount) {
      return false;
    }
    cls.lods.push_back(index);
  }
  const QJsonArray attributes =
      entry.value(QStringLiteral("attributes")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < attributes.size(); ++i) {
    const std::optional<VectorAttribute> attribute =
        readVectorAttribute(attributes.at(i).toObject());
    if (!attribute) {
      return false;
    }
    cls.attributes.push_back(*attribute);
  }
  if (findSource(config, source) == nullptr || cls.table.isEmpty() ||
      cls.minLevel < 0 || cls.dimension < 0 ||
      (!cls.lineColumns.isEmpty() &&
       (cls.lineColumns.size() != 4 || cls.dimension != 1))) {
    return false;
  }
  spec->classes.push_back(cls);
  spec->classSources.push_back(source);
  spec->classMembers.push_back(text(entry, "member"));
  spec->classDrivers.push_back(text(entry, "driver"));
  return true;
}

/** Reads the optional "vector" entry into @p config. */
bool readVector(const QJsonObject& json, PackConfig* config,
                QString* errorString)
{
  const QJsonObject entry = json.value(QStringLiteral("vector")).toObject();
  if (entry.isEmpty()) {
    return true;
  }
  VectorSpec spec;
  spec.dataset = text(entry, "dataset");
  spec.name = text(entry, "name");
  spec.licence = text(entry, "licence");
  spec.attribution = text(entry, "attribution");
  spec.sourceName = text(entry, "sourceName");
  spec.sourceVersion = text(entry, "sourceVersion");
  spec.sourceUrl = text(entry, "sourceUrl");
  spec.layers = entry.value(QStringLiteral("layers")).toArray();
  if (!readLods(entry, &spec.lods, errorString)) {
    return false;
  }
  const QJsonArray classes = entry.value(QStringLiteral("classes")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < classes.size(); ++i) {
    if (!readVectorClass(*config, classes.at(i).toObject(), spec.lods.size(),
                         &spec)) {
      return fail(errorString, QStringLiteral("Bad vector class %1").arg(i));
    }
  }
  if (spec.dataset.isEmpty() || spec.classes.empty()) {
    return fail(errorString, QStringLiteral("Bad vector entry"));
  }
  config->vector = spec;
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
  for (int32_t i = 0; i < sources.size(); ++i) {
    std::optional<SourceSpec> source = readSource(sources.at(i).toObject());
    if (!source) {
      fail(errorString, QStringLiteral("Bad source entry %1").arg(i));
      return std::nullopt;
    }
    config.sources.push_back(std::move(*source));
  }

  const QJsonObject imagery = json.value(QStringLiteral("imagery")).toObject();
  if (!imagery.isEmpty()) {
    const std::optional<ImagerySpec> spec = readImagery(config, imagery);
    if (!spec) {
      fail(errorString, QStringLiteral("Bad imagery entry"));
      return std::nullopt;
    }
    config.imagery = spec;
  }
  const QJsonArray variants =
      json.value(QStringLiteral("imageryVariants")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < variants.size(); ++i) {
    const std::optional<ImagerySpec> spec =
        readImagery(config, variants.at(i).toObject());
    if (!spec || spec->variantOf.isEmpty()) {
      fail(errorString, QStringLiteral("Bad imagery variant %1").arg(i));
      return std::nullopt;
    }
    config.imageryVariants.push_back(*spec);
  }
  const QJsonArray own =
      json.value(QStringLiteral("imageryDatasets")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < own.size(); ++i) {
    const std::optional<ImagerySpec> spec =
        readImagery(config, own.at(i).toObject());
    if (!spec || !spec->replaces.isEmpty() || !spec->variantOf.isEmpty() ||
        spec->layerName.isEmpty()) {
      fail(errorString, QStringLiteral("Bad imagery dataset %1 (one of the "
                                       "pack's own needs a layer, and "
                                       "replaces nothing)")
                            .arg(i));
      return std::nullopt;
    }
    config.imageryDatasets.push_back(*spec);
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
    for (int32_t i = 0; i < lods.size(); ++i) {
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
      !readElevation(json, &config, errorString) ||
      !readVmap0(json, &config, errorString) ||
      !readVector(json, &config, errorString)) {
    return std::nullopt;
  }
  // Regional packs of one theme share one name in Add Layer.
  config.name = text(json, "layerGroup");
  if (config.name.isEmpty()) {
    config.name = text(json.value(QStringLiteral("index")).toObject(), "name");
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
  QJsonArray vmap0Json;
  for (const Vmap0ClassStats& cls : vmap0) {
    vmap0Json.append(cls.toJson());
  }
  json.insert(QStringLiteral("vmap0"), vmap0Json);
  QJsonArray vectorJson;
  for (const FeatureClassStats& cls : vector) {
    vectorJson.append(cls.toJson());
  }
  json.insert(QStringLiteral("vector"), vectorJson);
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
    m_manifest.name = config.name;
  }

  bool run()
  {
    return checkSources() && prepareOutput() && buildImagery() &&
           buildShoreline() && buildCountries() && buildGeoids() &&
           buildDatumShifts() && buildElevation() && buildVmap0() &&
           buildVector() && copyNotices() && writeManifest() && checkBudget();
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
    if (m_config->imagery && !buildImagery(*m_config->imagery)) {
      return false;
    }
    const auto build = [this](const ImagerySpec& spec) {
      return buildImagery(spec);
    };
    return std::all_of(m_config->imageryVariants.begin(),
                       m_config->imageryVariants.end(), build) &&
           std::all_of(m_config->imageryDatasets.begin(),
                       m_config->imageryDatasets.end(), build);
  }

  bool buildImagery(const ImagerySpec& spec)
  {
    const SourceSpec& source = *findSource(*m_config, spec.source);
    const QString file = spec.dataset + QStringLiteral(".sqlite");
    const int32_t width =
        spec.tiling.tileSizePx *
        static_cast<int32_t>(TileKey::columns(spec.tiling.maxLevel));
    m_progress(QStringLiteral("Reading %1 into a %2 × %3 mosaic")
                   .arg(source.file)
                   .arg(width)
                   .arg(width / 2));
    const QString path =
        spec.member.isEmpty()
            ? gdalPath(m_sources.filePath(source.file))
            : QStringLiteral("/vsizip/%1/%2")
                  .arg(QFileInfo(m_sources.filePath(source.file))
                           .absoluteFilePath(),
                       spec.member);
    const QImage mosaic = readEquirectangular(path, width, width / 2, m_error);
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
    ImageryTilerStats stats;
    if (!writer.errorString().isEmpty() ||
        !tileMosaic(mosaic, tiling, &writer, &stats, m_error) ||
        !writer.finish()) {
      return fail(m_error,
                  m_error->isEmpty() ? writer.errorString() : *m_error);
    }
    m_report->imagery.tiles += stats.tiles;
    m_report->imagery.bytes += stats.bytes;
    addDataset(spec.dataset, PackDataset::Kind::Imagery, file, source);
    PackDataset& dataset = m_manifest.datasets.back();
    dataset.name = spec.name;
    dataset.licence = spec.licence;
    dataset.attribution = spec.attribution;
    dataset.replaces = spec.replaces;
    dataset.variantOf = spec.variantOf;
    dataset.variant = spec.variant;
    if (!spec.layerName.isEmpty()) {
      QJsonObject layerSource;
      layerSource.insert(QStringLiteral("kind"), QStringLiteral("basepack"));
      layerSource.insert(QStringLiteral("dataset"), spec.dataset);
      QJsonObject definition;
      definition.insert(QStringLiteral("id"),
                        QStringLiteral("pack:%1:image").arg(spec.dataset));
      definition.insert(QStringLiteral("origin"), QStringLiteral("catalogue"));
      definition.insert(QStringLiteral("type"), QStringLiteral("imagery"));
      definition.insert(QStringLiteral("name"), spec.layerName);
      definition.insert(QStringLiteral("source"), layerSource);
      m_manifest.layers.append(definition);
    }
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
      if (spec.copy) {
        if (!QFile::copy(m_sources.filePath(source.file),
                         m_output.filePath(file))) {
          return fail(
              m_error,
              QStringLiteral("Cannot copy %1 into the pack").arg(source.file));
        }
      } else {
        GeoidGridOptions options;
        options.sourcePath = m_sources.filePath(source.file);
        options.outputPath = m_output.filePath(file);
        options.step = spec.step;
        GeoidGridStats stats;
        if (!buildGeoidGrid(options, &stats, m_error)) {
          return false;
        }
      }
      addDataset(spec.dataset, PackDataset::Kind::Geoid, file, source);
      PackDataset& dataset = m_manifest.datasets.back();
      dataset.name = spec.name;
      dataset.licence = spec.licence;
      dataset.attribution = spec.attribution;
      dataset.replaces = spec.replaces;
    }
    return true;
  }

  bool buildDatumShifts()
  {
    for (const DatumShiftSpec& spec : m_config->datumShifts) {
      const SourceSpec& source = *findSource(*m_config, spec.source);
      const QString file = spec.dataset + QStringLiteral(".tif");
      m_progress(QStringLiteral("Copying the datum shift grid %1").arg(file));
      if (!QFile::copy(m_sources.filePath(source.file),
                       m_output.filePath(file))) {
        return fail(
            m_error,
            QStringLiteral("Cannot copy %1 into the pack").arg(source.file));
      }
      PackDataset shift;
      shift.id = spec.dataset;
      shift.file = file;
      shift.name = spec.name;
      shift.licence = spec.licence;
      shift.attribution = spec.attribution;
      shift.source = packSource(source);
      m_manifest.datumShifts.push_back(shift);
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
    if (!spec.members.empty()) {
      // One DEM in several files of a zip (GEBCO's tiles): a mosaic.
      options.sourcePath = mosaic(spec, gdalPath(options.sourcePath));
      if (options.sourcePath.isEmpty()) {
        return false;
      }
    }
    options.outputPath = m_output.filePath(file);
    options.maxLevel = spec.maxLevel;
    options.minLevel = spec.minLevel;
    options.westDeg = spec.westDeg;
    options.eastDeg = spec.eastDeg;
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
    dataset.replaces = spec.replaces;
    if (!spec.layerName.isEmpty()) {
      QJsonObject layerSource;
      layerSource.insert(QStringLiteral("kind"), QStringLiteral("basepack"));
      layerSource.insert(QStringLiteral("dataset"), spec.dataset);
      QJsonObject definition;
      definition.insert(QStringLiteral("id"),
                        QStringLiteral("pack:%1:terrain").arg(spec.dataset));
      definition.insert(QStringLiteral("origin"), QStringLiteral("catalogue"));
      definition.insert(QStringLiteral("type"), QStringLiteral("elevation"));
      definition.insert(QStringLiteral("name"), spec.layerName);
      definition.insert(QStringLiteral("source"), layerSource);
      definition.insert(QStringLiteral("style"),
                        QJsonObject{{QStringLiteral("exaggeration"), 1}});
      m_manifest.layers.append(definition);
    }
    return true;
  }

  /** A VRT mosaic of @p spec's members in @p zip, in GDAL's memory. */
  QString mosaic(const ElevationSpec& spec, const QString& zip)
  {
    std::vector<QByteArray> paths;
    paths.reserve(spec.members.size());
    for (const QString& member : spec.members) {
      paths.push_back((zip + QLatin1Char('/') + member).toUtf8());
    }
    std::vector<const char*> names;
    names.reserve(paths.size());
    for (const QByteArray& path : paths) {
      names.push_back(path.constData());
    }
    const QByteArray target =
        QStringLiteral("/vsimem/%1.vrt").arg(spec.dataset).toUtf8();
    int failed = 0;
    GDALDatasetH vrt =
        GDALBuildVRT(target.constData(), static_cast<int>(names.size()),
                     nullptr, names.data(), nullptr, &failed);
    if (vrt == nullptr || failed != 0) {
      fail(m_error, QStringLiteral("Cannot mosaic the DEM's files: %1")
                        .arg(QString::fromUtf8(CPLGetLastErrorMsg())));
      return QString();
    }
    GDALClose(vrt);
    return QString::fromUtf8(target);
  }

  bool buildVmap0()
  {
    if (!m_config->vmap0) {
      return true;
    }
    const Vmap0Spec& spec = *m_config->vmap0;
    const QString file = spec.dataset + QStringLiteral(".gpkg");
    Vmap0Options options;
    for (const QString& id : spec.sources) {
      const SourceSpec& source = *findSource(*m_config, id);
      const QString root = gdalPath(m_sources.filePath(source.file));
      const QString database = vpf::findEntry(root, QStringLiteral("vmaplv0"));
      if (database.isEmpty()) {
        return fail(m_error,
                    QStringLiteral("%1 has no VMAPLV0 directory").arg(root));
      }
      options.databases.push_back(database);
    }
    options.libraries = spec.libraries;
    options.lods = spec.lods;
    options.classes = spec.classes;
    options.progress = m_progress;
    GeoPackageFeatureWriter writer(m_output.filePath(file), false);
    if (!writer.errorString().isEmpty() ||
        !ts::packager::buildVmap0(options, &writer, &m_report->vmap0,
                                  m_error) ||
        !writer.finish()) {
      return fail(m_error,
                  m_error->isEmpty() ? writer.errorString() : *m_error);
    }
    // Every feature read is written, left out on purpose, or reported.
    for (const Vmap0ClassStats& cls : m_report->vmap0) {
      if (cls.skipped != 0) {
        return fail(m_error,
                    QStringLiteral("%1: %2 features without usable geometry")
                        .arg(cls.table)
                        .arg(cls.skipped));
      }
    }
    addFeatureDataset(spec, file);
    return true;
  }

  bool buildVector()
  {
    if (!m_config->vector) {
      return true;
    }
    const VectorSpec& spec = *m_config->vector;
    const QString file = spec.dataset + QStringLiteral(".gpkg");
    VectorOptions options;
    options.lods = spec.lods;
    options.classes = spec.classes;
    options.progress = m_progress;
    for (std::size_t c = 0; c < options.classes.size(); ++c) {
      const SourceSpec& source = *findSource(*m_config, spec.classSources[c]);
      const QString path = m_sources.filePath(source.file);
      options.classes[c].path =
          spec.classMembers[c].isEmpty()
              ? path
              : QStringLiteral("/vsizip/%1/%2")
                    .arg(QFileInfo(path).absoluteFilePath(),
                         spec.classMembers[c]);
      if (!spec.classDrivers[c].isEmpty()) {
        options.classes[c].path =
            spec.classDrivers[c] + QLatin1Char(':') + options.classes[c].path;
      }
    }
    GeoPackageFeatureWriter writer(m_output.filePath(file), false);
    if (!writer.errorString().isEmpty() ||
        !ts::packager::buildVector(options, &writer, &m_report->vector,
                                   m_error) ||
        !writer.finish()) {
      return fail(m_error,
                  m_error->isEmpty() ? writer.errorString() : *m_error);
    }
    for (const FeatureClassStats& cls : m_report->vector) {
      if (cls.skipped != 0) {
        return fail(m_error,
                    QStringLiteral("%1: %2 features without usable geometry")
                        .arg(cls.table)
                        .arg(cls.skipped));
      }
    }
    addFeatureDataset(spec, file);
    return true;
  }

  /**
   * Adds the vector dataset of @p spec (a Vmap0Spec or VectorSpec), of the
   * pack's own, and its layers to the manifest.
   */
  template <typename Spec>
  void addFeatureDataset(const Spec& spec, const QString& file)
  {
    PackDataset dataset;
    dataset.id = spec.dataset;
    dataset.kind = PackDataset::Kind::Features;
    dataset.file = file;
    dataset.name = spec.name;
    dataset.licence = spec.licence;
    dataset.attribution = spec.attribution;
    dataset.source.name = spec.sourceName;
    dataset.source.version = spec.sourceVersion;
    dataset.source.url = spec.sourceUrl;
    m_manifest.datasets.push_back(dataset);
    // The pack's layers, on its dataset.
    // NOLINTNEXTLINE(modernize-loop-convert)
    for (int32_t i = 0; i < spec.layers.size(); ++i) {
      const QJsonObject layer = spec.layers.at(i).toObject();
      QJsonObject source;
      source.insert(QStringLiteral("kind"), QStringLiteral("basepack"));
      source.insert(QStringLiteral("dataset"), spec.dataset);
      source.insert(QStringLiteral("tables"),
                    layer.value(QStringLiteral("tables")));
      if (layer.contains(QStringLiteral("styleRules"))) {
        source.insert(QStringLiteral("styleRules"),
                      layer.value(QStringLiteral("styleRules")));
      }
      QJsonObject definition;
      definition.insert(
          QStringLiteral("id"),
          QStringLiteral("pack:%1:%2").arg(spec.dataset, text(layer, "id")));
      definition.insert(QStringLiteral("origin"), QStringLiteral("catalogue"));
      definition.insert(QStringLiteral("type"), QStringLiteral("vector"));
      definition.insert(QStringLiteral("name"), text(layer, "name"));
      definition.insert(QStringLiteral("source"), source);
      definition.insert(QStringLiteral("style"),
                        layer.value(QStringLiteral("style")).toObject());
      m_manifest.layers.append(definition);
    }
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
