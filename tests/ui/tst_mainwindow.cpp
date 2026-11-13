// SPDX-License-Identifier: GPL-2.0-or-later
#include <functional>

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFontMetrics>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QNetworkProxy>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrlQuery>
#include <QWheelEvent>
#include <QtTest>

#include "addwebterraindialog.h"
#include "addwmsdialog.h"
#include "fixturepack.h"
#include "globeview.h"
#include "mainwindow.h"
#include "mockhttpserver.h"
#include "mockpacks.h"
#include "mockwms.h"
#include "newregiondialog.h"
#include "offlineregionmanager.h"
#include "offlineregionsdialog.h"
#include "packmanager.h"
#include "packmanagerdialog.h"
#include "ts/globe/layerstackdraws.h"
#include "ts/i18n/pseudotranslator.h"
#include "ts/layers/layertreemodel.h"
#include "ts/network/networkclient.h"
#include "ts/network/tilediskcache.h"
#include "ts/projection/projection.h"
#include "ts/providers/providerregistry.h"
#include "ts/wms/wmsimageryprovider.h"
#include "ts/wms/wmssource.h"

namespace
{

QString withoutMnemonic(QString text)
{
  text.replace(QStringLiteral("&&"), QStringLiteral("\x01"));
  text.remove(QLatin1Char('&'));
  text.replace(QStringLiteral("\x01"), QStringLiteral("&"));
  return text;
}

/**
 * True for Qt's own menus, such as the menu bar's overflow ("»") menu that
 * appears while the window is laid out: no object name and no title. All
 * our menus have object names.
 */
bool isQtInternalMenu(const QMenu* menu)
{
  return menu != nullptr && menu->objectName().isEmpty() &&
         menu->title().isEmpty();
}

/** Object name, or class and parent chain for unnamed (Qt-internal) ones. */
QString describe(const QObject* object)
{
  if (!object->objectName().isEmpty()) {
    return object->objectName();
  }
  QString chain = QString::fromLatin1(object->metaObject()->className());
  for (const QObject* parent = object->parent(); parent != nullptr;
       parent = parent->parent()) {
    chain += QStringLiteral(" in ") +
             (parent->objectName().isEmpty()
                  ? QString::fromLatin1(parent->metaObject()->className())
                  : parent->objectName());
  }
  return chain;
}

/** Every user-visible string of @p window, labelled for error messages. */
QList<QPair<QString, QString>> visibleTexts(QWidget& window)
{
  QList<QPair<QString, QString>> texts;
  texts << qMakePair(QStringLiteral("window title"), window.windowTitle());
  const QList<QAction*> actions = window.findChildren<QAction*>();
  for (QAction* action : actions) {
    if (action->isSeparator() || isQtInternalMenu(action->menu())) {
      continue;
    }
    const QString name =
        action->objectName().isEmpty() && !action->text().isEmpty()
            ? action->text()
            : describe(action);
    texts << qMakePair(name + QStringLiteral(" text"), action->text());
    if (!action->statusTip().isEmpty()) {
      texts << qMakePair(name + QStringLiteral(" status tip"),
                         action->statusTip());
    }
    if (!action->toolTip().isEmpty() &&
        action->toolTip() != withoutMnemonic(action->text())) {
      texts << qMakePair(name + QStringLiteral(" tool tip"), action->toolTip());
    }
  }
  const QList<QMenu*> menus = window.findChildren<QMenu*>();
  for (QMenu* menu : menus) {
    if (isQtInternalMenu(menu)) {
      continue;
    }
    texts << qMakePair(menu->objectName() + QStringLiteral(" title"),
                       menu->title());
  }
  const QList<QLabel*> labels = window.findChildren<QLabel*>();
  for (QLabel* label : labels) {
    // A label hidden while it has nothing to say (the download progress)
    // shows no text.
    if (label->isHidden() && label->text().isEmpty()) {
      continue;
    }
    texts << qMakePair(describe(label) + QStringLiteral(" text"),
                       label->text());
  }
  return texts;
}

void verifyAllPseudo(QWidget& window)
{
  const QList<QPair<QString, QString>> texts = visibleTexts(window);
  QVERIFY(texts.size() >= 8);
  for (const QPair<QString, QString>& text : texts) {
    QVERIFY2(ts::PseudoTranslator::isPseudoTranslated(text.second),
             qPrintable(QStringLiteral("untranslated %1: \"%2\"")
                            .arg(text.first, text.second)));
  }
}

/** Sends a mouse event of @p type at @p position with @p buttons held. */
void sendMouse(QWidget* widget, QEvent::Type type, QPointF position,
               Qt::MouseButton button, Qt::MouseButtons buttons)
{
  QMouseEvent event(type, position,
                    QPointF(widget->mapToGlobal(position.toPoint())), button,
                    buttons, Qt::NoModifier);
  QCoreApplication::sendEvent(widget, &event);
}

/** Returns the window's globe view, skipping the test without OpenGL. */
ts::GlobeView* globeOf(ts::MainWindow& window)
{
  return window.findChild<ts::GlobeView*>(QStringLiteral("globeView"));
}

/**
 * Waits until @p globe has drawn no frame for 300 ms (the frame for the last
 * tile can still be on its way on a slow software renderer). False if it is
 * still drawing after 10 s.
 */
bool waitUntilQuiet(const ts::GlobeView* globe)
{
  int frames = globe->framesRendered();
  QElapsedTimer quiet;
  quiet.start();
  return QTest::qWaitFor(
      [&]() {
        if (globe->framesRendered() != frames) {
          frames = globe->framesRendered();
          quiet.restart();
        }
        return quiet.elapsed() >= 300;
      },
      10000);
}

} // namespace

/**
 * UI tests for the main window: opens and closes cleanly, and every string
 * is translatable and fits (checked with the pseudo-locale).
 */
class TestMainWindow : public QObject
{
  Q_OBJECT

private:
  /** A base pack for every test, as an installed program has. */
  QTemporaryDir m_basePack;

  // NOLINTNEXTLINE(readability-redundant-access-specifiers): Qt's slots marker
private slots:
  void initTestCase()
  {
    // Caches (WMS tiles, the catalogue) go to a test location, and the
    // catalogue isn't fetched from the internet.
    QStandardPaths::setTestModeEnabled(true);
    qputenv("TS_WMS_CATALOGUE_URL", QByteArray());
    // No offline regions left over from an earlier run.
    QFile::remove(
        QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
            .filePath(QStringLiteral("wms/regions.json")));
    QVERIFY(m_basePack.isValid());
    ts::test::writeFixtureBasePack(m_basePack.path());
    qputenv("TS_BASEPACK_DIR", m_basePack.path().toUtf8());
  }

  void basePackStates()
  {
    // Installed and intact: graticule, shoreline and Blue Marble by
    // default (top to bottom), the terrain under them switched off, their
    // sources credited, "Ready" throughout.
    {
      ts::MainWindow window;
      QCOMPARE(window.basePackDirectory(), QDir::cleanPath(m_basePack.path()));
      const ts::LayerTreeModel* model = window.layerModel();
      QCOMPARE(model->rowCount(), 4);
      const QStringList expected = {QStringLiteral("builtin:graticule"),
                                    QStringLiteral("builtin:shoreline"),
                                    QStringLiteral("builtin:bluemarble"),
                                    QStringLiteral("builtin:terrain")};
      for (int row = 0; row < 4; ++row) {
        QCOMPARE(model->layerDefinition(model->index(row, 0)).id,
                 expected.at(row));
      }
      QCOMPARE(model->data(model->index(3, 0), Qt::CheckStateRole).toInt(),
               static_cast<int>(Qt::Unchecked));
      ts::GlobeView* globe = globeOf(window);
      if (globe != nullptr) {
        QCOMPARE(globe->imageryLayerCount(), 1);
        QCOMPARE(globe->vectorLayerCount(), 2);
        QCOMPARE(globe->attributions(),
                 (QStringList{QStringLiteral("Fixture shoreline"),
                              QStringLiteral("Fixture imagery")}));
      }
      QSignalSpy verified(&window, &ts::MainWindow::basePackVerified);
      QVERIFY(window.basePackState() == ts::MainWindow::BasePackState::Ok ||
              verified.wait(10000));
      QVERIFY(window.basePackState() == ts::MainWindow::BasePackState::Ok);
      QCOMPARE(window.findChild<QLabel*>(QStringLiteral("statusLabel"))->text(),
               QStringLiteral("Ready"));
    }
    // Damaged: reported in the status bar, naming the file; still usable.
    {
      const QTemporaryDir damaged;
      ts::test::writeFixtureBasePack(damaged.path());
      QFile file(
          QDir(damaged.path()).filePath(QStringLiteral("bluemarble.sqlite")));
      QVERIFY(file.open(QIODevice::ReadWrite));
      file.seek(file.size() / 2);
      file.write("damage");
      file.close();
      qputenv("TS_BASEPACK_DIR", damaged.path().toUtf8());
      ts::MainWindow window;
      QSignalSpy verified(&window, &ts::MainWindow::basePackVerified);
      QVERIFY(verified.wait(10000));
      QVERIFY(window.basePackState() == ts::MainWindow::BasePackState::Damaged);
      const QString text =
          window.findChild<QLabel*>(QStringLiteral("statusLabel"))->text();
      QVERIFY2(text.contains(QLatin1String("damaged")) &&
                   text.contains(QLatin1String("bluemarble.sqlite")),
               qPrintable(text));
    }
    // Missing: the test imagery, and the status bar says why.
    {
      const QTemporaryDir empty;
      qputenv("TS_BASEPACK_DIR", empty.path().toUtf8());
      ts::MainWindow window;
      QVERIFY(window.basePackState() == ts::MainWindow::BasePackState::Missing);
      // The checkerboard under the graticule.
      QCOMPARE(window.layerModel()->rowCount(), 2);
      QCOMPARE(window.layerModel()
                   ->layerDefinition(window.layerModel()->index(1, 0))
                   .id,
               QStringLiteral("builtin:checkerboard"));
      QVERIFY(window.findChild<QLabel*>(QStringLiteral("statusLabel"))
                  ->text()
                  .contains(QLatin1String("not installed")));
    }
    qputenv("TS_BASEPACK_DIR", m_basePack.path().toUtf8());
  }

  void closeDuringBasePackCheck()
  {
    // Closing at once while the check runs: the thread stops and is joined.
    for (int i = 0; i < 20; ++i) {
      const ts::MainWindow window;
    }
  }

  void opensAndClosesCleanly()
  {
    QPointer<ts::MainWindow> window = new ts::MainWindow;
    window->show();
    QVERIFY(QTest::qWaitForWindowExposed(window.data()));
    QVERIFY(window->close());
    QVERIFY(!window->isVisible());
    delete window.data();
    QVERIFY(window.isNull());
  }

  void quitActionCloses()
  {
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QAction* quit = window.findChild<QAction*>(QStringLiteral("quitAction"));
    QVERIFY(quit != nullptr);
    quit->trigger();
    QVERIFY(!window.isVisible());
  }

  void englishTexts()
  {
    ts::MainWindow window;
    QCOMPARE(window.windowTitle(), QStringLiteral("Terra Soar"));
    QLabel* status = window.findChild<QLabel*>(QStringLiteral("statusLabel"));
    QVERIFY(status != nullptr);
    QCOMPARE(status->text(), QStringLiteral("Ready"));
    QMenu* file = window.findChild<QMenu*>(QStringLiteral("fileMenu"));
    QVERIFY(file != nullptr);
    QCOMPARE(file->title(), QStringLiteral("&File"));
    QAction* about = window.findChild<QAction*>(QStringLiteral("aboutAction"));
    QCOMPARE(about->text(), QStringLiteral("&About Terra Soar"));
  }

  void pseudoLocaleAtCreation()
  {
    ts::PseudoTranslator translator;
    QCoreApplication::installTranslator(&translator);
    {
      ts::MainWindow window;
      verifyAllPseudo(window);
    }
    QCoreApplication::removeTranslator(&translator);
  }

  void pseudoLocaleAtRunTime()
  {
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ts::PseudoTranslator translator;
    QCoreApplication::installTranslator(&translator);
    QCoreApplication::processEvents();
    verifyAllPseudo(window);
    QCoreApplication::removeTranslator(&translator);
    QCoreApplication::processEvents();
    QCOMPARE(window.windowTitle(), QStringLiteral("Terra Soar"));
  }

  /** With 30% longer text, nothing is cut off. */
  void pseudoLocaleNoTruncation()
  {
    ts::PseudoTranslator translator;
    QCoreApplication::installTranslator(&translator);
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));

    const QList<QLabel*> labels = window.findChildren<QLabel*>();
    for (QLabel* label : labels) {
      if (!label->isVisible() || label->wordWrap()) {
        continue;
      }
      // With a buddy, '&' marks a shortcut and isn't shown.
      const QString shown = label->buddy() != nullptr
                                ? withoutMnemonic(label->text())
                                : label->text();
      const int needed = label->fontMetrics().horizontalAdvance(shown);
      QVERIFY2(label->contentsRect().width() >= needed,
               qPrintable(QStringLiteral("%1 is %2 px wide, needs %3")
                              .arg(label->objectName())
                              .arg(label->contentsRect().width())
                              .arg(needed)));
    }

    QMenuBar* bar = window.menuBar();
    const QList<QAction*> barActions = bar->actions();
    for (QAction* action : barActions) {
      const QRect geometry = bar->actionGeometry(action);
      const int needed =
          bar->fontMetrics().horizontalAdvance(withoutMnemonic(action->text()));
      QVERIFY2(geometry.width() >= needed && bar->rect().contains(geometry),
               qPrintable(action->text()));
      QMenu* menu = action->menu();
      if (menu == nullptr) {
        continue;
      }
      const QList<QAction*> items = menu->actions();
      for (QAction* item : items) {
        const int itemNeeded = menu->fontMetrics().horizontalAdvance(
            withoutMnemonic(item->text()));
        QVERIFY2(menu->sizeHint().width() >= itemNeeded,
                 qPrintable(item->text()));
      }
    }
    QCoreApplication::removeTranslator(&translator);
  }

  // --- Globe view (needs OpenGL; skipped without it) ------------------------

  void globeViewOrFallback()
  {
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ts::GlobeView* globe = globeOf(window);
    if (!ts::GlobeView::isOpenGlAvailable()) {
      QVERIFY(globe == nullptr);
      QLabel* label = window.findChild<QLabel*>(QStringLiteral("noGlobeLabel"));
      if (label == nullptr) {
        QFAIL("no noGlobeLabel");
      }
      QVERIFY(label->text().contains(QStringLiteral("OpenGL 3.3")));
      return;
    }
    if (globe == nullptr) {
      QFAIL("no globe view although OpenGL is available");
    }
    QVERIFY(
        QTest::qWaitFor([&]() { return globe->framesRendered() > 0; }, 5000));
    QVERIFY2(globe->errorString().isEmpty(), qPrintable(globe->errorString()));
  }

  void dragPansAndWheelZooms()
  {
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ts::GlobeView* globe = globeOf(window);
    if (globe == nullptr) {
      QSKIP("No OpenGL view on this platform.");
    }
    const QPointF centre(globe->width() / 2.0, globe->height() / 2.0);
    const ts::CameraPose before = globe->controller().camera().pose();
    sendMouse(globe, QEvent::MouseButtonPress, centre, Qt::LeftButton,
              Qt::LeftButton);
    sendMouse(globe, QEvent::MouseMove, centre + QPointF(-60.0, 0.0),
              Qt::NoButton, Qt::LeftButton);
    sendMouse(globe, QEvent::MouseButtonRelease, centre + QPointF(-60.0, 0.0),
              Qt::LeftButton, Qt::NoButton);
    // Dragging left moves the view east.
    QVERIFY(globe->controller().camera().pose().targetLonDeg >
            before.targetLonDeg + 1.0);

    const double rangeBefore = globe->controller().camera().pose().rangeM;
    QWheelEvent wheel(centre, QPointF(globe->mapToGlobal(centre.toPoint())),
                      QPoint(), QPoint(0, 240), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(globe, &wheel);
    QVERIFY(globe->controller().camera().pose().rangeM < 0.7 * rangeBefore);
  }

  void statusBarShowsPointerAndEye()
  {
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ts::GlobeView* globe = globeOf(window);
    if (globe == nullptr) {
      QSKIP("No OpenGL view on this platform.");
    }
    QLabel* pointer = window.findChild<QLabel*>(QStringLiteral("pointerLabel"));
    QLabel* eye = window.findChild<QLabel*>(QStringLiteral("eyeLabel"));
    if (pointer == nullptr || eye == nullptr) {
      QFAIL("status bar labels missing");
    }
    QCOMPARE(pointer->text(), QStringLiteral("Pointer not on the globe"));
    // The centre of the start view is 20° N, 10° E.
    sendMouse(globe, QEvent::MouseMove,
              QPointF(globe->width() / 2.0, globe->height() / 2.0),
              Qt::NoButton, Qt::NoButton);
    QVERIFY2(pointer->text().contains(QStringLiteral("° N")) &&
                 pointer->text().contains(QStringLiteral("° E")),
             qPrintable(pointer->text()));
    QVERIFY(QTest::qWaitFor(
        [&]() { return eye->text().startsWith(QStringLiteral("Eye height ")); },
        5000));
    QVERIFY2(eye->text().endsWith(QStringLiteral(" km")),
             qPrintable(eye->text()));
    sendMouse(globe, QEvent::MouseMove, QPointF(2.0, 2.0), Qt::NoButton,
              Qt::NoButton);
    QCOMPARE(pointer->text(), QStringLiteral("Pointer not on the globe"));
  }

  void overlayAndResetView()
  {
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ts::GlobeView* globe = globeOf(window);
    if (globe == nullptr) {
      QSKIP("No OpenGL view on this platform.");
    }
    QAction* overlay =
        window.findChild<QAction*>(QStringLiteral("debugOverlayAction"));
    QAction* reset =
        window.findChild<QAction*>(QStringLiteral("resetViewAction"));
    if (overlay == nullptr || reset == nullptr) {
      QFAIL("view actions missing");
    }
    QVERIFY(!globe->isDebugOverlayVisible());
    overlay->trigger();
    QVERIFY(globe->isDebugOverlayVisible());
    const int frames = globe->framesRendered();
    QVERIFY(QTest::qWaitFor([&]() { return globe->framesRendered() > frames; },
                            5000));

    // Move away, then Reset View flies home.
    globe->flyTo(ts::CameraPose{-33.9, 151.2, 1.0e5, 30.0, 40.0}, 0.2);
    QVERIFY(QTest::qWaitFor(
        [&]() { return !globe->controller().isAnimating(); }, 5000));
    reset->trigger();
    QVERIFY(globe->controller().isAnimating());
    QVERIFY(QTest::qWaitFor(
        [&]() { return !globe->controller().isAnimating(); }, 10000));
    const ts::CameraPose home = ts::GlobeView::homePose();
    const ts::CameraPose& pose = globe->controller().camera().pose();
    QCOMPARE(pose.targetLatDeg, home.targetLatDeg);
    QCOMPARE(pose.targetLonDeg, home.targetLonDeg);
    QCOMPARE(pose.rangeM, home.rangeM);
  }

  void layerDock()
  {
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ts::LayerTreeModel* model = window.layerModel();
    QTreeView* view =
        window.findChild<QTreeView*>(QStringLiteral("layerTreeView"));
    QSlider* slider =
        window.findChild<QSlider*>(QStringLiteral("opacitySlider"));
    QPushButton* addCollection =
        window.findChild<QPushButton*>(QStringLiteral("addCollectionButton"));
    QPushButton* remove =
        window.findChild<QPushButton*>(QStringLiteral("removeLayerButton"));
    if (view == nullptr || slider == nullptr || addCollection == nullptr ||
        remove == nullptr) {
      QFAIL("layer dock widgets missing");
    }
    // A new project shows the graticule and shoreline over Blue Marble
    // (and the terrain, off), and the dock says vectors stay above imagery.
    QCOMPARE(model->rowCount(), 4);
    QVERIFY(!window.findChild<QLabel*>(QStringLiteral("layerOrderNote"))
                 ->text()
                 .isEmpty());
    ts::GlobeView* globe = globeOf(window);
    if (globe != nullptr) {
      QCOMPARE(globe->imageryLayerCount(), 1);
      QCOMPARE(globe->vectorLayerCount(), 2);
    }
    // Hiding a layer takes it off the globe at once (the next frame), and
    // its attribution with it.
    const QModelIndex shoreline = model->index(1, 0);
    model->setData(shoreline, Qt::Unchecked, Qt::CheckStateRole);
    if (globe != nullptr) {
      QCOMPARE(globe->vectorLayerCount(), 1);
      QCOMPARE(globe->attributions(),
               QStringList{QStringLiteral("Fixture imagery")});
    }
    model->setData(shoreline, Qt::Checked, Qt::CheckStateRole);
    const QModelIndex layer = model->index(2, 0);
    model->setData(layer, Qt::Unchecked, Qt::CheckStateRole);
    if (globe != nullptr) {
      QCOMPARE(globe->imageryLayerCount(), 0);
    }
    model->setData(layer, Qt::Checked, Qt::CheckStateRole);
    // The slider sets the selected entry's opacity.
    view->setCurrentIndex(layer);
    slider->setValue(40);
    QCOMPARE(model->data(layer, ts::LayerTreeModel::OpacityRole).toDouble(),
             0.4);
    // A collection goes above the selected layer; remove takes it away.
    addCollection->click();
    QCOMPARE(model->rowCount(), 5);
    QVERIFY(
        model->data(model->index(2, 0), ts::LayerTreeModel::IsCollectionRole)
            .toBool());
    view->setCurrentIndex(model->index(2, 0));
    remove->click();
    QCOMPARE(model->rowCount(), 4);
  }

  void terrainLayer()
  {
    // The terrain comes off by default; ticking it shows it, the View menu
    // sets its exaggeration in the layer's style.
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ts::LayerTreeModel* model = window.layerModel();
    const QModelIndex terrain = model->index(model->rowCount() - 1, 0);
    QCOMPARE(model->layerDefinition(terrain).id,
             QStringLiteral("builtin:terrain"));
    QAction* triple =
        window.findChild<QAction*>(QStringLiteral("exaggeration30Action"));
    QAction* single =
        window.findChild<QAction*>(QStringLiteral("exaggeration10Action"));
    if (triple == nullptr || single == nullptr) {
      QFAIL("exaggeration actions missing");
    }
    QVERIFY(single->isChecked() && triple->isEnabled());
    ts::GlobeView* globe = globeOf(window);
    if (globe != nullptr) {
      QVERIFY(!globe->terrainShown());
    }
    model->setData(terrain, Qt::Checked, Qt::CheckStateRole);
    if (globe != nullptr) {
      QVERIFY(globe->terrainShown());
      QVERIFY(
          globe->attributions().contains(QStringLiteral("Fixture elevation")));
    }
    triple->trigger();
    QCOMPARE(ts::terrainExaggeration(model->layerDefinition(terrain)), 3.0);
    QVERIFY(triple->isChecked() && !single->isChecked());
    // Without an elevation layer the menu is off.
    const int rows = model->rowCount();
    QVERIFY(model->removeNode(terrain));
    QCOMPARE(model->rowCount(), rows - 1);
    QVERIFY(!triple->isEnabled());
    if (globe != nullptr) {
      QVERIFY(!globe->terrainShown());
    }
  }

  void elevationFileLayer()
  {
    // A DEM file becomes a visible elevation layer above the terrain, and
    // the status bar reads the height under the pointer.
    const QTemporaryDir directory;
    const QString dem = directory.filePath(QStringLiteral("hills.tif"));
    QVERIFY(ts::test::writeFixtureDem(dem, 1500.0F));
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.findChild<QAction*>(QStringLiteral("addElevationAction")));
    QVERIFY(
        window.findChild<QAction*>(QStringLiteral("addElevationFileAction")));
    QAction* ellipsoidal =
        window.findChild<QAction*>(QStringLiteral("ellipsoidalHeightsAction"));
    if (ellipsoidal == nullptr) {
      QFAIL("ellipsoidal heights action missing");
    }
    QVERIFY(!ellipsoidal->isChecked());

    ts::LayerTreeModel* model = window.layerModel();
    const int rows = model->rowCount();
    window.addElevationLayer(dem, QStringLiteral("ellipsoid"));
    QCOMPARE(model->rowCount(), rows + 1);
    // Above the terrain (at the bottom), so it takes priority.
    const QModelIndex added = model->index(rows - 1, 0);
    const ts::LayerDefinition definition = model->layerDefinition(added);
    QCOMPARE(definition.type, ts::LayerType::Elevation);
    QCOMPARE(definition.name, QStringLiteral("hills"));
    QCOMPARE(definition.source.value(QStringLiteral("kind")).toString(),
             QStringLiteral("file"));
    QCOMPARE(model->data(added, Qt::CheckStateRole).toInt(),
             static_cast<int>(Qt::Checked));
    QCOMPARE(model->layerDefinition(model->index(rows, 0)).id,
             QStringLiteral("builtin:terrain"));

    ts::GlobeView* globe = globeOf(window);
    if (globe == nullptr) {
      return;
    }
    QVERIFY(globe->terrainShown());
    QLabel* pointer = window.findChild<QLabel*>(QStringLiteral("pointerLabel"));
    QVERIFY(pointer != nullptr);
    // The start view's centre, 20° N 10° E, is on the DEM: 1500 m (the
    // fixture pack has no geoid, so sea level is the ellipsoid).
    const QString metres = QLocale().toString(1500) + QStringLiteral(" m");
    const QPointF centre(globe->width() / 2.0, globe->height() / 2.0);
    QVERIFY2(QTest::qWaitFor(
                 [&]() {
                   sendMouse(globe, QEvent::MouseMove, centre, Qt::NoButton,
                             Qt::NoButton);
                   return pointer->text().contains(metres);
                 },
                 20000),
             qPrintable(pointer->text()));
    QVERIFY(globe->pointerGround().has_value());
    ellipsoidal->trigger();
    QVERIFY2(pointer->text().contains(metres + QStringLiteral(" ellipsoidal")),
             qPrintable(pointer->text()));
  }

  void webTerrainDialog()
  {
    ts::AddWebTerrainDialog dialog(nullptr);
    QComboBox* presets =
        dialog.findChild<QComboBox*>(QStringLiteral("webTerrainPresetBox"));
    QLineEdit* address =
        dialog.findChild<QLineEdit*>(QStringLiteral("webTerrainAddressEdit"));
    QLabel* error =
        dialog.findChild<QLabel*>(QStringLiteral("webTerrainErrorLabel"));
    if (presets == nullptr || address == nullptr || error == nullptr) {
      QFAIL("dialog controls missing");
    }
    // A preset fills in the form.
    QVERIFY(presets->count() >= 2);
    presets->setCurrentIndex(1);
    emit presets->activated(1);
    QVERIFY(address->text().contains(QStringLiteral("{z}")));
    std::optional<ts::LayerDefinition> definition = dialog.buildDefinition();
    if (!definition) {
      QFAIL("the preset makes no layer");
    }
    QCOMPARE(definition->type, ts::LayerType::Elevation);
    QCOMPARE(definition->source.value(QStringLiteral("kind")).toString(),
             QStringLiteral("web"));
    QVERIFY(!definition->source.value(QStringLiteral("attribution"))
                 .toString()
                 .isEmpty());

    // Terrain-RGB tiles of one's own.
    dialog.setSource(
        ts::AddWebTerrainDialog::Kind::TerrainRgb,
        QStringLiteral("https://tiles.example/rgb/{z}/{x}/{y}.png"), 12,
        QStringLiteral("egm2008"), QString(), QStringLiteral("Example"));
    definition = dialog.buildDefinition();
    if (!definition) {
      QFAIL("no layer");
    }
    QCOMPARE(definition->name, QStringLiteral("tiles.example"));
    QCOMPARE(definition->source.value(QStringLiteral("encoding")).toString(),
             QStringLiteral("terrain-rgb"));
    QCOMPARE(definition->source.value(QStringLiteral("maximumZoom")).toInt(),
             12);
    QCOMPARE(
        definition->source.value(QStringLiteral("verticalDatum")).toString(),
        QStringLiteral("egm2008"));

    // An address without {z}, {x} and {y} is refused with a reason.
    dialog.setSource(ts::AddWebTerrainDialog::Kind::Terrarium,
                     QStringLiteral("https://tiles.example/terrain.png"), 12,
                     QStringLiteral("egm96"), QString(), QString());
    QVERIFY(!dialog.buildDefinition());
    QVERIFY(!error->isHidden() && !error->text().isEmpty());

    // A Cloud Optimized GeoTIFF is read through GDAL, where it can.
    dialog.setSource(ts::AddWebTerrainDialog::Kind::CloudGeoTiff,
                     QStringLiteral("https://data.example/dem/alps.tif"), 0,
                     QStringLiteral("ellipsoid"), QString(), QString());
    definition = dialog.buildDefinition();
    if (ts::AddWebTerrainDialog::canReadRemoteFiles()) {
      if (!definition) {
        QFAIL("no layer for a GeoTIFF");
      }
      QCOMPARE(definition->source.value(QStringLiteral("path")).toString(),
               QStringLiteral("/vsicurl/https://data.example/dem/alps.tif"));
      QCOMPARE(definition->name, QStringLiteral("alps.tif"));
    } else {
      QVERIFY(!definition);
    }

    // Everything in it is translatable.
    ts::PseudoTranslator translator;
    QCoreApplication::installTranslator(&translator);
    {
      ts::AddWebTerrainDialog pseudo(nullptr);
      verifyAllPseudo(pseudo);
    }
    QCoreApplication::removeTranslator(&translator);
  }

  void projectSaveAndOpen()
  {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("test.terrasoar"));
    QJsonArray saved;
    {
      ts::MainWindow window;
      ts::LayerTreeModel* model = window.layerModel();
      const QModelIndex collection =
          model->addCollection(QStringLiteral("Overlays"));
      model->setData(collection, 0.5, ts::LayerTreeModel::OpacityRole);
      ts::GlobeView* globe = globeOf(window);
      if (globe != nullptr) {
        globe->setPose(ts::CameraPose{51.5, -0.1, 2.0e5, 30.0, 40.0});
      }
      QString error;
      QVERIFY2(window.saveProject(path, &error), qPrintable(error));
      QCOMPARE(window.projectPath(), path);
      saved = model->toJson();
    }
    ts::MainWindow window;
    QString error;
    QVERIFY2(window.openProject(path, &error), qPrintable(error));
    QCOMPARE(window.layerModel()->toJson(), saved);
    ts::GlobeView* globe = globeOf(window);
    if (globe != nullptr) {
      const ts::CameraPose& pose = globe->controller().camera().pose();
      QCOMPARE(pose.targetLatDeg, 51.5);
      QCOMPARE(pose.rangeM, 2.0e5);
    }
    // A damaged file leaves the project as it was.
    QFile broken(directory.filePath(QStringLiteral("broken.terrasoar")));
    QVERIFY(broken.open(QIODevice::WriteOnly));
    broken.write("{ not json");
    broken.close();
    QVERIFY(!window.openProject(broken.fileName(), &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(window.layerModel()->toJson(), saved);
  }

  void mapViewToggle()
  {
    // View > 2D Map changes to the flat map and back, animated, keeping
    // the centre of the view; the map slides under a drag, never turns.
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ts::GlobeView* globe = globeOf(window);
    QAction* mapView =
        window.findChild<QAction*>(QStringLiteral("mapViewAction"));
    if (mapView == nullptr) {
      QFAIL("no mapViewAction");
    }
    if (globe == nullptr) {
      QVERIFY(!mapView->isEnabled());
      QSKIP("No OpenGL view on this platform.");
    }
    QVERIFY(mapView->isEnabled() && !mapView->isChecked());
    QVERIFY(QTest::qWaitFor([&]() { return !globe->isLoadingTiles(); }, 20000));
    globe->setPose(ts::CameraPose{48.0, 11.0, 4.0e6, 40.0, 30.0});
    QCOMPARE(globe->viewMode(), ts::ViewMode::Globe);
    QSignalSpy modeChanges(globe, &ts::GlobeView::viewModeChanged);

    const int framesBefore = globe->framesRendered();
    mapView->trigger();
    QVERIFY(mapView->isChecked());
    QCOMPARE(globe->viewMode(), ts::ViewMode::Map);
    QCOMPARE(modeChanges.size(), 1);
    QVERIFY(globe->controller().isMorphing());
    QVERIFY(QTest::qWaitFor([&]() { return !globe->controller().isMorphing(); },
                            10000));
    // Several frames of the change were drawn, not one jump.
    QVERIFY(globe->framesRendered() > framesBefore + 2);
    const ts::Camera& camera = globe->controller().camera();
    QCOMPARE(camera.morph(), 1.0);
    QCOMPARE(camera.pose().targetLatDeg, 48.0);
    QCOMPARE(camera.pose().targetLonDeg, 11.0);
    QCOMPARE(camera.pose().headingDeg, 0.0);
    QCOMPARE(camera.pose().tiltDeg, 0.0);

    // The pointer readout works on the map: the centre is where it was.
    QLabel* pointer = window.findChild<QLabel*>(QStringLiteral("pointerLabel"));
    QVERIFY(pointer != nullptr);
    const QPointF centre(globe->width() / 2.0, globe->height() / 2.0);
    sendMouse(globe, QEvent::MouseMove, centre, Qt::NoButton, Qt::NoButton);
    QVERIFY2(pointer->text().startsWith(QStringLiteral("48.0")),
             qPrintable(pointer->text()));

    // A left drag slides the map; a right drag does not turn it.
    sendMouse(globe, QEvent::MouseButtonPress, centre, Qt::LeftButton,
              Qt::LeftButton);
    sendMouse(globe, QEvent::MouseMove, centre + QPointF(-50.0, 0.0),
              Qt::NoButton, Qt::LeftButton);
    QTest::qWait(150); // held still: no inertia after the release
    sendMouse(globe, QEvent::MouseButtonRelease, centre + QPointF(-50.0, 0.0),
              Qt::LeftButton, Qt::NoButton);
    QVERIFY(camera.pose().targetLonDeg > 11.5);
    QCOMPARE(camera.pose().targetLatDeg, 48.0);
    sendMouse(globe, QEvent::MouseButtonPress, centre, Qt::RightButton,
              Qt::RightButton);
    sendMouse(globe, QEvent::MouseMove, centre + QPointF(80.0, 40.0),
              Qt::NoButton, Qt::RightButton);
    sendMouse(globe, QEvent::MouseButtonRelease, centre + QPointF(80.0, 40.0),
              Qt::RightButton, Qt::NoButton);
    QCOMPARE(camera.pose().headingDeg, 0.0);
    QCOMPARE(camera.pose().tiltDeg, 0.0);

    // The scale bar is drawn, and the map goes idle like the globe.
    QVERIFY(QTest::qWaitFor([&]() { return !globe->isLoadingTiles(); }, 20000));
    QVERIFY2(waitUntilQuiet(globe), "the map kept drawing");
    QVERIFY2(globe->scaleBarText().contains(QStringLiteral("km")),
             qPrintable(globe->scaleBarText()));
    QCOMPARE(window.stateReport().value(QStringLiteral("view")).toString(),
             QStringLiteral("map"));
    QVERIFY(window.stateReport()
                .value(QStringLiteral("imagery_tiles_drawn"))
                .toInt() > 0);
    const int idleFrames = globe->framesRendered();
    QTest::qWait(400);
    QCOMPARE(globe->framesRendered(), idleFrames);
    const QString shots = qEnvironmentVariable("TS_UI_SCREENSHOTS");
    if (!shots.isEmpty()) {
      globe->grabFramebuffer().save(
          QDir(shots).filePath(QStringLiteral("map-view.png")));
    }

    // Back to the globe.
    const double lonOnMap = camera.pose().targetLonDeg;
    mapView->trigger();
    QVERIFY(!mapView->isChecked());
    QCOMPARE(globe->viewMode(), ts::ViewMode::Globe);
    QVERIFY(QTest::qWaitFor(
        [&]() { return !globe->controller().isAnimating(); }, 10000));
    QCOMPARE(camera.morph(), 0.0);
    QCOMPARE(camera.pose().targetLonDeg, lonOnMap);
    QCOMPARE(modeChanges.size(), 2);
  }

  void projectionMenu()
  {
    // View > Map Projection lists the projections, one checked; choosing
    // another changes the map at once.
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QMenu* menu = window.findChild<QMenu*>(QStringLiteral("projectionMenu"));
    if (menu == nullptr) {
      QFAIL("no projectionMenu");
    }
    QCOMPARE(menu->title(), QStringLiteral("Map &Projection"));
    const QList<QAction*> actions = menu->actions();
    QCOMPARE(actions.size(), 3);
    QCOMPARE(actions.at(0)->text(), QStringLiteral("Mercator"));
    QCOMPARE(actions.at(1)->text(), QStringLiteral("Web Mercator"));
    QCOMPARE(actions.at(2)->text(), QStringLiteral("Miller Cylindrical"));
    QVERIFY(actions.at(0)->isChecked());
    ts::GlobeView* globe = globeOf(window);
    if (globe == nullptr) {
      QVERIFY(!actions.at(2)->isEnabled());
      QSKIP("No OpenGL view on this platform.");
    }
    QCOMPARE(globe->mapProjection()->id(), QStringLiteral("mercator"));
    globe->setViewMode(ts::ViewMode::Map, false);
    QVERIFY(!globe->controller().isMorphing());
    QCOMPARE(globe->controller().camera().morph(), 1.0);
    // Mercator can't be centred at 88° N; Miller can.
    globe->setPose(ts::CameraPose{88.0, 0.0, 1.0e6, 0.0, 0.0});
    QVERIFY(globe->controller().camera().pose().targetLatDeg < 85.1);
    actions.at(2)->trigger();
    QCOMPARE(globe->mapProjection()->id(), QStringLiteral("miller"));
    QVERIFY(actions.at(2)->isChecked() && !actions.at(0)->isChecked());
    globe->setPose(ts::CameraPose{88.0, 0.0, 1.0e6, 0.0, 0.0});
    QCOMPARE(globe->controller().camera().pose().targetLatDeg, 88.0);
    QCOMPARE(
        window.stateReport().value(QStringLiteral("projection")).toString(),
        QStringLiteral("miller"));
    QVERIFY(QTest::qWaitFor([&]() { return !globe->isLoadingTiles(); }, 20000));
    const QString shots = qEnvironmentVariable("TS_UI_SCREENSHOTS");
    if (!shots.isEmpty()) {
      globe->setPose(ts::CameraPose{20.0, 10.0, 5.0e7, 0.0, 0.0});
      QVERIFY(
          QTest::qWaitFor([&]() { return !globe->isLoadingTiles(); }, 20000));
      QTest::qWait(300);
      globe->grabFramebuffer().save(
          QDir(shots).filePath(QStringLiteral("map-miller-world.png")));
    }
  }

  // --- WMS
  // ---------------------------------------------------------------------

  void addWmsDialog()
  {
    // The whole dialog against the mock server: the current catalogue
    // arrives, the server's layers are listed, style and format follow the
    // layer, and Add makes the layer's definition.
    std::shared_ptr<ts::test::MockWmsBehaviour> behaviour =
        std::make_shared<ts::test::MockWmsBehaviour>();
    const ts::test::MockHttpServer::Handler wms =
        ts::test::mockWmsHandler(behaviour);
    ts::test::MockHttpServer server;
    const QUrl serviceUrl = server.url(QStringLiteral("/wms"));
    server.setHandler(
        [wms, serviceUrl](const ts::test::MockHttpRequest& request) {
          if (request.url.path() == QLatin1String("/catalogue.json")) {
            return ts::test::mockResponse(
                200,
                "{\"format\": \"terrasoar-wms-catalogue\", \"version\": 1, "
                "\"services\": [{\"title\": \"Mock service\", \"url\": \"" +
                    serviceUrl.toString().toUtf8() +
                    "\", \"description\": \"Circles and lines\", "
                    "\"attribution\": \"Mock Agency\", \"terms\": "
                    "\"https://example.invalid/terms\"}]}",
                "application/json");
          }
          return wms(request);
        });
    qputenv("TS_WMS_CATALOGUE_URL",
            server.url(QStringLiteral("/catalogue.json")).toString().toUtf8());
    ts::NetworkSettings settings;
    settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    std::shared_ptr<ts::NetworkClient> network =
        std::make_shared<ts::NetworkClient>(settings);
    ts::AddWmsDialog dialog(network, nullptr);
    qputenv("TS_WMS_CATALOGUE_URL", QByteArray());
    // Built in until the current catalogue arrives.
    QVERIFY(!dialog.catalogue().empty());
    QSignalSpy catalogueUpdated(&dialog, &ts::AddWmsDialog::catalogueUpdated);
    QVERIFY(catalogueUpdated.wait(5000));
    QCOMPARE(dialog.catalogue().size(), std::size_t{1});
    QComboBox* services =
        dialog.findChild<QComboBox*>(QStringLiteral("wmsServiceBox"));
    QVERIFY(services != nullptr);
    QCOMPARE(services->count(), 1);
    QCOMPARE(services->itemText(0), QStringLiteral("Mock service"));
    services->setCurrentIndex(0);
    QMetaObject::invokeMethod(services, "activated", Q_ARG(int, 0));
    QCOMPARE(services->currentText(), serviceUrl.toString());
    QLabel* about = dialog.findChild<QLabel*>(QStringLiteral("wmsAboutLabel"));
    QVERIFY(about->text().contains(QStringLiteral("Mock Agency")));

    QSignalSpy loaded(&dialog, &ts::AddWmsDialog::capabilitiesLoaded);
    QPushButton* connectButton =
        dialog.findChild<QPushButton*>(QStringLiteral("wmsConnectButton"));
    connectButton->click();
    QVERIFY(dialog.isBusy());
    QVERIFY(loaded.wait(5000));
    QVERIFY(dialog.capabilities().has_value());
    QVERIFY2(dialog.statusText().contains(QStringLiteral("2 layer")),
             qPrintable(dialog.statusText()));
    QTreeWidget* tree =
        dialog.findChild<QTreeWidget*>(QStringLiteral("wmsLayerTree"));
    QCOMPARE(tree->topLevelItemCount(), 1);
    QCOMPARE(tree->topLevelItem(0)->childCount(), 2);
    QPushButton* add =
        dialog.findChild<QPushButton*>(QStringLiteral("wmsAddButton"));
    QVERIFY(!add->isEnabled()); // the group has no name
    QComboBox* styles =
        dialog.findChild<QComboBox*>(QStringLiteral("wmsStyleBox"));
    QComboBox* formats =
        dialog.findChild<QComboBox*>(QStringLiteral("wmsFormatBox"));
    QVERIFY(dialog.selectLayer(QStringLiteral("pattern")));
    QVERIFY(add->isEnabled());
    QCOMPARE(styles->count(), 2); // Default, Plain
    QCOMPARE(formats->currentText(), QStringLiteral("image/png"));
    QVERIFY(dialog.selectLayer(QStringLiteral("background")));
    QCOMPARE(formats->currentText(), QStringLiteral("image/jpeg")); // opaque
    QVERIFY(dialog.findChild<QLabel*>(QStringLiteral("wmsDetailsLabel"))
                ->text()
                .contains(QStringLiteral("EPSG:4326")));
    add->click();
    QCOMPARE(dialog.result(), static_cast<int>(QDialog::Accepted));
    QVERIFY(dialog.layerDefinition().has_value());
    const ts::LayerDefinition definition =
        dialog.layerDefinition().value_or(ts::LayerDefinition{});
    QCOMPARE(definition.type, ts::LayerType::Imagery);
    QCOMPARE(definition.origin, ts::LayerOrigin::Catalogue);
    QCOMPARE(definition.name, QStringLiteral("Opaque background"));
    QString error;
    const std::optional<ts::WmsSource> source =
        ts::WmsSource::fromJson(definition.source, &error);
    QVERIFY2(source.has_value(), qPrintable(error));
    const ts::WmsSource added = source.value_or(ts::WmsSource{});
    QCOMPARE(added.layers, QStringList{QStringLiteral("background")});
    QCOMPARE(added.format, QStringLiteral("image/jpeg"));
    QVERIFY(!added.transparent);
    QCOMPARE(added.crs, QStringLiteral("EPSG:4326")); // all the mock offers
    QCOMPARE(added.url, serviceUrl);
    // The layer names no credit; the catalogue's is used.
    QCOMPARE(added.attribution, QStringLiteral("Mock Agency"));
  }

  void addWmsDialogErrors()
  {
    std::shared_ptr<ts::test::MockWmsBehaviour> behaviour =
        std::make_shared<ts::test::MockWmsBehaviour>();
    behaviour->refuse130 = true;
    ts::test::MockHttpServer server(ts::test::mockWmsHandler(behaviour));
    ts::NetworkSettings settings;
    settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    settings.attempts = 1;
    std::shared_ptr<ts::NetworkClient> network =
        std::make_shared<ts::NetworkClient>(settings);
    ts::AddWmsDialog dialog(network, nullptr);
    QSignalSpy loaded(&dialog, &ts::AddWmsDialog::capabilitiesLoaded);
    QSignalSpy failed(&dialog, &ts::AddWmsDialog::connectionFailed);
    // A server that refuses 1.3.0 is asked again in 1.1.1.
    dialog.connectTo(server.url(QStringLiteral("/wms")).toString());
    QVERIFY(loaded.wait(5000));
    QVERIFY(dialog.capabilities().has_value());
    QCOMPARE(dialog.capabilities().value_or(ts::WmsCapabilities{}).version,
             QStringLiteral("1.1.1"));
    QCOMPARE(server.requestCount(), 2);
    // Not a web address.
    dialog.connectTo(QStringLiteral("ftp://example.invalid/wms"));
    QCOMPARE(failed.size(), 1);
    QVERIFY(dialog.statusText().contains(QStringLiteral("https://")));
    // Nobody listening.
    quint16 closedPort = 0;
    {
      QTcpServer probe;
      QVERIFY(probe.listen(QHostAddress::LocalHost, 0));
      closedPort = probe.serverPort();
    }
    dialog.connectTo(QStringLiteral("http://127.0.0.1:%1/wms").arg(closedPort));
    QVERIFY(failed.wait(5000));
    QVERIFY2(dialog.statusText().startsWith(QStringLiteral("Can't reach")),
             qPrintable(dialog.statusText()));
    QVERIFY(dialog.capabilities() == std::nullopt);
    // A web page instead of capabilities.
    behaviour->refuse130 = false;
    ts::test::MockHttpServer page([](const ts::test::MockHttpRequest&) {
      return ts::test::mockResponse(200, "<html><body>Welcome</body></html>",
                                    "text/html");
    });
    dialog.connectTo(page.url(QStringLiteral("/")).toString());
    QVERIFY(failed.wait(5000));
    QVERIFY2(dialog.statusText().contains(QStringLiteral("web page")),
             qPrintable(dialog.statusText()));
  }

  void wmsLayerStatusInTheDock()
  {
    // A WMS layer that can't be opened is marked at once; one whose server
    // can't be reached is marked offline once tiles were asked for.
    QTemporaryDir cache; // before the window, which keeps the cache open
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ts::NetworkSettings settings;
    settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    settings.attempts = 1;
    ts::WmsProviderSettings wms;
    wms.cacheDirectory = cache.path();
    window.registry().setWmsNetwork(
        std::make_shared<ts::NetworkClient>(settings), wms);
    ts::LayerTreeModel* model = window.layerModel();

    ts::WmsSource broken;
    broken.url = QUrl(QStringLiteral("http://127.0.0.1:9/wms"));
    broken.layers = QStringList{QStringLiteral("x")};
    broken.styles = QStringList{QString()};
    QJsonObject badSource = broken.toJson();
    badSource.insert(QStringLiteral("crs"), QStringLiteral("EPSG:2056"));
    ts::LayerDefinition definition;
    definition.id = QStringLiteral("wms:test-broken");
    definition.origin = ts::LayerOrigin::Catalogue;
    definition.type = ts::LayerType::Imagery;
    definition.name = QStringLiteral("Broken");
    definition.source = badSource;
    const QModelIndex brokenIndex = model->addLayer(definition);
    QCOMPARE(model->data(brokenIndex, ts::LayerTreeModel::StatusRole).toInt(),
             static_cast<int>(ts::ProviderStatus::State::Error));
    QVERIFY(model->data(brokenIndex, Qt::ToolTipRole)
                .toString()
                .contains(QStringLiteral("EPSG:2056")));
    QVERIFY(
        !model->data(brokenIndex, Qt::DecorationRole).value<QIcon>().isNull());

    // Nobody listening on port 9 here (the discard port).
    definition.id = QStringLiteral("wms:test-offline");
    definition.name = QStringLiteral("Unreachable");
    definition.source = broken.toJson();
    const QModelIndex offline = model->addLayer(definition);
    QCOMPARE(model->data(offline, ts::LayerTreeModel::StatusRole).toInt(),
             static_cast<int>(ts::ProviderStatus::State::Ok));
    if (globeOf(window) == nullptr) {
      QSKIP("No OpenGL view: no tiles are asked for.");
    }
    QVERIFY(QTest::qWaitFor(
        [&]() {
          return model->data(offline, ts::LayerTreeModel::StatusRole).toInt() ==
                 static_cast<int>(ts::ProviderStatus::State::Offline);
        },
        15000));
    QVERIFY(model->data(offline, Qt::ToolTipRole)
                .toString()
                .contains(QStringLiteral("127.0.0.1")));
    // The view still goes idle: failed tiles aren't asked for every frame.
    QVERIFY(QTest::qWaitFor(
        [&]() { return !globeOf(window)->isLoadingTiles(); }, 20000));
    QVERIFY2(waitUntilQuiet(globeOf(window)), "the view kept drawing");
    const int frames = globeOf(window)->framesRendered();
    QTest::qWait(500);
    QCOMPARE(globeOf(window)->framesRendered(), frames);
    // Removing the layer forgets its status.
    model->removeNode(offline);
  }

  void wmsLayerDrawsAndIsCredited()
  {
    std::shared_ptr<ts::test::MockWmsBehaviour> behaviour =
        std::make_shared<ts::test::MockWmsBehaviour>();
    ts::test::MockHttpServer server(ts::test::mockWmsHandler(behaviour));
    // Before the window: its providers keep the disk cache open until it
    // goes, and Windows can't delete open files.
    QTemporaryDir cache;
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ts::NetworkSettings settings;
    settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    ts::WmsProviderSettings wms;
    wms.cacheDirectory = cache.path();
    window.registry().setWmsNetwork(
        std::make_shared<ts::NetworkClient>(settings), wms);
    ts::WmsSource source;
    source.url = server.url(QStringLiteral("/wms"));
    source.layers = QStringList{QStringLiteral("pattern")};
    source.styles = QStringList{QString()};
    source.title = QStringLiteral("Test pattern");
    source.attribution = QStringLiteral("Mock Agency");
    ts::LayerDefinition definition;
    definition.id = QStringLiteral("wms:test-pattern");
    definition.origin = ts::LayerOrigin::Catalogue;
    definition.type = ts::LayerType::Imagery;
    definition.name = source.title;
    definition.source = source.toJson();
    window.layerModel()->addLayer(definition);
    const QJsonObject state = window.stateReport();
    QVERIFY(state.value(QStringLiteral("attributions"))
                .toArray()
                .contains(QStringLiteral("Mock Agency")));
    QVERIFY(state.value(QStringLiteral("layers"))
                .toArray()
                .contains(QStringLiteral("Test pattern")));
    ts::GlobeView* globe = globeOf(window);
    if (globe == nullptr) {
      QSKIP("No OpenGL view on this platform.");
    }
    QCOMPARE(globe->imageryLayerCount(), 2);
    QVERIFY(QTest::qWaitFor(
        [&]() { return ts::test::mockGetMapCount(server) > 0; }, 10000));
    QVERIFY(QTest::qWaitFor([&]() { return !globe->isLoadingTiles(); }, 20000));
    // Tiles went to the disk cache.
    QVERIFY(
        !QDir(cache.path()).entryList({QStringLiteral("*.sqlite")}).isEmpty());
    const QString shots = qEnvironmentVariable("TS_UI_SCREENSHOTS");
    if (!shots.isEmpty()) {
      QTest::qWait(300);
      globe->grabFramebuffer().save(
          QDir(shots).filePath(QStringLiteral("wms-layer.png")));
    }
  }

  void offlineRegionEndToEnd()
  {
    // A WMS layer's region set up in the dialog, estimated, downloaded
    // and listed; refused for a server whose terms forbid it; deleted.
    std::shared_ptr<ts::test::MockWmsBehaviour> behaviour =
        std::make_shared<ts::test::MockWmsBehaviour>();
    ts::test::MockHttpServer server(ts::test::mockWmsHandler(behaviour));
    QTemporaryDir cache;
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ts::NetworkSettings settings;
    settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    ts::WmsProviderSettings wms;
    wms.cacheDirectory = cache.path();
    window.registry().setWmsNetwork(
        std::make_shared<ts::NetworkClient>(settings), wms);
    ts::OfflineRegionManager& regions = window.offlineRegions();
    regions.setCatalogue([]() { return std::vector<ts::WmsCatalogueEntry>{}; });

    // No WMS layer yet: nothing to keep (a message box says so).
    QTimer::singleShot(0, &window, [&]() {
      for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (QMessageBox* box = qobject_cast<QMessageBox*>(widget)) {
          box->accept();
        }
      }
    });
    QVERIFY(window.newOfflineRegion() == nullptr);

    ts::WmsSource source;
    source.url = server.url(QStringLiteral("/wms"));
    source.layers = QStringList{QStringLiteral("pattern")};
    source.styles = QStringList{QString()};
    source.title = QStringLiteral("Test pattern");
    source.maximumLevel = 12;
    ts::LayerDefinition definition;
    definition.id = QStringLiteral("wms:test-pattern");
    definition.origin = ts::LayerOrigin::Catalogue;
    definition.type = ts::LayerType::Imagery;
    definition.name = source.title;
    definition.source = source.toJson();
    window.layerModel()->addLayer(definition);

    QPointer<ts::NewRegionDialog> dialog = window.newOfflineRegion();
    QVERIFY(dialog);
    QVERIFY(dialog->isVisible());
    const ts::RegionGeometry uk =
        ts::RegionGeometry::rectangle(-10.0, 49.0, 2.0, 61.0);
    dialog->setArea(uk);
    dialog->setLevels(0, 6);
    QCOMPARE(dialog->tileCount(),
             ts::countRegionTiles(uk, 0, 6, std::nullopt).total());
    // Not in the catalogue: a warning, but allowed.
    QLabel* terms =
        dialog->findChild<QLabel*>(QStringLiteral("regionTermsLabel"));
    QVERIFY(terms->isVisibleTo(dialog));
    QVERIFY(terms->text().contains(QStringLiteral("terms of use")));
    QVERIFY(QTest::qWaitFor([&]() { return dialog->estimate().has_value(); },
                            20000));
    QVERIFY(dialog->estimate().value_or(ts::RegionEstimate{}).sampledTiles > 0);
    QVERIFY(dialog->findChild<QLabel*>(QStringLiteral("regionSizeLabel"))
                ->text()
                .contains(QStringLiteral("about")));
    if (ts::GlobeView* globe = globeOf(window)) {
      QCOMPARE(globe->regionOutlines().size(), std::size_t{1});
    }
    QVERIFY(dialog->canDownload());
    const QString shots = qEnvironmentVariable("TS_UI_SCREENSHOTS");
    if (!shots.isEmpty()) {
      dialog->grab().save(
          QDir(shots).filePath(QStringLiteral("new-region-dialog.png")));
    }
    dialog->findChild<QPushButton*>(QStringLiteral("regionDownloadButton"))
        ->click();
    QVERIFY(QTest::qWaitFor([&]() { return dialog.isNull(); }, 5000));
    QCOMPARE(regions.regions().size(), std::size_t{1});
    const long long id = regions.regions().front().id;
    ts::OfflineRegionsDialog* list =
        window.findChild<ts::OfflineRegionsDialog*>();
    QVERIFY(list && list->isVisible());
    QCOMPARE(list->selectedRegion(), id);
    QVERIFY(QTest::qWaitFor([&]() { return !regions.progress(id); }, 60000));
    const ts::OfflineRegion* region = regions.region(id);
    QVERIFY(region->complete);
    QCOMPARE(region->failedTiles, 0LL);
    QCOMPARE(region->tiles,
             ts::countRegionTiles(uk, 0, 6, std::nullopt).total());
    QVERIFY(region->bytes > 0);
    QVERIFY(list->list()->topLevelItem(0)->text(5).startsWith(
        QStringLiteral("Ready offline")));
    if (!shots.isEmpty()) {
      list->grab().save(
          QDir(shots).filePath(QStringLiteral("offline-regions-dialog.png")));
    }
    std::shared_ptr<ts::WmsImageryProvider> provider =
        regions.providerFor(source.toJson(), nullptr);
    QCOMPARE(provider->diskCache()->regionUsage(id).tiles, region->tiles);

    // A server whose terms forbid bulk downloads: refused in the dialog.
    regions.setCatalogue([&]() {
      ts::WmsCatalogueEntry entry;
      entry.title = QStringLiteral("Mock");
      entry.url = server.url(QStringLiteral("/wms"));
      entry.termsUrl = QUrl(QStringLiteral("https://example.org/terms"));
      entry.allowBulkDownload = false;
      return std::vector<ts::WmsCatalogueEntry>{entry};
    });
    dialog = window.newOfflineRegion();
    QVERIFY(dialog);
    dialog->setArea(uk);
    QVERIFY(!dialog->canDownload());
    QVERIFY(dialog->findChild<QLabel*>(QStringLiteral("regionTermsLabel"))
                ->text()
                .contains(QStringLiteral("don't allow")));
    // Too many tiles: refused too.
    regions.setCatalogue([]() { return std::vector<ts::WmsCatalogueEntry>{}; });
    dialog->setArea(ts::RegionGeometry::rectangle(-180.0, -60.0, 180.0, 60.0));
    dialog->setLevels(0, 12);
    QVERIFY(dialog->tileCount() > ts::NewRegionDialog::maximumTiles);
    QVERIFY(!dialog->canDownload());
    // A country from the base pack, by name: the area and the region's
    // name follow it.
    QComboBox* countries =
        dialog->findChild<QComboBox*>(QStringLiteral("regionCountryBox"));
    QVERIFY(countries && countries->isVisibleTo(dialog));
    QCOMPARE(countries->count(), 2);
    QCOMPARE(countries->itemText(0), QStringLiteral("Fixture Atoll"));
    QVERIFY(!dialog->chooseCountry(QStringLiteral("XXX")));
    QVERIFY(dialog->chooseCountry(QStringLiteral("FIX")));
    QCOMPARE(dialog->areaCountry(), QStringLiteral("Fixture Island"));
    QCOMPARE(dialog->region().name, QStringLiteral("Fixture Island"));
    QCOMPARE(countries->currentText(), QStringLiteral("Fixture Island"));
    const ts::RegionGeometry island =
        dialog->area().value_or(ts::RegionGeometry());
    QVERIFY(island.contains(45.0, 15.0));
    QVERIFY(!island.contains(45.0, 25.0));
    dialog->setLevels(0, 6);
    QCOMPARE(dialog->tileCount(),
             ts::countRegionTiles(island, 0, 6, std::nullopt).total());
    QVERIFY(dialog->findChild<QLabel*>(QStringLiteral("regionAreaLabel"))
                ->text()
                .startsWith(QStringLiteral("Fixture Island")));
    // Across the antimeridian, in one piece.
    QVERIFY(dialog->chooseCountry(QStringLiteral("ATL")));
    QCOMPARE(dialog->region().name, QStringLiteral("Fixture Atoll"));
    const ts::RegionGeometry atoll =
        dialog->area().value_or(ts::RegionGeometry());
    QVERIFY(atoll.contains(-7.0, 179.5));
    QVERIFY(atoll.contains(-7.0, -179.5));
    // Another way of choosing the area forgets the country, not the name.
    dialog->setArea(uk);
    QVERIFY(dialog->areaCountry().isEmpty());
    QCOMPARE(countries->currentIndex(), -1);
    QCOMPARE(dialog->region().name, QStringLiteral("Fixture Atoll"));
    dialog->reject();

    // Deleting unpins its tiles.
    regions.remove(id);
    QVERIFY(regions.regions().empty());
    QCOMPARE(provider->diskCache()->regionUsage(id).tiles, 0LL);
    QCOMPARE(list->list()->topLevelItemCount(), 0);
  }

  void dataPacksEndToEnd()
  {
    // A pack replacing Blue Marble, listed from the index, installed (the
    // globe credits it), updated and removed (the base pack's is back).
    const QTemporaryDir packsDirectory;
    qputenv("TS_PACKS_DIR", packsDirectory.path().toUtf8());
    const QTemporaryDir full;
    ts::test::writeFixtureBasePack(full.path(), QColor(200, 100, 50));
    QFile tilesFile(
        QDir(full.path()).filePath(QStringLiteral("bluemarble.sqlite")));
    QVERIFY(tilesFile.open(QIODevice::ReadOnly));
    const QByteArray tiles = tilesFile.readAll();
    const QString id = QStringLiteral("bluemarble-full");
    ts::test::MockHttpServer server;
    const ts::test::RemotePack v1 =
        ts::test::remotePack(server, id, QStringLiteral("1"), tiles);
    const ts::test::RemotePack v2 =
        ts::test::remotePack(server, id, QStringLiteral("2"), tiles);
    ts::test::Served files = v1.files;
    files.insert(v2.files.cbegin(), v2.files.cend());
    const std::function<void(const ts::PackIndexEntry&)> serveIndexOf =
        [&server, &files](const ts::PackIndexEntry& entry) {
          files[QStringLiteral("/index.json")] =
              QJsonDocument(QJsonObject{{QStringLiteral("format"), 1},
                                        {QStringLiteral("packs"),
                                         QJsonArray{entry.toJson()}}})
                  .toJson();
          server.setHandler(
              [served = files](const ts::test::MockHttpRequest& request) {
                return ts::test::serve(served, request, true);
              });
        };
    serveIndexOf(v1.entry);

    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ts::NetworkSettings settings;
    settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    window.registry().setWmsNetwork(
        std::make_shared<ts::NetworkClient>(settings),
        ts::WmsProviderSettings());
    ts::PackManager& packs = window.packs();
    packs.setIndexUrl(server.url(QStringLiteral("/index.json")));
    packs.setAllowLocalHttp(true);
    QVERIFY(packs.installed().empty());

    ts::PackManagerDialog* dialog = window.showDataPacks();
    QVERIFY(QTest::qWaitFor(
        [&]() { return dialog->list()->topLevelItemCount() == 1; }, 10000));
    dialog->selectPack(id);
    QCOMPARE(dialog->row(id).state, QStringLiteral("Not installed"));
    QCOMPARE(dialog->list()->topLevelItem(0)->text(0),
             QStringLiteral("Test pack"));
    QPushButton* action =
        dialog->findChild<QPushButton*>(QStringLiteral("packActionButton"));
    QPushButton* remove =
        dialog->findChild<QPushButton*>(QStringLiteral("packRemoveButton"));
    QCOMPARE(withoutMnemonic(action->text()), QStringLiteral("Install"));
    QVERIFY(!remove->isEnabled());

    action->click();
    QVERIFY(QTest::qWaitFor([&]() { return packs.installed().size() == 1; },
                            20000));
    QCOMPARE(dialog->row(id).state, QStringLiteral("Installed"));
    QVERIFY(remove->isEnabled());
    ts::GlobeView* globe = globeOf(window);
    if (globe != nullptr) {
      QVERIFY(
          globe->attributions().contains(QStringLiteral("Full attribution")));
    }

    // The index lists version 2: an update.
    serveIndexOf(v2.entry);
    dialog->findChild<QPushButton*>(QStringLiteral("packRefreshButton"))
        ->click();
    QVERIFY(QTest::qWaitFor(
        [&]() {
          return dialog->row(id).action ==
                 ts::PackManagerDialog::Action::Update;
        },
        10000));
    QCOMPARE(withoutMnemonic(action->text()), QStringLiteral("Update"));
    action->click();
    QVERIFY(QTest::qWaitFor(
        [&]() {
          return !packs.installed().empty() &&
                 packs.installed().front().manifest.version ==
                     QStringLiteral("2");
        },
        20000));

    // Removing it (after the question) brings the base pack's back.
    QTimer::singleShot(0, &window, [&]() {
      for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (QMessageBox* box = qobject_cast<QMessageBox*>(widget)) {
          box->button(QMessageBox::Yes)->click();
        }
      }
    });
    remove->click();
    QVERIFY(packs.installed().empty());
    QCOMPARE(dialog->row(id).state, QStringLiteral("Not installed"));
    QDirIterator left(packsDirectory.path(), QDir::Files | QDir::Hidden,
                      QDirIterator::Subdirectories);
    QVERIFY2(!left.hasNext(),
             qPrintable(left.hasNext() ? left.next() : QString()));
    if (globe != nullptr) {
      QVERIFY(
          globe->attributions().contains(QStringLiteral("Fixture imagery")));
    }
    qunsetenv("TS_PACKS_DIR");
  }

  void drawRegionOnTheGlobe()
  {
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ts::GlobeView* globe = globeOf(window);
    if (globe == nullptr) {
      QSKIP("No OpenGL view on this platform.");
    }
    QVERIFY(
        QTest::qWaitFor([&]() { return globe->framesRendered() > 0; }, 5000));
    QSignalSpy drawn(globe, &ts::GlobeView::regionDrawn);
    QSignalSpy cancelled(globe, &ts::GlobeView::regionDrawingCancelled);
    const QPointF centre(globe->width() / 2.0, globe->height() / 2.0);

    // A rectangle: drag from one corner to the other; the camera stays.
    const ts::CameraPose before = globe->controller().camera().pose();
    globe->startRegionDrawing(ts::GlobeView::RegionDrawing::Rectangle);
    sendMouse(globe, QEvent::MouseButtonPress, centre - QPointF(60, 40),
              Qt::LeftButton, Qt::LeftButton);
    sendMouse(globe, QEvent::MouseMove, centre, Qt::NoButton, Qt::LeftButton);
    sendMouse(globe, QEvent::MouseButtonRelease, centre + QPointF(60, 40),
              Qt::LeftButton, Qt::NoButton);
    QCOMPARE(drawn.count(), 1);
    const ts::RegionGeometry rectangle =
        drawn.at(0).at(0).value<ts::RegionGeometry>();
    QCOMPARE(rectangle.kind(), ts::RegionGeometry::Kind::Rectangle);
    const std::optional<ts::Geodetic> picked =
        globe->controller().camera().pickGeodetic(centre.x(), centre.y());
    QVERIFY(picked);
    const ts::Geodetic middle = picked.value_or(ts::Geodetic{});
    QVERIFY(rectangle.contains(middle.latDeg, middle.lonDeg));
    QCOMPARE(globe->controller().camera().pose().targetLonDeg,
             before.targetLonDeg);
    QCOMPARE(globe->regionDrawing(), ts::GlobeView::RegionDrawing::None);

    // A polygon: three clicks, then a double-click on the fourth corner.
    globe->startRegionDrawing(ts::GlobeView::RegionDrawing::Polygon);
    const std::vector<QPointF> corners{
        centre + QPointF(-50, -50), centre + QPointF(50, -50),
        centre + QPointF(60, 50), centre + QPointF(-40, 40)};
    for (const QPointF corner : corners) {
      sendMouse(globe, QEvent::MouseButtonPress, corner, Qt::LeftButton,
                Qt::LeftButton);
      sendMouse(globe, QEvent::MouseButtonRelease, corner, Qt::LeftButton,
                Qt::NoButton);
    }
    sendMouse(globe, QEvent::MouseButtonDblClick, corners.back(),
              Qt::LeftButton, Qt::LeftButton);
    QCOMPARE(drawn.count(), 2);
    const ts::RegionGeometry polygon =
        drawn.at(1).at(0).value<ts::RegionGeometry>();
    QCOMPARE(polygon.kind(), ts::RegionGeometry::Kind::Polygon);
    QCOMPARE(polygon.vertices().size(), std::size_t{4});
    QVERIFY(polygon.contains(middle.latDeg, middle.lonDeg));

    // Esc cancels.
    globe->startRegionDrawing(ts::GlobeView::RegionDrawing::Polygon);
    QTest::keyClick(globe, Qt::Key_Escape);
    QCOMPARE(cancelled.count(), 1);
    QCOMPARE(globe->regionDrawing(), ts::GlobeView::RegionDrawing::None);

    // The visible area holds the centre of the view.
    const std::optional<ts::RegionGeometry> visible = globe->visibleArea();
    QVERIFY(visible);
    QVERIFY(visible.value_or(ts::RegionGeometry())
                .contains(middle.latDeg, middle.lonDeg));
    const QString shots = qEnvironmentVariable("TS_UI_SCREENSHOTS");
    if (!shots.isEmpty()) {
      globe->setRegionOutlines({rectangle, polygon});
      globe->startRegionDrawing(ts::GlobeView::RegionDrawing::Polygon);
      QTest::qWait(300);
      globe->grabFramebuffer().save(
          QDir(shots).filePath(QStringLiteral("region-outlines.png")));
    }
  }

  void idleViewStopsDrawing()
  {
    // Render on demand: once nothing moves, no more frames are drawn.
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ts::GlobeView* globe = globeOf(window);
    if (globe == nullptr) {
      QSKIP("No OpenGL view on this platform.");
    }
    QVERIFY(
        QTest::qWaitFor([&]() { return globe->framesRendered() > 0; }, 5000));
    // The checkerboard imagery loads first; frames keep coming until then.
    QVERIFY(QTest::qWaitFor([&]() { return !globe->isLoadingTiles(); }, 20000));
    // The base pack check ends in a status bar change, which may redraw the
    // view once more (slow on Windows runners); wait for it first.
    QVERIFY(QTest::qWaitFor(
        [&]() {
          return window.basePackState() !=
                 ts::MainWindow::BasePackState::Checking;
        },
        20000));
    QVERIFY2(waitUntilQuiet(globe), "the view kept drawing");
    // Drawing continuously would be dozens of frames a second; one late
    // repaint (the window system exposing it again, a status bar change
    // resizing it, both seen on slow Windows runners) is not that.
    const int frames = globe->framesRendered();
    QTest::qWait(1000);
    QVERIFY2(globe->framesRendered() <= frames + 1,
             qPrintable(QStringLiteral("%1 frames drawn while idle")
                            .arg(globe->framesRendered() - frames)));
  }
};

QTEST_MAIN(TestMainWindow)
#include "tst_mainwindow.moc"
