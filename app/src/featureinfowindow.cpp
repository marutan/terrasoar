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
#include "featureinfowindow.h"

#include <cstdint>
#include <optional>
#include <utility>

#include <QEvent>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLocale>
#include <QShortcut>
#include <QTimer>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "helpwindow.h"
#include "ts/geo/geoformat.h"
#include "ts/network/networkclient.h"
#include "ts/wms/wmsfeatureinfo.h"

namespace ts
{

namespace
{

/** How often running requests are looked at, ms. */
constexpr int32_t pollIntervalMs = 50;

/** Most lines of a text answer shown. */
constexpr int32_t mostTextLines = 200;

} // namespace

QString FeatureInfoWindow::helpTarget()
{
  return QStringLiteral("adding-wms-layers.md#what-is-here");
}

FeatureInfoWindow::FeatureInfoWindow(std::shared_ptr<NetworkClient> network,
                                     QWidget* parent)
    : QDialog(parent), m_network(std::move(network)),
      m_placeLabel(new QLabel(this)), m_tree(new QTreeWidget(this)),
      m_pollTimer(new QTimer(this))
{
  setObjectName(QStringLiteral("featureInfoWindow"));
  QShortcut* help = new QShortcut(QKeySequence::HelpContents, this);
  connect(help, &QShortcut::activated, this,
          [this]() { showUserGuide(this, helpTarget()); });
  m_placeLabel->setObjectName(QStringLiteral("featureInfoPlace"));
  m_tree->setObjectName(QStringLiteral("featureInfoTree"));
  m_tree->setColumnCount(2);
  m_tree->setAlternatingRowColors(true);
  m_tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
  m_tree->setWordWrap(true);
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(m_placeLabel);
  layout->addWidget(m_tree, 1);
  m_pollTimer->setInterval(pollIntervalMs);
  connect(m_pollTimer, &QTimer::timeout, this, &FeatureInfoWindow::poll);
  retranslateUi();
  resize(460, 420);
}

FeatureInfoWindow::~FeatureInfoWindow()
{
  for (const Pending& pending : m_pending) {
    pending.ticket->cancel();
  }
}

void FeatureInfoWindow::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QDialog::changeEvent(event);
}

void FeatureInfoWindow::retranslateUi()
{
  setWindowTitle(tr("What Is Here"));
  m_tree->setHeaderLabels({tr("Layer and feature"), tr("Value")});
  if (m_hasPoint) {
    const QLocale locale;
    m_placeLabel->setText(tr("At %1, %2:")
                              .arg(formatLatitude(m_point.latDeg, locale),
                                   formatLongitude(m_point.lonDeg, locale)));
  } else {
    m_placeLabel->setText(
        tr("Click a WMS layer on the globe or the map to see what is there."));
  }
}

void FeatureInfoWindow::query(LonLat point,
                              const std::vector<FeatureInfoQuery>& queries)
{
  for (const Pending& pending : m_pending) {
    pending.ticket->cancel();
  }
  m_pending.clear();
  m_tree->clear();
  m_point = point;
  m_hasPoint = true;
  retranslateUi();
  for (const FeatureInfoQuery& query : queries) {
    QTreeWidgetItem* item = new QTreeWidgetItem(m_tree);
    item->setText(0, query.layerName);
    item->setText(1, tr("Asking…"));
    item->setExpanded(true);
    Pending pending;
    pending.item = item;
    pending.format = query.format;
    pending.ticket = m_network->start(HttpRequest{query.url, {}});
    m_pending.push_back(pending);
  }
  if (!m_pending.empty()) {
    m_pollTimer->start();
  } else {
    emit finished();
  }
}

void FeatureInfoWindow::poll()
{
  std::vector<Pending> still;
  for (const Pending& pending : m_pending) {
    if (pending.ticket->isFinished()) {
      answered(pending);
    } else {
      still.push_back(pending);
    }
  }
  m_pending = std::move(still);
  if (m_pending.empty()) {
    m_pollTimer->stop();
    emit finished();
  }
}

void FeatureInfoWindow::answered(const Pending& pending)
{
  const HttpResponse response = pending.ticket->response();
  QTreeWidgetItem* item = pending.item;
  if (response.error == NetworkError::Cancelled) {
    return;
  }
  if (!response.isSuccess()) {
    item->setText(1, response.errorString.isEmpty()
                         ? tr("The server answered %1.").arg(response.status)
                         : response.errorString);
    return;
  }
  QString contentType = QString::fromLatin1(response.header("content-type"));
  if (contentType.isEmpty()) {
    contentType = pending.format;
  }
  QString error;
  const std::optional<WmsFeatureInfo> info =
      parseFeatureInfo(response.body, contentType, &error);
  if (!info) {
    item->setText(1, error);
    return;
  }
  if (info->isEmpty()) {
    item->setText(1, tr("Nothing here"));
    return;
  }
  item->setText(1, info->features.empty()
                       ? QString()
                       : tr("%n feature(s)", nullptr,
                            static_cast<int32_t>(info->features.size())));
  int32_t number = 0;
  for (const FeatureInfoFeature& feature : info->features) {
    ++number;
    QTreeWidgetItem* featureItem = new QTreeWidgetItem(item);
    featureItem->setText(0, feature.id.isEmpty() ? tr("Feature %1").arg(number)
                                                 : feature.id);
    featureItem->setText(1, feature.layer);
    for (const std::pair<QString, QString>& attribute : feature.attributes) {
      QTreeWidgetItem* row = new QTreeWidgetItem(featureItem);
      row->setText(0, attribute.first);
      row->setText(1, attribute.second);
      row->setToolTip(1, attribute.second);
    }
    featureItem->setExpanded(true);
  }
  const QStringList lines = info->text.split(QLatin1Char('\n'));
  for (int32_t i = 0; i < lines.size() && i < mostTextLines; ++i) {
    if (!lines.at(i).trimmed().isEmpty()) {
      QTreeWidgetItem* row = new QTreeWidgetItem(item);
      row->setText(0, lines.at(i));
      row->setFirstColumnSpanned(true);
    }
  }
}

int32_t FeatureInfoWindow::pendingCount() const
{
  return static_cast<int32_t>(m_pending.size());
}

QString FeatureInfoWindow::resultsText() const
{
  QStringList lines;
  for (int32_t i = 0; i < m_tree->topLevelItemCount(); ++i) {
    const QTreeWidgetItem* layer = m_tree->topLevelItem(i);
    lines.append(layer->text(0) + QStringLiteral(": ") + layer->text(1));
    for (int32_t f = 0; f < layer->childCount(); ++f) {
      const QTreeWidgetItem* feature = layer->child(f);
      lines.append(
          QStringLiteral("  ") + feature->text(0) +
          (feature->text(1).isEmpty()
               ? QString()
               : QStringLiteral(" (") + feature->text(1) + QLatin1Char(')')));
      for (int32_t a = 0; a < feature->childCount(); ++a) {
        lines.append(QStringLiteral("    ") + feature->child(a)->text(0) +
                     QStringLiteral(": ") + feature->child(a)->text(1));
      }
    }
  }
  return lines.join(QLatin1Char('\n'));
}

} // namespace ts
