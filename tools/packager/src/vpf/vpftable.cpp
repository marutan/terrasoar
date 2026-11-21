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

#include "vpftable.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cpl_conv.h>
#include <cpl_string.h>
#include <cpl_vsi.h>

#include "ts/deps/dependencies.h"

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

/** Reads numbers from a byte array with bounds checks. */
class Reader
{
public:
  Reader(const QByteArray& bytes, bool bigEndian)
      : m_data(reinterpret_cast<const unsigned char*>(bytes.constData())),
        m_size(static_cast<std::size_t>(bytes.size())), m_bigEndian(bigEndian)
  {
  }

  std::size_t position() const { return m_pos; }
  void seek(std::size_t position) { m_pos = position; }
  bool atEnd() const { return m_pos >= m_size; }
  bool ok() const { return m_ok; }
  void setBigEndian(bool bigEndian) { m_bigEndian = bigEndian; }

  bool need(std::size_t bytes)
  {
    m_ok = m_ok && m_pos <= m_size && bytes <= m_size - m_pos;
    return m_ok;
  }

  uint64_t unsignedBytes(std::size_t bytes)
  {
    if (!need(bytes)) {
      return 0;
    }
    uint64_t value = 0;
    for (std::size_t i = 0; i < bytes; ++i) {
      const std::size_t index = m_pos + (m_bigEndian ? i : bytes - 1 - i);
      value = (value << 8U) | m_data[index];
    }
    m_pos += bytes;
    return value;
  }

  int16_t int16()
  {
    return static_cast<int16_t>(static_cast<uint16_t>(unsignedBytes(2)));
  }
  int32_t int32()
  {
    return static_cast<int32_t>(static_cast<uint32_t>(unsignedBytes(4)));
  }
  uint8_t byte() { return static_cast<uint8_t>(unsignedBytes(1)); }

  double float32()
  {
    const uint32_t bits = static_cast<uint32_t>(unsignedBytes(4));
    float value = 0.0F;
    std::memcpy(&value, &bits, sizeof value);
    return static_cast<double>(value);
  }

  double float64()
  {
    const uint64_t bits = unsignedBytes(8);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof value);
    return value;
  }

  QString latin1(std::size_t bytes)
  {
    if (!need(bytes)) {
      return QString();
    }
    const char* start = reinterpret_cast<const char*>(m_data + m_pos);
    std::size_t length = bytes;
    // Trailing blanks and NULs pad fixed-width text.
    while (length > 0 &&
           (start[length - 1] == ' ' || start[length - 1] == '\0')) {
      --length;
    }
    m_pos += bytes;
    return QString::fromLatin1(start, static_cast<int32_t>(length));
  }

private:
  const unsigned char* m_data;
  std::size_t m_size;
  std::size_t m_pos = 0;
  bool m_bigEndian;
  bool m_ok = true;
};

/** Returns the bytes of one element of a numeric or coordinate type. */
std::size_t elementBytes(char type)
{
  switch (type) {
  case 'S':
    return 2;
  case 'I':
  case 'F':
    return 4;
  case 'R':
  case 'C':
    return 8;
  case 'B':
    return 16;
  case 'Z':
    return 12;
  case 'Y':
    return 24;
  case 'D':
    return 20;
  default:
    return 1;
  }
}

bool isKnownType(char type)
{
  return type != '\0' && QByteArrayLiteral("TLNMFRSIDKCBZYX").contains(type);
}

/** Splits @p text at @p separator, outside nothing (VPF has no quoting). */
QStringList split(const QString& text, QChar separator)
{
  return text.split(separator);
}

/** Parses one column definition, "name=T,1,N,desc,vdt,index,narrative". */
std::optional<Column> parseColumn(const QString& text)
{
  const int32_t equals = static_cast<int32_t>(text.indexOf(QLatin1Char('=')));
  if (equals <= 0) {
    return std::nullopt;
  }
  Column column;
  column.name = text.left(equals).trimmed().toLower();
  const QStringList parts = split(text.mid(equals + 1), QLatin1Char(','));
  if (parts.size() < 2 || parts.at(0).trimmed().size() != 1) {
    return std::nullopt;
  }
  column.type = parts.at(0).trimmed().at(0).toUpper().toLatin1();
  if (!isKnownType(column.type)) {
    return std::nullopt;
  }
  const QString count = parts.at(1).trimmed();
  if (count == QLatin1String("*")) {
    column.count = -1;
  } else {
    bool ok = false;
    column.count = count.toInt(&ok);
    if (!ok || column.count < 1) {
      return std::nullopt;
    }
  }
  const std::function<QString(int32_t)> part = [&parts](int32_t index) {
    if (index >= parts.size()) {
      return QString();
    }
    const QString value = parts.at(index).trimmed();
    return value == QLatin1String("-") ? QString() : value;
  };
  const QString key = part(2);
  column.keyType = key.isEmpty() ? 'N' : key.at(0).toUpper().toLatin1();
  column.description = part(3);
  column.valueTable = part(4).toLower();
  column.thematicIndex = part(5).toLower();
  column.narrative = part(6).toLower();
  return column;
}

/** Reads a triplet id: a type byte, then up to three ids of 0–4 bytes. */
TripletId readTriplet(Reader& reader)
{
  const uint8_t type = reader.byte();
  const std::function<int32_t(uint32_t)> part = [&reader](uint32_t bits) {
    switch (bits & 3U) {
    case 1:
      return static_cast<int32_t>(reader.unsignedBytes(1));
    case 2:
      return static_cast<int32_t>(reader.unsignedBytes(2));
    case 3:
      return reader.int32();
    default:
      return -1;
    }
  };
  TripletId triplet;
  triplet.id = part(static_cast<uint32_t>(type) >> 6U);
  triplet.tile = part(static_cast<uint32_t>(type) >> 4U);
  triplet.external = part(static_cast<uint32_t>(type) >> 2U);
  return triplet;
}

/** Reads @p elements coordinates of a C, B, Z or Y column. */
// The type letter, then the count, as a column defines them.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
std::vector<Coordinate> readCoordinates(Reader& reader, char type,
                                        std::size_t elements)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  std::vector<Coordinate> coordinates;
  coordinates.reserve(elements);
  const bool isDouble = type == 'B' || type == 'Y';
  const bool is3d = type == 'Z' || type == 'Y';
  for (std::size_t i = 0; i < elements; ++i) {
    Coordinate coordinate;
    coordinate.x = isDouble ? reader.float64() : reader.float32();
    coordinate.y = isDouble ? reader.float64() : reader.float32();
    if (is3d) {
      coordinate.z = isDouble ? reader.float64() : reader.float32();
    }
    coordinates.push_back(coordinate);
  }
  return coordinates;
}

/** Reads one element of an S, I, F or R column. */
double readNumber(Reader& reader, char type)
{
  switch (type) {
  case 'S':
    return reader.int16();
  case 'I':
    return reader.int32();
  case 'F':
    return reader.float32();
  default:
    return reader.float64();
  }
}

/** Reads @p elements numbers of an S, I, F or R @p column into @p value. */
void readNumbers(Reader& reader, const Column& column, std::size_t elements,
                 Value* value)
{
  std::vector<double> numbers;
  numbers.reserve(elements);
  for (std::size_t i = 0; i < elements; ++i) {
    numbers.push_back(readNumber(reader, column.type));
  }
  if (elements != 1) {
    value->kind = Value::Kind::Numbers;
    value->numbers = std::move(numbers);
  } else if (column.isInteger()) {
    value->kind = Value::Kind::Integer;
    value->integer = static_cast<int64_t>(numbers.front());
  } else if (std::isnan(numbers.front())) {
    value->kind = Value::Kind::Null;
  } else {
    value->kind = Value::Kind::Real;
    value->real = numbers.front();
  }
}

/** Reads one value of @p column. */
Value readValue(Reader& reader, const Column& column)
{
  Value value;
  int64_t count = column.count;
  if (column.isVariable()) {
    count = reader.int32();
    if (count < 0 || !reader.ok()) {
      reader.need(std::numeric_limits<std::size_t>::max());
      return value;
    }
  }
  const std::size_t elements = static_cast<std::size_t>(count);
  if (column.type != 'K' && column.type != 'X' &&
      !reader.need(elements * elementBytes(column.type))) {
    return value;
  }
  switch (column.type) {
  case 'T':
  case 'L':
  case 'N':
  case 'M':
  case 'D':
    value.kind = Value::Kind::Text;
    value.text = reader.latin1(column.type == 'D' ? elements * 20 : elements);
    break;
  case 'X':
    break;
  case 'K':
    value.kind = Value::Kind::Triplet;
    value.triplet = readTriplet(reader);
    break;
  case 'C':
  case 'B':
  case 'Z':
  case 'Y':
    value.kind = Value::Kind::Coordinates;
    value.coordinates = readCoordinates(reader, column.type, elements);
    break;
  default: // S, I, F, R
    readNumbers(reader, column, elements, &value);
    break;
  }
  return value;
}

} // namespace

bool Column::isText() const
{
  return type == 'T' || type == 'L' || type == 'N' || type == 'M';
}

bool Column::isCoordinates() const
{
  return type == 'C' || type == 'B' || type == 'Z' || type == 'Y';
}

std::optional<QByteArray> readFile(const QString& path, QString* errorString)
{
  initialiseGdal();
  const QByteArray encoded = path.toUtf8();
  VSILFILE* file = VSIFOpenL(encoded.constData(), "rb");
  if (file == nullptr) {
    fail(errorString, QStringLiteral("%1: cannot open").arg(path));
    return std::nullopt;
  }
  QByteArray bytes;
  std::vector<char> buffer(std::size_t{1} << 16);
  for (;;) {
    const std::size_t read = VSIFReadL(buffer.data(), 1, buffer.size(), file);
    bytes.append(buffer.data(), static_cast<int32_t>(read));
    if (read < buffer.size()) {
      break;
    }
  }
  VSIFCloseL(file);
  return bytes;
}

// The directory comes first, as in a path.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
QString findEntry(const QString& directory, const QString& name)
{
  static std::mutex mutex;
  static std::map<QString, QStringList> listings;
  QStringList entries;
  {
    const std::lock_guard<std::mutex> lock(mutex);
    const std::map<QString, QStringList>::const_iterator found =
        listings.find(directory);
    if (found != listings.end()) {
      entries = found->second;
    } else {
      initialiseGdal();
      const QByteArray encoded = directory.toUtf8();
      char** list = VSIReadDir(encoded.constData());
      for (char** entry = list; entry != nullptr && *entry != nullptr;
           ++entry) {
        entries.append(QString::fromUtf8(*entry));
      }
      CSLDestroy(list);
      listings.emplace(directory, entries);
    }
  }
  for (const QString& entry : std::as_const(entries)) {
    if (entry.compare(name, Qt::CaseInsensitive) == 0) {
      return directory + QLatin1Char('/') + entry;
    }
  }
  return QString();
}

std::optional<Table> Table::read(const QString& path, QString* errorString)
{
  const std::optional<QByteArray> bytes = readFile(path, errorString);
  if (!bytes) {
    return std::nullopt;
  }
  return parse(*bytes, path, errorString);
}

std::optional<Table> Table::parse(const QByteArray& bytes, const QString& path,
                                  QString* errorString)
{
  Table table;
  table.m_path = path;
  if (bytes.size() < 4) {
    fail(errorString, QStringLiteral("%1: too short for a header").arg(path));
    return std::nullopt;
  }
  // The header length is little-endian unless the header says "M"; the
  // byte order letter comes right after it.
  const bool bigEndian =
      bytes.size() > 4 && (bytes.at(4) == 'M' || bytes.at(4) == 'm');
  Reader reader(bytes, bigEndian);
  const int32_t headerLength = reader.int32();
  if (headerLength <= 0 || headerLength > bytes.size() - 4) {
    fail(
        errorString,
        QStringLiteral("%1: bad header length %2").arg(path).arg(headerLength));
    return std::nullopt;
  }
  table.m_bigEndian = bigEndian;
  QString header =
      QString::fromLatin1(bytes.constData() + 4, headerLength).trimmed();
  if (header.size() >= 2 && header.at(1) == QLatin1Char(';') &&
      (header.at(0).toUpper() == QLatin1Char('L') ||
       header.at(0).toUpper() == QLatin1Char('M'))) {
    header = header.mid(2);
  }
  // description;narrative;columns:...:;
  const int32_t first = static_cast<int32_t>(header.indexOf(QLatin1Char(';')));
  const int32_t second =
      first < 0
          ? -1
          : static_cast<int32_t>(header.indexOf(QLatin1Char(';'), first + 1));
  if (first < 0 || second < 0) {
    fail(errorString, QStringLiteral("%1: malformed header").arg(path));
    return std::nullopt;
  }
  table.m_description = header.left(first).trimmed();
  table.m_narrative = header.mid(first + 1, second - first - 1).trimmed();
  QString columns = header.mid(second + 1).trimmed();
  if (columns.endsWith(QLatin1Char(';'))) {
    columns.chop(1);
  }
  for (const QString& text : split(columns, QLatin1Char(':'))) {
    if (text.trimmed().isEmpty()) {
      continue;
    }
    const std::optional<Column> column = parseColumn(text);
    if (!column) {
      fail(errorString,
           QStringLiteral("%1: bad column definition \"%2\"").arg(path, text));
      return std::nullopt;
    }
    table.m_columns.push_back(*column);
  }
  if (table.m_columns.empty()) {
    fail(errorString, QStringLiteral("%1: no columns").arg(path));
    return std::nullopt;
  }
  reader.seek(static_cast<std::size_t>(headerLength) + 4);
  while (!reader.atEnd()) {
    table.m_offsets.push_back(static_cast<uint32_t>(reader.position()));
    std::vector<Value> row;
    row.reserve(table.m_columns.size());
    for (const Column& column : table.m_columns) {
      row.push_back(readValue(reader, column));
      if (!reader.ok()) {
        fail(errorString,
             QStringLiteral("%1: row %2 is truncated or malformed (column %3)")
                 .arg(path)
                 .arg(table.m_rows.size() + 1)
                 .arg(column.name));
        return std::nullopt;
      }
    }
    table.m_rows.push_back(std::move(row));
  }
  return table;
}

int32_t Table::columnIndex(const QString& name) const
{
  for (std::size_t i = 0; i < m_columns.size(); ++i) {
    if (m_columns[i].name.compare(name, Qt::CaseInsensitive) == 0) {
      return static_cast<int32_t>(i);
    }
  }
  return -1;
}

int32_t Table::rowOfId(int64_t id) const
{
  const int32_t idColumn = columnIndex(QStringLiteral("id"));
  if (idColumn < 0) {
    return id >= 1 && id <= rowCount() ? static_cast<int32_t>(id - 1) : -1;
  }
  if (id >= 1 && id <= rowCount() &&
      integer(static_cast<int32_t>(id - 1), idColumn) == id) {
    return static_cast<int32_t>(id - 1);
  }
  for (int32_t row = 0; row < rowCount(); ++row) {
    if (integer(row, idColumn) == id) {
      return row;
    }
  }
  return -1;
}

QString Table::text(int32_t row, int32_t column) const
{
  if (row < 0 || row >= rowCount() || column < 0 ||
      column >= static_cast<int32_t>(m_columns.size())) {
    return QString();
  }
  const Value& value =
      m_rows[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)];
  switch (value.kind) {
  case Value::Kind::Text:
    return value.text;
  case Value::Kind::Integer:
    return QString::number(value.integer);
  case Value::Kind::Real:
    return QString::number(value.real, 'g', 10);
  default:
    return QString();
  }
}

// Row, then column, as everywhere in Table.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
int64_t Table::integer(int32_t row, int32_t column, int64_t fallback) const
{
  if (row < 0 || row >= rowCount() || column < 0 ||
      column >= static_cast<int32_t>(m_columns.size())) {
    return fallback;
  }
  const Value& value =
      m_rows[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)];
  switch (value.kind) {
  case Value::Kind::Integer:
    return value.integer;
  case Value::Kind::Triplet:
    return value.triplet.id;
  case Value::Kind::Real:
    return static_cast<int64_t>(std::llround(value.real));
  default:
    return fallback;
  }
}

TripletId Table::triplet(int32_t row, int32_t column) const
{
  if (row < 0 || row >= rowCount() || column < 0 ||
      column >= static_cast<int32_t>(m_columns.size())) {
    return TripletId();
  }
  const Value& value =
      m_rows[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)];
  if (value.kind == Value::Kind::Triplet) {
    return value.triplet;
  }
  if (value.kind == Value::Kind::Integer) {
    TripletId triplet;
    triplet.id = static_cast<int32_t>(value.integer);
    return triplet;
  }
  return TripletId();
}

const std::vector<Coordinate>& Table::coordinates(int32_t row,
                                                  int32_t column) const
{
  static const std::vector<Coordinate> none;
  if (row < 0 || row >= rowCount() || column < 0 ||
      column >= static_cast<int32_t>(m_columns.size())) {
    return none;
  }
  const Value& value =
      m_rows[static_cast<std::size_t>(row)][static_cast<std::size_t>(column)];
  return value.kind == Value::Kind::Coordinates ? value.coordinates : none;
}

bool Table::checkIndex(const QString& indexPath, QString* errorString) const
{
  const std::optional<QByteArray> bytes = readFile(indexPath, errorString);
  if (!bytes) {
    return false;
  }
  Reader reader(*bytes, m_bigEndian);
  const int32_t entries = reader.int32();
  reader.int32(); // The table's header length.
  if (!reader.ok() || entries != rowCount()) {
    return fail(errorString, QStringLiteral("%1: %2 entries for %3 rows")
                                 .arg(indexPath)
                                 .arg(entries)
                                 .arg(rowCount()));
  }
  for (int32_t i = 0; i < entries; ++i) {
    const uint32_t offset = static_cast<uint32_t>(reader.int32());
    reader.int32(); // The row's size.
    if (!reader.ok() || offset != m_offsets[static_cast<std::size_t>(i)]) {
      return fail(errorString,
                  QStringLiteral("%1: entry %2 does not match row %2")
                      .arg(indexPath)
                      .arg(i + 1));
    }
  }
  return true;
}

std::optional<ThematicIndex> ThematicIndex::read(const QString& path,
                                                 QString* errorString)
{
  const std::optional<QByteArray> bytes = readFile(path, errorString);
  if (!bytes) {
    return std::nullopt;
  }
  return parse(*bytes, path, errorString);
}

std::optional<ThematicIndex> ThematicIndex::parse(const QByteArray& bytes,
                                                  const QString& path,
                                                  QString* errorString)
{
  // The header: length, directory entries, rows indexed, index type (I,
  // an inverted list), field type, elements per value, row id type (S or
  // I), table name (12), column name (25), 4 spare; 60 bytes.
  Reader reader(bytes, false);
  const int32_t headerLength = reader.int32();
  const int32_t directoryEntries = reader.int32();
  ThematicIndex index;
  index.m_featureCount = reader.int32();
  const char indexType = static_cast<char>(reader.byte());
  const char fieldType = static_cast<char>(reader.byte());
  const int32_t elements = reader.int32();
  const char idType = static_cast<char>(reader.byte());
  index.m_table = reader.latin1(12).toLower();
  index.m_column = reader.latin1(25).toLower();
  reader.latin1(4);
  if (!reader.ok() || headerLength < 60 || headerLength > bytes.size() ||
      directoryEntries < 0 || elements < 1 ||
      (idType != 'S' && idType != 'I')) {
    fail(errorString,
         QStringLiteral("%1: malformed thematic index header").arg(path));
    return std::nullopt;
  }
  if (indexType != 'I' && indexType != 'T') {
    fail(errorString, QStringLiteral("%1: index type %2 is not supported")
                          .arg(path, QString(QLatin1Char(indexType))));
    return std::nullopt;
  }
  Column field;
  field.type = fieldType;
  field.count = elements;
  if (!isKnownType(fieldType) || field.isCoordinates() || fieldType == 'K') {
    fail(errorString, QStringLiteral("%1: bad field type").arg(path));
    return std::nullopt;
  }
  for (int32_t i = 0; i < directoryEntries; ++i) {
    Entry entry;
    const Value value = readValue(reader, field);
    switch (value.kind) {
    case Value::Kind::Text:
      entry.value = value.text;
      break;
    case Value::Kind::Integer:
      entry.value = QString::number(value.integer);
      break;
    case Value::Kind::Real:
      entry.value = QString::number(value.real, 'g', 10);
      break;
    default:
      break;
    }
    const int32_t offset = reader.int32();
    const int32_t count = reader.int32();
    if (!reader.ok() || offset < 0 || count < 0) {
      fail(errorString, QStringLiteral("%1: directory entry %2 is malformed")
                            .arg(path)
                            .arg(i + 1));
      return std::nullopt;
    }
    Reader rows(bytes, false);
    rows.seek(static_cast<std::size_t>(offset));
    entry.rows.reserve(static_cast<std::size_t>(count));
    for (int32_t r = 0; r < count; ++r) {
      entry.rows.push_back(idType == 'S' ? rows.int16() : rows.int32());
    }
    if (!rows.ok()) {
      fail(errorString,
           QStringLiteral("%1: the rows of entry %2 run past the end")
               .arg(path)
               .arg(i + 1));
      return std::nullopt;
    }
    index.m_entries.push_back(std::move(entry));
  }
  return index;
}

} // namespace ts::packager::vpf
