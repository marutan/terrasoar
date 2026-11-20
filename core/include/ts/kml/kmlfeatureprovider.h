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
 * A folder of a KML document as a source of vector features.
 */
#ifndef TS_KML_KMLFEATUREPROVIDER_H
#define TS_KML_KMLFEATUREPROVIDER_H

#include <memory>
#include <optional>
#include <vector>

#include <QJsonObject>
#include <QString>

#include "ts/kml/kmldocument.h"
#include "ts/providers/tileprovider.h"

namespace ts
{

/**
 * Returns the document at @p path, read once and shared while anything
 * holds it (every layer of one file shares one copy); null, with
 * @p errorString set, if it can't be read.
 *
 * @threadsafe
 */
std::shared_ptr<const KmlDocument> sharedKmlDocument(const QString& path,
                                                     QString* errorString);

/**
 * Returns the container (Document or Folder) at @p containerPath in
 * @p document: child indices from the root separated by dots ("" for the
 * root, "0.2" for the root's first child's third); null if there is none.
 */
const KmlFeature* kmlContainer(const KmlDocument& document,
                               const QString& containerPath);

/**
 * Returns the source object of a layer showing the placemarks directly in
 * the container at @p containerPath of the KML file at @p path:
 * `{"kind": "kml", "path": …, "container": …}`.
 */
QJsonObject kmlLayerSource(const QString& path, const QString& containerPath);

/**
 * Returns @p placemark's geometry as vector features (a MultiGeometry
 * gives one per member) with its name, description and extended data as
 * attributes and its KML style as their own (@p style, resolved): line
 * colour and width, polygon fill and outline, the icon's colour and size
 * for points (a yellow dot for the default icon). Longitudes are made
 * continuous across the antimeridian; a feature reaching past ±180° also
 * comes shifted by 360°, so the tiles on each side clip their part.
 */
std::vector<Feature> kmlPlacemarkFeatures(const KmlFeature& placemark,
                                          const KmlStyle& style);

/**
 * The placemarks directly in one container of a KML document, as vector
 * features: their styles from the document (KmlStyleResolver), served by
 * tile from memory (a tile gets the features whose extent meets it), and
 * the placemarks under a point for balloons and What Is Here.
 *
 * @threadsafe
 */
class KmlFeatureProvider : public FeatureProvider
{
public:
  /**
   * Shows the placemarks of the container at @p containerPath of
   * @p document (which it holds).
   */
  KmlFeatureProvider(std::shared_ptr<const KmlDocument> document,
                     const QString& containerPath);

  /** Returns "kml:" and the path and container. */
  QString id() const override;
  /** Returns the container's name. */
  QString name() const override;
  /** Returns TileKey's deepest level (the features are all in memory). */
  int32_t maximumLevel() const override;

  /** Returns the features meeting @p key's stored tile. */
  std::optional<FeatureTile>
  loadFeatures(TileKey key, const std::atomic<bool>& cancelled) override;

  /** Returns @p key's ancestor at level 6 at most (features are whole). */
  TileKey storedKey(TileKey key) const override;

  /** Returns the features under @p point (see FeatureProvider). */
  std::vector<Feature> featuresAt(GeoPoint point,
                                  double toleranceDeg) const override;

  /** Returns the placemarks under @p point, topmost (last drawn) first. */
  std::vector<const KmlFeature*> placemarksAt(GeoPoint point,
                                              double toleranceDeg) const;

  /** Returns the document. */
  const std::shared_ptr<const KmlDocument>& document() const
  {
    return m_document;
  }

  /** Returns how many features it has. */
  std::size_t featureCount() const { return m_entries.size(); }

private:
  struct Entry {
    Feature feature;
    GeoRect bounds;
    const KmlFeature* placemark = nullptr;
  };

  const std::shared_ptr<const KmlDocument> m_document;
  const QString m_containerPath;
  QString m_name;
  std::vector<Entry> m_entries;
};

} // namespace ts

#endif // TS_KML_KMLFEATUREPROVIDER_H
