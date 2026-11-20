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

/** A latitude/longitude box being grown; empty to start with. */
struct Bounds {
  double west = 1e9;
  double south = 1e9;
  double east = -1e9;
  double north = -1e9;
};

void extend(const std::vector<KmlCoordinate>& coordinates, Bounds& bounds)
{
  for (const KmlCoordinate& c : coordinates) {
    bounds.west = std::min(bounds.west, c.lonDeg);
    bounds.east = std::max(bounds.east, c.lonDeg);
    bounds.south = std::min(bounds.south, c.latDeg);
    bounds.north = std::max(bounds.north, c.latDeg);
  }
}

void extendGeometry(const KmlGeometry& geometry, Bounds& bounds)
{
  extend(geometry.coordinates, bounds);
  for (const KmlGeometry& child : geometry.children) {
    extendGeometry(child, bounds);
  }
}

/** The value of $[key] (or $[key/displayName]) in a balloon's template. */
QString balloonValue(const KmlFeature& placemark, const QString& key,
                     bool displayName)
{
  if (key == QLatin1String("name")) {
    return placemark.name.toHtmlEscaped();
  }
  if (key == QLatin1String("description")) {
    return placemark.description;
  }
  if (key == QLatin1String("Snippet")) {
    return placemark.snippet.toHtmlEscaped();
  }
  if (key == QLatin1String("id")) {
    return placemark.id.toHtmlEscaped();
  }
  QString value;
  for (const std::pair<QString, QString>& data : placemark.extendedData) {
    if (data.first == key) {
      value = displayName ? data.first.toHtmlEscaped()
                          : data.second.toHtmlEscaped();
    }
  }
  return value;
}

/** A BalloonStyle's text with its $[…] filled in. */
QString filledBalloon(const KmlFeature& placemark, const QString& text)
{
  static const QRegularExpression entity(
      QStringLiteral("\\$\\[([^\\]/]+)(/displayName)?\\]"));
  QString filled;
  int32_t from = 0;
  QRegularExpressionMatchIterator matches = entity.globalMatch(text);
  while (matches.hasNext()) {
    const QRegularExpressionMatch match = matches.next();
    filled +=
        text.mid(from, static_cast<int32_t>(match.capturedStart()) - from);
    filled += balloonValue(placemark, match.captured(1),
                           !match.captured(2).isEmpty());
    from = static_cast<int32_t>(match.capturedEnd());
  }
  return filled + text.mid(from);
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
  Bounds bounds;
  if (feature.geometry) {
    extendGeometry(*feature.geometry, bounds);
  } else if (feature.latLonBox && !feature.latLonBox->quad.empty()) {
    extend(feature.latLonBox->quad, bounds);
  } else if (feature.latLonBox) {
    bounds = Bounds{feature.latLonBox->west, feature.latLonBox->south,
                    feature.latLonBox->east, feature.latLonBox->north};
  }
  const double west = bounds.west;
  const double south = bounds.south;
  const double east = bounds.east;
  const double north = bounds.north;
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
    return filledBalloon(placemark, balloon.text);
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
