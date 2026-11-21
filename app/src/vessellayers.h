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
 * The ships (and other AIS stations) of the visible ship layers:
 * recordings, receivers and online feeds, drawn at the clock's time.
 */
#ifndef TS_APP_VESSELLAYERS_H
#define TS_APP_VESSELLAYERS_H

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QThreadPool>

#include "globeview.h"
#include "ts/ais/aisfeed.h"
#include "ts/ais/aisreaders.h"
#include "ts/ais/vessellayer.h"
#include "ts/ais/vesseltracks.h"
#include "ts/globe/globerenderer.h"
#include "ts/layers/layerdefinition.h"
#include "ts/tracks/trackdraws.h"

class QTimer;

namespace ts
{

class GeoidModel;
class NetworkClient;
class ProgramClock;

/** One ship layer's data, once loaded (or as it comes, live). */
struct VesselSet {
  /** How loading went. */
  enum class State : uint8_t {
    Loading, ///< Being read.
    Ready,   ///< Read (perhaps with problems), or receiving.
    Failed,  ///< Nothing could be read.
  };
  /** How loading went. */
  State state = State::Loading;
  /** The stations, by MMSI. */
  std::vector<std::shared_ptr<const VesselTrack>> tracks;
  /** What reading the recording found (recordings). */
  AisReadResult read;
  /** Why it couldn't be read (translated); empty if it could. */
  QString error;
  /** True for live data (receivers, feeds). */
  bool live = false;
  /** The live connection's state. */
  AisFeed::State feed = AisFeed::State::Stopped;
  /** The first report's time, Unix seconds (0: none). */
  double firstS = 0.0;
  /** The last report's time, Unix seconds (0: none). */
  double lastS = 0.0;

  /** Returns the track of @p mmsi, or null. */
  std::shared_ptr<const VesselTrack> find(uint32_t mmsi) const;
};

/** A visible ship layer, as the tree draws it. */
struct VesselLayerEntry {
  /** The tree entry. */
  QString nodeId;
  /** Its definition (source and style). */
  LayerDefinition definition;
  /** Its effective opacity. */
  double alpha = 1.0;
};

/** A station drawn: its layer, track and where it is. */
struct DrawnVessel {
  /** The layer's tree entry. */
  QString nodeId;
  /** Its track. */
  std::shared_ptr<const VesselTrack> track;
  /** Where it is now (interpolated). */
  TrackSample at;
  /** Its heading and rate of turn at the last report. */
  VesselMotion motion;
  /** What is known of it then. */
  VesselInfo info;
  /** The time of the last position, Unix seconds. */
  double lastReportS = 0.0;
};

/**
 * Keeps the stations of the visible ship layers: reads recordings on
 * worker threads, keeps receivers' and feeds' connections (AisFeed) and
 * builds their tracks as reports come, and turns them into markers,
 * labels, course vectors and trails at the clock's time, on the sea's
 * level (the geoid). While any are shown the clock ticks.
 *
 * @guithread
 */
class VesselLayers : public QObject
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(VesselLayers)

public:
  /** Returns the network client feeds fetch with (made when needed). */
  using NetworkSource = std::function<std::shared_ptr<NetworkClient>()>;

  /** Returns the geoid that is sea level, or null. */
  using GeoidSource = std::function<std::shared_ptr<const GeoidModel>()>;

  /** Follows @p clock (which must outlive it or be destroyed first). */
  VesselLayers(ProgramClock* clock, NetworkSource network, GeoidSource geoid,
               QObject* parent);

  /** Stops the feeds and waits for loads still running. */
  ~VesselLayers() override;

  /** Sets the visible ship layers; loads what isn't loaded. */
  void setLayers(std::vector<VesselLayerEntry> layers);

  /** Returns the visible ship layers. */
  const std::vector<VesselLayerEntry>& layers() const { return m_layers; }

  /** Returns the data of @p source: null if never asked for. */
  std::shared_ptr<const VesselSet> set(const QJsonObject& source) const;

  /** Loads @p source now unless loaded or loading. */
  void load(const QJsonObject& source);

  /** Reads a recording again, or connects again. */
  void reload(const QJsonObject& source);

  /** Returns true while a recording is loading. */
  bool isBusy() const;

  /** Selects station @p mmsi of layer @p nodeId (empty: none). */
  void select(const QString& nodeId, std::optional<uint32_t> mmsi);

  /** Returns the selected station's layer; empty if none. */
  const QString& selectedLayer() const { return m_selectedLayer; }

  /** Returns the selected station's MMSI, if any. */
  std::optional<uint32_t> selectedMmsi() const { return m_selected; }

  /** Returns the selected station as last drawn, if it was. */
  std::optional<DrawnVessel> selected() const;

  /** Returns the stations drawn, one per marker in markers()' order. */
  const std::vector<DrawnVessel>& drawn() const { return m_drawn; }

  /** Returns the markers. */
  const std::vector<GlobeMarker>& markers() const { return m_markers; }

  /** Returns the labels. */
  const std::vector<BillboardDraw>& labels() const { return m_labels; }

  /** Returns the course vectors. */
  const std::vector<SpaceLine>& courseVectors() const
  {
    return m_courseVectors;
  }

  /** Returns the trails (timed lines). */
  const std::vector<SpaceLine>& trails() const { return m_trails; }

  /** Returns the clock's time the draws are for, Unix seconds. */
  double timeS() const { return m_timeS; }

  /** Returns the visible recordings' time extent, for the time slider. */
  QString timeExtent() const;

  /** Returns the key a source is kept by (its JSON, compact). */
  static QString keyOf(const QJsonObject& source);

  /** Reads a recording (blocking). */
  static VesselSet readRecording(const VesselSource& source,
                                 const std::atomic<bool>& cancelled);

  /** Returns a label's text (name or MMSI, speed, destination). */
  static QString labelText(const DrawnVessel& vessel, const VesselStyle& style);

  /** Returns the colour of a station's marker in @p style. */
  static QColor markerColour(const VesselInfo& info, const VesselStyle& style);

signals:
  /** markers(), labels() and courseVectors() changed (each tick). */
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
    std::shared_ptr<const VesselSet> set;
  };
  /** A live source's connection and the tracks it builds. */
  struct Live {
    AisFeed* feed = nullptr;
    std::unique_ptr<VesselTrackBuilder> builder;
    QJsonObject source;
  };

  void startLoad(const QString& key, const QJsonObject& json);
  void startLive(const QString& key, const QJsonObject& json,
                 const VesselSource& source);
  void stopLive(const QString& key);
  void publishLive();
  void finishLoad(const QString& key, std::shared_ptr<const VesselSet> set);
  void updateDraws();
  void drawVessel(const DrawnVessel& vessel, const VesselStyle& style,
                  double alpha, bool selected);
  void rebuildTrails();
  TrailLook trailLook(const VesselTrack& track, const VesselInfo& known,
                      const VesselStyle& style, double alpha, bool selected);
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  double seaLevelM(double latDeg, double lonDeg);

  QPointer<ProgramClock> m_clock;
  NetworkSource m_network;
  GeoidSource m_geoid;
  std::shared_ptr<const GeoidModel> m_seaLevel;
  bool m_seaLevelAsked = false;
  std::unordered_map<int64_t, double> m_seaLevelCells;
  std::vector<VesselLayerEntry> m_layers;
  std::map<QString, std::shared_ptr<const VesselSet>> m_sets;
  std::map<QString, Live> m_live;
  QTimer* m_publish = nullptr;
  QThreadPool m_pool;
  std::shared_ptr<std::atomic<bool>> m_cancelled;
  std::vector<FinishedLoad> m_finished;
  std::mutex m_finishedMutex;
  QString m_selectedLayer;
  std::optional<uint32_t> m_selected;
  std::vector<DrawnVessel> m_drawn;
  std::vector<GlobeMarker> m_markers;
  std::vector<BillboardDraw> m_labels;
  std::vector<SpaceLine> m_courseVectors;
  std::vector<SpaceLine> m_trails;
  double m_timeS = 0.0;
};

} // namespace ts

#endif // TS_APP_VESSELLAYERS_H
