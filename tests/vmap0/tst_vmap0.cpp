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

// The packager's VPF reader and VMAP0 conversion (Phase 21): a synthetic
// VPF database written here, and one real VMAP0 tile clipped by
// tools/fixtures/make_vmap0_fixture.py (tests/data/vmap0).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <vector>

#include <QByteArray>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <QTemporaryDir>
#include <QtTest>

#define GEOS_USE_ONLY_R_API
#include <geos_c.h>
#include <sqlite3.h>

#include "concurrency.h"
#include "mockhttpserver.h"
#include "mockpacks.h"
#include "packbuilder.h"
#include "ts/deps/dependencies.h"
#include "ts/deps/geoscontext.h"
#include "ts/network/networkclient.h"
#include "ts/packs/packinstall.h"
#include "ts/packs/packlibrary.h"
#include "ts/packs/packmanifest.h"
#include "ts/providers/compositefeatureprovider.h"
#include "ts/providers/geopackagefeatureprovider.h"
#include "ts/providers/providerregistry.h"
#include "ts/tiling/tilekey.h"
#include "ts/vector/stylerules.h"
#include "vmap0builder.h"
#include "vpf/vpfcoverage.h"
#include "vpf/vpftable.h"

namespace vpf = ts::packager::vpf;

namespace
{

const std::atomic<bool> notCancelled{false};

/** Writes VPF tables: a header, then rows value by value. */
class VpfWriter
{
public:
  /** Starts a table with @p columns ("id=I,1,P,Row Id,-,-,-,:…"). */
  VpfWriter(const QString& description, const QString& columns,
            bool bigEndian = false)
      : m_big(bigEndian),
        m_header((QString(QLatin1Char(bigEndian ? 'M' : 'L')) +
                  QLatin1Char(';') + description + QStringLiteral(";-;") +
                  columns + QStringLiteral(";"))
                     .toLatin1())
  {
  }

  void i16(int32_t value) { number(static_cast<uint64_t>(value), 2); }
  void i32(int64_t value) { number(static_cast<uint64_t>(value), 4); }
  void f32(double value)
  {
    const float single = static_cast<float>(value);
    uint32_t bits = 0;
    std::memcpy(&bits, &single, sizeof bits);
    number(bits, 4);
  }
  void f64(double value)
  {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    number(bits, 8);
  }
  /** Fixed-width text, padded with blanks. */
  void text(const QString& value, int32_t width)
  {
    QByteArray bytes = value.toLatin1().left(width);
    bytes.append(QByteArray(width - static_cast<int32_t>(bytes.size()), ' '));
    m_rows.append(bytes);
  }
  /** Variable-length text. */
  void text(const QString& value)
  {
    const QByteArray bytes = value.toLatin1();
    i32(bytes.size());
    m_rows.append(bytes);
  }
  /** A triplet id with one-, two- or four-byte parts (-1: none). */
  void triplet(int32_t id, int32_t tile = -1, int32_t external = -1)
  {
    uint32_t type = 0;
    QByteArray parts;
    const std::array<int32_t, 3> values = {id, tile, external};
    for (std::size_t i = 0; i < values.size(); ++i) {
      if (values.at(i) < 0) {
        continue;
      }
      const uint32_t shift = 6U - 2U * static_cast<uint32_t>(i);
      uint32_t bytes = 4U;
      if (values.at(i) < 256) {
        bytes = 1U;
      } else if (values.at(i) < 65536) {
        bytes = 2U;
      }
      type |= (bytes == 4 ? 3U : bytes) << shift;
      QByteArray part;
      for (uint32_t b = 0; b < bytes; ++b) {
        const uint32_t byte = m_big ? bytes - 1 - b : b;
        part.append(static_cast<char>(
            (static_cast<uint32_t>(values.at(i)) >> (8U * byte)) & 0xffU));
      }
      parts.append(part);
    }
    m_rows.append(static_cast<char>(type));
    m_rows.append(parts);
  }
  /** Coordinates: 'C' (2D float), 'B' (2D double), 'Z', 'Y' (3D). */
  void coordinates(const std::vector<std::array<double, 3>>& points, char type,
                   bool variable = true)
  {
    if (variable) {
      i32(static_cast<int64_t>(points.size()));
    }
    for (const std::array<double, 3>& point : points) {
      const bool isDouble = type == 'B' || type == 'Y';
      const int32_t dimensions = (type == 'Z' || type == 'Y') ? 3 : 2;
      for (int32_t d = 0; d < dimensions; ++d) {
        if (isDouble) {
          f64(point.at(static_cast<std::size_t>(d)));
        } else {
          f32(point.at(static_cast<std::size_t>(d)));
        }
      }
    }
  }

  QByteArray bytes() const
  {
    QByteArray all;
    VpfWriter length(*this);
    length.m_rows.clear();
    length.i32(m_header.size());
    all.append(length.m_rows);
    all.append(m_header);
    all.append(m_rows);
    return all;
  }

  bool write(const QString& path) const
  {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) &&
           file.write(bytes()) == bytes().size();
  }

private:
  // The value, then its width.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  void number(uint64_t value, int32_t bytes)
  {
    for (int32_t i = 0; i < bytes; ++i) {
      const int32_t byte = m_big ? bytes - 1 - i : i;
      m_rows.append(static_cast<char>(
          (value >> (8U * static_cast<uint32_t>(byte))) & 0xffU));
    }
  }

  bool m_big = false;
  QByteArray m_header;
  QByteArray m_rows;
};

using Points = std::vector<std::array<double, 3>>;

/** Appends @p value as four little-endian bytes. */
void appendLe32(QByteArray* bytes, int32_t value)
{
  for (uint32_t i = 0; i < 4; ++i) {
    bytes->append(
        static_cast<char>((static_cast<uint32_t>(value) >> (8U * i)) & 0xffU));
  }
}

/** One edge of a synthetic tile. */
struct Edge {
  int32_t start = 0;
  int32_t end = 0;
  int32_t rightFace = 1;
  int32_t leftFace = 1;
  int32_t rightEdge = 0;
  int32_t leftEdge = 0;
  Points points;
};

void writeEdges(const QString& path, const std::vector<Edge>& edges,
                bool bigEndian)
{
  VpfWriter edg(QStringLiteral("Edge Primitive Table"),
                QStringLiteral("id=I,1,P,Row Identifier,-,-,-,:"
                               "start_node=I,1,N,Start Node,-,-,-,:"
                               "end_node=I,1,N,End Node,-,-,-,:"
                               "right_face=K,1,N,Right Face,-,-,-,:"
                               "left_face=K,1,N,Left Face,-,-,-,:"
                               "right_edge=K,1,N,Right Edge,-,-,-,:"
                               "left_edge=K,1,N,Left Edge,-,-,-,:"
                               "coordinates=C,*,N,Coordinates,-,-,-,:"),
                bigEndian);
  for (std::size_t i = 0; i < edges.size(); ++i) {
    const Edge& edge = edges[i];
    edg.i32(static_cast<int64_t>(i) + 1);
    edg.i32(edge.start);
    edg.i32(edge.end);
    edg.triplet(edge.rightFace);
    edg.triplet(edge.leftFace);
    edg.triplet(edge.rightEdge);
    edg.triplet(edge.leftEdge);
    edg.coordinates(edge.points, 'C');
  }
  QVERIFY(edg.write(path));
}

// Faces, then rings, as VPF lists them.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void writeFaces(const QString& directory,
                const std::vector<std::pair<int32_t, int32_t>>& faceRings,
                const std::vector<std::pair<int32_t, int32_t>>& rings)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  VpfWriter fac(QStringLiteral("Face Primitive Table"),
                QStringLiteral("id=I,1,P,Row Identifier,-,-,-,:"
                               "ring_ptr=I,1,N,Ring,-,-,-,:"));
  for (const std::pair<int32_t, int32_t>& face : faceRings) {
    fac.i32(face.first);
    fac.i32(face.second);
  }
  QVERIFY(fac.write(directory + QStringLiteral("/FAC")));
  VpfWriter rng(QStringLiteral("Ring Table"),
                QStringLiteral("id=I,1,P,Row Identifier,-,-,-,:"
                               "face_id=I,1,N,Face,-,-,-,:"
                               "start_edge=I,1,N,Start Edge,-,-,-,:"));
  for (std::size_t i = 0; i < rings.size(); ++i) {
    rng.i32(static_cast<int64_t>(i) + 1);
    rng.i32(rings[i].first);
    rng.i32(rings[i].second);
  }
  QVERIFY(rng.write(directory + QStringLiteral("/RNG")));
}

void writeNodes(const QString& path, const Points& points)
{
  VpfWriter end(QStringLiteral("Entity Node Primitive Table"),
                QStringLiteral("id=I,1,P,Row Identifier,-,-,-,:"
                               "containing_face=I,1,N,Face,-,-,-,:"
                               "coordinate=C,1,N,Coordinate,-,-,-,:"));
  for (std::size_t i = 0; i < points.size(); ++i) {
    end.i32(static_cast<int64_t>(i) + 1);
    end.i32(1);
    end.coordinates({points[i]}, 'C', false);
  }
  QVERIFY(end.write(path));
}

/**
 * Writes a VPF database with one tiled library "testlib" of two 15° tiles
 * (A/A: 0–15° E, B/A: 15–30° E, 0–15° N) and a coverage "test":
 * - lakes.aft: "Test Lake" in both tiles, cut at 15° E; in A/A it has a
 *   hole (a loop edge around face 3);
 * - roads.lft: one road cut at 15° E (use 23), and a short road of use 26;
 * - places.pft: "Alpha" in A/A and an unnamed place ("UNK") in B/A.
 * Tile B/A's edge table is big-endian.
 */
void writeSyntheticDatabase(const QString& root)
{
  VpfWriter dht(QStringLiteral("Database Header Table"),
                QStringLiteral("id=I,1,P,Row Identifier,-,-,-,:"
                               "database_name=T,8,N,Name,-,-,-,:"
                               "database_desc=T,*,N,Description,-,-,-,:"));
  dht.i32(1);
  dht.text(QStringLiteral("testdb"), 8);
  dht.text(QStringLiteral("A synthetic database"));
  QVERIFY(dht.write(root + QStringLiteral("/DHT")));
  VpfWriter lat(QStringLiteral("Library Attribute Table"),
                QStringLiteral("id=I,1,P,Row Identifier,-,-,-,:"
                               "library_name=T,8,N,Name,-,-,-,:"
                               "xmin=F,1,N,W,-,-,-,:ymin=F,1,N,S,-,-,-,:"
                               "xmax=F,1,N,E,-,-,-,:ymax=F,1,N,N,-,-,-,:"));
  lat.i32(1);
  lat.text(QStringLiteral("testlib"), 8);
  for (const double value : {0.0, 0.0, 30.0, 15.0}) {
    lat.f32(value);
  }
  QVERIFY(lat.write(root + QStringLiteral("/LAT")));
  const QString library = root + QStringLiteral("/TESTLIB");
  VpfWriter cat(QStringLiteral("Coverage Attribute Table"),
                QStringLiteral("id=I,1,P,Row Identifier,-,-,-,:"
                               "coverage_name=T,8,N,Name,-,-,-,:"
                               "description=T,24,N,Description,-,-,-,:"
                               "level=S,1,N,Topology level,-,-,-,:"));
  cat.i32(1);
  cat.text(QStringLiteral("tileref"), 8);
  cat.text(QStringLiteral("Tile Reference"), 24);
  cat.i16(3);
  cat.i32(2);
  cat.text(QStringLiteral("test"), 8);
  cat.text(QStringLiteral("Test"), 24);
  cat.i16(3);
  QVERIFY(cat.write(library + QStringLiteral("/CAT")));
  VpfWriter tiles(QStringLiteral("Tile Reference Area Feature Table"),
                  QStringLiteral("id=I,1,P,Row Identifier,-,-,-,:"
                                 "tile_name=T,9,N,Tile,-,-,-,:"
                                 "fac_id=I,1,N,Face,-,-,-,:"));
  tiles.i32(1);
  tiles.text(QStringLiteral("A\\A"), 9);
  tiles.i32(2);
  tiles.i32(2);
  tiles.text(QStringLiteral("B\\A"), 9);
  tiles.i32(3);
  QVERIFY(tiles.write(library + QStringLiteral("/TILEREF/TILEREF.AFT")));
  VpfWriter fbr(QStringLiteral("Bounding Rectangle Table"),
                QStringLiteral("id=I,1,P,Row Identifier,-,-,-,:"
                               "xmin=F,1,N,X,-,-,-,:ymin=F,1,N,Y,-,-,-,:"
                               "xmax=F,1,N,X,-,-,-,:ymax=F,1,N,Y,-,-,-,:"));
  const std::array<std::array<double, 4>, 3> boxes = {
      {{0, 0, 30, 15}, {0, 0, 15, 15}, {15, 0, 30, 15}}};
  for (std::size_t i = 0; i < boxes.size(); ++i) {
    fbr.i32(static_cast<int64_t>(i) + 1);
    for (const double value : boxes.at(i)) {
      fbr.f32(value);
    }
  }
  QVERIFY(fbr.write(library + QStringLiteral("/TILEREF/FBR")));

  const QString coverage = library + QStringLiteral("/TEST");
  VpfWriter fcs(QStringLiteral("Feature Class Schema Table"),
                QStringLiteral("id=I,1,P,Row Identifier,-,-,-,:"
                               "feature_class=T,8,N,Class,-,-,-,:"
                               "table1=T,12,N,First,-,-,-,:"
                               "table1_key=T,16,N,Key,-,-,-,:"
                               "table2=T,12,N,Second,-,-,-,:"
                               "table2_key=T,16,N,Key,-,-,-,:"));
  const std::array<std::array<const char*, 5>, 6> schema = {{
      {"lakes", "lakes.aft", "fac_id", "fac", "id"},
      {"lakes", "fac", "lakes.aft_id", "lakes.aft", "id"},
      {"roads", "roads.lft", "edg_id", "edg", "id"},
      {"roads", "edg", "roads.lft_id", "roads.lft", "id"},
      {"places", "places.pft", "end_id", "end", "id"},
      {"places", "end", "places.pft_id", "places.pft", "id"},
  }};
  for (std::size_t i = 0; i < schema.size(); ++i) {
    fcs.i32(static_cast<int64_t>(i) + 1);
    const std::array<int32_t, 5> widths = {8, 12, 16, 12, 16};
    for (std::size_t c = 0; c < 5; ++c) {
      fcs.text(QString::fromLatin1(schema.at(i).at(c)), widths.at(c));
    }
  }
  QVERIFY(fcs.write(coverage + QStringLiteral("/FCS")));
  VpfWriter chars(QStringLiteral("Character Value Description Table"),
                  QStringLiteral("id=I,1,P,Row Identifier,-,-,-,:"
                                 "table=T,12,N,Table,-,-,-,:"
                                 "attribute=T,6,N,Column,-,-,-,:"
                                 "value=T,5,N,Value,-,-,-,:"
                                 "description=T,50,N,Description,-,-,-,:"));
  const std::array<std::array<const char*, 4>, 5> charRows = {{
      {"lakes.aft", "f_code", "BH000", "Inland Water"},
      {"lakes.aft", "nam", "UNK", "No entry present"},
      {"roads.lft", "f_code", "AP030", "Road"},
      {"places.pft", "f_code", "AL105", "Settlement"},
      {"places.pft", "nam", "UNK", "No entry present"},
  }};
  for (std::size_t i = 0; i < charRows.size(); ++i) {
    chars.i32(static_cast<int64_t>(i) + 1);
    const std::array<int32_t, 4> widths = {12, 6, 5, 50};
    for (std::size_t c = 0; c < 4; ++c) {
      chars.text(QString::fromLatin1(charRows.at(i).at(c)), widths.at(c));
    }
  }
  QVERIFY(chars.write(coverage + QStringLiteral("/CHAR.VDT")));
  VpfWriter ints(QStringLiteral("Integer Value Description Table"),
                 QStringLiteral("id=I,1,P,Row Identifier,-,-,-,:"
                                "table=T,12,N,Table,-,-,-,:"
                                "attribute=T,3,N,Column,-,-,-,:"
                                "value=S,1,N,Value,-,-,-,:"
                                "description=T,50,N,Description,-,-,-,:"));
  const std::array<std::pair<int32_t, const char*>, 3> intRows = {
      {{8, "Perennial/Permanent"},
       {23, "International"},
       {26, "Primary/1st Order"}}};
  for (std::size_t i = 0; i < intRows.size(); ++i) {
    ints.i32(static_cast<int64_t>(i) + 1);
    ints.text(
        i == 0 ? QStringLiteral("lakes.aft") : QStringLiteral("roads.lft"), 12);
    ints.text(i == 0 ? QStringLiteral("hyc") : QStringLiteral("use"), 3);
    ints.i16(intRows.at(i).first);
    ints.text(QString::fromLatin1(intRows.at(i).second), 50);
  }
  QVERIFY(ints.write(coverage + QStringLiteral("/INT.VDT")));

  VpfWriter lakes(QStringLiteral("Lake Area Feature Table"),
                  QStringLiteral("id=I,1,P,Row Identifier,-,-,-,:"
                                 "f_code=T,5,N,FACC Code,char.vdt,-,-,:"
                                 "nam=T,*,N,Name,char.vdt,-,-,:"
                                 "hyc=S,1,N,Hydrologic Category,int.vdt,-,-,:"
                                 "tile_id=S,1,N,Tile,-,-,-,:"
                                 "fac_id=I,1,N,Face,-,-,-,:"));
  for (int32_t tile = 1; tile <= 2; ++tile) {
    lakes.i32(tile);
    lakes.text(QStringLiteral("BH000"), 5);
    lakes.text(QStringLiteral("Test Lake"));
    lakes.i16(8);
    lakes.i16(tile);
    lakes.i32(2);
  }
  QVERIFY(lakes.write(coverage + QStringLiteral("/LAKES.AFT")));
  VpfWriter roads(QStringLiteral("Road Line Feature Table"),
                  QStringLiteral("id=I,1,P,Row Identifier,-,-,-,:"
                                 "f_code=T,5,N,FACC Code,char.vdt,-,-,:"
                                 "use=S,1,N,Usage,int.vdt,-,-,:"
                                 "tile_id=S,1,N,Tile,-,-,-,:"
                                 "edg_id=I,1,N,Edge,-,-,-,:"));
  const std::array<std::array<int32_t, 3>, 3> roadRows = {
      {{23, 1, 4}, {23, 2, 3}, {26, 1, 5}}};
  for (std::size_t i = 0; i < roadRows.size(); ++i) {
    roads.i32(static_cast<int64_t>(i) + 1);
    roads.text(QStringLiteral("AP030"), 5);
    roads.i16(roadRows.at(i).at(0));
    roads.i16(roadRows.at(i).at(1));
    roads.i32(roadRows.at(i).at(2));
  }
  QVERIFY(roads.write(coverage + QStringLiteral("/ROADS.LFT")));
  VpfWriter places(QStringLiteral("Place Point Feature Table"),
                   QStringLiteral("id=I,1,P,Row Identifier,-,-,-,:"
                                  "f_code=T,5,N,FACC Code,char.vdt,-,-,:"
                                  "nam=T,*,N,Name,char.vdt,-,-,:"
                                  "tile_id=S,1,N,Tile,-,-,-,:"
                                  "end_id=I,1,N,Node,-,-,-,:"));
  places.i32(1);
  places.text(QStringLiteral("AL105"), 5);
  places.text(QStringLiteral("Alpha"));
  places.i16(1);
  places.i32(1);
  places.i32(2);
  places.text(QStringLiteral("AL105"), 5);
  places.text(QStringLiteral("UNK"));
  places.i16(2);
  places.i32(1);
  QVERIFY(places.write(coverage + QStringLiteral("/PLACES.PFT")));

  // Tile A/A: the lake's west part (face 2) with a hole (face 3, a loop
  // edge), a road ending at 15° E and the short road of use 26. Faces are
  // on the right of edges walked forward, so outer rings run clockwise.
  const QString tileA = coverage + QStringLiteral("/A/A");
  writeEdges(tileA + QStringLiteral("/EDG"),
             {
                 {1, 2, 2, 1, 2, 2, {{1, 1, 0}, {1, 14, 0}, {15, 14, 0}}},
                 {2, 1, 2, 1, 1, 1, {{15, 14, 0}, {15, 1, 0}, {1, 1, 0}}},
                 {3,
                  3,
                  2,
                  3,
                  3,
                  3,
                  {{5, 5, 0}, {9, 5, 0}, {9, 9, 0}, {5, 9, 0}, {5, 5, 0}}},
                 {4, 5, 1, 1, 4, 4, {{2, 0.5, 0}, {15, 0.5, 0}}},
                 {6, 7, 1, 1, 5, 5, {{1, 0.2, 0}, {3, 0.2, 0}}},
             },
             false);
  writeFaces(tileA, {{1, 4}, {2, 1}, {3, 3}}, {{2, 1}, {2, 3}, {3, 3}, {1, 1}});
  writeNodes(tileA + QStringLiteral("/END"), {{5, 12, 0}});
  // Tile B/A: the lake's east part and the road's east part; big-endian
  // edges.
  const QString tileB = coverage + QStringLiteral("/B/A");
  writeEdges(tileB + QStringLiteral("/EDG"),
             {
                 {1, 2, 2, 1, 2, 2, {{15, 1, 0}, {15, 14, 0}, {20, 14, 0}}},
                 {2, 1, 2, 1, 1, 1, {{20, 14, 0}, {20, 1, 0}, {15, 1, 0}}},
                 {3, 4, 1, 1, 3, 3, {{15, 0.5, 0}, {25, 0.5, 0}}},
             },
             true);
  writeFaces(tileB, {{1, 2}, {2, 1}}, {{2, 1}, {1, 1}});
  writeNodes(tileB + QStringLiteral("/END"), {{25, 10, 0}});
}

/** The rows of @p table in @p gpkg: geometry blob and named values. */
struct Row {
  QByteArray geometry;
  std::map<QString, QVariant> values;
};

// The file, then the table in it.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::vector<Row> readRows(const QString& gpkg, const QString& table)
{
  std::vector<Row> rows;
  sqlite3* db = nullptr;
  const QByteArray path = gpkg.toUtf8();
  if (sqlite3_open_v2(path.constData(), &db, SQLITE_OPEN_READONLY, nullptr) !=
      SQLITE_OK) {
    sqlite3_close(db);
    return rows;
  }
  sqlite3_stmt* statement = nullptr;
  const QByteArray sql =
      QStringLiteral("SELECT * FROM \"%1\"").arg(table).toUtf8();
  if (sqlite3_prepare_v2(db, sql.constData(), -1, &statement, nullptr) ==
      SQLITE_OK) {
    while (sqlite3_step(statement) == SQLITE_ROW) {
      Row row;
      for (int32_t c = 0; c < sqlite3_column_count(statement); ++c) {
        const QString name =
            QString::fromUtf8(sqlite3_column_name(statement, c));
        switch (sqlite3_column_type(statement, c)) {
        case SQLITE_BLOB:
          row.geometry = QByteArray(
              static_cast<const char*>(sqlite3_column_blob(statement, c)),
              sqlite3_column_bytes(statement, c));
          break;
        case SQLITE_INTEGER:
          row.values[name] =
              static_cast<qlonglong>(sqlite3_column_int64(statement, c));
          break;
        case SQLITE_TEXT:
          row.values[name] = QString::fromUtf8(
              reinterpret_cast<const char*>(sqlite3_column_text(statement, c)));
          break;
        default:
          break;
        }
      }
      rows.push_back(row);
    }
  }
  sqlite3_finalize(statement);
  sqlite3_close(db);
  return rows;
}

/** GEOS geometry of a GeoPackage blob (header skipped). */
GEOSGeometry* fromBlob(GEOSContextHandle_t context, const QByteArray& blob)
{
  const uint8_t flags = static_cast<uint8_t>(blob.at(3));
  static const std::array<int32_t, 5> envelopes = {0, 32, 48, 48, 64};
  const int32_t header =
      8 + envelopes.at(static_cast<std::size_t>((flags >> 1U) & 7U));
  GEOSWKBReader* reader = GEOSWKBReader_create_r(context);
  GEOSGeometry* geometry = GEOSWKBReader_read_r(
      context, reader,
      reinterpret_cast<const unsigned char*>(blob.constData()) + header,
      static_cast<std::size_t>(blob.size() - header));
  GEOSWKBReader_destroy_r(context, reader);
  return geometry;
}

/** Owns a GEOS geometry. */
struct Geometry {
  GEOSContextHandle_t context = nullptr;
  GEOSGeometry* geometry = nullptr;
  Geometry(GEOSContextHandle_t c, GEOSGeometry* g) : context(c), geometry(g) {}
  ~Geometry()
  {
    if (geometry != nullptr) {
      GEOSGeom_destroy_r(context, geometry);
    }
  }
  Geometry(const Geometry&) = delete;
  Geometry& operator=(const Geometry&) = delete;
  Geometry(Geometry&&) = delete;
  Geometry& operator=(Geometry&&) = delete;
};

/** A VMAP0 class entry for tests. */
ts::packager::Vmap0Class vmap0Class(
    const QString& coverage, const QString& featureClass, const QString& table,
    std::vector<ts::packager::Vmap0Attribute> attributes = {},
    std::vector<ts::packager::Vmap0Filter> filters = {}, int32_t minLevel = 0)
{
  ts::packager::Vmap0Class cls;
  cls.coverage = coverage;
  cls.featureClass = featureClass;
  cls.table = table;
  cls.attributes = std::move(attributes);
  cls.filters = std::move(filters);
  cls.minLevel = minLevel;
  return cls;
}

ts::packager::Vmap0Attribute
attribute(const QString& column, const QString& name,
          ts::packager::Vmap0Attribute::Kind kind =
              ts::packager::Vmap0Attribute::Kind::Decoded)
{
  return ts::packager::Vmap0Attribute{column, name, kind};
}

/** Opens library @p name of the database at @p path. */
// The database, then the library in it.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
std::optional<vpf::Library> openLibrary(const QString& path,
                                        const QString& name, QString* error)
{
  const std::optional<vpf::Database> database =
      vpf::Database::open(path, error);
  if (!database) {
    return std::nullopt;
  }
  return vpf::Library::open(*database, name, error);
}

QString fixtureDatabase()
{
  return QStringLiteral(TS_TEST_DATA_DIR "/vmap0/VMAPLV0");
}

/**
 * Builds the repository's transportation pack from the real tile into
 * @p output, as pack @p id version @p version (same dataset whatever the
 * id: regional packs of one theme).
 */
bool buildFixturePack(const QString& output, const QString& id,
                      const QString& version, QString* error,
                      ts::packager::BuildReport* report = nullptr)
{
  std::optional<ts::packager::PackConfig> config =
      ts::packager::PackConfig::read(
          QStringLiteral(TS_ROOT_DIR
                         "/tools/packager/packs/vmap0-transportation.json"),
          error);
  if (!config || !config->vmap0) {
    return false;
  }
  config->id = id;
  config->version = version;
  config->sources.resize(1);
  config->sources.front().file = QStringLiteral("vmap0");
  config->sources.front().notices.clear();
  config->sources.front().localNotices.clear();
  config->vmap0->sources = {config->sources.front().id};
  ts::packager::BuildOptions options;
  options.sourcesDirectory = QStringLiteral(TS_TEST_DATA_DIR);
  options.configDirectory = QStringLiteral(TS_ROOT_DIR "/tools/packager/packs");
  options.outputDirectory = output;
  options.verifySources = false;
  ts::packager::BuildReport own;
  return ts::packager::buildPack(*config, options,
                                 report != nullptr ? report : &own, error);
}

/** The pack built in @p directory as @p server serves it. */
ts::test::RemotePack servedPack(const ts::test::MockHttpServer& server,
                                const QString& directory)
{
  ts::test::RemotePack pack;
  QString error;
  const std::optional<ts::PackManifest> manifest =
      ts::PackManifest::read(directory, &error);
  if (!manifest) {
    return pack;
  }
  const QString base =
      QStringLiteral("/%1/%2/").arg(manifest->id, manifest->version);
  for (const ts::PackFile& file : manifest->files) {
    QFile in(QDir(directory).filePath(file.path));
    if (in.open(QIODevice::ReadOnly)) {
      pack.files[base + file.path] = in.readAll();
    }
    pack.entry.files[file.path] = server.url(base + file.path);
    pack.entry.sizeBytes += file.sizeBytes;
  }
  QFile in(QDir(directory).filePath(ts::PackManifest::fileName()));
  in.open(QIODevice::ReadOnly);
  const QByteArray json = in.readAll();
  pack.files[base + ts::PackManifest::fileName()] = json;
  pack.entry.id = manifest->id;
  pack.entry.version = manifest->version;
  pack.entry.name = manifest->name;
  pack.entry.manifestUrl = server.url(base + ts::PackManifest::fileName());
  pack.entry.manifestBytes = json.size();
  pack.entry.manifestSha256 = QString::fromLatin1(
      QCryptographicHash::hash(json, QCryptographicHash::Sha256).toHex());
  return pack;
}

/** Downloads @p entry into @p library, waits and activates it. */
bool installPack(ts::PackLibrary& library, const ts::PackIndexEntry& entry,
                 QString* error)
{
  ts::NetworkSettings settings;
  settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
  settings.attempts = 1;
  settings.timeout = std::chrono::milliseconds(5000);
  const std::shared_ptr<ts::NetworkClient> network =
      std::make_shared<ts::NetworkClient>(settings);
  const QString staging = library.prepareStaging(entry, error);
  if (staging.isEmpty()) {
    return false;
  }
  ts::PackInstallSettings installSettings;
  installSettings.chunkBytes = 1 << 20;
  installSettings.allowLocalHttp = true;
  ts::PackInstall install(network, entry, staging, installSettings);
  install.start();
  install.waitUntilSettled(std::chrono::seconds(60));
  if (install.progress().state != ts::PackInstallProgress::State::Verified) {
    *error = install.progress().errorString;
    return false;
  }
  return library.activate(entry.id, error);
}

/** A layer source on the transportation pack's tables @p tables. */
QJsonObject packSource(const QStringList& tables)
{
  return QJsonObject{
      {QStringLiteral("kind"), QStringLiteral("basepack")},
      {QStringLiteral("dataset"), QStringLiteral("vmap0-transportation")},
      {QStringLiteral("tables"), QJsonArray::fromStringList(tables)}};
}

/** A GEOS line string (or ring) through @p part. */
GEOSGeometry* lineThrough(GEOSContextHandle_t context,
                          const std::vector<vpf::Coordinate>& part, bool ring)
{
  GEOSCoordSequence* sequence =
      GEOSCoordSeq_create_r(context, static_cast<uint32_t>(part.size()), 2);
  for (std::size_t i = 0; i < part.size(); ++i) {
    GEOSCoordSeq_setXY_r(context, sequence, static_cast<uint32_t>(i), part[i].x,
                         part[i].y);
  }
  return ring ? GEOSGeom_createLinearRing_r(context, sequence)
              : GEOSGeom_createLineString_r(context, sequence);
}

/** The areas @p features as a collection of valid polygons. */
GEOSGeometry* polygonsOf(GEOSContextHandle_t context,
                         const std::vector<vpf::Feature>& features)
{
  std::vector<GEOSGeometry*> polygons;
  for (const vpf::Feature& feature : features) {
    std::vector<GEOSGeometry*> rings;
    rings.reserve(feature.parts.size());
    for (const std::vector<vpf::Coordinate>& part : feature.parts) {
      rings.push_back(lineThrough(context, part, true));
    }
    GEOSGeometry* polygon =
        GEOSGeom_createPolygon_r(context, rings.front(), rings.data() + 1,
                                 static_cast<uint32_t>(rings.size() - 1));
    polygons.push_back(GEOSMakeValid_r(context, polygon));
    GEOSGeom_destroy_r(context, polygon);
  }
  return GEOSGeom_createCollection_r(context, GEOS_GEOMETRYCOLLECTION,
                                     polygons.data(),
                                     static_cast<uint32_t>(polygons.size()));
}

/** Every ring of @p features as a collection of line strings. */
GEOSGeometry* ringsOf(GEOSContextHandle_t context,
                      const std::vector<vpf::Feature>& features)
{
  std::vector<GEOSGeometry*> rings;
  for (const vpf::Feature& feature : features) {
    for (const std::vector<vpf::Coordinate>& part : feature.parts) {
      rings.push_back(lineThrough(context, part, false));
    }
  }
  return GEOSGeom_createCollection_r(context, GEOS_GEOMETRYCOLLECTION,
                                     rings.data(),
                                     static_cast<uint32_t>(rings.size()));
}

/**
 * The farthest any vertex of the pieces in @p table of @p gpkg lies from
 * @p boundary, leaving out the vertices the tile cuts made (on multiples
 * of @p tileStepDeg); infinity if a piece is invalid.
 */
double farthestFrom(GEOSContextHandle_t context, const QString& gpkg,
                    const QString& table, const GEOSGeometry* boundary,
                    double tileStepDeg)
{
  double worst = 0.0;
  for (const Row& row : readRows(gpkg, table)) {
    const Geometry piece(context, fromBlob(context, row.geometry));
    if (GEOSisValid_r(context, piece.geometry) != 1) {
      return std::numeric_limits<double>::infinity();
    }
    const Geometry rings(context, GEOSBoundary_r(context, piece.geometry));
    const Geometry points(
        context, GEOSGeom_extractUniquePoints_r(context, rings.geometry));
    for (int32_t i = 0; i < GEOSGetNumGeometries_r(context, points.geometry);
         ++i) {
      const GEOSGeometry* point =
          GEOSGetGeometryN_r(context, points.geometry, i);
      double x = 0.0;
      double y = 0.0;
      GEOSGeomGetX_r(context, point, &x);
      GEOSGeomGetY_r(context, point, &y);
      if (std::abs(std::remainder(x, tileStepDeg)) < 1e-9 ||
          std::abs(std::remainder(y, tileStepDeg)) < 1e-9) {
        continue;
      }
      double distance = 0.0;
      GEOSDistance_r(context, point, boundary, &distance);
      worst = std::max(worst, distance);
    }
  }
  return worst;
}

/** The polygon of @p feature, its longitudes times @p scale. */
GEOSGeometry* scaledPolygon(GEOSContextHandle_t context,
                            const ts::Feature& feature, double scale)
{
  std::vector<GEOSGeometry*> rings;
  rings.reserve(feature.parts.size());
  for (const std::vector<ts::GeoPoint>& part : feature.parts) {
    GEOSCoordSequence* sequence = GEOSCoordSeq_create_r(
        context, static_cast<uint32_t>(part.size() + 1), 2);
    for (std::size_t i = 0; i <= part.size(); ++i) {
      const ts::GeoPoint& point = part[i % part.size()];
      GEOSCoordSeq_setXY_r(context, sequence, static_cast<uint32_t>(i),
                           point.lonDeg * scale, point.latDeg);
    }
    rings.push_back(GEOSGeom_createLinearRing_r(context, sequence));
  }
  return GEOSGeom_createPolygon_r(context, rings.front(), rings.data() + 1,
                                  static_cast<uint32_t>(rings.size() - 1));
}

/**
 * The land polygons of @p shoreline (the base pack's) at level 6 between
 * the given whole degrees, longitudes times @p scale.
 */
// West, east, south, north, as everywhere.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::vector<GEOSGeometry*> landAround(GEOSContextHandle_t context,
                                      ts::GeoPackageFeatureProvider& shoreline,
                                      int32_t west, int32_t east, int32_t south,
                                      int32_t north, double scale)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  std::set<uint64_t> keys;
  std::vector<GEOSGeometry*> land;
  for (int32_t lat = south; lat <= north; ++lat) {
    for (int32_t lon = west; lon <= east; ++lon) {
      const ts::TileKey key = shoreline.storedKey(ts::TileKey::containing(
          6, ts::Geodetic{static_cast<double>(lat), static_cast<double>(lon),
                          0.0}));
      if (!keys.insert(key.packed()).second) {
        continue;
      }
      const std::optional<ts::FeatureTile> tile =
          shoreline.loadFeatures(key, notCancelled);
      if (!tile) {
        continue;
      }
      for (const ts::Feature& feature : tile.value().features) {
        land.push_back(scaledPolygon(context, feature, scale));
      }
    }
  }
  return land;
}

/** The attributes the classes of @p spec write, and "table". */
std::set<QString> attributeNames(const ts::packager::Vmap0Spec& spec)
{
  std::set<QString> names = {QStringLiteral("table")};
  for (const ts::packager::Vmap0Class& cls : spec.classes) {
    for (const ts::packager::Vmap0Attribute& attribute : cls.attributes) {
      names.insert(attribute.name);
    }
  }
  return names;
}

/**
 * What is wrong with pack layer @p layer: tables that @p tables lacks,
 * style rules that don't parse or look at attributes not in
 * @p attributes, labels of such an attribute; empty if nothing.
 */
QString layerProblem(const QJsonObject& layer, const std::set<QString>& tables,
                     const std::set<QString>& attributes)
{
  if (layer.value(QStringLiteral("id")).toString().isEmpty() ||
      layer.value(QStringLiteral("name")).toString().isEmpty()) {
    return QStringLiteral("a layer without id or name");
  }
  const QJsonArray layerTables =
      layer.value(QStringLiteral("tables")).toArray();
  if (layerTables.isEmpty()) {
    return QStringLiteral("a layer without tables");
  }
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t t = 0; t < layerTables.size(); ++t) {
    if (tables.count(layerTables.at(t).toString()) == 0) {
      return QStringLiteral("no table ") + layerTables.at(t).toString();
    }
  }
  const QJsonArray rules = layer.value(QStringLiteral("styleRules")).toArray();
  if (ts::styleRulesFromJson(rules).size() !=
      static_cast<std::size_t>(rules.size())) {
    return QStringLiteral("a style rule that doesn't parse");
  }
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t r = 0; r < rules.size(); ++r) {
    const QString name =
        rules.at(r).toObject().value(QStringLiteral("attribute")).toString();
    if (attributes.count(name) == 0) {
      return QStringLiteral("a rule on no attribute ") + name;
    }
  }
  const QString labels = layer.value(QStringLiteral("style"))
                             .toObject()
                             .value(QStringLiteral("labels"))
                             .toString();
  if (!labels.isEmpty() && attributes.count(labels) == 0) {
    return QStringLiteral("labels of no attribute ") + labels;
  }
  return QString();
}

/**
 * Returns true if @p spec keeps any of ESRI's features: first-order
 * administrative boundaries (polbndl without use 26 excluded) or
 * administrative areas (polbnda).
 */
bool keepsEsriFeatures(const ts::packager::Vmap0Spec& spec)
{
  for (const ts::packager::Vmap0Class& cls : spec.classes) {
    if (cls.featureClass == QLatin1String("polbnda")) {
      return true;
    }
    if (cls.featureClass != QLatin1String("polbndl")) {
      continue;
    }
    const bool excludesUse26 = std::any_of(
        cls.filters.begin(), cls.filters.end(),
        [](const ts::packager::Vmap0Filter& filter) {
          return filter.column == QLatin1String("use") && filter.exclude;
        });
    if (!excludesUse26) {
      return true;
    }
  }
  return false;
}

/** The VMAP0 pack configurations of the repository. */
QStringList packConfigs()
{
  QStringList configs;
  QDirIterator it(QStringLiteral(TS_ROOT_DIR "/tools/packager/packs"),
                  {QStringLiteral("vmap0-*.json")}, QDir::Files);
  while (it.hasNext()) {
    configs.append(it.next());
  }
  configs.sort();
  return configs;
}

} // namespace

/** Unit tests for the packager's VPF reader and VMAP0 conversion. */
class TestVmap0 : public QObject
{
  Q_OBJECT

private slots:
  void initTestCase() { ts::initialiseGdal(); }

  // --- Tables ------------------------------------------------------------

  void everyColumnType_data()
  {
    QTest::addColumn<bool>("bigEndian");
    QTest::newRow("little-endian") << false;
    QTest::newRow("big-endian") << true;
  }

  void everyColumnType()
  {
    QFETCH(bool, bigEndian);
    VpfWriter table(QStringLiteral("All Types"),
                    QStringLiteral("id=I,1,P,Row Identifier,-,-,-,:"
                                   "fixed=T,6,N,Fixed text,char.vdt,-,-,:"
                                   "variable=T,*,N,Variable text,-,-,-,:"
                                   "latin=L,3,N,Level 1 text,-,-,-,:"
                                   "short=S,1,N,Short,int.vdt,-,-,:"
                                   "single=F,1,N,Float,-,-,-,:"
                                   "double=R,1,N,Double,-,-,-,:"
                                   "date=D,1,N,Date,-,-,-,:"
                                   "key=K,1,N,Triplet,-,-,-,:"
                                   "c2=C,*,N,2D float,-,-,-,:"
                                   "b2=B,*,N,2D double,-,-,-,:"
                                   "z3=Z,1,N,3D float,-,-,-,:"
                                   "y3=Y,*,N,3D double,-,-,-,:"
                                   "nothing=X,1,N,Null,-,-,-,:"
                                   "pair=S,2,N,Two shorts,-,-,-,:"),
                    bigEndian);
    for (int32_t row = 1; row <= 2; ++row) {
      table.i32(row);
      table.text(QStringLiteral("ab"), 6);
      table.text(row == 1 ? QStringLiteral("Hello, world") : QString());
      table.text(QString::fromLatin1("\xe9t\xe9"), 3);
      table.i16(-12 * row);
      table.f32(1.5 * row);
      table.f64(-2.25e10 * row);
      table.text(QStringLiteral("20000930000000.00000"), 20);
      table.triplet(row == 1 ? 7 : 300, row == 1 ? 70000 : -1, 2);
      table.coordinates({{-1.5, 2.5, 0}, {3, 4, 0}}, 'C');
      table.coordinates({{-179.123456789, -89.987654321, 0}}, 'B');
      table.coordinates({{1, 2, 3}}, 'Z', false);
      table.coordinates({{4, 5, 6}, {7, 8, 9}, {10, 11, 12}}, 'Y');
      table.i16(1);
      table.i16(-2);
    }
    QString error;
    const std::optional<vpf::Table> read =
        vpf::Table::parse(table.bytes(), QStringLiteral("all"), &error);
    if (!read) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(read->isBigEndian(), bigEndian);
    QCOMPARE(read->description(), QStringLiteral("All Types"));
    QCOMPARE(read->rowCount(), 2);
    QCOMPARE(read->columns().size(), std::size_t{15});
    QCOMPARE(read->columns().at(1).valueTable, QStringLiteral("char.vdt"));
    QVERIFY(read->columns().at(2).isVariable());
    QCOMPARE(read->text(0, 1), QStringLiteral("ab"));
    QCOMPARE(read->text(0, 2), QStringLiteral("Hello, world"));
    QCOMPARE(read->text(1, 2), QString());
    QCOMPARE(read->text(0, 3), QString::fromLatin1("\xe9t\xe9"));
    QCOMPARE(read->integer(1, 4), int64_t{-24});
    QCOMPARE(read->row(1).at(5).real, 3.0);
    QCOMPARE(read->row(0).at(6).real, -2.25e10);
    QCOMPARE(read->text(0, 7), QStringLiteral("20000930000000.00000"));
    const vpf::TripletId first = read->triplet(0, 8);
    QCOMPARE(first.id, 7);
    QCOMPARE(first.tile, 70000);
    QCOMPARE(first.external, 2);
    const vpf::TripletId second = read->triplet(1, 8);
    QCOMPARE(second.id, 300);
    QCOMPARE(second.tile, -1);
    QCOMPARE(read->coordinates(0, 9).size(), std::size_t{2});
    QCOMPARE(read->coordinates(0, 9).at(1).y, 4.0);
    QCOMPARE(read->coordinates(0, 10).at(0).x, -179.123456789);
    QCOMPARE(read->coordinates(0, 11).at(0).z, 3.0);
    QCOMPARE(read->coordinates(1, 12).at(2).z, 12.0);
    QCOMPARE(read->row(0).at(13).kind, vpf::Value::Kind::Null);
    QCOMPARE(read->row(0).at(14).kind, vpf::Value::Kind::Numbers);
    QCOMPARE(read->row(0).at(14).numbers.at(1), -2.0);
    QCOMPARE(read->columnIndex(QStringLiteral("DOUBLE")), 6);
    QCOMPARE(read->rowOfId(2), 1);
  }

  void malformedTablesAreReported()
  {
    VpfWriter table(QStringLiteral("Short"),
                    QStringLiteral("id=I,1,P,Row Identifier,-,-,-,:"
                                   "name=T,*,N,Name,-,-,-,:"));
    table.i32(1);
    table.text(QStringLiteral("first"));
    table.i32(2);
    table.text(QStringLiteral("second"));
    QByteArray bytes = table.bytes();
    bytes.chop(3);
    QString error;
    QVERIFY(!vpf::Table::parse(bytes, QStringLiteral("cut.lft"), &error));
    QVERIFY2(error.contains(QStringLiteral("cut.lft")) &&
                 error.contains(QStringLiteral("row 2")),
             qPrintable(error));
    // A header longer than the file, an unknown type, no columns.
    QByteArray longHeader = table.bytes();
    longHeader[0] = static_cast<char>(0x7f);
    longHeader[1] = static_cast<char>(0x7f);
    QVERIFY(!vpf::Table::parse(longHeader, QStringLiteral("a"), &error));
    QVERIFY(error.contains(QStringLiteral("header length")));
    const VpfWriter badType(QStringLiteral("Bad"),
                            QStringLiteral("id=Q,1,P,Row,-,-,-,:"));
    QVERIFY(!vpf::Table::parse(badType.bytes(), QStringLiteral("b"), &error));
    QVERIFY(error.contains(QStringLiteral("column definition")));
    QVERIFY(
        !vpf::Table::parse(QByteArray("\x01"), QStringLiteral("c"), &error));
    QVERIFY(!vpf::Table::read(QStringLiteral("/no/such/table"), &error));
  }

  void variableLengthIndex()
  {
    QTemporaryDir dir;
    VpfWriter table(QStringLiteral("Names"),
                    QStringLiteral("id=I,1,P,Row Identifier,-,-,-,:"
                                   "name=T,*,N,Name,-,-,-,:"));
    std::vector<int32_t> offsets;
    std::vector<int32_t> sizes;
    const QStringList names = {QStringLiteral("a"), QStringLiteral("bcd"),
                               QString()};
    int32_t offset = 0;
    for (int32_t i = 0; i < names.size(); ++i) {
      table.i32(i + 1);
      table.text(names.at(i));
      offsets.push_back(offset);
      sizes.push_back(8 + static_cast<int32_t>(names.at(i).size()));
      offset += sizes.back();
    }
    const QByteArray bytes = table.bytes();
    const int32_t header =
        static_cast<int32_t>(bytes.size()) - offset; // 4 + header length
    QVERIFY(table.write(dir.filePath(QStringLiteral("NAMES.LFT"))));
    QByteArray indexBytes;
    appendLe32(&indexBytes, static_cast<int32_t>(names.size()));
    appendLe32(&indexBytes, header - 4);
    for (std::size_t i = 0; i < offsets.size(); ++i) {
      appendLe32(&indexBytes, header + offsets[i]);
      appendLe32(&indexBytes, sizes[i]);
    }
    QFile file(dir.filePath(QStringLiteral("NAMES.LFX")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(indexBytes);
    file.close();
    QString error;
    const std::optional<vpf::Table> read =
        vpf::Table::read(dir.filePath(QStringLiteral("NAMES.LFT")), &error);
    if (!read) {
      QFAIL(qPrintable(error));
    }
    QVERIFY2(
        read->checkIndex(dir.filePath(QStringLiteral("NAMES.LFX")), &error),
        qPrintable(error));
    // An index pointing elsewhere is reported.
    indexBytes[16] = static_cast<char>(indexBytes[16] + 1);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(indexBytes);
    file.close();
    QVERIFY(
        !read->checkIndex(dir.filePath(QStringLiteral("NAMES.LFX")), &error));
    QVERIFY(error.contains(QStringLiteral("entry 2")));
  }

  void thematicIndex()
  {
    // Two values of a text column, an inverted list of 32-bit row ids.
    QByteArray bytes;
    const std::function<void(int32_t)> put32 = [&bytes](int32_t value) {
      appendLe32(&bytes, value);
    };
    const int32_t header = 60 + 2 * (5 + 8);
    put32(header);
    put32(2);
    put32(3);
    bytes.append('I');
    bytes.append('T');
    put32(5);
    bytes.append('I');
    bytes.append(QByteArray("roads.lft   ", 12));
    bytes.append(QByteArray("f_code", 6));
    bytes.append(QByteArray(19 + 4, '\0'));
    bytes.append(QByteArray("AP030", 5));
    put32(header);
    put32(2);
    bytes.append(QByteArray("AP050", 5));
    put32(header + 8);
    put32(1);
    put32(1);
    put32(3);
    put32(2);
    QString error;
    const std::optional<vpf::ThematicIndex> index =
        vpf::ThematicIndex::parse(bytes, QStringLiteral("f_code1.lti"), &error);
    if (!index) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(index->table(), QStringLiteral("roads.lft"));
    QCOMPARE(index->column(), QStringLiteral("f_code"));
    QCOMPARE(index->featureCount(), 3);
    QCOMPARE(index->entries().size(), std::size_t{2});
    QCOMPARE(index->entries().at(0).value, QStringLiteral("AP030"));
    QCOMPARE(index->entries().at(0).rows, (std::vector<int32_t>{1, 3}));
    QCOMPARE(index->entries().at(1).rows, (std::vector<int32_t>{2}));
    QByteArray cut = bytes.left(bytes.size() - 2);
    QVERIFY(!vpf::ThematicIndex::parse(cut, QStringLiteral("x"), &error));
    QVERIFY(error.contains(QStringLiteral("entry 2")));
  }

  // --- Structure and topology ---------------------------------------------

  void syntheticDatabase()
  {
    QTemporaryDir dir;
    writeSyntheticDatabase(dir.path());
    QString error;
    const std::optional<vpf::Database> database =
        vpf::Database::open(dir.path(), &error);
    if (!database) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(database->name, QStringLiteral("testdb"));
    QCOMPARE(database->libraries.size(), std::size_t{1});
    QCOMPARE(database->libraries.front().bounds.xmax, 30.0);
    const std::optional<vpf::Library> library =
        vpf::Library::open(*database, QStringLiteral("testlib"), &error);
    if (!library) {
      QFAIL(qPrintable(error));
    }
    QVERIFY(library->isTiled());
    QCOMPARE(library->tilePaths.at(2), QStringLiteral("B/A"));
    QCOMPARE(library->tileBounds.at(2).xmin, 15.0);
    std::optional<vpf::Coverage> coverage =
        vpf::Coverage::open(*library, QStringLiteral("test"), &error);
    if (!coverage) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(coverage->featureClasses().size(), std::size_t{3});
    QCOMPARE(coverage->valueDescription(QStringLiteral("lakes.aft"),
                                        QStringLiteral("hyc"),
                                        QStringLiteral("8")),
             QStringLiteral("Perennial/Permanent"));
    // The lake's west part: an outer ring and the hole, from the winged
    // edges (the hole a loop edge).
    std::vector<vpf::Feature> lakes;
    vpf::ReadStats stats;
    QVERIFY2(
        coverage->readFeatures(*coverage->featureClass(QStringLiteral("lakes")),
                               &lakes, &stats, &error),
        qPrintable(error));
    QCOMPARE(stats.features, int64_t{2});
    QCOMPARE(stats.skipped, int64_t{0});
    QCOMPARE(lakes.size(), std::size_t{2});
    QCOMPARE(lakes.front().parts.size(), std::size_t{2});
    const std::vector<vpf::Coordinate>& outer = lakes.front().parts.front();
    QCOMPARE(outer.size(), std::size_t{5});
    QCOMPARE(outer.front(), outer.back());
    QCOMPARE(lakes.front().parts.at(1).size(), std::size_t{5});
    // Tile B/A's big-endian edges read the same way.
    QCOMPARE(lakes.at(1).tile, 2);
    QCOMPARE(lakes.at(1).parts.front().at(2).x, 20.0);
    std::vector<vpf::Feature> roads;
    QVERIFY(
        coverage->readFeatures(*coverage->featureClass(QStringLiteral("roads")),
                               &roads, &stats, &error));
    QCOMPARE(roads.size(), std::size_t{3});
    QCOMPARE(roads.at(1).parts.front().back().x, 25.0);
    std::vector<vpf::Feature> places;
    QVERIFY(coverage->readFeatures(
        *coverage->featureClass(QStringLiteral("places")), &places, &stats,
        &error));
    QCOMPARE(places.size(), std::size_t{2});
    QCOMPARE(places.front().parts.front().front().y, 12.0);
  }

  void brokenTopologyIsSkippedAndReported()
  {
    QTemporaryDir dir;
    writeSyntheticDatabase(dir.path());
    // Edge 2 of tile A/A no longer leads back to edge 1.
    writeEdges(dir.filePath(QStringLiteral("TESTLIB/TEST/A/A/EDG")),
               {
                   {1, 2, 2, 1, 2, 2, {{1, 1, 0}, {1, 14, 0}, {15, 14, 0}}},
                   {2, 1, 2, 1, 9, 1, {{15, 14, 0}, {15, 1, 0}, {1, 1, 0}}},
                   {3,
                    3,
                    2,
                    3,
                    3,
                    3,
                    {{5, 5, 0}, {9, 5, 0}, {9, 9, 0}, {5, 9, 0}, {5, 5, 0}}},
               },
               false);
    QString error;
    const std::optional<vpf::Library> library =
        openLibrary(dir.path(), QStringLiteral("testlib"), &error);
    if (!library) {
      QFAIL(qPrintable(error));
    }
    std::optional<vpf::Coverage> coverage =
        vpf::Coverage::open(*library, QStringLiteral("test"), &error);
    if (!coverage) {
      QFAIL(qPrintable(error));
    }
    std::vector<vpf::Feature> lakes;
    vpf::ReadStats stats;
    QVERIFY(
        coverage->readFeatures(*coverage->featureClass(QStringLiteral("lakes")),
                               &lakes, &stats, &error));
    QCOMPARE(stats.skipped, int64_t{1});
    QCOMPARE(lakes.size(), std::size_t{1});
    QVERIFY2(stats.problems.front().contains(QStringLiteral("EDG")),
             qPrintable(stats.problems.front()));
  }

  void conversionJoinsAndFilters()
  {
    QTemporaryDir dir;
    writeSyntheticDatabase(dir.path());
    ts::packager::Vmap0Options options;
    options.databases = {dir.path()};
    options.lods = {ts::packager::Vmap0Lod{4, 4, -1, 0.0}};
    using Kind = ts::packager::Vmap0Attribute::Kind;
    options.classes = {
        vmap0Class(
            QStringLiteral("test"), QStringLiteral("lakes"),
            QStringLiteral("lakes"),
            {attribute(QStringLiteral("f_code"), QStringLiteral("feature")),
             attribute(QStringLiteral("nam"), QStringLiteral("name")),
             attribute(QStringLiteral("hyc"),
                       QStringLiteral("hydrologic_category"))}),
        vmap0Class(QStringLiteral("test"), QStringLiteral("roads"),
                   QStringLiteral("roads"),
                   {attribute(QStringLiteral("use"), QStringLiteral("use")),
                    attribute(QStringLiteral("use"), QStringLiteral("code"),
                              Kind::Number)},
                   {ts::packager::Vmap0Filter{
                       QStringLiteral("use"), {QStringLiteral("26")}, true}}),
        vmap0Class(QStringLiteral("test"), QStringLiteral("places"),
                   QStringLiteral("places"),
                   {attribute(QStringLiteral("nam"), QStringLiteral("name"))}),
    };
    const QString path = dir.filePath(QStringLiteral("out.gpkg"));
    std::vector<ts::packager::Vmap0ClassStats> stats;
    QString error;
    {
      ts::GeoPackageFeatureWriter writer(path, false);
      QVERIFY2(ts::packager::buildVmap0(options, &writer, &stats, &error),
               qPrintable(error));
      QVERIFY(writer.finish());
    }
    QCOMPARE(stats.size(), std::size_t{3});
    // Every feature accounted for: read = filtered + joined + written.
    for (const ts::packager::Vmap0ClassStats& cls : stats) {
      QCOMPARE(cls.sourceFeatures, cls.filtered + cls.skipped + cls.degenerate +
                                       cls.joined + cls.features);
    }
    QCOMPARE(stats.at(0).joined, int64_t{1});
    QCOMPARE(stats.at(1).filtered, int64_t{1});
    QCOMPARE(stats.at(1).joined, int64_t{1});

    ts::GeosContext geos;
    GEOSContextHandle_t context = static_cast<GEOSContextHandle_t>(geos.get());
    // The lake, joined across 15° E: one polygon with its hole, no edge
    // along the cut, 14 × 13 − 4 × 4 + 5 × 13 square degrees.
    const std::vector<Row> lakes = readRows(path, QStringLiteral("lakes_4"));
    QVERIFY(!lakes.empty());
    double area = 0.0;
    int32_t holes = 0;
    for (const Row& row : lakes) {
      const Geometry geometry(context, fromBlob(context, row.geometry));
      if (geometry.geometry == nullptr) {
        QFAIL("a lake's geometry does not read");
      }
      QCOMPARE(GEOSisValid_r(context, geometry.geometry), static_cast<char>(1));
      double part = 0.0;
      GEOSArea_r(context, geometry.geometry, &part);
      area += part;
      for (int32_t p = 0;
           p < GEOSGetNumGeometries_r(context, geometry.geometry); ++p) {
        const GEOSGeometry* polygon =
            GEOSGetGeometryN_r(context, geometry.geometry, p);
        holes += GEOSGetNumInteriorRings_r(context, polygon);
        const GEOSCoordSequence* ring = GEOSGeom_getCoordSeq_r(
            context, GEOSGetExteriorRing_r(context, polygon));
        uint32_t size = 0;
        GEOSCoordSeq_getSize_r(context, ring, &size);
        for (uint32_t v = 1; v < size; ++v) {
          double x0 = 0.0;
          double y0 = 0.0;
          double x1 = 0.0;
          double y1 = 0.0;
          GEOSCoordSeq_getXY_r(context, ring, v - 1, &x0, &y0);
          GEOSCoordSeq_getXY_r(context, ring, v, &x1, &y1);
          QVERIFY2(x0 != 15.0 || x1 != 15.0, "an edge along the tile cut");
        }
      }
      QCOMPARE(row.values.at(QStringLiteral("feature")).toString(),
               QStringLiteral("Inland Water"));
      QCOMPARE(row.values.at(QStringLiteral("name")).toString(),
               QStringLiteral("Test Lake"));
      QCOMPARE(row.values.at(QStringLiteral("hydrologic_category")).toString(),
               QStringLiteral("Perennial/Permanent"));
    }
    QCOMPARE(area, 14.0 * 13.0 - 16.0 + 5.0 * 13.0);
    QCOMPARE(holes, 1);
    // The road: its two pieces one line from 2° to 25° E; the road of use
    // 26 left out.
    const std::vector<Row> roads = readRows(path, QStringLiteral("roads_4"));
    double length = 0.0;
    std::set<QString> uses;
    for (const Row& row : roads) {
      const Geometry geometry(context, fromBlob(context, row.geometry));
      double part = 0.0;
      GEOSLength_r(context, geometry.geometry, &part);
      length += part;
      uses.insert(row.values.at(QStringLiteral("use")).toString());
      // A number its value table names (as VMAP0 names its "Unknown"
      // codes) is no number: NULL.
      QCOMPARE(row.values.count(QStringLiteral("code")), std::size_t{0});
    }
    QCOMPARE(length, 23.0);
    QCOMPARE(uses, (std::set<QString>{QStringLiteral("International")}));
    // Places: "UNK" is no name.
    const std::vector<Row> places = readRows(path, QStringLiteral("places_4"));
    QCOMPARE(places.size(), std::size_t{2});
    int32_t named = 0;
    for (const Row& row : places) {
      named += row.values.count(QStringLiteral("name")) != 0 ? 1 : 0;
    }
    QCOMPARE(named, 1);
    // The provider reads them back, with ids and picking.
    ts::GeoPackageFeatureProvider provider(path);
    QVERIFY2(provider.isValid(), qPrintable(provider.errorString()));
    const ts::TileKey key =
        ts::TileKey::containing(4, ts::Geodetic{12.0, 5.0, 0.0});
    const std::optional<ts::FeatureTile> tile =
        provider.loadFeatures(key, notCancelled);
    if (!tile) {
      QFAIL("no tile");
    }
    const std::vector<ts::Feature> picked =
        provider.featuresAt(ts::GeoPoint{12.0, 5.0}, 0.01);
    QVERIFY(std::any_of(
        picked.begin(), picked.end(), [](const ts::Feature& feature) {
          return feature.attributes.value(QStringLiteral("name")) ==
                 QStringLiteral("Alpha");
        }));
    QVERIFY(std::all_of(
        picked.begin(), picked.end(),
        [](const ts::Feature& feature) { return !feature.id.isEmpty(); }));
    const std::vector<ts::Feature> labels =
        provider.labelPoints(QStringLiteral("name"), {tile->key});
    QCOMPARE(labels.size(), std::size_t{1});
    QCOMPARE(labels.front().attributes.value(QStringLiteral("name")).toString(),
             QStringLiteral("Alpha"));
  }

  void esriFeaturesSplitByFilter()
  {
    // The public packs leave use 26 out; the ESRI pack keeps only it.
    QTemporaryDir dir;
    writeSyntheticDatabase(dir.path());
    for (const bool exclude : {true, false}) {
      ts::packager::Vmap0Options options;
      options.databases = {dir.path()};
      options.lods = {ts::packager::Vmap0Lod{4, 4, -1, 0.0}};
      options.classes = {vmap0Class(
          QStringLiteral("test"), QStringLiteral("roads"),
          QStringLiteral("roads"),
          {attribute(QStringLiteral("use"), QStringLiteral("use"))},
          {ts::packager::Vmap0Filter{
              QStringLiteral("use"), {QStringLiteral("26")}, exclude}})};
      const QString path = dir.filePath(exclude ? QStringLiteral("public.gpkg")
                                                : QStringLiteral("esri.gpkg"));
      std::vector<ts::packager::Vmap0ClassStats> stats;
      QString error;
      ts::GeoPackageFeatureWriter writer(path, false);
      QVERIFY2(ts::packager::buildVmap0(options, &writer, &stats, &error),
               qPrintable(error));
      QVERIFY(writer.finish());
      std::set<QString> uses;
      for (const Row& row : readRows(path, QStringLiteral("roads_4"))) {
        uses.insert(row.values.at(QStringLiteral("use")).toString());
      }
      QCOMPARE(uses,
               exclude
                   ? std::set<QString>{QStringLiteral("International")}
                   : std::set<QString>{QStringLiteral("Primary/1st Order")});
    }
  }

  // --- The real VMAP0 tile -------------------------------------------------

  void realTileReads_data()
  {
    QTest::addColumn<QString>("coverage");
    QTest::addColumn<QString>("featureClass");
    QTest::addColumn<int32_t>("features");
    // As VMAP0 edition 5 holds them for tile L/H of SOAMAFR (ESRI's
    // boundary features left out of the fixture).
    QTest::newRow("bnd/coastl") << "bnd" << "coastl" << 43;
    QTest::newRow("bnd/polbndl") << "bnd" << "polbndl" << 3;
    QTest::newRow("bnd/depthl") << "bnd" << "depthl" << 56;
    QTest::newRow("dq/dqarea") << "dq" << "dqarea" << 5;
    QTest::newRow("dq/dqline") << "dq" << "dqline" << 2;
    QTest::newRow("elev/contourl") << "elev" << "contourl" << 227;
    QTest::newRow("elev/elevp") << "elev" << "elevp" << 114;
    QTest::newRow("hydro/watrcrsl") << "hydro" << "watrcrsl" << 424;
    QTest::newRow("hydro/inwatera") << "hydro" << "inwatera" << 158;
    QTest::newRow("hydro/hydrotxt") << "hydro" << "hydrotxt" << 7;
    QTest::newRow("pop/mispopp") << "pop" << "mispopp" << 209;
    QTest::newRow("pop/builtupa") << "pop" << "builtupa" << 5;
    QTest::newRow("trans/roadl") << "trans" << "roadl" << 411;
    QTest::newRow("trans/aerofacp") << "trans" << "aerofacp" << 15;
    QTest::newRow("veg/grassa") << "veg" << "grassa" << 137;
    QTest::newRow("phys/grounda") << "phys" << "grounda" << 48;
    QTest::newRow("util/utill") << "util" << "utill" << 13;
    QTest::newRow("tileref/tileref") << "tileref" << "tileref" << 62;
  }

  void realTileReads()
  {
    QFETCH(QString, coverage);
    QFETCH(QString, featureClass);
    QFETCH(int32_t, features);
    QString error;
    const std::optional<vpf::Database> database =
        vpf::Database::open(fixtureDatabase(), &error);
    if (!database) {
      QFAIL(qPrintable(error));
    }
    const std::optional<vpf::Library> library =
        vpf::Library::open(*database, QStringLiteral("soamafr"), &error);
    if (!library) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(library->tilePaths.size(), std::size_t{62});
    std::optional<vpf::Coverage> opened =
        vpf::Coverage::open(*library, coverage, &error);
    if (!opened) {
      QFAIL(qPrintable(error));
    }
    const vpf::FeatureClass* cls = opened->featureClass(featureClass);
    if (cls == nullptr) {
      QFAIL("no such feature class");
    }
    std::vector<vpf::Feature> read;
    vpf::ReadStats stats;
    QVERIFY2(opened->readFeatures(*cls, &read, &stats, &error),
             qPrintable(error));
    QCOMPARE(stats.features, int64_t{features});
    QCOMPARE(stats.skipped, int64_t{0});
    QCOMPARE(read.size(), static_cast<std::size_t>(features));
    // Every area closes, inside the tile (30° W–15° W, 15° N–30° N) unless
    // it is the tile reference itself.
    for (const vpf::Feature& feature : read) {
      for (const std::vector<vpf::Coordinate>& part : feature.parts) {
        QVERIFY(!part.empty());
        if (feature.type == vpf::FeatureType::Area) {
          QCOMPARE(part.front(), part.back());
        }
        if (coverage != QLatin1String("tileref")) {
          for (const vpf::Coordinate& point : part) {
            QVERIFY(point.x >= -30.0 - 1e-4 && point.x <= -15.0 + 1e-4);
            QVERIFY(point.y >= 15.0 - 1e-4 && point.y <= 30.0 + 1e-4);
          }
        }
      }
    }
  }

  void realTileValues()
  {
    QString error;
    const std::optional<vpf::Library> library =
        openLibrary(fixtureDatabase(), QStringLiteral("soamafr"), &error);
    if (!library) {
      QFAIL(qPrintable(error));
    }
    std::optional<vpf::Coverage> hydro =
        vpf::Coverage::open(*library, QStringLiteral("hydro"), &error);
    if (!hydro) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(hydro->valueDescription(QStringLiteral("watrcrsl.lft"),
                                     QStringLiteral("hyc"),
                                     QStringLiteral("6")),
             QStringLiteral("Non-Perennial/Intermittent/Fluctuating"));
    QCOMPARE(hydro->valueDescription(QStringLiteral("inwatera.aft"),
                                     QStringLiteral("f_code"),
                                     QStringLiteral("BH000")),
             QStringLiteral("Inland Water"));
    // The data quality coverage: its areas name their sources.
    std::optional<vpf::Coverage> dq =
        vpf::Coverage::open(*library, QStringLiteral("dq"), &error);
    if (!dq) {
      QFAIL(qPrintable(error));
    }
    const vpf::Table* areas = dq->table(QStringLiteral("dqarea.aft"), &error);
    if (areas == nullptr) {
      qWarning("%s", qPrintable(error));
      QFAIL("dqarea.aft does not read");
    }
    QVERIFY(areas->columnIndex(QStringLiteral("source_id")) >= 0);
    QVERIFY(!areas->text(0, areas->columnIndex(QStringLiteral("source_id")))
                 .isEmpty());
    // The fixture's variable-length indexes match their tables.
    const vpf::Table* rivers =
        hydro->table(QStringLiteral("watrcrsl.lft"), &error);
    if (rivers == nullptr) {
      QFAIL(qPrintable(error));
    }
    const QString edges =
        fixtureDatabase() + QStringLiteral("/SOAMAFR/HYDRO/L/H");
    const std::optional<vpf::Table> edgeTable =
        vpf::Table::read(edges + QStringLiteral("/EDG"), &error);
    if (!edgeTable) {
      QFAIL(qPrintable(error));
    }
    QVERIFY2(edgeTable->checkIndex(edges + QStringLiteral("/EDX"), &error),
             qPrintable(error));
  }

  void realTileConversion()
  {
    // Hydrography of the real tile through the whole pack build: valid
    // geometry, within tolerance, tiles without gaps or overlaps, decoded
    // attributes, every feature accounted for.
    QTemporaryDir dir;
    const QString database = fixtureDatabase();
    ts::packager::Vmap0Options options;
    options.databases = {database};
    // Level 4 unsimplified, to check the clipping on its own.
    options.lods = {ts::packager::Vmap0Lod{2, 2, 3, 0.02},
                    ts::packager::Vmap0Lod{4, 4, 5, 0.0},
                    ts::packager::Vmap0Lod{6, 6, -1, 0.001}};
    options.classes = {
        vmap0Class(
            QStringLiteral("hydro"), QStringLiteral("inwatera"),
            QStringLiteral("lakes"),
            {attribute(QStringLiteral("f_code"), QStringLiteral("feature")),
             attribute(QStringLiteral("nam"), QStringLiteral("name")),
             attribute(QStringLiteral("hyc"),
                       QStringLiteral("hydrologic_category"))}),
        vmap0Class(
            QStringLiteral("bnd"), QStringLiteral("coastl"),
            QStringLiteral("coastlines"),
            {attribute(QStringLiteral("f_code"), QStringLiteral("feature"))}),
        vmap0Class(
            QStringLiteral("trans"), QStringLiteral("aerofacp"),
            QStringLiteral("airports"),
            {attribute(QStringLiteral("nam"), QStringLiteral("name")),
             attribute(QStringLiteral("zv3"), QStringLiteral("elevation_m"),
                       ts::packager::Vmap0Attribute::Kind::Number)}),
    };
    const QString path = dir.filePath(QStringLiteral("hydro.gpkg"));
    std::vector<ts::packager::Vmap0ClassStats> stats;
    QString error;
    {
      ts::GeoPackageFeatureWriter writer(path, false);
      QVERIFY2(ts::packager::buildVmap0(options, &writer, &stats, &error),
               qPrintable(error));
      QVERIFY(writer.finish());
    }
    QCOMPARE(stats.at(0).sourceFeatures, int64_t{158});
    QCOMPARE(stats.at(1).sourceFeatures, int64_t{43});
    QCOMPARE(stats.at(2).sourceFeatures, int64_t{15});
    for (const ts::packager::Vmap0ClassStats& cls : stats) {
      QCOMPARE(cls.skipped, int64_t{0});
      QCOMPARE(cls.degenerate, int64_t{0});
      QCOMPARE(cls.sourceFeatures, cls.filtered + cls.joined + cls.features);
    }

    ts::GeosContext geos;
    GEOSContextHandle_t context = static_cast<GEOSContextHandle_t>(geos.get());
    // The source lakes, united, for comparison.
    const std::optional<vpf::Library> library =
        openLibrary(database, QStringLiteral("soamafr"), &error);
    if (!library) {
      QFAIL(qPrintable(error));
    }
    std::optional<vpf::Coverage> hydro =
        vpf::Coverage::open(*library, QStringLiteral("hydro"), &error);
    if (!hydro) {
      QFAIL(qPrintable(error));
    }
    std::vector<vpf::Feature> lakes;
    vpf::ReadStats readStats;
    QVERIFY(
        hydro->readFeatures(*hydro->featureClass(QStringLiteral("inwatera")),
                            &lakes, &readStats, &error));
    const Geometry collection(context, polygonsOf(context, lakes));
    const Geometry source(context,
                          GEOSUnaryUnion_r(context, collection.geometry));
    double sourceArea = 0.0;
    GEOSArea_r(context, source.geometry, &sourceArea);

    // Unsimplified, the tiles' pieces neither overlap nor leave gaps: their
    // areas add up to their union's, which is the source's. Every piece is
    // valid, its attributes decoded.
    const std::vector<Row> fine = readRows(path, QStringLiteral("lakes_4"));
    QVERIFY(!fine.empty());
    std::vector<GEOSGeometry*> pieces;
    double pieceArea = 0.0;
    std::set<QString> categories;
    for (const Row& row : fine) {
      GEOSGeometry* piece = fromBlob(context, row.geometry);
      QCOMPARE(GEOSisValid_r(context, piece), static_cast<char>(1));
      double area = 0.0;
      GEOSArea_r(context, piece, &area);
      pieceArea += area;
      pieces.push_back(piece);
      const QString feature =
          row.values.at(QStringLiteral("feature")).toString();
      QVERIFY2(feature == QLatin1String("Inland Water") ||
                   feature == QLatin1String("Land Subject to Inundation"),
               qPrintable(feature));
      categories.insert(
          row.values.at(QStringLiteral("hydrologic_category")).toString());
      QCOMPARE(row.values.count(QStringLiteral("feature_id")), std::size_t{1});
    }
    QVERIFY(categories.count(QStringLiteral("Perennial/Permanent")) == 1);
    const Geometry all(context,
                       GEOSGeom_createCollection_r(
                           context, GEOS_GEOMETRYCOLLECTION, pieces.data(),
                           static_cast<uint32_t>(pieces.size())));
    const Geometry united(context, GEOSUnaryUnion_r(context, all.geometry));
    double unitedArea = 0.0;
    GEOSArea_r(context, united.geometry, &unitedArea);
    QVERIFY2(std::abs(pieceArea - unitedArea) <= 1e-9 * unitedArea,
             "tile pieces overlap");
    QVERIFY2(
        std::abs(unitedArea - sourceArea) <= 1e-9 * sourceArea,
        qPrintable(QStringLiteral("%1 vs %2").arg(unitedArea).arg(sourceArea)));
    // Simplified, every piece is valid and its vertices lie within the
    // level's tolerance of a source ring (shared ones too: lakes beside
    // land subject to inundation keep their common edge).
    const Geometry boundary(context, ringsOf(context, lakes));
    const double fine6 = farthestFrom(context, path, QStringLiteral("lakes_6"),
                                      boundary.geometry, 180.0 / 64.0);
    QVERIFY2(fine6 <= 0.001 * (1.0 + 1e-9), qPrintable(QString::number(fine6)));
    const double coarse2 =
        farthestFrom(context, path, QStringLiteral("lakes_2"),
                     boundary.geometry, 180.0 / 4.0);
    QVERIFY2(coarse2 <= 0.02 * (1.0 + 1e-9),
             qPrintable(QString::number(coarse2)));
    // Airports: names, and elevations as numbers.
    const std::vector<Row> airports =
        readRows(path, QStringLiteral("airports_6"));
    QCOMPARE(airports.size(), std::size_t{15});
    for (const Row& row : airports) {
      QVERIFY(row.values.count(QStringLiteral("name")) == 1);
      if (row.values.count(QStringLiteral("elevation_m")) != 0) {
        QVERIFY(row.values.at(QStringLiteral("elevation_m")).toLongLong() <
                5000);
      }
    }
  }

  void coastlinesMatchGshhg()
  {
    // VMAP0's coastlines lie on the base pack's GSHHG shoreline within
    // VMAP0's own stated accuracy (its data quality areas'
    // abs_horiz_acc) plus GSHHG's intermediate resolution (about 1 km,
    // which the base pack draws from level 4).
    const QString basePack = qEnvironmentVariable("TS_BASEPACK_DIR");
    if (basePack.isEmpty()) {
      QSKIP("TS_BASEPACK_DIR is not set.");
    }
    QString error;
    const std::optional<vpf::Library> library =
        openLibrary(fixtureDatabase(), QStringLiteral("soamafr"), &error);
    if (!library) {
      QFAIL(qPrintable(error));
    }
    std::optional<vpf::Coverage> dq =
        vpf::Coverage::open(*library, QStringLiteral("dq"), &error);
    if (!dq) {
      QFAIL(qPrintable(error));
    }
    const vpf::Table* areas = dq->table(QStringLiteral("dqarea.aft"), &error);
    if (areas == nullptr) {
      qWarning("%s", qPrintable(error));
      QFAIL("dqarea.aft does not read");
    }
    int64_t accuracyM = 0;
    for (int32_t row = 0; row < areas->rowCount(); ++row) {
      accuracyM = std::max(
          accuracyM, areas->integer(row, areas->columnIndex(
                                             QStringLiteral("abs_horiz_acc"))));
    }
    QVERIFY(accuracyM > 0 && accuracyM < 10000);
    std::optional<vpf::Coverage> bnd =
        vpf::Coverage::open(*library, QStringLiteral("bnd"), &error);
    if (!bnd) {
      QFAIL(qPrintable(error));
    }
    std::vector<vpf::Feature> coasts;
    vpf::ReadStats stats;
    QVERIFY(bnd->readFeatures(*bnd->featureClass(QStringLiteral("coastl")),
                              &coasts, &stats, &error));

    // GSHHG's land around the tile, united (no tile cuts), in a frame
    // where a degree of longitude is as long as one of latitude at 22° N.
    ts::GeoPackageFeatureProvider shoreline(
        QDir(basePack).filePath(QStringLiteral("shoreline.gpkg")));
    QVERIFY2(shoreline.isValid(), qPrintable(shoreline.errorString()));
    ts::GeosContext geos;
    GEOSContextHandle_t context = static_cast<GEOSContextHandle_t>(geos.get());
    const double scale = std::cos(22.0 * 3.14159265358979323846 / 180.0);
    std::vector<GEOSGeometry*> land =
        landAround(context, shoreline, -32, -13, 13, 32, scale);
    const Geometry pieces(context,
                          GEOSGeom_createCollection_r(
                              context, GEOS_GEOMETRYCOLLECTION, land.data(),
                              static_cast<uint32_t>(land.size())));
    const Geometry united(context, GEOSUnaryUnion_r(context, pieces.geometry));
    const Geometry shore(context, GEOSBoundary_r(context, united.geometry));
    if (shore.geometry == nullptr) {
      QFAIL("no shore line");
    }
    std::vector<double> distancesKm;
    for (const vpf::Feature& coast : coasts) {
      for (const std::vector<vpf::Coordinate>& part : coast.parts) {
        for (const vpf::Coordinate& vertex : part) {
          const Geometry point(
              context, GEOSGeom_createPointFromXY_r(context, vertex.x * scale,
                                                    vertex.y));
          double distance = 0.0;
          GEOSDistance_r(context, point.geometry, shore.geometry, &distance);
          distancesKm.push_back(distance * 111.0);
        }
      }
    }
    QVERIFY(distancesKm.size() > 1000);
    std::sort(distancesKm.begin(), distancesKm.end());
    const double median = distancesKm[distancesKm.size() / 2];
    const double p90 = distancesKm[distancesKm.size() * 9 / 10];
    qInfo("VMAP0 coastline to GSHHG: median %.2f km, 90%% %.2f km, max %.2f "
          "km (VMAP0 accuracy %d m)",
          median, p90, distancesKm.back(), static_cast<int32_t>(accuracyM));
    QVERIFY(p90 <= static_cast<double>(accuracyM) / 1000.0 + 1.0);
  }

  // --- Pack configurations -------------------------------------------------

  void packConfigsAreConsistent()
  {
    const QStringList configs = packConfigs();
    QVERIFY(configs.size() >= 8);
    for (const QString& path : configs) {
      QString error;
      const std::optional<ts::packager::PackConfig> config =
          ts::packager::PackConfig::read(path, &error);
      if (!config) {
        QFAIL(qPrintable(path + QStringLiteral(": ") + error));
      }
      if (!config->vmap0) {
        QFAIL(qPrintable(path + QStringLiteral(": no vmap0 entry")));
      }
      const ts::packager::Vmap0Spec& spec = *config->vmap0;
      QVERIFY(config->id.startsWith(QLatin1String("vmap0-")));
      QVERIFY(!spec.layers.isEmpty());
      std::set<QString> tables;
      for (const ts::packager::Vmap0Class& cls : spec.classes) {
        QVERIFY2(tables.insert(cls.table).second, qPrintable(cls.table));
        // Classes are shown only where a level of detail serves them.
        QVERIFY(cls.minLevel >= spec.lods.front().minLevel);
      }
      const std::set<QString> attributes = attributeNames(spec);
      // NOLINTNEXTLINE(modernize-loop-convert)
      for (int32_t i = 0; i < spec.layers.size(); ++i) {
        const QString problem =
            layerProblem(spec.layers.at(i).toObject(), tables, attributes);
        QVERIFY2(problem.isEmpty(),
                 qPrintable(path + QStringLiteral(": ") + problem));
      }
      // The ESRI parts only in the ESRI pack, with ESRI's terms.
      const bool esri = config->id == QLatin1String("vmap0-admin");
      QCOMPARE(keepsEsriFeatures(spec), esri);
      QCOMPARE(spec.licence.contains(QLatin1String("ESRI")), esri);
      QVERIFY(spec.attribution.contains(QLatin1String("VMAP0")));
      for (const ts::packager::SourceSpec& source : config->sources) {
        QVERIFY(std::find(source.notices.begin(), source.notices.end(),
                          QStringLiteral("README1.TXT")) !=
                source.notices.end());
      }
    }
  }

  void fixturePackBuilds()
  {
    // A pack from the real tile with the repository's transportation
    // configuration: its manifest lists the dataset and the layers.
    QTemporaryDir dir;
    QString error;
    const QString output = dir.filePath(QStringLiteral("pack"));
    ts::packager::BuildReport report;
    QVERIFY2(buildFixturePack(output, QStringLiteral("vmap0-transportation"),
                              QStringLiteral("5.1"), &error, &report),
             qPrintable(error));
    const std::optional<ts::packager::PackConfig> config =
        ts::packager::PackConfig::read(
            QStringLiteral(TS_ROOT_DIR
                           "/tools/packager/packs/vmap0-transportation.json"),
            &error);
    if (!config || !config->vmap0) {
      QFAIL(qPrintable(error));
    }
    const int32_t layerCount =
        static_cast<int32_t>(config->vmap0->layers.size());
    const std::optional<ts::PackManifest> manifest =
        ts::PackManifest::read(output, &error);
    if (!manifest) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(manifest->name, QStringLiteral("VMAP0 transportation"));
    QCOMPARE(manifest->datasets.size(), std::size_t{1});
    QCOMPARE(manifest->datasets.front().kind, ts::PackDataset::Kind::Features);
    QVERIFY(manifest->datasets.front().replaces.isEmpty());
    QCOMPARE(static_cast<int32_t>(manifest->layers.size()), layerCount);
    const QJsonObject first = manifest->layers.at(0).toObject();
    QCOMPARE(first.value(QStringLiteral("id")).toString(),
             QStringLiteral("pack:vmap0-transportation:roads"));
    QCOMPARE(first.value(QStringLiteral("source"))
                 .toObject()
                 .value(QStringLiteral("dataset"))
                 .toString(),
             QStringLiteral("vmap0-transportation"));
    const ts::PackVerification verification =
        manifest->verify(output, notCancelled);
    QVERIFY(verification.ok);
    // The report: the roads read and written.
    const QJsonArray classes =
        report.toJson().value(QStringLiteral("vmap0")).toArray();
    QVERIFY(!classes.isEmpty());
    QCOMPARE(classes.at(0).toObject().value(QStringLiteral("table")).toString(),
             QStringLiteral("main_roads"));
    QCOMPARE(classes.at(0)
                 .toObject()
                 .value(QStringLiteral("sourceFeatures"))
                 .toInt(),
             411);
  }

  void packInstallsUpdatesAndRemoves()
  {
    // The pack from the real tile, served by the mock server: installed,
    // read through the registry, updated, removed.
    QTemporaryDir dir;
    QString error;
    const QString v1 = dir.filePath(QStringLiteral("v1"));
    const QString v2 = dir.filePath(QStringLiteral("v2"));
    QVERIFY2(buildFixturePack(v1, QStringLiteral("vmap0-transportation"),
                              QStringLiteral("5.1"), &error),
             qPrintable(error));
    QVERIFY2(buildFixturePack(v2, QStringLiteral("vmap0-transportation"),
                              QStringLiteral("5.2"), &error),
             qPrintable(error));
    ts::test::MockHttpServer server;
    const ts::test::RemotePack first = servedPack(server, v1);
    const ts::test::RemotePack second = servedPack(server, v2);
    ts::test::Served files = first.files;
    files.insert(second.files.cbegin(), second.files.cend());
    server.setHandler([files](const ts::test::MockHttpRequest& request) {
      return ts::test::serve(files, request, true);
    });
    const QTemporaryDir root;
    ts::PackLibrary library(root.path());
    QVERIFY2(installPack(library, first.entry, &error), qPrintable(error));
    ts::ProviderRegistry registry;
    registry.setInstalledPacks(library.installed());
    const QJsonObject airports = packSource({QStringLiteral("airports")});
    std::shared_ptr<ts::FeatureProvider> provider =
        registry.features(airports, &error);
    QVERIFY2(provider, qPrintable(error));
    QCOMPARE(registry.attribution(airports),
             QStringLiteral("Vector Map Level 0 (VMAP0), NGA"));
    // Airports show from level 5, stored at level 4.
    const ts::TileKey key =
        ts::TileKey::containing(5, ts::Geodetic{20.9, -17.0, 0.0});
    const std::optional<ts::FeatureTile> tile =
        provider->loadFeatures(key, notCancelled);
    if (!tile) {
      QFAIL("no tile");
    }
    QCOMPARE(tile->key.level, 4);
    QVERIFY(!tile->features.empty());
    for (const ts::Feature& feature : tile->features) {
      QCOMPARE(feature.type, ts::Feature::Type::Points);
      QCOMPARE(feature.attributes.value(QStringLiteral("feature")).toString(),
               QStringLiteral("Airport/Airfield"));
    }
    QVERIFY(
        !provider->labelPoints(QStringLiteral("name"), {tile->key}).empty());
    provider.reset();

    QVERIFY2(installPack(library, second.entry, &error), qPrintable(error));
    QCOMPARE(library.installed().size(), std::size_t{1});
    QCOMPARE(library.installed().front().manifest.version,
             QStringLiteral("5.2"));
    registry.setInstalledPacks(library.installed());
    QVERIFY(registry.features(airports, &error));

    QVERIFY(library.remove(QStringLiteral("vmap0-transportation")));
    QVERIFY(library.installed().empty());
    registry.setInstalledPacks(library.installed());
    QVERIFY(!registry.features(airports, &error));
  }

  void regionalPacksReadTogether()
  {
    // Two packs of one dataset (as VMAP0's regional packs are): one
    // provider reading both; picking finds a feature in each.
    QTemporaryDir dir;
    QString error;
    std::vector<ts::InstalledPack> installed;
    for (const QString& id : {QStringLiteral("vmap0-transportation-a"),
                              QStringLiteral("vmap0-transportation-b")}) {
      ts::InstalledPack pack;
      pack.directory = dir.filePath(id);
      QVERIFY2(
          buildFixturePack(pack.directory, id, QStringLiteral("5.1"), &error),
          qPrintable(error));
      const std::optional<ts::PackManifest> manifest =
          ts::PackManifest::read(pack.directory, &error);
      if (!manifest) {
        QFAIL("no manifest");
      }
      pack.manifest = *manifest;
      installed.push_back(pack);
    }
    ts::ProviderRegistry registry;
    registry.setInstalledPacks(installed);
    const std::shared_ptr<ts::FeatureProvider> provider =
        registry.features(packSource({QStringLiteral("airports")}), &error);
    QVERIFY2(provider, qPrintable(error));
    const std::shared_ptr<ts::CompositeFeatureProvider> composite =
        std::dynamic_pointer_cast<ts::CompositeFeatureProvider>(provider);
    QVERIFY(composite);
    QCOMPARE(composite->sources().size(), std::size_t{2});
    const ts::TileKey key =
        ts::TileKey::containing(5, ts::Geodetic{20.9, -17.0, 0.0});
    const std::optional<ts::FeatureTile> tile =
        provider->loadFeatures(key, notCancelled);
    if (!tile) {
      QFAIL("no tile");
    }
    const ts::FeatureTile& loaded = *tile;
    // Each airport twice: once from each pack.
    std::map<QString, int32_t> names;
    for (const ts::Feature& feature : loaded.features) {
      names[feature.attributes.value(QStringLiteral("name")).toString()] += 1;
    }
    QVERIFY(!names.empty());
    for (const std::pair<const QString, int32_t>& name : names) {
      QCOMPARE(name.second % 2, 0);
    }
    const ts::Feature& one = loaded.features.front();
    const std::vector<ts::Feature> picked =
        provider->featuresAt(one.parts.front().front(), 0.01);
    QVERIFY(picked.size() >= 2);
    // With one of them removed, one provider again.
    registry.setInstalledPacks({installed.front()});
    QVERIFY(!std::dynamic_pointer_cast<ts::CompositeFeatureProvider>(
        registry.features(packSource({QStringLiteral("airports")}), &error)));
  }

  // --- Threads -------------------------------------------------------------

  void concurrentOwnInstances()
  {
    // Each thread reads the real tile with its own tables and coverage.
    const int32_t failures =
        ts::test::runConcurrently(ts::test::concurrentThreadCount, [](int32_t) {
          QString error;
          const std::optional<vpf::Database> database =
              vpf::Database::open(fixtureDatabase(), &error);
          if (!database) {
            return false;
          }
          const std::optional<vpf::Library> library =
              vpf::Library::open(*database, QStringLiteral("soamafr"), &error);
          if (!library) {
            return false;
          }
          std::optional<vpf::Coverage> coverage =
              vpf::Coverage::open(*library, QStringLiteral("trans"), &error);
          if (!coverage) {
            return false;
          }
          std::vector<vpf::Feature> roads;
          vpf::ReadStats stats;
          return coverage->readFeatures(
                     *coverage->featureClass(QStringLiteral("roadl")), &roads,
                     &stats, &error) &&
                 roads.size() == 411;
        });
    QCOMPARE(failures, 0);
  }

  void benchmarkReadTile()
  {
    QString error;
    const std::optional<vpf::Library> library =
        openLibrary(fixtureDatabase(), QStringLiteral("soamafr"), &error);
    if (!library) {
      QFAIL(qPrintable(error));
    }
    QBENCHMARK
    {
      std::optional<vpf::Coverage> coverage =
          vpf::Coverage::open(*library, QStringLiteral("hydro"), &error);
      if (!coverage) {
        QFAIL(qPrintable(error));
      }
      std::vector<vpf::Feature> rivers;
      vpf::ReadStats stats;
      coverage->readFeatures(
          *coverage->featureClass(QStringLiteral("watrcrsl")), &rivers, &stats,
          &error);
    }
  }
};

QTEST_GUILESS_MAIN(TestVmap0)
#include "tst_vmap0.moc"
