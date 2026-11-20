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

#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QDoubleSpinBox>
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
#include <QMetaEnum>
#include <QMouseEvent>
#include <QNetworkProxy>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QSignalSpy>
#include <QSlider>
#include <QSpinBox>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyleHints>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeView>
#include <QTreeWidget>
#include <QUrlQuery>
#include <QWheelEvent>
#include <QtTest>

#include "aboutdialog.h"
#include "addwebterraindialog.h"
#include "addwfsdialog.h"
#include "addwmsdialog.h"
#include "addwmtsdialog.h"
#include "addxyzdialog.h"
#include "appsettings.h"
#include "balloonwindow.h"
#include "elevationprofiledialog.h"
#include "featureinfowindow.h"
#include "fixturepack.h"
#include "globeview.h"
#include "helpwindow.h"
#include "layertreedock.h"
#include "mainwindow.h"
#include "mockhttpserver.h"
#include "mockpacks.h"
#include "mockwfs.h"
#include "mockwms.h"
#include "mockwmts.h"
#include "newregiondialog.h"
#include "offlineregionmanager.h"
#include "offlineregionsdialog.h"
#include "packmanager.h"
#include "packmanagerdialog.h"
#include "projectionsettingsdialog.h"
#include "timebar.h"
#include "timemodebox.h"
#include "ts/globe/layerstackdraws.h"
#include "ts/i18n/pseudotranslator.h"
#include "ts/layers/layertreemodel.h"
#include "ts/network/networkclient.h"
#include "ts/network/regiontilesource.h"
#include "ts/network/tilediskcache.h"
#include "ts/projection/projection.h"
#include "ts/providers/providerregistry.h"
#include "ts/time/layertime.h"
#include "ts/time/programclock.h"
#include "ts/time/timeextent.h"
#include "ts/vector/vectorstyle.h"
#include "ts/wms/wmscapabilities.h"
#include "ts/wms/wmsimageryprovider.h"
#include "ts/wms/wmssource.h"
#include "userguide.h"
#include "wmscataloguefile.h"

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
 * Records what reaches a main window and its globe view while the view
 * should be idle (events that can make it draw, status bar messages, the
 * base pack check and tile loading), with the time and the frame count,
 * for a test to print when frames were drawn anyway.
 *
 * Recording must not change what it records: a slow call here would let
 * several update requests merge into one frame. So each entry is only a
 * few numbers, taken with cheap getters; the text is made at the end.
 */
class IdleWitness : public QObject
{
  Q_OBJECT

public:
  IdleWitness(ts::MainWindow& window, ts::GlobeView& globe)
      : m_window(window), m_globe(globe)
  {
    m_entries.reserve(256);
    m_clock.start();
    note(Entry::Start, QEvent::None, false);
    m_window.installEventFilter(this);
    m_globe.installEventFilter(this);
    connect(m_window.statusBar(), &QStatusBar::messageChanged, this,
            [this](const QString& message) {
              m_messages << message;
              note(Entry::StatusMessage, QEvent::None, false);
            });
  }

  ~IdleWitness() override
  {
    m_window.removeEventFilter(this);
    m_globe.removeEventFilter(this);
  }

  IdleWitness(const IdleWitness&) = delete;
  IdleWitness& operator=(const IdleWitness&) = delete;
  IdleWitness(IdleWitness&&) = delete;
  IdleWitness& operator=(IdleWitness&&) = delete;

  /**
   * Returns how often the main window was activated or deactivated. The
   * window system does that (another window taking the focus); each time
   * Qt repaints the whole window, the globe view included.
   */
  int32_t activationChanges() const
  {
    return static_cast<int32_t>(std::count_if(
        m_entries.begin(), m_entries.end(), [](const Entry& entry) {
          return entry.kind == Entry::Event &&
                 entry.event == QEvent::ActivationChange && !entry.onGlobe;
        }));
  }

  /** Returns the record, one line per entry, ending with the state now. */
  QString report()
  {
    note(Entry::End, QEvent::None, false);
    QStringList lines;
    int32_t message = 0;
    for (const Entry& entry : m_entries) {
      QString what;
      switch (entry.kind) {
      case Entry::Start:
        what = QStringLiteral("start");
        break;
      case Entry::End:
        what = QStringLiteral("end");
        break;
      case Entry::StatusMessage:
        what = QStringLiteral("status bar message \"%1\"")
                   .arg(m_messages.value(message++));
        break;
      case Entry::Event:
        what = QStringLiteral("%1 on %2")
                   .arg(QString::fromLatin1(
                            QMetaEnum::fromType<QEvent::Type>().valueToKey(
                                entry.event)),
                        entry.onGlobe ? QStringLiteral("globe view")
                                      : QStringLiteral("main window"));
        break;
      }
      lines << QStringLiteral("  %1 ms, frame %2, %3 (base pack %4, %5)")
                   .arg(entry.ms, 5)
                   .arg(entry.frames)
                   .arg(what, basePackName(entry.basePack),
                        entry.loading ? QStringLiteral("loading tiles")
                                      : QStringLiteral("not loading"));
    }
    return lines.join(QLatin1Char('\n'));
  }

protected:
  bool eventFilter(QObject* watched, QEvent* event) override
  {
    switch (event->type()) {
    case QEvent::Paint:
    case QEvent::UpdateRequest:
    case QEvent::UpdateLater:
    case QEvent::Resize:
    case QEvent::Move:
    case QEvent::Show:
    case QEvent::Hide:
    case QEvent::Expose:
    case QEvent::WindowActivate:
    case QEvent::WindowDeactivate:
    case QEvent::ActivationChange:
    case QEvent::PaletteChange:
    case QEvent::StyleChange:
    case QEvent::FontChange:
    case QEvent::LayoutRequest:
    case QEvent::WindowStateChange:
    case QEvent::Enter:
    case QEvent::Leave:
    case QEvent::HoverMove:
    case QEvent::MouseMove:
    case QEvent::Timer:
      note(Entry::Event, event->type(), watched == &m_globe);
      break;
    default:
      break;
    }
    return false;
  }

private:
  /** One thing that happened. */
  struct Entry {
    enum Kind { Start, End, StatusMessage, Event };
    Kind kind;
    QEvent::Type event;
    bool onGlobe;
    int64_t ms;
    int32_t frames;
    ts::MainWindow::BasePackState basePack;
    bool loading;
  };

  static QString basePackName(ts::MainWindow::BasePackState state)
  {
    switch (state) {
    case ts::MainWindow::BasePackState::Missing:
      return QStringLiteral("missing");
    case ts::MainWindow::BasePackState::Checking:
      return QStringLiteral("checking");
    case ts::MainWindow::BasePackState::Ok:
      return QStringLiteral("ok");
    case ts::MainWindow::BasePackState::Damaged:
      return QStringLiteral("damaged");
    }
    return QString();
  }

  void note(Entry::Kind kind, QEvent::Type event, bool onGlobe)
  {
    m_entries.push_back(
        Entry{kind, event, onGlobe, m_clock.elapsed(), m_globe.framesRendered(),
              m_window.basePackState(), m_globe.isLoadingTiles()});
  }

  ts::MainWindow& m_window;
  ts::GlobeView& m_globe;
  QElapsedTimer m_clock;
  std::vector<Entry> m_entries;
  QStringList m_messages;
};

/**
 * Waits until @p globe has drawn no frame for 300 ms (the frame for the last
 * tile can still be on its way on a slow software renderer). False if it is
 * still drawing after 10 s.
 */
bool waitUntilQuiet(const ts::GlobeView* globe)
{
  int32_t frames = globe->framesRendered();
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

/**
 * Presses @p dialog's F1 shortcut and returns true if the user guide opens
 * at @p target, in a window of the dialog's.
 */
bool helpKeyOpens(QDialog* dialog, const QString& target)
{
  QShortcut* f1 = nullptr;
  for (QShortcut* shortcut : dialog->findChildren<QShortcut*>()) {
    if (shortcut->key() == QKeySequence(QKeySequence::HelpContents)) {
      f1 = shortcut;
    }
  }
  if (f1 == nullptr) {
    return false;
  }
  dialog->show();
  emit f1->activated();
  const ts::HelpWindow* shown = dialog->findChild<ts::HelpWindow*>();
  const bool opened = shown != nullptr &&
                      shown->currentPage() == ts::userguide::pageUrl(target);
  dialog->hide();
  return opened;
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
    qputenv("TS_XYZ_CATALOGUE_URL", QByteArray());
    qputenv("TS_WFS_CATALOGUE_URL", QByteArray());
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
      for (int32_t row = 0; row < 4; ++row) {
        QCOMPARE(model->layerDefinition(model->index(row, 0)).id,
                 expected.at(row));
      }
      QCOMPARE(model->data(model->index(3, 0), Qt::CheckStateRole).toInt(),
               static_cast<int32_t>(Qt::Unchecked));
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
    for (int32_t i = 0; i < 20; ++i) {
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
    if (!quit) {
      QFAIL("quit is missing");
    }
    quit->trigger();
    QVERIFY(!window.isVisible());
  }

  void englishTexts()
  {
    ts::MainWindow window;
    QCOMPARE(window.windowTitle(), QStringLiteral("Terra Soar"));
    QLabel* status = window.findChild<QLabel*>(QStringLiteral("statusLabel"));
    if (!status) {
      QFAIL("status is missing");
    }
    QCOMPARE(status->text(), QStringLiteral("Ready"));
    QMenu* file = window.findChild<QMenu*>(QStringLiteral("fileMenu"));
    if (!file) {
      QFAIL("file is missing");
    }
    QCOMPARE(file->title(), QStringLiteral("&File"));
    QAction* about = window.findChild<QAction*>(QStringLiteral("aboutAction"));
    QCOMPARE(about->text(), QStringLiteral("&About Terra Soar"));
  }

  void userGuideFromTheProgram()
  {
    // Help > User Guide (F1), above About, opens the guide at its contents.
    QFile::remove(ts::settingsFilePath());
    ts::MainWindow window;
    QAction* guide =
        window.findChild<QAction*>(QStringLiteral("userGuideAction"));
    QMenu* help = window.findChild<QMenu*>(QStringLiteral("helpMenu"));
    if (guide == nullptr || help == nullptr) {
      QFAIL("Help menu controls missing");
    }
    QCOMPARE(guide->text(), QStringLiteral("&User Guide"));
    QCOMPARE(guide->shortcut(), QKeySequence(QKeySequence::HelpContents));
    const QList<QAction*> actions = help->actions();
    QVERIFY(actions.indexOf(guide) < actions.indexOf(window.findChild<QAction*>(
                                         QStringLiteral("aboutAction"))));
    guide->trigger();
    ts::HelpWindow* guideWindow = window.findChild<ts::HelpWindow*>();
    if (guideWindow == nullptr) {
      QFAIL("guideWindow missing");
    }
    QVERIFY(guideWindow->isVisible());
    QVERIFY(guideWindow->isWindow());
    QCOMPARE(guideWindow->currentPage().fileName(),
             QStringLiteral("README.md"));
    guideWindow->close();

    // The About tab links to it; the link closes the (modal) About window
    // for the main window to open the guide.
    ts::AboutDialog about(&window, {});
    QLabel* label = about.findChild<QLabel*>(QStringLiteral("aboutLabel"));
    if (label == nullptr) {
      QFAIL("label missing");
    }
    QVERIFY(label->text().contains(ts::AboutDialog::userGuideLink()));
    QVERIFY(!about.userGuideRequested());
    emit label->linkActivated(ts::AboutDialog::userGuideLink());
    QVERIFY(about.userGuideRequested());
    QCOMPARE(about.result(), static_cast<int32_t>(QDialog::Accepted));

    // F1 in a dialog opens the guide at that dialog's section.
    ts::NetworkSettings settings;
    settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    ts::AddWmsDialog wms(std::make_shared<ts::NetworkClient>(settings),
                         nullptr);
    ts::AddWmtsDialog wmts(std::make_shared<ts::NetworkClient>(settings),
                           nullptr);
    ts::AddXyzDialog xyz(std::make_shared<ts::NetworkClient>(settings),
                         nullptr);
    ts::AddWebTerrainDialog terrain(nullptr);
    ts::FeatureInfoWindow featureInfo(
        std::make_shared<ts::NetworkClient>(settings), nullptr);
    ts::AddWfsDialog wfsDialog(std::make_shared<ts::NetworkClient>(settings),
                               nullptr);
    const std::vector<std::pair<QDialog*, QString>> dialogs{
        {&featureInfo, ts::FeatureInfoWindow::helpTarget()},
        {&wfsDialog, ts::AddWfsDialog::helpTarget()},
        {&wms, ts::AddWmsDialog::helpTarget()},
        {&wmts, ts::AddWmtsDialog::helpTarget()},
        {&xyz, ts::AddXyzDialog::helpTarget()},
        {&terrain, ts::AddWebTerrainDialog::helpTarget()}};
    for (const std::pair<QDialog*, QString>& entry : dialogs) {
      QVERIFY2(helpKeyOpens(entry.first, entry.second),
               qPrintable(entry.second));
    }
  }

  void aboutDialog()
  {
    // Help > About: the version, the data in use with its sources and
    // licences, the third-party notices and the GPL, all built in.
    ts::MainWindow window;
    const std::vector<ts::AboutDataRow> data = window.aboutData();
    QVERIFY(!data.empty());
    bool elevation = false;
    for (const ts::AboutDataRow& row : data) {
      QCOMPARE(row.pack, QStringLiteral("Base pack"));
      elevation =
          elevation || row.attribution == QLatin1String("Fixture elevation");
    }
    QVERIFY(elevation);
    ts::AboutDialog dialog(&window, data);
    QLabel* label = dialog.findChild<QLabel*>(QStringLiteral("aboutLabel"));
    QTreeWidget* tree =
        dialog.findChild<QTreeWidget*>(QStringLiteral("aboutDataTree"));
    if (label == nullptr || tree == nullptr) {
      QFAIL("about dialog controls missing");
    }
    QVERIFY(label->text().contains(QStringLiteral(TS_VERSION_STRING)));
    QVERIFY(label->text().contains(ts::AboutDialog::homePage()));
    QCOMPARE(tree->topLevelItemCount(), static_cast<int32_t>(data.size()));
    const QString shots = qEnvironmentVariable("TS_UI_SCREENSHOTS");
    if (!shots.isEmpty()) {
      dialog.show();
      dialog.grab().save(QDir(shots).filePath(QStringLiteral("about.png")));
      dialog.findChild<QTabWidget*>(QStringLiteral("aboutTabs"))
          ->setCurrentIndex(1);
      dialog.grab().save(
          QDir(shots).filePath(QStringLiteral("about-data.png")));
    }
    // The notices and licence as shipped (the licence audit keeps the
    // notices complete: tools/checks/check_notices.py).
    QVERIFY(ts::AboutDialog::thirdPartyNotices().contains(
        QStringLiteral("GeographicLib")));
    QVERIFY(ts::AboutDialog::thirdPartyNotices().contains(
        QStringLiteral("uk_os_OSGM15_GB.tif")));
    QVERIFY(ts::AboutDialog::licenceText().contains(
        QStringLiteral("GNU GENERAL PUBLIC LICENSE")));

    ts::PseudoTranslator translator;
    QCoreApplication::installTranslator(&translator);
    {
      ts::AboutDialog pseudo(nullptr, data);
      QVERIFY(ts::PseudoTranslator::isPseudoTranslated(pseudo.windowTitle()));
      QLabel* text = pseudo.findChild<QLabel*>(QStringLiteral("aboutLabel"));
      QVERIFY(text != nullptr &&
              ts::PseudoTranslator::isPseudoTranslated(text->text()));
    }
    QCoreApplication::removeTranslator(&translator);
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
      const int32_t needed = label->fontMetrics().horizontalAdvance(shown);
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
      const int32_t needed =
          bar->fontMetrics().horizontalAdvance(withoutMnemonic(action->text()));
      QVERIFY2(geometry.width() >= needed && bar->rect().contains(geometry),
               qPrintable(action->text()));
      QMenu* menu = action->menu();
      if (menu == nullptr) {
        continue;
      }
      const QList<QAction*> items = menu->actions();
      for (QAction* item : items) {
        const int32_t itemNeeded = menu->fontMetrics().horizontalAdvance(
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

  void attributionsToggle()
  {
    // View → Attributions, next to Scale Bar, is on by default and hides
    // and shows the credits in the corner of the view.
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QAction* attributions =
        window.findChild<QAction*>(QStringLiteral("attributionsAction"));
    QAction* scaleBar =
        window.findChild<QAction*>(QStringLiteral("scaleBarAction"));
    QMenu* view = window.findChild<QMenu*>(QStringLiteral("viewMenu"));
    if (attributions == nullptr || scaleBar == nullptr || view == nullptr) {
      QFAIL("view actions missing");
    }
    const QList<QAction*> actions = view->actions();
    QCOMPARE(actions.indexOf(attributions), actions.indexOf(scaleBar) + 1);
    QVERIFY(attributions->isCheckable() && attributions->isChecked());
    ts::GlobeView* globe = globeOf(window);
    if (globe == nullptr) {
      QSKIP("No OpenGL view on this platform.");
    }
    QVERIFY(globe->isAttributionsVisible());
    attributions->trigger();
    QVERIFY(!attributions->isChecked());
    QVERIFY(!globe->isAttributionsVisible());
    const int32_t frames = globe->framesRendered();
    QVERIFY(QTest::qWaitFor([&]() { return globe->framesRendered() > frames; },
                            5000));
    attributions->trigger();
    QVERIFY(globe->isAttributionsVisible());
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
    const int32_t frames = globe->framesRendered();
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
    // The map's hillshade, on by default, is kept in the style too.
    QAction* hillshade =
        window.findChild<QAction*>(QStringLiteral("hillshadeAction"));
    if (hillshade == nullptr) {
      QFAIL("hillshade action missing");
    }
    QVERIFY(hillshade->isEnabled() && hillshade->isChecked());
    hillshade->trigger();
    QVERIFY(!ts::terrainHillshade(model->layerDefinition(terrain)));
    QVERIFY(!hillshade->isChecked());
    hillshade->trigger();
    QVERIFY(ts::terrainHillshade(model->layerDefinition(terrain)));
    // Without an elevation layer the menu is off.
    const int32_t rows = model->rowCount();
    QVERIFY(model->removeNode(terrain));
    QCOMPARE(model->rowCount(), rows - 1);
    QVERIFY(!triple->isEnabled() && !hillshade->isEnabled());
    if (globe != nullptr) {
      QVERIFY(!globe->terrainShown());
    }
  }

  void imageryFileLayer()
  {
    // A raster file becomes a visible imagery layer at the top; an RPF
    // A.TOC is named after the disc or folder it is on, as RPF's own
    // name says nothing.
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    QVERIFY(window.findChild<QAction*>(QStringLiteral("addImageryAction")));
    QVERIFY(window.findChild<QAction*>(QStringLiteral("addImageryFileAction")));
    ts::LayerTreeModel* model = window.layerModel();
    const int32_t rows = model->rowCount();
    QString error;
    QVERIFY(window.addImageryLayer(
        QStringLiteral(TS_TEST_DATA_DIR "/rpf/zone2/RPF/A.TOC"), &error));
    QCOMPARE(model->rowCount(), rows + 1);
    const QModelIndex added = model->index(0, 0);
    const ts::LayerDefinition definition = model->layerDefinition(added);
    QCOMPARE(definition.type, ts::LayerType::Imagery);
    QCOMPARE(definition.name, QStringLiteral("zone2"));
    QCOMPARE(definition.source.value(QStringLiteral("kind")).toString(),
             QStringLiteral("file"));
    QCOMPARE(model->data(added, Qt::CheckStateRole).toInt(),
             static_cast<int32_t>(Qt::Checked));
    // A file that can't be used: no layer, and why.
    QVERIFY(!window.addImageryLayer(
        QStringLiteral(TS_TEST_DATA_DIR "/sample.wkt"), &error));
    QVERIFY(error.contains(QLatin1String("sample.wkt")));
    QCOMPARE(model->rowCount(), rows + 1);
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
    const int32_t rows = model->rowCount();
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
             static_cast<int32_t>(Qt::Checked));
    QCOMPARE(model->layerDefinition(model->index(rows, 0)).id,
             QStringLiteral("builtin:terrain"));

    ts::GlobeView* globe = globeOf(window);
    if (globe == nullptr) {
      return;
    }
    QVERIFY(globe->terrainShown());
    QLabel* pointer = window.findChild<QLabel*>(QStringLiteral("pointerLabel"));
    if (!pointer) {
      QFAIL("pointer is missing");
    }
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

  void elevationProfile()
  {
    // View → Elevation Profile: draw a line, see the heights along it.
    const QTemporaryDir directory;
    const QString dem = directory.filePath(QStringLiteral("hills.tif"));
    QVERIFY(ts::test::writeFixtureDem(dem, 1500.0F));
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ts::GlobeView* globe = globeOf(window);
    QAction* action =
        window.findChild<QAction*>(QStringLiteral("profileAction"));
    if (action == nullptr) {
      QFAIL("profile action missing");
    }
    if (globe == nullptr) {
      QVERIFY(!action->isEnabled());
      QSKIP("No OpenGL view on this platform.");
    }
    QVERIFY(action->isEnabled());
    window.addElevationLayer(dem, QStringLiteral("ellipsoid"));

    // Drawing: click the points, Enter finishes.
    action->trigger();
    QPointer<ts::ElevationProfileDialog> dialog =
        window.findChild<ts::ElevationProfileDialog*>();
    QVERIFY(dialog && dialog->isVisible());
    QCOMPARE(globe->regionDrawing(), ts::GlobeView::RegionDrawing::Line);
    QSignalSpy drawn(globe, &ts::GlobeView::lineDrawn);
    const QPointF centre(globe->width() / 2.0, globe->height() / 2.0);
    for (const QPointF& at :
         {centre - QPointF(20.0, 0.0), centre + QPointF(20.0, 0.0)}) {
      sendMouse(globe, QEvent::MouseButtonPress, at, Qt::LeftButton,
                Qt::LeftButton);
      sendMouse(globe, QEvent::MouseButtonRelease, at, Qt::LeftButton,
                Qt::NoButton);
    }
    QTest::keyClick(globe, Qt::Key_Return);
    QCOMPARE(drawn.size(), 1);
    QCOMPARE(globe->regionDrawing(), ts::GlobeView::RegionDrawing::None);
    QCOMPARE(globe->profileLine().size(), std::size_t{2});

    // A line across the DEM (1500 m above the ellipsoid; the fixture pack
    // has no geoid, so sea level is the ellipsoid).
    window.profileLine({ts::LonLat{9.0, 19.0}, ts::LonLat{11.0, 21.0}});
    QVERIFY(QTest::qWaitFor(
        [&]() { return dialog->chart()->profile().has_value(); }, 20000));
    const std::optional<ts::ElevationProfile> shown =
        dialog->chart()->profile();
    if (!shown) {
      QFAIL("no profile");
    }
    const ts::ElevationProfile& profile = *shown;
    QCOMPARE(profile.points.size(),
             static_cast<std::size_t>(ts::MainWindow::profileSamples));
    QVERIFY(std::abs(profile.minimumM - 1500.0) < 0.5);
    QVERIFY(std::abs(profile.maximumM - 1500.0) < 0.5);
    QVERIFY(profile.lengthM > 300000.0 && profile.lengthM < 320000.0);
    QVERIFY2(dialog->summaryText().contains(QLocale().toString(1500)),
             qPrintable(dialog->summaryText()));
    const QByteArray csv = ts::ElevationProfileDialog::toCsv(profile);
    QVERIFY(csv.startsWith("distance_m,latitude,longitude,height_m,"));
    QCOMPARE(csv.count('\n'), ts::MainWindow::profileSamples + 1);
    QVERIFY(
        csv.split('\n').at(1).startsWith("0.0,19.0000000,9.0000000,1500.0"));

    // Pointing at the chart marks the point on the globe.
    QSignalSpy hovered(dialog.data(),
                       &ts::ElevationProfileDialog::pointHovered);
    ts::ElevationProfileChart* chart = dialog->chart();
    const std::optional<std::size_t> middle =
        chart->pointAt(chart->width() / 2.0);
    QVERIFY(middle && *middle > 100 && *middle < 400);
    sendMouse(chart, QEvent::MouseMove,
              QPointF(chart->width() / 2.0, chart->height() / 2.0),
              Qt::NoButton, Qt::NoButton);
    QVERIFY(!hovered.isEmpty());

    const QString shots = qEnvironmentVariable("TS_UI_SCREENSHOTS");
    if (!shots.isEmpty()) {
      dialog->grab().save(
          QDir(shots).filePath(QStringLiteral("elevation-profile.png")));
    }

    // Closing the window takes the line off the globe.
    dialog->close();
    QVERIFY(QTest::qWaitFor([&]() { return dialog.isNull(); }, 5000));
    QVERIFY(globe->profileLine().empty());
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

    const int32_t framesBefore = globe->framesRendered();
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
    if (!pointer) {
      QFAIL("pointer is missing");
    }
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
    const int32_t idleFrames = globe->framesRendered();
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
    // The projections, a separator and Projection Settings.
    QCOMPARE(actions.size(), 25);
    QCOMPARE(actions.at(0)->text(), QStringLiteral("Mercator"));
    QCOMPARE(actions.at(1)->text(), QStringLiteral("Web Mercator"));
    QCOMPARE(actions.at(2)->text(), QStringLiteral("Miller Cylindrical"));
    QCOMPARE(actions.at(3)->text(),
             QStringLiteral("Equidistant Cylindrical (Plate Carrée)"));
    QCOMPARE(actions.at(4)->text(), QStringLiteral("Gall Stereographic"));
    QCOMPARE(actions.at(5)->text(),
             QStringLiteral("Gall–Peters (Gall Orthographic)"));
    QCOMPARE(actions.at(6)->text(), QStringLiteral("Transverse Mercator"));
    QCOMPARE(actions.at(7)->text(),
             QStringLiteral("Universal Transverse Mercator (UTM)"));
    QCOMPARE(actions.at(8)->text(), QStringLiteral("Cassini–Soldner"));
    QCOMPARE(actions.at(9)->text(),
             QStringLiteral("UK National Grid (OSGB36)"));
    QCOMPARE(actions.at(10)->text(),
             QStringLiteral("Rotated Mercator (Oblique Mercator)"));
    QCOMPARE(actions.at(11)->text(), QStringLiteral("Lambert Conformal Conic"));
    QCOMPARE(actions.at(12)->text(), QStringLiteral("American Polyconic"));
    QCOMPARE(actions.at(13)->text(), QStringLiteral("Azimuthal Equidistant"));
    QCOMPARE(actions.at(14)->text(), QStringLiteral("Gnomonic"));
    QCOMPARE(actions.at(15)->text(),
             QStringLiteral("Polar Stereographic (North)"));
    QCOMPARE(actions.at(16)->text(),
             QStringLiteral("Polar Stereographic (South)"));
    QCOMPARE(actions.at(17)->text(), QStringLiteral("Robinson"));
    QCOMPARE(actions.at(18)->text(), QStringLiteral("Mollweide"));
    QCOMPARE(actions.at(19)->text(), QStringLiteral("Winkel Tripel"));
    QCOMPARE(actions.at(20)->text(), QStringLiteral("ARC Zones (RPF)"));
    QCOMPARE(actions.at(21)->text(),
             QStringLiteral("ARC North Polar Zone (RPF)"));
    QCOMPARE(actions.at(22)->text(),
             QStringLiteral("ARC South Polar Zone (RPF)"));
    QVERIFY(actions.at(23)->isSeparator());
    QCOMPARE(actions.at(24)->objectName(),
             QStringLiteral("projectionSettingsAction"));
    // Mercator has no settings.
    QVERIFY(!actions.at(24)->isEnabled());
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

  void nationalGridReadout()
  {
    // On the UK National Grid map the status bar gives the grid reference
    // beside the latitude and longitude; on the globe it doesn't.
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ts::GlobeView* globe = globeOf(window);
    if (globe == nullptr) {
      QSKIP("No OpenGL view on this platform.");
    }
    QAction* nationalGrid = window.findChild<QAction*>(
        QStringLiteral("projectionAction_uk-national-grid"));
    QAction* settings =
        window.findChild<QAction*>(QStringLiteral("projectionSettingsAction"));
    QLabel* pointer = window.findChild<QLabel*>(QStringLiteral("pointerLabel"));
    if (nationalGrid == nullptr || settings == nullptr || pointer == nullptr) {
      QFAIL("missing actions or labels");
    }
    nationalGrid->trigger();
    QCOMPARE(globe->mapProjection()->id(), QStringLiteral("uk-national-grid"));
    QVERIFY(!settings->isEnabled()); // nothing to set
    // Westminster, on the globe: no grid reference.
    globe->setPose(ts::CameraPose{51.5007, -0.1246, 2.0e4, 0.0, 0.0});
    const QPointF centre(globe->width() / 2.0, globe->height() / 2.0);
    sendMouse(globe, QEvent::MouseMove, centre, Qt::NoButton, Qt::NoButton);
    QVERIFY2(!pointer->text().contains(QStringLiteral("TQ")),
             qPrintable(pointer->text()));
    globe->setViewMode(ts::ViewMode::Map, false);
    globe->setPose(ts::CameraPose{51.5007, -0.1246, 2.0e4, 0.0, 0.0});
    sendMouse(globe, QEvent::MouseMove, centre + QPointF(1.0, 0.0),
              Qt::NoButton, Qt::NoButton);
    QVERIFY2(pointer->text().contains(QStringLiteral("TQ 30")),
             qPrintable(pointer->text()));
    QVERIFY2(pointer->text().startsWith(QStringLiteral("51.50")),
             qPrintable(pointer->text()));
  }

  void rotatedMercatorTwoPlaces()
  {
    // Rotated Mercator's settings: the centre line through two places
    // fills the centre and azimuth; the same place twice says why not.
    const ts::Projection* rotated =
        ts::projectionById(QStringLiteral("rotated-mercator"));
    if (!rotated) {
      QFAIL("rotated is missing");
    }
    ts::ProjectionSettingsDialog dialog(nullptr, rotated);
    QPushButton* use =
        dialog.findChild<QPushButton*>(QStringLiteral("projectionUsePoints"));
    QLabel* message =
        dialog.findChild<QLabel*>(QStringLiteral("projectionPointsMessage"));
    QDoubleSpinBox* azimuth = dialog.findChild<QDoubleSpinBox*>(
        QStringLiteral("projectionParameter_azimuth"));
    QDoubleSpinBox* centreLat = dialog.findChild<QDoubleSpinBox*>(
        QStringLiteral("projectionParameter_centreLatitude"));
    QDoubleSpinBox* secondLat =
        dialog.findChild<QDoubleSpinBox*>(QStringLiteral("projectionPoint2"));
    QDoubleSpinBox* secondLon =
        dialog.findChild<QDoubleSpinBox*>(QStringLiteral("projectionPoint3"));
    if (use == nullptr || message == nullptr || azimuth == nullptr ||
        centreLat == nullptr || secondLat == nullptr || secondLon == nullptr) {
      QFAIL("missing two-places controls");
    }
    // London to New York, the places it starts with.
    use->click();
    QVERIFY(std::abs(centreLat->value() - 52.24) < 0.01);
    QVERIFY(std::abs(azimuth->value() - 75.33) < 0.01);
    QVERIFY(message->text().isEmpty());
    const ts::Projection* chosen = dialog.projection();
    QCOMPARE(chosen->id(), QStringLiteral("rotated-mercator"));
    QVERIFY(chosen != rotated);
    // The same place twice.
    secondLat->setValue(51.4775);
    secondLon->setValue(-0.4614);
    use->click();
    QVERIFY(!message->text().isEmpty());
    QVERIFY(std::abs(azimuth->value() - 75.33) < 0.01); // unchanged
    // Other projections have no such group.
    ts::ProjectionSettingsDialog other(
        nullptr, ts::projectionById(QStringLiteral("transverse-mercator")));
    QVERIFY(other.findChild<QPushButton*>(
                QStringLiteral("projectionUsePoints")) == nullptr);
  }

  void azimuthalPolarPresets()
  {
    // Azimuthal Equidistant's polar presets set the centre on a pole.
    ts::ProjectionSettingsDialog dialog(
        nullptr,
        ts::projectionWith(QStringLiteral("azimuthal-equidistant"),
                           {{QStringLiteral("centreLatitude"), 30.0},
                            {QStringLiteral("centreLongitude"), 20.0}}));
    QPushButton* south =
        dialog.findChild<QPushButton*>(QStringLiteral("projectionSouthPole"));
    QDoubleSpinBox* lat = dialog.findChild<QDoubleSpinBox*>(
        QStringLiteral("projectionParameter_centreLatitude"));
    QDoubleSpinBox* lon = dialog.findChild<QDoubleSpinBox*>(
        QStringLiteral("projectionParameter_centreLongitude"));
    if (south == nullptr || lat == nullptr || lon == nullptr) {
      QFAIL("missing preset controls");
    }
    QCOMPARE(lat->value(), 30.0);
    south->click();
    QCOMPARE(lat->value(), -90.0);
    QCOMPARE(lon->value(), 0.0);
    QCOMPARE(dialog.projection()->parameterValues().at(
                 QStringLiteral("centreLatitude")),
             -90.0);
  }

  void projectionSettings()
  {
    // Equidistant Cylindrical's standard parallel: the settings action is
    // enabled for it, the dialog changes the map, choosing the same
    // projection again keeps the setting, and projects keep it.
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ts::GlobeView* globe = globeOf(window);
    if (globe == nullptr) {
      QSKIP("No OpenGL view on this platform.");
    }
    QAction* equidistant = window.findChild<QAction*>(
        QStringLiteral("projectionAction_equidistant-cylindrical"));
    QAction* settings =
        window.findChild<QAction*>(QStringLiteral("projectionSettingsAction"));
    if (equidistant == nullptr || settings == nullptr) {
      QFAIL("missing projection actions");
    }
    equidistant->trigger();
    QCOMPARE(globe->mapProjection()->id(),
             QStringLiteral("equidistant-cylindrical"));
    QVERIFY(settings->isEnabled());

    ts::ProjectionSettingsDialog dialog(&window, globe->mapProjection());
    QDoubleSpinBox* box = dialog.findChild<QDoubleSpinBox*>(
        QStringLiteral("projectionParameter_standardParallel"));
    QPushButton* defaults = dialog.findChild<QPushButton*>(
        QStringLiteral("projectionDefaultsButton"));
    if (box == nullptr || defaults == nullptr) {
      QFAIL("missing dialog controls");
    }
    QCOMPARE(box->value(), 0.0);
    QCOMPARE(box->maximum(), 85.0);
    QCOMPARE(dialog.projection(), globe->mapProjection());
    box->setValue(60.0);
    const ts::Projection* sixty = dialog.projection();
    QVERIFY(sixty != globe->mapProjection());
    defaults->click();
    QCOMPARE(box->value(), 0.0);
    box->setValue(60.0);
    globe->setMapProjection(dialog.projection());
    QCOMPARE(globe->mapProjection(), sixty);
    // Half as wide as Plate Carrée: cos 60° = 0.5.
    QVERIFY(std::abs(globe->mapProjection()->bounds().widthM() -
                     3.14159265358979323846 * 6378137.0) < 1e-3);
    equidistant->trigger();
    QCOMPARE(globe->mapProjection(), sixty);

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("settings.terrasoar"));
    QString error;
    QVERIFY2(window.saveProject(path, &error), qPrintable(error));
    QAction* mercator =
        window.findChild<QAction*>(QStringLiteral("projectionAction_mercator"));
    if (mercator == nullptr) {
      QFAIL("no Mercator action");
    }
    mercator->trigger();
    QVERIFY(!settings->isEnabled());
    QVERIFY2(window.openProject(path, &error), qPrintable(error));
    QCOMPARE(globe->mapProjection(), sixty);
    QVERIFY(settings->isEnabled());
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
    if (!services) {
      QFAIL("services is missing");
    }
    QCOMPARE(services->count(), 1);
    QCOMPARE(services->itemText(0), QStringLiteral("Mock service"));
    services->setCurrentIndex(0);
    // As if the user picked it (signals are public).
    emit services->activated(0);
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
    QCOMPARE(dialog.result(), static_cast<int32_t>(QDialog::Accepted));
    QVERIFY(dialog.layerDefinition().has_value());
    const ts::LayerDefinition definition =
        dialog.layerDefinition().value_or(ts::LayerDefinition{});
    QCOMPARE(definition.type, ts::LayerType::Imagery);
    QCOMPARE(definition.origin, ts::LayerOrigin::Catalogue);
    QCOMPARE(definition.name, QStringLiteral("Opaque background"));
    QString error;
    const std::optional<ts::WmsSource> source =
        ts::WmsSource::fromJson(definition.source, &error);
    if (!source) {
      QFAIL(qPrintable(error));
    }
    const ts::WmsSource added = source.value_or(ts::WmsSource{});
    QCOMPARE(added.layers, QStringList{QStringLiteral("background")});
    QCOMPARE(added.format, QStringLiteral("image/jpeg"));
    QVERIFY(!added.transparent);
    QCOMPARE(added.crs, QStringLiteral("EPSG:4326")); // all the mock offers
    QCOMPARE(added.url, serviceUrl);
    // The layer names no credit; the catalogue's is used.
    QCOMPARE(added.attribution, QStringLiteral("Mock Agency"));
  }

  void addWmtsDialog()
  {
    // The whole dialog against the mock WMTS: the current catalogue
    // arrives, the server's layers are listed, style, format, tile matrix
    // set and the Time dimension follow the layer, the catalogue's credit
    // is used, and Add makes the layer's definition.
    std::shared_ptr<ts::test::MockWmtsBehaviour> behaviour =
        std::make_shared<ts::test::MockWmtsBehaviour>();
    ts::test::MockHttpServer server;
    const QUrl capabilitiesUrl =
        server.url(QStringLiteral("/WMTSCapabilities.xml"));
    const ts::test::MockHttpServer::Handler wmts =
        ts::test::mockWmtsHandler(behaviour, server.url());
    server.setHandler(
        [wmts, capabilitiesUrl](const ts::test::MockHttpRequest& request) {
          if (request.url.path() == QLatin1String("/catalogue.json")) {
            return ts::test::mockResponse(
                200,
                "{\"format\": \"terrasoar-wmts-catalogue\", \"version\": "
                "1, \"services\": [{\"title\": \"Mock tiles\", \"url\": \"" +
                    capabilitiesUrl.toString().toUtf8() +
                    "\", \"description\": \"Circles and lines\", "
                    "\"attribution\": \"Mock Mapping Agency\", \"terms\": "
                    "\"https://example.invalid/terms\"}]}",
                "application/json");
          }
          return wmts(request);
        });
    qputenv("TS_WMTS_CATALOGUE_URL",
            server.url(QStringLiteral("/catalogue.json")).toString().toUtf8());
    ts::NetworkSettings settings;
    settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    std::shared_ptr<ts::NetworkClient> network =
        std::make_shared<ts::NetworkClient>(settings);
    ts::AddWmtsDialog dialog(network, nullptr);
    qputenv("TS_WMTS_CATALOGUE_URL", QByteArray());
    // Built in until the current catalogue arrives.
    QVERIFY(!dialog.catalogue().empty());
    QSignalSpy catalogueUpdated(&dialog, &ts::AddWmtsDialog::catalogueUpdated);
    QVERIFY(catalogueUpdated.wait(5000));
    QCOMPARE(dialog.catalogue().size(), std::size_t{1});
    QComboBox* services =
        dialog.findChild<QComboBox*>(QStringLiteral("wmtsServiceBox"));
    if (services == nullptr) {
      QFAIL("wmtsServiceBox missing");
    }
    QCOMPARE(services->itemText(0), QStringLiteral("Mock tiles"));
    services->setCurrentIndex(0);
    emit services->activated(0);
    QCOMPARE(services->currentText(), capabilitiesUrl.toString());
    QLabel* about = dialog.findChild<QLabel*>(QStringLiteral("wmtsAboutLabel"));
    if (about == nullptr) {
      QFAIL("wmtsAboutLabel missing");
    }
    QVERIFY(about->text().contains(QStringLiteral("Mock Mapping Agency")));

    QSignalSpy loaded(&dialog, &ts::AddWmtsDialog::capabilitiesLoaded);
    QPushButton* connectButton =
        dialog.findChild<QPushButton*>(QStringLiteral("wmtsConnectButton"));
    if (connectButton == nullptr) {
      QFAIL("wmtsConnectButton missing");
    }
    connectButton->click();
    QVERIFY(dialog.isBusy());
    QVERIFY(loaded.wait(5000));
    QVERIFY2(dialog.statusText().contains(QStringLiteral("1 layer")),
             qPrintable(dialog.statusText()));
    QVERIFY(dialog.selectLayer(QStringLiteral("pattern")));

    // The layer's choices.
    QComboBox* formats =
        dialog.findChild<QComboBox*>(QStringLiteral("wmtsFormatBox"));
    QComboBox* sets =
        dialog.findChild<QComboBox*>(QStringLiteral("wmtsSetBox"));
    QComboBox* time =
        dialog.findChild<QComboBox*>(QStringLiteral("wmtsDimension_Time"));
    if (formats == nullptr || sets == nullptr || time == nullptr) {
      QFAIL("choice boxes missing");
    }
    QCOMPARE(formats->currentText(), QStringLiteral("image/png"));
    QCOMPARE(formats->count(), 2);
    QCOMPARE(sets->count(), static_cast<int32_t>(ts::test::mockWmtsSetCount));
    // Geographic first: it needs no reprojection.
    QVERIFY(sets->currentData().toString() !=
            QLatin1String("GoogleMapsCompatible"));
    QCOMPARE(time->currentText(), QStringLiteral("2026-01-01"));
    QVERIFY2(dialog.detailsText().contains(
                 QStringLiteral("Credit: Mock Mapping Agency")),
             qPrintable(dialog.detailsText()));
    QVERIFY(dialog.detailsText().contains(QStringLiteral("Tests only")));

    // Another set and a typed date.
    sets->setCurrentIndex(
        sets->findData(QStringLiteral("GoogleMapsCompatible")));
    QVERIFY(dialog.setDimension(QStringLiteral("time"),
                                QStringLiteral("2026-02-01")));
    QVERIFY2(dialog.detailsText().contains(QStringLiteral("Web Mercator")),
             qPrintable(dialog.detailsText()));
    const QString shots = qEnvironmentVariable("TS_UI_SCREENSHOTS");
    if (!shots.isEmpty()) {
      dialog.show();
      dialog.grab().save(
          QDir(shots).filePath(QStringLiteral("add-wmts-dialog.png")));
    }
    QPushButton* add =
        dialog.findChild<QPushButton*>(QStringLiteral("wmtsAddButton"));
    if (add == nullptr) {
      QFAIL("wmtsAddButton missing");
    }
    QVERIFY(add->isEnabled());
    add->click();
    const std::optional<ts::LayerDefinition>& definition =
        dialog.layerDefinition();
    if (!definition) {
      QFAIL("no layer definition");
    }
    QCOMPARE(definition->type, ts::LayerType::Imagery);
    QCOMPARE(definition->name, QStringLiteral("Test pattern"));
    QString error;
    const std::optional<ts::WmtsSource> source =
        ts::WmtsSource::fromJson(definition->source, &error);
    if (!source) {
      QFAIL(qPrintable(error));
    }
    QCOMPARE(source->tileMatrixSet, QStringLiteral("GoogleMapsCompatible"));
    QCOMPARE(source->service, capabilitiesUrl);
    QCOMPARE(source->attribution, QStringLiteral("Mock Mapping Agency"));
    QCOMPARE(source->dimensions.size(), std::size_t{1});
    QCOMPARE(source->dimensions.front().second, QStringLiteral("2026-02-01"));

    // A KVP address works too: GetCapabilities is asked for.
    qputenv("TS_WMTS_CATALOGUE_URL", QByteArray());
    ts::AddWmtsDialog kvp(network, nullptr);
    QSignalSpy kvpLoaded(&kvp, &ts::AddWmtsDialog::capabilitiesLoaded);
    kvp.connectTo(server.url(QStringLiteral("/kvp")).toString());
    QVERIFY(kvpLoaded.wait(5000));
    QVERIFY(kvp.selectLayer(QStringLiteral("pattern")));

    // A WMS server is told apart, with a pointer to the right dialog.
    ts::test::MockHttpServer wmsServer(ts::test::mockWmsHandler(
        std::make_shared<ts::test::MockWmsBehaviour>()));
    ts::AddWmtsDialog wrong(network, nullptr);
    QSignalSpy failed(&wrong, &ts::AddWmtsDialog::connectionFailed);
    wrong.connectTo(wmsServer.url(QStringLiteral("/wms")).toString());
    QVERIFY(failed.wait(5000));
    QVERIFY2(wrong.statusText().contains(QStringLiteral("Add WMS Layer")),
             qPrintable(wrong.statusText()));
  }

  void addXyzDialog()
  {
    // The current catalogue arrives; choosing a service fills the fields;
    // a template the program can't use is explained and Add stays off;
    // Add makes the layer's definition.
    ts::test::MockHttpServer server;
    server.setHandler([](const ts::test::MockHttpRequest& request) {
      if (request.url.path() == QLatin1String("/catalogue.json")) {
        return ts::test::mockResponse(
            200,
            "{\"format\": \"terrasoar-xyz-catalogue\", \"version\": 1, "
            "\"services\": [{\"title\": \"Mock tiles\", \"template\": "
            "\"https://{s}.tiles.example.invalid/{z}/{x}/{y}.png\", "
            "\"subdomains\": [\"a\", \"b\"], \"maxZoom\": 12, "
            "\"description\": \"Circles and lines\", \"attribution\": "
            "\"Mock Mapping Agency\", \"terms\": "
            "\"https://example.invalid/terms\", \"allowBulkDownload\": "
            "false}]}",
            "application/json");
      }
      return ts::test::mockResponse(404, "Not Found", "text/plain");
    });
    // The copy an earlier run fetched would stand in for the built-in one.
    QFile::remove(ts::xyzCatalogueCachePath());
    qputenv("TS_XYZ_CATALOGUE_URL",
            server.url(QStringLiteral("/catalogue.json")).toString().toUtf8());
    ts::NetworkSettings settings;
    settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    ts::AddXyzDialog dialog(std::make_shared<ts::NetworkClient>(settings),
                            nullptr);
    qputenv("TS_XYZ_CATALOGUE_URL", QByteArray());
    // Built in until the current one arrives.
    QVERIFY(dialog.catalogue().size() >= 4);
    QSignalSpy catalogueUpdated(&dialog, &ts::AddXyzDialog::catalogueUpdated);
    QVERIFY(catalogueUpdated.wait(5000));
    QCOMPARE(dialog.catalogue().size(), std::size_t{1});
    QPushButton* add =
        dialog.findChild<QPushButton*>(QStringLiteral("xyzAddButton"));
    QLineEdit* address =
        dialog.findChild<QLineEdit*>(QStringLiteral("xyzTemplateEdit"));
    QLineEdit* subdomains =
        dialog.findChild<QLineEdit*>(QStringLiteral("xyzSubdomainsEdit"));
    QLineEdit* name =
        dialog.findChild<QLineEdit*>(QStringLiteral("xyzNameEdit"));
    QCheckBox* tms = dialog.findChild<QCheckBox*>(QStringLiteral("xyzTmsBox"));
    QSpinBox* maximumZoom =
        dialog.findChild<QSpinBox*>(QStringLiteral("xyzMaximumZoomBox"));
    QLabel* about = dialog.findChild<QLabel*>(QStringLiteral("xyzAboutLabel"));
    if (add == nullptr || address == nullptr || subdomains == nullptr ||
        name == nullptr || tms == nullptr || maximumZoom == nullptr ||
        about == nullptr) {
      QFAIL("widgets missing");
    }
    QVERIFY(!add->isEnabled());

    QVERIFY(dialog.chooseService(0));
    QCOMPARE(name->text(), QStringLiteral("Mock tiles"));
    QCOMPARE(subdomains->text(), QStringLiteral("a, b"));
    QCOMPARE(maximumZoom->value(), 12);
    QVERIFY(about->text().contains(QStringLiteral("Circles and lines")));
    QVERIFY(about->text().contains(QStringLiteral("offline")));
    QVERIFY(add->isEnabled());

    // {s} without subdomains, then a variable it doesn't know.
    subdomains->clear();
    QVERIFY(!add->isEnabled());
    QVERIFY2(dialog.statusText().contains(QStringLiteral("{s}")),
             qPrintable(dialog.statusText()));
    address->setText(QStringLiteral(
        "https://tiles.example.invalid/{z}/{x}/{y}.png?k={key}"));
    QVERIFY(!add->isEnabled());
    QVERIFY(dialog.statusText().contains(QStringLiteral("{key}")));
    address->setText(
        QStringLiteral("https://tiles.example.invalid/{z}/{x}/{y}.jpg"));
    QVERIFY(add->isEnabled());
    QVERIFY(dialog.statusText().isEmpty());
    tms->setChecked(true);
    const QString shots = qEnvironmentVariable("TS_UI_SCREENSHOTS");
    if (!shots.isEmpty()) {
      dialog.show();
      dialog.grab().save(
          QDir(shots).filePath(QStringLiteral("add-xyz-dialog.png")));
    }
    add->click();
    const std::optional<ts::LayerDefinition>& definition =
        dialog.layerDefinition();
    if (!definition) {
      QFAIL("no layer definition");
    }
    QCOMPARE(definition->type, ts::LayerType::Imagery);
    QCOMPARE(definition->name, QStringLiteral("Mock tiles"));
    QString error;
    const std::optional<ts::XyzSource> source =
        ts::XyzSource::fromJson(definition->source, &error);
    if (!source) {
      QFAIL(qPrintable(error));
    }
    QVERIFY(source->tms);
    QVERIFY(source->subdomains.isEmpty());
    QCOMPARE(source->maximumZoom, 12);
    QCOMPARE(source->format, QStringLiteral("image/jpeg"));
    QCOMPARE(source->attribution, QStringLiteral("Mock Mapping Agency"));
    QCOMPARE(source->service.host(), QStringLiteral("tiles.example.invalid"));
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
             static_cast<int32_t>(ts::ProviderStatus::State::Error));
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
             static_cast<int32_t>(ts::ProviderStatus::State::Ok));
    if (globeOf(window) == nullptr) {
      QSKIP("No OpenGL view: no tiles are asked for.");
    }
    QVERIFY(QTest::qWaitFor(
        [&]() {
          return model->data(offline, ts::LayerTreeModel::StatusRole).toInt() ==
                 static_cast<int32_t>(ts::ProviderStatus::State::Offline);
        },
        15000));
    QVERIFY(model->data(offline, Qt::ToolTipRole)
                .toString()
                .contains(QStringLiteral("127.0.0.1")));
    // The view still goes idle: failed tiles aren't asked for every frame.
    QVERIFY(QTest::qWaitFor(
        [&]() { return !globeOf(window)->isLoadingTiles(); }, 20000));
    QVERIFY2(waitUntilQuiet(globeOf(window)), "the view kept drawing");
    // As in idleViewStopsDrawing: repaints the window system asks for are
    // not the view drawing by itself.
    const int32_t frames = globeOf(window)->framesRendered();
    IdleWitness witness(window, *globeOf(window));
    QTest::qWait(1000);
    const int32_t allowed = 2 + witness.activationChanges();
    QVERIFY2(globeOf(window)->framesRendered() <= frames + allowed,
             qPrintable(QStringLiteral("%1 frames drawn while idle (%2 "
                                       "allowed):\n%3")
                            .arg(globeOf(window)->framesRendered() - frames)
                            .arg(allowed)
                            .arg(witness.report())));
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

  void wmsTimeSlider()
  {
    // A WMS layer with TIME: the dialog makes it follow the clock; the
    // time slider appears; each slider position asks GetMap for its own
    // TIME (and the play button steps through them); a fixed time stays.
    std::shared_ptr<ts::test::MockWmsBehaviour> behaviour =
        std::make_shared<ts::test::MockWmsBehaviour>();
    behaviour->timeLayer = true;
    ts::test::MockHttpServer server(ts::test::mockWmsHandler(behaviour));
    QTemporaryDir cache;
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ts::NetworkSettings settings;
    settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    ts::WmsProviderSettings wms;
    wms.cacheDirectory = cache.path();
    std::shared_ptr<ts::NetworkClient> network =
        std::make_shared<ts::NetworkClient>(settings);
    window.registry().setWmsNetwork(network, wms);

    ts::AddWmsDialog dialog(network, nullptr);
    QSignalSpy loaded(&dialog, &ts::AddWmsDialog::capabilitiesLoaded);
    dialog.connectTo(server.url(QStringLiteral("/wms")).toString());
    QVERIFY(loaded.wait(5000));
    QVERIFY(dialog.selectLayer(QStringLiteral("clouds")));
    QVERIFY(dialog.timeModeBox() != nullptr);
    QVERIFY(dialog.timeModeBox()->isVisibleTo(&dialog));
    QCOMPARE(dialog.timeModeBox()->mode(), ts::LayerTime::Mode::Clock);
    const std::optional<ts::LayerTime> chosen = dialog.chosenTime();
    if (!chosen) {
      QFAIL("chosen is missing");
    }
    QCOMPARE(chosen->extent, QStringLiteral("2026-11-01/2026-11-05/P1D"));
    QComboBox* values =
        dialog.findChild<QComboBox*>(QStringLiteral("wmsTimeValueBox"));
    if (values == nullptr) {
      QFAIL("values is missing");
    }
    QVERIFY(!values->isEnabled());
    // A layer without TIME hides the choice.
    QVERIFY(dialog.selectLayer(QStringLiteral("pattern")));
    QVERIFY(!dialog.timeModeBox()->isVisibleTo(&dialog));
    QVERIFY(!dialog.chosenTime().has_value());
    QVERIFY(dialog.selectLayer(QStringLiteral("clouds")));
    QPushButton* add =
        dialog.findChild<QPushButton*>(QStringLiteral("wmsAddButton"));
    if (add == nullptr) {
      QFAIL("add is missing");
    }
    QVERIFY(add->isEnabled());
    add->click();
    const std::optional<ts::LayerDefinition> added = dialog.layerDefinition();
    if (!added) {
      QFAIL("no layer definition");
    }
    const ts::LayerDefinition& definition = *added;
    QVERIFY(definition.source.contains(QStringLiteral("time")));

    // Before the layer: no slider.
    QVERIFY(!window.timeBar()->isVisible());
    window.clock()->setTime(
        ts::parseIsoTime(QStringLiteral("2026-11-03T12:00Z"), QDateTime())
            .value_or(QDateTime()));
    window.layerModel()->addLayer(definition);
    QVERIFY(window.timeBar()->isVisible());
    QCOMPARE(window.timeBar()->extent().count(), int64_t{5});
    QCOMPARE(window.timeBar()->timeText(), QStringLiteral("2026-11-03"));
    QSlider* slider = window.timeBar()->slider();
    QCOMPARE(slider->maximum(), 4);
    QCOMPARE(slider->value(), 2);

    const std::function<QStringList()> requestedTimes = [&server]() {
      QStringList times;
      for (const ts::test::MockHttpRequest& request : server.requests()) {
        const QUrlQuery query(request.url);
        if (query.queryItemValue(QStringLiteral("REQUEST")) ==
            QLatin1String("GetMap")) {
          const QString time = query.queryItemValue(QStringLiteral("TIME"));
          if (!times.contains(time)) {
            times.append(time);
          }
        }
      }
      return times;
    };
    ts::GlobeView* globe = globeOf(window);
    if (globe != nullptr) {
      QVERIFY(QTest::qWaitFor(
          [&]() {
            return requestedTimes().contains(QStringLiteral("2026-11-03"));
          },
          10000));
    }
    // Each slider position: its day, in the clock and in GetMap.
    for (int32_t position = 0; position <= slider->maximum(); ++position) {
      slider->setValue(position);
      const QString day = QStringLiteral("2026-11-0%1").arg(position + 1);
      QCOMPARE(window.timeBar()->timeText(), day);
      QCOMPARE(window.clock()->time(),
               ts::parseIsoTime(day, QDateTime()).value_or(QDateTime()));
      if (globe != nullptr) {
        QVERIFY2(QTest::qWaitFor(
                     [&]() { return requestedTimes().contains(day); }, 10000),
                 qPrintable(requestedTimes().join(QLatin1Char(' '))));
      }
    }
    // Every request asked for one of the layer's days (none for "now").
    for (const QString& time : requestedTimes()) {
      QVERIFY2(time.startsWith(QLatin1String("2026-11-0")), qPrintable(time));
    }
    // Back and forward; playing stops at the last day.
    QVERIFY(window.timeBar()->stepBack());
    QCOMPARE(window.timeBar()->timeText(), QStringLiteral("2026-11-04"));
    QVERIFY(window.timeBar()->stepForward());
    QVERIFY(!window.timeBar()->stepForward());
    window.timeBar()->setStepIntervalMs(20);
    window.timeBar()->play();
    QCOMPARE(window.timeBar()->timeText(), QStringLiteral("2026-11-01"));
    QVERIFY(window.timeBar()->isPlaying());
    QVERIFY(QTest::qWaitFor([&]() { return !window.timeBar()->isPlaying(); },
                            5000));
    QCOMPARE(window.timeBar()->timeText(), QStringLiteral("2026-11-05"));

    // The layer's Time menu fixes it at a day; the clock no longer moves it.
    const QModelIndex layer = window.layerModel()->index(0, 0);
    QMenu* menu = window.findChild<ts::LayerTreeDock*>()->timeMenu(layer);
    if (!menu) {
      QFAIL("menu is missing");
    }
    QAction* second = nullptr;
    for (QAction* action : menu->actions()) {
      if (action->text() == QLatin1String("2026-11-02")) {
        second = action;
      }
    }
    if (!second) {
      QFAIL("second is missing");
    }
    second->trigger();
    const std::optional<ts::LayerTime> fixed = ts::LayerTime::fromSource(
        window.layerModel()->layerDefinition(layer).source);
    if (!fixed) {
      QFAIL("fixed is missing");
    }
    QCOMPARE(fixed->mode, ts::LayerTime::Mode::Fixed);
    QCOMPARE(fixed->valueAt(window.clock()->time()),
             QStringLiteral("2026-11-02"));
    QVERIFY(!window.timeBar()->slider()->isEnabled());

    // Saved in the project.
    QTemporaryDir projects;
    const QString path = projects.filePath(QStringLiteral("time.terrasoar"));
    QString error;
    QVERIFY2(window.saveProject(path, &error), qPrintable(error));
    ts::MainWindow reopened;
    QVERIFY2(reopened.openProject(path, &error), qPrintable(error));
    QCOMPARE(reopened.clock()->time(), window.clock()->time());
  }

  void addWfsDialogAndLayer()
  {
    // The dialog against the mock WFS: the current catalogue arrives, the
    // feature types are listed, Add makes a vector layer in the chosen
    // colour; on the globe near the grid its features are asked for by
    // tile, drawn, and listed by What Is Here where clicked.
    std::shared_ptr<ts::test::MockWmsBehaviour> behaviour =
        std::make_shared<ts::test::MockWmsBehaviour>();
    ts::test::MockHttpServer server;
    const QUrl address = server.url(QStringLiteral("/wfs"));
    const ts::test::MockHttpServer::Handler wfs =
        ts::test::mockWfsHandler(behaviour, address);
    server.setHandler([wfs, address](const ts::test::MockHttpRequest& request) {
      if (request.url.path() == QLatin1String("/catalogue.json")) {
        return ts::test::mockResponse(
            200,
            "{\"format\": \"terrasoar-wfs-catalogue\", \"version\": 1, "
            "\"services\": [{\"title\": \"Mock features\", \"url\": \"" +
                address.toString().toUtf8() +
                "\", \"description\": \"A grid\", \"attribution\": "
                "\"Mock Survey Office\", \"terms\": "
                "\"https://example.invalid/terms\"}]}",
            "application/json");
      }
      return wfs(request);
    });
    QFile::remove(ts::wfsCatalogueCachePath());
    qputenv("TS_WFS_CATALOGUE_URL",
            server.url(QStringLiteral("/catalogue.json")).toString().toUtf8());
    ts::NetworkSettings settings;
    settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    std::shared_ptr<ts::NetworkClient> network =
        std::make_shared<ts::NetworkClient>(settings);
    ts::AddWfsDialog dialog(network, nullptr);
    qputenv("TS_WFS_CATALOGUE_URL", QByteArray());
    QVERIFY(dialog.catalogue().size() >= 3); // built in
    QSignalSpy catalogueUpdated(&dialog, &ts::AddWfsDialog::catalogueUpdated);
    QVERIFY(catalogueUpdated.wait(5000));
    QCOMPARE(dialog.catalogue().size(), std::size_t{1});
    QSignalSpy loaded(&dialog, &ts::AddWfsDialog::capabilitiesLoaded);
    dialog.connectTo(address.toString());
    QVERIFY(loaded.wait(5000));
    QVERIFY2(dialog.statusText().contains(QStringLiteral("3 feature type")),
             qPrintable(dialog.statusText()));
    QVERIFY(!dialog.selectFeatureType(QStringLiteral("test:nothing")));
    QVERIFY(dialog.selectFeatureType(QStringLiteral("test:areas")));
    QVERIFY2(
        dialog.detailsText().contains(QStringLiteral("Mock Survey Office")),
        qPrintable(dialog.detailsText()));
    QVERIFY(dialog.detailsText().contains(QStringLiteral("application/json")));
    dialog.setColour(QColor(10, 200, 30));
    QPushButton* add =
        dialog.findChild<QPushButton*>(QStringLiteral("wfsAddButton"));
    if (add == nullptr) {
      QFAIL("add is missing");
    }
    QVERIFY(add->isEnabled());
    add->click();
    const std::optional<ts::LayerDefinition> added = dialog.layerDefinition();
    if (!added) {
      QFAIL("no layer definition");
    }
    const ts::LayerDefinition& definition = *added;
    QCOMPARE(definition.type, ts::LayerType::Vector);
    QCOMPARE(definition.source.value(QStringLiteral("kind")).toString(),
             QStringLiteral("wfs"));
    const ts::VectorStyle style = ts::VectorStyle::fromJson(definition.style);
    QCOMPARE(style.outline, QColor(10, 200, 30));
    QCOMPARE(style.fill.alpha(), 64);

    // In the window.
    QTemporaryDir cache;
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ts::WmsProviderSettings web;
    web.cacheDirectory = cache.path();
    window.registry().setWmsNetwork(network, web);
    window.layerModel()->addLayer(definition);
    QVERIFY(window.stateReport()
                .value(QStringLiteral("attributions"))
                .toArray()
                .contains(QStringLiteral("Mock Survey Office")));
    ts::GlobeView* globe = globeOf(window);
    if (globe == nullptr) {
      QSKIP("No OpenGL view on this platform.");
    }
    QCOMPARE(globe->vectorLayerCount(), 3);
    // Whole globe: the layer's minimum level isn't reached, nothing asked.
    QTest::qWait(300);
    QVERIFY(ts::test::mockWfsRequests(server).empty());
    globe->setPose(ts::CameraPose{-5.0, -5.0, 1.5e6, 0.0, 0.0});
    QVERIFY(QTest::qWaitFor(
        [&]() { return !ts::test::mockWfsRequests(server).empty(); }, 10000));
    QVERIFY(QTest::qWaitFor([&]() { return !globe->isLoadingTiles(); }, 20000));
    for (const ts::test::MockWfsRequest& request :
         ts::test::mockWfsRequests(server)) {
      QCOMPARE(request.type, QStringLiteral("test:areas"));
    }
    // What Is Here: the square under the point, from the features loaded.
    QCOMPARE(window.identifyAt(ts::LonLat{-5.5, -5.5}, 0.002), 1);
    ts::FeatureInfoWindow* results = window.featureInfoWindow();
    if (!results) {
      QFAIL("results is missing");
    }
    const QString text = results->resultsText();
    QVERIFY2(text.contains(QStringLiteral("cell -6 -6")), qPrintable(text));
    QVERIFY2(text.startsWith(QStringLiteral("Grid areas")), qPrintable(text));
    // Beside the square: nothing to show.
    QCOMPARE(window.identifyAt(ts::LonLat{-5.9, -5.9}, 0.002), 0);
    const QString shots = qEnvironmentVariable("TS_UI_SCREENSHOTS");
    if (!shots.isEmpty()) {
      globe->grabFramebuffer().save(
          QDir(shots).filePath(QStringLiteral("wfs-layer.png")));
      dialog.grab().save(
          QDir(shots).filePath(QStringLiteral("add-wfs-dialog.png")));
    }
  }

  void openKml()
  {
    // A KML document in the layer tree: a collection per folder, the
    // placemarks of each in a vector layer, visibility as written; a click
    // on a placemark shows its balloon; a folder can be flown to.
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    const int32_t before = window.layerModel()->rowCount();
    QString error;
    QVERIFY(!window.openFile(
        QStringLiteral(TS_TEST_DATA_DIR "/kml/nothing.kml"), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY2(
        window.openFile(QStringLiteral(TS_TEST_DATA_DIR "/kml/everything.kml"),
                        &error),
        qPrintable(error));
    ts::LayerTreeModel* model = window.layerModel();
    QCOMPARE(model->rowCount(), before + 1);
    const QModelIndex document = model->index(0, 0);
    QCOMPARE(model->data(document).toString(), QStringLiteral("Everything"));
    QVERIFY(
        model->data(document, ts::LayerTreeModel::IsCollectionRole).toBool());
    // Shapes (its places), Times and places (its places), the document's
    // own placemark.
    QCOMPARE(model->rowCount(document), 3);
    const QModelIndex shapes = model->index(0, 0, document);
    QCOMPARE(model->data(shapes).toString(), QStringLiteral("Shapes"));
    QCOMPARE(model->rowCount(shapes), 1);
    const QModelIndex places = model->index(0, 0, shapes);
    QCOMPARE(model->data(places, ts::LayerTreeModel::LayerTypeRole).toInt(),
             static_cast<int32_t>(ts::LayerType::Vector));
    QCOMPARE(model->layerDefinition(places)
                 .source.value(QStringLiteral("kind"))
                 .toString(),
             QStringLiteral("kml"));
    // A click on the point: its balloon.
    QCOMPARE(window.identifyAt(ts::LonLat{-1.8904, 52.4862}, 0.001), 1);
    ts::BalloonWindow* balloon = window.balloonWindow();
    if (balloon == nullptr) {
      QFAIL("no balloon");
    }
    QVERIFY(balloon->isVisible());
    QVERIFY2(balloon->text().contains(QStringLiteral("A point")),
             qPrintable(balloon->text()));
    QVERIFY(balloon->text().contains(QStringLiteral("Fly there")));
    // Nowhere near a placemark: nothing.
    QCOMPARE(window.identifyAt(ts::LonLat{100.0, -40.0}, 0.001), 0);
    // Fly There on the Shapes folder.
    ts::LayerTreeDock* dock = window.findChild<ts::LayerTreeDock*>();
    QVERIFY(dock != nullptr);
    QMenu* menu = dock->contextMenu(shapes);
    if (menu == nullptr) {
      QFAIL("no context menu");
    }
    QAction* fly = menu->findChild<QAction*>(QStringLiteral("flyThereAction"));
    if (fly == nullptr) {
      QFAIL("no Fly There");
    }
    fly->trigger();
    // The point's icons and labels are drawn over the view where they
    // are in sight.
    ts::GlobeView* view = window.findChild<ts::GlobeView*>();
    QVERIFY(view != nullptr);
    view->setPose(ts::CameraPose{52.4862, -1.8904, 400000.0, 0.0, 0.0});
    QTRY_VERIFY(view->billboardsDrawn() >= 1);
    QVERIFY(view->labelsDrawn() >= 1);
    // From the other side of the Earth they are hidden.
    view->setPose(ts::CameraPose{-52.0, 178.0, 400000.0, 0.0, 0.0});
    QTRY_COMPARE(view->billboardsDrawn(), 0);
    // Switched off with their folder: none.
    view->setPose(ts::CameraPose{52.4862, -1.8904, 400000.0, 0.0, 0.0});
    model->setData(shapes, Qt::Unchecked, Qt::CheckStateRole);
    QTRY_COMPARE(view->billboardsDrawn(), 0);
    model->setData(shapes, Qt::Checked, Qt::CheckStateRole);
    QTRY_VERIFY(view->billboardsDrawn() >= 1);
    // A KMZ, through the same door.
    QVERIFY2(window.openFile(
                 QStringLiteral(TS_TEST_DATA_DIR "/kml/archive.kmz"), &error),
             qPrintable(error));
    QCOMPARE(model->data(model->index(0, 0)).toString(),
             QStringLiteral("In an archive"));
    const QJsonObject state = window.stateReport();
    QVERIFY(state.value(QStringLiteral("layers"))
                .toArray()
                .contains(QStringLiteral("Places")));
  }

  void wmsWhatIsHere()
  {
    // A click on the globe reports the point; asking a queryable WMS layer
    // there sends GetFeatureInfo for that point (the mock answers with
    // the point it was asked about) and lists the answer; layers that
    // can't be asked aren't; the action turns it off.
    std::shared_ptr<ts::test::MockWmsBehaviour> behaviour =
        std::make_shared<ts::test::MockWmsBehaviour>();
    behaviour->featureInfo = true;
    ts::test::MockHttpServer server(ts::test::mockWmsHandler(behaviour));
    QTemporaryDir cache;
    ts::MainWindow window;
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ts::NetworkSettings settings;
    settings.proxy = QNetworkProxy(QNetworkProxy::NoProxy);
    ts::WmsProviderSettings wms;
    wms.cacheDirectory = cache.path();
    std::shared_ptr<ts::NetworkClient> network =
        std::make_shared<ts::NetworkClient>(settings);
    window.registry().setWmsNetwork(network, wms);
    QString error;
    const std::optional<ts::WmsCapabilities> capabilities =
        ts::parseWmsCapabilities(ts::test::mockWmsCapabilities(
                                     QStringLiteral("1.3.0"),
                                     server.url(QStringLiteral("/wms")),
                                     QStringLiteral("EPSG:4326"), false, true),
                                 &error);
    if (!capabilities) {
      QFAIL(qPrintable(error));
    }
    // No layer to ask: nothing happens.
    QCOMPARE(window.identifyAt(ts::LonLat{-2.0, 51.0}, 0.01), 0);
    QVERIFY(window.featureInfoWindow() == nullptr);
    for (const QString& name :
         {QStringLiteral("background"), QStringLiteral("pattern")}) {
      const std::optional<ts::WmsSource> source = ts::wmsSourceFor(
          *capabilities, {name}, {QString()}, QString(), &error);
      if (!source) {
        QFAIL(qPrintable(error));
      }
      QCOMPARE(source->featureInfoUrl.isEmpty(),
               name == QLatin1String("background"));
      ts::LayerDefinition definition;
      definition.id = QStringLiteral("wms:") + name;
      definition.type = ts::LayerType::Imagery;
      definition.name = source->title;
      definition.source = source->toJson();
      window.layerModel()->addLayer(definition);
    }
    for (const QString& format :
         {QStringLiteral("application/json"), QStringLiteral("text/html"),
          QStringLiteral("text/plain")}) {
      // Asked about (-2.3, 51.4) at about 0.01° a pixel.
      QJsonObject source =
          window.layerModel()
              ->layerDefinition(window.layerModel()->index(0, 0))
              .source;
      QJsonObject info = source.value(QStringLiteral("featureInfo")).toObject();
      info.insert(QStringLiteral("format"), format);
      source.insert(QStringLiteral("featureInfo"), info);
      ts::LayerDefinition changed = window.layerModel()->layerDefinition(
          window.layerModel()->index(0, 0));
      changed.source = source;
      window.layerModel()->setLayerDefinition(window.layerModel()->index(0, 0),
                                              changed);
      QCOMPARE(window.identifyAt(ts::LonLat{-2.3, 51.4}, 0.01), 1);
      ts::FeatureInfoWindow* results = window.featureInfoWindow();
      if (results == nullptr) {
        QFAIL("results is missing");
      }
      QVERIFY(results->isVisible());
      QSignalSpy finished(results, &ts::FeatureInfoWindow::allAnswered);
      QVERIFY(results->pendingCount() == 0 || finished.wait(5000));
      const QString text = results->resultsText();
      QVERIFY2(text.startsWith(QLatin1String("Test pattern")),
               qPrintable(text));
      // The point asked about, to the pixel of the tile it was asked in.
      static const QRegularExpression lon(QStringLiteral("lon: (-?[0-9.]+)"));
      static const QRegularExpression lat(QStringLiteral("lat: (-?[0-9.]+)"));
      const double answeredLon = lon.match(text).captured(1).toDouble();
      const double answeredLat = lat.match(text).captured(1).toDouble();
      QVERIFY2(std::abs(answeredLon + 2.3) < 0.02 &&
                   std::abs(answeredLat - 51.4) < 0.02,
               qPrintable(format + QLatin1Char('\n') + text));
      QVERIFY(!text.contains(QLatin1String("Opaque background")));
    }
    // GetFeatureInfo asked for the queryable layer only.
    int32_t infoRequests = 0;
    for (const ts::test::MockHttpRequest& request : server.requests()) {
      const QUrlQuery query(request.url);
      if (query.queryItemValue(QStringLiteral("REQUEST")) ==
          QLatin1String("GetFeatureInfo")) {
        ++infoRequests;
        QCOMPARE(query.queryItemValue(QStringLiteral("QUERY_LAYERS")),
                 QStringLiteral("pattern"));
      }
    }
    QCOMPARE(infoRequests, 3);

    // Turned off: nothing is asked.
    QAction* action =
        window.findChild<QAction*>(QStringLiteral("featureInfoAction"));
    if (action == nullptr) {
      QFAIL("action is missing");
    }
    QVERIFY(action->isChecked());
    action->setChecked(false);
    QCOMPARE(window.identifyAt(ts::LonLat{-2.3, 51.4}, 0.01), 0);
    action->setChecked(true);

    // A click (not a drag) on the globe is reported once, with the point.
    ts::GlobeView* globe = globeOf(window);
    if (globe == nullptr) {
      return;
    }
    QSignalSpy clicked(globe, &ts::GlobeView::pointClicked);
    const QPoint middle = globe->rect().center();
    QTest::mouseClick(globe, Qt::LeftButton, Qt::NoModifier, middle);
    QVERIFY(clicked.wait(3000));
    QCOMPARE(clicked.count(), 1);
    QVERIFY(clicked.at(0).at(1).toDouble() > 0.0);
    // A drag is not a click.
    QTest::mousePress(globe, Qt::LeftButton, Qt::NoModifier, middle);
    QMouseEvent move(QEvent::MouseMove, QPointF(middle + QPoint(40, 0)),
                     QPointF(globe->mapToGlobal(middle + QPoint(40, 0))),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(globe, &move);
    QTest::mouseRelease(globe, Qt::LeftButton, Qt::NoModifier,
                        middle + QPoint(40, 0));
    QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() +
                 200);
    QCOMPARE(clicked.count(), 1);
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
    const int64_t id = regions.regions().front().id;
    ts::OfflineRegionsDialog* list =
        window.findChild<ts::OfflineRegionsDialog*>();
    QVERIFY(list && list->isVisible());
    QCOMPARE(list->selectedRegion(), id);
    QVERIFY(QTest::qWaitFor([&]() { return !regions.progress(id); }, 60000));
    const ts::OfflineRegion* region = regions.region(id);
    QVERIFY(region->complete);
    QCOMPARE(region->failedTiles, int64_t{0});
    QCOMPARE(region->tiles,
             ts::countRegionTiles(uk, 0, 6, std::nullopt).total());
    QVERIFY(region->bytes > 0);
    QVERIFY(list->list()->topLevelItem(0)->text(5).startsWith(
        QStringLiteral("Ready offline")));
    if (!shots.isEmpty()) {
      list->grab().save(
          QDir(shots).filePath(QStringLiteral("offline-regions-dialog.png")));
    }
    std::shared_ptr<ts::RegionTileSource> provider =
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
    QCOMPARE(provider->diskCache()->regionUsage(id).tiles, int64_t{0});
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
    // Drawing continuously would be dozens of frames a second. Repaints the
    // window system asks for are not that: a late one or two (exposing the
    // window again, a status bar change resizing it), and one each time
    // the window is activated or deactivated (on Windows runners focus has
    // been seen to go back and forth every 50-250 ms, a frame each time).
    const int32_t frames = globe->framesRendered();
    IdleWitness witness(window, *globe);
    QTest::qWait(1000);
    const int32_t allowed = 2 + witness.activationChanges();
    QVERIFY2(globe->framesRendered() <= frames + allowed,
             qPrintable(QStringLiteral("%1 frames drawn while idle (%2 "
                                       "allowed):\n%3")
                            .arg(globe->framesRendered() - frames)
                            .arg(allowed)
                            .arg(witness.report())));
  }
};

QTEST_MAIN(TestMainWindow)
#include "tst_mainwindow.moc"
