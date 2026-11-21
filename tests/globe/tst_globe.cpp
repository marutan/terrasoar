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

#include <algorithm>
#include <array>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_set>
#include <vector>

#include <QColor>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QNetworkProxy>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include "goldenimage.h"
#include "mockhttpserver.h"
#include "mockwms.h"
#include "mockwmts.h"
#include "offscreengl.h"
#include "packbuilder.h"
#include "ts/adsb/adsbreaders.h"
#include "ts/adsb/aircraftdraws.h"
#include "ts/adsb/aircrafttracks.h"
#include "ts/ais/aisreaders.h"
#include "ts/ais/vesseldraws.h"
#include "ts/ais/vesseltracks.h"
#include "ts/camera/camera.h"
#include "ts/elevation/heightgrid.h"
#include "ts/geo/ellipsoid.h"
#include "ts/geo/geodetic.h"
#include "ts/globe/globerenderer.h"
#include "ts/globe/layerstackdraws.h"
#include "ts/globe/tilerenderer.h"
#include "ts/globe/vectorrenderer.h"
#include "ts/layers/layerdefinition.h"
#include "ts/layers/layertreemodel.h"
#include "ts/network/networkclient.h"
#include "ts/network/tilediskcache.h"
#include "ts/orbits/orbitalelements.h"
#include "ts/orbits/orbitcomputer.h"
#include "ts/orbits/satellitedraws.h"
#include "ts/orbits/satellitetrack.h"
#include "ts/orbits/sgp4.h"
#include "ts/packs/packlibrary.h"
#include "ts/packs/packmanifest.h"
#include "ts/projection/projection.h"
#include "ts/providers/gdalimageryprovider.h"
#include "ts/providers/geopackagefeatureprovider.h"
#include "ts/providers/providerregistry.h"
#include "ts/providers/testproviders.h"
#include "ts/regions/regiondownload.h"
#include "ts/tracks/movingtrack.h"
#include "ts/tracks/trackdraws.h"
#include "ts/wms/wmsimageryprovider.h"
#include "ts/wms/wmssource.h"
#include "ts/wmts/wmtscapabilities.h"
#include "ts/wmts/wmtsimageryprovider.h"
#include "ts/wmts/wmtssource.h"

namespace
{

constexpr int32_t imageWidthPx = 320;
constexpr int32_t imageHeightPx = 240;

/**
 * Golden images are compared only on Mesa llvmpipe, whose output is the
 * same everywhere for a given version; other drivers differ in rounding.
 * Channel tolerance and differing-pixel fraction allow for small changes
 * between llvmpipe versions (Linux packages, the Windows Mesa build).
 */
constexpr ts::test::GoldenTolerance goldenTolerance{12, 0.005};

void appendUint32(QByteArray* wkb, std::uint32_t value)
{
  for (int32_t i = 0; i < 4; ++i) {
    wkb->append(
        static_cast<char>((value >> (8U * static_cast<uint32_t>(i))) & 0xffU));
  }
}

void appendDouble(QByteArray* wkb, double value)
{
  std::uint64_t bits = 0;
  std::memcpy(&bits, &value, sizeof bits);
  for (int32_t i = 0; i < 8; ++i) {
    wkb->append(
        static_cast<char>((bits >> (8U * static_cast<uint32_t>(i))) & 0xffU));
  }
}

/** Little-endian WKB of a polygon from rings of lon, lat pairs. */
QByteArray polygonWkb(const std::vector<std::vector<double>>& rings)
{
  QByteArray wkb;
  wkb.append(static_cast<char>(1));
  appendUint32(&wkb, 3);
  appendUint32(&wkb, static_cast<std::uint32_t>(rings.size()));
  for (const std::vector<double>& ring : rings) {
    appendUint32(&wkb, static_cast<std::uint32_t>(ring.size() / 2 + 1));
    for (const double value : ring) {
      appendDouble(&wkb, value);
    }
    appendDouble(&wkb, ring[0]);
    appendDouble(&wkb, ring[1]);
  }
  return wkb;
}

/**
 * Writes a GeoPackage with one table stored at level 0 for every level:
 * a polygon split at the antimeridian (both halves end on 180°, an
 * artificial edge not to be outlined), a polar cap down to the north pole
 * in each hemisphere tile, and an island with a lake.
 */
bool writeVectorFixture(const QString& path)
{
  ts::GeoPackageFeatureWriter writer(path);
  ts::FeatureTableSpec table;
  table.name = QStringLiteral("land");
  table.storageLevel = 0;
  table.minLevel = 0;
  table.maxLevel = -1;
  const ts::TileKey west{0, 0, 0};
  const ts::TileKey east{0, 1, 0};
  return writer.addTable(table) &&
         writer.putFeature(
             table.name, east,
             polygonWkb({{170, -25, 180, -25, 180, -5, 170, -5}})) &&
         writer.putFeature(
             table.name, west,
             polygonWkb({{-180, -25, -165, -25, -165, -10, -180, -10}})) &&
         writer.putFeature(table.name, west,
                           polygonWkb({{-180, 75, 0, 75, 0, 90, -180, 90}})) &&
         writer.putFeature(table.name, east,
                           polygonWkb({{0, 75, 180, 75, 180, 90, 0, 90}})) &&
         writer.putFeature(table.name, east,
                           polygonWkb({{20, 30, 40, 30, 40, 50, 20, 50},
                                       {25, 35, 32, 35, 32, 42, 25, 42}})) &&
         writer.finish();
}

ts::Camera makeCamera(const ts::CameraPose& pose)
{
  ts::Camera camera;
  camera.setViewportSize(imageWidthPx, imageHeightPx);
  camera.setPose(pose);
  return camera;
}

/** A checkerboard that takes as long per tile as a network server. */
class SlowCheckerboard : public ts::CheckerboardImageryProvider
{
public:
  QImage loadImage(ts::TileKey key, const std::atomic<bool>& cancelled) override
  {
    QThread::msleep(10);
    return ts::CheckerboardImageryProvider::loadImage(key, cancelled);
  }
};

/**
 * Blanks the pixels of both @p images that don't show a point inside
 * @p bounds, a little inside the edge (where both layers have pixels).
 */
void keepOnlyInside(std::array<QImage, 2>& images, const ts::Camera& camera,
                    const std::optional<ts::GeoRect>& bounds)
{
  if (!bounds) {
    return;
  }
  const double margin = 0.3;
  for (int32_t y = 0; y < images[0].height(); ++y) {
    for (int32_t x = 0; x < images[0].width(); ++x) {
      const std::optional<ts::Geodetic> point =
          camera.pickGeodetic(x + 0.5, y + 0.5);
      const bool inside = point && point->latDeg > bounds->southDeg + margin &&
                          point->latDeg < bounds->northDeg - margin &&
                          point->lonDeg > bounds->westDeg + margin &&
                          point->lonDeg < bounds->eastDeg - margin;
      if (!inside) {
        images[0].setPixel(x, y, qRgb(0, 0, 0));
        images[1].setPixel(x, y, qRgb(0, 0, 0));
      }
    }
  }
}

/** Centroid of the pixels where red clearly exceeds blue (the markers). */
std::optional<QPointF> redCentroid(const QImage& image)
{
  double sum = 0.0;
  double sumX = 0.0;
  double sumY = 0.0;
  for (int32_t y = 0; y < image.height(); ++y) {
    for (int32_t x = 0; x < image.width(); ++x) {
      const QColor c = image.pixelColor(x, y);
      const double weight = std::max(0, c.red() - c.blue());
      sum += weight;
      sumX += weight * (x + 0.5);
      sumY += weight * (y + 0.5);
    }
  }
  if (sum <= 0.0) {
    return std::nullopt;
  }
  return QPointF(sumX / sum, sumY / sum);
}

int32_t to8Bit(float value)
{
  return qRound(value * 255.0F);
}

/** True if (x, y) shows the globe rather than the background colour. */
bool isGlobe(const QImage& image, int32_t x, int32_t y)
{
  const QColor c = image.pixelColor(x, y);
  const ts::GlobeStyle style;
  return std::abs(c.red() - to8Bit(style.backgroundRgb[0])) +
             std::abs(c.green() - to8Bit(style.backgroundRgb[1])) +
             std::abs(c.blue() - to8Bit(style.backgroundRgb[2])) >
         6;
}

/**
 * Steep synthetic mountains: 4 km ridges less than a degree apart, sea
 * level 30 m above the ellipsoid. Tiles of different levels sample them
 * differently, so neighbours at different levels disagree, which the
 * skirts must hide.
 */
class SineElevation : public ts::ElevationProvider
{
public:
  QString id() const override { return QStringLiteral("test:sine"); }
  QString name() const override { return QStringLiteral("Sine"); }
  int32_t maximumLevel() const override { return 8; }

  static double heightM(double latDeg, double lonDeg)
  {
    return 30.0 + 2000.0 +
           2000.0 * std::sin(latDeg * 7.0) * std::cos(lonDeg * 9.0);
  }

  std::optional<ts::HeightTile>
  loadHeights(ts::TileKey key, const std::atomic<bool>& /*cancelled*/) override
  {
    // Beyond level 8, the level-8 ancestor's samples interpolated.
    ts::TileKey stored = key;
    while (stored.level > maximumLevel()) {
      stored = stored.parent();
    }
    ts::HeightTile tile;
    tile.key = stored;
    tile.samples = 17;
    for (int32_t row = 0; row < 17; ++row) {
      for (int32_t column = 0; column < 17; ++column) {
        const ts::GeoPoint point =
            ts::heightSamplePoint(stored, 17, row, column);
        tile.heightsM.push_back(
            static_cast<float>(heightM(point.latDeg, point.lonDeg)));
        tile.seaLevelM.push_back(30.0F);
      }
    }
    return stored == key ? tile : ts::resampleHeightTile(tile, key, 17);
  }
};

/**
 * SineElevation that fails the first time each tile is asked for (an
 * incomplete tile at sea level, like a web source that didn't answer).
 */
class FlakyElevation : public SineElevation
{
public:
  QString id() const override { return QStringLiteral("test:flaky"); }

  std::optional<ts::HeightTile>
  loadHeights(ts::TileKey key, const std::atomic<bool>& cancelled) override
  {
    {
      const std::lock_guard<std::mutex> lock(m_mutex);
      if (m_asked.insert(key).second) {
        ts::HeightTile tile;
        tile.key = key;
        tile.samples = 2;
        tile.heightsM.assign(4, 30.0F);
        tile.seaLevelM.assign(4, 30.0F);
        tile.incomplete = true;
        return tile;
      }
      ++m_retries;
    }
    return SineElevation::loadHeights(key, cancelled);
  }

  /** Tiles asked for again after an incomplete answer. */
  int32_t retries() const
  {
    const std::lock_guard<std::mutex> lock(m_mutex);
    return m_retries;
  }

private:
  mutable std::mutex m_mutex;
  std::unordered_set<ts::TileKey, ts::TileKeyHash> m_asked;
  int32_t m_retries = 0;
};

/**
 * Returns the projection a golden row names: "id", or "id@v1,v2,…" with
 * its parameters set in order; null for an unknown id.
 */
const ts::Projection* projectionFromSpec(const QString& spec)
{
  const QStringList idAndValues = spec.split(QLatin1Char('@'));
  const ts::Projection* standard = ts::projectionById(idAndValues.front());
  if (standard == nullptr || idAndValues.size() < 2) {
    return standard;
  }
  const std::vector<ts::ProjectionParameter> parameters =
      standard->parameters();
  const QStringList values = idAndValues.back().split(QLatin1Char(','));
  ts::ProjectionParameterValues chosen;
  const std::size_t count =
      std::min(static_cast<std::size_t>(values.size()), parameters.size());
  for (std::size_t i = 0; i < count; ++i) {
    chosen[parameters.at(i).key] =
        values.at(static_cast<int32_t>(i)).toDouble();
  }
  return ts::projectionWith(idAndValues.front(), chosen);
}

/** The first element set of @p name in tests/orbits/data. */
ts::OrbitalElements orbitFixture(const char* name)
{
  QFile file(QStringLiteral(TS_TEST_DATA_DIR "/../orbits/data/") +
             QLatin1String(name));
  if (!file.open(QIODevice::ReadOnly)) {
    return ts::OrbitalElements();
  }
  const ts::ElementSetList list = ts::parseElementSets(file.readAll());
  return list.elements.empty() ? ts::OrbitalElements() : list.elements.front();
}

/** Pixels within 40 of @p color in every channel. */
int32_t countColor(const QImage& image, const QColor& color)
{
  const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
  int32_t count = 0;
  for (int32_t y = 0; y < rgba.height(); ++y) {
    for (int32_t x = 0; x < rgba.width(); ++x) {
      const QColor p = rgba.pixelColor(x, y);
      count += std::abs(p.red() - color.red()) < 40 &&
                       std::abs(p.green() - color.green()) < 40 &&
                       std::abs(p.blue() - color.blue()) < 40
                   ? 1
                   : 0;
    }
  }
  return count;
}

/** A straight line between two points, @p count steps, at @p heightM. */
// From, then to, as named.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
ts::SpaceLine lineBetween(ts::LonLat from, ts::LonLat to, double heightM,
                          int32_t count)
{
  ts::SpaceLine line;
  line.rgba = {255, 0, 255, 255};
  line.widthPx = 3.0F;
  for (int32_t i = 0; i <= count; ++i) {
    const double f = static_cast<double>(i) / count;
    double dLon = to.lonDeg - from.lonDeg;
    dLon -= 360.0 * std::round(dLon / 360.0);
    ts::SpaceLinePoint p;
    p.lonDeg = ts::normaliseLonDeg(from.lonDeg + f * dLon);
    p.latDeg = from.latDeg + f * (to.latDeg - from.latDeg);
    p.positionEcef =
        ts::geodeticToEcef(ts::Geodetic{p.latDeg, p.lonDeg, heightM});
    line.points.push_back(p);
  }
  return line;
}

/** The made-up flights of tests/adsb's fixtures, as tracks. */
std::vector<ts::AircraftTrack> aircraftFixture()
{
  ts::AircraftTrackBuilder builder;
  ts::readAdsbFile(
      QStringLiteral(TS_TEST_DATA_DIR "/../adsb/data/flights-aircraft.jsonl"),
      ts::AdsbFormat::Unknown, ts::AdsbReadOptions(),
      [&builder](const ts::AircraftReport& report) { builder.add(report); });
  return builder.finish();
}

/** The fixtures' start, 2026-11-20T12:00Z. */
constexpr double aircraftFixtureStartS = 1795176000.0;

/**
 * The fixture flights at @p timeS as the program draws them: markers,
 * drop lines and trails coloured by altitude.
 */
void aircraftScene(double timeS, ts::TrailSettings trail,
                   std::vector<ts::GlobeMarker>& markers,
                   std::vector<ts::SpaceLine>& lines)
{
  ts::TrackTiming timing;
  ts::TrailLook look;
  look.widthPx = 3.0F;
  look.colourOf = [](const ts::TrackSample& p) {
    return ts::altitudeColour(p.heightM);
  };
  for (const ts::AircraftTrack& track : aircraftFixture()) {
    for (ts::SpaceLine& line :
         ts::trailLines(track.track, timing, trail, look)) {
      lines.push_back(std::move(line));
    }
    const std::optional<ts::TrackSample> at = track.track.at(timeS, timing);
    if (at) {
      markers.push_back(ts::aircraftMarker(*at, 0.0, {255, 230, 0, 255}, 6.0));
      lines.push_back(ts::dropLine(*at, 0.0, {255, 230, 0, 255}));
    }
  }
}

/** The made-up harbour of tests/ais's fixtures, as tracks. */
std::vector<ts::VesselTrack> harbourFixture()
{
  ts::VesselTrackBuilder builder;
  ts::readAisFile(
      QStringLiteral(TS_TEST_DATA_DIR "/../ais/data/harbour-tagged.nmea"),
      ts::AisFormat::Unknown, ts::AisReadOptions(),
      [&builder](const ts::VesselReport& report) { builder.add(report); });
  return builder.finish();
}

/**
 * The harbour at @p timeS as the program draws it: stations by type
 * colour, six-minute course vectors and trails coloured by type.
 */
void harbourScene(double timeS, ts::TrailSettings trail,
                  std::vector<ts::GlobeMarker>& markers,
                  std::vector<ts::SpaceLine>& lines)
{
  ts::TrackTiming timing;
  timing.gapS = 60.0;
  for (const ts::VesselTrack& vessel : harbourFixture()) {
    const ts::VesselInfo* info = vessel.infoAt(timeS);
    const ts::VesselInfo known = info != nullptr ? *info : ts::VesselInfo();
    const ts::ShipTypeGroup group = known.kind == ts::AisStationKind::Vessel
                                        ? ts::shipTypeGroup(known.shipType)
                                        : ts::ShipTypeGroup::Navigation;
    const std::array<std::uint8_t, 4> rgba = ts::shipTypeColour(group);
    ts::TrailLook look;
    look.rgba = rgba;
    look.widthPx = 2.0F;
    for (ts::SpaceLine& line :
         ts::trailLines(vessel.track, timing, trail, look)) {
      lines.push_back(std::move(line));
    }
    timing.staleS = ts::vesselStaleS(known, 0.0);
    const std::optional<ts::TrackSample> at = vessel.track.at(timeS, timing);
    if (!at) {
      continue;
    }
    const std::size_t i = vessel.track.indexAtOrBefore(timeS).value_or(0);
    markers.push_back(
        ts::vesselMarker(*at, vessel.motion.at(i), known, 0.0, rgba, 7.0));
    lines.push_back(ts::courseVector(*at, 6.0, 0.0, rgba));
  }
}

/** The bounding box of @p image's pixels near @p color. */
QRect boxOfColor(const QImage& image, const QColor& color)
{
  const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
  QRect box;
  for (int32_t y = 0; y < rgba.height(); ++y) {
    for (int32_t x = 0; x < rgba.width(); ++x) {
      const QColor p = rgba.pixelColor(x, y);
      if (std::abs(p.red() - color.red()) < 40 &&
          std::abs(p.green() - color.green()) < 40 &&
          std::abs(p.blue() - color.blue()) < 40) {
        box = box.united(QRect(x, y, 1, 1));
      }
    }
  }
  return box;
}

/** Pixels of @p image near @p color in each part of a 2×2 split. */
std::array<int32_t, 4> quadrantCounts(const QImage& image, const QColor& color)
{
  const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
  std::array<int32_t, 4> counts{}; // top-left, top-right, bottom-left, -right
  for (int32_t y = 0; y < rgba.height(); ++y) {
    for (int32_t x = 0; x < rgba.width(); ++x) {
      const QColor p = rgba.pixelColor(x, y);
      if (std::abs(p.red() - color.red()) < 40 &&
          std::abs(p.green() - color.green()) < 40 &&
          std::abs(p.blue() - color.blue()) < 40) {
        const std::size_t i = (y < rgba.height() / 2 ? 0U : 2U) +
                              (x < rgba.width() / 2 ? 0U : 1U);
        counts.at(i) += 1;
      }
    }
  }
  return counts;
}

/** The ISS and a GPS satellite with their tracks, as the program draws them. */
void satelliteScene(std::vector<ts::GlobeMarker>& markers,
                    std::vector<ts::SpaceLine>& lines)
{
  const std::shared_ptr<const ts::Sgp4> iss =
      std::make_shared<const ts::Sgp4>(orbitFixture("stations.tle"));
  const std::shared_ptr<const ts::Sgp4> gps =
      std::make_shared<const ts::Sgp4>(orbitFixture("gps-ops.tle"));
  ts::JulianDate time = iss->elements().epoch;
  time.fraction += 30.0 / 1440.0;
  ts::OrbitRequest request;
  request.time = time;
  request.tasks.push_back(
      ts::OrbitTask{iss, true, iss->periodMinutes(), iss->periodMinutes()});
  request.tasks.push_back(ts::OrbitTask{gps, true, 0.5 * gps->periodMinutes(),
                                        0.5 * gps->periodMinutes()});
  std::map<const ts::Sgp4*, std::unique_ptr<ts::TrackSampler>> samplers;
  const ts::OrbitResult result = ts::OrbitComputer::compute(request, samplers);
  const std::array<std::array<std::uint8_t, 4>, 2> colours{
      {{255, 204, 0, 255}, {60, 220, 255, 255}}};
  for (std::size_t i = 0; i < result.states.size(); ++i) {
    ts::TrackLook look;
    look.rgba = colours.at(i);
    look.periodMinutes = request.tasks[i].sgp4->periodMinutes();
    lines.push_back(ts::pastTrackLine(result.tracks[i], look));
    lines.push_back(ts::futureTrackLine(result.tracks[i], look));
    markers.push_back(
        ts::satelliteMarker(result.states[i], colours.at(i), 5.0));
  }
}
} // namespace

/** GPU tests for ts/globe: golden images, precision near the surface. */
class TestGlobe : public QObject
{
  Q_OBJECT

private:
  /** Renders @p camera into an image (needs a valid m_gl). */
  /** Renders until nothing is loading any more, then once more. */
  QImage renderSettled(const ts::Camera& camera)
  {
    QImage image;
    const bool settled = QTest::qWaitFor(
        [&]() {
          image = render(camera);
          return !m_renderer->isBusy();
        },
        60000);
    if (!settled) {
      return QImage();
    }
    return render(camera);
  }

  QImage render(const ts::Camera& camera,
                const std::vector<ts::GlobeMarker>& markers = {})
  {
    m_gl->makeCurrent();
    m_renderer->setMarkers(markers);
    ts::PassDesc pass;
    pass.target = m_target.get();
    pass.viewportPx = QRect(0, 0, imageWidthPx, imageHeightPx);
    m_renderer->render(camera, pass, 1.0);
    return m_gl->device().readPixels(*m_target);
  }

  /** A WMS overlay from the mock server, in @p crs. */
  std::shared_ptr<ts::WmsImageryProvider>
  mockWms(const QString& crs,
          const std::optional<ts::GeoRect>& bounds = std::nullopt)
  {
    if (!m_wmsServer) {
      m_wmsServer =
          std::make_unique<ts::test::MockHttpServer>(ts::test::mockWmsHandler(
              std::make_shared<ts::test::MockWmsBehaviour>()));
      ts::NetworkSettings settings;
      settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
      m_network = std::make_shared<ts::NetworkClient>(settings);
    }
    ts::WmsSource source;
    source.url = m_wmsServer->url(QStringLiteral("/wms"));
    source.layers = QStringList{QStringLiteral("pattern")};
    source.styles = QStringList{QString()};
    source.crs = crs;
    source.bounds = bounds;
    source.title = QStringLiteral("Mock overlay");
    return std::make_shared<ts::WmsImageryProvider>(source, m_network);
  }

  /** A WMTS overlay from the mock server, in tile matrix set @p set. */
  std::shared_ptr<ts::WmtsImageryProvider> mockWmts(const QString& set)
  {
    if (!m_wmtsServer) {
      m_wmtsServer = std::make_unique<ts::test::MockHttpServer>();
      m_wmtsServer->setHandler(ts::test::mockWmtsHandler(
          std::make_shared<ts::test::MockWmtsBehaviour>(),
          m_wmtsServer->url()));
    }
    if (!m_network) {
      ts::NetworkSettings settings;
      settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
      m_network = std::make_shared<ts::NetworkClient>(settings);
    }
    QString error;
    const std::optional<ts::WmtsCapabilities> capabilities =
        ts::parseWmtsCapabilities(
            ts::test::mockWmtsCapabilities(m_wmtsServer->url()), &error);
    const std::optional<ts::WmtsSource> source =
        capabilities
            ? ts::wmtsSourceFor(*capabilities, QStringLiteral("pattern"),
                                ts::WmtsChoices{QString(), QString(), set, {}},
                                &error)
            : std::nullopt;
    if (!source) {
      qWarning("mock WMTS: %s", qPrintable(error));
      return nullptr;
    }
    return std::make_shared<ts::WmtsImageryProvider>(*source, m_network);
  }

  std::unique_ptr<ts::test::MockHttpServer> m_wmtsServer;
  std::unique_ptr<ts::test::MockHttpServer> m_wmsServer;
  std::shared_ptr<ts::NetworkClient> m_network;
  std::unique_ptr<ts::test::OffscreenGl> m_gl;
  std::unique_ptr<ts::GlobeRenderer> m_renderer;
  std::unique_ptr<ts::RenderTarget> m_target;
  bool m_softwareRenderer = false;
  std::array<QImage, 3> m_compositingImages;
  /** VMAP0 packs built from the real tile, by configuration id. */
  std::map<QString, QString> m_vmap0Packs;
  std::unique_ptr<QTemporaryDir> m_vmap0Directory;

  /**
   * Returns the pack of configuration @p id (tools/packager/packs) built
   * from the clipped VMAP0 tile (tests/data/vmap0), building it on first
   * use; empty with @p error on failure.
   */
  QString vmap0Pack(const QString& id, QString* error)
  {
    const std::map<QString, QString>::const_iterator built =
        m_vmap0Packs.find(id);
    if (built != m_vmap0Packs.end()) {
      return built->second;
    }
    if (!m_vmap0Directory) {
      m_vmap0Directory = std::make_unique<QTemporaryDir>();
    }
    std::optional<ts::packager::PackConfig> config =
        ts::packager::PackConfig::read(
            QStringLiteral(TS_ROOT_DIR "/tools/packager/packs/") + id +
                QStringLiteral(".json"),
            error);
    if (!config || !config->vmap0) {
      return QString();
    }
    // The fixture is one tile of SOAMAFR: as that library.
    config->sources.resize(1);
    config->sources.front().file = QStringLiteral("vmap0");
    config->sources.front().notices.clear();
    config->sources.front().localNotices.clear();
    config->vmap0->sources = {config->sources.front().id};
    config->vmap0->libraries = {QStringLiteral("soamafr")};
    ts::packager::BuildOptions options;
    options.sourcesDirectory = QStringLiteral(TS_TEST_DATA_DIR);
    options.configDirectory =
        QStringLiteral(TS_ROOT_DIR "/tools/packager/packs");
    options.outputDirectory = m_vmap0Directory->filePath(id);
    options.verifySources = false;
    ts::packager::BuildReport report;
    if (!ts::packager::buildPack(*config, options, &report, error)) {
      return QString();
    }
    m_vmap0Packs[id] = options.outputDirectory;
    return options.outputDirectory;
  }

  // NOLINTNEXTLINE(readability-redundant-access-specifiers): Qt's slots marker
private slots:
  void initTestCase()
  {
    m_gl = std::make_unique<ts::test::OffscreenGl>();
    if (!m_gl->isValid()) {
      return;
    }
    m_renderer = std::make_unique<ts::GlobeRenderer>(m_gl->device());
    QString error;
    QVERIFY2(m_renderer->initialise(&error), qPrintable(error));
    m_target = m_gl->device().createRenderTarget(imageWidthPx, imageHeightPx);
    QVERIFY(m_target);
    const ts::RenderDeviceInfo info = m_gl->device().info();
    m_softwareRenderer = info.renderer.contains(QLatin1String("llvmpipe"));
    qInfo("renderer: %s", qPrintable(info.renderer));
  }

  void cleanupTestCase()
  {
    if (m_gl && m_gl->isValid()) {
      m_gl->makeCurrent();
    }
    m_target.reset();
    m_renderer.reset();
    m_gl.reset();
  }

  void goldenImages_data()
  {
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    QTest::addColumn<double>("headingDeg");
    QTest::addColumn<double>("tiltDeg");
    QTest::addColumn<bool>("withMarkers");
    QTest::newRow("whole-globe") << 20.0 << 0.0 << 1.6e7 << 0.0 << 0.0 << false;
    QTest::newRow("europe-tilted")
        << 48.0 << 10.0 << 3.0e6 << 30.0 << 50.0 << false;
    QTest::newRow("north-pole") << 89.9 << 0.0 << 1.5e7 << 0.0 << 0.0 << false;
    QTest::newRow("antimeridian-markers")
        << -15.0 << 180.0 << 8.0e6 << 0.0 << 20.0 << true;
    QTest::newRow("low-horizon")
        << 45.0 << 7.0 << 3000.0 << 0.0 << 80.0 << false;
  }

  void goldenImages()
  {
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    QFETCH(double, headingDeg);
    QFETCH(double, tiltDeg);
    QFETCH(bool, withMarkers);
    std::vector<ts::GlobeMarker> markers;
    if (withMarkers) {
      for (const double lon : {170.0, 180.0, -170.0}) {
        ts::GlobeMarker marker;
        marker.positionEcef = ts::geodeticToEcef(ts::Geodetic{-15.0, lon, 0.0});
        marker.rgba = {230, 40, 40, 255};
        marker.radiusPx = 5.0;
        markers.push_back(marker);
      }
    }
    const QImage image = render(
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, headingDeg, tiltDeg}),
        markers);
    QVERIFY(!image.isNull());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void checkerboardGolden_data()
  {
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    QTest::addColumn<double>("tiltDeg");
    QTest::newRow("checkerboard-globe") << 20.0 << 10.0 << 1.6e7 << 0.0;
    QTest::newRow("checkerboard-tilted") << 48.0 << 10.0 << 2.0e6 << 50.0;
    QTest::newRow("checkerboard-pole") << 89.0 << 0.0 << 6.0e6 << 20.0;
  }

  void checkerboardGolden()
  {
    // The globe drawn from the tile quadtree with the checkerboard test
    // imagery, once every wanted tile has loaded.
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    QFETCH(double, tiltDeg);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, 0.0, tiltDeg});
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(
        std::make_shared<ts::CheckerboardImageryProvider>());
    QImage image;
    QVERIFY(QTest::qWaitFor(
        [&]() {
          image = render(camera);
          return !m_renderer->isBusy();
        },
        30000));
    // Once more, now that every tile is uploaded.
    image = render(camera);
    const ts::TileStatistics tiles = m_renderer->tileStatistics();
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(nullptr);
    qInfo("%d tiles drawn, deepest level %d", tiles.rendered,
          tiles.deepestLevel);
    QVERIFY(tiles.rendered > 0);
    QCOMPARE(m_gl->device().takeErrors(), QString());
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void layerCompositing_data()
  {
    QTest::addColumn<int32_t>("scene");
    QTest::newRow("two-layers-50") << 0;
    QTest::newRow("group-composite") << 1;
    QTest::newRow("group-multiply") << 2;
  }

  void layerCompositing()
  {
    // 0: the checkerboard with red at 50 % over it. 1 and 2: a collection
    //    at 50 % holding the checkerboard and opaque blue over it; drawn as
    //    one composite image (1) the checkerboard doesn't show through, with
    //    multiplied opacity (2) it does.
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(int32_t, scene);
    const std::shared_ptr<ts::ImageryProvider> checkerboard =
        std::make_shared<ts::CheckerboardImageryProvider>();
    const std::shared_ptr<ts::ImageryProvider> red =
        std::make_shared<ts::SolidColorImageryProvider>(QColor(255, 0, 0), 64);
    const std::shared_ptr<ts::ImageryProvider> blue =
        std::make_shared<ts::SolidColorImageryProvider>(QColor(0, 0, 255), 64);
    std::vector<ts::ImageryLayerDraw> layers;
    std::vector<double> groups;
    if (scene == 0) {
      layers = {{checkerboard, 1.0, -1}, {red, 0.5, -1}};
    } else {
      layers = {{checkerboard, 1.0, 0}, {blue, 1.0, 0}};
      groups = {0.5};
    }
    m_gl->makeCurrent();
    m_renderer->setGroupCompositing(scene != 2);
    m_renderer->setImageryLayers(layers, groups);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{48.0, 10.0, 2.0e6, 0.0, 30.0});
    QImage image;
    QVERIFY(QTest::qWaitFor(
        [&]() {
          image = render(camera);
          return !m_renderer->isBusy();
        },
        30000));
    image = render(camera);
    const ts::TileStatistics tiles = m_renderer->tileStatistics();
    m_gl->makeCurrent();
    m_renderer->setImageryLayers({}, {});
    m_renderer->setGroupCompositing(true);
    QCOMPARE(tiles.groupsComposited, scene == 1 ? 1 : 0);
    QCOMPARE(m_gl->device().takeErrors(), QString());
    // In the middle of the view: red over the checkerboard keeps half its
    // pattern; the composite group is plain blue over the globe; the
    // multiplied one lets the checkerboard through.
    const QColor centre = image.pixelColor(imageWidthPx / 2, imageHeightPx / 2);
    if (scene == 0) {
      QVERIFY2(centre.red() > 140 && centre.blue() < 120,
               qPrintable(centre.name()));
    } else {
      QVERIFY2(centre.blue() > 110 && centre.red() < 100,
               qPrintable(centre.name()));
    }
    m_compositingImages.at(static_cast<std::size_t>(scene)) = image;
    if (scene == 2) {
      QVERIFY(m_compositingImages[1] != m_compositingImages[2]);
    }
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void layersShareSources()
  {
    // Two layers with the same provider share one loader and set of
    // textures; a third provider adds a second source.
    TS_TEST_REQUIRE_GL(*m_gl);
    m_gl->makeCurrent();
    ts::TileRenderer tiles(m_gl->device());
    QString error;
    QVERIFY2(tiles.initialise(&error), qPrintable(error));
    const std::shared_ptr<ts::ImageryProvider> shared =
        std::make_shared<ts::CheckerboardImageryProvider>(64);
    const std::shared_ptr<ts::ImageryProvider> other =
        std::make_shared<ts::SolidColorImageryProvider>(QColor(0, 255, 0));
    tiles.setLayers({{shared, 1.0, -1}, {shared, 0.5, -1}}, {});
    QCOMPARE(tiles.sourceCount(), 1);
    tiles.setLayers({{shared, 1.0, -1}, {other, 0.5, -1}, {shared, 0.2, -1}},
                    {});
    QCOMPARE(tiles.sourceCount(), 2);
    tiles.setLayers({}, {});
    QCOMPARE(tiles.sourceCount(), 0);
  }

  void vectorGolden_data()
  {
    QTest::addColumn<int32_t>("scene");
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    QTest::addColumn<double>("tiltDeg");
    // Fill and outline at the antimeridian (no line along 180°), at the
    // north pole (no line along the pole or 0°/180°), an island with a
    // lake; and the graticule layer over imagery.
    QTest::newRow("vector-antimeridian") << 0 << -15.0 << 180.0 << 6.0e6 << 0.0;
    QTest::newRow("vector-pole") << 0 << 80.0 << 30.0 << 6.0e6 << 30.0;
    QTest::newRow("vector-lake") << 0 << 40.0 << 30.0 << 4.0e6 << 0.0;
    QTest::newRow("graticule-over-imagery")
        << 1 << 20.0 << 10.0 << 1.6e7 << 0.0;
  }

  void vectorGolden()
  {
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(int32_t, scene);
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    QFETCH(double, tiltDeg);
    const QTemporaryDir dir;
    const QString path = QDir(dir.path()).filePath(QStringLiteral("v.gpkg"));
    QVERIFY(writeVectorFixture(path));
    const std::shared_ptr<ts::GeoPackageFeatureProvider> features =
        std::make_shared<ts::GeoPackageFeatureProvider>(path);
    QVERIFY(features->isValid());

    ts::VectorLayerDraw graticule;
    graticule.kind = ts::VectorLayerDraw::Kind::Graticule;
    graticule.style.outline = QColor(255, 255, 255, 200);
    graticule.style.outlineWidthPx = 1.0;
    ts::VectorLayerDraw land;
    land.provider = features;
    land.style.fill = QColor(200, 185, 140, 200);
    land.style.outline = QColor(255, 220, 40);
    land.style.outlineWidthPx = 2.0;

    m_gl->makeCurrent();
    ts::GlobeStyle style;
    style.graticule = false;
    m_renderer->setStyle(style);
    if (scene == 1) {
      // Red, to stand out on the pale checkerboard.
      graticule.style.outline = QColor(220, 30, 30);
      graticule.style.outlineWidthPx = 1.5;
      m_renderer->setImageryProvider(
          std::make_shared<ts::CheckerboardImageryProvider>());
      m_renderer->setVectorLayers({graticule});
    } else {
      // No graticule here: at close range llvmpipe versions place its 1°
      // parallels a pixel apart (their asin differs); it has its own
      // golden images at whole-globe scale.
      m_renderer->setVectorLayers({land});
    }
    const QImage image = renderSettled(
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, 0.0, tiltDeg}));
    const ts::VectorStatistics vectors = m_renderer->vectorStatistics();
    m_gl->makeCurrent();
    m_renderer->setVectorLayers({});
    m_renderer->setImageryProvider(nullptr);
    m_renderer->setStyle(ts::GlobeStyle());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    if (scene == 0) {
      QVERIFY(vectors.tilesDrawn > 0);
      QVERIFY(vectors.triangles > 0);
      QVERIFY(vectors.segments > 0);
    }
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void morphGolden_data()
  {
    QTest::addColumn<QString>("projection");
    QTest::addColumn<double>("morph");
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    QTest::addColumn<double>("headingDeg");
    QTest::addColumn<double>("tiltDeg");
    const QString mercator = QStringLiteral("mercator");
    const QString miller = QStringLiteral("miller");
    const QString web = QStringLiteral("web-mercator");
    // The change from globe to map: t = 0, 0.5 and 1 with the same centre.
    QTest::newRow("morph-0")
        << mercator << 0.0 << 20.0 << 10.0 << 2.6e7 << 0.0 << 0.0;
    QTest::newRow("morph-half")
        << mercator << 0.5 << 20.0 << 10.0 << 2.6e7 << 0.0 << 0.0;
    QTest::newRow("morph-1")
        << mercator << 1.0 << 20.0 << 10.0 << 2.6e7 << 0.0 << 0.0;
    // Part of the way, the camera still turned and tilted.
    QTest::newRow("morph-quarter-tilted")
        << miller << 0.25 << 35.0 << 30.0 << 1.2e7 << 30.0 << 40.0;
    // The other projections, the whole world.
    QTest::newRow("map-miller")
        << miller << 1.0 << 0.0 << 0.0 << 5.0e7 << 0.0 << 0.0;
    QTest::newRow("map-web-mercator")
        << web << 1.0 << 0.0 << -60.0 << 5.0e7 << 0.0 << 0.0;
    // Centred on the antimeridian: polygons that cross it, markers either
    // side, no streaks across the map.
    QTest::newRow("map-antimeridian")
        << mercator << 1.0 << -15.0 << 180.0 << 8.0e6 << 0.0 << 0.0;
    QTest::newRow("map-antimeridian-wide")
        << miller << 1.0 << 10.0 << 179.0 << 5.0e7 << 0.0 << 0.0;
    // The top edge of the Mercator map and Miller's pole.
    QTest::newRow("map-mercator-north")
        << mercator << 1.0 << 78.0 << 30.0 << 4.0e6 << 0.0 << 0.0;
    QTest::newRow("map-miller-north")
        << miller << 1.0 << 80.0 << 30.0 << 2.0e6 << 0.0 << 0.0;
    // Close in: the island with its lake.
    QTest::newRow("map-lake")
        << mercator << 1.0 << 40.0 << 30.0 << 4.0e6 << 0.0 << 0.0;
    // Phase 11a's projections, the whole world; Equidistant Cylindrical
    // also with a standard parallel of 45° (narrower), and Gall–Peters'
    // squashed pole.
    QTest::newRow("map-equidistant-cylindrical")
        << QStringLiteral("equidistant-cylindrical") << 1.0 << 0.0 << 0.0
        << 5.0e7 << 0.0 << 0.0;
    QTest::newRow("map-equidistant-cylindrical-45")
        << QStringLiteral("equidistant-cylindrical@45") << 1.0 << 0.0 << 0.0
        << 5.0e7 << 0.0 << 0.0;
    QTest::newRow("map-gall-stereographic")
        << QStringLiteral("gall-stereographic") << 1.0 << 0.0 << 0.0 << 5.0e7
        << 0.0 << 0.0;
    QTest::newRow("map-gall-peters") << QStringLiteral("gall-peters") << 1.0
                                     << 0.0 << 0.0 << 5.0e7 << 0.0 << 0.0;
    QTest::newRow("map-gall-peters-north")
        << QStringLiteral("gall-peters") << 1.0 << 80.0 << 30.0 << 2.0e6 << 0.0
        << 0.0;
    // Phase 11b's maps that don't repeat: Transverse Mercator's whole
    // domain (curved meridians and parallels, the edge at ±40°), half way
    // there from the globe, UTM close in, Cassini over a pole, and
    // Transverse Mercator centred on 30° E.
    const QString tm = QStringLiteral("transverse-mercator");
    QTest::newRow("map-transverse-mercator")
        << tm << 1.0 << 0.0 << 0.0 << 5.0e7 << 0.0 << 0.0;
    QTest::newRow("morph-half-transverse-mercator")
        << tm << 0.5 << 20.0 << 10.0 << 2.6e7 << 0.0 << 0.0;
    QTest::newRow("map-utm-close") << QStringLiteral("utm@36") << 1.0 << 40.0
                                   << 30.0 << 4.0e6 << 0.0 << 0.0;
    QTest::newRow("map-cassini-pole") << QStringLiteral("cassini") << 1.0
                                      << 80.0 << 0.0 << 6.0e6 << 0.0 << 0.0;
    QTest::newRow("map-transverse-mercator-30e")
        << QStringLiteral("transverse-mercator@30") << 1.0 << 30.0 << 30.0
        << 1.0e7 << 0.0 << 0.0;
    // Phase 11b-2's UK National Grid: its whole domain about 2° W, and
    // half way there from the globe.
    const QString nationalGrid = QStringLiteral("uk-national-grid");
    QTest::newRow("map-national-grid")
        << nationalGrid << 1.0 << 0.0 << -2.0 << 5.0e7 << 0.0 << 0.0;
    QTest::newRow("morph-half-national-grid")
        << nationalGrid << 0.5 << 50.0 << -2.0 << 1.2e7 << 0.0 << 0.0;
    // Phase 11b-3's Rotated Mercator: its whole domain of its own (edges
    // across the map, nothing squashed onto them), and half way there.
    const QString rotated = QStringLiteral("rotated-mercator");
    QTest::newRow("map-rotated-mercator")
        << rotated << 1.0 << 45.0 << 0.0 << 9.0e7 << 0.0 << 0.0;
    QTest::newRow("morph-half-rotated-mercator")
        << rotated << 0.5 << 45.0 << 0.0 << 1.2e7 << 0.0 << 0.0;
    // Phase 11c-1's conics: Lambert's fan (EPSG:3034's, apex at the pole,
    // the cut opposite 10° E left out), half way there, and the
    // Polyconic's curved meridians.
    const QString lambert = QStringLiteral("lambert-conformal-conic");
    QTest::newRow("map-lambert-conformal-conic")
        << lambert << 1.0 << 52.0 << 10.0 << 6.0e7 << 0.0 << 0.0;
    QTest::newRow("morph-half-lambert-conformal-conic")
        << lambert << 0.5 << 52.0 << 10.0 << 1.2e7 << 0.0 << 0.0;
    QTest::newRow("map-polyconic") << QStringLiteral("polyconic") << 1.0 << 20.0
                                   << 0.0 << 4.0e7 << 0.0 << 0.0;
    // Phase 11c-2's azimuthals: the polar Azimuthal Equidistant (nearly
    // the whole Earth, the antipode left out), half way there, the
    // Gnomonic's hemisphere and the Arctic on Polar Stereographic.
    const QString equidistant = QStringLiteral("azimuthal-equidistant");
    QTest::newRow("map-azimuthal-equidistant")
        << equidistant << 1.0 << 90.0 << 0.0 << 9.0e7 << 0.0 << 0.0;
    QTest::newRow("morph-half-azimuthal-equidistant")
        << equidistant << 0.5 << 60.0 << 0.0 << 1.2e7 << 0.0 << 0.0;
    QTest::newRow("map-gnomonic") << QStringLiteral("gnomonic") << 1.0 << 45.0
                                  << 0.0 << 6.0e7 << 0.0 << 0.0;
    QTest::newRow("map-polar-stereographic-north")
        << QStringLiteral("polar-stereographic-north") << 1.0 << 80.0 << -45.0
        << 2.0e7 << 0.0 << 0.0;
    // Phase 11d's world maps, each the whole world, Winkel Tripel also
    // half way there.
    QTest::newRow("map-robinson") << QStringLiteral("robinson") << 1.0 << 0.0
                                  << 0.0 << 5.0e7 << 0.0 << 0.0;
    QTest::newRow("map-mollweide") << QStringLiteral("mollweide") << 1.0 << 0.0
                                   << 0.0 << 5.0e7 << 0.0 << 0.0;
    QTest::newRow("map-winkel-tripel") << QStringLiteral("winkel-tripel") << 1.0
                                       << 0.0 << 0.0 << 5.0e7 << 0.0 << 0.0;
    // Phase 11e's ARC zones: zone 2's band (32° to 48° N), following the
    // view, and the north polar zone.
    QTest::newRow("map-rpf-arc-zone-2") << QStringLiteral("rpf-arc") << 1.0
                                        << 40.0 << 10.0 << 8.0e6 << 0.0 << 0.0;
    QTest::newRow("map-rpf-arc-north") << QStringLiteral("rpf-arc-north") << 1.0
                                       << 90.0 << 0.0 << 4.0e6 << 0.0 << 0.0;
    QTest::newRow("morph-half-winkel-tripel")
        << QStringLiteral("winkel-tripel") << 0.5 << 20.0 << 10.0 << 2.6e7
        << 0.0 << 0.0;
    // Phase 20: the map turned, tilted (fading into the haze and the sky
    // at its far limit), tilted at the limit on a conic, and half way back
    // to the globe from a turned and tilted map.
    QTest::newRow("map-turned")
        << mercator << 1.0 << 40.0 << 30.0 << 4.0e6 << 40.0 << 0.0;
    QTest::newRow("map-tilted")
        << mercator << 1.0 << 38.0 << 26.0 << 1.5e6 << 30.0 << 60.0;
    QTest::newRow("map-tilted-limit-lambert")
        << lambert << 1.0 << 52.0 << 10.0 << 2.0e6 << 200.0
        << ts::Camera::maximumMapTiltDeg;
    QTest::newRow("morph-half-turned-tilted")
        << mercator << 0.5 << 38.0 << 26.0 << 3.0e6 << 30.0 << 40.0;
  }

  void morphGolden()
  {
    // Every kind of layer on the flat map and part of the way to it:
    // imagery, vector fills and outlines, the graticule layer, markers.
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(QString, projection);
    QFETCH(double, morph);
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    QFETCH(double, headingDeg);
    QFETCH(double, tiltDeg);
    const QTemporaryDir dir;
    const QString path = QDir(dir.path()).filePath(QStringLiteral("v.gpkg"));
    QVERIFY(writeVectorFixture(path));
    const std::shared_ptr<ts::GeoPackageFeatureProvider> features =
        std::make_shared<ts::GeoPackageFeatureProvider>(path);
    QVERIFY(features->isValid());

    ts::VectorLayerDraw graticule;
    graticule.kind = ts::VectorLayerDraw::Kind::Graticule;
    graticule.style.outline = QColor(220, 30, 30);
    graticule.style.outlineWidthPx = 1.5;
    ts::VectorLayerDraw land;
    land.provider = features;
    land.style.fill = QColor(200, 185, 140, 200);
    land.style.outline = QColor(255, 220, 40);
    land.style.outlineWidthPx = 2.0;
    std::vector<ts::GlobeMarker> markers;
    for (const double lon : {170.0, 180.0, -170.0}) {
      ts::GlobeMarker marker;
      marker.positionEcef = ts::geodeticToEcef(ts::Geodetic{-15.0, lon, 0.0});
      marker.rgba = {230, 40, 40, 255};
      marker.radiusPx = 5.0;
      markers.push_back(marker);
    }

    ts::Camera camera;
    camera.setViewportSize(imageWidthPx, imageHeightPx);
    const ts::Projection* chosen = projectionFromSpec(projection);
    if (chosen == nullptr) {
      QFAIL("unknown projection");
    }
    camera.setMapProjection(chosen);
    camera.setPose(ts::CameraPose{latDeg, lonDeg, rangeM, headingDeg, tiltDeg});
    camera.setMorph(morph);

    m_gl->makeCurrent();
    ts::GlobeStyle style;
    style.graticule = false;
    m_renderer->setStyle(style);
    m_renderer->setImageryProvider(
        std::make_shared<ts::CheckerboardImageryProvider>());
    m_renderer->setVectorLayers({land, graticule});
    QImage image;
    const bool settled = QTest::qWaitFor(
        [&]() {
          image = render(camera, markers);
          return !m_renderer->isBusy();
        },
        60000);
    image = render(camera, markers);
    const ts::TileStatistics tiles = m_renderer->tileStatistics();
    m_gl->makeCurrent();
    m_renderer->setMarkers({});
    m_renderer->setVectorLayers({});
    m_renderer->setImageryProvider(nullptr);
    m_renderer->setStyle(ts::GlobeStyle());
    QVERIFY(settled);
    QCOMPARE(m_gl->device().takeErrors(), QString());
    qInfo("%d tiles drawn, deepest level %d, %d draw calls", tiles.rendered,
          tiles.deepestLevel, tiles.drawCalls);
    QVERIFY(tiles.rendered > 0);
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void centreStaysFixedThroughTheMorph()
  {
    // A marker at the centre of the view, and one a little way off, drawn
    // at steps from globe to map: the first never leaves the middle of
    // the image, and neither jumps from one step to the next.
    TS_TEST_REQUIRE_GL(*m_gl);
    const ts::CameraPose pose{35.0, 139.0, 4.0e6, 0.0, 0.0};
    ts::GlobeMarker centre;
    centre.positionEcef = ts::geodeticToEcef(ts::Geodetic{35.0, 139.0, 0.0});
    centre.rgba = {255, 0, 0, 255};
    centre.radiusPx = 4.0;
    ts::GlobeMarker other = centre;
    other.positionEcef = ts::geodeticToEcef(ts::Geodetic{43.0, 131.0, 0.0});
    for (const char* id : {"mercator", "miller"}) {
      ts::Camera camera = makeCamera(pose);
      camera.setMapProjection(ts::projectionById(QLatin1String(id)));
      std::optional<QPointF> previous;
      for (int32_t step = 0; step <= 10; ++step) {
        camera.setMorph(0.1 * step);
        const std::optional<QPointF> atCentre =
            redCentroid(render(camera, {centre}));
        if (!atCentre) {
          QFAIL("centre marker not drawn");
        }
        QVERIFY2(std::abs(atCentre->x() - imageWidthPx / 2.0) < 0.6 &&
                     std::abs(atCentre->y() - imageHeightPx / 2.0) < 0.6,
                 qPrintable(QStringLiteral("%1, t = %2: (%3, %4)")
                                .arg(QLatin1String(id))
                                .arg(0.1 * step)
                                .arg(atCentre->x())
                                .arg(atCentre->y())));
        const std::optional<QPointF> atOther =
            redCentroid(render(camera, {other}));
        if (!atOther) {
          QFAIL("second marker not drawn");
        }
        // Where the camera says it is, at both ends of the change.
        if (step == 0 || step == 10) {
          const ts::ScreenPoint expected =
              camera.projectGeodetic(43.0, 131.0)
                  .value_or(ts::ScreenPoint{-1e9, -1e9, 0.0});
          QVERIFY(std::abs(atOther->x() - expected.xPx) < 0.6 &&
                  std::abs(atOther->y() - expected.yPx) < 0.6);
        }
        if (previous) {
          QVERIFY(std::hypot(atOther->x() - previous->x(),
                             atOther->y() - previous->y()) < 4.0);
        }
        previous = atOther;
      }
    }
    m_gl->makeCurrent();
    m_renderer->setMarkers({});
    QCOMPARE(m_gl->device().takeErrors(), QString());
  }

  void mapWrapsAroundTheWorld()
  {
    // Zoomed right out on the antimeridian the map fills the view from
    // edge to edge: the tiles that straddle the wrap are drawn on both
    // sides, and no column of pixels is left as background.
    TS_TEST_REQUIRE_GL(*m_gl);
    ts::Camera camera;
    camera.setViewportSize(imageWidthPx, imageHeightPx);
    camera.setMapProjection(ts::projectionById(QStringLiteral("miller")));
    camera.setMorph(1.0);
    camera.setPose(ts::CameraPose{0.0, 170.0, 5.0e7, 0.0, 0.0});
    m_gl->makeCurrent();
    ts::GlobeStyle style;
    style.graticule = false;
    m_renderer->setStyle(style);
    const QImage image = renderSettled(camera);
    const ts::TileStatistics tiles = m_renderer->tileStatistics();
    m_gl->makeCurrent();
    m_renderer->setStyle(ts::GlobeStyle());
    QVERIFY(!image.isNull());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    // 8 × 4 tiles of level 2, and one column of 4 twice.
    QCOMPARE(tiles.deepestLevel, 2);
    QCOMPARE(tiles.rendered, 36);
    const QColor background(image.pixel(2, 2));
    const QColor sea(image.pixel(imageWidthPx / 2, imageHeightPx / 2));
    QVERIFY(sea.blue() > background.blue() + 40);
    for (int32_t x = 0; x < imageWidthPx; ++x) {
      const QColor pixel(image.pixel(x, imageHeightPx / 2));
      QVERIFY2(std::abs(pixel.blue() - sea.blue()) <= 2,
               qPrintable(QStringLiteral("column %1").arg(x)));
    }
    // The flat map is not shaded: the surface colour itself.
    const ts::GlobeStyle defaults;
    QVERIFY(std::abs(sea.red() - to8Bit(defaults.surfaceRgb[0])) <= 2);
    QVERIFY(std::abs(sea.green() - to8Bit(defaults.surfaceRgb[1])) <= 2);
    QVERIFY(std::abs(sea.blue() - to8Bit(defaults.surfaceRgb[2])) <= 2);
  }

  void morphHoldsFrameRate()
  {
    // The change from globe to map with every kind of layer on screen,
    // frame by frame as the application draws it. The budget on software
    // rendering is 10 frames per second (20 on the reference hardware, a
    // GPU several times faster than this); the limit here leaves room for
    // a loaded CI machine.
    TS_TEST_REQUIRE_GL(*m_gl);
    const QTemporaryDir dir;
    const QString path = QDir(dir.path()).filePath(QStringLiteral("v.gpkg"));
    QVERIFY(writeVectorFixture(path));
    ts::VectorLayerDraw graticule;
    graticule.kind = ts::VectorLayerDraw::Kind::Graticule;
    ts::VectorLayerDraw land;
    land.provider = std::make_shared<ts::GeoPackageFeatureProvider>(path);
    land.style.fill = QColor(200, 185, 140, 200);
    land.style.outline = QColor(255, 220, 40);
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(
        std::make_shared<ts::CheckerboardImageryProvider>());
    m_renderer->setVectorLayers({land, graticule});
    ts::Camera camera = makeCamera(ts::CameraPose{20.0, 10.0, 2.0e7, 0.0, 0.0});
    // Everything loaded at both ends first, as after a moment's viewing.
    for (const double morph : {1.0, 0.5, 0.0}) {
      camera.setMorph(morph);
      QVERIFY(!renderSettled(camera).isNull());
    }
    constexpr int32_t frames = 30;
    std::vector<double> timesS;
    for (int32_t frame = 0; frame <= frames; ++frame) {
      const double u = static_cast<double>(frame) / frames;
      camera.setMorph(u * u * (3.0 - 2.0 * u));
      QElapsedTimer one;
      one.start();
      render(camera);
      timesS.push_back(static_cast<double>(one.nsecsElapsed()) * 1e-9);
    }
    m_gl->makeCurrent();
    m_renderer->setVectorLayers({});
    m_renderer->setImageryProvider(nullptr);
    QCOMPARE(m_gl->device().takeErrors(), QString());
    std::sort(timesS.begin(), timesS.end());
    double sumS = 0.0;
    for (const double timeS : timesS) {
      sumS += timeS;
    }
    const double meanS = sumS / static_cast<double>(timesS.size());
    // The quarter of the frames least disturbed by whatever else the
    // machine is doing: a busy CI runner must not fail this.
    const double quartileS = timesS.at(timesS.size() / 4);
    qInfo("morph: %.1f ms per frame (%.0f fps), fastest quarter within "
          "%.1f ms, slowest %.1f ms",
          meanS * 1000.0, 1.0 / meanS, quartileS * 1000.0,
          timesS.back() * 1000.0);
#if defined(TS_INSTRUMENTED_BUILD) || !defined(QT_NO_DEBUG)
    // Sanitizer, coverage and debug builds are several times slower: the
    // change is drawn (and checked for errors) but not timed.
    Q_UNUSED(quartileS)
#else
    QVERIFY2(quartileS < 0.1, "the morph is below 10 frames per second");
#endif
  }

  void wmsGolden_data()
  {
    QTest::addColumn<QString>("crs");
    QTest::addColumn<QString>("projection");
    QTest::addColumn<double>("morph");
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    const QString geographic = QStringLiteral("EPSG:4326");
    const QString webMercator = QStringLiteral("EPSG:3857");
    const QString none;
    const QString mercator = QStringLiteral("mercator");
    const QString miller = QStringLiteral("miller");
    // The mock WMS's test pattern at 50 % over plain imagery: red discs at
    // every 30° intersection, yellow bands at 45° N and 60° E.
    QTest::newRow("wms-overlay-globe")
        << geographic << none << 0.0 << 30.0 << 20.0 << 1.6e7;
    QTest::newRow("wms-overlay-close")
        << geographic << none << 0.0 << 45.0 << 60.0 << 1.5e6;
    QTest::newRow("wms-overlay-mercator")
        << geographic << mercator << 1.0 << 30.0 << 20.0 << 2.6e7;
    QTest::newRow("wms-overlay-miller")
        << geographic << miller << 1.0 << 0.0 << 0.0 << 5.0e7;
    QTest::newRow("wms-overlay-morph-half")
        << geographic << mercator << 0.5 << 30.0 << 20.0 << 2.6e7;
    // A server with Web Mercator only: reprojected, the same picture.
    QTest::newRow("wms-web-mercator-globe")
        << webMercator << none << 0.0 << 30.0 << 20.0 << 1.6e7;
    QTest::newRow("wms-web-mercator-close")
        << webMercator << none << 0.0 << 60.0 << 30.0 << 1.2e6;
    QTest::newRow("wms-web-mercator-miller")
        << webMercator << miller << 1.0 << 0.0 << 0.0 << 5.0e7;
  }

  void wmsGolden()
  {
    // A WMS layer at 50 % over plain imagery, through the network client
    // and the provider, from a local mock server.
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(QString, crs);
    QFETCH(QString, projection);
    QFETCH(double, morph);
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    ts::Camera camera =
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, 0.0, 0.0});
    if (!projection.isEmpty()) {
      camera.setMapProjection(projectionFromSpec(projection));
      camera.setMorph(morph);
    }
    m_gl->makeCurrent();
    ts::GlobeStyle style;
    style.graticule = false;
    m_renderer->setStyle(style);
    m_renderer->setImageryLayers(
        {ts::ImageryLayerDraw{std::make_shared<ts::SolidColorImageryProvider>(
                                  QColor(40, 80, 130), 64, 12),
                              1.0, -1},
         ts::ImageryLayerDraw{mockWms(crs), 0.5, -1}},
        {});
    const QImage image = renderSettled(camera);
    const ts::TileStatistics tiles = m_renderer->tileStatistics();
    m_gl->makeCurrent();
    m_renderer->setImageryLayers({}, {});
    m_renderer->setStyle(ts::GlobeStyle());
    QVERIFY(!image.isNull());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    qInfo("%d tiles drawn, deepest level %d", tiles.rendered,
          tiles.deepestLevel);
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void rpfGolden_data()
  {
    QTest::addColumn<QString>("chart");
    QTest::addColumn<QString>("projection");
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    // The synthetic CADRG charts (tests/data/rpf): on the globe, on the
    // ARC zones (following the view, so zone 2) and on the north polar
    // zone, each square of the checker 0.25° on a side.
    QTest::newRow("rpf-zone2-globe")
        << QStringLiteral("zone2") << QString() << 35.25 << 2.75 << 3.0e5;
    QTest::newRow("rpf-zone2-arc")
        << QStringLiteral("zone2") << QStringLiteral("rpf-arc") << 35.25 << 2.75
        << 3.0e5;
    QTest::newRow("rpf-zone9-arc-north")
        << QStringLiteral("zone9") << QStringLiteral("rpf-arc-north") << 85.75
        << 5.0 << 4.0e5;
  }

  void rpfGolden()
  {
    // An RPF chart, read through its A.TOC by GDAL, over plain imagery.
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(QString, chart);
    QFETCH(QString, projection);
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    const std::shared_ptr<ts::GdalImageryProvider> provider =
        std::make_shared<ts::GdalImageryProvider>(
            QStringLiteral(TS_TEST_DATA_DIR "/rpf/") + chart +
            QStringLiteral("/RPF/A.TOC"));
    QVERIFY2(provider->isValid(), qPrintable(provider->errorString()));
    ts::Camera camera =
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, 0.0, 0.0});
    if (!projection.isEmpty()) {
      camera.setMapProjection(projectionFromSpec(projection));
      camera.setMorph(1.0);
    }
    m_gl->makeCurrent();
    ts::GlobeStyle style;
    style.graticule = false;
    m_renderer->setStyle(style);
    m_renderer->setImageryLayers(
        {ts::ImageryLayerDraw{std::make_shared<ts::SolidColorImageryProvider>(
                                  QColor(40, 80, 130), 64, 12),
                              1.0, -1},
         ts::ImageryLayerDraw{provider, 1.0, -1}},
        {});
    const QImage image = renderSettled(camera);
    const ts::TileStatistics tiles = m_renderer->tileStatistics();
    m_gl->makeCurrent();
    m_renderer->setImageryLayers({}, {});
    m_renderer->setStyle(ts::GlobeStyle());
    QVERIFY(!image.isNull());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    qInfo("%d tiles drawn, deepest level %d", tiles.rendered,
          tiles.deepestLevel);
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void wmsWebMercatorMatchesGeographic_data()
  {
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    QTest::addColumn<double>("morph");
    QTest::newRow("whole globe") << 30.0 << 20.0 << 1.6e7 << 0.0;
    QTest::newRow("60 N close") << 60.0 << 30.0 << 1.2e6 << 0.0;
    // Up to 86° N, near the Mercator limit (the pole itself has no
    // Mercator picture, so it is left out of view).
    QTest::newRow("near the limit") << 72.0 << -30.0 << 4.0e6 << 0.0;
    QTest::newRow("deep") << 45.0 << 60.0 << 2.0e4 << 0.0;
    QTest::newRow("Mercator map") << 30.0 << 0.0 << 2.0e7 << 1.0;
  }

  void wmsWebMercatorMatchesGeographic()
  {
    // The same test pattern from a server in EPSG:4326 and from one in
    // Web Mercator, reprojected in the shader: the two pictures agree
    // except along the edges of the discs and lines, where the two images'
    // pixels fall differently.
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    QFETCH(double, morph);
    ts::Camera camera =
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, 0.0, 0.0});
    camera.setMorph(morph);
    std::array<QImage, 2> images;
    for (std::size_t i = 0; i < images.size(); ++i) {
      m_gl->makeCurrent();
      ts::GlobeStyle style;
      style.graticule = false;
      m_renderer->setStyle(style);
      m_renderer->setImageryLayers(
          {ts::ImageryLayerDraw{mockWms(i == 0 ? QStringLiteral("EPSG:4326")
                                               : QStringLiteral("EPSG:3857")),
                                1.0, -1}},
          {});
      images.at(i) = renderSettled(camera);
      QVERIFY(!images.at(i).isNull());
    }
    m_gl->makeCurrent();
    m_renderer->setImageryLayers({}, {});
    m_renderer->setStyle(ts::GlobeStyle());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    const ts::test::ImageDifference difference =
        ts::test::compareImages(images[1], images[0], 40);
    qInfo("%.2f %% of pixels differ by more than 40",
          100.0 * difference.differingFraction);
    // Something is drawn at all: the pattern covers part of the view.
    int32_t coloured = 0;
    for (int32_t y = 0; y < images[0].height(); y += 4) {
      for (int32_t x = 0; x < images[0].width(); x += 4) {
        const QColor c(images[0].pixel(x, y));
        coloured += c.red() > c.blue() + 60 ? 1 : 0;
      }
    }
    QVERIFY(coloured > 20);
    QVERIFY2(difference.differingFraction < 0.01,
             qPrintable(QString::number(difference.differingFraction)));
  }

  void wmtsMatchesWms_data()
  {
    QTest::addColumn<QString>("set");
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    QTest::addColumn<double>("morph");
    QTest::addColumn<double>("worst");
    // Lined up with the quadtree, each quadtree tile is one WMTS tile with
    // the same pixels as the WMS image: the same picture.
    QTest::newRow("CRS84 quad globe") << QStringLiteral("WorldCRS84Quad")
                                      << 30.0 << 20.0 << 1.6e7 << 0.0 << 0.0005;
    QTest::newRow("CRS84 quad close") << QStringLiteral("WorldCRS84Quad")
                                      << 45.0 << 60.0 << 1.5e6 << 0.0 << 0.0005;
    QTest::newRow("CRS84 quad Mercator map")
        << QStringLiteral("WorldCRS84Quad") << 30.0 << 0.0 << 2.0e7 << 1.0
        << 0.0005;
    // Web Mercator tiles (reprojected in the shader) and tiles cut from
    // a grid that isn't the quadtree's: the same but along edges.
    QTest::newRow("Google globe") << QStringLiteral("GoogleMapsCompatible")
                                  << 30.0 << 20.0 << 1.6e7 << 0.0 << 0.01;
    QTest::newRow("Google close") << QStringLiteral("GoogleMapsCompatible")
                                  << 60.0 << 30.0 << 1.2e6 << 0.0 << 0.01;
    QTest::newRow("Google morph half") << QStringLiteral("GoogleMapsCompatible")
                                       << 30.0 << 0.0 << 2.0e7 << 0.5 << 0.01;
    QTest::newRow("offset globe")
        << QStringLiteral("Offset4326") << 30.0 << 20.0 << 1.6e7 << 0.0 << 0.01;
    QTest::newRow("offset close")
        << QStringLiteral("Offset4326") << 45.0 << 60.0 << 1.5e6 << 0.0 << 0.01;
    // British National Grid tiles, reprojected through PROJ on the CPU:
    // lined up with the WMS picture over the grid's area.
    QTest::newRow("BNG over Britain") << QStringLiteral("BritishNationalGrid")
                                      << 56.0 << -2.0 << 2.5e6 << 0.0 << 0.01;
    QTest::newRow("BNG close") << QStringLiteral("BritishNationalGrid") << 59.0
                               << 0.5 << 6.0e5 << 0.0 << 0.01;
    QTest::newRow("BNG Mercator map") << QStringLiteral("BritishNationalGrid")
                                      << 56.0 << -2.0 << 3.0e6 << 1.0 << 0.01;
  }

  void wmtsMatchesWms()
  {
    // The mock WMTS draws the mock WMS's picture of the Earth: a WMTS
    // layer in any tile matrix set looks like the WMS layer, on the globe,
    // the map and between.
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(QString, set);
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    QFETCH(double, morph);
    QFETCH(double, worst);
    ts::Camera camera =
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, 0.0, 0.0});
    camera.setMorph(morph);
    std::array<QImage, 2> images;
    for (std::size_t i = 0; i < images.size(); ++i) {
      m_gl->makeCurrent();
      ts::GlobeStyle style;
      style.graticule = false;
      m_renderer->setStyle(style);
      std::shared_ptr<ts::ImageryProvider> overlay;
      const std::shared_ptr<ts::WmtsImageryProvider> wmts = mockWmts(set);
      if (i == 0) {
        // Only where the WMTS layer has data (a national grid's area).
        overlay = mockWms(QStringLiteral("EPSG:4326"),
                          wmts ? wmts->source().bounds : std::nullopt);
      } else {
        overlay = wmts;
      }
      if (overlay == nullptr) {
        QFAIL("no overlay");
      }
      m_renderer->setImageryLayers({ts::ImageryLayerDraw{overlay, 1.0, -1}},
                                   {});
      images.at(i) = renderSettled(camera);
      QVERIFY(!images.at(i).isNull());
    }
    m_gl->makeCurrent();
    m_renderer->setImageryLayers({}, {});
    m_renderer->setStyle(ts::GlobeStyle());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    // A layer limited to an area (a national grid's area of use) is
    // compared there only: the WMS layer's bounds keep whole tiles, so
    // it draws on past them.
    const std::shared_ptr<ts::WmtsImageryProvider> limited = mockWmts(set);
    if (limited && ts::isProjectedWmtsCrs(limited->source().crs)) {
      keepOnlyInside(images, camera, limited->source().bounds);
    }
    const ts::test::ImageDifference difference =
        ts::test::compareImages(images[1], images[0], 40);
    qInfo("%.3f %% of pixels differ by more than 40",
          100.0 * difference.differingFraction);
    int32_t coloured = 0;
    for (int32_t y = 0; y < images[1].height(); y += 4) {
      for (int32_t x = 0; x < images[1].width(); x += 4) {
        const QColor c(images[1].pixel(x, y));
        coloured += c.red() > c.blue() + 60 ? 1 : 0;
      }
    }
    QVERIFY(coloured > 20);
    QVERIFY2(difference.differingFraction <= worst,
             qPrintable(QString::number(difference.differingFraction)));
  }

  void wmtsGolden_data()
  {
    QTest::addColumn<QString>("set");
    QTest::addColumn<QString>("projection");
    QTest::addColumn<double>("morph");
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    const QString quad = QStringLiteral("WorldCRS84Quad");
    const QString google = QStringLiteral("GoogleMapsCompatible");
    const QString none;
    const QString mercator = QStringLiteral("mercator");
    const QString miller = QStringLiteral("miller");
    // The mock WMTS's test pattern at 50 % over plain imagery, in the
    // EPSG:4326 and the Web Mercator tile matrix sets.
    QTest::newRow("wmts-4326-globe")
        << quad << none << 0.0 << 30.0 << 20.0 << 1.6e7;
    QTest::newRow("wmts-3857-globe")
        << google << none << 0.0 << 30.0 << 20.0 << 1.6e7;
    QTest::newRow("wmts-3857-close")
        << google << none << 0.0 << 60.0 << 30.0 << 1.2e6;
    QTest::newRow("wmts-4326-mercator")
        << quad << mercator << 1.0 << 30.0 << 20.0 << 2.6e7;
    QTest::newRow("wmts-3857-miller")
        << google << miller << 1.0 << 0.0 << 0.0 << 5.0e7;
    QTest::newRow("wmts-3857-morph-half")
        << google << mercator << 0.5 << 30.0 << 20.0 << 2.6e7;
    // British National Grid tiles over Britain, reprojected through PROJ.
    QTest::newRow("wmts-bng-britain") << QStringLiteral("BritishNationalGrid")
                                      << none << 0.0 << 56.0 << -2.0 << 2.5e6;
  }

  void wmtsGolden()
  {
    // A WMTS layer at 50 % over plain imagery, through the network client
    // and the provider, from a local mock server.
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(QString, set);
    QFETCH(QString, projection);
    QFETCH(double, morph);
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    ts::Camera camera =
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, 0.0, 0.0});
    if (!projection.isEmpty()) {
      camera.setMapProjection(ts::projectionById(projection));
      camera.setMorph(morph);
    }
    const std::shared_ptr<ts::WmtsImageryProvider> overlay = mockWmts(set);
    if (overlay == nullptr) {
      QFAIL("no overlay");
    }
    m_gl->makeCurrent();
    ts::GlobeStyle style;
    style.graticule = false;
    m_renderer->setStyle(style);
    m_renderer->setImageryLayers(
        {ts::ImageryLayerDraw{std::make_shared<ts::SolidColorImageryProvider>(
                                  QColor(40, 80, 130), 64, 12),
                              1.0, -1},
         ts::ImageryLayerDraw{overlay, 0.5, -1}},
        {});
    const QImage image = renderSettled(camera);
    m_gl->makeCurrent();
    m_renderer->setImageryLayers({}, {});
    m_renderer->setStyle(ts::GlobeStyle());
    QVERIFY(!image.isNull());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void offlineRegionGolden()
  {
    // A region over the British Isles downloaded from the mock WMS, then
    // drawn with no network at all (every request goes to a proxy on a
    // closed port): the same picture as online, from the pinned tiles.
    TS_TEST_REQUIRE_GL(*m_gl);
    QTemporaryDir cacheDir;
    ts::WmsProviderSettings settings;
    settings.cacheDirectory = cacheDir.path();
    ts::WmsSource source;
    source.layers = QStringList{QStringLiteral("pattern")};
    source.styles = QStringList{QString()};
    source.title = QStringLiteral("Mock overlay");
    const ts::RegionGeometry region =
        ts::RegionGeometry::rectangle(-12.0, 49.0, 8.0, 62.0);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{55.0, -2.0, 7.0e5, 0.0, 0.0});
    const std::shared_ptr<ts::ImageryProvider> background =
        std::make_shared<ts::SolidColorImageryProvider>(QColor(40, 80, 130), 64,
                                                        12);
    std::array<QImage, 2> images;
    {
      ts::test::MockHttpServer server(ts::test::mockWmsHandler(
          std::make_shared<ts::test::MockWmsBehaviour>()));
      source.url = server.url(QStringLiteral("/wms"));
      ts::NetworkSettings online;
      online.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
      std::shared_ptr<ts::WmsImageryProvider> provider =
          std::make_shared<ts::WmsImageryProvider>(
              source, std::make_shared<ts::NetworkClient>(online), settings);
      ts::RegionDownload download(provider, 1, region, 0, 8);
      QString error;
      QVERIFY(download.start({}, &error));
      QVERIFY(download.waitUntilSettled(std::chrono::seconds(120)));
      QCOMPARE(download.progress().state,
               ts::RegionDownloadProgress::State::Finished);
      QCOMPARE(download.progress().failedTiles, int64_t{0});
      m_gl->makeCurrent();
      ts::GlobeStyle style;
      style.graticule = false;
      m_renderer->setStyle(style);
      m_renderer->setImageryLayers({ts::ImageryLayerDraw{background, 1.0, -1},
                                    ts::ImageryLayerDraw{provider, 0.5, -1}},
                                   {});
      images[0] = renderSettled(camera);
      qInfo("online: deepest level %d",
            m_renderer->tileStatistics().deepestLevel);
      m_gl->makeCurrent();
      m_renderer->setImageryLayers({}, {});
    }
    // The server is gone, and the network is unusable anyway.
    ts::test::MockHttpServer closed;
    ts::NetworkSettings offline;
    offline.proxy = QNetworkProxy(QNetworkProxy::HttpProxy,
                                  QStringLiteral("127.0.0.1"), closed.port());
    offline.attempts = 1;
    std::shared_ptr<ts::NetworkClient> network =
        std::make_shared<ts::NetworkClient>(offline);
    std::shared_ptr<ts::WmsImageryProvider> provider =
        std::make_shared<ts::WmsImageryProvider>(source, network, settings);
    QCOMPARE(provider->diskCache()->regionUsage(1).tiles,
             ts::countRegionTiles(region, 0, 8, std::nullopt).total());
    m_gl->makeCurrent();
    m_renderer->setImageryLayers({ts::ImageryLayerDraw{background, 1.0, -1},
                                  ts::ImageryLayerDraw{provider, 0.5, -1}},
                                 {});
    images[1] = renderSettled(camera);
    const ts::TileStatistics tiles = m_renderer->tileStatistics();
    m_gl->makeCurrent();
    m_renderer->setImageryLayers({}, {});
    m_renderer->setStyle(ts::GlobeStyle());
    QVERIFY(!images[0].isNull() && !images[1].isNull());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    qInfo("offline: %d tiles drawn, deepest level %d", tiles.rendered,
          tiles.deepestLevel);
    QVERIFY(tiles.deepestLevel >= 5);
    // Not one request, and every tile from the disk.
    QCOMPARE(network->attemptsSent(), 0);
    QCOMPARE(provider->statistics().requests, 0);
    QVERIFY(provider->statistics().cacheHits > 0);
    QCOMPARE(images[1], images[0]);
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        images[1], QStringLiteral("offline-region-uk"), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void basePackWmsGolden_data()
  {
    QTest::addColumn<QString>("projection");
    QTest::addColumn<double>("morph");
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    QTest::newRow("base-wms-globe")
        << QString() << 0.0 << 30.0 << 20.0 << 1.6e7;
    QTest::newRow("base-wms-mercator")
        << QStringLiteral("mercator") << 1.0 << 30.0 << 20.0 << 2.6e7;
    QTest::newRow("base-wms-miller")
        << QStringLiteral("miller") << 1.0 << 0.0 << 0.0 << 5.0e7;
    QTest::newRow("base-wms-morph-half")
        << QStringLiteral("mercator") << 0.5 << 30.0 << 20.0 << 2.6e7;
  }

  void basePackWmsGolden()
  {
    // The mock WMS overlay at 50 % over Blue Marble, set up through the
    // layer tree and the provider registry like the application.
    TS_TEST_REQUIRE_GL(*m_gl);
    const QString directory = qEnvironmentVariable("TS_BASEPACK_DIR");
    if (directory.isEmpty()) {
      QSKIP("TS_BASEPACK_DIR is not set.");
    }
    QFETCH(QString, projection);
    QFETCH(double, morph);
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    QString error;
    const std::optional<ts::PackManifest> manifest =
        ts::PackManifest::read(directory, &error);
    if (!manifest) {
      QFAIL(qPrintable(error));
    }
    const std::shared_ptr<ts::WmsImageryProvider> mock =
        mockWms(QStringLiteral("EPSG:4326"));
    ts::ProviderRegistry registry;
    registry.setBasePack(directory, *manifest);
    registry.setWmsNetwork(m_network, ts::WmsProviderSettings());
    ts::LayerTreeModel model;
    for (const ts::LayerDefinition& definition :
         ts::builtinLayerDefinitions()) {
      if (definition.id == QLatin1String("builtin:bluemarble")) {
        model.addLayer(definition);
      }
    }
    ts::LayerDefinition wms;
    wms.id = QStringLiteral("user:mock-wms");
    wms.origin = ts::LayerOrigin::User;
    wms.type = ts::LayerType::Imagery;
    wms.name = QStringLiteral("Mock overlay");
    wms.source = mock->source().toJson();
    const QModelIndex overlay = model.addLayer(wms);
    model.setData(overlay, 0.5, ts::LayerTreeModel::OpacityRole);
    const ts::LayerStack stack = model.layerStack();
    QStringList errors;
    const ts::ImageryDraws imagery =
        ts::imageryDrawsForStack(stack, registry, &errors);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char(' '))));
    QCOMPARE(imagery.layers.size(), std::size_t{2});
    QCOMPARE(imagery.layers.back().alpha, 0.5);

    m_gl->makeCurrent();
    ts::GlobeStyle style;
    style.graticule = false;
    m_renderer->setStyle(style);
    m_renderer->setImageryLayers(imagery.layers, imagery.groupAlphas);
    ts::Camera camera =
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, 0.0, 0.0});
    if (!projection.isEmpty()) {
      camera.setMapProjection(ts::projectionById(projection));
      camera.setMorph(morph);
    }
    const QImage image = renderSettled(camera);
    m_gl->makeCurrent();
    m_renderer->setImageryLayers({}, {});
    m_renderer->setStyle(ts::GlobeStyle());
    QVERIFY(!image.isNull());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void basePackGolden_data()
  {
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    QTest::addColumn<double>("tiltDeg");
    QTest::addColumn<QString>("projection");
    QTest::addColumn<double>("morph");
    // The terrain's exaggeration; 0 for none (no elevation source).
    QTest::addColumn<double>("terrain");
    const QString none;
    QTest::newRow("base-uk")
        << 54.5 << -3.0 << 2.0e6 << 0.0 << none << 0.0 << 0.0;
    QTest::newRow("base-uk-close")
        << 51.5 << -0.5 << 1.5e5 << 30.0 << none << 0.0 << 0.0;
    // About 5 km up, looking towards the horizon: the coarse tiles far
    // away lie below the ray-cast ellipsoid by more along the ray than at
    // a steep angle, and must still cover it (the plain blue showed).
    QTest::newRow("base-uk-grazing")
        << 51.5 << -0.5 << 3.0e4 << 80.0 << none << 0.0 << 0.0;
    QTest::newRow("base-pacific-antimeridian")
        << -17.5 << 179.5 << 1.5e6 << 0.0 << none << 0.0 << 0.0;
    QTest::newRow("base-pacific-wide")
        << 0.0 << 180.0 << 1.2e7 << 0.0 << none << 0.0 << 0.0;
    QTest::newRow("base-north-pole")
        << 90.0 << 0.0 << 7.0e6 << 0.0 << none << 0.0 << 0.0;
    QTest::newRow("base-north-pole-close")
        << 82.0 << -40.0 << 1.5e6 << 30.0 << none << 0.0 << 0.0;
    QTest::newRow("base-south-pole")
        << -90.0 << 0.0 << 9.0e6 << 0.0 << none << 0.0 << 0.0;
    QTest::newRow("base-south-pole-close")
        << -77.5 << 166.0 << 6.0e5 << 30.0 << none << 0.0 << 0.0;
    // The same data on the flat maps and half-way there.
    const QString mercator = QStringLiteral("mercator");
    const QString miller = QStringLiteral("miller");
    QTest::newRow("base-map-mercator-world")
        << 0.0 << 0.0 << 5.0e7 << 0.0 << mercator << 1.0 << 0.0;
    QTest::newRow("base-map-miller-world")
        << 0.0 << 0.0 << 5.0e7 << 0.0 << miller << 1.0 << 0.0;
    QTest::newRow("base-map-uk")
        << 54.5 << -3.0 << 2.0e6 << 0.0 << mercator << 1.0 << 0.0;
    QTest::newRow("base-map-pacific-antimeridian")
        << -17.5 << 179.5 << 1.5e6 << 0.0 << mercator << 1.0 << 0.0;
    QTest::newRow("base-map-pacific-wide")
        << 0.0 << 180.0 << 1.2e7 << 0.0 << miller << 1.0 << 0.0;
    QTest::newRow("base-map-antarctica")
        << -75.0 << 0.0 << 6.0e6 << 0.0 << miller << 1.0 << 0.0;
    QTest::newRow("base-morph-half")
        << 30.0 << 20.0 << 1.6e7 << 0.0 << mercator << 0.5 << 0.0;
    // Great Britain on the National Grid, the shoreline on the imagery
    // (the Helmert datum: no OSTN15 pack here), and close in on the
    // Thames estuary.
    const QString nationalGrid = QStringLiteral("uk-national-grid");
    QTest::newRow("base-map-national-grid-uk")
        << 54.5 << -3.0 << 2.0e6 << 0.0 << nationalGrid << 1.0 << 0.0;
    QTest::newRow("base-map-national-grid-thames")
        << 51.5 << 0.6 << 1.5e5 << 0.0 << nationalGrid << 1.0 << 0.0;
    // The Alps on the ARC zones following the view: zone 2 (32° to 48° N),
    // as an ONC chart is made, the band's north edge across the top.
    QTest::newRow("base-map-rpf-arc-alps")
        << 46.0 << 9.0 << 1.5e6 << 0.0 << QStringLiteral("rpf-arc") << 1.0
        << 0.0;
    // The world on Winkel Tripel, as atlases show it.
    QTest::newRow("base-map-winkel-tripel-world")
        << 0.0 << 0.0 << 5.0e7 << 0.0 << QStringLiteral("winkel-tripel") << 1.0
        << 0.0;
    // Antarctica on Polar Stereographic (EPSG:3031), and the world about
    // London on the Azimuthal Equidistant.
    QTest::newRow("base-map-polar-south")
        << -90.0 << 0.0 << 1.2e7 << 0.0
        << QStringLiteral("polar-stereographic-south") << 1.0 << 0.0;
    QTest::newRow("base-map-azimuthal-london")
        << 51.5 << -0.1 << 6.0e7 << 0.0
        << QStringLiteral("azimuthal-equidistant@51.5,-0.1") << 1.0 << 0.0;
    // Europe on Lambert Conformal Conic (EPSG:3034).
    QTest::newRow("base-map-lambert-europe")
        << 52.0 << 10.0 << 5.0e6 << 0.0
        << QStringLiteral("lambert-conformal-conic") << 1.0 << 0.0;
    // The North Atlantic on the centre line from London to New York.
    QTest::newRow("base-map-rotated-mercator-atlantic")
        << 52.2398 << -41.2968 << 7.0e6 << 0.0
        << QStringLiteral("rotated-mercator@52.2398,-41.2968,75.3336") << 1.0
        << 0.0;
    // Terrain from the base pack's DEM, looking across mountains.
    QTest::newRow("base-terrain-alps")
        << 46.0 << 8.0 << 2.0e5 << 65.0 << none << 0.0 << 3.0;
    QTest::newRow("base-terrain-everest")
        << 27.0 << 86.9 << 3.0e5 << 65.0 << none << 0.0 << 2.0;
    QTest::newRow("base-terrain-erebus-antimeridian")
        << -77.0 << 167.0 << 2.0e5 << 65.0 << none << 0.0 << 3.0;
    // Half-way to the map the terrain is half flattened.
    QTest::newRow("base-terrain-morph-half")
        << 46.0 << 8.0 << 3.0e5 << 45.0 << mercator << 0.5 << 3.0;
    // On the map the terrain is flat and hillshaded.
    QTest::newRow("base-terrain-alps-map")
        << 46.0 << 8.0 << 4.0e5 << 0.0 << mercator << 1.0 << 1.0;
    QTest::newRow("base-terrain-everest-map")
        << 28.0 << 86.9 << 6.0e5 << 0.0 << mercator << 1.0 << 1.0;
    // Loaded but switched off: the same picture as without terrain (its
    // golden image is base-uk-close's).
    QTest::newRow("base-uk-close-terrain-off")
        << 51.5 << -0.5 << 1.5e5 << 30.0 << none << 0.0 << -1.0;
  }

  void basePackGolden()
  {
    // Blue Marble and the shoreline from the real base pack, set up through
    // the layer tree like the application. (Not the graticule: at close
    // range llvmpipe versions place its 1° parallels a pixel apart; it has
    // its own golden images at whole-globe scale.)
    TS_TEST_REQUIRE_GL(*m_gl);
    const QString directory = qEnvironmentVariable("TS_BASEPACK_DIR");
    if (directory.isEmpty()) {
      QSKIP("TS_BASEPACK_DIR is not set.");
    }
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    QFETCH(double, tiltDeg);
    QFETCH(QString, projection);
    QFETCH(double, morph);
    QFETCH(double, terrain);
    QString error;
    const std::optional<ts::PackManifest> manifest =
        ts::PackManifest::read(directory, &error);
    if (!manifest) {
      QFAIL(qPrintable(error));
    }
    ts::ProviderRegistry registry;
    registry.setBasePack(directory, *manifest);
    ts::LayerTreeModel model;
    for (const QString& id : {QStringLiteral("builtin:bluemarble"),
                              QStringLiteral("builtin:shoreline")}) {
      for (const ts::LayerDefinition& definition :
           ts::builtinLayerDefinitions()) {
        if (definition.id == id) {
          model.addLayer(definition);
        }
      }
    }
    const ts::LayerStack stack = model.layerStack();
    QStringList errors;
    const ts::ImageryDraws imagery =
        ts::imageryDrawsForStack(stack, registry, &errors);
    const std::vector<ts::VectorLayerDraw> vectors =
        ts::vectorDrawsForStack(stack, registry, &errors);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char(' '))));
    QCOMPARE(ts::attributionsForStack(stack, registry).size(), 2);

    m_gl->makeCurrent();
    ts::GlobeStyle style;
    style.graticule = false;
    m_renderer->setStyle(style);
    m_renderer->setImageryLayers(imagery.layers, imagery.groupAlphas);
    m_renderer->setVectorLayers(vectors);
    if (terrain != 0.0) {
      const std::shared_ptr<ts::ElevationProvider> elevation =
          registry.elevation(
              QJsonObject{
                  {QStringLiteral("kind"), QStringLiteral("basepack")},
                  {QStringLiteral("dataset"), QStringLiteral("elevation")}},
              &error);
      QVERIFY2(elevation, qPrintable(error));
      m_renderer->setElevationProvider(elevation);
      m_renderer->setTerrain(terrain > 0.0, std::max(terrain, 1.0));
    }
    ts::Camera camera =
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, 0.0, tiltDeg});
    if (!projection.isEmpty()) {
      camera.setMapProjection(ts::projectionById(projection));
      camera.setMorph(morph);
    }
    const QImage image = renderSettled(camera);
    const ts::VectorStatistics drawn = m_renderer->vectorStatistics();
    qInfo("vectors: %d tiles, %" PRId64 " triangles, %" PRId64
          " segments, %d draw calls",
          drawn.tilesDrawn, drawn.triangles, drawn.segments, drawn.drawCalls);
    m_gl->makeCurrent();
    m_renderer->setVectorLayers({});
    m_renderer->setImageryLayers({}, {});
    m_renderer->setElevationProvider(nullptr);
    m_renderer->setTerrain(false, 1.0);
    m_renderer->setStyle(ts::GlobeStyle());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    QString golden = QString::fromLatin1(QTest::currentDataTag());
    golden.remove(QStringLiteral("-terrain-off"));
    const QString failure =
        ts::test::checkGolden(image, golden, goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void vmap0Golden_data()
  {
    QTest::addColumn<QString>("pack");
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    QTest::addColumn<QString>("projection");
    // Each theme's layers (its pack's, as Add Layer offers them) over Blue
    // Marble, from the clipped VMAP0 tile (30° W–15° W, 15°–30° N: the
    // coast of Western Sahara and Mauritania, the Canaries, Cape Verde).
    const std::array<std::pair<const char*, const char*>, 7> themes = {{
        {"boundaries", "vmap0-boundaries"},
        {"hydrography", "vmap0-hydrography-soamafr"},
        {"transportation", "vmap0-transportation"},
        {"population", "vmap0-population"},
        {"elevation", "vmap0-elevation-soamafr"},
        {"vegetation", "vmap0-vegetation-soamafr"},
        {"other", "vmap0-other"},
    }};
    const QString globe;
    const QString mercator = QStringLiteral("mercator");
    for (const std::pair<const char*, const char*>& theme : themes) {
      const QString name = QString::fromLatin1(theme.first);
      const QString pack = QString::fromLatin1(theme.second);
      QTest::newRow(qPrintable(QStringLiteral("vmap0-%1-globe").arg(name)))
          << pack << 22.5 << -22.5 << 1.2e7 << globe;
      // Close enough (about level 6) for every class but the smallest; the
      // vegetation is further south, in Senegal.
      const bool south = name == QLatin1String("vegetation");
      const double regionLat = south ? 16.0 : 21.5;
      const double regionLon = south ? -16.0 : -17.0;
      QTest::newRow(qPrintable(QStringLiteral("vmap0-%1-region").arg(name)))
          << pack << regionLat << regionLon << 3.0e5 << globe;
      QTest::newRow(qPrintable(QStringLiteral("vmap0-%1-map").arg(name)))
          << pack << 0.0 << 0.0 << 5.0e7 << mercator;
      QTest::newRow(qPrintable(QStringLiteral("vmap0-%1-map-region").arg(name)))
          << pack << regionLat << regionLon << 5.0e4 << mercator;
    }
  }

  void vmap0Golden()
  {
    TS_TEST_REQUIRE_GL(*m_gl);
    const QString basePack = qEnvironmentVariable("TS_BASEPACK_DIR");
    if (basePack.isEmpty()) {
      QSKIP("TS_BASEPACK_DIR is not set.");
    }
    QFETCH(QString, pack);
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    QFETCH(QString, projection);
    QString error;
    const QString directory = vmap0Pack(pack, &error);
    QVERIFY2(!directory.isEmpty(), qPrintable(error));
    const std::optional<ts::PackManifest> base =
        ts::PackManifest::read(basePack, &error);
    const std::optional<ts::PackManifest> manifest =
        ts::PackManifest::read(directory, &error);
    if (!base || !manifest) {
      QFAIL(qPrintable(error));
    }
    ts::ProviderRegistry registry;
    registry.setBasePack(basePack, *base);
    ts::InstalledPack installed;
    installed.directory = directory;
    installed.manifest = *manifest;
    registry.setInstalledPacks({installed});
    ts::LayerTreeModel model;
    for (const ts::LayerDefinition& definition :
         ts::builtinLayerDefinitions()) {
      if (definition.id == QLatin1String("builtin:bluemarble")) {
        model.addLayer(definition);
      }
    }
    // NOLINTNEXTLINE(modernize-loop-convert)
    for (int32_t i = 0; i < manifest->layers.size(); ++i) {
      const std::optional<ts::LayerDefinition> definition =
          ts::LayerDefinition::fromJson(manifest->layers.at(i).toObject(),
                                        &error);
      if (!definition) {
        QFAIL(qPrintable(error));
      }
      model.addLayer(*definition);
    }
    const ts::LayerStack stack = model.layerStack();
    QStringList errors;
    const ts::ImageryDraws imagery =
        ts::imageryDrawsForStack(stack, registry, &errors);
    const std::vector<ts::VectorLayerDraw> vectors =
        ts::vectorDrawsForStack(stack, registry, &errors);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char(' '))));
    QVERIFY(ts::attributionsForStack(stack, registry)
                .contains(QStringLiteral("Vector Map Level 0 (VMAP0), NGA")));
    m_gl->makeCurrent();
    ts::GlobeStyle style;
    style.graticule = false;
    m_renderer->setStyle(style);
    m_renderer->setImageryLayers(imagery.layers, imagery.groupAlphas);
    m_renderer->setVectorLayers(vectors);
    ts::Camera camera =
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, 0.0, 0.0});
    if (!projection.isEmpty()) {
      camera.setMapProjection(ts::projectionById(projection));
      camera.setMorph(1.0);
    }
    const QImage image = renderSettled(camera);
    const ts::VectorStatistics drawn = m_renderer->vectorStatistics();
    m_gl->makeCurrent();
    m_renderer->setVectorLayers({});
    m_renderer->setImageryLayers({}, {});
    m_renderer->setStyle(ts::GlobeStyle());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    QVERIFY(drawn.tilesDrawn > 0);
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void vmap0FrameBudget()
  {
    // Every VMAP0 theme at once (each pack's layers as Add Layer gives
    // them) over Blue Marble: frames within the software budget and the
    // vector meshes within their GPU budget, from the whole Earth down to
    // a region. From the clipped tile; with TS_VMAP0_PACKS_DIR (a
    // directory of built packs, one per subdirectory), from the real
    // packs, over Europe, the densest part of VMAP0.
    TS_TEST_REQUIRE_GL(*m_gl);
    QString error;
    std::vector<ts::InstalledPack> packs;
    const QString real = qEnvironmentVariable("TS_VMAP0_PACKS_DIR");
    if (real.isEmpty()) {
      for (const char* id : {"vmap0-boundaries", "vmap0-hydrography-soamafr",
                             "vmap0-transportation", "vmap0-population",
                             "vmap0-elevation-soamafr",
                             "vmap0-vegetation-soamafr", "vmap0-other"}) {
        ts::InstalledPack pack;
        pack.directory = vmap0Pack(QString::fromLatin1(id), &error);
        QVERIFY2(!pack.directory.isEmpty(), qPrintable(error));
        packs.push_back(pack);
      }
    } else {
      const QDir root(real);
      for (const QString& entry :
           root.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        ts::InstalledPack pack;
        pack.directory = root.filePath(entry);
        packs.push_back(pack);
      }
    }
    ts::LayerTreeModel model;
    std::set<QString> listed;
    for (ts::InstalledPack& pack : packs) {
      const std::optional<ts::PackManifest> manifest =
          ts::PackManifest::read(pack.directory, &error);
      if (!manifest) {
        QFAIL(qPrintable(error));
      }
      pack.manifest = *manifest;
      // NOLINTNEXTLINE(modernize-loop-convert)
      for (int32_t i = 0; i < manifest->layers.size(); ++i) {
        const std::optional<ts::LayerDefinition> definition =
            ts::LayerDefinition::fromJson(manifest->layers.at(i).toObject(),
                                          &error);
        if (!definition) {
          QFAIL(qPrintable(error));
        }
        // Regional packs of one theme offer the same layers.
        if (listed.insert(definition->id).second) {
          model.addLayer(*definition);
        }
      }
    }
    ts::ProviderRegistry registry;
    registry.setInstalledPacks(packs);
    const ts::LayerStack stack = model.layerStack();
    QStringList errors;
    const std::vector<ts::VectorLayerDraw> vectors =
        ts::vectorDrawsForStack(stack, registry, &errors);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char(' '))));
    QVERIFY(vectors.size() >= 20);
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(
        std::make_shared<ts::CheckerboardImageryProvider>(256));
    m_renderer->setVectorLayers(vectors);
    const double lat = real.isEmpty() ? 21.5 : 48.0;
    const double lon = real.isEmpty() ? -17.0 : 8.0;
    struct View {
      const char* name;
      double rangeM;
      double morph;
    };
    for (const View& view :
         {View{"whole Earth", 1.5e7, 0.0}, View{"continent", 3.0e6, 0.0},
          View{"region", 3.0e5, 0.0}, View{"region on the map", 5.0e4, 1.0}}) {
      ts::Camera camera =
          makeCamera(ts::CameraPose{lat, lon, view.rangeM, 0.0, 0.0});
      camera.setMapProjection(ts::projectionById(QStringLiteral("mercator")));
      camera.setMorph(view.morph);
      QElapsedTimer settling;
      settling.start();
      renderSettled(camera);
      const double settleS =
          static_cast<double>(settling.nsecsElapsed()) * 1e-9;
      std::vector<double> timesS;
      for (int32_t frame = 0; frame < 12; ++frame) {
        QElapsedTimer one;
        one.start();
        render(camera, {});
        timesS.push_back(static_cast<double>(one.nsecsElapsed()) * 1e-9);
      }
      std::sort(timesS.begin(), timesS.end());
      const double quartileS = timesS.at(timesS.size() / 4);
      const ts::VectorStatistics drawn = m_renderer->vectorStatistics();
      qInfo("VMAP0 %s: loaded in %.1f s, %d tiles, %" PRId64
            " triangles, %" PRId64 " segments, %d draw calls, %.1f MB on "
            "the GPU, fastest quarter %.1f ms",
            view.name, settleS, drawn.tilesDrawn, drawn.triangles,
            drawn.segments, drawn.drawCalls,
            static_cast<double>(drawn.gpuBytes) / (1024.0 * 1024.0),
            quartileS * 1000.0);
      // The software budget holds for the clipped tile at every view, and
      // for the real packs at the whole Earth and a continent. A region of
      // Europe with every theme on is some 300 000 line segments, more than
      // software GL draws in 100 ms (about 190 ms on llvmpipe, see
      // docs/data/vmap0.md): reported, not failed. The GPU budget holds
      // everywhere.
      if (real.isEmpty() || view.rangeM >= 3.0e6) {
        QVERIFY2(quartileS < 0.1, qPrintable(QString::number(quartileS)));
      }
      QVERIFY(drawn.gpuBytes <= ts::VectorRendererSettings().gpuBytes);
    }
    m_gl->makeCurrent();
    m_renderer->setVectorLayers({});
    m_renderer->setImageryProvider(nullptr);
    QCOMPARE(m_gl->device().takeErrors(), QString());
  }

  void terrainHasNoCracks()
  {
    // Steep exaggerated terrain seen at a grazing angle, against a
    // magenta sky: below the first ground pixel of each column there must
    // be no sky at all, so no gap between tiles of different levels.
    TS_TEST_REQUIRE_GL(*m_gl);
    m_gl->makeCurrent();
    ts::GlobeStyle style;
    style.graticule = false;
    style.backgroundRgb = {1.0F, 0.0F, 1.0F};
    m_renderer->setStyle(style);
    m_renderer->setImageryProvider(
        std::make_shared<ts::CheckerboardImageryProvider>(256));
    m_renderer->setElevationProvider(std::make_shared<SineElevation>());
    m_renderer->setTerrain(true, 5.0);
    const QImage image = renderSettled(
        makeCamera(ts::CameraPose{20.0, 30.0, 4.0e5, 30.0, 70.0}));
    m_gl->makeCurrent();

    m_renderer->setElevationProvider(nullptr);
    m_renderer->setTerrain(false, 1.0);
    m_renderer->setImageryProvider(nullptr);
    m_renderer->setStyle(ts::GlobeStyle());
    QVERIFY(!image.isNull());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    const QString output = qEnvironmentVariable("TS_GOLDEN_OUTPUT");
    if (!output.isEmpty()) {
      image.save(QDir(output).filePath(QStringLiteral("terrain-cracks.png")));
    }
    const std::function<bool(int32_t, int32_t)> sky = [&image](int32_t x,
                                                               int32_t y) {
      const QColor c = image.pixelColor(x, y);
      return c.red() > 240 && c.green() < 15 && c.blue() > 240;
    };
    int32_t ground = 0;
    int32_t cracks = 0;
    for (int32_t x = 0; x < image.width(); ++x) {
      int32_t y = 0;
      while (y < image.height() && sky(x, y)) {
        ++y;
      }
      for (; y < image.height(); ++y) {
        ++ground;
        cracks += sky(x, y) ? 1 : 0;
      }
    }
    QVERIFY(ground > image.width() * image.height() / 3);
    QCOMPARE(cracks, 0);
  }

  void tiltedViewsHoldFrameRate()
  {
    // At the tilt limit over detailed imagery, with and without terrain,
    // on the globe and the map: frames within the software budget, the
    // tiles drawn within a fixed limit, and the GPU budget kept.
    TS_TEST_REQUIRE_GL(*m_gl);
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(
        std::make_shared<ts::CheckerboardImageryProvider>(256));
    m_renderer->setElevationProvider(std::make_shared<SineElevation>());
    for (const bool terrain : {false, true}) {
      m_renderer->setTerrain(terrain, 1.0);
      for (const double morph : {0.0, 1.0}) {
        ts::Camera camera;
        camera.setViewportSize(imageWidthPx, imageHeightPx);
        camera.setMapProjection(ts::projectionById(QStringLiteral("mercator")));
        camera.setMorph(morph);
        camera.setPose(
            ts::CameraPose{20.0, 30.0, 2.0e4, 30.0,
                           morph > 0.0 ? ts::Camera::maximumMapTiltDeg : 80.0});
        renderSettled(camera);
        std::vector<double> timesS;
        for (int32_t frame = 0; frame < 12; ++frame) {
          QElapsedTimer one;
          one.start();
          render(camera, {});
          timesS.push_back(static_cast<double>(one.nsecsElapsed()) * 1e-9);
        }
        std::sort(timesS.begin(), timesS.end());
        const double quartileS = timesS.at(timesS.size() / 4);
        const ts::TileStatistics tiles = m_renderer->tileStatistics();
        qInfo("%s%s: %d tiles drawn, %.1f MB on the GPU, fastest quarter "
              "%.1f ms",
              morph > 0.0 ? "map" : "globe", terrain ? " with terrain" : "",
              tiles.rendered,
              static_cast<double>(tiles.gpuBytes) / (1024.0 * 1024.0),
              quartileS * 1000.0);
        QVERIFY2(quartileS < 0.1, qPrintable(QString::number(quartileS)));
        QVERIFY(tiles.rendered < 400);
        QVERIFY(tiles.gpuBytes <= std::size_t{96} * 1024 * 1024);
      }
    }
    m_gl->makeCurrent();
    m_renderer->setTerrain(false, 1.0);
    m_renderer->setElevationProvider(nullptr);
    m_renderer->setImageryProvider(nullptr);
    QCOMPARE(m_gl->device().takeErrors(), QString());
  }

  void tiltedMapShowsRelief()
  {
    // A tilted map with terrain shows it in relief, without cracks between
    // tiles (no background showing below the sky), and the same map
    // straight down is flat with its hillshade, as before.
    TS_TEST_REQUIRE_GL(*m_gl);
    m_gl->makeCurrent();
    ts::GlobeStyle style;
    style.graticule = false;
    style.backgroundRgb = {1.0F, 0.0F, 1.0F};
    m_renderer->setStyle(style);
    m_renderer->setImageryProvider(
        std::make_shared<ts::CheckerboardImageryProvider>(256));
    m_renderer->setElevationProvider(std::make_shared<SineElevation>());
    ts::Camera camera;
    camera.setViewportSize(imageWidthPx, imageHeightPx);
    camera.setMapProjection(ts::projectionById(QStringLiteral("mercator")));
    camera.setMorph(1.0);
    camera.setPose(ts::CameraPose{20.0, 30.0, 4.0e5, 30.0, 65.0});
    m_renderer->setTerrain(true, 5.0);
    const QImage relief = renderSettled(camera);
    m_renderer->setTerrain(false, 1.0);
    const QImage flat = renderSettled(camera);
    m_gl->makeCurrent();
    m_renderer->setElevationProvider(nullptr);
    m_renderer->setImageryProvider(nullptr);
    m_renderer->setStyle(ts::GlobeStyle());
    QCOMPARE(m_gl->device().takeErrors(), QString());
    const QString output = qEnvironmentVariable("TS_GOLDEN_OUTPUT");
    if (!output.isEmpty()) {
      relief.save(QDir(output).filePath(QStringLiteral("map-relief.png")));
      flat.save(QDir(output).filePath(QStringLiteral("map-relief-off.png")));
    }
    int32_t background = 0;
    int32_t different = 0;
    for (int32_t y = 0; y < relief.height(); ++y) {
      for (int32_t x = 0; x < relief.width(); ++x) {
        const QColor c = relief.pixelColor(x, y);
        background += c.red() > 240 && c.green() < 15 && c.blue() > 240 ? 1 : 0;
        different += relief.pixel(x, y) != flat.pixel(x, y) ? 1 : 0;
      }
    }
    QCOMPARE(background, 0);
    QVERIFY(different > relief.width() * relief.height() / 10);
  }

  void terrainOffDrawsTheEllipsoid()
  {
    // Heights loaded but the terrain off: the same picture as without
    // them (the vertex shader adds nothing).
    TS_TEST_REQUIRE_GL(*m_gl);
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(
        std::make_shared<ts::CheckerboardImageryProvider>(256));
    const ts::Camera camera =
        makeCamera(ts::CameraPose{20.0, 30.0, 2.0e6, 10.0, 40.0});
    const QImage plain = renderSettled(camera);
    m_gl->makeCurrent();
    m_renderer->setElevationProvider(std::make_shared<SineElevation>());
    m_renderer->setTerrain(false, 5.0);
    const QImage off = renderSettled(camera);
    m_gl->makeCurrent();
    m_renderer->setTerrain(true, 5.0);
    const QImage on = renderSettled(camera);
    m_gl->makeCurrent();
    m_renderer->setElevationProvider(nullptr);
    m_renderer->setTerrain(false, 1.0);
    m_renderer->setImageryProvider(nullptr);
    QVERIFY(!plain.isNull() && !off.isNull() && !on.isNull());
    int32_t differing = 0;
    int32_t changed = 0;
    for (int32_t y = 0; y < plain.height(); ++y) {
      for (int32_t x = 0; x < plain.width(); ++x) {
        const QColor a = plain.pixelColor(x, y);
        const QColor b = off.pixelColor(x, y);
        const QColor c = on.pixelColor(x, y);
        differing += std::abs(a.red() - b.red()) +
                                 std::abs(a.green() - b.green()) +
                                 std::abs(a.blue() - b.blue()) >
                             6
                         ? 1
                         : 0;
        changed += a != c ? 1 : 0;
      }
    }
    QCOMPARE(differing, 0);
    // And switched on, it does change the picture.
    QVERIFY(changed > plain.width() * plain.height() / 10);
  }

  void hillshadeShadesTheMap()
  {
    // On the flat map the terrain is flat and hillshaded: slopes facing
    // the north-west sun lighter, the others darker. Without terrain, or
    // with the hillshade off, the map is as before.
    TS_TEST_REQUIRE_GL(*m_gl);
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(
        std::make_shared<ts::SolidColorImageryProvider>(QColor(128, 128, 128),
                                                        64));
    ts::Camera camera = makeCamera(ts::CameraPose{20.0, 30.0, 2.0e6, 0.0, 0.0});
    camera.setMapProjection(ts::projectionById(QStringLiteral("mercator")));
    camera.setMorph(1.0);
    const QImage plain = renderSettled(camera);
    m_gl->makeCurrent();
    m_renderer->setElevationProvider(std::make_shared<SineElevation>());
    m_renderer->setTerrain(true, 1.0);
    m_renderer->setHillshade(0.0);
    const QImage off = renderSettled(camera);
    m_gl->makeCurrent();
    m_renderer->setHillshade(1.0);
    const QImage on = renderSettled(camera);
    m_gl->makeCurrent();
    m_renderer->setTerrain(false, 1.0);
    const QImage hidden = renderSettled(camera);
    m_gl->makeCurrent();
    m_renderer->setElevationProvider(nullptr);
    m_renderer->setImageryProvider(nullptr);
    QVERIFY(!plain.isNull() && !off.isNull() && !on.isNull() &&
            !hidden.isNull());
    int32_t darker = 0;
    int32_t lighter = 0;
    for (int32_t y = 0; y < plain.height(); ++y) {
      for (int32_t x = 0; x < plain.width(); ++x) {
        const int32_t base = qGray(plain.pixel(x, y));
        QVERIFY(std::abs(qGray(off.pixel(x, y)) - base) <= 2);
        QVERIFY(std::abs(qGray(hidden.pixel(x, y)) - base) <= 2);
        const int32_t shaded = qGray(on.pixel(x, y));
        darker += shaded < base - 8 ? 1 : 0;
        lighter += shaded > base + 8 ? 1 : 0;
      }
    }
    const int32_t pixels = plain.width() * plain.height();
    QVERIFY2(darker > pixels / 20 && lighter > pixels / 20,
             qPrintable(QStringLiteral("%1 darker, %2 lighter of %3")
                            .arg(darker)
                            .arg(lighter)
                            .arg(pixels)));
  }

  void incompleteHeightsAreAskedForAgain()
  {
    // A source that couldn't deliver a tile gets asked again, and the
    // terrain then takes the full heights (the mesh is rebuilt).
    TS_TEST_REQUIRE_GL(*m_gl);
    m_gl->makeCurrent();
    ts::TileRendererSettings settings;
    settings.heightsRetryDelay = std::chrono::milliseconds(0);
    ts::GlobeRenderer renderer(m_gl->device(), settings);
    QString error;
    QVERIFY2(renderer.initialise(&error), qPrintable(error));
    std::shared_ptr<FlakyElevation> flaky = std::make_shared<FlakyElevation>();
    renderer.setImageryProvider(
        std::make_shared<ts::CheckerboardImageryProvider>(256));
    renderer.setElevationProvider(flaky);
    renderer.setTerrain(true, 1.0);
    const ts::CameraPose pose{46.0, 8.0, 3.0e5, 0.0, 0.0};
    const ts::Camera camera = makeCamera(pose);
    const std::function<void()> frame = [&]() {
      m_gl->makeCurrent();
      ts::PassDesc pass;
      pass.target = m_target.get();
      pass.viewportPx = QRect(0, 0, imageWidthPx, imageHeightPx);
      renderer.render(camera, pass, 1.0);
    };
    // Settled with the full heights: the finest heights at the target are
    // the source's own tile there (not the flat incomplete stand-in).
    const std::function<bool()> complete = [&]() {
      const std::optional<ts::GroundHeight> ground =
          renderer.groundHeight(46.0, 8.0);
      if (!ground) {
        return false;
      }
      const std::optional<ts::HeightTile> own = SineElevation().loadHeights(
          ts::TileKey::containing(ground->level, ts::Geodetic{46.0, 8.0, 0.0}),
          std::atomic<bool>{false});
      const std::optional<double> expected =
          own ? ts::sampleHeightM(*own, 46.0, 8.0) : std::nullopt;
      return expected && std::abs(ground->ellipsoidalM - *expected) < 0.5;
    };
    const bool settled = QTest::qWaitFor(
        [&]() {
          frame();
          return !renderer.isBusy() && complete();
        },
        60000);
    const std::optional<ts::GroundHeight> ground =
        renderer.groundHeight(46.0, 8.0);
    const QString state =
        QStringLiteral("busy %1, retries %2, ground %3 m at level %4")
            .arg(renderer.isBusy() ? QStringLiteral("yes")
                                   : QStringLiteral("no"))
            .arg(flaky->retries())
            .arg(ground ? ground->ellipsoidalM : -1.0)
            .arg(ground ? ground->level : -1);
    m_gl->makeCurrent();
    renderer.setElevationProvider(nullptr);
    renderer.setImageryProvider(nullptr);
    QVERIFY2(settled, qPrintable(QStringLiteral("the full heights never "
                                                "arrived: ") +
                                 state));
    QVERIFY(flaky->retries() > 0);
  }

  void silhouetteMatchesPicking()
  {
    // Every pixel the CPU picking hits shows the globe and every pixel it
    // misses shows the background, except along the antialiased edge.
    TS_TEST_REQUIRE_GL(*m_gl);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{10.0, 20.0, 1.6e7, 0.0, 0.0});
    const QImage image = render(camera);
    int32_t mismatches = 0;
    for (int32_t y = 0; y < imageHeightPx; ++y) {
      for (int32_t x = 0; x < imageWidthPx; ++x) {
        // A pixel is decided if its centre and all four neighbours'
        // centres agree.
        const bool hit = camera.pickGeodetic(x + 0.5, y + 0.5).has_value();
        bool decided = true;
        for (const QPointF& offset :
             {QPointF(-1, 0), QPointF(1, 0), QPointF(0, -1), QPointF(0, 1)}) {
          decided = decided && camera.pickGeodetic(x + 0.5 + offset.x(),
                                                   y + 0.5 + offset.y())
                                       .has_value() == hit;
        }
        if (decided && isGlobe(image, x, y) != hit) {
          ++mismatches;
        }
      }
    }
    QCOMPARE(mismatches, 0);
  }

  void detailHoldsUnderGpuPressure()
  {
    // With the GPU budget full, moving the camera keeps the detail: the
    // deepest level drawn never drops by two or more from one frame to
    // the next, and the number of tiles never collapses. (Tiles the
    // selection walks through but doesn't draw, and the coarse stand-in
    // images, were evicted first, being least recently drawn; the next
    // frame then fell back to a handful of coarse tiles, the one after
    // evicted the fine ones, and the detail dropped and came back over
    // and over: level 10 to 3 and back, zooming with a WMS layer.)
    TS_TEST_REQUIRE_GL(*m_gl);
    m_gl->makeCurrent();
    ts::TileRendererSettings settings;
    settings.gpuBytes = std::size_t{24} * 1024 * 1024;
    ts::GlobeRenderer renderer(m_gl->device(), settings);
    QString error;
    QVERIFY2(renderer.initialise(&error), qPrintable(error));
    renderer.setImageryLayers(
        {ts::ImageryLayerDraw{
             std::make_shared<ts::CheckerboardImageryProvider>(), 1.0, -1},
         ts::ImageryLayerDraw{std::make_shared<SlowCheckerboard>(), 1.0, -1}},
        {});
    struct Move {
      const char* name = nullptr;
      ts::CameraPose from;
      ts::CameraPose to;
    };
    const std::array<Move, 5> moves = {
        {{"zoom in",
          {51.5, -0.5, 2.0e7, 0.0, 0.0},
          {51.5, -0.5, 3.0e4, 0.0, 0.0}},
         {"hold", {51.5, -0.5, 3.0e4, 0.0, 0.0}, {51.5, -0.5, 3.0e4, 0.0, 0.0}},
         {"pan", {51.5, -0.5, 3.0e4, 0.0, 0.0}, {52.5, -1.5, 3.0e4, 0.0, 0.0}},
         {"tilt",
          {52.5, -1.5, 3.0e4, 0.0, 0.0},
          {52.5, -1.5, 3.0e4, 90.0, 75.0}},
         {"zoom out",
          {52.5, -1.5, 3.0e4, 90.0, 75.0},
          {52.5, -1.5, 2.0e7, 90.0, 0.0}}}};
    for (const Move& move : moves) {
      int32_t previousDeepest = -1;
      int32_t previousDrawn = -1;
      for (int32_t i = 0; i < 45; ++i) {
        const double t = i / 44.0;
        const ts::CameraPose& a = move.from;
        const ts::CameraPose& b = move.to;
        ts::Camera camera;
        // Selection for a full HD window, drawn into the small target.
        camera.setViewportSize(1920, 1080);
        camera.setPose(ts::CameraPose{
            a.targetLatDeg + t * (b.targetLatDeg - a.targetLatDeg),
            a.targetLonDeg + t * (b.targetLonDeg - a.targetLonDeg),
            std::exp(std::log(a.rangeM) +
                     t * (std::log(b.rangeM) - std::log(a.rangeM))),
            a.headingDeg + t * (b.headingDeg - a.headingDeg),
            a.tiltDeg + t * (b.tiltDeg - a.tiltDeg)});
        ts::PassDesc pass;
        pass.target = m_target.get();
        pass.viewportPx = QRect(0, 0, imageWidthPx, imageHeightPx);
        m_gl->makeCurrent();
        renderer.render(camera, pass, 1.0);
        const ts::TileStatistics tiles = renderer.tileStatistics();
        // The first frames of the zoom in are still loading the roots.
        if (previousDeepest >= 0 && previousDrawn > 0) {
          QVERIFY2(tiles.deepestLevel >= previousDeepest - 1,
                   qPrintable(QStringLiteral("%1, frame %2: deepest level %3 "
                                             "after %4")
                                  .arg(QLatin1String(move.name))
                                  .arg(i)
                                  .arg(tiles.deepestLevel)
                                  .arg(previousDeepest)));
          QVERIFY2(previousDrawn <= 20 || tiles.rendered >= previousDrawn / 4,
                   qPrintable(QStringLiteral("%1, frame %2: %3 tiles drawn "
                                             "after %4")
                                  .arg(QLatin1String(move.name))
                                  .arg(i)
                                  .arg(tiles.rendered)
                                  .arg(previousDrawn)));
        }
        previousDeepest = tiles.deepestLevel;
        previousDrawn = tiles.rendered;
        QTest::qWait(16);
      }
    }
    QCOMPARE(m_gl->device().takeErrors(), QString());
  }

  void horizonOneMetreAboveSurface()
  {
    // Looking towards the horizon from 1 m up. The horizon row from the
    // GPU must match CPU picking (double precision) to within a pixel; the
    // ray-cast's constant term is 3e-7 here and would vanish in float.
    TS_TEST_REQUIRE_GL(*m_gl);
    ts::Camera camera;
    camera.setViewportSize(imageWidthPx, imageHeightPx);
    camera.setPose(ts::CameraPose{51.4769, -0.0005, 1.0, 0.0, 85.0});
    QVERIFY(std::abs(camera.eyeGeodetic().heightM - 1.0) < 0.01);
    const QImage image = render(camera);
    const int32_t x = imageWidthPx / 2;
    int32_t cpuRow = -1;
    int32_t gpuRow = -1;
    for (int32_t y = 0; y < imageHeightPx; ++y) {
      if (cpuRow < 0 && camera.pickGeodetic(x + 0.5, y + 0.5)) {
        cpuRow = y;
      }
      if (gpuRow < 0 && isGlobe(image, x, y)) {
        gpuRow = y;
      }
    }
    qInfo("horizon row: CPU %d, GPU %d", cpuRow, gpuRow);
    QVERIFY(cpuRow > 0 && cpuRow < imageHeightPx - 1);
    QVERIFY(std::abs(cpuRow - gpuRow) <= 1);
  }

  void noJitterOneMetreAboveSurface()
  {
    // The camera is 1 m above the ground and moves east in 1 mm steps; a
    // marker 2 m away must land where the double-precision projection puts
    // it, to well under a pixel, in every frame. In plain float, ECEF
    // positions round to 0.5 m and the marker would jump by tens of pixels.
    TS_TEST_REQUIRE_GL(*m_gl);
    const ts::CameraPose start{51.4769, -0.0005, 2.0, 0.0, 60.0};
    ts::Camera camera = makeCamera(start);
    QVERIFY(std::abs(camera.eyeGeodetic().heightM - 1.0) < 0.01);
    ts::GlobeMarker marker;
    marker.positionEcef = camera.targetEcef();
    marker.rgba = {255, 0, 0, 255};
    marker.radiusPx = 6.0;
    const std::vector<ts::GlobeMarker> markers = {marker};

    const ts::LocalFrame frame =
        ts::localFrame(start.targetLatDeg, start.targetLonDeg);
    double worstPx = 0.0;
    for (int32_t step = 0; step <= 40; ++step) {
      // Move the camera, not the marker: shift the target east.
      const ts::Geodetic moved = ts::ecefToGeodetic(
          marker.positionEcef + frame.eastEcef * (0.001 * step));
      ts::CameraPose pose = start;
      pose.targetLatDeg = moved.latDeg;
      pose.targetLonDeg = moved.lonDeg;
      camera = makeCamera(pose);
      const std::optional<ts::ScreenPoint> expected =
          camera.project(marker.positionEcef);
      if (!expected) {
        QFAIL("expected: no value");
      }
      const std::optional<QPointF> drawn = redCentroid(render(camera, markers));
      if (!drawn) {
        QFAIL("drawn: no value");
      }
      const double errorPx =
          std::hypot(drawn->x() - expected->xPx, drawn->y() - expected->yPx);
      worstPx = std::max(worstPx, errorPx);
    }
    qInfo("worst marker position error: %.3f px", worstPx);
    QVERIFY(worstPx < 0.25);
  }

  void highDpiScalesMarkers()
  {
    // The same camera at device pixel ratio 2 covers twice the pixels in
    // each direction; markers keep their logical size.
    TS_TEST_REQUIRE_GL(*m_gl);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{0.0, 0.0, 2.0e7, 0.0, 0.0});
    ts::GlobeMarker marker;
    marker.positionEcef = camera.targetEcef();
    marker.rgba = {255, 0, 0, 255};
    marker.radiusPx = 5.0;
    m_gl->makeCurrent();
    std::unique_ptr<ts::RenderTarget> large =
        m_gl->device().createRenderTarget(2 * imageWidthPx, 2 * imageHeightPx);
    QVERIFY(large);
    m_renderer->setMarkers({marker});
    ts::PassDesc pass;
    pass.target = large.get();
    pass.viewportPx = QRect(0, 0, 2 * imageWidthPx, 2 * imageHeightPx);
    m_renderer->render(camera, pass, 2.0);
    const QImage image = m_gl->device().readPixels(*large);
    int32_t redPixels = 0;
    for (int32_t y = 0; y < image.height(); ++y) {
      for (int32_t x = 0; x < image.width(); ++x) {
        const QColor c = image.pixelColor(x, y);
        redPixels += c.red() > 200 && c.blue() < 100 ? 1 : 0;
      }
    }
    // A disc of radius 2 × 5 − 1 (inside the rim) device pixels.
    const double expected = 3.14159265358979 * 9.0 * 9.0;
    qInfo("red pixels at 2x: %d (disc %.0f)", redPixels, expected);
    QVERIFY(std::abs(redPixels - expected) < 0.15 * expected);
    const std::optional<QPointF> centre = redCentroid(image);
    if (!centre) {
      QFAIL("centre: no value");
    }
    QVERIFY(std::abs(centre->x() - imageWidthPx) < 0.5);
    QVERIFY(std::abs(centre->y() - imageHeightPx) < 0.5);
  }

  void benchmarkTiledFrame()
  {
    // A tilted view over Europe with every checkerboard tile loaded.
    TS_TEST_REQUIRE_GL(*m_gl);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{48.0, 10.0, 2.0e5, 30.0, 70.0});
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(
        std::make_shared<ts::CheckerboardImageryProvider>());
    QVERIFY(QTest::qWaitFor(
        [&]() {
          render(camera);
          return !m_renderer->isBusy();
        },
        30000));
    QVERIFY(m_renderer->tileStatistics().rendered > 20);
    QBENCHMARK
    {
      render(camera);
    }
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(nullptr);
  }

  void benchmarkMapFrame()
  {
    // The flat map with imagery, on whatever renderer is there.
    TS_TEST_REQUIRE_GL(*m_gl);
    ts::Camera camera = makeCamera(ts::CameraPose{20.0, 10.0, 2.0e7, 0.0, 0.0});
    camera.setMorph(1.0);
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(
        std::make_shared<ts::CheckerboardImageryProvider>());
    QVERIFY(!renderSettled(camera).isNull());
    QBENCHMARK
    {
      render(camera);
    }
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(nullptr);
  }

  void benchmarkMorphFrame()
  {
    // Half-way between globe and map.
    TS_TEST_REQUIRE_GL(*m_gl);
    ts::Camera camera = makeCamera(ts::CameraPose{20.0, 10.0, 2.0e7, 0.0, 0.0});
    camera.setMorph(0.5);
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(
        std::make_shared<ts::CheckerboardImageryProvider>());
    QVERIFY(!renderSettled(camera).isNull());
    QBENCHMARK
    {
      render(camera);
    }
    m_gl->makeCurrent();
    m_renderer->setImageryProvider(nullptr);
  }

  void satelliteGolden_data()
  {
    QTest::addColumn<QString>("projection");
    QTest::addColumn<double>("morph");
    QTest::addColumn<double>("latDeg");
    QTest::addColumn<double>("lonDeg");
    QTest::addColumn<double>("rangeM");
    QTest::addColumn<bool>("gpsInView");
    // The ISS and a GPS satellite half an hour after the ISS's epoch, with
    // their past and future tracks: in space on the globe (from far enough
    // out to see GPS's orbit), as ground tracks on the maps.
    QTest::newRow("satellites-globe")
        << QString() << 0.0 << 20.0 << 60.0 << 6.5e7 << true;
    QTest::newRow("satellites-globe-near")
        << QString() << 0.0 << 30.0 << 100.0 << 1.2e7 << false;
    QTest::newRow("satellites-mercator")
        << QStringLiteral("mercator") << 1.0 << 0.0 << 0.0 << 4.5e7 << true;
    QTest::newRow("satellites-miller")
        << QStringLiteral("miller") << 1.0 << 0.0 << 150.0 << 2.4e7 << false;
  }

  void satelliteGolden()
  {
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(QString, projection);
    QFETCH(double, morph);
    QFETCH(double, latDeg);
    QFETCH(double, lonDeg);
    QFETCH(double, rangeM);
    QFETCH(bool, gpsInView);
    std::vector<ts::GlobeMarker> markers;
    std::vector<ts::SpaceLine> lines;
    satelliteScene(markers, lines);
    QCOMPARE(markers.size(), std::size_t{2});
    ts::Camera camera =
        makeCamera(ts::CameraPose{latDeg, lonDeg, rangeM, 0.0, 0.0});
    if (!projection.isEmpty()) {
      const ts::Projection* chosen = projectionFromSpec(projection);
      if (chosen == nullptr) {
        QFAIL("unknown projection");
      }
      camera.setMapProjection(chosen);
      camera.setMorph(morph);
    }
    m_gl->makeCurrent();
    m_renderer->setStyle(ts::GlobeStyle());
    m_renderer->setImageryProvider(nullptr);
    m_renderer->setVectorLayers({});
    m_renderer->setSpaceLines(lines);
    // The map's surface is tiles: wait for them.
    QImage image;
    QVERIFY(QTest::qWaitFor(
        [&]() {
          image = render(camera, markers);
          return !m_renderer->isBusy();
        },
        60000));
    image = render(camera, markers);
    const int32_t segments = m_renderer->spaceSegmentsDrawn();
    m_gl->makeCurrent();
    m_renderer->setSpaceLines({});
    QCOMPARE(m_gl->device().takeErrors(), QString());
    QVERIFY(segments > 200);
    // The tracks' colours are on screen.
    QVERIFY(countColor(image, QColor(255, 204, 0)) > 50);
    QVERIFY(!gpsInView || countColor(image, QColor(60, 220, 255)) > 50);
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void spaceLinesHiddenByGlobe()
  {
    // Seen from over (0°, 0°): a line in orbit over the near side shows, one
    // over the far side is hidden by the Earth, and one high above the
    // Earth's edge (GPS height) shows past it.
    TS_TEST_REQUIRE_GL(*m_gl);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{0.0, 0.0, 3.0e7, 0.0, 0.0});
    const QColor magenta(255, 0, 255);
    const std::function<int32_t(const ts::SpaceLine&)> pixels =
        [&](const ts::SpaceLine& line) {
          m_gl->makeCurrent();
          m_renderer->setSpaceLines({line});
          const QImage image = render(camera);
          m_gl->makeCurrent();
          m_renderer->setSpaceLines({});
          return countColor(image, magenta);
        };
    const int32_t nearSide =
        pixels(lineBetween({-20.0, -10.0}, {20.0, 10.0}, 500e3, 20));
    const int32_t farSide =
        pixels(lineBetween({160.0, -10.0}, {-160.0, 10.0}, 500e3, 20));
    const int32_t pastTheEdge =
        pixels(lineBetween({100.0, -10.0}, {120.0, 10.0}, 20000e3, 20));
    QCOMPARE(m_gl->device().takeErrors(), QString());
    qInfo("near %d, far %d, past the edge %d", nearSide, farSide, pastTheEdge);
    QVERIFY(nearSide > 50);
    QCOMPARE(farSide, 0);
    QVERIFY(pastTheEdge > 20);
  }

  void markersInSpace()
  {
    // Satellite markers: one at the ISS's height in front of the Earth,
    // one high up beside its edge (18° from the view's centre, the Earth
    // filling 10°) and farther than the horizon (the far plane), both
    // drawn; one at GPS height behind the Earth, hidden.
    TS_TEST_REQUIRE_GL(*m_gl);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{0.0, 0.0, 3.0e7, 0.0, 0.0});
    const QColor magenta(255, 0, 255);
    const std::function<int32_t(const ts::Geodetic&)> pixels =
        [&](const ts::Geodetic& at) {
          ts::GlobeMarker marker;
          marker.positionEcef = ts::geodeticToEcef(at);
          marker.rgba = {255, 0, 255, 255};
          marker.radiusPx = 6.0;
          return countColor(render(camera, {marker}), magenta);
        };
    const int32_t front = pixels(ts::Geodetic{10.0, 10.0, 420e3});
    const int32_t beside = pixels(ts::Geodetic{0.0, 123.7, 11650e3});
    const int32_t behind = pixels(ts::Geodetic{0.0, 180.0, 20200e3});
    render(camera);
    QCOMPARE(m_gl->device().takeErrors(), QString());
    qInfo("front %d, beside %d, behind %d", front, beside, behind);
    QVERIFY(front > 40);
    QVERIFY(beside > 40);
    QCOMPARE(behind, 0);
  }

  void spaceLinesSplitOnMap()
  {
    // On a map centred at 0°, a ground track across the antimeridian is
    // drawn to the map's edges on both sides, not as a streak across it.
    TS_TEST_REQUIRE_GL(*m_gl);
    // Far enough out to see the whole width of the world.
    ts::Camera camera = makeCamera(ts::CameraPose{0.0, 0.0, 4.5e7, 0.0, 0.0});
    camera.setMapProjection(ts::projectionById(QStringLiteral("mercator")));
    camera.setMorph(1.0);
    ts::SpaceLine line = lineBetween({170.0, 10.0}, {-170.0, -10.0}, 0.0, 1);
    m_gl->makeCurrent();
    m_renderer->setSpaceLines({line});
    const QImage image =
        render(camera).convertToFormat(QImage::Format_RGBA8888);
    const int32_t segments = m_renderer->spaceSegmentsDrawn();
    // Globe only: not on the map.
    line.globeOnly = true;
    m_renderer->setSpaceLines({line});
    render(camera);
    const int32_t globeOnly = m_renderer->spaceSegmentsDrawn();
    m_gl->makeCurrent();
    m_renderer->setSpaceLines({});
    QCOMPARE(m_gl->device().takeErrors(), QString());
    QCOMPARE(segments, 2);
    QCOMPARE(globeOnly, 0);
    // Magenta at the edges, none in the middle half.
    int32_t middle = 0;
    int32_t edges = 0;
    for (int32_t y = 0; y < image.height(); ++y) {
      for (int32_t x = 0; x < image.width(); ++x) {
        const QColor p = image.pixelColor(x, y);
        if (p.red() > 200 && p.green() < 60 && p.blue() > 200) {
          (x > image.width() / 4 && x < 3 * image.width() / 4 ? middle
                                                              : edges) += 1;
        }
      }
    }
    qInfo("edges %d, middle %d", edges, middle);
    QCOMPARE(middle, 0);
    QVERIFY(edges > 0);
  }

  void aircraftMarkersTurn_data()
  {
    QTest::addColumn<double>("headingDeg");
    QTest::newRow("north") << 0.0;
    QTest::newRow("east") << 90.0;
    QTest::newRow("south") << 180.0;
    QTest::newRow("west") << 270.0;
  }

  void aircraftMarkersTurn()
  {
    // An aircraft at the view's centre, seen from above with north up: its
    // swept wings and tailplane put more of it behind its middle, so most
    // of it lies on the side it heads away from.
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(double, headingDeg);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{45.0, 10.0, 2.0e5, 0.0, 0.0});
    ts::TrackSample at;
    at.latDeg = 45.0;
    at.lonDeg = 10.0;
    at.heightM = 10000.0;
    at.trackDeg = static_cast<float>(headingDeg);
    const ts::GlobeMarker marker =
        ts::aircraftMarker(at, 0.0, {255, 0, 255, 255}, 40.0);
    const std::array<int32_t, 4> q =
        quadrantCounts(render(camera, {marker}), QColor(255, 0, 255));
    QCOMPARE(m_gl->device().takeErrors(), QString());
    const int32_t top = q[0] + q[1];
    const int32_t bottom = q[2] + q[3];
    const int32_t left = q[0] + q[2];
    const int32_t right = q[1] + q[3];
    qInfo("top %d bottom %d left %d right %d", top, bottom, left, right);
    QVERIFY(top + bottom > 400);
    if (headingDeg == 0.0) {
      QVERIFY(bottom > top * 1.3);
    } else if (headingDeg == 90.0) {
      QVERIFY(left > right * 1.3);
    } else if (headingDeg == 180.0) {
      QVERIFY(top > bottom * 1.3);
    } else {
      QVERIFY(right > left * 1.3);
    }
  }

  void trailsCutByTime()
  {
    // A timed line along the equator from 20° W to 20° E, its points' times
    // 0 to 40 s: with the line time at 20 s and a 10 s span, only 10° W to
    // 0° is drawn; moving the time moves it, with nothing uploaded.
    TS_TEST_REQUIRE_GL(*m_gl);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{0.0, 0.0, 9.0e6, 0.0, 0.0});
    ts::SpaceLine line = lineBetween({-20.0, 0.0}, {20.0, 0.0}, 10000.0, 40);
    for (std::size_t i = 0; i < line.points.size(); ++i) {
      line.points[i].timeS = 1.0e9 + static_cast<double>(i);
    }
    line.timed = true;
    line.shownS = 10.0;
    const QColor magenta(255, 0, 255);
    m_gl->makeCurrent();
    m_renderer->setSpaceLineTime(1.0e9 + 20.0);
    m_renderer->setSpaceLines({line});
    const QImage first = render(camera);
    const int64_t uploads = m_renderer->spaceLineUploads();
    m_renderer->setSpaceLineTime(1.0e9 + 35.0);
    const QImage later = render(camera);
    const int64_t uploadsLater = m_renderer->spaceLineUploads();
    m_renderer->setSpaceLineTime(1.0e9 - 5.0);
    const QImage before = render(camera);
    // All of it.
    line.shownS = std::numeric_limits<double>::infinity();
    m_renderer->setSpaceLines({line});
    m_renderer->setSpaceLineTime(1.0e9 + 40.0);
    const QImage all = render(camera);
    m_gl->makeCurrent();
    m_renderer->setSpaceLines({});
    QCOMPARE(m_gl->device().takeErrors(), QString());
    QCOMPARE(uploadsLater, uploads);
    // Columns with magenta: where the line is drawn.
    const std::function<std::pair<int32_t, int32_t>(const QImage&)> extent =
        [&magenta](const QImage& image) {
          const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
          int32_t lo = rgba.width();
          int32_t hi = -1;
          for (int32_t y = 0; y < rgba.height(); ++y) {
            for (int32_t x = 0; x < rgba.width(); ++x) {
              const QColor p = rgba.pixelColor(x, y);
              if (std::abs(p.red() - magenta.red()) < 40 && p.green() < 60 &&
                  std::abs(p.blue() - magenta.blue()) < 40) {
                lo = std::min(lo, x);
                hi = std::max(hi, x);
              }
            }
          }
          return std::make_pair(lo, hi);
        };
    const std::pair<int32_t, int32_t> a = extent(first);
    const std::pair<int32_t, int32_t> b = extent(later);
    const std::pair<int32_t, int32_t> c = extent(before);
    const std::pair<int32_t, int32_t> d = extent(all);
    qInfo("20 s: %d-%d, 35 s: %d-%d, all: %d-%d", a.first, a.second, b.first,
          b.second, d.first, d.second);
    const std::optional<ts::ScreenPoint> west =
        camera.project(ts::geodeticToEcef(ts::Geodetic{0.0, -10.0, 10000.0}));
    const std::optional<ts::ScreenPoint> centre =
        camera.project(ts::geodeticToEcef(ts::Geodetic{0.0, 0.0, 10000.0}));
    const std::optional<ts::ScreenPoint> east =
        camera.project(ts::geodeticToEcef(ts::Geodetic{0.0, 5.0, 10000.0}));
    const std::optional<ts::ScreenPoint> east15 =
        camera.project(ts::geodeticToEcef(ts::Geodetic{0.0, 15.0, 10000.0}));
    QVERIFY(west && centre && east && east15);
    // From 10° W to 0° (with the line's half width and caps at the ends).
    QVERIFY(std::abs(a.first - static_cast<int32_t>(
                                   west.value_or(ts::ScreenPoint()).xPx)) <= 3);
    QVERIFY(std::abs(a.second - static_cast<int32_t>(
                                    centre.value_or(ts::ScreenPoint()).xPx)) <=
            3);
    // From 5° E to 15° E.
    QVERIFY(std::abs(b.first - static_cast<int32_t>(
                                   east.value_or(ts::ScreenPoint()).xPx)) <= 3);
    QVERIFY(std::abs(b.second - static_cast<int32_t>(
                                    east15.value_or(ts::ScreenPoint()).xPx)) <=
            3);
    // Before the line starts: nothing.
    QCOMPARE(c.second, -1);
    // Four times as long (40 s against 10 s).
    QVERIFY(d.second - d.first > 3 * (a.second - a.first));
  }

  void aircraftGolden_data()
  {
    QTest::addColumn<QString>("mode");
    QTest::addColumn<bool>("map");
    // The fixture flights 200 s in, over England and France, tilted to see
    // their heights; the trails coloured by altitude.
    QTest::newRow("aircraft-globe-off") << QStringLiteral("off") << false;
    QTest::newRow("aircraft-globe-span") << QStringLiteral("span") << false;
    QTest::newRow("aircraft-globe-all") << QStringLiteral("all") << false;
    QTest::newRow("aircraft-mercator") << QStringLiteral("all") << true;
  }

  void aircraftGolden()
  {
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(QString, mode);
    QFETCH(bool, map);
    ts::TrailSettings trail;
    trail.mode = ts::TrailMode::All;
    if (mode == QLatin1String("off")) {
      trail.mode = ts::TrailMode::Off;
    } else if (mode == QLatin1String("span")) {
      trail.mode = ts::TrailMode::Span;
    }
    trail.spanS = 150.0;
    std::vector<ts::GlobeMarker> markers;
    std::vector<ts::SpaceLine> lines;
    const double timeS = aircraftFixtureStartS + 200.0;
    aircraftScene(timeS, trail, markers, lines);
    QCOMPARE(markers.size(), std::size_t{3});
    ts::Camera camera =
        map ? makeCamera(ts::CameraPose{49.7, 0.9, 5.0e5, 0.0, 0.0})
            : makeCamera(ts::CameraPose{49.4, 0.9, 6.0e5, 0.0, 45.0});
    if (map) {
      camera.setMapProjection(ts::projectionById(QStringLiteral("mercator")));
      camera.setMorph(1.0);
    }
    m_gl->makeCurrent();
    // Without the graticule: its lines at a grazing angle differ by a few
    // levels between Mesa builds (Windows), and these images are about the
    // aircraft.
    ts::GlobeStyle style;
    style.graticule = false;
    m_renderer->setStyle(style);
    m_renderer->setImageryProvider(nullptr);
    m_renderer->setVectorLayers({});
    m_renderer->setSpaceLineTime(timeS);
    m_renderer->setSpaceLines(lines);
    QImage image;
    QVERIFY(QTest::qWaitFor(
        [&]() {
          image = render(camera, markers);
          return !m_renderer->isBusy();
        },
        60000));
    image = render(camera, markers);
    m_gl->makeCurrent();
    m_renderer->setSpaceLines({});
    QCOMPARE(m_gl->device().takeErrors(), QString());
    // The markers' yellow is on screen.
    QVERIFY2(
        countColor(image, QColor(255, 230, 0)) > 15,
        qPrintable(QString::number(countColor(image, QColor(255, 230, 0)))));
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void aircraftHoldFrameRate()
  {
    // A busy live view: 1000 aircraft, each with an hour of trail (all
    // the data, 1800 reports each, simplified), playing: the clock moves
    // every frame, which moves the markers and cuts the trails in the
    // shader, uploading no trail. Frames on the globe (tilted) and the map
    // within the software budget.
    TS_TEST_REQUIRE_GL(*m_gl);
    std::vector<ts::SpaceLine> trails;
    std::vector<ts::MovingTrack> tracks(1000);
    ts::TrackTiming timing;
    ts::TrailSettings all;
    all.mode = ts::TrailMode::All;
    ts::TrailLook look;
    look.colourOf = [](const ts::TrackSample& p) {
      return ts::altitudeColour(p.heightM);
    };
    for (std::size_t a = 0; a < tracks.size(); ++a) {
      for (int32_t i = 0; i < 1800; ++i) {
        ts::TrackSample p;
        p.timeS = 1.8e9 + 2.0 * i;
        const double phase = 0.0005 * i + static_cast<double>(a);
        p.latDeg = 35.0 + 15.0 * std::sin(0.37 * static_cast<double>(a)) +
                   2.0 * std::sin(phase);
        p.lonDeg = -10.0 + 30.0 * std::cos(0.71 * static_cast<double>(a)) +
                   3.0 * std::cos(0.8 * phase);
        p.heightM = 3000.0 +
                    8000.0 * std::fabs(std::sin(0.13 * static_cast<double>(a)));
        p.trackDeg = static_cast<float>(std::fmod(40.0 * phase, 360.0));
        tracks[a].add(p);
      }
      tracks[a].finish();
      for (ts::SpaceLine& line : ts::trailLines(tracks[a], timing, all, look)) {
        trails.push_back(std::move(line));
      }
    }
    m_gl->makeCurrent();
    m_renderer->setSpaceLines(trails, 1);
    for (const double morph : {0.0, 1.0}) {
      ts::Camera camera =
          morph > 0.0 ? makeCamera(ts::CameraPose{40.0, 0.0, 6.0e6, 0.0, 0.0})
                      : makeCamera(ts::CameraPose{40.0, 0.0, 4.0e6, 0.0, 40.0});
      camera.setMapProjection(ts::projectionById(QStringLiteral("mercator")));
      camera.setMorph(morph);
      m_renderer->setSpaceLineTime(1.8e9 + 1800.0);
      render(camera);
      const int64_t uploads = m_renderer->spaceLineUploads();
      std::vector<double> timesS;
      for (int32_t frame = 0; frame < 20; ++frame) {
        const double timeS = 1.8e9 + 1800.0 + 10.0 * frame;
        std::vector<ts::GlobeMarker> markers;
        markers.reserve(tracks.size());
        for (const ts::MovingTrack& track : tracks) {
          const std::optional<ts::TrackSample> at = track.at(timeS, timing);
          if (at) {
            markers.push_back(
                ts::aircraftMarker(*at, 0.0, {255, 230, 0, 255}, 8.0));
          }
        }
        QElapsedTimer one;
        one.start();
        m_renderer->setSpaceLineTime(timeS);
        render(camera, markers);
        timesS.push_back(static_cast<double>(one.nsecsElapsed()) * 1e-9);
      }
      // The trails were placed once (the view didn't move).
      QCOMPARE(m_renderer->spaceLineUploads(), uploads);
      std::sort(timesS.begin(), timesS.end());
      const double quartileS = timesS.at(timesS.size() / 4);
      qInfo("aircraft %s (%d segments): fastest quarter within %.1f ms, "
            "slowest %.1f ms",
            morph > 0.0 ? "map" : "globe", m_renderer->spaceSegmentsDrawn(),
            quartileS * 1000.0, timesS.back() * 1000.0);
      QVERIFY2(quartileS < 0.1, qPrintable(QString::number(quartileS)));
    }
    m_gl->makeCurrent();
    m_renderer->setSpaceLines({}, 1);
    m_renderer->setMarkers({});
    QCOMPARE(m_gl->device().takeErrors(), QString());
  }

  void hullsToScale()
  {
    // A 200 m by 32 m ship heading north, its antenna 50 m behind the
    // middle: close up its hull is drawn to scale (as long on the screen as
    // the camera says 200 m is, ahead of the antenna by 50 m); far off,
    // the symbol at its size in pixels.
    TS_TEST_REQUIRE_GL(*m_gl);
    ts::TrackSample at;
    at.latDeg = 50.85;
    at.lonDeg = -1.30;
    at.trackDeg = 0.0F;
    at.speedMS = 5.0F;
    ts::VesselMotion motion;
    motion.headingDeg = 0.0F;
    ts::VesselInfo info;
    info.dimensions = ts::AisDimensions{150, 50, 16, 16};
    const ts::GlobeMarker marker =
        ts::vesselMarker(at, motion, info, 0.0, {255, 0, 255, 255}, 8.0);
    const QColor magenta(255, 0, 255);
    const ts::Camera close =
        makeCamera(ts::CameraPose{50.85, -1.30, 1500.0, 0.0, 0.0});
    const QRect hull = boxOfColor(render(close, {marker}), magenta);
    const std::optional<ts::ScreenPoint> bow = close.project(
        ts::geodeticToEcef(ts::Geodetic{50.85 + 150.0 / 111195.0, -1.30, 0.0}));
    const std::optional<ts::ScreenPoint> stern = close.project(
        ts::geodeticToEcef(ts::Geodetic{50.85 - 50.0 / 111195.0, -1.30, 0.0}));
    QVERIFY(bow && stern);
    const double bowY = bow.value_or(ts::ScreenPoint()).yPx;
    const double sternY = stern.value_or(ts::ScreenPoint()).yPx;
    qInfo("hull rows %d to %d; bow %.1f, stern %.1f", hull.top(), hull.bottom(),
          bowY, sternY);
    QVERIFY(std::fabs(hull.top() - bowY) <= 3.0);
    QVERIFY(std::fabs(hull.bottom() - sternY) <= 3.0);
    // Its beam: 32 m, much narrower than long.
    QVERIFY(hull.width() * 4 < hull.height());
    // Far off: the symbol, 16 px long.
    // (Not "far": a macro in Windows' headers.)
    const ts::Camera distant =
        makeCamera(ts::CameraPose{50.85, -1.30, 2.0e5, 0.0, 0.0});
    const QRect symbol = boxOfColor(render(distant, {marker}), magenta);
    QCOMPARE(m_gl->device().takeErrors(), QString());
    qInfo("symbol %dx%d", symbol.width(), symbol.height());
    QVERIFY(symbol.height() >= 13 && symbol.height() <= 18);
    QVERIFY(symbol.width() < symbol.height());
  }

  void harbourGolden_data()
  {
    QTest::addColumn<QString>("mode");
    QTest::addColumn<bool>("map");
    QTest::addColumn<double>("rangeM");
    // The made-up harbour five minutes in: stations by type colour,
    // course vectors, trails; close in, the hulls.
    QTest::newRow("harbour-globe-off")
        << QStringLiteral("off") << false << 9000.0;
    QTest::newRow("harbour-globe-span")
        << QStringLiteral("span") << false << 9000.0;
    QTest::newRow("harbour-globe-all")
        << QStringLiteral("all") << false << 9000.0;
    QTest::newRow("harbour-mercator")
        << QStringLiteral("all") << true << 9000.0;
    QTest::newRow("harbour-hulls") << QStringLiteral("span") << false << 2500.0;
  }

  void harbourGolden()
  {
    TS_TEST_REQUIRE_GL(*m_gl);
    QFETCH(QString, mode);
    QFETCH(bool, map);
    QFETCH(double, rangeM);
    ts::TrailSettings trail;
    trail.mode = ts::TrailMode::All;
    if (mode == QLatin1String("off")) {
      trail.mode = ts::TrailMode::Off;
    } else if (mode == QLatin1String("span")) {
      trail.mode = ts::TrailMode::Span;
    }
    trail.spanS = 120.0;
    std::vector<ts::GlobeMarker> markers;
    std::vector<ts::SpaceLine> lines;
    const double timeS = 1795176000.0 + 300.0;
    harbourScene(timeS, trail, markers, lines);
    QCOMPARE(markers.size(), std::size_t{8});
    // The close view looks at the cargo ship.
    const double lat = rangeM < 5000.0 ? 50.845 : 50.85;
    const double lon = rangeM < 5000.0 ? -1.318 : -1.30;
    ts::Camera camera = makeCamera(ts::CameraPose{lat, lon, rangeM, 0.0, 0.0});
    if (map) {
      camera.setMapProjection(ts::projectionById(QStringLiteral("mercator")));
      camera.setMorph(1.0);
    }
    m_gl->makeCurrent();
    ts::GlobeStyle style;
    style.graticule = false;
    m_renderer->setStyle(style);
    m_renderer->setImageryProvider(nullptr);
    m_renderer->setVectorLayers({});
    m_renderer->setSpaceLineTime(timeS);
    m_renderer->setSpaceLines(lines);
    QImage image;
    QVERIFY(QTest::qWaitFor(
        [&]() {
          image = render(camera, markers);
          return !m_renderer->isBusy();
        },
        60000));
    image = render(camera, markers);
    m_gl->makeCurrent();
    m_renderer->setSpaceLines({});
    QCOMPARE(m_gl->device().takeErrors(), QString());
    // The cargo ship's green is on screen.
    const std::array<std::uint8_t, 4> cargo =
        ts::shipTypeColour(ts::ShipTypeGroup::Cargo);
    QVERIFY(countColor(image, QColor(cargo[0], cargo[1], cargo[2])) > 15);
    if (!m_softwareRenderer &&
        qEnvironmentVariableIntValue("TS_UPDATE_GOLDEN") == 0) {
      QSKIP("Golden images are compared on Mesa llvmpipe only.");
    }
    const QString failure = ts::test::checkGolden(
        image, QString::fromLatin1(QTest::currentDataTag()), goldenTolerance);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
  }

  void shipsHoldFrameRate()
  {
    // A live national feed: 10 000 stations (ships with their hulls'
    // sizes), 1500 of them with a trail of their last hour, the clock
    // moving every frame; on the globe and the map within the software
    // budget, no trail uploaded as the clock moves. (Software GL is bound
    // by the markers' pixels: 20 000 take about 65 ms there.)
    TS_TEST_REQUIRE_GL(*m_gl);
    std::vector<ts::GlobeMarker> markers;
    std::vector<ts::SpaceLine> trails;
    ts::TrackTiming timing;
    timing.gapS = 600.0;
    ts::TrailSettings span;
    span.mode = ts::TrailMode::Span;
    span.spanS = 3600.0;
    for (int32_t v = 0; v < 10000; ++v) {
      ts::TrackSample at;
      at.latDeg = 54.0 + 12.0 * std::sin(0.37 * v);
      at.lonDeg = 5.0 + 25.0 * std::cos(0.71 * v);
      at.trackDeg = static_cast<float>(std::fmod(17.0 * v, 360.0));
      at.speedMS = 5.0F;
      ts::VesselMotion motion;
      motion.headingDeg = at.trackDeg;
      ts::VesselInfo info;
      info.shipType = 70 + v % 20;
      info.dimensions = ts::AisDimensions{80, 20, 8, 8};
      markers.push_back(ts::vesselMarker(
          at, motion, info, 0.0,
          ts::shipTypeColour(ts::shipTypeGroup(info.shipType)), 6.0));
      if (v % 7 != 0 || trails.size() >= 1500) {
        continue;
      }
      ts::MovingTrack track;
      for (int32_t m = 0; m <= 60; ++m) {
        ts::TrackSample p = at;
        p.timeS = 1.8e9 + 60.0 * m;
        p.lonDeg = at.lonDeg + 0.002 * m;
        p.latDeg = at.latDeg + 0.001 * std::sin(0.2 * m);
        track.add(p);
      }
      track.finish();
      for (ts::SpaceLine& line :
           ts::trailLines(track, timing, span, ts::TrailLook())) {
        trails.push_back(std::move(line));
      }
    }
    m_gl->makeCurrent();
    m_renderer->setSpaceLines(trails, 3);
    for (const double morph : {0.0, 1.0}) {
      ts::Camera camera =
          makeCamera(ts::CameraPose{55.0, 10.0, 6.0e6, 0.0, 0.0});
      camera.setMapProjection(ts::projectionById(QStringLiteral("mercator")));
      camera.setMorph(morph);
      m_renderer->setSpaceLineTime(1.8e9 + 3600.0);
      render(camera, markers);
      const int64_t uploads = m_renderer->spaceLineUploads();
      std::vector<double> timesS;
      for (int32_t frame = 0; frame < 15; ++frame) {
        QElapsedTimer one;
        one.start();
        m_renderer->setSpaceLineTime(1.8e9 + 3000.0 + 10.0 * frame);
        // The markers move every frame, as the clock runs.
        m_renderer->setMarkers(markers);
        render(camera, markers);
        timesS.push_back(static_cast<double>(one.nsecsElapsed()) * 1e-9);
      }
      QCOMPARE(m_renderer->spaceLineUploads(), uploads);
      std::sort(timesS.begin(), timesS.end());
      const double quartileS = timesS.at(timesS.size() / 4);
      qInfo("ships %s: fastest quarter within %.1f ms, slowest %.1f ms",
            morph > 0.0 ? "map" : "globe", quartileS * 1000.0,
            timesS.back() * 1000.0);
      QVERIFY2(quartileS < 0.1, qPrintable(QString::number(quartileS)));
    }
    m_gl->makeCurrent();
    m_renderer->setSpaceLines({}, 3);
    m_renderer->setMarkers({});
    QCOMPARE(m_gl->device().takeErrors(), QString());
  }

  void satellitesHoldFrameRate()
  {
    // A whole constellation: 6000 satellites (72 planes at 53°, 550 km),
    // ten of them with their tracks. Their positions in well under a
    // second off the GUI thread, and frames on the globe and the map
    // within the software budget (10 frames a second).
    TS_TEST_REQUIRE_GL(*m_gl);
    ts::OrbitalElements base = orbitFixture("stations.tle");
    base.name.clear();
    base.inclinationDeg = 53.0;
    base.eccentricity = 0.0001;
    base.meanMotionRevPerDay = 15.06;
    base.bstar = 0.0001;
    ts::OrbitRequest request;
    request.time = base.epoch;
    constexpr int32_t planes = 72;
    constexpr int32_t perPlane = 84;
    for (int32_t p = 0; p < planes; ++p) {
      for (int32_t k = 0; k < perPlane; ++k) {
        ts::OrbitalElements e = base;
        e.catalogueNumber = 100000 + p * perPlane + k;
        e.raanDeg = 360.0 * p / planes;
        e.meanAnomalyDeg = std::fmod(360.0 * k / perPlane + 5.0 * p, 360.0);
        const bool track = p * perPlane + k < 10;
        const std::shared_ptr<const ts::Sgp4> sgp4 =
            std::make_shared<const ts::Sgp4>(e);
        request.tasks.push_back(ts::OrbitTask{
            sgp4, track, sgp4->periodMinutes(), sgp4->periodMinutes()});
      }
    }
    QCOMPARE(request.tasks.size(), std::size_t{6048});
    std::map<const ts::Sgp4*, std::unique_ptr<ts::TrackSampler>> samplers;
    ts::OrbitResult result = ts::OrbitComputer::compute(request, samplers);
    // A tick later (the clock moving): the tracks' points are kept.
    request.time.fraction += 1.0 / 86400.0;
    result = ts::OrbitComputer::compute(request, samplers);
    qInfo("6048 positions and 10 tracks: %.1f ms", result.computeMs);
    QVERIFY(result.computeMs < 1000.0);
    std::vector<ts::GlobeMarker> markers;
    std::vector<ts::SpaceLine> lines;
    for (std::size_t i = 0; i < result.states.size(); ++i) {
      QVERIFY(result.states[i].ok());
      markers.push_back(
          ts::satelliteMarker(result.states[i], {255, 204, 0, 255}, 3.5));
      if (request.tasks[i].track) {
        ts::TrackLook look;
        lines.push_back(ts::pastTrackLine(result.tracks[i], look));
        lines.push_back(ts::futureTrackLine(result.tracks[i], look));
      }
    }
    m_gl->makeCurrent();
    m_renderer->setSpaceLines(lines);
    for (const double morph : {0.0, 1.0}) {
      ts::Camera camera =
          makeCamera(ts::CameraPose{20.0, 10.0, 2.0e7, 0.0, 0.0});
      camera.setMapProjection(ts::projectionById(QStringLiteral("mercator")));
      camera.setMorph(morph);
      render(camera, markers);
      std::vector<double> timesS;
      for (int32_t frame = 0; frame < 20; ++frame) {
        // The markers move every frame, as the clock runs.
        m_renderer->setMarkers(markers);
        m_renderer->setSpaceLines(lines);
        QElapsedTimer one;
        one.start();
        render(camera, markers);
        timesS.push_back(static_cast<double>(one.nsecsElapsed()) * 1e-9);
      }
      std::sort(timesS.begin(), timesS.end());
      const double quartileS = timesS.at(timesS.size() / 4);
      qInfo("%s: fastest quarter within %.1f ms, slowest %.1f ms",
            morph > 0.0 ? "map" : "globe", quartileS * 1000.0,
            timesS.back() * 1000.0);
      QVERIFY2(quartileS < 0.1, qPrintable(QString::number(quartileS)));
    }
    m_gl->makeCurrent();
    m_renderer->setSpaceLines({});
    m_renderer->setMarkers({});
    QCOMPARE(m_gl->device().takeErrors(), QString());
  }

  void benchmarkFrame()
  {
    TS_TEST_REQUIRE_GL(*m_gl);
    const ts::Camera camera =
        makeCamera(ts::CameraPose{20.0, 0.0, 2.2e7, 0.0, 0.0});
    QBENCHMARK
    {
      render(camera);
    }
  }
};

QTEST_MAIN(TestGlobe)
#include "tst_globe.moc"
