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
 * The user guide built into the program: its pages, the links between
 * them, heading anchors and search. See docs/design/help.md.
 */
#ifndef TS_APP_USERGUIDE_H
#define TS_APP_USERGUIDE_H

#include <vector>

#include <QString>
#include <QStringList>
#include <QUrl>

class QTextDocument;

namespace ts::userguide
{

/** One page of the guide. */
struct Page {
  /** The page's title, its first heading. */
  QString title;
  /** The page, e.g. `qrc:/guide/user/terrain.md`. */
  QUrl url;
};

/** Where a link in the guide goes. */
enum class LinkKind {
  /** A page of the guide, opened in the help window. */
  GuidePage,
  /** A web or mail address, opened by the system. */
  External,
  /** A file of the source tree outside the guide, opened on GitHub. */
  Repository,
  /** A guide page that doesn't exist. */
  Missing,
};

/** A link resolved against the page it is on. */
struct LinkTarget {
  /** What kind of link it is. */
  LinkKind kind = LinkKind::Missing;
  /** Where it goes: a `qrc:` page (with its fragment) or a web address. */
  QUrl url;
};

/** One page that matches a search. */
struct SearchHit {
  /** The page. */
  Page page;
  /** The text around the first match, on one line. */
  QString snippet;
};

/**
 * Returns the guide's directory in the resources, `qrc:/guide/user/`. The
 * resources mirror `docs/`: the pages are under `/guide/user/` and the
 * images under `/guide/images/`, so the pages' relative links work
 * unchanged.
 */
QUrl rootUrl();

/** Returns the contents page, `README.md`. */
QUrl contentsUrl();

/**
 * Returns the URL of @p target, a page name relative to the guide with an
 * optional heading anchor, e.g. `terrain.md#elevation-profiles`. Empty
 * gives the contents page.
 */
QUrl pageUrl(const QString& target);

/** Returns true if @p url is a page built into the program. */
bool pageExists(const QUrl& url);

/** Returns the Markdown text of the page @p url; empty if missing. */
QString pageText(const QUrl& url);

/** Returns the title of the page @p url (its first `#` heading). */
QString pageTitle(const QUrl& url);

/**
 * Returns the guide's pages in reading order: the contents page, then the
 * pages it links to in the order it lists them, then any other pages.
 */
std::vector<Page> pages();

/**
 * Returns GitHub's anchor for a heading, as GitHub makes them: lower
 * case, letters, digits, `-` and `_` kept, spaces turned into `-`, all
 * other characters dropped. "Space on disk" gives `space-on-disk`.
 */
QString headingSlug(const QString& heading);

/**
 * Returns the anchors of @p headings in order, with GitHub's numbering of
 * repeats: a second "Notes" gives `notes-1`.
 */
QStringList headingAnchors(const QStringList& headings);

/** Returns the text of every heading in @p document, in order. */
QStringList documentHeadings(const QTextDocument& document);

/**
 * Resolves @p link, as written on the page @p current:
 * - `http`, `https`, `mailto` and `ftp` addresses are External;
 * - links to pages of the guide (with their anchors) are GuidePage, or
 *   Missing if the page isn't built in;
 * - links out of the guide (`../building.md`, `../design/...`) are
 *   Repository, mapped to the file on GitHub at this version's tag.
 */
LinkTarget resolveLink(const QUrl& current, const QUrl& link);

/**
 * Returns the GitHub address of @p path, relative to the repository's
 * root (e.g. `docs/building.md`), at the tag of @p version (`v0.0.1`).
 */
QUrl repositoryUrl(const QString& path, const QString& version);

/**
 * Returns the pages whose text contains every word of @p query (case
 * insensitive), in reading order; nothing for an empty query.
 */
std::vector<SearchHit> search(const QString& query);

} // namespace ts::userguide

#endif // TS_APP_USERGUIDE_H
