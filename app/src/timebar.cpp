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
#include "timebar.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>

#include <QAction>
#include <QComboBox>
#include <QEvent>
#include <QLabel>
#include <QSignalBlocker>
#include <QSlider>
#include <QStyle>
#include <QTimer>
#include <QToolButton>

#include "ts/time/programclock.h"

namespace ts
{

namespace
{

/** Most times with a slider step of their own. */
constexpr int64_t mostSteps = 2000;
/** Slider positions over a longer extent. */
constexpr int32_t continuousSteps = 10000;

} // namespace

TimeBar::TimeBar(ProgramClock* clock, QWidget* parent)
    : QToolBar(parent), m_clock(clock), m_backButton(new QToolButton(this)),
      m_playButton(new QToolButton(this)),
      m_forwardButton(new QToolButton(this)),
      m_slider(new QSlider(Qt::Horizontal, this)),
      m_timeLabel(new QLabel(this)), m_speedBox(new QComboBox(this)),
      m_playTimer(new QTimer(this))
{
  setObjectName(QStringLiteral("timeBar"));
  setMovable(false);
  m_backButton->setObjectName(QStringLiteral("timeBackButton"));
  m_backButton->setIcon(style()->standardIcon(QStyle::SP_MediaSeekBackward));
  m_playButton->setObjectName(QStringLiteral("timePlayButton"));
  m_playButton->setCheckable(true);
  m_forwardButton->setObjectName(QStringLiteral("timeForwardButton"));
  m_forwardButton->setIcon(style()->standardIcon(QStyle::SP_MediaSeekForward));
  m_slider->setObjectName(QStringLiteral("timeSlider"));
  m_slider->setMinimumWidth(240);
  m_timeLabel->setObjectName(QStringLiteral("timeLabel"));
  m_timeLabel->setMinimumWidth(m_timeLabel->fontMetrics().horizontalAdvance(
                                   QStringLiteral("0000-00-00T00:00:00Z")) +
                               12);
  m_speedBox->setObjectName(QStringLiteral("timeSpeedBox"));
  for (const int32_t ms : {250, 500, 1000, 2000, 5000}) {
    m_speedBox->addItem(QString(), ms);
  }
  m_speedBox->setCurrentIndex(2);
  m_playTimer->setInterval(1000);
  for (QWidget* widget : std::initializer_list<QWidget*>{
           m_backButton, m_playButton, m_forwardButton, m_slider, m_timeLabel,
           m_speedBox}) {
    m_widgetActions.push_back(addWidget(widget));
  }

  connect(m_backButton, &QToolButton::clicked, this, [this]() { stepBack(); });
  connect(m_forwardButton, &QToolButton::clicked, this,
          [this]() { stepForward(); });
  connect(m_playButton, &QToolButton::toggled, this, [this](bool checked) {
    if (checked) {
      play();
    } else {
      pause();
    }
  });
  connect(m_slider, &QSlider::valueChanged, this, &TimeBar::sliderMoved);
  connect(m_speedBox, qOverload<int32_t>(&QComboBox::currentIndexChanged), this,
          [this]() {
            m_playTimer->setInterval(m_speedBox->currentData().toInt());
          });
  connect(m_playTimer, &QTimer::timeout, this, &TimeBar::tick);
  connect(m_clock, &ProgramClock::timeChanged, this, &TimeBar::clockMoved);
  retranslateUi();
  setExtent(TimeExtent());
}

void TimeBar::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QToolBar::changeEvent(event);
}

void TimeBar::retranslateUi()
{
  setWindowTitle(tr("Time"));
  m_backButton->setToolTip(tr("Previous time"));
  m_forwardButton->setToolTip(tr("Next time"));
  m_playButton->setToolTip(tr("Play the layers' times one after another"));
  m_slider->setToolTip(tr("The time the layers show"));
  m_speedBox->setToolTip(tr("How long each time is shown while playing"));
  // The tool bar's own actions for its widgets, as its overflow menu
  // lists them.
  const QStringList names{tr("Previous time"), tr("Play"), tr("Next time"),
                          tr("Time slider"),   tr("Time"), tr("Speed")};
  for (std::size_t i = 0; i < m_widgetActions.size(); ++i) {
    m_widgetActions.at(i)->setText(names.value(static_cast<int32_t>(i)));
  }
  if (!m_timeText.isEmpty()) {
    m_timeLabel->setText(tr("%1 UTC").arg(m_timeText));
  }
  const QStringList speeds{tr("¼ s each"), tr("½ s each"), tr("1 s each"),
                           tr("2 s each"), tr("5 s each")};
  for (int32_t i = 0; i < m_speedBox->count() && i < speeds.size(); ++i) {
    m_speedBox->setItemText(i, speeds.at(i));
  }
  updateButtons();
}

void TimeBar::setExtent(const TimeExtent& extent)
{
  m_extent = extent;
  m_values.clear();
  const int64_t count = extent.count();
  if (count <= mostSteps) {
    m_values = extent.values(mostSteps);
  }
  const bool usable = count > 1;
  const QSignalBlocker blocker(m_slider);
  m_slider->setRange(
      0, m_values.empty()
             ? continuousSteps
             : std::max(0, static_cast<int32_t>(m_values.size()) - 1));
  m_slider->setPageStep(std::max(1, m_slider->maximum() / 10));
  for (QWidget* widget : std::initializer_list<QWidget*>{
           m_slider, m_backButton, m_playButton, m_forwardButton, m_speedBox}) {
    widget->setEnabled(usable);
  }
  if (!usable) {
    pause();
  }
  clockMoved();
}

QDateTime TimeBar::timeAtPosition(int32_t position) const
{
  if (!m_values.empty()) {
    return m_values.at(static_cast<std::size_t>(
        std::clamp(position, 0, static_cast<int32_t>(m_values.size()) - 1)));
  }
  const std::optional<QDateTime> first = m_extent.first();
  const std::optional<QDateTime> last = m_extent.last();
  if (!first || !last) {
    return QDateTime();
  }
  const double fraction =
      static_cast<double>(position) / static_cast<double>(continuousSteps);
  const QDateTime place = first->addMSecs(static_cast<int64_t>(
      static_cast<double>(first->msecsTo(*last)) * fraction));
  return m_extent.atOrBefore(place).value_or(place);
}

int32_t TimeBar::positionOf(const QDateTime& time) const
{
  if (!m_values.empty()) {
    const std::optional<QDateTime> at = m_extent.atOrBefore(time);
    const std::vector<QDateTime>::const_iterator found =
        std::find(m_values.begin(), m_values.end(), at.value_or(QDateTime()));
    return found == m_values.end()
               ? 0
               : static_cast<int32_t>(found - m_values.begin());
  }
  const std::optional<QDateTime> first = m_extent.first();
  const std::optional<QDateTime> last = m_extent.last();
  if (!first || !last || *first >= *last) {
    return 0;
  }
  const double fraction = static_cast<double>(first->msecsTo(time)) /
                          static_cast<double>(first->msecsTo(*last));
  return static_cast<int32_t>(
      std::lround(std::clamp(fraction, 0.0, 1.0) * continuousSteps));
}

void TimeBar::clockMoved()
{
  const QDateTime time = m_clock->time();
  m_timeText = m_extent.isEmpty()
                   ? time.toString(QStringLiteral("yyyy-MM-ddTHH:mm"))
                   : m_extent.format(time);
  m_timeLabel->setText(tr("%1 UTC").arg(m_timeText));
  const QSignalBlocker blocker(m_slider);
  m_slider->setValue(positionOf(time));
  updateButtons();
}

void TimeBar::sliderMoved(int32_t position)
{
  const QDateTime time = timeAtPosition(position);
  if (time.isValid()) {
    m_clock->setTime(time);
  }
}

bool TimeBar::isPlaying() const
{
  return m_playTimer->isActive();
}

void TimeBar::play()
{
  if (m_extent.count() < 2) {
    return;
  }
  if (!m_extent.after(m_clock->time())) {
    m_clock->setTime(m_extent.first().value_or(m_clock->time()));
  }
  m_playTimer->start();
  updateButtons();
}

void TimeBar::pause()
{
  m_playTimer->stop();
  updateButtons();
}

void TimeBar::tick()
{
  if (!stepForward()) {
    pause();
  }
}

bool TimeBar::stepForward()
{
  const std::optional<QDateTime> next = m_extent.after(m_clock->time());
  if (!next) {
    return false;
  }
  m_clock->setTime(*next);
  return true;
}

bool TimeBar::stepBack()
{
  const QDateTime now = m_clock->time();
  // From between two times, back to the one shown, then the one before.
  const std::optional<QDateTime> shown = m_extent.atOrBefore(now);
  std::optional<QDateTime> previous =
      shown && *shown < now ? shown : m_extent.before(now);
  if (!previous) {
    return false;
  }
  m_clock->setTime(*previous);
  return true;
}

void TimeBar::setStepIntervalMs(int32_t intervalMs)
{
  m_playTimer->setInterval(intervalMs);
}

void TimeBar::updateButtons()
{
  const bool playing = isPlaying();
  {
    const QSignalBlocker blocker(m_playButton);
    m_playButton->setChecked(playing);
  }
  m_playButton->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause
                                                      : QStyle::SP_MediaPlay));
  m_playButton->setText(playing ? tr("Pause") : tr("Play"));
}

QString TimeBar::timeText() const
{
  return m_timeText;
}

TimeBar::~TimeBar() = default;

} // namespace ts
