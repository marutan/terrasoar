// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The dockable layer tree: layers and collections, order, visibility and
 * opacity.
 */
#ifndef TS_APP_LAYERTREEDOCK_H
#define TS_APP_LAYERTREEDOCK_H

#include <QDockWidget>

class QCheckBox;
class QEvent;
class QLabel;
class QMenu;
class QPushButton;
class QSlider;
class QToolButton;
class QTreeView;

namespace ts
{

class LayerTreeModel;
struct LayerDefinition;

/**
 * Shows the layer tree in a tree view: check boxes for visibility, drag
 * and drop to reorder or move between collections, double click to
 * rename. Below it: buttons to add layers and collections and to remove
 * entries, an opacity slider and, for collections, "Composite as group"
 * for the selected entry.
 *
 * Changes go straight into the model, which the globe view follows, so
 * they show in the next frame.
 *
 * @guithread
 */
class LayerTreeDock : public QDockWidget
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(LayerTreeDock)

public:
  /** Creates the dock showing @p model. */
  LayerTreeDock(LayerTreeModel* model, QWidget* parent = nullptr);

  /** Destroys the dock. */
  ~LayerTreeDock() override;

  /** Returns the tree view. */
  QTreeView* view() const { return m_view; }

  /**
   * Tells the dock whether composite groups are drawn as such; when not
   * (software rendering), the option explains that opacity multiplies.
   */
  void setGroupCompositingAvailable(bool available);

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

private:
  void retranslateUi();
  void rebuildAddLayerMenu();
  void updateControls();
  void addLayer(const LayerDefinition& definition);
  void addSolidColorLayer();
  void addCollection();
  void removeCurrent();
  void setOpacityPercent(int percent);
  void setCompositeAsGroup(bool composite);

  LayerTreeModel* m_model;
  QTreeView* m_view = nullptr;
  QToolButton* m_addLayerButton = nullptr;
  QMenu* m_addLayerMenu = nullptr;
  QPushButton* m_addCollectionButton = nullptr;
  QPushButton* m_removeButton = nullptr;
  QLabel* m_opacityLabel = nullptr;
  QSlider* m_opacitySlider = nullptr;
  QLabel* m_opacityValue = nullptr;
  QCheckBox* m_compositeBox = nullptr;
  bool m_compositingAvailable = true;
};

} // namespace ts

#endif // TS_APP_LAYERTREEDOCK_H
