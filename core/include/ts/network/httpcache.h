// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * How long an HTTP response may be used without asking the server again.
 */
#ifndef TS_NETWORK_HTTPCACHE_H
#define TS_NETWORK_HTTPCACHE_H

#include <optional>
#include <utility>
#include <vector>

#include <QByteArray>

namespace ts
{

struct HttpResponse;

/** What a response's headers say about keeping it. */
struct HttpFreshness {
  /** False if it must not be stored at all (Cache-Control: no-store). */
  bool storable = true;
  /**
   * Until when it may be used without asking the server, seconds since
   * 1970 (UTC). After that it is stale: it is revalidated when the server
   * can be reached and shown as it is when not.
   */
  long long expiresS = 0;
  /** The ETag, for If-None-Match; empty if none. */
  QByteArray etag;
  /** The Last-Modified date, for If-Modified-Since; empty if none. */
  QByteArray lastModified;
};

/**
 * Returns what @p response's headers say about caching it, at @p nowS
 * (seconds since 1970). Follows RFC 9111 for a private cache:
 *
 * - `Cache-Control: no-store`: not stored.
 * - `Cache-Control: no-cache`: stored, but stale at once (revalidated).
 * - `Cache-Control: max-age=N`: fresh for N seconds, less the `Age`.
 * - Otherwise `Expires` relative to `Date` (an invalid `Expires` means
 *   stale at once).
 * - Otherwise, with `Last-Modified`, a tenth of the time since then, at
 *   most @p defaultLifetimeS (the usual heuristic).
 * - Otherwise @p defaultLifetimeS (some map servers send none of these).
 *
 * @threadsafe
 */
HttpFreshness httpFreshness(const HttpResponse& response, long long nowS,
                            long long defaultLifetimeS);

/**
 * Returns @p previous updated by a 304 Not Modified @p response: a new
 * lifetime from its headers, the validators it sends (or the old ones).
 *
 * @threadsafe
 */
HttpFreshness revalidatedFreshness(const HttpFreshness& previous,
                                   const HttpResponse& response, long long nowS,
                                   long long defaultLifetimeS);

/**
 * Returns the request headers that ask the server whether a stored
 * response with these validators is still current (If-None-Match,
 * If-Modified-Since); empty if there are none.
 *
 * @threadsafe
 */
std::vector<std::pair<QByteArray, QByteArray>>
revalidationHeaders(const QByteArray& etag, const QByteArray& lastModified);

/**
 * Parses an HTTP date in any of the three formats HTTP allows
 * (IMF-fixdate "Sun, 06 Nov 1994 08:49:37 GMT", the obsolete RFC 850
 * "Sunday, 06-Nov-94 08:49:37 GMT" and asctime "Sun Nov  6 08:49:37
 * 1994"). Returns seconds since 1970 (UTC), or nothing if it isn't one.
 *
 * @threadsafe
 */
std::optional<long long> parseHttpDate(const QByteArray& value);

} // namespace ts

#endif // TS_NETWORK_HTTPCACHE_H
