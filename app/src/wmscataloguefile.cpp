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

#include "wmscataloguefile.h"

#include <QDir>
#include <QFile>
#include <QStandardPaths>

namespace ts
{

QString wmsCatalogueCachePath()
{
  return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
      .filePath(QStringLiteral("wms-catalogue.json"));
}

std::vector<WmsCatalogueEntry> readWmsCatalogueFile(const QString& path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  QString error;
  return parseWmsCatalogue(file.readAll(), &error)
      .value_or(std::vector<WmsCatalogueEntry>{});
}

std::vector<WmsCatalogueEntry> currentWmsCatalogue()
{
  std::vector<WmsCatalogueEntry> catalogue =
      readWmsCatalogueFile(wmsCatalogueCachePath());
  if (catalogue.empty()) {
    catalogue =
        readWmsCatalogueFile(QStringLiteral(":/data/wms-catalogue.json"));
  }
  return catalogue;
}

QString wmtsCatalogueCachePath()
{
  return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
      .filePath(QStringLiteral("wmts-catalogue.json"));
}

std::vector<WmsCatalogueEntry> readWmtsCatalogueFile(const QString& path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  QString error;
  return parseWmtsCatalogue(file.readAll(), &error)
      .value_or(std::vector<WmsCatalogueEntry>{});
}

std::vector<WmsCatalogueEntry> currentWmtsCatalogue()
{
  std::vector<WmsCatalogueEntry> catalogue =
      readWmtsCatalogueFile(wmtsCatalogueCachePath());
  if (catalogue.empty()) {
    catalogue =
        readWmtsCatalogueFile(QStringLiteral(":/data/wmts-catalogue.json"));
  }
  return catalogue;
}

QString xyzCatalogueCachePath()
{
  return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
      .filePath(QStringLiteral("xyz-catalogue.json"));
}

std::vector<XyzCatalogueEntry> readXyzCatalogueFile(const QString& path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  QString error;
  return parseXyzCatalogue(file.readAll(), &error)
      .value_or(std::vector<XyzCatalogueEntry>{});
}

std::vector<XyzCatalogueEntry> currentXyzCatalogue()
{
  std::vector<XyzCatalogueEntry> catalogue =
      readXyzCatalogueFile(xyzCatalogueCachePath());
  if (catalogue.empty()) {
    catalogue =
        readXyzCatalogueFile(QStringLiteral(":/data/xyz-catalogue.json"));
  }
  return catalogue;
}

QString wfsCatalogueCachePath()
{
  return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
      .filePath(QStringLiteral("wfs-catalogue.json"));
}

std::vector<WmsCatalogueEntry> currentWfsCatalogue()
{
  for (const QString& path :
       {wfsCatalogueCachePath(), QStringLiteral(":/data/wfs-catalogue.json")}) {
    QFile file(path);
    if (file.open(QIODevice::ReadOnly)) {
      QString error;
      const std::optional<std::vector<WmsCatalogueEntry>> entries =
          parseWfsCatalogue(file.readAll(), &error);
      if (entries && !entries->empty()) {
        return *entries;
      }
    }
  }
  return {};
}

std::vector<WmsCatalogueEntry> currentServiceCatalogues()
{
  std::vector<WmsCatalogueEntry> catalogues = currentWmsCatalogue();
  const std::vector<WmsCatalogueEntry> wmts = currentWmtsCatalogue();
  catalogues.insert(catalogues.end(), wmts.begin(), wmts.end());
  const std::vector<WmsCatalogueEntry> wfs = currentWfsCatalogue();
  catalogues.insert(catalogues.end(), wfs.begin(), wfs.end());
  for (const XyzCatalogueEntry& entry : currentXyzCatalogue()) {
    catalogues.push_back(entry.serviceEntry());
  }
  return catalogues;
}

} // namespace ts
