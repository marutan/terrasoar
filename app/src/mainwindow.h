// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The application's main window.
 */
#ifndef TS_APP_MAINWINDOW_H
#define TS_APP_MAINWINDOW_H

#include <QMainWindow>

class QAction;
class QEvent;
class QLabel;
class QMenu;

namespace ts
{

/**
 * The top-level window: menus, status bar and (from Phase 1) the globe
 * view. In Phase 0 the central area is empty.
 *
 * All text is set in retranslateUi(), which runs again when the language
 * changes, so installing a translator at run time updates the window.
 *
 * @guithread
 */
class MainWindow : public QMainWindow
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(MainWindow)

public:
  /** Creates the window. @param parent Usual QWidget parent. */
  explicit MainWindow(QWidget* parent = nullptr);

  /** Destroys the window and its children. */
  ~MainWindow() override;

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

private:
  void createActions();
  void createMenus();
  void retranslateUi();
  void showAbout();

  QMenu* m_fileMenu = nullptr;
  QMenu* m_helpMenu = nullptr;
  QAction* m_quitAction = nullptr;
  QAction* m_aboutAction = nullptr;
  QAction* m_aboutQtAction = nullptr;
  QLabel* m_statusLabel = nullptr;
};

} // namespace ts

#endif // TS_APP_MAINWINDOW_H
