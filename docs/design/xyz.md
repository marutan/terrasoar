# XYZ and TMS tile services

How a web map served as `{z}/{x}/{y}` tiles (OpenStreetMap, OpenTopoMap,
ArcGIS tile caches, GIBS' REST addresses) becomes tiles on the globe and
the maps. Code: `core/include/ts/xyz/`. Almost everything is shared with
WMTS ([wmts.md](wmts.md)): an XYZ service is a Web Mercator
(GoogleMapsCompatible) tile matrix set whose tiles are named by a template.
The dialog is described for users in
[adding-xyz-layers.md](../user/adding-xyz-layers.md).

## The source

An XYZ layer's definition has the source `{"kind": "xyz", …}`
(`ts::XyzSource::toJson()`):

| Member | Meaning |
| --- | --- |
| `template` | The address, with `{z}`, `{x}`, `{y}` or `{-y}`, and `{s}` |
| `subdomains` | What `{s}` stands for (only with `{s}`) |
| `tms` | True if `{y}` counts rows from the south |
| `minZoom`, `maxZoom` | The service's zoom levels (0–30) |
| `tileSize` | 256, or 512 for some services (64–1024) |
| `format` | Image MIME type; from the template's extension by default |
| `bounds` | Optional `[west, south, east, north]`: tiles outside aren't asked for |
| `service` | The address matched against the catalogue's terms |
| `title`, `attribution` | Name and credit |

`ts::checkXyzTemplate()` accepts only http(s) addresses with `{z}`, `{x}`
and `{y}` (or `{-y}`), and no variable but those and `{s}`, which then
needs subdomains. Variable names are matched without regard to case. A
template with an API key variable (`{apikey}`, `{accessToken}`) is refused:
key handling is Phase 23's.

## Addresses

Zoom z has 2^z × 2^z tiles over Web Mercator's square (±20 037 508.34 m,
±85.051 129°); columns run east from the antimeridian, rows south from the
top. `ts::xyzTileUrl()` fills a template for (z, x, row):

- `{y}` is the row, or 2^z − 1 − row for a TMS source;
- `{-y}` is always 2^z − 1 − row (how some templates write TMS);
- `{s}` is subdomain (x + row) mod n, so a tile always comes from the same
  host and caches along the way see it once.

## Drawing: through WMTS

`XyzSource::toWmtsSource()` describes the same tiles as a WMTS source:
CRS EPSG:3857, one matrix per zoom from `minZoom` to `maxZoom`, each named
by its zoom, with zoom z's pixel 2π × 6 378 137 / (tileSize × 2^z) metres,
the corner at (−πa, πa), and the rows and columns limited to the bounds if
there are any. `ts::XyzImageryProvider` is a `WmtsImageryProvider` for that
source that overrides only `tileUrl()`: the matrix index plus `minZoom` is
the zoom. So everything else is WMTS's, unchanged:

- choosing the matrix for a quadtree tile and the block of tiles covering
  it, and cutting the quadtree tile's image out of their mosaic
  ([wmts.md](wmts.md#fitting-tiles-to-the-quadtree)); quadtree level n is
  zoom n + 1 for 256-pixel tiles (n for 512), exactly lined up in
  longitude;
- Web Mercator images (rows evenly spaced in northing), reprojected per
  pixel in the shader on the globe, the maps and through the morph, as
  WMS's Web Mercator fallback ([wms.md](wms.md#the-web-mercator-fallback));
- the disk cache per tile (zoom index, column, row), revalidation, the
  offline state, 404 as "no tile here", and status messages;
- offline regions (`RegionTileSource`), counted in the same tiles.

The provider's id (`XyzSource::id()`: template, row order, zooms, tile
size; not the subdomains, which name the same tiles) names its disk cache
file. Its host, for the offline state, is the template's with the first
subdomain.

The deepest quadtree level is the one whose pixels are as fine as
`maxZoom`'s at the equator: `maxZoom − 1` for 256-pixel tiles. Zoomed out
past `minZoom`, a quadtree tile would need more than 64 of the coarsest
tiles and draws nothing; it appears when zoomed in.

## The catalogue

`data/xyz-catalogue.json` (format `terrasoar-xyz-catalogue`, version 1) is
built into the program and fetched from the repository's main branch
(`ts::xyzCatalogueUrl`, or `TS_XYZ_CATALOGUE_URL`), like the WMS and WMTS
catalogues; it is a file of its own so older programs never see it. Each
entry has a title, the template, subdomains, `tms`, `minZoom`, `maxZoom`,
a description, the attribution, a terms link and optionally
`allowBulkDownload`. Entries without a title or with an unusable template
are skipped.

For offline regions' terms, each entry becomes a service entry whose
address is the template's scheme and host (`ts::xyzServiceUrl()`,
`XyzCatalogueEntry::serviceEntry()`): a layer is matched by host, so an
entry can forbid bulk downloads but, as for WMS, a match by host alone
can't allow them (`bulkDownloadPolicy()`). OpenStreetMap's and
OpenTopoMap's tile usage policies forbid bulk downloading; their entries
say so, and Terra Soar refuses to download regions of them.

The network client sends Terra Soar's own User-Agent with every request,
as OpenStreetMap's tile policy requires.

## Tests

`tests/xyz`: templates accepted and refused; formats from extensions;
addresses with `{y}`, `{-y}`, TMS and subdomains; service addresses; the
matrices made for a source (zooms, pixel sizes, deepest level for 256 and
512 pixels, bounds to rows and columns); JSON round trip and refused
sources; the shipped catalogue (every entry usable, OpenStreetMap
forbidden) and catalogue errors; against the mock XYZ server
(`tests/support/mockxyz.h`, the mock WMS's picture in Web Mercator tiles)
each way of addressing the same tiles drawing the same picture, the disk
cache, 404 and server errors, bounds and `minZoom`, an offline region
downloaded and drawn with the server gone, a forbidden one refused; the
registry. `tests/ui`: the Add XYZ Tile Layer dialog end to end.
