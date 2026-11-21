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
#include "ts/adsb/aircraftlayer.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonValue>

namespace ts
{

namespace
{

QJsonValue colorJson(const QColor& color)
{
  return color.name(QColor::HexArgb);
}

QColor colorFromJson(const QJsonValue& value, const QColor& fallback)
{
  const QColor color(value.toString());
  return color.isValid() ? color : fallback;
}

std::optional<double> optionalNumber(const QJsonValue& value)
{
  return value.isDouble() ? std::optional<double>(value.toDouble())
                          : std::nullopt;
}

QString colouringId(TrailColouring colouring)
{
  switch (colouring) {
  case TrailColouring::Aircraft:
    return QStringLiteral("aircraft");
  case TrailColouring::Altitude:
    break;
  case TrailColouring::VerticalRate:
    return QStringLiteral("verticalRate");
  }
  return QStringLiteral("altitude");
}

TrailColouring colouringFromId(const QString& id)
{
  if (id == QLatin1String("aircraft")) {
    return TrailColouring::Aircraft;
  }
  if (id == QLatin1String("verticalRate")) {
    return TrailColouring::VerticalRate;
  }
  return TrailColouring::Altitude;
}

} // namespace

QString receiverProtocolId(ReceiverProtocol protocol)
{
  switch (protocol) {
  case ReceiverProtocol::Sbs:
    break;
  case ReceiverProtocol::Avr:
    return QStringLiteral("avr");
  case ReceiverProtocol::Beast:
    return QStringLiteral("beast");
  case ReceiverProtocol::Json:
    return QStringLiteral("json");
  }
  return QStringLiteral("sbs");
}

std::optional<ReceiverProtocol> receiverProtocolFromId(const QString& id)
{
  for (const ReceiverProtocol protocol :
       {ReceiverProtocol::Sbs, ReceiverProtocol::Avr, ReceiverProtocol::Beast,
        ReceiverProtocol::Json}) {
    if (receiverProtocolId(protocol) == id) {
      return protocol;
    }
  }
  return std::nullopt;
}

uint16_t defaultReceiverPort(ReceiverProtocol protocol)
{
  switch (protocol) {
  case ReceiverProtocol::Sbs:
    break;
  case ReceiverProtocol::Avr:
    return 30002;
  case ReceiverProtocol::Beast:
    return 30005;
  case ReceiverProtocol::Json:
    return 8080;
  }
  return 30003;
}

bool AircraftSource::isAircraft(const QJsonObject& source)
{
  return source.value(QLatin1String("kind")).toString() == kind();
}

QJsonObject AircraftSource::toJson() const
{
  QJsonObject json;
  json.insert(QStringLiteral("kind"), kind());
  if (origin == Origin::File) {
    json.insert(QStringLiteral("origin"), QStringLiteral("file"));
    json.insert(QStringLiteral("path"), path);
    if (format != AdsbFormat::Unknown) {
      json.insert(QStringLiteral("format"), adsbFormatId(format));
    }
    if (startTimeS != 0.0) {
      json.insert(QStringLiteral("startTimeS"), startTimeS);
    }
    json.insert(QStringLiteral("sbsLocalTime"), sbsLocalTime);
    if (format == AdsbFormat::Csv) {
      json.insert(QStringLiteral("csv"), csv.toJson());
    }
  } else {
    json.insert(QStringLiteral("origin"), QStringLiteral("receiver"));
    json.insert(QStringLiteral("protocol"), receiverProtocolId(protocol));
    if (protocol == ReceiverProtocol::Json) {
      json.insert(QStringLiteral("url"), url.toString());
      json.insert(QStringLiteral("pollS"), pollS);
    } else {
      json.insert(QStringLiteral("host"), host);
      json.insert(QStringLiteral("port"), static_cast<int32_t>(port));
    }
    if (!recordPath.isEmpty()) {
      json.insert(QStringLiteral("recordPath"), recordPath);
    }
  }
  if (receiverPosition) {
    json.insert(QStringLiteral("receiverLatDeg"), receiverPosition->latDeg);
    json.insert(QStringLiteral("receiverLonDeg"), receiverPosition->lonDeg);
  }
  return json;
}

std::optional<AircraftSource> AircraftSource::fromJson(const QJsonObject& json)
{
  if (!isAircraft(json)) {
    return std::nullopt;
  }
  AircraftSource source;
  const QString origin = json.value(QLatin1String("origin")).toString();
  if (origin == QLatin1String("file")) {
    source.origin = Origin::File;
    source.path = json.value(QLatin1String("path")).toString();
    if (source.path.isEmpty()) {
      return std::nullopt;
    }
    source.format =
        adsbFormatFromId(json.value(QLatin1String("format")).toString());
    source.startTimeS = json.value(QLatin1String("startTimeS")).toDouble();
    source.sbsLocalTime =
        json.value(QLatin1String("sbsLocalTime")).toBool(true);
    source.csv =
        CsvColumns::fromJson(json.value(QLatin1String("csv")).toObject());
  } else if (origin == QLatin1String("receiver")) {
    source.origin = Origin::Receiver;
    const std::optional<ReceiverProtocol> protocol = receiverProtocolFromId(
        json.value(QLatin1String("protocol")).toString());
    if (!protocol) {
      return std::nullopt;
    }
    source.protocol = *protocol;
    if (source.protocol == ReceiverProtocol::Json) {
      source.url = QUrl(json.value(QLatin1String("url")).toString());
      source.pollS =
          std::max(json.value(QLatin1String("pollS")).toDouble(1.0), 1.0);
      if (!source.url.isValid() || source.url.isEmpty()) {
        return std::nullopt;
      }
    } else {
      source.host = json.value(QLatin1String("host")).toString();
      const int32_t port = json.value(QLatin1String("port"))
                               .toInt(defaultReceiverPort(source.protocol));
      if (source.host.isEmpty() || port <= 0 || port > 65535) {
        return std::nullopt;
      }
      source.port = static_cast<uint16_t>(port);
    }
    source.recordPath = json.value(QLatin1String("recordPath")).toString();
  } else {
    return std::nullopt;
  }
  const QJsonValue lat = json.value(QLatin1String("receiverLatDeg"));
  const QJsonValue lon = json.value(QLatin1String("receiverLonDeg"));
  if (lat.isDouble() && lon.isDouble() && std::fabs(lat.toDouble()) <= 90.0) {
    source.receiverPosition = CprPosition{lat.toDouble(), lon.toDouble()};
  }
  return source;
}

AdsbReadOptions AircraftSource::readOptions() const
{
  AdsbReadOptions options;
  options.decoder.receiver = receiverPosition;
  options.startTimeS = startTimeS;
  options.sbsLocalTime = sbsLocalTime;
  options.csv = csv;
  return options;
}

QString AircraftSource::describe() const
{
  if (origin == Origin::File) {
    return QFileInfo(path).fileName();
  }
  if (protocol == ReceiverProtocol::Json) {
    return url.toDisplayString();
  }
  return QStringLiteral("%1:%2").arg(host).arg(port);
}

// A latitude, then a longitude, as named.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool LatLonBox::contains(double latDeg, double lonDeg) const
{
  if (latDeg < southDeg || latDeg > northDeg) {
    return false;
  }
  if (westDeg <= eastDeg) {
    return lonDeg >= westDeg && lonDeg <= eastDeg;
  }
  return lonDeg >= westDeg || lonDeg <= eastDeg;
}

TrailSettings AircraftStyle::trailOf(uint32_t address) const
{
  const std::map<uint32_t, AircraftOverride>::const_iterator found =
      overrides.find(address);
  if (found != overrides.end()) {
    return found->second.trail.value_or(trail);
  }
  return trail;
}

bool AircraftStyle::isFiltered() const
{
  return !callsignFilter.isEmpty() || !addressFilter.isEmpty() ||
         minimumAltitudeFt || maximumAltitudeFt || area;
}

// A latitude, a longitude and an altitude, as named.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
bool AircraftStyle::shows(uint32_t address, const QString& callsign,
                          double latDeg, double lonDeg, double altitudeFt) const
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  if (hidden.count(address) != 0) {
    return false;
  }
  if (!callsignFilter.isEmpty() &&
      !callsign.contains(callsignFilter.trimmed(), Qt::CaseInsensitive)) {
    return false;
  }
  if (!addressFilter.isEmpty() &&
      !formatIcaoAddress(address).startsWith(addressFilter.trimmed(),
                                             Qt::CaseInsensitive)) {
    return false;
  }
  if (minimumAltitudeFt &&
      (std::isnan(altitudeFt) || altitudeFt < *minimumAltitudeFt)) {
    return false;
  }
  if (maximumAltitudeFt && !std::isnan(altitudeFt) &&
      altitudeFt > *maximumAltitudeFt) {
    return false;
  }
  return !area || area->contains(latDeg, lonDeg);
}

QJsonObject AircraftStyle::toJson() const
{
  QJsonObject json;
  json.insert(QStringLiteral("color"), colorJson(color));
  json.insert(QStringLiteral("markerSizePx"), markerSizePx);
  json.insert(QStringLiteral("labels"), labels);
  json.insert(QStringLiteral("labelAltitude"), labelAltitude);
  json.insert(QStringLiteral("labelSpeed"), labelSpeed);
  json.insert(QStringLiteral("dropLines"), dropLines);
  json.insert(QStringLiteral("trail"), trail.toJson());
  json.insert(QStringLiteral("colouring"), colouringId(colouring));
  json.insert(QStringLiteral("staleS"), timing.staleS);
  json.insert(QStringLiteral("gapS"), timing.gapS);
  if (!callsignFilter.isEmpty()) {
    json.insert(QStringLiteral("callsignFilter"), callsignFilter);
  }
  if (!addressFilter.isEmpty()) {
    json.insert(QStringLiteral("addressFilter"), addressFilter);
  }
  if (minimumAltitudeFt) {
    json.insert(QStringLiteral("minimumAltitudeFt"), *minimumAltitudeFt);
  }
  if (maximumAltitudeFt) {
    json.insert(QStringLiteral("maximumAltitudeFt"), *maximumAltitudeFt);
  }
  if (area) {
    json.insert(QStringLiteral("area"),
                QJsonArray{area->southDeg, area->westDeg, area->northDeg,
                           area->eastDeg});
  }
  QJsonArray hiddenJson;
  for (const uint32_t address : hidden) {
    hiddenJson.append(formatIcaoAddress(address));
  }
  if (!hiddenJson.isEmpty()) {
    json.insert(QStringLiteral("hidden"), hiddenJson);
  }
  QJsonObject overridesJson;
  for (const std::pair<const uint32_t, AircraftOverride>& entry : overrides) {
    if (entry.second.trail) {
      QJsonObject one;
      one.insert(QStringLiteral("trail"), entry.second.trail->toJson());
      overridesJson.insert(formatIcaoAddress(entry.first), one);
    }
  }
  if (!overridesJson.isEmpty()) {
    json.insert(QStringLiteral("overrides"), overridesJson);
  }
  return json;
}

AircraftStyle AircraftStyle::fromJson(const QJsonObject& json)
{
  AircraftStyle style;
  style.color = colorFromJson(json.value(QLatin1String("color")), style.color);
  style.markerSizePx = std::clamp(
      json.value(QLatin1String("markerSizePx")).toDouble(style.markerSizePx),
      4.0, 64.0);
  style.labels = json.value(QLatin1String("labels")).toBool(style.labels);
  style.labelAltitude =
      json.value(QLatin1String("labelAltitude")).toBool(style.labelAltitude);
  style.labelSpeed =
      json.value(QLatin1String("labelSpeed")).toBool(style.labelSpeed);
  style.dropLines =
      json.value(QLatin1String("dropLines")).toBool(style.dropLines);
  const std::optional<TrailSettings> trail =
      TrailSettings::fromJson(json.value(QLatin1String("trail")).toObject());
  if (trail) {
    style.trail = *trail;
  }
  style.colouring =
      colouringFromId(json.value(QLatin1String("colouring")).toString());
  style.timing.staleS = std::max(
      json.value(QLatin1String("staleS")).toDouble(style.timing.staleS), 1.0);
  style.timing.gapS = std::max(
      json.value(QLatin1String("gapS")).toDouble(style.timing.gapS), 1.0);
  style.callsignFilter = json.value(QLatin1String("callsignFilter")).toString();
  style.addressFilter = json.value(QLatin1String("addressFilter")).toString();
  style.minimumAltitudeFt =
      optionalNumber(json.value(QLatin1String("minimumAltitudeFt")));
  style.maximumAltitudeFt =
      optionalNumber(json.value(QLatin1String("maximumAltitudeFt")));
  const QJsonArray area = json.value(QLatin1String("area")).toArray();
  if (area.size() == 4) {
    style.area = LatLonBox{area.at(0).toDouble(), area.at(1).toDouble(),
                           area.at(2).toDouble(), area.at(3).toDouble()};
  }
  const QJsonArray hiddenJson = json.value(QLatin1String("hidden")).toArray();
  // An index loop: QJsonArray's iterator differs between Qt 5 and Qt 6.
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < hiddenJson.size(); ++i) {
    const std::optional<uint32_t> address =
        parseIcaoAddress(hiddenJson.at(i).toString());
    if (address) {
      style.hidden.insert(*address);
    }
  }
  const QJsonObject overrides =
      json.value(QLatin1String("overrides")).toObject();
  for (QJsonObject::const_iterator i = overrides.begin(); i != overrides.end();
       ++i) {
    const std::optional<uint32_t> address = parseIcaoAddress(i.key());
    const std::optional<TrailSettings> own = TrailSettings::fromJson(
        i.value().toObject().value(QLatin1String("trail")).toObject());
    if (address && own) {
      style.overrides[*address].trail = own;
    }
  }
  return style;
}

bool AircraftStyle::operator==(const AircraftStyle& other) const
{
  return color == other.color && markerSizePx == other.markerSizePx &&
         labels == other.labels && labelAltitude == other.labelAltitude &&
         labelSpeed == other.labelSpeed && dropLines == other.dropLines &&
         trail == other.trail && colouring == other.colouring &&
         timing.staleS == other.timing.staleS &&
         timing.gapS == other.timing.gapS &&
         callsignFilter == other.callsignFilter &&
         addressFilter == other.addressFilter &&
         minimumAltitudeFt == other.minimumAltitudeFt &&
         maximumAltitudeFt == other.maximumAltitudeFt && area == other.area &&
         hidden == other.hidden && overrides == other.overrides;
}

} // namespace ts
