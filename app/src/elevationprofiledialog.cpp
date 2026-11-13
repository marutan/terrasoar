// SPDX-License-Identifier: GPL-2.0-or-later
#include "elevationprofiledialog.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <utility>

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>

namespace ts
{

namespace
{

/** Metres as the user reads them: whole metres, or km beyond 10 km. */
QString formatDistance(double metres, const QLocale& locale)
{
  if (metres >= 10000.0) {
    return QCoreApplication::translate("ts::ElevationProfileChart", "%1 km")
        .arg(locale.toString(metres / 1000.0, 'f', 1));
  }
  return QCoreApplication::translate("ts::ElevationProfileChart", "%1 m")
      .arg(locale.toString(std::lround(metres)));
}

QString formatHeight(double metres, const QLocale& locale)
{
  return QCoreApplication::translate("ts::ElevationProfileChart", "%1 m")
      .arg(locale.toString(std::lround(metres)));
}

} // namespace

// --- ElevationProfileChart ----------------------------------------------

ElevationProfileChart::ElevationProfileChart(QWidget* parent) : QWidget(parent)
{
  setObjectName(QStringLiteral("elevationProfileChart"));
  setMouseTracking(true);
  setMinimumSize(320, 160);
}

ElevationProfileChart::~ElevationProfileChart() = default;

QSize ElevationProfileChart::sizeHint() const
{
  return QSize(640, 260);
}

void ElevationProfileChart::setProfile(std::optional<ElevationProfile> profile)
{
  m_profile = std::move(profile);
  m_hovered = -1;
  update();
}

QRectF ElevationProfileChart::plotArea() const
{
  const QFontMetrics metrics(font());
  const double left =
      metrics.horizontalAdvance(QStringLiteral("−10 000 m")) + 8;
  const double bottom = metrics.height() + 6;
  return QRectF(left, 6.0, std::max(width() - left - 10.0, 1.0),
                std::max(height() - bottom - 12.0, 1.0));
}

std::optional<std::size_t> ElevationProfileChart::pointAt(double xPx) const
{
  const QRectF area = plotArea();
  if (!m_profile || m_profile->points.size() < 2 || xPx < area.left() ||
      xPx > area.right()) {
    return std::nullopt;
  }
  const double distance =
      (xPx - area.left()) / area.width() * m_profile->lengthM;
  const std::vector<ProfilePoint>& points = m_profile->points;
  const std::vector<ProfilePoint>::const_iterator next = std::lower_bound(
      points.begin(), points.end(), distance,
      [](const ProfilePoint& point, double d) { return point.distanceM < d; });
  if (next == points.end()) {
    return points.size() - 1;
  }
  std::size_t index = static_cast<std::size_t>(next - points.begin());
  if (index > 0 &&
      distance - points[index - 1].distanceM < next->distanceM - distance) {
    --index;
  }
  return index;
}

void ElevationProfileChart::paintEvent(QPaintEvent* /*event*/)
{
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.fillRect(rect(), palette().base());
  const QRectF area = plotArea();
  painter.setPen(palette().color(QPalette::Mid));
  painter.drawRect(area);
  if (!m_profile || m_profile->points.size() < 2) {
    return;
  }
  const QLocale locale;
  const ElevationProfile& profile = *m_profile;
  // A little room above and below; at least 20 m of range.
  const double middle = 0.5 * (profile.minimumM + profile.maximumM);
  double low = std::min(profile.minimumM, middle - 10.0);
  double high = std::max(profile.maximumM, middle + 10.0);
  const double margin = (high - low) * 0.08;
  low -= margin;
  high += margin;
  const std::function<QPointF(const ProfilePoint&)> toScreen =
      [&](const ProfilePoint& point) {
        return QPointF(area.left() +
                           point.distanceM / profile.lengthM * area.width(),
                       area.bottom() - (point.aboveSeaLevelM() - low) /
                                           (high - low) * area.height());
      };
  QPainterPath line;
  line.moveTo(toScreen(profile.points.front()));
  for (const ProfilePoint& point : profile.points) {
    line.lineTo(toScreen(point));
  }
  QPainterPath ground = line;
  ground.lineTo(area.bottomRight());
  ground.lineTo(area.bottomLeft());
  ground.closeSubpath();
  painter.fillPath(ground, QColor(120, 150, 100, 140));
  if (low < 0.0 && high > 0.0) {
    // Sea level.
    const double y = area.bottom() - (0.0 - low) / (high - low) * area.height();
    painter.setPen(QPen(QColor(60, 120, 200), 1.0, Qt::DashLine));
    painter.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
  }
  painter.setPen(QPen(QColor(60, 80, 40), 1.5));
  painter.drawPath(line);

  // The scale's ends.
  painter.setPen(palette().color(QPalette::Text));
  const QFontMetrics metrics(font());
  const QRectF labels(0.0, area.top(), area.left() - 6.0, area.height());
  painter.drawText(labels, Qt::AlignRight | Qt::AlignTop,
                   formatHeight(high, locale));
  painter.drawText(labels, Qt::AlignRight | Qt::AlignBottom,
                   formatHeight(low, locale));
  const QRectF below(area.left(), area.bottom() + 3.0, area.width(),
                     metrics.height());
  painter.drawText(below, Qt::AlignLeft | Qt::AlignTop,
                   formatDistance(0.0, locale));
  painter.drawText(below, Qt::AlignRight | Qt::AlignTop,
                   formatDistance(profile.lengthM, locale));

  if (m_hovered >= 0 &&
      static_cast<std::size_t>(m_hovered) < profile.points.size()) {
    const ProfilePoint& point =
        profile.points[static_cast<std::size_t>(m_hovered)];
    const QPointF at = toScreen(point);
    painter.setPen(QPen(QColor(255, 90, 60), 1.0));
    painter.drawLine(QPointF(at.x(), area.top()),
                     QPointF(at.x(), area.bottom()));
    painter.setBrush(QColor(255, 90, 60));
    painter.drawEllipse(at, 4.0, 4.0);
    const QString text =
        tr("%1, %2").arg(formatDistance(point.distanceM, locale),
                         formatHeight(point.aboveSeaLevelM(), locale));
    const double textWidth = metrics.horizontalAdvance(text) + 8.0;
    const double x = std::min(at.x() + 6.0, area.right() - textWidth);
    const QRectF box(x, area.top() + 2.0, textWidth, metrics.height() + 4.0);
    painter.fillRect(box, palette().toolTipBase());
    painter.setPen(palette().color(QPalette::ToolTipText));
    painter.drawText(box, Qt::AlignCenter, text);
  }
}

void ElevationProfileChart::mouseMoveEvent(QMouseEvent* event)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  const double x = event->position().x();
#else
  const double x = event->localPos().x();
#endif
  const std::optional<std::size_t> index = pointAt(x);
  const int hovered = index ? static_cast<int>(*index) : -1;
  if (hovered != m_hovered) {
    m_hovered = hovered;
    update();
    emit this->hovered(hovered);
  }
  QWidget::mouseMoveEvent(event);
}

void ElevationProfileChart::leaveEvent(QEvent* event)
{
  if (m_hovered != -1) {
    m_hovered = -1;
    update();
    emit hovered(-1);
  }
  QWidget::leaveEvent(event);
}

// --- ElevationProfileDialog ---------------------------------------------

ElevationProfileDialog::ElevationProfileDialog(QWidget* parent)
    : QDialog(parent), m_chart(new ElevationProfileChart(this)),
      m_summaryLabel(new QLabel(this)), m_statusLabel(new QLabel(this)),
      m_drawButton(new QPushButton(this)), m_saveButton(new QPushButton(this)),
      m_closeButton(new QPushButton(this))
{
  setObjectName(QStringLiteral("elevationProfileDialog"));
  m_summaryLabel->setObjectName(QStringLiteral("profileSummaryLabel"));
  m_statusLabel->setObjectName(QStringLiteral("profileStatusLabel"));
  m_drawButton->setObjectName(QStringLiteral("profileDrawButton"));
  m_saveButton->setObjectName(QStringLiteral("profileSaveButton"));
  m_summaryLabel->setWordWrap(true);
  m_statusLabel->setWordWrap(true);
  m_saveButton->setEnabled(false);

  QHBoxLayout* buttons = new QHBoxLayout;
  buttons->addWidget(m_drawButton);
  buttons->addWidget(m_saveButton);
  buttons->addStretch(1);
  buttons->addWidget(m_closeButton);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(m_chart, 1);
  layout->addWidget(m_summaryLabel);
  layout->addWidget(m_statusLabel);
  layout->addLayout(buttons);

  connect(m_drawButton, &QPushButton::clicked, this,
          &ElevationProfileDialog::drawAnotherRequested);
  connect(m_saveButton, &QPushButton::clicked, this,
          &ElevationProfileDialog::saveCsv);
  connect(m_closeButton, &QPushButton::clicked, this, &QDialog::close);
  connect(m_chart, &ElevationProfileChart::hovered, this, [this](int index) {
    const std::optional<ElevationProfile>& profile = m_chart->profile();
    if (index < 0 || !profile.has_value() ||
        static_cast<std::size_t>(index) >= profile->points.size()) {
      emit pointHovered(std::nullopt);
    } else {
      emit pointHovered(
          profile->points[static_cast<std::size_t>(index)].position);
    }
  });
  retranslateUi();
}

ElevationProfileDialog::~ElevationProfileDialog() = default;

void ElevationProfileDialog::setDrawing()
{
  m_state = State::Drawing;
  retranslateUi();
}

void ElevationProfileDialog::setComputing()
{
  m_state = State::Computing;
  retranslateUi();
}

void ElevationProfileDialog::setProfile(const ElevationProfile& profile)
{
  m_chart->setProfile(profile);
  m_state = State::Ready;
  m_saveButton->setEnabled(true);
  retranslateUi();
}

void ElevationProfileDialog::setFailed(const QString& message)
{
  m_failure = message;
  m_state = State::Failed;
  retranslateUi();
}

QString ElevationProfileDialog::summaryText() const
{
  const std::optional<ElevationProfile>& profile = m_chart->profile();
  if (!profile.has_value()) {
    return QString();
  }
  const QLocale locale;
  return tr("Length %1 · lowest %2 · highest %3 · climb %4 · descent %5 "
            "(heights above sea level)")
      .arg(formatDistance(profile->lengthM, locale),
           formatHeight(profile->minimumM, locale),
           formatHeight(profile->maximumM, locale),
           formatHeight(profile->ascentM, locale),
           formatHeight(profile->descentM, locale));
}

QByteArray ElevationProfileDialog::toCsv(const ElevationProfile& profile)
{
  // ts-notr: file format
  QByteArray csv =
      "distance_m,latitude,longitude,height_m,ellipsoidal_height_m\n";
  const QLocale c = QLocale::c();
  for (const ProfilePoint& point : profile.points) {
    csv += c.toString(point.distanceM, 'f', 1).toLatin1() + ',' +
           c.toString(point.position.latDeg, 'f', 7).toLatin1() + ',' +
           c.toString(point.position.lonDeg, 'f', 7).toLatin1() + ',' +
           c.toString(point.aboveSeaLevelM(), 'f', 1).toLatin1() + ',' +
           c.toString(point.ellipsoidalM, 'f', 1).toLatin1() + '\n';
  }
  return csv;
}

void ElevationProfileDialog::saveCsv()
{
  const std::optional<ElevationProfile>& profile = m_chart->profile();
  if (!profile.has_value()) {
    return;
  }
  const QString path = QFileDialog::getSaveFileName(
      this, tr("Save Elevation Profile"), tr("profile.csv"),
      tr("CSV files (*.csv);;All files (*)"));
  if (path.isEmpty()) {
    return;
  }
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
      file.write(toCsv(*profile)) < 0) {
    QMessageBox::warning(
        this, tr("Save Elevation Profile"),
        tr("Cannot save \"%1\": %2")
            .arg(QDir::toNativeSeparators(path), file.errorString()));
  }
}

void ElevationProfileDialog::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QDialog::changeEvent(event);
}

void ElevationProfileDialog::retranslateUi()
{
  setWindowTitle(tr("Elevation Profile"));
  m_drawButton->setText(tr("&Draw Another Line"));
  m_saveButton->setText(tr("&Save as CSV…"));
  m_closeButton->setText(tr("Close"));
  m_summaryLabel->setText(summaryText());
  switch (m_state) {
  case State::Waiting:
  case State::Drawing:
    m_statusLabel->setText(
        tr("Click points along the line on the globe or map; double-click "
           "or press Enter to finish."));
    break;
  case State::Computing:
    m_statusLabel->setText(tr("Reading the heights along the line…"));
    break;
  case State::Ready:
    m_statusLabel->setText(
        tr("Point at the chart to see where a height is on the globe."));
    break;
  case State::Failed:
    m_statusLabel->setText(m_failure);
    break;
  }
}

} // namespace ts
