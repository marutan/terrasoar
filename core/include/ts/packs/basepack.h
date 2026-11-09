// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Finding the built-in base pack.
 */
#ifndef TS_PACKS_BASEPACK_H
#define TS_PACKS_BASEPACK_H

#include <QString>
#include <QStringList>

namespace ts
{

/** The identifier in the base pack's manifest. */
QString basePackId();

/**
 * Returns the directories searched for the base pack, in order, for an
 * application in @p applicationDir:
 *
 * 1. `TS_BASEPACK_DIR` from the environment, if set (and only it);
 * 2. `<applicationDir>/basepack` (Windows installs);
 * 3. `<applicationDir>/../share/terrasoar/basepack` (Linux installs);
 * 4. `<applicationDir>/../basepack` (a build tree after `make basepack`).
 */
QStringList basePackSearchPath(const QString& applicationDir);

/**
 * Returns the first directory of basePackSearchPath() holding a manifest
 * whose id is basePackId(), or an empty string. Reads only the manifest;
 * PackManifest::verify() checks the files.
 */
QString findBasePack(const QString& applicationDir);

} // namespace ts

#endif // TS_PACKS_BASEPACK_H
