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
#include "ts/wmts/wmtscrs.h"

#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <utility>

#include <QByteArray>
#include <QStringList>

#include <proj.h>

#include "ts/deps/dependencies.h"
#include "ts/deps/projcontext.h"
#include "ts/wmts/wmtssource.h"

namespace ts
{

namespace
{

/** Points PROJ uses (proj.db), as for every context of the program. */
void setSearchPaths(const ProjContext& context)
{
  const QStringList paths = projSearchPaths();
  if (paths.isEmpty()) {
    return;
  }
  std::vector<QByteArray> encoded;
  std::vector<const char*> pointers;
  encoded.reserve(static_cast<std::size_t>(paths.size()));
  for (const QString& path : paths) {
    encoded.push_back(path.toUtf8());
    pointers.push_back(encoded.back().constData());
  }
  proj_context_set_search_paths(
      context.get(), static_cast<int32_t>(pointers.size()), pointers.data());
}

/** Destroys a PJ when it goes out of scope. */
struct PjHolder {
  PJ* pj = nullptr;
  explicit PjHolder(PJ* object) : pj(object) {}
  ~PjHolder()
  {
    if (pj != nullptr) {
      proj_destroy(pj);
    }
  }
  PjHolder(const PjHolder&) = delete;
  PjHolder& operator=(const PjHolder&) = delete;
  PjHolder(PjHolder&&) = delete;
  PjHolder& operator=(PjHolder&&) = delete;
};

/** Reads what PROJ knows of @p code; nothing if it isn't usable. */
std::optional<WmtsCrsInfo> askProj(const QString& code)
{
  ProjContext context;
  if (!context.isValid()) {
    return std::nullopt;
  }
  setSearchPaths(context);
  const PjHolder crs(proj_create(context.get(), code.toUtf8().constData()));
  if (crs.pj == nullptr) {
    return std::nullopt;
  }
  const PJ_TYPE type = proj_get_type(crs.pj);
  if (type != PJ_TYPE_GEOGRAPHIC_2D_CRS && type != PJ_TYPE_PROJECTED_CRS) {
    return std::nullopt;
  }
  const PjHolder cs(proj_crs_get_coordinate_system(context.get(), crs.pj));
  if (cs.pj == nullptr || proj_cs_get_axis_count(context.get(), cs.pj) < 2) {
    return std::nullopt;
  }
  const char* direction = nullptr;
  double unitFactor = 1.0;
  if (proj_cs_get_axis_info(context.get(), cs.pj, 0, nullptr, nullptr,
                            &direction, &unitFactor, nullptr, nullptr,
                            nullptr) == 0) {
    return std::nullopt;
  }
  WmtsCrsInfo info;
  info.code = code;
  const char* name = proj_get_name(crs.pj);
  info.name = name != nullptr ? QString::fromUtf8(name) : code;
  info.geographic = type == PJ_TYPE_GEOGRAPHIC_2D_CRS;
  const QString first = direction != nullptr
                            ? QString::fromLatin1(direction).toLower()
                            : QString();
  info.northingFirst =
      first == QLatin1String("north") || first == QLatin1String("south");
  // A projected CRS's unit factor is metres per unit; a geographic one's
  // is radians per unit, and WMTS defines a degree's metres itself.
  info.metresPerUnit = info.geographic ? wmtsMetresPerDegree : unitFactor;
  double west = 0.0;
  double south = 0.0;
  double east = 0.0;
  double north = 0.0;
  if (proj_get_area_of_use(context.get(), crs.pj, &west, &south, &east, &north,
                           nullptr) != 0 &&
      west > -1000.0 && west <= east && south <= north) {
    // (An area across the antimeridian, west > east, counts as anywhere.)
    info.areaOfUse = GeoRect{west, south, east, north};
  }
  return info;
}

/** Runs @p transform over @p points in @p direction; NaN where it fails. */
void transformPoints(PJ* transform, PJ_DIRECTION direction,
                     std::vector<std::pair<double, double>>& points)
{
  if (points.empty()) {
    return;
  }
  const double nan = std::numeric_limits<double>::quiet_NaN();
  if (transform == nullptr) {
    for (std::pair<double, double>& point : points) {
      point = {nan, nan};
    }
    return;
  }
  std::vector<PJ_COORD> coordinates(points.size());
  for (std::size_t i = 0; i < points.size(); ++i) {
    coordinates[i] = proj_coord(points[i].first, points[i].second, 0.0, 0.0);
  }
  proj_errno_reset(transform);
  proj_trans_array(transform, direction, coordinates.size(),
                   coordinates.data());
  for (std::size_t i = 0; i < points.size(); ++i) {
    const double x = coordinates[i].xy.x;
    const double y = coordinates[i].xy.y;
    const bool good = std::isfinite(x) && std::isfinite(y) &&
                      std::abs(x) < 1e15 && std::abs(y) < 1e15;
    points[i] = good ? std::make_pair(x, y) : std::make_pair(nan, nan);
  }
}

} // namespace

std::optional<WmtsCrsInfo> wmtsCrsInfo(const QString& code)
{
  static std::mutex mutex;
  static std::map<QString, std::optional<WmtsCrsInfo>> known;
  const std::lock_guard<std::mutex> lock(mutex);
  const std::map<QString, std::optional<WmtsCrsInfo>>::const_iterator found =
      known.find(code);
  if (found != known.end()) {
    return found->second;
  }
  std::optional<WmtsCrsInfo> info = askProj(code);
  known.emplace(code, info);
  return info;
}

struct WmtsCrsTransform::Private {
  ProjContext context;
  PJ* transform = nullptr;

  Private() = default;
  ~Private()
  {
    if (transform != nullptr) {
      proj_destroy(transform);
    }
  }
  Private(const Private&) = delete;
  Private& operator=(const Private&) = delete;
  Private(Private&&) = delete;
  Private& operator=(Private&&) = delete;
};

WmtsCrsTransform::WmtsCrsTransform(const QString& code)
    : m_code(code), d(std::make_unique<Private>())
{
  if (!d->context.isValid()) {
    return;
  }
  setSearchPaths(d->context);
  PJ* raw = proj_create_crs_to_crs(d->context.get(), "EPSG:4326",
                                   code.toUtf8().constData(), nullptr);
  if (raw == nullptr) {
    return;
  }
  // Longitude first in, easting first out, whatever the CRSs' own order.
  d->transform = proj_normalize_for_visualization(d->context.get(), raw);
  proj_destroy(raw);
}

WmtsCrsTransform::~WmtsCrsTransform() = default;

bool WmtsCrsTransform::isValid() const
{
  return d->transform != nullptr;
}

void WmtsCrsTransform::forward(
    std::vector<std::pair<double, double>>& points) const
{
  transformPoints(d->transform, PJ_FWD, points);
}

void WmtsCrsTransform::inverse(
    std::vector<std::pair<double, double>>& points) const
{
  transformPoints(d->transform, PJ_INV, points);
}

} // namespace ts
