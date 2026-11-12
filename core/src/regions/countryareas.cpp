// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/regions/countryareas.h"

#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QDir>

#include <sqlite3.h>

#include "ts/deps/sqliteconnection.h"
#include "ts/packs/packmanifest.h"
#include "ts/providers/geopackagefeatureprovider.h"

namespace ts
{

namespace
{

QString columnText(sqlite3_stmt* statement, int column)
{
  const unsigned char* text = sqlite3_column_text(statement, column);
  return text == nullptr
             ? QString()
             : QString::fromUtf8(reinterpret_cast<const char*>(text));
}

/** Finalises a statement when it goes. */
struct Statement {
  sqlite3_stmt* handle = nullptr;
  Statement() = default;
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;
  Statement(Statement&&) = delete;
  Statement& operator=(Statement&&) = delete;
  ~Statement() { sqlite3_finalize(handle); }
};

/** Nothing, and the reason in @p errorString. */
std::optional<RegionGeometry> unreadable(QString* errorString)
{
  if (errorString != nullptr) {
    *errorString = QCoreApplication::translate(
        "ts::CountryAreas", "The country's outline can't be read.");
  }
  return std::nullopt;
}

} // namespace

CountryAreas::CountryAreas(QString path) : m_path(std::move(path))
{
  const SqliteConnection connection(m_path,
                                    SqliteConnection::OpenMode::ReadOnly);
  if (!connection.isOpen()) {
    m_error = connection.errorString();
    return;
  }
  Statement statement;
  if (sqlite3_prepare_v2(connection.get(),
                         "SELECT id, name, iso_a2, iso_a3 FROM countries", -1,
                         &statement.handle, nullptr) != SQLITE_OK) {
    m_error = QString::fromUtf8(sqlite3_errmsg(connection.get()));
    return;
  }
  while (sqlite3_step(statement.handle) == SQLITE_ROW) {
    m_countries.push_back(CountryArea{
        columnText(statement.handle, 0), columnText(statement.handle, 1),
        columnText(statement.handle, 2), columnText(statement.handle, 3)});
  }
  std::sort(m_countries.begin(), m_countries.end(),
            [](const CountryArea& a, const CountryArea& b) {
              return a.name.localeAwareCompare(b.name) < 0;
            });
  if (m_countries.empty()) {
    m_error = QStringLiteral("%1 lists no countries").arg(m_path);
  }
}

QString CountryAreas::fileInPack(const PackManifest& manifest,
                                 const QString& directory)
{
  for (const PackDataset& dataset : manifest.datasets) {
    if (dataset.kind == PackDataset::Kind::Areas) {
      return QDir(directory).filePath(dataset.file);
    }
  }
  return QString();
}

std::optional<CountryArea> CountryAreas::country(const QString& id) const
{
  for (const CountryArea& entry : m_countries) {
    if (entry.id == id) {
      return entry;
    }
  }
  return std::nullopt;
}

std::optional<RegionGeometry> CountryAreas::area(const QString& id,
                                                 QString* errorString) const
{
  const SqliteConnection connection(m_path,
                                    SqliteConnection::OpenMode::ReadOnly);
  Statement statement;
  if (!connection.isOpen() ||
      sqlite3_prepare_v2(connection.get(),
                         "SELECT geom FROM countries WHERE id = ?", -1,
                         &statement.handle, nullptr) != SQLITE_OK) {
    return unreadable(errorString);
  }
  const QByteArray key = id.toUtf8();
  sqlite3_bind_text(statement.handle, 1, key.constData(),
                    static_cast<int>(key.size()), SQLITE_TRANSIENT);
  if (sqlite3_step(statement.handle) != SQLITE_ROW) {
    return unreadable(errorString);
  }
  const unsigned char* blob = static_cast<const unsigned char*>(
      sqlite3_column_blob(statement.handle, 0));
  const int bytes = sqlite3_column_bytes(statement.handle, 0);
  std::vector<Feature> features;
  if (blob == nullptr || bytes <= 0 ||
      !decodeGeoPackageGeometry(blob, static_cast<std::size_t>(bytes),
                                &features)) {
    return unreadable(errorString);
  }
  std::vector<std::vector<LonLat>> rings;
  for (const Feature& feature : features) {
    for (const std::vector<GeoPoint>& part : feature.parts) {
      std::vector<LonLat> ring;
      ring.reserve(part.size());
      for (const GeoPoint& point : part) {
        ring.push_back(LonLat{point.lonDeg, point.latDeg});
      }
      rings.push_back(std::move(ring));
    }
  }
  return RegionGeometry::polygons(rings, errorString);
}

} // namespace ts
