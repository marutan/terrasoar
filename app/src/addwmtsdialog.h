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
/**
 * @file
 * The dialog that adds a layer from a WMTS server.
 */
#ifndef TS_APP_ADDWMTSDIALOG_H
#define TS_APP_ADDWMTSDIALOG_H

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <QDialog>
#include <QUrl>

#include "ts/layers/layerdefinition.h"
#include "ts/wms/wmscatalogue.h"
#include "ts/wmts/wmtscapabilities.h"
#include "ts/wmts/wmtssource.h"

class QComboBox;
class QDialogButtonBox;
class QEvent;
class QGridLayout;
class QLabel;
class QPushButton;
class QTimer;
class QTreeWidget;
class QWidget;

namespace ts
{

class NetworkClient;
class NetworkTicket;

/**
 * Adds a layer from a WMTS server: the user picks a service from the
 * catalogue or types its address, connects, browses the server's layers
 * and chooses a style, an image format, a tile matrix set and a value for
 * each of the layer's dimensions (TIME and others); the service's terms and
 * credit are shown before Add makes the layer definition
 * (layerDefinition()).
 *
 * The address may be a KVP service (GetCapabilities is asked for) or a
 * RESTful WMTSCapabilities.xml (fetched as it is), see
 * wmtsCapabilitiesUrl(). Requests go through @p network without blocking
 * the GUI; a timer polls them while they run.
 *
 * The catalogue is the copy built into the program until the current one
 * has been fetched from the repository (wmtsCatalogueUrl, or the
 * environment variable TS_WMTS_CATALOGUE_URL; empty to stay offline); the
 * fetched copy is kept for the next time.
 *
 * See docs/user/adding-wmts-layers.md.
 *
 * @guithread
 */
class AddWmtsDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(AddWmtsDialog)

public:
  /**
   * The user guide's section on this window
   * (`adding-wmts-layers.md#adding-a-layer`), opened by F1.
   */
  static QString helpTarget();

  /** Creates the dialog; requests go through @p network. */
  AddWmtsDialog(std::shared_ptr<NetworkClient> network, QWidget* parent);

  /** Cancels any request still running. */
  ~AddWmtsDialog() override;

  /** Puts @p address in the service box and connects to it. */
  void connectTo(const QString& address);

  /** Returns the layer to add, once Add was pressed. */
  const std::optional<LayerDefinition>& layerDefinition() const
  {
    return m_definition;
  }

  /** Returns the services offered (the catalogue). */
  const std::vector<WmsCatalogueEntry>& catalogue() const
  {
    return m_catalogue;
  }

  /** Returns the capabilities last loaded, if any. */
  const std::optional<WmtsCapabilities>& capabilities() const
  {
    return m_capabilities;
  }

  /** Selects the layer with @p identifier; false if there is none. */
  bool selectLayer(const QString& identifier);

  /**
   * Sets the value of the dimension @p identifier of the chosen layer;
   * false if it has no such dimension.
   */
  // A dimension's name, then its value, as everywhere in WMTS.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  bool setDimension(const QString& identifier, const QString& value);

  /** Returns the status line (for tests). */
  QString statusText() const;

  /** Returns the details line about the chosen layer (for tests). */
  QString detailsText() const;

  /** Returns true while a request runs. */
  bool isBusy() const;

signals:
  /** The capabilities arrived and the layer list is filled. */
  void capabilitiesLoaded();
  /** Connecting failed; statusText() says why. */
  void connectionFailed();
  /** The catalogue changed (the current one arrived). */
  void catalogueUpdated();

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

private:
  void retranslateUi();
  void loadCatalogue();
  void fillCatalogueBox();
  void serviceChosen(int32_t index);
  void startConnect();
  void poll();
  void capabilitiesArrived();
  void catalogueArrived();
  void fillLayers();
  void layerChosen();
  void fillChoices(const WmtsLayer& layer);
  void showDetails();
  void accept() override;
  std::optional<WmtsSource> chosenSource(QString* errorString) const;
  const WmtsLayer* chosenLayer() const;
  void setStatus(const QString& text, bool error);

  std::shared_ptr<NetworkClient> m_network;
  QComboBox* m_serviceBox = nullptr;
  QPushButton* m_connectButton = nullptr;
  QLabel* m_serviceLabel = nullptr;
  QLabel* m_aboutLabel = nullptr;
  QLabel* m_statusLabel = nullptr;
  QLabel* m_layersLabel = nullptr;
  QTreeWidget* m_layerTree = nullptr;
  QLabel* m_styleLabel = nullptr;
  QComboBox* m_styleBox = nullptr;
  QLabel* m_formatLabel = nullptr;
  QComboBox* m_formatBox = nullptr;
  QLabel* m_setLabel = nullptr;
  QComboBox* m_setBox = nullptr;
  /** Style, format, set and dimension rows, aligned in one grid. */
  QGridLayout* m_choicesLayout = nullptr;
  /** One box per dimension of the chosen layer, with its identifier. */
  std::vector<std::pair<QString, QComboBox*>> m_dimensionBoxes;
  /** The dimension rows' labels (removed with the boxes). */
  std::vector<QLabel*> m_dimensionLabels;
  QLabel* m_detailsLabel = nullptr;
  QDialogButtonBox* m_buttons = nullptr;
  QPushButton* m_addButton = nullptr;
  QTimer* m_pollTimer = nullptr;
  std::vector<WmsCatalogueEntry> m_catalogue;
  std::shared_ptr<NetworkTicket> m_capabilitiesTicket;
  std::shared_ptr<NetworkTicket> m_catalogueTicket;
  QUrl m_serviceUrl;
  std::optional<WmtsCapabilities> m_capabilities;
  std::optional<LayerDefinition> m_definition;
  QString m_status;
  bool m_statusIsError = false;
};

} // namespace ts

#endif // TS_APP_ADDWMTSDIALOG_H
