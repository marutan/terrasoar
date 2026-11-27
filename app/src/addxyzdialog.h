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
 * The dialog that adds a layer from an XYZ or TMS tile service.
 */
#ifndef TS_APP_ADDXYZDIALOG_H
#define TS_APP_ADDXYZDIALOG_H

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <QDialog>

#include "ts/layers/layerdefinition.h"
#include "ts/xyz/xyzsource.h"

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;
class QWidget;

namespace ts
{

class NetworkClient;
class NetworkTicket;

/**
 * Returns the layer for @p xyz, as Add in the Add XYZ Tile Layer dialog
 * makes it: a catalogue imagery layer with a new id, named after its
 * title.
 */
LayerDefinition xyzLayerDefinition(const XyzSource& xyz);

/**
 * Adds a layer from an XYZ or TMS tile service: the user picks one from
 * the catalogue, which fills in everything, or types the address template
 * (`https://…/{z}/{x}/{y}.png`) and says what the service needs: its
 * subdomains for `{s}`, whether rows count from the south (TMS), its zoom
 * levels and whom to credit. The template is checked as it is typed
 * (checkXyzTemplate()); Add makes the layer definition (layerDefinition()).
 * There is no capabilities document, so nothing is asked of the service
 * until the layer draws.
 *
 * The catalogue is the copy built into the program until the current one
 * has been fetched from the repository (xyzCatalogueUrl, or the
 * environment variable TS_XYZ_CATALOGUE_URL; empty to stay offline); the
 * fetched copy is kept for the next time.
 *
 * See docs/user/adding-xyz-layers.md.
 *
 * @guithread
 */
class AddXyzDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(AddXyzDialog)

public:
  /** The user guide's section on this window, opened by F1. */
  static QString helpTarget();

  /** Creates the dialog; the catalogue is fetched through @p network. */
  AddXyzDialog(std::shared_ptr<NetworkClient> network, QWidget* parent);

  /** Cancels the catalogue request if it is still running. */
  ~AddXyzDialog() override;

  /** Returns the layer to add, once Add was pressed. */
  const std::optional<LayerDefinition>& layerDefinition() const
  {
    return m_definition;
  }

  /** Returns the services offered (the catalogue). */
  const std::vector<XyzCatalogueEntry>& catalogue() const
  {
    return m_catalogue;
  }

  /**
   * Fills the fields from catalogue entry @p index; false if there is no
   * such entry.
   */
  bool chooseService(int32_t index);

  /** Returns the source the fields describe, if they describe one. */
  std::optional<XyzSource> source() const;

  /** Returns the status line: what is wrong with the fields (for tests). */
  QString statusText() const;

signals:
  /** The catalogue changed (the current one arrived). */
  void catalogueUpdated();

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

private:
  void retranslateUi();
  void loadCatalogue();
  void fillCatalogueBox();
  void poll();
  void catalogueArrived();
  void serviceChosen(int32_t index);
  void check();
  void accept() override;

  std::shared_ptr<NetworkClient> m_network;
  QLabel* m_serviceLabel = nullptr;
  QComboBox* m_serviceBox = nullptr;
  QLabel* m_aboutLabel = nullptr;
  QLabel* m_nameLabel = nullptr;
  QLineEdit* m_nameEdit = nullptr;
  QLabel* m_templateLabel = nullptr;
  QLineEdit* m_templateEdit = nullptr;
  QLabel* m_subdomainsLabel = nullptr;
  QLineEdit* m_subdomainsEdit = nullptr;
  QCheckBox* m_tmsBox = nullptr;
  QLabel* m_zoomLabel = nullptr;
  QSpinBox* m_minimumZoomBox = nullptr;
  QSpinBox* m_maximumZoomBox = nullptr;
  QLabel* m_attributionLabel = nullptr;
  QLineEdit* m_attributionEdit = nullptr;
  QLabel* m_statusLabel = nullptr;
  QDialogButtonBox* m_buttons = nullptr;
  QPushButton* m_addButton = nullptr;
  QTimer* m_pollTimer = nullptr;
  std::vector<XyzCatalogueEntry> m_catalogue;
  std::shared_ptr<NetworkTicket> m_catalogueTicket;
  std::optional<LayerDefinition> m_definition;
  QString m_status;
};

} // namespace ts

#endif // TS_APP_ADDXYZDIALOG_H
