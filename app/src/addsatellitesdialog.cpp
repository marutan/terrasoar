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
#include "addsatellitesdialog.h"

#include <QDialogButtonBox>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTabWidget>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>

#include "satellitelayers.h"
#include "ts/orbits/elementsource.h"
#include "ts/orbits/satellitelayer.h"

namespace ts
{

namespace
{

constexpr int32_t groupRole = Qt::UserRole + 1;

} // namespace

AddSatellitesDialog::AddSatellitesDialog(QWidget* parent)
    : QDialog(parent), m_tabs(new QTabWidget(this)),
      m_groupFilter(new QLineEdit(this)), m_groups(new QTreeWidget(this)),
      m_path(new QLineEdit(this)), m_browse(new QPushButton(this)),
      m_fileLabel(new QLabel(this)), m_text(new QPlainTextEdit(this)),
      m_textName(new QLineEdit(this)), m_textNameLabel(new QLabel(this)),
      m_message(new QLabel(this))
{
  setObjectName(QStringLiteral("addSatellitesDialog"));
  m_tabs->setObjectName(QStringLiteral("satelliteSourceTabs"));
  m_groupFilter->setObjectName(QStringLiteral("groupFilter"));
  m_groupFilter->setClearButtonEnabled(true);
  m_groups->setObjectName(QStringLiteral("celestrakGroups"));
  m_groups->setHeaderHidden(true);
  m_path->setObjectName(QStringLiteral("satelliteFile"));
  m_text->setObjectName(QStringLiteral("satelliteText"));
  m_textName->setObjectName(QStringLiteral("satelliteTextName"));
  m_message->setObjectName(QStringLiteral("addSatellitesMessage"));
  m_message->setWordWrap(true);
  QFont mono = m_text->font();
  mono.setFamily(QStringLiteral("monospace"));
  mono.setStyleHint(QFont::Monospace);
  m_text->setFont(mono);
  m_text->setLineWrapMode(QPlainTextEdit::NoWrap);

  QWidget* celestrak = new QWidget(this);
  QVBoxLayout* celestrakLayout = new QVBoxLayout(celestrak);
  celestrakLayout->addWidget(m_groupFilter);
  celestrakLayout->addWidget(m_groups);
  QWidget* file = new QWidget(this);
  QVBoxLayout* fileLayout = new QVBoxLayout(file);
  fileLayout->addWidget(m_fileLabel);
  QHBoxLayout* pathRow = new QHBoxLayout;
  pathRow->addWidget(m_path);
  pathRow->addWidget(m_browse);
  fileLayout->addLayout(pathRow);
  fileLayout->addStretch(1);
  QWidget* text = new QWidget(this);
  QVBoxLayout* textLayout = new QVBoxLayout(text);
  QFormLayout* nameRow = new QFormLayout;
  nameRow->addRow(m_textNameLabel, m_textName);
  textLayout->addLayout(nameRow);
  textLayout->addWidget(m_text);
  m_tabs->addTab(celestrak, QString());
  m_tabs->addTab(file, QString());
  m_tabs->addTab(text, QString());

  QDialogButtonBox* buttons = new QDialogButtonBox(
      QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  connect(buttons, &QDialogButtonBox::accepted, this,
          &AddSatellitesDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(m_browse, &QPushButton::clicked, this, &AddSatellitesDialog::browse);
  connect(m_groupFilter, &QLineEdit::textChanged, this,
          &AddSatellitesDialog::filterGroups);
  connect(m_groups, &QTreeWidget::itemDoubleClicked, this,
          [this](QTreeWidgetItem* item) {
            if (item->data(0, groupRole).isValid()) {
              accept();
            }
          });
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addWidget(m_tabs);
  layout->addWidget(m_message);
  layout->addWidget(buttons);
  resize(520, 560);
  retranslateUi();
}

AddSatellitesDialog::~AddSatellitesDialog() = default;

QUrl AddSatellitesDialog::celestrakUrl()
{
  if (qEnvironmentVariableIsSet("TS_CELESTRAK_URL")) {
    return QUrl(qEnvironmentVariable("TS_CELESTRAK_URL"));
  }
  return celestrakBaseUrl();
}

LayerDefinition AddSatellitesDialog::definitionFor(const QJsonObject& source,
                                                   const QString& name)
{
  LayerDefinition definition;
  definition.id = QStringLiteral("user:satellites:") +
                  QUuid::createUuid().toString(QUuid::WithoutBraces);
  definition.origin = LayerOrigin::User;
  definition.type = LayerType::Vector;
  definition.name = name;
  definition.source = source;
  definition.style = SatelliteStyle().toJson();
  return definition;
}

void AddSatellitesDialog::fillGroups()
{
  const QString chosen =
      m_groups->currentItem() != nullptr
          ? m_groups->currentItem()->data(0, groupRole).toString()
          : QString();
  m_groups->clear();
  QTreeWidgetItem* category = nullptr;
  for (const CelestrakGroup& group : celestrakGroups()) {
    if (category == nullptr || category->text(0) != group.category) {
      category = new QTreeWidgetItem(m_groups, QStringList{group.category});
      category->setFlags(Qt::ItemIsEnabled);
      category->setExpanded(true);
    }
    QTreeWidgetItem* item =
        new QTreeWidgetItem(category, QStringList{group.name});
    item->setData(0, groupRole, group.id);
    item->setToolTip(0, group.id);
    if (group.id == chosen) {
      m_groups->setCurrentItem(item);
    }
  }
  filterGroups(m_groupFilter->text());
}

void AddSatellitesDialog::filterGroups(const QString& text)
{
  for (int32_t c = 0; c < m_groups->topLevelItemCount(); ++c) {
    QTreeWidgetItem* category = m_groups->topLevelItem(c);
    bool any = false;
    for (int32_t i = 0; i < category->childCount(); ++i) {
      QTreeWidgetItem* item = category->child(i);
      const bool match = text.isEmpty() ||
                         item->text(0).contains(text, Qt::CaseInsensitive) ||
                         item->data(0, groupRole)
                             .toString()
                             .contains(text, Qt::CaseInsensitive);
      item->setHidden(!match);
      any = any || match;
    }
    category->setHidden(!any);
  }
}

void AddSatellitesDialog::chooseGroup(const QString& group)
{
  m_tabs->setCurrentIndex(0);
  for (QTreeWidgetItemIterator it(m_groups); *it != nullptr; ++it) {
    if ((*it)->data(0, groupRole).toString() == group) {
      m_groups->setCurrentItem(*it);
    }
  }
}

void AddSatellitesDialog::chooseFile(const QString& path)
{
  m_tabs->setCurrentIndex(1);
  m_path->setText(path);
}

void AddSatellitesDialog::chooseText(const QString& text)
{
  m_tabs->setCurrentIndex(2);
  m_text->setPlainText(text);
}

QString AddSatellitesDialog::message() const
{
  return m_message->text();
}

void AddSatellitesDialog::browse()
{
  const QString path = QFileDialog::getOpenFileName(
      this, tr("Open Element Sets"), m_path->text(),
      tr("Element sets (*.tle *.3le *.2le *.txt *.json);;All files (*)"));
  if (!path.isEmpty()) {
    m_path->setText(path);
  }
}

void AddSatellitesDialog::accept()
{
  m_message->clear();
  SatelliteSource source;
  QString name;
  switch (m_tabs->currentIndex()) {
  case 0: {
    QTreeWidgetItem* item = m_groups->currentItem();
    if (item == nullptr || !item->data(0, groupRole).isValid()) {
      m_message->setText(tr("Choose a group."));
      return;
    }
    const QString group = item->data(0, groupRole).toString();
    source = SatelliteSource::celestrak(group, celestrakUrl());
    name = celestrakGroupName(group);
    break;
  }
  case 1:
    source.origin = SatelliteSource::Origin::File;
    source.path = QFileInfo(m_path->text().trimmed()).absoluteFilePath();
    name = QFileInfo(source.path).completeBaseName();
    if (m_path->text().trimmed().isEmpty()) {
      m_message->setText(tr("Choose a file."));
      return;
    }
    break;
  default:
    source.origin = SatelliteSource::Origin::Text;
    source.text = m_text->toPlainText().trimmed();
    name = m_textName->text().trimmed();
    if (name.isEmpty()) {
      name = tr("Satellites");
    }
    break;
  }
  if (!source.isDownloaded()) {
    const SatelliteSet set = SatelliteLayers::readLocal(source);
    if (set.satellites.empty()) {
      QString text = set.error;
      if (!set.problems.isEmpty()) {
        text += QLatin1Char(' ') + set.problems.front();
      }
      m_message->setText(text);
      return;
    }
    if (source.origin == SatelliteSource::Origin::Text &&
        m_textName->text().trimmed().isEmpty() && set.satellites.size() == 1) {
      name = set.satellites.front()->elements().displayName();
    }
  }
  m_definition = definitionFor(source.toJson(), name);
  QDialog::accept();
}

void AddSatellitesDialog::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QDialog::changeEvent(event);
}

void AddSatellitesDialog::retranslateUi()
{
  setWindowTitle(tr("Add Satellites"));
  m_tabs->setTabText(0, tr("&CelesTrak"));
  m_tabs->setTabText(1, tr("&File"));
  m_tabs->setTabText(2, tr("&Text"));
  m_groupFilter->setPlaceholderText(tr("Filter groups"));
  m_groups->setToolTip(tr("CelesTrak's current element sets, by group. They "
                          "are downloaded again after two hours at the "
                          "soonest, and kept for use offline."));
  m_browse->setText(tr("&Browse…"));
  m_fileLabel->setText(
      tr("A file of TLEs (two-line or three-line) or OMM JSON:"));
  m_textNameLabel->setText(tr("&Name:"));
  m_textNameLabel->setBuddy(m_textName);
  m_text->setPlaceholderText(
      tr("Paste TLEs here: two-line or three-line, as CelesTrak and "
         "Space-Track give them."));
  fillGroups();
}

} // namespace ts
