// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/packs/packindex.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

namespace ts
{

namespace
{

QString text(const QJsonObject& json, const char* name)
{
  return json.value(QLatin1String(name)).toString();
}

qint64 bytes(const QJsonObject& json)
{
  return static_cast<qint64>(json.value(QStringLiteral("bytes")).toDouble(-1));
}

bool isSha256(const QString& hex)
{
  if (hex.size() != 64) {
    return false;
  }
  for (const QChar c : hex) {
    if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
          (c >= QLatin1Char('a') && c <= QLatin1Char('f')))) {
      return false;
    }
  }
  return true;
}

std::nullopt_t fail(QString* errorString, const QString& problem)
{
  if (errorString) {
    *errorString = problem;
  }
  return std::nullopt;
}

} // namespace

std::optional<PackIndexEntry> PackIndexEntry::fromJson(const QJsonObject& json,
                                                       bool allowLocalHttp,
                                                       QString* errorString)
{
  PackIndexEntry entry;
  entry.id = text(json, "id");
  entry.version = text(json, "version");
  entry.name = text(json, "name");
  entry.description = text(json, "description");
  entry.licence = text(json, "licence");
  entry.sizeBytes = bytes(json);
  if (!isValidPackId(entry.id)) {
    return fail(errorString, QStringLiteral("bad id \"%1\"").arg(entry.id));
  }
  if (entry.version.isEmpty() || entry.name.isEmpty() || entry.sizeBytes < 0) {
    return fail(errorString,
                QStringLiteral("%1: no version, name or size").arg(entry.id));
  }
  const QJsonObject manifest =
      json.value(QStringLiteral("manifest")).toObject();
  entry.manifestUrl = QUrl(text(manifest, "url"));
  entry.manifestSha256 = text(manifest, "sha256").toLower();
  entry.manifestBytes = bytes(manifest);
  if (!isAllowedPackUrl(entry.manifestUrl, allowLocalHttp) ||
      !isSha256(entry.manifestSha256) || entry.manifestBytes < 0) {
    return fail(errorString,
                QStringLiteral("%1: bad manifest entry").arg(entry.id));
  }
  const QJsonObject files = json.value(QStringLiteral("files")).toObject();
  for (QJsonObject::const_iterator it = files.constBegin();
       it != files.constEnd(); ++it) {
    const QUrl url(it.value().toString());
    if (!isAllowedPackUrl(url, allowLocalHttp)) {
      return fail(errorString,
                  QStringLiteral("%1: bad URL for %2").arg(entry.id, it.key()));
    }
    entry.files[it.key()] = url;
  }
  if (entry.files.empty()) {
    return fail(errorString, QStringLiteral("%1: no files").arg(entry.id));
  }
  return entry;
}

QJsonObject PackIndexEntry::toJson() const
{
  QJsonObject manifest;
  manifest.insert(QStringLiteral("url"), manifestUrl.toString());
  manifest.insert(QStringLiteral("sha256"), manifestSha256);
  manifest.insert(QStringLiteral("bytes"), static_cast<double>(manifestBytes));
  QJsonObject fileUrls;
  for (const std::pair<const QString, QUrl>& file : files) {
    fileUrls.insert(file.first, file.second.toString());
  }
  QJsonObject json;
  json.insert(QStringLiteral("id"), id);
  json.insert(QStringLiteral("version"), version);
  json.insert(QStringLiteral("name"), name);
  json.insert(QStringLiteral("description"), description);
  json.insert(QStringLiteral("licence"), licence);
  json.insert(QStringLiteral("bytes"), static_cast<double>(sizeBytes));
  json.insert(QStringLiteral("manifest"), manifest);
  json.insert(QStringLiteral("files"), fileUrls);
  return json;
}

bool isValidPackId(const QString& id)
{
  if (id.isEmpty() || id.size() > 64 || id.startsWith(QLatin1Char('-'))) {
    return false;
  }
  for (const QChar c : id) {
    if (!((c >= QLatin1Char('a') && c <= QLatin1Char('z')) ||
          (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
          c == QLatin1Char('-'))) {
      return false;
    }
  }
  return true;
}

bool isAllowedPackUrl(const QUrl& url, bool allowLocalHttp)
{
  if (!url.isValid() || url.host().isEmpty()) {
    return false;
  }
  if (url.scheme() == QLatin1String("https")) {
    return true;
  }
  return allowLocalHttp && url.scheme() == QLatin1String("http") &&
         QHostAddress(url.host()).isLoopback();
}

QUrl defaultPackIndexUrl()
{
  const QString fromEnvironment = qEnvironmentVariable("TS_PACK_INDEX_URL");
  if (!fromEnvironment.isEmpty()) {
    return QUrl(fromEnvironment);
  }
  return QUrl(QStringLiteral( // ts-notr: URL
      "https://raw.githubusercontent.com/marutan/terrasoar-packs/main/"
      "index.json"));
}

std::optional<PackIndex> PackIndex::fromJson(const QByteArray& json,
                                             QString* errorString,
                                             std::vector<QString>* skipped,
                                             bool allowLocalHttp)
{
  QJsonParseError parseError{};
  const QJsonDocument document = QJsonDocument::fromJson(json, &parseError);
  if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
    if (errorString) {
      *errorString = QStringLiteral("The pack index is not a JSON object: %1")
                         .arg(parseError.errorString());
    }
    return std::nullopt;
  }
  const QJsonObject root = document.object();
  const int format = root.value(QStringLiteral("format")).toInt(0);
  if (format < 1 || format > currentFormat) {
    if (errorString) {
      *errorString =
          QStringLiteral("Unsupported pack index format %1").arg(format);
    }
    return std::nullopt;
  }
  PackIndex index;
  const QJsonArray packs = root.value(QStringLiteral("packs")).toArray();
  // An index loop: QJsonArray's iterator differs between Qt 5 and Qt 6.
  // NOLINTNEXTLINE(modernize-loop-convert)
  for (int i = 0; i < packs.size(); ++i) {
    QString problem;
    std::optional<PackIndexEntry> entry = PackIndexEntry::fromJson(
        packs.at(i).toObject(), allowLocalHttp, &problem);
    if (entry && index.pack(entry->id)) {
      problem = QStringLiteral("%1: listed twice").arg(entry->id);
      entry.reset();
    }
    if (!entry) {
      if (skipped) {
        skipped->push_back(
            QStringLiteral("Pack index entry %1: %2").arg(i).arg(problem));
      }
      continue;
    }
    index.packs.push_back(*entry);
  }
  return index;
}

std::optional<PackIndexEntry> PackIndex::pack(const QString& id) const
{
  for (const PackIndexEntry& entry : packs) {
    if (entry.id == id) {
      return entry;
    }
  }
  return std::nullopt;
}

} // namespace ts
