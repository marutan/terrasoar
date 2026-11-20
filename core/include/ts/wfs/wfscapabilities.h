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
 * What a Web Feature Service offers: its feature types, the GetFeature
 * address, output formats and paging, read from GetCapabilities.
 */
#ifndef TS_WFS_WFSCAPABILITIES_H
#define TS_WFS_WFSCAPABILITIES_H

#include <cstdint>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QUrl>

#include "ts/tiling/tilekey.h"

namespace ts
{

/** One feature type (a layer of features) a WFS offers. */
struct WfsFeatureType {
  /** Qualified name, for TYPENAMES (`prefix:name`). */
  QString name;
  /** Title, for the user. */
  QString title;
  /** Description. */
  QString abstract;
  /** The CRS its features are stored in (DefaultCRS, or DefaultSRS). */
  QString defaultCrs;
  /** Other CRSs it can be asked in. */
  QStringList otherCrs;
  /** Where it has features, longitude and latitude; nothing if not given. */
  std::optional<GeoRect> wgs84Bounds;
  /** Output formats of its own (else the service's). */
  QStringList outputFormats;
};

/**
 * A WFS GetCapabilities document (2.0.0; 1.1.0 is read too).
 *
 * @reentrant
 */
struct WfsCapabilities {
  /** "2.0.0" or what the server said. */
  QString version;
  /** The service's title. */
  QString title;
  /** The service's description. */
  QString abstract;
  /** Fees, as the server states them. */
  QString fees;
  /** Access constraints, joined. */
  QString accessConstraints;
  /** Who runs it. */
  QString providerName;
  /** Where GetFeature requests go (HTTP GET). */
  QUrl getFeatureUrl;
  /** GetFeature's output formats. */
  QStringList outputFormats;
  /** True if the server pages results (STARTINDEX and COUNT). */
  bool resultPaging = false;
  /** The most features the server returns at once, or 0 if not said. */
  int64_t countDefault = 0;
  /** The feature types. */
  std::vector<WfsFeatureType> featureTypes;

  /** Returns the feature type named @p name, or null. */
  const WfsFeatureType* findFeatureType(const QString& name) const;
};

/**
 * Reads a WFS GetCapabilities document, matching elements by local name
 * (prefixes don't matter). Returns nothing and sets @p errorString
 * (translated) if it isn't one: an OWS exception (its message passed on),
 * an HTML page, or a WMS or WMTS document.
 */
std::optional<WfsCapabilities> parseWfsCapabilities(const QByteArray& xml,
                                                    QString* errorString);

/**
 * Returns the GetCapabilities address of the WFS at @p serviceUrl: KVP
 * SERVICE=WFS, REQUEST=GetCapabilities, VERSION=2.0.0, other query items
 * kept (those it sets replaced).
 */
QUrl wfsCapabilitiesUrl(const QUrl& serviceUrl);

} // namespace ts

#endif // TS_WFS_WFSCAPABILITIES_H
