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
#include <cstdint>
#include <iterator>
#include <vector>

#include <QAction>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QScrollBar>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QtTest>

#include "addwebterraindialog.h"
#include "addwmsdialog.h"
#include "addwmtsdialog.h"
#include "appsettings.h"
#include "elevationprofiledialog.h"
#include "helpbrowser.h"
#include "helpwindow.h"
#include "newregiondialog.h"
#include "offlineregionsdialog.h"
#include "packmanagerdialog.h"
#include "projectionsettingsdialog.h"
#include "ts/base/buildinfo.h"
#include "userguide.h"

namespace
{

namespace guide = ts::userguide;

/** Returns the GitHub address prefix for this version's tag. */
QString repositoryPrefix()
{
  return QStringLiteral("https://github.com/marutan/terrasoar/blob/v") +
         ts::buildInfo().version + QLatin1Char('/');
}

/** The href of every link in @p document. */
QStringList documentLinks(const QTextDocument& document)
{
  QStringList links;
  for (QTextBlock block = document.begin(); block.isValid();
       block = block.next()) {
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
      const QTextCharFormat format = it.fragment().charFormat();
      if (format.isAnchor() && !format.anchorHref().isEmpty() &&
          !links.contains(format.anchorHref())) {
        links.append(format.anchorHref());
      }
    }
  }
  return links;
}

/** The image names in @p document. */
QStringList documentImages(const QTextDocument& document)
{
  QStringList images;
  for (QTextBlock block = document.begin(); block.isValid();
       block = block.next()) {
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
      const QTextImageFormat format =
          it.fragment().charFormat().toImageFormat();
      if (format.isValid()) {
        images.append(format.name());
      }
    }
  }
  return images;
}

/** The anchors of @p page's headings, as the window computes them. */
QStringList pageAnchors(const QUrl& page)
{
  QTextDocument document;
  document.setMarkdown(guide::pageText(page));
  return guide::headingAnchors(guide::documentHeadings(document));
}

/** Returns @p url without its fragment. */
QUrl pageOf(const QUrl& url)
{
  QUrl page = url;
  page.setFragment(QString());
  return page;
}

} // namespace

/**
 * Tests for the user guide in the program: anchors, link resolution and
 * search (userguide), every built-in page and link, and the window.
 */
class TestHelp : public QObject
{
  Q_OBJECT

  // NOLINTNEXTLINE(readability-redundant-access-specifiers): Qt's slots marker
private slots:
  void initTestCase()
  {
    // Settings go to a test location.
    QStandardPaths::setTestModeEnabled(true);
  }

  void init()
  {
    // Each test starts without saved window state.
    QFile::remove(ts::settingsFilePath());
  }

  void headingSlugs_data()
  {
    QTest::addColumn<QString>("heading");
    QTest::addColumn<QString>("slug");
    QTest::newRow("words") << QStringLiteral("Space on disk")
                           << QStringLiteral("space-on-disk");
    QTest::newRow("brackets") << QStringLiteral("Mercator (on WGS 84)")
                              << QStringLiteral("mercator-on-wgs-84");
    QTest::newRow("punctuation")
        << QStringLiteral("C++ & Qt: notes") << QStringLiteral("c--qt-notes");
    QTest::newRow("hyphen and underscore")
        << QStringLiteral("Read-only snake_case")
        << QStringLiteral("read-only-snake_case");
    QTest::newRow("accents")
        << QStringLiteral("Größe und Höhe") << QStringLiteral("größe-und-höhe");
    QTest::newRow("trimmed")
        << QStringLiteral("  Limits  ") << QStringLiteral("limits");
  }

  void headingSlugs()
  {
    QFETCH(QString, heading);
    QFETCH(QString, slug);
    QCOMPARE(guide::headingSlug(heading), slug);
  }

  void repeatedHeadingsAreNumbered()
  {
    QCOMPARE(
        guide::headingAnchors(
            {QStringLiteral("Notes"), QStringLiteral("Limits"),
             QStringLiteral("Notes"), QStringLiteral("Notes")}),
        (QStringList{QStringLiteral("notes"), QStringLiteral("limits"),
                     QStringLiteral("notes-1"), QStringLiteral("notes-2")}));
  }

  void resolveLinks_data()
  {
    QTest::addColumn<QString>("page");
    QTest::addColumn<QString>("link");
    QTest::addColumn<int32_t>("kind");
    QTest::addColumn<QString>("url");
    const QString root = QStringLiteral("qrc:/guide/user/");
    const int32_t page = static_cast<int32_t>(guide::LinkKind::GuidePage);
    const int32_t external = static_cast<int32_t>(guide::LinkKind::External);
    const int32_t repository =
        static_cast<int32_t>(guide::LinkKind::Repository);
    const int32_t missing = static_cast<int32_t>(guide::LinkKind::Missing);
    QTest::newRow("page") << QStringLiteral("README.md")
                          << QStringLiteral("terrain.md") << page
                          << root + QStringLiteral("terrain.md");
    QTest::newRow("page and anchor")
        << QStringLiteral("README.md")
        << QStringLiteral("offline-use.md#space-on-disk") << page
        << root + QStringLiteral("offline-use.md#space-on-disk");
    QTest::newRow("anchor on the page")
        << QStringLiteral("terrain.md") << QStringLiteral("#elevation-profiles")
        << page << root + QStringLiteral("terrain.md#elevation-profiles");
    QTest::newRow("web") << QStringLiteral("README.md")
                         << QStringLiteral("https://example.org/a?b=1")
                         << external
                         << QStringLiteral("https://example.org/a?b=1");
    QTest::newRow("mail") << QStringLiteral("README.md")
                          << QStringLiteral("mailto:someone@example.org")
                          << external
                          << QStringLiteral("mailto:someone@example.org");
    QTest::newRow("docs file")
        << QStringLiteral("installation.md") << QStringLiteral("../building.md")
        << repository
        << repositoryPrefix() + QStringLiteral("docs/building.md");
    QTest::newRow("docs file and anchor")
        << QStringLiteral("terrain.md")
        << QStringLiteral("../design/vertical-datums.md#egm2008") << repository
        << repositoryPrefix() +
               QStringLiteral("docs/design/vertical-datums.md#egm2008");
    QTest::newRow("repository root")
        << QStringLiteral("README.md") << QStringLiteral("../../CHANGELOG.md")
        << repository << repositoryPrefix() + QStringLiteral("CHANGELOG.md");
    QTest::newRow("no such page")
        << QStringLiteral("README.md") << QStringLiteral("no-such-page.md")
        << missing << root + QStringLiteral("no-such-page.md");
  }

  void resolveLinks()
  {
    QFETCH(QString, page);
    QFETCH(QString, link);
    QFETCH(int32_t, kind);
    QFETCH(QString, url);
    const guide::LinkTarget target =
        guide::resolveLink(guide::pageUrl(page), QUrl(link));
    QCOMPARE(static_cast<int32_t>(target.kind), kind);
    QCOMPARE(target.url.toString(), url);
  }

  void contentsInReadingOrder()
  {
    const std::vector<guide::Page> pages = guide::pages();
    QStringList files;
    for (const guide::Page& page : pages) {
      files.append(page.url.fileName());
    }
    QCOMPARE(
        files,
        (QStringList{
            QStringLiteral("README.md"), QStringLiteral("installation.md"),
            QStringLiteral("getting-around.md"), QStringLiteral("layers.md"),
            QStringLiteral("adding-wms-layers.md"),
            QStringLiteral("adding-wmts-layers.md"),
            QStringLiteral("adding-xyz-layers.md"),
            QStringLiteral("adding-wfs-layers.md"), QStringLiteral("kml.md"),
            QStringLiteral("terrain.md"), QStringLiteral("offline-use.md")}));
    QCOMPARE(pages.front().title, QStringLiteral("Terra Soar user guide"));
    QCOMPARE(pages.back().title, QStringLiteral("Using Terra Soar offline"));
  }

  void everyPageOfDocsUserIsBuiltIn()
  {
    const QDir docs(QStringLiteral(TS_SOURCE_DIR "/docs/user"));
    const QStringList sources =
        docs.entryList({QStringLiteral("*.md")}, QDir::Files);
    QVERIFY(!sources.isEmpty());
    for (const QString& file : sources) {
      QVERIFY2(guide::pageExists(guide::pageUrl(file)), qPrintable(file));
      // And built in unchanged.
      QFile source(docs.filePath(file));
      QVERIFY(source.open(QIODevice::ReadOnly));
      QCOMPARE(guide::pageText(guide::pageUrl(file)),
               QString::fromUtf8(source.readAll()));
    }
    const QStringList builtIn =
        QDir(QStringLiteral(":/guide/user"))
            .entryList({QStringLiteral("*.md")}, QDir::Files);
    QCOMPARE(builtIn.size(), sources.size());
  }

  void everyPageRendersAndItsLinksResolve()
  {
    ts::HelpBrowser browser(nullptr);
    for (const guide::Page& page : guide::pages()) {
      browser.setSource(page.url, QTextDocument::MarkdownResource);
      const QString name = page.url.fileName();
      QVERIFY2(!browser.toPlainText().trimmed().isEmpty(), qPrintable(name));
      QVERIFY2(!browser.headingAnchors().isEmpty(), qPrintable(name));
      for (const QString& href : documentLinks(*browser.document())) {
        const QString where = name + QStringLiteral(": ") + href;
        const guide::LinkTarget target =
            guide::resolveLink(page.url, QUrl(href));
        switch (target.kind) {
        case guide::LinkKind::GuidePage:
          if (!target.url.fragment().isEmpty()) {
            QVERIFY2(
                pageAnchors(pageOf(target.url)).contains(target.url.fragment()),
                qPrintable(where));
          }
          break;
        case guide::LinkKind::Repository: {
          QString path = target.url.path();
          const QString tag = QStringLiteral("/blob/v") +
                              ts::buildInfo().version + QLatin1Char('/');
          QVERIFY2(path.contains(tag), qPrintable(where));
          path = path.mid(path.indexOf(tag) + tag.size());
          QVERIFY2(QFileInfo::exists(QStringLiteral(TS_SOURCE_DIR "/") + path),
                   qPrintable(where));
          break;
        }
        case guide::LinkKind::External:
          QVERIFY2(target.url.isValid(), qPrintable(where));
          break;
        case guide::LinkKind::Missing:
          QFAIL(qPrintable(QStringLiteral("broken link: ") + where));
        }
      }
      for (const QString& image : documentImages(*browser.document())) {
        const QUrl url = page.url.resolved(QUrl(image));
        QVERIFY2(QFile::exists(QLatin1Char(':') + url.path()),
                 qPrintable(name + QStringLiteral(": ") + image));
      }
    }
  }

  void dialogHelpTargetsExist()
  {
    const QStringList targets{ts::AddWmsDialog::helpTarget(),
                              ts::AddWmtsDialog::helpTarget(),
                              ts::AddWebTerrainDialog::helpTarget(),
                              ts::OfflineRegionsDialog::helpTarget(),
                              ts::NewRegionDialog::helpTarget(),
                              ts::PackManagerDialog::helpTarget(),
                              ts::ElevationProfileDialog::helpTarget(),
                              ts::ProjectionSettingsDialog::helpTarget()};
    for (const QString& target : targets) {
      const QUrl url = guide::pageUrl(target);
      QVERIFY2(guide::pageExists(pageOf(url)), qPrintable(target));
      QVERIFY2(pageAnchors(pageOf(url)).contains(url.fragment()),
               qPrintable(target));
    }
  }

  void imagesFitTheView()
  {
    // A page with an image much wider than the view.
    const QTemporaryDir directory;
    QImage wide(3000, 600, QImage::Format_RGB32);
    wide.fill(Qt::darkCyan);
    QVERIFY(wide.save(directory.filePath(QStringLiteral("wide.png"))));
    QFile page(directory.filePath(QStringLiteral("page.md")));
    QVERIFY(page.open(QIODevice::WriteOnly));
    page.write("# Picture\n\n![A wide picture](wide.png)\n");
    page.close();
    ts::HelpBrowser browser(nullptr);
    browser.resize(500, 400);
    browser.setSource(QUrl::fromLocalFile(page.fileName()),
                      QTextDocument::MarkdownResource);
    const QStringList images = documentImages(*browser.document());
    QCOMPARE(images.size(), 1);
    double width = 0.0;
    double height = 0.0;
    for (QTextBlock block = browser.document()->begin(); block.isValid();
         block = block.next()) {
      for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
        const QTextImageFormat format =
            it.fragment().charFormat().toImageFormat();
        if (format.isValid()) {
          width = format.width();
          height = format.height();
        }
      }
    }
    QVERIFY(width > 0.0);
    QVERIFY(width <= browser.viewport()->width());
    QVERIFY(qAbs(height - width / 5.0) < 1.0);
  }

  void searchFindsPagesWithEveryWord()
  {
    const std::vector<guide::SearchHit> hits =
        guide::search(QStringLiteral("SPACE on Disk"));
    QVERIFY(!hits.empty());
    bool offline = false;
    for (const guide::SearchHit& hit : hits) {
      offline =
          offline || hit.page.url.fileName() == QLatin1String("offline-use.md");
      QVERIFY(
          hit.snippet.contains(QStringLiteral("space"), Qt::CaseInsensitive));
    }
    QVERIFY(offline);
    QVERIFY(guide::search(QStringLiteral("hillshade xyzzyplugh")).empty());
    QVERIFY(guide::search(QStringLiteral("   ")).empty());
  }

  void windowOpensAtTheContents()
  {
    ts::HelpWindow* window = ts::showUserGuide(nullptr);
    if (window == nullptr) {
      QFAIL("window missing");
    }
    QVERIFY(window->isVisible());
    QCOMPARE(window->currentPage(), guide::contentsUrl());
    QVERIFY(window->windowTitle().contains(
        QStringLiteral("Terra Soar user guide")));
    QListWidget* list =
        window->findChild<QListWidget*>(QStringLiteral("guidePageList"));
    if (list == nullptr) {
      QFAIL("list missing");
    }
    QCOMPARE(list->count(), static_cast<int32_t>(guide::pages().size()));
    QCOMPARE(list->currentRow(), 0);
    // A second request reuses the window.
    QCOMPARE(ts::showUserGuide(nullptr, QStringLiteral("terrain.md")), window);
    QCOMPARE(window->currentPage().fileName(), QStringLiteral("terrain.md"));
    // The page's place in the contents.
    const std::vector<guide::Page> pages = guide::pages();
    const std::vector<guide::Page>::const_iterator terrain =
        std::find_if(pages.begin(), pages.end(), [](const guide::Page& page) {
          return page.url.fileName() == QLatin1String("terrain.md");
        });
    QCOMPARE(list->currentRow(),
             static_cast<int32_t>(std::distance(pages.begin(), terrain)));
    // For the docs and for looking at: set TS_UI_SCREENSHOTS to a directory.
    const QString shots = qEnvironmentVariable("TS_UI_SCREENSHOTS");
    if (!shots.isEmpty()) {
      window->resize(1000, 680);
      QVERIFY(QTest::qWaitForWindowExposed(window));
      window->grab().save(
          QDir(shots).filePath(QStringLiteral("user-guide.png")));
      window->setSearchText(QStringLiteral("offline region"));
      window->grab().save(
          QDir(shots).filePath(QStringLiteral("user-guide-search.png")));
      window->setSearchText(QString());
    }
    delete window;
  }

  void linksBackForwardAndHome()
  {
    ts::HelpWindow window(nullptr);
    window.resize(700, 320);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    ts::HelpBrowser* browser = window.browser();
    browser->setOpenOutsideLinks(false);
    QAction* back =
        window.findChild<QAction*>(QStringLiteral("guideBackAction"));
    QAction* forward =
        window.findChild<QAction*>(QStringLiteral("guideForwardAction"));
    QAction* home =
        window.findChild<QAction*>(QStringLiteral("guideHomeAction"));
    if (back == nullptr || forward == nullptr || home == nullptr) {
      QFAIL("back, forward, home missing");
    }
    QVERIFY(!back->isEnabled() && !forward->isEnabled());

    // A link to a heading opens the page there.
    emit browser->anchorClicked(
        QUrl(QStringLiteral("terrain.md#elevation-profiles")));
    QCOMPARE(window.currentPage(),
             guide::pageUrl(QStringLiteral("terrain.md#elevation-profiles")));
    QVERIFY(browser->verticalScrollBar()->value() > 0);
    QCOMPARE(browser->headingAtTop(), QStringLiteral("elevation-profiles"));
    QVERIFY(back->isEnabled());

    // A link out of the guide goes to the system's browser, not here.
    QSignalSpy outside(browser, &ts::HelpBrowser::externalLinkActivated);
    emit browser->anchorClicked(
        QUrl(QStringLiteral("../design/vertical-datums.md")));
    QCOMPARE(outside.count(), 1);
    QCOMPARE(outside.at(0).at(0).toUrl().toString(),
             repositoryPrefix() +
                 QStringLiteral("docs/design/vertical-datums.md"));
    emit browser->anchorClicked(QUrl(QStringLiteral("https://example.org/")));
    QCOMPARE(outside.count(), 2);
    QCOMPARE(window.currentPage().fileName(), QStringLiteral("terrain.md"));

    back->trigger();
    QCOMPARE(window.currentPage(), guide::contentsUrl());
    QVERIFY(forward->isEnabled());
    forward->trigger();
    QCOMPARE(pageOf(window.currentPage()).fileName(),
             QStringLiteral("terrain.md"));
    home->trigger();
    QCOMPARE(window.currentPage(), guide::contentsUrl());

    // A link to a missing page goes nowhere.
    emit browser->anchorClicked(QUrl(QStringLiteral("no-such-page.md")));
    QCOMPARE(window.currentPage(), guide::contentsUrl());
  }

  void searchBoxListsMatchingPages()
  {
    ts::HelpWindow window(nullptr);
    window.show();
    QListWidget* list =
        window.findChild<QListWidget*>(QStringLiteral("guidePageList"));
    if (list == nullptr) {
      QFAIL("list missing");
    }
    window.setSearchText(QStringLiteral("space on disk"));
    QListWidgetItem* offline = nullptr;
    for (int32_t row = 0; row < list->count(); ++row) {
      if (list->item(row)->text() ==
          QLatin1String("Using Terra Soar offline")) {
        offline = list->item(row);
      }
    }
    if (offline == nullptr) {
      QFAIL("offline missing");
    }
    QVERIFY(!offline->toolTip().isEmpty());
    // Opening a match shows its first occurrence.
    emit list->itemActivated(offline);
    QCOMPARE(window.currentPage().fileName(), QStringLiteral("offline-use.md"));
    QCOMPARE(window.browser()->textCursor().selectedText().toLower(),
             QStringLiteral("space"));

    window.setSearchText(QStringLiteral("xyzzyplugh"));
    QCOMPARE(list->count(), 1);
    QCOMPARE(list->item(0)->flags(), Qt::NoItemFlags);
    // Clearing the search shows the contents again.
    window.setSearchText(QString());
    QCOMPARE(list->count(), static_cast<int32_t>(guide::pages().size()));
  }

  void findInPage()
  {
    ts::HelpWindow window(nullptr);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    window.showTarget(QStringLiteral("terrain.md"));
    QWidget* bar = window.findChild<QWidget*>(QStringLiteral("findBar"));
    QLineEdit* edit = window.findChild<QLineEdit*>(QStringLiteral("findEdit"));
    QLabel* status = window.findChild<QLabel*>(QStringLiteral("findStatus"));
    if (bar == nullptr || edit == nullptr || status == nullptr) {
      QFAIL("bar, edit, status missing");
    }
    QVERIFY(!bar->isVisible());
    window.findChild<QAction*>(QStringLiteral("guideFindAction"))->trigger();
    QVERIFY(bar->isVisible());

    QVERIFY(window.findInPage(QStringLiteral("HILLSHADE")));
    QCOMPARE(window.browser()->textCursor().selectedText().toLower(),
             QStringLiteral("hillshade"));
    const int32_t first = window.browser()->textCursor().selectionStart();
    QVERIFY(window.findInPage(QStringLiteral("hillshade")));
    QVERIFY(window.browser()->textCursor().selectionStart() > first);
    QVERIFY(window.findInPage(QStringLiteral("hillshade"), true));
    QCOMPARE(window.browser()->textCursor().selectionStart(), first);

    QVERIFY(!window.findInPage(QStringLiteral("xyzzyplugh")));
    QCOMPARE(status->text(), QStringLiteral("Not found"));

    edit->setFocus();
    QTest::keyClick(edit, Qt::Key_Escape);
    QVERIFY(!bar->isVisible());
  }

  void remembersItsPageAndSize()
  {
    {
      ts::HelpWindow window(nullptr);
      window.show();
      QVERIFY(QTest::qWaitForWindowExposed(&window));
      window.resize(640, 420);
      QVERIFY(
          QTest::qWaitFor([&window]() { return window.width() == 640; }, 2000));
      window.showTarget(QStringLiteral("layers.md#projects"));
      window.close();
    }
    ts::HelpWindow again(nullptr);
    QCOMPARE(again.currentPage(),
             guide::pageUrl(QStringLiteral("layers.md#projects")));
    QVERIFY(qAbs(again.width() - 640) <= 2);
    QVERIFY(qAbs(again.height() - 420) <= 2);
    // The restored page starts a fresh history.
    QVERIFY(!again.findChild<QAction*>(QStringLiteral("guideBackAction"))
                 ->isEnabled());
  }

  void ownedByTheMainWindowOrAModalDialog()
  {
    // From a dialog of a main window: the main window's guide window.
    QWidget main;
    QDialog dialog(&main);
    dialog.show();
    ts::HelpWindow* shared = ts::showUserGuide(&dialog);
    QCOMPARE(shared->parentWidget(), &main);
    QVERIFY(shared->isWindow());
    QCOMPARE(ts::showUserGuide(&main), shared);

    // From a modal dialog: a window of the dialog's, which the dialog
    // doesn't block.
    QDialog modal(&main);
    modal.setWindowModality(Qt::ApplicationModal);
    modal.show();
    QVERIFY(QTest::qWaitForWindowExposed(&modal));
    QCOMPARE(QApplication::activeModalWidget(), &modal);
    ts::HelpWindow* own =
        ts::showUserGuide(&modal, QStringLiteral("terrain.md"));
    QCOMPARE(own->parentWidget(), &modal);
    QVERIFY(own != shared);
    QCOMPARE(own->currentPage().fileName(), QStringLiteral("terrain.md"));
    modal.hide();
  }
};

QTEST_MAIN(TestHelp)
#include "tst_help.moc"
