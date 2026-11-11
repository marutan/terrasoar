// SPDX-License-Identifier: GPL-2.0-or-later
#include "addwmsdialog.h"

#include <algorithm>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTimer>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

#include "ts/network/networkclient.h"
#include "ts/wms/wmssource.h"

namespace ts
{

namespace
{

/** How often running requests are looked at, ms. */
constexpr int pollIntervalMs = 50;

/** Item data: the server's layer name (empty for groups). */
constexpr int layerNameRole = Qt::UserRole + 1;

QString catalogueCachePath()
{
  return QDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
      .filePath(QStringLiteral("wms-catalogue.json"));
}

QUrl catalogueSourceUrl()
{
  if (qEnvironmentVariableIsSet("TS_WMS_CATALOGUE_URL")) {
    return QUrl(qEnvironmentVariable("TS_WMS_CATALOGUE_URL"));
  }
  return QUrl(QString::fromLatin1(wmsCatalogueUrl));
}

std::vector<WmsCatalogueEntry> readCatalogue(const QString& path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return {};
  }
  QString error;
  return parseWmsCatalogue(file.readAll(), &error)
      .value_or(std::vector<WmsCatalogueEntry>{});
}

bool isImageFormat(const QString& format)
{
  return format.contains(QLatin1String("png"), Qt::CaseInsensitive) ||
         format.contains(QLatin1String("jpeg"), Qt::CaseInsensitive) ||
         format.contains(QLatin1String("gif"), Qt::CaseInsensitive);
}

QTreeWidgetItem* addLayerItems(const WmsLayer& layer, QTreeWidgetItem* parent,
                               QTreeWidget* tree)
{
  QTreeWidgetItem* item =
      parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
  item->setText(0, layer.title.isEmpty() ? layer.name : layer.title);
  item->setData(0, layerNameRole, layer.name);
  if (layer.name.isEmpty()) {
    // A group: open it, but there is nothing to request.
    item->setFlags(Qt::ItemIsEnabled);
  } else {
    // The server's name for it, then its description (both its own text).
    QStringList tip{layer.name};
    if (!layer.abstract.isEmpty()) {
      tip.append(layer.abstract.left(600));
    }
    item->setToolTip(0, tip.join(QLatin1Char('\n')));
  }
  for (const WmsLayer& child : layer.children) {
    addLayerItems(child, item, tree);
  }
  return item;
}

} // namespace

AddWmsDialog::AddWmsDialog(std::shared_ptr<NetworkClient> network,
                           QWidget* parent)
    : QDialog(parent), m_network(std::move(network))
{
  setObjectName(QStringLiteral("addWmsDialog"));
  m_serviceLabel = new QLabel(this);
  m_serviceBox = new QComboBox(this);
  m_serviceBox->setObjectName(QStringLiteral("wmsServiceBox"));
  m_serviceBox->setEditable(true);
  m_serviceBox->setInsertPolicy(QComboBox::NoInsert);
  m_serviceBox->setMinimumContentsLength(40);
  m_serviceLabel->setBuddy(m_serviceBox);
  m_connectButton = new QPushButton(this);
  m_connectButton->setObjectName(QStringLiteral("wmsConnectButton"));
  m_aboutLabel = new QLabel(this);
  m_aboutLabel->setObjectName(QStringLiteral("wmsAboutLabel"));
  m_aboutLabel->setWordWrap(true);
  m_aboutLabel->setOpenExternalLinks(true);
  m_aboutLabel->setTextFormat(Qt::RichText);
  m_statusLabel = new QLabel(this);
  m_statusLabel->setObjectName(QStringLiteral("wmsStatusLabel"));
  m_statusLabel->setWordWrap(true);
  m_layersLabel = new QLabel(this);
  m_layerTree = new QTreeWidget(this);
  m_layerTree->setObjectName(QStringLiteral("wmsLayerTree"));
  m_layerTree->setHeaderHidden(true);
  m_layerTree->setColumnCount(1);
  m_layersLabel->setBuddy(m_layerTree);
  m_styleLabel = new QLabel(this);
  m_styleBox = new QComboBox(this);
  m_styleBox->setObjectName(QStringLiteral("wmsStyleBox"));
  m_styleLabel->setBuddy(m_styleBox);
  m_formatLabel = new QLabel(this);
  m_formatBox = new QComboBox(this);
  m_formatBox->setObjectName(QStringLiteral("wmsFormatBox"));
  m_formatLabel->setBuddy(m_formatBox);
  m_detailsLabel = new QLabel(this);
  m_detailsLabel->setObjectName(QStringLiteral("wmsDetailsLabel"));
  m_detailsLabel->setWordWrap(true);
  m_buttons = new QDialogButtonBox(this);
  m_addButton = m_buttons->addButton(QString(), QDialogButtonBox::AcceptRole);
  m_addButton->setObjectName(QStringLiteral("wmsAddButton"));
  m_addButton->setEnabled(false);
  m_buttons->addButton(QDialogButtonBox::Cancel);
  m_pollTimer = new QTimer(this);
  m_pollTimer->setInterval(pollIntervalMs);

  QHBoxLayout* service = new QHBoxLayout;
  service->addWidget(m_serviceBox, 1);
  service->addWidget(m_connectButton);
  QGridLayout* choices = new QGridLayout;
  choices->addWidget(m_styleLabel, 0, 0);
  choices->addWidget(m_styleBox, 0, 1);
  choices->addWidget(m_formatLabel, 1, 0);
  choices->addWidget(m_formatBox, 1, 1);
  choices->setColumnStretch(1, 1);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(m_serviceLabel);
  layout->addLayout(service);
  layout->addWidget(m_aboutLabel);
  layout->addWidget(m_statusLabel);
  layout->addWidget(m_layersLabel);
  layout->addWidget(m_layerTree, 1);
  layout->addLayout(choices);
  layout->addWidget(m_detailsLabel);
  layout->addWidget(m_buttons);

  connect(m_connectButton, &QPushButton::clicked, this,
          &AddWmsDialog::startConnect);
  connect(m_serviceBox->lineEdit(), &QLineEdit::returnPressed, this,
          &AddWmsDialog::startConnect);
  connect(m_serviceBox, qOverload<int>(&QComboBox::activated), this,
          &AddWmsDialog::serviceChosen);
  connect(m_layerTree, &QTreeWidget::currentItemChanged, this,
          &AddWmsDialog::layerChosen);
  connect(m_styleBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
          &AddWmsDialog::layerChosen);
  connect(m_formatBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
          &AddWmsDialog::layerChosen);
  connect(m_buttons, &QDialogButtonBox::accepted, this, &AddWmsDialog::accept);
  connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_pollTimer, &QTimer::timeout, this, &AddWmsDialog::poll);

  loadCatalogue();
  retranslateUi();
  resize(640, 560);
}

AddWmsDialog::~AddWmsDialog()
{
  for (const std::shared_ptr<NetworkTicket>& ticket :
       {m_capabilitiesTicket, m_catalogueTicket}) {
    if (ticket) {
      ticket->cancel();
    }
  }
}

void AddWmsDialog::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QDialog::changeEvent(event);
}

void AddWmsDialog::retranslateUi()
{
  setWindowTitle(tr("Add WMS Layer"));
  m_serviceLabel->setText(tr("&Service (choose one or type its address):"));
  m_connectButton->setText(tr("&Connect"));
  m_layersLabel->setText(tr("&Layers:"));
  m_styleLabel->setText(tr("S&tyle:"));
  m_formatLabel->setText(tr("&Format:"));
  m_addButton->setText(tr("&Add"));
  m_serviceBox->lineEdit()->setPlaceholderText(tr("https://example.org/wms"));
  if (m_status.isEmpty()) {
    m_statusLabel->setText(tr("Connect to see the layers the server offers."));
  }
  if (m_styleBox->count() > 0) {
    m_styleBox->setItemText(0, tr("Default"));
  }
  serviceChosen(m_serviceBox->currentIndex());
  layerChosen();
}

// --- Catalogue
// ----------------------------------------------------------------

void AddWmsDialog::loadCatalogue()
{
  // The last copy fetched, else the one built in; then ask for the current.
  m_catalogue = readCatalogue(catalogueCachePath());
  if (m_catalogue.empty()) {
    m_catalogue = readCatalogue(QStringLiteral(":/data/wms-catalogue.json"));
  }
  fillCatalogueBox();
  const QUrl source = catalogueSourceUrl();
  if (m_network && source.isValid() && !source.isEmpty()) {
    m_catalogueTicket = m_network->start(HttpRequest{source, {}});
    m_pollTimer->start();
  }
}

void AddWmsDialog::fillCatalogueBox()
{
  const QString typed = m_serviceBox->currentText();
  {
    const QSignalBlocker blocker(m_serviceBox);
    m_serviceBox->clear();
    for (const WmsCatalogueEntry& entry : m_catalogue) {
      m_serviceBox->addItem(entry.title, entry.url.toString());
    }
    m_serviceBox->setCurrentIndex(-1);
    m_serviceBox->setEditText(typed);
  }
}

void AddWmsDialog::catalogueArrived()
{
  const HttpResponse response = m_catalogueTicket->response();
  m_catalogueTicket.reset();
  if (!response.isSuccess()) {
    return; // offline: the copy we have will do
  }
  QString error;
  const std::optional<std::vector<WmsCatalogueEntry>> entries =
      parseWmsCatalogue(response.body, &error);
  if (!entries || entries->empty()) {
    return;
  }
  m_catalogue = *entries;
  QDir().mkpath(QFileInfo(catalogueCachePath()).absolutePath());
  QSaveFile file(catalogueCachePath());
  if (file.open(QIODevice::WriteOnly)) {
    file.write(response.body);
    file.commit();
  }
  fillCatalogueBox();
  emit catalogueUpdated();
}

void AddWmsDialog::serviceChosen(int index)
{
  if (index < 0 || index >= static_cast<int>(m_catalogue.size())) {
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

void AddWmsDialog::connectTo(const QString& address)
{
  m_serviceBox->setEditText(address);
  startConnect();
}

void AddWmsDialog::startConnect()
{
  const QUrl url = QUrl::fromUserInput(m_serviceBox->currentText().trimmed());
  if (!url.isValid() || (url.scheme() != QLatin1String("http") &&
                         url.scheme() != QLatin1String("https"))) {
    setStatus(tr("Enter the address of a WMS server, starting with "
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
  m_styleBox->clear();
  m_formatBox->clear();
  m_detailsLabel->clear();
  m_addButton->setEnabled(false);
  requestCapabilities(QStringLiteral("1.3.0"));
}

void AddWmsDialog::requestCapabilities(const QString& version)
{
  if (m_capabilitiesTicket) {
    m_capabilitiesTicket->cancel();
  }
  m_requestedVersion = version;
  m_capabilitiesTicket =
      m_network->start(HttpRequest{capabilitiesUrl(m_serviceUrl, version), {}});
  setStatus(tr("Connecting to %1…").arg(m_serviceUrl.host()), false);
  m_connectButton->setEnabled(false);
  m_pollTimer->start();
}

void AddWmsDialog::poll()
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

void AddWmsDialog::capabilitiesArrived()
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
  std::optional<WmsCapabilities> parsed =
      response.status >= 200 && response.status < 300
          ? parseWmsCapabilities(response.body, &error)
          : std::nullopt;
  if (!parsed && m_requestedVersion != QLatin1String("1.1.1")) {
    // Older servers refuse 1.3.0 rather than answering in 1.1.1.
    requestCapabilities(QStringLiteral("1.1.1"));
    return;
  }
  if (!parsed) {
    setStatus(error.isEmpty()
                  ? tr("The server answered: %1").arg(response.errorString)
                  : error,
              true);
    emit connectionFailed();
    return;
  }
  m_capabilities = std::move(parsed);
  fillLayers();
  setStatus(tr("%1: %n layer(s).", nullptr,
               static_cast<int>(m_capabilities->namedLayers().size()))
                .arg(m_capabilities->title.isEmpty() ? m_serviceUrl.host()
                                                     : m_capabilities->title),
            false);
  emit capabilitiesLoaded();
}

void AddWmsDialog::fillLayers()
{
  m_layerTree->clear();
  for (const WmsLayer& layer : m_capabilities->layers) {
    addLayerItems(layer, nullptr, m_layerTree);
  }
  m_layerTree->expandToDepth(1);
  QStringList formats;
  for (const QString& format : m_capabilities->getMapFormats) {
    if (isImageFormat(format) && !formats.contains(format)) {
      formats.append(format);
    }
  }
  const QSignalBlocker blocker(m_formatBox);
  m_formatBox->clear();
  m_formatBox->addItems(formats);
}

bool AddWmsDialog::selectLayer(const QString& name)
{
  const QList<QTreeWidgetItem*> items = m_layerTree->findItems(
      QStringLiteral("*"), Qt::MatchWildcard | Qt::MatchRecursive);
  for (QTreeWidgetItem* item : items) {
    if (item->data(0, layerNameRole).toString() == name) {
      m_layerTree->setCurrentItem(item);
      return true;
    }
  }
  return false;
}

void AddWmsDialog::layerChosen()
{
  if (!m_capabilities) {
    return;
  }
  const QTreeWidgetItem* item = m_layerTree->currentItem();
  const QString name =
      item ? item->data(0, layerNameRole).toString() : QString();
  const WmsLayer* layer =
      name.isEmpty() ? nullptr : m_capabilities->findLayer(name);
  if (sender() == m_layerTree) {
    // A new layer: its styles, and the format that suits it.
    const QSignalBlocker styles(m_styleBox);
    const QSignalBlocker formats(m_formatBox);
    m_styleBox->clear();
    if (layer) {
      m_styleBox->addItem(tr("Default"), QString());
      for (const WmsStyle& style : layer->styles) {
        m_styleBox->addItem(style.title.isEmpty() ? style.name : style.title,
                            style.name);
      }
      QStringList offered;
      for (int i = 0; i < m_formatBox->count(); ++i) {
        offered.append(m_formatBox->itemText(i));
      }
      const int preferred =
          m_formatBox->findText(chooseFormat(offered, layer->opaque));
      m_formatBox->setCurrentIndex(std::max(preferred, 0));
    }
  }
  QString error;
  const std::optional<WmsSource> source = chosenSource(&error);
  m_addButton->setEnabled(source.has_value());
  if (!layer) {
    m_detailsLabel->setText(item ? tr("Choose a layer within this group.")
                                 : QString());
    return;
  }
  if (!source) {
    m_detailsLabel->setText(error);
    return;
  }
  QStringList details;
  details << (source->imageProjection() == TileImageProjection::WebMercator
                  ? tr("Drawn from Web Mercator images (%1), reprojected.")
                        .arg(source->crs)
                  : tr("Drawn in %1.").arg(source->crs));
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

std::optional<WmsSource> AddWmsDialog::chosenSource(QString* errorString) const
{
  const QTreeWidgetItem* item = m_layerTree->currentItem();
  if (!m_capabilities || !item ||
      item->data(0, layerNameRole).toString().isEmpty()) {
    return std::nullopt;
  }
  std::optional<WmsSource> source = wmsSourceFor(
      *m_capabilities, QStringList{item->data(0, layerNameRole).toString()},
      QStringList{m_styleBox->currentData().toString()},
      m_formatBox->currentText(), errorString);
  if (source && source->attribution.isEmpty()) {
    // Credit as the catalogue says, if the layer doesn't.
    for (const WmsCatalogueEntry& entry : m_catalogue) {
      if (entry.url.host() == m_serviceUrl.host()) {
        source->attribution = entry.attribution;
      }
    }
  }
  return source;
}

void AddWmsDialog::accept()
{
  QString error;
  const std::optional<WmsSource> source = chosenSource(&error);
  if (!source) {
    return;
  }
  LayerDefinition definition;
  definition.id = QStringLiteral("wms:") +
                  QUuid::createUuid().toString(QUuid::WithoutBraces);
  definition.origin = LayerOrigin::Catalogue;
  definition.type = LayerType::Imagery;
  definition.name = source->title;
  definition.source = source->toJson();
  m_definition = definition;
  QDialog::accept();
}

void AddWmsDialog::setStatus(const QString& text, bool error)
{
  m_status = text;
  m_statusIsError = error;
  m_statusLabel->setText(text);
  m_statusLabel->setStyleSheet(error ? QStringLiteral("color: #c0392b;")
                                     : QString());
}

QString AddWmsDialog::statusText() const
{
  return m_status;
}

bool AddWmsDialog::isBusy() const
{
  return m_capabilitiesTicket != nullptr;
}

} // namespace ts
