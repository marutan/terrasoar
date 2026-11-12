// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The online list of data packs that can be installed.
 */
#ifndef TS_PACKS_PACKINDEX_H
#define TS_PACKS_PACKINDEX_H

#include <map>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QUrl>

namespace ts
{

/**
 * One pack in the index: what it is, and where its manifest and files
 * are.
 *
 * @reentrant
 */
struct PackIndexEntry {
  /** Pack identifier, as in its manifest; also its directory name. */
  QString id;
  /** Version; opaque, compared for equality only. */
  QString version;
  /** Name for the user (English). */
  QString name;
  /** A sentence or two for the user (English). */
  QString description;
  /** Licence of the data, short text. */
  QString licence;
  /** Total size of the pack's files, bytes. */
  qint64 sizeBytes = 0;
  /** Where the manifest is. */
  QUrl manifestUrl;
  /** SHA-256 of the manifest, lower-case hexadecimal. */
  QString manifestSha256;
  /** Size of the manifest, bytes. */
  qint64 manifestBytes = 0;
  /** Path in the pack (as in the manifest) → where to download it. */
  std::map<QString, QUrl> files;

  /**
   * Parses one entry of an index (see PackIndex). Returns nothing and
   * sets @p errorString (English) if it is malformed.
   */
  static std::optional<PackIndexEntry>
  fromJson(const QJsonObject& json, bool allowLocalHttp, QString* errorString);

  /** Returns the entry as JSON (the format read by fromJson()). */
  QJsonObject toJson() const;
};

/**
 * The index of downloadable packs (`index.json` of the packs repository).
 *
 * Format version 1:
 *
 * ```json
 * {
 *   "format": 1,
 *   "packs": [ {"id": "...", "version": "...", "name": "...",
 *               "description": "...", "licence": "...", "bytes": 123,
 *               "manifest": {"url": "...", "sha256": "...", "bytes": 123},
 *               "files": {"<path>": "<url>"}} ]
 * }
 * ```
 *
 * See docs/design/packs.md.
 *
 * @reentrant
 */
class PackIndex
{
public:
  /** The highest format version this code reads. */
  static constexpr int currentFormat = 1;

  /**
   * Parses an index. Returns nothing and sets @p errorString (English) if
   * it is not an index or of a newer format. Malformed entries are left
   * out and described in @p skipped (English), if given.
   *
   * URLs must be https, or http to a loopback address when
   * @p allowLocalHttp is true (tests).
   */
  static std::optional<PackIndex> fromJson(const QByteArray& json,
                                           QString* errorString,
                                           std::vector<QString>* skipped,
                                           bool allowLocalHttp = false);

  /** Returns the entry for pack @p id, or nothing. */
  std::optional<PackIndexEntry> pack(const QString& id) const;

  /** The packs, in the index's order. */
  std::vector<PackIndexEntry> packs;
};

/**
 * Returns true if @p id can name a pack: 1 to 64 characters of a–z, 0–9
 * and '-', not starting with '-'. Pack ids are directory names.
 */
bool isValidPackId(const QString& id);

/**
 * Returns true if @p url may be downloaded from: https, or http to a
 * loopback address when @p allowLocalHttp is true.
 */
bool isAllowedPackUrl(const QUrl& url, bool allowLocalHttp);

/**
 * Returns the index the application reads: `TS_PACK_INDEX_URL` from the
 * environment if set, else the packs repository's `index.json`.
 */
QUrl defaultPackIndexUrl();

} // namespace ts

#endif // TS_PACKS_PACKINDEX_H
