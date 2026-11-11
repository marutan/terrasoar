// SPDX-License-Identifier: GPL-2.0-or-later
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontMetrics>
#include <QJsonArray>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMouseEvent>
#include <QPointer>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QTemporaryDir>
#include <QTreeView>
#include <QWheelEvent>
#include <QtTest>

#include "fixturepack.h"
#include "globeview.h"
#include "mainwindow.h"
#include "ts/i18n/pseudotranslator.h"
#include "ts/layers/layertreemodel.h"
#include "ts/projection/projection.h"

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
QList<QPair<QString, QString>> visibleTexts(QMainWindow& window)
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
    texts << qMakePair(describe(label) + QStringLiteral(" text"),
                       label->text());
  }
  return texts;
}

void verifyAllPseudo(QMainWindow& window)
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
    QVERIFY(m_basePack.isValid());
    ts::test::writeFixtureBasePack(m_basePack.path());
    qputenv("TS_BASEPACK_DIR", m_basePack.path().toUtf8());
  }

  void basePackStates()
  {
    // Installed and intact: graticule, shoreline and Blue Marble by
    // default (top to bottom), their sources credited, "Ready" throughout.
    {
      ts::MainWindow window;
      QCOMPARE(window.basePackDirectory(), QDir::cleanPath(m_basePack.path()));
      const ts::LayerTreeModel* model = window.layerModel();
      QCOMPARE(model->rowCount(), 3);
      const QStringList expected = {QStringLiteral("builtin:graticule"),
                                    QStringLiteral("builtin:shoreline"),
                                    QStringLiteral("builtin:bluemarble")};
      for (int row = 0; row < 3; ++row) {
        QCOMPARE(model->layerDefinition(model->index(row, 0)).id,
                 expected.at(row));
      }
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
    // A new project shows the graticule and shoreline over Blue Marble,
    // and the dock says vectors stay above imagery.
    QCOMPARE(model->rowCount(), 3);
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
    QCOMPARE(model->rowCount(), 4);
    QVERIFY(
        model->data(model->index(2, 0), ts::LayerTreeModel::IsCollectionRole)
            .toBool());
    view->setCurrentIndex(model->index(2, 0));
    remove->click();
    QCOMPARE(model->rowCount(), 3);
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
    QTest::qWait(200);
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
    QTest::qWait(200);
    const int frames = globe->framesRendered();
    QTest::qWait(500);
    QCOMPARE(globe->framesRendered(), frames);
  }
};

QTEST_MAIN(TestMainWindow)
#include "tst_mainwindow.moc"
