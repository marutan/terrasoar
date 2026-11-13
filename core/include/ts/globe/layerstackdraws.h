// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Turns the layer tree's stack into what the tile and vector renderers
 * draw.
 */
#ifndef TS_GLOBE_LAYERSTACKDRAWS_H
#define TS_GLOBE_LAYERSTACKDRAWS_H

#include <memory>
#include <vector>

#include <QStringList>

#include "ts/globe/tilerenderer.h"
#include "ts/globe/vectorrenderer.h"
#include "ts/layers/layertreemodel.h"

namespace ts
{

class ProviderRegistry;

/** The imagery layers and group opacities for TileRenderer::setLayers(). */
struct ImageryDraws {
  /** Layers, bottom to top. */
  std::vector<ImageryLayerDraw> layers;
  /** Opacity of each composite group. */
  std::vector<double> groupAlphas;
};

/**
 * Returns the imagery of @p stack with providers from @p registry (so
 * entries with the same source share one provider and its tiles). Layers
 * whose source can't be opened are left out and their errors appended to
 * @p errors, if not null.
 *
 * @guithread
 */
ImageryDraws imageryDrawsForStack(const LayerStack& stack,
                                  ProviderRegistry& registry,
                                  QStringList* errors);

/**
 * Returns the vector layers of @p stack, bottom to top, with their
 * styles: graticule sources (`{"kind": "graticule"}`) as graticule draws,
 * the rest with feature providers from @p registry. Layers whose source
 * can't be opened are left out and their errors appended to @p errors.
 *
 * @guithread
 */
std::vector<VectorLayerDraw> vectorDrawsForStack(const LayerStack& stack,
                                                 ProviderRegistry& registry,
                                                 QStringList* errors);

/** The terrain for GlobeRenderer::setElevationProvider() and setTerrain(). */
struct ElevationDraw {
  /** Where the heights come from; null for none. */
  std::shared_ptr<ElevationProvider> provider;
  /** True if the terrain is shown. */
  bool visible = false;
  /** Vertical exaggeration. */
  double exaggeration = 1.0;
};

/**
 * Returns the exaggeration in an elevation layer's style
 * (`{"exaggeration": 2}`), 1 if none, held to 0.1–20.
 */
double terrainExaggeration(const LayerDefinition& definition);

/**
 * Returns the terrain of @p stack: its visible elevation layers as one
 * source, each over those below it (ProviderRegistry::elevationComposite()),
 * with the topmost one's exaggeration; or, if none is visible, the topmost
 * hidden one with visible false (its heights stay loaded, so switching it
 * on is immediate). Visible layers whose source can't be opened add their
 * errors to @p errors and are left out.
 *
 * @guithread
 */
ElevationDraw elevationDrawForStack(const LayerStack& stack,
                                    ProviderRegistry& registry,
                                    QStringList* errors);

/**
 * Returns the attributions of the layers of @p stack that are drawn, each
 * once, in drawing order from the top (vectors, then imagery).
 *
 * @guithread
 */
QStringList attributionsForStack(const LayerStack& stack,
                                 const ProviderRegistry& registry);

} // namespace ts

#endif // TS_GLOBE_LAYERSTACKDRAWS_H
