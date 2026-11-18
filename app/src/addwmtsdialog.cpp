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
#include "addwmtsdialog.h"

#include <algorithm>
#include <cstdint>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QShortcut>
#include <QTimer>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

#include "helpwindow.h"
#include "ts/network/networkclient.h"
#include "ts/wms/wmssource.h"
#include "wmscataloguefile.h"

namespace ts
{

namespace
{

/** How often running requests are looked at, ms. */
constexpr int32_t pollIntervalMs = 50;

/** Item data: the layer's identifier. */
constexpr int32_t layerIdRole = Qt::UserRole + 1;

QUrl catalogueSourceUrl()
{
  if (qEnvironmentVariableIsSet("TS_WMTS_CATALOGUE_URL")) {
    return QUrl(qEnvironmentVariable("TS_WMTS_CATALOGUE_URL"));
  }
  return QUrl(QString::fromLatin1(wmtsCatalogueUrl));
}

QString setLabel(const WmtsCapabilities& capabilities, const QString& set)
{
  const WmtsTileMatrixSet* found = capabilities.findTileMatrixSet(set);
  const QString crs =
      found ? normalisedWmtsCrs(found->supportedCrs) : QString();
  return crs.isEmpty() ? set : QStringLiteral("%1 (%2)").arg(set, crs);
}

} // namespace

QString AddWmtsDialog::helpTarget()
{
  return QStringLiteral("adding-wmts-layers.md#adding-a-layer");
}

AddWmtsDialog::AddWmtsDialog(std::shared_ptr<NetworkClient> network,
                             QWidget* parent)
    : QDialog(parent), m_network(std::move(network)),
      m_serviceBox(new QComboBox(this)), m_connectButton(new QPushButton(this)),
      m_serviceLabel(new QLabel(this)), m_aboutLabel(new QLabel(this)),
      m_statusLabel(new QLabel(this)), m_layersLabel(new QLabel(this)),
      m_layerTree(new QTreeWidget(this)), m_styleLabel(new QLabel(this)),
      m_styleBox(new QComboBox(this)), m_formatLabel(new QLabel(this)),
      m_formatBox(new QComboBox(this)), m_setLabel(new QLabel(this)),
      m_setBox(new QComboBox(this)), m_choicesLayout(new QGridLayout),
      m_detailsLabel(new QLabel(this)), m_buttons(new QDialogButtonBox(this)),
      m_pollTimer(new QTimer(this))
{
  QShortcut* help = new QShortcut(QKeySequence::HelpContents, this);
  connect(help, &QShortcut::activated, this,
          [this]() { showUserGuide(this, helpTarget()); });
  setObjectName(QStringLiteral("addWmtsDialog"));
  m_serviceBox->setObjectName(QStringLiteral("wmtsServiceBox"));
  m_serviceBox->setEditable(true);
  m_serviceBox->setInsertPolicy(QComboBox::NoInsert);
  m_serviceBox->setMinimumContentsLength(40);
  m_serviceLabel->setBuddy(m_serviceBox);
  m_connectButton->setObjectName(QStringLiteral("wmtsConnectButton"));
  m_aboutLabel->setObjectName(QStringLiteral("wmtsAboutLabel"));
  m_aboutLabel->setWordWrap(true);
  m_aboutLabel->setOpenExternalLinks(true);
  m_aboutLabel->setTextFormat(Qt::RichText);
  m_statusLabel->setObjectName(QStringLiteral("wmtsStatusLabel"));
  m_statusLabel->setWordWrap(true);
  m_layerTree->setObjectName(QStringLiteral("wmtsLayerTree"));
  m_layerTree->setHeaderHidden(true);
  m_layerTree->setColumnCount(1);
  m_layerTree->setRootIsDecorated(false);
  m_layersLabel->setBuddy(m_layerTree);
  m_styleBox->setObjectName(QStringLiteral("wmtsStyleBox"));
  m_styleLabel->setBuddy(m_styleBox);
  m_formatBox->setObjectName(QStringLiteral("wmtsFormatBox"));
  m_formatLabel->setBuddy(m_formatBox);
  m_setBox->setObjectName(QStringLiteral("wmtsSetBox"));
  m_setLabel->setBuddy(m_setBox);
  m_detailsLabel->setObjectName(QStringLiteral("wmtsDetailsLabel"));
  m_detailsLabel->setWordWrap(true);
  m_detailsLabel->setOpenExternalLinks(true);
  m_detailsLabel->setTextFormat(Qt::RichText);
  m_addButton = m_buttons->addButton(QString(), QDialogButtonBox::AcceptRole);
  m_addButton->setObjectName(QStringLiteral("wmtsAddButton"));
  m_addButton->setEnabled(false);
  m_buttons->addButton(QDialogButtonBox::Cancel);
  m_pollTimer->setInterval(pollIntervalMs);

  QHBoxLayout* service = new QHBoxLayout;
  service->addWidget(m_serviceBox, 1);
  service->addWidget(m_connectButton);
  m_choicesLayout->addWidget(m_styleLabel, 0, 0);
  m_choicesLayout->addWidget(m_styleBox, 0, 1);
  m_choicesLayout->addWidget(m_formatLabel, 1, 0);
  m_choicesLayout->addWidget(m_formatBox, 1, 1);
  m_choicesLayout->addWidget(m_setLabel, 2, 0);
  m_choicesLayout->addWidget(m_setBox, 2, 1);
  m_choicesLayout->setColumnStretch(1, 1);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(m_serviceLabel);
  layout->addLayout(service);
  layout->addWidget(m_aboutLabel);
  layout->addWidget(m_statusLabel);
  layout->addWidget(m_layersLabel);
  layout->addWidget(m_layerTree, 1);
  layout->addLayout(m_choicesLayout);
  layout->addWidget(m_detailsLabel);
  layout->addWidget(m_buttons);

  connect(m_connectButton, &QPushButton::clicked, this,
          &AddWmtsDialog::startConnect);
  connect(m_serviceBox->lineEdit(), &QLineEdit::returnPressed, this,
          &AddWmtsDialog::startConnect);
  connect(m_serviceBox, qOverload<int32_t>(&QComboBox::activated), this,
          &AddWmtsDialog::serviceChosen);
  connect(m_layerTree, &QTreeWidget::currentItemChanged, this,
          &AddWmtsDialog::layerChosen);
  for (QComboBox* box : {m_styleBox, m_formatBox, m_setBox}) {
    connect(box, qOverload<int32_t>(&QComboBox::currentIndexChanged), this,
            &AddWmtsDialog::showDetails);
  }
  connect(m_buttons, &QDialogButtonBox::accepted, this, &AddWmtsDialog::accept);
  connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_pollTimer, &QTimer::timeout, this, &AddWmtsDialog::poll);

  loadCatalogue();
  retranslateUi();
  resize(660, 620);
}

AddWmtsDialog::~AddWmtsDialog()
{
  for (const std::shared_ptr<NetworkTicket>& ticket :
       {m_capabilitiesTicket, m_catalogueTicket}) {
    if (ticket) {
      ticket->cancel();
    }
  }
}

void AddWmtsDialog::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QDialog::changeEvent(event);
}

void AddWmtsDialog::retranslateUi()
{
  setWindowTitle(tr("Add WMTS Layer"));
  m_serviceLabel->setText(tr("&Service (choose one or type its address):"));
  m_connectButton->setText(tr("&Connect"));
  m_layersLabel->setText(tr("&Layers:"));
  m_styleLabel->setText(tr("S&tyle:"));
  m_formatLabel->setText(tr("&Format:"));
  m_setLabel->setText(tr("Tile &matrix set:"));
  m_addButton->setText(tr("&Add"));
  m_serviceBox->lineEdit()->setPlaceholderText(
      tr("https://example.org/wmts/1.0.0/WMTSCapabilities.xml"));
  if (m_status.isEmpty()) {
    m_statusLabel->setText(tr("Connect to see the layers the server offers."));
  }
  serviceChosen(m_serviceBox->currentIndex());
  showDetails();
}

// --- Catalogue
// ----------------------------------------------------------------

void AddWmtsDialog::loadCatalogue()
{
  // The last copy fetched, else the one built in; then ask for the current.
  m_catalogue = currentWmtsCatalogue();
  fillCatalogueBox();
  const QUrl source = catalogueSourceUrl();
  if (m_network && source.isValid() && !source.isEmpty()) {
    m_catalogueTicket = m_network->start(HttpRequest{source, {}});
    m_pollTimer->start();
  }
}

void AddWmtsDialog::fillCatalogueBox()
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

void AddWmtsDialog::catalogueArrived()
{
  const HttpResponse response = m_catalogueTicket->response();
  m_catalogueTicket.reset();
  if (!response.isSuccess()) {
    return; // offline: the copy we have will do
  }
  QString error;
  const std::optional<std::vector<WmsCatalogueEntry>> entries =
      parseWmtsCatalogue(response.body, &error);
  if (!entries || entries->empty()) {
    return;
  }
  m_catalogue = *entries;
  QDir().mkpath(QFileInfo(wmtsCatalogueCachePath()).absolutePath());
  QSaveFile file(wmtsCatalogueCachePath());
  if (file.open(QIODevice::WriteOnly)) {
    file.write(response.body);
    file.commit();
  }
  fillCatalogueBox();
  emit catalogueUpdated();
}

void AddWmtsDialog::serviceChosen(int32_t index)
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

// --- Capabilities
// -------------------------------------------------------------

void AddWmtsDialog::connectTo(const QString& address)
{
  m_serviceBox->setEditText(address);
  startConnect();
}

void AddWmtsDialog::startConnect()
{
  const QUrl url = QUrl::fromUserInput(m_serviceBox->currentText().trimmed());
  if (!url.isValid() || (url.scheme() != QLatin1String("http") &&
                         url.scheme() != QLatin1String("https"))) {
    setStatus(tr("Enter the address of a WMTS service, starting with "
                 "https:// or http://."),
              true);
    emit connectionFailed();
    return;
  }
  if (!m_network) {
    return;
  }
  m_serviceUrl = url;
  m_capabilities.reset();
  m_layerTree->clear();
  fillChoices(WmtsLayer{});
  m_detailsLabel->clear();
  m_addButton->setEnabled(false);
  if (m_capabilitiesTicket) {
    m_capabilitiesTicket->cancel();
  }
  m_capabilitiesTicket =
      m_network->start(HttpRequest{wmtsCapabilitiesUrl(m_serviceUrl), {}});
  setStatus(tr("Connecting to %1…").arg(m_serviceUrl.host()), false);
  m_connectButton->setEnabled(false);
  m_pollTimer->start();
}

void AddWmtsDialog::poll()
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

void AddWmtsDialog::capabilitiesArrived()
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
  std::optional<WmtsCapabilities> parsed =
      response.status >= 200 && response.status < 300
          ? parseWmtsCapabilities(response.body, &error)
          : std::nullopt;
  if (!parsed) {
    // An error status may still carry an OWS ExceptionReport.
    if (error.isEmpty()) {
      parseWmtsCapabilities(response.body, &error);
    }
    setStatus(error.isEmpty() || response.status >= 300
                  ? tr("The server answered: %1").arg(response.errorString)
                  : error,
              true);
    emit connectionFailed();
    return;
  }
  m_capabilities = std::move(parsed);
  fillLayers();
  setStatus(tr("%1: %n layer(s).", nullptr,
               static_cast<int32_t>(m_capabilities->layers.size()))
                .arg(m_capabilities->title.isEmpty() ? m_serviceUrl.host()
                                                     : m_capabilities->title),
            false);
  emit capabilitiesLoaded();
}

void AddWmtsDialog::fillLayers()
{
  m_layerTree->clear();
  if (!m_capabilities) {
    return;
  }
  for (const WmtsLayer& layer : m_capabilities->layers) {
    QTreeWidgetItem* item = new QTreeWidgetItem(m_layerTree);
    item->setText(0, layer.title.isEmpty() ? layer.identifier : layer.title);
    item->setData(0, layerIdRole, layer.identifier);
    // The server's identifier for it, then its description (its own text).
    QStringList tip{layer.identifier};
    if (!layer.abstract.isEmpty()) {
      tip.append(layer.abstract.left(600));
    }
    item->setToolTip(0, tip.join(QLatin1Char('\n')));
  }
}

bool AddWmtsDialog::selectLayer(const QString& identifier)
{
  for (int32_t i = 0; i < m_layerTree->topLevelItemCount(); ++i) {
    QTreeWidgetItem* item = m_layerTree->topLevelItem(i);
    if (item->data(0, layerIdRole).toString() == identifier) {
      m_layerTree->setCurrentItem(item);
      return true;
    }
  }
  return false;
}

// A dimension's name, then its value, as everywhere in WMTS.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
bool AddWmtsDialog::setDimension(const QString& identifier,
                                 const QString& value)
{
  for (const std::pair<QString, QComboBox*>& dimension : m_dimensionBoxes) {
    if (dimension.first.compare(identifier, Qt::CaseInsensitive) == 0) {
      dimension.second->setEditText(value);
      showDetails();
      return true;
    }
  }
  return false;
}

const WmtsLayer* AddWmtsDialog::chosenLayer() const
{
  const QTreeWidgetItem* item = m_layerTree->currentItem();
  if (!m_capabilities || item == nullptr) {
    return nullptr;
  }
  return m_capabilities->findLayer(item->data(0, layerIdRole).toString());
}

void AddWmtsDialog::layerChosen()
{
  const WmtsLayer* layer = chosenLayer();
  fillChoices(layer ? *layer : WmtsLayer{});
  showDetails();
}

void AddWmtsDialog::fillChoices(const WmtsLayer& layer)
{
  const QSignalBlocker styles(m_styleBox);
  const QSignalBlocker formats(m_formatBox);
  const QSignalBlocker sets(m_setBox);
  m_styleBox->clear();
  m_formatBox->clear();
  m_setBox->clear();
  for (const std::pair<QString, QComboBox*>& dimension : m_dimensionBoxes) {
    delete dimension.second;
  }
  for (QLabel* label : m_dimensionLabels) {
    delete label;
  }
  m_dimensionBoxes.clear();
  m_dimensionLabels.clear();
  if (!m_capabilities || layer.identifier.isEmpty()) {
    return;
  }
  for (const WmtsStyle& style : layer.styles) {
    m_styleBox->addItem(style.title.isEmpty() ? style.identifier : style.title,
                        style.identifier);
  }
  m_styleBox->setCurrentIndex(
      std::max(0, m_styleBox->findData(layer.defaultStyle())));
  QStringList offered = layer.formats;
  for (const WmtsResourceUrl& resource : layer.resourceUrls) {
    if (!offered.contains(resource.format)) {
      offered.append(resource.format);
    }
  }
  m_formatBox->addItems(offered);
  m_formatBox->setCurrentIndex(
      std::max(0, m_formatBox->findText(chooseFormat(offered, false))));
  for (const QString& set :
       usableTileMatrixSets(*m_capabilities, layer.identifier)) {
    m_setBox->addItem(setLabel(*m_capabilities, set), set);
  }
  // A value for each dimension: the default first, the others offered;
  // any value may be typed (TIME takes dates the list doesn't hold).
  for (const WmtsDimension& dimension : layer.dimensions) {
    QComboBox* box = new QComboBox(this);
    box->setObjectName(QStringLiteral("wmtsDimension_") + dimension.identifier);
    box->setEditable(true);
    box->setInsertPolicy(QComboBox::NoInsert);
    QStringList values = dimension.values;
    if (!dimension.defaultValue.isEmpty()) {
      values.removeAll(dimension.defaultValue);
      values.prepend(dimension.defaultValue);
    }
    box->addItems(values.mid(0, 500));
    box->setCurrentIndex(values.isEmpty() ? -1 : 0);
    connect(box, &QComboBox::currentTextChanged, this,
            &AddWmtsDialog::showDetails);
    const QString title =
        dimension.title.isEmpty() ? dimension.identifier : dimension.title;
    QLabel* label =
        new QLabel(dimension.unitOfMeasure.isEmpty()
                       ? tr("%1:").arg(title)
                       : tr("%1 (%2):").arg(title, dimension.unitOfMeasure),
                   this);
    label->setBuddy(box);
    const int32_t row = 3 + static_cast<int32_t>(m_dimensionBoxes.size());
    m_choicesLayout->addWidget(label, row, 0);
    m_choicesLayout->addWidget(box, row, 1);
    m_dimensionBoxes.emplace_back(dimension.identifier, box);
    m_dimensionLabels.push_back(label);
  }
}

void AddWmtsDialog::showDetails()
{
  const WmtsLayer* layer = chosenLayer();
  QString error;
  const std::optional<WmtsSource> source = chosenSource(&error);
  m_addButton->setEnabled(source.has_value());
  if (!layer || !m_capabilities) {
    m_detailsLabel->clear();
    return;
  }
  if (!source) {
    m_detailsLabel->setText(error.toHtmlEscaped());
    return;
  }
  QStringList details;
  QString drawn;
  if (source->imageProjection() == TileImageProjection::WebMercator) {
    drawn = tr("Drawn from Web Mercator tiles (%1), reprojected.")
                .arg(source->tileMatrixSet);
  } else if (isProjectedWmtsCrs(source->crs)) {
    drawn = tr("Drawn from %1 tiles in %2, reprojected.")
                .arg(source->tileMatrixSet, source->crs);
  } else {
    drawn = tr("Drawn from %1 tiles in %2.")
                .arg(source->tileMatrixSet, source->crs);
  }
  details << drawn.toHtmlEscaped();
  if (!source->attribution.isEmpty()) {
    details << tr("Credit: %1").arg(source->attribution).toHtmlEscaped();
  }
  if (!m_capabilities->fees.isEmpty() &&
      m_capabilities->fees.compare(QLatin1String("none"),
                                   Qt::CaseInsensitive) != 0) {
    details
        << tr("Fees: %1").arg(m_capabilities->fees.left(200)).toHtmlEscaped();
  }
  if (!m_capabilities->accessConstraints.isEmpty() &&
      m_capabilities->accessConstraints.compare(QLatin1String("none"),
                                                Qt::CaseInsensitive) != 0) {
    details << tr("The server's terms: %1")
                   .arg(m_capabilities->accessConstraints.left(400))
                   .toHtmlEscaped();
  }
  const std::vector<WmtsStyle>::const_iterator style =
      std::find_if(layer->styles.begin(), layer->styles.end(),
                   [&source](const WmtsStyle& candidate) {
                     return candidate.identifier == source->style;
                   });
  if (style != layer->styles.end() && style->legendUrl.isValid() &&
      !style->legendUrl.isEmpty()) {
    details << QStringLiteral("<a href=\"%1\">%2</a>")
                   .arg(style->legendUrl.toString(QUrl::FullyEncoded)
                            .toHtmlEscaped(),
                        tr("Legend"));
  }
  // Lines of rich text (the legend is a link).
  const QString html = details.join(QStringLiteral("<br>"));
  m_detailsLabel->setText(html);
}

std::optional<WmtsSource>
AddWmtsDialog::chosenSource(QString* errorString) const
{
  const WmtsLayer* layer = chosenLayer();
  if (!m_capabilities || layer == nullptr) {
    return std::nullopt;
  }
  WmtsChoices choices;
  choices.style = m_styleBox->currentData().toString();
  choices.format = m_formatBox->currentText();
  choices.tileMatrixSet = m_setBox->currentData().toString();
  for (const std::pair<QString, QComboBox*>& dimension : m_dimensionBoxes) {
    const QString value = dimension.second->currentText().trimmed();
    if (!value.isEmpty()) {
      choices.dimensions.emplace_back(dimension.first, value);
    }
  }
  std::optional<WmtsSource> source =
      wmtsSourceFor(*m_capabilities, layer->identifier, choices, errorString);
  if (source) {
    source->service = m_serviceUrl;
    // Credit as the catalogue says when the service is in it: the wording
    // the provider asks for.
    const WmsCatalogueEntry* entry =
        findWmsCatalogueEntry(m_serviceUrl, m_catalogue);
    if (entry != nullptr && !entry->attribution.isEmpty()) {
      source->attribution = entry->attribution;
    }
  }
  return source;
}

void AddWmtsDialog::accept()
{
  QString error;
  const std::optional<WmtsSource> source = chosenSource(&error);
  if (!source) {
    return;
  }
  LayerDefinition definition;
  definition.id = QStringLiteral("wmts:") +
                  QUuid::createUuid().toString(QUuid::WithoutBraces);
  definition.origin = LayerOrigin::Catalogue;
  definition.type = LayerType::Imagery;
  definition.name = source->title;
  definition.source = source->toJson();
  m_definition = definition;
  QDialog::accept();
}

void AddWmtsDialog::setStatus(const QString& text, bool error)
{
  m_status = text;
  m_statusIsError = error;
  m_statusLabel->setText(text);
  m_statusLabel->setStyleSheet(error ? QStringLiteral("color: #c0392b;")
                                     : QString());
}

QString AddWmtsDialog::statusText() const
{
  return m_status;
}

QString AddWmtsDialog::detailsText() const
{
  return m_detailsLabel->text();
}

bool AddWmtsDialog::isBusy() const
{
  return m_capabilitiesTicket != nullptr;
}

} // namespace ts
