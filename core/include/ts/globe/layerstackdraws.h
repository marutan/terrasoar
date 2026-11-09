// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Turns the layer tree's stack into what the tile and vector renderers
 * draw.
 */
#ifndef TS_GLOBE_LAYERSTACKDRAWS_H
#define TS_GLOBE_LAYERSTACKDRAWS_H

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
