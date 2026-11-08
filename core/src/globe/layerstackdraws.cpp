// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/globe/layerstackdraws.h"

#include "ts/providers/providerregistry.h"
#include "ts/providers/tileprovider.h"

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

} // namespace ts
