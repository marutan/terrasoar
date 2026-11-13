// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <atomic>
#include <cmath>
#include <memory>

#include <QAbstractItemModelTester>
#include <QColor>
#include <QDir>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeData>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include "concurrency.h"
#include "fixturepack.h"
#include "ts/elevation/compositeelevationprovider.h"
#include "ts/globe/layerstackdraws.h"
#include "ts/layers/layerdefinition.h"
#include "ts/layers/layertreemodel.h"
#include "ts/layers/projectfile.h"
#include "ts/packs/packmanifest.h"
#include "ts/providers/providerregistry.h"
#include "ts/providers/tileprovider.h"
#include "ts/vector/vectorstyle.h"

namespace
{

// A test helper; every call names both strings literally.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
ts::LayerDefinition imagery(const QString& id, const QString& color)
{
  ts::LayerDefinition definition;
  definition.id = id;
  definition.origin = ts::LayerOrigin::User;
  definition.type = ts::LayerType::Imagery;
  definition.name = id;
  QJsonObject source;
  source.insert(QStringLiteral("kind"), QStringLiteral("solid"));
  source.insert(QStringLiteral("color"), color);
  definition.source = source;
  return definition;
}

ts::LayerDefinition vector(const QString& id)
{
  ts::LayerDefinition definition = imagery(id, QStringLiteral("#ff000000"));
  definition.type = ts::LayerType::Vector;
  QJsonObject source;
  source.insert(QStringLiteral("kind"), QStringLiteral("test-vectors"));
  definition.source = source;
  return definition;
}

QString nameAt(const ts::LayerTreeModel& model, int row,
               const QModelIndex& parent = QModelIndex())
{
  return model.data(model.index(row, 0, parent)).toString();
}

QStringList imageryNames(const ts::LayerStack& stack)
{
  QStringList names;
  for (const ts::ImageryStackEntry& entry : stack.imagery) {
    names << entry.definition.name;
  }
  return names;
}

QString dataPath(const char* name)
{
  return QDir(QStringLiteral(TS_TEST_DATA_DIR)).filePath(QLatin1String(name));
}

} // namespace

/** Unit tests for ts/layers: the layer tree, stack, definitions, projects. */
class TestLayers : public QObject
{
  Q_OBJECT

private slots:
  void modelIsConsistent()
  {
    // QAbstractItemModelTester checks every signal and index while the
    // tree is built, edited, moved and reset.
    ts::LayerTreeModel model;
    QAbstractItemModelTester tester(
        &model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    const QModelIndex base = model.addCollection(QStringLiteral("Base"));
    model.addLayer(imagery(QStringLiteral("a"), QStringLiteral("#ff0000")),
                   base);
    model.addLayer(imagery(QStringLiteral("b"), QStringLiteral("#00ff00")),
                   base, 1);
    const QModelIndex overlays =
        model.addCollection(QStringLiteral("Overlays"));
    model.addLayer(imagery(QStringLiteral("c"), QStringLiteral("#0000ff")),
                   overlays);
    model.setData(overlays, 0.5, ts::LayerTreeModel::OpacityRole);
    model.setData(overlays, Qt::Unchecked, Qt::CheckStateRole);
    QVERIFY(model.moveNode(model.index(0, 0, base), overlays, 1));
    QVERIFY(model.removeNode(model.index(0, 0)));
    const QJsonArray json = model.toJson();
    QString error;
    QVERIFY(model.fromJson(json, &error));
    model.clear();
    QCOMPARE(model.rowCount(), 0);
  }

  void insertOrderAndRoles()
  {
    ts::LayerTreeModel model;
    model.addLayer(
        imagery(QStringLiteral("bottom"), QStringLiteral("#ff0000")));
    model.addLayer(imagery(QStringLiteral("top"), QStringLiteral("#00ff00")));
    // Row 0 is the top of the drawing order.
    QCOMPARE(nameAt(model, 0), QStringLiteral("top"));
    QCOMPARE(nameAt(model, 1), QStringLiteral("bottom"));
    const QModelIndex top = model.index(0, 0);
    QCOMPARE(model.data(top, Qt::CheckStateRole).toInt(),
             static_cast<int>(Qt::Checked));
    QCOMPARE(model.data(top, ts::LayerTreeModel::LayerTypeRole).toInt(),
             static_cast<int>(ts::LayerType::Imagery));
    QVERIFY(!model.data(top, ts::LayerTreeModel::IsCollectionRole).toBool());
    QVERIFY(model.setData(top, QStringLiteral("renamed")));
    QCOMPARE(nameAt(model, 0), QStringLiteral("renamed"));
    QVERIFY(model.setData(top, 1.7, ts::LayerTreeModel::OpacityRole));
    QCOMPARE(model.data(top, ts::LayerTreeModel::OpacityRole).toDouble(), 1.0);
    // Only collections can composite.
    QVERIFY(
        !model.setData(top, true, ts::LayerTreeModel::CompositeAsGroupRole));
    QVERIFY(model.flags(QModelIndex()).testFlag(Qt::ItemIsDropEnabled));
    QVERIFY(!model.flags(top).testFlag(Qt::ItemIsDropEnabled));
    QVERIFY(model.flags(top).testFlag(Qt::ItemIsUserCheckable));
  }

  void effectiveOpacityAndVisibility()
  {
    // Effective opacity is the product of the ancestors'; visibility needs
    // every ancestor visible.
    ts::LayerTreeModel model;
    const QModelIndex outer = model.addCollection(QStringLiteral("outer"));
    const QModelIndex inner =
        model.addCollection(QStringLiteral("inner"), outer);
    const QModelIndex layer = model.addLayer(
        imagery(QStringLiteral("x"), QStringLiteral("#ff0000")), inner);
    model.setData(outer, 0.5, ts::LayerTreeModel::OpacityRole);
    model.setData(inner, 0.8, ts::LayerTreeModel::OpacityRole);
    model.setData(layer, 0.5, ts::LayerTreeModel::OpacityRole);
    QCOMPARE(
        model.data(layer, ts::LayerTreeModel::EffectiveOpacityRole).toDouble(),
        0.2);
    QVERIFY(
        model.data(layer, ts::LayerTreeModel::EffectivelyVisibleRole).toBool());
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    model.setData(outer, Qt::Unchecked, Qt::CheckStateRole);
    QVERIFY(!model.data(layer, ts::LayerTreeModel::EffectivelyVisibleRole)
                 .toBool());
    // The descendants are reported changed too (their effective values).
    QVERIFY(changed.count() >= 3);
    QVERIFY(model.layerStack().imagery.empty());
  }

  void moveBetweenCollections()
  {
    ts::LayerTreeModel model;
    const QModelIndex a = model.addCollection(QStringLiteral("A"));
    const QModelIndex b = model.addCollection(QStringLiteral("B"), {}, 1);
    model.addLayer(imagery(QStringLiteral("one"), QStringLiteral("#ff0000")),
                   a);
    model.addLayer(imagery(QStringLiteral("two"), QStringLiteral("#00ff00")), a,
                   1);
    // Move "one" into B, then back to the bottom of A.
    QVERIFY(model.moveNode(model.index(0, 0, a), b, 0));
    QCOMPARE(model.rowCount(a), 1);
    QCOMPARE(nameAt(model, 0, b), QStringLiteral("one"));
    QVERIFY(model.moveNode(model.index(0, 0, b), a, 5));
    QCOMPARE(nameAt(model, 1, a), QStringLiteral("one"));
    // Within one collection, down and up.
    QVERIFY(model.moveNode(model.index(0, 0, a), a, 1));
    QCOMPARE(nameAt(model, 0, a), QStringLiteral("one"));
    // A collection can't go into itself or below itself.
    QVERIFY(!model.moveNode(a, a, 0));
    const QModelIndex nested = model.addCollection(QStringLiteral("C"), a);
    QVERIFY(!model.moveNode(a, nested, 0));
    // Nor can anything go into a layer.
    QVERIFY(!model.moveNode(nested, model.index(1, 0, a), 0));
  }

  void dragAndDrop()
  {
    ts::LayerTreeModel model;
    QAbstractItemModelTester tester(
        &model, QAbstractItemModelTester::FailureReportingMode::QtTest);
    const QModelIndex group = model.addCollection(QStringLiteral("group"));
    model.addLayer(imagery(QStringLiteral("x"), QStringLiteral("#ff0000")), {},
                   1);
    model.addLayer(imagery(QStringLiteral("y"), QStringLiteral("#00ff00")), {},
                   2);
    // Drag "y" (row 2) onto the collection: it goes to the collection's top.
    std::unique_ptr<QMimeData> data(model.mimeData({model.index(2, 0)}));
    QVERIFY(data->hasFormat(QLatin1String(ts::LayerTreeModel::mimeType)));
    // Returns false so the view doesn't delete the source rows itself.
    QVERIFY(!model.dropMimeData(data.get(), Qt::MoveAction, -1, 0, group));
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(nameAt(model, 0, model.index(0, 0)), QStringLiteral("y"));
    // Drag "x" (row 1) to the top of the root, between rows.
    data.reset(model.mimeData({model.index(1, 0)}));
    model.dropMimeData(data.get(), Qt::MoveAction, 0, 0, QModelIndex());
    QCOMPARE(nameAt(model, 0), QStringLiteral("x"));
    // A copy drop is refused.
    QVERIFY(!model.dropMimeData(data.get(), Qt::CopyAction, 0, 0, {}));
    QCOMPARE(model.supportedDropActions(), Qt::MoveAction);
  }

  void stackOrderAndGroups()
  {
    ts::LayerTreeModel model;
    // Tree (top first): vec, overlay group {top, middle}, bottom.
    model.addLayer(
        imagery(QStringLiteral("bottom"), QStringLiteral("#ff0000")));
    const QModelIndex group = model.addCollection(QStringLiteral("overlay"));
    model.addLayer(imagery(QStringLiteral("middle"), QStringLiteral("#00ff00")),
                   group);
    model.addLayer(imagery(QStringLiteral("top"), QStringLiteral("#0000ff")),
                   group);
    model.addLayer(vector(QStringLiteral("vec")));
    model.setData(group, 0.5, ts::LayerTreeModel::OpacityRole);
    model.setData(model.index(0, 0, group), 0.8,
                  ts::LayerTreeModel::OpacityRole);

    // Multiply (default): bottom to top, alphas multiplied, no groups.
    ts::LayerStack stack = model.layerStack();
    QCOMPARE(imageryNames(stack),
             (QStringList{QStringLiteral("bottom"), QStringLiteral("middle"),
                          QStringLiteral("top")}));
    QVERIFY(stack.groups.empty());
    QCOMPARE(stack.imagery[2].alpha, 0.4);
    QCOMPARE(stack.imagery[1].alpha, 0.5);

    // Composite: members at their own alpha, the group fades.
    model.setData(group, true, ts::LayerTreeModel::CompositeAsGroupRole);
    stack = model.layerStack();
    QCOMPARE(static_cast<int>(stack.groups.size()), 1);
    QCOMPARE(stack.groups[0].alpha, 0.5);
    QCOMPARE(stack.imagery[0].group, -1);
    QCOMPARE(stack.imagery[1].group, 0);
    QCOMPARE(stack.imagery[2].group, 0);
    QCOMPARE(stack.imagery[2].alpha, 0.8);
    QCOMPARE(stack.imagery[1].alpha, 1.0);

    // Vectors come after all imagery whatever the tree order: move the
    // vector layer to the very bottom of the tree.
    QVERIFY(model.moveNode(model.index(0, 0), QModelIndex(), 2));
    QCOMPARE(nameAt(model, 2), QStringLiteral("vec"));
    stack = model.layerStack();
    QCOMPARE(static_cast<int>(stack.vectors.size()), 1);
    QCOMPARE(stack.vectors[0].definition.name, QStringLiteral("vec"));
    QCOMPARE(static_cast<int>(stack.imagery.size()), 3);
  }

  void nestedGroupsMultiply()
  {
    // Only the outermost composite group composites; inside it, nested
    // groups multiply. An empty group is dropped.
    ts::LayerTreeModel model;
    const QModelIndex outer = model.addCollection(QStringLiteral("outer"));
    const QModelIndex inner =
        model.addCollection(QStringLiteral("inner"), outer);
    model.addLayer(imagery(QStringLiteral("x"), QStringLiteral("#ff0000")),
                   inner);
    model.addCollection(QStringLiteral("empty"));
    model.setData(outer, true, ts::LayerTreeModel::CompositeAsGroupRole);
    model.setData(inner, true, ts::LayerTreeModel::CompositeAsGroupRole);
    model.setData(inner, 0.5, ts::LayerTreeModel::OpacityRole);
    model.setData(model.index(0, 0), true,
                  ts::LayerTreeModel::CompositeAsGroupRole);
    const ts::LayerStack stack = model.layerStack();
    QCOMPARE(static_cast<int>(stack.groups.size()), 1);
    QCOMPARE(stack.imagery[0].alpha, 0.5);
    QCOMPARE(stack.imagery[0].group, 0);
  }

  void stackChangesAreSignalled()
  {
    ts::LayerTreeModel model;
    QSignalSpy spy(&model, &ts::LayerTreeModel::layerStackChanged);
    const QModelIndex layer =
        model.addLayer(imagery(QStringLiteral("x"), QStringLiteral("#ff0000")));
    model.setData(layer, 0.3, ts::LayerTreeModel::OpacityRole);
    model.setData(layer, Qt::Unchecked, Qt::CheckStateRole);
    model.addCollection(QStringLiteral("c"));
    model.moveNode(model.index(1, 0), QModelIndex(), 0);
    model.removeNode(model.index(0, 0));
    QVERIFY(spy.count() >= 6);
  }

  // --- JSON and projects
  // --------------------------------------------------------

  void jsonRoundTrip()
  {
    ts::LayerTreeModel model;
    const QModelIndex group = model.addCollection(QStringLiteral("group"));
    model.setData(group, true, ts::LayerTreeModel::CompositeAsGroupRole);
    model.setData(group, 0.25, ts::LayerTreeModel::OpacityRole);
    model.addLayer(imagery(QStringLiteral("x"), QStringLiteral("#ff0000")),
                   group);
    model.addLayer(vector(QStringLiteral("v")));
    model.setData(model.index(0, 0), Qt::Unchecked, Qt::CheckStateRole);
    const QJsonArray json = model.toJson();
    ts::LayerTreeModel copy;
    QString error;
    QVERIFY2(copy.fromJson(json, &error), qPrintable(error));
    QCOMPARE(copy.toJson(), json);
    // Ids survive, so references to entries stay valid.
    QVERIFY(
        copy.indexOfId(
                model.data(group, ts::LayerTreeModel::NodeIdRole).toString())
            .isValid());
  }

  void badJsonIsRefused()
  {
    ts::LayerTreeModel model;
    model.addLayer(imagery(QStringLiteral("keep"), QStringLiteral("#ff0000")));
    const QJsonArray before = model.toJson();
    QJsonObject unknown;
    unknown.insert(QStringLiteral("kind"), QStringLiteral("spaceship"));
    QString error;
    QVERIFY(!model.fromJson(QJsonArray{unknown}, &error));
    QVERIFY(!error.isEmpty());
    QJsonObject noSource;
    noSource.insert(QStringLiteral("kind"), QStringLiteral("layer"));
    noSource.insert(
        QStringLiteral("definition"),
        QJsonObject{{QStringLiteral("id"), QStringLiteral("x")},
                    {QStringLiteral("type"), QStringLiteral("imagery")},
                    {QStringLiteral("origin"), QStringLiteral("user")}});
    QVERIFY(!model.fromJson(QJsonArray{noSource}, &error));
    QCOMPARE(model.toJson(), before);
  }

  void layerStatus()
  {
    // A layer's source status shows as an icon and tooltip; it is neither
    // saved nor a change of the stack.
    ts::LayerTreeModel model;
    // Icons named after files that don't exist: enough to tell them
    // apart (no images without a GUI application).
    const QIcon offlineIcon(QStringLiteral(":/test/offline.png"));
    const QIcon errorIcon(QStringLiteral(":/test/error.png"));
    model.setStatusIcons(offlineIcon, errorIcon);
    const QModelIndex layer = model.addLayer(
        imagery(QStringLiteral("w"), QStringLiteral("#ff0000ff")));
    const QString id =
        model.data(layer, ts::LayerTreeModel::NodeIdRole).toString();
    QCOMPARE(model.data(layer, ts::LayerTreeModel::StatusRole).toInt(),
             static_cast<int>(ts::ProviderStatus::State::Ok));
    QVERIFY(model.data(layer, Qt::DecorationRole).isNull());
    QVERIFY(model.data(layer, Qt::ToolTipRole).isNull());
    const QJsonArray before = model.toJson();
    QSignalSpy changed(&model, &QAbstractItemModel::dataChanged);
    QSignalSpy stack(&model, &ts::LayerTreeModel::layerStackChanged);
    model.setLayerStatus(id,
                         ts::ProviderStatus{ts::ProviderStatus::State::Offline,
                                            QStringLiteral("Can't reach it")});
    QCOMPARE(changed.size(), 1);
    QCOMPARE(model.data(layer, ts::LayerTreeModel::StatusRole).toInt(),
             static_cast<int>(ts::ProviderStatus::State::Offline));
    QCOMPARE(model.data(layer, Qt::ToolTipRole).toString(),
             QStringLiteral("Can't reach it"));
    QCOMPARE(
        model.data(layer, ts::LayerTreeModel::StatusMessageRole).toString(),
        QStringLiteral("Can't reach it"));
    QCOMPARE(model.data(layer, Qt::DecorationRole).value<QIcon>().cacheKey(),
             offlineIcon.cacheKey());
    // The same again changes nothing.
    model.setLayerStatus(id,
                         ts::ProviderStatus{ts::ProviderStatus::State::Offline,
                                            QStringLiteral("Can't reach it")});
    QCOMPARE(changed.size(), 1);
    model.setLayerStatus(id,
                         ts::ProviderStatus{ts::ProviderStatus::State::Error,
                                            QStringLiteral("Bad")});
    QCOMPARE(model.data(layer, Qt::DecorationRole).value<QIcon>().cacheKey(),
             errorIcon.cacheKey());
    // Ok again: no icon, no tooltip.
    model.setLayerStatus(id, ts::ProviderStatus{});
    QCOMPARE(changed.size(), 3);
    QVERIFY(model.data(layer, Qt::DecorationRole).isNull());
    QCOMPARE(stack.size(), 0);
    QCOMPARE(model.toJson(), before);
    // Unknown ids are ignored.
    model.setLayerStatus(
        QStringLiteral("nothing"),
        ts::ProviderStatus{ts::ProviderStatus::State::Error, {}});
    QCOMPARE(changed.size(), 3);
  }

  void projectRoundTripIsIdentical()
  {
    ts::LayerTreeModel model;
    const QModelIndex group = model.addCollection(QStringLiteral("Overlays"));
    model.addLayer(imagery(QStringLiteral("x"), QStringLiteral("#80ff0000")),
                   group);
    model.addLayer(ts::builtinLayerDefinitions().front());
    ts::Project project;
    project.camera = ts::CameraPose{51.5, -0.1, 12345.5, 30.0, 45.0};
    project.mapView = true;
    project.projection = QStringLiteral("miller");
    project.layers = model.toJson();
    QTemporaryDir directory;
    const QString path = directory.filePath(QStringLiteral("p.terrasoar"));
    QString error;
    QVERIFY2(ts::saveProject(path, project, &error), qPrintable(error));
    const std::optional<ts::Project> loaded = ts::loadProject(path, &error);
    if (!loaded) {
      QFAIL(qPrintable(error));
    }
    // Save → load → save gives the same bytes.
    QCOMPARE(ts::projectToJson(*loaded), ts::projectToJson(project));
    ts::LayerTreeModel reloaded;
    QVERIFY(reloaded.fromJson(loaded->layers, &error));
    QCOMPARE(reloaded.toJson(), model.toJson());
    QCOMPARE(loaded->camera.rangeM, 12345.5);
    QVERIFY(loaded->mapView);
    QCOMPARE(loaded->projection, QStringLiteral("miller"));
    // A new project is the globe, with Mercator ready for the map.
    QVERIFY(!ts::Project().mapView);
    QCOMPARE(ts::Project().projection, QStringLiteral("mercator"));
    QVERIFY(ts::projectToJson(ts::Project()).contains("\"mode\": \"globe\""));
  }

  void projectVersions()
  {
    // Every earlier format version keeps loading: each has a fixture.
    QString error;
    const std::optional<ts::Project> v1 =
        ts::loadProject(dataPath("projects/v1-basic.terrasoar"), &error);
    if (!v1) {
      QFAIL(qPrintable(error));
    }
    ts::LayerTreeModel model;
    QVERIFY2(model.fromJson(v1->layers, &error), qPrintable(error));
    QCOMPARE(model.rowCount(), 2);
    QCOMPARE(v1->camera.targetLatDeg, 48.0);
    QVERIFY(!model.layerStack().imagery.empty());
    // Version 1 had no view: it was always the globe.
    QVERIFY(!v1->mapView);
    QCOMPARE(v1->projection, QStringLiteral("mercator"));
    // Version 2 added the view (globe or map) and the map's projection.
    QCOMPARE(ts::projectFormatVersion, 2);
    const std::optional<ts::Project> v2 =
        ts::loadProject(dataPath("projects/v2-map.terrasoar"), &error);
    if (!v2) {
      QFAIL(qPrintable(error));
    }
    QVERIFY(v2->mapView);
    QCOMPARE(v2->projection, QStringLiteral("miller"));
    QCOMPARE(v2->camera.targetLatDeg, 48.0);
    QVERIFY2(model.fromJson(v2->layers, &error), qPrintable(error));
    QCOMPARE(model.rowCount(), 2);
    // A view that is missing or damaged falls back to the globe.
    const std::optional<ts::Project> noView = ts::projectFromJson(
        "{\"format\": \"terrasoar-project\", \"version\": 2, "
        "\"view\": 7}",
        &error);
    if (!noView) {
      QFAIL(qPrintable(error));
    }
    QVERIFY(!noView->mapView);
    QCOMPARE(noView->projection, QStringLiteral("mercator"));
    // A newer format, or something that isn't a project, is refused.
    QJsonObject newer;
    newer.insert(QStringLiteral("format"), QStringLiteral("terrasoar-project"));
    newer.insert(QStringLiteral("version"), ts::projectFormatVersion + 1);
    QVERIFY(!ts::projectFromJson(QJsonDocument(newer).toJson(), &error));
    QVERIFY(error.contains(QStringLiteral("newer")));
    QVERIFY(!ts::projectFromJson("{\"format\": \"something else\"}", &error));
    QVERIFY(!ts::projectFromJson("[1, 2", &error));
    QVERIFY(!ts::loadProject(dataPath("projects/missing.terrasoar"), &error));
  }

  // --- Definitions and providers
  // -----------------------------------------------

  void definitions()
  {
    const std::vector<ts::LayerDefinition> builtin =
        ts::builtinLayerDefinitions();
    QVERIFY(builtin.size() >= 2);
    for (const ts::LayerDefinition& definition : builtin) {
      QCOMPARE(definition.origin, ts::LayerOrigin::Builtin);
      QString error;
      const std::optional<ts::LayerDefinition> back =
          ts::LayerDefinition::fromJson(definition.toJson(), &error);
      QVERIFY2(back.has_value(), qPrintable(error));
      QCOMPARE(back.value_or(ts::LayerDefinition()), definition);
      const QString kind =
          definition.source.value(QStringLiteral("kind")).toString();
      ts::ProviderRegistry registry;
      if (definition.type == ts::LayerType::Vector) {
        // Vector layers have a style; a pack source needs the pack.
        QVERIFY(!definition.style.isEmpty());
        QVERIFY(ts::VectorStyle::fromJson(definition.style).hasOutline() ||
                ts::VectorStyle::fromJson(definition.style).hasFill());
        if (kind == QLatin1String("basepack")) {
          QVERIFY(!registry.features(definition.source, &error));
          QVERIFY(error.contains(QLatin1String("base pack")));
        }
      } else if (kind == QLatin1String("basepack")) {
        QVERIFY(!registry.imagery(definition.source, &error));
        QVERIFY(error.contains(QLatin1String("base pack")));
      } else {
        QVERIFY(ts::createImageryProvider(definition.source, &error));
      }
    }
    QString error;
    QVERIFY(!ts::createImageryProvider(
        QJsonObject{{QStringLiteral("kind"), QStringLiteral("nope")}}, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(ts::layerTypeName(ts::LayerType::Vector),
             QStringLiteral("vector"));
    QCOMPARE(ts::layerOriginName(ts::LayerOrigin::Catalogue),
             QStringLiteral("catalogue"));
  }

  void vectorStyles()
  {
    // #rrggbbaa with alpha last; missing members keep their defaults.
    const ts::VectorStyle style = ts::VectorStyle::fromJson(
        QJsonObject{{QStringLiteral("fill"), QStringLiteral("#10203040")},
                    {QStringLiteral("outline"), QStringLiteral("#ff8000")},
                    {QStringLiteral("outlineWidthPx"), 2.5}});
    QCOMPARE(style.fill, QColor(0x10, 0x20, 0x30, 0x40));
    QCOMPARE(style.outline, QColor(255, 128, 0));
    QCOMPARE(style.outlineWidthPx, 2.5);
    QCOMPARE(ts::VectorStyle::fromJson(style.toJson()).fill, style.fill);
    QCOMPARE(ts::colorToRgbaHex(style.fill), QStringLiteral("#10203040"));
    QVERIFY(!ts::colorFromRgbaHex(QStringLiteral("red")).isValid());
    QVERIFY(!ts::colorFromRgbaHex(QStringLiteral("#12345")).isValid());
    const ts::VectorStyle empty = ts::VectorStyle::fromJson(QJsonObject());
    QVERIFY(!empty.hasFill() && !empty.hasOutline());
    const std::array<float, 4> rgba =
        ts::premultipliedRgba(QColor(255, 0, 0, 128), 0.5);
    QVERIFY(std::abs(rgba[0] - 0.251F) < 0.002F);
    QVERIFY(std::abs(rgba[3] - 0.251F) < 0.002F);

    // A definition keeps its style through JSON (project files).
    ts::LayerDefinition definition;
    definition.id = QStringLiteral("user:roads");
    definition.type = ts::LayerType::Vector;
    definition.source =
        QJsonObject{{QStringLiteral("kind"), QStringLiteral("basepack")}};
    definition.style = style.toJson();
    QString error;
    const std::optional<ts::LayerDefinition> back =
        ts::LayerDefinition::fromJson(definition.toJson(), &error);
    QVERIFY(back.has_value());
    QCOMPARE(back.value_or(ts::LayerDefinition()), definition);
  }

  void vectorDraws()
  {
    // The stack's vector layers become graticule and feature draws, in
    // order, with their styles and opacities; attributions follow what is
    // drawn, top first.
    const QTemporaryDir dir;
    const ts::PackManifest manifest =
        ts::test::writeFixtureBasePack(dir.path());
    ts::ProviderRegistry registry;
    registry.setBasePack(dir.path(), manifest);
    ts::LayerTreeModel model;
    // Top to bottom: graticule, shoreline, Blue Marble.
    for (const QString& id : {QStringLiteral("builtin:graticule"),
                              QStringLiteral("builtin:shoreline"),
                              QStringLiteral("builtin:bluemarble")}) {
      for (const ts::LayerDefinition& definition :
           ts::builtinLayerDefinitions()) {
        if (definition.id == id) {
          model.addLayer(definition, QModelIndex(), model.rowCount());
        }
      }
    }
    model.setData(model.index(1, 0), 0.5, ts::LayerTreeModel::OpacityRole);
    ts::LayerStack stack = model.layerStack();
    QStringList errors;
    const std::vector<ts::VectorLayerDraw> draws =
        ts::vectorDrawsForStack(stack, registry, &errors);
    QVERIFY(errors.isEmpty());
    QCOMPARE(draws.size(), std::size_t{2});
    // Bottom first: the shoreline, then the graticule.
    QVERIFY(draws[0].kind == ts::VectorLayerDraw::Kind::Features);
    QVERIFY(draws[0].provider != nullptr);
    QCOMPARE(draws[0].alpha, 0.5);
    QVERIFY(draws[0].style.hasOutline());
    QVERIFY(draws[1].kind == ts::VectorLayerDraw::Kind::Graticule);
    QCOMPARE(ts::attributionsForStack(stack, registry),
             (QStringList{QStringLiteral("Fixture shoreline"),
                          QStringLiteral("Fixture imagery")}));
    // The same source twice shares one feature provider.
    QCOMPARE(registry.features(stack.vectors[0].definition.source, nullptr),
             draws[0].provider);
    // Without the pack the feature layer is left out, with a reason.
    ts::ProviderRegistry empty;
    errors.clear();
    QCOMPARE(ts::vectorDrawsForStack(stack, empty, &errors).size(),
             std::size_t{1});
    QCOMPARE(errors.size(), 1);
    QVERIFY(ts::attributionsForStack(stack, empty).isEmpty());
  }

  void registryBasePack()
  {
    // Base pack sources resolve through the pack the registry was given.
    const QTemporaryDir dir;
    const ts::PackManifest manifest =
        ts::test::writeFixtureBasePack(dir.path(), QColor(1, 2, 3));
    const QJsonObject source{
        {QStringLiteral("kind"), QStringLiteral("basepack")},
        {QStringLiteral("dataset"), QStringLiteral("bluemarble")}};
    ts::ProviderRegistry registry;
    QString error;
    QVERIFY(!registry.imagery(source, &error));
    registry.setBasePack(dir.path(), manifest);
    QCOMPARE(registry.basePackDirectory(), dir.path());
    const std::shared_ptr<ts::ImageryProvider> provider =
        registry.imagery(source, &error);
    QVERIFY2(provider, qPrintable(error));
    QCOMPARE(provider->maximumLevel(), 1);
    const std::atomic<bool> notCancelled{false};
    QCOMPARE(QColor(provider->loadImage(ts::TileKey{1, 2, 1}, notCancelled)
                        .pixel(3, 3)),
             QColor(1, 2, 3));
    QCOMPARE(registry.imagery(source, &error), provider);
    // An unknown dataset, and no pack at all.
    QJsonObject other = source;
    other.insert(QStringLiteral("dataset"), QStringLiteral("nothing"));
    QVERIFY(!registry.imagery(other, &error));
    registry.setBasePack(QString(), ts::PackManifest());
    QVERIFY(!registry.imagery(source, &error));
  }

  void registrySharesProviders()
  {
    // Two entries for the same source get one provider (and so one loader
    // and cache in the renderer); a different source gets another.
    ts::ProviderRegistry registry;
    const ts::LayerDefinition a =
        imagery(QStringLiteral("a"), QStringLiteral("#ff0000"));
    const ts::LayerDefinition b =
        imagery(QStringLiteral("b"), QStringLiteral("#ff0000"));
    const ts::LayerDefinition c =
        imagery(QStringLiteral("c"), QStringLiteral("#00ff00"));
    QString error;
    std::shared_ptr<ts::ImageryProvider> first =
        registry.imagery(a.source, &error);
    std::shared_ptr<ts::ImageryProvider> second =
        registry.imagery(b.source, &error);
    std::shared_ptr<ts::ImageryProvider> third =
        registry.imagery(c.source, &error);
    QVERIFY(first && second && third);
    QCOMPARE(first.get(), second.get());
    QVERIFY(first.get() != third.get());
    QCOMPARE(registry.liveImageryProviders(), 2);
    first.reset();
    second.reset();
    QCOMPARE(registry.liveImageryProviders(), 1);
  }

  void concurrentSharedRegistry()
  {
    // Thread-safe: many threads asking for the same sources get one
    // provider per source.
    ts::ProviderRegistry registry;
    const QJsonObject source =
        imagery(QStringLiteral("a"), QStringLiteral("#ff0000")).source;
    std::shared_ptr<ts::ImageryProvider> expected =
        registry.imagery(source, nullptr);
    const int failures = ts::test::runConcurrently(
        ts::test::concurrentThreadCount, [&registry, &source, &expected](int) {
          for (int i = 0; i < 200; ++i) {
            if (registry.imagery(source, nullptr) != expected) {
              return false;
            }
          }
          return true;
        });
    QCOMPARE(failures, 0);
  }

  void elevationInTheStack()
  {
    // Elevation layers are in the stack hidden or not; the topmost visible
    // one is the terrain, else the topmost keeps its heights loaded.
    ts::LayerTreeModel model;
    ts::LayerDefinition terrain;
    for (const ts::LayerDefinition& definition :
         ts::builtinLayerDefinitions()) {
      if (definition.id == QLatin1String("builtin:terrain")) {
        terrain = definition;
      }
    }
    QCOMPARE(terrain.type, ts::LayerType::Elevation);
    QCOMPARE(ts::terrainExaggeration(terrain), 1.0);
    const QModelIndex lower = model.addLayer(terrain);
    ts::LayerDefinition steep = terrain;
    steep.id = QStringLiteral("user:steep");
    steep.style.insert(QStringLiteral("exaggeration"), 3.0);
    const QModelIndex collection =
        model.addCollection(QStringLiteral("Hidden"));
    model.addLayer(steep, collection);
    model.setData(collection, Qt::Unchecked, Qt::CheckStateRole);

    ts::LayerStack stack = model.layerStack();
    QCOMPARE(stack.elevation.size(), std::size_t{2});
    QVERIFY(stack.elevation[0].visible);  // the lower, checked
    QVERIFY(!stack.elevation[1].visible); // in a hidden collection
    QCOMPARE(stack.elevation[1].definition.id, QStringLiteral("user:steep"));
    QVERIFY(stack.imagery.empty() && stack.vectors.empty());

    // Without a base pack nothing can be opened: an error only for the
    // visible one.
    ts::ProviderRegistry registry;
    QStringList errors;
    ts::ElevationDraw draw =
        ts::elevationDrawForStack(stack, registry, &errors);
    QVERIFY(!draw.provider);
    QCOMPARE(errors.size(), 1);
    model.setData(lower, Qt::Unchecked, Qt::CheckStateRole);
    stack = model.layerStack();
    QVERIFY(!stack.elevation[0].visible);
    errors.clear();
    draw = ts::elevationDrawForStack(stack, registry, &errors);
    QVERIFY(errors.isEmpty());
    QVERIFY(!draw.visible);

    // Exaggeration from the style, held to 0.1–20.
    QCOMPARE(ts::terrainExaggeration(steep), 3.0);
    steep.style.insert(QStringLiteral("exaggeration"), 500.0);
    QCOMPARE(ts::terrainExaggeration(steep), 20.0);
    steep.style.insert(QStringLiteral("exaggeration"), QStringLiteral("x"));
    QCOMPARE(ts::terrainExaggeration(steep), 1.0);

    // The map's hillshade: on unless the style says not.
    QVERIFY(ts::terrainHillshade(steep));
    steep.style.insert(QStringLiteral("hillshade"), false);
    QVERIFY(!ts::terrainHillshade(steep));
  }

  void elevationSourcesInPriority()
  {
    // Visible elevation layers combine, the upper over the lower; the
    // exaggeration is the topmost one's.
    const QTemporaryDir dir;
    const ts::PackManifest manifest =
        ts::test::writeFixtureBasePack(dir.path());
    const QString dem = dir.filePath(QStringLiteral("dem.tif"));
    QVERIFY(ts::test::writeFixtureDem(dem, 1500.0F));
    ts::ProviderRegistry registry;
    registry.setBasePack(dir.path(), manifest);

    ts::LayerTreeModel model;
    ts::LayerDefinition terrain;
    for (const ts::LayerDefinition& definition :
         ts::builtinLayerDefinitions()) {
      if (definition.id == QLatin1String("builtin:terrain")) {
        terrain = definition;
      }
    }
    model.addLayer(terrain);
    ts::LayerDefinition file;
    file.id = QStringLiteral("user:dem");
    file.type = ts::LayerType::Elevation;
    file.name = QStringLiteral("dem");
    file.source = QJsonObject{
        {QStringLiteral("kind"), QStringLiteral("file")},
        {QStringLiteral("path"), dem},
        {QStringLiteral("verticalDatum"), QStringLiteral("ellipsoid")}};
    file.style = QJsonObject{{QStringLiteral("exaggeration"), 2.0}};
    const QModelIndex upper = model.addLayer(file, QModelIndex(), 0);

    QStringList errors;
    ts::ElevationDraw draw =
        ts::elevationDrawForStack(model.layerStack(), registry, &errors);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char(' '))));
    QVERIFY(draw.visible);
    QCOMPARE(draw.exaggeration, 2.0);
    QVERIFY(draw.hillshade);
    const ts::CompositeElevationProvider* composite =
        dynamic_cast<const ts::CompositeElevationProvider*>(
            draw.provider.get());
    if (composite == nullptr) {
      QFAIL("not a composite");
    }
    QCOMPARE(composite->sources().size(), std::size_t{2});
    QVERIFY(composite->sources()[0]->id().startsWith(
        QStringLiteral("gdalheights:")));
    // The same layers, the same provider (the renderer keeps its heights).
    QCOMPARE(ts::elevationDrawForStack(model.layerStack(), registry, &errors)
                 .provider,
             draw.provider);
    // The DEM's heights where it has them, the base pack's elsewhere.
    const std::atomic<bool> notCancelled{false};
    const std::optional<ts::HeightTile> inside = draw.provider->loadHeights(
        ts::TileKey::containing(6, ts::Geodetic{20.0, 10.0, 0.0}),
        notCancelled);
    const std::optional<ts::HeightTile> outside = draw.provider->loadHeights(
        ts::TileKey::containing(6, ts::Geodetic{-40.0, 100.0, 0.0}),
        notCancelled);
    if (!inside || !outside) {
      QFAIL("no heights");
    }
    QCOMPARE(inside->heightsM[inside->heightsM.size() / 2], 1500.0F);
    QCOMPARE(outside->heightsM[outside->heightsM.size() / 2], 500.0F);

    // Hiding the DEM leaves the base pack's terrain alone, at 1×.
    model.setData(upper, Qt::Unchecked, Qt::CheckStateRole);
    draw = ts::elevationDrawForStack(model.layerStack(), registry, &errors);
    composite = dynamic_cast<const ts::CompositeElevationProvider*>(
        draw.provider.get());
    QVERIFY(composite != nullptr && composite->sources().size() == 1);
    QCOMPARE(draw.exaggeration, 1.0);

    // A DEM that can't be opened is said, and left out.
    model.setData(upper, Qt::Checked, Qt::CheckStateRole);
    file.source.insert(QStringLiteral("path"),
                       dir.filePath(QStringLiteral("gone.tif")));
    model.setLayerDefinition(upper, file);
    errors.clear();
    draw = ts::elevationDrawForStack(model.layerStack(), registry, &errors);
    QCOMPARE(errors.size(), 1);
    QVERIFY(draw.provider && draw.visible);
  }

  void benchmarkLayerStack()
  {
    ts::LayerTreeModel model;
    for (int i = 0; i < 10; ++i) {
      const QModelIndex group =
          model.addCollection(QStringLiteral("group %1").arg(i));
      for (int j = 0; j < 10; ++j) {
        model.addLayer(imagery(QStringLiteral("l%1-%2").arg(i).arg(j),
                               QStringLiteral("#ff0000")),
                       group);
      }
    }
    QBENCHMARK
    {
      model.layerStack();
    }
  }
};

QTEST_GUILESS_MAIN(TestLayers)
#include "tst_layers.moc"
