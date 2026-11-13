// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/layers/layertreemodel.h"

#include <algorithm>
#include <utility>

#include <QCoreApplication>
#include <QJsonObject>
#include <QMimeData>
#include <QUuid>

namespace ts
{

/** One entry of the tree. */
struct LayerTreeModel::Node {
  enum class Kind { Layer, Collection };

  Kind kind = Kind::Layer;
  QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  QString name;
  bool visible = true;
  double opacity = 1.0;
  bool compositeAsGroup = false;
  LayerDefinition definition;
  Node* parent = nullptr;
  std::vector<std::unique_ptr<Node>> children;

  int row() const
  {
    if (!parent) {
      return 0;
    }
    const std::vector<std::unique_ptr<Node>>& siblings = parent->children;
    for (std::size_t i = 0; i < siblings.size(); ++i) {
      if (siblings[i].get() == this) {
        return static_cast<int>(i);
      }
    }
    return 0;
  }

  bool isAncestorOf(const Node* other) const
  {
    for (const Node* n = other; n != nullptr; n = n->parent) {
      if (n == this) {
        return true;
      }
    }
    return false;
  }

  double effectiveOpacity() const
  {
    double alpha = 1.0;
    for (const Node* n = this; n != nullptr; n = n->parent) {
      alpha *= n->opacity;
    }
    return alpha;
  }

  bool effectivelyVisible() const
  {
    for (const Node* n = this; n != nullptr; n = n->parent) {
      if (!n->visible) {
        return false;
      }
    }
    return true;
  }
};

namespace
{

QString nodeKindName(bool collection)
{
  return collection ? QStringLiteral("collection") : QStringLiteral("layer");
}

} // namespace

LayerTreeModel::LayerTreeModel(QObject* parent)
    : QAbstractItemModel(parent), m_root(std::make_unique<Node>())
{
  m_root->kind = Node::Kind::Collection;
}

LayerTreeModel::~LayerTreeModel() = default;

LayerTreeModel::Node* LayerTreeModel::nodeAt(const QModelIndex& index) const
{
  if (!index.isValid()) {
    return m_root.get();
  }
  return static_cast<Node*>(index.internalPointer());
}

QModelIndex LayerTreeModel::indexOf(const Node* node) const
{
  if (node == nullptr || node == m_root.get()) {
    return QModelIndex();
  }
  // Qt's internal pointer is non-const; every node is owned by this model.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
  Node* const mutableNode = const_cast<Node*>(node);
  return createIndex(node->row(), 0, mutableNode);
}

QModelIndex LayerTreeModel::index(int row, int column,
                                  const QModelIndex& parent) const
{
  const Node* parentNode = nodeAt(parent);
  if (column != 0 || row < 0 ||
      row >= static_cast<int>(parentNode->children.size())) {
    return QModelIndex();
  }
  return createIndex(row, 0,
                     parentNode->children[static_cast<std::size_t>(row)].get());
}

QModelIndex LayerTreeModel::parent(const QModelIndex& child) const
{
  if (!child.isValid()) {
    return QModelIndex();
  }
  return indexOf(nodeAt(child)->parent);
}

int LayerTreeModel::rowCount(const QModelIndex& parent) const
{
  if (parent.column() > 0) {
    return 0;
  }
  return static_cast<int>(nodeAt(parent)->children.size());
}

int LayerTreeModel::columnCount(const QModelIndex& /*parent*/) const
{
  return 1;
}

QVariant LayerTreeModel::data(const QModelIndex& index, int role) const
{
  if (!index.isValid()) {
    return QVariant();
  }
  const Node* node = nodeAt(index);
  const bool collection = node->kind == Node::Kind::Collection;
  switch (role) {
  case Qt::DisplayRole:
  case Qt::EditRole:
    return node->name;
  case Qt::CheckStateRole:
    return node->visible ? Qt::Checked : Qt::Unchecked;
  case OpacityRole:
    return node->opacity;
  case IsCollectionRole:
    return collection;
  case CompositeAsGroupRole:
    return node->compositeAsGroup;
  case NodeIdRole:
    return node->id;
  case EffectiveOpacityRole:
    return node->effectiveOpacity();
  case EffectivelyVisibleRole:
    return node->effectivelyVisible();
  case LayerTypeRole:
    return collection ? -1 : static_cast<int>(node->definition.type);
  case StatusRole:
  case StatusMessageRole:
  case Qt::DecorationRole:
  case Qt::ToolTipRole: {
    const std::map<QString, ProviderStatus>::const_iterator status =
        m_status.find(node->id);
    if (status == m_status.end()) {
      if (role == StatusRole) {
        return static_cast<int>(ProviderStatus::State::Ok);
      }
      if (role == StatusMessageRole) {
        return QString();
      }
      return QVariant();
    }
    if (role == StatusRole) {
      return static_cast<int>(status->second.state);
    }
    if (role == Qt::DecorationRole) {
      return status->second.state == ProviderStatus::State::Error
                 ? m_errorIcon
                 : m_offlineIcon;
    }
    return status->second.message;
  }
  default:
    return QVariant();
  }
}

void LayerTreeModel::setLayerStatus(const QString& nodeId,
                                    const ProviderStatus& status)
{
  const QModelIndex index = indexOfId(nodeId);
  if (!index.isValid()) {
    return;
  }
  const std::map<QString, ProviderStatus>::iterator known =
      m_status.find(nodeId);
  if (status.state == ProviderStatus::State::Ok) {
    if (known == m_status.end()) {
      return;
    }
    m_status.erase(known);
  } else if (known != m_status.end() && known->second == status) {
    return;
  } else {
    m_status[nodeId] = status;
  }
  emit dataChanged(
      index, index,
      {Qt::DecorationRole, Qt::ToolTipRole, StatusRole, StatusMessageRole});
}

void LayerTreeModel::setStatusIcons(const QIcon& offline, const QIcon& error)
{
  m_offlineIcon = offline;
  m_errorIcon = error;
}

bool LayerTreeModel::setLayerDefinition(const QModelIndex& index,
                                        const LayerDefinition& definition)
{
  if (!index.isValid() || nodeAt(index)->kind == Node::Kind::Collection) {
    return false;
  }
  nodeAt(index)->definition = definition;
  emitSubtreeChanged(index);
  return true;
}

bool LayerTreeModel::setData(const QModelIndex& index, const QVariant& value,
                             int role)
{
  if (!index.isValid()) {
    return false;
  }
  Node* node = nodeAt(index);
  switch (role) {
  case Qt::EditRole:
    node->name = value.toString();
    emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole});
    return true;
  case Qt::CheckStateRole:
    node->visible = value.toInt() == Qt::Checked;
    emitSubtreeChanged(index);
    return true;
  case OpacityRole:
    node->opacity = std::clamp(value.toDouble(), 0.0, 1.0);
    emitSubtreeChanged(index);
    return true;
  case CompositeAsGroupRole:
    if (node->kind != Node::Kind::Collection) {
      return false;
    }
    node->compositeAsGroup = value.toBool();
    emitSubtreeChanged(index);
    return true;
  default:
    return false;
  }
}

void LayerTreeModel::emitDataChangedBelow(const QModelIndex& index)
{
  // Effective values of every descendant change too.
  emit dataChanged(index, index);
  const int rows = rowCount(index);
  for (int row = 0; row < rows; ++row) {
    emitDataChangedBelow(this->index(row, 0, index));
  }
}

void LayerTreeModel::emitSubtreeChanged(const QModelIndex& index)
{
  emitDataChangedBelow(index);
  emit layerStackChanged();
}

Qt::ItemFlags LayerTreeModel::flags(const QModelIndex& index) const
{
  if (!index.isValid()) {
    return Qt::ItemIsDropEnabled;
  }
  Qt::ItemFlags flags = Qt::ItemIsSelectable | Qt::ItemIsEnabled |
                        Qt::ItemIsUserCheckable | Qt::ItemIsEditable |
                        Qt::ItemIsDragEnabled;
  if (nodeAt(index)->kind == Node::Kind::Collection) {
    flags |= Qt::ItemIsDropEnabled;
  }
  return flags;
}

Qt::DropActions LayerTreeModel::supportedDropActions() const
{
  return Qt::MoveAction;
}

QStringList LayerTreeModel::mimeTypes() const
{
  return {QLatin1String(mimeType)};
}

QMimeData* LayerTreeModel::mimeData(const QModelIndexList& indexes) const
{
  QStringList ids;
  for (const QModelIndex& index : indexes) {
    if (index.isValid()) {
      ids << nodeAt(index)->id;
    }
  }
  QMimeData* data = new QMimeData;
  data->setData(QLatin1String(mimeType), ids.join(QLatin1Char('\n')).toUtf8());
  return data;
}

bool LayerTreeModel::dropMimeData(const QMimeData* data, Qt::DropAction action,
                                  int row, int /*column*/,
                                  const QModelIndex& parent)
{
  if (action != Qt::MoveAction || data == nullptr ||
      !data->hasFormat(QLatin1String(mimeType))) {
    return false;
  }
  const QStringList ids = QString::fromUtf8(data->data(QLatin1String(mimeType)))
                              .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
  // Dropped on a collection itself (row −1): to its top.
  int target = row < 0 ? 0 : row;
  for (const QString& id : ids) {
    const QModelIndex source = indexOfId(id);
    if (!source.isValid()) {
      continue;
    }
    // The drop row counts the dragged entry if it sits above it in the
    // same collection.
    int finalRow = target;
    if (source.parent() == parent && source.row() < target) {
      --finalRow;
    }
    if (moveNode(source, parent, finalRow)) {
      target = finalRow + 1;
    }
  }
  return false;
}

QModelIndex LayerTreeModel::insertNode(std::unique_ptr<Node> node,
                                       const QModelIndex& parent, int row)
{
  Node* parentNode = nodeAt(parent);
  if (parentNode->kind != Node::Kind::Collection) {
    return QModelIndex();
  }
  const int position =
      std::clamp(row, 0, static_cast<int>(parentNode->children.size()));
  beginInsertRows(parent, position, position);
  node->parent = parentNode;
  Node* raw = node.get();
  parentNode->children.insert(parentNode->children.begin() + position,
                              std::move(node));
  endInsertRows();
  emit layerStackChanged();
  return indexOf(raw);
}

QModelIndex LayerTreeModel::addLayer(const LayerDefinition& definition,
                                     const QModelIndex& parent, int row)
{
  std::unique_ptr<Node> node = std::make_unique<Node>();
  node->kind = Node::Kind::Layer;
  node->name = definition.name;
  node->definition = definition;
  return insertNode(std::move(node), parent, row);
}

QModelIndex LayerTreeModel::addCollection(const QString& name,
                                          const QModelIndex& parent, int row)
{
  std::unique_ptr<Node> node = std::make_unique<Node>();
  node->kind = Node::Kind::Collection;
  node->name = name;
  return insertNode(std::move(node), parent, row);
}

bool LayerTreeModel::removeNode(const QModelIndex& index)
{
  if (!index.isValid()) {
    return false;
  }
  Node* node = nodeAt(index);
  Node* parentNode = node->parent;
  const int row = node->row();
  beginRemoveRows(index.parent(), row, row);
  parentNode->children.erase(parentNode->children.begin() + row);
  endRemoveRows();
  emit layerStackChanged();
  return true;
}

bool LayerTreeModel::moveNode(const QModelIndex& index,
                              const QModelIndex& newParent, int row)
{
  if (!index.isValid()) {
    return false;
  }
  Node* node = nodeAt(index);
  Node* destination = nodeAt(newParent);
  if (destination->kind != Node::Kind::Collection ||
      node->isAncestorOf(destination)) {
    return false;
  }
  Node* source = node->parent;
  const int sourceRow = node->row();
  const int destinationSize = static_cast<int>(destination->children.size());
  const int finalRow = std::clamp(
      row, 0, source == destination ? destinationSize - 1 : destinationSize);
  if (source == destination && finalRow == sourceRow) {
    return true;
  }
  // Qt counts the destination row before the move.
  const int destinationChild =
      source == destination && finalRow > sourceRow ? finalRow + 1 : finalRow;
  if (!beginMoveRows(index.parent(), sourceRow, sourceRow, newParent,
                     destinationChild)) {
    return false;
  }
  std::unique_ptr<Node> moving =
      std::move(source->children[static_cast<std::size_t>(sourceRow)]);
  source->children.erase(source->children.begin() + sourceRow);
  moving->parent = destination;
  destination->children.insert(destination->children.begin() + finalRow,
                               std::move(moving));
  endMoveRows();
  emitSubtreeChanged(indexOf(node));
  return true;
}

void LayerTreeModel::clear()
{
  beginResetModel();
  m_root->children.clear();
  endResetModel();
  emit layerStackChanged();
}

QModelIndex LayerTreeModel::indexOfId(const QString& id) const
{
  std::vector<const Node*> pending = {m_root.get()};
  while (!pending.empty()) {
    const Node* node = pending.back();
    pending.pop_back();
    if (node != m_root.get() && node->id == id) {
      return indexOf(node);
    }
    for (const std::unique_ptr<Node>& child : node->children) {
      pending.push_back(child.get());
    }
  }
  return QModelIndex();
}

namespace
{

/** What a node inherits from its ancestors. */
struct Inherited {
  double alpha = 1.0;  ///< Product of the ancestors' opacities.
  int group = -1;      ///< Enclosing composite group, or -1.
  bool visible = true; ///< All ancestors checked.
};

/** Walks the tree bottom to top, collecting what is drawn. */
class StackBuilder
{
public:
  LayerStack stack;

  void visitChildren(const LayerTreeModel& model, const QModelIndex& parent,
                     Inherited inherited)
  {
    // Row 0 is the top: walk from the last row up.
    for (int row = model.rowCount(parent) - 1; row >= 0; --row) {
      visit(model, model.index(row, 0, parent), inherited);
    }
  }

private:
  void visit(const LayerTreeModel& model, const QModelIndex& index,
             Inherited inherited)
  {
    if (model.data(index, Qt::CheckStateRole).toInt() != Qt::Checked ||
        !inherited.visible) {
      // Hidden, but its elevation layers still count (see
      // LayerStack::elevation).
      if (model.data(index, LayerTreeModel::IsCollectionRole).toBool()) {
        visitChildren(model, index, Inherited{0.0, -1, false});
      } else if (model.layerDefinition(index).type == LayerType::Elevation) {
        stack.elevation.push_back(ElevationStackEntry{
            model.data(index, LayerTreeModel::NodeIdRole).toString(),
            model.layerDefinition(index), false});
      }
      return;
    }
    const int group = inherited.group;
    const double alpha =
        inherited.alpha *
        model.data(index, LayerTreeModel::OpacityRole).toDouble();
    if (model.data(index, LayerTreeModel::IsCollectionRole).toBool()) {
      if (group < 0 &&
          model.data(index, LayerTreeModel::CompositeAsGroupRole).toBool()) {
        // Children blend among themselves at full strength; the group
        // fades as a whole. Nested groups inside multiply.
        stack.groups.push_back(StackGroup{
            model.data(index, LayerTreeModel::NodeIdRole).toString(), alpha});
        visitChildren(
            model, index,
            Inherited{1.0, static_cast<int>(stack.groups.size()) - 1, true});
      } else {
        visitChildren(model, index, Inherited{alpha, group, true});
      }
      return;
    }
    const LayerDefinition definition = model.layerDefinition(index);
    const QString id = model.data(index, LayerTreeModel::NodeIdRole).toString();
    if (definition.type == LayerType::Imagery) {
      stack.imagery.push_back(ImageryStackEntry{id, definition, alpha, group});
    } else if (definition.type == LayerType::Vector) {
      // Vectors go above all imagery; a group's opacity still applies.
      const double vectorAlpha =
          group >= 0
              ? alpha * stack.groups[static_cast<std::size_t>(group)].alpha
              : alpha;
      stack.vectors.push_back(VectorStackEntry{id, definition, vectorAlpha});
    } else if (definition.type == LayerType::Elevation) {
      stack.elevation.push_back(ElevationStackEntry{id, definition, true});
    }
  }
};

} // namespace

LayerDefinition LayerTreeModel::layerDefinition(const QModelIndex& index) const
{
  return index.isValid() ? nodeAt(index)->definition : LayerDefinition();
}

LayerStack LayerTreeModel::layerStack() const
{
  StackBuilder builder;
  builder.visitChildren(*this, QModelIndex(), Inherited());
  // Drop groups that ended up empty, renumbering the rest.
  std::vector<int> used(builder.stack.groups.size(), 0);
  for (const ImageryStackEntry& entry : builder.stack.imagery) {
    if (entry.group >= 0) {
      used[static_cast<std::size_t>(entry.group)] = 1;
    }
  }
  std::vector<int> renumber(used.size(), -1);
  std::vector<StackGroup> groups;
  for (std::size_t i = 0; i < used.size(); ++i) {
    if (used[i] != 0) {
      renumber[i] = static_cast<int>(groups.size());
      groups.push_back(builder.stack.groups[i]);
    }
  }
  for (ImageryStackEntry& entry : builder.stack.imagery) {
    if (entry.group >= 0) {
      entry.group = renumber[static_cast<std::size_t>(entry.group)];
    }
  }
  builder.stack.groups = std::move(groups);
  return builder.stack;
}

// --- JSON
// -----------------------------------------------------------------------

namespace
{

QJsonArray nodesToJson(const LayerTreeModel& model, const QModelIndex& parent)
{
  QJsonArray array;
  for (int row = 0; row < model.rowCount(parent); ++row) {
    const QModelIndex index = model.index(row, 0, parent);
    const bool collection =
        model.data(index, LayerTreeModel::IsCollectionRole).toBool();
    QJsonObject json;
    json.insert(QStringLiteral("kind"), nodeKindName(collection));
    json.insert(QStringLiteral("id"),
                model.data(index, LayerTreeModel::NodeIdRole).toString());
    json.insert(QStringLiteral("name"), model.data(index).toString());
    json.insert(QStringLiteral("visible"),
                model.data(index, Qt::CheckStateRole).toInt() == Qt::Checked);
    json.insert(QStringLiteral("opacity"),
                model.data(index, LayerTreeModel::OpacityRole).toDouble());
    if (collection) {
      json.insert(
          QStringLiteral("compositeAsGroup"),
          model.data(index, LayerTreeModel::CompositeAsGroupRole).toBool());
      json.insert(QStringLiteral("children"), nodesToJson(model, index));
    } else {
      json.insert(QStringLiteral("definition"),
                  model.layerDefinition(index).toJson());
    }
    array.append(json);
  }
  return array;
}

} // namespace

QJsonArray LayerTreeModel::toJson() const
{
  return nodesToJson(*this, QModelIndex());
}

namespace
{

/** Builds nodes from JSON into @p parent; false and @p error on failure. */
template <typename NodeType>
bool nodesFromJson(const QJsonArray& array, NodeType& parent, QString* error)
{
  // An index loop, as in builtinLayerDefinitions(): no range-for spelling
  // suits QJsonArray on both Qt 5 and Qt 6.
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int i = 0; i < array.size(); ++i) {
    const QJsonObject json = array.at(i).toObject();
    std::unique_ptr<NodeType> node = std::make_unique<NodeType>();
    const QString kind = json.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("collection")) {
      node->kind = NodeType::Kind::Collection;
    } else if (kind == QLatin1String("layer")) {
      node->kind = NodeType::Kind::Layer;
    } else {
      if (error) {
        *error = QCoreApplication::translate("ts::LayerTreeModel",
                                             "Unknown layer tree entry \"%1\".")
                     .arg(kind);
      }
      return false;
    }
    const QString id = json.value(QStringLiteral("id")).toString();
    if (!id.isEmpty()) {
      node->id = id;
    }
    node->name = json.value(QStringLiteral("name")).toString();
    node->visible = json.value(QStringLiteral("visible")).toBool(true);
    node->opacity = std::clamp(
        json.value(QStringLiteral("opacity")).toDouble(1.0), 0.0, 1.0);
    node->compositeAsGroup =
        json.value(QStringLiteral("compositeAsGroup")).toBool(false);
    if (node->kind == NodeType::Kind::Layer) {
      std::optional<LayerDefinition> definition = LayerDefinition::fromJson(
          json.value(QStringLiteral("definition")).toObject(), error);
      if (!definition) {
        return false;
      }
      node->definition = std::move(*definition);
    } else if (!nodesFromJson(json.value(QStringLiteral("children")).toArray(),
                              *node, error)) {
      return false;
    }
    node->parent = &parent;
    parent.children.push_back(std::move(node));
  }
  return true;
}

} // namespace

bool LayerTreeModel::fromJson(const QJsonArray& json, QString* errorString)
{
  std::unique_ptr<Node> root = std::make_unique<Node>();
  root->kind = Node::Kind::Collection;
  if (!nodesFromJson(json, *root, errorString)) {
    return false;
  }
  beginResetModel();
  m_root = std::move(root);
  endResetModel();
  emit layerStackChanged();
  return true;
}

} // namespace ts
