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
#include "satelliteinfowindow.h"

#include <cmath>

#include <QDialogButtonBox>
#include <QEvent>
#include <QLocale>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "ts/geo/geoformat.h"
#include "ts/orbits/earthframes.h"

namespace ts
{

namespace
{

QString timeText(JulianDate time)
{
  return time.toDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QString row(const QString& name, const QString& value)
{
  return QStringLiteral("<tr><td><b>%1</b></td><td>%2</td></tr>")
      .arg(name.toHtmlEscaped(), value);
}

bool sameObserver(const std::optional<Geodetic>& a,
                  const std::optional<Geodetic>& b)
{
  if (!a || !b) {
    return !a && !b;
  }
  // Passes hardly change within a kilometre.
  return std::fabs(a->latDeg - b->latDeg) < 0.01 &&
         std::fabs(a->lonDeg - b->lonDeg) < 0.01;
}

} // namespace

SatelliteInfoWindow::SatelliteInfoWindow(QWidget* parent)
    : QDialog(parent), m_browser(new QTextBrowser(this)),
      m_refresh(new QPushButton(this))
{
  setObjectName(QStringLiteral("satelliteInfoWindow"));
  m_browser->setObjectName(QStringLiteral("satelliteInfo"));
  m_refresh->setObjectName(QStringLiteral("refreshSatellite"));
  QDialogButtonBox* buttons =
      new QDialogButtonBox(QDialogButtonBox::Close, this);
  buttons->addButton(m_refresh, QDialogButtonBox::ActionRole);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::hide);
  connect(m_refresh, &QPushButton::clicked, this,
          &SatelliteInfoWindow::refreshRequested);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(m_browser);
  layout->addWidget(buttons);
  resize(420, 460);
  setWindowTitle(tr("Satellite"));
  m_refresh->setText(tr("&Refresh Elements"));
}

SatelliteInfoWindow::~SatelliteInfoWindow() = default;

int32_t SatelliteInfoWindow::catalogueNumber() const
{
  return m_satellite ? m_satellite->sgp4->elements().catalogueNumber : -1;
}

void SatelliteInfoWindow::showSatellite(const DrawnSatellite& satellite,
                                        const SatelliteStyle& style,
                                        const QString& layerName,
                                        const QDateTime& time, bool canRefresh)
{
  m_satellite = satellite;
  m_style = style;
  m_layerName = layerName;
  m_time = time;
  m_canRefresh = canRefresh;
  updatePass();
  render();
}

void SatelliteInfoWindow::setObserver(const std::optional<Geodetic>& observer)
{
  m_observer = observer;
  updatePass();
  render();
}

QString SatelliteInfoWindow::text() const
{
  return m_browser->toPlainText();
}

void SatelliteInfoWindow::updatePass()
{
  if (!m_satellite || !m_observer) {
    m_pass.reset();
    m_passSearched = false;
    return;
  }
  const int32_t number = m_satellite->sgp4->elements().catalogueNumber;
  const JulianDate now = JulianDate::fromDateTime(m_time);
  // Searched again for another satellite or place, when the clock went
  // back before the search, once the pass found is over, or a day on
  // after finding none.
  const bool stale = !m_passSearched || number != m_passSatellite ||
                     !sameObserver(m_observer, m_passObserver) ||
                     now.minutesSince(m_passFrom) < 0.0 ||
                     (m_pass && now.minutesSince(m_pass->set) > 0.0) ||
                     (!m_pass && now.minutesSince(m_passFrom) > 1440.0);
  if (!stale) {
    return;
  }
  m_pass = nextPass(*m_satellite->sgp4, *m_observer, now,
                    m_style.footprintElevationDeg);
  m_passSatellite = number;
  m_passObserver = m_observer;
  m_passFrom = now;
  m_passSearched = true;
}

void SatelliteInfoWindow::render()
{
  if (!m_satellite) {
    m_browser->clear();
    return;
  }
  const QLocale locale;
  const OrbitalElements& e = m_satellite->sgp4->elements();
  const SatelliteState& s = m_satellite->state;
  const double age = e.ageDays(m_time);
  QString html = QStringLiteral("<h3>%1</h3><table cellspacing=\"4\">")
                     .arg(e.displayName().toHtmlEscaped());
  html += row(tr("Layer"), m_layerName.toHtmlEscaped());
  html += row(tr("Catalogue number"), QString::number(e.catalogueNumber));
  if (!e.internationalDesignator.isEmpty()) {
    html += row(tr("International designator"),
                e.internationalDesignator.toHtmlEscaped());
  }
  QString ageText = tr("%1 UTC, %2 days before the clock's time")
                        .arg(timeText(e.epoch), locale.toString(age, 'f', 1));
  if (age < 0.0) {
    ageText = tr("%1 UTC, %2 days after the clock's time")
                  .arg(timeText(e.epoch), locale.toString(-age, 'f', 1));
  }
  if (m_satellite->old) {
    ageText += QStringLiteral("<br/><font color=\"#c06000\">%1</font>")
                   .arg(tr("Older than %1 days: the position may be far off.")
                            .arg(locale.toString(m_style.maximumAgeDays))
                            .toHtmlEscaped());
  }
  html += row(tr("Elements"), ageText);
  html += row(tr("Latitude"), formatLatitude(s.geodetic.latDeg, locale));
  html += row(tr("Longitude"), formatLongitude(s.geodetic.lonDeg, locale));
  html += row(tr("Altitude"), tr("%1 km").arg(locale.toString(
                                  s.geodetic.heightM / 1000.0, 'f', 1)));
  html +=
      row(tr("Speed"), tr("%1 km/s").arg(locale.toString(s.speedKmS, 'f', 2)));
  html += row(tr("Period"), tr("%1 min").arg(locale.toString(
                                m_satellite->sgp4->periodMinutes(), 'f', 1)));
  html += row(tr("Inclination"),
              tr("%1°").arg(locale.toString(e.inclinationDeg, 'f', 2)));
  QString pass;
  if (!m_observer) {
    pass = tr("Point at the ground to see the next pass over it.");
  } else if (!m_pass) {
    pass = tr("None above %1° in the next 3 days.")
               .arg(locale.toString(m_style.footprintElevationDeg));
  } else if (m_pass->alwaysUp) {
    pass = tr("Always above %1° from there (highest %2°).")
               .arg(locale.toString(m_style.footprintElevationDeg),
                    locale.toString(m_pass->maxElevationDeg, 'f', 1));
  } else {
    const JulianDate now = JulianDate::fromDateTime(m_time);
    const bool up = now.minutesSince(m_pass->rise) >= 0.0 &&
                    now.minutesSince(m_pass->set) <= 0.0;
    pass =
        tr("Rises %1 UTC (azimuth %2°), highest %3° at %4, sets %5 "
           "(azimuth %6°)%7")
            .arg(timeText(m_pass->rise),
                 locale.toString(m_pass->riseAzimuthDeg, 'f', 0),
                 locale.toString(m_pass->maxElevationDeg, 'f', 1),
                 m_pass->culmination.toDateTime().toString(
                     QStringLiteral("HH:mm:ss")),
                 m_pass->set.toDateTime().toString(QStringLiteral("HH:mm:ss")),
                 locale.toString(m_pass->setAzimuthDeg, 'f', 0),
                 up ? tr(": up now") : QString());
  }
  if (m_observer) {
    pass += QStringLiteral("<br/><small>%1</small>")
                .arg(tr("Over %1 %2")
                         .arg(formatLatitude(m_observer->latDeg, locale),
                              formatLongitude(m_observer->lonDeg, locale))
                         .toHtmlEscaped());
  }
  html += row(tr("Next pass"), pass);
  html += QStringLiteral("</table>");
  m_browser->setHtml(html);
  setWindowTitle(e.displayName());
  m_refresh->setVisible(m_canRefresh);
}

void SatelliteInfoWindow::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    m_refresh->setText(tr("&Refresh Elements"));
    render();
  }
  QDialog::changeEvent(event);
}

} // namespace ts
