// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/globe/layerstackdraws.h"

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
  return attributions;
}

} // namespace ts
