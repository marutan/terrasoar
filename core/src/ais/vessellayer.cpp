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
#include "ts/ais/vessellayer.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonValue>

namespace ts
{

namespace
{

QString colouringId(VesselColouring colouring)
{
  switch (colouring) {
  case VesselColouring::Vessel:
    return QStringLiteral("vessel");
  case VesselColouring::Type:
    break;
  case VesselColouring::Speed:
    return QStringLiteral("speed");
  }
  return QStringLiteral("type");
}

VesselColouring colouringFromId(const QString& id)
{
  if (id == QLatin1String("vessel")) {
    return VesselColouring::Vessel;
  }
  if (id == QLatin1String("speed")) {
    return VesselColouring::Speed;
  }
  return VesselColouring::Type;
}

std::optional<double> optionalNumber(const QJsonValue& value)
{
  return value.isDouble() ? std::optional<double>(value.toDouble())
                          : std::nullopt;
}

QString originId(VesselSource::Origin origin)
{
  switch (origin) {
  case VesselSource::Origin::File:
    break;
  case VesselSource::Origin::Receiver:
    return QStringLiteral("receiver");
  case VesselSource::Origin::Feed:
    return QStringLiteral("feed");
  }
  return QStringLiteral("file");
}

/** Reads a file source's members. */
bool readFile(const QJsonObject& json, VesselSource& source)
{
  source.path = json.value(QLatin1String("path")).toString();
  source.format =
      aisFormatFromId(json.value(QLatin1String("format")).toString());
  source.startTimeS = json.value(QLatin1String("startTimeS")).toDouble();
  source.csv =
      AisCsvColumns::fromJson(json.value(QLatin1String("csv")).toObject());
  return !source.path.isEmpty();
}

/** Reads a receiver source's members. */
bool readReceiver(const QJsonObject& json, VesselSource& source)
{
  source.udp = json.value(QLatin1String("udp")).toBool();
  source.host = json.value(QLatin1String("host")).toString();
  const int32_t port = json.value(QLatin1String("port")).toInt(10110);
  source.recordPath = json.value(QLatin1String("recordPath")).toString();
  if (port <= 0 || port > 65535 || (!source.udp && source.host.isEmpty())) {
    return false;
  }
  source.port = static_cast<uint16_t>(port);
  return true;
}

} // namespace

const std::vector<AisFeedEntry>& aisFeedCatalogue()
{
  static const std::vector<AisFeedEntry> catalogue = [] {
    std::vector<AisFeedEntry> entries;
    AisFeedEntry digitraffic;
    digitraffic.feed = AisFeedKind::Digitraffic;
    digitraffic.id = QStringLiteral("digitraffic");
    digitraffic.name = QCoreApplication::translate(
        "ts::AisFeeds", "Fintraffic Digitraffic (Finland, the Baltic)");
    digitraffic.coverage = QCoreApplication::translate(
        "ts::AisFeeds", "The Baltic Sea and Finnish waters, from Finland's "
                        "coastal AIS stations.");
    digitraffic.licence = QStringLiteral("CC BY 4.0");
    digitraffic.attribution = QStringLiteral("Fintraffic / digitraffic.fi");
    digitraffic.url = QUrl(QStringLiteral("https://meri.digitraffic.fi"));
    digitraffic.pollS = 30.0;
    entries.push_back(digitraffic);
    AisFeedEntry kystverket;
    kystverket.feed = AisFeedKind::Kystverket;
    kystverket.id = QStringLiteral("kystverket");
    kystverket.name = QCoreApplication::translate(
        "ts::AisFeeds", "Norwegian Coastal Administration (Kystverket)");
    kystverket.coverage = QCoreApplication::translate(
        "ts::AisFeeds", "Norwegian waters, from Kystverket's AIS network.");
    kystverket.licence = QStringLiteral("NLOD");
    kystverket.attribution =
        QStringLiteral("Norwegian Coastal Administration (NLOD)");
    kystverket.url = QUrl(QStringLiteral("tcp://153.44.253.27:5631"));
    entries.push_back(kystverket);
    return entries;
  }();
  return catalogue;
}

std::optional<AisFeedEntry> aisFeedEntry(const QString& id)
{
  for (const AisFeedEntry& entry : aisFeedCatalogue()) {
    if (entry.id == id) {
      return entry;
    }
  }
  return std::nullopt;
}

bool VesselSource::isVessels(const QJsonObject& source)
{
  return source.value(QLatin1String("kind")).toString() == kind();
}

QJsonObject VesselSource::toJson() const
{
  QJsonObject json;
  json.insert(QStringLiteral("kind"), kind());
  json.insert(QStringLiteral("origin"), originId(origin));
  switch (origin) {
  case Origin::File:
    json.insert(QStringLiteral("path"), path);
    if (format != AisFormat::Unknown) {
      json.insert(QStringLiteral("format"), aisFormatId(format));
    }
    if (startTimeS != 0.0) {
      json.insert(QStringLiteral("startTimeS"), startTimeS);
    }
    if (format == AisFormat::Csv) {
      json.insert(QStringLiteral("csv"), csv.toJson());
    }
    break;
  case Origin::Receiver:
    json.insert(QStringLiteral("udp"), udp);
    if (!udp) {
      json.insert(QStringLiteral("host"), host);
    }
    json.insert(QStringLiteral("port"), static_cast<int32_t>(port));
    if (!recordPath.isEmpty()) {
      json.insert(QStringLiteral("recordPath"), recordPath);
    }
    break;
  case Origin::Feed:
    json.insert(QStringLiteral("feed"), feed);
    if (!url.isEmpty()) {
      json.insert(QStringLiteral("url"), url.toString());
    }
    json.insert(QStringLiteral("attribution"), attribution());
    break;
  }
  return json;
}

std::optional<VesselSource> VesselSource::fromJson(const QJsonObject& json)
{
  if (!isVessels(json)) {
    return std::nullopt;
  }
  VesselSource source;
  const QString origin = json.value(QLatin1String("origin")).toString();
  bool ok = false;
  if (origin == QLatin1String("file")) {
    source.origin = Origin::File;
    ok = readFile(json, source);
  } else if (origin == QLatin1String("receiver")) {
    source.origin = Origin::Receiver;
    ok = readReceiver(json, source);
  } else if (origin == QLatin1String("feed")) {
    source.origin = Origin::Feed;
    source.feed = json.value(QLatin1String("feed")).toString();
    source.url = QUrl(json.value(QLatin1String("url")).toString());
    ok = aisFeedEntry(source.feed).has_value();
  }
  return ok ? std::optional<VesselSource>(source) : std::nullopt;
}

AisReadOptions VesselSource::readOptions() const
{
  AisReadOptions options;
  options.startTimeS = startTimeS;
  options.csv = csv;
  return options;
}

QUrl VesselSource::feedUrl() const
{
  if (!url.isEmpty()) {
    return url;
  }
  const std::optional<AisFeedEntry> entry = aisFeedEntry(feed);
  return entry ? entry->url : QUrl();
}

QString VesselSource::attribution() const
{
  const std::optional<AisFeedEntry> entry = aisFeedEntry(feed);
  return origin == Origin::Feed && entry ? entry->attribution : QString();
}

TrailSettings VesselStyle::trailOf(uint32_t mmsi) const
{
  const std::map<uint32_t, TrailSettings>::const_iterator found =
      trails.find(mmsi);
  return found != trails.end() ? found->second : trail;
}

ShipTypeGroup VesselStyle::groupOf(const VesselInfo& info)
{
  return info.kind == AisStationKind::Vessel ? shipTypeGroup(info.shipType)
                                             : ShipTypeGroup::Navigation;
}

bool VesselStyle::shows(uint32_t mmsi, const VesselInfo& info,
                        const TrackSample& at) const
{
  if (hidden.count(mmsi) != 0) {
    return false;
  }
  const QString filter = nameFilter.trimmed();
  if (!filter.isEmpty() && !info.name.contains(filter, Qt::CaseInsensitive) &&
      !QString::number(mmsi).startsWith(filter)) {
    return false;
  }
  if (!typeGroups.empty() && typeGroups.count(groupOf(info)) == 0) {
    return false;
  }
  if (!statuses.empty() && statuses.count(info.status) == 0) {
    return false;
  }
  const double knots = static_cast<double>(at.speedMS) / (1852.0 / 3600.0);
  if ((minimumSpeedKn && !(knots >= *minimumSpeedKn)) ||
      (maximumSpeedKn && knots > *maximumSpeedKn)) {
    return false;
  }
  return !area || area->contains(at.latDeg, at.lonDeg);
}

QJsonObject VesselStyle::toJson() const
{
  QJsonObject json;
  json.insert(QStringLiteral("color"), color.name(QColor::HexArgb));
  json.insert(QStringLiteral("colourByType"), colourByType);
  json.insert(QStringLiteral("markerSizePx"), markerSizePx);
  json.insert(QStringLiteral("labels"), labels);
  json.insert(QStringLiteral("labelSpeed"), labelSpeed);
  json.insert(QStringLiteral("labelDestination"), labelDestination);
  json.insert(QStringLiteral("courseVectorMinutes"), courseVectorMinutes);
  json.insert(QStringLiteral("trail"), trail.toJson());
  json.insert(QStringLiteral("trailColouring"), colouringId(trailColouring));
  json.insert(QStringLiteral("staleMinimumS"), staleMinimumS);
  json.insert(QStringLiteral("gapS"), gapS);
  if (!nameFilter.isEmpty()) {
    json.insert(QStringLiteral("nameFilter"), nameFilter);
  }
  QJsonArray groups;
  for (const ShipTypeGroup group : typeGroups) {
    groups.append(static_cast<int32_t>(group));
  }
  if (!groups.isEmpty()) {
    json.insert(QStringLiteral("typeGroups"), groups);
  }
  QJsonArray statusList;
  for (const AisNavStatus status : statuses) {
    statusList.append(static_cast<int32_t>(status));
  }
  if (!statusList.isEmpty()) {
    json.insert(QStringLiteral("statuses"), statusList);
  }
  if (minimumSpeedKn) {
    json.insert(QStringLiteral("minimumSpeedKn"), *minimumSpeedKn);
  }
  if (maximumSpeedKn) {
    json.insert(QStringLiteral("maximumSpeedKn"), *maximumSpeedKn);
  }
  if (area) {
    json.insert(QStringLiteral("area"),
                QJsonArray{area->southDeg, area->westDeg, area->northDeg,
                           area->eastDeg});
  }
  QJsonArray hiddenJson;
  for (const uint32_t mmsi : hidden) {
    hiddenJson.append(static_cast<double>(mmsi));
  }
  if (!hiddenJson.isEmpty()) {
    json.insert(QStringLiteral("hidden"), hiddenJson);
  }
  QJsonObject own;
  for (const std::pair<const uint32_t, TrailSettings>& entry : trails) {
    own.insert(QString::number(entry.first), entry.second.toJson());
  }
  if (!own.isEmpty()) {
    json.insert(QStringLiteral("trails"), own);
  }
  return json;
}

namespace
{

/** Reads the lists and the per-station members of a style. */
void readStyleSets(const QJsonObject& json, VesselStyle& style)
{
  const QJsonArray groups = json.value(QLatin1String("typeGroups")).toArray();
  // An index loop: QJsonArray's iterator differs between Qt 5 and Qt 6.
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < groups.size(); ++i) {
    const int32_t g = groups.at(i).toInt(-1);
    if (g >= 0 && g <= static_cast<int32_t>(ShipTypeGroup::Navigation)) {
      style.typeGroups.insert(static_cast<ShipTypeGroup>(g));
    }
  }
  const QJsonArray statuses = json.value(QLatin1String("statuses")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < statuses.size(); ++i) {
    const int32_t s = statuses.at(i).toInt(-1);
    if (s >= 0 && s <= 15) {
      style.statuses.insert(static_cast<AisNavStatus>(s));
    }
  }
  const QJsonArray hidden = json.value(QLatin1String("hidden")).toArray();
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < hidden.size(); ++i) {
    const double mmsi = hidden.at(i).toDouble(-1.0);
    if (mmsi > 0.0 && mmsi < 4294967296.0) {
      style.hidden.insert(static_cast<uint32_t>(mmsi));
    }
  }
  const QJsonObject own = json.value(QLatin1String("trails")).toObject();
  for (QJsonObject::const_iterator i = own.begin(); i != own.end(); ++i) {
    bool ok = false;
    const uint32_t mmsi = i.key().toUInt(&ok);
    const std::optional<TrailSettings> trail =
        TrailSettings::fromJson(i.value().toObject());
    if (ok && trail) {
      style.trails[mmsi] = *trail;
    }
  }
}

} // namespace

VesselStyle VesselStyle::fromJson(const QJsonObject& json)
{
  VesselStyle style;
  const QColor color(json.value(QLatin1String("color")).toString());
  if (color.isValid()) {
    style.color = color;
  }
  style.colourByType =
      json.value(QLatin1String("colourByType")).toBool(style.colourByType);
  style.markerSizePx = std::clamp(
      json.value(QLatin1String("markerSizePx")).toDouble(style.markerSizePx),
      4.0, 64.0);
  style.labels = json.value(QLatin1String("labels")).toBool(style.labels);
  style.labelSpeed =
      json.value(QLatin1String("labelSpeed")).toBool(style.labelSpeed);
  style.labelDestination = json.value(QLatin1String("labelDestination"))
                               .toBool(style.labelDestination);
  style.courseVectorMinutes = std::max(
      json.value(QLatin1String("courseVectorMinutes")).toDouble(0.0), 0.0);
  const std::optional<TrailSettings> trail =
      TrailSettings::fromJson(json.value(QLatin1String("trail")).toObject());
  if (trail) {
    style.trail = *trail;
  }
  style.trailColouring =
      colouringFromId(json.value(QLatin1String("trailColouring")).toString());
  style.staleMinimumS = std::max(
      json.value(QLatin1String("staleMinimumS")).toDouble(style.staleMinimumS),
      10.0);
  style.gapS =
      std::max(json.value(QLatin1String("gapS")).toDouble(style.gapS), 1.0);
  style.nameFilter = json.value(QLatin1String("nameFilter")).toString();
  style.minimumSpeedKn =
      optionalNumber(json.value(QLatin1String("minimumSpeedKn")));
  style.maximumSpeedKn =
      optionalNumber(json.value(QLatin1String("maximumSpeedKn")));
  const QJsonArray area = json.value(QLatin1String("area")).toArray();
  if (area.size() == 4) {
    style.area = LatLonBox{area.at(0).toDouble(), area.at(1).toDouble(),
                           area.at(2).toDouble(), area.at(3).toDouble()};
  }
  readStyleSets(json, style);
  return style;
}

bool VesselStyle::operator==(const VesselStyle& other) const
{
  return color == other.color && colourByType == other.colourByType &&
         markerSizePx == other.markerSizePx && labels == other.labels &&
         labelSpeed == other.labelSpeed &&
         labelDestination == other.labelDestination &&
         courseVectorMinutes == other.courseVectorMinutes &&
         trail == other.trail && trailColouring == other.trailColouring &&
         staleMinimumS == other.staleMinimumS && gapS == other.gapS &&
         nameFilter == other.nameFilter && typeGroups == other.typeGroups &&
         statuses == other.statuses && minimumSpeedKn == other.minimumSpeedKn &&
         maximumSpeedKn == other.maximumSpeedKn && area == other.area &&
         hidden == other.hidden && trails == other.trails;
}

} // namespace ts
