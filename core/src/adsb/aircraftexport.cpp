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
#include "ts/adsb/aircraftexport.h"

#include <cmath>
#include <cstddef>

#include <QDateTime>
#include <QFileInfo>
#include <QXmlStreamWriter>

namespace ts
{

namespace
{

QString isoTime(double timeS)
{
  // UTC as a time spec, not a zone: written with "Z".
  return QDateTime::fromMSecsSinceEpoch(std::llround(timeS * 1000.0))
      .toUTC()
      .toString(Qt::ISODateWithMs);
}

/** The height above sea level, or NaN. */
double heightMsl(const TrackSample& p, const UndulationFunction& undulation)
{
  if (std::isnan(p.heightM)) {
    return p.heightM;
  }
  double n = undulation ? undulation(p.latDeg, p.lonDeg) : 0.0;
  if (std::isnan(n)) {
    n = 0.0;
  }
  return p.heightM - n;
}

QString number(double value, int32_t decimals)
{
  return std::isnan(value) ? QString() : QString::number(value, 'f', decimals);
}

QByteArray toKml(const AircraftTrack& track, const QString& name,
                 const UndulationFunction& undulation)
{
  QByteArray out;
  QXmlStreamWriter xml(&out);
  xml.setAutoFormatting(true);
  xml.writeStartDocument();
  xml.writeStartElement(QStringLiteral("kml"));
  xml.writeDefaultNamespace(QStringLiteral("http://www.opengis.net/kml/2.2"));
  xml.writeNamespace(QStringLiteral("http://www.google.com/kml/ext/2.2"),
                     QStringLiteral("gx"));
  xml.writeStartElement(QStringLiteral("Document"));
  xml.writeTextElement(QStringLiteral("name"), name);
  xml.writeStartElement(QStringLiteral("Placemark"));
  xml.writeTextElement(QStringLiteral("name"), name);
  xml.writeStartElement(QStringLiteral("http://www.google.com/kml/ext/2.2"),
                        QStringLiteral("Track"));
  bool anyHeight = false;
  for (const TrackSample& p : track.track.points()) {
    anyHeight = anyHeight || !std::isnan(p.heightM);
  }
  xml.writeTextElement(QStringLiteral("altitudeMode"),
                       anyHeight ? QStringLiteral("absolute")
                                 : QStringLiteral("clampToGround"));
  for (const TrackSample& p : track.track.points()) {
    xml.writeTextElement(QStringLiteral("when"), isoTime(p.timeS));
  }
  for (const TrackSample& p : track.track.points()) {
    const double height = heightMsl(p, undulation);
    xml.writeTextElement(
        QStringLiteral("http://www.google.com/kml/ext/2.2"),
        QStringLiteral("coord"),
        QStringLiteral("%1 %2 %3")
            .arg(QString::number(p.lonDeg, 'f', 7),
                 QString::number(p.latDeg, 'f', 7),
                 QString::number(std::isnan(height) ? 0.0 : height, 'f', 1)));
  }
  xml.writeEndElement(); // gx:Track
  xml.writeEndElement(); // Placemark
  xml.writeEndElement(); // Document
  xml.writeEndElement(); // kml
  xml.writeEndDocument();
  return out;
}

QByteArray toGpx(const AircraftTrack& track, const QString& name,
                 const UndulationFunction& undulation)
{
  QByteArray out;
  QXmlStreamWriter xml(&out);
  xml.setAutoFormatting(true);
  xml.writeStartDocument();
  xml.writeStartElement(QStringLiteral("gpx"));
  xml.writeDefaultNamespace(
      QStringLiteral("http://www.topografix.com/GPX/1/1"));
  xml.writeAttribute(QStringLiteral("version"), QStringLiteral("1.1"));
  xml.writeAttribute(QStringLiteral("creator"), QStringLiteral("Terra Soar"));
  xml.writeStartElement(QStringLiteral("trk"));
  xml.writeTextElement(QStringLiteral("name"), name);
  xml.writeStartElement(QStringLiteral("trkseg"));
  for (const TrackSample& p : track.track.points()) {
    xml.writeStartElement(QStringLiteral("trkpt"));
    xml.writeAttribute(QStringLiteral("lat"),
                       QString::number(p.latDeg, 'f', 7));
    xml.writeAttribute(QStringLiteral("lon"),
                       QString::number(p.lonDeg, 'f', 7));
    const double height = heightMsl(p, undulation);
    if (!std::isnan(height)) {
      xml.writeTextElement(QStringLiteral("ele"),
                           QString::number(height, 'f', 1));
    }
    xml.writeTextElement(QStringLiteral("time"), isoTime(p.timeS));
    xml.writeEndElement();
  }
  xml.writeEndElement(); // trkseg
  xml.writeEndElement(); // trk
  xml.writeEndElement(); // gpx
  xml.writeEndDocument();
  return out;
}

QByteArray toCsv(const AircraftTrack& track,
                 const UndulationFunction& undulation)
{
  QByteArray out = "time,unix_time,icao,callsign,lat,lon,height_ellipsoid_m,"
                   "height_msl_m,alt_baro_ft,alt_geom_ft,gs_kt,track,"
                   "vr_ftmin\n";
  const std::vector<TrackSample>& points = track.track.points();
  for (std::size_t i = 0; i < points.size(); ++i) {
    const TrackSample& p = points[i];
    const AircraftAltitudes& a = track.altitudes.at(i);
    const AircraftInfo* info = track.infoAt(p.timeS);
    const QStringList fields = {
        isoTime(p.timeS),
        QString::number(p.timeS, 'f', 3),
        formatIcaoAddress(track.address),
        info != nullptr ? info->callsign : QString(),
        QString::number(p.latDeg, 'f', 7),
        QString::number(p.lonDeg, 'f', 7),
        number(p.heightM, 1),
        number(heightMsl(p, undulation), 1),
        number(static_cast<double>(a.baroFt), 0),
        number(static_cast<double>(a.geometricFt), 0),
        number(static_cast<double>(p.speedMS) / knotsToMetresPerSecond, 1),
        number(static_cast<double>(p.trackDeg), 1),
        number(static_cast<double>(p.verticalRateMS) / (feetToMetres / 60.0),
               0)};
    out += fields.join(QLatin1Char(',')).toUtf8() + '\n';
  }
  return out;
}

} // namespace

TrackExportFormat trackExportFormatFor(const QString& path)
{
  const QString suffix = QFileInfo(path).suffix().toLower();
  if (suffix == QLatin1String("gpx")) {
    return TrackExportFormat::Gpx;
  }
  if (suffix == QLatin1String("csv")) {
    return TrackExportFormat::Csv;
  }
  return TrackExportFormat::Kml;
}

QByteArray exportAircraftTrack(const AircraftTrack& track, const QString& name,
                               TrackExportFormat format,
                               const UndulationFunction& undulation)
{
  switch (format) {
  case TrackExportFormat::Kml:
    break;
  case TrackExportFormat::Gpx:
    return toGpx(track, name, undulation);
  case TrackExportFormat::Csv:
    return toCsv(track, undulation);
  }
  return toKml(track, name, undulation);
}

} // namespace ts
