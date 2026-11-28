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
#include "ts/kml/kmldocument.h"

#include <functional>
#include <map>

namespace ts
{

KmlStyle KmlStyle::mergedWith(const KmlStyle& over) const
{
  KmlStyle merged = *this;
  if (!over.id.isEmpty()) {
    merged.id = over.id;
  }
  if (over.icon) {
    merged.icon = over.icon;
  }
  if (over.label) {
    merged.label = over.label;
  }
  if (over.line) {
    merged.line = over.line;
  }
  if (over.poly) {
    merged.poly = over.poly;
  }
  if (over.balloon) {
    merged.balloon = over.balloon;
  }
  if (over.list) {
    merged.list = over.list;
  }
  return merged;
}

bool KmlTime::contains(const QDateTime& time) const
{
  return (!begin || time >= *begin) && (!end || time <= *end);
}

namespace
{

/** Returns the first item with @p id in @p feature or inside it, or null. */
template <typename T>
const T*
findIn(const KmlFeature& feature, const QString& id,
       const std::function<const std::vector<T>&(const KmlFeature&)>& listOf)
{
  for (const T& item : listOf(feature)) {
    if (item.id == id) {
      return &item;
    }
  }
  for (const KmlFeature& child : feature.children) {
    const T* found = findIn<T>(child, id, listOf);
    if (found != nullptr) {
      return found;
    }
  }
  return nullptr;
}

} // namespace

void KmlStyleIndex::build(const KmlFeature& root)
{
  // Depth first, the first of an id kept: what findIn() finds.
  m_styles.clear();
  m_styleMaps.clear();
  std::vector<const KmlFeature*> pending = {&root};
  while (!pending.empty()) {
    const KmlFeature* feature = pending.back();
    pending.pop_back();
    for (const KmlStyle& style : feature->styles) {
      m_styles.emplace(style.id, &style);
    }
    for (const KmlStyleMap& map : feature->styleMaps) {
      m_styleMaps.emplace(map.id, &map);
    }
    for (std::vector<KmlFeature>::const_reverse_iterator child =
             feature->children.rbegin();
         child != feature->children.rend(); ++child) {
      pending.push_back(&*child);
    }
  }
  m_built = true;
}

const KmlStyle* KmlStyleIndex::style(const QString& id) const
{
  const std::unordered_map<QString, const KmlStyle*>::const_iterator found =
      m_styles.find(id);
  return found != m_styles.end() ? found->second : nullptr;
}

const KmlStyleMap* KmlStyleIndex::styleMap(const QString& id) const
{
  const std::unordered_map<QString, const KmlStyleMap*>::const_iterator found =
      m_styleMaps.find(id);
  return found != m_styleMaps.end() ? found->second : nullptr;
}

const KmlStyle* KmlDocument::findStyle(const QString& id) const
{
  if (styleIndex.isBuilt()) {
    return styleIndex.style(id);
  }
  return findIn<KmlStyle>(
      root, id, [](const KmlFeature& f) -> const std::vector<KmlStyle>& {
        return f.styles;
      });
}

const KmlStyleMap* KmlDocument::findStyleMap(const QString& id) const
{
  if (styleIndex.isBuilt()) {
    return styleIndex.styleMap(id);
  }
  return findIn<KmlStyleMap>(
      root, id, [](const KmlFeature& f) -> const std::vector<KmlStyleMap>& {
        return f.styleMaps;
      });
}

std::shared_ptr<const KmlDocument> makeSharedKmlDocument(KmlDocument document)
{
  std::shared_ptr<KmlDocument> shared =
      std::make_shared<KmlDocument>(std::move(document));
  shared->styleIndex.build(shared->root);
  return shared;
}

QString kmlSlotOf(const QString& localName)
{
  static const std::map<QString, QString> slotNames{
      {QStringLiteral("LookAt"), QStringLiteral("view")},
      {QStringLiteral("Camera"), QStringLiteral("view")},
      {QStringLiteral("TimeStamp"), QStringLiteral("time")},
      {QStringLiteral("TimeSpan"), QStringLiteral("time")},
      {QStringLiteral("Point"), QStringLiteral("geometry")},
      {QStringLiteral("LineString"), QStringLiteral("geometry")},
      {QStringLiteral("LinearRing"), QStringLiteral("geometry")},
      {QStringLiteral("Polygon"), QStringLiteral("geometry")},
      {QStringLiteral("MultiGeometry"), QStringLiteral("geometry")},
      {QStringLiteral("Track"), QStringLiteral("geometry")},
      {QStringLiteral("MultiTrack"), QStringLiteral("geometry")},
      {QStringLiteral("snippet"), QStringLiteral("Snippet")},
      {QStringLiteral("Url"), QStringLiteral("Link")},
  };
  const std::map<QString, QString>::const_iterator found =
      slotNames.find(localName);
  return found != slotNames.end() ? found->second : localName;
}

QStringList kmlElementsOfSlot(const QString& slot)
{
  static const std::map<QString, QStringList> names{
      {QStringLiteral("view"),
       {QStringLiteral("LookAt"), QStringLiteral("Camera")}},
      {QStringLiteral("time"),
       {QStringLiteral("TimeStamp"), QStringLiteral("TimeSpan")}},
      {QStringLiteral("geometry"),
       {QStringLiteral("Point"), QStringLiteral("LineString"),
        QStringLiteral("LinearRing"), QStringLiteral("Polygon"),
        QStringLiteral("MultiGeometry"), QStringLiteral("Track"),
        QStringLiteral("MultiTrack")}},
      {QStringLiteral("Snippet"),
       {QStringLiteral("Snippet"), QStringLiteral("snippet")}},
      {QStringLiteral("Link"), {QStringLiteral("Link"), QStringLiteral("Url")}},
  };
  const std::map<QString, QStringList>::const_iterator found = names.find(slot);
  return found != names.end() ? found->second : QStringList{slot};
}

QString kmlAltitudeModeName(KmlAltitudeMode mode)
{
  switch (mode) {
  case KmlAltitudeMode::RelativeToGround:
    return QStringLiteral("relativeToGround");
  case KmlAltitudeMode::Absolute:
    return QStringLiteral("absolute");
  case KmlAltitudeMode::ClampToSeaFloor:
    return QStringLiteral("clampToSeaFloor");
  case KmlAltitudeMode::RelativeToSeaFloor:
    return QStringLiteral("relativeToSeaFloor");
  case KmlAltitudeMode::ClampToGround:
    break;
  }
  return QStringLiteral("clampToGround");
}

QColor kmlColor(const QString& text)
{
  const QString hex = text.trimmed().remove(QLatin1Char('#'));
  bool ok = false;
  const uint32_t value = hex.toUInt(&ok, 16);
  if (!ok || (hex.size() != 8 && hex.size() != 6)) {
    return QColor();
  }
  // aabbggrr (or bbggrr, opaque).
  const int32_t a =
      hex.size() == 8 ? static_cast<int32_t>((value >> 24U) & 0xFFU) : 255;
  const int32_t b = static_cast<int32_t>((value >> 16U) & 0xFFU);
  const int32_t g = static_cast<int32_t>((value >> 8U) & 0xFFU);
  const int32_t r = static_cast<int32_t>(value & 0xFFU);
  return QColor(r, g, b, a);
}

QString kmlColorText(const QColor& color)
{
  return QStringLiteral("%1%2%3%4")
      .arg(color.alpha(), 2, 16, QLatin1Char('0'))
      .arg(color.blue(), 2, 16, QLatin1Char('0'))
      .arg(color.green(), 2, 16, QLatin1Char('0'))
      .arg(color.red(), 2, 16, QLatin1Char('0'));
}

} // namespace ts
