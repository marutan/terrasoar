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
#include "aircraftpropertiesdialog.h"

#include <cmath>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QLinearGradient>
#include <QLocale>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace ts
{

QPixmap altitudeLegend(int32_t widthPx)
{
  const std::vector<AltitudeStop>& stops = altitudeStops();
  const double top = stops.back().heightM;
  const int32_t barPx = 12;
  const int32_t heightPx = barPx + 18;
  QPixmap pixmap(widthPx, heightPx);
  pixmap.fill(Qt::transparent);
  QPainter painter(&pixmap);
  QLinearGradient gradient(0.0, 0.0, widthPx, 0.0);
  for (const AltitudeStop& stop : stops) {
    gradient.setColorAt(stop.heightM / top,
                        QColor(stop.rgba[0], stop.rgba[1], stop.rgba[2]));
  }
  painter.fillRect(QRect(0, 0, widthPx, barPx), gradient);
  QFont font = painter.font();
  font.setPointSizeF(font.pointSizeF() * 0.8);
  painter.setFont(font);
  painter.setPen(QColor(128, 128, 128));
  const QLocale locale;
  for (const double feet : {0.0, 10000.0, 20000.0, 30000.0, 40000.0}) {
    const double x = feet * 0.3048 / top * widthPx;
    painter.drawLine(QPointF(x, barPx), QPointF(x, barPx + 3));
    const QString text = AircraftPropertiesDialog::tr("%1 ft").arg(
        locale.toString(static_cast<int32_t>(feet)));
    const int32_t textWidth = painter.fontMetrics().horizontalAdvance(text);
    const double left = std::clamp(x - textWidth / 2.0, 0.0,
                                   static_cast<double>(widthPx - textWidth));
    painter.drawText(QPointF(left, heightPx - 2), text);
  }
  return pixmap;
}

AircraftPropertiesDialog::AircraftPropertiesDialog(const AircraftStyle& style,
                                                   const QString& name,
                                                   ViewArea viewArea,
                                                   QWidget* parent)
    : QDialog(parent), m_style(style), m_name(name),
      m_viewArea(std::move(viewArea)), m_color(style.color),
      m_colorButton(new QPushButton(this)), m_markerSize(new QSpinBox(this)),
      m_labels(new QCheckBox(tr("Callsigns"), this)),
      m_labelAltitude(new QCheckBox(tr("with altitudes"), this)),
      m_labelSpeed(new QCheckBox(tr("with speeds"), this)),
      m_dropLines(new QCheckBox(tr("Lines down to the ground"), this)),
      m_trailMode(new QComboBox(this)), m_trailSpan(new QDoubleSpinBox(this)),
      m_colouring(new QComboBox(this)), m_legend(new QLabel(this)),
      m_stale(new QDoubleSpinBox(this)), m_gap(new QDoubleSpinBox(this)),
      m_callsignFilter(new QLineEdit(this)),
      m_addressFilter(new QLineEdit(this)),
      m_hasMinimum(new QCheckBox(tr("At or above"), this)),
      m_minimum(new QSpinBox(this)),
      m_hasMaximum(new QCheckBox(tr("At or below"), this)),
      m_maximum(new QSpinBox(this)),
      m_hasArea(new QCheckBox(tr("Only within"), this)),
      m_south(new QDoubleSpinBox(this)), m_west(new QDoubleSpinBox(this)),
      m_north(new QDoubleSpinBox(this)), m_east(new QDoubleSpinBox(this)),
      m_useView(new QPushButton(tr("Use the View"), this))
{
  setObjectName(QStringLiteral("aircraftPropertiesDialog"));
  setWindowTitle(tr("%1 – Aircraft Layer Properties").arg(name));
  m_trailMode->setObjectName(QStringLiteral("trailMode"));
  m_trailSpan->setObjectName(QStringLiteral("trailSpan"));
  m_colouring->setObjectName(QStringLiteral("trailColouring"));
  m_callsignFilter->setObjectName(QStringLiteral("callsignFilter"));
  m_dropLines->setObjectName(QStringLiteral("dropLines"));
  m_markerSize->setRange(4, 64);
  m_markerSize->setSuffix(tr(" px"));
  m_markerSize->setValue(static_cast<int32_t>(std::lround(style.markerSizePx)));
  m_labels->setChecked(style.labels);
  m_labelAltitude->setChecked(style.labelAltitude);
  m_labelSpeed->setChecked(style.labelSpeed);
  m_dropLines->setChecked(style.dropLines);
  m_trailMode->addItem(tr("Off"), static_cast<int32_t>(TrailMode::Off));
  m_trailMode->addItem(tr("A time span before the clock"),
                       static_cast<int32_t>(TrailMode::Span));
  m_trailMode->addItem(tr("All the data up to the clock"),
                       static_cast<int32_t>(TrailMode::All));
  m_trailMode->setCurrentIndex(
      m_trailMode->findData(static_cast<int32_t>(style.trail.mode)));
  m_trailSpan->setRange(0.1, 24.0 * 60.0);
  m_trailSpan->setDecimals(1);
  m_trailSpan->setSuffix(tr(" min"));
  m_trailSpan->setValue(style.trail.spanS / 60.0);
  m_colouring->addItem(tr("The layer's colour"),
                       static_cast<int32_t>(TrailColouring::Aircraft));
  m_colouring->addItem(tr("Altitude"),
                       static_cast<int32_t>(TrailColouring::Altitude));
  m_colouring->addItem(tr("Climbing or descending"),
                       static_cast<int32_t>(TrailColouring::VerticalRate));
  m_colouring->setCurrentIndex(
      m_colouring->findData(static_cast<int32_t>(style.colouring)));
  m_legend->setPixmap(altitudeLegend(320));
  m_stale->setRange(1.0, 3600.0);
  m_stale->setDecimals(0);
  m_stale->setSuffix(tr(" s"));
  m_stale->setValue(style.timing.staleS);
  m_gap->setRange(1.0, 3600.0);
  m_gap->setDecimals(0);
  m_gap->setSuffix(tr(" s"));
  m_gap->setValue(style.timing.gapS);
  m_callsignFilter->setText(style.callsignFilter);
  m_callsignFilter->setPlaceholderText(tr("Any"));
  m_addressFilter->setText(style.addressFilter);
  m_addressFilter->setPlaceholderText(tr("Any"));
  for (QSpinBox* box : {m_minimum, m_maximum}) {
    box->setRange(-2000, 100000);
    box->setSingleStep(1000);
    box->setSuffix(tr(" ft"));
  }
  m_hasMinimum->setChecked(style.minimumAltitudeFt.has_value());
  m_minimum->setValue(
      static_cast<int32_t>(std::lround(style.minimumAltitudeFt.value_or(0.0))));
  m_hasMaximum->setChecked(style.maximumAltitudeFt.has_value());
  m_maximum->setValue(static_cast<int32_t>(
      std::lround(style.maximumAltitudeFt.value_or(45000.0))));
  const LatLonBox area = style.area.value_or(LatLonBox());
  m_hasArea->setChecked(style.area.has_value());
  for (QDoubleSpinBox* box : {m_south, m_north}) {
    box->setRange(-90.0, 90.0);
    box->setDecimals(3);
  }
  for (QDoubleSpinBox* box : {m_west, m_east}) {
    box->setRange(-180.0, 180.0);
    box->setDecimals(3);
  }
  m_south->setValue(area.southDeg);
  m_west->setValue(area.westDeg);
  m_north->setValue(area.northDeg);
  m_east->setValue(area.eastDeg);
  m_useView->setEnabled(static_cast<bool>(m_viewArea));

  QGroupBox* look = new QGroupBox(tr("Aircraft"), this);
  QFormLayout* lookForm = new QFormLayout(look);
  lookForm->addRow(tr("Colour:"), m_colorButton);
  lookForm->addRow(tr("Size:"), m_markerSize);
  QHBoxLayout* labelRow = new QHBoxLayout;
  labelRow->addWidget(m_labels);
  labelRow->addWidget(m_labelAltitude);
  labelRow->addWidget(m_labelSpeed);
  labelRow->addStretch(1);
  lookForm->addRow(tr("Labels:"), labelRow);
  lookForm->addRow(QString(), m_dropLines);
  lookForm->addRow(tr("Gone after no reports for:"), m_stale);

  QGroupBox* trails = new QGroupBox(tr("Trails"), this);
  QFormLayout* trailForm = new QFormLayout(trails);
  trailForm->addRow(tr("Show:"), m_trailMode);
  trailForm->addRow(tr("Span:"), m_trailSpan);
  trailForm->addRow(tr("Coloured by:"), m_colouring);
  trailForm->addRow(QString(), m_legend);
  trailForm->addRow(tr("Broken at gaps over:"), m_gap);

  QGroupBox* filters = new QGroupBox(tr("Show Only"), this);
  QGridLayout* filterGrid = new QGridLayout(filters);
  filterGrid->addWidget(new QLabel(tr("Callsigns containing:"), filters), 0, 0);
  filterGrid->addWidget(m_callsignFilter, 0, 1, 1, 3);
  filterGrid->addWidget(new QLabel(tr("ICAO addresses starting:"), filters), 1,
                        0);
  filterGrid->addWidget(m_addressFilter, 1, 1, 1, 3);
  filterGrid->addWidget(m_hasMinimum, 2, 0);
  filterGrid->addWidget(m_minimum, 2, 1);
  filterGrid->addWidget(m_hasMaximum, 2, 2);
  filterGrid->addWidget(m_maximum, 2, 3);
  filterGrid->addWidget(m_hasArea, 3, 0);
  filterGrid->addWidget(new QLabel(tr("South, west:"), filters), 3, 1);
  filterGrid->addWidget(m_south, 3, 2);
  filterGrid->addWidget(m_west, 3, 3);
  filterGrid->addWidget(m_useView, 4, 0);
  filterGrid->addWidget(new QLabel(tr("North, east:"), filters), 4, 1);
  filterGrid->addWidget(m_north, 4, 2);
  filterGrid->addWidget(m_east, 4, 3);

  QDialogButtonBox* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_colorButton, &QPushButton::clicked, this,
          &AircraftPropertiesDialog::chooseColor);
  for (QCheckBox* box : {m_labels, m_hasMinimum, m_hasMaximum, m_hasArea}) {
    connect(box, &QCheckBox::toggled, this,
            &AircraftPropertiesDialog::updateEnabled);
  }
  for (QComboBox* box : {m_trailMode, m_colouring}) {
    connect(box, qOverload<int32_t>(&QComboBox::currentIndexChanged), this,
            &AircraftPropertiesDialog::updateEnabled);
  }
  connect(m_useView, &QPushButton::clicked, this, [this]() {
    const std::optional<LatLonBox> view = m_viewArea();
    if (view) {
      m_hasArea->setChecked(true);
      m_south->setValue(view->southDeg);
      m_west->setValue(view->westDeg);
      m_north->setValue(view->northDeg);
      m_east->setValue(view->eastDeg);
    }
  });
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(look);
  layout->addWidget(trails);
  layout->addWidget(filters);
  layout->addWidget(buttons);
  updateSwatch();
  updateEnabled();
}

AircraftPropertiesDialog::~AircraftPropertiesDialog() = default;

void AircraftPropertiesDialog::chooseColor()
{
  const QColor chosen =
      QColorDialog::getColor(m_color, this, tr("Colour of %1").arg(m_name),
                             QColorDialog::ShowAlphaChannel);
  if (chosen.isValid()) {
    m_color = chosen;
    updateSwatch();
  }
}

void AircraftPropertiesDialog::updateSwatch()
{
  QPixmap swatch(32, 16);
  swatch.fill(m_color);
  m_colorButton->setIcon(QIcon(swatch));
  m_colorButton->setText(m_color.name());
}

void AircraftPropertiesDialog::updateEnabled()
{
  m_labelAltitude->setEnabled(m_labels->isChecked());
  m_labelSpeed->setEnabled(m_labels->isChecked());
  const TrailMode mode =
      static_cast<TrailMode>(m_trailMode->currentData().toInt());
  m_trailSpan->setEnabled(mode == TrailMode::Span);
  m_colouring->setEnabled(mode != TrailMode::Off);
  m_legend->setVisible(
      static_cast<TrailColouring>(m_colouring->currentData().toInt()) ==
      TrailColouring::Altitude);
  m_minimum->setEnabled(m_hasMinimum->isChecked());
  m_maximum->setEnabled(m_hasMaximum->isChecked());
  for (QDoubleSpinBox* box : {m_south, m_west, m_north, m_east}) {
    box->setEnabled(m_hasArea->isChecked());
  }
}

AircraftStyle AircraftPropertiesDialog::style() const
{
  AircraftStyle style = m_style;
  style.color = m_color;
  style.markerSizePx = m_markerSize->value();
  style.labels = m_labels->isChecked();
  style.labelAltitude = m_labelAltitude->isChecked();
  style.labelSpeed = m_labelSpeed->isChecked();
  style.dropLines = m_dropLines->isChecked();
  style.trail.mode = static_cast<TrailMode>(m_trailMode->currentData().toInt());
  style.trail.spanS = m_trailSpan->value() * 60.0;
  style.colouring =
      static_cast<TrailColouring>(m_colouring->currentData().toInt());
  style.timing.staleS = m_stale->value();
  style.timing.gapS = m_gap->value();
  style.callsignFilter = m_callsignFilter->text().trimmed();
  style.addressFilter = m_addressFilter->text().trimmed();
  style.minimumAltitudeFt =
      m_hasMinimum->isChecked()
          ? std::optional<double>(static_cast<double>(m_minimum->value()))
          : std::nullopt;
  style.maximumAltitudeFt =
      m_hasMaximum->isChecked()
          ? std::optional<double>(static_cast<double>(m_maximum->value()))
          : std::nullopt;
  style.area = m_hasArea->isChecked() ? std::optional<LatLonBox>(LatLonBox{
                                            m_south->value(), m_west->value(),
                                            m_north->value(), m_east->value()})
                                      : std::nullopt;
  return style;
}

} // namespace ts
