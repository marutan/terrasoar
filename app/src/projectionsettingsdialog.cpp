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
#include "projectionsettingsdialog.h"

#include <cstddef>
#include <cstdint>

#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QPushButton>
#include <QShortcut>
#include <QVBoxLayout>

#include "helpwindow.h"
#include "ts/projection/rotatedmercator.h"

namespace ts
{

QString ProjectionSettingsDialog::helpTarget()
{
  return QStringLiteral("getting-around.md#projection-settings");
}

ProjectionSettingsDialog::ProjectionSettingsDialog(QWidget* parent,
                                                   const Projection* projection)
    : QDialog(parent), m_projection(projection),
      m_parameters(projection->parameters()), m_titleLabel(new QLabel(this)),
      m_form(new QFormLayout()), m_defaultsButton(new QPushButton(this)),
      m_buttons(new QDialogButtonBox(
          QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this))
{
  QShortcut* help = new QShortcut(QKeySequence::HelpContents, this);
  connect(help, &QShortcut::activated, this,
          [this]() { showUserGuide(this, helpTarget()); });
  setObjectName(QStringLiteral("projectionSettingsDialog"));
  m_defaultsButton->setObjectName(QStringLiteral("projectionDefaultsButton"));
  const ProjectionParameterValues current = projection->parameterValues();
  for (const ProjectionParameter& parameter : m_parameters) {
    QDoubleSpinBox* box = new QDoubleSpinBox(this);
    box->setObjectName(QStringLiteral("projectionParameter_") + parameter.key);
    box->setDecimals(parameter.decimals);
    box->setRange(parameter.minimum, parameter.maximum);
    box->setSuffix(parameter.suffix);
    const ProjectionParameterValues::const_iterator value =
        current.find(parameter.key);
    box->setValue(value != current.end() ? value->second
                                         : parameter.defaultValue);
    m_form->addRow(parameter.label, box);
    m_boxes.push_back(box);
  }
  m_buttons->addButton(m_defaultsButton, QDialogButtonBox::ResetRole);
  connect(m_defaultsButton, &QPushButton::clicked, this,
          &ProjectionSettingsDialog::restoreDefaults);
  connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(m_titleLabel);
  layout->addLayout(m_form);
  if (projection->id() == QLatin1String("rotated-mercator")) {
    addTwoPoints();
    layout->addWidget(m_twoPointsGroup);
  }
  if (projection->id() == QLatin1String("azimuthal-equidistant")) {
    addPolarPresets();
    layout->addWidget(m_presetsGroup);
  }
  layout->addWidget(m_buttons);
  retranslateUi();
}

ProjectionSettingsDialog::~ProjectionSettingsDialog() = default;

ProjectionParameterValues ProjectionSettingsDialog::values() const
{
  ProjectionParameterValues values;
  for (std::size_t i = 0; i < m_parameters.size(); ++i) {
    values[m_parameters[i].key] = m_boxes[i]->value();
  }
  return values;
}

const Projection* ProjectionSettingsDialog::projection() const
{
  const Projection* chosen = projectionWith(m_projection->id(), values());
  return chosen != nullptr ? chosen : m_projection;
}

void ProjectionSettingsDialog::restoreDefaults()
{
  for (std::size_t i = 0; i < m_parameters.size(); ++i) {
    m_boxes[i]->setValue(m_parameters[i].defaultValue);
  }
}

void ProjectionSettingsDialog::addTwoPoints()
{
  m_twoPointsGroup = new QGroupBox(this);
  m_twoPointsGroup->setObjectName(QStringLiteral("projectionTwoPoints"));
  QFormLayout* form = new QFormLayout(m_twoPointsGroup);
  // London Heathrow and New York JFK: a line across the North Atlantic.
  const std::array<double, 4> starts = {51.4775, -0.4614, 40.6413, -73.7781};
  for (std::size_t i = 0; i < m_pointBoxes.size(); ++i) {
    QDoubleSpinBox* box = new QDoubleSpinBox(m_twoPointsGroup);
    box->setObjectName(QStringLiteral("projectionPoint%1").arg(i));
    box->setDecimals(4);
    const double limit = i % 2 == 0 ? 90.0 : 180.0;
    box->setRange(-limit, limit);
    box->setSuffix(QStringLiteral("°"));
    box->setValue(starts.at(i));
    m_pointBoxes.at(i) = box;
    form->addRow(QString(), box);
  }
  m_usePointsButton = new QPushButton(m_twoPointsGroup);
  m_usePointsButton->setObjectName(QStringLiteral("projectionUsePoints"));
  m_pointsMessage = new QLabel(m_twoPointsGroup);
  m_pointsMessage->setObjectName(QStringLiteral("projectionPointsMessage"));
  m_pointsMessage->setWordWrap(true);
  form->addRow(m_usePointsButton);
  form->addRow(m_pointsMessage);
  connect(m_usePointsButton, &QPushButton::clicked, this,
          &ProjectionSettingsDialog::useTwoPoints);
}

void ProjectionSettingsDialog::addPolarPresets()
{
  m_presetsGroup = new QGroupBox(this);
  m_presetsGroup->setObjectName(QStringLiteral("projectionPresets"));
  QHBoxLayout* row = new QHBoxLayout(m_presetsGroup);
  m_northPoleButton = new QPushButton(m_presetsGroup);
  m_northPoleButton->setObjectName(QStringLiteral("projectionNorthPole"));
  m_southPoleButton = new QPushButton(m_presetsGroup);
  m_southPoleButton->setObjectName(QStringLiteral("projectionSouthPole"));
  row->addWidget(m_northPoleButton);
  row->addWidget(m_southPoleButton);
  connect(m_northPoleButton, &QPushButton::clicked, this,
          [this]() { setCentre(90.0); });
  connect(m_southPoleButton, &QPushButton::clicked, this,
          [this]() { setCentre(-90.0); });
}

void ProjectionSettingsDialog::setCentre(double latDeg)
{
  for (std::size_t i = 0; i < m_parameters.size(); ++i) {
    if (m_parameters[i].key == QLatin1String("centreLatitude")) {
      m_boxes[i]->setValue(latDeg);
    } else if (m_parameters[i].key == QLatin1String("centreLongitude")) {
      m_boxes[i]->setValue(0.0);
    }
  }
}

void ProjectionSettingsDialog::useTwoPoints()
{
  const std::optional<ProjectionParameterValues> line =
      RotatedMercatorProjection::throughPoints(
          m_pointBoxes[0]->value(), m_pointBoxes[1]->value(),
          m_pointBoxes[2]->value(), m_pointBoxes[3]->value());
  if (!line) {
    m_pointsMessage->setText(
        tr("No single line: the places are the same or opposite each "
           "other."));
    return;
  }
  m_pointsMessage->clear();
  for (std::size_t i = 0; i < m_parameters.size(); ++i) {
    const ProjectionParameterValues::const_iterator value =
        line->find(m_parameters[i].key);
    if (value != line->end()) {
      m_boxes[i]->setValue(value->second);
    }
  }
}

void ProjectionSettingsDialog::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QDialog::changeEvent(event);
}

void ProjectionSettingsDialog::retranslateUi()
{
  setWindowTitle(tr("Map Projection Settings"));
  m_titleLabel->setText(m_projection->displayName());
  // The labels come from the projection, translated in its context.
  const std::vector<ProjectionParameter> parameters =
      m_projection->parameters();
  for (std::size_t i = 0; i < parameters.size() && i < m_boxes.size(); ++i) {
    if (QLabel* label =
            qobject_cast<QLabel*>(m_form->labelForField(m_boxes[i]))) {
      label->setText(parameters[i].label);
    }
  }
  m_defaultsButton->setText(tr("Restore &Defaults"));
  if (m_twoPointsGroup != nullptr) {
    m_twoPointsGroup->setTitle(tr("Centre Line Through Two Places"));
    QFormLayout* form = qobject_cast<QFormLayout*>(m_twoPointsGroup->layout());
    const std::array<QString, 4> labels = {
        tr("First latitude"), tr("First longitude"), tr("Second latitude"),
        tr("Second longitude")};
    for (std::size_t i = 0; i < m_pointBoxes.size(); ++i) {
      if (QLabel* label = form != nullptr
                              ? qobject_cast<QLabel*>(
                                    form->labelForField(m_pointBoxes.at(i)))
                              : nullptr) {
        label->setText(labels.at(i));
      }
    }
    m_usePointsButton->setText(tr("&Use These Places"));
  }
  if (m_presetsGroup != nullptr) {
    m_presetsGroup->setTitle(tr("Polar Presets"));
    m_northPoleButton->setText(tr("&North Pole"));
    m_southPoleButton->setText(tr("&South Pole"));
  }
}

} // namespace ts
