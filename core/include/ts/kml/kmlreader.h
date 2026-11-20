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
 * Reading KML and KMZ into the element model, resolving styles, and
 * reading the files a document refers to.
 */
#ifndef TS_KML_KMLREADER_H
#define TS_KML_KMLREADER_H

#include <map>
#include <memory>
#include <optional>

#include <QByteArray>
#include <QString>

#include "ts/kml/kmldocument.h"

namespace ts
{

/**
 * Reads a KML 2.2 document (2.0 and 2.1 too), matching elements by local
 * name so the namespace prefixes (kml:, gx:) don't matter. @p base is
 * where relative addresses start (KmlDocument::base). Unknown elements
 * are kept as XML (KmlFeature::unknown) and listed in
 * KmlDocument::skipped; nothing else makes it fail. Returns nothing and
 * sets @p errorString (translated, with the line and column) for XML
 * that isn't well formed or a file that isn't KML.
 *
 * @reentrant
 */
std::optional<KmlDocument> parseKml(const QByteArray& xml, const QString& base,
                                    QString* errorString);

/**
 * Reads a .kml file, or a .kmz archive (its doc.kml, else the first .kml
 * at its top level, through GDAL's /vsizip/). Returns nothing and sets
 * @p errorString (translated) if it can't be read.
 *
 * @reentrant
 */
std::optional<KmlDocument> readKmlFile(const QString& path,
                                       QString* errorString);

/**
 * Returns where @p href points for @p document: unchanged if it is a web
 * address (http, https); else a path relative to the document's base
 * (inside the KMZ for an archive). Empty for an empty href.
 */
QString resolveKmlHref(const KmlDocument& document, const QString& href);

/**
 * Reads the local file (or KMZ member) @p href points to for
 * @p document; empty, with @p errorString set (translated), if it can't.
 * Web addresses aren't read here (the caller fetches them).
 */
QByteArray readKmlResource(const KmlDocument& document, const QString& href,
                           QString* errorString);

/**
 * Reads the local file at @p where (a path, or a /vsizip/ member, as
 * resolveKmlHref() gives); empty if it can't.
 */
QByteArray readKmlLocalFile(const QString& where);

/**
 * Resolves features' styles: the styleUrl's Style or StyleMap (its
 * normal or highlight style) in the document, or in another local KML
 * file ("other.kml\#id", read once and kept), with the feature's inline
 * Style laid over it. Unset sub-styles keep KML's defaults when used.
 *
 * Not thread-safe (it caches external documents): one per thread.
 */
class KmlStyleResolver
{
public:
  /** Resolves styles of @p document, which must outlive it. */
  explicit KmlStyleResolver(const KmlDocument& document);

  /** Returns @p feature's style, normal or @p highlight. */
  KmlStyle resolve(const KmlFeature& feature, bool highlight = false);

  /** Returns the style @p url names (\#id, or file\#id), else none. */
  std::optional<KmlStyle> styleAt(const QString& url, bool highlight);

private:
  const KmlDocument& m_document;
  std::map<QString, std::shared_ptr<const KmlDocument>> m_external;
};

} // namespace ts

#endif // TS_KML_KMLREADER_H
