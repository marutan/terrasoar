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
 * Where the application keeps its copies of the WMS, WMTS, WFS and XYZ
 * catalogues.
 */
#ifndef TS_APP_WMSCATALOGUEFILE_H
#define TS_APP_WMSCATALOGUEFILE_H

#include <vector>

#include <QString>

#include "ts/wms/wmscatalogue.h"
#include "ts/xyz/xyzsource.h"

namespace ts
{

/**
 * Returns the path of the copy of the catalogue last fetched (in the
 * cache directory).
 */
QString wmsCatalogueCachePath();

/**
 * Returns the catalogue in the file at @p path, or an empty list if it
 * can't be read.
 */
std::vector<WmsCatalogueEntry> readWmsCatalogueFile(const QString& path);

/**
 * Returns the newest catalogue at hand: the copy last fetched, else the
 * one built into the program. Used for the Add WMS dialog's list and for
 * the terms of offline regions (bulkDownloadPolicy()).
 */
std::vector<WmsCatalogueEntry> currentWmsCatalogue();

/**
 * Returns the path of the copy of the WMTS catalogue last fetched (in the
 * cache directory).
 */
QString wmtsCatalogueCachePath();

/**
 * Returns the WMTS catalogue in the file at @p path, or an empty list if
 * it can't be read.
 */
std::vector<WmsCatalogueEntry> readWmtsCatalogueFile(const QString& path);

/**
 * Returns the newest WMTS catalogue at hand: the copy last fetched, else
 * the one built into the program.
 */
std::vector<WmsCatalogueEntry> currentWmtsCatalogue();

/**
 * Returns the path of the copy of the XYZ catalogue last fetched (in the
 * cache directory).
 */
QString xyzCatalogueCachePath();

/**
 * Returns the XYZ catalogue in the file at @p path, or an empty list if it
 * can't be read.
 */
std::vector<XyzCatalogueEntry> readXyzCatalogueFile(const QString& path);

/**
 * Returns the newest XYZ catalogue at hand: the copy last fetched, else
 * the one built into the program.
 */
std::vector<XyzCatalogueEntry> currentXyzCatalogue();

/**
 * Returns the path of the copy of the WFS catalogue last fetched (in the
 * cache directory).
 */
QString wfsCatalogueCachePath();

/**
 * Returns the newest WFS catalogue at hand: the copy last fetched, else
 * the one built into the program.
 */
std::vector<WmsCatalogueEntry> currentWfsCatalogue();

/**
 * Returns every catalogue, WMS, WMTS, WFS then XYZ (as service entries, see
 * XyzCatalogueEntry::serviceEntry()): for the terms of offline regions,
 * which may be of any kind (their addresses tell them apart).
 */
std::vector<WmsCatalogueEntry> currentServiceCatalogues();

} // namespace ts

#endif // TS_APP_WMSCATALOGUEFILE_H
