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
 * Builds a data pack from its configuration and downloaded sources.
 */
#ifndef TS_PACKAGER_PACKBUILDER_H
#define TS_PACKAGER_PACKBUILDER_H

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

#include "countrybuilder.h"
#include "elevationbuilder.h"
#include "imagerytiler.h"
#include "shorelinebuilder.h"
#include "vectorbuilder.h"
#include "vmap0builder.h"

namespace ts::packager
{

/** One downloaded source file. */
struct SourceSpec {
  /** Identifier used by the datasets, e.g. "gshhg". */
  QString id;
  /** File name in the sources directory. */
  QString file;
  /** Download URL. */
  QString url;
  /** Expected SHA-256, lower-case hexadecimal. */
  QString sha256;
  /** Product name. */
  QString name;
  /** Product version. */
  QString version;
  /** Licence and notice files inside the source (a zip) to ship. */
  std::vector<QString> notices;
  /**
   * Further notice files to ship with this source, relative to the
   * configuration file (e.g. a licence text the source refers to but
   * does not include).
   */
  std::vector<QString> localNotices;
};

/** The imagery dataset. */
struct ImagerySpec {
  /** Dataset id in the manifest. */
  QString dataset;
  /** Source id. */
  QString source;
  /** Name for the user. */
  QString name;
  /** Licence. */
  QString licence;
  /** Attribution. */
  QString attribution;
  /** Base pack dataset it replaces (PackDataset::replaces), or empty. */
  QString replaces;
  /** Dataset it is a variant of (PackDataset::variantOf), or empty. */
  QString variantOf;
  /** Which variant (PackDataset::variant). */
  QString variant;
  /**
   * The image's file inside the source, if it is a zip (Natural Earth's
   * rasters); empty for a source that is the image itself.
   */
  QString member;
  /**
   * For a dataset of the pack's own ("imageryDatasets"): the name of the
   * imagery layer the manifest offers on it; empty for none.
   */
  QString layerName;
  /** Tiling. */
  ImageryTilerOptions tiling;
};

/** The shoreline dataset. */
struct ShorelineSpec {
  /** Dataset id in the manifest. */
  QString dataset;
  /** Source id (a GSHHG shapefile zip). */
  QString source;
  /** Name for the user. */
  QString name;
  /** Licence. */
  QString licence;
  /** Attribution. */
  QString attribution;
  /** Base pack dataset it replaces (PackDataset::replaces), or empty. */
  QString replaces;
  /** Levels of detail. */
  std::vector<ShorelineLod> lods;
  /** Smallest polygon kept, km². */
  double minimumAreaKm2 = 0.0;
};

/** The global DEM of the pack. */
struct ElevationSpec {
  /** Dataset id in the manifest. */
  QString dataset;
  /** Source id (a global DEM such as ETOPO 2022). */
  QString source;
  /** Name for the user. */
  QString name;
  /** Licence. */
  QString licence;
  /** Attribution. */
  QString attribution;
  /** Deepest level stored. */
  int32_t maxLevel = 6;
  /** Samples along a tile's side. */
  int32_t samples = 65;
  /** The heights' vertical datum: a geoid dataset of the pack, or empty. */
  QString verticalDatum;
};

/** A geoid grid of the pack. */
struct GeoidSpec {
  /** Dataset id in the manifest (e.g. "egm2008"). */
  QString dataset;
  /** Source id (a PROJ GeoTIFF grid). */
  QString source;
  /** Name for the user. */
  QString name;
  /** Licence. */
  QString licence;
  /** Attribution. */
  QString attribution;
  /** Keep every step-th sample. */
  int32_t step = 1;
  /** The base pack dataset it takes the place of, or empty. */
  QString replaces;
  /**
   * True to ship the source grid as it is (small regional grids, which
   * may have no-data cells), false to write it as 16-bit centimetres.
   */
  bool copy = false;
};

/**
 * A horizontal datum shift grid of the pack (PROJ GeoTIFF, such as
 * OSTN15), copied as it is and listed in the manifest's "datumShifts".
 */
struct DatumShiftSpec {
  /** Its id for the projections (e.g. "ostn15"); the file is <id>.tif. */
  QString dataset;
  /** Source id. */
  QString source;
  /** Name for the user. */
  QString name;
  /** Licence. */
  QString licence;
  /** Attribution. */
  QString attribution;
};

/** The country outlines of the pack. */
struct CountriesSpec {
  /** Dataset id in the manifest. */
  QString dataset;
  /** Source id (Natural Earth admin-0 countries). */
  QString source;
  /** Name for the user. */
  QString name;
  /** Licence. */
  QString licence;
  /** Attribution. */
  QString attribution;
  /** See CountryOptions. */
  double bufferDeg = 0.05;
  /** See CountryOptions. */
  double simplifyDeg = 0.02;
  /** See CountryOptions. */
  std::vector<QString> exclude;
};

/** A VMAP0 dataset (Phase 21): feature classes into one GeoPackage. */
struct Vmap0Spec {
  /** Dataset id in the manifest, e.g. "vmap0-transportation". */
  QString dataset;
  /** Source ids: one zip per VMAP0 library CD. */
  std::vector<QString> sources;
  /** Name for the user. */
  QString name;
  /** Licence. */
  QString licence;
  /** Attribution. */
  QString attribution;
  /** The source as the manifest names it (all the zips together). */
  QString sourceName;
  /** Its version. */
  QString sourceVersion;
  /** Where it is published. */
  QString sourceUrl;
  /** Libraries used (empty: all but the reference library). */
  std::vector<QString> libraries;
  /** Levels of detail. */
  std::vector<Vmap0Lod> lods;
  /** Feature classes. */
  std::vector<Vmap0Class> classes;
  /**
   * The layers the pack offers ("layers"): each `name`, `tables`, `style`
   * and optional `styleRules`, made into layer definitions on this
   * dataset in the manifest.
   */
  QJsonArray layers;
};

/** A vector dataset read through GDAL/OGR ("vector"). */
struct VectorSpec {
  /** Dataset id in the manifest, e.g. "naturalearth-10m". */
  QString dataset;
  /** Name for the user. */
  QString name;
  /** Licence. */
  QString licence;
  /** Attribution. */
  QString attribution;
  /** The source as the manifest names it. */
  QString sourceName;
  /** Its version. */
  QString sourceVersion;
  /** Where it is published. */
  QString sourceUrl;
  /** Levels of detail. */
  std::vector<FeatureLod> lods;
  /**
   * The classes, with their paths still the source ids and members
   * ("source", "member"): resolved against the sources directory when
   * built.
   */
  std::vector<VectorClass> classes;
  /** Each class's source id. */
  std::vector<QString> classSources;
  /** Each class's file inside its source, if the source is a zip. */
  std::vector<QString> classMembers;
  /**
   * Each class's GDAL driver, if it must be named ("CSV" for the names
   * files, which are .txt): the path is then given as "<driver>:<path>".
   */
  std::vector<QString> classDrivers;
  /** The layers the pack offers, as for VMAP0. */
  QJsonArray layers;
};

/** The whole pack configuration (tools/packager/basepack.json). */
struct PackConfig {
  /** Pack id. */
  QString id;
  /** Pack version. */
  QString version;
  /** Most bytes the pack may have, everything included. */
  qint64 sizeLimitBytes = 0;
  /** Bytes of the limit kept free for later contents. */
  qint64 reservedBytes = 0;
  /** Downloaded sources. */
  std::vector<SourceSpec> sources;
  /** Imagery dataset, if any. */
  std::optional<ImagerySpec> imagery;
  /**
   * Further imagery datasets ("imageryVariants"), each a variant of a base
   * pack dataset (Blue Marble's months).
   */
  std::vector<ImagerySpec> imageryVariants;
  /**
   * Imagery datasets of the pack's own ("imageryDatasets"), neither
   * replacing nor a variant of a base pack dataset, each with its layer
   * (Black Marble, Natural Earth's rasters).
   */
  std::vector<ImagerySpec> imageryDatasets;
  /** Shoreline dataset, if any. */
  std::optional<ShorelineSpec> shoreline;
  /** Country outlines, if any. */
  std::optional<CountriesSpec> countries;
  /** Global DEM, if any. */
  std::optional<ElevationSpec> elevation;
  /** Geoid grids. */
  std::vector<GeoidSpec> geoids;
  /** Datum shift grids. */
  std::vector<DatumShiftSpec> datumShifts;
  /** A VMAP0 dataset, if any. */
  std::optional<Vmap0Spec> vmap0;
  /** A vector dataset read through OGR, if any. */
  std::optional<VectorSpec> vector;
  /**
   * The pack's name in the manifest, under which Add Layer lists its
   * layers: "layerGroup" (regional packs of one theme share it), else the
   * index entry's name.
   */
  QString name;

  /** Parses the configuration; nothing and @p errorString on failure. */
  static std::optional<PackConfig> fromJson(const QJsonObject& json,
                                            QString* errorString);
  /** Reads the configuration file at @p path. */
  static std::optional<PackConfig> read(const QString& path,
                                        QString* errorString);
};

/** How to run. */
struct BuildOptions {
  /** Directory with the downloaded source files. */
  QString sourcesDirectory;
  /** Directory of the configuration file (for local notices). */
  QString configDirectory;
  /** Directory to create the pack in (must not exist or be empty). */
  QString outputDirectory;
  /** Check the sources' SHA-256 against the configuration. */
  bool verifySources = true;
  /** Called with progress messages; may be empty. */
  std::function<void(const QString&)> progress;
};

/** What was built. */
struct BuildReport {
  /** Total bytes of the pack. */
  qint64 totalBytes = 0;
  /** Imagery results. */
  ImageryTilerStats imagery;
  /** Shoreline results per level of detail. */
  std::vector<ShorelineLodStats> shoreline;
  /** Country outline results. */
  CountryStats countries;
  /** DEM results. */
  ElevationStats elevation;
  /** VMAP0 results per feature class. */
  std::vector<Vmap0ClassStats> vmap0;
  /** Vector ("vector") results per class. */
  std::vector<FeatureClassStats> vector;
  /** The report as JSON, for CI and tests. */
  QJsonObject toJson() const;
};

/**
 * Largest relative difference allowed between the land area written and
 * the source polygons' (0.1 %).
 */
constexpr double areaTolerance = 0.001;

/**
 * Builds the pack described by @p config: checks the sources, writes the
 * imagery store and shoreline GeoPackage, copies the licence notices and
 * writes the manifest. Fails if a shoreline level's land area is more than
 * areaTolerance off the source polygons', or the pack exceeds the size limit
 * minus the reserve. Returns false and sets @p errorString on failure.
 */
bool buildPack(const PackConfig& config, const BuildOptions& options,
               BuildReport* report, QString* errorString);

} // namespace ts::packager

#endif // TS_PACKAGER_PACKBUILDER_H
