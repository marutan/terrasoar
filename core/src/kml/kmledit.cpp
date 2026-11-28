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
#include "ts/kml/kmledit.h"

#include <algorithm>
#include <utility>

#include <QStringList>

#include "ts/kml/kmlfeatureprovider.h"

namespace ts
{

namespace
{

/** A list of coordinates of a geometry, to change. */
struct Ring {
  std::vector<KmlCoordinate>* coordinates = nullptr;
  /** A ring: its last point repeats its first. */
  bool closed = false;
  /** Points can't be added or taken away (a point, a track). */
  bool fixed = false;
};

void collectRings(KmlGeometry& geometry, std::vector<Ring>& rings)
{
  switch (geometry.kind) {
  case KmlGeometry::Kind::Point:
  case KmlGeometry::Kind::Track:
    rings.push_back(Ring{&geometry.coordinates, false, true});
    break;
  case KmlGeometry::Kind::LineString:
    rings.push_back(Ring{&geometry.coordinates, false, false});
    break;
  case KmlGeometry::Kind::LinearRing:
    rings.push_back(Ring{&geometry.coordinates, true, false});
    break;
  case KmlGeometry::Kind::Polygon:
    rings.push_back(Ring{&geometry.coordinates, true, false});
    for (std::vector<KmlCoordinate>& hole : geometry.inner) {
      rings.push_back(Ring{&hole, true, false});
    }
    break;
  case KmlGeometry::Kind::MultiGeometry:
    for (KmlGeometry& child : geometry.children) {
      collectRings(child, rings);
    }
    break;
  }
}

std::vector<Ring> ringsOf(KmlFeature& placemark)
{
  std::vector<Ring> rings;
  if (placemark.geometry) {
    collectRings(*placemark.geometry, rings);
  }
  return rings;
}

/** How many vertices a ring has, its repeated closing point not counted. */
int32_t distinct(Ring ring)
{
  const std::vector<KmlCoordinate>& c = *ring.coordinates;
  const int32_t n = static_cast<int32_t>(c.size());
  return ring.closed && n >= 2 && c.front() == c.back() ? n - 1 : n;
}

/** The ring @p ring of @p placemark; null if none. */
std::optional<Ring> ringAt(KmlFeature& placemark, int32_t ring)
{
  std::vector<Ring> rings = ringsOf(placemark);
  if (ring < 0 || static_cast<std::size_t>(ring) >= rings.size()) {
    return std::nullopt;
  }
  return rings[static_cast<std::size_t>(ring)];
}

std::vector<int32_t> pathRows(const QString& path)
{
  std::vector<int32_t> rows;
  if (path.isEmpty()) {
    return rows;
  }
  for (const QString& part : path.split(QLatin1Char('.'))) {
    bool ok = false;
    const int32_t row = part.toInt(&ok);
    rows.push_back(ok ? row : -1);
  }
  return rows;
}

bool isContainer(const KmlFeature& feature)
{
  return feature.kind == KmlFeature::Kind::Document ||
         feature.kind == KmlFeature::Kind::Folder;
}

/** A ground overlay's corners: its quad, or its box's (SW, SE, NE, NW). */
std::vector<KmlCoordinate> overlayCorners(const KmlFeature& overlay)
{
  const KmlLatLonBox box = overlay.latLonBox.value_or(KmlLatLonBox{});
  if (box.quad.size() == 4) {
    return box.quad;
  }
  return {KmlCoordinate{box.west, box.south, 0.0},
          KmlCoordinate{box.east, box.south, 0.0},
          KmlCoordinate{box.east, box.north, 0.0},
          KmlCoordinate{box.west, box.north, 0.0}};
}

// Longitude, then latitude, as everywhere in the program.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool moveOverlayCorner(KmlFeature& overlay, int32_t index, double lonDeg,
                       double latDeg)
{
  if (!overlay.latLonBox || index < 0 || index > 3) {
    return false;
  }
  KmlLatLonBox& box = *overlay.latLonBox;
  if (box.quad.size() == 4) {
    box.quad[static_cast<std::size_t>(index)].lonDeg = lonDeg;
    box.quad[static_cast<std::size_t>(index)].latDeg = latDeg;
    return true;
  }
  // A box stays a box: the corner's two edges move.
  (index == 0 || index == 3 ? box.west : box.east) = lonDeg;
  (index == 0 || index == 1 ? box.south : box.north) = latDeg;
  if (box.south > box.north) {
    std::swap(box.south, box.north);
  }
  return true;
}

int32_t countUsers(const KmlFeature& feature, const QString& url)
{
  int32_t count = feature.styleUrl == url ? 1 : 0;
  for (const KmlStyleMap& map : feature.styleMaps) {
    count += map.normalUrl == url || map.highlightUrl == url ? 1 : 0;
  }
  for (const KmlFeature& child : feature.children) {
    count += countUsers(child, url);
  }
  return count;
}

} // namespace

KmlFeature* kmlFeatureAt(KmlDocument& document, const QString& path)
{
  KmlFeature* feature = &document.root;
  for (const int32_t row : pathRows(path)) {
    if (row < 0 || static_cast<std::size_t>(row) >= feature->children.size()) {
      return nullptr;
    }
    feature = &feature->children[static_cast<std::size_t>(row)];
  }
  return feature;
}

namespace
{

bool findPath(const KmlFeature& at, const KmlFeature* feature,
              const QString& path, QString* found)
{
  if (&at == feature) {
    *found = path;
    return true;
  }
  for (std::size_t i = 0; i < at.children.size(); ++i) {
    if (findPath(at.children[i], feature,
                 kmlChildPath(path, static_cast<int32_t>(i)), found)) {
      return true;
    }
  }
  return false;
}

} // namespace

std::optional<QString> kmlPathOf(const KmlDocument& document,
                                 const KmlFeature* feature)
{
  QString path;
  if (findPath(document.root, feature, QString(), &path)) {
    return path;
  }
  return std::nullopt;
}

KmlFeature kmlWithoutSource(KmlFeature feature)
{
  feature.source.reset();
  for (KmlFeature& child : feature.children) {
    child = kmlWithoutSource(std::move(child));
  }
  return feature;
}

const KmlFeature* kmlFeatureAt(const KmlDocument& document, const QString& path)
{
  const KmlFeature* feature = &document.root;
  for (const int32_t row : pathRows(path)) {
    if (row < 0 || static_cast<std::size_t>(row) >= feature->children.size()) {
      return nullptr;
    }
    feature = &feature->children[static_cast<std::size_t>(row)];
  }
  return feature;
}

QString kmlChildPath(const QString& parentPath, int32_t row)
{
  return parentPath.isEmpty()
             ? QString::number(row)
             : parentPath + QLatin1Char('.') + QString::number(row);
}

QString kmlParentPath(const QString& path)
{
  const int32_t dot = static_cast<int32_t>(path.lastIndexOf(QLatin1Char('.')));
  return dot < 0 ? QString() : path.left(dot);
}

int32_t kmlRowOf(const QString& path)
{
  if (path.isEmpty()) {
    return -1;
  }
  bool ok = false;
  const int32_t row = path.section(QLatin1Char('.'), -1).toInt(&ok);
  return ok ? row : -1;
}

QString kmlInsertFeature(KmlDocument& document, const QString& parentPath,
                         KmlFeature feature, int32_t row)
{
  KmlFeature* parent = kmlFeatureAt(document, parentPath);
  if (parent == nullptr || !isContainer(*parent)) {
    return QString();
  }
  const int32_t size = static_cast<int32_t>(parent->children.size());
  const int32_t at = row < 0 || row > size ? size : row;
  parent->children.insert(parent->children.begin() + at, std::move(feature));
  return kmlChildPath(parentPath, at);
}

std::optional<KmlFeature> kmlTakeFeature(KmlDocument& document,
                                         const QString& path)
{
  KmlFeature* parent = kmlFeatureAt(document, kmlParentPath(path));
  const int32_t row = kmlRowOf(path);
  if (parent == nullptr || row < 0 ||
      static_cast<std::size_t>(row) >= parent->children.size()) {
    return std::nullopt;
  }
  KmlFeature taken = std::move(parent->children[static_cast<std::size_t>(row)]);
  parent->children.erase(parent->children.begin() + row);
  return taken;
}

QString kmlMoveFeature(KmlDocument& document, const QString& path,
                       const QString& parentPath, int32_t row)
{
  // Not into itself or anything inside it.
  if (path.isEmpty() || parentPath == path ||
      parentPath.startsWith(path + QLatin1Char('.'))) {
    return QString();
  }
  KmlFeature* parent = kmlFeatureAt(document, parentPath);
  if (parent == nullptr || !isContainer(*parent) ||
      kmlFeatureAt(document, path) == nullptr) {
    return QString();
  }
  // The parent's path once the feature is out (an earlier sibling of one
  // of its containers moves up a row).
  const std::vector<int32_t> moved = pathRows(path);
  std::vector<int32_t> target = pathRows(parentPath);
  if (target.size() >= moved.size() &&
      std::equal(moved.begin(), moved.end() - 1, target.begin()) &&
      target[moved.size() - 1] > moved.back()) {
    --target[moved.size() - 1];
  }
  QStringList parts;
  for (const int32_t r : target) {
    parts << QString::number(r);
  }
  std::optional<KmlFeature> feature = kmlTakeFeature(document, path);
  if (!feature) {
    return QString();
  }
  return kmlInsertFeature(document, parts.join(QLatin1Char('.')),
                          std::move(*feature), row);
}

std::vector<KmlHandle> kmlHandles(const KmlFeature& placemark)
{
  std::vector<KmlHandle> handles;
  if (placemark.kind == KmlFeature::Kind::GroundOverlay &&
      placemark.latLonBox) {
    const std::vector<KmlCoordinate> corners = overlayCorners(placemark);
    for (std::size_t i = 0; i < corners.size(); ++i) {
      handles.push_back(KmlHandle{0, static_cast<int32_t>(i), false,
                                  corners[i].lonDeg, corners[i].latDeg});
    }
    return handles;
  }
  KmlFeature copy = placemark;
  const std::vector<Ring> rings = ringsOf(copy);
  for (std::size_t r = 0; r < rings.size(); ++r) {
    const Ring& ring = rings[r];
    const std::vector<KmlCoordinate>& c = *ring.coordinates;
    const int32_t n = distinct(ring);
    const int32_t index = static_cast<int32_t>(r);
    for (int32_t i = 0; i < n; ++i) {
      const KmlCoordinate& at = c[static_cast<std::size_t>(i)];
      handles.push_back(KmlHandle{index, i, false, at.lonDeg, at.latDeg});
    }
    if (ring.fixed) {
      continue;
    }
    // Middles of the edges, the closing one of a ring too.
    const int32_t edges = ring.closed ? n : n - 1;
    for (int32_t i = 0; i < edges; ++i) {
      const KmlCoordinate& a = c[static_cast<std::size_t>(i)];
      const KmlCoordinate& b = c[static_cast<std::size_t>((i + 1) % n)];
      handles.push_back(KmlHandle{index, i + 1, true,
                                  (a.lonDeg + b.lonDeg) / 2.0,
                                  (a.latDeg + b.latDeg) / 2.0});
    }
  }
  return handles;
}

bool kmlMoveVertex(KmlFeature& placemark, int32_t ring, int32_t index,
                   double lonDeg, double latDeg)
{
  if (placemark.kind == KmlFeature::Kind::GroundOverlay) {
    return ring == 0 && moveOverlayCorner(placemark, index, lonDeg, latDeg);
  }
  const std::optional<Ring> found = ringAt(placemark, ring);
  if (!found || index < 0 || index >= distinct(*found)) {
    return false;
  }
  std::vector<KmlCoordinate>& c = *found->coordinates;
  const bool closing = found->closed && c.size() >= 2 && c.front() == c.back();
  c[static_cast<std::size_t>(index)].lonDeg = lonDeg;
  c[static_cast<std::size_t>(index)].latDeg = latDeg;
  if (closing && index == 0) {
    c.back() = c.front();
  }
  return true;
}

// The ring, then the vertex in it (as KmlHandle).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool kmlInsertVertex(KmlFeature& placemark, int32_t ring, int32_t index,
                     double lonDeg, double latDeg)
{
  const std::optional<Ring> found = ringAt(placemark, ring);
  if (!found || found->fixed || index < 0 ||
      index > static_cast<int32_t>(found->coordinates->size())) {
    return false;
  }
  std::vector<KmlCoordinate>& c = *found->coordinates;
  const std::size_t at = static_cast<std::size_t>(index);
  const double before = at > 0 ? c[at - 1].altM : 0.0;
  const double after = at < c.size() ? c[at].altM : before;
  c.insert(c.begin() + index,
           KmlCoordinate{lonDeg, latDeg, (before + after) / 2.0});
  return true;
}

// The ring, then the vertex in it (as KmlHandle).
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool kmlDeleteVertex(KmlFeature& placemark, int32_t ring, int32_t index)
{
  const std::optional<Ring> found = ringAt(placemark, ring);
  if (!found || found->fixed || index < 0 || index >= distinct(*found)) {
    return false;
  }
  const int32_t least = found->closed ? 3 : 2;
  if (distinct(*found) <= least) {
    return false;
  }
  std::vector<KmlCoordinate>& c = *found->coordinates;
  const bool closing = found->closed && c.front() == c.back();
  c.erase(c.begin() + index);
  if (closing && index == 0) {
    c.back() = c.front();
  }
  return true;
}

// Longitude, then latitude, as everywhere in the program.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void kmlTranslate(KmlFeature& placemark, double dLonDeg, double dLatDeg)
{
  for (const Ring& ring : ringsOf(placemark)) {
    for (KmlCoordinate& c : *ring.coordinates) {
      c.lonDeg += dLonDeg;
      c.latDeg = std::clamp(c.latDeg + dLatDeg, -90.0, 90.0);
    }
  }
  if (placemark.latLonBox) {
    KmlLatLonBox& box = *placemark.latLonBox;
    box.west += dLonDeg;
    box.east += dLonDeg;
    box.south = std::clamp(box.south + dLatDeg, -90.0, 90.0);
    box.north = std::clamp(box.north + dLatDeg, -90.0, 90.0);
    for (KmlCoordinate& c : box.quad) {
      c.lonDeg += dLonDeg;
      c.latDeg = std::clamp(c.latDeg + dLatDeg, -90.0, 90.0);
    }
  }
}

int32_t kmlStyleUsers(const KmlDocument& document, const QString& id)
{
  return countUsers(document.root, QLatin1Char('#') + id);
}

// The key drawing knows it by, then its file.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
KmlEditDocument::KmlEditDocument(KmlDocument document, QString key,
                                 QString filePath)
    : m_document(std::move(document)), m_key(std::move(key)),
      m_filePath(std::move(filePath))
{
  m_document.path = m_key;
  publish();
}

void KmlEditDocument::setSaved(const QString& filePath)
{
  m_filePath = filePath;
  m_savedRevision = m_revision;
}

bool KmlEditDocument::edit(const QString& description,
                           const std::function<bool(KmlDocument&)>& change)
{
  Step before{description, m_document, m_revision};
  if (!change(m_document)) {
    m_document = std::move(before.document);
    return false;
  }
  m_undo.push_back(std::move(before));
  if (m_undo.size() > mostUndo) {
    m_undo.erase(m_undo.begin());
  }
  m_redo.clear();
  m_revision = m_nextRevision++;
  publish();
  return true;
}

void KmlEditDocument::adjust(const std::function<void(KmlDocument&)>& change)
{
  change(m_document);
  // Kept by undo steps too, so it isn't lost by undoing an edit.
  for (Step& step : m_undo) {
    change(step.document);
  }
  for (Step& step : m_redo) {
    change(step.document);
  }
  m_revision = m_nextRevision++;
  publish();
}

QString KmlEditDocument::undoText() const
{
  return m_undo.empty() ? QString() : m_undo.back().description;
}

QString KmlEditDocument::redoText() const
{
  return m_redo.empty() ? QString() : m_redo.back().description;
}

bool KmlEditDocument::undo()
{
  if (m_undo.empty()) {
    return false;
  }
  Step step = std::move(m_undo.back());
  m_undo.pop_back();
  m_redo.push_back(Step{step.description, m_document, m_revision});
  m_document = std::move(step.document);
  m_revision = step.revision;
  publish();
  return true;
}

bool KmlEditDocument::redo()
{
  if (m_redo.empty()) {
    return false;
  }
  Step step = std::move(m_redo.back());
  m_redo.pop_back();
  m_undo.push_back(Step{step.description, m_document, m_revision});
  m_document = std::move(step.document);
  m_revision = step.revision;
  publish();
  return true;
}

void KmlEditDocument::publish()
{
  m_shared = makeSharedKmlDocument(m_document);
  registerKmlDocument(m_key, m_shared);
}

} // namespace ts
