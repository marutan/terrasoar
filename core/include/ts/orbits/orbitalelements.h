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
 * Orbital elements of satellites as two-line element sets (TLEs) and
 * Orbit Mean-Elements Messages (OMM) give them, and reading them.
 */
#ifndef TS_ORBITS_ORBITALELEMENTS_H
#define TS_ORBITS_ORBITALELEMENTS_H

#include <cstdint>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace ts
{

/** The Julian date of the Unix epoch, 1970-01-01T00:00Z. */
constexpr double unixEpochJd = 2440587.5;

/**
 * A time as a Julian date split in two (whole days ending in .5, and the
 * fraction of a day after them), which keeps microseconds over centuries.
 *
 * @reentrant
 */
struct JulianDate {
  /** Whole days: a Julian date at 0h UTC (ending in .5). */
  double day = unixEpochJd;
  /** The fraction of a day after day, in [0, 1). */
  double fraction = 0.0;

  /** Returns @p time (converted to UTC) as a Julian date. */
  static JulianDate fromDateTime(const QDateTime& time);

  /** Returns it as a UTC time, to the millisecond. */
  QDateTime toDateTime() const;

  /** Returns the minutes from @p other to this date. */
  double minutesSince(JulianDate other) const
  {
    return ((day - other.day) + (fraction - other.fraction)) * 1440.0;
  }

  /** Returns it in one number (losing about 20 µs of precision). */
  double value() const { return day + fraction; }
};

/**
 * One satellite's mean orbital elements, fitted for SGP4/SDP4, as read
 * from a TLE or an OMM (units as written there, angles in degrees).
 *
 * @reentrant
 */
struct OrbitalElements {
  /** The satellite's name (line 0 of a three-line set); may be empty. */
  QString name;
  /** The NORAD catalogue number (Alpha-5 numbers decoded). */
  int32_t catalogueNumber = 0;
  /** The classification: 'U' unclassified, 'C', 'S'. */
  char classification = 'U';
  /** The international (COSPAR) designator, "1998-067A"; may be empty. */
  QString internationalDesignator;
  /** The epoch: the time the elements are for (UTC). */
  JulianDate epoch;
  /** First time derivative of the mean motion over 2, revolutions/day². */
  double meanMotionDot = 0.0;
  /** Second time derivative of the mean motion over 6, revolutions/day³. */
  double meanMotionDdot = 0.0;
  /** The drag term B*, per Earth radius. */
  double bstar = 0.0;
  /** Inclination, degrees. */
  double inclinationDeg = 0.0;
  /** Right ascension of the ascending node, degrees. */
  double raanDeg = 0.0;
  /** Eccentricity. */
  double eccentricity = 0.0;
  /** Argument of perigee, degrees. */
  double argOfPerigeeDeg = 0.0;
  /** Mean anomaly, degrees. */
  double meanAnomalyDeg = 0.0;
  /** Mean motion, revolutions per day (Kozai mean motion). */
  double meanMotionRevPerDay = 0.0;
  /** Ephemeris type (0 for SGP4 elements). */
  int32_t ephemerisType = 0;
  /** Element set number. */
  int32_t elementSetNumber = 0;
  /** Revolution number at epoch. */
  int32_t revolutionNumber = 0;

  /** Returns the epoch as a UTC time, to the millisecond. */
  QDateTime epochTime() const { return epoch.toDateTime(); }

  /** Returns the time from the epoch to @p time, days. */
  double ageDays(const QDateTime& time) const;

  /** Returns the period from the mean motion, minutes. */
  double periodMinutes() const;

  /** Returns the name, or the catalogue number if it has none. */
  QString displayName() const;

  /** Equality of every member. */
  bool operator==(const OrbitalElements& other) const;
  /** Inequality. */
  bool operator!=(const OrbitalElements& other) const
  {
    return !(*this == other);
  }
};

/** How strictly TLEs are read. */
struct TleOptions {
  /** Check the checksum at the end of each line (column 69). */
  bool checkChecksums = true;
};

/**
 * Reads one two-line element set (@p line1, @p line2, and the name from
 * line 0, if any). Columns are checked against the format; nothing (and
 * the reason in @p errorString) if the lines aren't a TLE.
 */
std::optional<OrbitalElements> parseTle(const QString& line1,
                                        const QString& line2,
                                        const QString& name = QString(),
                                        QString* errorString = nullptr,
                                        TleOptions options = {});

/** Returns the checksum of a TLE line: its digits, and 1 for each '-'. */
int32_t tleChecksum(const QString& line);

/**
 * Returns the catalogue number written in @p text (five digits, or Alpha-5:
 * a letter for the hundred-thousands then four digits); nothing if it
 * isn't one.
 */
std::optional<int32_t> parseCatalogueNumber(const QString& text);

/** What reading many element sets gave. */
struct ElementSetList {
  /** The element sets read, in order. */
  std::vector<OrbitalElements> elements;
  /** What couldn't be read, one entry each ("Line 12: …"). */
  QStringList errors;
};

/**
 * Reads the TLEs in @p text, two-line or three-line ("0 NAME" or a plain
 * name before line 1), with blank lines and '#' comments allowed between
 * them.
 */
ElementSetList parseTleText(const QString& text, TleOptions options = {});

/**
 * Reads one OMM in its JSON form (CelesTrak's FORMAT=json, Space-Track's
 * omm), whose members may be numbers or strings.
 */
std::optional<OrbitalElements> parseOmm(const QJsonObject& object,
                                        QString* errorString = nullptr);

/** Reads a JSON array of OMMs (or a single one). */
ElementSetList parseOmmJson(const QByteArray& json);

/**
 * Reads element sets in either form: OMM JSON if @p data starts with '['
 * or '{', else TLE text.
 */
ElementSetList parseElementSets(const QByteArray& data,
                                TleOptions options = {});

/** Returns @p elements as TLE lines: line 1 and line 2. */
QStringList formatTle(const OrbitalElements& elements);

} // namespace ts

#endif // TS_ORBITS_ORBITALELEMENTS_H
