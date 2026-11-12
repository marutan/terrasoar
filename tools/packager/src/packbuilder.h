// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Builds a data pack from its configuration and downloaded sources.
 */
#ifndef TS_PACKAGER_PACKBUILDER_H
#define TS_PACKAGER_PACKBUILDER_H

#include <functional>
#include <optional>
#include <vector>

#include <QJsonObject>
#include <QString>

#include "imagerytiler.h"
#include "shorelinebuilder.h"

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
  /** Shoreline dataset, if any. */
  std::optional<ShorelineSpec> shoreline;

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
