// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Generated pseudo-locale for translation testing.
 */
#ifndef TS_I18N_PSEUDOTRANSLATOR_H
#define TS_I18N_PSEUDOTRANSLATOR_H

#include <QString>
#include <QTranslator>

namespace ts
{

/**
 * A translator that generates a test locale from the source strings:
 * letters are swapped for accented look-alikes, the text is padded to about
 * 130% of its length and wrapped in brackets, e.g. "Quit" → "[Qüîţ~~]".
 *
 * Placeholders (%1, %L1, %n), mnemonics (&File, &&) and HTML tags are kept
 * as they are, so the result still formats correctly.
 *
 * Installed with QCoreApplication::installTranslator(), it shows every
 * string that is not wrapped for translation (it stays plain) and every
 * widget too small for longer text (the closing bracket is cut off).
 *
 * @reentrant translate() is const and keeps no state, so one instance may
 * be used from several threads at once.
 */
class PseudoTranslator : public QTranslator
{
  Q_OBJECT

public:
  /** Creates the translator. @param parent Usual QObject parent. */
  explicit PseudoTranslator(QObject* parent = nullptr);

  /** Returns pseudoTranslate(@p sourceText); the other arguments are
   * ignored. */
  QString translate(const char* context, const char* sourceText,
                    const char* disambiguation = nullptr,
                    int n = -1) const override;

  /** Always false: the translator handles every string. */
  bool isEmpty() const override;

  /** Returns the pseudo-translation of @p source. */
  static QString pseudoTranslate(const QString& source);

  /**
   * Returns true if @p text looks like the output of pseudoTranslate(), i.e.
   * starts with '[' and ends with ']' once mnemonic '&' signs are removed.
   */
  static bool isPseudoTranslated(const QString& text);

  /** Length growth applied to every string, in percent (rounded up). */
  static constexpr int expansionPercent = 30;
};

} // namespace ts

#endif // TS_I18N_PSEUDOTRANSLATOR_H
