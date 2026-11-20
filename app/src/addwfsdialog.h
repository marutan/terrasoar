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
 * The dialog that adds a layer of features from a WFS.
 */
#ifndef TS_APP_ADDWFSDIALOG_H
#define TS_APP_ADDWFSDIALOG_H

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <QColor>
#include <QDialog>
#include <QUrl>

#include "ts/layers/layerdefinition.h"
#include "ts/wfs/wfscapabilities.h"
#include "ts/wfs/wfssource.h"
#include "ts/wms/wmscatalogue.h"

class QComboBox;
class QDialogButtonBox;
class QEvent;
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
 * Adds a layer of features from a Web Feature Service: the user picks a
 * service from the catalogue or types its address, connects, chooses a
 * feature type and a colour; the service's terms and credit are shown
 * before Add makes the layer definition (a vector layer whose source is
 * `{"kind": "wfs", …}`, layerDefinition()).
 *
 * The catalogue is the copy built into the program until the current one
 * has been fetched (wfsCatalogueUrl, or TS_WFS_CATALOGUE_URL; empty to
 * stay offline). Requests go through @p network without blocking the GUI.
 *
 * See docs/user/adding-wfs-layers.md.
 *
 * @guithread
 */
class AddWfsDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(AddWfsDialog)

public:
  /** The user guide's section on this window, opened by F1. */
  static QString helpTarget();

  /** Creates the dialog; requests go through @p network. */
  AddWfsDialog(std::shared_ptr<NetworkClient> network, QWidget* parent);

  /** Cancels the requests still running. */
  ~AddWfsDialog() override;

  /** Puts @p address in the service box and connects to it. */
  void connectTo(const QString& address);

  /** Selects the feature type @p name; false if there is none. */
  bool selectFeatureType(const QString& name);

  /** Sets the layer's colour. */
  void setColour(const QColor& colour);

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

  /** Returns the status line (for tests). */
  QString statusText() const;

  /** Returns the details about the chosen feature type (for tests). */
  QString detailsText() const;

signals:
  /** The capabilities arrived and the list is filled. */
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
  void typeChosen();
  void chooseColour();
  void showColour();
  void accept() override;
  std::optional<WfsSource> chosenSource(QString* errorString) const;
  void setStatus(const QString& text, bool error);

  std::shared_ptr<NetworkClient> m_network;
  QLabel* m_serviceLabel = nullptr;
  QComboBox* m_serviceBox = nullptr;
  QPushButton* m_connectButton = nullptr;
  QLabel* m_aboutLabel = nullptr;
  QLabel* m_statusLabel = nullptr;
  QLabel* m_typesLabel = nullptr;
  QTreeWidget* m_typeTree = nullptr;
  QLabel* m_colourLabel = nullptr;
  QPushButton* m_colourButton = nullptr;
  QLabel* m_detailsLabel = nullptr;
  QDialogButtonBox* m_buttons = nullptr;
  QPushButton* m_addButton = nullptr;
  QTimer* m_pollTimer = nullptr;
  std::vector<WmsCatalogueEntry> m_catalogue;
  std::shared_ptr<NetworkTicket> m_capabilitiesTicket;
  std::shared_ptr<NetworkTicket> m_catalogueTicket;
  QUrl m_serviceUrl;
  std::optional<WfsCapabilities> m_capabilities;
  std::optional<LayerDefinition> m_definition;
  QColor m_colour;
  QString m_status;
};

} // namespace ts

#endif // TS_APP_ADDWFSDIALOG_H
