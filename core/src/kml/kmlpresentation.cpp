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
#include "ts/kml/kmlpresentation.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

#include <QRegularExpression>

namespace ts
{

namespace
{

constexpr double pi = 3.14159265358979323846;
constexpr double metresPerDegree = 111320.0;
constexpr double largestTiltDeg = 85.0;

void extend(const std::vector<KmlCoordinate>& coordinates, double* west,
            double* south, double* east, double* north)
{
  for (const KmlCoordinate& c : coordinates) {
    *west = std::min(*west, c.lonDeg);
    *east = std::max(*east, c.lonDeg);
    *south = std::min(*south, c.latDeg);
    *north = std::max(*north, c.latDeg);
  }
}

void extendGeometry(const KmlGeometry& geometry, double* west, double* south,
                    double* east, double* north)
{
  extend(geometry.coordinates, west, south, east, north);
  for (const KmlGeometry& child : geometry.children) {
    extendGeometry(child, west, south, east, north);
  }
}

} // namespace

CameraPose kmlViewPose(const KmlView& view)
{
  CameraPose pose;
  pose.headingDeg = view.headingDeg;
  pose.tiltDeg = std::clamp(view.tiltDeg, 0.0, largestTiltDeg);
  if (view.kind == KmlView::Kind::LookAt) {
    pose.targetLatDeg = view.latDeg;
    pose.targetLonDeg = view.lonDeg;
    pose.rangeM = std::max(view.rangeM, 10.0);
    return pose;
  }
  // A camera at a height looking down at a tilt sees the ground ahead.
  const double height = std::max(view.altM, 10.0);
  const double tilt = pose.tiltDeg * pi / 180.0;
  const double ahead = height * std::tan(tilt);
  const double heading = view.headingDeg * pi / 180.0;
  pose.targetLatDeg = std::clamp(
      view.latDeg + ahead * std::cos(heading) / metresPerDegree, -89.9, 89.9);
  pose.targetLonDeg =
      view.lonDeg + ahead * std::sin(heading) /
                        (metresPerDegree *
                         std::max(std::cos(view.latDeg * pi / 180.0), 0.01));
  pose.rangeM = height / std::cos(tilt);
  return pose;
}

std::optional<CameraPose> kmlFeaturePose(const KmlFeature& feature)
{
  if (feature.view) {
    return kmlViewPose(*feature.view);
  }
  double west = 1e9;
  double south = 1e9;
  double east = -1e9;
  double north = -1e9;
  if (feature.geometry) {
    extendGeometry(*feature.geometry, &west, &south, &east, &north);
  } else if (feature.latLonBox) {
    west = feature.latLonBox->west;
    east = feature.latLonBox->east;
    south = feature.latLonBox->south;
    north = feature.latLonBox->north;
    extend(feature.latLonBox->quad, &west, &south, &east, &north);
  }
  if (west > east) {
    return std::nullopt;
  }
  CameraPose pose;
  pose.targetLatDeg = (south + north) / 2.0;
  pose.targetLonDeg = (west + east) / 2.0;
  const double spanM = std::max((north - south) * metresPerDegree,
                                (east - west) * metresPerDegree *
                                    std::cos(pose.targetLatDeg * pi / 180.0));
  // Points get a street-level view; areas fit the view.
  pose.rangeM = std::clamp(spanM * 2.0, 1500.0, 2.0e7);
  return pose;
}

QString kmlBalloonHtml(const KmlFeature& placemark, const KmlStyle& style)
{
  const KmlBalloonStyle balloon = style.balloon.value_or(KmlBalloonStyle{});
  QString html;
  if (!balloon.text.isEmpty()) {
    static const QRegularExpression entity(
        QStringLiteral("\\$\\[([^\\]/]+)(/displayName)?\\]"));
    html = balloon.text;
    QString filled;
    int32_t from = 0;
    QRegularExpressionMatchIterator matches = entity.globalMatch(html);
    while (matches.hasNext()) {
      const QRegularExpressionMatch match = matches.next();
      const QString key = match.captured(1);
      QString value;
      if (key == QLatin1String("name")) {
        value = placemark.name.toHtmlEscaped();
      } else if (key == QLatin1String("description")) {
        value = placemark.description;
      } else if (key == QLatin1String("Snippet")) {
        value = placemark.snippet.toHtmlEscaped();
      } else if (key == QLatin1String("id")) {
        value = placemark.id.toHtmlEscaped();
      } else if (key == QLatin1String("geDirections")) {
        value = QString();
      } else {
        for (const std::pair<QString, QString>& data : placemark.extendedData) {
          if (data.first == key) {
            value = match.captured(2).isEmpty() ? data.second.toHtmlEscaped()
                                                : data.first.toHtmlEscaped();
          }
        }
      }
      filled +=
          html.mid(from, static_cast<int32_t>(match.capturedStart()) - from);
      filled += value;
      from = static_cast<int32_t>(match.capturedEnd());
    }
    filled += html.mid(from);
    return filled;
  }
  if (!placemark.name.isEmpty()) {
    html += QStringLiteral("<h3>%1</h3>").arg(placemark.name.toHtmlEscaped());
  }
  if (!placemark.description.isEmpty()) {
    html += QStringLiteral("<div>%1</div>").arg(placemark.description);
  }
  if (!placemark.extendedData.empty()) {
    html += QStringLiteral("<table border=\"1\" cellspacing=\"0\" "
                           "cellpadding=\"3\">");
    for (const std::pair<QString, QString>& data : placemark.extendedData) {
      html += QStringLiteral("<tr><th align=\"left\">%1</th><td>%2</td></tr>")
                  .arg(data.first.toHtmlEscaped(), data.second.toHtmlEscaped());
    }
    html += QStringLiteral("</table>");
  }
  return html;
}

} // namespace ts
