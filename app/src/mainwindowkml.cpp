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
// The main window's KML editing: new and editable documents, drawing
// elements, selecting and dragging their points, properties, the
// clipboard, undo and redo, saving, and KML in projects. The rest of the
// window is in mainwindow.cpp.

#include <algorithm>
#include <cstdint>
#include <set>
#include <utility>

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QStatusBar>
#include <QTimer>
#include <QUuid>

#include "globeview.h"
#include "kmlexport.h"
#include "kmlpropertiesdialog.h"
#include "layertreedock.h"
#include "mainwindow.h"
#include "ts/kml/kmlfeatureprovider.h"
#include "ts/kml/kmlreader.h"
#include "ts/kml/kmlwriter.h"
#include "ts/layers/layertreemodel.h"
#include "ts/providers/providerregistry.h"

namespace ts
{

namespace
{

/** What is being drawn for KML (MainWindow::m_kmlDrawing). */
enum KmlDrawing : int32_t {
  NotDrawing = 0,
  DrawingPlacemark,
  DrawingPath,
  DrawingPolygon,
  DrawingHole,
  DrawingOverlay
};

const QString kmlMimeType =
    QStringLiteral("application/vnd.google-earth.kml+xml");

std::vector<KmlCoordinate> closedRing(const std::vector<LonLat>& corners)
{
  std::vector<KmlCoordinate> ring;
  for (const LonLat& corner : corners) {
    ring.push_back(KmlCoordinate{corner.lonDeg, corner.latDeg, 0.0});
  }
  if (!ring.empty() && !(ring.front() == ring.back())) {
    ring.push_back(ring.front());
  }
  return ring;
}

bool isContainer(const KmlFeature* feature)
{
  return feature != nullptr && (feature->kind == KmlFeature::Kind::Document ||
                                feature->kind == KmlFeature::Kind::Folder);
}

/** A drawn element's style: the program's yellow, lines 3 pixels wide. */
KmlStyle drawnStyle()
{
  const QColor yellow(255, 200, 40);
  KmlStyle style;
  KmlLineStyle line;
  line.color = yellow;
  line.width = 3.0;
  style.line = line;
  KmlPolyStyle poly;
  QColor fill = yellow;
  fill.setAlpha(96);
  poly.color = fill;
  style.poly = poly;
  return style;
}

QAction* newAction(QObject* parent, const char* name)
{
  QAction* action = new QAction(parent);
  action->setObjectName(QString::fromLatin1(name));
  return action;
}

/** @p layers (LayerTreeModel::toJson()) with no children under @p ids. */
QJsonArray withoutChildren(const QJsonArray& layers,
                           const std::set<QString>& ids)
{
  QJsonArray out;
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < layers.size(); ++i) {
    QJsonObject node = layers.at(i).toObject();
    if (ids.count(node.value(QStringLiteral("id")).toString()) != 0) {
      node.insert(QStringLiteral("children"), QJsonArray());
    } else if (node.contains(QStringLiteral("children"))) {
      node.insert(QStringLiteral("children"),
                  withoutChildren(
                      node.value(QStringLiteral("children")).toArray(), ids));
    }
    out.append(node);
  }
  return out;
}

} // namespace

void MainWindow::createKmlEditing()
{
  m_newKmlAction = newAction(this, "newKmlAction");
  connect(m_newKmlAction, &QAction::triggered, this,
          [this]() { newKmlDocument(); });
  m_editKmlAction = newAction(this, "editKmlAction");
  connect(m_editKmlAction, &QAction::triggered, this, [this]() {
    const QString id =
        m_layers->data(m_layerDock->currentIndex(), LayerTreeModel::NodeIdRole)
            .toString();
    if (editKmlDocument(id).isEmpty()) {
      statusBar()->showMessage(tr("Select a KML entry to edit its file."),
                               10000);
    }
  });
  const std::vector<std::pair<QAction**, std::pair<const char*, int32_t>>>
      drawing{{&m_addPlacemarkAction, {"addPlacemarkAction", DrawingPlacemark}},
              {&m_addPathAction, {"addPathAction", DrawingPath}},
              {&m_addPolygonAction, {"addPolygonAction", DrawingPolygon}},
              {&m_addHoleAction, {"addHoleAction", DrawingHole}},
              {&m_addOverlayAction, {"addOverlayAction", DrawingOverlay}}};
  for (const std::pair<QAction**, std::pair<const char*, int32_t>>& draw :
       drawing) {
    *draw.first = newAction(this, draw.second.first);
    const int32_t kind = draw.second.second;
    connect(*draw.first, &QAction::triggered, this,
            [this, kind]() { startKmlDrawing(kind); });
  }
  m_addFolderAction = newAction(this, "addFolderAction");
  connect(m_addFolderAction, &QAction::triggered, this,
          [this]() { addKmlFolder(); });
  m_kmlPropertiesAction = newAction(this, "kmlPropertiesAction");
  m_kmlPropertiesAction->setShortcut(QKeySequence(Qt::ALT | Qt::Key_Return));
  connect(m_kmlPropertiesAction, &QAction::triggered, this,
          &MainWindow::showKmlProperties);
  m_kmlCutAction = newAction(this, "kmlCutAction");
  m_kmlCutAction->setShortcut(QKeySequence::Cut);
  connect(m_kmlCutAction, &QAction::triggered, this, [this]() { cutKml(); });
  m_kmlCopyAction = newAction(this, "kmlCopyAction");
  m_kmlCopyAction->setShortcut(QKeySequence::Copy);
  connect(m_kmlCopyAction, &QAction::triggered, this, [this]() { copyKml(); });
  m_kmlPasteAction = newAction(this, "kmlPasteAction");
  m_kmlPasteAction->setShortcut(QKeySequence::Paste);
  connect(m_kmlPasteAction, &QAction::triggered, this,
          [this]() { pasteKml(); });
  m_kmlDeleteAction = newAction(this, "kmlDeleteAction");
  m_kmlDeleteAction->setShortcut(QKeySequence::Delete);
  connect(m_kmlDeleteAction, &QAction::triggered, this,
          [this]() { deleteKml(); });
  m_kmlUndoAction = newAction(this, "kmlUndoAction");
  m_kmlUndoAction->setShortcut(QKeySequence::Undo);
  connect(m_kmlUndoAction, &QAction::triggered, this, [this]() { undoKml(); });
  m_kmlRedoAction = newAction(this, "kmlRedoAction");
  m_kmlRedoAction->setShortcut(QKeySequence::Redo);
  connect(m_kmlRedoAction, &QAction::triggered, this, [this]() { redoKml(); });
  m_kmlSaveAction = newAction(this, "kmlSaveAction");
  connect(m_kmlSaveAction, &QAction::triggered, this,
          [this]() { saveKmlActive(false); });
  m_kmlSaveAsAction = newAction(this, "kmlSaveAsAction");
  connect(m_kmlSaveAsAction, &QAction::triggered, this,
          [this]() { saveKmlActive(true); });

  m_kmlMenu = new QMenu(this);
  m_kmlMenu->setObjectName(QStringLiteral("kmlMenu"));
  menuBar()->insertMenu(m_helpMenu->menuAction(), m_kmlMenu);
  m_kmlMenu->addAction(m_newKmlAction);
  m_kmlMenu->addAction(m_editKmlAction);
  m_kmlMenu->addSeparator();
  m_kmlMenu->addAction(m_addPlacemarkAction);
  m_kmlMenu->addAction(m_addPathAction);
  m_kmlMenu->addAction(m_addPolygonAction);
  m_kmlMenu->addAction(m_addHoleAction);
  m_kmlMenu->addAction(m_addOverlayAction);
  m_kmlMenu->addAction(m_addFolderAction);
  m_kmlMenu->addSeparator();
  m_kmlMenu->addAction(m_kmlPropertiesAction);
  m_kmlMenu->addAction(m_kmlCutAction);
  m_kmlMenu->addAction(m_kmlCopyAction);
  m_kmlMenu->addAction(m_kmlPasteAction);
  m_kmlMenu->addAction(m_kmlDeleteAction);
  m_kmlMenu->addSeparator();
  m_kmlMenu->addAction(m_kmlUndoAction);
  m_kmlMenu->addAction(m_kmlRedoAction);
  m_kmlMenu->addSeparator();
  m_kmlMenu->addAction(m_kmlSaveAction);
  m_kmlMenu->addAction(m_kmlSaveAsAction);
  connect(m_kmlMenu, &QMenu::aboutToShow, this, &MainWindow::updateKmlActions);

  if (m_globeView != nullptr) {
    connect(m_globeView, &GlobeView::handleMoved, this,
            &MainWindow::kmlHandleMoved);
    connect(m_globeView, &GlobeView::handleDeleteRequested, this,
            &MainWindow::kmlHandleDeleted);
    connect(m_globeView, &GlobeView::lineDrawn, this,
            &MainWindow::kmlLineDrawn);
    connect(m_globeView, &GlobeView::regionDrawn, this,
            &MainWindow::kmlRegionDrawn);
    connect(m_globeView, &GlobeView::regionDrawingCancelled, this,
            [this]() { m_kmlDrawing = NotDrawing; });
  }
  connect(m_layers, &QAbstractItemModel::rowsMoved, this,
          &MainWindow::kmlNodesMoved);
  updateKmlActions();
}

void MainWindow::retranslateKmlEditing()
{
  m_kmlMenu->setTitle(tr("&KML"));
  m_newKmlAction->setText(tr("&New KML Document"));
  m_newKmlAction->setStatusTip(
      tr("Start a KML document of your own to draw places, paths and areas"));
  m_editKmlAction->setText(tr("&Edit This Document"));
  m_editKmlAction->setStatusTip(
      tr("Make the KML document of the selected entry editable"));
  m_addPlacemarkAction->setText(tr("Add &Placemark"));
  m_addPlacemarkAction->setStatusTip(tr("Click where the placemark goes"));
  m_addPathAction->setText(tr("Add Pa&th"));
  m_addPathAction->setStatusTip(
      tr("Click each point of the path; double-click or Enter to finish"));
  m_addPolygonAction->setText(tr("Add Pol&ygon"));
  m_addPolygonAction->setStatusTip(
      tr("Click each corner; double-click or Enter to finish"));
  m_addHoleAction->setText(tr("Add &Hole to Polygon"));
  m_addHoleAction->setStatusTip(
      tr("Draw a hole in the selected polygon, corner by corner"));
  m_addOverlayAction->setText(tr("Add &Ground Overlay…"));
  m_addOverlayAction->setStatusTip(
      tr("Choose a picture, then drag the rectangle it covers"));
  m_addFolderAction->setText(tr("Add &Folder"));
  m_kmlPropertiesAction->setText(tr("P&roperties…"));
  m_kmlCutAction->setText(tr("Cu&t"));
  m_kmlCopyAction->setText(tr("&Copy"));
  m_kmlPasteAction->setText(tr("Pa&ste"));
  m_kmlDeleteAction->setText(tr("&Delete"));
  m_kmlSaveAction->setText(tr("Sa&ve KML Document"));
  m_kmlSaveAsAction->setText(tr("Save KML Document &As…"));
  updateKmlActions();
}

void MainWindow::updateKmlActions()
{
  const QString key = activeKmlDocument();
  const KmlEditDocument* document = kmlEditDocument(key);
  const bool editing = document != nullptr;
  for (QAction* action :
       {m_addPlacemarkAction, m_addPathAction, m_addPolygonAction,
        m_addOverlayAction, m_addFolderAction, m_kmlSaveAction,
        m_kmlSaveAsAction}) {
    action->setEnabled(editing && m_globeView != nullptr);
  }
  m_addFolderAction->setEnabled(editing);
  m_kmlSaveAction->setEnabled(editing);
  m_kmlSaveAsAction->setEnabled(editing);
  const bool selected = m_kmlSelection.has_value();
  const KmlFeature* feature =
      selected
          ? kmlFeatureAt(kmlEditDocument(m_kmlSelection->first)->document(),
                         m_kmlSelection->second)
          : nullptr;
  m_addHoleAction->setEnabled(feature != nullptr && feature->geometry &&
                              feature->geometry->kind ==
                                  KmlGeometry::Kind::Polygon);
  for (QAction* action : {m_kmlPropertiesAction, m_kmlCutAction,
                          m_kmlCopyAction, m_kmlDeleteAction}) {
    action->setEnabled(selected);
  }
  m_kmlPasteAction->setEnabled(editing && m_kmlClipboard.has_value());
  m_kmlUndoAction->setEnabled(editing && document->canUndo());
  m_kmlRedoAction->setEnabled(editing && document->canRedo());
  m_kmlUndoAction->setText(editing && document->canUndo()
                               ? tr("&Undo %1").arg(document->undoText())
                               : tr("&Undo"));
  m_kmlRedoAction->setText(editing && document->canRedo()
                               ? tr("&Redo %1").arg(document->redoText())
                               : tr("&Redo"));
}

KmlEditDocument* MainWindow::kmlEditDocument(const QString& key) const
{
  const std::map<QString, std::unique_ptr<KmlEditDocument>>::const_iterator
      found = m_kmlEdits.find(key);
  return found != m_kmlEdits.end() ? found->second.get() : nullptr;
}

QString MainWindow::kmlKeyOf(const QString& nodeId) const
{
  const std::map<QString, std::pair<QString, QString>>::const_iterator found =
      m_kmlNodes.find(nodeId);
  if (found == m_kmlNodes.end() || m_kmlEdits.count(found->second.first) == 0) {
    return QString();
  }
  return found->second.first;
}

QString MainWindow::activeKmlDocument() const
{
  const QString selected = kmlKeyOf(
      m_layers->data(m_layerDock->currentIndex(), LayerTreeModel::NodeIdRole)
          .toString());
  if (!selected.isEmpty()) {
    return selected;
  }
  if (m_kmlEdits.count(m_kmlActive) != 0) {
    return m_kmlActive;
  }
  return m_kmlEdits.empty() ? QString() : m_kmlEdits.begin()->first;
}

QString MainWindow::kmlTargetFolder(const QString& key) const
{
  const KmlEditDocument* document = kmlEditDocument(key);
  if (document == nullptr) {
    return QString();
  }
  // The selected entry's folder, else the selected feature's, else the
  // document itself.
  QString path;
  const QString id =
      m_layers->data(m_layerDock->currentIndex(), LayerTreeModel::NodeIdRole)
          .toString();
  if (kmlKeyOf(id) == key) {
    path = m_kmlNodes.at(id).second;
  } else if (m_kmlSelection && m_kmlSelection->first == key) {
    path = m_kmlSelection->second;
  }
  while (!path.isEmpty() &&
         !isContainer(kmlFeatureAt(document->document(), path))) {
    path = kmlParentPath(path);
  }
  return path;
}

QJsonObject MainWindow::withKmlRevision(QJsonObject source) const
{
  // A document being edited changes: its layers' sources change with it,
  // so their providers (and tiles) are made again.
  const KmlEditDocument* document =
      kmlEditDocument(source.value(QStringLiteral("path")).toString());
  if (document != nullptr) {
    source.insert(QStringLiteral("rev"),
                  static_cast<double>(document->revision()));
  }
  return source;
}

QString MainWindow::newKmlDocument()
{
  KmlDocument document;
  document.root.kind = KmlFeature::Kind::Document;
  document.root.name = tr("Untitled");
  document.root.open = true;
  const QString key = QStringLiteral("kmledit:") +
                      QUuid::createUuid().toString(QUuid::WithoutBraces);
  m_kmlEdits[key] = std::make_unique<KmlEditDocument>(document, key, QString());
  const std::shared_ptr<const KmlDocument> shared = m_kmlEdits[key]->shared();
  addKmlFeature(*shared, shared->root, QString(), QModelIndex(), 0);
  const QModelIndex root = m_layers->index(0, 0);
  m_kmlRoots[m_layers->data(root, LayerTreeModel::NodeIdRole).toString()] = key;
  m_kmlActive = key;
  m_layerDock->setCurrentIndex(root);
  updateKmlActions();
  return key;
}

QString MainWindow::editKmlDocument(const QString& nodeId)
{
  // The document's own entry: the top of the entries of its file.
  QModelIndex root = m_layers->indexOfId(nodeId);
  QString rootId;
  for (QModelIndex up = root; up.isValid(); up = up.parent()) {
    const QString id =
        m_layers->data(up, LayerTreeModel::NodeIdRole).toString();
    if (m_kmlRoots.count(id) != 0) {
      root = up;
      rootId = id;
    }
  }
  if (rootId.isEmpty()) {
    return QString();
  }
  const QString path = m_kmlRoots.at(rootId);
  if (m_kmlEdits.count(path) != 0) {
    return path;
  }
  KmlReadOptions options;
  options.keepSource = true;
  QString error;
  std::optional<KmlDocument> read = readKmlFile(path, &error, options);
  if (!read) {
    QMessageBox::warning(this, tr("Edit KML"), error);
    return QString();
  }
  const std::shared_ptr<const KmlDocument> shown =
      sharedKmlDocument(path, nullptr);
  read->webBase = shown ? shown->webBase : QString();
  const QString key = QStringLiteral("kmledit:") +
                      QUuid::createUuid().toString(QUuid::WithoutBraces);
  m_kmlEdits[key] = std::make_unique<KmlEditDocument>(*read, key, path);
  // Its entries again, now from the document being edited.
  const QModelIndex parent = root.parent();
  const int32_t row = root.row();
  m_refreshingKml = true;
  m_layers->removeNode(root);
  forgetRemovedKmlNodes();
  m_kmlRoots.erase(rootId);
  const std::shared_ptr<const KmlDocument> shared = m_kmlEdits[key]->shared();
  addKmlFeature(*shared, shared->root, QString(), parent, row);
  m_refreshingKml = false;
  const QModelIndex edited = m_layers->index(row, 0, parent);
  m_kmlRoots[m_layers->data(edited, LayerTreeModel::NodeIdRole).toString()] =
      key;
  m_kmlActive = key;
  m_layerDock->setCurrentIndex(edited);
  updateKmlActions();
  return key;
}

void MainWindow::refreshKmlDocument(const QString& key)
{
  const KmlEditDocument* document = kmlEditDocument(key);
  if (document == nullptr) {
    return;
  }
  const std::shared_ptr<const KmlDocument> shared = document->shared();
  const bool wasRefreshing = m_refreshingKml;
  m_refreshingKml = true;
  for (const std::pair<const QString, QString>& root :
       std::map<QString, QString>(m_kmlRoots)) {
    if (root.second != key) {
      continue;
    }
    const QModelIndex index = m_layers->indexOfId(root.first);
    if (!index.isValid()) {
      continue;
    }
    // Its entries made again from the document as it is now.
    while (m_layers->rowCount(index) > 0) {
      m_layers->removeNode(
          m_layers->index(m_layers->rowCount(index) - 1, 0, index));
    }
    forgetRemovedKmlNodes();
    m_layers->setData(
        index, shared->root.name.isEmpty() ? tr("Untitled") : shared->root.name,
        Qt::EditRole);
    if (isContainer(&shared->root)) {
      addKmlChildren(*shared, shared->root, QString(), index);
    } else if (shared->root.kind == KmlFeature::Kind::Placemark) {
      addKmlPlaces(*shared, QString(), index, 0, false);
    }
  }
  m_refreshingKml = wasRefreshing;
  // The selection, if it is still there.
  if (m_kmlSelection && m_kmlSelection->first == key &&
      kmlFeatureAt(*shared, m_kmlSelection->second) == nullptr) {
    m_kmlSelection.reset();
  }
  showKmlHandles();
  updateKmlActions();
}

void MainWindow::selectKmlFeature(const QString& key, const QString& path)
{
  if (key.isEmpty() || kmlEditDocument(key) == nullptr) {
    m_kmlSelection.reset();
  } else {
    m_kmlSelection = std::make_pair(key, path);
    m_kmlActive = key;
  }
  showKmlHandles();
  updateKmlActions();
}

std::optional<QString> MainWindow::selectedKmlFeature() const
{
  return m_kmlSelection ? std::optional<QString>(m_kmlSelection->second)
                        : std::nullopt;
}

void MainWindow::showKmlHandles()
{
  m_kmlHandles.clear();
  const KmlEditDocument* document =
      m_kmlSelection ? kmlEditDocument(m_kmlSelection->first) : nullptr;
  const KmlFeature* feature =
      document != nullptr
          ? kmlFeatureAt(document->document(), m_kmlSelection->second)
          : nullptr;
  if (feature != nullptr) {
    m_kmlHandles = kmlHandles(*feature);
  }
  if (m_globeView == nullptr) {
    return;
  }
  std::vector<EditHandleDraw> draws;
  for (const KmlHandle& handle : m_kmlHandles) {
    draws.push_back(
        EditHandleDraw{LonLat{handle.lonDeg, handle.latDeg}, handle.middle});
  }
  m_globeView->setEditHandles(std::move(draws));
}

bool MainWindow::addKmlFeatureToActive(const QString& description,
                                       KmlFeature feature)
{
  const QString key = activeKmlDocument();
  KmlEditDocument* document = kmlEditDocument(key);
  if (document == nullptr) {
    return false;
  }
  const QString folder = kmlTargetFolder(key);
  QString added;
  const bool done = document->edit(description, [&](KmlDocument& changed) {
    added = kmlInsertFeature(changed, folder, std::move(feature));
    return !added.isEmpty();
  });
  if (!done) {
    return false;
  }
  m_kmlActive = key;
  refreshKmlDocument(key);
  selectKmlFeature(key, added);
  return true;
}

bool MainWindow::addKmlPlacemark(LonLat at)
{
  KmlFeature placemark;
  placemark.name = tr("Placemark");
  KmlGeometry point;
  point.coordinates = {KmlCoordinate{at.lonDeg, at.latDeg, 0.0}};
  placemark.geometry = point;
  return addKmlFeatureToActive(tr("Add Placemark"), placemark);
}

bool MainWindow::addKmlPath(const std::vector<LonLat>& points)
{
  if (points.size() < 2) {
    return false;
  }
  KmlFeature path;
  path.name = tr("Path");
  path.style = drawnStyle();
  KmlGeometry line;
  line.kind = KmlGeometry::Kind::LineString;
  line.tessellate = true;
  for (const LonLat& point : points) {
    line.coordinates.push_back(KmlCoordinate{point.lonDeg, point.latDeg, 0.0});
  }
  path.geometry = line;
  return addKmlFeatureToActive(tr("Add Path"), path);
}

bool MainWindow::addKmlPolygon(const std::vector<LonLat>& corners)
{
  if (corners.size() < 3) {
    return false;
  }
  KmlFeature polygon;
  polygon.name = tr("Polygon");
  polygon.style = drawnStyle();
  KmlGeometry area;
  area.kind = KmlGeometry::Kind::Polygon;
  area.tessellate = true;
  area.coordinates = closedRing(corners);
  polygon.geometry = area;
  return addKmlFeatureToActive(tr("Add Polygon"), polygon);
}

bool MainWindow::addKmlHole(const std::vector<LonLat>& corners)
{
  if (!m_kmlSelection || corners.size() < 3) {
    return false;
  }
  const QString key = m_kmlSelection->first;
  const QString path = m_kmlSelection->second;
  KmlEditDocument* document = kmlEditDocument(key);
  if (document == nullptr) {
    return false;
  }
  const bool done = document->edit(tr("Add Hole"), [&](KmlDocument& changed) {
    KmlFeature* polygon = kmlFeatureAt(changed, path);
    if (polygon == nullptr || !polygon->geometry ||
        polygon->geometry->kind != KmlGeometry::Kind::Polygon) {
      return false;
    }
    polygon->geometry->inner.push_back(closedRing(corners));
    return true;
  });
  if (done) {
    refreshKmlDocument(key);
  }
  return done;
}

bool MainWindow::addKmlGroundOverlay(const QString& image,
                                     const RegionGeometry& area)
{
  if (image.isEmpty() || !area.isValid()) {
    return false;
  }
  KmlFeature overlay;
  overlay.kind = KmlFeature::Kind::GroundOverlay;
  overlay.name = QFileInfo(image).completeBaseName();
  overlay.iconHref = QFileInfo(image).absoluteFilePath();
  KmlLatLonBox box;
  const GeoRect& extent = area.extent();
  box.west = extent.westDeg;
  box.east = extent.eastDeg > 180.0 ? extent.eastDeg - 360.0 : extent.eastDeg;
  box.south = extent.southDeg;
  box.north = extent.northDeg;
  overlay.latLonBox = box;
  return addKmlFeatureToActive(tr("Add Ground Overlay"), overlay);
}

bool MainWindow::addKmlFolder()
{
  KmlFeature folder;
  folder.kind = KmlFeature::Kind::Folder;
  folder.name = tr("Folder");
  folder.open = true;
  return addKmlFeatureToActive(tr("Add Folder"), folder);
}

bool MainWindow::setKmlFeature(const QString& key, const QString& path,
                               const KmlFeature& feature,
                               const std::optional<KmlStyle>& shared)
{
  KmlEditDocument* document = kmlEditDocument(key);
  if (document == nullptr) {
    return false;
  }
  const bool done =
      document->edit(tr("Change Properties"), [&](KmlDocument& changed) {
        KmlFeature* target = kmlFeatureAt(changed, path);
        if (target == nullptr) {
          return false;
        }
        KmlFeature updated = feature;
        updated.children = target->children;
        *target = std::move(updated);
        if (shared) {
          for (KmlStyle& style : changed.root.styles) {
            if (style.id == shared->id) {
              style = *shared;
            }
          }
        }
        return true;
      });
  if (done) {
    refreshKmlDocument(key);
  }
  return done;
}

bool MainWindow::undoKml()
{
  const QString key = activeKmlDocument();
  KmlEditDocument* document = kmlEditDocument(key);
  if (document == nullptr || !document->undo()) {
    return false;
  }
  refreshKmlDocument(key);
  return true;
}

bool MainWindow::redoKml()
{
  const QString key = activeKmlDocument();
  KmlEditDocument* document = kmlEditDocument(key);
  if (document == nullptr || !document->redo()) {
    return false;
  }
  refreshKmlDocument(key);
  return true;
}

bool MainWindow::copyKml()
{
  const KmlEditDocument* document =
      m_kmlSelection ? kmlEditDocument(m_kmlSelection->first) : nullptr;
  if (document == nullptr) {
    return false;
  }
  // The feature with the shared styles it uses; on the clipboard as KML
  // too, for other programs.
  m_kmlClipboard = kmlExtract(*document->shared(), m_kmlSelection->second);
  QMimeData* data = new QMimeData;
  const QByteArray kml = writeKml(*m_kmlClipboard);
  data->setData(kmlMimeType, kml);
  data->setText(QString::fromUtf8(kml));
  QApplication::clipboard()->setMimeData(data);
  updateKmlActions();
  return true;
}

bool MainWindow::cutKml()
{
  return copyKml() && deleteKml();
}

bool MainWindow::deleteKml()
{
  if (!m_kmlSelection) {
    return false;
  }
  const QString key = m_kmlSelection->first;
  const QString path = m_kmlSelection->second;
  KmlEditDocument* document = kmlEditDocument(key);
  if (document == nullptr || path.isEmpty()) {
    return false;
  }
  const bool done = document->edit(tr("Delete"), [&](KmlDocument& changed) {
    return kmlTakeFeature(changed, path).has_value();
  });
  if (done) {
    m_kmlSelection.reset();
    refreshKmlDocument(key);
  }
  return done;
}

bool MainWindow::pasteKml()
{
  const QString key = activeKmlDocument();
  KmlEditDocument* document = kmlEditDocument(key);
  if (document == nullptr) {
    return false;
  }
  // What was copied here, else KML on the clipboard from elsewhere.
  std::optional<KmlDocument> copied = m_kmlClipboard;
  const QMimeData* data = QApplication::clipboard()->mimeData();
  if (!copied && data != nullptr) {
    const QByteArray kml = data->hasFormat(kmlMimeType)
                               ? data->data(kmlMimeType)
                               : data->text().toUtf8();
    QString error;
    copied = parseKml(kml, QString(), &error);
  }
  if (!copied) {
    return false;
  }
  const QString folder = kmlTargetFolder(key);
  QString first;
  const bool done = document->edit(tr("Paste"), [&](KmlDocument& changed) {
    std::vector<KmlFeature> features =
        isContainer(&copied->root) ? copied->root.children
                                   : std::vector<KmlFeature>{copied->root};
    for (KmlFeature& feature : features) {
      const QString added =
          kmlInsertFeature(changed, folder, kmlWithoutSource(feature));
      first = first.isEmpty() ? added : first;
    }
    // The shared styles they use, if the document hasn't them.
    if (changed.root.kind == KmlFeature::Kind::Document) {
      for (const KmlStyle& style : copied->root.styles) {
        if (changed.findStyle(style.id) == nullptr) {
          changed.root.styles.push_back(style);
        }
      }
      for (const KmlStyleMap& map : copied->root.styleMaps) {
        if (changed.findStyleMap(map.id) == nullptr) {
          changed.root.styleMaps.push_back(map);
        }
      }
    }
    return !first.isEmpty();
  });
  if (done) {
    refreshKmlDocument(key);
    selectKmlFeature(key, first);
  }
  return done;
}

bool MainWindow::saveKmlDocument(const QString& key, const QString& path)
{
  KmlEditDocument* document = kmlEditDocument(key);
  if (document == nullptr) {
    return false;
  }
  if (!saveKmlTo(this, document->document(), path, tr("Save KML Document"))) {
    return false;
  }
  document->setSaved(path);
  updateKmlActions();
  return true;
}

bool MainWindow::saveKmlActive(bool as)
{
  const QString key = activeKmlDocument();
  KmlEditDocument* document = kmlEditDocument(key);
  if (document == nullptr) {
    return false;
  }
  QString path = document->filePath();
  if (as || path.isEmpty()) {
    path = QFileDialog::getSaveFileName(
        this, tr("Save KML Document"),
        path.isEmpty() ? tr("%1.kml").arg(document->document().root.name)
                       : path,
        tr("KML (*.kml);;KMZ with its pictures (*.kmz)"));
    if (path.isEmpty()) {
      return false;
    }
    if (!path.endsWith(QLatin1String(".kml"), Qt::CaseInsensitive) &&
        !path.endsWith(QLatin1String(".kmz"), Qt::CaseInsensitive)) {
      path += QStringLiteral(".kml");
    }
  }
  return saveKmlDocument(key, path);
}

void MainWindow::startKmlDrawing(int32_t kind)
{
  if (m_globeView == nullptr) {
    return;
  }
  if (activeKmlDocument().isEmpty()) {
    newKmlDocument();
  }
  m_kmlDrawing = kind;
  switch (kind) {
  case DrawingPlacemark:
    statusBar()->showMessage(tr("Click where the placemark goes."), 10000);
    break;
  case DrawingPath:
    m_globeView->startRegionDrawing(GlobeView::RegionDrawing::Line);
    break;
  case DrawingPolygon:
  case DrawingHole:
    m_globeView->startRegionDrawing(GlobeView::RegionDrawing::Polygon);
    break;
  case DrawingOverlay: {
    m_kmlOverlayImage = QFileDialog::getOpenFileName(
        this, tr("Choose the Picture"), QString(),
        tr("Pictures (*.png *.jpg *.jpeg *.gif *.tif *.tiff);;All files (*)"));
    if (m_kmlOverlayImage.isEmpty()) {
      m_kmlDrawing = NotDrawing;
      return;
    }
    m_globeView->startRegionDrawing(GlobeView::RegionDrawing::Rectangle);
    break;
  }
  default:
    m_kmlDrawing = NotDrawing;
    break;
  }
}

void MainWindow::kmlLineDrawn(const std::vector<LonLat>& points)
{
  if (m_kmlDrawing != DrawingPath) {
    return;
  }
  m_kmlDrawing = NotDrawing;
  addKmlPath(points);
}

void MainWindow::kmlRegionDrawn(const RegionGeometry& region)
{
  const int32_t drawing = m_kmlDrawing;
  m_kmlDrawing = NotDrawing;
  switch (drawing) {
  case DrawingPolygon:
    addKmlPolygon(region.vertices());
    break;
  case DrawingHole:
    addKmlHole(region.vertices());
    break;
  case DrawingOverlay:
    addKmlGroundOverlay(m_kmlOverlayImage, region);
    break;
  default:
    break;
  }
}

bool MainWindow::kmlClicked(LonLat point, double degreesPerPixel)
{
  if (m_kmlDrawing == DrawingPlacemark) {
    m_kmlDrawing = NotDrawing;
    addKmlPlacemark(point);
    return true;
  }
  if (m_kmlEdits.empty()) {
    return false;
  }
  // A placemark of a document being edited: selected.
  const LayerStack stack = m_layers->layerStack();
  for (std::size_t i = stack.vectors.size(); i-- > 0;) {
    const QJsonObject& source = stack.vectors[i].definition.source;
    const QString key = source.value(QStringLiteral("path")).toString();
    if (source.value(QStringLiteral("kind")).toString() !=
            QLatin1String("kml") ||
        kmlEditDocument(key) == nullptr) {
      continue;
    }
    const std::shared_ptr<KmlFeatureProvider> provider =
        std::dynamic_pointer_cast<KmlFeatureProvider>(
            m_registry->features(source, nullptr));
    const std::vector<const KmlFeature*> found =
        provider ? provider->placemarksAt(GeoPoint{point.latDeg, point.lonDeg},
                                          4.0 * degreesPerPixel)
                 : std::vector<const KmlFeature*>{};
    const std::optional<QString> path =
        found.empty() ? std::nullopt
                      : kmlPathOf(*provider->document(), found.front());
    if (path) {
      selectKmlFeature(key, *path);
      return true;
    }
  }
  if (m_kmlSelection) {
    selectKmlFeature(QString(), QString());
  }
  return false;
}

void MainWindow::kmlHandleMoved(int32_t index, LonLat to)
{
  if (!m_kmlSelection || index < 0 ||
      static_cast<std::size_t>(index) >= m_kmlHandles.size()) {
    return;
  }
  const KmlHandle handle = m_kmlHandles[static_cast<std::size_t>(index)];
  const QString key = m_kmlSelection->first;
  const QString path = m_kmlSelection->second;
  KmlEditDocument* document = kmlEditDocument(key);
  if (document == nullptr) {
    return;
  }
  document->edit(handle.middle ? tr("Add Point") : tr("Move Point"),
                 [&](KmlDocument& changed) {
                   KmlFeature* feature = kmlFeatureAt(changed, path);
                   if (feature == nullptr) {
                     return false;
                   }
                   return handle.middle ? kmlInsertVertex(*feature, handle.ring,
                                                          handle.index,
                                                          to.lonDeg, to.latDeg)
                                        : kmlMoveVertex(*feature, handle.ring,
                                                        handle.index, to.lonDeg,
                                                        to.latDeg);
                 });
  refreshKmlDocument(key);
}

void MainWindow::kmlHandleDeleted(int32_t index)
{
  if (!m_kmlSelection || index < 0 ||
      static_cast<std::size_t>(index) >= m_kmlHandles.size()) {
    return;
  }
  const KmlHandle handle = m_kmlHandles[static_cast<std::size_t>(index)];
  const QString key = m_kmlSelection->first;
  const QString path = m_kmlSelection->second;
  KmlEditDocument* document = kmlEditDocument(key);
  if (document == nullptr) {
    return;
  }
  const bool done =
      document->edit(tr("Delete Point"), [&](KmlDocument& changed) {
        KmlFeature* feature = kmlFeatureAt(changed, path);
        return feature != nullptr &&
               kmlDeleteVertex(*feature, handle.ring, handle.index);
      });
  if (done) {
    refreshKmlDocument(key);
  } else {
    statusBar()->showMessage(
        tr("That point can't go: a path keeps two, an area three."), 10000);
  }
}

void MainWindow::showKmlProperties()
{
  if (!m_kmlSelection) {
    return;
  }
  const QString key = m_kmlSelection->first;
  const QString path = m_kmlSelection->second;
  const KmlEditDocument* document = kmlEditDocument(key);
  const KmlFeature* feature =
      document != nullptr ? kmlFeatureAt(document->document(), path) : nullptr;
  if (feature == nullptr) {
    return;
  }
  KmlPropertiesDialog dialog(
      *feature, document->document(),
      [this]() {
        KmlView view;
        if (m_globeView != nullptr) {
          const CameraPose& pose = m_globeView->controller().camera().pose();
          view.latDeg = pose.targetLatDeg;
          view.lonDeg = pose.targetLonDeg;
          view.rangeM = pose.rangeM;
          view.headingDeg = pose.headingDeg;
          view.tiltDeg = pose.tiltDeg;
        }
        return view;
      },
      this);
  if (dialog.exec() == QDialog::Accepted) {
    setKmlFeature(key, path, dialog.feature(), dialog.sharedStyle());
  }
}

void MainWindow::kmlNodesMoved()
{
  // An entry of a document being edited dragged into one of its folders:
  // the feature moves there. The model's indexes for the move may be stale
  // by now, so the entry is found by its place: the one whose folder in
  // the tree isn't its feature's in the document. Afterwards (not while the
  // model tells of the move) the documents' entries are made again, so an
  // entry dragged where it can't go goes back.
  if (m_refreshingKml || m_kmlEdits.empty()) {
    return;
  }
  std::set<QString> touched;
  for (const std::pair<const QString, std::pair<QString, QString>>& node :
       std::map<QString, std::pair<QString, QString>>(m_kmlNodes)) {
    KmlEditDocument* document = kmlEditDocument(node.second.first);
    const QModelIndex index = m_layers->indexOfId(node.first);
    if (document == nullptr || !index.isValid() ||
        m_kmlRoots.count(node.first) != 0) {
      continue;
    }
    const QString path = node.second.second;
    const bool places =
        !m_layers->data(index, LayerTreeModel::IsCollectionRole).toBool() &&
        isContainer(kmlFeatureAt(document->document(), path));
    const QString parentId =
        m_layers->data(index.parent(), LayerTreeModel::NodeIdRole).toString();
    const std::map<QString, std::pair<QString, QString>>::const_iterator
        parent = m_kmlNodes.find(parentId);
    const QString expected = places ? path : kmlParentPath(path);
    if (parent != m_kmlNodes.end() && parent->second.second == expected &&
        parent->second.first == node.second.first) {
      continue; // where it was
    }
    touched.insert(node.second.first);
    if (parent == m_kmlNodes.end() ||
        parent->second.first != node.second.first ||
        !isContainer(
            kmlFeatureAt(document->document(), parent->second.second))) {
      continue; // somewhere it can't go
    }
    const QString folder = parent->second.second;
    document->edit(tr("Move"), [&](KmlDocument& changed) {
      if (!places) {
        return !kmlMoveFeature(changed, path, folder, -1).isEmpty();
      }
      // A Places entry: every placemark of its folder.
      bool moved = false;
      for (;;) {
        const KmlFeature* container = kmlFeatureAt(changed, path);
        const std::vector<KmlFeature>& children = container->children;
        const std::vector<KmlFeature>::const_iterator placemark = std::find_if(
            children.begin(), children.end(), [](const KmlFeature& child) {
              return child.kind == KmlFeature::Kind::Placemark;
            });
        if (placemark == children.end() ||
            kmlMoveFeature(
                changed,
                kmlChildPath(
                    path, static_cast<int32_t>(placemark - children.begin())),
                folder, -1)
                .isEmpty()) {
          return moved;
        }
        moved = true;
      }
    });
    break; // one move at a time
  }
  if (touched.empty()) {
    return;
  }
  // Until then the entries' paths are the document's before: nothing they
  // say (names, ticks) is passed on to it.
  m_refreshingKml = true;
  QTimer::singleShot(0, this, [this, touched]() {
    for (const QString& key : touched) {
      refreshKmlDocument(key);
    }
    m_refreshingKml = false;
  });
}

QJsonArray MainWindow::kmlProjectEntries(bool* cancelled)
{
  QJsonArray entries;
  for (const std::pair<const QString, QString>& root : m_kmlRoots) {
    if (!m_layers->indexOfId(root.first).isValid()) {
      continue;
    }
    QJsonObject entry;
    entry.insert(QStringLiteral("node"), root.first);
    KmlEditDocument* document = kmlEditDocument(root.second);
    if (document == nullptr) {
      entry.insert(QStringLiteral("path"), root.second);
      entries.append(entry);
      continue;
    }
    entry.insert(QStringLiteral("editable"), true);
    bool embed = document->filePath().isEmpty();
    if (!embed && document->isModified()) {
      // Linked to its file: saved there first, or its changes kept in the
      // project.
      const QMessageBox::StandardButton answer = QMessageBox::question(
          this, tr("Save Project"),
          tr("Save the changes to %1 before saving the project? If not, the "
             "project keeps them, and the file stays as it was.")
              .arg(QFileInfo(document->filePath()).fileName()),
          QMessageBox::Save | QMessageBox::No | QMessageBox::Cancel);
      if (answer == QMessageBox::Cancel) {
        *cancelled = true;
        return QJsonArray();
      }
      embed = answer != QMessageBox::Save ||
              !saveKmlDocument(root.second, document->filePath());
    }
    if (embed) {
      entry.insert(QStringLiteral("text"),
                   QString::fromUtf8(writeKml(document->document())));
      if (!document->filePath().isEmpty()) {
        entry.insert(QStringLiteral("file"), document->filePath());
      }
    } else {
      entry.insert(QStringLiteral("path"), document->filePath());
    }
    entries.append(entry);
  }
  return entries;
}

void MainWindow::openKmlProjectEntries(const QJsonArray& entries)
{
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < entries.size(); ++i) {
    const QJsonObject entry = entries.at(i).toObject();
    const QString id = entry.value(QStringLiteral("node")).toString();
    const QModelIndex node = m_layers->indexOfId(id);
    if (!node.isValid()) {
      continue;
    }
    const bool editable = entry.value(QStringLiteral("editable")).toBool();
    const QString file = entry.contains(QStringLiteral("path"))
                             ? entry.value(QStringLiteral("path")).toString()
                             : entry.value(QStringLiteral("file")).toString();
    QString error;
    std::shared_ptr<const KmlDocument> document;
    QString path = file;
    if (editable || entry.contains(QStringLiteral("text"))) {
      KmlReadOptions options;
      options.keepSource = true;
      std::optional<KmlDocument> read =
          entry.contains(QStringLiteral("text"))
              ? parseKml(
                    entry.value(QStringLiteral("text")).toString().toUtf8(),
                    file.isEmpty() ? QString() : QFileInfo(file).absolutePath(),
                    &error, options)
              : readKmlFile(file, &error, options);
      if (read) {
        path = QStringLiteral("kmledit:") +
               QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_kmlEdits[path] = std::make_unique<KmlEditDocument>(*read, path, file);
        // Changes kept in the project are changes to the file.
        if (entry.contains(QStringLiteral("text")) && !file.isEmpty()) {
          m_kmlEdits[path]->adjust([](KmlDocument&) {});
        }
        document = m_kmlEdits[path]->shared();
      }
    } else {
      document = sharedKmlDocument(file, &error);
    }
    if (!document) {
      statusBar()->showMessage(error, 10000);
      continue;
    }
    m_kmlRoots[id] = path;
    m_kmlNodes[id] = {path, QString()};
    m_refreshingKml = true;
    while (m_layers->rowCount(node) > 0) {
      m_layers->removeNode(
          m_layers->index(m_layers->rowCount(node) - 1, 0, node));
    }
    if (isContainer(&document->root)) {
      addKmlChildren(*document, document->root, QString(), node);
    } else if (document->root.kind == KmlFeature::Kind::Placemark) {
      addKmlPlaces(*document, QString(), node, 0, false);
    }
    m_refreshingKml = false;
  }
  updateKmlActions();
}

void MainWindow::syncKmlEntry(const QModelIndex& index)
{
  // A folder (or picture or link) of a document being edited switched on
  // or off, or renamed, in the layer tree: the document follows.
  const QString id =
      m_layers->data(index, LayerTreeModel::NodeIdRole).toString();
  const QString key = kmlKeyOf(id);
  KmlEditDocument* document = kmlEditDocument(key);
  if (m_refreshingKml || document == nullptr) {
    return;
  }
  const QString path = m_kmlNodes.at(id).second;
  const KmlFeature* feature = kmlFeatureAt(document->document(), path);
  const bool collection =
      m_layers->data(index, LayerTreeModel::IsCollectionRole).toBool();
  const bool overlay =
      feature != nullptr && (feature->kind == KmlFeature::Kind::GroundOverlay ||
                             feature->kind == KmlFeature::Kind::ScreenOverlay);
  if (feature == nullptr || (!collection && !overlay)) {
    return;
  }
  const bool visible =
      m_layers->data(index, Qt::CheckStateRole).toInt() != Qt::Unchecked;
  if (feature->visibility != visible) {
    document->adjust([&path, visible](KmlDocument& changed) {
      KmlFeature* target = kmlFeatureAt(changed, path);
      if (target != nullptr) {
        target->visibility = visible;
      }
    });
  }
  const QString name = m_layers->data(index).toString();
  if (feature->name != name && !(feature->name.isEmpty() && path.isEmpty())) {
    document->edit(tr("Rename"), [&path, &name](KmlDocument& changed) {
      KmlFeature* target = kmlFeatureAt(changed, path);
      if (target == nullptr) {
        return false;
      }
      target->name = name;
      return true;
    });
    updateKmlActions();
  }
}

QJsonArray MainWindow::kmlProjectLayers(const QJsonArray& entries) const
{
  std::set<QString> roots;
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int32_t i = 0; i < entries.size(); ++i) {
    roots.insert(
        entries.at(i).toObject().value(QStringLiteral("node")).toString());
  }
  return withoutChildren(m_layers->toJson(), roots);
}

} // namespace ts
