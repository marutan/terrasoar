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
#include "aircraftlayers.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <unordered_map>
#include <utility>

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTimeZone>
#include <QTimer>

#include "ts/adsb/aircraftdraws.h"
#include "ts/elevation/geoidmodel.h"
#include "ts/network/networkclient.h"
#include "ts/time/programclock.h"
#include "ts/tracks/trackdraws.h"

namespace ts
{

namespace
{

/** How often live tracks are rebuilt from the reports, milliseconds. */
constexpr int32_t publishMs = 1000;

/** Live aircraft not heard for this long are forgotten, seconds. */
constexpr double liveForgetS = 6.0 * 3600.0;

std::array<std::uint8_t, 4> rgbaOf(const QColor& color, double alpha)
{
  return {static_cast<std::uint8_t>(color.red()),
          static_cast<std::uint8_t>(color.green()),
          static_cast<std::uint8_t>(color.blue()),
          static_cast<std::uint8_t>(
              std::lround(color.alpha() * std::clamp(alpha, 0.0, 1.0)))};
}

/** The altitude shown and filtered on: barometric, else the height. */
double altitudeFtOf(AircraftAltitudes altitudes, const TrackSample& at)
{
  if (!std::isnan(altitudes.baroFt)) {
    return static_cast<double>(altitudes.baroFt);
  }
  if (!std::isnan(altitudes.geometricFt)) {
    return static_cast<double>(altitudes.geometricFt);
  }
  return std::isnan(at.heightM) ? std::numeric_limits<double>::quiet_NaN()
                                : at.heightM / feetToMetres;
}

/** The set's extent and counts from its tracks. */
void summarise(AircraftSet& set)
{
  set.firstS = 0.0;
  set.lastS = 0.0;
  set.positions = 0;
  for (const std::shared_ptr<const AircraftTrack>& track : set.tracks) {
    if (track->track.isEmpty()) {
      continue;
    }
    set.positions += static_cast<int64_t>(track->track.points().size());
    set.firstS = set.firstS == 0.0
                     ? track->track.firstTimeS()
                     : std::min(set.firstS, track->track.firstTimeS());
    set.lastS = std::max(set.lastS, track->track.lastTimeS());
  }
}

} // namespace

std::shared_ptr<const AircraftTrack> AircraftSet::find(uint32_t address) const
{
  const std::vector<std::shared_ptr<const AircraftTrack>>::const_iterator
      found =
          std::lower_bound(tracks.begin(), tracks.end(), address,
                           [](const std::shared_ptr<const AircraftTrack>& track,
                              uint32_t a) { return track->address < a; });
  return found != tracks.end() && (*found)->address == address ? *found
                                                               : nullptr;
}

AircraftLayers::AircraftLayers(ProgramClock* clock, NetworkSource network,
                               GeoidSource geoid, QObject* parent)
    : QObject(parent), m_clock(clock), m_network(std::move(network)),
      m_geoid(std::move(geoid)), m_publish(new QTimer(this)),
      m_cancelled(std::make_shared<std::atomic<bool>>(false))
{
  m_pool.setMaxThreadCount(2);
  m_publish->setInterval(publishMs);
  connect(m_publish, &QTimer::timeout, this, &AircraftLayers::publishLive);
  connect(m_clock, &ProgramClock::timeChanged, this,
          &AircraftLayers::updateDraws);
}

AircraftLayers::~AircraftLayers()
{
  // Nothing more is said to the window, which may be going too.
  blockSignals(true);
  if (m_clock) {
    m_clock->setTicking(this, false);
  }
  m_cancelled->store(true);
  m_pool.waitForDone();
  for (std::pair<const QString, Live>& live : m_live) {
    live.second.feed->disconnect(this);
    live.second.feed->stop();
  }
}

QString AircraftLayers::keyOf(const QJsonObject& source)
{
  return QString::fromUtf8(
      QJsonDocument(source).toJson(QJsonDocument::Compact));
}

void AircraftLayers::setLayers(std::vector<AircraftLayerEntry> layers)
{
  m_layers = std::move(layers);
  std::set<QString> wanted;
  for (const AircraftLayerEntry& layer : m_layers) {
    wanted.insert(keyOf(layer.definition.source));
    load(layer.definition.source);
  }
  // Receivers no visible layer shows are disconnected.
  std::vector<QString> idle;
  for (const std::pair<const QString, Live>& live : m_live) {
    if (wanted.count(live.first) == 0) {
      idle.push_back(live.first);
    }
  }
  for (const QString& key : idle) {
    stopLive(key);
  }
  rebuildTrails();
  updateDraws();
}

std::shared_ptr<const AircraftSet>
AircraftLayers::set(const QJsonObject& source) const
{
  const std::map<QString, std::shared_ptr<const AircraftSet>>::const_iterator
      found = m_sets.find(keyOf(source));
  return found != m_sets.end() ? found->second : nullptr;
}

void AircraftLayers::load(const QJsonObject& source)
{
  const QString key = keyOf(source);
  if (m_sets.find(key) == m_sets.end()) {
    startLoad(key, source);
  }
}

void AircraftLayers::reload(const QJsonObject& source)
{
  const QString key = keyOf(source);
  const std::shared_ptr<const AircraftSet> old = set(source);
  if (old && old->state == AircraftSet::State::Loading) {
    return;
  }
  stopLive(key);
  startLoad(key, source);
}

bool AircraftLayers::isBusy() const
{
  return std::any_of(
      m_sets.begin(), m_sets.end(),
      [](const std::pair<const QString, std::shared_ptr<const AircraftSet>>&
             entry) {
        return entry.second->state == AircraftSet::State::Loading;
      });
}

AircraftHeightSettings
AircraftLayers::heightSettings(const std::shared_ptr<const GeoidModel>& geoid)
{
  AircraftHeightSettings settings;
  if (!geoid || !geoid->isValid()) {
    return settings;
  }
  // The geoid changes slowly: a value for each quarter degree.
  std::shared_ptr<std::unordered_map<int64_t, double>> cells =
      std::make_shared<std::unordered_map<int64_t, double>>();
  settings.undulation = [geoid, cells](double latDeg, double lonDeg) {
    const int64_t row = static_cast<int64_t>(std::floor(latDeg * 4.0));
    const int64_t column = static_cast<int64_t>(std::floor(lonDeg * 4.0));
    const int64_t key = row * 4096 + column;
    const std::unordered_map<int64_t, double>::const_iterator found =
        cells->find(key);
    if (found != cells->end()) {
      return found->second;
    }
    const double n =
        geoid->undulationM((static_cast<double>(row) + 0.5) / 4.0,
                           (static_cast<double>(column) + 0.5) / 4.0);
    cells->emplace(key, n);
    return n;
  };
  return settings;
}

AircraftSet
AircraftLayers::readRecording(const AircraftSource& source,
                              const std::atomic<bool>& cancelled,
                              const std::shared_ptr<const GeoidModel>& geoid)
{
  AircraftSet set;
  AircraftTrackBuilder builder(heightSettings(geoid));
  set.read = readAdsbFile(
      source.path, source.format, source.readOptions(),
      [&builder](const AircraftReport& report) { builder.add(report); },
      [&cancelled](qint64) { return !cancelled.load(); });
  for (AircraftTrack& track : builder.finish()) {
    if (!track.track.isEmpty()) {
      set.tracks.push_back(
          std::make_shared<const AircraftTrack>(std::move(track)));
    }
  }
  summarise(set);
  if (!set.read.error.isEmpty() && set.tracks.empty()) {
    set.state = AircraftSet::State::Failed;
    set.error = set.read.error;
  } else if (set.tracks.empty()) {
    set.state = AircraftSet::State::Failed;
    set.error = QCoreApplication::translate(
        "ts::AircraftLayers", "No aircraft positions were found.");
  } else {
    set.state = AircraftSet::State::Ready;
  }
  return set;
}

void AircraftLayers::startLoad(const QString& key, const QJsonObject& json)
{
  const std::optional<AircraftSource> source = AircraftSource::fromJson(json);
  if (!source) {
    std::shared_ptr<AircraftSet> failed = std::make_shared<AircraftSet>();
    failed->state = AircraftSet::State::Failed;
    failed->error = tr("This isn't an aircraft layer's source.");
    finishLoad(key, failed);
    return;
  }
  if (source->origin == AircraftSource::Origin::Receiver) {
    startLive(key, json, *source);
    return;
  }
  // Keep showing what was there while it loads again.
  const std::shared_ptr<const AircraftSet> previous = set(json);
  std::shared_ptr<AircraftSet> loading =
      previous ? std::make_shared<AircraftSet>(*previous)
               : std::make_shared<AircraftSet>();
  loading->state = AircraftSet::State::Loading;
  m_sets[key] = loading;
  const std::shared_ptr<std::atomic<bool>> cancelled = m_cancelled;
  std::shared_ptr<const GeoidModel> geoid = m_geoid ? m_geoid() : nullptr;
  m_pool.start([this, key, what = *source, cancelled, geoid]() {
    std::shared_ptr<const AircraftSet> result =
        std::make_shared<const AircraftSet>(
            readRecording(what, *cancelled, geoid));
    // Handed over under our own lock, then a call by name, made and freed
    // inside Qt (see SatelliteLayers). The destructor waits for the pool,
    // so this outlives the task.
    {
      const std::lock_guard<std::mutex> lock(m_finishedMutex);
      m_finished.push_back(FinishedLoad{key, std::move(result)});
    }
    QMetaObject::invokeMethod(this, "takeFinishedLoads", Qt::QueuedConnection);
  });
}

void AircraftLayers::takeFinishedLoads()
{
  std::vector<FinishedLoad> finished;
  {
    const std::lock_guard<std::mutex> lock(m_finishedMutex);
    finished.swap(m_finished);
  }
  for (FinishedLoad& load : finished) {
    finishLoad(load.key, std::move(load.set));
  }
}

void AircraftLayers::finishLoad(const QString& key,
                                std::shared_ptr<const AircraftSet> set)
{
  m_sets[key] = std::move(set);
  QJsonParseError error{};
  const QJsonObject source =
      QJsonDocument::fromJson(key.toUtf8(), &error).object();
  rebuildTrails();
  updateDraws();
  emit setChanged(source);
}

void AircraftLayers::startLive(const QString& key, const QJsonObject& json,
                               const AircraftSource& source)
{
  stopLive(key);
  Live& live = m_live[key];
  live.source = json;
  live.builder = std::make_unique<AircraftTrackBuilder>(
      heightSettings(m_geoid ? m_geoid() : nullptr));
  live.feed = new ReceiverFeed(
      source,
      source.protocol == ReceiverProtocol::Json && m_network ? m_network()
                                                             : nullptr,
      this);
  std::shared_ptr<AircraftSet> empty = std::make_shared<AircraftSet>();
  empty->state = AircraftSet::State::Ready;
  empty->live = true;
  empty->feed = ReceiverFeed::State::Connecting;
  m_sets[key] = empty;
  ReceiverFeed* feed = live.feed;
  connect(feed, &ReceiverFeed::reportsReady, this, [this, key]() {
    const std::map<QString, Live>::iterator found = m_live.find(key);
    if (found == m_live.end()) {
      return;
    }
    for (const AircraftReport& report : found->second.feed->takeReports()) {
      found->second.builder->add(report);
    }
  });
  connect(feed, &ReceiverFeed::stateChanged, this, [this, key, feed]() {
    const std::shared_ptr<const AircraftSet> old = m_sets[key];
    std::shared_ptr<AircraftSet> changed =
        old ? std::make_shared<AircraftSet>(*old)
            : std::make_shared<AircraftSet>();
    changed->live = true;
    changed->feed = feed->state();
    changed->error = feed->state() == ReceiverFeed::State::Reconnecting
                         ? feed->errorString()
                         : QString();
    m_sets[key] = changed;
    const std::map<QString, Live>::const_iterator found = m_live.find(key);
    if (found != m_live.end()) {
      emit setChanged(found->second.source);
    }
  });
  feed->start();
  m_publish->start();
}

void AircraftLayers::stopLive(const QString& key)
{
  const std::map<QString, Live>::iterator found = m_live.find(key);
  if (found == m_live.end()) {
    return;
  }
  found->second.feed->disconnect(this);
  found->second.feed->stop();
  found->second.feed->deleteLater();
  m_live.erase(found);
  m_sets.erase(key);
  if (m_live.empty()) {
    m_publish->stop();
  }
}

void AircraftLayers::publishLive()
{
  bool changed = false;
  const double now =
      static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
  for (std::pair<const QString, Live>& entry : m_live) {
    Live& live = entry.second;
    live.builder->forgetBefore(now - liveForgetS);
    std::vector<AircraftTrack> tracks = live.builder->buildChanged();
    if (tracks.empty()) {
      continue;
    }
    const std::shared_ptr<const AircraftSet> old = m_sets[entry.first];
    std::shared_ptr<AircraftSet> updated =
        old ? std::make_shared<AircraftSet>(*old)
            : std::make_shared<AircraftSet>();
    std::map<uint32_t, std::shared_ptr<const AircraftTrack>> byAddress;
    for (const std::shared_ptr<const AircraftTrack>& track : updated->tracks) {
      if (track->track.lastTimeS() >= now - liveForgetS) {
        byAddress[track->address] = track;
      }
    }
    for (AircraftTrack& track : tracks) {
      if (!track.track.isEmpty()) {
        const uint32_t address = track.address;
        byAddress[address] =
            std::make_shared<const AircraftTrack>(std::move(track));
      }
    }
    updated->tracks.clear();
    for (std::pair<const uint32_t, std::shared_ptr<const AircraftTrack>>& t :
         byAddress) {
      updated->tracks.push_back(std::move(t.second));
    }
    summarise(*updated);
    m_sets[entry.first] = updated;
    changed = true;
  }
  if (changed) {
    rebuildTrails();
    updateDraws();
  }
}

void AircraftLayers::select(const QString& nodeId,
                            std::optional<uint32_t> address)
{
  m_selectedLayer = address ? nodeId : QString();
  m_selected = address;
  rebuildTrails();
  updateDraws();
  emit selectionChanged();
}

std::optional<DrawnAircraft> AircraftLayers::selected() const
{
  if (!m_selected) {
    return std::nullopt;
  }
  for (const DrawnAircraft& aircraft : m_drawn) {
    if (aircraft.nodeId == m_selectedLayer &&
        aircraft.track->address == *m_selected) {
      return aircraft;
    }
  }
  return std::nullopt;
}

QString AircraftLayers::labelText(const DrawnAircraft& aircraft,
                                  const AircraftStyle& style)
{
  QString text = aircraft.info.callsign.isEmpty()
                     ? formatIcaoAddress(aircraft.track->address)
                     : aircraft.info.callsign;
  const double altitudeFt = altitudeFtOf(aircraft.altitudes, aircraft.at);
  if (style.labelAltitude) {
    if (aircraft.info.onGround.value_or(false) && std::isnan(altitudeFt)) {
      text += QLatin1Char(' ') + tr("GND");
    } else if (!std::isnan(altitudeFt)) {
      text += QLatin1Char(' ') +
              tr("%1 ft").arg(QLocale().toString(std::lround(altitudeFt)));
    }
  }
  if (style.labelSpeed && !std::isnan(aircraft.at.speedMS)) {
    text +=
        QLatin1Char(' ') +
        tr("%1 kt").arg(std::lround(static_cast<double>(aircraft.at.speedMS) /
                                    knotsToMetresPerSecond));
  }
  return text;
}

namespace
{

/**
 * Returns aircraft @p track of layer @p nodeId where it is at @p timeS,
 * with what is known of it then; nothing if it isn't shown then.
 */
std::optional<DrawnAircraft>
placeAircraft(const QString& nodeId,
              const std::shared_ptr<const AircraftTrack>& track, double timeS,
              TrackTiming timing)
{
  const std::optional<TrackSample> at = track->track.at(timeS, timing);
  if (!at) {
    return std::nullopt;
  }
  DrawnAircraft aircraft;
  aircraft.nodeId = nodeId;
  aircraft.track = track;
  aircraft.at = *at;
  const std::optional<std::size_t> index = track->track.indexAtOrBefore(timeS);
  if (index) {
    aircraft.altitudes = track->altitudes.at(*index);
    aircraft.lastReportS = track->track.points().at(*index).timeS;
  }
  const AircraftInfo* info = track->infoAt(timeS);
  if (info != nullptr) {
    aircraft.info = *info;
  }
  return aircraft;
}

} // namespace

void AircraftLayers::drawAircraft(const DrawnAircraft& aircraft,
                                  const AircraftStyle& style, double alpha,
                                  bool selected)
{
  const std::array<std::uint8_t, 4> rgba = rgbaOf(style.color, alpha);
  const TrackSample& at = aircraft.at;
  const double scale = selected ? 1.4 : 1.0;
  m_markers.push_back(
      aircraftMarker(at, 0.0, rgba, 0.5 * style.markerSizePx * scale));
  if (style.dropLines && !std::isnan(at.heightM)) {
    m_dropLines.push_back(dropLine(at, 0.0, rgba));
  }
  if (!style.labels && !selected) {
    return;
  }
  BillboardDraw label;
  label.latDeg = at.latDeg;
  label.lonDeg = at.lonDeg;
  label.heightM = std::isnan(at.heightM) ? 0.0 : at.heightM;
  label.height = OverlayHeight::Absolute;
  label.sizePx = style.markerSizePx * scale;
  label.label = labelText(aircraft, style);
  QColor labelColor = style.color.lighter(130);
  labelColor.setAlpha(
      static_cast<int32_t>(std::lround(labelColor.alpha() * alpha)));
  label.labelColor = labelColor;
  label.labelScale = selected ? 1.0 : 0.85;
  m_labels.push_back(std::move(label));
}

void AircraftLayers::updateDraws()
{
  m_timeS = static_cast<double>(m_clock->time().toMSecsSinceEpoch()) / 1000.0;
  m_drawn.clear();
  m_markers.clear();
  m_labels.clear();
  m_dropLines.clear();
  bool any = false;
  for (const AircraftLayerEntry& layer : m_layers) {
    const std::shared_ptr<const AircraftSet> data =
        set(layer.definition.source);
    any = any || (data && (data->live || !data->tracks.empty()));
    if (!data) {
      continue;
    }
    const AircraftStyle style = AircraftStyle::fromJson(layer.definition.style);
    for (const std::shared_ptr<const AircraftTrack>& track : data->tracks) {
      std::optional<DrawnAircraft> aircraft =
          placeAircraft(layer.nodeId, track, m_timeS, style.timing);
      if (!aircraft) {
        continue;
      }
      const TrackSample& at = aircraft->at;
      const bool selected = m_selected && *m_selected == track->address &&
                            m_selectedLayer == layer.nodeId;
      if (selected ||
          style.shows(track->address, aircraft->info.callsign, at.latDeg,
                      at.lonDeg, altitudeFtOf(aircraft->altitudes, at))) {
        drawAircraft(*aircraft, style, layer.alpha, selected);
        m_drawn.push_back(std::move(*aircraft));
      }
    }
  }
  if (m_clock) {
    m_clock->setTicking(this, any);
  }
  emit drawsChanged();
}

void AircraftLayers::rebuildTrails()
{
  m_trails.clear();
  for (const AircraftLayerEntry& layer : m_layers) {
    const std::shared_ptr<const AircraftSet> data =
        set(layer.definition.source);
    if (!data) {
      continue;
    }
    const AircraftStyle style = AircraftStyle::fromJson(layer.definition.style);
    const std::array<std::uint8_t, 4> rgba = rgbaOf(style.color, layer.alpha);
    TrailLook look;
    look.rgba = rgba;
    look.widthPx = 2.0F;
    const TrailColouring colouring = style.colouring;
    const double alpha = layer.alpha;
    if (colouring != TrailColouring::Aircraft) {
      look.colourOf = [colouring, rgba, alpha](const TrackSample& p) {
        std::array<std::uint8_t, 4> c = trailColour(p, colouring, rgba);
        c[3] = static_cast<std::uint8_t>(std::lround(c[3] * alpha));
        return c;
      };
    }
    for (const std::shared_ptr<const AircraftTrack>& track : data->tracks) {
      const bool selected = m_selected && *m_selected == track->address &&
                            m_selectedLayer == layer.nodeId;
      // Trails follow the filters that don't move with the clock; those
      // that do (altitude, area) hide only the markers.
      AircraftStyle fixed = style;
      fixed.minimumAltitudeFt.reset();
      fixed.maximumAltitudeFt.reset();
      fixed.area.reset();
      const AircraftInfo* info = track->infoAt(m_timeS);
      if (!selected &&
          !fixed.shows(track->address,
                       info != nullptr ? info->callsign : QString(), 0.0, 0.0,
                       std::numeric_limits<double>::quiet_NaN())) {
        continue;
      }
      look.widthPx = selected ? 3.0F : 2.0F;
      for (SpaceLine& line : trailLines(track->track, style.timing,
                                        style.trailOf(track->address), look)) {
        m_trails.push_back(std::move(line));
      }
    }
  }
  emit trailsChanged();
}

QString AircraftLayers::timeExtent() const
{
  QStringList parts;
  for (const AircraftLayerEntry& layer : m_layers) {
    const std::shared_ptr<const AircraftSet> data =
        set(layer.definition.source);
    if (!data || data->live || data->firstS <= 0.0 ||
        data->lastS <= data->firstS) {
      continue;
    }
    const QDateTime first = QDateTime::fromMSecsSinceEpoch(
        std::llround(data->firstS * 1000.0), QTimeZone::utc());
    const QDateTime last = QDateTime::fromMSecsSinceEpoch(
        std::llround(data->lastS * 1000.0), QTimeZone::utc());
    const int64_t step =
        std::max<int64_t>(1, static_cast<int64_t>(first.secsTo(last) / 600));
    parts << QStringLiteral("%1/%2/PT%3S")
                 .arg(first.toString(Qt::ISODate), last.toString(Qt::ISODate))
                 .arg(step);
  }
  return parts.join(QLatin1Char(','));
}

} // namespace ts
