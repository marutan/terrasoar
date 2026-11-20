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
#include "balloonwindow.h"

#include <QDesktopServices>
#include <QEvent>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace ts
{

BalloonWindow::BalloonWindow(QWidget* parent)
    : QDialog(parent), m_browser(new QTextBrowser(this))
{
  setObjectName(QStringLiteral("balloonWindow"));
  qRegisterMetaType<ts::CameraPose>("ts::CameraPose");
  m_browser->setObjectName(QStringLiteral("balloonText"));
  m_browser->setOpenLinks(false);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(m_browser);
  connect(m_browser, &QTextBrowser::anchorClicked, this,
          &BalloonWindow::linkClicked);
  resize(380, 300);
}

BalloonWindow::~BalloonWindow() = default;

void BalloonWindow::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange && !m_html.isEmpty()) {
    showBalloon(m_title, m_html, m_background, m_pose, m_base);
  }
  QDialog::changeEvent(event);
}

void BalloonWindow::showBalloon(const QString& title, const QString& html,
                                const QColor& background,
                                const std::optional<CameraPose>& pose,
                                const QString& baseDirectory)
{
  m_title = title;
  m_html = html;
  m_background = background;
  m_pose = pose;
  m_base = baseDirectory;
  setWindowTitle(title.isEmpty() ? tr("Placemark") : title);
  m_browser->setSearchPaths(
      baseDirectory.isEmpty() ? QStringList() : QStringList{baseDirectory});
  QPalette palette = m_browser->palette();
  palette.setColor(QPalette::Base,
                   background.isValid() ? background : QColor(Qt::white));
  m_browser->setPalette(palette);
  QString shown = html;
  if (pose) {
    shown += QStringLiteral("<p><a href=\"terrasoar:flyto\">%1</a></p>")
                 .arg(tr("Fly there").toHtmlEscaped());
  }
  m_browser->setHtml(shown);
  show();
  raise();
}

QString BalloonWindow::text() const
{
  return m_browser->toPlainText();
}

void BalloonWindow::linkClicked(const QUrl& url)
{
  if (url.scheme() == QLatin1String("terrasoar")) {
    if (m_pose) {
      emit flyToRequested(*m_pose);
    }
    return;
  }
  if (url.scheme() == QLatin1String("http") ||
      url.scheme() == QLatin1String("https") ||
      url.scheme() == QLatin1String("mailto")) {
    QDesktopServices::openUrl(url);
  }
}

} // namespace ts
