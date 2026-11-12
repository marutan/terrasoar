# Providers

A provider is a source of tiles: imagery, elevation or vector features.
The tiling engine asks providers for tiles by `ts::TileKey` and never
knows where the data comes from (a generated pattern, a file, a WMS
server). Code: `core/include/ts/providers/`.

## Interfaces

```
TileProvider            id(), name(), maximumLevel(), preferredLoaderThreads(),
│                       status(), setStatusCallback()
├── ImageryProvider     tileSizePx(), imageProjection(),
│                       loadImage(key, cancelled) → QImage,
│                       loadTileImage(key, cancelled) → ImageLoadResult
├── ElevationProvider   loadHeights(key, cancelled) → optional<HeightTile>
└── FeatureProvider     loadFeatures(key, cancelled) → optional<FeatureTile>
```

| Function | Contract |
| --- | --- |
| `id()` | Identifies the source: file path, URL plus layer, or a test pattern with its parameters. Two providers with the same id deliver the same tiles, so the layer tree (Phase 2b) shares one provider and one cache between entries pointing at the same source. |
| `name()` | For the user; translated where it is fixed text. |
| `maximumLevel()` | Deepest level with data. Selection stops there, and the deepest tiles are stretched further as the camera comes closer. |
| `loadImage()` | A square image of `tileSizePx()` pixels covering exactly the tile's rectangle, row 0 at the north edge, linear in latitude and longitude. A null image means no data there (or failure); the tile is then not drawn and the globe shows through. |
| `loadTileImage()` | The same, telling apart an image, no data (for good), a failure worth retrying (a network error) and a cancelled load. The loader uses this; the default calls `loadImage()`, a null image meaning no data. A failed tile is asked for again later instead of staying empty ([tiling.md](tiling.md)). |
| `imageProjection()` | Geographic (above), or Web Mercator: rows evenly spaced in Mercator northing over the tile's rectangle clipped to ±85.051°. The tile shader reprojects such images per pixel ([wms.md](wms.md)). |
| `preferredLoaderThreads()` | 0 for the loader's default (two). Network sources ask for more (six), since their threads wait on servers. |
| `status()` | How the source is doing, for the user: Ok, Offline (its server can't be reached; saved tiles shown) or Error with a message. A change calls the status callback on the thread that made it, usually a loader thread, so the callback must only hand over to the GUI thread. The Layers dock shows it as an icon and tooltip. |
| `loadHeights()` | A square grid of heights above the ellipsoid, edges included, row 0 north. Used from Phase 6. |
| `loadFeatures()` | The features intersecting the tile at this level of detail, as points, line strings or polygons in latitude/longitude with attributes. A source may answer for a coarser ancestor (`FeatureTile::key` says which). Used from Phase 3 (the base pack's shoreline). |
| `storedKey()` | The tile whose features serve a key (the key itself, or that coarser ancestor), so the vector renderer loads and meshes each stored tile once for all the drawn tiles it serves. |

## Threading rules

Providers are **thread-safe**: one instance is shared by the render
thread and all the loader's workers.

- The metadata (`id`, `name`, `maximumLevel`, `tileSizePx`) is fixed at
  construction and may be read from any thread.
- The load functions are called on worker threads, **several at once**,
  and may block (disk, network). They must not touch widgets or OpenGL.
- Per-thread resources (a GDAL dataset handle, a PROJ context, an SQLite
  connection) are created per worker, following the rules in
  [architecture.md](../architecture.md#third-party-libraries); a provider
  keeps them in thread-local storage or a small pool.
- A load should check its `cancelled` flag in long loops and give up when
  it becomes true: the camera has moved and nobody wants the tile any
  more. The loader then discards the result.
- Drawing into a `QImage` with `QPainter` is allowed on worker threads;
  text rendering through the system fonts is best avoided (it serialises
  on the font engine and differs between platforms).

## Test providers

`core/include/ts/providers/testproviders.h` has two generated sources,
used by the tests and for development until real data arrives:

- `SolidColorImageryProvider`: every tile one colour, alpha included
  (blending tests).
- `CheckerboardImageryProvider`: 8 × 8 squares per tile with a one-pixel
  outline and the key written in the middle. The colours change with the
  level (four palettes), so level boundaries are visible. The label uses
  a built-in 3 × 5 bitmap font rather than the system fonts, so tiles
  are identical on every platform (golden images) and generating them
  needs no font engine on the workers. In the application: Layers dock →
  Add Layer → Checkerboard.

`tests/providers` checks both (sizes, determinism, the outline and label,
levels, cancellation, a shared instance on many threads); `tests/tiling`
loads thousands of checkerboard tiles while a simulated camera flies
around, under ThreadSanitizer in CI.

## Network providers

`ts::WmsImageryProvider` (`core/include/ts/wms/`) loads one GetMap per
tile through the shared `ts::NetworkClient`, keeps responses in a
`ts::TileDiskCache`, and reports Offline or Error. Created by the registry
for sources `{"kind": "wms", …}`; see [wms.md](wms.md) and
[network-and-cache.md](network-and-cache.md).

## File providers

Two providers read the base pack's files
([docs/data/base-pack.md](../data/base-pack.md) has the formats). Both
open SQLite directly (a GeoPackage is a SQLite file) through a small pool
of read-only connections, each with its prepared statement
(`core/src/providers/sqlitestatementpool.h`): a worker borrows one for a
load and gives it back, so a provider has at most one connection per
worker and no connection is ever used by two threads at once.

- `SqliteTileImageryProvider` reads a tile store: encoded tiles keyed by
  `TileKey`. A missing tile, or one that does not decode, is no data (a
  null image). `maximumLevel()` is the store's deepest level; the tiling
  engine stretches it beyond.
- `GeoPackageFeatureProvider` reads a tiled GeoPackage: tables of
  features cut to the tiles of one storage level, each serving a range of
  display levels (`FeatureTableSpec`). `loadFeatures(key)` returns the
  features of the key's ancestor at the storage level of the tables
  serving `key.level`, with that ancestor as `FeatureTile::key`; each
  feature carries its table name and integer attributes. Geometry is
  decoded from the GeoPackage blob (header and WKB, 2D/Z/M/ZM, polygons
  and line strings with their multi forms) with bounds checks: a damaged
  blob is skipped, never read past its end.

Their writers, `SqliteTileStoreWriter` and `GeoPackageFeatureWriter`
(through GDAL's GPKG driver, with an R-tree on every table), live next to
them in core: the packager and the tests write files the same way.

Layer sources `{"kind": "basepack", "dataset": "<id>"}` resolve through
`ProviderRegistry::setBasePack()`, which the main window calls with the
pack it found; without a pack such sources fail with a message saying so.

## Adding a provider

1. Subclass `ImageryProvider` (or another interface) in `core/`; keep
   third-party types out of the header.
2. Make `id()` unique and stable for the same source.
3. Make loading thread-safe and honour `cancelled`.
4. Add a test project with a concurrent test on one shared instance.
