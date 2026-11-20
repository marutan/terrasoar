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
 * The SGP4/SDP4 orbit propagator, which TLEs are fitted for.
 */
#ifndef TS_ORBITS_SGP4_H
#define TS_ORBITS_SGP4_H

#include <cstdint>
#include <memory>

#include <QString>

#include "ts/maths/vec3.h"
#include "ts/orbits/orbitalelements.h"

namespace ts
{

/** The Earth's gravity constants SGP4 uses. */
enum class GravityModel : uint8_t {
  /** WGS 72 with Spacetrack Report #3's rounded xke. */
  Wgs72Old,
  /** WGS 72: what TLEs are fitted with (the default). */
  Wgs72,
  /** WGS 84 (not what TLEs are fitted with; for comparison). */
  Wgs84,
};

/** How SGP4 runs (Vallado et al.'s "opsmode"). */
enum class Sgp4Mode : uint8_t {
  /** As the US Air Force's operational code ('a'). */
  Afspc,
  /** Improved: modern sidereal time ('i', the default). */
  Improved,
};

/** Why SGP4 gave no position (the reference error codes). */
enum class Sgp4Error : uint8_t {
  /** None: the position is good. */
  None = 0,
  /** The mean eccentricity left [0, 1) (or fell below −0.001). */
  MeanEccentricity = 1,
  /** The mean motion fell to zero or below. */
  MeanMotion = 2,
  /** The perturbed eccentricity left [0, 1] (deep space). */
  PerturbedEccentricity = 3,
  /** The semi-latus rectum fell below zero. */
  SemiLatusRectum = 4,
  /** The orbit fell inside the Earth: the satellite has decayed. */
  Decayed = 6,
};

/** Returns what @p error means, for the user. */
QString sgp4ErrorText(Sgp4Error error);

/**
 * A position and velocity in TEME, the True Equator, Mean Equinox frame
 * SGP4 works in.
 *
 * @reentrant
 */
struct TemeState {
  /** Position, kilometres. */
  Vec3 positionKm;
  /** Velocity, kilometres per second. */
  Vec3 velocityKmS;
};

/** What propagating to one time gave. */
struct Sgp4Result {
  /** None, or why there is no position. */
  Sgp4Error error = Sgp4Error::None;
  /**
   * The state (TEME). Meaningless after an error, except Decayed, where
   * it is the position inside the Earth.
   */
  TemeState teme;

  /** Returns true if there is a position. */
  bool ok() const { return error == Sgp4Error::None; }
};

/**
 * SGP4 and SDP4: the analytical models two-line element sets are made
 * for, near-Earth (periods under 225 minutes) and deep-space (SDP4, with
 * the Sun's and Moon's pull and the 12 and 24-hour resonances).
 *
 * Terra Soar's own implementation of the published reference: Spacetrack
 * Report #3 as revised by Vallado, Crawford, Hujsak and Kelso,
 * "Revisiting Spacetrack Report #3" (AIAA 2006-6753), with its later
 * fixes, so it matches the reference's test set to millimetres. See
 * docs/design/satellites.md.
 *
 * Built once per element set (the costly part); propagate() is const and
 * keeps no state, so one instance can be shared between threads.
 *
 * @threadsafe
 */
class Sgp4
{
public:
  /** Prepares @p elements for propagation. */
  explicit Sgp4(const OrbitalElements& elements,
                GravityModel gravity = GravityModel::Wgs72,
                Sgp4Mode mode = Sgp4Mode::Improved);

  /** Returns the elements. */
  const OrbitalElements& elements() const { return m_elements; }

  /** Returns the error propagating to the epoch gave (None if good). */
  Sgp4Error epochError() const;

  /** Returns true if it uses SDP4, the deep-space model. */
  bool isDeepSpace() const;

  /** Returns the Brouwer (un-Kozai'd) mean motion, radians per minute. */
  double meanMotionRadPerMin() const;

  /** Returns the anomalistic period, minutes. */
  double periodMinutes() const;

  /** Returns the state @p minutes after the epoch (before it if < 0). */
  Sgp4Result propagate(double minutes) const;

  /** Returns the state at @p time. */
  Sgp4Result propagate(JulianDate time) const;

private:
  struct Model;
  OrbitalElements m_elements;
  std::shared_ptr<const Model> m_model;
};

} // namespace ts

#endif // TS_ORBITS_SGP4_H
