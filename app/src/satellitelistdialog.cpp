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
#include "satellitelistdialog.h"

#include <cmath>
#include <memory>
#include <utility>

#include <QDialogButtonBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QStandardItemModel>
#include <QStyle>
#include <QTableView>
#include <QVBoxLayout>

#include "satellitelayers.h"
#include "satellitepropertiesdialog.h"
#include "ts/layers/layertreemodel.h"
#include "ts/orbits/satellitelayer.h"
#include "ts/time/programclock.h"

namespace ts
{

namespace
{

constexpr int32_t numberRole = Qt::UserRole + 1;

enum Column : int32_t { NameColumn, NumberColumn, AgeColumn, SpansColumn };

} // namespace

SatelliteListDialog::SatelliteListDialog(QString nodeId, LayerTreeModel* layers,
                                         SatelliteLayers* satellites,
                                         ProgramClock* clock, QWidget* parent)
    : QDialog(parent), m_nodeId(std::move(nodeId)), m_layers(layers),
      m_satellites(satellites), m_clock(clock),
      m_model(new QStandardItemModel(this)),
      m_filterModel(new QSortFilterProxyModel(this)),
      m_table(new QTableView(this)), m_filter(new QLineEdit(this)),
      m_count(new QLabel(this)), m_showAll(new QPushButton(this)),
      m_hideAll(new QPushButton(this)), m_showOnly(new QPushButton(this)),
      m_spans(new QPushButton(this))
{
  setObjectName(QStringLiteral("satelliteListDialog"));
  m_table->setObjectName(QStringLiteral("satelliteTable"));
  m_filter->setObjectName(QStringLiteral("satelliteFilter"));
  m_filter->setClearButtonEnabled(true);
  m_showAll->setObjectName(QStringLiteral("showAllSatellites"));
  m_hideAll->setObjectName(QStringLiteral("hideAllSatellites"));
  m_showOnly->setObjectName(QStringLiteral("showOnlyFiltered"));
  m_spans->setObjectName(QStringLiteral("satelliteSpans"));
  m_filterModel->setSourceModel(m_model);
  m_filterModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
  // Names and numbers.
  m_filterModel->setFilterKeyColumn(-1);
  m_table->setModel(m_filterModel);
  m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
  m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  m_table->setSortingEnabled(true);
  m_table->verticalHeader()->hide();
  m_table->horizontalHeader()->setStretchLastSection(true);
  connect(m_filter, &QLineEdit::textChanged, this,
          &SatelliteListDialog::setFilter);
  connect(m_model, &QStandardItemModel::itemChanged, this,
          &SatelliteListDialog::itemChanged);
  connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged,
          this, &SatelliteListDialog::rowSelected);
  connect(m_showAll, &QPushButton::clicked, this,
          [this]() { setAllShown(true); });
  connect(m_hideAll, &QPushButton::clicked, this,
          [this]() { setAllShown(false); });
  connect(m_showOnly, &QPushButton::clicked, this,
          &SatelliteListDialog::showOnlyFiltered);
  connect(m_spans, &QPushButton::clicked, this,
          &SatelliteListDialog::editSpans);
  connect(m_satellites, &SatelliteLayers::setLoaded, this,
          [this]() { reload(); });

  QHBoxLayout* top = new QHBoxLayout;
  top->addWidget(m_filter, 1);
  top->addWidget(m_count);
  QHBoxLayout* actions = new QHBoxLayout;
  actions->addWidget(m_showAll);
  actions->addWidget(m_hideAll);
  actions->addWidget(m_showOnly);
  actions->addStretch(1);
  actions->addWidget(m_spans);
  QDialogButtonBox* buttons =
      new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addLayout(top);
  layout->addWidget(m_table, 1);
  layout->addLayout(actions);
  layout->addWidget(buttons);
  resize(560, 600);
  retranslateUi();
  reload();
}

SatelliteListDialog::~SatelliteListDialog() = default;

void SatelliteListDialog::reload()
{
  const QModelIndex index = m_layers->indexOfId(m_nodeId);
  if (!index.isValid()) {
    return;
  }
  const LayerDefinition definition = m_layers->layerDefinition(index);
  m_satellites->load(definition.source);
  const std::shared_ptr<const SatelliteSet> set =
      m_satellites->set(definition.source);
  const SatelliteStyle layerStyle = SatelliteStyle::fromJson(definition.style);
  const QDateTime now = m_clock->time();
  const QLocale locale;
  const QIcon warning = style()->standardIcon(QStyle::SP_MessageBoxWarning);
  m_filling = true;
  m_model->removeRows(0, m_model->rowCount());
  if (set) {
    for (const std::shared_ptr<const Sgp4>& sat : set->satellites) {
      const OrbitalElements& e = sat->elements();
      QStandardItem* name = new QStandardItem(e.displayName());
      name->setCheckable(true);
      name->setCheckState(
          layerStyle.isShown(e.catalogueNumber) ? Qt::Checked : Qt::Unchecked);
      name->setData(e.catalogueNumber, numberRole);
      QStandardItem* number = new QStandardItem;
      number->setData(e.catalogueNumber, Qt::DisplayRole);
      const double age = SatelliteLayers::ageDays(e, now);
      // A number, so it sorts as one; to a tenth of a day.
      QStandardItem* ageItem = new QStandardItem;
      ageItem->setData(std::round(age * 10.0) / 10.0, Qt::DisplayRole);
      const QString epoch =
          e.epochTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
      ageItem->setToolTip(tr("Epoch %1 UTC").arg(epoch));
      if (std::fabs(age) > layerStyle.maximumAgeDays) {
        ageItem->setIcon(warning);
        ageItem->setToolTip(
            tr("Older than %1 days: positions may be far off. %2")
                .arg(locale.toString(layerStyle.maximumAgeDays),
                     ageItem->toolTip()));
      }
      const std::map<int32_t, SpanOverride>::const_iterator own =
          layerStyle.overrides.find(e.catalogueNumber);
      QStandardItem* spans = new QStandardItem(
          own != layerStyle.overrides.end() && !own->second.isEmpty()
              ? tr("Own")
              : QString());
      m_model->appendRow({name, number, ageItem, spans});
    }
  }
  m_filling = false;
  m_table->resizeColumnsToContents();
  setFilter(m_filter->text());
  setWindowTitle(tr("Satellites of %1").arg(definition.name));
}

void SatelliteListDialog::setFilter(const QString& text)
{
  if (m_filter->text() != text) {
    m_filter->setText(text);
    return;
  }
  m_filterModel->setFilterFixedString(text);
  m_count->setText(tr("%1 of %2").arg(visibleRows()).arg(rows()));
}

int32_t SatelliteListDialog::visibleRows() const
{
  return m_filterModel->rowCount();
}

int32_t SatelliteListDialog::rows() const
{
  return m_model->rowCount();
}

void SatelliteListDialog::writeStyle(const QJsonObject& style)
{
  const QModelIndex index = m_layers->indexOfId(m_nodeId);
  if (!index.isValid()) {
    return;
  }
  LayerDefinition definition = m_layers->layerDefinition(index);
  definition.style = style;
  m_layers->setLayerDefinition(index, definition);
}

void SatelliteListDialog::itemChanged(QStandardItem* item)
{
  if (m_filling || item->column() != NameColumn) {
    return;
  }
  const QModelIndex index = m_layers->indexOfId(m_nodeId);
  SatelliteStyle style =
      SatelliteStyle::fromJson(m_layers->layerDefinition(index).style);
  style.setShown(item->data(numberRole).toInt(),
                 item->checkState() == Qt::Checked);
  writeStyle(style.toJson());
}

bool SatelliteListDialog::setShown(int32_t catalogueNumber, bool shown)
{
  for (int32_t row = 0; row < m_model->rowCount(); ++row) {
    QStandardItem* item = m_model->item(row, NameColumn);
    if (item->data(numberRole).toInt() == catalogueNumber) {
      item->setCheckState(shown ? Qt::Checked : Qt::Unchecked);
      return true;
    }
  }
  return false;
}

void SatelliteListDialog::setAllShown(bool shown)
{
  const QModelIndex index = m_layers->indexOfId(m_nodeId);
  SatelliteStyle style =
      SatelliteStyle::fromJson(m_layers->layerDefinition(index).style);
  style.setAllShown(shown);
  writeStyle(style.toJson());
  reload();
}

void SatelliteListDialog::showOnlyFiltered()
{
  const QModelIndex index = m_layers->indexOfId(m_nodeId);
  SatelliteStyle style =
      SatelliteStyle::fromJson(m_layers->layerDefinition(index).style);
  style.setAllShown(false);
  for (int32_t row = 0; row < m_filterModel->rowCount(); ++row) {
    style.setShown(
        m_filterModel->index(row, NameColumn).data(numberRole).toInt(), true);
  }
  writeStyle(style.toJson());
  reload();
}

std::vector<int32_t> SatelliteListDialog::selectedNumbers() const
{
  std::vector<int32_t> numbers;
  for (const QModelIndex& row : m_table->selectionModel()->selectedRows()) {
    numbers.push_back(
        m_filterModel->index(row.row(), NameColumn).data(numberRole).toInt());
  }
  return numbers;
}

bool SatelliteListDialog::selectSatellite(int32_t catalogueNumber)
{
  for (int32_t row = 0; row < m_filterModel->rowCount(); ++row) {
    const QModelIndex index = m_filterModel->index(row, NameColumn);
    if (index.data(numberRole).toInt() == catalogueNumber) {
      m_table->selectRow(row);
      m_table->scrollTo(index);
      return true;
    }
  }
  return false;
}

void SatelliteListDialog::rowSelected()
{
  const std::vector<int32_t> numbers = selectedNumbers();
  m_spans->setEnabled(!numbers.empty());
  if (numbers.size() == 1) {
    m_satellites->select(m_nodeId, numbers.front());
  }
}

void SatelliteListDialog::editSpans()
{
  const std::vector<int32_t> numbers = selectedNumbers();
  if (numbers.empty()) {
    return;
  }
  const QModelIndex index = m_layers->indexOfId(m_nodeId);
  SatelliteStyle style =
      SatelliteStyle::fromJson(m_layers->layerDefinition(index).style);
  const std::map<int32_t, SpanOverride>::const_iterator own =
      style.overrides.find(numbers.front());
  QStringList names;
  for (const QModelIndex& row : m_table->selectionModel()->selectedRows()) {
    names.append(m_filterModel->index(row.row(), NameColumn).data().toString());
  }
  SpanOverrideDialog dialog(
      names.size() > 3
          ? tr("%n satellite(s)", nullptr, static_cast<int32_t>(names.size()))
          : names.join(QStringLiteral(", ")),
      own != style.overrides.end() ? own->second : SpanOverride(), style.past,
      style.future, this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  const SpanOverride spans = dialog.spans();
  for (const int32_t n : numbers) {
    if (spans.isEmpty()) {
      style.overrides.erase(n);
    } else {
      style.overrides[n] = spans;
    }
  }
  writeStyle(style.toJson());
  reload();
}

void SatelliteListDialog::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QDialog::changeEvent(event);
}

void SatelliteListDialog::retranslateUi()
{
  m_filter->setPlaceholderText(tr("Filter by name or catalogue number"));
  m_showAll->setText(tr("Show &All"));
  m_hideAll->setText(tr("&Hide All"));
  m_showOnly->setText(tr("Show &Only These"));
  m_showOnly->setToolTip(tr("Show only the satellites the filter leaves"));
  m_spans->setText(tr("Track &Spans…"));
  m_spans->setToolTip(tr("The selected satellites' own past and future "
                         "track spans"));
  m_spans->setEnabled(!selectedNumbers().empty());
  m_model->setHorizontalHeaderLabels(
      {tr("Name"), tr("Number"), tr("Age (days)"), tr("Spans")});
}

} // namespace ts
