// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/elevation/geoidmodel.h"

#include <cmath>
#include <limits>
#include <mutex>

#include <QByteArray>
#include <QDir>
#include <QStringList>

#include <proj.h>

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
    : d(std::make_unique<Private>()), m_gridFile(gridFile)
{
  if (!d->context.isValid()) {
    d->error = QStringLiteral("Cannot create a PROJ context");
    return;
  }
  // The grid's directory first, then PROJ's own (for proj.db).
#ifdef Q_OS_WIN
  const QChar separator = QLatin1Char(';');
#else
  const QChar separator = QLatin1Char(':');
#endif
  QStringList paths{QDir::toNativeSeparators(QDir(directory).absolutePath())};
  const char* defaults = proj_info().searchpath;
  if (defaults != nullptr) {
    paths += QString::fromUtf8(defaults).split(separator, Qt::SkipEmptyParts);
  }
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
  // PROJ opens the grid on first use: try it.
  if (!std::isfinite(undulationM(0.0, 0.0))) {
    d->error = QStringLiteral("Cannot read the geoid grid %1 in %2")
                   .arg(gridFile, directory);
    proj_destroy(d->transform);
    d->transform = nullptr;
  }
}

GeoidModel::~GeoidModel() = default;

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
  const std::vector<double> undulations = geoid->undulationsM(points);
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
