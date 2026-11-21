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

// The main window's pack layers (Phase 21): the layers installed data
// packs offer in Add Layer, and the labels of their named points. See
// docs/design/packs.md#pack-layers.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <QJsonArray>
#include <QTimer>

#include "globeview.h"
#include "layertreedock.h"
#include "mainwindow.h"
#include "packmanager.h"
#include "ts/layers/layertreemodel.h"
#include "ts/providers/providerregistry.h"
#include "ts/vector/vectorstyle.h"

namespace ts
{

namespace
{

/** How often the labels follow the view, milliseconds. */
constexpr int32_t packLabelIntervalMs = 400;

/** Most labels handed to the view (it thins them further). */
constexpr std::size_t maximumPackLabels = 3000;

} // namespace

void MainWindow::createPackLayers()
{
  connect(m_packs.get(), &PackManager::packsChanged, this,
          &MainWindow::updatePackLayers);
  m_packLabelTimer = new QTimer(this);
  m_packLabelTimer->setInterval(packLabelIntervalMs);
  connect(m_packLabelTimer, &QTimer::timeout, this,
          &MainWindow::refreshPackLabels);
  updatePackLayers();
}

void MainWindow::updatePackLayers()
{
  // Regional packs of one theme share a name and the same layers: listed
  // once, they show every region installed.
  std::vector<LayerTreeDock::PackLayers> packs;
  for (const InstalledPack& pack : m_packs->installed()) {
    if (pack.damaged || pack.manifest.layers.isEmpty()) {
      continue;
    }
    const QString name =
        pack.manifest.name.isEmpty() ? pack.manifest.id : pack.manifest.name;
    std::vector<LayerTreeDock::PackLayers>::iterator entry =
        std::find_if(packs.begin(), packs.end(),
                     [&name](const LayerTreeDock::PackLayers& listed) {
                       return listed.name == name;
                     });
    if (entry == packs.end()) {
      packs.push_back(LayerTreeDock::PackLayers{name, {}});
      entry = packs.end() - 1;
    }
    // NOLINTNEXTLINE(modernize-loop-convert)
    for (int32_t i = 0; i < pack.manifest.layers.size(); ++i) {
      const std::optional<LayerDefinition> definition =
          LayerDefinition::fromJson(pack.manifest.layers.at(i).toObject(),
                                    nullptr);
      const bool listed =
          definition &&
          std::any_of(entry->layers.begin(), entry->layers.end(),
                      [&definition](const LayerDefinition& layer) {
                        return layer.id == definition->id;
                      });
      if (definition && !listed) {
        entry->layers.push_back(*definition);
      }
    }
  }
  std::sort(packs.begin(), packs.end(),
            [](const LayerTreeDock::PackLayers& a,
               const LayerTreeDock::PackLayers& b) { return a.name < b.name; });
  m_layerDock->setPackLayers(std::move(packs));
}

bool MainWindow::hasLabelLayers(const LayerStack& stack)
{
  return std::any_of(stack.vectors.begin(), stack.vectors.end(),
                     [](const VectorStackEntry& entry) {
                       return !entry.definition.style
                                   .value(QStringLiteral("labels"))
                                   .toString()
                                   .isEmpty();
                     });
}

void MainWindow::refreshPackLabels()
{
  if (m_globeView == nullptr) {
    return;
  }
  const LayerStack stack = m_layers->layerStack();
  std::vector<BillboardDraw> labels;
  for (const VectorStackEntry& entry : stack.vectors) {
    const QString attribute =
        entry.definition.style.value(QStringLiteral("labels")).toString();
    if (attribute.isEmpty() || entry.alpha <= 0.0) {
      continue;
    }
    const std::shared_ptr<FeatureProvider> provider =
        m_registry->features(entry.definition.source, nullptr);
    if (!provider) {
      continue;
    }
    const VectorStyle style = VectorStyle::fromJson(entry.definition.style);
    QColor color = style.hasOutline() ? style.outline : QColor(255, 255, 255);
    color.setAlpha(static_cast<int32_t>(
        std::lround(static_cast<double>(color.alpha()) * entry.alpha)));
    const std::vector<Feature> points = provider->labelPoints(
        attribute, m_globeView->drawnVectorTiles(provider.get()));
    for (const Feature& point : points) {
      if (labels.size() >= maximumPackLabels) {
        break;
      }
      BillboardDraw draw;
      draw.latDeg = point.parts.front().front().latDeg;
      draw.lonDeg = point.parts.front().front().lonDeg;
      draw.sizePx = style.pointSizePx;
      draw.hotSpot = QPointF(0.5, 0.5);
      draw.label = point.attributes.value(attribute).toString();
      draw.labelColor = color;
      draw.labelScale = 0.85;
      labels.push_back(std::move(draw));
    }
  }
  // Only a change reaches the view: the labels of a still view stay put.
  bool same = labels.size() == m_packLabels.size();
  for (std::size_t i = 0; same && i < labels.size(); ++i) {
    same = labels[i].label == m_packLabels[i].label &&
           labels[i].latDeg == m_packLabels[i].latDeg &&
           labels[i].lonDeg == m_packLabels[i].lonDeg &&
           labels[i].labelColor == m_packLabels[i].labelColor;
  }
  if (same) {
    return;
  }
  m_packLabels = std::move(labels);
  refreshBillboards(stack);
}

void MainWindow::updatePackLabelTimer(const LayerStack& stack)
{
  if (m_packLabelTimer == nullptr) {
    return;
  }
  if (hasLabelLayers(stack)) {
    if (!m_packLabelTimer->isActive()) {
      m_packLabelTimer->start();
    }
    return;
  }
  m_packLabelTimer->stop();
  if (!m_packLabels.empty()) {
    m_packLabels.clear();
    refreshBillboards(stack);
  }
}

} // namespace ts
