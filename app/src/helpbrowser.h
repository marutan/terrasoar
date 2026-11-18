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
 * The text view of the user guide window: shows the guide's Markdown
 * pages, follows their links and heading anchors, and fits images to the
 * window.
 */
#ifndef TS_APP_HELPBROWSER_H
#define TS_APP_HELPBROWSER_H

#include <QHash>
#include <QSize>
#include <QStringList>
#include <QTextBrowser>
#include <QTextDocument>
#include <QUrl>

class QResizeEvent;

namespace ts
{

/**
 * Shows pages of the user guide (Markdown, built into the program). Links
 * are handled by kind (see userguide::resolveLink()):
 * - guide pages open here, scrolled to the heading their anchor names
 *   (GitHub's anchors, e.g. `offline-use.md#space-on-disk`);
 * - web addresses, and files of the source tree outside the guide (on
 *   GitHub, at this version's tag), open in the system's browser.
 *
 * Images wider than the view are scaled down to fit it. Open pages with
 * setSource(url, QTextDocument::MarkdownResource): Qt 5 doesn't tell
 * Markdown by the file name.
 */
class HelpBrowser : public QTextBrowser
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(HelpBrowser)

public:
  /** Creates an empty view. */
  explicit HelpBrowser(QWidget* parent);
  ~HelpBrowser() override;

  /** Returns the anchors of the current page's headings, in order. */
  const QStringList& headingAnchors() const { return m_anchors; }

  /**
   * Scrolls so that the heading with the anchor @p anchor is at the top.
   * Returns false if the page has no such heading.
   */
  bool showHeading(const QString& anchor);

  /** Returns the anchor of the heading at the top of the view, if any. */
  QString headingAtTop() const;

  /**
   * Chooses whether links out of the guide are opened in the system's
   * browser (the default). Tests turn it off and watch
   * externalLinkActivated() instead.
   */
  void setOpenOutsideLinks(bool open) { m_openOutsideLinks = open; }

signals:
  /** A link out of the guide was followed, to @p url. */
  void externalLinkActivated(const QUrl& url);

protected:
  /** Fits the images to the new width. */
  void resizeEvent(QResizeEvent* event) override;

private:
  void followLink(const QUrl& link);
  void pageLoaded(const QUrl& name);
  void fitImages();
  QSize imageSize(const QUrl& url);

  QStringList m_anchors;
  QHash<QUrl, QSize> m_imageSizes;
  bool m_openOutsideLinks = true;
};

} // namespace ts

#endif // TS_APP_HELPBROWSER_H
