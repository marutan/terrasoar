// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The manifest of a data pack: its files with checksums, its datasets
 * with sources, licences and attribution.
 */
#ifndef TS_PACKS_PACKMANIFEST_H
#define TS_PACKS_PACKMANIFEST_H

#include <atomic>
#include <optional>
#include <vector>

#include <QJsonObject>
#include <QString>

namespace ts
{

/**
 * One file of a pack, with the checksum it must have.
 *
 * @reentrant
 */
struct PackFile {
  /** Path relative to the pack directory, '/' separated. */
  QString path;
  /** SHA-256 of the contents, lower-case hexadecimal. */
  QString sha256;
  /** Size, bytes. */
  qint64 sizeBytes = 0;
};

/**
 * Where a dataset of a pack comes from.
 *
 * @reentrant
 */
struct PackSource {
  /** Name of the source product, e.g. "GSHHG". */
  QString name;
  /** Version of the source product. */
  QString version;
  /** Where the source was downloaded from. */
  QString url;
  /** SHA-256 of the downloaded source file, lower-case hexadecimal. */
  QString sha256;
};

/**
 * One dataset of a pack: an imagery pyramid or a set of vector features.
 *
 * @reentrant
 */
struct PackDataset {
  /** Kind of data. */
  enum class Kind {
    Imagery,  ///< Tiles in a SQLite tile store.
    Features, ///< Tiled vector features in a GeoPackage.
    /**
     * Named areas (countries) in one GeoPackage table, not tiled, for
     * choosing an offline region (ts::CountryAreas).
     */
    Areas,
    /**
     * Height grids in a SQLite tile store (format "heights"), read by
     * ts::SqliteElevationProvider.
     */
    Elevation,
    /** A geoid grid in PROJ's GeoTIFF format, read by ts::GeoidModel. */
    Geoid,
  };
  /** Identifier within the pack, e.g. "bluemarble". */
  QString id;
  /** Kind of data. */
  Kind kind = Kind::Imagery;
  /** The file holding it, one of the pack's files. */
  QString file;
  /** Name for the user (English; the application translates known ids). */
  QString name;
  /** Licence of the data, as an SPDX identifier or short text. */
  QString licence;
  /** Attribution to show while the data is on screen. */
  QString attribution;
  /** Where it comes from. */
  PackSource source;
  /**
   * The base pack dataset this one takes the place of while its pack is
   * installed, or empty. See docs/design/packs.md.
   */
  QString replaces;
};

/** The result of checking a pack's files against its manifest. */
struct PackVerification {
  /** True if every file exists with the right size and checksum. */
  bool ok = false;
  /** One line per problem, naming the file (English, for logs). */
  std::vector<QString> problems;
};

/**
 * The manifest of a data pack (`manifest.json` in the pack directory).
 *
 * Format version 1:
 *
 * ```json
 * {
 *   "format": 1,
 *   "id": "terrasoar-base",
 *   "version": "1",
 *   "files": [ {"path": "...", "sha256": "...", "bytes": 123} ],
 *   "datasets": [ {"id": "...", "kind": "imagery|features", "file": "...",
 *                  "name": "...", "licence": "...", "attribution": "...",
 *                  "source": {"name": "...", "version": "...",
 *                             "url": "...", "sha256": "..."},
 *                  "replaces": "..."} ],
 *   "notices": ["licences/..."]
 * }
 * ```
 *
 * See docs/data/base-pack.md.
 *
 * @reentrant
 */
class PackManifest
{
public:
  /** The file name of a manifest inside its pack directory. */
  static QString fileName();

  /** The highest format version this code reads and writes. */
  static constexpr int currentFormat = 1;

  /**
   * Reads the manifest of the pack in @p directory. Returns nothing and
   * sets @p errorString (English) if it is missing or malformed.
   */
  static std::optional<PackManifest> read(const QString& directory,
                                          QString* errorString);

  /**
   * Parses a manifest from JSON. Returns nothing and sets @p errorString
   * (English) if it is malformed or of a newer format.
   */
  static std::optional<PackManifest> fromJson(const QJsonObject& json,
                                              QString* errorString);

  /** Returns the manifest as JSON (the format read by fromJson()). */
  QJsonObject toJson() const;

  /**
   * Checks every file of the pack in @p directory: present, of the listed
   * size and with the listed SHA-256. Reads every byte, so run it off the
   * GUI thread. Gives up, not ok, when @p cancelled becomes true.
   */
  PackVerification verify(const QString& directory,
                          const std::atomic<bool>& cancelled) const;

  /** Returns the dataset with @p id, or nothing. */
  std::optional<PackDataset> dataset(const QString& id) const;

  /** Pack identifier, e.g. "terrasoar-base". */
  QString id;
  /** Pack version. */
  QString version;
  /** The pack's files. */
  std::vector<PackFile> files;
  /** The pack's datasets. */
  std::vector<PackDataset> datasets;
  /** Licence and notice files in the pack, relative paths. */
  std::vector<QString> notices;
};

/**
 * Computes the SHA-256 of the file at @p path as lower-case hexadecimal.
 * Returns an empty string if it cannot be read or @p cancelled becomes
 * true.
 */
QString fileSha256(const QString& path, const std::atomic<bool>& cancelled);

} // namespace ts

#endif // TS_PACKS_PACKMANIFEST_H
