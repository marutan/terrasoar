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
 * The Map Projection Settings dialog: the parameters of the flat map's
 * projection (a standard parallel, for instance).
 */
#ifndef TS_APP_PROJECTIONSETTINGSDIALOG_H
#define TS_APP_PROJECTIONSETTINGSDIALOG_H

#include <array>
#include <vector>

#include <QDialog>
#include <QString>

#include "ts/projection/projection.h"

class QDialogButtonBox;
class QDoubleSpinBox;
class QEvent;
class QFormLayout;
class QGroupBox;
class QLabel;
class QPushButton;

namespace ts
{

/**
 * Edits the parameters of a projection (Projection::parameters()): one
 * box per parameter, its range the parameter's, and a button back to the
 * defaults. projection() gives the projection with the chosen values once
 * OK was pressed.
 */
class ProjectionSettingsDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(ProjectionSettingsDialog)

public:
  /**
   * The user guide's section on this window
   * (`getting-around.md#projection-settings`), opened by F1.
   */
  static QString helpTarget();

  /** Creates the dialog for @p projection, showing its current values. */
  ProjectionSettingsDialog(QWidget* parent, const Projection* projection);
  ~ProjectionSettingsDialog() override;

  /** Returns the values in the boxes, by parameter key. */
  ProjectionParameterValues values() const;

  /** Returns the projection with values() (projectionWith()). */
  const Projection* projection() const;

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

private:
  void retranslateUi();
  void restoreDefaults();
  /** Rotated Mercator: the group that sets the centre line through two
   *  places. */
  void addTwoPoints();
  /** Fills the centre and azimuth from the two places. */
  void useTwoPoints();
  /** Azimuthal Equidistant: the polar presets. */
  void addPolarPresets();
  /** Sets the centre to latitude @p latDeg, longitude 0. */
  void setCentre(double latDeg);

  const Projection* m_projection;
  std::vector<ProjectionParameter> m_parameters;
  QLabel* m_titleLabel;
  QFormLayout* m_form;
  std::vector<QDoubleSpinBox*> m_boxes;
  QPushButton* m_defaultsButton;
  QDialogButtonBox* m_buttons;
  /** Rotated Mercator's two places (null otherwise). */
  QGroupBox* m_twoPointsGroup = nullptr;
  /** Latitude and longitude of the first place, then of the second. */
  std::array<QDoubleSpinBox*, 4> m_pointBoxes{};
  QPushButton* m_usePointsButton = nullptr;
  QLabel* m_pointsMessage = nullptr;
  /** Azimuthal Equidistant's presets (null otherwise). */
  QGroupBox* m_presetsGroup = nullptr;
  QPushButton* m_northPoleButton = nullptr;
  QPushButton* m_southPoleButton = nullptr;
};

} // namespace ts

#endif // TS_APP_PROJECTIONSETTINGSDIALOG_H
