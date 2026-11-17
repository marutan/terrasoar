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

#include "ts/i18n/pseudotranslator.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

#include <QChar>

namespace ts
{

namespace
{

/** Accented look-alikes for 'A'..'Z' and 'a'..'z'. */
constexpr std::u16string_view upperMap = u"ÅƁÇĐÉƑĜĤÎĴĶĹṀÑÖÞǪŘŠŢÛṼŴẊÝŽ";
constexpr std::u16string_view lowerMap = u"åƀçđéƒĝĥîĵķĺɱñöþǫŕšţûṽŵẋýž";

QChar accent(QChar c)
{
  const char16_t u = c.unicode();
  if (u >= u'A' && u <= u'Z') {
    return QChar(upperMap[static_cast<std::size_t>(u - u'A')]);
  }
  if (u >= u'a' && u <= u'z') {
    return QChar(lowerMap[static_cast<std::size_t>(u - u'a')]);
  }
  return c;
}

bool isDigit(QChar c)
{
  return c >= QLatin1Char('0') && c <= QLatin1Char('9');
}

/**
 * Length of the Qt placeholder (%1..%99, %L1, %n, %Ln) starting at
 * @p i in @p source, or 0 if there is none.
 */
int32_t placeholderLength(const QString& source, int32_t i)
{
  const int32_t size = static_cast<int32_t>(source.size());
  if (source.at(i) != QLatin1Char('%') || i + 1 >= size) {
    return 0;
  }
  int32_t j = i + 1;
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
  // "%" alone, or "%L" with nothing after it, is not a placeholder.
  if (j == i + 1 || source.at(j - 1) == QLatin1Char('L')) {
    return 0;
  }
  return j - i;
}

} // namespace

PseudoTranslator::PseudoTranslator(QObject* parent) : QTranslator(parent) {}

QString PseudoTranslator::translate(const char* /*context*/,
                                    const char* sourceText,
                                    const char* /*disambiguation*/,
                                    int32_t /*n*/) const
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
  int32_t visible = 0;
  int32_t i = 0;
  const int32_t size = static_cast<int32_t>(source.size());
  while (i < size) {
    const QChar c = source.at(i);
    // Placeholders are copied untouched.
    const int32_t placeholder = placeholderLength(source, i);
    if (placeholder > 0) {
      out += source.mid(i, placeholder);
      visible += 2;
      i += placeholder;
      continue;
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
      const int32_t end =
          static_cast<int32_t>(source.indexOf(QLatin1Char('>'), i));
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
  const int32_t padding = (visible * expansionPercent + 99) / 100;
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
