// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * What a layer shows: its type and its data source, as JSON.
 */
#ifndef TS_LAYERS_LAYERDEFINITION_H
#define TS_LAYERS_LAYERDEFINITION_H

#include <optional>
#include <vector>

#include <QJsonObject>
#include <QString>

namespace ts
{

/** The kind of data a layer draws. */
enum class LayerType {
  /** Imagery tiles, blended in the tile shader. */
  Imagery,
  /** Vector features, drawn above all imagery (from Phase 3). */
  Vector,
  /** Heights (from Phase 6). */
  Elevation,
};

/** Where a layer definition comes from. */
enum class LayerOrigin {
  /** Shipped with the application. */
  Builtin,
  /** From a server's or pack's catalogue (from Phase 4). */
  Catalogue,
  /** Added by the user. */
  User,
};

/**
 * A layer's data: what it is, where it comes from and how to open it.
 *
 * Stored in project files and in layer catalogues as JSON:
 *
 * ```json
 * { "id": "builtin:checkerboard", "origin": "builtin", "type": "imagery",
 *   "name": "Checkerboard",
 *   "source": { "kind": "checkerboard", "tileSize": 256 } }
 * ```
 *
 * Vector layers also have a "style" object (see ts::VectorStyle):
 * `{"fill": "#rrggbbaa", "outline": "#rrggbbaa", "outlineWidthPx": 1.5}`.
 *
 * The source object is interpreted by the provider factory
 * (createImageryProvider()); two definitions with equal source objects
 * share one provider. See docs/design/layer-model.md.
 *
 * @reentrant
 */
struct LayerDefinition {
  /** Identifier, unique within its origin ("builtin:checkerboard"). */
  QString id;
  /** Where it comes from. */
  LayerOrigin origin = LayerOrigin::User;
  /** What it draws. */
  LayerType type = LayerType::Imagery;
  /** Default name for the user. */
  QString name;
  /** Source parameters; "kind" selects the provider. */
  QJsonObject source;
  /** Drawing style of a vector layer; empty for other layers. */
  QJsonObject style;

  /** Returns the JSON form. */
  QJsonObject toJson() const;

  /**
   * Reads the JSON form. Returns nothing and sets @p errorString (if not
   * null) when a required member is missing or has an unknown value.
   */
  static std::optional<LayerDefinition> fromJson(const QJsonObject& json,
                                                 QString* errorString);

  /** Equality of every member. */
  bool operator==(const LayerDefinition& other) const;
  /** Inequality. */
  bool operator!=(const LayerDefinition& other) const
  {
    return !(*this == other);
  }
};

/** Returns "imagery", "vector" or "elevation". */
QString layerTypeName(LayerType type);

/** Returns "builtin", "catalogue" or "user". */
QString layerOriginName(LayerOrigin origin);

/**
 * Returns the layer definitions shipped with the application.
 *
 * @threadsafe
 */
std::vector<LayerDefinition> builtinLayerDefinitions();

} // namespace ts

#endif // TS_LAYERS_LAYERDEFINITION_H
