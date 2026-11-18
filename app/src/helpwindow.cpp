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
#include "helpwindow.h"

#include <memory>

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QEvent>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPointer>
#include <QRegularExpression>
#include <QSettings>
#include <QSplitter>
#include <QStyle>
#include <QTextCursor>
#include <QTextDocument>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include "appsettings.h"
#include "helpbrowser.h"
#include "userguide.h"

namespace ts
{

namespace
{

/** The page list's item data: the page's URL. */
constexpr int32_t pageUrlRole = Qt::UserRole;

/** Settings keys. */
const QLatin1String geometryKey("userGuide/geometry");
const QLatin1String splitterKey("userGuide/splitter");
const QLatin1String pageKey("userGuide/page");

/** Returns the top-level window above @p widget's window, if any. */
QWidget* outermostWindow(QWidget* widget)
{
  QWidget* window = widget->window();
  while (window->parentWidget() != nullptr) {
    window = window->parentWidget()->window();
  }
  return window;
}

} // namespace

HelpWindow::HelpWindow(QWidget* parent)
    : QMainWindow(parent), m_browser(new HelpBrowser(this)),
      m_splitter(new QSplitter(Qt::Horizontal, this)),
      m_findBar(new QWidget(this)), m_findLabel(new QLabel(m_findBar)),
      m_findEdit(new QLineEdit(m_findBar)),
      m_findPreviousButton(new QToolButton(m_findBar)),
      m_findNextButton(new QToolButton(m_findBar)),
      m_findCloseButton(new QToolButton(m_findBar)),
      m_findStatus(new QLabel(m_findBar)), m_backAction(new QAction(this)),
      m_forwardAction(new QAction(this)), m_homeAction(new QAction(this)),
      m_findAction(new QAction(this)), m_findNextAction(new QAction(this)),
      m_findPreviousAction(new QAction(this))
{
  setObjectName(QStringLiteral("userGuideWindow"));
  setWindowFlags(Qt::Window);

  // The sidebar: the search box over the list of pages (or of matches).
  QWidget* sidebar = new QWidget(this);
  m_searchEdit = new QLineEdit(sidebar);
  m_searchEdit->setObjectName(QStringLiteral("guideSearchEdit"));
  m_searchEdit->setClearButtonEnabled(true);
  m_pageList = new QListWidget(sidebar);
  m_pageList->setObjectName(QStringLiteral("guidePageList"));
  m_pageList->setWordWrap(true);
  QVBoxLayout* sideLayout = new QVBoxLayout(sidebar);
  sideLayout->setContentsMargins(0, 0, 0, 0);
  sideLayout->addWidget(m_searchEdit);
  sideLayout->addWidget(m_pageList);

  // The find bar under the page, hidden until Ctrl+F.
  m_findBar->setObjectName(QStringLiteral("findBar"));
  m_findEdit->setObjectName(QStringLiteral("findEdit"));
  m_findLabel->setBuddy(m_findEdit);
  m_findPreviousButton->setArrowType(Qt::UpArrow);
  m_findNextButton->setArrowType(Qt::DownArrow);
  m_findStatus->setObjectName(QStringLiteral("findStatus"));
  m_findCloseButton->setIcon(
      style()->standardIcon(QStyle::SP_DialogCloseButton));
  m_findCloseButton->setAutoRaise(true);
  QHBoxLayout* findLayout = new QHBoxLayout(m_findBar);
  findLayout->setContentsMargins(4, 2, 4, 2);
  findLayout->addWidget(m_findLabel);
  findLayout->addWidget(m_findEdit, 1);
  findLayout->addWidget(m_findPreviousButton);
  findLayout->addWidget(m_findNextButton);
  findLayout->addWidget(m_findStatus);
  findLayout->addWidget(m_findCloseButton);
  m_findBar->hide();

  QWidget* pageArea = new QWidget(this);
  QVBoxLayout* pageLayout = new QVBoxLayout(pageArea);
  pageLayout->setContentsMargins(0, 0, 0, 0);
  pageLayout->setSpacing(0);
  pageLayout->addWidget(m_browser, 1);
  pageLayout->addWidget(m_findBar);

  m_splitter->setObjectName(QStringLiteral("guideSplitter"));
  m_splitter->addWidget(sidebar);
  m_splitter->addWidget(pageArea);
  m_splitter->setStretchFactor(1, 1);
  m_splitter->setSizes({220, 700});
  setCentralWidget(m_splitter);

  // Actions, on the toolbar and as shortcuts.
  m_backAction->setObjectName(QStringLiteral("guideBackAction"));
  m_backAction->setIcon(style()->standardIcon(QStyle::SP_ArrowBack));
  m_backAction->setShortcut(QKeySequence::Back);
  m_backAction->setEnabled(false);
  m_forwardAction->setObjectName(QStringLiteral("guideForwardAction"));
  m_forwardAction->setIcon(style()->standardIcon(QStyle::SP_ArrowForward));
  m_forwardAction->setShortcut(QKeySequence::Forward);
  m_forwardAction->setEnabled(false);
  m_homeAction->setObjectName(QStringLiteral("guideHomeAction"));
  m_homeAction->setIcon(style()->standardIcon(QStyle::SP_DirHomeIcon));
  m_homeAction->setShortcut(QKeySequence(static_cast<int32_t>(Qt::ALT) |
                                         static_cast<int32_t>(Qt::Key_Home)));
  m_findAction->setObjectName(QStringLiteral("guideFindAction"));
  m_findAction->setShortcut(QKeySequence::Find);
  m_findNextAction->setShortcut(QKeySequence::FindNext);
  m_findPreviousAction->setShortcut(QKeySequence::FindPrevious);
  addAction(m_findNextAction);
  addAction(m_findPreviousAction);

  m_toolBar = addToolBar(QString());
  m_toolBar->setObjectName(QStringLiteral("guideToolBar"));
  m_toolBar->setMovable(false);
  m_toolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
  m_toolBar->addAction(m_backAction);
  m_toolBar->addAction(m_forwardAction);
  m_toolBar->addAction(m_homeAction);
  m_toolBar->addSeparator();
  m_toolBar->addAction(m_findAction);

  connect(m_backAction, &QAction::triggered, m_browser,
          &QTextBrowser::backward);
  connect(m_forwardAction, &QAction::triggered, m_browser,
          &QTextBrowser::forward);
  connect(m_homeAction, &QAction::triggered, this, [this]() {
    m_browser->setSource(userguide::contentsUrl(),
                         QTextDocument::MarkdownResource);
  });
  connect(m_findAction, &QAction::triggered, this, &HelpWindow::showFindBar);
  connect(m_findNextAction, &QAction::triggered, this,
          [this]() { findAgain(false); });
  connect(m_findPreviousAction, &QAction::triggered, this,
          [this]() { findAgain(true); });
  connect(m_browser, &QTextBrowser::backwardAvailable, m_backAction,
          &QAction::setEnabled);
  connect(m_browser, &QTextBrowser::forwardAvailable, m_forwardAction,
          &QAction::setEnabled);
  connect(m_browser, &QTextBrowser::sourceChanged, this,
          &HelpWindow::pageChanged);

  connect(m_searchEdit, &QLineEdit::textChanged, this,
          &HelpWindow::updatePageList);
  connect(m_pageList, &QListWidget::itemActivated, this, &HelpWindow::openItem);
  connect(m_pageList, &QListWidget::itemClicked, this, &HelpWindow::openItem);

  // Find: as you type from where the last match started; Return and the
  // arrows for the next and previous; Escape or the cross to close.
  connect(m_findEdit, &QLineEdit::textEdited, this,
          [this](const QString& text) {
            QTextCursor cursor = m_browser->textCursor();
            cursor.setPosition(cursor.selectionStart());
            m_browser->setTextCursor(cursor);
            findInPage(text);
          });
  connect(m_findEdit, &QLineEdit::returnPressed, this,
          [this]() { findAgain(false); });
  connect(m_findNextButton, &QToolButton::clicked, this,
          [this]() { findAgain(false); });
  connect(m_findPreviousButton, &QToolButton::clicked, this,
          [this]() { findAgain(true); });
  connect(m_findCloseButton, &QToolButton::clicked, this,
          &HelpWindow::closeFindBar);
  m_findEdit->installEventFilter(this);

  retranslateUi();
  updatePageList();
  restoreWindowState();
}

HelpWindow::~HelpWindow() = default;

void HelpWindow::showTarget(const QString& target)
{
  if (target.isEmpty() && !m_browser->source().isEmpty()) {
    return;
  }
  m_browser->setSource(userguide::pageUrl(target),
                       QTextDocument::MarkdownResource);
}

QUrl HelpWindow::currentPage() const
{
  return m_browser->source();
}

void HelpWindow::setSearchText(const QString& text)
{
  m_searchEdit->setText(text);
}

void HelpWindow::showFindBar()
{
  m_findBar->show();
  m_findEdit->setFocus();
  m_findEdit->selectAll();
}

bool HelpWindow::findInPage(const QString& text, bool backward)
{
  m_findStatus->clear();
  if (text.isEmpty()) {
    return false;
  }
  const QTextDocument::FindFlags flags =
      backward ? QTextDocument::FindBackward : QTextDocument::FindFlags();
  if (m_browser->find(text, flags)) {
    return true;
  }
  // Round the end of the page, then give up.
  const QTextCursor before = m_browser->textCursor();
  m_browser->moveCursor(backward ? QTextCursor::End : QTextCursor::Start);
  if (m_browser->find(text, flags)) {
    m_findStatus->setText(backward ? tr("Continued from the end")
                                   : tr("Continued from the start"));
    return true;
  }
  m_browser->setTextCursor(before);
  m_findStatus->setText(tr("Not found"));
  return false;
}

bool HelpWindow::eventFilter(QObject* watched, QEvent* event)
{
  if (watched == m_findEdit && event->type() == QEvent::KeyPress) {
    const QKeyEvent* key = dynamic_cast<const QKeyEvent*>(event);
    if (key != nullptr && key->key() == Qt::Key_Escape) {
      closeFindBar();
      return true;
    }
    if (key != nullptr &&
        (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) &&
        key->modifiers().testFlag(Qt::ShiftModifier)) {
      findAgain(true);
      return true;
    }
  }
  return QMainWindow::eventFilter(watched, event);
}

void HelpWindow::closeFindBar()
{
  m_findBar->hide();
  m_browser->setFocus();
}

void HelpWindow::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QMainWindow::changeEvent(event);
}

void HelpWindow::closeEvent(QCloseEvent* event)
{
  saveWindowState();
  QMainWindow::closeEvent(event);
}

void HelpWindow::hideEvent(QHideEvent* event)
{
  saveWindowState();
  QMainWindow::hideEvent(event);
}

void HelpWindow::retranslateUi()
{
  pageChanged(m_browser->source());
  m_toolBar->setWindowTitle(tr("Navigation"));
  m_backAction->setText(tr("Back"));
  m_backAction->setToolTip(tr("Back to the previous page"));
  m_forwardAction->setText(tr("Forward"));
  m_forwardAction->setToolTip(tr("Forward to the next page"));
  m_homeAction->setText(tr("Contents"));
  m_homeAction->setToolTip(tr("The guide's contents"));
  m_findAction->setText(tr("Find"));
  m_findAction->setToolTip(tr("Find text in this page"));
  m_searchEdit->setPlaceholderText(tr("Search the guide"));
  m_searchEdit->setToolTip(tr("List the pages that contain every word typed"));
  m_findLabel->setText(tr("Find:"));
  m_findPreviousButton->setToolTip(tr("Previous match"));
  m_findNextButton->setToolTip(tr("Next match"));
  m_findCloseButton->setToolTip(tr("Close the find bar"));
  updatePageList();
}

void HelpWindow::pageChanged(const QUrl& url)
{
  const QString title = userguide::pageTitle(url);
  setWindowTitle(url.isEmpty() ? tr("Terra Soar User Guide")
                               : tr("%1 – Terra Soar User Guide").arg(title));
  if (!url.isEmpty()) {
    openSettings()->setValue(pageKey, url.toString());
  }
  // Show where we are in the contents list.
  if (m_searchEdit->text().trimmed().isEmpty()) {
    QUrl page = url;
    page.setFragment(QString());
    for (int32_t row = 0; row < m_pageList->count(); ++row) {
      QListWidgetItem* item = m_pageList->item(row);
      if (item->data(pageUrlRole).toUrl() == page) {
        m_pageList->setCurrentItem(item);
      }
    }
  }
}

void HelpWindow::updatePageList()
{
  m_pageList->clear();
  const QString query = m_searchEdit->text().trimmed();
  if (query.isEmpty()) {
    for (const userguide::Page& page : userguide::pages()) {
      QListWidgetItem* item = new QListWidgetItem(page.title, m_pageList);
      item->setData(pageUrlRole, page.url);
    }
    pageChanged(m_browser->source());
    return;
  }
  const std::vector<userguide::SearchHit> hits = userguide::search(query);
  for (const userguide::SearchHit& hit : hits) {
    QListWidgetItem* item = new QListWidgetItem(hit.page.title, m_pageList);
    item->setData(pageUrlRole, hit.page.url);
    item->setToolTip(hit.snippet);
  }
  if (hits.empty()) {
    QListWidgetItem* item =
        new QListWidgetItem(tr("No pages match"), m_pageList);
    item->setFlags(Qt::NoItemFlags);
  }
}

void HelpWindow::openItem(const QListWidgetItem* item)
{
  const QUrl url = item->data(pageUrlRole).toUrl();
  if (url.isEmpty()) {
    return;
  }
  m_browser->setSource(url, QTextDocument::MarkdownResource);
  // From a search: show the first match on the page.
  static const QRegularExpression spaces(QStringLiteral("\\s+"));
  const QStringList words =
      m_searchEdit->text().split(spaces, Qt::SkipEmptyParts);
  if (!words.isEmpty()) {
    m_browser->moveCursor(QTextCursor::Start);
    m_browser->find(words.first());
  }
}

void HelpWindow::findAgain(bool backward)
{
  if (m_findEdit->text().isEmpty()) {
    showFindBar();
    return;
  }
  findInPage(m_findEdit->text(), backward);
}

void HelpWindow::saveWindowState() const
{
  const std::unique_ptr<QSettings> settings = openSettings();
  settings->setValue(geometryKey, saveGeometry());
  settings->setValue(splitterKey, m_splitter->saveState());
}

void HelpWindow::restoreWindowState()
{
  const std::unique_ptr<QSettings> settings = openSettings();
  if (!restoreGeometry(settings->value(geometryKey).toByteArray())) {
    resize(960, 680);
  }
  m_splitter->restoreState(settings->value(splitterKey).toByteArray());
  const QUrl page(settings->value(pageKey).toString());
  QUrl withoutAnchor = page;
  withoutAnchor.setFragment(QString());
  m_browser->setSource(
      userguide::pageExists(withoutAnchor) ? page : userguide::contentsUrl(),
      QTextDocument::MarkdownResource);
  // The restored page starts the history: Back has nowhere to go yet.
  m_browser->clearHistory();
}

HelpWindow* showUserGuide(QWidget* from, const QString& target)
{
  // A modal dialog blocks every window but its own, so from inside one
  // the guide gets a window of the dialog's.
  QWidget* modal = QApplication::activeModalWidget();
  QWidget* owner = nullptr;
  if (from != nullptr) {
    owner = (modal != nullptr &&
             (from->window() == modal || modal->isAncestorOf(from)))
                ? modal
                : outermostWindow(from);
  }
  static QPointer<HelpWindow> unowned;
  HelpWindow* window =
      owner != nullptr
          ? owner->findChild<HelpWindow*>(QString(), Qt::FindDirectChildrenOnly)
          : unowned.data();
  if (window == nullptr) {
    window = new HelpWindow(owner);
    if (owner == nullptr) {
      unowned = window;
    }
  }
  window->showTarget(target);
  window->show();
  window->raise();
  window->activateWindow();
  return window;
}

} // namespace ts
