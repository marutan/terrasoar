// SPDX-License-Identifier: GPL-2.0-or-later
#include "offlineregionsdialog.h"

#include <cmath>
#include <optional>
#include <vector>

#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "globeview.h"
#include "offlineregionmanager.h"

namespace ts
{

namespace
{

/** Item data: the region's id. */
constexpr int regionIdRole = Qt::UserRole + 1;

enum Column {
  NameColumn,
  LayerColumn,
  LevelsColumn,
  TilesColumn,
  SizeColumn,
  StateColumn,
  ColumnCount
};

} // namespace

OfflineRegionsDialog::OfflineRegionsDialog(OfflineRegionManager& manager,
                                           GlobeView* view, QWidget* parent)
    : QDialog(parent), m_manager(manager), m_view(view),
      m_list(new QTreeWidget(this)), m_emptyLabel(new QLabel(this)),
      m_newButton(new QPushButton(this)), m_pauseButton(new QPushButton(this)),
      m_deleteButton(new QPushButton(this)),
      m_closeButton(new QPushButton(this))
{
  setObjectName(QStringLiteral("offlineRegionsDialog"));
  m_list->setObjectName(QStringLiteral("regionList"));
  m_newButton->setObjectName(QStringLiteral("newRegionButton"));
  m_pauseButton->setObjectName(QStringLiteral("pauseRegionButton"));
  m_deleteButton->setObjectName(QStringLiteral("deleteRegionButton"));
  m_list->setColumnCount(ColumnCount);
  m_list->setRootIsDecorated(false);
  m_list->setUniformRowHeights(true);
  m_list->header()->setStretchLastSection(true);
  m_emptyLabel->setWordWrap(true);

  QHBoxLayout* buttons = new QHBoxLayout;
  buttons->addWidget(m_newButton);
  buttons->addWidget(m_pauseButton);
  buttons->addWidget(m_deleteButton);
  buttons->addStretch();
  buttons->addWidget(m_closeButton);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(m_list);
  layout->addWidget(m_emptyLabel);
  layout->addLayout(buttons);

  connect(m_newButton, &QPushButton::clicked, this,
          &OfflineRegionsDialog::newRegionRequested);
  connect(m_pauseButton, &QPushButton::clicked, this,
          &OfflineRegionsDialog::pauseOrResume);
  connect(m_deleteButton, &QPushButton::clicked, this,
          &OfflineRegionsDialog::deleteRegion);
  connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);
  connect(m_list, &QTreeWidget::currentItemChanged, this, [this]() {
    updateButtons();
    showOutline();
  });
  connect(&m_manager, &OfflineRegionManager::regionsChanged, this,
          &OfflineRegionsDialog::fill);
  connect(&m_manager, &OfflineRegionManager::progressChanged, this,
          &OfflineRegionsDialog::updateProgress);

  retranslateUi();
  fill();
  resize(760, 360);
}

void OfflineRegionsDialog::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QDialog::changeEvent(event);
}

void OfflineRegionsDialog::done(int result)
{
  if (m_view) {
    m_view->setRegionOutlines({});
  }
  QDialog::done(result);
}

void OfflineRegionsDialog::retranslateUi()
{
  setWindowTitle(tr("Offline Regions"));
  m_list->setHeaderLabels({tr("Name"), tr("Layer"), tr("Levels"), tr("Tiles"),
                           tr("Size"), tr("State")});
  m_emptyLabel->setText(
      tr("No regions yet. A region keeps a WMS layer's tiles for an area on "
         "this computer, so the area can be seen without a network."));
  m_newButton->setText(tr("&New Region…"));
  m_deleteButton->setText(tr("&Delete"));
  m_closeButton->setText(tr("Close"));
  updateProgress();
}

long long OfflineRegionsDialog::selectedRegion() const
{
  const QTreeWidgetItem* item = m_list->currentItem();
  return item ? item->data(NameColumn, regionIdRole).toLongLong() : 0;
}

void OfflineRegionsDialog::selectRegion(long long id)
{
  for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
    QTreeWidgetItem* item = m_list->topLevelItem(i);
    if (item->data(NameColumn, regionIdRole).toLongLong() == id) {
      m_list->setCurrentItem(item);
    }
  }
}

void OfflineRegionsDialog::fill()
{
  const long long selected = selectedRegion();
  m_list->clear();
  for (const OfflineRegion& region : m_manager.regions()) {
    QTreeWidgetItem* item = new QTreeWidgetItem(m_list);
    item->setData(NameColumn, regionIdRole, region.id);
    item->setText(NameColumn, region.name);
    item->setText(LayerColumn, region.sourceName);
    item->setText(LevelsColumn, QStringLiteral("%1–%2")
                                    .arg(region.minimumLevel)
                                    .arg(region.maximumLevel));
  }
  m_list->setVisible(!m_manager.regions().empty());
  m_emptyLabel->setVisible(m_manager.regions().empty());
  selectRegion(selected);
  if (!m_list->currentItem() && m_list->topLevelItemCount() > 0) {
    m_list->setCurrentItem(m_list->topLevelItem(0));
  }
  updateProgress();
  showOutline();
}

void OfflineRegionsDialog::updateProgress()
{
  const QLocale locale;
  using State = RegionDownloadProgress::State;
  for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
    QTreeWidgetItem* item = m_list->topLevelItem(i);
    const long long id = item->data(NameColumn, regionIdRole).toLongLong();
    const OfflineRegion* region = m_manager.region(id);
    if (!region) {
      continue;
    }
    const std::optional<RegionDownloadProgress> progress =
        m_manager.progress(id);
    QString state;
    long long bytes = region->bytes;
    if (progress) {
      bytes = progress->bytes;
      const int percent =
          static_cast<int>(std::floor(progress->fraction() * 100.0));
      switch (progress->state) {
      case State::Ready:
      case State::Running:
        state = tr("Downloading: %1 % (%2 of %3 tiles)")
                    .arg(percent)
                    .arg(locale.toString(progress->doneTiles),
                         locale.toString(progress->totalTiles));
        break;
      case State::Paused:
        state = progress->errorString.isEmpty()
                    ? tr("Paused at %1 %").arg(percent)
                    : tr("Paused at %1 %: %2")
                          .arg(percent)
                          .arg(progress->errorString);
        break;
      case State::Finished:
      case State::Cancelled:
      case State::Refused:
        state = tr("Finishing…");
        break;
      }
    } else if (region->complete && region->failedTiles > 0) {
      state = tr("%n tile(s) missing; update to try again", "",
                 static_cast<int>(region->failedTiles));
    } else if (region->complete) {
      state = tr("Ready offline (downloaded %1)")
                  .arg(locale.toString(region->downloaded.toLocalTime().date(),
                                       QLocale::ShortFormat));
    } else {
      state = tr("Not finished; resume to carry on");
    }
    item->setText(TilesColumn, locale.toString(region->tiles));
    item->setText(SizeColumn, locale.formattedDataSize(bytes));
    item->setText(StateColumn, state);
    item->setToolTip(StateColumn, state);
  }
  updateButtons();
}

void OfflineRegionsDialog::updateButtons()
{
  const long long id = selectedRegion();
  const std::optional<RegionDownloadProgress> progress =
      id != 0 ? m_manager.progress(id) : std::nullopt;
  const OfflineRegion* region = id != 0 ? m_manager.region(id) : nullptr;
  if (progress && progress->state == RegionDownloadProgress::State::Running) {
    m_pauseButton->setText(tr("&Pause"));
  } else if (progress || (region && !region->complete)) {
    m_pauseButton->setText(tr("&Resume"));
  } else {
    m_pauseButton->setText(tr("&Update"));
  }
  m_pauseButton->setToolTip(
      tr("Update fetches the tiles that are missing or out of date"));
  m_pauseButton->setEnabled(region != nullptr);
  m_deleteButton->setEnabled(region != nullptr);
}

void OfflineRegionsDialog::showOutline()
{
  if (!m_view) {
    return;
  }
  const OfflineRegion* region = m_manager.region(selectedRegion());
  m_view->setRegionOutlines(region
                                ? std::vector<RegionGeometry>{region->geometry}
                                : std::vector<RegionGeometry>{});
}

void OfflineRegionsDialog::pauseOrResume()
{
  const long long id = selectedRegion();
  const std::optional<RegionDownloadProgress> progress = m_manager.progress(id);
  if (progress && progress->state == RegionDownloadProgress::State::Running) {
    m_manager.pause(id);
    return;
  }
  QString error;
  if (!m_manager.resume(id, &error)) {
    QMessageBox::warning(this, tr("Offline Regions"), error);
  }
  updateProgress();
}

void OfflineRegionsDialog::deleteRegion()
{
  const long long id = selectedRegion();
  const OfflineRegion* region = m_manager.region(id);
  if (!region) {
    return;
  }
  const QMessageBox::StandardButton answer = QMessageBox::question(
      this, tr("Delete Region"),
      tr("Delete the region “%1”? Its tiles stay in the cache for now but "
         "may be removed to make room.")
          .arg(region->name),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (answer == QMessageBox::Yes) {
    m_manager.remove(id);
  }
}

} // namespace ts
