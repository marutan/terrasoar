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
 * The aircraft of the visible aircraft layers: recordings and receivers,
 * and how they are drawn at the clock's time.
 */
#ifndef TS_APP_AIRCRAFTLAYERS_H
#define TS_APP_AIRCRAFTLAYERS_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>

#include "globeview.h"
#include "ts/adsb/adsbreaders.h"
#include "ts/adsb/aircraftlayer.h"
#include "ts/adsb/aircrafttracks.h"
#include "ts/adsb/receiverfeed.h"
#include "ts/globe/globerenderer.h"
#include "ts/layers/layerdefinition.h"

class QTimer;

namespace ts
{

class GeoidModel;
class NetworkClient;
class ProgramClock;

/** One aircraft layer's data, once loaded (or as it comes, live). */
struct AircraftSet {
  /** How loading went. */
  enum class State : uint8_t {
    Loading, ///< Being read.
    Ready,   ///< Read (perhaps with problems), or receiving.
    Failed,  ///< Nothing could be read.
  };
  /** How loading went. */
  State state = State::Loading;
  /** The aircraft, by address. */
  std::vector<std::shared_ptr<const AircraftTrack>> tracks;
  /** What reading the recording found (recordings). */
  AdsbReadResult read;
  /** Why it couldn't be read (translated); empty if it could. */
  QString error;
  /** True for a receiver's (live) data. */
  bool live = false;
  /** The live connection's state (receivers). */
  ReceiverFeed::State feed = ReceiverFeed::State::Stopped;
  /** The first report's time, Unix seconds (0: none). */
  double firstS = 0.0;
  /** The last report's time, Unix seconds (0: none). */
  double lastS = 0.0;
  /** Reports in all. */
  int64_t positions = 0;

  /** Returns the track of @p address, or null. */
  std::shared_ptr<const AircraftTrack> find(uint32_t address) const;
};

/** A visible aircraft layer, as the tree draws it. */
struct AircraftLayerEntry {
  /** The tree entry. */
  QString nodeId;
  /** Its definition (source and style). */
  LayerDefinition definition;
  /** Its effective opacity. */
  double alpha = 1.0;
};

/** An aircraft drawn: its layer, track and where it is. */
struct DrawnAircraft {
  /** The layer's tree entry. */
  QString nodeId;
  /** Its track. */
  std::shared_ptr<const AircraftTrack> track;
  /** Where it is now (interpolated). */
  TrackSample at;
  /** Its altitudes at the last report. */
  AircraftAltitudes altitudes;
  /** What it was called then (default if not known). */
  AircraftInfo info;
  /** The time of the last report, Unix seconds. */
  double lastReportS = 0.0;
};

/**
 * Keeps the aircraft of the visible aircraft layers: reads recordings on
 * worker threads, keeps receivers' connections (ReceiverFeed) and builds
 * their tracks as reports come, and turns them into markers, labels, drop
 * lines and trails at the clock's time. While any are shown the clock
 * ticks.
 *
 * Trails are timed lines (SpaceLine::timed) holding every report: they are
 * rebuilt only when the data or the style changes, and the clock's time
 * cuts them in the shader.
 *
 * @guithread
 */
class AircraftLayers : public QObject
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(AircraftLayers)

public:
  /** Returns the network client to fetch JSON with (made when needed). */
  using NetworkSource = std::function<std::shared_ptr<NetworkClient>()>;

  /** Returns the geoid barometric altitudes are measured from, or null. */
  using GeoidSource = std::function<std::shared_ptr<const GeoidModel>()>;

  /**
   * Follows @p clock (which must outlive it), fetching JSON with
   * @p network and placing barometric altitudes on @p geoid's sea level.
   */
  AircraftLayers(ProgramClock* clock, NetworkSource network, GeoidSource geoid,
                 QObject* parent);

  /** Stops receivers and waits for loads still running. */
  ~AircraftLayers() override;

  /** Sets the visible aircraft layers; loads what isn't loaded. */
  void setLayers(std::vector<AircraftLayerEntry> layers);

  /** Returns the visible aircraft layers. */
  const std::vector<AircraftLayerEntry>& layers() const { return m_layers; }

  /** Returns the data of @p source: null if never asked for. */
  std::shared_ptr<const AircraftSet> set(const QJsonObject& source) const;

  /** Loads @p source now unless loaded or loading. */
  void load(const QJsonObject& source);

  /** Reads a recording again, or connects to a receiver again. */
  void reload(const QJsonObject& source);

  /** Returns true while a recording is loading. */
  bool isBusy() const;

  /** Selects aircraft @p address of layer @p nodeId (empty: none). */
  void select(const QString& nodeId, std::optional<uint32_t> address);

  /** Returns the selected aircraft's layer; empty if none. */
  const QString& selectedLayer() const { return m_selectedLayer; }

  /** Returns the selected aircraft's address, if any. */
  std::optional<uint32_t> selectedAddress() const { return m_selected; }

  /** Returns the selected aircraft as last drawn, if it was. */
  std::optional<DrawnAircraft> selected() const;

  /** Returns the aircraft drawn, one per marker in markers()' order. */
  const std::vector<DrawnAircraft>& drawn() const { return m_drawn; }

  /** Returns the markers. */
  const std::vector<GlobeMarker>& markers() const { return m_markers; }

  /** Returns the labels. */
  const std::vector<BillboardDraw>& labels() const { return m_labels; }

  /** Returns the drop lines. */
  const std::vector<SpaceLine>& dropLines() const { return m_dropLines; }

  /** Returns the trails (timed lines). */
  const std::vector<SpaceLine>& trails() const { return m_trails; }

  /** Returns the clock's time the draws are for, Unix seconds. */
  double timeS() const { return m_timeS; }

  /**
   * Returns the time extent of the visible recordings ("first/last/PTnS"
   * joined by commas), for the time slider; empty if none.
   */
  QString timeExtent() const;

  /** Returns the key a source is kept by (its JSON, compact). */
  static QString keyOf(const QJsonObject& source);

  /**
   * Reads a recording (blocking): for dialogs that check before adding,
   * and the worker threads.
   */
  static AircraftSet
  readRecording(const AircraftSource& source,
                const std::atomic<bool>& cancelled,
                const std::shared_ptr<const GeoidModel>& geoid);

  /**
   * Returns how a builder makes heights with @p geoid (its undulations
   * kept in cells of a quarter degree; null: sea level is the ellipsoid).
   */
  static AircraftHeightSettings
  heightSettings(const std::shared_ptr<const GeoidModel>& geoid);

  /** Returns a label's text for an aircraft (callsign, altitude, speed). */
  static QString labelText(const DrawnAircraft& aircraft,
                           const AircraftStyle& style);

signals:
  /** markers(), labels() and dropLines() changed (each tick). */
  void drawsChanged();

  /** trails() changed. */
  void trailsChanged();

  /** A source finished loading (or failed), or its feed's state changed. */
  void setChanged(const QJsonObject& source);

  /** The selection changed. */
  void selectionChanged();

private:
  /** Takes the loads the pool finished (queued from its threads). */
  Q_INVOKABLE void takeFinishedLoads();

  struct FinishedLoad {
    QString key;
    std::shared_ptr<const AircraftSet> set;
  };
  /** A receiver's connection and the tracks it builds. */
  struct Live {
    ReceiverFeed* feed = nullptr;
    std::unique_ptr<AircraftTrackBuilder> builder;
    QJsonObject source;
  };

  void startLoad(const QString& key, const QJsonObject& json);
  void startLive(const QString& key, const QJsonObject& json,
                 const AircraftSource& source);
  void stopLive(const QString& key);
  void publishLive();
  void finishLoad(const QString& key, std::shared_ptr<const AircraftSet> set);
  void updateDraws();
  void drawAircraft(const DrawnAircraft& aircraft, const AircraftStyle& style,
                    double alpha, bool selected);
  void rebuildTrails();

  /** Guarded: the window may destroy the clock first. */
  QPointer<ProgramClock> m_clock;
  NetworkSource m_network;
  GeoidSource m_geoid;
  std::vector<AircraftLayerEntry> m_layers;
  std::map<QString, std::shared_ptr<const AircraftSet>> m_sets;
  std::map<QString, Live> m_live;
  QTimer* m_publish = nullptr;
  QThreadPool m_pool;
  std::shared_ptr<std::atomic<bool>> m_cancelled;
  std::vector<FinishedLoad> m_finished;
  std::mutex m_finishedMutex;
  QString m_selectedLayer;
  std::optional<uint32_t> m_selected;
  std::vector<DrawnAircraft> m_drawn;
  std::vector<GlobeMarker> m_markers;
  std::vector<BillboardDraw> m_labels;
  std::vector<SpaceLine> m_dropLines;
  std::vector<SpaceLine> m_trails;
  double m_timeS = 0.0;
};

} // namespace ts

#endif // TS_APP_AIRCRAFTLAYERS_H
