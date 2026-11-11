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
 *                    "attribution": …, "terms": …}, …]}
 *
 * Entries without a title or an http(s) URL are skipped. Returns
 * nothing and sets @p errorString (translated) if it isn't a catalogue
 * or is of a newer version.
 *
 * @reentrant
 */
std::optional<std::vector<WmsCatalogueEntry>>
parseWmsCatalogue(const QByteArray& json, QString* errorString);

} // namespace ts

#endif // TS_WMS_WMSCATALOGUE_H
