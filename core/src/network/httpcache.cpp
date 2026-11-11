// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/network/httpcache.h"

#include <algorithm>
#include <array>

#include <QList>

#include "ts/network/networkclient.h"

namespace ts
{

namespace
{

/** Days from 1970-01-01 to the civil date (proleptic Gregorian). */
long long daysFromCivil(long long year, int month, int day)
{
  year -= month <= 2 ? 1 : 0;
  const long long era = (year >= 0 ? year : year - 399) / 400;
  const long long yearOfEra = year - era * 400;
  const long long dayOfYear =
      (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const long long dayOfEra =
      yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
  return era * 146097 + dayOfEra - 719468;
}

int monthNumber(const QByteArray& token)
{
  static const std::array<const char*, 12> months = {
      "jan", "feb", "mar", "apr", "may", "jun",
      "jul", "aug", "sep", "oct", "nov", "dec"};
  const QByteArray prefix = token.left(3).toLower();
  for (std::size_t i = 0; i < months.size(); ++i) {
    if (prefix == months.at(i)) {
      return static_cast<int>(i) + 1;
    }
  }
  return 0;
}

bool isDigits(const QByteArray& token)
{
  return !token.isEmpty() &&
         std::all_of(token.begin(), token.end(),
                     [](char c) { return c >= '0' && c <= '9'; });
}

/** The directives of a Cache-Control header, names in lower case. */
struct CacheControl {
  bool noStore = false;
  bool noCache = false;
  std::optional<long long> maxAge;
};

CacheControl parseCacheControl(const QByteArray& header)
{
  CacheControl result;
  const QList<QByteArray> directives = header.split(',');
  for (const QByteArray& directive : directives) {
    const int equals = static_cast<int>(directive.indexOf('='));
    const QByteArray name =
        (equals < 0 ? directive : directive.left(equals)).trimmed().toLower();
    QByteArray value =
        equals < 0 ? QByteArray() : directive.mid(equals + 1).trimmed();
    if (value.startsWith('"') && value.endsWith('"') && value.size() >= 2) {
      value = value.mid(1, value.size() - 2);
    }
    if (name == "no-store") {
      result.noStore = true;
    } else if (name == "no-cache") {
      result.noCache = true;
    } else if (name == "max-age") {
      bool ok = false;
      const long long seconds = value.toLongLong(&ok);
      if (ok) {
        result.maxAge = std::max(seconds, 0LL);
      }
    }
  }
  return result;
}

/** The response's lifetime in seconds, and how old it already is. */
long long lifetimeS(const HttpResponse& response, const CacheControl& control,
                    long long nowS, long long defaultLifetimeS)
{
  if (control.noCache) {
    return 0;
  }
  if (control.maxAge) {
    return *control.maxAge;
  }
  const long long dateS = parseHttpDate(response.header("date")).value_or(nowS);
  const QByteArray expires = response.header("expires");
  if (!expires.isEmpty()) {
    const std::optional<long long> expiresS = parseHttpDate(expires);
    return expiresS ? std::max(*expiresS - dateS, 0LL) : 0;
  }
  const std::optional<long long> modifiedS =
      parseHttpDate(response.header("last-modified"));
  if (modifiedS && *modifiedS <= dateS) {
    return std::min((dateS - *modifiedS) / 10, defaultLifetimeS);
  }
  return defaultLifetimeS;
}

long long ageS(const HttpResponse& response)
{
  bool ok = false;
  const long long age = response.header("age").trimmed().toLongLong(&ok);
  return ok && age > 0 ? age : 0;
}

} // namespace

std::optional<long long> parseHttpDate(const QByteArray& value)
{
  // Commas and the dashes of RFC 850 dates separate fields like spaces.
  QByteArray text = value.trimmed();
  for (char& c : text) {
    if (c == ',' || c == '-') {
      c = ' ';
    }
  }
  int month = 0;
  int day = -1;
  long long year = -1;
  int hour = -1;
  int minute = -1;
  int second = -1;
  const QList<QByteArray> tokens = text.simplified().split(' ');
  for (const QByteArray& token : tokens) {
    if (token.contains(':')) {
      const QList<QByteArray> parts = token.split(':');
      if (parts.size() != 3 || !isDigits(parts.at(0)) ||
          !isDigits(parts.at(1)) || !isDigits(parts.at(2))) {
        return std::nullopt;
      }
      hour = parts.at(0).toInt();
      minute = parts.at(1).toInt();
      second = parts.at(2).toInt();
    } else if (isDigits(token)) {
      if (token.size() <= 2 && day < 0) {
        day = token.toInt();
      } else if (year < 0) {
        year = token.toLongLong();
        if (token.size() == 2) {
          year += year < 70 ? 2000 : 1900;
        }
      } else {
        return std::nullopt;
      }
    } else if (month == 0) {
      // The weekday and "GMT" are no month and change nothing.
      month = monthNumber(token);
    }
  }
  if (month == 0 || day < 1 || day > 31 || year < 1970 || hour < 0 ||
      hour > 23 || minute > 59 || second > 60) {
    return std::nullopt;
  }
  return ((daysFromCivil(year, month, day) * 24 + hour) * 60 + minute) * 60 +
         second;
}

HttpFreshness httpFreshness(const HttpResponse& response, long long nowS,
                            long long defaultLifetimeS)
{
  const CacheControl control =
      parseCacheControl(response.header("cache-control"));
  HttpFreshness freshness;
  freshness.storable = !control.noStore;
  freshness.expiresS =
      nowS + std::max(lifetimeS(response, control, nowS, defaultLifetimeS) -
                          ageS(response),
                      0LL);
  freshness.etag = response.header("etag");
  freshness.lastModified = response.header("last-modified");
  return freshness;
}

HttpFreshness revalidatedFreshness(const HttpFreshness& previous,
                                   const HttpResponse& response, long long nowS,
                                   long long defaultLifetimeS)
{
  HttpFreshness freshness = httpFreshness(response, nowS, defaultLifetimeS);
  freshness.storable = previous.storable && freshness.storable;
  if (freshness.etag.isEmpty()) {
    freshness.etag = previous.etag;
  }
  if (freshness.lastModified.isEmpty()) {
    freshness.lastModified = previous.lastModified;
  }
  return freshness;
}

std::vector<std::pair<QByteArray, QByteArray>>
revalidationHeaders(const QByteArray& etag, const QByteArray& lastModified)
{
  std::vector<std::pair<QByteArray, QByteArray>> headers;
  if (!etag.isEmpty()) {
    headers.emplace_back(QByteArrayLiteral("If-None-Match"), etag);
  }
  if (!lastModified.isEmpty()) {
    headers.emplace_back(QByteArrayLiteral("If-Modified-Since"), lastModified);
  }
  return headers;
}

} // namespace ts
