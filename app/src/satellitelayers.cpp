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
#include "satellitelayers.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

#include "ts/network/networkclient.h"
#include "ts/orbits/satellitedraws.h"
#include "ts/time/programclock.h"

namespace ts
{

namespace
{

std::array<std::uint8_t, 4> rgbaOf(const QColor& color, double alpha)
{
  return {static_cast<std::uint8_t>(color.red()),
          static_cast<std::uint8_t>(color.green()),
          static_cast<std::uint8_t>(color.blue()),
          static_cast<std::uint8_t>(
              std::lround(std::clamp(color.alpha() * alpha, 0.0, 255.0)))};
}

/** Old elements: their colour faded to grey. */
QColor oldColor(const QColor& color)
{
  // Integer forms: the float ones differ between Qt 5 and Qt 6.
  return QColor::fromHsv(std::max(color.hsvHue(), 0), color.hsvSaturation() / 5,
                         191, color.alpha());
}

std::shared_ptr<const SatelliteSet> setFromElements(const ElementSetList& list)
{
  std::shared_ptr<SatelliteSet> set = std::make_shared<SatelliteSet>();
  set->problems = list.errors;
  set->satellites.reserve(list.elements.size());
  for (const OrbitalElements& e : list.elements) {
    set->satellites.push_back(std::make_shared<const Sgp4>(e));
  }
  set->state = set->satellites.empty() ? SatelliteSet::State::Failed
                                       : SatelliteSet::State::Ready;
  return set;
}

} // namespace

std::shared_ptr<const Sgp4> SatelliteSet::find(int32_t catalogueNumber) const
{
  for (const std::shared_ptr<const Sgp4>& s : satellites) {
    if (s->elements().catalogueNumber == catalogueNumber) {
      return s;
    }
  }
  return nullptr;
}

SatelliteLayers::SatelliteLayers(ProgramClock* clock, NetworkSource network,
                                 const QString& cacheDirectory, QObject* parent)
    : QObject(parent), m_clock(clock), m_network(std::move(network)),
      m_cancelled(std::make_shared<std::atomic<bool>>(false)),
      m_computer(new OrbitComputer(this))
{
  m_download.cacheDirectory = cacheDirectory;
  m_pool.setMaxThreadCount(2);
  connect(m_clock, &ProgramClock::timeChanged, this,
          &SatelliteLayers::requestPositions);
  connect(m_computer, &OrbitComputer::resultReady, this,
          &SatelliteLayers::positionsReady);
}

SatelliteLayers::~SatelliteLayers()
{
  m_cancelled->store(true);
  m_pool.waitForDone();
}

QString SatelliteLayers::keyOf(const QJsonObject& source)
{
  return QString::fromUtf8(
      QJsonDocument(source).toJson(QJsonDocument::Compact));
}

double SatelliteLayers::ageDays(const OrbitalElements& elements,
                                const QDateTime& at)
{
  return elements.ageDays(at);
}

std::shared_ptr<const SatelliteSet>
SatelliteLayers::set(const QJsonObject& source) const
{
  const std::map<QString, std::shared_ptr<const SatelliteSet>>::const_iterator
      it = m_sets.find(keyOf(source));
  return it == m_sets.end() ? nullptr : it->second;
}

SatelliteSet SatelliteLayers::readLocal(const SatelliteSource& source)
{
  QByteArray data;
  QString error;
  if (source.origin == SatelliteSource::Origin::Text) {
    data = source.text.toUtf8();
  } else {
    QFile file(source.path);
    if (file.open(QIODevice::ReadOnly)) {
      data = file.readAll();
    } else {
      error = QCoreApplication::translate("ts::SatelliteLayers",
                                          "Cannot read %1: %2")
                  .arg(QFileInfo(source.path).fileName(), file.errorString());
    }
  }
  SatelliteSet set = *setFromElements(parseElementSets(data));
  if (!error.isEmpty()) {
    set.error = error;
  } else if (set.satellites.empty()) {
    set.error = QCoreApplication::translate(
        "ts::SatelliteLayers", "It has no element sets (TLEs or OMM JSON).");
  }
  return set;
}

void SatelliteLayers::setLayers(std::vector<SatelliteLayerEntry> layers)
{
  m_layers = std::move(layers);
  for (const SatelliteLayerEntry& layer : m_layers) {
    load(layer.definition.source);
  }
  requestPositions();
}

void SatelliteLayers::load(const QJsonObject& source)
{
  const QString key = keyOf(source);
  if (m_sets.find(key) == m_sets.end()) {
    startLoad(key, source, false);
  }
}

void SatelliteLayers::refresh(const QJsonObject& source)
{
  const QString key = keyOf(source);
  const std::shared_ptr<const SatelliteSet> old = set(source);
  if (old && old->state == SatelliteSet::State::Loading) {
    return;
  }
  startLoad(key, source, true);
}

void SatelliteLayers::startLoad(const QString& key, const QJsonObject& json,
                                bool force)
{
  const std::optional<SatelliteSource> source = SatelliteSource::fromJson(json);
  // Keep showing what was there while it loads again.
  const std::shared_ptr<const SatelliteSet> previous = set(json);
  std::shared_ptr<SatelliteSet> loading =
      previous ? std::make_shared<SatelliteSet>(*previous)
               : std::make_shared<SatelliteSet>();
  loading->state = SatelliteSet::State::Loading;
  m_sets[key] = loading;
  if (!source) {
    std::shared_ptr<SatelliteSet> failed = std::make_shared<SatelliteSet>();
    failed->state = SatelliteSet::State::Failed;
    failed->error = tr("This isn't a satellite layer's source.");
    finishLoad(key, json, failed);
    return;
  }
  std::shared_ptr<NetworkClient> network =
      source->isDownloaded() && m_network ? m_network() : nullptr;
  const ElementDownloadSettings settings = m_download;
  const std::shared_ptr<std::atomic<bool>> cancelled = m_cancelled;
  m_pool.start([this, key, json, what = *source, network, settings, force,
                cancelled]() {
    std::shared_ptr<const SatelliteSet> result;
    if (!what.isDownloaded()) {
      result = std::make_shared<const SatelliteSet>(readLocal(what));
    } else if (!network) {
      std::shared_ptr<SatelliteSet> failed = std::make_shared<SatelliteSet>();
      failed->state = SatelliteSet::State::Failed;
      failed->error =
          QCoreApplication::translate("ts::SatelliteLayers", "No network.");
      result = failed;
    } else {
      const ElementDownload download =
          downloadElementSets(*network, what.url, settings, force, *cancelled);
      std::shared_ptr<SatelliteSet> loaded =
          std::const_pointer_cast<SatelliteSet>(setFromElements(download.sets));
      loaded->error = download.errorString;
      loaded->origin = download.origin;
      loaded->downloadedS = download.downloadedS;
      result = loaded;
    }
    // Handed over under our own lock, then a call by name, made and freed
    // inside Qt: a functor's slot object would cross the threads through
    // Qt's event queue, whose locks ThreadSanitizer can't see. The
    // destructor waits for the pool, so this outlives the task.
    {
      const std::lock_guard<std::mutex> lock(m_finishedMutex);
      m_finished.push_back(FinishedLoad{key, json, result});
    }
    QMetaObject::invokeMethod(this, "takeFinishedLoads", Qt::QueuedConnection);
  });
}

void SatelliteLayers::takeFinishedLoads()
{
  std::vector<FinishedLoad> finished;
  {
    const std::lock_guard<std::mutex> lock(m_finishedMutex);
    finished.swap(m_finished);
  }
  for (FinishedLoad& load : finished) {
    finishLoad(load.key, load.source, std::move(load.set));
  }
}

void SatelliteLayers::finishLoad(const QString& key, const QJsonObject& source,
                                 std::shared_ptr<const SatelliteSet> set)
{
  m_sets[key] = std::move(set);
  emit setLoaded(source);
  requestPositions();
}

bool SatelliteLayers::isBusy() const
{
  for (const std::pair<const QString, std::shared_ptr<const SatelliteSet>>&
           entry : m_sets) {
    if (entry.second->state == SatelliteSet::State::Loading) {
      return true;
    }
  }
  return m_computer->isBusy() || !m_pending.empty();
}

void SatelliteLayers::select(const QString& nodeId, int32_t catalogueNumber)
{
  const QString layer = catalogueNumber < 0 ? QString() : nodeId;
  if (layer == m_selectedLayer && catalogueNumber == m_selectedSatellite) {
    return;
  }
  m_selectedLayer = layer;
  m_selectedSatellite = catalogueNumber < 0 ? -1 : catalogueNumber;
  emit selectionChanged();
  requestPositions();
}

std::optional<DrawnSatellite> SatelliteLayers::selected() const
{
  for (const DrawnSatellite& d : m_drawn) {
    if (d.nodeId == m_selectedLayer &&
        d.sgp4->elements().catalogueNumber == m_selectedSatellite) {
      return d;
    }
  }
  return std::nullopt;
}

int32_t SatelliteLayers::oldCount(const QJsonObject& source,
                                  const SatelliteStyle& style,
                                  const QDateTime& at) const
{
  const std::shared_ptr<const SatelliteSet> s = set(source);
  if (!s) {
    return 0;
  }
  int32_t count = 0;
  for (const std::shared_ptr<const Sgp4>& sat : s->satellites) {
    count +=
        std::fabs(ageDays(sat->elements(), at)) > style.maximumAgeDays ? 1 : 0;
  }
  return count;
}

void SatelliteLayers::requestPositions()
{
  OrbitRequest request;
  request.generation = ++m_generation;
  const QDateTime now = m_clock->time();
  request.time = JulianDate::fromDateTime(now);
  std::shared_ptr<std::vector<TaskInfo>> infos =
      std::make_shared<std::vector<TaskInfo>>();
  for (std::size_t l = 0; l < m_layers.size(); ++l) {
    const SatelliteLayerEntry& layer = m_layers[l];
    const std::shared_ptr<const SatelliteSet> s = set(layer.definition.source);
    if (!s) {
      continue;
    }
    const SatelliteStyle style =
        SatelliteStyle::fromJson(layer.definition.style);
    int64_t shown = 0;
    for (const std::shared_ptr<const Sgp4>& sat : s->satellites) {
      shown += style.isShown(sat->elements().catalogueNumber) ? 1 : 0;
    }
    for (const std::shared_ptr<const Sgp4>& sat : s->satellites) {
      const int32_t n = sat->elements().catalogueNumber;
      if (!style.isShown(n)) {
        continue;
      }
      const bool selected =
          layer.nodeId == m_selectedLayer && n == m_selectedSatellite;
      OrbitTask task;
      task.sgp4 = sat;
      task.track = style.drawsTrack(selected, shown);
      task.pastMinutes = style.pastOf(n).minutes(sat->periodMinutes());
      task.futureMinutes = style.futureOf(n).minutes(sat->periodMinutes());
      request.tasks.push_back(std::move(task));
      TaskInfo info;
      info.nodeId = layer.nodeId;
      info.sgp4 = sat;
      info.layer = l;
      info.selected = selected;
      info.old =
          std::fabs(ageDays(sat->elements(), now)) > style.maximumAgeDays;
      info.footprint = style.drawsFootprint(selected);
      infos->push_back(std::move(info));
    }
  }
  m_clock->setTicking(!request.tasks.empty());
  if (request.tasks.empty() && m_drawn.empty() && m_pending.empty()) {
    // Nothing shown before or now: nothing to draw again.
    return;
  }
  m_pending.emplace_back(request.generation, std::move(infos));
  // Results come back in order; a few may be skipped (latest wins).
  while (m_pending.size() > 64) {
    m_pending.pop_front();
  }
  m_computer->request(std::move(request));
}

void SatelliteLayers::drawSatellite(const TaskInfo& info,
                                    const SatelliteState& state,
                                    const SatelliteTrack& track,
                                    const SatelliteStyle& style)
{
  const SatelliteLayerEntry& layer = m_layers[info.layer];
  const std::shared_ptr<const Sgp4>& sgp4 = info.sgp4;
  const QColor color = info.old ? oldColor(style.color) : style.color;
  const std::array<std::uint8_t, 4> rgba = rgbaOf(color, layer.alpha);
  const double radius = 0.5 * style.markerSizePx * (info.selected ? 1.6 : 1.0);
  m_drawn.push_back(DrawnSatellite{info.nodeId, sgp4, state, info.old});
  m_markers.push_back(satelliteMarker(state, rgba, radius));
  if (style.labels || info.selected) {
    BillboardDraw label;
    label.latDeg = state.geodetic.latDeg;
    label.lonDeg = state.geodetic.lonDeg;
    label.heightM = state.geodetic.heightM;
    label.height = OverlayHeight::Absolute;
    label.sizePx = style.markerSizePx * (info.selected ? 1.6 : 1.0);
    label.label = info.old ? tr("%1 (old)").arg(sgp4->elements().displayName())
                           : sgp4->elements().displayName();
    QColor labelColor = color.lighter(130);
    labelColor.setAlpha(static_cast<int32_t>(
        std::lround(std::clamp(color.alpha() * layer.alpha, 0.0, 255.0))));
    label.labelColor = labelColor;
    label.labelScale = info.selected ? 1.1 : 0.9;
    m_labels.push_back(std::move(label));
  }
  TrackLook look;
  look.rgba = rgba;
  look.periodMinutes = sgp4->periodMinutes();
  look.widthPx = info.selected ? 2.0F : 1.5F;
  if (!track.past.empty() || !track.future.empty()) {
    m_lines.push_back(pastTrackLine(track, look));
    m_lines.push_back(futureTrackLine(track, look));
    if (style.groundTrack) {
      for (SpaceLine& line : groundTrackLines(track, look)) {
        m_lines.push_back(std::move(line));
      }
    }
  }
  if (info.footprint) {
    m_lines.push_back(footprintLine(state, style.footprintElevationDeg, look));
  }
}

void SatelliteLayers::positionsReady()
{
  const std::shared_ptr<const OrbitResult> result = m_computer->result();
  if (!result) {
    return;
  }
  while (!m_pending.empty() && m_pending.front().first < result->generation) {
    m_pending.pop_front();
  }
  if (m_pending.empty() || m_pending.front().first != result->generation) {
    return;
  }
  const std::shared_ptr<std::vector<TaskInfo>> infos = m_pending.front().second;
  m_pending.pop_front();
  m_lastComputeMs = result->computeMs;
  m_drawn.clear();
  m_markers.clear();
  m_lines.clear();
  m_labels.clear();
  std::vector<SatelliteStyle> styles;
  styles.reserve(m_layers.size());
  for (const SatelliteLayerEntry& layer : m_layers) {
    styles.push_back(SatelliteStyle::fromJson(layer.definition.style));
  }
  for (std::size_t i = 0; i < infos->size() && i < result->states.size(); ++i) {
    const TaskInfo& info = (*infos)[i];
    const SatelliteState& state = result->states[i];
    if (!state.ok() || info.layer >= m_layers.size()) {
      continue;
    }
    drawSatellite(info, state, result->tracks[i], styles[info.layer]);
  }
  // The selected label first, so crowding doesn't hide it.
  std::stable_partition(
      m_labels.begin(), m_labels.end(),
      [](const BillboardDraw& b) { return b.labelScale > 1.0; });
  emit drawsChanged();
}

} // namespace ts
