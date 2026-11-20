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
 * XYZ and TMS tile services (`{z}/{x}/{y}` address templates): their
 * description, the catalogue, and imagery drawn from them.
 */
#ifndef TS_XYZ_XYZSOURCE_H
#define TS_XYZ_XYZSOURCE_H

#include <cstdint>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>

#include "ts/providers/tileprovider.h"
#include "ts/wms/wmscatalogue.h"
#include "ts/wmts/wmtssource.h"

namespace ts
{

/**
 * A web map's tiles addressed by zoom, column and row through a template,
 * as OpenStreetMap and most web maps serve them: Web Mercator, 2^z × 2^z
 * tiles at zoom z, columns from the antimeridian eastwards and rows from
 * the north (or from the south for TMS). Stored in layer definitions as
 * the source object `{"kind": "xyz", …}` (toJson()).
 *
 * The template's variables are `{z}`, `{x}`, `{y}` (the row, counted from
 * the south when tms is set), `{-y}` (the row counted from the south, as
 * some templates write TMS) and `{s}` (one of the subdomains, chosen by
 * the tile so each tile always comes from the same one).
 *
 * @reentrant
 */
struct XyzSource {
  /** The address template, e.g. `https://tile.example.org/{z}/{x}/{y}.png`. */
  QString urlTemplate;
  /** Values of `{s}`; empty if the template has none. */
  QStringList subdomains;
  /** True if `{y}` counts rows from the south (TMS). */
  bool tms = false;
  /** Coarsest zoom the service has tiles at. */
  int32_t minimumZoom = 0;
  /** Finest zoom the service has tiles at. */
  int32_t maximumZoom = 19;
  /** Size of the service's tiles, pixels (256, or 512 for some). */
  int32_t tileSizePx = 256;
  /** Image MIME type, from the template's extension unless given. */
  QString format = QStringLiteral("image/png");
  /** Where the service has tiles; tiles outside aren't requested. */
  std::optional<GeoRect> bounds;
  /** Name for the user. */
  QString title;
  /** Who to credit, shown while the layer is on screen. */
  QString attribution;
  /**
   * The service's address for the catalogue's terms of use (the
   * template's scheme and host, with the first subdomain, by default).
   */
  QUrl service;

  /** Returns a string identifying the tiles (template, zooms, row order). */
  QString id() const;

  /** Returns the source object for a layer definition. */
  QJsonObject toJson() const;

  /**
   * Reads a source object. Returns nothing and sets @p errorString
   * (translated) if the template isn't usable (see checkXyzTemplate()) or
   * the zooms make no sense.
   */
  static std::optional<XyzSource> fromJson(const QJsonObject& json,
                                           QString* errorString);

  /**
   * Returns the same tiles as a WMTS source in Web Mercator
   * (GoogleMapsCompatible, one matrix per zoom from minimumZoom, named by
   * the zoom), for the WMTS machinery to fit them to the quadtree.
   */
  WmtsSource toWmtsSource() const;
};

/**
 * Returns an empty string if @p urlTemplate can be used: an http(s)
 * address with `{z}`, `{x}` and one of `{y}` or `{-y}`, and no other
 * variables than those and `{s}` (which then needs @p subdomains). Else a
 * sentence for the user (translated) saying what is wrong.
 */
QString checkXyzTemplate(const QString& urlTemplate,
                         const QStringList& subdomains);

/**
 * Returns the image MIME type a template's extension names: JPEG for
 * `.jpg` and `.jpeg`, WebP for `.webp`, else PNG.
 */
QString xyzFormatFor(const QString& urlTemplate);

/**
 * Returns the address of the tile at @p zoom, @p column and @p row
 * (counted from the north) of @p source: the template with its variables
 * filled in.
 */
// Zoom, column, row: the order of the template's {z}/{x}/{y}.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
QUrl xyzTileUrl(const XyzSource& source, int32_t zoom, int64_t column,
                int64_t row);

/**
 * Returns the scheme and host of @p urlTemplate as an address (`{s}`
 * filled with the first of @p subdomains), for matching the catalogue.
 */
QUrl xyzServiceUrl(const QString& urlTemplate, const QStringList& subdomains);

/** One XYZ service in the catalogue. */
struct XyzCatalogueEntry {
  /** Name for the user. */
  QString title;
  /** The address template. */
  QString urlTemplate;
  /** Values of `{s}`. */
  QStringList subdomains;
  /** True if `{y}` counts rows from the south. */
  bool tms = false;
  /** Coarsest zoom. */
  int32_t minimumZoom = 0;
  /** Finest zoom. */
  int32_t maximumZoom = 19;
  /** What it shows. */
  QString description;
  /** Who to credit, as the provider asks. */
  QString attribution;
  /** Where its terms of use are. */
  QUrl termsUrl;
  /** Whether the terms allow offline regions; nothing if not said. */
  std::optional<bool> allowBulkDownload;

  /** Returns a source for the entry (title, attribution and service set). */
  XyzSource source() const;

  /**
   * Returns the entry as a service catalogue entry (its url the template's
   * host, see xyzServiceUrl()), for the offline regions' terms check.
   */
  WmsCatalogueEntry serviceEntry() const;
};

/** Format name in every XYZ catalogue file. */
inline constexpr const char* xyzCatalogueFormatName = "terrasoar-xyz-catalogue";

/** Version of the XYZ catalogue format this build reads. */
inline constexpr int32_t xyzCatalogueFormatVersion = 1;

/**
 * Where the application fetches the current XYZ catalogue: the file
 * `data/xyz-catalogue.json` on the main branch of the repository.
 */
inline constexpr const char* xyzCatalogueUrl =
    "https://raw.githubusercontent.com/marutan/terrasoar/main/data/"
    "xyz-catalogue.json";

/**
 * Reads an XYZ catalogue document:
 *
 *     {"format": "terrasoar-xyz-catalogue", "version": 1,
 *      "services": [{"title": …, "template": …, "subdomains": […],
 *                    "tms": false, "minZoom": 0, "maxZoom": 19,
 *                    "description": …, "attribution": …, "terms": …,
 *                    "allowBulkDownload": true|false}, …]}
 *
 * Entries without a title or a usable template are skipped. Returns
 * nothing and sets @p errorString (translated) if it isn't a catalogue
 * or is of a newer version.
 *
 * @reentrant
 */
std::optional<std::vector<XyzCatalogueEntry>>
parseXyzCatalogue(const QByteArray& json, QString* errorString);

} // namespace ts

#endif // TS_XYZ_XYZSOURCE_H
