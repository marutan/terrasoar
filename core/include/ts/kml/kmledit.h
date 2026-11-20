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
 * Editing KML documents: changing their features and geometry, and an
 * editable document with undo and redo.
 */
#ifndef TS_KML_KMLEDIT_H
#define TS_KML_KMLEDIT_H

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <QString>

#include "ts/kml/kmldocument.h"

namespace ts
{

/** Returns the feature at @p path of @p document to change; null if none. */
KmlFeature* kmlFeatureAt(KmlDocument& document, const QString& path);

/** Returns the feature at @p path of @p document; null if none. */
const KmlFeature* kmlFeatureAt(const KmlDocument& document,
                               const QString& path);

/**
 * Returns the path of @p feature (a feature of @p document, by address);
 * nothing if it isn't one. Empty for the root.
 */
std::optional<QString> kmlPathOf(const KmlDocument& document,
                                 const KmlFeature* feature);

/**
 * Returns @p feature without where it was in its document's text, so it
 * is written anew wherever it goes (a copy for another document).
 */
KmlFeature kmlWithoutSource(KmlFeature feature);

/** Returns the path of child @p row of the feature at @p parentPath. */
QString kmlChildPath(const QString& parentPath, int32_t row);

/** Returns the path of the container holding the feature at @p path. */
QString kmlParentPath(const QString& path);

/** Returns the feature at @p path's row in its container (-1 for root). */
int32_t kmlRowOf(const QString& path);

/**
 * Puts @p feature into the container at @p parentPath (the root for an
 * empty path) at @p row (at the end if out of range); returns its path, or
 * empty if @p parentPath isn't a Document or Folder.
 */
QString kmlInsertFeature(KmlDocument& document, const QString& parentPath,
                         KmlFeature feature, int32_t row = -1);

/** Takes the feature at @p path out of @p document; nothing for the root. */
std::optional<KmlFeature> kmlTakeFeature(KmlDocument& document,
                                         const QString& path);

/**
 * Moves the feature at @p path into the container at @p parentPath at
 * @p row (as kmlInsertFeature(), @p row counted after taking it out);
 * returns its new path, or empty (and nothing moved) if it can't go there
 * (not a container, or inside itself).
 */
QString kmlMoveFeature(KmlDocument& document, const QString& path,
                       const QString& parentPath, int32_t row);

/**
 * A point of a placemark's geometry that can be dragged: a vertex, or the
 * middle of an edge (dragging it adds a vertex there).
 */
struct KmlHandle {
  /**
   * Which list of coordinates: the geometry's lines, rings and points in
   * order (a polygon's outer ring, then its holes; a MultiGeometry's
   * members in turn).
   */
  int32_t ring = 0;
  /** The vertex; for a middle, the vertex it goes before. */
  int32_t index = 0;
  /** True for the middle of an edge. */
  bool middle = false;
  /** Where it is. */
  double lonDeg = 0.0;
  /** As lonDeg. */
  double latDeg = 0.0;
};

/** Returns the handles of @p placemark's geometry (closing points once). */
std::vector<KmlHandle> kmlHandles(const KmlFeature& placemark);

/**
 * Moves vertex @p index of @p ring of @p placemark to @p lonDeg,
 * @p latDeg, keeping its height (a closed ring's first and last together).
 * Returns false if there is no such vertex.
 */
bool kmlMoveVertex(KmlFeature& placemark, int32_t ring, int32_t index,
                   double lonDeg, double latDeg);

/**
 * Adds a vertex at @p lonDeg, @p latDeg before vertex @p index of
 * @p ring (its height between its neighbours'). Returns false if there is
 * no such place.
 */
bool kmlInsertVertex(KmlFeature& placemark, int32_t ring, int32_t index,
                     double lonDeg, double latDeg);

/**
 * Removes vertex @p index of @p ring; refused (false) if a line would keep
 * fewer than 2 or a ring fewer than 3 distinct vertices, or for a point.
 */
bool kmlDeleteVertex(KmlFeature& placemark, int32_t ring, int32_t index);

/** Moves all of @p placemark's geometry by @p dLonDeg, @p dLatDeg. */
void kmlTranslate(KmlFeature& placemark, double dLonDeg, double dLatDeg);

/** Returns how many features use the shared style or map with @p id. */
int32_t kmlStyleUsers(const KmlDocument& document, const QString& id);

/**
 * A KML document being edited: changes are made through edit(), each
 * undoable; the latest state is also given as an unchanging shared copy
 * (shared()), registered with sharedKmlDocument() under the document's
 * path, for drawing.
 *
 * @reentrant (one thread at a time)
 */
class KmlEditDocument
{
public:
  /**
   * Edits @p document, known to drawing as @p key (its path is set to it);
   * @p filePath is where it is saved (empty for a new document).
   */
  KmlEditDocument(KmlDocument document, QString key, QString filePath);

  /** Returns the document as it is now. */
  const KmlDocument& document() const { return m_document; }

  /** Returns the document as it is now, shared (for drawing). */
  std::shared_ptr<const KmlDocument> shared() const { return m_shared; }

  /** Returns the key drawing knows it by (also its path). */
  const QString& key() const { return m_key; }

  /** Returns where it is saved; empty if never saved. */
  const QString& filePath() const { return m_filePath; }

  /** Notes it was saved to @p filePath as it is now. */
  void setSaved(const QString& filePath);

  /** Returns true if it changed since it was opened or saved. */
  bool isModified() const { return m_revision != m_savedRevision; }

  /** Returns a number that changes with every change, undo and redo. */
  int64_t revision() const { return m_revision; }

  /**
   * Makes the change @p change (described by @p description for Undo and
   * Redo); returns false, keeping nothing, if @p change returns false.
   */
  bool edit(const QString& description,
            const std::function<bool(KmlDocument&)>& change);

  /**
   * Changes the document without an undo step (things that aren't edits:
   * folders switched on and off).
   */
  void adjust(const std::function<void(KmlDocument&)>& change);

  /** Returns true if there is a change to undo. */
  bool canUndo() const { return !m_undo.empty(); }

  /** Returns true if there is an undone change to redo. */
  bool canRedo() const { return !m_redo.empty(); }

  /** Returns the description of the change Undo would undo. */
  QString undoText() const;

  /** Returns the description of the change Redo would redo. */
  QString redoText() const;

  /** Undoes the last change; false if none. */
  bool undo();

  /** Redoes the last undone change; false if none. */
  bool redo();

  /** How many changes are kept to undo. */
  static constexpr std::size_t mostUndo = 100;

private:
  struct Step {
    QString description;
    KmlDocument document;
    int64_t revision = 0;
  };

  void publish();

  KmlDocument m_document;
  QString m_key;
  QString m_filePath;
  std::shared_ptr<const KmlDocument> m_shared;
  std::vector<Step> m_undo;
  std::vector<Step> m_redo;
  int64_t m_revision = 0;
  int64_t m_savedRevision = 0;
  int64_t m_nextRevision = 1;
};

} // namespace ts

#endif // TS_KML_KMLEDIT_H
