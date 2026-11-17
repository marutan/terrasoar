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

#include "ts/i18n/translationloader.h"

#include <cstdint>
#include <utility>

#include <QDir>
#include <QStringList>
#include <QTranslator>

namespace ts
{

QString loadTranslation(QTranslator& translator, const QLocale& locale,
                        const QString& directory)
{
  const QString base = QString::fromLatin1(translationBaseName);
  QStringList candidates;
  const QStringList languages = locale.uiLanguages();
  for (const QString& language : languages) {
    QString name = language;
    name.replace(QLatin1Char('-'), QLatin1Char('_'));
    candidates << name;
    const int32_t separator =
        static_cast<int32_t>(name.indexOf(QLatin1Char('_')));
    if (separator > 0) {
      candidates << name.left(separator);
    }
  }
  candidates << QStringLiteral("en");
  candidates.removeDuplicates();

  const QDir dir(directory);
  for (const QString& name : std::as_const(candidates)) {
    // QTranslator::load() would itself strip "_GB" etc. and report success
    // for a less specific file; look for the exact file so the returned name
    // is the one really loaded.
    const QString fileName =
        base + QLatin1Char('_') + name + QStringLiteral(".qm");
    if (dir.exists(fileName) && translator.load(dir.filePath(fileName))) {
      return name;
    }
  }
  return QString();
}

} // namespace ts
