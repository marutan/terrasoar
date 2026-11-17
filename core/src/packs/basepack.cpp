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

#include "ts/packs/basepack.h"

#include <optional>

#include <QDir>
#include <QtGlobal>

#include "ts/packs/packmanifest.h"

namespace ts
{

QString basePackId()
{
  return QStringLiteral("terrasoar-base"); // ts-notr: identifier
}

QStringList basePackSearchPath(const QString& applicationDir)
{
  const QString fromEnvironment =
      qEnvironmentVariable("TS_BASEPACK_DIR"); // ts-notr: variable name
  if (!fromEnvironment.isEmpty()) {
    return {QDir::cleanPath(fromEnvironment)};
  }
  const QDir dir(applicationDir);
  return {
      QDir::cleanPath(dir.filePath(QStringLiteral("basepack"))),
      QDir::cleanPath(
          dir.filePath(QStringLiteral("../share/terrasoar/basepack"))),
      QDir::cleanPath(dir.filePath(QStringLiteral("../basepack"))),
  };
}

QString findBasePack(const QString& applicationDir)
{
  const QStringList candidates = basePackSearchPath(applicationDir);
  for (const QString& candidate : candidates) {
    const std::optional<PackManifest> manifest =
        PackManifest::read(candidate, nullptr);
    if (manifest && manifest->id == basePackId()) {
      return candidate;
    }
  }
  return QString();
}

} // namespace ts
