// SPDX-License-Identifier: GPL-2.0-or-later
#include "packmanagerdialog.h"

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

#include "packmanager.h"

namespace ts
{

namespace
{

/** Item data: the pack's id. */
constexpr int packIdRole = Qt::UserRole + 1;

enum Column { NameColumn, VersionColumn, SizeColumn, StateColumn, ColumnCount };

qint64 installedBytes(const InstalledPack& pack)
{
  qint64 bytes = 0;
  for (const PackFile& file : pack.manifest.files) {
    bytes += file.sizeBytes;
  }
  return bytes;
}

} // namespace

PackManagerDialog::PackManagerDialog(PackManager& manager, QWidget* parent)
    : QDialog(parent), m_manager(manager), m_list(new QTreeWidget(this)),
      m_detailsLabel(new QLabel(this)), m_indexLabel(new QLabel(this)),
      m_actionButton(new QPushButton(this)),
      m_removeButton(new QPushButton(this)),
      m_refreshButton(new QPushButton(this)),
      m_closeButton(new QPushButton(this))
{
  setObjectName(QStringLiteral("packManagerDialog"));
  m_list->setObjectName(QStringLiteral("packList"));
  m_actionButton->setObjectName(QStringLiteral("packActionButton"));
  m_removeButton->setObjectName(QStringLiteral("packRemoveButton"));
  m_refreshButton->setObjectName(QStringLiteral("packRefreshButton"));
  m_indexLabel->setObjectName(QStringLiteral("packIndexLabel"));
  m_detailsLabel->setObjectName(QStringLiteral("packDetailsLabel"));
  m_list->setColumnCount(ColumnCount);
  m_list->setRootIsDecorated(false);
  m_list->setUniformRowHeights(true);
  m_list->header()->setStretchLastSection(true);
  m_detailsLabel->setWordWrap(true);
  m_detailsLabel->setTextFormat(Qt::PlainText);
  m_indexLabel->setWordWrap(true);

  QHBoxLayout* buttons = new QHBoxLayout;
  buttons->addWidget(m_actionButton);
  buttons->addWidget(m_removeButton);
  buttons->addWidget(m_refreshButton);
  buttons->addStretch();
  buttons->addWidget(m_closeButton);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(m_list);
  layout->addWidget(m_detailsLabel);
  layout->addWidget(m_indexLabel);
  layout->addLayout(buttons);

  connect(m_actionButton, &QPushButton::clicked, this, &PackManagerDialog::act);
  connect(m_removeButton, &QPushButton::clicked, this,
          &PackManagerDialog::removeOrCancel);
  connect(m_refreshButton, &QPushButton::clicked, &m_manager,
          &PackManager::refreshIndex);
  connect(m_closeButton, &QPushButton::clicked, this, &QDialog::accept);
  connect(m_list, &QTreeWidget::currentItemChanged, this,
          &PackManagerDialog::updateButtons);
  connect(&m_manager, &PackManager::packsChanged, this,
          &PackManagerDialog::fill);
  connect(&m_manager, &PackManager::progressChanged, this,
          &PackManagerDialog::updateRows);

  retranslateUi();
  fill();
  resize(720, 340);
  if (m_manager.indexState() != PackManager::IndexState::Loaded) {
    m_manager.refreshIndex();
  }
}

PackManagerDialog::~PackManagerDialog() = default;

void PackManagerDialog::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QDialog::changeEvent(event);
}

void PackManagerDialog::retranslateUi()
{
  setWindowTitle(tr("Data Packs"));
  m_list->setHeaderLabels({tr("Name"), tr("Version"), tr("Size"), tr("State")});
  m_refreshButton->setText(tr("Re&fresh List"));
  m_refreshButton->setToolTip(tr("Read the list of packs again"));
  m_closeButton->setText(tr("Close"));
  fill();
}

QString PackManagerDialog::selectedPack() const
{
  const QTreeWidgetItem* item = m_list->currentItem();
  return item ? item->data(NameColumn, packIdRole).toString() : QString();
}

void PackManagerDialog::selectPack(const QString& id)
{
  for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
    QTreeWidgetItem* item = m_list->topLevelItem(i);
    if (item->data(NameColumn, packIdRole).toString() == id) {
      m_list->setCurrentItem(item);
    }
  }
}

PackManagerDialog::Row PackManagerDialog::row(const QString& id) const
{
  const InstalledPack* installed = m_manager.installedPack(id);
  const std::optional<PackIndex>& index = m_manager.index();
  const std::optional<PackIndexEntry> entry =
      index ? index->pack(id) : std::nullopt;
  const std::optional<PendingPack> pending = m_manager.pending(id);

  Row row;
  row.name = id;
  if (entry) {
    row.name = entry->name;
    row.version = entry->version;
    row.sizeBytes = entry->sizeBytes;
    row.details =
        entry->licence.isEmpty()
            ? entry->description
            : tr("%1 Licence: %2.").arg(entry->description, entry->licence);
  } else if (pending) {
    row.name = pending->entry.name;
    row.version = pending->entry.version;
    row.sizeBytes = pending->entry.sizeBytes;
  }
  if (installed) {
    if (!installed->manifest.datasets.empty() && !entry) {
      row.name = installed->manifest.datasets.front().name;
    }
    row.version = installed->manifest.version;
    row.sizeBytes = installedBytes(*installed);
    row.removal = Removal::Remove;
  }
  if (describeDownload(&row, m_manager.progress(id), installed != nullptr)) {
    return row;
  }

  if (pending && pending->ready) {
    row.state = tr("Will be installed when Terra Soar starts again");
  } else if (pending) {
    row.state = tr("Download not finished; resume to carry on");
    row.action = Action::Resume;
    row.removal = Removal::CancelDownload;
  } else if (installed && installed->damaged) {
    row.state = tr("Damaged; not used");
    row.action = entry ? Action::Reinstall : Action::None;
  } else if (installed && entry &&
             entry->version != installed->manifest.version) {
    row.state = tr("Installed; version %1 available").arg(entry->version);
    row.action = Action::Update;
  } else if (installed) {
    row.state = tr("Installed");
  } else {
    row.state = tr("Not installed");
    row.action = entry ? Action::Install : Action::None;
  }
  return row;
}

bool PackManagerDialog::describeDownload(
    Row* row, const std::optional<PackInstallProgress>& progress,
    bool installed)
{
  if (!progress) {
    return false;
  }
  using State = PackInstallProgress::State;
  const int percent =
      static_cast<int>(std::floor(progress->fraction() * 100.0));
  switch (progress->state) {
  case State::Running:
  case State::Ready:
  case State::Verified:
    row->state = progress->totalBytes > 0 ? tr("Downloading: %1 %").arg(percent)
                                          : tr("Starting the download…");
    row->action = Action::Pause;
    row->removal = Removal::CancelDownload;
    return true;
  case State::Paused:
    row->state =
        progress->errorString.isEmpty()
            ? tr("Paused at %1 %").arg(percent)
            : tr("Paused at %1 %: %2").arg(percent).arg(progress->errorString);
    row->action = Action::Resume;
    row->removal = Removal::CancelDownload;
    return true;
  case State::Failed:
    row->state = tr("Failed: %1").arg(progress->errorString);
    row->action = Action::Retry;
    row->removal = installed ? Removal::Remove : Removal::CancelDownload;
    return true;
  case State::Cancelled:
    break;
  }
  return false;
}

void PackManagerDialog::fill()
{
  const QString selected = selectedPack();
  m_list->clear();
  for (const QString& id : m_manager.packIds()) {
    QTreeWidgetItem* item = new QTreeWidgetItem(m_list);
    item->setData(NameColumn, packIdRole, id);
  }
  selectPack(selected);
  if (!m_list->currentItem() && m_list->topLevelItemCount() > 0) {
    m_list->setCurrentItem(m_list->topLevelItem(0));
  }
  switch (m_manager.indexState()) {
  case PackManager::IndexState::NotLoaded:
  case PackManager::IndexState::Loaded:
    m_indexLabel->setText(m_list->topLevelItemCount() == 0
                              ? tr("No data packs are available.")
                              : QString());
    break;
  case PackManager::IndexState::Loading:
    m_indexLabel->setText(tr("Reading the list of packs…"));
    break;
  case PackManager::IndexState::Failed:
    m_indexLabel->setText(m_manager.indexError());
    break;
  }
  m_indexLabel->setVisible(!m_indexLabel->text().isEmpty());
  m_refreshButton->setEnabled(m_manager.indexState() !=
                              PackManager::IndexState::Loading);
  updateRows();
}

void PackManagerDialog::updateRows()
{
  const QLocale locale;
  for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
    QTreeWidgetItem* item = m_list->topLevelItem(i);
    const Row shown = row(item->data(NameColumn, packIdRole).toString());
    item->setText(NameColumn, shown.name);
    item->setText(VersionColumn, shown.version);
    item->setText(SizeColumn, shown.sizeBytes > 0
                                  ? locale.formattedDataSize(shown.sizeBytes)
                                  : QString());
    item->setText(StateColumn, shown.state);
    item->setToolTip(StateColumn, shown.state);
  }
  updateButtons();
}

void PackManagerDialog::updateButtons()
{
  const QString id = selectedPack();
  const Row shown = id.isEmpty() ? Row() : row(id);
  m_detailsLabel->setText(shown.details);
  m_detailsLabel->setVisible(!shown.details.isEmpty());
  switch (shown.action) {
  case Action::None:
  case Action::Install:
    m_actionButton->setText(tr("&Install"));
    break;
  case Action::Update:
    m_actionButton->setText(tr("&Update"));
    break;
  case Action::Reinstall:
    m_actionButton->setText(tr("Re&install"));
    break;
  case Action::Resume:
    m_actionButton->setText(tr("&Resume"));
    break;
  case Action::Retry:
    m_actionButton->setText(tr("&Try Again"));
    break;
  case Action::Pause:
    m_actionButton->setText(tr("&Pause"));
    break;
  }
  m_actionButton->setEnabled(shown.action != Action::None);
  m_removeButton->setText(shown.removal == Removal::CancelDownload
                              ? tr("&Cancel Download")
                              : tr("Re&move"));
  m_removeButton->setEnabled(shown.removal != Removal::None);
}

void PackManagerDialog::act()
{
  const QString id = selectedPack();
  if (id.isEmpty()) {
    return;
  }
  if (row(id).action == Action::Pause) {
    m_manager.pause(id);
    return;
  }
  QString error;
  if (!m_manager.install(id, &error)) {
    QMessageBox::warning(this, tr("Data Packs"), error);
  }
}

void PackManagerDialog::removeOrCancel()
{
  const QString id = selectedPack();
  const Row shown = id.isEmpty() ? Row() : row(id);
  if (shown.removal == Removal::CancelDownload) {
    m_manager.cancel(id);
    return;
  }
  if (shown.removal != Removal::Remove) {
    return;
  }
  const QMessageBox::StandardButton answer = QMessageBox::question(
      this, tr("Remove Pack"),
      tr("Remove “%1” from this computer? Where it replaced data of the "
         "base pack, the base pack's is used again.")
          .arg(shown.name),
      QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
  if (answer == QMessageBox::Yes) {
    m_manager.remove(id);
  }
}

} // namespace ts
