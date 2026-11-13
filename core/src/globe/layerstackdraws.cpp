// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/globe/layerstackdraws.h"

#include <algorithm>
#include <cmath>
#include <functional>

#include "ts/providers/providerregistry.h"
#include "ts/providers/tileprovider.h"
#include "ts/vector/vectorstyle.h"

namespace ts
{

ImageryDraws imageryDrawsForStack(const LayerStack& stack,
                                  ProviderRegistry& registry,
                                  QStringList* errors)
{
  ImageryDraws draws;
  draws.groupAlphas.reserve(stack.groups.size());
  for (const StackGroup& group : stack.groups) {
    draws.groupAlphas.push_back(group.alpha);
  }
  draws.layers.reserve(stack.imagery.size());
  for (const ImageryStackEntry& entry : stack.imagery) {
    QString error;
    std::shared_ptr<ImageryProvider> provider =
        registry.imagery(entry.definition.source, &error);
    if (!provider) {
      if (errors) {
        errors->append(error);
      }
      continue;
    }
    draws.layers.push_back(
        ImageryLayerDraw{std::move(provider), entry.alpha, entry.group});
  }
  return draws;
}

std::vector<VectorLayerDraw> vectorDrawsForStack(const LayerStack& stack,
                                                 ProviderRegistry& registry,
                                                 QStringList* errors)
{
  std::vector<VectorLayerDraw> draws;
  // The stack lists vectors from the bottom, like imagery.
  for (const VectorStackEntry& entry : stack.vectors) {
    VectorLayerDraw draw;
    draw.style = VectorStyle::fromJson(entry.definition.style);
    draw.alpha = entry.alpha;
    if (entry.definition.source.value(QStringLiteral("kind")).toString() ==
        QLatin1String("graticule")) {
      draw.kind = VectorLayerDraw::Kind::Graticule;
    } else {
      QString error;
      draw.provider = registry.features(entry.definition.source, &error);
      if (!draw.provider) {
        if (errors) {
          errors->append(error);
        }
        continue;
      }
    }
    draws.push_back(std::move(draw));
  }
  return draws;
}

QStringList attributionsForStack(const LayerStack& stack,
                                 const ProviderRegistry& registry)
{
  QStringList attributions;
  const std::function<void(const QJsonObject&)> add =
      [&](const QJsonObject& source) {
        const QString text = registry.attribution(source);
        if (!text.isEmpty() && !attributions.contains(text)) {
          attributions.append(text);
        }
      };
  for (std::size_t i = stack.vectors.size(); i-- > 0;) {
    add(stack.vectors[i].definition.source);
  }
  for (std::size_t i = stack.imagery.size(); i-- > 0;) {
    add(stack.imagery[i].definition.source);
  }
  for (std::size_t i = stack.elevation.size(); i-- > 0;) {
    if (stack.elevation[i].visible) {
      add(stack.elevation[i].definition.source);
      break;
    }
  }
  return attributions;
}

double terrainExaggeration(const LayerDefinition& definition)
{
  const double value =
      definition.style.value(QStringLiteral("exaggeration")).toDouble(1.0);
  return std::isfinite(value) ? std::clamp(value, 0.1, 20.0) : 1.0;
}

ElevationDraw elevationDrawForStack(const LayerStack& stack,
                                    ProviderRegistry& registry,
                                    QStringList* errors)
{
  ElevationDraw draw;
  // The visible layers, topmost first: each has priority over those below
  // it. If none is visible, the topmost of all (its heights stay loaded
  // for when it is switched on).
  std::vector<const ElevationStackEntry*> chosen;
  for (std::size_t i = stack.elevation.size(); i-- > 0;) {
    if (stack.elevation[i].visible) {
      chosen.push_back(&stack.elevation[i]);
    }
  }
  const bool visible = !chosen.empty();
  if (!visible && !stack.elevation.empty()) {
    chosen.push_back(&stack.elevation.back());
  }
  std::vector<std::shared_ptr<ElevationProvider>> sources;
  for (const ElevationStackEntry* entry : chosen) {
    QString error;
    std::shared_ptr<ElevationProvider> provider =
        registry.elevation(entry->definition.source, &error);
    if (provider) {
      sources.push_back(std::move(provider));
    } else if (errors != nullptr && entry->visible) {
      errors->append(error);
    }
  }
  if (sources.empty()) {
    return draw;
  }
  draw.provider = registry.elevationComposite(std::move(sources));
  draw.visible = visible;
  draw.exaggeration = terrainExaggeration(chosen.front()->definition);
  return draw;
}

} // namespace ts
