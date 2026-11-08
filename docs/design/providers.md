# Providers

A provider is a source of tiles: imagery, elevation or vector features.
The tiling engine asks providers for tiles by `ts::TileKey` and never
knows where the data comes from (a generated pattern, a file, a WMS
server). Code: `core/include/ts/providers/`.

## Interfaces

```
TileProvider            id(), name(), maximumLevel()
├── ImageryProvider     tileSizePx(), loadImage(key, cancelled) → QImage
├── ElevationProvider   loadHeights(key, cancelled) → optional<HeightTile>
└── FeatureProvider     loadFeatures(key, cancelled) → optional<FeatureTile>
```

| Function | Contract |
| --- | --- |
| `id()` | Identifies the source: file path, URL plus layer, or a test pattern with its parameters. Two providers with the same id deliver the same tiles, so the layer tree (Phase 2b) shares one provider and one cache between entries pointing at the same source. |
| `name()` | For the user; translated where it is fixed text. |
| `maximumLevel()` | Deepest level with data. Selection stops there, and the deepest tiles are stretched further as the camera comes closer. |
| `loadImage()` | A square image of `tileSizePx()` pixels covering exactly the tile's rectangle, row 0 at the north edge, linear in latitude and longitude. A null image means no data there (or failure); the tile is then not drawn and the globe shows through. |
| `loadHeights()` | A square grid of heights above the ellipsoid, edges included, row 0 north. Used from Phase 6. |
| `loadFeatures()` | The features intersecting the tile at this level of detail, as points, line strings or polygons in latitude/longitude with attributes. Used from Phase 3 (World Vector Shoreline). |

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
  needs no font engine on the workers. View → Imagery → Checkerboard in
  the application.

`tests/providers` checks both (sizes, determinism, the outline and label,
levels, cancellation, a shared instance on many threads); `tests/tiling`
loads thousands of checkerboard tiles while a simulated camera flies
around, under ThreadSanitizer in CI.

## Adding a provider

1. Subclass `ImageryProvider` (or another interface) in `core/`; keep
   third-party types out of the header.
2. Make `id()` unique and stable for the same source.
3. Make loading thread-safe and honour `cancelled`.
4. Add a test project with a concurrent test on one shared instance.
