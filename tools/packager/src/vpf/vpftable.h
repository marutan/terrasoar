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
 * Tables of the Vector Product Format (VPF, MIL-STD-2407): the header,
 * every column type, fixed and variable-length rows, both byte orders,
 * and the thematic indexes. See docs/tools/packager.md.
 */
#ifndef TS_PACKAGER_VPF_VPFTABLE_H
#define TS_PACKAGER_VPF_VPFTABLE_H

#include <cstdint>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QString>

namespace ts::packager::vpf
{

/** One coordinate of a coordinate column (z is 0 for 2D columns). */
struct Coordinate {
  /** Longitude (x), degrees in VMAP0. */
  double x = 0.0;
  /** Latitude (y), degrees in VMAP0. */
  double y = 0.0;
  /** Height (z), or 0. */
  double z = 0.0;

  /** Returns true if both have the same x and y (z is not compared). */
  bool operator==(const Coordinate& other) const
  {
    return x == other.x && y == other.y;
  }
  /** Returns true if x or y differ. */
  bool operator!=(const Coordinate& other) const { return !(*this == other); }
};

/**
 * A triplet id (column type K): a row id in this tile, and for a
 * primitive across a tile edge the tile and the row id there. Missing
 * parts are -1.
 */
struct TripletId {
  /** The row id in the table's own tile, or -1. */
  int32_t id = -1;
  /** The other tile's id (tileref.aft), or -1. */
  int32_t tile = -1;
  /** The row id in the other tile, or -1. */
  int32_t external = -1;
};

/** One column of a table, as its header describes it. */
struct Column {
  /** Name (lower case, as in the header). */
  QString name;
  /**
   * Type letter: T, L, N, M (text), F (float), R (double), S (short
   * integer), I (integer), D (date), K (triplet id), C, B, Z, Y (2D float,
   * 2D double, 3D float, 3D double coordinates) or X (null).
   */
  char type = 'T';
  /** Elements per value; -1 for a variable count ("*"). */
  int32_t count = 1;
  /** P (primary key), U (unique), N (not a key). */
  char keyType = 'N';
  /** Description. */
  QString description;
  /** Value description table naming its codes ("int.vdt"), or empty. */
  QString valueTable;
  /** Thematic index file, or empty. */
  QString thematicIndex;
  /** Narrative table, or empty. */
  QString narrative;

  /** Returns true for a variable-length column. */
  bool isVariable() const { return count < 0; }
  /** Returns true for a text column (T, L, N or M). */
  bool isText() const;
  /** Returns true for a coordinate column (C, B, Z or Y). */
  bool isCoordinates() const;
  /** Returns true for a whole-number column (S or I). */
  bool isInteger() const { return type == 'S' || type == 'I'; }
  /** Returns true for a floating-point column (F or R). */
  bool isReal() const { return type == 'F' || type == 'R'; }
};

/** One value of a row. */
struct Value {
  /** What it holds. */
  enum class Kind {
    Null,        ///< Nothing (type X, or a null value).
    Text,        ///< Text.
    Integer,     ///< S or I.
    Real,        ///< F or R (NaN is VPF's null for them).
    Triplet,     ///< K.
    Coordinates, ///< C, B, Z or Y.
    Numbers,     ///< An S, I, F or R column with more than one element.
  };
  /** What it holds. */
  Kind kind = Kind::Null;
  /** Text, trailing blanks removed. */
  QString text;
  /** S or I. */
  int64_t integer = 0;
  /** F or R. */
  double real = 0.0;
  /** K. */
  TripletId triplet;
  /** C, B, Z or Y. */
  std::vector<Coordinate> coordinates;
  /** Several S, I, F or R elements. */
  std::vector<double> numbers;
};

/**
 * One VPF table read whole into memory: header and rows. Text is decoded
 * as Latin-1 (VMAP0's level 1 text). Rows are read in order (the
 * variable-length index, `*.?x`, isn't needed for that; check() compares
 * it with the rows).
 *
 * @reentrant
 */
class Table
{
public:
  /**
   * Reads the table at @p path (a file path GDAL can open: plain, or
   * /vsizip/…). Returns nothing and sets @p errorString, naming the file
   * and the row, if it can't be read or is malformed.
   */
  static std::optional<Table> read(const QString& path, QString* errorString);

  /** Parses a table from its bytes; @p path is only for messages. */
  static std::optional<Table> parse(const QByteArray& bytes,
                                    const QString& path, QString* errorString);

  /** Returns the file it was read from. */
  const QString& path() const { return m_path; }
  /** Returns the description from the header. */
  const QString& description() const { return m_description; }
  /** Returns the narrative table named in the header, or "-". */
  const QString& narrative() const { return m_narrative; }
  /** Returns true for a big-endian table ("M"). */
  bool isBigEndian() const { return m_bigEndian; }
  /** Returns the columns. */
  const std::vector<Column>& columns() const { return m_columns; }
  /** Returns the column named @p name (case-insensitive), or -1. */
  int32_t columnIndex(const QString& name) const;
  /** Returns the number of rows. */
  int32_t rowCount() const { return static_cast<int32_t>(m_rows.size()); }
  /** Returns row @p row (0-based), each value in column order. */
  const std::vector<Value>& row(int32_t row) const
  {
    return m_rows.at(static_cast<std::size_t>(row));
  }
  /** Returns each row's offset in the file (for check()). */
  const std::vector<uint32_t>& rowOffsets() const { return m_offsets; }

  /**
   * Returns the row whose "id" column is @p id. VPF ids are 1-based row
   * numbers, so row id-1 is tried first; otherwise the rows are searched.
   * Returns -1 if there is none.
   */
  int32_t rowOfId(int64_t id) const;

  /** Returns the text in @p column of @p row, or an empty string. */
  QString text(int32_t row, int32_t column) const;
  /** Returns the integer in @p column of @p row, or @p fallback. */
  int64_t integer(int32_t row, int32_t column, int64_t fallback = -1) const;
  /** Returns the triplet in @p column of @p row (all -1 if none). */
  TripletId triplet(int32_t row, int32_t column) const;
  /** Returns the coordinates in @p column of @p row (none if none). */
  const std::vector<Coordinate>& coordinates(int32_t row, int32_t column) const;

  /**
   * Compares a variable-length index (`*.?x`, read from @p indexPath)
   * with the rows: as many entries, at the rows' offsets. False with
   * @p errorString if they differ.
   */
  bool checkIndex(const QString& indexPath, QString* errorString) const;

private:
  QString m_path;
  QString m_description;
  QString m_narrative;
  bool m_bigEndian = false;
  std::vector<Column> m_columns;
  std::vector<std::vector<Value>> m_rows;
  std::vector<uint32_t> m_offsets;
};

/**
 * A thematic index (`*.tti`, `*.ati`, `*.lti`, `*.pti`): for each value of
 * one column, the ids of the rows having it. Only inverted lists (index
 * type T) are supported; VMAP0 has no bit-array indexes.
 *
 * @reentrant
 */
class ThematicIndex
{
public:
  /** One value and its rows. */
  struct Entry {
    /** The value: text, or a number as text. */
    QString value;
    /** The row ids having it, as stored. */
    std::vector<int32_t> rows;
  };

  /** Reads the index at @p path; nothing and @p errorString on failure. */
  static std::optional<ThematicIndex> read(const QString& path,
                                           QString* errorString);

  /** Parses an index from its bytes; @p path is only for messages. */
  static std::optional<ThematicIndex>
  parse(const QByteArray& bytes, const QString& path, QString* errorString);

  /** Returns the table the index is of. */
  const QString& table() const { return m_table; }
  /** Returns the column the index is of. */
  const QString& column() const { return m_column; }
  /** Returns the number of rows indexed, as the header says. */
  int32_t featureCount() const { return m_featureCount; }
  /** Returns the values and their rows. */
  const std::vector<Entry>& entries() const { return m_entries; }

private:
  QString m_table;
  QString m_column;
  int32_t m_featureCount = 0;
  std::vector<Entry> m_entries;
};

/**
 * Reads a whole file GDAL can open (plain, /vsizip/…). Returns nothing and
 * sets @p errorString if it can't.
 */
std::optional<QByteArray> readFile(const QString& path, QString* errorString);

/**
 * Returns the entry of directory @p directory whose name is @p name,
 * ignoring case (VMAP0's CD-ROM names are upper case, the standard's lower
 * case), as a path below @p directory; empty if there is none. Listings
 * are cached per directory.
 */
QString findEntry(const QString& directory, const QString& name);

} // namespace ts::packager::vpf

#endif // TS_PACKAGER_VPF_VPFTABLE_H
