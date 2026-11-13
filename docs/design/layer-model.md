# Layer model

What the user sees on the globe is described by a tree of layers and
collections. This page defines the tree's semantics, how it turns into
what is drawn, the JSON of layer definitions and project files, and the
rules about versions. Code: `core/include/ts/layers/`,
`core/include/ts/globe/layerstackdraws.h`, `app/src/layertreedock.h`.

## The tree

`ts::LayerTreeModel` is a `QAbstractItemModel` with one column. Each
entry is either

- a **layer**: a name, visibility, opacity, and a layer definition (what
  it draws, see below), or
- a **collection**: a name, visibility, opacity, "Composite as group",
  and any number of child entries (layers and collections, nested freely).

Every entry has exactly one parent and a unique id (a UUID, kept in
project files). **Row 0 is drawn on top**, as in the layer lists of GIS
applications: an entry covers the ones below it.

| Rule | Definition |
| --- | --- |
| Effective visibility | An entry is drawn only if it and all its ancestors are visible. |
| Effective opacity | The product of the entry's and all its ancestors' opacities (`EffectiveOpacityRole`). |
| Collection opacity, default ("multiply") | A collection's opacity multiplies into its children. Costs nothing. |
| "Composite as group" | The collection's layers are drawn together into an offscreen image, which is then faded as one with the collection's opacity (and its ancestors'). Layers inside see only their own and their in-group ancestors' opacities. Costs a full-screen buffer and an extra pass. Only the outermost composite group composites; groups nested inside it multiply. |
| Software rendering | Software rasterisers don't composite: a group's opacity multiplies into its layers, and the option's tooltip says so. |
| Mixed types | Collections may hold imagery and vector layers together. All imagery is drawn first, in tree order; vector layers are drawn after it, in tree order, so a vector layer is never hidden by imagery whatever its position in the tree. |
| Sharing | Entries whose sources are equal share one provider (`ts::ProviderRegistry`, keyed by the source's canonical JSON), and through it one loader, decoded-image cache and set of GPU textures. A second entry for the same source costs almost nothing. |
| Blend modes | Alpha ("over", premultiplied) only. The compositing code keeps each layer's colour and alpha separate until the blend, so multiply or screen can be added per layer later. |

The view edits the model directly: check boxes set visibility, drag and
drop moves entries (also between collections; a collection can't be
moved into itself), double click renames, and the dock's slider and
check box set opacity and "Composite as group". Every change emits
`layerStackChanged()`; the main window rebuilds what is drawn and the
globe view shows it in the next frame.

## From tree to pixels

`LayerTreeModel::layerStack()` flattens the visible tree, bottom to top:

```
LayerStack
├── imagery  [ {definition, alpha, group}, … ]   bottom → top
├── groups   [ {alpha}, … ]                       composite groups
└── vectors  [ {definition, alpha}, … ]           bottom → top, above all imagery
```

The members of one composite group are contiguous in `imagery`.
`ts::imageryDrawsForStack()` gets each entry's provider from the
registry, and `ts::TileRenderer` draws them on the tile quadtree
([tiling.md](tiling.md)):

- Each selected tile is drawn with up to **8 layers blended in the tile
  fragment shader**, bottom to top: `result = layer + result × (1 − α)`
  in premultiplied colour, each layer at its alpha. More layers draw the
  tile again over itself (depth test "less or equal"), 8 at a time.
- A layer whose image for a tile hasn't loaded uses the **nearest loaded
  ancestor's** image, through a texture transform, so a new or reordered
  layer shows at once, blurred, and sharpens as its tiles arrive. A tile
  the provider has no image for stays transparent.
- A **composite group** ends the main pass, draws its layers into an
  offscreen target (cleared to transparent, with its own depth), resumes
  the main pass and draws that image over the scene at the group's
  opacity.

Tests: `tests/layers` (model, stack, JSON, projects, sharing) and the
golden images `two-layers-50`, `group-composite` and `group-multiply`
in `tests/globe`.

## Layer definitions

A layer definition says what a layer shows. The same JSON is used in
project files, the built-in list, and (Phase 5) server catalogues:

```json
{
  "id": "builtin:checkerboard",
  "origin": "builtin",
  "type": "imagery",
  "name": "Checkerboard",
  "source": { "kind": "checkerboard", "tileSize": 256 }
}
```

| Member | Values |
| --- | --- |
| `id` | Unique within its origin: `builtin:…`, `catalogue:<server>:…`, `user:…` |
| `origin` | `builtin` (shipped), `catalogue` (from a server's or pack's catalogue), `user` (added by the user) |
| `type` | `imagery`, `vector` (Phase 3), `elevation` (Phase 7: the terrain, see [terrain.md](terrain.md)) |
| `name` | Default name; built-in names are translated |
| `source` | Provider parameters; `kind` selects the provider. Now: `checkerboard` (`tileSize`, `maximumLevel`), `solid` (`color` as `#aarrggbb`, `tileSize`, `maximumLevel`), `basepack` (`dataset`: a dataset of the base pack, imagery, features or elevation), `file` (elevation: `path` of a DEM GDAL reads, `/vsicurl/<address>` for one on a web server, `verticalDatum` `egm2008`, `egm96` or `ellipsoid`, optional `attribution`; see [terrain.md](terrain.md)), `web` (elevation: web terrain tiles, `url` with `{z}/{x}/{y}`, `encoding` `terrarium` or `terrain-rgb`, `maximumZoom`, `tileSize`, `verticalDatum`, `attribution`), `graticule` (generated lines) |
| `style` | Optional. Vector layers: `fill` and `outline` as `#rrggbbaa` (alpha last) and `outlineWidthPx` (see [vector-rendering.md](vector-rendering.md)). Elevation layers: `exaggeration` (1; 0.1–20) |

A layer in the tree keeps a copy of its definition, so a project file is
self-contained.

## Project files

A project (`*.terrasoar`) is an indented JSON document:

```json
{
  "format": "terrasoar-project",
  "version": 2,
  "camera": { "targetLatDeg": 48, "targetLonDeg": 10, "rangeM": 2500000,
              "headingDeg": 10, "tiltDeg": 35 },
  "view": { "mode": "globe", "projection": "mercator" },
  "layers": [
    { "kind": "collection", "id": "…", "name": "Overlays",
      "visible": true, "opacity": 0.75, "compositeAsGroup": true,
      "children": [ { "kind": "layer", "id": "…", "name": "…",
                      "visible": true, "opacity": 1,
                      "definition": { … } } ] },
    { "kind": "layer", … }
  ]
}
```

- `layers` lists entries top to bottom, like the tree.
- A WMS layer's `definition.source` is `{"kind": "wms", "url": …,
  "version": …, "layers": […], "styles": […], "format": …,
  "transparent": …, "crs": …, "tileSize": …, "maximumLevel": …,
  "bounds": [w, s, e, n], "title": …, "attribution": …}`
  ([wms.md](wms.md)); its `origin` is `"catalogue"`. Everything needed to
  draw it is there: opening a project asks the server for nothing but
  tiles.
- `view.mode` is `"globe"` or `"map"`; `view.projection` is the flat
  map's projection by its identifier ([projections.md](projections.md)),
  kept also while the globe is shown. A projection this build doesn't
  know leaves the current one.
- Writing is deterministic (sorted keys): save → load → save gives the
  same bytes. Saving goes through `QSaveFile`, so a failed save never
  leaves a half-written file.
- A file that isn't a project, or is damaged, is refused with a message;
  the open project stays as it was.

### Versions

- `version` is the format version, `ts::projectFormatVersion` (2).
  Version 2 added `view`; a version 1 file opens as the globe with
  Mercator ready for the map.
- A file of a **newer** version is refused ("saved by a newer version of
  the program"), never half-read.
- Files of **every earlier** version must keep loading. A change to the
  format increments the version, adds a step to `migrate()` in
  `projectfile.cpp`, and adds a fixture of the old version to
  `tests/data/projects/`. `tests/layers` loads every fixture there:
  `v1-basic.terrasoar`, `v2-map.terrasoar`.
- Optional members may be added without a new version as long as older
  files load with the right defaults.
