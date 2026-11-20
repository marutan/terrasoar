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
 * The balloon of a KML placemark: its description and data.
 */
#ifndef TS_APP_BALLOONWINDOW_H
#define TS_APP_BALLOONWINDOW_H

#include <optional>

#include <QDialog>
#include <QString>

#include "ts/camera/camera.h"

class QEvent;
class QTextBrowser;
class QUrl;
class QWidget;

namespace ts
{

/**
 * Shows a KML placemark's balloon (kmlBalloonHtml()) when it is clicked:
 * rich text only, nothing run and nothing fetched from the web; web links
 * open in the browser. A "Fly there" link flies to the placemark's view.
 *
 * @guithread
 */
class BalloonWindow : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(BalloonWindow)

public:
  /** Creates the window, hidden. */
  explicit BalloonWindow(QWidget* parent);

  /** Destroys it. */
  ~BalloonWindow() override;

  /**
   * Shows @p html titled @p title on @p background, offering to fly to
   * @p pose if given; local images resolve against @p baseDirectory.
   */
  void showBalloon(const QString& title, const QString& html,
                   const QColor& background,
                   const std::optional<CameraPose>& pose,
                   const QString& baseDirectory);

  /** Returns the text shown (for tests). */
  QString text() const;

signals:
  /** The user asked to fly to the placemark. */
  void flyToRequested(const ts::CameraPose& pose);

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

private:
  void linkClicked(const QUrl& url);

  QTextBrowser* m_browser = nullptr;
  std::optional<CameraPose> m_pose;
  QString m_title;
  QString m_html;
  QColor m_background;
  QString m_base;
};

} // namespace ts

Q_DECLARE_METATYPE(ts::CameraPose)

#endif // TS_APP_BALLOONWINDOW_H
