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
/**
 * @file
 * The user guide window (Help → User Guide, F1), and showUserGuide() for
 * the main window and dialogs to open it.
 */
#ifndef TS_APP_HELPWINDOW_H
#define TS_APP_HELPWINDOW_H

#include <QMainWindow>
#include <QString>
#include <QUrl>

class QAction;
class QCloseEvent;
class QEvent;
class QHideEvent;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QSplitter;
class QToolBar;
class QToolButton;

namespace ts
{

class HelpBrowser;

/**
 * The user guide, read in the program: the pages of `docs/user/` built
 * into it, so the guide works offline and matches the installed version.
 *
 * A window of its own (not modal), with the guide's pages down the side
 * (or, while something is typed in the search box, the pages that match
 * it), Back, Forward and Home, and find in page (Ctrl+F, F3). It
 * remembers its size, position and last page between runs.
 */
class HelpWindow : public QMainWindow
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(HelpWindow)

public:
  /**
   * Creates the window, at the size, position and page saved last time
   * (the contents page the first time). @p parent owns it; it is still a
   * window of its own.
   */
  explicit HelpWindow(QWidget* parent);
  ~HelpWindow() override;

  /**
   * Opens @p target, a page of the guide with an optional heading anchor
   * (e.g. `terrain.md#elevation-profiles`). Empty keeps the current page.
   */
  void showTarget(const QString& target);

  /** Returns the page shown, with the anchor it was opened at. */
  QUrl currentPage() const;

  /** Returns the text view. */
  HelpBrowser* browser() const { return m_browser; }

  /** Types @p text into the search box, listing the pages that match. */
  void setSearchText(const QString& text);

  /** Shows the find bar and puts the cursor in it. */
  void showFindBar();

  /**
   * Finds @p text in the page, after the current selection (before it
   * if @p backward), going round the end. Returns false if it isn't on
   * the page.
   */
  bool findInPage(const QString& text, bool backward = false);

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;
  /** Saves the window's size and position. */
  void closeEvent(QCloseEvent* event) override;
  /** Saves the window's size and position. */
  void hideEvent(QHideEvent* event) override;
  /**
   * In the find box, Escape closes the find bar and Shift+Return finds
   * the previous match.
   */
  bool eventFilter(QObject* watched, QEvent* event) override;

private:
  void retranslateUi();
  void pageChanged(const QUrl& url);
  void updatePageList();
  void openItem(const QListWidgetItem* item);
  void findAgain(bool backward);
  void closeFindBar();
  void saveWindowState() const;
  void restoreWindowState();

  HelpBrowser* m_browser = nullptr;
  QToolBar* m_toolBar = nullptr;
  QSplitter* m_splitter = nullptr;
  QLineEdit* m_searchEdit = nullptr;
  QListWidget* m_pageList = nullptr;
  QWidget* m_findBar = nullptr;
  QLabel* m_findLabel = nullptr;
  QLineEdit* m_findEdit = nullptr;
  QToolButton* m_findPreviousButton = nullptr;
  QToolButton* m_findNextButton = nullptr;
  QToolButton* m_findCloseButton = nullptr;
  QLabel* m_findStatus = nullptr;
  QAction* m_backAction = nullptr;
  QAction* m_forwardAction = nullptr;
  QAction* m_homeAction = nullptr;
  QAction* m_findAction = nullptr;
  QAction* m_findNextAction = nullptr;
  QAction* m_findPreviousAction = nullptr;
};

/**
 * Opens the user guide at @p target (see HelpWindow::showTarget()) and
 * brings it to the front; returns its window.
 *
 * The guide normally has one window, owned by the top-level window of
 * @p from (the main window). From inside a modal dialog it gets a window
 * owned by that dialog instead, because the dialog would otherwise block
 * it; that window closes with the dialog.
 */
HelpWindow* showUserGuide(QWidget* from, const QString& target = QString());

} // namespace ts

#endif // TS_APP_HELPWINDOW_H
