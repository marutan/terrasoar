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

#include "layertreedock.h"

#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include <QAction>
#include <QActionGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPersistentModelIndex>
#include <QPushButton>
#include <QSlider>
#include <QToolButton>
#include <QTreeView>
#include <QUuid>
#include <QVBoxLayout>

#include "ts/layers/layertreemodel.h"

namespace ts
{

LayerTreeDock::LayerTreeDock(LayerTreeModel* model, QWidget* parent)
    : QDockWidget(parent), m_model(model)
{
  setObjectName(QStringLiteral("layerTreeDock"));
  QWidget* content = new QWidget(this);
  QVBoxLayout* layout = new QVBoxLayout(content);

  m_view = new QTreeView(content);
  m_view->setObjectName(QStringLiteral("layerTreeView"));
  m_view->setModel(m_model);
  m_view->setHeaderHidden(true);
  m_view->setDragDropMode(QAbstractItemView::InternalMove);
  m_view->setDefaultDropAction(Qt::MoveAction);
  m_view->setSelectionMode(QAbstractItemView::SingleSelection);
  m_view->setEditTriggers(QAbstractItemView::DoubleClicked |
                          QAbstractItemView::EditKeyPressed);
  m_view->expandAll();
  m_view->setContextMenuPolicy(Qt::CustomContextMenu);
  connect(m_view, &QTreeView::customContextMenuRequested, this,
          [this](QPoint position) {
            QMenu* menu = contextMenu(m_view->indexAt(position));
            if (menu != nullptr) {
              menu->popup(m_view->viewport()->mapToGlobal(position));
            }
          });
  m_view->viewport()->installEventFilter(this);
  layout->addWidget(m_view, 1);

  // The drawing rule the tree order can't show: vectors over imagery.
  m_orderNote = new QLabel(content);
  m_orderNote->setObjectName(QStringLiteral("layerOrderNote"));
  m_orderNote->setWordWrap(true);
  QFont noteFont = m_orderNote->font();
  noteFont.setPointSizeF(noteFont.pointSizeF() * 0.9);
  m_orderNote->setFont(noteFont);
  layout->addWidget(m_orderNote);

  QHBoxLayout* buttons = new QHBoxLayout;
  m_addLayerButton = new QToolButton(content);
  m_addLayerButton->setObjectName(QStringLiteral("addLayerButton"));
  m_addLayerButton->setPopupMode(QToolButton::InstantPopup);
  m_addLayerMenu = new QMenu(content);
  m_addLayerMenu->setObjectName(QStringLiteral("addLayerMenu"));
  m_addLayerButton->setMenu(m_addLayerMenu);
  m_addCollectionButton = new QPushButton(content);
  m_addCollectionButton->setObjectName(QStringLiteral("addCollectionButton"));
  m_removeButton = new QPushButton(content);
  m_removeButton->setObjectName(QStringLiteral("removeLayerButton"));
  buttons->addWidget(m_addLayerButton);
  buttons->addWidget(m_addCollectionButton);
  buttons->addWidget(m_removeButton);
  buttons->addStretch(1);
  layout->addLayout(buttons);

  QHBoxLayout* opacity = new QHBoxLayout;
  m_opacityLabel = new QLabel(content);
  m_opacityLabel->setObjectName(QStringLiteral("opacityLabel"));
  m_opacitySlider = new QSlider(Qt::Horizontal, content);
  m_opacitySlider->setObjectName(QStringLiteral("opacitySlider"));
  m_opacitySlider->setRange(0, 100);
  m_opacityLabel->setBuddy(m_opacitySlider);
  m_opacityValue = new QLabel(content);
  m_opacityValue->setObjectName(QStringLiteral("opacityValue"));
  opacity->addWidget(m_opacityLabel);
  opacity->addWidget(m_opacitySlider, 1);
  opacity->addWidget(m_opacityValue);
  layout->addLayout(opacity);

  m_compositeBox = new QCheckBox(content);
  m_compositeBox->setObjectName(QStringLiteral("compositeAsGroupBox"));
  layout->addWidget(m_compositeBox);
  setWidget(content);

  connect(m_addCollectionButton, &QPushButton::clicked, this,
          &LayerTreeDock::addCollection);
  connect(m_removeButton, &QPushButton::clicked, this,
          &LayerTreeDock::removeCurrent);
  connect(m_opacitySlider, &QSlider::valueChanged, this,
          &LayerTreeDock::setOpacityPercent);
  connect(m_compositeBox, &QCheckBox::toggled, this,
          &LayerTreeDock::setCompositeAsGroup);
  connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged, this,
          &LayerTreeDock::updateControls);
  connect(m_model, &QAbstractItemModel::dataChanged, this,
          &LayerTreeDock::updateControls);
  connect(m_model, &QAbstractItemModel::modelReset, this, [this]() {
    m_view->expandAll();
    updateControls();
  });
  connect(m_model, &QAbstractItemModel::rowsInserted, this,
          [this](const QModelIndex& inserted) { m_view->expand(inserted); });

  retranslateUi();
  updateControls();
}

LayerTreeDock::~LayerTreeDock() = default;

void LayerTreeDock::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QDockWidget::changeEvent(event);
}

void LayerTreeDock::retranslateUi()
{
  setWindowTitle(tr("Layers"));
  m_addLayerButton->setText(tr("Add &Layer"));
  m_addLayerMenu->setTitle(tr("Add Layer"));
  m_orderNote->setText(tr("Vector layers (shorelines, graticule) are always "
                          "drawn above imagery, in their order in the tree."));
  m_addCollectionButton->setText(tr("Add &Collection"));
  m_removeButton->setText(tr("&Remove"));
  m_opacityLabel->setText(tr("&Opacity:"));
  m_compositeBox->setText(tr("Composite as &group"));
  m_compositeBox->setToolTip(
      m_compositingAvailable
          ? tr("Draw the collection's layers together and fade them as one "
               "image. Costs an extra full-screen pass.")
          : tr("Not available with software rendering: the collection's "
               "opacity multiplies into its layers instead."));
  rebuildAddLayerMenu();
  updateControls();
}

void LayerTreeDock::rebuildAddLayerMenu()
{
  m_addLayerMenu->clear();
  for (const LayerDefinition& definition : builtinLayerDefinitions()) {
    QAction* action = m_addLayerMenu->addAction(definition.name);
    connect(action, &QAction::triggered, this,
            [this, definition]() { addLayer(definition); });
  }
  m_addLayerMenu->addSeparator();
  QAction* wms = m_addLayerMenu->addAction(tr("WMS Layer…"));
  wms->setObjectName(QStringLiteral("addWmsLayerAction"));
  connect(wms, &QAction::triggered, this, &LayerTreeDock::addWmsLayerRequested);
  QAction* wmts = m_addLayerMenu->addAction(tr("WMTS Layer…"));
  wmts->setObjectName(QStringLiteral("addWmtsLayerAction"));
  connect(wmts, &QAction::triggered, this,
          &LayerTreeDock::addWmtsLayerRequested);
  QAction* wfs = m_addLayerMenu->addAction(tr("WFS Layer…"));
  wfs->setObjectName(QStringLiteral("addWfsLayerAction"));
  connect(wfs, &QAction::triggered, this, &LayerTreeDock::addWfsLayerRequested);
  QAction* xyz = m_addLayerMenu->addAction(tr("XYZ Tile Layer…"));
  xyz->setObjectName(QStringLiteral("addXyzLayerAction"));
  connect(xyz, &QAction::triggered, this, &LayerTreeDock::addXyzLayerRequested);
  QAction* imagery = m_addLayerMenu->addAction(tr("Imagery File…"));
  imagery->setObjectName(QStringLiteral("addImageryFileAction"));
  connect(imagery, &QAction::triggered, this,
          &LayerTreeDock::addImageryFileRequested);
  QAction* elevation = m_addLayerMenu->addAction(tr("Elevation File…"));
  elevation->setObjectName(QStringLiteral("addElevationFileAction"));
  connect(elevation, &QAction::triggered, this,
          &LayerTreeDock::addElevationFileRequested);
  QAction* webTerrain = m_addLayerMenu->addAction(tr("Web Terrain…"));
  webTerrain->setObjectName(QStringLiteral("addWebTerrainLayerAction"));
  connect(webTerrain, &QAction::triggered, this,
          &LayerTreeDock::addWebTerrainRequested);
  QAction* solid = m_addLayerMenu->addAction(tr("Solid Colour…"));
  solid->setObjectName(QStringLiteral("addSolidColourAction"));
  connect(solid, &QAction::triggered, this, &LayerTreeDock::addSolidColorLayer);
}

void LayerTreeDock::setGroupCompositingAvailable(bool available)
{
  m_compositingAvailable = available;
  retranslateUi();
}

void LayerTreeDock::updateControls()
{
  const QModelIndex current = m_view->currentIndex();
  const bool selected = current.isValid();
  m_removeButton->setEnabled(selected);
  m_opacitySlider->setEnabled(selected);
  const double opacity =
      selected ? m_model->data(current, LayerTreeModel::OpacityRole).toDouble()
               : 1.0;
  const int32_t percent = static_cast<int32_t>(std::lround(opacity * 100.0));
  {
    const QSignalBlocker blocker(m_opacitySlider);
    m_opacitySlider->setValue(percent);
  }
  m_opacityValue->setText(tr("%1 %").arg(QLocale().toString(percent)));
  const bool collection =
      selected &&
      m_model->data(current, LayerTreeModel::IsCollectionRole).toBool();
  m_compositeBox->setEnabled(collection);
  {
    const QSignalBlocker blocker(m_compositeBox);
    m_compositeBox->setChecked(
        collection &&
        m_model->data(current, LayerTreeModel::CompositeAsGroupRole).toBool());
  }
}

namespace
{

/** Where a new entry goes: into a selected collection at its top, else
 *  just above the selected entry, else at the top of the tree. */
void insertionPoint(const LayerTreeModel& model, const QModelIndex& current,
                    QModelIndex* parent, int32_t* row)
{
  if (!current.isValid()) {
    *parent = QModelIndex();
    *row = 0;
  } else if (model.data(current, LayerTreeModel::IsCollectionRole).toBool()) {
    *parent = current;
    *row = 0;
  } else {
    *parent = current.parent();
    *row = current.row();
  }
}

} // namespace

void LayerTreeDock::addLayer(const LayerDefinition& definition)
{
  QModelIndex parent;
  int32_t row = 0;
  insertionPoint(*m_model, m_view->currentIndex(), &parent, &row);
  const QModelIndex added = m_model->addLayer(definition, parent, row);
  m_view->setCurrentIndex(added);
}

void LayerTreeDock::addSolidColorLayer()
{
  const QColor color = QColorDialog::getColor(QColor(255, 255, 255, 128), this,
                                              tr("Layer Colour"),
                                              QColorDialog::ShowAlphaChannel);
  if (!color.isValid()) {
    return;
  }
  LayerDefinition definition;
  definition.id = QStringLiteral("user:solid-") +
                  QUuid::createUuid().toString(QUuid::WithoutBraces);
  definition.origin = LayerOrigin::User;
  definition.type = LayerType::Imagery;
  definition.name = tr("Colour %1").arg(color.name(QColor::HexArgb));
  QJsonObject source;
  source.insert(QStringLiteral("kind"), QStringLiteral("solid"));
  source.insert(QStringLiteral("color"), color.name(QColor::HexArgb));
  source.insert(QStringLiteral("tileSize"), 64);
  definition.source = source;
  addLayer(definition);
}

void LayerTreeDock::addCollection()
{
  QModelIndex parent;
  int32_t row = 0;
  insertionPoint(*m_model, m_view->currentIndex(), &parent, &row);
  const QModelIndex added =
      m_model->addCollection(tr("New Collection"), parent, row);
  m_view->setCurrentIndex(added);
  m_view->edit(added);
}

void LayerTreeDock::removeCurrent()
{
  m_model->removeNode(m_view->currentIndex());
  updateControls();
}

void LayerTreeDock::setOpacityPercent(int32_t percent)
{
  const QModelIndex current = m_view->currentIndex();
  if (current.isValid()) {
    m_model->setData(current, percent / 100.0, LayerTreeModel::OpacityRole);
  }
}

void LayerTreeDock::setCompositeAsGroup(bool composite)
{
  const QModelIndex current = m_view->currentIndex();
  if (current.isValid()) {
    m_model->setData(current, composite, LayerTreeModel::CompositeAsGroupRole);
  }
}

void LayerTreeDock::setTimeLookup(
    std::function<std::optional<LayerTime>(const QJsonObject&)> lookup)
{
  m_timeLookup = std::move(lookup);
}

void LayerTreeDock::setNodeActions(
    std::function<std::vector<QAction*>(const QModelIndex&, QObject*)> actions)
{
  m_nodeActions = std::move(actions);
}

void LayerTreeDock::setDoubleClickAction(
    std::function<bool(const QModelIndex&)> action)
{
  m_doubleClick = std::move(action);
}

QModelIndex LayerTreeDock::currentIndex() const
{
  return m_view->currentIndex();
}

void LayerTreeDock::setCurrentIndex(const QModelIndex& index)
{
  m_view->setCurrentIndex(index);
}

void LayerTreeDock::doubleClick(const QModelIndex& index)
{
  if (!m_doubleClick || !m_doubleClick(index)) {
    m_view->edit(index);
  }
}

bool LayerTreeDock::eventFilter(QObject* watched, QEvent* event)
{
  if (watched == m_view->viewport() &&
      event->type() == QEvent::MouseButtonDblClick && m_doubleClick) {
    const QMouseEvent* mouse = dynamic_cast<QMouseEvent*>(event);
    const QModelIndex index =
        mouse != nullptr ? m_view->indexAt(mouse->pos()) : QModelIndex();
    if (index.isValid() && m_doubleClick(index)) {
      return true; // done: not renamed
    }
  }
  return QDockWidget::eventFilter(watched, event);
}

QMenu* LayerTreeDock::contextMenu(const QModelIndex& index)
{
  if (!index.isValid()) {
    return nullptr;
  }
  if (m_contextMenu == nullptr) {
    m_contextMenu = new QMenu(this);
    m_contextMenu->setObjectName(QStringLiteral("layerContextMenu"));
  }
  m_contextMenu->clear();
  // Actions made for the last menu go with it.
  for (QAction* old : m_contextActions) {
    old->deleteLater();
  }
  m_contextActions.clear();
  if (m_nodeActions) {
    m_contextActions = m_nodeActions(index, m_contextMenu);
    for (QAction* action : m_contextActions) {
      m_contextMenu->addAction(action);
    }
  }
  QMenu* time = timeMenu(index);
  if (time != nullptr) {
    m_contextMenu->addMenu(time);
  }
  return m_contextMenu->isEmpty() ? nullptr : m_contextMenu;
}

QMenu* LayerTreeDock::timeMenu(const QModelIndex& index)
{
  if (!index.isValid()) {
    return nullptr;
  }
  const LayerDefinition definition = m_model->layerDefinition(index);
  std::optional<LayerTime> time = LayerTime::fromSource(definition.source);
  if (!time && m_timeLookup) {
    time = m_timeLookup(definition.source);
  }
  if (!time || time->times().isEmpty()) {
    return nullptr;
  }
  const LayerTime current = *time;
  if (m_timeMenu == nullptr) {
    m_timeMenu = new QMenu(this);
    m_timeMenu->setObjectName(QStringLiteral("layerTimeMenu"));
  }
  m_timeMenu->clear();
  for (QActionGroup* old : m_timeMenu->findChildren<QActionGroup*>()) {
    delete old;
  }
  m_timeMenu->setTitle(tr("Time"));
  const QPersistentModelIndex layer(index);
  const std::function<void(const LayerTime&)> choose =
      [this, layer](const LayerTime& chosen) {
        if (!layer.isValid()) {
          return;
        }
        LayerDefinition changed = m_model->layerDefinition(layer);
        changed.source.insert(QStringLiteral("time"), chosen.toJson());
        m_model->setLayerDefinition(layer, changed);
      };
  QActionGroup* group = new QActionGroup(m_timeMenu);
  const std::function<QAction*(const QString&, LayerTime::Mode, const QString&)>
      add =
          [&](const QString& text, LayerTime::Mode mode, const QString& value) {
            QAction* action = m_timeMenu->addAction(text);
            action->setCheckable(true);
            action->setActionGroup(group);
            action->setChecked(
                current.mode == mode &&
                (mode != LayerTime::Mode::Fixed || current.value == value));
            LayerTime chosen = current;
            chosen.mode = mode;
            chosen.value = value;
            connect(action, &QAction::triggered, this,
                    [choose, chosen]() { choose(chosen); });
            return action;
          };
  add(tr("Follow the program's clock"), LayerTime::Mode::Clock, QString())
      ->setObjectName(QStringLiteral("timeFollowClockAction"));
  const TimeExtent extent = current.times();
  if (LayerTime::isMonthly(extent)) {
    add(tr("The clock's month, any year"), LayerTime::Mode::MonthOfYear,
        QString())
        ->setObjectName(QStringLiteral("timeMonthAction"));
  }
  m_timeMenu->addSeparator();
  // Its latest times (at most 24), latest first.
  for (const QDateTime& latest : extent.latest(24)) {
    const QString value = extent.format(latest);
    add(value, LayerTime::Mode::Fixed, value);
  }
  return m_timeMenu;
}

} // namespace ts
