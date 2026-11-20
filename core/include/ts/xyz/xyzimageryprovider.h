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
 * Imagery from an XYZ or TMS tile service.
 */
#ifndef TS_XYZ_XYZIMAGERYPROVIDER_H
#define TS_XYZ_XYZIMAGERYPROVIDER_H

#include <memory>

#include <QUrl>

#include "ts/network/cachedtilefetcher.h"
#include "ts/wmts/wmtsimageryprovider.h"
#include "ts/xyz/xyzsource.h"

namespace ts
{

class NetworkClient;

/**
 * Imagery from an XYZ (or TMS) tile service. Its tiles are Web Mercator
 * tiles of a GoogleMapsCompatible tile matrix set
 * (XyzSource::toWmtsSource()), so they are fitted to the quadtree, cached,
 * revalidated and kept for offline regions exactly as a WMTS layer's
 * (WmtsImageryProvider, docs/design/wmts.md), and drawn as Web Mercator
 * images reprojected in the shader. Only the addresses differ: the
 * template's `{z}/{x}/{y}`, rows flipped for TMS, `{s}` from the
 * subdomains. See docs/design/xyz.md.
 *
 * @threadsafe
 */
class XyzImageryProvider : public WmtsImageryProvider
{
public:
  /** Requests @p source's tiles through @p network. */
  XyzImageryProvider(XyzSource source, std::shared_ptr<NetworkClient> network,
                     WebTileSettings settings = {});

  /** Returns the source. */
  const XyzSource& xyzSource() const { return m_xyz; }

protected:
  /** Returns the template filled in for @p tile. */
  QUrl tileUrl(const WmtsTileIndex& tile) const override;

private:
  const XyzSource m_xyz;
};

} // namespace ts

#endif // TS_XYZ_XYZIMAGERYPROVIDER_H
