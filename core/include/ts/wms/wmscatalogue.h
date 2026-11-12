// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The catalogue of open WMS services offered in the Add WMS dialog.
 */
#ifndef TS_WMS_WMSCATALOGUE_H
#define TS_WMS_WMSCATALOGUE_H

#include <optional>
#include <vector>

#include <QByteArray>
#include <QString>
#include <QUrl>

namespace ts
{

/** One service in the catalogue. */
struct WmsCatalogueEntry {
  /** Name for the user. */
  QString title;
  /** The service's address (the GetCapabilities URL without its WMS
   *  parameters is enough). */
  QUrl url;
  /** What it shows. */
  QString description;
  /** Who to credit, as the provider asks. */
  QString attribution;
  /** Where its terms of use are. */
  QUrl termsUrl;
  /**
   * Whether the terms allow downloading regions for offline use; nothing
   * if the catalogue doesn't say.
   */
  std::optional<bool> allowBulkDownload;
};

/** Whether tiles of a WMS may be downloaded in bulk (offline regions). */
enum class BulkDownloadPolicy {
  Allowed,   ///< The catalogue says its terms allow it.
  Unknown,   ///< Not in the catalogue, or it doesn't say: warn first.
  Forbidden, ///< The catalogue says its terms forbid it: refuse.
};

/** Format name in every catalogue file. */
inline constexpr const char* wmsCatalogueFormatName = "terrasoar-wms-catalogue";

/** Version of the catalogue format this build reads. */
inline constexpr int wmsCatalogueFormatVersion = 1;

/**
 * Where the application fetches the current catalogue: the file
 * `data/wms-catalogue.json` on the main branch of the repository, so it
 * can be changed without a release. A copy built into the program is
 * used until (or unless) that succeeds.
 */
inline constexpr const char* wmsCatalogueUrl =
    "https://raw.githubusercontent.com/marutan/terrasoar/main/data/"
    "wms-catalogue.json";

/**
 * Reads a catalogue document:
 *
 *     {"format": "terrasoar-wms-catalogue", "version": 1,
 *      "services": [{"title": …, "url": …, "description": …,
 *                    "attribution": …, "terms": …,
 *                    "allowBulkDownload": true|false}, …]}
 *
 * allowBulkDownload is optional (older programs ignore it).
 *
 * Entries without a title or an http(s) URL are skipped. Returns
 * nothing and sets @p errorString (translated) if it isn't a catalogue
 * or is of a newer version.
 *
 * @reentrant
 */
std::optional<std::vector<WmsCatalogueEntry>>
parseWmsCatalogue(const QByteArray& json, QString* errorString);

/**
 * Returns the catalogue entry for the service at @p url (a GetMap or
 * GetCapabilities address): the entry with the same host and path, else
 * the first with the same host; null if none. Query, scheme, case of the
 * host and a trailing slash don't matter.
 *
 * @reentrant
 */
const WmsCatalogueEntry*
findWmsCatalogueEntry(const QUrl& url,
                      const std::vector<WmsCatalogueEntry>& catalogue);

/**
 * Returns whether the service at @p url may be downloaded in bulk,
 * according to its entry in @p catalogue (findWmsCatalogueEntry()). An
 * entry found by host alone can forbid but not allow: another service on
 * the same host may have other terms.
 *
 * @reentrant
 */
BulkDownloadPolicy
bulkDownloadPolicy(const QUrl& url,
                   const std::vector<WmsCatalogueEntry>& catalogue);

} // namespace ts

#endif // TS_WMS_WMSCATALOGUE_H
