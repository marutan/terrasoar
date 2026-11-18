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

#include "aboutdialog.h"

#include <cstdint>
#include <utility>

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFile>
#include <QFontDatabase>
#include <QHeaderView>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "ts/base/buildinfo.h"

namespace ts
{

namespace
{

/** The text of a resource built into the program. */
QString resourceText(const QString& path)
{
  QFile file(path);
  return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll())
                                        : QString();
}

/** A read-only page of @p text in a fixed-width font. */
QPlainTextEdit* textPage(QWidget* parent, const QString& text)
{
  QPlainTextEdit* page = new QPlainTextEdit(parent);
  page->setReadOnly(true);
  page->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
  page->setLineWrapMode(QPlainTextEdit::NoWrap);
  page->setPlainText(text);
  return page;
}

} // namespace

AboutDialog::AboutDialog(QWidget* parent, std::vector<AboutDataRow> rows)
    : QDialog(parent), m_data(std::move(rows)), m_tabs(new QTabWidget(this)),
      m_aboutLabel(new QLabel(this)), m_dataTree(new QTreeWidget(this))
{
  setObjectName(QStringLiteral("aboutDialog"));
  m_tabs->setObjectName(QStringLiteral("aboutTabs"));
  m_aboutLabel->setObjectName(QStringLiteral("aboutLabel"));
  m_aboutLabel->setWordWrap(true);
  // Web links open in the browser; the user guide link closes the window
  // (see userGuideLink()).
  m_aboutLabel->setOpenExternalLinks(false);
  connect(m_aboutLabel, &QLabel::linkActivated, this, &AboutDialog::openLink);
  m_aboutLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
  m_aboutLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  m_aboutLabel->setMargin(12);
  m_dataTree->setObjectName(QStringLiteral("aboutDataTree"));
  m_dataTree->setRootIsDecorated(false);
  m_dataTree->setColumnCount(5);
  m_dataTree->setWordWrap(true);
  m_dataTree->header()->setStretchLastSection(true);
  for (const AboutDataRow& row : m_data) {
    QTreeWidgetItem* item = new QTreeWidgetItem(m_dataTree);
    item->setText(0, row.pack);
    item->setText(1, row.name);
    item->setText(2, row.source);
    item->setText(3, row.licence);
    item->setText(4, row.attribution);
  }
  // Wide enough for their headers and contents; the attribution takes the
  // rest.
  for (int32_t column = 0; column < 4; ++column) {
    m_dataTree->header()->setSectionResizeMode(column,
                                               QHeaderView::ResizeToContents);
  }
  m_noticesText = textPage(this, thirdPartyNotices());
  m_noticesText->setObjectName(QStringLiteral("aboutNoticesText"));
  m_licenceText = textPage(this, licenceText());
  m_licenceText->setObjectName(QStringLiteral("aboutLicenceText"));
  m_tabs->addTab(m_aboutLabel, QString());
  m_tabs->addTab(m_dataTree, QString());
  m_tabs->addTab(m_noticesText, QString());
  m_tabs->addTab(m_licenceText, QString());

  QDialogButtonBox* buttons =
      new QDialogButtonBox(QDialogButtonBox::Close, this);
  m_closeButton = buttons->button(QDialogButtonBox::Close);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(m_tabs, 1);
  layout->addWidget(buttons);
  resize(720, 480);
  retranslateUi();
}

AboutDialog::~AboutDialog() = default;

QString AboutDialog::thirdPartyNotices()
{
  return resourceText(QStringLiteral(":/notices/THIRD_PARTY_NOTICES"));
}

QString AboutDialog::licenceText()
{
  return resourceText(QStringLiteral(":/notices/COPYING"));
}

QString AboutDialog::homePage()
{
  return QStringLiteral("https://github.com/marutan/terrasoar");
}

QString AboutDialog::userGuideLink()
{
  return QStringLiteral("terrasoar-guide:contents");
}

void AboutDialog::openLink(const QString& link)
{
  if (link == userGuideLink()) {
    m_userGuideRequested = true;
    accept();
    return;
  }
  QDesktopServices::openUrl(QUrl(link));
}

void AboutDialog::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QDialog::changeEvent(event);
}

void AboutDialog::retranslateUi()
{
  const BuildInfo info = buildInfo();
  setWindowTitle(tr("About Terra Soar"));
  m_aboutLabel->setText(
      tr("<h2>Terra Soar %1</h2>"
         "<p>Copyright (C) 2026 Peter Howkins</p>"
         "<p>A 3D globe and 2D map viewer: imagery, shorelines, terrain and "
         "web map services, online and offline.</p>"
         "<p>Built with Qt %2 (running on Qt %3) and %4.</p>"
         "<p>Terra Soar is free software: you can redistribute it and/or "
         "modify it under the terms of the GNU General Public License, "
         "version 2 or (at your option) any later version. It comes with "
         "no warranty. See the Licence tab.</p>"
         "<p>It uses Qt, GDAL, PROJ, GEOS, SQLite and GeographicLib, and "
         "data from NASA, NOAA, NGA, Natural Earth and others: see the Data "
         "and Third-Party Notices tabs.</p>"
         "<p><a href=\"%6\">Read the user guide</a> (Help → User Guide, "
         "F1).</p>"
         "<p><a href=\"%5\">%5</a></p>")
          .arg(info.version, info.qtCompileVersion, info.qtRuntimeVersion,
               info.compiler, homePage(), userGuideLink()));
  m_tabs->setTabText(0, tr("About"));
  m_tabs->setTabText(1, tr("Data"));
  m_tabs->setTabText(2, tr("Third-Party Notices"));
  m_tabs->setTabText(3, tr("Licence"));
  m_dataTree->setHeaderLabels(
      {tr("Pack"), tr("Data"), tr("Source"), tr("Licence"), tr("Attribution")});
  if (m_closeButton != nullptr) {
    m_closeButton->setText(tr("Close"));
  }
}

} // namespace ts
