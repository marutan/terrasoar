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
#include "kmlexport.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QMessageBox>
#include <QStringList>
#include <QWidget>

#include "ts/kml/kmlwriter.h"

namespace ts
{

namespace
{

double wrapped(double lonDeg)
{
  while (lonDeg > 180.0) {
    lonDeg -= 360.0;
  }
  while (lonDeg < -180.0) {
    lonDeg += 360.0;
  }
  return lonDeg;
}

std::vector<KmlCoordinate> closedRing(const std::vector<LonLat>& ring)
{
  std::vector<KmlCoordinate> coordinates;
  for (const LonLat& point : ring) {
    coordinates.push_back(
        KmlCoordinate{wrapped(point.lonDeg), point.latDeg, 0.0});
  }
  if (!coordinates.empty() && !(coordinates.front() == coordinates.back())) {
    coordinates.push_back(coordinates.front());
  }
  return coordinates;
}

} // namespace

QString saveKmlAs(QWidget* parent, const KmlDocument& document,
                  const QString& title, const QString& suggestedName)
{
  QString path = QFileDialog::getSaveFileName(
      parent, title, suggestedName,
      QCoreApplication::translate(
          "ts::KmlExport", "KML (*.kml);;KMZ with its pictures (*.kmz)"));
  if (path.isEmpty()) {
    return QString();
  }
  if (!path.endsWith(QLatin1String(".kml"), Qt::CaseInsensitive) &&
      !path.endsWith(QLatin1String(".kmz"), Qt::CaseInsensitive)) {
    path += QStringLiteral(".kml");
  }
  return saveKmlTo(parent, document, path, title) ? path : QString();
}

bool saveKmlTo(QWidget* parent, const KmlDocument& document,
               const QString& path, const QString& title)
{
  QString error;
  QStringList missing;
  if (!writeKmlFile(document, path, &error, &missing)) {
    QMessageBox::warning(parent, title, error);
    return false;
  }
  if (!missing.isEmpty()) {
    QMessageBox::warning(
        parent, title,
        QCoreApplication::translate(
            "ts::KmlExport",
            "Saved \"%1\", but these files it uses couldn't be read and "
            "aren't in it:\n%2")
            .arg(QDir::toNativeSeparators(path),
                 missing.join(QLatin1Char('\n'))));
  }
  return true;
}

KmlGeometry kmlLineThrough(const std::vector<GeoPoint>& points)
{
  KmlGeometry line;
  line.kind = KmlGeometry::Kind::LineString;
  line.tessellate = true;
  for (const GeoPoint& point : points) {
    line.coordinates.push_back(
        KmlCoordinate{wrapped(point.lonDeg), point.latDeg, 0.0});
  }
  return line;
}

KmlGeometry kmlAreaOf(const RegionGeometry& region)
{
  KmlGeometry area;
  area.kind = KmlGeometry::Kind::Polygon;
  area.tessellate = true;
  const std::vector<std::vector<LonLat>>& rings = region.rings();
  for (std::size_t i = 0; i < rings.size(); ++i) {
    if (i == 0) {
      area.coordinates = closedRing(rings[i]);
    } else {
      area.inner.push_back(closedRing(rings[i]));
    }
  }
  return area;
}

} // namespace ts
