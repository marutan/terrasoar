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
#include "vessellayers.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <utility>

#include <QCoreApplication>
#include <QDateTime>
#include <QJsonDocument>
#include <QLocale>
#include <QTimeZone>
#include <QTimer>

#include "ts/ais/vesseldraws.h"
#include "ts/elevation/geoidmodel.h"
#include "ts/network/networkclient.h"
#include "ts/time/programclock.h"
#include "ts/tracks/trackdraws.h"

namespace ts
{

namespace
{

/** How often live tracks are rebuilt from the reports, milliseconds. */
constexpr int32_t publishMs = 2000;

/** Live stations not heard for this long are forgotten, seconds. */
constexpr double liveForgetS = 6.0 * 3600.0;

/** Knots in metres a second. */
constexpr double knot = 1852.0 / 3600.0;

std::array<std::uint8_t, 4> rgbaOf(const QColor& color, double alpha)
{
  return {static_cast<std::uint8_t>(color.red()),
          static_cast<std::uint8_t>(color.green()),
          static_cast<std::uint8_t>(color.blue()),
          static_cast<std::uint8_t>(
              std::lround(color.alpha() * std::clamp(alpha, 0.0, 1.0)))};
}

void summarise(VesselSet& set)
{
  set.firstS = 0.0;
  set.lastS = 0.0;
  for (const std::shared_ptr<const VesselTrack>& track : set.tracks) {
    if (track->track.isEmpty()) {
      continue;
    }
    set.firstS = set.firstS == 0.0
                     ? track->track.firstTimeS()
                     : std::min(set.firstS, track->track.firstTimeS());
    set.lastS = std::max(set.lastS, track->track.lastTimeS());
  }
}

/** True if @p a and @p b draw the same. */
bool sameLayers(const std::vector<VesselLayerEntry>& a,
                const std::vector<VesselLayerEntry>& b)
{
  return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                    [](const VesselLayerEntry& x, const VesselLayerEntry& y) {
                      return x.nodeId == y.nodeId &&
                             x.definition.source == y.definition.source &&
                             x.definition.style == y.definition.style &&
                             x.alpha == y.alpha;
                    });
}

/**
 * Station @p track of layer @p nodeId where it is at @p timeS; nothing if
 * it isn't shown then (stale by its class, speed and status).
 */
std::optional<DrawnVessel>
placeVessel(const QString& nodeId,
            const std::shared_ptr<const VesselTrack>& track, double timeS,
            const VesselStyle& style)
{
  const std::optional<std::size_t> index = track->track.indexAtOrBefore(timeS);
  if (!index) {
    return std::nullopt;
  }
  const TrackSample& last = track->track.points().at(*index);
  const VesselInfo* info = track->infoAt(timeS);
  const VesselInfo known = info != nullptr ? *info : VesselInfo();
  TrackTiming timing;
  timing.gapS = style.gapS;
  timing.staleS = vesselStaleS(known, static_cast<double>(last.speedMS) / knot,
                               style.staleMinimumS);
  const std::optional<TrackSample> at = track->track.at(timeS, timing);
  if (!at) {
    return std::nullopt;
  }
  DrawnVessel vessel;
  vessel.nodeId = nodeId;
  vessel.track = track;
  vessel.at = *at;
  vessel.motion = track->motion.at(*index);
  vessel.info = known;
  vessel.lastReportS = last.timeS;
  return vessel;
}

} // namespace

std::shared_ptr<const VesselTrack> VesselSet::find(uint32_t mmsi) const
{
  const std::vector<std::shared_ptr<const VesselTrack>>::const_iterator found =
      std::lower_bound(tracks.begin(), tracks.end(), mmsi,
                       [](const std::shared_ptr<const VesselTrack>& t,
                          uint32_t m) { return t->mmsi < m; });
  return found != tracks.end() && (*found)->mmsi == mmsi ? *found : nullptr;
}

VesselLayers::VesselLayers(ProgramClock* clock, NetworkSource network,
                           GeoidSource geoid, QObject* parent)
    : QObject(parent), m_clock(clock), m_network(std::move(network)),
      m_geoid(std::move(geoid)), m_publish(new QTimer(this)),
      m_cancelled(std::make_shared<std::atomic<bool>>(false))
{
  m_pool.setMaxThreadCount(2);
  m_publish->setInterval(publishMs);
  connect(m_publish, &QTimer::timeout, this, &VesselLayers::publishLive);
  connect(m_clock, &ProgramClock::timeChanged, this,
          &VesselLayers::updateDraws);
}

VesselLayers::~VesselLayers()
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

QString VesselLayers::keyOf(const QJsonObject& source)
{
  return QString::fromUtf8(
      QJsonDocument(source).toJson(QJsonDocument::Compact));
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
double VesselLayers::seaLevelM(double latDeg, double lonDeg)
{
  if (!m_seaLevelAsked) {
    m_seaLevelAsked = true;
    m_seaLevel = m_geoid ? m_geoid() : nullptr;
  }
  if (!m_seaLevel || !m_seaLevel->isValid()) {
    return 0.0;
  }
  // The geoid changes slowly: a value for each quarter degree.
  const int64_t row = static_cast<int64_t>(std::floor(latDeg * 4.0));
  const int64_t column = static_cast<int64_t>(std::floor(lonDeg * 4.0));
  const int64_t key = row * 4096 + column;
  const std::unordered_map<int64_t, double>::const_iterator found =
      m_seaLevelCells.find(key);
  if (found != m_seaLevelCells.end()) {
    return found->second;
  }
  double n = m_seaLevel->undulationM((static_cast<double>(row) + 0.5) / 4.0,
                                     (static_cast<double>(column) + 0.5) / 4.0);
  if (std::isnan(n)) {
    n = 0.0;
  }
  m_seaLevelCells.emplace(key, n);
  return n;
}

void VesselLayers::setLayers(std::vector<VesselLayerEntry> layers)
{
  // Set again whenever the tree is drawn: trails are rebuilt only if the
  // layers changed.
  const bool changed = !sameLayers(m_layers, layers);
  m_layers = std::move(layers);
  std::set<QString> wanted;
  for (const VesselLayerEntry& layer : m_layers) {
    wanted.insert(keyOf(layer.definition.source));
    load(layer.definition.source);
  }
  std::vector<QString> idle;
  for (const std::pair<const QString, Live>& live : m_live) {
    if (wanted.count(live.first) == 0) {
      idle.push_back(live.first);
    }
  }
  for (const QString& key : idle) {
    stopLive(key);
  }
  if (changed) {
    rebuildTrails();
    updateDraws();
  }
}

std::shared_ptr<const VesselSet>
VesselLayers::set(const QJsonObject& source) const
{
  const std::map<QString, std::shared_ptr<const VesselSet>>::const_iterator
      found = m_sets.find(keyOf(source));
  return found != m_sets.end() ? found->second : nullptr;
}

void VesselLayers::load(const QJsonObject& source)
{
  const QString key = keyOf(source);
  if (m_sets.find(key) == m_sets.end()) {
    startLoad(key, source);
  }
}

void VesselLayers::reload(const QJsonObject& source)
{
  const QString key = keyOf(source);
  const std::shared_ptr<const VesselSet> old = set(source);
  if (old && old->state == VesselSet::State::Loading) {
    return;
  }
  stopLive(key);
  startLoad(key, source);
}

bool VesselLayers::isBusy() const
{
  return std::any_of(
      m_sets.begin(), m_sets.end(),
      [](const std::pair<const QString, std::shared_ptr<const VesselSet>>&
             entry) {
        return entry.second->state == VesselSet::State::Loading;
      });
}

VesselSet VesselLayers::readRecording(const VesselSource& source,
                                      const std::atomic<bool>& cancelled)
{
  VesselSet set;
  VesselTrackBuilder builder;
  set.read = readAisFile(
      source.path, source.format, source.readOptions(),
      [&builder](const VesselReport& report) { builder.add(report); },
      [&cancelled](qint64) { return !cancelled.load(); });
  for (VesselTrack& track : builder.finish()) {
    if (!track.track.isEmpty()) {
      set.tracks.push_back(
          std::make_shared<const VesselTrack>(std::move(track)));
    }
  }
  summarise(set);
  if (set.tracks.empty()) {
    set.state = VesselSet::State::Failed;
    set.error = !set.read.error.isEmpty()
                    ? set.read.error
                    : QCoreApplication::translate("ts::VesselLayers",
                                                  "No positions were found.");
  } else {
    set.state = VesselSet::State::Ready;
  }
  return set;
}

void VesselLayers::startLoad(const QString& key, const QJsonObject& json)
{
  const std::optional<VesselSource> source = VesselSource::fromJson(json);
  if (!source) {
    std::shared_ptr<VesselSet> failed = std::make_shared<VesselSet>();
    failed->state = VesselSet::State::Failed;
    failed->error = tr("This isn't a ship layer's source.");
    finishLoad(key, failed);
    return;
  }
  if (source->origin != VesselSource::Origin::File) {
    startLive(key, json, *source);
    return;
  }
  const std::shared_ptr<const VesselSet> previous = set(json);
  std::shared_ptr<VesselSet> loading =
      previous ? std::make_shared<VesselSet>(*previous)
               : std::make_shared<VesselSet>();
  loading->state = VesselSet::State::Loading;
  m_sets[key] = loading;
  const std::shared_ptr<std::atomic<bool>> cancelled = m_cancelled;
  m_pool.start([this, key, what = *source, cancelled]() {
    std::shared_ptr<const VesselSet> result =
        std::make_shared<const VesselSet>(readRecording(what, *cancelled));
    // Handed over under our own lock, then a call by name (see
    // SatelliteLayers); the destructor waits for the pool.
    {
      const std::lock_guard<std::mutex> lock(m_finishedMutex);
      m_finished.push_back(FinishedLoad{key, std::move(result)});
    }
    QMetaObject::invokeMethod(this, "takeFinishedLoads", Qt::QueuedConnection);
  });
}

void VesselLayers::takeFinishedLoads()
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

void VesselLayers::finishLoad(const QString& key,
                              std::shared_ptr<const VesselSet> set)
{
  m_sets[key] = std::move(set);
  const QJsonObject source = QJsonDocument::fromJson(key.toUtf8()).object();
  rebuildTrails();
  updateDraws();
  emit setChanged(source);
}

void VesselLayers::startLive(const QString& key, const QJsonObject& json,
                             const VesselSource& source)
{
  stopLive(key);
  Live& live = m_live[key];
  live.source = json;
  live.builder = std::make_unique<VesselTrackBuilder>();
  live.feed = new AisFeed(source, m_network ? m_network() : nullptr, this);
  std::shared_ptr<VesselSet> empty = std::make_shared<VesselSet>();
  empty->state = VesselSet::State::Ready;
  empty->live = true;
  empty->feed = AisFeed::State::Connecting;
  m_sets[key] = empty;
  AisFeed* feed = live.feed;
  connect(feed, &AisFeed::reportsReady, this, [this, key]() {
    const std::map<QString, Live>::iterator found = m_live.find(key);
    if (found == m_live.end()) {
      return;
    }
    for (const VesselReport& report : found->second.feed->takeReports()) {
      found->second.builder->add(report);
    }
  });
  connect(feed, &AisFeed::stateChanged, this, [this, key, feed]() {
    const std::shared_ptr<const VesselSet> old = m_sets[key];
    std::shared_ptr<VesselSet> changed =
        old ? std::make_shared<VesselSet>(*old) : std::make_shared<VesselSet>();
    changed->live = true;
    changed->feed = feed->state();
    changed->error = feed->state() == AisFeed::State::Reconnecting
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

void VesselLayers::stopLive(const QString& key)
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

void VesselLayers::publishLive()
{
  bool changed = false;
  const double now =
      static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
  for (std::pair<const QString, Live>& entry : m_live) {
    Live& live = entry.second;
    live.builder->forgetBefore(now - liveForgetS);
    std::vector<VesselTrack> tracks = live.builder->buildChanged();
    if (tracks.empty()) {
      continue;
    }
    const std::shared_ptr<const VesselSet> old = m_sets[entry.first];
    std::shared_ptr<VesselSet> updated =
        old ? std::make_shared<VesselSet>(*old) : std::make_shared<VesselSet>();
    std::map<uint32_t, std::shared_ptr<const VesselTrack>> byMmsi;
    for (const std::shared_ptr<const VesselTrack>& track : updated->tracks) {
      if (track->track.isEmpty() ||
          track->track.lastTimeS() >= now - liveForgetS) {
        byMmsi[track->mmsi] = track;
      }
    }
    for (VesselTrack& track : tracks) {
      const uint32_t mmsi = track.mmsi;
      byMmsi[mmsi] = std::make_shared<const VesselTrack>(std::move(track));
    }
    updated->tracks.clear();
    for (std::pair<const uint32_t, std::shared_ptr<const VesselTrack>>& t :
         byMmsi) {
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

void VesselLayers::select(const QString& nodeId, std::optional<uint32_t> mmsi)
{
  m_selectedLayer = mmsi ? nodeId : QString();
  m_selected = mmsi;
  rebuildTrails();
  updateDraws();
  emit selectionChanged();
}

std::optional<DrawnVessel> VesselLayers::selected() const
{
  if (!m_selected) {
    return std::nullopt;
  }
  for (const DrawnVessel& vessel : m_drawn) {
    if (vessel.nodeId == m_selectedLayer && vessel.track->mmsi == *m_selected) {
      return vessel;
    }
  }
  return std::nullopt;
}

QString VesselLayers::labelText(const DrawnVessel& vessel,
                                const VesselStyle& style)
{
  QString text = vessel.info.name.isEmpty()
                     ? QString::number(vessel.track->mmsi)
                     : vessel.info.name;
  if (style.labelSpeed && !std::isnan(vessel.at.speedMS)) {
    text += QLatin1Char(' ') +
            tr("%1 kn").arg(QLocale().toString(
                static_cast<double>(vessel.at.speedMS) / knot, 'f', 1));
  }
  if (style.labelDestination && !vessel.info.destination.isEmpty()) {
    text += QStringLiteral(" → ") + vessel.info.destination;
  }
  return text;
}

QColor VesselLayers::markerColour(const VesselInfo& info,
                                  const VesselStyle& style)
{
  if (!style.colourByType) {
    return style.color;
  }
  const std::array<std::uint8_t, 4> c =
      shipTypeColour(VesselStyle::groupOf(info));
  return QColor(c[0], c[1], c[2], style.color.alpha());
}

void VesselLayers::drawVessel(const DrawnVessel& vessel,
                              const VesselStyle& style, double alpha,
                              bool selected)
{
  const QColor colour = markerColour(vessel.info, style);
  const std::array<std::uint8_t, 4> rgba = rgbaOf(colour, alpha);
  const double sea = seaLevelM(vessel.at.latDeg, vessel.at.lonDeg);
  const double scale = selected ? 1.4 : 1.0;
  m_markers.push_back(vesselMarker(vessel.at, vessel.motion, vessel.info, sea,
                                   rgba, 0.5 * style.markerSizePx * scale));
  if (style.courseVectorMinutes > 0.0) {
    SpaceLine vector =
        courseVector(vessel.at, style.courseVectorMinutes, sea, rgba);
    if (!vector.points.empty()) {
      m_courseVectors.push_back(std::move(vector));
    }
  }
  if (!style.labels && !selected) {
    return;
  }
  BillboardDraw label;
  label.latDeg = vessel.at.latDeg;
  label.lonDeg = vessel.at.lonDeg;
  label.heightM = std::isnan(vessel.at.heightM) ? sea : vessel.at.heightM;
  label.height = OverlayHeight::Absolute;
  label.sizePx = style.markerSizePx * scale;
  label.label = labelText(vessel, style);
  QColor labelColor = colour.lighter(130);
  labelColor.setAlpha(
      static_cast<int32_t>(std::lround(labelColor.alpha() * alpha)));
  label.labelColor = labelColor;
  label.labelScale = selected ? 1.0 : 0.85;
  m_labels.push_back(std::move(label));
}

void VesselLayers::updateDraws()
{
  m_timeS = m_clock ? static_cast<double>(m_clock->time().toMSecsSinceEpoch()) /
                          1000.0
                    : 0.0;
  m_drawn.clear();
  m_markers.clear();
  m_labels.clear();
  m_courseVectors.clear();
  bool any = false;
  for (const VesselLayerEntry& layer : m_layers) {
    const std::shared_ptr<const VesselSet> data = set(layer.definition.source);
    any = any || (data && (data->live || !data->tracks.empty()));
    if (!data) {
      continue;
    }
    const VesselStyle style = VesselStyle::fromJson(layer.definition.style);
    for (const std::shared_ptr<const VesselTrack>& track : data->tracks) {
      std::optional<DrawnVessel> vessel =
          placeVessel(layer.nodeId, track, m_timeS, style);
      if (!vessel) {
        continue;
      }
      const bool selected = m_selected && *m_selected == track->mmsi &&
                            m_selectedLayer == layer.nodeId;
      if (selected || style.shows(track->mmsi, vessel->info, vessel->at)) {
        drawVessel(*vessel, style, layer.alpha, selected);
        m_drawn.push_back(std::move(*vessel));
      }
    }
  }
  if (m_clock) {
    m_clock->setTicking(this, any);
  }
  emit drawsChanged();
}

TrailLook VesselLayers::trailLook(const VesselTrack& track,
                                  const VesselInfo& known,
                                  const VesselStyle& style, double alpha,
                                  bool selected)
{
  TrailLook look;
  look.rgba = rgbaOf(style.trailColouring == VesselColouring::Type
                         ? markerColour(known, VesselStyle())
                         : style.color,
                     alpha);
  look.widthPx = selected ? 3.0F : 2.0F;
  if (!track.track.isEmpty()) {
    look.groundHeightM = seaLevelM(track.track.points().front().latDeg,
                                   track.track.points().front().lonDeg);
  }
  if (style.trailColouring == VesselColouring::Speed) {
    look.colourOf = [alpha](const TrackSample& p) {
      std::array<std::uint8_t, 4> c =
          speedColour(static_cast<double>(p.speedMS));
      c[3] = static_cast<std::uint8_t>(std::lround(c[3] * alpha));
      return c;
    };
  }
  return look;
}

void VesselLayers::rebuildTrails()
{
  m_trails.clear();
  for (const VesselLayerEntry& layer : m_layers) {
    const std::shared_ptr<const VesselSet> data = set(layer.definition.source);
    if (!data) {
      continue;
    }
    const VesselStyle style = VesselStyle::fromJson(layer.definition.style);
    TrackTiming timing;
    timing.gapS = style.gapS;
    // Trails follow the filters that don't move with the clock.
    VesselStyle fixed = style;
    fixed.statuses.clear();
    fixed.minimumSpeedKn.reset();
    fixed.maximumSpeedKn.reset();
    fixed.area.reset();
    for (const std::shared_ptr<const VesselTrack>& track : data->tracks) {
      const bool selected = m_selected && *m_selected == track->mmsi &&
                            m_selectedLayer == layer.nodeId;
      const VesselInfo* info = track->infoAt(m_timeS);
      const VesselInfo known = info != nullptr ? *info : VesselInfo();
      if (!selected && !fixed.shows(track->mmsi, known, TrackSample())) {
        continue;
      }
      const TrailLook look =
          trailLook(*track, known, style, layer.alpha, selected);
      for (SpaceLine& line :
           trailLines(track->track, timing, style.trailOf(track->mmsi), look)) {
        m_trails.push_back(std::move(line));
      }
    }
  }
  emit trailsChanged();
}

QString VesselLayers::timeExtent() const
{
  QStringList parts;
  for (const VesselLayerEntry& layer : m_layers) {
    const std::shared_ptr<const VesselSet> data = set(layer.definition.source);
    if (!data || data->live || data->firstS <= 0.0 ||
        data->lastS <= data->firstS) {
      continue;
    }
    const QDateTime first =
        QDateTime::fromMSecsSinceEpoch(std::llround(data->firstS * 1000.0))
            .toUTC();
    const QDateTime last =
        QDateTime::fromMSecsSinceEpoch(std::llround(data->lastS * 1000.0))
            .toUTC();
    const int64_t step =
        std::max<int64_t>(1, static_cast<int64_t>(first.secsTo(last) / 600));
    parts << QStringLiteral("%1/%2/PT%3S")
                 .arg(first.toString(Qt::ISODate), last.toString(Qt::ISODate))
                 .arg(step);
  }
  return parts.join(QLatin1Char(','));
}

} // namespace ts
