// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The Add Web Terrain dialog: web terrain tiles or a Cloud Optimized
 * GeoTIFF on a server, as an elevation layer.
 */
#ifndef TS_APP_ADDWEBTERRAINDIALOG_H
#define TS_APP_ADDWEBTERRAINDIALOG_H

#include <optional>
#include <vector>

#include <QDialog>

#include "ts/layers/layerdefinition.h"
#include "ts/providers/providerregistry.h"

class QComboBox;
class QDialogButtonBox;
class QEvent;
class QFormLayout;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace ts
{

/**
 * Asks for a web elevation source: a tile set of Terrarium or Terrain-RGB
 * PNGs (an address with {z}, {x} and {y}), or one Cloud Optimized
 * GeoTIFF read over HTTP by GDAL; what its heights are measured from; and
 * the attribution its provider asks for. Offers ready-made sources
 * (presets). layerDefinition() gives the layer once Add was pressed.
 */
class AddWebTerrainDialog : public QDialog
{
  Q_OBJECT
  Q_DISABLE_COPY_MOVE(AddWebTerrainDialog)

public:
  /** What kind of source. */
  enum class Kind {
    /** Terrarium-encoded tiles. */
    Terrarium,
    /** Terrain-RGB-encoded tiles. */
    TerrainRgb,
    /** One Cloud Optimized GeoTIFF. */
    CloudGeoTiff,
  };

  /**
   * Creates the dialog, offering @p datums (ProviderRegistry::
   * verticalDatums(); empty for the built-in EGM2008, EGM96 and
   * ellipsoid).
   */
  explicit AddWebTerrainDialog(QWidget* parent,
                               std::vector<VerticalDatum> datums = {});
  ~AddWebTerrainDialog() override;

  /** Returns the layer to add, once Add was pressed. */
  const std::optional<LayerDefinition>& layerDefinition() const
  {
    return m_definition;
  }

  /**
   * Fills the form (tests and presets): @p kind, @p address, the deepest
   * zoom, the vertical datum ("egm2008", "egm96", "ellipsoid"), the name
   * and the attribution.
   */
  // The form's fields, in its order.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  void setSource(Kind kind, const QString& address, int maximumZoom,
                 const QString& datum, const QString& name,
                 const QString& attribution);

  /**
   * Builds the layer from the form, or returns nothing and shows why in
   * the dialog (what Add does before closing).
   */
  std::optional<LayerDefinition> buildDefinition();

  /**
   * Returns what to call @p datum in a list of what heights are measured
   * from (translated; a pack's datum by the pack's name for it).
   */
  static QString datumLabel(const VerticalDatum& datum);

  /** Returns true if GDAL can read files over HTTP (/vsicurl/). */
  static bool canReadRemoteFiles();

protected:
  /** Re-applies all text on QEvent::LanguageChange. */
  void changeEvent(QEvent* event) override;

private:
  void retranslateUi();
  void applyPreset(int index);
  void updateControls();
  void accept() override;

  QComboBox* m_presetBox = nullptr;
  QComboBox* m_kindBox = nullptr;
  QLineEdit* m_addressEdit = nullptr;
  QSpinBox* m_zoomBox = nullptr;
  QComboBox* m_tileSizeBox = nullptr;
  QComboBox* m_datumBox = nullptr;
  QLineEdit* m_nameEdit = nullptr;
  QLineEdit* m_attributionEdit = nullptr;
  QLabel* m_noteLabel = nullptr;
  QLabel* m_errorLabel = nullptr;
  QFormLayout* m_form = nullptr;
  /** The form's labels, in the order of its rows. */
  std::vector<QLabel*> m_labels;
  QDialogButtonBox* m_buttons = nullptr;
  std::optional<LayerDefinition> m_definition;
  /** The datums offered, in the datum box's order. */
  std::vector<VerticalDatum> m_datums;
};

} // namespace ts

#endif // TS_APP_ADDWEBTERRAINDIALOG_H
