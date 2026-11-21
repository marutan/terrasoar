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

#include "vpfcoverage.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <QString>
#include <QStringList>

namespace ts::packager::vpf
{

namespace
{

bool fail(QString* errorString, const QString& message)
{
  if (errorString != nullptr) {
    *errorString = message;
  }
  return false;
}

/** Reads table @p name of directory @p directory (any case). */
std::optional<Table> readTable(const QString& directory, const QString& name,
                               QString* errorString)
{
  const QString path = findEntry(directory, name);
  if (path.isEmpty()) {
    fail(errorString,
         QStringLiteral("%1/%2: no such table").arg(directory, name));
    return std::nullopt;
  }
  return Table::read(path, errorString);
}

/** Returns the column of @p table named @p name, failing if it is missing. */
int32_t needColumn(const Table& table, const QString& name,
                   QString* errorString)
{
  const int32_t column = table.columnIndex(name);
  if (column < 0) {
    fail(errorString,
         QStringLiteral("%1: no column %2").arg(table.path(), name));
  }
  return column;
}

double real(const Table& table, int32_t row, int32_t column)
{
  const Value& value = table.row(row).at(static_cast<std::size_t>(column));
  if (value.kind == Value::Kind::Real) {
    return value.real;
  }
  if (value.kind == Value::Kind::Integer) {
    return static_cast<double>(value.integer);
  }
  return std::nan("");
}

FeatureType typeOfTable(const QString& table, bool* ok)
{
  *ok = true;
  if (table.endsWith(QLatin1String(".pft"))) {
    return FeatureType::Point;
  }
  if (table.endsWith(QLatin1String(".lft"))) {
    return FeatureType::Line;
  }
  if (table.endsWith(QLatin1String(".aft"))) {
    return FeatureType::Area;
  }
  if (table.endsWith(QLatin1String(".tft"))) {
    return FeatureType::Text;
  }
  *ok = false;
  return FeatureType::Line;
}

bool isPrimitive(const QString& table)
{
  return table == QLatin1String("edg") || table == QLatin1String("fac") ||
         table == QLatin1String("end") || table == QLatin1String("cnd") ||
         table == QLatin1String("txt");
}

QString descriptionKey(const QString& table, const QString& column,
                       const QString& value)
{
  return table + QLatin1Char('|') + column + QLatin1Char('|') + value;
}

/**
 * Adds to @p bounds each tile's extent: the bounding rectangle (@p
 * rectangles, the tile reference coverage's fbr) of its face.
 */
void addTileBounds(const Table& tiles, int32_t idColumn, int32_t faceColumn,
                   const Table& rectangles, std::map<int32_t, Bounds>* bounds)
{
  const std::array<int32_t, 4> columns = {
      rectangles.columnIndex(QStringLiteral("xmin")),
      rectangles.columnIndex(QStringLiteral("ymin")),
      rectangles.columnIndex(QStringLiteral("xmax")),
      rectangles.columnIndex(QStringLiteral("ymax"))};
  for (int32_t row = 0; row < tiles.rowCount(); ++row) {
    const int32_t rectangle =
        rectangles.rowOfId(tiles.integer(row, faceColumn));
    if (rectangle >= 0) {
      (*bounds)[static_cast<int32_t>(tiles.integer(row, idColumn))] =
          Bounds{real(rectangles, rectangle, columns[0]),
                 real(rectangles, rectangle, columns[1]),
                 real(rectangles, rectangle, columns[2]),
                 real(rectangles, rectangle, columns[3])};
    }
  }
}

/** Adds the rows of a value description table to @p descriptions. */
void addDescriptions(const Table& table,
                     std::map<QString, QString>* descriptions)
{
  const int32_t name = table.columnIndex(QStringLiteral("table"));
  const int32_t attribute = table.columnIndex(QStringLiteral("attribute"));
  const int32_t value = table.columnIndex(QStringLiteral("value"));
  const int32_t description = table.columnIndex(QStringLiteral("description"));
  for (int32_t row = 0; row < table.rowCount(); ++row) {
    (*descriptions)[descriptionKey(
        table.text(row, name).toLower(), table.text(row, attribute).toLower(),
        table.text(row, value))] = table.text(row, description);
  }
}

} // namespace

std::optional<Database> Database::open(const QString& path,
                                       QString* errorString)
{
  Database database;
  database.path = path;
  const std::optional<Table> header =
      readTable(path, QStringLiteral("dht"), errorString);
  if (!header) {
    return std::nullopt;
  }
  if (header->rowCount() > 0) {
    database.name =
        header->text(0, header->columnIndex(QStringLiteral("database_name")));
    database.description =
        header->text(0, header->columnIndex(QStringLiteral("database_desc")));
  }
  const std::optional<Table> libraries =
      readTable(path, QStringLiteral("lat"), errorString);
  if (!libraries) {
    return std::nullopt;
  }
  const int32_t name =
      needColumn(*libraries, QStringLiteral("library_name"), errorString);
  const int32_t xmin =
      needColumn(*libraries, QStringLiteral("xmin"), errorString);
  const int32_t ymin =
      needColumn(*libraries, QStringLiteral("ymin"), errorString);
  const int32_t xmax =
      needColumn(*libraries, QStringLiteral("xmax"), errorString);
  const int32_t ymax =
      needColumn(*libraries, QStringLiteral("ymax"), errorString);
  if (name < 0 || xmin < 0 || ymin < 0 || xmax < 0 || ymax < 0) {
    return std::nullopt;
  }
  for (int32_t row = 0; row < libraries->rowCount(); ++row) {
    LibraryInfo library;
    library.name = libraries->text(row, name).toLower();
    library.bounds =
        Bounds{real(*libraries, row, xmin), real(*libraries, row, ymin),
               real(*libraries, row, xmax), real(*libraries, row, ymax)};
    database.libraries.push_back(library);
  }
  return database;
}

std::optional<Library> Library::open(const Database& database,
                                     const QString& name, QString* errorString)
{
  Library library;
  library.name = name.toLower();
  library.path = findEntry(database.path, name);
  if (library.path.isEmpty()) {
    fail(errorString,
         QStringLiteral("%1: no library %2").arg(database.path, name));
    return std::nullopt;
  }
  const std::optional<Table> coverages =
      readTable(library.path, QStringLiteral("cat"), errorString);
  if (!coverages) {
    return std::nullopt;
  }
  const int32_t coverageName =
      needColumn(*coverages, QStringLiteral("coverage_name"), errorString);
  if (coverageName < 0) {
    return std::nullopt;
  }
  const int32_t description =
      coverages->columnIndex(QStringLiteral("description"));
  const int32_t level = coverages->columnIndex(QStringLiteral("level"));
  for (int32_t row = 0; row < coverages->rowCount(); ++row) {
    CoverageInfo coverage;
    coverage.name = coverages->text(row, coverageName).toLower();
    coverage.description = coverages->text(row, description);
    coverage.level = static_cast<int32_t>(coverages->integer(row, level, 0));
    library.coverages.push_back(coverage);
  }
  // A tiled library has a tile reference coverage.
  const QString tileref = findEntry(library.path, QStringLiteral("tileref"));
  if (tileref.isEmpty()) {
    return library;
  }
  const std::optional<Table> tiles =
      readTable(tileref, QStringLiteral("tileref.aft"), errorString);
  if (!tiles) {
    return std::nullopt;
  }
  const Table& tileTable = *tiles;
  const int32_t id = needColumn(tileTable, QStringLiteral("id"), errorString);
  const int32_t tileName =
      needColumn(tileTable, QStringLiteral("tile_name"), errorString);
  if (id < 0 || tileName < 0) {
    return std::nullopt;
  }
  for (int32_t row = 0; row < tileTable.rowCount(); ++row) {
    const int32_t tile = static_cast<int32_t>(tileTable.integer(row, id));
    QString path = tileTable.text(row, tileName).trimmed();
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    library.tilePaths[tile] = path;
  }
  // The tiles' extents are their faces' bounding rectangles.
  const int32_t face = tileTable.columnIndex(QStringLiteral("fac_id"));
  if (face < 0 || findEntry(tileref, QStringLiteral("fbr")).isEmpty()) {
    return library;
  }
  const std::optional<Table> rectangles =
      readTable(tileref, QStringLiteral("fbr"), errorString);
  if (!rectangles) {
    return std::nullopt;
  }
  addTileBounds(tileTable, id, face, *rectangles, &library.tileBounds);
  return library;
}

/** The primitive tables of one tile (or of an untiled coverage). */
struct Coverage::Primitives {
  QString path;
  std::optional<Table> edges;
  std::optional<Table> faces;
  std::optional<Table> rings;
  std::optional<Table> entityNodes;
  std::optional<Table> connectedNodes;
  std::optional<Table> texts;
  bool edgesRead = false;
  bool facesRead = false;
  bool ringsRead = false;
  bool entityRead = false;
  bool connectedRead = false;
  bool textsRead = false;
};

Coverage::Coverage(Coverage&&) noexcept = default;
Coverage& Coverage::operator=(Coverage&&) noexcept = default;
Coverage::~Coverage() = default;

std::optional<Coverage> Coverage::open(const Library& library,
                                       const QString& name,
                                       QString* errorString)
{
  Coverage coverage;
  coverage.m_name = name.toLower();
  coverage.m_library = &library;
  coverage.m_tilePaths = library.tilePaths;
  coverage.m_path = findEntry(library.path, name);
  if (coverage.m_path.isEmpty()) {
    fail(errorString,
         QStringLiteral("%1: no coverage %2").arg(library.path, name));
    return std::nullopt;
  }
  const std::optional<Table> schema =
      readTable(coverage.m_path, QStringLiteral("fcs"), errorString);
  if (!schema) {
    return std::nullopt;
  }
  const int32_t featureClass =
      needColumn(*schema, QStringLiteral("feature_class"), errorString);
  const int32_t table1 =
      needColumn(*schema, QStringLiteral("table1"), errorString);
  const int32_t key1 =
      needColumn(*schema, QStringLiteral("table1_key"), errorString);
  const int32_t table2 =
      needColumn(*schema, QStringLiteral("table2"), errorString);
  if (featureClass < 0 || table1 < 0 || key1 < 0 || table2 < 0) {
    return std::nullopt;
  }
  for (int32_t row = 0; row < schema->rowCount(); ++row) {
    const QString first = schema->text(row, table1).toLower();
    const QString second = schema->text(row, table2).toLower();
    bool ok = false;
    const FeatureType type = typeOfTable(first, &ok);
    if (!ok || !isPrimitive(second)) {
      continue; // The reverse relation, or a join we don't need.
    }
    const QString className = schema->text(row, featureClass).toLower();
    if (coverage.featureClass(className) != nullptr) {
      continue;
    }
    FeatureClass entry;
    entry.name = className;
    entry.table = first;
    entry.type = type;
    entry.primitive = second;
    entry.key = schema->text(row, key1).toLower();
    if (entry.key == QLatin1String("id")) {
      entry.key.clear(); // The primitive names the feature.
    }
    coverage.m_classes.push_back(entry);
  }
  // Value descriptions, if the coverage has them.
  for (const QString& vdt :
       {QStringLiteral("char.vdt"), QStringLiteral("int.vdt")}) {
    if (findEntry(coverage.m_path, vdt).isEmpty()) {
      continue;
    }
    const std::optional<Table> descriptions =
        readTable(coverage.m_path, vdt, errorString);
    if (!descriptions) {
      return std::nullopt;
    }
    addDescriptions(*descriptions, &coverage.m_descriptions);
  }
  return coverage;
}

const FeatureClass* Coverage::featureClass(const QString& name) const
{
  for (const FeatureClass& entry : m_classes) {
    if (entry.name == name) {
      return &entry;
    }
  }
  return nullptr;
}

const Table* Coverage::table(const QString& table, QString* errorString)
{
  const std::map<QString, std::unique_ptr<Table>>::const_iterator found =
      m_tables.find(table);
  if (found != m_tables.end()) {
    return found->second.get();
  }
  std::optional<Table> read = readTable(m_path, table, errorString);
  if (!read) {
    return nullptr;
  }
  std::unique_ptr<Table>& slot = m_tables[table];
  slot = std::make_unique<Table>(std::move(*read));
  return slot.get();
}

QString Coverage::valueDescription(const QString& table, const QString& column,
                                   const QString& value) const
{
  const std::map<QString, QString>::const_iterator found =
      m_descriptions.find(descriptionKey(table, column, value));
  return found == m_descriptions.end() ? QString() : found->second;
}

Coverage::Primitives* Coverage::primitives(int32_t tile, QString* errorString)
{
  const std::map<int32_t, std::unique_ptr<Primitives>>::const_iterator found =
      m_primitives.find(tile);
  if (found != m_primitives.end()) {
    return found->second.get();
  }
  std::unique_ptr<Primitives> primitives = std::make_unique<Primitives>();
  if (tile <= 0 || m_tilePaths.empty()) {
    primitives->path = m_path;
  } else {
    const std::map<int32_t, QString>::const_iterator path =
        m_tilePaths.find(tile);
    if (path == m_tilePaths.end()) {
      fail(errorString, QStringLiteral("%1: no tile %2").arg(m_path).arg(tile));
      return nullptr;
    }
    QString directory = m_path;
    for (const QString& part : path->second.split(QLatin1Char('/'))) {
      directory = findEntry(directory, part);
      if (directory.isEmpty()) {
        break;
      }
    }
    // A tile without data in this coverage has no directory.
    primitives->path = directory;
  }
  Primitives* result = primitives.get();
  // Keep a few tiles: features come grouped by tile, so older ones are
  // rarely needed again.
  if (m_primitives.size() > 8) {
    m_primitives.clear();
  }
  m_primitives[tile] = std::move(primitives);
  return result;
}

namespace
{

/** Reads a primitive table once; a missing table is no error. */
bool loadOnce(const QString& directory, const QString& name, bool* read,
              std::optional<Table>* table, QString* errorString)
{
  if (*read) {
    return true;
  }
  *read = true;
  if (directory.isEmpty() || findEntry(directory, name).isEmpty()) {
    return true;
  }
  *table = readTable(directory, name, errorString);
  return table->has_value();
}

/** Returns the table in @p table, or null. */
const Table* tableOf(const std::optional<Table>& table)
{
  return table.has_value() ? &table.value() : nullptr;
}

/** The winged-edge columns of an edge table. */
struct EdgeColumns {
  int32_t startNode = -1;
  int32_t endNode = -1;
  int32_t rightFace = -1;
  int32_t leftFace = -1;
  int32_t rightEdge = -1;
  int32_t leftEdge = -1;
  int32_t coordinates = -1;

  explicit EdgeColumns(const Table& edges)
      : startNode(edges.columnIndex(QStringLiteral("start_node"))),
        endNode(edges.columnIndex(QStringLiteral("end_node"))),
        rightFace(edges.columnIndex(QStringLiteral("right_face"))),
        leftFace(edges.columnIndex(QStringLiteral("left_face"))),
        rightEdge(edges.columnIndex(QStringLiteral("right_edge"))),
        leftEdge(edges.columnIndex(QStringLiteral("left_edge"))),
        coordinates(edges.columnIndex(QStringLiteral("coordinates")))
  {
  }

  bool valid() const
  {
    return startNode >= 0 && endNode >= 0 && rightFace >= 0 && leftFace >= 0 &&
           rightEdge >= 0 && leftEdge >= 0 && coordinates >= 0;
  }
};

/** Appends @p points to @p ring, walked forward or backward, joined. */
void appendEdge(const std::vector<Coordinate>& points, bool forward,
                std::vector<Coordinate>* ring)
{
  const std::size_t skip = ring->empty() ? 0 : 1;
  if (forward) {
    ring->insert(ring->end(),
                 points.begin() + static_cast<std::ptrdiff_t>(skip),
                 points.end());
  } else {
    ring->insert(ring->end(),
                 points.rbegin() + static_cast<std::ptrdiff_t>(skip),
                 points.rend());
  }
}

/** Returns true if edge row @p row has face @p face on a side. */
bool bounds(const Table& edges, const EdgeColumns& columns, int32_t row,
            int32_t face)
{
  return edges.triplet(row, columns.rightFace).id == face ||
         edges.triplet(row, columns.leftFace).id == face;
}

/**
 * Returns true to walk edge row @p row forward when reached at node
 * @p node: from its start node forward, from its end node backward; a loop
 * edge (or one not at the node) by the side @p face is on.
 */
// Row, node, face: the edge, where it is reached, around what.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool walkForward(const Table& edges, const EdgeColumns& columns, int32_t row,
                 int64_t node, int32_t face)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  const bool startsHere = edges.integer(row, columns.startNode) == node;
  const bool endsHere = edges.integer(row, columns.endNode) == node;
  if (startsHere != endsHere) {
    return startsHere;
  }
  return edges.triplet(row, columns.rightFace).id == face;
}

/**
 * Walks the ring of @p face from edge @p startEdge along the winged-edge
 * topology into @p ring (closed): forward along an edge with the face on
 * its right, then on to its right edge from its end node; backward along
 * one with the face on its left, then on to its left edge from its start
 * node. An edge with the face on both sides (a dangle) is walked both
 * ways. False with @p problem if the ring is broken.
 */
bool buildRing(const Table& edges, int32_t face, int32_t startEdge,
               std::vector<Coordinate>* ring, QString* problem)
{
  const EdgeColumns columns(edges);
  if (!columns.valid()) {
    *problem = QStringLiteral("%1: not a winged-edge table").arg(edges.path());
    return false;
  }
  int32_t row = edges.rowOfId(startEdge);
  if (row < 0 || !bounds(edges, columns, row, face)) {
    *problem = QStringLiteral("%1: edge %2 does not bound face %3")
                   .arg(edges.path())
                   .arg(startEdge)
                   .arg(face);
    return false;
  }
  bool forward = edges.triplet(row, columns.rightFace).id == face;
  const int32_t firstRow = row;
  const bool firstForward = forward;
  const int32_t limit = 2 * edges.rowCount() + 2;
  for (int32_t steps = 0; steps <= limit; ++steps) {
    const std::vector<Coordinate>& points =
        edges.coordinates(row, columns.coordinates);
    if (points.empty()) {
      *problem = QStringLiteral("%1: edge row %2 has no coordinates")
                     .arg(edges.path())
                     .arg(row + 1);
      return false;
    }
    appendEdge(points, forward, ring);
    const int64_t node =
        edges.integer(row, forward ? columns.endNode : columns.startNode);
    const int32_t next =
        edges.triplet(row, forward ? columns.rightEdge : columns.leftEdge).id;
    const int32_t nextRow = edges.rowOfId(next);
    if (nextRow < 0 || !bounds(edges, columns, nextRow, face)) {
      *problem = QStringLiteral("%1: edge %2 leads to edge %3, which does not "
                                "bound face %4")
                     .arg(edges.path())
                     .arg(edges.integer(row, 0))
                     .arg(next)
                     .arg(face);
      return false;
    }
    const bool nextForward = walkForward(edges, columns, nextRow, node, face);
    if (nextRow == firstRow && nextForward == firstForward) {
      if (ring->size() < 3) {
        *problem = QStringLiteral("%1: the ring of face %2 has %3 points")
                       .arg(edges.path())
                       .arg(face)
                       .arg(ring->size());
        return false;
      }
      if (ring->front() != ring->back()) {
        ring->push_back(ring->front());
      }
      return true;
    }
    row = nextRow;
    forward = nextForward;
  }
  *problem = QStringLiteral("%1: the ring of face %2 does not close")
                 .arg(edges.path())
                 .arg(face);
  return false;
}

/** Adds face @p face's rings (outer first) to @p feature. */
bool buildFace(const Table& faces, const Table& rings, const Table& edges,
               int32_t face, Feature* feature, QString* problem)
{
  const int32_t faceRow = faces.rowOfId(face);
  const int32_t ringPointer = faces.columnIndex(QStringLiteral("ring_ptr"));
  const int32_t ringFace = rings.columnIndex(QStringLiteral("face_id"));
  const int32_t startEdge = rings.columnIndex(QStringLiteral("start_edge"));
  int32_t ringRow = faceRow < 0 || ringPointer < 0
                        ? -1
                        : rings.rowOfId(faces.integer(faceRow, ringPointer));
  if (ringRow < 0 || ringFace < 0 || startEdge < 0) {
    *problem =
        QStringLiteral("%1: face %2 has no ring").arg(faces.path()).arg(face);
    return false;
  }
  // The face's rings follow each other from its ring pointer.
  for (; ringRow < rings.rowCount() && rings.integer(ringRow, ringFace) == face;
       ++ringRow) {
    std::vector<Coordinate> ring;
    if (!buildRing(edges, face,
                   static_cast<int32_t>(rings.integer(ringRow, startEdge)),
                   &ring, problem)) {
      return false;
    }
    feature->parts.push_back(std::move(ring));
  }
  return !feature->parts.empty();
}

/**
 * Returns the ids of the rows of @p primitives whose column "<feature
 * table>_id" names feature @p id: for classes whose primitives name their
 * feature instead of the other way round.
 */
std::vector<int64_t> primitivesNaming(const Table& primitives,
                                      const QString& featureTable, int64_t id)
{
  std::vector<int64_t> keys;
  const int32_t column =
      primitives.columnIndex(featureTable + QStringLiteral("_id"));
  for (int32_t row = 0; column >= 0 && row < primitives.rowCount(); ++row) {
    if (primitives.integer(row, column) == id) {
      keys.push_back(primitives.integer(row, 0));
    }
  }
  return keys;
}

/** A line feature from its edges. */
bool buildLine(const Table& edges, const std::vector<int64_t>& keys,
               Feature* feature, QString* problem)
{
  const int32_t coordinates = edges.columnIndex(QStringLiteral("coordinates"));
  for (const int64_t key : keys) {
    const int32_t edge = edges.rowOfId(key);
    if (edge < 0) {
      *problem = QStringLiteral("%1: no edge %2").arg(edges.path()).arg(key);
      return false;
    }
    feature->parts.push_back(edges.coordinates(edge, coordinates));
  }
  if (feature->parts.empty() || feature->parts.front().size() < 2) {
    *problem = QStringLiteral("%1: line feature %2 has no edge")
                   .arg(edges.path())
                   .arg(feature->id);
    return false;
  }
  return true;
}

/**
 * The first coordinate of @p column of the row with the first of @p keys
 * in @p table (a node or a text): empty if there is none.
 */
std::vector<Coordinate> firstShape(const Table& table, const QString& column,
                                   const std::vector<int64_t>& keys,
                                   int32_t* row)
{
  *row = keys.empty() ? -1 : table.rowOfId(keys.front());
  return table.coordinates(*row, table.columnIndex(column));
}

} // namespace

// The feature's problem (data), then the read error (files).
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool Coverage::buildLineFeature(Primitives& primitives,
                                const FeatureClass& featureClass,
                                std::vector<int64_t> keys, Feature* feature,
                                QString* problem, QString* errorString)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  const QString& directory = primitives.path;
  if (!loadOnce(directory, QStringLiteral("edg"), &primitives.edgesRead,
                &primitives.edges, errorString)) {
    return false;
  }
  const Table* edges = tableOf(primitives.edges);
  if (edges == nullptr) {
    *problem = QStringLiteral("%1: no edge table").arg(directory);
    return true;
  }
  if (keys.empty()) {
    keys = primitivesNaming(*edges, featureClass.table, feature->id);
  }
  buildLine(*edges, keys, feature, problem);
  return true;
}

// The feature's problem (data), then the read error (files).
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool Coverage::buildAreaFeature(Primitives& primitives,
                                const FeatureClass& featureClass,
                                std::vector<int64_t> keys, Feature* feature,
                                QString* problem, QString* errorString)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  const QString& directory = primitives.path;
  const QString& name = featureClass.table;
  if (!loadOnce(directory, QStringLiteral("edg"), &primitives.edgesRead,
                &primitives.edges, errorString) ||
      !loadOnce(directory, QStringLiteral("fac"), &primitives.facesRead,
                &primitives.faces, errorString) ||
      !loadOnce(directory, QStringLiteral("rng"), &primitives.ringsRead,
                &primitives.rings, errorString)) {
    return false;
  }
  const Table* edges = tableOf(primitives.edges);
  const Table* faces = tableOf(primitives.faces);
  const Table* rings = tableOf(primitives.rings);
  if (edges == nullptr || faces == nullptr || rings == nullptr) {
    *problem = QStringLiteral("%1: no face, ring or edge table").arg(directory);
    return true;
  }
  if (keys.empty()) {
    keys = primitivesNaming(*faces, name, feature->id);
  }
  if (keys.empty()) {
    *problem = QStringLiteral("%1: area feature %2 has no face")
                   .arg(name)
                   .arg(feature->id);
  }
  for (const int64_t key : keys) {
    if (!buildFace(*faces, *rings, *edges, static_cast<int32_t>(key), feature,
                   problem)) {
      feature->parts.clear();
      break;
    }
  }
  return true;
}

// The feature's problem (data), then the read error (files).
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool Coverage::buildNodeFeature(Primitives& primitives,
                                const FeatureClass& featureClass,
                                std::vector<int64_t> keys, Feature* feature,
                                QString* problem, QString* errorString)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  const QString& directory = primitives.path;
  const QString& name = featureClass.table;
  const bool text = featureClass.type == FeatureType::Text;
  const bool entity = featureClass.primitive == QLatin1String("end");
  bool* read = &primitives.connectedRead;
  std::optional<Table>* table = &primitives.connectedNodes;
  if (text) {
    read = &primitives.textsRead;
    table = &primitives.texts;
  } else if (entity) {
    read = &primitives.entityRead;
    table = &primitives.entityNodes;
  }
  if (!loadOnce(directory, featureClass.primitive, read, table, errorString)) {
    return false;
  }
  const Table* shapes = tableOf(*table);
  if (shapes == nullptr) {
    *problem = QStringLiteral("%1: no %2 table")
                   .arg(directory, featureClass.primitive);
    return true;
  }
  if (keys.empty()) {
    keys = primitivesNaming(*shapes, name, feature->id);
  }
  int32_t row = -1;
  std::vector<Coordinate> shape = firstShape(
      *shapes,
      text ? QStringLiteral("shape_line") : QStringLiteral("coordinate"), keys,
      &row);
  if (shape.empty()) {
    *problem = QStringLiteral("%1: feature %2 has no %3")
                   .arg(name)
                   .arg(feature->id)
                   .arg(featureClass.primitive);
    return true;
  }
  if (text) {
    feature->text =
        shapes->text(row, shapes->columnIndex(QStringLiteral("string")));
  } else {
    shape.resize(1);
  }
  feature->parts.push_back(std::move(shape));
  return true;
}

// The feature's problem (data), then the read error (files).
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool Coverage::buildFeature(Primitives& primitives,
                            const FeatureClass& featureClass,
                            std::vector<int64_t> keys, Feature* feature,
                            QString* problem, QString* errorString)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  switch (featureClass.type) {
  case FeatureType::Line:
    return buildLineFeature(primitives, featureClass, std::move(keys), feature,
                            problem, errorString);
  case FeatureType::Area:
    return buildAreaFeature(primitives, featureClass, std::move(keys), feature,
                            problem, errorString);
  case FeatureType::Point:
  case FeatureType::Text:
    break;
  }
  return buildNodeFeature(primitives, featureClass, std::move(keys), feature,
                          problem, errorString);
}

bool Coverage::readFeatures(const FeatureClass& featureClass,
                            std::vector<Feature>* features, ReadStats* stats,
                            QString* errorString)
{
  const Table* table = this->table(featureClass.table, errorString);
  if (table == nullptr) {
    return false;
  }
  const int32_t idColumn = table->columnIndex(QStringLiteral("id"));
  const int32_t tileColumn = table->columnIndex(QStringLiteral("tile_id"));
  // Without a key column the primitives name their feature
  // ("<table>_id" in the primitive table).
  const int32_t keyColumn =
      featureClass.key.isEmpty() ? -1 : table->columnIndex(featureClass.key);
  if (!featureClass.key.isEmpty() && keyColumn < 0) {
    return fail(errorString, QStringLiteral("%1: no column %2")
                                 .arg(table->path(), featureClass.key));
  }
  for (int32_t row = 0; row < table->rowCount(); ++row) {
    Feature feature;
    feature.row = row;
    feature.id = static_cast<int32_t>(table->integer(row, idColumn, row + 1));
    feature.tile = static_cast<int32_t>(table->integer(row, tileColumn, 0));
    feature.type = featureClass.type;
    stats->features += 1;
    Primitives* primitives = this->primitives(feature.tile, errorString);
    if (primitives == nullptr) {
      return false;
    }
    std::vector<int64_t> keys;
    if (keyColumn >= 0) {
      keys.push_back(table->integer(row, keyColumn));
    }
    QString problem;
    if (!buildFeature(*primitives, featureClass, keys, &feature, &problem,
                      errorString)) {
      return false;
    }
    if (feature.parts.empty()) {
      stats->skipped += 1;
      if (stats->problems.size() < 20) {
        stats->problems.push_back(problem);
      }
      continue;
    }
    features->push_back(std::move(feature));
  }
  return true;
}

} // namespace ts::packager::vpf
