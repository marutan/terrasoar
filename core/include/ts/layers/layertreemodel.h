// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The layer tree: layers and nestable collections, as a Qt item model.
 */
#ifndef TS_LAYERS_LAYERTREEMODEL_H
#define TS_LAYERS_LAYERTREEMODEL_H

#include <memory>
#include <vector>

#include <QAbstractItemModel>
#include <QJsonArray>
#include <QString>

#include "ts/layers/layerdefinition.h"

namespace ts
{

/** One imagery layer as the renderer sees it, bottom to top. */
struct ImageryStackEntry {
  /** The tree entry. */
  QString nodeId;
  /** What it draws. */
  LayerDefinition definition;
  /**
   * Opacity to draw with: the product of the layer's and its ancestors'
   * opacities, up to (not including) its composite group if it has one.
   */
  double alpha = 1.0;
  /** Index into LayerStack::groups, or −1. */
  int group = -1;
};

/** A collection drawn offscreen and faded as one image. */
struct StackGroup {
  /** The collection. */
  QString nodeId;
  /** Opacity of the whole group: its own times its ancestors'. */
  double alpha = 1.0;
};

/** One vector layer as the renderer sees it, bottom to top. */
struct VectorStackEntry {
  /** The tree entry. */
  QString nodeId;
  /** What it draws. */
  LayerDefinition definition;
  /** Effective opacity (product of ancestors). */
  double alpha = 1.0;
};

/**
 * The visible layers flattened for drawing: imagery bottom to top (the
 * members of one composite group contiguous), then vectors bottom to top,
 * which are always drawn above all imagery.
 *
 * @reentrant
 */
struct LayerStack {
  /** Imagery, bottom to top. */
  std::vector<ImageryStackEntry> imagery;
  /** Composite groups referenced by imagery entries. */
  std::vector<StackGroup> groups;
  /** Vector layers, bottom to top. */
  std::vector<VectorStackEntry> vectors;
};

/**
 * The layer tree: layers and collections that nest, each with a name,
 * visibility and opacity; collections can also be drawn as one composite
 * group. Rows are in drawing order from the top: row 0 is drawn above
 * row 1.
 *
 * Effective visibility is the layer's and all its ancestors'; effective
 * opacity is the product of its and its ancestors' opacities. A
 * collection with "Composite as group" is drawn offscreen and faded as one
 * image instead (layerStack()). Imagery is always drawn before vector
 * layers, whatever the tree order. See docs/design/layer-model.md.
 *
 * One column. Check state is visibility; the custom roles carry the
 * rest. Rows can be moved by drag and drop (MIME type
 * "application/x-terrasoar-layer-nodes") or moveNode().
 *
 * @guithread
 */
class LayerTreeModel : public QAbstractItemModel
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(LayerTreeModel)

public:
  /** Data roles beyond Qt's. */
  enum Role {
    /** Own opacity, double in [0, 1] (editable). */
    OpacityRole = Qt::UserRole + 1,
    /** True for collections. */
    IsCollectionRole,
    /** "Composite as group", bool (editable, collections only). */
    CompositeAsGroupRole,
    /** The entry's unique id, QString. */
    NodeIdRole,
    /** Product of the entry's and its ancestors' opacities, double. */
    EffectiveOpacityRole,
    /** True if the entry and all its ancestors are visible. */
    EffectivelyVisibleRole,
    /** LayerType of a layer as int; −1 for collections. */
    LayerTypeRole,
  };

  /** MIME type of dragged entries (their ids, one per line). */
  static constexpr const char* mimeType = "application/x-terrasoar-layer-nodes";

  /** Creates an empty tree. */
  explicit LayerTreeModel(QObject* parent = nullptr);

  /** Destroys the tree. */
  ~LayerTreeModel() override;

  /** Returns the index of @p row, @p column under @p parent. */
  QModelIndex index(int row, int column,
                    const QModelIndex& parent = QModelIndex()) const override;
  /** Returns the parent of @p child. */
  QModelIndex parent(const QModelIndex& child) const override;
  /** Returns the number of children of @p parent. */
  int rowCount(const QModelIndex& parent = QModelIndex()) const override;
  /** Returns 1. */
  int columnCount(const QModelIndex& parent = QModelIndex()) const override;
  /** Returns the data of @p index for @p role. */
  QVariant data(const QModelIndex& index,
                int role = Qt::DisplayRole) const override;
  /** Sets the name, visibility, opacity or composite flag. */
  bool setData(const QModelIndex& index, const QVariant& value,
               int role = Qt::EditRole) override;
  /** Returns the item flags; collections and the root accept drops. */
  Qt::ItemFlags flags(const QModelIndex& index) const override;
  /** Returns Qt::MoveAction. */
  Qt::DropActions supportedDropActions() const override;
  /** Returns the layer-node MIME type. */
  QStringList mimeTypes() const override;
  /** Returns the ids of @p indexes as MIME data. */
  QMimeData* mimeData(const QModelIndexList& indexes) const override;
  /**
   * Moves the dragged entries to @p row under @p parent. Returns false
   * even when it moved them, so that the view doesn't remove the source
   * rows a second time.
   */
  bool dropMimeData(const QMimeData* data, Qt::DropAction action, int row,
                    int column, const QModelIndex& parent) override;

  /**
   * Adds a layer showing @p definition at @p row (0 = top) under
   * @p parent (a collection, or the root). Returns its index.
   */
  QModelIndex addLayer(const LayerDefinition& definition,
                       const QModelIndex& parent = QModelIndex(), int row = 0);

  /** Adds an empty collection named @p name. Returns its index. */
  QModelIndex addCollection(const QString& name,
                            const QModelIndex& parent = QModelIndex(),
                            int row = 0);

  /** Removes @p index and everything under it. */
  bool removeNode(const QModelIndex& index);

  /**
   * Moves @p index under @p newParent so that it ends up at @p row there.
   * Refuses to move a collection into itself or its descendants.
   */
  bool moveNode(const QModelIndex& index, const QModelIndex& newParent,
                int row);

  /** Removes everything. */
  void clear();

  /** Returns the definition of the layer at @p index (empty for collections).
   */
  LayerDefinition layerDefinition(const QModelIndex& index) const;

  /** Returns the index of the entry with id @p id, or an invalid one. */
  QModelIndex indexOfId(const QString& id) const;

  /** Returns the visible layers flattened for drawing. */
  LayerStack layerStack() const;

  /** Returns the tree as JSON (top to bottom). */
  QJsonArray toJson() const;

  /**
   * Replaces the tree with @p json. On error leaves the tree unchanged,
   * returns false and sets @p errorString.
   */
  bool fromJson(const QJsonArray& json, QString* errorString);

signals:
  /** Anything that changes what is drawn changed. */
  void layerStackChanged();

private:
  struct Node;

  Node* nodeAt(const QModelIndex& index) const;
  QModelIndex indexOf(const Node* node) const;
  QModelIndex insertNode(std::unique_ptr<Node> node, const QModelIndex& parent,
                         int row);
  void emitSubtreeChanged(const QModelIndex& index);
  void emitDataChangedBelow(const QModelIndex& index);

  std::unique_ptr<Node> m_root;
};

} // namespace ts

#endif // TS_LAYERS_LAYERTREEMODEL_H
