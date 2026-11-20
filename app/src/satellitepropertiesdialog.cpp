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
#include "satellitepropertiesdialog.h"

#include <utility>

#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPixmap>
#include <QPushButton>
#include <QVBoxLayout>

namespace ts
{

TimeSpanEdit::TimeSpanEdit(QWidget* parent)
    : QWidget(parent), m_amount(new QDoubleSpinBox(this)),
      m_unit(new QComboBox(this))
{
  m_amount->setObjectName(QStringLiteral("spanAmount"));
  m_unit->setObjectName(QStringLiteral("spanUnit"));
  m_amount->setRange(0.0, 100000.0);
  m_amount->setDecimals(2);
  m_unit->addItem(QString(), static_cast<int32_t>(SpanUnit::Orbits));
  m_unit->addItem(QString(), static_cast<int32_t>(SpanUnit::Minutes));
  QHBoxLayout* layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->addWidget(m_amount, 1);
  layout->addWidget(m_unit);
  retranslateUi();
}

TimeSpanEdit::~TimeSpanEdit() = default;

TimeSpan TimeSpanEdit::span() const
{
  TimeSpan span;
  span.amount = m_amount->value();
  span.unit = static_cast<SpanUnit>(m_unit->currentData().toInt());
  return span;
}

void TimeSpanEdit::setSpan(TimeSpan span)
{
  m_amount->setValue(span.amount);
  m_unit->setCurrentIndex(m_unit->findData(static_cast<int32_t>(span.unit)));
}

void TimeSpanEdit::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QWidget::changeEvent(event);
}

void TimeSpanEdit::retranslateUi()
{
  m_unit->setItemText(0, tr("orbits"));
  m_unit->setItemText(1, tr("minutes"));
}

SatellitePropertiesDialog::SatellitePropertiesDialog(
    const SatelliteStyle& style, QString name, QWidget* parent)
    : QDialog(parent), m_original(style), m_name(std::move(name)),
      m_color(style.color), m_colorButton(new QPushButton(this)),
      m_size(new QDoubleSpinBox(this)), m_labels(new QCheckBox(this)),
      m_tracks(new QComboBox(this)), m_past(new TimeSpanEdit(this)),
      m_future(new TimeSpanEdit(this)), m_groundTrack(new QCheckBox(this)),
      m_footprints(new QComboBox(this)), m_elevation(new QDoubleSpinBox(this)),
      m_maximumAge(new QDoubleSpinBox(this)), m_colorLabel(new QLabel(this)),
      m_sizeLabel(new QLabel(this)), m_tracksLabel(new QLabel(this)),
      m_pastLabel(new QLabel(this)), m_futureLabel(new QLabel(this)),
      m_footprintsLabel(new QLabel(this)), m_elevationLabel(new QLabel(this)),
      m_ageLabel(new QLabel(this))
{
  setObjectName(QStringLiteral("satellitePropertiesDialog"));
  m_past->setObjectName(QStringLiteral("pastSpan"));
  m_future->setObjectName(QStringLiteral("futureSpan"));
  m_tracks->setObjectName(QStringLiteral("trackMode"));
  m_footprints->setObjectName(QStringLiteral("footprintMode"));
  m_size->setRange(1.0, 64.0);
  m_size->setDecimals(1);
  m_size->setValue(style.markerSizePx);
  m_labels->setChecked(style.labels);
  for (const TrackMode mode : {TrackMode::None, TrackMode::Selected,
                               TrackMode::Automatic, TrackMode::All}) {
    m_tracks->addItem(QString(), static_cast<int32_t>(mode));
  }
  m_tracks->setCurrentIndex(
      m_tracks->findData(static_cast<int32_t>(style.tracks)));
  m_past->setSpan(style.past);
  m_future->setSpan(style.future);
  m_groundTrack->setChecked(style.groundTrack);
  for (const FootprintMode mode :
       {FootprintMode::None, FootprintMode::Selected, FootprintMode::All}) {
    m_footprints->addItem(QString(), static_cast<int32_t>(mode));
  }
  m_footprints->setCurrentIndex(
      m_footprints->findData(static_cast<int32_t>(style.footprints)));
  m_elevation->setRange(0.0, 89.0);
  m_elevation->setValue(style.footprintElevationDeg);
  m_maximumAge->setRange(0.0, 100000.0);
  m_maximumAge->setDecimals(1);
  m_maximumAge->setValue(style.maximumAgeDays);
  connect(m_colorButton, &QPushButton::clicked, this,
          &SatellitePropertiesDialog::chooseColor);

  QFormLayout* form = new QFormLayout;
  form->addRow(m_colorLabel, m_colorButton);
  form->addRow(m_sizeLabel, m_size);
  form->addRow(QString(), m_labels);
  form->addRow(m_tracksLabel, m_tracks);
  form->addRow(m_pastLabel, m_past);
  form->addRow(m_futureLabel, m_future);
  form->addRow(QString(), m_groundTrack);
  form->addRow(m_footprintsLabel, m_footprints);
  form->addRow(m_elevationLabel, m_elevation);
  form->addRow(m_ageLabel, m_maximumAge);
  QDialogButtonBox* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addLayout(form);
  layout->addWidget(buttons);
  retranslateUi();
}

SatellitePropertiesDialog::~SatellitePropertiesDialog() = default;

SatelliteStyle SatellitePropertiesDialog::style() const
{
  SatelliteStyle style = m_original;
  style.color = m_color;
  style.markerSizePx = m_size->value();
  style.labels = m_labels->isChecked();
  style.tracks = static_cast<TrackMode>(m_tracks->currentData().toInt());
  style.past = m_past->span();
  style.future = m_future->span();
  style.groundTrack = m_groundTrack->isChecked();
  style.footprints =
      static_cast<FootprintMode>(m_footprints->currentData().toInt());
  style.footprintElevationDeg = m_elevation->value();
  style.maximumAgeDays = m_maximumAge->value();
  return style;
}

void SatellitePropertiesDialog::chooseColor()
{
  const QColor chosen =
      QColorDialog::getColor(m_color, this, tr("Colour of %1").arg(m_name),
                             QColorDialog::ShowAlphaChannel);
  if (chosen.isValid()) {
    m_color = chosen;
    showColor();
  }
}

void SatellitePropertiesDialog::showColor()
{
  QPixmap swatch(32, 16);
  swatch.fill(m_color);
  m_colorButton->setIcon(QIcon(swatch));
  m_colorButton->setText(m_color.name(QColor::HexArgb));
}

void SatellitePropertiesDialog::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QDialog::changeEvent(event);
}

void SatellitePropertiesDialog::retranslateUi()
{
  setWindowTitle(tr("Properties of %1").arg(m_name));
  m_colorLabel->setText(tr("&Colour:"));
  m_colorLabel->setBuddy(m_colorButton);
  m_sizeLabel->setText(tr("&Marker size:"));
  m_sizeLabel->setBuddy(m_size);
  m_size->setSuffix(tr(" px"));
  m_labels->setText(tr("&Names beside the markers"));
  m_tracksLabel->setText(tr("&Tracks:"));
  m_tracksLabel->setBuddy(m_tracks);
  m_tracks->setItemText(0, tr("None"));
  m_tracks->setItemText(1, tr("The selected satellite"));
  m_tracks->setItemText(
      2, tr("All shown, or the selected one when more than %1 are shown")
             .arg(SatelliteStyle::automaticTrackLimit));
  m_tracks->setItemText(3, tr("All shown"));
  m_pastLabel->setText(tr("&Past track:"));
  m_pastLabel->setBuddy(m_past);
  m_futureLabel->setText(tr("&Future track:"));
  m_futureLabel->setBuddy(m_future);
  m_groundTrack->setText(tr("&Ground tracks under them on the globe"));
  m_footprintsLabel->setText(tr("F&ootprints:"));
  m_footprintsLabel->setBuddy(m_footprints);
  m_footprints->setItemText(0, tr("None"));
  m_footprints->setItemText(1, tr("The selected satellite"));
  m_footprints->setItemText(2, tr("All shown"));
  m_elevationLabel->setText(tr("Seen &above:"));
  m_elevationLabel->setBuddy(m_elevation);
  m_elevation->setSuffix(tr("° elevation"));
  m_ageLabel->setText(tr("Elements &old after:"));
  m_ageLabel->setBuddy(m_maximumAge);
  m_maximumAge->setSuffix(tr(" days"));
  showColor();
}

SpanOverrideDialog::SpanOverrideDialog(const QString& names,
                                       const SpanOverride& current,
                                       TimeSpan layerPast, TimeSpan layerFuture,
                                       QWidget* parent)
    : QDialog(parent), m_ownPast(new QCheckBox(this)),
      m_ownFuture(new QCheckBox(this)), m_past(new TimeSpanEdit(this)),
      m_future(new TimeSpanEdit(this))
{
  setObjectName(QStringLiteral("spanOverrideDialog"));
  setWindowTitle(tr("Track Spans of %1").arg(names));
  m_ownPast->setText(tr("Own &past track:"));
  m_ownFuture->setText(tr("Own &future track:"));
  m_ownPast->setChecked(current.past.has_value());
  m_ownFuture->setChecked(current.future.has_value());
  m_past->setSpan(current.past.value_or(layerPast));
  m_future->setSpan(current.future.value_or(layerFuture));
  m_past->setEnabled(m_ownPast->isChecked());
  m_future->setEnabled(m_ownFuture->isChecked());
  connect(m_ownPast, &QCheckBox::toggled, m_past, &QWidget::setEnabled);
  connect(m_ownFuture, &QCheckBox::toggled, m_future, &QWidget::setEnabled);
  QFormLayout* form = new QFormLayout;
  form->addRow(m_ownPast, m_past);
  form->addRow(m_ownFuture, m_future);
  QLabel* note = new QLabel(
      tr("Unchecked, a span is the layer's (Properties of the layer)."), this);
  note->setWordWrap(true);
  QDialogButtonBox* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addLayout(form);
  layout->addWidget(note);
  layout->addWidget(buttons);
}

SpanOverrideDialog::~SpanOverrideDialog() = default;

SpanOverride SpanOverrideDialog::spans() const
{
  SpanOverride spans;
  if (m_ownPast->isChecked()) {
    spans.past = m_past->span();
  }
  if (m_ownFuture->isChecked()) {
    spans.future = m_future->span();
  }
  return spans;
}

} // namespace ts
