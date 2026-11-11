// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The dialog that adds a layer from a WMS server.
 */
#ifndef TS_APP_ADDWMSDIALOG_H
#define TS_APP_ADDWMSDIALOG_H

#include <memory>
#include <optional>
#include <vector>

#include <QDialog>
#include <QUrl>

#include "ts/layers/layerdefinition.h"
#include "ts/wms/wmscapabilities.h"
#include "ts/wms/wmscatalogue.h"
#include "ts/wms/wmssource.h"

class QComboBox;
class QDialogButtonBox;
class QEvent;
class QLabel;
class QPushButton;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace ts
{

class NetworkClient;
class NetworkTicket;

/**
 * Adds a layer from a WMS server: the user picks a service from the
 * catalogue or types its address, connects, browses the server's layers
 * and chooses a style and format; Add makes the layer definition
 * (layerDefinition()).
 *
 * Connecting asks for the capabilities in WMS 1.3.0, and in 1.1.1 if the
 * server refuses that. Requests go through @p network without blocking
 * the GUI; a timer polls them while they run.
 *
 * The catalogue is the copy built into the program until the current one
 * has been fetched from the repository (wmsCatalogueUrl, or the
 * environment variable TS_WMS_CATALOGUE_URL; empty to stay offline); the
 * fetched copy is kept for the next time.
 *
 * See docs/user/adding-wms-layers.md.
 *
 * @guithread
 */
class AddWmsDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(AddWmsDialog)

public:
  /** Creates the dialog; requests go through @p network. */
  AddWmsDialog(std::shared_ptr<NetworkClient> network, QWidget* parent);

  /** Cancels any request still running. */
  ~AddWmsDialog() override;

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
  const std::optional<WmsCapabilities>& capabilities() const
  {
    return m_capabilities;
  }

  /** Selects the server's layer named @p name; false if there is none. */
  bool selectLayer(const QString& name);

  /** Returns the status line (for tests). */
  QString statusText() const;

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
  void serviceChosen(int index);
  void startConnect();
  void requestCapabilities(const QString& version);
  void poll();
  void capabilitiesArrived();
  void catalogueArrived();
  void fillLayers();
  void layerChosen();
  void accept() override;
  std::optional<WmsSource> chosenSource(QString* errorString) const;
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
  QLabel* m_detailsLabel = nullptr;
  QDialogButtonBox* m_buttons = nullptr;
  QPushButton* m_addButton = nullptr;
  QTimer* m_pollTimer = nullptr;

  std::vector<WmsCatalogueEntry> m_catalogue;
  std::shared_ptr<NetworkTicket> m_capabilitiesTicket;
  std::shared_ptr<NetworkTicket> m_catalogueTicket;
  QUrl m_serviceUrl;
  QString m_requestedVersion;
  std::optional<WmsCapabilities> m_capabilities;
  std::optional<LayerDefinition> m_definition;
  QString m_status;
  bool m_statusIsError = false;
};

} // namespace ts

#endif // TS_APP_ADDWMSDIALOG_H
