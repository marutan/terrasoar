// SPDX-License-Identifier: GPL-2.0-or-later
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

} // namespace ts
