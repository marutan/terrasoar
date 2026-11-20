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
#include "addxyzdialog.h"

#include <cstdint>
#include <utility>

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QShortcut>
#include <QSpinBox>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include "helpwindow.h"
#include "ts/network/networkclient.h"
#include "wmscataloguefile.h"

namespace ts
{

namespace
{

/** How often the catalogue request is looked at, ms. */
constexpr int32_t pollIntervalMs = 50;

/** Deepest zoom offered. */
constexpr int32_t deepestZoom = 30;

QUrl catalogueSourceUrl()
{
  if (qEnvironmentVariableIsSet("TS_XYZ_CATALOGUE_URL")) {
    return QUrl(qEnvironmentVariable("TS_XYZ_CATALOGUE_URL"));
  }
  return QUrl(QString::fromLatin1(xyzCatalogueUrl));
}

QStringList subdomainList(const QString& text)
{
  QStringList list;
  static const QRegularExpression separators(QStringLiteral("[,\\s]+"));
  const QStringList parts = text.split(separators, Qt::SkipEmptyParts);
  for (const QString& part : parts) {
    list.append(part.trimmed());
  }
  return list;
}

} // namespace

QString AddXyzDialog::helpTarget()
{
  return QStringLiteral("adding-xyz-layers.md#adding-a-layer");
}

AddXyzDialog::AddXyzDialog(std::shared_ptr<NetworkClient> network,
                           QWidget* parent)
    : QDialog(parent), m_network(std::move(network)),
      m_serviceLabel(new QLabel(this)), m_serviceBox(new QComboBox(this)),
      m_aboutLabel(new QLabel(this)), m_nameLabel(new QLabel(this)),
      m_nameEdit(new QLineEdit(this)), m_templateLabel(new QLabel(this)),
      m_templateEdit(new QLineEdit(this)), m_subdomainsLabel(new QLabel(this)),
      m_subdomainsEdit(new QLineEdit(this)), m_tmsBox(new QCheckBox(this)),
      m_zoomLabel(new QLabel(this)), m_minimumZoomBox(new QSpinBox(this)),
      m_maximumZoomBox(new QSpinBox(this)),
      m_attributionLabel(new QLabel(this)),
      m_attributionEdit(new QLineEdit(this)), m_statusLabel(new QLabel(this)),
      m_buttons(new QDialogButtonBox(this)), m_pollTimer(new QTimer(this))
{
  QShortcut* help = new QShortcut(QKeySequence::HelpContents, this);
  connect(help, &QShortcut::activated, this,
          [this]() { showUserGuide(this, helpTarget()); });
  setObjectName(QStringLiteral("addXyzDialog"));
  m_serviceBox->setObjectName(QStringLiteral("xyzServiceBox"));
  m_serviceLabel->setBuddy(m_serviceBox);
  m_aboutLabel->setObjectName(QStringLiteral("xyzAboutLabel"));
  m_aboutLabel->setWordWrap(true);
  m_aboutLabel->setOpenExternalLinks(true);
  m_aboutLabel->setTextFormat(Qt::RichText);
  m_nameEdit->setObjectName(QStringLiteral("xyzNameEdit"));
  m_nameLabel->setBuddy(m_nameEdit);
  m_templateEdit->setObjectName(QStringLiteral("xyzTemplateEdit"));
  m_templateEdit->setMinimumWidth(420);
  m_templateLabel->setBuddy(m_templateEdit);
  m_subdomainsEdit->setObjectName(QStringLiteral("xyzSubdomainsEdit"));
  m_subdomainsLabel->setBuddy(m_subdomainsEdit);
  m_tmsBox->setObjectName(QStringLiteral("xyzTmsBox"));
  for (QSpinBox* box : {m_minimumZoomBox, m_maximumZoomBox}) {
    box->setRange(0, deepestZoom);
  }
  m_minimumZoomBox->setObjectName(QStringLiteral("xyzMinimumZoomBox"));
  m_maximumZoomBox->setObjectName(QStringLiteral("xyzMaximumZoomBox"));
  m_maximumZoomBox->setValue(19);
  m_zoomLabel->setBuddy(m_minimumZoomBox);
  m_attributionEdit->setObjectName(QStringLiteral("xyzAttributionEdit"));
  m_attributionLabel->setBuddy(m_attributionEdit);
  m_statusLabel->setObjectName(QStringLiteral("xyzStatusLabel"));
  m_statusLabel->setWordWrap(true);
  m_statusLabel->setStyleSheet(QStringLiteral("color: #c0392b;"));
  m_addButton = m_buttons->addButton(QString(), QDialogButtonBox::AcceptRole);
  m_addButton->setObjectName(QStringLiteral("xyzAddButton"));
  m_addButton->setEnabled(false);
  m_buttons->addButton(QDialogButtonBox::Cancel);
  m_pollTimer->setInterval(pollIntervalMs);

  QHBoxLayout* zooms = new QHBoxLayout;
  zooms->addWidget(m_minimumZoomBox);
  zooms->addWidget(new QLabel(QStringLiteral("–"), this));
  zooms->addWidget(m_maximumZoomBox);
  zooms->addStretch(1);
  QFormLayout* form = new QFormLayout;
  form->addRow(m_nameLabel, m_nameEdit);
  form->addRow(m_templateLabel, m_templateEdit);
  form->addRow(m_subdomainsLabel, m_subdomainsEdit);
  form->addRow(QString(), m_tmsBox);
  form->addRow(m_zoomLabel, zooms);
  form->addRow(m_attributionLabel, m_attributionEdit);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(m_serviceLabel);
  layout->addWidget(m_serviceBox);
  layout->addWidget(m_aboutLabel);
  layout->addLayout(form);
  layout->addWidget(m_statusLabel);
  layout->addStretch(1);
  layout->addWidget(m_buttons);

  connect(m_serviceBox, qOverload<int32_t>(&QComboBox::activated), this,
          &AddXyzDialog::serviceChosen);
  for (QLineEdit* edit : {m_nameEdit, m_templateEdit, m_subdomainsEdit}) {
    connect(edit, &QLineEdit::textChanged, this, &AddXyzDialog::check);
  }
  for (QSpinBox* box : {m_minimumZoomBox, m_maximumZoomBox}) {
    connect(box, qOverload<int32_t>(&QSpinBox::valueChanged), this,
            &AddXyzDialog::check);
  }
  connect(m_buttons, &QDialogButtonBox::accepted, this, &AddXyzDialog::accept);
  connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_pollTimer, &QTimer::timeout, this, &AddXyzDialog::poll);

  loadCatalogue();
  retranslateUi();
  check();
}

AddXyzDialog::~AddXyzDialog()
{
  if (m_catalogueTicket) {
    m_catalogueTicket->cancel();
  }
}

void AddXyzDialog::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QDialog::changeEvent(event);
}

void AddXyzDialog::retranslateUi()
{
  setWindowTitle(tr("Add XYZ Tile Layer"));
  m_serviceLabel->setText(tr("&Service (choose one, or fill in the fields):"));
  m_serviceBox->setPlaceholderText(tr("Another service"));
  m_nameLabel->setText(tr("&Name:"));
  m_templateLabel->setText(tr("&Address:"));
  m_templateEdit->setPlaceholderText(
      tr("https://tile.example.org/{z}/{x}/{y}.png"));
  m_subdomainsLabel->setText(tr("S&ubdomains for {s}:"));
  m_subdomainsEdit->setPlaceholderText(tr("a, b, c"));
  m_tmsBox->setText(tr("Rows are counted from the south (&TMS)"));
  m_zoomLabel->setText(tr("&Zoom levels:"));
  m_attributionLabel->setText(tr("&Credit:"));
  m_addButton->setText(tr("&Add"));
  check();
}

void AddXyzDialog::loadCatalogue()
{
  m_catalogue = currentXyzCatalogue();
  fillCatalogueBox();
  const QUrl address = catalogueSourceUrl();
  if (m_network && address.isValid() && !address.isEmpty()) {
    m_catalogueTicket = m_network->start(HttpRequest{address, {}});
    m_pollTimer->start();
  }
}

void AddXyzDialog::fillCatalogueBox()
{
  const QSignalBlocker blocker(m_serviceBox);
  m_serviceBox->clear();
  for (const XyzCatalogueEntry& entry : m_catalogue) {
    m_serviceBox->addItem(entry.title);
  }
  m_serviceBox->setCurrentIndex(-1);
}

void AddXyzDialog::poll()
{
  if (m_catalogueTicket && m_catalogueTicket->isFinished()) {
    catalogueArrived();
  }
  if (!m_catalogueTicket) {
    m_pollTimer->stop();
  }
}

void AddXyzDialog::catalogueArrived()
{
  const HttpResponse response = m_catalogueTicket->response();
  m_catalogueTicket.reset();
  if (!response.isSuccess()) {
    return; // offline: the copy we have will do
  }
  QString error;
  const std::optional<std::vector<XyzCatalogueEntry>> entries =
      parseXyzCatalogue(response.body, &error);
  if (!entries || entries->empty()) {
    return;
  }
  const int32_t chosen = m_serviceBox->currentIndex();
  const QString chosenTitle = m_serviceBox->currentText();
  m_catalogue = *entries;
  QDir().mkpath(QFileInfo(xyzCatalogueCachePath()).absolutePath());
  QSaveFile file(xyzCatalogueCachePath());
  if (file.open(QIODevice::WriteOnly)) {
    file.write(response.body);
    file.commit();
  }
  fillCatalogueBox();
  if (chosen >= 0) {
    m_serviceBox->setCurrentIndex(m_serviceBox->findText(chosenTitle));
  }
  emit catalogueUpdated();
}

bool AddXyzDialog::chooseService(int32_t index)
{
  if (index < 0 || index >= static_cast<int32_t>(m_catalogue.size())) {
    return false;
  }
  m_serviceBox->setCurrentIndex(index);
  serviceChosen(index);
  return true;
}

void AddXyzDialog::serviceChosen(int32_t index)
{
  if (index < 0 || index >= static_cast<int32_t>(m_catalogue.size())) {
    m_aboutLabel->clear();
    return;
  }
  const XyzCatalogueEntry& entry =
      m_catalogue.at(static_cast<std::size_t>(index));
  m_nameEdit->setText(entry.title);
  m_templateEdit->setText(entry.urlTemplate);
  m_subdomainsEdit->setText(entry.subdomains.join(QStringLiteral(", ")));
  m_tmsBox->setChecked(entry.tms);
  m_minimumZoomBox->setValue(entry.minimumZoom);
  m_maximumZoomBox->setValue(entry.maximumZoom);
  m_attributionEdit->setText(entry.attribution);
  QString about = entry.description.toHtmlEscaped();
  if (entry.termsUrl.isValid() && !entry.termsUrl.isEmpty()) {
    about +=
        QStringLiteral("<br><a href=\"%1\">%2</a>")
            .arg(entry.termsUrl.toString(QUrl::FullyEncoded).toHtmlEscaped(),
                 tr("Terms of use"));
  }
  if (entry.allowBulkDownload == std::optional<bool>(false)) {
    about += QStringLiteral("<br>") +
             tr("Its terms don't allow keeping areas for offline use.")
                 .toHtmlEscaped();
  }
  m_aboutLabel->setText(about);
  check();
}

std::optional<XyzSource> AddXyzDialog::source() const
{
  XyzSource xyz;
  xyz.urlTemplate = m_templateEdit->text().trimmed();
  xyz.subdomains = subdomainList(m_subdomainsEdit->text());
  if (!checkXyzTemplate(xyz.urlTemplate, xyz.subdomains).isEmpty() ||
      m_nameEdit->text().trimmed().isEmpty() ||
      m_minimumZoomBox->value() > m_maximumZoomBox->value()) {
    return std::nullopt;
  }
  if (!xyz.urlTemplate.contains(QLatin1String("{s}"), Qt::CaseInsensitive)) {
    xyz.subdomains.clear();
  }
  xyz.tms = m_tmsBox->isChecked();
  xyz.minimumZoom = m_minimumZoomBox->value();
  xyz.maximumZoom = m_maximumZoomBox->value();
  xyz.format = xyzFormatFor(xyz.urlTemplate);
  xyz.title = m_nameEdit->text().trimmed();
  xyz.attribution = m_attributionEdit->text().trimmed();
  xyz.service = xyzServiceUrl(xyz.urlTemplate, xyz.subdomains);
  return xyz;
}

void AddXyzDialog::check()
{
  const QString address = m_templateEdit->text().trimmed();
  QString problem;
  if (!address.isEmpty()) {
    problem =
        checkXyzTemplate(address, subdomainList(m_subdomainsEdit->text()));
  }
  if (problem.isEmpty() &&
      m_minimumZoomBox->value() > m_maximumZoomBox->value()) {
    problem = tr("The first zoom level can't be deeper than the last.");
  }
  if (problem.isEmpty() && !address.isEmpty() &&
      m_nameEdit->text().trimmed().isEmpty()) {
    problem = tr("Give the layer a name.");
  }
  m_status = problem;
  m_statusLabel->setText(problem);
  m_addButton->setEnabled(source().has_value());
}

void AddXyzDialog::accept()
{
  const std::optional<XyzSource> xyz = source();
  if (!xyz) {
    return;
  }
  LayerDefinition definition;
  definition.id = QStringLiteral("xyz:") +
                  QUuid::createUuid().toString(QUuid::WithoutBraces);
  definition.origin = LayerOrigin::Catalogue;
  definition.type = LayerType::Imagery;
  definition.name = xyz->title;
  definition.source = xyz->toJson();
  m_definition = definition;
  QDialog::accept();
}

QString AddXyzDialog::statusText() const
{
  return m_status;
}

} // namespace ts
