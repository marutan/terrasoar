// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Where the application keeps its copy of the WMS catalogue.
 */
#ifndef TS_APP_WMSCATALOGUEFILE_H
#define TS_APP_WMSCATALOGUEFILE_H

#include <vector>

#include <QString>

#include "ts/wms/wmscatalogue.h"

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

} // namespace ts

#endif // TS_APP_WMSCATALOGUEFILE_H
