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
#include "addwfsdialog.h"

#include <cstdint>
#include <utility>

#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPixmap>
#include <QPushButton>
#include <QSaveFile>
#include <QShortcut>
#include <QTimer>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

#include "helpwindow.h"
#include "ts/geo/geoformat.h"
#include "ts/network/networkclient.h"
#include "ts/vector/vectorstyle.h"
#include "wmscataloguefile.h"

namespace ts
{

namespace
{

constexpr int32_t pollIntervalMs = 50;

/** Item data: the feature type's name. */
constexpr int32_t typeNameRole = Qt::UserRole + 1;

QUrl catalogueSourceUrl()
{
  if (qEnvironmentVariableIsSet("TS_WFS_CATALOGUE_URL")) {
    return QUrl(qEnvironmentVariable("TS_WFS_CATALOGUE_URL"));
  }
  return QUrl(QString::fromLatin1(wfsCatalogueUrl));
}

} // namespace

QString AddWfsDialog::helpTarget()
{
  return QStringLiteral("adding-wfs-layers.md#adding-a-layer");
}

AddWfsDialog::AddWfsDialog(std::shared_ptr<NetworkClient> network,
                           QWidget* parent)
    : QDialog(parent), m_network(std::move(network)),
      m_serviceLabel(new QLabel(this)), m_serviceBox(new QComboBox(this)),
      m_connectButton(new QPushButton(this)), m_aboutLabel(new QLabel(this)),
      m_statusLabel(new QLabel(this)), m_typesLabel(new QLabel(this)),
      m_typeTree(new QTreeWidget(this)), m_colourLabel(new QLabel(this)),
      m_colourButton(new QPushButton(this)), m_detailsLabel(new QLabel(this)),
      m_buttons(new QDialogButtonBox(this)), m_pollTimer(new QTimer(this)),
      m_colour(QColor(0xe8, 0x50, 0x3a))
{
  QShortcut* help = new QShortcut(QKeySequence::HelpContents, this);
  connect(help, &QShortcut::activated, this,
          [this]() { showUserGuide(this, helpTarget()); });
  setObjectName(QStringLiteral("addWfsDialog"));
  m_serviceBox->setObjectName(QStringLiteral("wfsServiceBox"));
  m_serviceBox->setEditable(true);
  m_serviceBox->setInsertPolicy(QComboBox::NoInsert);
  m_serviceBox->setMinimumContentsLength(40);
  m_serviceLabel->setBuddy(m_serviceBox);
  m_connectButton->setObjectName(QStringLiteral("wfsConnectButton"));
  m_aboutLabel->setObjectName(QStringLiteral("wfsAboutLabel"));
  m_aboutLabel->setWordWrap(true);
  m_aboutLabel->setOpenExternalLinks(true);
  m_aboutLabel->setTextFormat(Qt::RichText);
  m_statusLabel->setObjectName(QStringLiteral("wfsStatusLabel"));
  m_statusLabel->setWordWrap(true);
  m_typeTree->setObjectName(QStringLiteral("wfsTypeTree"));
  m_typeTree->setHeaderHidden(true);
  m_typeTree->setColumnCount(1);
  m_typeTree->setRootIsDecorated(false);
  m_typesLabel->setBuddy(m_typeTree);
  m_colourButton->setObjectName(QStringLiteral("wfsColourButton"));
  m_colourLabel->setBuddy(m_colourButton);
  m_detailsLabel->setObjectName(QStringLiteral("wfsDetailsLabel"));
  m_detailsLabel->setWordWrap(true);
  m_addButton = m_buttons->addButton(QString(), QDialogButtonBox::AcceptRole);
  m_addButton->setObjectName(QStringLiteral("wfsAddButton"));
  m_addButton->setEnabled(false);
  m_buttons->addButton(QDialogButtonBox::Cancel);
  m_pollTimer->setInterval(pollIntervalMs);

  QHBoxLayout* service = new QHBoxLayout;
  service->addWidget(m_serviceBox, 1);
  service->addWidget(m_connectButton);
  QHBoxLayout* colour = new QHBoxLayout;
  colour->addWidget(m_colourLabel);
  colour->addWidget(m_colourButton);
  colour->addStretch(1);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(m_serviceLabel);
  layout->addLayout(service);
  layout->addWidget(m_aboutLabel);
  layout->addWidget(m_statusLabel);
  layout->addWidget(m_typesLabel);
  layout->addWidget(m_typeTree, 1);
  layout->addLayout(colour);
  layout->addWidget(m_detailsLabel);
  layout->addWidget(m_buttons);

  connect(m_connectButton, &QPushButton::clicked, this,
          &AddWfsDialog::startConnect);
  connect(m_serviceBox->lineEdit(), &QLineEdit::returnPressed, this,
          &AddWfsDialog::startConnect);
  connect(m_serviceBox, qOverload<int32_t>(&QComboBox::activated), this,
          &AddWfsDialog::serviceChosen);
  connect(m_typeTree, &QTreeWidget::currentItemChanged, this,
          &AddWfsDialog::typeChosen);
  connect(m_colourButton, &QPushButton::clicked, this,
          &AddWfsDialog::chooseColour);
  connect(m_buttons, &QDialogButtonBox::accepted, this, &AddWfsDialog::accept);
  connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_pollTimer, &QTimer::timeout, this, &AddWfsDialog::poll);

  loadCatalogue();
  showColour();
  retranslateUi();
  resize(620, 560);
}

AddWfsDialog::~AddWfsDialog()
{
  for (const std::shared_ptr<NetworkTicket>& ticket :
       {m_capabilitiesTicket, m_catalogueTicket}) {
    if (ticket) {
      ticket->cancel();
    }
  }
}

void AddWfsDialog::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QDialog::changeEvent(event);
}

void AddWfsDialog::retranslateUi()
{
  setWindowTitle(tr("Add WFS Layer"));
  m_serviceLabel->setText(tr("&Service (choose one or type its address):"));
  m_connectButton->setText(tr("&Connect"));
  m_typesLabel->setText(tr("&Feature types:"));
  m_colourLabel->setText(tr("C&olour:"));
  m_colourButton->setToolTip(tr("The colour the features are drawn in"));
  m_addButton->setText(tr("&Add"));
  m_serviceBox->lineEdit()->setPlaceholderText(tr("https://example.org/wfs"));
  if (m_status.isEmpty()) {
    m_statusLabel->setText(
        tr("Connect to see the feature types the server offers."));
  }
  serviceChosen(m_serviceBox->currentIndex());
  typeChosen();
}

void AddWfsDialog::loadCatalogue()
{
  m_catalogue = currentWfsCatalogue();
  fillCatalogueBox();
  const QUrl source = catalogueSourceUrl();
  if (m_network && source.isValid() && !source.isEmpty()) {
    m_catalogueTicket = m_network->start(HttpRequest{source, {}});
    m_pollTimer->start();
  }
}

void AddWfsDialog::fillCatalogueBox()
{
  const QString typed = m_serviceBox->currentText();
  const QSignalBlocker blocker(m_serviceBox);
  m_serviceBox->clear();
  for (const WmsCatalogueEntry& entry : m_catalogue) {
    m_serviceBox->addItem(entry.title, entry.url.toString());
  }
  m_serviceBox->setCurrentIndex(-1);
  m_serviceBox->setEditText(typed);
}

void AddWfsDialog::catalogueArrived()
{
  const HttpResponse response = m_catalogueTicket->response();
  m_catalogueTicket.reset();
  if (!response.isSuccess()) {
    return;
  }
  QString error;
  const std::optional<std::vector<WmsCatalogueEntry>> entries =
      parseWfsCatalogue(response.body, &error);
  if (!entries || entries->empty()) {
    return;
  }
  m_catalogue = *entries;
  QDir().mkpath(QFileInfo(wfsCatalogueCachePath()).absolutePath());
  QSaveFile file(wfsCatalogueCachePath());
  if (file.open(QIODevice::WriteOnly)) {
    file.write(response.body);
    file.commit();
  }
  fillCatalogueBox();
  emit catalogueUpdated();
}

void AddWfsDialog::serviceChosen(int32_t index)
{
  if (index < 0 || index >= static_cast<int32_t>(m_catalogue.size())) {
    m_aboutLabel->clear();
    return;
  }
  const WmsCatalogueEntry& entry =
      m_catalogue.at(static_cast<std::size_t>(index));
  m_serviceBox->setEditText(entry.url.toString());
  QString about = entry.description.toHtmlEscaped();
  if (!entry.attribution.isEmpty()) {
    about += QStringLiteral("<br>") +
             tr("Credit: %1").arg(entry.attribution.toHtmlEscaped());
  }
  if (entry.termsUrl.isValid() && !entry.termsUrl.isEmpty()) {
    about +=
        QStringLiteral("<br><a href=\"%1\">%2</a>")
            .arg(entry.termsUrl.toString(QUrl::FullyEncoded).toHtmlEscaped(),
                 tr("Terms of use"));
  }
  m_aboutLabel->setText(about);
}

void AddWfsDialog::connectTo(const QString& address)
{
  m_serviceBox->setEditText(address);
  startConnect();
}

void AddWfsDialog::startConnect()
{
  const QUrl url = QUrl::fromUserInput(m_serviceBox->currentText().trimmed());
  if (!url.isValid() || (url.scheme() != QLatin1String("http") &&
                         url.scheme() != QLatin1String("https"))) {
    setStatus(tr("Enter the address of a WFS, starting with http:// or "
                 "https://."),
              true);
    emit connectionFailed();
    return;
  }
  if (m_capabilitiesTicket) {
    m_capabilitiesTicket->cancel();
  }
  m_serviceUrl = url;
  m_capabilities.reset();
  m_typeTree->clear();
  setStatus(tr("Connecting to %1…").arg(url.host()), false);
  m_connectButton->setEnabled(false);
  m_capabilitiesTicket =
      m_network->start(HttpRequest{wfsCapabilitiesUrl(url), {}});
  m_pollTimer->start();
}

void AddWfsDialog::poll()
{
  if (m_catalogueTicket && m_catalogueTicket->isFinished()) {
    catalogueArrived();
  }
  if (m_capabilitiesTicket && m_capabilitiesTicket->isFinished()) {
    capabilitiesArrived();
  }
  if (!m_catalogueTicket && !m_capabilitiesTicket) {
    m_pollTimer->stop();
  }
}

void AddWfsDialog::capabilitiesArrived()
{
  const HttpResponse response = m_capabilitiesTicket->response();
  m_capabilitiesTicket.reset();
  m_connectButton->setEnabled(true);
  if (response.error == NetworkError::Cancelled) {
    return;
  }
  if (response.error != NetworkError::None) {
    setStatus(
        tr("Can't reach %1: %2").arg(m_serviceUrl.host(), response.errorString),
        true);
    emit connectionFailed();
    return;
  }
  QString error;
  std::optional<WfsCapabilities> parsed =
      parseWfsCapabilities(response.body, &error);
  if (!parsed) {
    setStatus(error, true);
    emit connectionFailed();
    return;
  }
  m_capabilities = std::move(parsed);
  for (const WfsFeatureType& type : m_capabilities->featureTypes) {
    QTreeWidgetItem* item = new QTreeWidgetItem(m_typeTree);
    item->setText(0, type.title.isEmpty() ? type.name : type.title);
    item->setToolTip(0, type.abstract.isEmpty() ? type.name : type.abstract);
    item->setData(0, typeNameRole, type.name);
  }
  setStatus(tr("%1: %n feature type(s).", nullptr,
               static_cast<int32_t>(m_capabilities->featureTypes.size()))
                .arg(m_capabilities->title.isEmpty() ? m_serviceUrl.host()
                                                     : m_capabilities->title),
            false);
  emit capabilitiesLoaded();
}

bool AddWfsDialog::selectFeatureType(const QString& name)
{
  for (int32_t i = 0; i < m_typeTree->topLevelItemCount(); ++i) {
    QTreeWidgetItem* item = m_typeTree->topLevelItem(i);
    if (item->data(0, typeNameRole).toString() == name) {
      m_typeTree->setCurrentItem(item);
      return true;
    }
  }
  return false;
}

std::optional<WfsSource> AddWfsDialog::chosenSource(QString* errorString) const
{
  const QTreeWidgetItem* item = m_typeTree->currentItem();
  if (!m_capabilities || item == nullptr) {
    return std::nullopt;
  }
  std::optional<WfsSource> source = wfsSourceFor(
      *m_capabilities, item->data(0, typeNameRole).toString(), errorString);
  if (source) {
    source->service = m_serviceUrl;
    const WmsCatalogueEntry* entry =
        findWmsCatalogueEntry(m_serviceUrl, m_catalogue);
    if (entry != nullptr && !entry->attribution.isEmpty()) {
      source->attribution = entry->attribution;
    }
  }
  return source;
}

void AddWfsDialog::typeChosen()
{
  QString error;
  const std::optional<WfsSource> source = chosenSource(&error);
  m_addButton->setEnabled(source.has_value());
  if (!m_capabilities || m_typeTree->currentItem() == nullptr) {
    m_detailsLabel->clear();
    return;
  }
  if (!source) {
    m_detailsLabel->setText(error);
    return;
  }
  QStringList details;
  details << tr("Features come as %1, %2 at a time.")
                 .arg(source->outputFormat.isEmpty() ? tr("the server's GML")
                                                     : source->outputFormat)
                 .arg(source->pageSize);
  if (source->minimumLevel > 0) {
    const QLocale locale;
    details << tr("They show when the view is about %1 across or less.")
                   .arg(formatRoundDistance(
                       TileKey::sizeDeg(source->minimumLevel) * 111320.0 * 2.0,
                       locale));
  }
  if (!source->attribution.isEmpty()) {
    details << tr("Credit: %1").arg(source->attribution);
  }
  if (!m_capabilities->accessConstraints.isEmpty() &&
      m_capabilities->accessConstraints.compare(QLatin1String("none"),
                                                Qt::CaseInsensitive) != 0) {
    details << tr("The server's terms: %1")
                   .arg(m_capabilities->accessConstraints.left(400));
  }
  m_detailsLabel->setText(details.join(QLatin1Char('\n')));
}

void AddWfsDialog::chooseColour()
{
  const QColor chosen =
      QColorDialog::getColor(m_colour, this, tr("Colour of the Features"));
  if (chosen.isValid()) {
    setColour(chosen);
  }
}

void AddWfsDialog::setColour(const QColor& colour)
{
  m_colour = colour;
  showColour();
}

void AddWfsDialog::showColour()
{
  QPixmap swatch(24, 14);
  swatch.fill(m_colour);
  m_colourButton->setIcon(QIcon(swatch));
  m_colourButton->setIconSize(swatch.size());
}

void AddWfsDialog::accept()
{
  QString error;
  const std::optional<WfsSource> source = chosenSource(&error);
  if (!source) {
    return;
  }
  LayerDefinition definition;
  definition.id = QStringLiteral("wfs:") +
                  QUuid::createUuid().toString(QUuid::WithoutBraces);
  definition.origin = LayerOrigin::Catalogue;
  definition.type = LayerType::Vector;
  definition.name = source->title;
  definition.source = source->toJson();
  // Lines and points in the colour, areas a translucent wash of it.
  VectorStyle style;
  style.outline = m_colour;
  QColor fill = m_colour;
  fill.setAlpha(64);
  style.fill = fill;
  style.outlineWidthPx = 1.5;
  style.pointSizePx = 7.0;
  definition.style = style.toJson();
  m_definition = definition;
  QDialog::accept();
}

void AddWfsDialog::setStatus(const QString& text, bool error)
{
  m_status = text;
  m_statusLabel->setText(text);
  m_statusLabel->setStyleSheet(error ? QStringLiteral("color: #c0392b;")
                                     : QString());
}

QString AddWfsDialog::statusText() const
{
  return m_status;
}

QString AddWfsDialog::detailsText() const
{
  return m_detailsLabel->text();
}

} // namespace ts
