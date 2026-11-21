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
/**
 * @file
 * The satellites of the layer tree: their element sets loaded, where they
 * are at the clock's time, and what the view draws for them.
 */
#ifndef TS_APP_SATELLITELAYERS_H
#define TS_APP_SATELLITELAYERS_H

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QThreadPool>

#include "globeview.h"
#include "ts/globe/globerenderer.h"
#include "ts/layers/layerdefinition.h"
#include "ts/orbits/elementsource.h"
#include "ts/orbits/orbitcomputer.h"
#include "ts/orbits/satellitelayer.h"
#include "ts/orbits/satellitetrack.h"
#include "ts/orbits/sgp4.h"

namespace ts
{

class NetworkClient;
class ProgramClock;

/** One satellite layer's element sets, once loaded. */
struct SatelliteSet {
  /** How loading went. */
  enum class State : uint8_t {
    Loading, ///< Being read or downloaded.
    Ready,   ///< Read (perhaps with problems, or from an old download).
    Failed,  ///< Nothing could be read.
  };
  /** How loading went. */
  State state = State::Loading;
  /** The satellites, in the source's order. */
  std::vector<std::shared_ptr<const Sgp4>> satellites;
  /** What couldn't be read. */
  QStringList problems;
  /** Why the source couldn't be used (translated); empty if it could. */
  QString error;
  /** Where downloaded sets came from. */
  ElementOrigin origin = ElementOrigin::None;
  /** When they were downloaded, seconds since 1970 (0: not downloaded). */
  int64_t downloadedS = 0;

  /** Returns the satellite with @p catalogueNumber, or null. */
  std::shared_ptr<const Sgp4> find(int32_t catalogueNumber) const;
};

/** A visible satellite layer, as the tree draws it. */
struct SatelliteLayerEntry {
  /** The tree entry. */
  QString nodeId;
  /** Its definition (source and style). */
  LayerDefinition definition;
  /** Its effective opacity. */
  double alpha = 1.0;
};

/** A satellite drawn: its layer and catalogue number. */
struct DrawnSatellite {
  /** The layer's tree entry. */
  QString nodeId;
  /** The satellite. */
  std::shared_ptr<const Sgp4> sgp4;
  /** Where it is. */
  SatelliteState state;
  /** True if its elements are older than the layer allows. */
  bool old = false;
};

/**
 * Keeps the satellites of the visible satellite layers: reads files and
 * pasted text and downloads CelesTrak's groups (through the cache, see
 * downloadElementSets()) on worker threads, asks the OrbitComputer where
 * the shown satellites are at the clock's time (and their tracks), and
 * turns the answer into markers, tracks, footprints and labels for the
 * view. While any are shown the clock ticks.
 *
 * Sources are kept by their JSON, so layers with the same source share
 * one download.
 *
 * @guithread
 */
class SatelliteLayers : public QObject
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(SatelliteLayers)

public:
  /** Returns the network client to download with (made when needed). */
  using NetworkSource = std::function<std::shared_ptr<NetworkClient>()>;

  /**
   * Follows @p clock (which must outlive it), downloading with @p network
   * and keeping downloads in @p cacheDirectory.
   */
  SatelliteLayers(ProgramClock* clock, NetworkSource network,
                  const QString& cacheDirectory, QObject* parent);

  /** Waits for loads still running. */
  ~SatelliteLayers() override;

  /** Sets the visible satellite layers; loads what isn't loaded. */
  void setLayers(std::vector<SatelliteLayerEntry> layers);

  /** Returns the visible satellite layers. */
  const std::vector<SatelliteLayerEntry>& layers() const { return m_layers; }

  /**
   * Returns the element sets of @p source: null if never asked for (no
   * visible layer has it), else as loading goes.
   */
  std::shared_ptr<const SatelliteSet> set(const QJsonObject& source) const;

  /**
   * Loads @p source now (a layer not visible yet, such as one just added
   * or listed) unless it is loaded or loading.
   */
  void load(const QJsonObject& source);

  /**
   * Loads @p source again: downloads asking the server whatever the
   * cache's age, reads files again.
   */
  void refresh(const QJsonObject& source);

  /** Returns true while any source is loading or positions computing. */
  bool isBusy() const;

  /** Selects satellite @p catalogueNumber of layer @p nodeId (−1: none). */
  void select(const QString& nodeId, int32_t catalogueNumber);

  /** Returns the selected satellite's layer; empty if none. */
  const QString& selectedLayer() const { return m_selectedLayer; }

  /** Returns the selected satellite's catalogue number, −1 if none. */
  int32_t selectedSatellite() const { return m_selectedSatellite; }

  /** Returns the selected satellite as last drawn, if it was. */
  std::optional<DrawnSatellite> selected() const;

  /** Returns the satellites drawn, one per marker in markers()' order. */
  const std::vector<DrawnSatellite>& drawn() const { return m_drawn; }

  /** Returns the markers to draw. */
  const std::vector<GlobeMarker>& markers() const { return m_markers; }

  /** Returns the tracks and footprints to draw. */
  const std::vector<SpaceLine>& lines() const { return m_lines; }

  /** Returns the labels to draw. */
  const std::vector<BillboardDraw>& labels() const { return m_labels; }

  /** Returns how long the last positions took to compute, milliseconds. */
  double lastComputeMs() const { return m_lastComputeMs; }

  /** Returns how many satellites the layer of @p source has that are old. */
  int32_t oldCount(const QJsonObject& source, const SatelliteStyle& style,
                   const QDateTime& at) const;

  /** Returns @p elements's age at @p at, days (negative: the epoch after). */
  static double ageDays(const OrbitalElements& elements, const QDateTime& at);

  /** Returns the key a source is kept by (its JSON, compact). */
  static QString keyOf(const QJsonObject& source);

  /**
   * Reads element sets from @p source (a file or text; blocking). For
   * dialogs that check before adding.
   */
  static SatelliteSet readLocal(const SatelliteSource& source);

signals:
  /** markers(), lines() and labels() changed. */
  void drawsChanged();

  /** A source finished loading (or failed); @p source is its JSON. */
  void setLoaded(const QJsonObject& source);

  /** The selection changed. */
  void selectionChanged();

private:
  /** Takes the loads the pool finished (queued from its threads). */
  Q_INVOKABLE void takeFinishedLoads();

  /** A load a pool thread finished, waiting for the GUI thread. */
  struct FinishedLoad {
    QString key;
    QJsonObject source;
    std::shared_ptr<const SatelliteSet> set;
  };

  struct TaskInfo {
    QString nodeId;
    std::shared_ptr<const Sgp4> sgp4;
    std::size_t layer = 0;
    bool selected = false;
    bool old = false;
    bool footprint = false;
  };

  void startLoad(const QString& key, const QJsonObject& json, bool force);
  void finishLoad(const QString& key, const QJsonObject& source,
                  std::shared_ptr<const SatelliteSet> set);
  void requestPositions();
  void positionsReady();
  void drawSatellite(const TaskInfo& info, const SatelliteState& state,
                     const SatelliteTrack& track, const SatelliteStyle& style);

  ProgramClock* m_clock;
  NetworkSource m_network;
  ElementDownloadSettings m_download;
  std::vector<SatelliteLayerEntry> m_layers;
  std::map<QString, std::shared_ptr<const SatelliteSet>> m_sets;
  QThreadPool m_pool;
  std::shared_ptr<std::atomic<bool>> m_cancelled;
  /** Loads finished on the pool's threads (under m_finishedMutex). */
  std::vector<FinishedLoad> m_finished;
  std::mutex m_finishedMutex;
  OrbitComputer* m_computer = nullptr;
  uint64_t m_generation = 0;
  /** What each task of the requests still to come back was. */
  std::deque<std::pair<uint64_t, std::shared_ptr<std::vector<TaskInfo>>>>
      m_pending;
  QString m_selectedLayer;
  int32_t m_selectedSatellite = -1;
  std::vector<DrawnSatellite> m_drawn;
  std::vector<GlobeMarker> m_markers;
  std::vector<SpaceLine> m_lines;
  std::vector<BillboardDraw> m_labels;
  double m_lastComputeMs = 0.0;
};

} // namespace ts

#endif // TS_APP_SATELLITELAYERS_H
