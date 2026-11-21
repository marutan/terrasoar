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
#include "ts/ais/vesselexport.h"

#include <cmath>
#include <cstddef>

#include <QDateTime>
#include <QStringList>
#include <QXmlStreamWriter>

namespace ts
{

namespace
{

QString isoTime(double timeS)
{
  return QDateTime::fromMSecsSinceEpoch(std::llround(timeS * 1000.0))
      .toUTC()
      .toString(Qt::ISODateWithMs);
}

QString number(double value, int32_t decimals)
{
  return std::isnan(value) ? QString() : QString::number(value, 'f', decimals);
}

QByteArray toKml(const VesselTrack& track, const QString& name)
{
  QByteArray out;
  QXmlStreamWriter xml(&out);
  const QString gx = QStringLiteral("http://www.google.com/kml/ext/2.2");
  xml.setAutoFormatting(true);
  xml.writeStartDocument();
  xml.writeStartElement(QStringLiteral("kml"));
  xml.writeDefaultNamespace(QStringLiteral("http://www.opengis.net/kml/2.2"));
  xml.writeNamespace(gx, QStringLiteral("gx"));
  xml.writeStartElement(QStringLiteral("Document"));
  xml.writeTextElement(QStringLiteral("name"), name);
  xml.writeStartElement(QStringLiteral("Placemark"));
  xml.writeTextElement(QStringLiteral("name"), name);
  xml.writeStartElement(gx, QStringLiteral("Track"));
  bool aircraft = false;
  for (const TrackSample& p : track.track.points()) {
    aircraft = aircraft || !std::isnan(p.heightM);
  }
  xml.writeTextElement(QStringLiteral("altitudeMode"),
                       aircraft ? QStringLiteral("absolute")
                                : QStringLiteral("clampToGround"));
  for (const TrackSample& p : track.track.points()) {
    xml.writeTextElement(QStringLiteral("when"), isoTime(p.timeS));
  }
  for (const TrackSample& p : track.track.points()) {
    xml.writeTextElement(
        gx, QStringLiteral("coord"),
        QStringLiteral("%1 %2 %3")
            .arg(QString::number(p.lonDeg, 'f', 7),
                 QString::number(p.latDeg, 'f', 7),
                 QString::number(std::isnan(p.heightM) ? 0.0 : p.heightM, 'f',
                                 1)));
  }
  xml.writeEndElement();
  xml.writeEndElement();
  xml.writeEndElement();
  xml.writeEndElement();
  xml.writeEndDocument();
  return out;
}

QByteArray toGpx(const VesselTrack& track, const QString& name)
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
    if (!std::isnan(p.heightM)) {
      xml.writeTextElement(QStringLiteral("ele"),
                           QString::number(p.heightM, 'f', 1));
    }
    xml.writeTextElement(QStringLiteral("time"), isoTime(p.timeS));
    xml.writeEndElement();
  }
  xml.writeEndElement();
  xml.writeEndElement();
  xml.writeEndElement();
  xml.writeEndDocument();
  return out;
}

QByteArray toCsv(const VesselTrack& track)
{
  QByteArray out = "time,unix_time,mmsi,name,lat,lon,sog_kn,cog,heading,"
                   "rot_deg_min,status\n";
  const std::vector<TrackSample>& points = track.track.points();
  for (std::size_t i = 0; i < points.size(); ++i) {
    const TrackSample& p = points[i];
    const VesselMotion& m = track.motion.at(i);
    const VesselInfo* info = track.infoAt(p.timeS);
    const QStringList fields = {
        isoTime(p.timeS),
        QString::number(p.timeS, 'f', 3),
        QString::number(track.mmsi),
        info != nullptr ? info->name : QString(),
        QString::number(p.latDeg, 'f', 7),
        QString::number(p.lonDeg, 'f', 7),
        number(static_cast<double>(p.speedMS) / (1852.0 / 3600.0), 1),
        number(static_cast<double>(p.trackDeg), 1),
        number(static_cast<double>(m.headingDeg), 0),
        number(static_cast<double>(m.turnDegMin), 1),
        info != nullptr ? QString::number(static_cast<int32_t>(info->status))
                        : QString()};
    out += fields.join(QLatin1Char(',')).toUtf8() + '\n';
  }
  return out;
}

} // namespace

QByteArray exportVesselTrack(const VesselTrack& track, const QString& name,
                             TrackExportFormat format)
{
  switch (format) {
  case TrackExportFormat::Kml:
    break;
  case TrackExportFormat::Gpx:
    return toGpx(track, name);
  case TrackExportFormat::Csv:
    return toCsv(track);
  }
  return toKml(track, name);
}

} // namespace ts
