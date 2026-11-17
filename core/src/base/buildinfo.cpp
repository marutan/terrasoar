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

#include "ts/base/buildinfo.h"

#include <QCoreApplication>
#include <QtGlobal>

namespace ts
{

namespace
{

QString compilerName()
{
#if defined(__clang__)
  return QStringLiteral("Clang %1.%2.%3")
      .arg(__clang_major__)
      .arg(__clang_minor__)
      .arg(__clang_patchlevel__);
#elif defined(__GNUC__)
  return QStringLiteral("GCC %1.%2.%3")
      .arg(__GNUC__)
      .arg(__GNUC_MINOR__)
      .arg(__GNUC_PATCHLEVEL__);
#elif defined(_MSC_VER)
  return QStringLiteral("MSVC %1").arg(_MSC_FULL_VER);
#else
  return QStringLiteral("unknown compiler");
#endif
}

} // namespace

BuildInfo buildInfo()
{
  BuildInfo info;
  info.version = QStringLiteral(TS_VERSION_STRING);
  info.qtCompileVersion = QStringLiteral(QT_VERSION_STR);
  info.qtRuntimeVersion = QString::fromLatin1(qVersion());
  info.compiler = compilerName();
#ifdef QT_DEBUG
  info.debugBuild = true;
#endif
  return info;
}

QString programName()
{
  return QCoreApplication::translate("ts", "Terra Soar",
                                     "Program name; normally kept as it is");
}

} // namespace ts
