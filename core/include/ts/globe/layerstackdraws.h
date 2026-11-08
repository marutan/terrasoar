// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Turns the layer tree's stack into what the tile renderer draws.
 */
#ifndef TS_GLOBE_LAYERSTACKDRAWS_H
#define TS_GLOBE_LAYERSTACKDRAWS_H

#include <vector>

#include <QStringList>

#include "ts/globe/tilerenderer.h"
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

} // namespace ts

#endif // TS_GLOBE_LAYERSTACKDRAWS_H
