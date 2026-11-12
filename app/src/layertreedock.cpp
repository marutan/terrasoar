// SPDX-License-Identifier: GPL-2.0-or-later
#include "layertreedock.h"

#include <cmath>
#include <vector>

#include <QAction>
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
  const int percent = static_cast<int>(std::lround(opacity * 100.0));
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
                    QModelIndex* parent, int* row)
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
  int row = 0;
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
  int row = 0;
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

void LayerTreeDock::setOpacityPercent(int percent)
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

} // namespace ts
