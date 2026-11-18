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
 * The dialog that sets up a new offline region.
 */
#ifndef TS_APP_NEWREGIONDIALOG_H
#define TS_APP_NEWREGIONDIALOG_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include <QDialog>
#include <QJsonObject>
#include <QPointer>
#include <QString>

#include "ts/regions/offlineregionstore.h"
#include "ts/regions/regiondownload.h"

class QComboBox;
class QDialogButtonBox;
class QEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;

namespace ts
{

class CountryAreas;
class GlobeView;
class OfflineRegionManager;
class WmsImageryProvider;

/** A layer that can be kept for offline use: its name and source. */
struct RegionLayerChoice {
  /** The layer's name. */
  QString name;
  /** Its source (`{"kind": "wms", …}`). */
  QJsonObject source;
};

/**
 * Sets up an offline region: its name, the layer whose tiles to keep, the
 * area (the current view, or drawn on the globe as a rectangle or a
 * polygon) and the levels. Shows how many tiles that is and, from a few
 * sample tiles fetched in the background, about how much space; warns
 * when the server's terms of use are unknown and refuses when they forbid
 * bulk downloads (bulkDownloadPolicy()), or when the region is too big.
 *
 * While the user draws on the globe the dialog hides; it shows the area's
 * outline on the globe while open.
 *
 * @guithread
 */
class NewRegionDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(NewRegionDialog)

public:
  /**
   * The user guide's section on this window
   * (`offline-use.md#downloading-a-region`), opened by F1.
   */
  static QString helpTarget();

  /** Most tiles in one region. */
  static constexpr int64_t maximumTiles = 1000000;

  /**
   * Sets up a region of @p manager for one of @p layers.
   * @param manager The regions the new one joins.
   * @param layers The layers to choose from (at least one).
   * @param view Where the area is drawn and shown; may be null.
   * @param countries Country outlines to choose an area from; may be null
   *        (no choice of country).
   * @param parent Usual QWidget parent.
   */
  NewRegionDialog(OfflineRegionManager& manager,
                  std::vector<RegionLayerChoice> layers, GlobeView* view,
                  std::shared_ptr<const CountryAreas> countries,
                  QWidget* parent = nullptr);

  /** Stops the size estimate. */
  ~NewRegionDialog() override;

  /** Returns the region as set up (no id yet). */
  OfflineRegion region() const;

  /** Sets the area. */
  void setArea(const RegionGeometry& area);

  /**
   * Sets the area to the country with id @p id (CountryArea::id). Returns
   * false if there is no such country or its outline can't be read.
   */
  bool chooseCountry(const QString& id);

  /** Returns the name of the country the area is, or an empty string. */
  const QString& areaCountry() const { return m_areaCountry; }

  /** Returns the area, if one is set. */
  const std::optional<RegionGeometry>& area() const { return m_area; }

  /** Sets the levels. */
  void setLevels(int32_t minimumLevel, int32_t maximumLevel);

  /** Returns the tile count shown. */
  int64_t tileCount() const { return m_tiles; }

  /** Returns true while the size is being estimated. */
  bool isEstimating() const { return m_estimating; }

  /** Returns the size estimate, once there is one. */
  const std::optional<RegionEstimate>& estimate() const { return m_estimate; }

  /** Returns true if the Download button can be pressed. */
  bool canDownload() const;

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

  /** Hides the outline on the globe. */
  void done(int32_t result) override;

private:
  void retranslateUi();
  void layerChanged();
  void useCurrentView();
  void draw(bool polygon);
  void areaDrawn(const RegionGeometry& area);
  void refresh();
  void startEstimate();
  void stopEstimate();
  void pollEstimate();
  void showOutline();
  void countryActivated(int32_t index);

  OfflineRegionManager& m_manager;
  const std::vector<RegionLayerChoice> m_layers;
  QPointer<GlobeView> m_view;
  std::shared_ptr<WmsImageryProvider> m_provider;
  std::optional<RegionGeometry> m_area;
  std::shared_ptr<const CountryAreas> m_countries;
  /** The country the area is, if chosen by country. */
  QString m_areaCountry;
  /** The name the dialog gave the region, until the user changes it. */
  QString m_automaticName;
  int64_t m_tiles = 0;
  bool m_refused = false;

  QLabel* m_nameLabel = nullptr;
  QLineEdit* m_nameEdit = nullptr;
  QLabel* m_layerLabel = nullptr;
  QComboBox* m_layerBox = nullptr;
  QLabel* m_areaTitle = nullptr;
  QLabel* m_areaLabel = nullptr;
  QPushButton* m_viewButton = nullptr;
  QPushButton* m_rectangleButton = nullptr;
  QPushButton* m_polygonButton = nullptr;
  QLabel* m_countryLabel = nullptr;
  QComboBox* m_countryBox = nullptr;
  QLabel* m_levelsLabel = nullptr;
  QSpinBox* m_minimumBox = nullptr;
  QLabel* m_toLabel = nullptr;
  QSpinBox* m_maximumBox = nullptr;
  QLabel* m_detailLabel = nullptr;
  QLabel* m_sizeLabel = nullptr;
  QLabel* m_termsLabel = nullptr;
  QDialogButtonBox* m_buttons = nullptr;
  QPushButton* m_downloadButton = nullptr;

  /** Waits a moment after a change before estimating. */
  QTimer* m_estimateDelay = nullptr;
  QTimer* m_estimatePoll = nullptr;
  std::thread m_estimateThread;
  std::shared_ptr<std::atomic<bool>> m_estimateCancel;
  std::mutex m_estimateMutex;
  std::optional<RegionEstimate> m_estimateResult;
  std::optional<RegionEstimate> m_estimate;
  bool m_estimating = false;
};

} // namespace ts

#endif // TS_APP_NEWREGIONDIALOG_H
