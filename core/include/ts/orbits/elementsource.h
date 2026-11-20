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
/**
 * @file
 * Where element sets come from on the web (CelesTrak's groups), and
 * downloading them through a cache kept for offline use.
 */
#ifndef TS_ORBITS_ELEMENTSOURCE_H
#define TS_ORBITS_ELEMENTSOURCE_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <QString>
#include <QUrl>

#include "ts/orbits/orbitalelements.h"

namespace ts
{

class NetworkClient;

/** One of CelesTrak's published groups of satellites. */
struct CelestrakGroup {
  /** CelesTrak's name for it (GROUP=…), e.g. "stations". */
  QString id;
  /** Its name for the user (translated). */
  QString name;
  /** The heading it is listed under (translated). */
  QString category;
};

/** Returns CelesTrak's groups, by category, as CelesTrak lists them. */
std::vector<CelestrakGroup> celestrakGroups();

/** Returns the user's name of group @p id; @p id itself if unknown. */
QString celestrakGroupName(const QString& id);

/** CelesTrak's address for current elements. */
QUrl celestrakBaseUrl();

/** The form element sets are fetched in. */
enum class ElementFormat : uint8_t {
  /** OMM as JSON: the default, as it has six-digit catalogue numbers. */
  OmmJson,
  /** Three-line TLEs. */
  Tle,
};

/**
 * Returns the address of group @p group's elements at @p base (null for
 * CelesTrak's): `gp.php?GROUP=stations&FORMAT=json`.
 */
QUrl celestrakGroupUrl(const QString& group,
                       ElementFormat format = ElementFormat::OmmJson,
                       const QUrl& base = QUrl());

/** Returns the address of one satellite's elements (CATNR=…). */
QUrl celestrakSatelliteUrl(int32_t catalogueNumber,
                           ElementFormat format = ElementFormat::OmmJson,
                           const QUrl& base = QUrl());

/** How element sets are downloaded. */
struct ElementDownloadSettings {
  /** Where the last download of each address is kept; empty for nowhere. */
  QString cacheDirectory;
  /**
   * A download younger than this is used without asking again, seconds:
   * CelesTrak updates its elements every few hours and asks clients not
   * to fetch a group more than once in two.
   */
  int64_t minimumAgeS = int64_t{2} * 3600;
  /** Seconds since 1970, UTC (for tests); null for the system clock. */
  std::function<int64_t()> clock;
};

/** Where downloaded element sets came from. */
enum class ElementOrigin : uint8_t {
  /** Nothing: see the error. */
  None,
  /** The server, now (or confirmed unchanged). */
  Server,
  /** The cache, recent enough not to ask. */
  Cache,
  /** The cache, because the server couldn't be reached or failed. */
  StaleCache,
};

/** What a download gave. */
struct ElementDownload {
  /** The element sets, and what couldn't be read. */
  ElementSetList sets;
  /** Where they came from. */
  ElementOrigin origin = ElementOrigin::None;
  /** When they were downloaded, seconds since 1970 (0 if never). */
  int64_t downloadedS = 0;
  /** Why the server's answer wasn't used (translated); empty if it was. */
  QString errorString;

  /** Returns true if there are element sets. */
  bool ok() const { return !sets.elements.empty(); }
};

/**
 * Downloads the element sets at @p url, blocking (on a worker thread),
 * through the cache: a download younger than the minimum age is used
 * without asking (unless @p force), an older one is revalidated, and when
 * the server can't be reached or answers with something that isn't
 * element sets the last download is used however old. A new download
 * replaces the cached one only if it has element sets.
 */
ElementDownload downloadElementSets(NetworkClient& client, const QUrl& url,
                                    const ElementDownloadSettings& settings,
                                    bool force,
                                    const std::atomic<bool>& cancelled);

/**
 * Returns the last download of @p url from the cache, without the network;
 * nothing if there is none.
 */
std::optional<ElementDownload>
cachedElementSets(const QUrl& url, const ElementDownloadSettings& settings);

} // namespace ts

#endif // TS_ORBITS_ELEMENTSOURCE_H
