// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/providers/providerregistry.h"

#include <QColor>
#include <QCoreApplication>
#include <QJsonDocument>

#include "ts/providers/testproviders.h"
#include "ts/tiling/tilekey.h"

namespace ts
{

namespace
{

/** The source as compact JSON with sorted keys: equal sources, equal keys. */
QString canonical(const QJsonObject& source)
{
  return QString::fromUtf8(
      QJsonDocument(source).toJson(QJsonDocument::Compact));
}

int intMember(const QJsonObject& source, const char* name, int fallback)
{
  return source.value(QLatin1String(name)).toInt(fallback);
}

} // namespace

std::shared_ptr<ImageryProvider>
createImageryProvider(const QJsonObject& source, QString* errorString)
{
  const QString kind = source.value(QStringLiteral("kind")).toString();
  const int maximumLevel =
      intMember(source, "maximumLevel", TileKey::maximumLevel);
  if (kind == QLatin1String("checkerboard")) {
    return std::make_shared<CheckerboardImageryProvider>(
        intMember(source, "tileSize", 256), maximumLevel);
  }
  if (kind == QLatin1String("solid")) {
    const QColor color(source.value(QStringLiteral("color")).toString());
    if (color.isValid()) {
      return std::make_shared<SolidColorImageryProvider>(
          color, intMember(source, "tileSize", 64), maximumLevel);
    }
  }
  if (errorString) {
    *errorString =
        QCoreApplication::translate("ts::ProviderRegistry",
                                    "Unknown or invalid imagery source: %1")
            .arg(canonical(source));
  }
  return nullptr;
}

std::shared_ptr<ImageryProvider>
ProviderRegistry::imagery(const QJsonObject& source, QString* errorString)
{
  const QString key = canonical(source);
  const std::lock_guard<std::mutex> lock(m_mutex);
  const std::unordered_map<QString, std::weak_ptr<ImageryProvider>>::iterator
      it = m_imagery.find(key);
  if (it != m_imagery.end()) {
    std::shared_ptr<ImageryProvider> live = it->second.lock();
    if (live) {
      return live;
    }
  }
  std::shared_ptr<ImageryProvider> provider =
      createImageryProvider(source, errorString);
  if (provider) {
    m_imagery[key] = provider;
  }
  return provider;
}

int ProviderRegistry::liveImageryProviders() const
{
  const std::lock_guard<std::mutex> lock(m_mutex);
  int count = 0;
  for (const std::pair<const QString, std::weak_ptr<ImageryProvider>>& entry :
       m_imagery) {
    count += entry.second.expired() ? 0 : 1;
  }
  return count;
}

} // namespace ts
