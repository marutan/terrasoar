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
#include "userguide.h"

#include <algorithm>
#include <cstdint>

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextDocument>

#include "aboutdialog.h"
#include "ts/base/buildinfo.h"

namespace ts::userguide
{

namespace
{

/** The guide's pages in the resources (mirroring docs/user/). */
const QLatin1String userPrefix("/guide/user/");
/** The resources' copy of docs/. */
const QLatin1String docsPrefix("/guide/");

/** Returns the resource path (":/guide/...") of a `qrc:` URL. */
QString resourcePath(const QUrl& url)
{
  return QLatin1Char(':') + url.path();
}

/** Returns @p url without its fragment. */
QUrl withoutFragment(const QUrl& url)
{
  QUrl page = url;
  page.setFragment(QString());
  return page;
}

/** Returns the page's text as shown: its Markdown rendered as plain text. */
QString plainText(const QUrl& url)
{
  QTextDocument document;
  document.setMarkdown(pageText(url));
  return document.toPlainText();
}

/** Returns about 140 characters of @p text around @p index, on one line. */
QString snippetAround(const QString& text, int32_t index)
{
  const int32_t context = 60;
  const int32_t start = std::max(0, index - context);
  const int32_t length = 2 * context + 20;
  QString snippet = text.mid(start, length).simplified();
  if (start > 0) {
    snippet.prepend(QStringLiteral("… "));
  }
  if (start + length < text.size()) {
    snippet.append(QStringLiteral(" …"));
  }
  return snippet;
}

/** Adds the page @p url to @p pages unless it is there already or missing. */
void addPage(std::vector<Page>& pages, const QUrl& url)
{
  const QUrl page = withoutFragment(url);
  const bool known =
      std::any_of(pages.begin(), pages.end(), [&page](const Page& existing) {
        return existing.url == page;
      });
  if (!known && pageExists(page)) {
    pages.push_back(Page{pageTitle(page), page});
  }
}

} // namespace

QUrl rootUrl()
{
  return QUrl(QStringLiteral("qrc:/guide/user/"));
}

QUrl contentsUrl()
{
  return rootUrl().resolved(QUrl(QStringLiteral("README.md")));
}

QUrl pageUrl(const QString& target)
{
  if (target.isEmpty()) {
    return contentsUrl();
  }
  return rootUrl().resolved(QUrl(target));
}

bool pageExists(const QUrl& url)
{
  return url.scheme() == QLatin1String("qrc") &&
         url.path().startsWith(userPrefix) &&
         url.path().endsWith(QLatin1String(".md")) &&
         QFile::exists(resourcePath(url));
}

QString pageText(const QUrl& url)
{
  if (!pageExists(url)) {
    return QString();
  }
  QFile file(resourcePath(url));
  if (!file.open(QIODevice::ReadOnly)) {
    return QString();
  }
  return QString::fromUtf8(file.readAll());
}

QString pageTitle(const QUrl& url)
{
  const QStringList lines = pageText(url).split(QLatin1Char('\n'));
  for (const QString& line : lines) {
    if (line.startsWith(QLatin1String("# "))) {
      return line.mid(2).trimmed();
    }
  }
  return url.fileName();
}

std::vector<Page> pages()
{
  std::vector<Page> result;
  addPage(result, contentsUrl());
  // The contents page lists the others in reading order.
  static const QRegularExpression link(QStringLiteral(R"(\]\(([^)\s]+)\))"));
  QRegularExpressionMatchIterator matches =
      link.globalMatch(pageText(contentsUrl()));
  while (matches.hasNext()) {
    const QRegularExpressionMatch match = matches.next();
    const LinkTarget target =
        resolveLink(contentsUrl(), QUrl(match.captured(1)));
    if (target.kind == LinkKind::GuidePage) {
      addPage(result, target.url);
    }
  }
  // Then any page it doesn't list (the CI check requires that there are
  // none, but the window still shows them).
  const QStringList files =
      QDir(resourcePath(rootUrl()))
          .entryList({QStringLiteral("*.md")}, QDir::Files, QDir::Name);
  for (const QString& file : files) {
    addPage(result, rootUrl().resolved(QUrl(file)));
  }
  return result;
}

QString headingSlug(const QString& heading)
{
  QString slug;
  const QString lower = heading.trimmed().toLower();
  for (const QChar c : lower) {
    if (c == QLatin1Char(' ')) {
      slug.append(QLatin1Char('-'));
    } else if (c.isLetterOrNumber() || c.isMark() || c == QLatin1Char('-') ||
               c.category() == QChar::Punctuation_Connector) {
      slug.append(c);
    }
  }
  return slug;
}

QStringList headingAnchors(const QStringList& headings)
{
  QStringList anchors;
  for (const QString& heading : headings) {
    const QString slug = headingSlug(heading);
    QString anchor = slug;
    for (int32_t repeat = 1; anchors.contains(anchor); ++repeat) {
      anchor = slug + QLatin1Char('-') + QString::number(repeat);
    }
    anchors.append(anchor);
  }
  return anchors;
}

QStringList documentHeadings(const QTextDocument& document)
{
  QStringList headings;
  for (QTextBlock block = document.begin(); block.isValid();
       block = block.next()) {
    if (block.blockFormat().headingLevel() > 0) {
      headings.append(block.text());
    }
  }
  return headings;
}

LinkTarget resolveLink(const QUrl& current, const QUrl& link)
{
  const QString scheme = link.scheme().toLower();
  if (scheme == QLatin1String("http") || scheme == QLatin1String("https") ||
      scheme == QLatin1String("mailto") || scheme == QLatin1String("ftp")) {
    return LinkTarget{LinkKind::External, link};
  }
  const QUrl absolute = current.resolved(link);
  if (absolute.scheme() != QLatin1String("qrc")) {
    return LinkTarget{LinkKind::Missing, absolute};
  }
  const QString path = absolute.path();
  if (path.startsWith(userPrefix)) {
    return LinkTarget{pageExists(withoutFragment(absolute))
                          ? LinkKind::GuidePage
                          : LinkKind::Missing,
                      absolute};
  }
  // Out of the guide: the resources' /guide/ is the repository's docs/,
  // and anything above it is the repository's root.
  const QString repositoryPath =
      path.startsWith(docsPrefix)
          ? QStringLiteral("docs/") + path.mid(docsPrefix.size())
          : path.mid(1);
  QUrl url = repositoryUrl(repositoryPath, buildInfo().version);
  url.setFragment(absolute.fragment());
  return LinkTarget{LinkKind::Repository, url};
}

QUrl repositoryUrl(const QString& path, const QString& version)
{
  return QUrl(AboutDialog::homePage() + QStringLiteral("/blob/v") + version +
              QLatin1Char('/') + path);
}

std::vector<SearchHit> search(const QString& query)
{
  std::vector<SearchHit> hits;
  static const QRegularExpression spaces(QStringLiteral("\\s+"));
  const QStringList words = query.split(spaces, Qt::SkipEmptyParts);
  if (words.isEmpty()) {
    return hits;
  }
  for (const Page& page : pages()) {
    const QString text = plainText(page.url);
    const bool all =
        std::all_of(words.begin(), words.end(), [&text](const QString& word) {
          return text.contains(word, Qt::CaseInsensitive);
        });
    if (all) {
      hits.push_back(SearchHit{
          page,
          snippetAround(text, static_cast<int32_t>(text.indexOf(
                                  words.first(), 0, Qt::CaseInsensitive)))});
    }
  }
  return hits;
}

} // namespace ts::userguide
