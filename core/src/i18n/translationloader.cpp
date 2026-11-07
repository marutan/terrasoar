// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/i18n/translationloader.h"

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
    const int separator = static_cast<int>(name.indexOf(QLatin1Char('_')));
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
