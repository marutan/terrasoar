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
    // Installed and intact: Blue Marble by default, "Ready" throughout.
    {
      ts::MainWindow window;
      QCOMPARE(window.basePackDirectory(), QDir::cleanPath(m_basePack.path()));
      QCOMPARE(window.layerModel()->rowCount(), 1);
      QCOMPARE(window.layerModel()
                   ->layerDefinition(window.layerModel()->index(0, 0))
                   .id,
               QStringLiteral("builtin:bluemarble"));
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
      QCOMPARE(window.layerModel()
                   ->layerDefinition(window.layerModel()->index(0, 0))
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
    // A new project shows Blue Marble from the base pack.
    QCOMPARE(model->rowCount(), 1);
    ts::GlobeView* globe = globeOf(window);
    if (globe != nullptr) {
      QCOMPARE(globe->imageryLayerCount(), 1);
    }
    // Hiding it takes it off the globe at once (the next frame).
    const QModelIndex layer = model->index(0, 0);
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
    QCOMPARE(model->rowCount(), 2);
    QVERIFY(
        model->data(model->index(0, 0), ts::LayerTreeModel::IsCollectionRole)
            .toBool());
    view->setCurrentIndex(model->index(0, 0));
    remove->click();
    QCOMPARE(model->rowCount(), 1);
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
