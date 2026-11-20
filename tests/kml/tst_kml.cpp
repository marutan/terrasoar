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
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>

#include <QFile>
#include <QImage>
#include <QJsonObject>
#include <QTimeZone>
#include <QUrlQuery>
#include <QtTest>

#include "concurrency.h"
#include "ts/kml/kmlactivity.h"
#include "ts/kml/kmldocument.h"
#include "ts/kml/kmlfeatureprovider.h"
#include "ts/kml/kmloverlays.h"
#include "ts/kml/kmlpresentation.h"
#include "ts/kml/kmlreader.h"
#include "ts/providers/providerregistry.h"
#include "ts/time/timeextent.h"

namespace
{

QString fixture(const QString& name)
{
  return QStringLiteral(TS_TEST_DATA_DIR "/kml/") + name;
}

/** The names of the placemarks @p provider shows, in order, each once. */
std::vector<QString> names(const ts::KmlFeatureProvider& provider)
{
  std::vector<QString> found;
  const std::vector<ts::Feature> all =
      provider.featuresAt(ts::GeoPoint{0.0, 0.0}, 1000.0);
  for (const ts::Feature& feature : all) {
    const QString name =
        feature.attributes.value(QStringLiteral("name")).toString();
    if (std::find(found.begin(), found.end(), name) == found.end()) {
      found.push_back(name);
    }
  }
  return found;
}

ts::KmlDocument read(const QString& name)
{
  QString error;
  const std::optional<ts::KmlDocument> document =
      ts::readKmlFile(fixture(name), &error);
  if (!document) {
    qWarning("%s: %s", qPrintable(name), qPrintable(error));
    return ts::KmlDocument{};
  }
  return *document;
}

/** The first feature named @p name, depth first; null if none. */
const ts::KmlFeature* find(const ts::KmlFeature& feature, const QString& name)
{
  if (feature.name == name) {
    return &feature;
  }
  for (const ts::KmlFeature& child : feature.children) {
    const ts::KmlFeature* found = find(child, name);
    if (found != nullptr) {
      return found;
    }
  }
  return nullptr;
}

int32_t countPlacemarks(const ts::KmlFeature& feature)
{
  int32_t count = feature.kind == ts::KmlFeature::Kind::Placemark ? 1 : 0;
  for (const ts::KmlFeature& child : feature.children) {
    count += countPlacemarks(child);
  }
  return count;
}

QDateTime utc(const char* text)
{
  return ts::parseIsoTime(QLatin1String(text), QDateTime())
      .value_or(QDateTime());
}

} // namespace

/**
 * Unit tests for ts/kml: reading every element, styles and their
 * resolution (shared, StyleMap, inline, external), KMZ archives, files
 * written by other programs (GDAL's test files), and errors.
 */
class TestKml : public QObject
{
  Q_OBJECT

private slots:
  void structure()
  {
    const ts::KmlDocument document = read(QStringLiteral("everything.kml"));
    const ts::KmlFeature& root = document.root;
    QCOMPARE(root.kind, ts::KmlFeature::Kind::Document);
    QCOMPARE(root.name, QStringLiteral("Everything"));
    QVERIFY(root.open);
    QCOMPARE(root.styles.size(), std::size_t{3});
    QCOMPARE(root.styleMaps.size(), std::size_t{1});
    // Two folders, two ground overlays, a screen overlay, a network link
    // and a placemark; the PhotoOverlay is kept as XML.
    QCOMPARE(root.children.size(), std::size_t{7});
    QCOMPARE(root.children.at(0).kind, ts::KmlFeature::Kind::Folder);
    QCOMPARE(root.children.at(0).name, QStringLiteral("Shapes"));
    QCOMPARE(root.children.at(0).children.size(), std::size_t{5});
    QCOMPARE(countPlacemarks(root), 9);
    // The view.
    if (!root.view) {
      QFAIL("no view");
    }
    QCOMPARE(root.view->kind, ts::KmlView::Kind::LookAt);
    QCOMPARE(root.view->rangeM, 250000.0);
    QCOMPARE(root.view->altitudeMode, ts::KmlAltitudeMode::RelativeToGround);
    // Unknown: atom:author and PhotoOverlay, kept and listed.
    QCOMPARE(root.unknown.size(), std::size_t{2});
    QVERIFY(root.unknown.at(0).second.contains(QLatin1String("author")));
    QCOMPARE(root.unknown.at(0).first, QStringLiteral("open"));
    QVERIFY(root.unknown.at(1).second.contains(QLatin1String("PhotoOverlay")));
    QCOMPARE(root.unknown.at(1).first, QStringLiteral("NetworkLink"));
    QCOMPARE(document.skipped.size(), 2);
    QVERIFY(document.skipped.at(0).contains(QLatin1String("line")));
  }

  void geometries()
  {
    const ts::KmlDocument document = read(QStringLiteral("everything.kml"));
    const ts::KmlFeature* point =
        find(document.root, QStringLiteral("A point"));
    if (point == nullptr || !point->geometry) {
      QFAIL("no point");
    }
    QCOMPARE(point->geometry->kind, ts::KmlGeometry::Kind::Point);
    QCOMPARE(point->geometry->coordinates.size(), std::size_t{1});
    QCOMPARE(point->geometry->coordinates.front().altM, 140.0);
    QCOMPARE(point->geometry->altitudeMode, ts::KmlAltitudeMode::Absolute);
    QVERIFY(point->geometry->extrude);
    QCOMPARE(point->extendedData.size(), std::size_t{3});
    QCOMPARE(point->extendedData.at(0).first, QStringLiteral("Population"));
    QCOMPARE(point->extendedData.at(2).second, QStringLiteral("BHM"));
    QVERIFY(point->description.contains(QLatin1String("<i>point</i>")));

    // Spaces after commas are tolerated.
    const ts::KmlFeature* line = find(document.root, QStringLiteral("A line"));
    if (line == nullptr || !line->geometry) {
      QFAIL("no line");
    }
    QCOMPARE(line->geometry->coordinates.size(), std::size_t{3});
    QCOMPARE(line->geometry->coordinates.at(0).latDeg, 53.4);
    QVERIFY(line->geometry->tessellate);

    const ts::KmlFeature* polygon =
        find(document.root, QStringLiteral("A polygon with a hole"));
    if (polygon == nullptr || !polygon->geometry) {
      QFAIL("no polygon");
    }
    QCOMPARE(polygon->geometry->kind, ts::KmlGeometry::Kind::Polygon);
    QCOMPARE(polygon->geometry->coordinates.size(), std::size_t{5});
    QCOMPARE(polygon->geometry->inner.size(), std::size_t{1});
    QCOMPARE(polygon->geometry->altitudeMode,
             ts::KmlAltitudeMode::RelativeToGround);

    const ts::KmlFeature* multi =
        find(document.root, QStringLiteral("Two points and a line"));
    if (multi == nullptr || !multi->geometry) {
      QFAIL("no multigeometry");
    }
    QCOMPARE(multi->geometry->kind, ts::KmlGeometry::Kind::MultiGeometry);
    QCOMPARE(multi->geometry->children.size(), std::size_t{3});
    QCOMPARE(multi->geometry->children.at(2).kind,
             ts::KmlGeometry::Kind::LineString);

    const ts::KmlFeature* track = find(document.root, QStringLiteral("Track"));
    if (track == nullptr || !track->geometry) {
      QFAIL("no track");
    }
    QCOMPARE(track->geometry->kind, ts::KmlGeometry::Kind::Track);
    QCOMPARE(track->geometry->coordinates.size(), std::size_t{2});
    QCOMPARE(track->geometry->when.size(), std::size_t{2});
    QCOMPARE(track->geometry->when.at(1), utc("2026-11-20T11:00:00Z"));
    QCOMPARE(track->geometry->coordinates.at(1).altM, 200.0);
    QCOMPARE(track->geometry->altitudeMode,
             ts::KmlAltitudeMode::RelativeToSeaFloor);
  }

  void timesViewsRegions()
  {
    const ts::KmlDocument document = read(QStringLiteral("everything.kml"));
    const ts::KmlFeature* stamped =
        find(document.root, QStringLiteral("Stamped"));
    if (stamped == nullptr || !stamped->time || !stamped->view ||
        !stamped->region) {
      QFAIL("stamped incomplete");
    }
    QVERIFY(stamped->time->contains(utc("2026-11-20T12:00:00Z")));
    QVERIFY(!stamped->time->contains(utc("2026-11-20T12:00:01Z")));
    QCOMPARE(stamped->view->kind, ts::KmlView::Kind::Camera);
    QCOMPARE(stamped->view->rollDeg, 2.0);
    QCOMPARE(stamped->region->minLodPixels, 128.0);
    QCOMPARE(stamped->region->north, 51.6);
    const ts::KmlFeature* spanned =
        find(document.root, QStringLiteral("Spanned"));
    if (spanned == nullptr || !spanned->time) {
      QFAIL("spanned incomplete");
    }
    QVERIFY(spanned->time->contains(utc("2026-03-01")));
    QVERIFY(!spanned->time->contains(utc("2026-07-01")));
    // Open-ended spans.
    ts::KmlTime open;
    open.begin = utc("2026-01-01");
    QVERIFY(open.contains(utc("2099-01-01")));
    QVERIFY(!open.contains(utc("2025-12-31")));
  }

  void overlaysAndLinks()
  {
    const ts::KmlDocument document = read(QStringLiteral("everything.kml"));
    const ts::KmlFeature* picture =
        find(document.root, QStringLiteral("A picture"));
    if (picture == nullptr || !picture->latLonBox) {
      QFAIL("no ground overlay");
    }
    QCOMPARE(picture->kind, ts::KmlFeature::Kind::GroundOverlay);
    QCOMPARE(picture->iconHref, QStringLiteral("images/overlay.png"));
    QCOMPARE(picture->color.alpha(), 128);
    QCOMPARE(picture->drawOrder, 2);
    QCOMPARE(picture->latLonBox->rotationDeg, 15.0);
    QCOMPARE(picture->latLonBox->west, -1.0);
    const ts::KmlFeature* quad = find(document.root, QStringLiteral("A quad"));
    if (quad == nullptr || !quad->latLonBox) {
      QFAIL("no quad");
    }
    QCOMPARE(quad->latLonBox->quad.size(), std::size_t{4});
    QCOMPARE(quad->latLonBox->quad.at(3).lonDeg, -0.5);
    const ts::KmlFeature* legend =
        find(document.root, QStringLiteral("A legend"));
    if (legend == nullptr || !legend->screen) {
      QFAIL("no screen overlay");
    }
    QCOMPARE(legend->screen->overlayXY.y, 1.0);
    QCOMPARE(legend->screen->screenXY.yUnits, ts::KmlVec2::Units::InsetPixels);
    QCOMPARE(legend->screen->rotationDeg, 5.0);
    const ts::KmlFeature* live = find(document.root, QStringLiteral("Live"));
    if (live == nullptr || !live->link) {
      QFAIL("no network link");
    }
    QVERIFY(live->flyToView);
    QCOMPARE(live->link->refreshMode, ts::KmlLink::RefreshMode::OnInterval);
    QCOMPARE(live->link->refreshIntervalS, 30.0);
    QCOMPARE(live->link->viewRefreshMode, ts::KmlLink::ViewRefreshMode::OnStop);
    QCOMPARE(live->link->viewBoundScale, 0.9);
  }

  void styles()
  {
    const ts::KmlDocument document = read(QStringLiteral("everything.kml"));
    ts::KmlStyleResolver resolver(document);
    // Through a StyleMap: normal and highlight.
    const ts::KmlFeature* point =
        find(document.root, QStringLiteral("A point"));
    if (point == nullptr) {
      QFAIL("no point");
    }
    const ts::KmlStyle normal = resolver.resolve(*point);
    if (!normal.icon || !normal.label || !normal.line || !normal.poly ||
        !normal.balloon) {
      QFAIL("normal style incomplete");
    }
    QCOMPARE(normal.icon->color, QColor(255, 0, 0));
    QCOMPARE(normal.icon->scale, 1.5);
    QCOMPARE(normal.icon->href, QStringLiteral("icons/pin.png"));
    QCOMPARE(normal.icon->hotSpot.y, 0.0);
    QCOMPARE(normal.label->color, QColor(255, 255, 0));
    QCOMPARE(normal.poly->color, QColor(0, 255, 0, 127));
    QVERIFY(!normal.poly->outline);
    QVERIFY(normal.balloon->text.contains(QLatin1String("$[name]")));
    const ts::KmlStyle highlight = resolver.resolve(*point, true);
    if (!highlight.line) {
      QFAIL("no highlight line");
    }
    QCOMPARE(highlight.line->color, QColor(0, 0, 255));
    QVERIFY(!highlight.icon);
    // Inline.
    const ts::KmlFeature* line = find(document.root, QStringLiteral("A line"));
    if (line == nullptr) {
      QFAIL("no line");
    }
    const ts::KmlStyle inline_ = resolver.resolve(*line);
    if (!inline_.line) {
      QFAIL("no inline line");
    }
    QCOMPARE(inline_.line->color, QColor(255, 165, 0));
    QCOMPARE(inline_.line->width, 5.0);
    // ListStyle on a folder.
    const ts::KmlStyle radio = resolver.resolve(document.root.children.at(1));
    if (!radio.list) {
      QFAIL("no list style");
    }
    QCOMPARE(radio.list->itemType, ts::KmlListStyle::ItemType::RadioFolder);
    // In another file, through its StyleMap.
    const ts::KmlFeature* external =
        find(document.root, QStringLiteral("External style"));
    if (external == nullptr) {
      QFAIL("no external");
    }
    const ts::KmlStyle shared = resolver.resolve(*external);
    if (!shared.line) {
      QFAIL("external style not resolved");
    }
    QCOMPARE(shared.line->width, 7.0);
    QCOMPARE(shared.line->color, QColor(0x56, 0x34, 0x12));
    // A style that doesn't exist: the defaults.
    ts::KmlFeature missing;
    missing.styleUrl = QStringLiteral("#nothing");
    QVERIFY(!resolver.resolve(missing).line);
  }

  void colours()
  {
    QCOMPARE(ts::kmlColor(QStringLiteral("7f00ff00")), QColor(0, 255, 0, 127));
    QCOMPARE(ts::kmlColor(QStringLiteral("#ff0000ff")), QColor(255, 0, 0));
    QCOMPARE(ts::kmlColor(QStringLiteral("0000ff")), QColor(255, 0, 0));
    QVERIFY(!ts::kmlColor(QStringLiteral("red")).isValid());
    QCOMPARE(ts::kmlColorText(QColor(0x12, 0x34, 0x56, 0x78)),
             QStringLiteral("78563412"));
    QCOMPARE(ts::kmlAltitudeModeName(ts::KmlAltitudeMode::ClampToSeaFloor),
             QStringLiteral("clampToSeaFloor"));
  }

  void kmz()
  {
    const ts::KmlDocument document = read(QStringLiteral("archive.kmz"));
    QVERIFY(document.kmz);
    QCOMPARE(document.root.name, QStringLiteral("In an archive"));
    QVERIFY(document.base.startsWith(QLatin1String("/vsizip/")));
    ts::KmlStyleResolver resolver(document);
    const ts::KmlStyle style = resolver.resolve(document.root.children.at(0));
    if (!style.icon) {
      QFAIL("no icon style");
    }
    QString error;
    const QByteArray icon =
        ts::readKmlResource(document, style.icon->href, &error);
    QVERIFY2(!icon.isEmpty(), qPrintable(error));
    QVERIFY(QImage::fromData(icon).width() == 8);
    const QByteArray square = ts::readKmlResource(
        document, document.root.children.at(1).iconHref, &error);
    QCOMPARE(QImage::fromData(square).size(), QSize(16, 16));
    // Missing and web resources aren't read.
    QVERIFY(
        ts::readKmlResource(document, QStringLiteral("files/none.png"), &error)
            .isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY(ts::readKmlResource(document,
                                QStringLiteral("https://example.invalid/a.png"),
                                &error)
                .isEmpty());
    QCOMPARE(ts::resolveKmlHref(
                 document, QStringLiteral("https://example.invalid/a.png")),
             QStringLiteral("https://example.invalid/a.png"));
  }

  void otherPrograms_data()
  {
    QTest::addColumn<QString>("file");
    QTest::addColumn<int32_t>("placemarks");
    // False where the file has empty geometries on purpose.
    QTest::addColumn<bool>("complete");
    QTest::newRow("Google's samples") << "gdal/samples.kml" << 20 << true;
    // 25: a Placemark inside a MultiGeometry isn't one (skipped).
    QTest::newRow("geometries") << "gdal/geometries.kml" << 25 << false;
    QTest::newRow("gx:Track") << "gdal/gxtrack.kml" << 1 << true;
    QTest::newRow("gx:MultiTrack") << "gdal/gxmultitrack.kml" << 1 << true;
    QTest::newRow("kml: prefix")
        << "gdal/placemark_with_kml_prefix.kml" << 1 << true;
    QTest::newRow("spaces in coordinates")
        << "gdal/kml_with_space_content_in_coordinates.kml" << 1 << false;
    QTest::newRow("tabs in coordinates")
        << "gdal/tab_separated_coord_triplet.kml" << 1 << true;
    QTest::newRow("extended data")
        << "gdal/extended_data_without_schema_data.kml" << 2 << true;
    QTest::newRow("schema") << "gdal/test_schema.kml" << 4 << true;
  }

  void otherPrograms()
  {
    QFETCH(QString, file);
    QFETCH(int32_t, placemarks);
    QFETCH(bool, complete);
    QString error;
    const std::optional<ts::KmlDocument> document =
        ts::readKmlFile(fixture(file), &error);
    if (!document) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(countPlacemarks(document->root), placemarks);
    // Every placemark has a geometry with coordinates.
    const std::function<void(const ts::KmlFeature&)> check =
        [&check](const ts::KmlFeature& f) {
          if (f.kind == ts::KmlFeature::Kind::Placemark && f.geometry) {
            const bool any = !f.geometry->coordinates.empty() ||
                             !f.geometry->children.empty();
            QVERIFY2(any, qPrintable(f.name));
          }
          for (const ts::KmlFeature& child : f.children) {
            check(child);
          }
        };
    if (complete) {
      check(document->root);
    }
  }

  void googleSamplesDetails()
  {
    const ts::KmlDocument document = read(QStringLiteral("gdal/samples.kml"));
    ts::KmlStyleResolver resolver(document);
    const ts::KmlFeature* tessellated =
        find(document.root, QStringLiteral("Tessellated"));
    if (tessellated == nullptr || !tessellated->geometry) {
      QFAIL("no Tessellated");
    }
    QVERIFY(tessellated->geometry->tessellate);
    const ts::KmlFeature* purple =
        find(document.root, QStringLiteral("Building 40"));
    if (purple == nullptr) {
      QFAIL("no Building 40");
    }
    const ts::KmlStyle style = resolver.resolve(*purple);
    if (!style.poly) {
      QFAIL("no poly style");
    }
    QCOMPARE(style.poly->color, QColor(255, 0, 0, 0x7d)); // 7d0000ff
    // External style file, relative to the document.
    const ts::KmlDocument pointDoc =
        read(QStringLiteral("gdal/point_with_external_style.kml"));
    ts::KmlStyleResolver external(pointDoc);
    const ts::KmlFeature* point =
        find(pointDoc.root, QStringLiteral("my point"));
    if (point == nullptr) {
      QFAIL("no point");
    }
    const ts::KmlStyle pointStyle = external.resolve(*point);
    if (!pointStyle.label || !pointStyle.balloon) {
      QFAIL("external style not read");
    }
    QCOMPARE(pointStyle.label->scale, 1.1);
    QCOMPARE(pointStyle.balloon->text, QStringLiteral("This is $[name]"));
  }

  void errors()
  {
    QString error;
    QVERIFY(!ts::readKmlFile(fixture(QStringLiteral("broken.kml")), &error));
    QVERIFY2(error.contains(QLatin1String("line")), qPrintable(error));
    QVERIFY(!ts::readKmlFile(fixture(QStringLiteral("gdal/truncated.kml")),
                             &error));
    QVERIFY(error.contains(QLatin1String("line")));
    QVERIFY(!ts::readKmlFile(fixture(QStringLiteral("nothing.kml")), &error));
    QVERIFY(!ts::parseKml("<html><body/></html>", QString(), &error));
    QVERIFY(error.contains(QLatin1String("html")));
    QVERIFY(!ts::parseKml("", QString(), &error));
    // A bare Placemark at the top is read.
    const std::optional<ts::KmlDocument> bare = ts::parseKml(
        "<Placemark><name>x</name><Point><coordinates>1,2</coordinates>"
        "</Point></Placemark>",
        QString(), &error);
    QVERIFY(bare.has_value());
    QCOMPARE(bare.value_or(ts::KmlDocument{}).root.name, QStringLiteral("x"));
  }

  void placemarkFeatures()
  {
    const ts::KmlDocument document = read(QStringLiteral("everything.kml"));
    ts::KmlStyleResolver resolver(document);
    // A polygon with a hole: its fill from PolyStyle, no outline (outline 0).
    const ts::KmlFeature* polygon =
        find(document.root, QStringLiteral("A polygon with a hole"));
    if (polygon == nullptr) {
      QFAIL("no polygon");
    }
    const std::vector<ts::Feature> areas =
        ts::kmlPlacemarkFeatures(*polygon, resolver.resolve(*polygon));
    QCOMPARE(areas.size(), std::size_t{1});
    QCOMPARE(areas.front().type, ts::Feature::Type::Polygon);
    QCOMPARE(areas.front().parts.size(), std::size_t{2});
    const ts::FeatureStyle area =
        areas.front().style.value_or(ts::FeatureStyle());
    QCOMPARE(area.fill.value_or(QColor()), QColor(0, 255, 0, 127));
    QCOMPARE(area.outline.value_or(QColor()).alpha(), 0);
    QCOMPARE(areas.front().attributes.value(QStringLiteral("name")).toString(),
             QStringLiteral("A polygon with a hole"));
    // A line: its colour and width.
    const ts::KmlFeature* line = find(document.root, QStringLiteral("A line"));
    if (line == nullptr) {
      QFAIL("no line");
    }
    const std::vector<ts::Feature> lines =
        ts::kmlPlacemarkFeatures(*line, resolver.resolve(*line));
    QCOMPARE(lines.size(), std::size_t{1});
    QCOMPARE(lines.front()
                 .style.value_or(ts::FeatureStyle())
                 .outline.value_or(QColor()),
             QColor(255, 165, 0));
    QCOMPARE(lines.front()
                 .style.value_or(ts::FeatureStyle())
                 .outlineWidthPx.value_or(0.0),
             5.0);
    // Points: the icon's colour and scale; the default icon is yellow.
    const ts::KmlFeature* point =
        find(document.root, QStringLiteral("A point"));
    if (point == nullptr) {
      QFAIL("no point");
    }
    const std::vector<ts::Feature> dots =
        ts::kmlPlacemarkFeatures(*point, resolver.resolve(*point));
    QCOMPARE(dots.front().type, ts::Feature::Type::Points);
    QCOMPARE(dots.front()
                 .style.value_or(ts::FeatureStyle())
                 .outline.value_or(QColor()),
             QColor(255, 0, 0));
    QCOMPARE(dots.front()
                 .style.value_or(ts::FeatureStyle())
                 .pointSizePx.value_or(0.0),
             13.5);
    QCOMPARE(
        dots.front().attributes.value(QStringLiteral("Population")).toString(),
        QStringLiteral("1141816"));
    const ts::KmlFeature* plain =
        find(document.root, QStringLiteral("Spanned"));
    if (plain == nullptr) {
      QFAIL("no plain point");
    }
    QCOMPARE(ts::kmlPlacemarkFeatures(*plain, ts::KmlStyle{})
                 .front()
                 .style.value_or(ts::FeatureStyle())
                 .outline.value_or(QColor()),
             QColor(255, 214, 0));
    // A MultiGeometry: one feature per member.
    const ts::KmlFeature* multi =
        find(document.root, QStringLiteral("Two points and a line"));
    if (multi == nullptr) {
      QFAIL("no multigeometry");
    }
    QCOMPARE(ts::kmlPlacemarkFeatures(*multi, ts::KmlStyle{}).size(),
             std::size_t{3});
  }

  void antimeridian()
  {
    // A line from 170° E to 170° W goes the short way, and comes twice:
    // as written past 180°, and shifted to the western side.
    ts::KmlFeature placemark;
    placemark.kind = ts::KmlFeature::Kind::Placemark;
    ts::KmlGeometry line;
    line.kind = ts::KmlGeometry::Kind::LineString;
    line.coordinates = {ts::KmlCoordinate{170.0, 10.0, 0.0},
                        ts::KmlCoordinate{-170.0, 12.0, 0.0}};
    placemark.geometry = line;
    const std::vector<ts::Feature> features =
        ts::kmlPlacemarkFeatures(placemark, ts::KmlStyle{});
    QCOMPARE(features.size(), std::size_t{2});
    QCOMPARE(features.at(0).parts.front().back().lonDeg, -170.0);
    QCOMPARE(features.at(0).parts.front().front().lonDeg, -190.0);
    QCOMPARE(features.at(1).parts.front().back().lonDeg, 190.0);
  }

  void provider()
  {
    QString error;
    const std::shared_ptr<const ts::KmlDocument> document =
        ts::sharedKmlDocument(fixture(QStringLiteral("everything.kml")),
                              &error);
    if (!document) {
      QFAIL(qPrintable(error));
    }
    // One copy while anything holds it.
    QCOMPARE(ts::sharedKmlDocument(fixture(QStringLiteral("everything.kml")),
                                   &error),
             document);
    QVERIFY(
        !ts::sharedKmlDocument(fixture(QStringLiteral("none.kml")), &error));
    // Containers by path.
    QCOMPARE(ts::kmlContainer(*document, QString()), &document->root);
    const ts::KmlFeature* shapes =
        ts::kmlContainer(*document, QStringLiteral("0"));
    QVERIFY(shapes != nullptr);
    QCOMPARE(shapes->name, QStringLiteral("Shapes"));
    QVERIFY(ts::kmlContainer(*document, QStringLiteral("9")) == nullptr);
    QVERIFY(ts::kmlContainer(*document, QStringLiteral("x")) == nullptr);
    const QJsonObject source =
        ts::kmlLayerSource(document->path, QStringLiteral("0"));
    QCOMPARE(source.value(QStringLiteral("kind")).toString(),
             QStringLiteral("kml"));

    ts::KmlFeatureProvider provider(document, QStringLiteral("0"));
    QCOMPARE(provider.name(), QStringLiteral("Shapes"));
    // Point, line, ring, polygon and the three of the MultiGeometry.
    QCOMPARE(provider.featureCount(), std::size_t{7});
    const std::atomic<bool> cancelled{false};
    // England's tile has them; the Pacific's none.
    const std::optional<ts::FeatureTile> england = provider.loadFeatures(
        ts::TileKey::containing(5, ts::Geodetic{52.5, -1.5, 0.0}), cancelled);
    QVERIFY(england.has_value());
    QVERIFY(!england.value_or(ts::FeatureTile{}).features.empty());
    QVERIFY(!provider.loadFeatures(
        ts::TileKey::containing(5, ts::Geodetic{0.0, -150.0, 0.0}), cancelled));
    QCOMPARE(provider
                 .storedKey(
                     ts::TileKey::containing(12, ts::Geodetic{52.5, -1.5, 0.0}))
                 .level,
             6);
    // Picking: inside the polygon, not in its hole; near the point.
    const std::vector<const ts::KmlFeature*> inPolygon =
        provider.placemarksAt(ts::GeoPoint{52.2, -1.8}, 0.01);
    QCOMPARE(inPolygon.size(), std::size_t{1});
    QCOMPARE(inPolygon.front()->name, QStringLiteral("A polygon with a hole"));
    QVERIFY(provider.placemarksAt(ts::GeoPoint{52.45, -1.5}, 0.001).empty());
    const std::vector<const ts::KmlFeature*> atPoint =
        provider.placemarksAt(ts::GeoPoint{52.4862, -1.8904}, 0.01);
    QVERIFY(!atPoint.empty());
    QCOMPARE(atPoint.front()->name, QStringLiteral("A point"));
    QCOMPARE(provider.featuresAt(ts::GeoPoint{52.2, -1.8}, 0.01).size(),
             std::size_t{1});
    // A container with no placemarks of its own has none.
    ts::KmlFeatureProvider root(document, QString());
    QCOMPARE(root.featureCount(), std::size_t{1}); // "External style"
  }

  void billboards()
  {
    // A point's icon, tint, scale, heading, hot spot and label.
    QString error;
    const std::shared_ptr<const ts::KmlDocument> document =
        ts::sharedKmlDocument(fixture(QStringLiteral("everything.kml")),
                              &error);
    if (!document) {
      QFAIL(qPrintable(error));
    }
    ts::KmlFeatureProvider shapes(document, QStringLiteral("0"));
    // The point and the two of the MultiGeometry; only the first of a
    // placemark's points is labelled.
    QCOMPARE(shapes.billboards().size(), std::size_t{3});
    const ts::KmlBillboard& point = shapes.billboards().front();
    QCOMPARE(point.label, QStringLiteral("A point"));
    QVERIFY(point.icon.endsWith(QLatin1String("icons/pin.png")));
    QCOMPARE(point.tint, QColor(255, 0, 0));
    QCOMPARE(point.scale, 1.5);
    QCOMPARE(point.headingDeg, 45.0);
    QCOMPARE(point.hotSpot.x, 0.5);
    QCOMPARE(point.hotSpot.y, 0.0);
    QCOMPARE(point.labelColor, QColor(255, 255, 0));
    QCOMPARE(point.labelScale, 0.8);
    QCOMPARE(point.lonDeg, -1.8904);
    QVERIFY(point.placemark != nullptr);
    QVERIFY(!shapes.billboards().at(1).label.isEmpty());
    QVERIFY(shapes.billboards().at(2).label.isEmpty());
    // Points come as billboards, not as dots in tiles; picking still finds
    // them.
    const std::atomic<bool> cancelled{false};
    const ts::FeatureTile tile =
        shapes
            .loadFeatures(
                ts::TileKey::containing(5, ts::Geodetic{52.5, -1.5, 0.0}),
                cancelled)
            .value_or(ts::FeatureTile{});
    QVERIFY(std::none_of(tile.features.begin(), tile.features.end(),
                         [](const ts::Feature& feature) {
                           return feature.type == ts::Feature::Type::Points;
                         }));
    QVERIFY(!shapes.placemarksAt(ts::GeoPoint{52.4862, -1.8904}, 0.01).empty());
    // A KMZ's icon is inside the archive, and can be read.
    const std::shared_ptr<const ts::KmlDocument> kmz =
        ts::sharedKmlDocument(fixture(QStringLiteral("archive.kmz")), &error);
    if (!kmz) {
      QFAIL(qPrintable(error));
    }
    ts::KmlFeatureProvider pinned(kmz, QString());
    QCOMPARE(pinned.billboards().size(), std::size_t{1});
    const QString icon = pinned.billboards().front().icon;
    QVERIFY2(icon.startsWith(QLatin1String("/vsizip/")), qPrintable(icon));
    QVERIFY(!QImage::fromData(ts::readKmlLocalFile(icon)).isNull());
    QVERIFY(ts::readKmlLocalFile(icon + QStringLiteral(".none")).isEmpty());
    // No style: the default pushpin (no icon address), the name in white.
    ts::KmlFeature plain;
    plain.name = QStringLiteral("Plain");
    ts::KmlGeometry at;
    at.kind = ts::KmlGeometry::Kind::Point;
    at.coordinates = {ts::KmlCoordinate{1.0, 2.0, 300.0}};
    at.altitudeMode = ts::KmlAltitudeMode::Absolute;
    plain.geometry = at;
    const std::vector<ts::KmlBillboard> one =
        ts::kmlBillboards(*kmz, plain, ts::KmlStyle{});
    QCOMPARE(one.size(), std::size_t{1});
    QVERIFY(one.front().icon.isEmpty());
    QCOMPARE(one.front().labelColor, QColor(255, 255, 255));
    QCOMPARE(one.front().altM, 300.0);
    QCOMPARE(one.front().altitudeMode, ts::KmlAltitudeMode::Absolute);
    QVERIFY(ts::kmlBillboards(*kmz, ts::KmlFeature{}, ts::KmlStyle{}).empty());
  }

  void heights()
  {
    // Shapes above the ground: drawn over the view, not in the tiles.
    QString error;
    const std::shared_ptr<const ts::KmlDocument> document =
        ts::sharedKmlDocument(fixture(QStringLiteral("heights.kml")), &error);
    if (!document) {
      QFAIL(qPrintable(error));
    }
    ts::KmlFeatureProvider provider(document, QString());
    const std::vector<ts::KmlRaisedShape>& raised = provider.raisedShapes();
    QCOMPARE(raised.size(), std::size_t{2});
    const ts::KmlRaisedShape& block = raised.at(0);
    QVERIFY(block.closed);
    QVERIFY(block.surface);
    QVERIFY(block.extrude);
    QVERIFY(!block.relative);
    QCOMPARE(block.fill, QColor(0, 128, 255, 192));
    QCOMPARE(block.line, QColor(0, 0, 0));
    QCOMPARE(block.widthPx, 2.0);
    QCOMPARE(block.rings.size(), std::size_t{1});
    QCOMPARE(block.rings.front().front().altM, 40000.0);
    const ts::KmlRaisedShape& wall = raised.at(1);
    QVERIFY(!wall.closed);
    QVERIFY(!wall.surface);
    QVERIFY(wall.relative);
    QCOMPARE(wall.fill, QColor(255, 0, 0, 128));
    // The 2.4° edge is divided into three, heights kept.
    QCOMPARE(wall.rings.front().size(), std::size_t{5});
    QCOMPARE(wall.rings.front().at(1).altM, 30000.0);
    QCOMPARE(wall.rings.front().back().lonDeg, 2.7);
    // The stalk under the point.
    QCOMPARE(provider.billboards().size(), std::size_t{1});
    QVERIFY(provider.billboards().front().extrude);
    QCOMPARE(provider.billboards().front().stalkColor, QColor(255, 0, 0));
    // The tiles have only the draped line; picking finds all four.
    const std::atomic<bool> cancelled{false};
    const ts::FeatureTile tile =
        provider
            .loadFeatures(
                ts::TileKey::containing(4, ts::Geodetic{51.4, 0.0, 0.0}),
                cancelled)
            .value_or(ts::FeatureTile{});
    QCOMPARE(tile.features.size(), std::size_t{1});
    QCOMPARE(tile.features.front().attributes.value(QStringLiteral("name")),
             QVariant(QStringLiteral("Draped")));
    QCOMPARE(
        provider.placemarksAt(ts::GeoPoint{51.53, -0.08}, 0.001).front()->name,
        QStringLiteral("A block"));
    QCOMPARE(provider.featureCount(), std::size_t{4});
    // A line without extrude: no walls, so no fill; a polygon with outline
    // 0: no line.
    ts::KmlFeature line;
    ts::KmlGeometry geometry;
    geometry.kind = ts::KmlGeometry::Kind::LineString;
    geometry.altitudeMode = ts::KmlAltitudeMode::Absolute;
    geometry.coordinates = {ts::KmlCoordinate{0.0, 0.0, 10.0},
                            ts::KmlCoordinate{0.5, 0.0, 10.0}};
    line.geometry = geometry;
    QCOMPARE(ts::kmlRaisedShapes(line, ts::KmlStyle{}).front().fill.alpha(), 0);
    geometry.kind = ts::KmlGeometry::Kind::Polygon;
    geometry.coordinates.push_back(ts::KmlCoordinate{0.5, 0.5, 10.0});
    line.geometry = geometry;
    ts::KmlStyle noOutline;
    ts::KmlPolyStyle poly;
    poly.outline = false;
    noOutline.poly = poly;
    QCOMPARE(ts::kmlRaisedShapes(line, noOutline).front().line.alpha(), 0);
    // Clamped: none.
    geometry.altitudeMode = ts::KmlAltitudeMode::ClampToGround;
    line.geometry = geometry;
    QVERIFY(ts::kmlRaisedShapes(line, ts::KmlStyle{}).empty());
  }

  void overlayPlacement()
  {
    ts::KmlLatLonBox box;
    box.north = 10.0;
    box.south = 0.0;
    box.east = 20.0;
    box.west = 0.0;
    const QPointF centre =
        ts::kmlOverlayImagePoint(box, 10.0, 5.0).value_or(QPointF(-1, -1));
    QCOMPARE(centre, QPointF(0.5, 0.5));
    // y from the top of the picture.
    const QPointF upperLeft =
        ts::kmlOverlayImagePoint(box, 5.0, 7.5).value_or(QPointF(-1, -1));
    QCOMPARE(upperLeft, QPointF(0.25, 0.25));
    QVERIFY(!ts::kmlOverlayImagePoint(box, 21.0, 5.0));
    QVERIFY(!ts::kmlOverlayImagePoint(box, 10.0, -1.0));
    // Turned 90° anticlockwise: the picture's right edge faces north.
    box.rotationDeg = 90.0;
    const QPointF north =
        ts::kmlOverlayImagePoint(box, 10.0, 9.0).value_or(QPointF(-1, -1));
    QVERIFY(std::abs(north.x() - 0.7) < 1e-9);
    QVERIFY(std::abs(north.y() - 0.5) < 1e-9);
    const ts::GeoRect turned = ts::kmlOverlayBounds(box);
    QVERIFY(std::abs(turned.northDeg - 15.0) < 1e-9);
    QVERIFY(std::abs(turned.westDeg - 5.0) < 1e-9);
    // Across the antimeridian.
    box.rotationDeg = 0.0;
    box.west = 170.0;
    box.east = -170.0;
    const QPointF across =
        ts::kmlOverlayImagePoint(box, -175.0, 5.0).value_or(QPointF(-1, -1));
    QCOMPARE(across, QPointF(0.75, 0.5));
    QCOMPARE(ts::kmlOverlayBounds(box).eastDeg, 190.0);
    // A gx:LatLonQuad: its corners are the picture's.
    ts::KmlLatLonBox quad;
    quad.quad = {
        ts::KmlCoordinate{0.0, 40.0, 0.0}, ts::KmlCoordinate{2.0, 40.0, 0.0},
        ts::KmlCoordinate{2.5, 42.0, 0.0}, ts::KmlCoordinate{-0.5, 42.0, 0.0}};
    const QPointF lowerLeft = ts::kmlOverlayImagePoint(quad, 0.0001, 40.0001)
                                  .value_or(QPointF(-1, -1));
    QVERIFY(lowerLeft.x() < 0.01 && lowerLeft.y() > 0.99);
    const QPointF middle =
        ts::kmlOverlayImagePoint(quad, 1.0, 41.0).value_or(QPointF(-1, -1));
    QVERIFY(std::abs(middle.x() - 0.5) < 1e-9);
    QVERIFY(std::abs(middle.y() - 0.5) < 1e-9);
    QVERIFY(!ts::kmlOverlayImagePoint(quad, 2.2, 40.5));
    QCOMPARE(ts::kmlOverlayBounds(quad).westDeg, -0.5);
  }

  void groundOverlays()
  {
    // A picture warped into tiles, the right way up, tinted.
    QString error;
    const QString xml = QStringLiteral(
        "<kml xmlns=\"http://www.opengis.net/kml/2.2\"><GroundOverlay>"
        "<name>Quarters</name><color>80ffffff</color>"
        "<Icon><href>images/overlay.png</href></Icon>"
        "<LatLonBox><north>10</north><south>0</south><east>20</east>"
        "<west>0</west></LatLonBox></GroundOverlay></kml>");
    std::optional<ts::KmlDocument> parsed =
        ts::parseKml(xml.toUtf8(), fixture(QString()), &error);
    if (!parsed) {
      QFAIL(qPrintable(error));
    }
    const std::shared_ptr<const ts::KmlDocument> document =
        std::make_shared<const ts::KmlDocument>(std::move(*parsed));
    ts::KmlGroundOverlayProvider provider(document, QString(), nullptr);
    QVERIFY(provider.isValid());
    QCOMPARE(provider.name(), QStringLiteral("Quarters"));
    const std::atomic<bool> cancelled{false};
    // Level 4 tiles are 11.25° across; this one holds the picture's lower
    // right.
    const ts::TileKey key =
        ts::TileKey::containing(4, ts::Geodetic{5.0, 15.0, 0.0});
    const ts::ImageLoadResult result = provider.loadTileImage(key, cancelled);
    QCOMPARE(result.outcome, ts::ImageLoadResult::Outcome::Image);
    const QImage image = result.image.convertToFormat(QImage::Format_ARGB32);
    const ts::GeoRect bounds = key.bounds();
    const std::function<QColor(double, double)> pixelAt = [&](double lon,
                                                              double lat) {
      const int32_t x = static_cast<int32_t>((lon - bounds.westDeg) /
                                             bounds.widthDeg() * image.width());
      const int32_t y = static_cast<int32_t>(
          (bounds.northDeg - lat) / (bounds.northDeg - bounds.southDeg) *
          image.height());
      return QColor::fromRgba(image.pixel(x, y));
    };
    // Lower right white, lower left blue, upper right green; half opaque.
    QCOMPARE(pixelAt(15.0, 2.5), QColor(255, 255, 255, 128));
    QCOMPARE(pixelAt(15.0, 7.5), QColor(0, 255, 0, 128));
    // Outside the box: transparent.
    if (bounds.northDeg > 10.5) {
      QCOMPARE(pixelAt(15.0, 10.5).alpha(), 0);
    }
    // Tiles it doesn't touch have nothing.
    QCOMPARE(provider
                 .loadTileImage(ts::TileKey::containing(
                                    4, ts::Geodetic{-40.0, -100.0, 0.0}),
                                cancelled)
                 .outcome,
             ts::ImageLoadResult::Outcome::NoData);
    // 0.3125° a pixel: level 2's tiles match it; one finer.
    QCOMPARE(provider.maximumLevel(), 3);
    // A missing picture: nothing anywhere.
    const std::shared_ptr<const ts::KmlDocument> everything =
        ts::sharedKmlDocument(fixture(QStringLiteral("everything.kml")),
                              &error);
    if (!everything) {
      QFAIL(qPrintable(error));
    }
    // A placemark isn't a ground overlay.
    QVERIFY(!ts::KmlGroundOverlayProvider(everything, QStringLiteral("0.0"),
                                          nullptr)
                 .isValid());
    // The KMZ's square, read from the archive.
    const std::shared_ptr<const ts::KmlDocument> kmz =
        ts::sharedKmlDocument(fixture(QStringLiteral("archive.kmz")), &error);
    if (!kmz) {
      QFAIL(qPrintable(error));
    }
    ts::KmlGroundOverlayProvider square(kmz, QStringLiteral("1"), nullptr);
    QVERIFY(square.isValid());
    const ts::ImageLoadResult inside = square.loadTileImage(
        ts::TileKey::containing(6, ts::Geodetic{50.5, 10.5, 0.0}), cancelled);
    QCOMPARE(inside.outcome, ts::ImageLoadResult::Outcome::Image);
    // Through the registry.
    ts::ProviderRegistry registry;
    const std::shared_ptr<ts::ImageryProvider> fromRegistry = registry.imagery(
        ts::kmlOverlaySource(fixture(QStringLiteral("archive.kmz")),
                             QStringLiteral("1")),
        &error);
    QVERIFY(
        std::dynamic_pointer_cast<ts::KmlGroundOverlayProvider>(fromRegistry));
    QVERIFY(!registry.imagery(
        ts::kmlOverlaySource(fixture(QStringLiteral("archive.kmz")),
                             QStringLiteral("0")),
        &error));
  }

  void screenOverlays()
  {
    // everything.kml's legend: its top left 10 pixels in from the top left.
    const ts::KmlDocument document = read(QStringLiteral("everything.kml"));
    const ts::KmlFeature* legend =
        find(document.root, QStringLiteral("A legend"));
    if (legend == nullptr) {
      QFAIL("no legend");
    }
    const ts::KmlScreenRect placed =
        ts::kmlScreenRect(legend->screen.value_or(ts::KmlScreenPlacement{}),
                          QSizeF(40, 20), QSizeF(800, 600));
    QCOMPARE(placed.rect, QRectF(10, 10, 40, 20));
    QCOMPARE(placed.pivot, QPointF(400, 300));
    QCOMPARE(placed.rotationDeg, -5.0);
    // Defaults: the bottom left corner, its own size.
    ts::KmlScreenPlacement plain;
    QCOMPARE(ts::kmlScreenRect(plain, QSizeF(40, 20), QSizeF(800, 600)).rect,
             QRectF(0, 580, 40, 20));
    // Half the width, keeping its aspect, centred.
    plain.size = ts::KmlVec2{0.5, 0.0};
    plain.overlayXY = ts::KmlVec2{0.5, 0.5};
    plain.screenXY = ts::KmlVec2{0.5, 0.5};
    QCOMPARE(ts::kmlScreenRect(plain, QSizeF(40, 20), QSizeF(800, 600)).rect,
             QRectF(200, 200, 400, 200));
    // A ScreenOverlay's layer: the provider has it.
    QString error;
    const std::shared_ptr<const ts::KmlDocument> shared = ts::sharedKmlDocument(
        fixture(QStringLiteral("everything.kml")), &error);
    if (!shared) {
      QFAIL(qPrintable(error));
    }
    QString path;
    for (std::size_t i = 0; i < shared->root.children.size(); ++i) {
      if (shared->root.children.at(i).kind ==
          ts::KmlFeature::Kind::ScreenOverlay) {
        path = QString::number(i);
      }
    }
    ts::KmlFeatureProvider provider(shared, path);
    QCOMPARE(provider.screenOverlays().size(), std::size_t{1});
    QCOMPARE(provider.featureCount(), std::size_t{0});
  }

  void times()
  {
    const QDateTime june(QDate(2026, 6, 1), QTime(0, 0), QTimeZone::utc());
    const QDateTime july(QDate(2026, 7, 1), QTime(0, 0), QTimeZone::utc());
    ts::KmlTime stamp;
    stamp.begin = june;
    stamp.end = june;
    // A TimeStamp: from then on.
    QVERIFY(!ts::kmlShownAt(stamp, june.addSecs(-1)));
    QVERIFY(ts::kmlShownAt(stamp, june));
    QVERIFY(ts::kmlShownAt(stamp, july));
    // A TimeSpan: up to its end.
    ts::KmlTime span;
    span.begin = june;
    span.end = july;
    QVERIFY(ts::kmlShownAt(span, june));
    QVERIFY(!ts::kmlShownAt(span, july));
    QVERIFY(ts::kmlShownAt(ts::KmlTime{}, july));
    // Narrowed by a container's.
    ts::KmlTime later;
    later.begin = june.addDays(10);
    const ts::KmlTime narrowed =
        ts::kmlNarrowTime(span, later).value_or(ts::KmlTime{});
    QCOMPARE(narrowed.begin.value_or(QDateTime()), june.addDays(10));
    QCOMPARE(narrowed.end.value_or(QDateTime()), july);
    QVERIFY(!ts::kmlNarrowTime(std::nullopt, std::nullopt));

    // everything.kml's "Times and places": a stamp and a span.
    QString error;
    const std::shared_ptr<const ts::KmlDocument> document =
        ts::sharedKmlDocument(fixture(QStringLiteral("everything.kml")),
                              &error);
    if (!document) {
      QFAIL(qPrintable(error));
    }
    const QString times = QStringLiteral("1");
    QCOMPARE(ts::kmlContainer(*document, times)->name,
             QStringLiteral("Times and places"));
    const std::vector<QDateTime> boundaries =
        ts::kmlTimeBoundaries(*document, times, false);
    QCOMPARE(boundaries.size(), std::size_t{3});
    QCOMPARE(boundaries.front(),
             QDateTime(QDate(2026, 1, 1), QTime(0, 0), QTimeZone::utc()));
    // The key stays put between boundaries.
    QCOMPARE(ts::kmlTimeKey(boundaries, june), boundaries.front());
    QCOMPARE(ts::kmlTimeKey(boundaries, june.addDays(3)), boundaries.front());
    QCOMPARE(ts::kmlTimeKey(boundaries, july), boundaries.at(1));
    QCOMPARE(ts::kmlTimeKey(boundaries, boundaries.front().addDays(-9)),
             boundaries.front().addSecs(-1));
    QVERIFY(ts::kmlTimeKey({}, july).isNull());
    // The time bar: the boundaries and the track's hour in 36 s steps.
    const QString extent = ts::kmlTimeExtent(*document, times, false);
    QVERIFY2(extent.contains(QLatin1String(
                 "2026-11-20T10:00:00Z/2026-11-20T11:00:00Z/PT36S")),
             qPrintable(extent));
    QVERIFY(extent.startsWith(QLatin1String("2026-01-01T00:00:00Z,")));
    QVERIFY(ts::kmlTimeExtent(*document, QStringLiteral("0"), false).isEmpty());
    // The provider at a time: in June the span shows, not the stamp.
    const std::vector<QString> inJune = names(ts::KmlFeatureProvider(
        document, times, ts::kmlTimeKey(boundaries, june)));
    QCOMPARE(inJune, (std::vector<QString>{QStringLiteral("Spanned"),
                                           QStringLiteral("Track")}));
    const QDateTime evening(QDate(2026, 11, 20), QTime(18, 0),
                            QTimeZone::utc());
    const std::vector<QString> inNovember = names(ts::KmlFeatureProvider(
        document, times, ts::kmlTimeKey(boundaries, evening)));
    QCOMPARE(inNovember, (std::vector<QString>{QStringLiteral("Stamped"),
                                               QStringLiteral("Track")}));
    // Without a time, all.
    QCOMPARE(names(ts::KmlFeatureProvider(document, times)).size(),
             std::size_t{3});
    QCOMPARE(ts::kmlEffectiveTime(*document, QStringLiteral("1.1"))
                 .value_or(ts::KmlTime{})
                 .end.value_or(QDateTime()),
             QDateTime(QDate(2026, 6, 30), QTime(0, 0), QTimeZone::utc()));
  }

  void tracks()
  {
    QString error;
    const std::shared_ptr<const ts::KmlDocument> document =
        ts::sharedKmlDocument(fixture(QStringLiteral("everything.kml")),
                              &error);
    if (!document) {
      QFAIL(qPrintable(error));
    }
    const ts::KmlFeature* track =
        ts::kmlContainer(*document, QStringLiteral("1.2"));
    QVERIFY(track != nullptr && track->geometry.has_value());
    const ts::KmlGeometry& geometry =
        track->geometry.value_or(ts::KmlGeometry{});
    const QDateTime start(QDate(2026, 11, 20), QTime(10, 0), QTimeZone::utc());
    // Half way: half way along, half way up.
    const ts::KmlCoordinate half =
        ts::kmlTrackPosition(geometry, start.addSecs(1800))
            .value_or(ts::KmlCoordinate{});
    QCOMPARE(half.lonDeg, -0.75);
    QCOMPARE(half.latDeg, 51.1);
    QCOMPARE(half.altM, 150.0);
    QVERIFY(!ts::kmlTrackPosition(geometry, start.addSecs(-1)));
    QVERIFY(!ts::kmlTrackPosition(geometry, start.addSecs(3601)));
    // Across the antimeridian: the short way.
    ts::KmlGeometry across;
    across.kind = ts::KmlGeometry::Kind::Track;
    across.when = {start, start.addSecs(100)};
    across.coordinates = {ts::KmlCoordinate{179.0, 0.0, 0.0},
                          ts::KmlCoordinate{-179.0, 0.0, 0.0}};
    QCOMPARE(ts::kmlTrackPosition(across, start.addSecs(75))
                 .value_or(ts::KmlCoordinate{})
                 .lonDeg,
             -179.5);
    // A MultiGeometry's track.
    ts::KmlGeometry multi;
    multi.kind = ts::KmlGeometry::Kind::MultiGeometry;
    multi.children = {geometry};
    QVERIFY(ts::kmlTrackPosition(multi, start.addSecs(60)).has_value());
    // The provider's marker: where the track is, as its placemark.
    ts::KmlFeatureProvider provider(document, QStringLiteral("1"));
    QVERIFY(provider.hasTracks());
    const std::vector<ts::KmlBillboard> markers =
        provider.trackMarkersAt(start.addSecs(1800));
    QCOMPARE(markers.size(), std::size_t{1});
    QCOMPARE(markers.front().label, QStringLiteral("Track"));
    QCOMPARE(markers.front().lonDeg, -0.75);
    QCOMPARE(markers.front().altitudeMode,
             ts::KmlAltitudeMode::RelativeToSeaFloor);
    QVERIFY(provider.trackMarkersAt(start.addDays(1)).empty());
    QVERIFY(!ts::KmlFeatureProvider(document, QStringLiteral("0")).hasTracks());
  }

  void regions()
  {
    ts::Camera camera;
    camera.setViewportSize(800.0, 600.0);
    ts::KmlRegion region;
    region.north = 51.6;
    region.south = 51.4;
    region.east = 0.1;
    region.west = -0.3;
    region.minLodPixels = 256.0;
    // From far away it is small; close up large; from the other side of
    // the Earth not in sight at all.
    camera.setPose(ts::CameraPose{51.5, -0.1, 2.0e6, 0.0, 0.0});
    const double far = ts::kmlRegionPixels(region, camera);
    QVERIFY2(far > 0.0 && far < 256.0, qPrintable(QString::number(far)));
    QVERIFY(!ts::kmlRegionActive(region, camera));
    camera.setPose(ts::CameraPose{51.5, -0.1, 40000.0, 0.0, 0.0});
    QVERIFY(ts::kmlRegionPixels(region, camera) > 256.0);
    QVERIFY(ts::kmlRegionActive(region, camera));
    region.maxLodPixels = 300.0;
    QVERIFY(!ts::kmlRegionActive(region, camera));
    camera.setPose(ts::CameraPose{-51.5, 179.0, 2.0e6, 0.0, 0.0});
    QCOMPARE(ts::kmlRegionPixels(region, camera), -1.0);
    // Off to the side of the view: not in sight.
    camera.setPose(ts::CameraPose{51.5, 10.0, 40000.0, 0.0, 0.0});
    QCOMPARE(ts::kmlRegionPixels(region, camera), -1.0);
    // Regions on the way to a feature.
    const ts::KmlDocument document = read(QStringLiteral("links.kml"));
    QVERIFY(ts::kmlHasRegion(document, QStringLiteral("1")));
    QVERIFY(!ts::kmlHasRegion(document, QStringLiteral("0")));
    camera.setPose(ts::CameraPose{51.5, -0.1, 40000.0, 0.0, 0.0});
    QVERIFY(ts::kmlRegionsActive(document, QStringLiteral("1"), camera));
    camera.setPose(ts::CameraPose{51.5, -0.1, 2.0e6, 0.0, 0.0});
    QVERIFY(!ts::kmlRegionsActive(document, QStringLiteral("1"), camera));
    QVERIFY(ts::kmlRegionsActive(document, QStringLiteral("0"), camera));
  }

  void linkAddresses()
  {
    ts::Camera camera;
    camera.setViewportSize(800.0, 600.0);
    camera.setPose(ts::CameraPose{51.5, -0.1, 100000.0, 0.0, 0.0});
    const ts::KmlViewParameters view = ts::kmlViewParameters(camera, false);
    QVERIFY(view.bounds.westDeg < -0.1 && view.bounds.eastDeg > -0.1);
    QVERIFY(view.bounds.southDeg < 51.5 && view.bounds.northDeg > 51.5);
    QCOMPARE(view.widthPx, 800);
    // The whole Earth in sight: all of it.
    camera.setPose(ts::CameraPose{0.0, 0.0, 3.0e7, 0.0, 0.0});
    QCOMPARE(ts::kmlViewParameters(camera, false).bounds.westDeg, -180.0);

    ts::KmlDocument document;
    document.base = QStringLiteral("/data");
    ts::KmlLink link;
    link.href = QStringLiteral("https://example.invalid/live.kml?key=1");
    // No view refresh: as written.
    QCOMPARE(ts::kmlLinkUrl(link, document, view).toString(),
             QStringLiteral("https://example.invalid/live.kml?key=1"));
    // onStop: the view's box by default, scaled.
    link.viewRefreshMode = ts::KmlLink::ViewRefreshMode::OnStop;
    link.viewBoundScale = 0.5;
    const QUrl boxed = ts::kmlLinkUrl(link, document, view);
    const QString query = boxed.query();
    QVERIFY2(query.startsWith(QLatin1String("key=1&BBOX=")), qPrintable(query));
    const QStringList box = QUrlQuery(boxed)
                                .queryItemValue(QStringLiteral("BBOX"))
                                .split(QLatin1Char(','));
    QCOMPARE(box.size(), 4);
    const double halfWidth = (view.bounds.eastDeg - view.bounds.westDeg) / 4.0;
    QVERIFY(std::abs(box.at(0).toDouble() -
                     ((view.bounds.westDeg + view.bounds.eastDeg) / 2.0 -
                      halfWidth)) < 1e-6);
    // viewFormat and httpQuery filled in.
    link.viewFormat =
        QStringLiteral("at=[lookatLat],[lookatLon]&w=[horizPixels]");
    link.httpQuery = QStringLiteral("client=[clientName]&v=[kmlVersion]");
    const QString filled = ts::kmlLinkUrl(link, document, view).query();
    QVERIFY2(filled.contains(QLatin1String("w=800")), qPrintable(filled));
    QVERIFY(filled.contains(QLatin1String("client=TerraSoar&v=2.2")));
    QVERIFY(!filled.contains(QLatin1Char('[')));
    // A relative address: a file next to the document, or on the web
    // next to a document fetched from there.
    link = ts::KmlLink{};
    link.href = QStringLiteral("more.kml");
    QCOMPARE(ts::kmlLinkUrl(link, document, view),
             QUrl::fromLocalFile(QStringLiteral("/data/more.kml")));
    document.webBase = QStringLiteral("https://example.invalid/kml/root.kml");
    QCOMPARE(ts::kmlLinkUrl(link, document, view).toString(),
             QStringLiteral("https://example.invalid/kml/more.kml"));
    QCOMPARE(ts::resolveKmlHref(document, QStringLiteral("../icon.png")),
             QStringLiteral("https://example.invalid/icon.png"));
    // A fetched copy's base is remembered for it.
    ts::setKmlWebBase(fixture(QStringLiteral("links.kml")),
                      QStringLiteral("https://example.invalid/links.kml"));
    QString error;
    const std::shared_ptr<const ts::KmlDocument> copy =
        ts::sharedKmlDocument(fixture(QStringLiteral("links.kml")), &error);
    if (!copy) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(copy->webBase,
             QStringLiteral("https://example.invalid/links.kml"));
  }

  void hiddenChildren()
  {
    // A checkHideChildren folder's layer: everything inside.
    QString error;
    const std::shared_ptr<const ts::KmlDocument> document =
        ts::sharedKmlDocument(fixture(QStringLiteral("heights.kml")), &error);
    const std::shared_ptr<const ts::KmlDocument> links = ts::sharedKmlDocument(
        fixture(QStringLiteral("everything.kml")), &error);
    if (!document || !links) {
      QFAIL(qPrintable(error));
    }
    // everything.kml's root, deep: all placemarks of every folder.
    ts::KmlFeatureProvider shallow(links, QString());
    ts::KmlFeatureProvider deep(links, QString(), std::nullopt, true);
    QVERIFY(deep.featureCount() > shallow.featureCount());
    QCOMPARE(deep.screenOverlays().size(), std::size_t{1});
    QVERIFY(deep.id() != shallow.id());
    QCOMPARE(ts::kmlLayerSource(QStringLiteral("x.kml"), QString(), true)
                 .value(QStringLiteral("deep"))
                 .toBool(),
             true);
  }

  void registryCreatesKml()
  {
    ts::ProviderRegistry registry;
    QString error;
    const QJsonObject source = ts::kmlLayerSource(
        fixture(QStringLiteral("everything.kml")), QStringLiteral("0"));
    const std::shared_ptr<ts::FeatureProvider> provider =
        registry.features(source, &error);
    if (!provider) {
      QFAIL(qPrintable(error));
    }
    QVERIFY(std::dynamic_pointer_cast<ts::KmlFeatureProvider>(provider));
    QCOMPARE(registry.features(source, &error), provider);
    QVERIFY(!registry.features(
        ts::kmlLayerSource(fixture(QStringLiteral("none.kml")), QString()),
        &error));
    QVERIFY(!error.isEmpty());
  }

  void views()
  {
    ts::KmlView lookAt;
    lookAt.latDeg = 52.0;
    lookAt.lonDeg = -1.0;
    lookAt.rangeM = 5000.0;
    lookAt.headingDeg = 30.0;
    lookAt.tiltDeg = 89.0;
    const ts::CameraPose pose = ts::kmlViewPose(lookAt);
    QCOMPARE(pose.targetLatDeg, 52.0);
    QCOMPARE(pose.rangeM, 5000.0);
    QCOMPARE(pose.tiltDeg, 85.0); // kept off the horizon
    // A camera looking straight down: the point below, at its height.
    ts::KmlView camera;
    camera.kind = ts::KmlView::Kind::Camera;
    camera.latDeg = 10.0;
    camera.lonDeg = 20.0;
    camera.altM = 3000.0;
    ts::CameraPose below = ts::kmlViewPose(camera);
    QCOMPARE(below.targetLatDeg, 10.0);
    QCOMPARE(below.rangeM, 3000.0);
    // Tilted 45° facing north: the point 3 km ahead, 4.2 km away.
    camera.tiltDeg = 45.0;
    below = ts::kmlViewPose(camera);
    QVERIFY(std::abs(below.targetLatDeg - (10.0 + 3000.0 / 111320.0)) < 1e-6);
    QVERIFY(std::abs(below.rangeM - 3000.0 * std::sqrt(2.0)) < 1e-6);
    // A feature without a view: framed.
    const ts::KmlDocument document = read(QStringLiteral("everything.kml"));
    const ts::KmlFeature* polygon =
        find(document.root, QStringLiteral("A polygon with a hole"));
    if (polygon == nullptr) {
      QFAIL("no polygon");
    }
    const std::optional<ts::CameraPose> framed = ts::kmlFeaturePose(*polygon);
    QVERIFY(framed.has_value());
    QCOMPARE(framed.value_or(ts::CameraPose{}).targetLatDeg, 52.5);
    QVERIFY(framed.value_or(ts::CameraPose{}).rangeM > 100000.0);
    QVERIFY(!ts::kmlFeaturePose(ts::KmlFeature{}).has_value());
  }

  void balloons()
  {
    const ts::KmlDocument document = read(QStringLiteral("everything.kml"));
    ts::KmlStyleResolver resolver(document);
    const ts::KmlFeature* point =
        find(document.root, QStringLiteral("A point"));
    if (point == nullptr) {
      QFAIL("no point");
    }
    // The BalloonStyle's template.
    QCOMPARE(ts::kmlBalloonHtml(*point, resolver.resolve(*point)),
             QStringLiteral("<b>A point</b>"));
    // The default: name, description, extended data.
    const QString html = ts::kmlBalloonHtml(*point, ts::KmlStyle{});
    QVERIFY(html.contains(QLatin1String("<h3>A point</h3>")));
    QVERIFY(html.contains(QLatin1String("<i>point</i>")));
    QVERIFY(html.contains(QLatin1String("West Midlands")));
    QVERIFY(html.contains(QLatin1String("<th align=\"left\">Population</th>")));
    // Values are escaped; $[field] and $[field/displayName].
    ts::KmlFeature tricky;
    tricky.name = QStringLiteral("<script>");
    tricky.extendedData = {{QStringLiteral("depth"), QStringLiteral("5 & 6")}};
    ts::KmlStyle style;
    ts::KmlBalloonStyle balloon;
    balloon.text = QStringLiteral("$[name]: $[depth/displayName] $[depth]");
    style.balloon = balloon;
    QCOMPARE(ts::kmlBalloonHtml(tricky, style),
             QStringLiteral("&lt;script&gt;: depth 5 &amp; 6"));
  }

  void concurrentOwnInstances()
  {
    const int32_t failures =
        ts::test::runConcurrently(ts::test::concurrentThreadCount, [](int32_t) {
          for (int32_t i = 0; i < 20; ++i) {
            QString error;
            const std::optional<ts::KmlDocument> document = ts::readKmlFile(
                fixture(QStringLiteral("gdal/samples.kml")), &error);
            if (!document || countPlacemarks(document->root) != 20) {
              return false;
            }
          }
          return true;
        });
    QCOMPARE(failures, 0);
  }

  void benchmarkRead()
  {
    QFile file(fixture(QStringLiteral("gdal/samples.kml")));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray xml = file.readAll();
    QBENCHMARK
    {
      QString error;
      QVERIFY(ts::parseKml(xml, QString(), &error).has_value());
    }
  }
};

QTEST_GUILESS_MAIN(TestKml)
#include "tst_kml.moc"
