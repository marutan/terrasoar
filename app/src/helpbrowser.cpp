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
#include "helpbrowser.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <QAbstractTextDocumentLayout>
#include <QDesktopServices>
#include <QImage>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QTextFragment>
#include <QTextImageFormat>

#include "userguide.h"

namespace ts
{

namespace
{

/** An image in the document, to be resized. */
struct ImageSpan {
  int32_t position = 0;
  int32_t length = 0;
  QTextImageFormat format;
};

} // namespace

HelpBrowser::HelpBrowser(QWidget* parent) : QTextBrowser(parent)
{
  setObjectName(QStringLiteral("helpBrowser"));
  // Every link goes through followLink(), which decides where it opens.
  setOpenLinks(false);
  setOpenExternalLinks(false);
  connect(this, &QTextBrowser::anchorClicked, this, &HelpBrowser::followLink);
  // After every page load, including Back and Forward (Qt 5 has no
  // doSetSource() to override).
  connect(this, &QTextBrowser::sourceChanged, this, &HelpBrowser::pageLoaded);
}

HelpBrowser::~HelpBrowser() = default;

bool HelpBrowser::showHeading(const QString& anchor)
{
  const qsizetype index = m_anchors.indexOf(anchor);
  if (index < 0) {
    return false;
  }
  qsizetype heading = -1;
  for (QTextBlock block = document()->begin(); block.isValid();
       block = block.next()) {
    if (block.blockFormat().headingLevel() > 0 && ++heading == index) {
      const QRectF bounds =
          document()->documentLayout()->blockBoundingRect(block);
      verticalScrollBar()->setValue(static_cast<int32_t>(bounds.top()));
      setTextCursor(QTextCursor(block));
      return true;
    }
  }
  return false;
}

QString HelpBrowser::headingAtTop() const
{
  // The last heading that starts at or above the top of the view.
  const double top = verticalScrollBar()->value() + 1.0;
  QString anchor;
  int32_t heading = -1;
  for (QTextBlock block = document()->begin(); block.isValid();
       block = block.next()) {
    if (block.blockFormat().headingLevel() <= 0) {
      continue;
    }
    ++heading;
    if (document()->documentLayout()->blockBoundingRect(block).top() > top) {
      break;
    }
    anchor = m_anchors.value(heading);
  }
  return anchor;
}

void HelpBrowser::pageLoaded(const QUrl& name)
{
  m_anchors =
      userguide::headingAnchors(userguide::documentHeadings(*document()));
  fitImages();
  if (!name.fragment().isEmpty()) {
    showHeading(name.fragment());
  }
}

void HelpBrowser::resizeEvent(QResizeEvent* event)
{
  QTextBrowser::resizeEvent(event);
  if (!m_imageSizes.isEmpty()) {
    fitImages();
  }
}

void HelpBrowser::followLink(const QUrl& link)
{
  const userguide::LinkTarget target = userguide::resolveLink(source(), link);
  switch (target.kind) {
  case userguide::LinkKind::GuidePage:
    setSource(target.url, QTextDocument::MarkdownResource);
    break;
  case userguide::LinkKind::External:
  case userguide::LinkKind::Repository:
    emit externalLinkActivated(target.url);
    if (m_openOutsideLinks) {
      QDesktopServices::openUrl(target.url);
    }
    break;
  case userguide::LinkKind::Missing:
    break;
  }
}

void HelpBrowser::fitImages()
{
  const int32_t margin =
      2 * static_cast<int32_t>(document()->documentMargin()) + 8;
  const double available = std::max(64, viewport()->width() - margin);
  // Collect first: changing a fragment's format can merge fragments.
  std::vector<ImageSpan> images;
  for (QTextBlock block = document()->begin(); block.isValid();
       block = block.next()) {
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
      const QTextFragment fragment = it.fragment();
      const QTextImageFormat format = fragment.charFormat().toImageFormat();
      if (fragment.isValid() && format.isValid()) {
        images.push_back(
            ImageSpan{fragment.position(), fragment.length(), format});
      }
    }
  }
  for (ImageSpan& image : images) {
    const QSize natural =
        imageSize(source().resolved(QUrl(image.format.name())));
    if (natural.isEmpty()) {
      continue;
    }
    const double width =
        std::min(static_cast<double>(natural.width()), available);
    image.format.setWidth(width);
    image.format.setHeight(width * natural.height() / natural.width());
    QTextCursor cursor(document());
    cursor.setPosition(image.position);
    cursor.setPosition(image.position + image.length, QTextCursor::KeepAnchor);
    cursor.setCharFormat(image.format);
  }
}

QSize HelpBrowser::imageSize(const QUrl& url)
{
  const QHash<QUrl, QSize>::const_iterator known = m_imageSizes.constFind(url);
  if (known != m_imageSizes.constEnd()) {
    return known.value();
  }
  const QVariant resource = loadResource(QTextDocument::ImageResource, url);
  QImage image = resource.value<QImage>();
  if (image.isNull()) {
    image.loadFromData(resource.toByteArray());
  }
  m_imageSizes.insert(url, image.size());
  return image.size();
}

} // namespace ts
