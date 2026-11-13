// SPDX-License-Identifier: GPL-2.0-or-later
#include "addwebterraindialog.h"

#include <algorithm>
#include <array>
#include <initializer_list>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFormLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <cpl_string.h>
#include <cpl_vsi.h>

#include "ts/elevation/webterrainprovider.h"

namespace ts
{

namespace
{

/** A ready-made source. */
struct Preset {
  AddWebTerrainDialog::Kind kind;
  const char* address;
  int maximumZoom;
  const char* datum;
  const char* name;
  const char* attribution;
};

/** The presets after "Custom" (index 0) in the preset box. */
constexpr std::array<Preset, 1> presets = {{
    {AddWebTerrainDialog::Kind::Terrarium,
     "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/{z}/{x}/{y}.png",
     15, "egm96", "Terrain Tiles (AWS Open Data)",
     "Terrain Tiles: Mapzen, AWS Open Data; SRTM, GMTED2010, ETOPO1, NED "
     "and others"},
}};

constexpr int presetCount = static_cast<int>(presets.size());

} // namespace

AddWebTerrainDialog::AddWebTerrainDialog(QWidget* parent)
    : QDialog(parent), m_presetBox(new QComboBox(this)),
      m_kindBox(new QComboBox(this)), m_addressEdit(new QLineEdit(this)),
      m_zoomBox(new QSpinBox(this)), m_tileSizeBox(new QComboBox(this)),
      m_datumBox(new QComboBox(this)), m_nameEdit(new QLineEdit(this)),
      m_attributionEdit(new QLineEdit(this)), m_noteLabel(new QLabel(this)),
      m_errorLabel(new QLabel(this)),
      m_buttons(new QDialogButtonBox(
          QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this))
{
  setObjectName(QStringLiteral("addWebTerrainDialog"));
  m_presetBox->setObjectName(QStringLiteral("webTerrainPresetBox"));
  m_kindBox->setObjectName(QStringLiteral("webTerrainKindBox"));
  m_addressEdit->setObjectName(QStringLiteral("webTerrainAddressEdit"));
  m_zoomBox->setObjectName(QStringLiteral("webTerrainZoomBox"));
  m_tileSizeBox->setObjectName(QStringLiteral("webTerrainTileSizeBox"));
  m_datumBox->setObjectName(QStringLiteral("webTerrainDatumBox"));
  m_nameEdit->setObjectName(QStringLiteral("webTerrainNameEdit"));
  m_attributionEdit->setObjectName(QStringLiteral("webTerrainAttributionEdit"));
  m_errorLabel->setObjectName(QStringLiteral("webTerrainErrorLabel"));

  m_presetBox->addItem(QString());
  for (const Preset& preset : presets) {
    m_presetBox->addItem(QString::fromUtf8(preset.name));
  }
  for (int i = 0; i < 3; ++i) {
    m_kindBox->addItem(QString());
  }
  m_zoomBox->setRange(0, 22);
  m_zoomBox->setValue(15);
  m_tileSizeBox->addItem(QStringLiteral("256"), 256);
  m_tileSizeBox->addItem(QStringLiteral("512"), 512);
  for (const char* datum : {"egm2008", "egm96", "ellipsoid"}) {
    m_datumBox->addItem(QString(), QString::fromLatin1(datum));
  }
  m_noteLabel->setWordWrap(true);
  m_errorLabel->setWordWrap(true);
  m_errorLabel->setStyleSheet(QStringLiteral("color: palette(highlight)"));
  m_errorLabel->hide();

  m_form = new QFormLayout;
  for (QWidget* field : std::initializer_list<QWidget*>{
           m_presetBox, m_kindBox, m_addressEdit, m_zoomBox, m_tileSizeBox,
           m_datumBox, m_nameEdit, m_attributionEdit}) {
    QLabel* label = new QLabel(this);
    label->setBuddy(field);
    m_labels.push_back(label);
    m_form->addRow(label, field);
  }
  QVBoxLayout* layout = new QVBoxLayout(this);
  layout->addLayout(m_form);
  layout->addWidget(m_noteLabel);
  layout->addWidget(m_errorLabel);
  layout->addWidget(m_buttons);

  connect(m_presetBox, qOverload<int>(&QComboBox::activated), this,
          &AddWebTerrainDialog::applyPreset);
  connect(m_kindBox, qOverload<int>(&QComboBox::currentIndexChanged), this,
          &AddWebTerrainDialog::updateControls);
  connect(m_buttons, &QDialogButtonBox::accepted, this,
          &AddWebTerrainDialog::accept);
  connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  retranslateUi();
  updateControls();
}

AddWebTerrainDialog::~AddWebTerrainDialog()
{
  // The kind box says its index changed as it is destroyed, after the
  // controls updateControls() touches.
  disconnect(m_kindBox, nullptr, this, nullptr);
}

bool AddWebTerrainDialog::canReadRemoteFiles()
{
  char** prefixes = VSIGetFileSystemsPrefixes();
  bool found = false;
  for (char** prefix = prefixes; prefix != nullptr && *prefix != nullptr;
       ++prefix) {
    found = found || QLatin1String(*prefix) == QLatin1String("/vsicurl/");
  }
  CSLDestroy(prefixes);
  return found;
}

// The form's fields, in its order.
// NOLINTBEGIN(bugprone-easily-swappable-parameters)
void AddWebTerrainDialog::setSource(Kind kind, const QString& address,
                                    int maximumZoom, const QString& datum,
                                    const QString& name,
                                    const QString& attribution)
// NOLINTEND(bugprone-easily-swappable-parameters)
{
  m_kindBox->setCurrentIndex(static_cast<int>(kind));
  m_addressEdit->setText(address);
  m_zoomBox->setValue(maximumZoom);
  m_datumBox->setCurrentIndex(std::max(m_datumBox->findData(datum), 0));
  m_nameEdit->setText(name);
  m_attributionEdit->setText(attribution);
  updateControls();
}

void AddWebTerrainDialog::applyPreset(int index)
{
  if (index < 1 || index > presetCount) {
    return;
  }
  const Preset& preset = presets.at(static_cast<std::size_t>(index - 1));
  setSource(preset.kind, QString::fromUtf8(preset.address), preset.maximumZoom,
            QString::fromLatin1(preset.datum), QString::fromUtf8(preset.name),
            QString::fromUtf8(preset.attribution));
  m_tileSizeBox->setCurrentIndex(0);
}

void AddWebTerrainDialog::updateControls()
{
  const bool tiles =
      m_kindBox->currentIndex() != static_cast<int>(Kind::CloudGeoTiff);
  m_zoomBox->setEnabled(tiles);
  m_tileSizeBox->setEnabled(tiles);
  if (tiles) {
    m_noteLabel->setText(
        tr("The address has {z}, {x} and {y} where the zoom, column and row "
           "go, e.g. https://example.org/terrain/{z}/{x}/{y}.png. Tiles are "
           "kept on this computer like WMS tiles."));
  } else if (canReadRemoteFiles()) {
    m_noteLabel->setText(tr("The address of one GeoTIFF with overviews, read "
                            "in parts as needed (Cloud Optimized GeoTIFF)."));
  } else {
    m_noteLabel->setText(tr("This build of Terra Soar can't read files over "
                            "the network; download the file and use Add "
                            "Elevation File instead."));
  }
}

std::optional<LayerDefinition> AddWebTerrainDialog::buildDefinition()
{
  const QString address = m_addressEdit->text().trimmed();
  const QString datum = m_datumBox->currentData().toString();
  const Kind kind = static_cast<Kind>(m_kindBox->currentIndex());
  LayerDefinition definition;
  definition.id = QStringLiteral("user:elevation-") +
                  QUuid::createUuid().toString(QUuid::WithoutBraces);
  definition.origin = LayerOrigin::User;
  definition.type = LayerType::Elevation;
  QString error;
  if (kind == Kind::CloudGeoTiff) {
    const QUrl url(address);
    if ((url.scheme() != QLatin1String("http") &&
         url.scheme() != QLatin1String("https")) ||
        url.host().isEmpty()) {
      error = tr("Enter an http or https address of a GeoTIFF.");
    } else if (!canReadRemoteFiles()) {
      error = tr("This build of Terra Soar can't read files over the "
                 "network.");
    } else {
      QJsonObject source;
      source.insert(QStringLiteral("kind"), QStringLiteral("file"));
      source.insert(QStringLiteral("path"),
                    QString(QStringLiteral("/vsicurl/") + url.toString()));
      source.insert(QStringLiteral("verticalDatum"), datum);
      if (!m_attributionEdit->text().trimmed().isEmpty()) {
        source.insert(QStringLiteral("attribution"),
                      m_attributionEdit->text().trimmed());
      }
      definition.source = source;
      definition.name = m_nameEdit->text().trimmed().isEmpty()
                            ? url.fileName()
                            : m_nameEdit->text().trimmed();
    }
  } else {
    WebTerrainSource web;
    web.urlTemplate = address;
    web.encoding = kind == Kind::TerrainRgb ? TerrainEncoding::TerrainRgb
                                            : TerrainEncoding::Terrarium;
    web.maximumZoom = m_zoomBox->value();
    web.tileSizePx = m_tileSizeBox->currentData().toInt();
    web.verticalDatum = datum;
    web.name = m_nameEdit->text().trimmed();
    web.attribution = m_attributionEdit->text().trimmed();
    // Checked as the registry will read it.
    if (WebTerrainSource::fromJson(web.toJson(), &error)) {
      definition.source = web.toJson();
      definition.name =
          web.name.isEmpty() ? QUrl(web.urlTemplate).host() : web.name;
    }
  }
  if (definition.source.isEmpty()) {
    m_errorLabel->setText(error);
    m_errorLabel->show();
    return std::nullopt;
  }
  m_errorLabel->hide();
  return definition;
}

void AddWebTerrainDialog::accept()
{
  m_definition = buildDefinition();
  if (m_definition) {
    QDialog::accept();
  }
}

void AddWebTerrainDialog::changeEvent(QEvent* event)
{
  if (event->type() == QEvent::LanguageChange) {
    retranslateUi();
  }
  QDialog::changeEvent(event);
}

void AddWebTerrainDialog::retranslateUi()
{
  setWindowTitle(tr("Add Web Terrain"));
  m_presetBox->setItemText(0, tr("Custom"));
  m_kindBox->setItemText(static_cast<int>(Kind::Terrarium),
                         tr("Terrain tiles, Terrarium encoding"));
  m_kindBox->setItemText(static_cast<int>(Kind::TerrainRgb),
                         tr("Terrain tiles, Terrain-RGB encoding"));
  m_kindBox->setItemText(static_cast<int>(Kind::CloudGeoTiff),
                         tr("Cloud Optimized GeoTIFF"));
  m_datumBox->setItemText(0, tr("Sea level, EGM2008"));
  m_datumBox->setItemText(1, tr("Sea level, EGM96"));
  m_datumBox->setItemText(2, tr("WGS 84 ellipsoid"));
  const QStringList labels = {tr("&Source:"),    tr("&Kind:"),
                              tr("&Address:"),   tr("Deepest &zoom:"),
                              tr("&Tile size:"), tr("&Heights above:"),
                              tr("&Name:"),      tr("Attri&bution:")};
  for (std::size_t i = 0; i < m_labels.size(); ++i) {
    m_labels[i]->setText(labels.value(static_cast<int>(i)));
  }
  m_nameEdit->setPlaceholderText(tr("The server's name"));
  m_attributionEdit->setPlaceholderText(
      tr("What the provider asks to be credited with"));
  m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Add"));
  updateControls();
}

} // namespace ts
