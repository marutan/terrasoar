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
 * Writing KML and KMZ: documents made in Terra Soar in KML 2.2's order,
 * documents read from files with everything not changed as it was.
 */
#ifndef TS_KML_KMLWRITER_H
#define TS_KML_KMLWRITER_H

#include <cstdint>
#include <vector>

#include <QByteArray>
#include <QString>
#include <QStringList>

#include "ts/kml/kmldocument.h"

namespace ts
{

/**
 * Returns @p document as KML (UTF-8). A document read with its source kept
 * (KmlReadOptions::keepSource) is written as it was read wherever it is
 * unchanged: elements, their order, the text and comments between them,
 * and elements Terra Soar doesn't know; changed elements are written anew,
 * and new ones where KML 2.2's order puts them. Other documents are written
 * in KML 2.2's order, with unknown elements after the element they
 * followed.
 */
QByteArray writeKml(const KmlDocument& document);

/**
 * Writes @p document to @p path: KML, or a KMZ archive if @p path ends
 * ".kmz", holding doc.kml and the local icons and pictures it uses (those
 * given by relative address keep it; others go in files/ and the document
 * points there). Web addresses are left as they are. Returns false and sets
 * @p errorString if it can't be written; @p missing (if given) gets the
 * addresses of local files that couldn't be read.
 */
bool writeKmlFile(const KmlDocument& document, const QString& path,
                  QString* errorString, QStringList* missing = nullptr);

/**
 * Returns the element of @p feature in @p slot as Terra Soar writes it
 * (KmlSourcePart::slot; the @p ordinal-th for repeated ones), or empty if
 * it has none. For telling whether an element read has changed.
 */
QString kmlFeatureElement(const KmlFeature& feature, const QString& slot,
                          int32_t ordinal);

/** Returns the hash used for KmlSourcePart::hash of @p text. */
uint64_t kmlTextHash(const QString& text);

/**
 * Returns a new document holding a copy of the feature at @p featurePath
 * of @p document (child indices from the root, as kmlContainer()), with
 * the shared styles and style maps of @p document it uses. Its source is
 * kept as @p document's, so it is written as it was read.
 */
KmlDocument kmlExtract(const KmlDocument& document, const QString& featurePath);

/**
 * Returns a document named @p name holding a placemark for each of
 * @p placemarks (each a name and a geometry), drawn in @p color (lines
 * and outlines; areas filled with it at a third of its opacity).
 */
KmlDocument
kmlDocumentOf(const QString& name,
              const std::vector<std::pair<QString, KmlGeometry>>& placemarks,
              const QColor& color);

} // namespace ts

#endif // TS_KML_KMLWRITER_H
