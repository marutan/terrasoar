// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Loading the program's translation files.
 */
#ifndef TS_I18N_TRANSLATIONLOADER_H
#define TS_I18N_TRANSLATIONLOADER_H

#include <QLocale>
#include <QString>

class QTranslator;

namespace ts
{

/** Base name of the translation files: terrasoar_<locale>.qm. */
inline constexpr const char* translationBaseName = "terrasoar";

/** Resource directory the build embeds the .qm files in. */
inline constexpr const char* translationResourceDir = ":/i18n";

/**
 * Loads the translation for @p locale from @p directory into
 * @p translator, falling back to English when no better match exists.
 *
 * Tries each of QLocale::uiLanguages() in order (e.g. "de-AT", "de"), then
 * "en".
 *
 * @reentrant
 *
 * @param translator Translator to load into.
 * @param locale Wanted locale, normally QLocale::system().
 * @param directory Directory or resource path holding the .qm files.
 * @return The locale name that was loaded (e.g. "en"), or an empty string if
 *         not even the English file was found.
 */
QString loadTranslation(
    QTranslator& translator, const QLocale& locale,
    const QString& directory = QString::fromLatin1(translationResourceDir));

} // namespace ts

#endif // TS_I18N_TRANSLATIONLOADER_H
