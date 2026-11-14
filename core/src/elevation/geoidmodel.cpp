// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/elevation/geoidmodel.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <mutex>

#include <QByteArray>
#include <QDir>
#include <QStringList>

#include <gdal.h>
#include <proj.h>

#include "ts/deps/dependencies.h"
#include "ts/deps/projcontext.h"

namespace ts
{

struct GeoidModel::Private {
  ProjContext context;
  PJ* transform = nullptr;
  QString error;
  /** PROJ objects may only be used by one thread at a time. */
  mutable std::mutex mutex;

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

GeoidModel::GeoidModel(const QString& directory, const QString& gridFile)
    : d(std::make_unique<Private>()), m_gridFile(gridFile),
      m_id(QDir::cleanPath(QDir(directory).filePath(gridFile)))
{
  if (!d->context.isValid()) {
    d->error = QStringLiteral("Cannot create a PROJ context");
    return;
  }
  // The grid's directory first, then PROJ's (for proj.db; the packages'
  // own copy, see useProjData()).
  QStringList paths{QDir::toNativeSeparators(QDir(directory).absolutePath())};
  paths += projSearchPaths();
  std::vector<QByteArray> encoded;
  std::vector<const char*> pointers;
  encoded.reserve(static_cast<std::size_t>(paths.size()));
  for (const QString& path : paths) {
    encoded.push_back(path.toUtf8());
    pointers.push_back(encoded.back().constData());
  }
  proj_context_set_search_paths(
      d->context.get(), static_cast<int>(pointers.size()), pointers.data());
  // Only plain file names: PROJ strings split on spaces.
  if (gridFile.isEmpty() || gridFile.contains(QLatin1Char(' ')) ||
      gridFile.contains(QLatin1Char('/')) ||
      gridFile.contains(QLatin1Char('\\'))) {
    d->error = QStringLiteral("Bad geoid grid name \"%1\"").arg(gridFile);
    return;
  }
  const QByteArray definition = QByteArrayLiteral("+proj=vgridshift +grids=") +
                                gridFile.toUtf8() +
                                QByteArrayLiteral(" +multiplier=1");
  d->transform = proj_create(d->context.get(), definition.constData());
  if (d->transform == nullptr) {
    d->error = QStringLiteral("Cannot use the geoid grid %1: %2")
                   .arg(gridFile, d->context.lastErrorString());
    return;
  }
  // PROJ opens the grid on first use: try it, at 0° N 0° E for a global
  // grid, else at the middle of a regional one (such as OSGM15).
  if (!std::isfinite(undulationM(0.0, 0.0)) &&
      !std::isfinite(undulationAtCentre(QDir(directory).filePath(gridFile)))) {
    d->error = QStringLiteral("Cannot read the geoid grid %1 in %2")
                   .arg(gridFile, directory);
    proj_destroy(d->transform);
    d->transform = nullptr;
  }
}

GeoidModel::~GeoidModel() = default;

double GeoidModel::undulationAtCentre(const QString& path) const
{
  initialiseGdal();
  const QByteArray encoded = path.toUtf8();
  GDALDatasetH grid =
      GDALOpenEx(encoded.constData(), GDAL_OF_READONLY | GDAL_OF_RASTER,
                 nullptr, nullptr, nullptr);
  if (grid == nullptr) {
    return std::numeric_limits<double>::quiet_NaN();
  }
  std::array<double, 6> transform{};
  const bool placed = GDALGetGeoTransform(grid, transform.data()) == CE_None;
  const double lon =
      transform[0] + transform[1] * GDALGetRasterXSize(grid) / 2.0;
  const double lat =
      transform[3] + transform[5] * GDALGetRasterYSize(grid) / 2.0;
  GDALClose(grid);
  return placed ? undulationM(lat, lon)
                : std::numeric_limits<double>::quiet_NaN();
}

bool GeoidModel::isValid() const
{
  return d->transform != nullptr && d->error.isEmpty();
}

QString GeoidModel::errorString() const
{
  return d->error;
}

double GeoidModel::undulationM(double latDeg, double lonDeg) const
{
  return undulationsM({GeoPoint{latDeg, lonDeg}}).front();
}

std::vector<double>
GeoidModel::undulationsM(const std::vector<GeoPoint>& points) const
{
  const double nan = std::numeric_limits<double>::quiet_NaN();
  std::vector<double> undulations(points.size(), nan);
  if (d->transform == nullptr || points.empty()) {
    return undulations;
  }
  std::vector<PJ_COORD> coordinates(points.size());
  for (std::size_t i = 0; i < points.size(); ++i) {
    coordinates[i] = proj_coord(proj_torad(points[i].lonDeg),
                                proj_torad(points[i].latDeg), 0.0, 0.0);
  }
  {
    const std::lock_guard<std::mutex> lock(d->mutex);
    proj_errno_reset(d->transform);
    proj_trans_array(d->transform, PJ_FWD, coordinates.size(),
                     coordinates.data());
  }
  // Forward, vgridshift adds the grid's value (multiplier 1): from 0 m it
  // gives N itself.
  for (std::size_t i = 0; i < points.size(); ++i) {
    const double z = coordinates[i].xyz.z;
    if (std::isfinite(z) && z != HUGE_VAL) {
      undulations[i] = z;
    }
  }
  return undulations;
}

void applyVerticalDatum(HeightTile& tile, const std::vector<GeoPoint>& points,
                        const GeoidModel* datum, const GeoidModel* seaLevel)
{
  const GeoidModel* geoid = datum != nullptr ? datum : seaLevel;
  if (geoid == nullptr) {
    return;
  }
  std::vector<double> undulations = geoid->undulationsM(points);
  // A regional datum (OSGM15) has nothing outside its area: there the
  // global geoid stands in, which is near it (better than nothing, a
  // cliff of tens of metres at the grid's edge).
  if (seaLevel != nullptr && seaLevel != geoid &&
      std::any_of(undulations.begin(), undulations.end(),
                  [](double n) { return !std::isfinite(n); })) {
    const std::vector<double> global = seaLevel->undulationsM(points);
    for (std::size_t i = 0; i < undulations.size(); ++i) {
      if (!std::isfinite(undulations[i])) {
        undulations[i] = global[i];
      }
    }
  }
  tile.seaLevelM.assign(points.size(), 0.0F);
  for (std::size_t i = 0; i < points.size() && i < tile.heightsM.size(); ++i) {
    const double n = std::isfinite(undulations[i]) ? undulations[i] : 0.0;
    tile.seaLevelM[i] = static_cast<float>(n);
    if (datum != nullptr) {
      tile.heightsM[i] += static_cast<float>(n);
    }
  }
}

} // namespace ts
