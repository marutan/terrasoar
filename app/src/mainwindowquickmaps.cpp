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
// The main window's Quick Maps: the toolbar menu of catalogue layers that
// are added at once, with the choices the Add dialogs would make, instead
// of opening a dialog. The rest of the window is in mainwindow.cpp.

#include <cstddef>
#include <vector>

#include <QAction>
#include <QMenu>
#include <QStatusBar>

#include "addxyzdialog.h"
#include "layertreedock.h"
#include "mainwindow.h"
#include "ts/xyz/xyzsource.h"
#include "wmscataloguefile.h"

namespace ts
{

namespace
{

/** The catalogue a Quick Maps entry comes from. */
enum class QuickMapCatalogue { Xyz };

/** An entry of the Quick Maps menu. */
struct QuickMap {
  /** Its action's object name (for tests). */
  const char* objectName;
  /** The catalogue it is in. */
  QuickMapCatalogue catalogue;
  /** Its service's title in that catalogue, also the menu's text. */
  const char* service;
};

/** The Quick Maps menu, in order. */
const std::vector<QuickMap>& quickMaps()
{
  static const std::vector<QuickMap> maps{
      {"quickMapOpenStreetMapAction", QuickMapCatalogue::Xyz, "OpenStreetMap"},
      {"quickMapOpenTopoMapAction", QuickMapCatalogue::Xyz, "OpenTopoMap"},
      {"quickMapGoogleMapsAction", QuickMapCatalogue::Xyz, "Google Maps"},
      {"quickMapGoogleSatelliteAction", QuickMapCatalogue::Xyz,
       "Google Maps - Satellite"},
      {"quickMapGoogleHybridAction", QuickMapCatalogue::Xyz,
       "Google Maps - Hybrid"},
      {"quickMapEsriImageryAction", QuickMapCatalogue::Xyz,
       "ESRI - World Imagery"},
      {"quickMapEsriStreetMapAction", QuickMapCatalogue::Xyz,
       "ESRI - World Street Map"},
      {"quickMapEsriTopoMapAction", QuickMapCatalogue::Xyz,
       "ESRI - World Topo Map"},
  };
  return maps;
}

} // namespace

void MainWindow::createQuickMaps()
{
  const std::vector<QuickMap>& maps = quickMaps();
  for (std::size_t i = 0; i < maps.size(); ++i) {
    // A service's name, as in the catalogue: not translated.
    QAction* action =
        m_quickMapsMenu->addAction(QString::fromUtf8(maps[i].service));
    action->setObjectName(QString::fromLatin1(maps[i].objectName));
    // Said, so that the pseudo-locale tests know it isn't to be
    // translated.
    action->setProperty("ts_untranslated", true);
    connect(action, &QAction::triggered, this, [this, i]() { addQuickMap(i); });
  }
}

void MainWindow::addQuickMap(std::size_t index)
{
  const QuickMap& map = quickMaps().at(index);
  const QString service = QString::fromUtf8(map.service);
  switch (map.catalogue) {
  case QuickMapCatalogue::Xyz:
    for (const XyzCatalogueEntry& entry : currentXyzCatalogue()) {
      if (entry.title != service) {
        continue;
      }
      XyzSource xyz = entry.source();
      // As the dialog: subdomains only for a template that uses them.
      if (!xyz.urlTemplate.contains(QLatin1String("{s}"),
                                    Qt::CaseInsensitive)) {
        xyz.subdomains.clear();
      }
      m_layerDock->addLayer(xyzLayerDefinition(xyz));
      return;
    }
    statusBar()->showMessage(
        tr("%1 isn't in the XYZ catalogue any more.").arg(service), 10000);
    return;
  }
}

} // namespace ts
