// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/regions/regiongeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <unordered_set>
#include <utility>

#include <QCoreApplication>
#include <QJsonArray>

#include "ts/geo/geodetic.h"

namespace ts
{

namespace
{

constexpr double pi = 3.14159265358979323846;

/** Radius of the sphere with the WGS 84 ellipsoid's area, km. */
constexpr double authalicRadiusKm = 6371.0072;

/** Brings @p lonDeg into [−180°, 180°). */
double wrapLon(double lonDeg)
{
  return normaliseLonDeg(lonDeg);
}

/** Brings a longitude difference into (−180°, 180°]. */
double wrapDelta(double deltaDeg)
{
  double delta = std::fmod(deltaDeg, 360.0);
  if (delta > 180.0) {
    delta -= 360.0;
  } else if (delta <= -180.0) {
    delta += 360.0;
  }
  return delta;
}

GeoRect shifted(const GeoRect& rect, double byDeg)
{
  return GeoRect{rect.westDeg + byDeg, rect.southDeg, rect.eastDeg + byDeg,
                 rect.northDeg};
}

/** True if the rectangles share some area (touching edges don't count). */
bool overlaps(const GeoRect& a, const GeoRect& b)
{
  return a.westDeg < b.eastDeg && b.westDeg < a.eastDeg &&
         a.southDeg < b.northDeg && b.southDeg < a.northDeg;
}

/** True if @p inner lies inside @p outer (edges may coincide). */
bool within(const GeoRect& inner, const GeoRect& outer)
{
  return inner.westDeg >= outer.westDeg && inner.eastDeg <= outer.eastDeg &&
         inner.southDeg >= outer.southDeg && inner.northDeg <= outer.northDeg;
}

bool strictlyInside(const GeoRect& rect, double lonDeg, double latDeg)
{
  return lonDeg > rect.westDeg && lonDeg < rect.eastDeg &&
         latDeg > rect.southDeg && latDeg < rect.northDeg;
}

/**
 * Clips the segment @p a – @p b to @p rect (Liang–Barsky). Returns true
 * and the clipped part's parameters if some of it lies in the rectangle.
 */
bool clipSegment(const LonLat& a, const LonLat& b, const GeoRect& rect,
                 double& t0, double& t1)
{
  const double dx = b.lonDeg - a.lonDeg;
  const double dy = b.latDeg - a.latDeg;
  const std::array<double, 4> p{-dx, dx, -dy, dy};
  const std::array<double, 4> q{
      a.lonDeg - rect.westDeg, rect.eastDeg - a.lonDeg,
      a.latDeg - rect.southDeg, rect.northDeg - a.latDeg};
  t0 = 0.0;
  t1 = 1.0;
  for (std::size_t i = 0; i < 4; ++i) {
    if (p[i] == 0.0) {
      if (q[i] < 0.0) {
        return false;
      }
    } else {
      const double t = q[i] / p[i];
      if (p[i] < 0.0) {
        t0 = std::max(t0, t);
      } else {
        t1 = std::min(t1, t);
      }
    }
  }
  return t0 < t1;
}

GeoRect extentOf(const std::vector<LonLat>& vertices)
{
  GeoRect extent{vertices.front().lonDeg, vertices.front().latDeg,
                 vertices.front().lonDeg, vertices.front().latDeg};
  for (const LonLat& vertex : vertices) {
    extent.westDeg = std::min(extent.westDeg, vertex.lonDeg);
    extent.eastDeg = std::max(extent.eastDeg, vertex.lonDeg);
    extent.southDeg = std::min(extent.southDeg, vertex.latDeg);
    extent.northDeg = std::max(extent.northDeg, vertex.latDeg);
  }
  return extent;
}

QString translated(const char* text)
{
  return QCoreApplication::translate("ts::RegionGeometry", text);
}

bool fail(QString* errorString, const QString& message)
{
  if (errorString) {
    *errorString = message;
  }
  return false;
}

/**
 * True if the part of @p rect where the source has data (inside
 * @p dataBounds, if given) shares some area with @p region.
 */
bool wantedTile(const RegionGeometry& region, const GeoRect& rect,
                const std::optional<GeoRect>& dataBounds)
{
  if (!dataBounds) {
    return region.intersects(rect);
  }
  if (!overlaps(rect, *dataBounds)) {
    return false;
  }
  const GeoRect clipped{std::max(rect.westDeg, dataBounds->westDeg),
                        std::max(rect.southDeg, dataBounds->southDeg),
                        std::min(rect.eastDeg, dataBounds->eastDeg),
                        std::min(rect.northDeg, dataBounds->northDeg)};
  return region.intersects(clipped);
}

bool allData(const GeoRect& rect, const std::optional<GeoRect>& dataBounds)
{
  return !dataBounds || within(rect, *dataBounds);
}

long long pow4(int exponent)
{
  return 1LL << (2 * exponent);
}

} // namespace

RegionGeometry RegionGeometry::rectangle(double westDeg, double southDeg,
                                         double eastDeg, double northDeg)
{
  double south = std::clamp(southDeg, -90.0, 90.0);
  double north = std::clamp(northDeg, -90.0, 90.0);
  if (south > north) {
    std::swap(south, north);
  }
  double span = eastDeg - westDeg;
  if (span >= 360.0) {
    span = 360.0;
  } else {
    span = std::fmod(span, 360.0);
    if (span <= 0.0) {
      span += 360.0;
    }
  }
  const double west = span >= 360.0 ? -180.0 : wrapLon(westDeg);
  const double east = west + span;
  RegionGeometry geometry;
  geometry.m_kind = Kind::Rectangle;
  geometry.m_vertices = {LonLat{west, south}, LonLat{east, south},
                         LonLat{east, north}, LonLat{west, north}};
  geometry.m_extent = GeoRect{west, south, east, north};
  return geometry;
}

std::optional<RegionGeometry>
RegionGeometry::polygon(const std::vector<LonLat>& vertices,
                        QString* errorString)
{
  // Drop repeated vertices, including a closing copy of the first.
  std::vector<LonLat> ring;
  for (const LonLat& vertex : vertices) {
    if (!std::isfinite(vertex.lonDeg) || !std::isfinite(vertex.latDeg) ||
        vertex.latDeg < -90.0 || vertex.latDeg > 90.0) {
      fail(errorString, translated("A point of the area is not on the Earth "
                                   "(latitude beyond ±90°)."));
      return std::nullopt;
    }
    if (ring.empty() ||
        std::abs(wrapDelta(vertex.lonDeg - ring.back().lonDeg)) > 1e-12 ||
        vertex.latDeg != ring.back().latDeg) {
      ring.push_back(vertex);
    }
  }
  while (ring.size() > 1 &&
         std::abs(wrapDelta(ring.back().lonDeg - ring.front().lonDeg)) <=
             1e-12 &&
         ring.back().latDeg == ring.front().latDeg) {
    ring.pop_back();
  }
  if (ring.size() < 3) {
    fail(errorString,
         translated("An area needs at least three different points."));
    return std::nullopt;
  }
  // Unwrap: each vertex within 180° of the one before.
  ring.front().lonDeg = wrapLon(ring.front().lonDeg);
  for (std::size_t i = 1; i < ring.size(); ++i) {
    ring[i].lonDeg =
        ring[i - 1].lonDeg + wrapDelta(ring[i].lonDeg - ring[i - 1].lonDeg);
  }
  const double closing =
      ring.back().lonDeg + wrapDelta(ring.front().lonDeg - ring.back().lonDeg);
  if (std::abs(closing - ring.front().lonDeg) > 1e-9) {
    fail(errorString, translated("An area can't go round a pole; use a "
                                 "rectangle that reaches the pole instead."));
    return std::nullopt;
  }
  // Move the whole ring so that its western edge lies in [−180°, 180°).
  const double west = extentOf(ring).westDeg;
  const double shift = wrapLon(west) - west;
  for (LonLat& vertex : ring) {
    vertex.lonDeg += shift;
  }
  RegionGeometry geometry;
  geometry.m_kind = Kind::Polygon;
  geometry.m_vertices = std::move(ring);
  geometry.m_extent = extentOf(geometry.m_vertices);
  if (geometry.areaKm2() <= 0.0) {
    fail(errorString, translated("The area has no size."));
    return std::nullopt;
  }
  return geometry;
}

bool RegionGeometry::isValid() const
{
  return !m_vertices.empty() && m_extent.widthDeg() > 0.0 &&
         m_extent.heightDeg() > 0.0;
}

bool RegionGeometry::containsUnwrapped(double latDeg, double lonDeg) const
{
  if (!m_extent.contains(latDeg, lonDeg)) {
    return false;
  }
  if (m_kind == Kind::Rectangle) {
    return true;
  }
  // Even–odd rule, a ray towards the east.
  bool inside = false;
  const std::size_t n = m_vertices.size();
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    const LonLat& a = m_vertices[i];
    const LonLat& b = m_vertices[j];
    if ((a.latDeg > latDeg) != (b.latDeg > latDeg)) {
      const double crossLon = a.lonDeg + (latDeg - a.latDeg) *
                                             (b.lonDeg - a.lonDeg) /
                                             (b.latDeg - a.latDeg);
      if (lonDeg < crossLon) {
        inside = !inside;
      }
    }
  }
  return inside;
}

bool RegionGeometry::contains(double latDeg, double lonDeg) const
{
  if (!isValid()) {
    return false;
  }
  // The one copy of the longitude in [west, west + 360°).
  const double offset = std::fmod(lonDeg - m_extent.westDeg, 360.0);
  const double lon =
      m_extent.westDeg + (offset < 0.0 ? offset + 360.0 : offset);
  return containsUnwrapped(latDeg, lon) ||
         (lon == m_extent.westDeg && containsUnwrapped(latDeg, lon + 360.0));
}

bool RegionGeometry::edgeCrossesInterior(const GeoRect& rect) const
{
  const std::size_t n = m_vertices.size();
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    const LonLat& a = m_vertices[j];
    const LonLat& b = m_vertices[i];
    double t0 = 0.0;
    double t1 = 0.0;
    if (clipSegment(a, b, rect, t0, t1)) {
      const double t = 0.5 * (t0 + t1);
      if (strictlyInside(rect, a.lonDeg + t * (b.lonDeg - a.lonDeg),
                         a.latDeg + t * (b.latDeg - a.latDeg))) {
        return true;
      }
    }
  }
  return false;
}

bool RegionGeometry::intersectsShifted(const GeoRect& rect) const
{
  if (!overlaps(rect, m_extent)) {
    return false;
  }
  if (m_kind == Kind::Rectangle) {
    return true;
  }
  if (containsUnwrapped(rect.centreLatDeg(), rect.centreLonDeg())) {
    return true;
  }
  for (const LonLat& vertex : m_vertices) {
    if (strictlyInside(rect, vertex.lonDeg, vertex.latDeg)) {
      return true;
    }
  }
  return edgeCrossesInterior(rect);
}

bool RegionGeometry::coversShifted(const GeoRect& rect) const
{
  if (!within(rect, m_extent)) {
    return false;
  }
  if (m_kind == Kind::Rectangle) {
    return true;
  }
  if (!containsUnwrapped(rect.centreLatDeg(), rect.centreLonDeg())) {
    return false;
  }
  for (const LonLat& vertex : m_vertices) {
    if (strictlyInside(rect, vertex.lonDeg, vertex.latDeg)) {
      return false;
    }
  }
  return !edgeCrossesInterior(rect);
}

bool RegionGeometry::intersects(const GeoRect& rect) const
{
  // The region lies in [west, west + 360°) with west ≥ −180°, so a
  // rectangle within ±180° meets it as it is or one turn further east.
  return isValid() &&
         (intersectsShifted(rect) || intersectsShifted(shifted(rect, 360.0)));
}

bool RegionGeometry::covers(const GeoRect& rect) const
{
  return isValid() &&
         (coversShifted(rect) || coversShifted(shifted(rect, 360.0)));
}

double RegionGeometry::areaKm2() const
{
  if (m_vertices.size() < 3) {
    return 0.0;
  }
  // Green's theorem: area = −∮ sin φ dλ, each edge straight in (λ, φ).
  const double radians = pi / 180.0;
  double sum = 0.0;
  const std::size_t n = m_vertices.size();
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    const double lon1 = m_vertices[j].lonDeg * radians;
    const double lat1 = m_vertices[j].latDeg * radians;
    const double lon2 = m_vertices[i].lonDeg * radians;
    const double lat2 = m_vertices[i].latDeg * radians;
    const double meanSin =
        std::abs(lat2 - lat1) < 1e-12
            ? std::sin(lat1)
            : (std::cos(lat1) - std::cos(lat2)) / (lat2 - lat1);
    sum += (lon2 - lon1) * meanSin;
  }
  return std::abs(sum) * authalicRadiusKm * authalicRadiusKm;
}

QJsonObject RegionGeometry::toJson() const
{
  QJsonObject json;
  if (m_kind == Kind::Rectangle) {
    json.insert(QStringLiteral("kind"), QStringLiteral("rectangle"));
    json.insert(QStringLiteral("west"), m_extent.westDeg);
    json.insert(QStringLiteral("south"), m_extent.southDeg);
    json.insert(QStringLiteral("east"), m_extent.eastDeg);
    json.insert(QStringLiteral("north"), m_extent.northDeg);
    return json;
  }
  QJsonArray vertices;
  for (const LonLat& vertex : m_vertices) {
    vertices.append(QJsonArray{vertex.lonDeg, vertex.latDeg});
  }
  json.insert(QStringLiteral("kind"), QStringLiteral("polygon"));
  json.insert(QStringLiteral("vertices"), vertices);
  return json;
}

std::optional<RegionGeometry> RegionGeometry::fromJson(const QJsonObject& json,
                                                       QString* errorString)
{
  const QString kind = json.value(QStringLiteral("kind")).toString();
  if (kind == QLatin1String("rectangle")) {
    const std::array<const char*, 4> names{"west", "south", "east", "north"};
    for (const char* name : names) {
      if (!json.value(QLatin1String(name)).isDouble()) {
        fail(errorString, translated("The region's rectangle is incomplete."));
        return std::nullopt;
      }
    }
    const double west = json.value(QStringLiteral("west")).toDouble();
    const double east = json.value(QStringLiteral("east")).toDouble();
    RegionGeometry geometry =
        rectangle(west, json.value(QStringLiteral("south")).toDouble(),
                  east - west >= 360.0 ? west + 360.0 : east,
                  json.value(QStringLiteral("north")).toDouble());
    if (!geometry.isValid()) {
      fail(errorString, translated("The area has no size."));
      return std::nullopt;
    }
    return geometry;
  }
  if (kind == QLatin1String("polygon")) {
    std::vector<LonLat> vertices;
    const QJsonArray array = json.value(QStringLiteral("vertices")).toArray();
    // NOLINTNEXTLINE(modernize-loop-convert)
    for (int i = 0; i < array.size(); ++i) {
      const QJsonArray pair = array.at(i).toArray();
      if (pair.size() != 2 || !pair.at(0).isDouble() ||
          !pair.at(1).isDouble()) {
        fail(errorString, translated("A point of the region is not a pair "
                                     "of numbers."));
        return std::nullopt;
      }
      vertices.push_back(LonLat{pair.at(0).toDouble(), pair.at(1).toDouble()});
    }
    return polygon(vertices, errorString);
  }
  fail(errorString, translated("The region's area is of an unknown kind."));
  return std::nullopt;
}

bool RegionGeometry::operator==(const RegionGeometry& other) const
{
  if (m_kind != other.m_kind || m_vertices.size() != other.m_vertices.size()) {
    return false;
  }
  for (std::size_t i = 0; i < m_vertices.size(); ++i) {
    if (m_vertices[i].lonDeg != other.m_vertices[i].lonDeg ||
        m_vertices[i].latDeg != other.m_vertices[i].latDeg) {
      return false;
    }
  }
  return true;
}

// --- Tiles ------------------------------------------------------------------

long long RegionTileCount::total() const
{
  long long sum = 0;
  for (const long long count : perLevel) {
    sum += count;
  }
  return sum;
}

RegionTileCount countRegionTiles(const RegionGeometry& region, int minimumLevel,
                                 int maximumLevel,
                                 const std::optional<GeoRect>& dataBounds)
{
  RegionTileCount counts;
  const int first = std::clamp(minimumLevel, 0, TileKey::maximumLevel);
  const int last = std::clamp(maximumLevel, 0, TileKey::maximumLevel);
  counts.minimumLevel = first;
  if (last < first || !region.isValid()) {
    return counts;
  }
  counts.perLevel.assign(static_cast<std::size_t>(last - first + 1), 0);
  std::vector<TileKey> stack;
  for (const TileKey root : TileKey::roots()) {
    stack.push_back(root);
  }
  while (!stack.empty()) {
    const TileKey key = stack.back();
    stack.pop_back();
    const GeoRect rect = key.bounds();
    if (!wantedTile(region, rect, dataBounds)) {
      continue;
    }
    if (allData(rect, dataBounds) && region.covers(rect)) {
      // Every descendant is in: count them without visiting them.
      for (int level = std::max(key.level, first); level <= last; ++level) {
        counts.perLevel[static_cast<std::size_t>(level - first)] +=
            pow4(level - key.level);
      }
      continue;
    }
    if (key.level >= first) {
      ++counts.perLevel[static_cast<std::size_t>(key.level - first)];
    }
    if (key.level < last) {
      for (int child = 0; child < 4; ++child) {
        stack.push_back(key.child(child));
      }
    }
  }
  return counts;
}

RegionTileIterator::RegionTileIterator(RegionGeometry region, int minimumLevel,
                                       int maximumLevel,
                                       std::optional<GeoRect> dataBounds)
    : m_region(std::move(region)),
      m_maximumLevel(std::clamp(maximumLevel, 0, TileKey::maximumLevel)),
      m_dataBounds(dataBounds),
      m_level(std::clamp(minimumLevel, 0, TileKey::maximumLevel))
{
  if (m_region.isValid()) {
    startLevel();
  }
}

void RegionTileIterator::startLevel()
{
  // Popped from the back: the western root first.
  const std::array<TileKey, 2> roots = TileKey::roots();
  m_stack.assign(roots.rbegin(), roots.rend());
}

bool RegionTileIterator::wanted(TileKey key) const
{
  return wantedTile(m_region, key.bounds(), m_dataBounds);
}

std::optional<TileKey> RegionTileIterator::next()
{
  while (m_level <= m_maximumLevel) {
    while (!m_stack.empty()) {
      const TileKey key = m_stack.back();
      m_stack.pop_back();
      if (!wanted(key)) {
        continue;
      }
      if (key.level == m_level) {
        return key;
      }
      for (int child = 3; child >= 0; --child) {
        m_stack.push_back(key.child(child));
      }
    }
    ++m_level;
    if (m_level <= m_maximumLevel && m_region.isValid()) {
      startLevel();
    }
  }
  return std::nullopt;
}

std::vector<TileKey> sampleRegionTiles(const RegionGeometry& region,
                                       const RegionTileCount& counts,
                                       const std::optional<GeoRect>& dataBounds,
                                       int count, std::mt19937_64& random)
{
  std::vector<TileKey> samples;
  if (count <= 0 || counts.total() <= 0 || !region.isValid()) {
    return samples;
  }
  std::vector<double> weights;
  for (const long long levelCount : counts.perLevel) {
    weights.push_back(static_cast<double>(levelCount));
  }
  std::discrete_distribution<int> pickLevel(weights.begin(), weights.end());
  const GeoRect& extent = region.extent();
  std::uniform_real_distribution<double> pickLon(extent.westDeg,
                                                 extent.eastDeg);
  std::uniform_real_distribution<double> pickLat(extent.southDeg,
                                                 extent.northDeg);
  std::unordered_set<TileKey, TileKeyHash> seen;
  const long long wanted = std::min<long long>(count, counts.total());
  for (long long attempt = 0; attempt < 200LL * count &&
                              static_cast<long long>(samples.size()) < wanted;
       ++attempt) {
    const int level = counts.minimumLevel + pickLevel(random);
    const double lon = pickLon(random);
    const double lat = pickLat(random);
    if (!region.contains(lat, lon)) {
      continue;
    }
    const TileKey key = TileKey::containing(level, Geodetic{lat, lon, 0.0});
    if (wantedTile(region, key.bounds(), dataBounds) &&
        seen.insert(key).second) {
      samples.push_back(key);
    }
  }
  return samples;
}

} // namespace ts
