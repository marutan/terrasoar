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
 * The properties of a satellite layer (its style), and a satellite's own
 * track spans.
 */
#ifndef TS_APP_SATELLITEPROPERTIESDIALOG_H
#define TS_APP_SATELLITEPROPERTIESDIALOG_H

#include <QColor>
#include <QDialog>
#include <QWidget>

#include "ts/orbits/satellitelayer.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QEvent;
class QLabel;
class QPushButton;

namespace ts
{

/**
 * A span of time: an amount and its unit (minutes or orbits).
 *
 * @guithread
 */
class TimeSpanEdit : public QWidget
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(TimeSpanEdit)

public:
  /** Creates the editor. */
  explicit TimeSpanEdit(QWidget* parent);

  /** Destroys it. */
  ~TimeSpanEdit() override;

  /** Returns the span. */
  TimeSpan span() const;

  /** Sets the span. */
  void setSpan(TimeSpan span);

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

private:
  void retranslateUi();

  QDoubleSpinBox* m_amount = nullptr;
  QComboBox* m_unit = nullptr;
};

/**
 * The style of a satellite layer: colour, marker size, labels, which
 * satellites draw tracks and footprints, the past and future spans of
 * tracks, ground tracks, the footprints' minimum elevation, and the age
 * past which elements are marked as old.
 *
 * @guithread
 */
class SatellitePropertiesDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(SatellitePropertiesDialog)

public:
  /** Creates the dialog for @p style, of the layer named @p name. */
  SatellitePropertiesDialog(const SatelliteStyle& style, QString name,
                            QWidget* parent);

  /** Destroys it. */
  ~SatellitePropertiesDialog() override;

  /** Returns the style chosen (with the original's satellites' settings). */
  SatelliteStyle style() const;

  /** Returns the past-span editor (for tests). */
  TimeSpanEdit* pastEdit() const { return m_past; }

  /** Returns the future-span editor (for tests). */
  TimeSpanEdit* futureEdit() const { return m_future; }

  /** Returns the track box (for tests). */
  QComboBox* tracksBox() const { return m_tracks; }

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

private:
  void retranslateUi();
  void chooseColor();
  void showColor();

  SatelliteStyle m_original;
  QString m_name;
  QColor m_color;
  QPushButton* m_colorButton = nullptr;
  QDoubleSpinBox* m_size = nullptr;
  QCheckBox* m_labels = nullptr;
  QComboBox* m_tracks = nullptr;
  TimeSpanEdit* m_past = nullptr;
  TimeSpanEdit* m_future = nullptr;
  QCheckBox* m_groundTrack = nullptr;
  QComboBox* m_footprints = nullptr;
  QDoubleSpinBox* m_elevation = nullptr;
  QDoubleSpinBox* m_maximumAge = nullptr;
  QLabel* m_colorLabel = nullptr;
  QLabel* m_sizeLabel = nullptr;
  QLabel* m_tracksLabel = nullptr;
  QLabel* m_pastLabel = nullptr;
  QLabel* m_futureLabel = nullptr;
  QLabel* m_footprintsLabel = nullptr;
  QLabel* m_elevationLabel = nullptr;
  QLabel* m_ageLabel = nullptr;
};

/**
 * One satellite's own track spans: its past and future, each the layer's
 * or its own.
 *
 * @guithread
 */
class SpanOverrideDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(SpanOverrideDialog)

public:
  /**
   * Creates the dialog for the satellites named @p names, with @p current
   * and the layer's spans @p layerPast and @p layerFuture.
   */
  SpanOverrideDialog(const QString& names, const SpanOverride& current,
                     TimeSpan layerPast, TimeSpan layerFuture, QWidget* parent);

  /** Destroys it. */
  ~SpanOverrideDialog() override;

  /** Returns what was chosen. */
  SpanOverride spans() const;

private:
  QCheckBox* m_ownPast = nullptr;
  QCheckBox* m_ownFuture = nullptr;
  TimeSpanEdit* m_past = nullptr;
  TimeSpanEdit* m_future = nullptr;
};

} // namespace ts

#endif // TS_APP_SATELLITEPROPERTIESDIALOG_H
