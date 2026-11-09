// SPDX-License-Identifier: GPL-2.0-or-later
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
