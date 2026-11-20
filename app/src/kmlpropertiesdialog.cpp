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
#include "kmlpropertiesdialog.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QShortcut>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextBrowser>
#include <QTimeZone>
#include <QVBoxLayout>

#include "helpwindow.h"
#include "ts/kml/kmledit.h"
#include "ts/kml/kmlpresentation.h"
#include "ts/kml/kmlreader.h"

namespace ts
{

namespace
{

/** The altitude modes in the order the combo box lists them. */
const std::vector<KmlAltitudeMode>& altitudeModes()
{
  static const std::vector<KmlAltitudeMode> modes{
      KmlAltitudeMode::ClampToGround, KmlAltitudeMode::RelativeToGround,
      KmlAltitudeMode::Absolute, KmlAltitudeMode::ClampToSeaFloor,
      KmlAltitudeMode::RelativeToSeaFloor};
  return modes;
}

/** Every coordinate of @p geometry, to change. */
void forEachCoordinate(KmlGeometry& geometry,
                       const std::function<void(KmlCoordinate&)>& visit)
{
  for (KmlCoordinate& c : geometry.coordinates) {
    visit(c);
  }
  for (std::vector<KmlCoordinate>& ring : geometry.inner) {
    for (KmlCoordinate& c : ring) {
      visit(c);
    }
  }
  for (KmlGeometry& child : geometry.children) {
    forEachCoordinate(child, visit);
  }
}

void setGeometryHeight(KmlGeometry& geometry, KmlAltitudeMode mode,
                       bool extrude)
{
  geometry.altitudeMode = mode;
  geometry.extrude = extrude;
  for (KmlGeometry& child : geometry.children) {
    setGeometryHeight(child, mode, extrude);
  }
}

QDoubleSpinBox* spin(QWidget* parent, double low, double high, double step,
                     int32_t decimals)
{
  QDoubleSpinBox* box = new QDoubleSpinBox(parent);
  box->setRange(low, high);
  box->setSingleStep(step);
  box->setDecimals(decimals);
  return box;
}

} // namespace

QString KmlPropertiesDialog::helpTarget()
{
  return QStringLiteral("kml.md#editing");
}

KmlPropertiesDialog::KmlPropertiesDialog(const KmlFeature& feature,
                                         const KmlDocument& document,
                                         std::function<KmlView()> currentView,
                                         QWidget* parent)
    : QDialog(parent), m_feature(feature), m_document(&document),
      m_currentView(std::move(currentView)), m_view(feature.view)
{
  setObjectName(QStringLiteral("kmlPropertiesDialog"));
  setWindowTitle(
      tr("Properties of %1")
          .arg(feature.name.isEmpty() ? tr("the element") : feature.name));
  QShortcut* help = new QShortcut(QKeySequence::HelpContents, this);
  connect(help, &QShortcut::activated, this,
          [this]() { showUserGuide(this, helpTarget()); });
  QTabWidget* tabs = new QTabWidget(this);
  tabs->addTab(makeGeneral(), tr("General"));
  tabs->addTab(makeStyle(), tr("Style"));
  if (feature.geometry) {
    tabs->addTab(makePosition(), tr("Height"));
  }
  tabs->addTab(makeTime(), tr("Time"));
  tabs->addTab(makeData(), tr("Data"));
  QDialogButtonBox* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(tabs);
  layout->addWidget(buttons);
  updatePreview();
}

KmlPropertiesDialog::~KmlPropertiesDialog() = default;

QWidget* KmlPropertiesDialog::makeGeneral()
{
  QWidget* page = new QWidget(this);
  m_name = new QLineEdit(m_feature.name, page);
  m_name->setObjectName(QStringLiteral("kmlName"));
  m_description = new QPlainTextEdit(m_feature.description, page);
  m_description->setObjectName(QStringLiteral("kmlDescription"));
  m_preview = new QTextBrowser(page);
  m_preview->setObjectName(QStringLiteral("kmlPreview"));
  m_preview->setOpenLinks(false);
  m_viewLabel = new QLabel(page);
  QPushButton* setView = new QPushButton(tr("Set from the &View"), page);
  setView->setObjectName(QStringLiteral("kmlSetView"));
  QPushButton* clearView = new QPushButton(tr("C&lear"), page);
  connect(setView, &QPushButton::clicked, this, [this]() {
    if (m_currentView) {
      m_view = m_currentView();
      updateView();
    }
  });
  connect(clearView, &QPushButton::clicked, this, [this]() {
    m_view.reset();
    updateView();
  });
  connect(m_name, &QLineEdit::textChanged, this,
          &KmlPropertiesDialog::updatePreview);
  connect(m_description, &QPlainTextEdit::textChanged, this,
          &KmlPropertiesDialog::updatePreview);
  QHBoxLayout* view = new QHBoxLayout;
  view->addWidget(m_viewLabel, 1);
  view->addWidget(setView);
  view->addWidget(clearView);
  QFormLayout* form = new QFormLayout(page);
  form->addRow(tr("&Name:"), m_name);
  form->addRow(tr("&Description (HTML):"), m_description);
  form->addRow(tr("Balloon:"), m_preview);
  form->addRow(tr("View:"), view);
  updateView();
  return page;
}

QWidget* KmlPropertiesDialog::makeStyle()
{
  QWidget* page = new QWidget(this);
  m_style = new QComboBox(page);
  m_style->setObjectName(QStringLiteral("kmlStyleChoice"));
  m_style->addItem(tr("This element's own"), QString());
  for (const KmlStyle& shared : m_document->root.styles) {
    if (!shared.id.isEmpty()) {
      m_style->addItem(tr("Shared: %1 (used by %n element(s))", nullptr,
                          kmlStyleUsersOf(shared.id))
                           .arg(shared.id),
                       shared.id);
    }
  }
  m_icon = new QLineEdit(page);
  m_icon->setObjectName(QStringLiteral("kmlIcon"));
  m_icon->setPlaceholderText(tr("The standard pushpin"));
  QPushButton* chooseIcon = new QPushButton(tr("C&hoose…"), page);
  connect(chooseIcon, &QPushButton::clicked, this, [this]() {
    const QString path = QFileDialog::getOpenFileName(
        this, tr("Choose an Icon"), QString(),
        tr("Pictures (*.png *.jpg *.jpeg *.gif *.svg);;All files (*)"));
    if (!path.isEmpty()) {
      m_icon->setText(path);
    }
  });
  m_iconColorButton = new QPushButton(page);
  m_iconScale = spin(page, 0.0, 10.0, 0.1, 1);
  m_labelColorButton = new QPushButton(page);
  m_labelScale = spin(page, 0.0, 10.0, 0.1, 1);
  m_lineColorButton = new QPushButton(page);
  m_lineColorButton->setObjectName(QStringLiteral("kmlLineColor"));
  m_lineWidth = spin(page, 0.0, 50.0, 0.5, 1);
  m_lineWidth->setObjectName(QStringLiteral("kmlLineWidth"));
  m_fillColorButton = new QPushButton(page);
  m_fill = new QCheckBox(tr("&Fill"), page);
  m_outline = new QCheckBox(tr("&Outline"), page);
  for (const std::pair<QPushButton*, QColor*>& button :
       {std::make_pair(m_iconColorButton, &m_iconColor),
        std::make_pair(m_labelColorButton, &m_labelColor),
        std::make_pair(m_lineColorButton, &m_lineColor),
        std::make_pair(m_fillColorButton, &m_fillColor)}) {
    connect(button.first, &QPushButton::clicked, this,
            [this, button]() { chooseColor(button.first, button.second); });
  }
  QHBoxLayout* icon = new QHBoxLayout;
  icon->addWidget(m_icon, 1);
  icon->addWidget(chooseIcon);
  QHBoxLayout* fill = new QHBoxLayout;
  fill->addWidget(m_fillColorButton);
  fill->addWidget(m_fill);
  fill->addWidget(m_outline);
  QFormLayout* form = new QFormLayout(page);
  form->addRow(tr("&Style:"), m_style);
  form->addRow(tr("&Icon:"), icon);
  form->addRow(tr("Icon colour:"), m_iconColorButton);
  form->addRow(tr("Icon si&ze:"), m_iconScale);
  form->addRow(tr("Label colour:"), m_labelColorButton);
  form->addRow(tr("&Label size:"), m_labelScale);
  form->addRow(tr("Line colour:"), m_lineColorButton);
  form->addRow(tr("Line &width (pixels):"), m_lineWidth);
  form->addRow(tr("Area:"), fill);
  // The style it uses now: a shared one by its id, else its own.
  const QString url = m_feature.styleUrl;
  const int32_t shared =
      url.startsWith(QLatin1Char('#')) ? m_style->findData(url.mid(1)) : -1;
  m_style->setCurrentIndex(shared >= 0 ? shared : 0);
  KmlStyleResolver resolver(*m_document);
  loadStyle(shared >= 0
                ? m_document->findStyle(url.mid(1))->mergedWith(KmlStyle{})
                : resolver.resolve(m_feature));
  connect(m_style, qOverload<int32_t>(&QComboBox::currentIndexChanged), this,
          &KmlPropertiesDialog::styleChosen);
  return page;
}

int32_t KmlPropertiesDialog::kmlStyleUsersOf(const QString& id) const
{
  return kmlStyleUsers(*m_document, id);
}

QWidget* KmlPropertiesDialog::makePosition()
{
  QWidget* page = new QWidget(this);
  m_altitude = spin(page, -12000.0, 1.0e7, 10.0, 1);
  m_altitude->setObjectName(QStringLiteral("kmlAltitude"));
  m_altitude->setSuffix(tr(" m"));
  m_altitudeMode = new QComboBox(page);
  m_altitudeMode->setObjectName(QStringLiteral("kmlAltitudeMode"));
  m_altitudeMode->addItems({tr("On the ground"), tr("Above the ground"),
                            tr("Above sea level"), tr("On the sea floor"),
                            tr("Above the sea floor")});
  m_extrude = new QCheckBox(tr("&Walls down to the ground (extrude)"), page);
  m_extrude->setObjectName(QStringLiteral("kmlExtrude"));
  const KmlGeometry geometry = m_feature.geometry.value_or(KmlGeometry{});
  KmlGeometry first = geometry;
  while (first.kind == KmlGeometry::Kind::MultiGeometry &&
         !first.children.empty()) {
    first = KmlGeometry(first.children.front());
  }
  m_altitude->setValue(
      first.coordinates.empty() ? 0.0 : first.coordinates.front().altM);
  const std::vector<KmlAltitudeMode>& modes = altitudeModes();
  m_altitudeMode->setCurrentIndex(static_cast<int32_t>(
      std::find(modes.begin(), modes.end(), first.altitudeMode) -
      modes.begin()));
  m_extrude->setChecked(first.extrude);
  QFormLayout* form = new QFormLayout(page);
  form->addRow(tr("&Measured:"), m_altitudeMode);
  form->addRow(tr("&Height:"), m_altitude);
  form->addRow(m_extrude);
  return page;
}

QWidget* KmlPropertiesDialog::makeTime()
{
  QWidget* page = new QWidget(this);
  m_timeKind = new QComboBox(page);
  m_timeKind->setObjectName(QStringLiteral("kmlTimeKind"));
  m_timeKind->addItems({tr("Always"), tr("From a moment on (time stamp)"),
                        tr("During a period (time span)")});
  m_begin = new QDateTimeEdit(page);
  m_begin->setObjectName(QStringLiteral("kmlBegin"));
  m_end = new QDateTimeEdit(page);
  m_end->setObjectName(QStringLiteral("kmlEnd"));
  for (QDateTimeEdit* edit : {m_begin, m_end}) {
    edit->setTimeSpec(Qt::UTC);
    edit->setCalendarPopup(true);
    edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'"));
  }
  const QDateTime now = QDateTime::currentDateTimeUtc();
  const std::optional<KmlTime>& time = m_feature.time;
  m_begin->setDateTime(time && time->begin ? time->begin->toUTC() : now);
  m_end->setDateTime(time && time->end ? time->end->toUTC() : now);
  if (time && time->begin && time->end && *time->begin == *time->end) {
    m_timeKind->setCurrentIndex(1);
  } else if (time) {
    m_timeKind->setCurrentIndex(2);
  }
  connect(m_timeKind, qOverload<int32_t>(&QComboBox::currentIndexChanged), this,
          &KmlPropertiesDialog::updateTime);
  QFormLayout* form = new QFormLayout(page);
  form->addRow(tr("&Shown:"), m_timeKind);
  form->addRow(tr("&From:"), m_begin);
  form->addRow(tr("&Until:"), m_end);
  updateTime();
  return page;
}

QWidget* KmlPropertiesDialog::makeData()
{
  QWidget* page = new QWidget(this);
  m_data = new QTableWidget(0, 2, page);
  m_data->setObjectName(QStringLiteral("kmlData"));
  m_data->setHorizontalHeaderLabels({tr("Field"), tr("Value")});
  m_data->horizontalHeader()->setStretchLastSection(true);
  for (const std::pair<QString, QString>& field : m_feature.extendedData) {
    const int32_t row = m_data->rowCount();
    m_data->insertRow(row);
    m_data->setItem(row, 0, new QTableWidgetItem(field.first));
    m_data->setItem(row, 1, new QTableWidgetItem(field.second));
  }
  QPushButton* add = new QPushButton(tr("&Add Field"), page);
  add->setObjectName(QStringLiteral("kmlAddField"));
  QPushButton* remove = new QPushButton(tr("&Remove Field"), page);
  connect(add, &QPushButton::clicked, this, [this]() {
    const int32_t row = m_data->rowCount();
    m_data->insertRow(row);
    m_data->setItem(row, 0, new QTableWidgetItem(tr("field")));
    m_data->setItem(row, 1, new QTableWidgetItem());
    m_data->editItem(m_data->item(row, 0));
  });
  connect(remove, &QPushButton::clicked, this, [this]() {
    if (m_data->currentRow() >= 0) {
      m_data->removeRow(m_data->currentRow());
      updatePreview();
    }
  });
  connect(m_data, &QTableWidget::itemChanged, this,
          &KmlPropertiesDialog::updatePreview);
  QHBoxLayout* buttons = new QHBoxLayout;
  buttons->addWidget(add);
  buttons->addWidget(remove);
  buttons->addStretch(1);
  QVBoxLayout* layout = new QVBoxLayout(page);
  layout->addWidget(m_data, 1);
  layout->addLayout(buttons);
  return page;
}

void KmlPropertiesDialog::showColor(QPushButton* button, const QColor& color)
{
  button->setText(color.name(QColor::HexArgb));
  button->setStyleSheet(
      QStringLiteral("background-color: %1;").arg(color.name(QColor::HexRgb)));
}

void KmlPropertiesDialog::chooseColor(QPushButton* button, QColor* color)
{
  const QColor chosen = QColorDialog::getColor(*color, this, tr("Colour"),
                                               QColorDialog::ShowAlphaChannel);
  if (chosen.isValid()) {
    *color = chosen;
    showColor(button, chosen);
  }
}

void KmlPropertiesDialog::loadStyle(const KmlStyle& style)
{
  m_loaded = style;
  const KmlIconStyle icon = style.icon.value_or(KmlIconStyle{});
  const KmlLabelStyle label = style.label.value_or(KmlLabelStyle{});
  const KmlLineStyle line = style.line.value_or(KmlLineStyle{});
  const KmlPolyStyle poly = style.poly.value_or(KmlPolyStyle{});
  m_icon->setText(icon.href);
  m_iconColor = icon.color;
  m_iconScale->setValue(icon.scale);
  m_labelColor = label.color;
  m_labelScale->setValue(label.scale);
  m_lineColor = line.color;
  m_lineWidth->setValue(line.width);
  m_fillColor = poly.color;
  m_fill->setChecked(poly.fill);
  m_outline->setChecked(poly.outline);
  showColor(m_iconColorButton, m_iconColor);
  showColor(m_labelColorButton, m_labelColor);
  showColor(m_lineColorButton, m_lineColor);
  showColor(m_fillColorButton, m_fillColor);
}

void KmlPropertiesDialog::styleChosen()
{
  const QString id = m_style->currentData().toString();
  const KmlStyle* shared = id.isEmpty() ? nullptr : m_document->findStyle(id);
  if (shared != nullptr) {
    loadStyle(*shared);
  }
}

KmlStyle KmlPropertiesDialog::editedStyle() const
{
  // Sub-styles the style had, and those now set otherwise than KML's
  // defaults: nothing else, so a style written back stays small.
  KmlStyle style = m_loaded;
  KmlIconStyle icon = style.icon.value_or(KmlIconStyle{});
  icon.href = m_icon->text().trimmed();
  icon.color = m_iconColor;
  icon.scale = m_iconScale->value();
  if (style.icon || !icon.href.isEmpty() ||
      icon.color != QColor(255, 255, 255) || icon.scale != 1.0) {
    style.icon = icon;
  }
  KmlLabelStyle label;
  label.color = m_labelColor;
  label.scale = m_labelScale->value();
  if (style.label || label.color != QColor(255, 255, 255) ||
      label.scale != 1.0) {
    style.label = label;
  }
  KmlLineStyle line;
  line.color = m_lineColor;
  line.width = m_lineWidth->value();
  if (style.line || line.color != QColor(255, 255, 255) || line.width != 1.0) {
    style.line = line;
  }
  KmlPolyStyle poly;
  poly.color = m_fillColor;
  poly.fill = m_fill->isChecked();
  poly.outline = m_outline->isChecked();
  if (style.poly || poly.color != QColor(255, 255, 255) || !poly.fill ||
      !poly.outline) {
    style.poly = poly;
  }
  return style;
}

std::optional<KmlStyle> KmlPropertiesDialog::sharedStyle() const
{
  const QString id = m_style->currentData().toString();
  if (id.isEmpty()) {
    return std::nullopt;
  }
  KmlStyle style = editedStyle();
  style.id = id;
  return style;
}

KmlFeature KmlPropertiesDialog::feature() const
{
  KmlFeature feature = m_feature;
  feature.name = m_name->text();
  feature.description = m_description->toPlainText();
  feature.view = m_view;
  // The style: a shared one by reference, or its own written in.
  const QString shared = m_style->currentData().toString();
  if (!shared.isEmpty()) {
    feature.styleUrl = QLatin1Char('#') + shared;
    feature.style.reset();
    feature.styleMap.reset();
  } else {
    KmlStyle own = editedStyle();
    own.id.clear();
    if (feature.style || own.icon || own.label || own.line || own.poly) {
      feature.styleUrl.clear();
      feature.styleMap.reset();
      feature.style = own;
    }
  }
  if (feature.geometry && m_altitude != nullptr) {
    const KmlAltitudeMode mode = altitudeModes().at(
        static_cast<std::size_t>(std::max(0, m_altitudeMode->currentIndex())));
    setGeometryHeight(*feature.geometry, mode, m_extrude->isChecked());
    const double height = m_altitude->value();
    forEachCoordinate(*feature.geometry,
                      [height](KmlCoordinate& c) { c.altM = height; });
  }
  switch (m_timeKind->currentIndex()) {
  case 1: {
    KmlTime stamp;
    stamp.begin = m_begin->dateTime().toUTC();
    stamp.end = stamp.begin;
    feature.time = stamp;
    break;
  }
  case 2: {
    KmlTime span;
    span.begin = m_begin->dateTime().toUTC();
    span.end = m_end->dateTime().toUTC();
    feature.time = span;
    break;
  }
  default:
    feature.time.reset();
    break;
  }
  feature.extendedData.clear();
  for (int32_t row = 0; row < m_data->rowCount(); ++row) {
    const QTableWidgetItem* key = m_data->item(row, 0);
    const QTableWidgetItem* value = m_data->item(row, 1);
    if (key != nullptr && !key->text().trimmed().isEmpty()) {
      feature.extendedData.emplace_back(
          key->text().trimmed(), value != nullptr ? value->text() : QString());
    }
  }
  return feature;
}

void KmlPropertiesDialog::updatePreview()
{
  if (m_preview == nullptr || m_data == nullptr) {
    return;
  }
  KmlFeature shown = m_feature;
  shown.name = m_name->text();
  shown.description = m_description->toPlainText();
  shown.extendedData.clear();
  for (int32_t row = 0; row < m_data->rowCount(); ++row) {
    const QTableWidgetItem* key = m_data->item(row, 0);
    const QTableWidgetItem* value = m_data->item(row, 1);
    if (key != nullptr) {
      shown.extendedData.emplace_back(
          key->text(), value != nullptr ? value->text() : QString());
    }
  }
  m_preview->setHtml(kmlBalloonHtml(shown, KmlStyle{}));
}

void KmlPropertiesDialog::updateTime()
{
  m_begin->setEnabled(m_timeKind->currentIndex() != 0);
  m_end->setEnabled(m_timeKind->currentIndex() == 2);
}

void KmlPropertiesDialog::updateView()
{
  if (!m_view) {
    m_viewLabel->setText(tr("None: flying there shows all of it"));
    return;
  }
  m_viewLabel->setText(tr("%1, %2, %3 m away")
                           .arg(m_view->latDeg, 0, 'f', 4)
                           .arg(m_view->lonDeg, 0, 'f', 4)
                           .arg(m_view->rangeM, 0, 'f', 0));
}

} // namespace ts
