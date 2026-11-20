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
#include <cstdint>
#include <functional>
#include <optional>

#include <QFile>
#include <QtTest>

#include "concurrency.h"
#include "ts/kml/kmldocument.h"
#include "ts/kml/kmlreader.h"
#include "ts/time/timeextent.h"

namespace
{

QString fixture(const QString& name)
{
  return QStringLiteral(TS_TEST_DATA_DIR "/kml/") + name;
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
