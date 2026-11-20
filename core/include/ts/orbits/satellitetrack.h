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
 * Where a satellite is, and its track over spans of time before and after
 * then: positions on the Earth (WGS 84), ground tracks split at the
 * antimeridian, and the footprint seen from the ground.
 */
#ifndef TS_ORBITS_SATELLITETRACK_H
#define TS_ORBITS_SATELLITETRACK_H

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

#include <QJsonObject>

#include "ts/geo/geodetic.h"
#include "ts/maths/vec3.h"
#include "ts/orbits/orbitalelements.h"
#include "ts/orbits/sgp4.h"
#include "ts/regions/regiongeometry.h"

namespace ts
{

/**
 * Where a satellite is at one time, Earth-fixed.
 *
 * @reentrant
 */
struct SatelliteState {
  /** None, or why SGP4 gave no position (the rest is then meaningless). */
  Sgp4Error error = Sgp4Error::None;
  /** Position, ECEF metres. */
  Vec3 positionEcefM;
  /** Velocity relative to the Earth, ECEF metres per second. */
  Vec3 velocityEcefMS;
  /** The point below it and its height above the ellipsoid. */
  Geodetic geodetic;
  /** Speed in space (the inertial TEME frame), kilometres per second. */
  double speedKmS = 0.0;

  /** Returns true if there is a position. */
  bool ok() const { return error == Sgp4Error::None; }
};

/** Returns where the satellite @p sgp4 is at @p time (UTC as UT1). */
SatelliteState satelliteState(const Sgp4& sgp4, JulianDate time);

/** How a span of time is given. */
enum class SpanUnit : uint8_t {
  /** In minutes. */
  Minutes,
  /** In the satellite's orbits (its period). */
  Orbits,
};

/**
 * A span of time before or after the clock's time: minutes, or orbits of
 * the satellite. Stored in projects as `{"amount": 1, "unit": "orbits"}`.
 *
 * @reentrant
 */
struct TimeSpan {
  /** How many minutes or orbits (≥ 0). */
  double amount = 1.0;
  /** What amount counts. */
  SpanUnit unit = SpanUnit::Orbits;

  /** Returns the span in minutes for a satellite of @p periodMinutes. */
  double minutes(double periodMinutes) const;

  /** Returns the JSON form. */
  QJsonObject toJson() const;

  /** Reads the JSON form; nothing if it isn't one. */
  static std::optional<TimeSpan> fromJson(const QJsonObject& json);

  /** Equality. */
  bool operator==(TimeSpan other) const
  {
    return amount == other.amount && unit == other.unit;
  }
  /** Inequality. */
  bool operator!=(TimeSpan other) const { return !(*this == other); }
};

/** One point of a track. */
struct TrackPoint {
  /** Minutes from the time the track is for (negative before it). */
  double minutes = 0.0;
  /** Position, ECEF metres. */
  Vec3 positionEcefM;
  /** The point below it and its height. */
  Geodetic geodetic;
};

/**
 * A satellite's track around one time: where it was over the past span,
 * oldest first, ending at the time; and where it will be over the future
 * span, starting at the time. Both run exactly to the ends of their
 * spans; between, the points lie on a fixed grid of times (see
 * TrackSampler). Points SGP4 can't give (a decayed orbit) are left out.
 *
 * @reentrant
 */
struct SatelliteTrack {
  /** The past part, oldest first; its last point is at the time. */
  std::vector<TrackPoint> past;
  /** The future part; its first point is at the time. */
  std::vector<TrackPoint> future;
};

/**
 * Returns the step between the points of tracks for a satellite of
 * @p periodMinutes: about 1/120 of an orbit (3°), from 15 s to 30 min.
 */
double trackStepMinutes(double periodMinutes);

/**
 * Samples one satellite's track, keeping the points it computed: they lie
 * on a fixed grid of times (multiples of trackStepMinutes() from the
 * epoch), so as the clock moves only the points that come into the span
 * are computed, and those that leave it are dropped. Only the ends (the
 * span's limits and the time itself) are computed each time.
 *
 * @reentrant
 */
class TrackSampler
{
public:
  /** Samples @p sgp4 (which must not be null). */
  explicit TrackSampler(std::shared_ptr<const Sgp4> sgp4);

  /** Returns the satellite. */
  const std::shared_ptr<const Sgp4>& sgp4() const { return m_sgp4; }

  /** Returns the step between grid points, minutes. */
  double stepMinutes() const { return m_stepMinutes; }

  /**
   * Returns the track from @p pastMinutes before @p time to
   * @p futureMinutes after it (each ≥ 0).
   */
  SatelliteTrack track(JulianDate time, double pastMinutes,
                       double futureMinutes);

  /** Returns how many grid points were computed so far (for tests). */
  int64_t gridPointsComputed() const { return m_computed; }

private:
  std::optional<TrackPoint> pointAt(double minutesSinceEpoch) const;

  std::shared_ptr<const Sgp4> m_sgp4;
  double m_stepMinutes = 1.0;
  /** Grid points by index (minutes since the epoch / step); absolute. */
  std::map<int64_t, std::optional<TrackPoint>> m_grid;
  int64_t m_computed = 0;
};

/**
 * Returns the ground track through @p points (longitudes in [−180, 180])
 * in pieces that don't cross the antimeridian: where a step jumps across
 * it, one piece ends on it and the next starts there on the other side.
 */
std::vector<std::vector<LonLat>>
splitAtAntimeridian(const std::vector<LonLat>& points);

/**
 * Returns the Earth central angle, radians, from the point below a
 * satellite at @p heightM to the edge of its footprint: where it is seen
 * at @p minElevationDeg above the horizon (a sphere of the Earth's mean
 * radius).
 */
double footprintRadiusRad(double heightM, double minElevationDeg);

/**
 * Returns the edge of the footprint of a satellite at @p subSatellite (its
 * height used): @p points points round the ring, longitudes in
 * [−180, 180], first not repeated at the end.
 */
std::vector<LonLat> footprint(const Geodetic& subSatellite,
                              double minElevationDeg, int32_t points = 90);

} // namespace ts

#endif // TS_ORBITS_SATELLITETRACK_H
