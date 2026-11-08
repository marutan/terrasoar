// SPDX-License-Identifier: GPL-2.0-or-later
#include "mainwindow.h"

#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QStatusBar>
#include <QWidget>

#include "ts/base/buildinfo.h"

namespace ts
{

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
  setObjectName(QStringLiteral("mainWindow"));
  // Phase 1 puts the globe view here.
  QWidget* central = new QWidget(this);
  central->setObjectName(QStringLiteral("centralArea"));
  setCentralWidget(central);

  m_statusLabel = new QLabel(this);
  m_statusLabel->setObjectName(QStringLiteral("statusLabel"));
  statusBar()->addWidget(m_statusLabel, 1);

  createActions();
  createMenus();
  retranslateUi();

  resize(1024, 768);
}

MainWindow::~MainWindow() = default;

void MainWindow::createActions()
{
  m_quitAction = new QAction(this);
  m_quitAction->setObjectName(QStringLiteral("quitAction"));
  m_quitAction->setShortcut(QKeySequence::Quit);
  m_quitAction->setMenuRole(QAction::QuitRole);
  connect(m_quitAction, &QAction::triggered, this, &QWidget::close);

  m_aboutAction = new QAction(this);
  m_aboutAction->setObjectName(QStringLiteral("aboutAction"));
  m_aboutAction->setMenuRole(QAction::AboutRole);
  connect(m_aboutAction, &QAction::triggered, this, &MainWindow::showAbout);

  m_aboutQtAction = new QAction(this);
  m_aboutQtAction->setObjectName(QStringLiteral("aboutQtAction"));
  m_aboutQtAction->setMenuRole(QAction::AboutQtRole);
  connect(m_aboutQtAction, &QAction::triggered, qApp, &QApplication::aboutQt);
}

void MainWindow::createMenus()
{
  m_fileMenu = menuBar()->addMenu(QString());
  m_fileMenu->setObjectName(QStringLiteral("fileMenu"));
  m_fileMenu->addAction(m_quitAction);

  m_helpMenu = menuBar()->addMenu(QString());
  m_helpMenu->setObjectName(QStringLiteral("helpMenu"));
  m_helpMenu->addAction(m_aboutAction);
  m_helpMenu->addAction(m_aboutQtAction);
}

void MainWindow::retranslateUi()
{
  setWindowTitle(programName());
  m_fileMenu->setTitle(tr("&File"));
  m_helpMenu->setTitle(tr("&Help"));
  m_quitAction->setText(tr("&Quit"));
  m_quitAction->setStatusTip(tr("Close the program"));
  m_aboutAction->setText(tr("&About %1").arg(programName()));
  m_aboutAction->setStatusTip(tr("Show the program version and licence"));
  m_aboutQtAction->setText(tr("About &Qt"));
  m_aboutQtAction->setStatusTip(tr("Show the Qt version in use"));
  m_statusLabel->setText(tr("Ready"));
}

void MainWindow::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QMainWindow::changeEvent(event);
}

void MainWindow::showAbout()
{
  const BuildInfo info = buildInfo();
  QMessageBox::about(
      this, tr("About %1").arg(programName()),
      tr("<h3>%1 %2</h3>"
         "<p>A 3D globe and 2D map viewer.</p>"
         "<p>Built with Qt %3 (running on Qt %4) and %5.</p>"
         "<p>This program is free software, licensed under the GNU General "
         "Public License, version 2 or later.</p>")
          .arg(programName(), info.version, info.qtCompileVersion,
               info.qtRuntimeVersion, info.compiler));
}

} // namespace ts
