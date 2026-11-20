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
#include "clockwidget.h"

#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

#include <QAction>
#include <QDateTimeEdit>
#include <QDialogButtonBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QPushButton>
#include <QStyle>
#include <QTimeZone>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include "ts/time/programclock.h"

namespace ts
{

namespace
{

/** The steps offered, seconds. */
const std::vector<double>& stepChoices()
{
  static const std::vector<double> steps{1.0,   10.0,   60.0,
                                         600.0, 3600.0, 86400.0};
  return steps;
}

QString stepLabel(double seconds)
{
  if (seconds >= 86400.0) {
    return ClockWidget::tr(
        "%n day(s)", nullptr,
        static_cast<int32_t>(std::lround(seconds / 86400.0)));
  }
  if (seconds >= 3600.0) {
    return ClockWidget::tr("%n hour(s)", nullptr,
                           static_cast<int32_t>(std::lround(seconds / 3600.0)));
  }
  if (seconds >= 60.0) {
    return ClockWidget::tr("%n minute(s)", nullptr,
                           static_cast<int32_t>(std::lround(seconds / 60.0)));
  }
  return ClockWidget::tr("%n second(s)", nullptr,
                         static_cast<int32_t>(std::lround(seconds)));
}

} // namespace

JumpToTimeDialog::JumpToTimeDialog(const QDateTime& time, QWidget* parent)
    : QDialog(parent), m_edit(new QDateTimeEdit(this))
{
  setObjectName(QStringLiteral("jumpToTimeDialog"));
  setWindowTitle(tr("Jump to Date and Time"));
  m_edit->setObjectName(QStringLiteral("jumpTime"));
  m_edit->setCalendarPopup(true);
  m_edit->setDisplayFormat(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
  // The clock is UTC; the edit shows UTC.
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
  m_edit->setTimeZone(QTimeZone::utc());
#else
  m_edit->setTimeSpec(Qt::UTC);
#endif
  m_edit->setDateTimeRange(
      QDateTime(QDate(1900, 1, 1), QTime(0, 0), QTimeZone::utc()),
      QDateTime(QDate(2200, 12, 31), QTime(23, 59, 59), QTimeZone::utc()));
  setTime(time);
  QLabel* label = new QLabel(tr("&Date and time (UTC):"), this);
  label->setBuddy(m_edit);
  QDialogButtonBox* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  QPushButton* now =
      buttons->addButton(tr("&Now"), QDialogButtonBox::ResetRole);
  now->setObjectName(QStringLiteral("jumpNow"));
  connect(now, &QPushButton::clicked, this,
          [this]() { setTime(QDateTime::currentDateTimeUtc()); });
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(label);
  layout->addWidget(m_edit);
  layout->addWidget(buttons);
}

JumpToTimeDialog::~JumpToTimeDialog() = default;

QDateTime JumpToTimeDialog::time() const
{
  QDateTime time = m_edit->dateTime();
  time.setTimeZone(QTimeZone::utc());
  return time;
}

void JumpToTimeDialog::setTime(const QDateTime& time)
{
  m_edit->setDateTime(time.toUTC());
}

ClockWidget::ClockWidget(ProgramClock* clock, QWidget* parent)
    : QWidget(parent), m_clock(clock), m_timeButton(new QToolButton(this)),
      m_rateButton(new QToolButton(this)), m_nowButton(new QToolButton(this)),
      m_rateMenu(new QMenu(this)), m_stepMenu(new QMenu(this)),
      m_playAction(new QAction(this)), m_forwardAction(new QAction(this)),
      m_backAction(new QAction(this)), m_fasterAction(new QAction(this)),
      m_slowerAction(new QAction(this)), m_realTimeAction(new QAction(this)),
      m_jumpAction(new QAction(this)), m_refresh(new QTimer(this))
{
  setObjectName(QStringLiteral("clockWidget"));
  m_playAction->setObjectName(QStringLiteral("clockPlayAction"));
  m_playAction->setShortcut(QKeySequence(static_cast<int32_t>(Qt::CTRL) |
                                         static_cast<int32_t>(Qt::Key_Space)));
  m_forwardAction->setObjectName(QStringLiteral("clockStepForwardAction"));
  m_forwardAction->setShortcut(QKeySequence(
      static_cast<int32_t>(Qt::CTRL) | static_cast<int32_t>(Qt::Key_Period)));
  m_backAction->setObjectName(QStringLiteral("clockStepBackAction"));
  m_backAction->setShortcut(QKeySequence(static_cast<int32_t>(Qt::CTRL) |
                                         static_cast<int32_t>(Qt::Key_Comma)));
  m_fasterAction->setObjectName(QStringLiteral("clockFasterAction"));
  m_fasterAction->setShortcut(
      QKeySequence(static_cast<int32_t>(Qt::CTRL) |
                   static_cast<int32_t>(Qt::Key_BracketRight)));
  m_slowerAction->setObjectName(QStringLiteral("clockSlowerAction"));
  m_slowerAction->setShortcut(
      QKeySequence(static_cast<int32_t>(Qt::CTRL) |
                   static_cast<int32_t>(Qt::Key_BracketLeft)));
  m_realTimeAction->setObjectName(QStringLiteral("clockRealTimeAction"));
  m_jumpAction->setObjectName(QStringLiteral("clockJumpAction"));
  m_jumpAction->setShortcut(QKeySequence(static_cast<int32_t>(Qt::CTRL) |
                                         static_cast<int32_t>(Qt::Key_J)));

  connect(m_playAction, &QAction::triggered, this, &ClockWidget::togglePlay);
  connect(m_forwardAction, &QAction::triggered, this,
          [this]() { m_clock->step(m_stepSeconds); });
  connect(m_backAction, &QAction::triggered, this,
          [this]() { m_clock->step(-m_stepSeconds); });
  connect(m_fasterAction, &QAction::triggered, this,
          [this]() { m_clock->setRate(m_clock->fasterRate()); });
  connect(m_slowerAction, &QAction::triggered, this,
          [this]() { m_clock->setRate(m_clock->slowerRate()); });
  connect(m_realTimeAction, &QAction::triggered, m_clock,
          &ProgramClock::setRealTime);
  connect(m_jumpAction, &QAction::triggered, this, &ClockWidget::jump);

  const std::vector<std::pair<QToolButton**, QAction*>> buttons{
      {&m_backButton, m_backAction},
      {&m_playButton, m_playAction},
      {&m_forwardButton, m_forwardAction}};
  QHBoxLayout* layout = new QHBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  layout->setSpacing(1);
  for (const std::pair<QToolButton**, QAction*>& b : buttons) {
    *b.first = new QToolButton(this);
    (*b.first)->setDefaultAction(b.second);
    (*b.first)->setAutoRaise(true);
    layout->addWidget(*b.first);
  }
  m_backAction->setIcon(style()->standardIcon(QStyle::SP_MediaSeekBackward));
  m_forwardAction->setIcon(style()->standardIcon(QStyle::SP_MediaSeekForward));
  m_timeButton->setObjectName(QStringLiteral("clockTime"));
  m_timeButton->setAutoRaise(true);
  connect(m_timeButton, &QToolButton::clicked, this, &ClockWidget::jump);
  m_timeButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
  layout->addWidget(m_timeButton);
  m_rateButton->setObjectName(QStringLiteral("clockRate"));
  m_rateButton->setAutoRaise(true);
  m_rateButton->setPopupMode(QToolButton::InstantPopup);
  m_rateButton->setMenu(m_rateMenu);
  layout->addWidget(m_rateButton);
  m_nowButton->setObjectName(QStringLiteral("clockNow"));
  m_nowButton->setAutoRaise(true);
  m_nowButton->setDefaultAction(m_realTimeAction);
  m_nowButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
  layout->addWidget(m_nowButton);

  m_refresh->setInterval(250);
  connect(m_refresh, &QTimer::timeout, this, &ClockWidget::updateTime);
  connect(m_clock, &ProgramClock::timeChanged, this, &ClockWidget::updateTime);
  connect(m_clock, &ProgramClock::runningChanged, this,
          &ClockWidget::updateState);
  retranslateUi();
}

ClockWidget::~ClockWidget() = default;

QString ClockWidget::rateLabel(double rate)
{
  const QLocale locale;
  // Whole speeds without decimals, slow ones with one.
  const QString number =
      std::fabs(rate - std::round(rate)) < 1e-9
          ? locale.toString(static_cast<qint64>(std::llround(rate)))
          : locale.toString(rate, 'g', 3);
  return tr("×%1").arg(number);
}

void ClockWidget::setStepSeconds(double seconds)
{
  m_stepSeconds = std::max(seconds, 0.001);
  retranslateUi();
}

QString ClockWidget::timeText() const
{
  return m_timeButton->text();
}

QString ClockWidget::rateText() const
{
  return m_rateButton->text();
}

void ClockWidget::togglePlay()
{
  if (m_clock->isRunning()) {
    m_clock->pause();
  } else {
    m_clock->play();
  }
}

void ClockWidget::jump()
{
  JumpToTimeDialog dialog(m_clock->time(), this);
  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  const bool running = m_clock->isRunning();
  m_clock->setTime(dialog.time());
  if (running) {
    m_clock->play();
  }
}

void ClockWidget::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QWidget::changeEvent(event);
}

void ClockWidget::retranslateUi()
{
  const QString step = stepLabel(m_stepSeconds);
  m_forwardAction->setText(tr("Step &Forward"));
  m_forwardAction->setToolTip(tr("Step forward %1").arg(step));
  m_backAction->setText(tr("Step &Back"));
  m_backAction->setToolTip(tr("Step back %1").arg(step));
  m_fasterAction->setText(tr("&Faster"));
  m_slowerAction->setText(tr("&Slower"));
  m_realTimeAction->setText(tr("&Now"));
  m_realTimeAction->setToolTip(tr("Run the clock in real time, from now"));
  m_jumpAction->setText(tr("&Jump to Date and Time…"));
  m_jumpAction->setToolTip(tr("The program's time (UTC): click to jump to a "
                              "date and time"));
  m_rateButton->setToolTip(tr("How fast the clock runs"));
  m_timeButton->setToolTip(m_jumpAction->toolTip());
  rebuildRateMenu();
  updateState();
}

void ClockWidget::rebuildRateMenu()
{
  m_rateMenu->clear();
  m_rateMenu->addAction(m_fasterAction);
  m_rateMenu->addAction(m_slowerAction);
  m_rateMenu->addSeparator();
  for (const double rate : ProgramClock::rates()) {
    QAction* action = m_rateMenu->addAction(rateLabel(rate));
    action->setCheckable(true);
    action->setData(rate);
    connect(action, &QAction::triggered, this,
            [this, rate]() { m_clock->setRate(rate); });
  }
  m_rateMenu->addSeparator();
  // The submenu stays (clear() keeps it); only its actions are made again.
  m_stepMenu->clear();
  m_stepMenu->setTitle(tr("S&tep"));
  m_rateMenu->addMenu(m_stepMenu);
  for (const double seconds : stepChoices()) {
    QAction* action = m_stepMenu->addAction(stepLabel(seconds));
    action->setCheckable(true);
    action->setChecked(seconds == m_stepSeconds);
    action->setData(seconds);
    connect(action, &QAction::triggered, this,
            [this, seconds]() { setStepSeconds(seconds); });
  }
}

void ClockWidget::updateState()
{
  const bool running = m_clock->isRunning();
  m_playAction->setText(running ? tr("&Pause") : tr("&Play"));
  m_playAction->setToolTip(running ? tr("Pause the clock")
                                   : tr("Run the clock"));
  m_playAction->setIcon(style()->standardIcon(running ? QStyle::SP_MediaPause
                                                      : QStyle::SP_MediaPlay));
  m_rateButton->setText(m_clock->isRealTime() ? tr("Real time")
                                              : rateLabel(m_clock->rate()));
  for (QAction* action : m_rateMenu->actions()) {
    if (action->isCheckable()) {
      action->setChecked(action->data().toDouble() == m_clock->rate());
    }
  }
  m_fasterAction->setEnabled(m_clock->fasterRate() != m_clock->rate());
  m_slowerAction->setEnabled(m_clock->slowerRate() != m_clock->rate());
  m_realTimeAction->setEnabled(!m_clock->isRealTime());
  if (running) {
    m_refresh->start();
  } else {
    m_refresh->stop();
  }
  updateTime();
}

void ClockWidget::updateTime()
{
  const QString text =
      m_clock->time().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
  const QString shown = tr("%1 UTC").arg(text);
  if (m_timeButton->text() != shown) {
    m_timeButton->setText(shown);
  }
}

} // namespace ts
