// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/i18n/pseudotranslator.h"

#include <QChar>

namespace ts
{

namespace
{

/** Accented look-alikes for 'A'..'Z' and 'a'..'z'. */
const char16_t upperMap[] = u"ÅƁÇĐÉƑĜĤÎĴĶĹṀÑÖÞǪŘŠŢÛṼŴẊÝŽ";
const char16_t lowerMap[] = u"åƀçđéƒĝĥîĵķĺɱñöþǫŕšţûṽŵẋýž";

QChar accent(QChar c)
{
  const char16_t u = c.unicode();
  if (u >= u'A' && u <= u'Z') {
    return QChar(upperMap[u - u'A']);
  }
  if (u >= u'a' && u <= u'z') {
    return QChar(lowerMap[u - u'a']);
  }
  return c;
}

bool isDigit(QChar c)
{
  return c >= QLatin1Char('0') && c <= QLatin1Char('9');
}

} // namespace

PseudoTranslator::PseudoTranslator(QObject* parent) : QTranslator(parent) {}

QString PseudoTranslator::translate(const char* /*context*/,
                                    const char* sourceText,
                                    const char* /*disambiguation*/,
                                    int /*n*/) const
{
  if (sourceText == nullptr || *sourceText == '\0') {
    return QString();
  }
  return pseudoTranslate(QString::fromUtf8(sourceText));
}

bool PseudoTranslator::isEmpty() const
{
  return false;
}

QString PseudoTranslator::pseudoTranslate(const QString& source)
{
  QString out;
  out.reserve(source.size() * 2 + 2);
  out += QLatin1Char('[');
  int visible = 0;
  int i = 0;
  const int size = static_cast<int>(source.size());
  while (i < size) {
    const QChar c = source.at(i);
    if (c == QLatin1Char('%') && i + 1 < size) {
      // %1..%99, %L1, %n, %Ln: copy untouched.
      int j = i + 1;
      if (source.at(j) == QLatin1Char('L')) {
        ++j;
      }
      if (j < size && source.at(j) == QLatin1Char('n')) {
        ++j;
      } else {
        while (j < size && isDigit(source.at(j))) {
          ++j;
        }
      }
      if (j > i + 1 && (source.at(j - 1) != QLatin1Char('L'))) {
        out += source.mid(i, j - i);
        visible += 2;
        i = j;
        continue;
      }
    }
    if (c == QLatin1Char('&')) {
      // "&&" is a literal ampersand; "&x" marks a mnemonic.
      if (i + 1 < size && source.at(i + 1) == QLatin1Char('&')) {
        out += QStringLiteral("&&");
        ++visible;
        i += 2;
        continue;
      }
      out += c;
      ++i;
      continue;
    }
    if (c == QLatin1Char('<')) {
      // Keep HTML tags as they are.
      const int end = static_cast<int>(source.indexOf(QLatin1Char('>'), i));
      if (end > i) {
        out += source.mid(i, end - i + 1);
        i = end + 1;
        continue;
      }
    }
    out += accent(c);
    ++visible;
    ++i;
  }
  // Integer maths so the rounding is exact.
  const int padding = (visible * expansionPercent + 99) / 100;
  out += QString(padding, QLatin1Char('~'));
  out += QLatin1Char(']');
  return out;
}

bool PseudoTranslator::isPseudoTranslated(const QString& text)
{
  QString plain = text;
  plain.replace(QStringLiteral("&&"), QStringLiteral("\x01"));
  plain.remove(QLatin1Char('&'));
  return plain.startsWith(QLatin1Char('[')) && plain.endsWith(QLatin1Char(']'));
}

} // namespace ts
