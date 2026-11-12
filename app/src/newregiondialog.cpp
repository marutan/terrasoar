// SPDX-License-Identifier: GPL-2.0-or-later
#include "newregiondialog.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

#include "globeview.h"
#include "offlineregionmanager.h"
#include "ts/geo/geoformat.h"
#include "ts/regions/countryareas.h"
#include "ts/wms/wmscatalogue.h"
#include "ts/wms/wmsimageryprovider.h"

namespace ts
{

namespace
{

/** Pause after a change before the size is estimated, ms. */
constexpr int estimateDelayMs = 400;

/** How often the estimate is looked at, ms. */
constexpr int pollIntervalMs = 50;

/** Sample tiles for the estimate. */
constexpr int estimateSamples = 24;

/** Ground size of one pixel at the equator at @p level, metres. */
double metresPerPixel(int level, int tileSizePx)
{
  const double equatorM = 40075016.7;
  return equatorM / 360.0 * TileKey::sizeDeg(level) / tileSizePx;
}

} // namespace

NewRegionDialog::NewRegionDialog(OfflineRegionManager& manager,
                                 std::vector<RegionLayerChoice> layers,
                                 GlobeView* view,
                                 std::shared_ptr<const CountryAreas> countries,
                                 QWidget* parent)
    : QDialog(parent), m_manager(manager), m_layers(std::move(layers)),
      m_view(view), m_countries(std::move(countries)),
      m_nameLabel(new QLabel(this)), m_nameEdit(new QLineEdit(this)),
      m_layerLabel(new QLabel(this)), m_layerBox(new QComboBox(this)),
      m_areaTitle(new QLabel(this)), m_areaLabel(new QLabel(this)),
      m_viewButton(new QPushButton(this)),
      m_rectangleButton(new QPushButton(this)),
      m_polygonButton(new QPushButton(this)), m_countryLabel(new QLabel(this)),
      m_countryBox(new QComboBox(this)), m_levelsLabel(new QLabel(this)),
      m_minimumBox(new QSpinBox(this)), m_toLabel(new QLabel(this)),
      m_maximumBox(new QSpinBox(this)), m_detailLabel(new QLabel(this)),
      m_sizeLabel(new QLabel(this)), m_termsLabel(new QLabel(this)),
      m_buttons(new QDialogButtonBox(QDialogButtonBox::Cancel, this)),
      m_estimateDelay(new QTimer(this)), m_estimatePoll(new QTimer(this))
{
  setObjectName(QStringLiteral("newRegionDialog"));
  m_nameEdit->setObjectName(QStringLiteral("regionNameEdit"));
  m_layerBox->setObjectName(QStringLiteral("regionLayerBox"));
  m_areaLabel->setObjectName(QStringLiteral("regionAreaLabel"));
  m_viewButton->setObjectName(QStringLiteral("regionViewButton"));
  m_rectangleButton->setObjectName(QStringLiteral("regionRectangleButton"));
  m_polygonButton->setObjectName(QStringLiteral("regionPolygonButton"));
  m_countryBox->setObjectName(QStringLiteral("regionCountryBox"));
  m_minimumBox->setObjectName(QStringLiteral("regionMinimumLevelBox"));
  m_maximumBox->setObjectName(QStringLiteral("regionMaximumLevelBox"));
  m_detailLabel->setObjectName(QStringLiteral("regionDetailLabel"));
  m_sizeLabel->setObjectName(QStringLiteral("regionSizeLabel"));
  m_termsLabel->setObjectName(QStringLiteral("regionTermsLabel"));
  m_nameLabel->setBuddy(m_nameEdit);
  m_layerLabel->setBuddy(m_layerBox);
  m_countryLabel->setBuddy(m_countryBox);
  // Type part of a name to find a country.
  m_countryBox->setEditable(true);
  m_countryBox->setInsertPolicy(QComboBox::NoInsert);
  m_countryBox->completer()->setFilterMode(Qt::MatchContains);
  m_countryBox->completer()->setCaseSensitivity(Qt::CaseInsensitive);
  m_countryBox->completer()->setCompletionMode(QCompleter::PopupCompletion);
  if (m_countries && m_countries->isValid()) {
    for (const CountryArea& country : m_countries->countries()) {
      m_countryBox->addItem(country.name, country.id);
    }
  } else {
    m_countryLabel->hide();
    m_countryBox->hide();
  }
  m_countryBox->setCurrentIndex(-1);
  m_levelsLabel->setBuddy(m_minimumBox);
  m_areaLabel->setWordWrap(true);
  m_termsLabel->setWordWrap(true);
  m_termsLabel->setOpenExternalLinks(true);
  m_termsLabel->setTextFormat(Qt::RichText);
  m_sizeLabel->setWordWrap(true);
  for (const RegionLayerChoice& layer : m_layers) {
    m_layerBox->addItem(layer.name);
  }
  m_downloadButton =
      m_buttons->addButton(QString(), QDialogButtonBox::AcceptRole);
  m_downloadButton->setObjectName(QStringLiteral("regionDownloadButton"));

  QGridLayout* grid = new QGridLayout;
  grid->addWidget(m_nameLabel, 0, 0);
  grid->addWidget(m_nameEdit, 0, 1);
  grid->addWidget(m_layerLabel, 1, 0);
  grid->addWidget(m_layerBox, 1, 1);
  grid->addWidget(m_areaTitle, 2, 0, Qt::AlignTop);
  QVBoxLayout* areaColumn = new QVBoxLayout;
  areaColumn->addWidget(m_areaLabel);
  QHBoxLayout* areaButtons = new QHBoxLayout;
  areaButtons->addWidget(m_viewButton);
  areaButtons->addWidget(m_rectangleButton);
  areaButtons->addWidget(m_polygonButton);
  areaButtons->addStretch();
  areaColumn->addLayout(areaButtons);
  QHBoxLayout* countryRow = new QHBoxLayout;
  countryRow->addWidget(m_countryLabel);
  countryRow->addWidget(m_countryBox, 1);
  areaColumn->addLayout(countryRow);
  grid->addLayout(areaColumn, 2, 1);
  grid->addWidget(m_levelsLabel, 3, 0);
  QHBoxLayout* levels = new QHBoxLayout;
  levels->addWidget(m_minimumBox);
  levels->addWidget(m_toLabel);
  levels->addWidget(m_maximumBox);
  levels->addStretch();
  grid->addLayout(levels, 3, 1);
  grid->addWidget(m_detailLabel, 4, 1);
  grid->setColumnStretch(1, 1);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addLayout(grid);
  layout->addWidget(m_sizeLabel);
  layout->addWidget(m_termsLabel);
  layout->addStretch();
  layout->addWidget(m_buttons);

  connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_layerBox, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &NewRegionDialog::layerChanged);
  connect(m_nameEdit, &QLineEdit::textChanged, this, &NewRegionDialog::refresh);
  connect(m_minimumBox, QOverload<int>::of(&QSpinBox::valueChanged), this,
          &NewRegionDialog::refresh);
  connect(m_maximumBox, QOverload<int>::of(&QSpinBox::valueChanged), this,
          &NewRegionDialog::refresh);
  connect(m_viewButton, &QPushButton::clicked, this,
          &NewRegionDialog::useCurrentView);
  connect(m_rectangleButton, &QPushButton::clicked, this,
          [this]() { draw(false); });
  connect(m_polygonButton, &QPushButton::clicked, this,
          [this]() { draw(true); });
  connect(m_countryBox, QOverload<int>::of(&QComboBox::activated), this,
          &NewRegionDialog::countryActivated);
  m_estimateDelay->setSingleShot(true);
  m_estimateDelay->setInterval(estimateDelayMs);
  connect(m_estimateDelay, &QTimer::timeout, this,
          &NewRegionDialog::startEstimate);
  m_estimatePoll->setInterval(pollIntervalMs);
  connect(m_estimatePoll, &QTimer::timeout, this,
          &NewRegionDialog::pollEstimate);
  if (m_view) {
    connect(m_view, &GlobeView::regionDrawn, this, &NewRegionDialog::areaDrawn);
    connect(m_view, &GlobeView::regionDrawingCancelled, this,
            [this]() { show(); });
  } else {
    m_viewButton->setEnabled(false);
    m_rectangleButton->setEnabled(false);
    m_polygonButton->setEnabled(false);
  }

  retranslateUi();
  m_automaticName =
      tr("Region %1").arg(static_cast<int>(m_manager.regions().size()) + 1);
  m_nameEdit->setText(m_automaticName);
  layerChanged();
  if (m_view) {
    useCurrentView();
  }
  resize(560, sizeHint().height());
}

NewRegionDialog::~NewRegionDialog()
{
  stopEstimate();
  if (m_view && m_view->regionDrawing() != GlobeView::RegionDrawing::None) {
    m_view->startRegionDrawing(GlobeView::RegionDrawing::None);
  }
}

void NewRegionDialog::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QDialog::changeEvent(event);
}

void NewRegionDialog::done(int result)
{
  stopEstimate();
  if (m_view) {
    if (m_view->regionDrawing() != GlobeView::RegionDrawing::None) {
      m_view->startRegionDrawing(GlobeView::RegionDrawing::None);
    }
    m_view->setRegionOutlines({});
  }
  QDialog::done(result);
}

void NewRegionDialog::retranslateUi()
{
  setWindowTitle(tr("New Offline Region"));
  m_nameLabel->setText(tr("&Name:"));
  m_layerLabel->setText(tr("&Layer:"));
  m_areaTitle->setText(tr("Area:"));
  m_viewButton->setText(tr("&Current View"));
  m_viewButton->setToolTip(tr("The part of the Earth the view shows now"));
  m_rectangleButton->setText(tr("Draw &Rectangle"));
  m_rectangleButton->setToolTip(tr("Drag across the area on the globe"));
  m_polygonButton->setText(tr("Draw &Polygon"));
  m_polygonButton->setToolTip(tr("Click the corners of the area on the "
                                 "globe; double-click to finish"));
  m_countryLabel->setText(tr("Or c&ountry:"));
  m_countryBox->lineEdit()->setPlaceholderText(tr("Type a country's name"));
  m_countryBox->setToolTip(tr("The whole country with its islands, and "
                              "about 5 km around its coasts and borders"));
  m_levelsLabel->setText(tr("L&evels:"));
  m_toLabel->setText(tr("to"));
  m_downloadButton->setText(tr("&Download"));
  refresh();
}

OfflineRegion NewRegionDialog::region() const
{
  OfflineRegion region;
  region.name = m_nameEdit->text().trimmed();
  const int index = m_layerBox->currentIndex();
  if (index >= 0 && index < static_cast<int>(m_layers.size())) {
    region.source = m_layers[static_cast<std::size_t>(index)].source;
    region.sourceName = m_layers[static_cast<std::size_t>(index)].name;
  }
  if (m_area) {
    region.geometry = *m_area;
  }
  region.minimumLevel = m_minimumBox->value();
  region.maximumLevel = m_maximumBox->value();
  region.tiles = m_tiles;
  return region;
}

void NewRegionDialog::setArea(const RegionGeometry& area)
{
  m_area = area;
  m_areaCountry.clear();
  m_countryBox->setCurrentIndex(-1);
  showOutline();
  refresh();
}

bool NewRegionDialog::chooseCountry(const QString& id)
{
  if (!m_countries) {
    return false;
  }
  const std::optional<CountryArea> country = m_countries->country(id);
  if (!country) {
    return false;
  }
  QString error;
  const std::optional<RegionGeometry> area = m_countries->area(id, &error);
  if (!area) {
    return false;
  }
  setArea(*area);
  m_areaCountry = country->name;
  m_countryBox->setCurrentIndex(m_countryBox->findData(id));
  // A name the user hasn't changed follows the country.
  if (m_nameEdit->text() == m_automaticName) {
    m_automaticName = country->name;
    m_nameEdit->setText(m_automaticName);
  }
  refresh();
  return true;
}

void NewRegionDialog::countryActivated(int index)
{
  if (index >= 0) {
    chooseCountry(m_countryBox->itemData(index).toString());
  }
}

void NewRegionDialog::setLevels(int minimumLevel, int maximumLevel)
{
  m_minimumBox->setValue(minimumLevel);
  m_maximumBox->setValue(maximumLevel);
}

void NewRegionDialog::layerChanged()
{
  const int index = m_layerBox->currentIndex();
  m_provider.reset();
  if (index >= 0 && index < static_cast<int>(m_layers.size())) {
    QString error;
    m_provider = m_manager.providerFor(
        m_layers[static_cast<std::size_t>(index)].source, &error);
  }
  const int deepest = m_provider ? m_provider->maximumLevel() : 0;
  m_minimumBox->setRange(0, deepest);
  m_maximumBox->setRange(0, deepest);
  // A sensible start: the whole pyramid down to about 150 m per pixel.
  if (m_maximumBox->value() == 0) {
    m_maximumBox->setValue(std::min(deepest, 8));
  }
  refresh();
}

void NewRegionDialog::useCurrentView()
{
  if (!m_view) {
    return;
  }
  const std::optional<RegionGeometry> visible = m_view->visibleArea();
  if (visible) {
    setArea(*visible);
  }
}

void NewRegionDialog::draw(bool polygon)
{
  if (!m_view) {
    return;
  }
  hide();
  m_view->startRegionDrawing(polygon ? GlobeView::RegionDrawing::Polygon
                                     : GlobeView::RegionDrawing::Rectangle);
}

void NewRegionDialog::areaDrawn(const RegionGeometry& area)
{
  setArea(area);
  show();
  raise();
  activateWindow();
}

void NewRegionDialog::showOutline()
{
  if (m_view) {
    m_view->setRegionOutlines(m_area ? std::vector<RegionGeometry>{*m_area}
                                     : std::vector<RegionGeometry>{});
  }
}

void NewRegionDialog::refresh()
{
  const QLocale locale;
  // The area in words.
  if (!m_area) {
    m_areaLabel->setText(tr("None yet: use the current view, draw one on "
                            "the globe or choose a country."));
  } else if (!m_areaCountry.isEmpty()) {
    m_areaLabel->setText(
        tr("%1 with about 5 km around its coasts and borders (%2 km²)")
            .arg(m_areaCountry, locale.toString(m_area->areaKm2(), 'f', 0)));
  } else {
    const GeoRect& extent = m_area->extent();
    const QString area = locale.toString(m_area->areaKm2(), 'f', 0);
    if (m_area->kind() == RegionGeometry::Kind::Rectangle) {
      m_areaLabel->setText(tr("Rectangle from %1 to %2 and from %3 to %4 "
                              "(%5 km²)")
                               .arg(formatLatitude(extent.southDeg, locale),
                                    formatLatitude(extent.northDeg, locale),
                                    formatLongitude(extent.westDeg, locale),
                                    formatLongitude(extent.eastDeg, locale),
                                    area));
    } else if (m_area->rings().size() > 1) {
      m_areaLabel->setText(tr("Polygon in %n part(s) (%1 km²)", "",
                              static_cast<int>(m_area->rings().size()))
                               .arg(area));
    } else {
      m_areaLabel->setText(tr("Polygon with %n corner(s) (%1 km²)", "",
                              static_cast<int>(m_area->vertices().size()))
                               .arg(area));
    }
  }

  // Levels, tiles and terms.
  if (m_maximumBox->value() < m_minimumBox->value()) {
    m_maximumBox->setValue(m_minimumBox->value());
  }
  const int tileSize = m_provider ? m_provider->tileSizePx() : 256;
  m_detailLabel->setText(
      tr("Down to about %1 per pixel")
          .arg(formatDistance(metresPerPixel(m_maximumBox->value(), tileSize),
                              locale)));
  m_tiles = 0;
  if (m_area && m_provider) {
    // Past the limit the exact number doesn't matter, and counting a
    // country to deep levels would take a while.
    const RegionTileCount counts =
        countRegionTiles(*m_area, m_minimumBox->value(), m_maximumBox->value(),
                         m_provider->dataBounds(), maximumTiles);
    m_tiles = counts.total();
  }
  m_refused = false;
  QString terms;
  if (m_provider) {
    const std::vector<WmsCatalogueEntry> catalogue = m_manager.catalogue();
    const QUrl url = m_provider->source().url;
    const WmsCatalogueEntry* entry = findWmsCatalogueEntry(url, catalogue);
    const QString termsLink =
        entry && entry->termsUrl.isValid()
            ? QStringLiteral(" <a href=\"%1\">%2</a>")
                  .arg(entry->termsUrl.toString(QUrl::FullyEncoded)
                           .toHtmlEscaped(),
                       tr("Terms of use"))
            : QString();
    switch (bulkDownloadPolicy(url, catalogue)) {
    case BulkDownloadPolicy::Forbidden:
      m_refused = true;
      terms = tr("<b>The terms of use of %1 don't allow downloading regions "
                 "for offline use.</b>")
                  .arg(url.host().toHtmlEscaped()) +
              termsLink;
      break;
    case BulkDownloadPolicy::Unknown:
      terms = tr("Check that the terms of use of %1 allow downloading many "
                 "tiles at once before you go on.")
                  .arg(url.host().toHtmlEscaped()) +
              termsLink;
      break;
    case BulkDownloadPolicy::Allowed:
      break;
    }
  } else {
    terms = tr("<b>This layer can't be kept for offline use.</b>");
  }
  m_termsLabel->setText(terms);
  m_termsLabel->setVisible(!terms.isEmpty());

  // The size: the count at once, then an estimate from samples.
  m_estimate.reset();
  stopEstimate();
  if (!m_area) {
    m_sizeLabel->clear();
  } else if (m_tiles > maximumTiles) {
    m_sizeLabel->setText(
        tr("<b>More than %1 tiles: too many.</b> Choose fewer levels or a "
           "smaller area.")
            .arg(locale.toString(maximumTiles)));
  } else if (m_provider && !m_refused) {
    m_sizeLabel->setText(
        tr("%1 tiles; estimating the size…").arg(locale.toString(m_tiles)));
    m_estimating = true;
    m_estimateDelay->start();
  } else {
    m_sizeLabel->setText(tr("%1 tiles").arg(locale.toString(m_tiles)));
  }
  m_downloadButton->setEnabled(canDownload());
}

bool NewRegionDialog::canDownload() const
{
  return m_provider && m_area && !m_refused && m_tiles > 0 &&
         m_tiles <= maximumTiles && !m_nameEdit->text().trimmed().isEmpty();
}

void NewRegionDialog::startEstimate()
{
  stopEstimate();
  if (!m_provider || !m_area) {
    return;
  }
  m_estimating = true;
  m_estimateCancel = std::make_shared<std::atomic<bool>>(false);
  m_estimateThread = std::thread([this, provider = m_provider, area = *m_area,
                                  minimum = m_minimumBox->value(),
                                  maximum = m_maximumBox->value(),
                                  cancel = m_estimateCancel]() {
    RegionEstimate estimate = estimateRegionSize(
        *provider, area, minimum, maximum, estimateSamples, *cancel);
    if (!cancel->load()) {
      const std::lock_guard<std::mutex> lock(m_estimateMutex);
      m_estimateResult = estimate;
    }
  });
  m_estimatePoll->start();
}

void NewRegionDialog::stopEstimate()
{
  m_estimateDelay->stop();
  m_estimatePoll->stop();
  if (m_estimateCancel) {
    m_estimateCancel->store(true);
  }
  if (m_estimateThread.joinable()) {
    m_estimateThread.join();
  }
  m_estimateCancel.reset();
  const std::lock_guard<std::mutex> lock(m_estimateMutex);
  m_estimateResult.reset();
  m_estimating = false;
}

void NewRegionDialog::pollEstimate()
{
  std::optional<RegionEstimate> result;
  {
    const std::lock_guard<std::mutex> lock(m_estimateMutex);
    result.swap(m_estimateResult);
  }
  if (!result) {
    return;
  }
  m_estimatePoll->stop();
  m_estimateThread.join();
  m_estimateCancel.reset();
  m_estimating = false;
  m_estimate = result;
  const QLocale locale;
  const QString size = locale.formattedDataSize(result->bytes);
  m_sizeLabel->setText(
      result->sampledTiles > 0
          ? tr("%1 tiles, about %2.").arg(locale.toString(result->tiles), size)
          : tr("%1 tiles, roughly %2 (the server couldn't be asked for "
               "samples).")
                .arg(locale.toString(result->tiles), size));
}

} // namespace ts
