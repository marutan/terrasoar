// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The dialog that sets up a new offline region.
 */
#ifndef TS_APP_NEWREGIONDIALOG_H
#define TS_APP_NEWREGIONDIALOG_H

#include <atomic>
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
  /** Most tiles in one region. */
  static constexpr long long maximumTiles = 1000000;

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
  void setLevels(int minimumLevel, int maximumLevel);

  /** Returns the tile count shown. */
  long long tileCount() const { return m_tiles; }

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
  void done(int result) override;

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
  void countryActivated(int index);

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
  long long m_tiles = 0;
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
