# WMTS

How a layer from a Web Map Tile Service becomes tiles on the globe and the
maps: the capabilities model, tile matrix sets and how their tiles are
fitted to the quadtree, axis order, dimensions, requests and the cache.
Code: `core/include/ts/wmts/`. It shares the network client, the disk
cache, revalidation and the offline state with WMS ([wms.md](wms.md),
[network-and-cache.md](network-and-cache.md)). The dialog is described
for users in [adding-wmts-layers.md](../user/adding-wmts-layers.md).

Phase 10 delivers WMTS in three parts: **10a**, the engine (EPSG:4326 and
Web Mercator tile matrix sets); **10b**, the Add WMTS Layer dialog, the
catalogue and offline regions; and 10c, other CRSs through PROJ (such as
British National Grid).

## From an address to tiles

```
capabilities (KVP or WMTSCapabilities.xml)       loader threads (6)
  parseWmtsCapabilities()                          WmtsImageryProvider::loadTileImage
  pick layer, style, format, set, dimensions         wmtsMatrixFor(): which matrix
  wmtsSourceFor() → WmtsSource → layer definition    wmtsTileWindow(): which tiles
                                                     each: disk cache / GetTile
                                                     mosaic → the quadtree tile's image
```

A WMTS layer is an ordinary imagery layer whose definition's source is
`{"kind": "wmts", …}` (`ts::WmtsSource::toJson()`). It holds everything
needed to draw, the tile matrices included, so opening a project makes no
capabilities request. The registry creates a `WmtsImageryProvider` for it
with the WMS network client and cache settings, shared by every layer with
the same source (`WmtsSource::id()`).

## Capabilities

`ts::parseWmtsCapabilities()` reads WMTS 1.0.0 documents with
`QXmlStreamReader`, matching elements by local name, so namespace prefixes
don't matter. The same parser reads a KVP `GetCapabilities` answer and a
RESTful `WMTSCapabilities.xml`. It keeps:

- **Service:** title, abstract, fees, access constraints (several are
  joined), and the provider's name and site. `ServiceContact` names people
  and is not read.
- **GetTile's KVP address:** the `Get` whose `GetEncoding` constraint
  allows KVP, or one without a constraint. `Get`s marked RESTful or REST
  only don't count: basemap.at and swisstopo list several and are RESTful
  only.
- **Layers:** identifier, title, abstract, WGS 84 bounding box, styles
  (the default and legends), formats, dimensions (identifier, title, unit,
  default, values), tile matrix set links with their limits, and RESTful
  templates of `resourceType="tile"` (GIBS also lists "Domains" templates,
  which are ignored).
- **Tile matrix sets:** identifier, `SupportedCRS`, well-known scale set,
  and each matrix's identifier, scale denominator, top-left corner, tile
  size and matrix size, in document order.

What can't be used is dropped, not fatal: a layer without an identifier, a
matrix with a zero tile size, a limits entry whose minimum is above its
maximum.

What isn't capabilities gives the user a sentence: an OWS
`ExceptionReport` passes the server's message on; an HTML page says to
check the address; a WMS document says to use Add WMS Layer instead.

## CRSs and axis order

`ts::normalisedWmtsCrs()` reads the many spellings of a CRS:

| Written | Read as |
| --- | --- |
| `urn:ogc:def:crs:OGC:1.3:CRS84`, `…OGC::CRS84`, `http://www.opengis.net/def/crs/OGC/1.3/CRS84` | `CRS:84` |
| `urn:ogc:def:crs:EPSG::4326`, `http://www.opengis.net/def/crs/EPSG/0/4326`, `EPSG:4326` | `EPSG:4326` |
| `urn:ogc:def:crs:EPSG:6.18.3:3857` (basemap.at), `urn:ogc:def:crs:EPSG:6.18:3:3857` (GIBS), EPSG:900913, EPSG:102100, … | `EPSG:3857` |

10a draws `CRS:84`, `EPSG:4326` and `EPSG:3857`; others are kept for 10c.

A matrix's **top-left corner** is written in the CRS's own axis order, as
WMS 1.3.0 does: EPSG:4326 is latitude first (EMODnet's `inspire_quad`:
`90.0 -180.0`), CRS:84 and Web Mercator x first. `ts::wmtsMatrix()`
turns every corner into easting (or longitude) first. Servers often write
an EPSG:4326 corner longitude first anyway; a first value beyond ±90° can't
be a latitude, so such a corner is read the other way round.

## Tile matrices

A **pixel size** comes from the scale denominator and WMTS's standardized
0.28 mm pixel (WMTS 1.0.0 6.1.1): `scaleDenominator × 0.00028 /
metresPerUnit`, where a degree counts as 2π × 6 378 137 / 360 =
111 319.49 m (the WMTS definition, Annex E). So `WorldCRS84Quad`'s level 0
(scale 279 541 132.014) has 0.703125° pixels, and `GoogleMapsCompatible`'s
zoom 0 (559 082 264.029) 156 543.034 m pixels.

A source keeps only the matrices its layer has tiles in, coarsest first
(`ts::WmtsMatrix`):

- **Limits:** a link with `TileMatrixSetLimits` allows only the matrices
  it lists, within their rows and columns; a link without limits allows
  every tile.
- **At most 60 matrices:** the disk cache keys tiles by matrix index in
  `TileKey`'s six-bit level (below). With more, the coarsest are dropped.

Of a layer's usable sets, `ts::usableTileMatrixSets()` prefers geographic
ones (their images need no reprojection), then the one reaching finest.

## Fitting tiles to the quadtree

Each quadtree tile becomes one 256 × 256 image
([tiling.md](tiling.md)):

1. **Which matrix** (`ts::wmtsMatrixFor()`): the coarsest whose pixels are
   at least as fine as the quadtree tile's (its width over 256), or the
   finest if none is. The quadtree's level n has 180° / 2ⁿ / 256 pixels:
   exactly `WorldCRS84Quad`'s level n and `GoogleMapsCompatible`'s zoom
   n + 1.
2. **Which tiles** (`ts::wmtsTileWindow()`): the tile's extent in the
   matrix's pixels from its corner gives the block of WMTS tiles covering
   it, and where the tile lies in the block. A sliver of a millionth of a
   pixel past a tile edge doesn't bring in the next tile. Tiles of the
   block beyond the matrix's edge or the layer's limits are left
   transparent (`ts::wmtsTileExists()`). A block needing more than 16 tiles
   (a layer without matrices as coarse as the quadtree tile) is no data at
   that level: the tile shows when zoomed in.
3. **The image** (`WmtsImageryProvider::loadTileImage()`): the block's
   tiles side by side in one image, and one scaled copy of the tile's part
   of it to 256 × 256 (bilinear). Copying from one mosaic leaves no seams
   between WMTS tiles. When the matrix lines up with the quadtree, the copy
   is exact.

The **image layout** follows the set's CRS:

- **Geographic:** rows evenly spaced in latitude, as every other source.
- **Web Mercator:** the extent is the tile's rectangle clipped to the
  Mercator limit, in metres, so rows are evenly spaced in northing. That is
  exactly the layout WMS's Web Mercator fallback delivers
  (`TileImageProjection::WebMercator`), and the shader reprojects it per
  pixel on the globe, the maps and through the morph ([wms.md](wms.md)).
  Nothing is resampled across projections on the CPU, so fitting Web
  Mercator tiles is a scale and an offset, as for geographic ones.

`maximumLevel` is the quadtree level whose pixels match the finest matrix;
deeper tiles reuse it, as for every source.

`tests/wmts` checks the tile and pixel window chosen against hand-computed
values: `WorldCRS84Quad` 1:1; Web Mercator tile (2, 1, 1) from zoom 3's
column 1, rows 2–3 (pixels 736.717 to 1024); and a grid of 200-pixel tiles
from (185° W, 95° N), where tile (1, 1, 0) takes four.

## Requests

`ts::getTileUrl()` builds either kind:

- **RESTful**, when the layer has a template for the format: the template
  with `{TileMatrixSet}`, `{TileMatrix}`, `{TileRow}`, `{TileCol}`,
  `{Style}` and each dimension filled in, variables matched without regard
  to case and values percent-encoded. A variable it doesn't know is left as
  written. Of several templates for one format (GIBS has one with `{Time}`
  and others without), the one with a variable for every dimension is used,
  so their values are in every address.
- **KVP** otherwise: `SERVICE=WMTS&REQUEST=GetTile&VERSION=1.0.0&LAYER&STYLE
  &FORMAT&TILEMATRIXSET&TILEMATRIX&TILEROW&TILECOL`, then each dimension by
  its identifier. Query items of the server's own address are kept; WMTS
  parameters in it are dropped and set afresh. `+` is percent-encoded, so a
  time zone in a TIME value survives.

`ts::wmtsCapabilitiesUrl()` leaves an address ending in `.xml`, or already
asking for `GetCapabilities`, as it is, and otherwise makes a KVP
`GetCapabilities` request.

## Choices

`ts::wmtsSourceFor()` makes a source from capabilities, a layer and the
user's choices (`ts::WmtsChoices`, which the dialog fills in):

- **Style:** the given one, or the layer's default (`isDefault`, else the
  first). A layer without styles uses `default`.
- **Format:** as for WMS, PNG first (it can be transparent), then JPEG; if
  the layer offers none of the usual ones (ArcGIS's `image/jpgpng`), the
  first offered. The image's own bytes say what it is when it arrives.
- **Tile matrix set:** the given one, or the first of
  `usableTileMatrixSets()`.
- **Dimensions:** each the given value, else the layer's default, else its
  first value. TIME takes the service's default until the program has a
  clock (Phase 15).
- **Attribution:** the provider's name, else the service's title.

## The dialog

`AddWmtsDialog` (in `app/`) follows Add WMS Layer
([wms.md](wms.md)):

- **Service:** a catalogue entry, or any address. `wmtsCapabilitiesUrl()`
  leaves a `WMTSCapabilities.xml` (RESTful) as it is and asks a KVP
  address for `GetCapabilities`. A WMS document says to use Add WMS Layer
  instead.
- **Layers:** a flat list (WMTS has no groups), the identifier and abstract
  as the tooltip.
- **Choices:** style (the default selected), format (PNG first),
  tile matrix set (`usableTileMatrixSets()`, each with its CRS), and one
  editable box per dimension, the default first and the other values
  offered (up to 500); any value may be typed, as TIME takes dates no list
  holds. The boxes share one grid with the other choices, so they line up.
- **Before adding:** which tiles will be drawn (and whether reprojected),
  the credit, the service's fees and access constraints, and a link to the
  chosen style's legend.
- **Credit:** the catalogue entry's when the service is in the catalogue
  (the wording the provider asks for, such as "Datenquelle: basemap.at"),
  else the provider's name from the capabilities.
- **Add:** a layer definition with the source, its `service` set to the
  address the user connected to (for the catalogue's terms, see below).

F1 opens [the user guide](../user/adding-wmts-layers.md#adding-a-layer).
File → Add WMTS Layer… (Alt+M in the File menu) and Add Layer → WMTS
Layer… in the Layers panel open it.

## The catalogue

`data/wmts-catalogue.json` lists open WMTS services, read by
`ts::parseWmtsCatalogue()`: the same entries as the WMS catalogue
([wms.md](wms.md#the-catalogue)), with the format name
`terrasoar-wmts-catalogue`. Each entry's `url` is the service's
capabilities address.

It is a **file of its own**, fetched from the repository's main branch
(`wmtsCatalogueUrl`, or `TS_WMTS_CATALOGUE_URL` for tests) with the copy
built into the program as the fallback and the last fetched copy kept in
the cache directory, as for WMS. Entries can't go into the WMS catalogue:
programs released before WMTS fetch that file and would offer a WMTS
service in their Add WMS dialog. Each parser refuses the other's file.

The shipped catalogue, with each provider's terms as they stated them on
2026-11-18:

| Service | Credit | Bulk download |
| --- | --- | --- |
| NASA GIBS (EPSG:4326 and Web Mercator) | NASA EOSDIS GIBS | Allowed (as the WMS entry) |
| basemap.at | Datenquelle: basemap.at (CC BY 4.0) | Not stated: warn |
| swisstopo | © swisstopo | Forbidden: fair use, bulk data from the download service |
| PDOK BRT background map | Kadaster / PDOK (CC BY 4.0) | Not stated: warn |
| EMODnet Bathymetry | EMODnet Bathymetry Consortium (CC BY 4.0) | Not stated: warn |
| USGS The National Map | USGS The National Map | Allowed (public domain, as the WMS entry) |

## Offline regions

A WMTS layer can be kept as an offline region like a WMS one
([offline-regions.md](offline-regions.md)). Regions count and download
quadtree tiles; `WmtsImageryProvider` implements `ts::RegionTileSource`,
and its `fetchForRegion()` fetches and pins each WMTS tile the quadtree
tile is made from (the same matrix and window as drawing it), through
`CachedTileFetcher::fetchForRegion()`. So an offline region holds exactly
the tiles drawing it needs, and drawing it offline makes no request.

The terms are checked against the WMS and WMTS catalogues together, by the
source's `serviceUrl()`: the capabilities address the user connected to,
which is what the catalogue lists. A RESTful template's path is full of
`{variables}` and matches no entry, so without it a listed service could
only ever be found by host, which can forbid but not allow.

## Caching and dimensions

Each WMTS tile is fetched through `ts::CachedTileFetcher` and kept in the
source's disk cache as it came, with its HTTP freshness, exactly as WMS
tiles: a fresh tile is used without asking, a stale one revalidated, and
saved tiles are shown while the server can't be reached. Tiles are keyed by
(matrix index, column, row) in `TileKey`'s three fields, so quadtree tiles
sharing a WMTS tile fetch it once.

The **cache file** is chosen by the source's id, which includes the
template or address, layer, style, format, tile matrix set and every
dimension value. A layer at another time is another source: other requests,
another cache, never a stale picture from a different date.

## The provider

`ts::WmtsImageryProvider` behaves as `WmsImageryProvider` for the network
(six loader threads, the shared client's concurrency limit, retries with
back-off, leaving an unreachable server alone for 30 s, HTTP cache headers)
and for its status (Ok, Offline, Error with the server's message, read from
an OWS `ExceptionReport` if there is one). Unlike WMS, **404 and 204 mean
no tile there**: WMTS servers answer that way for empty areas.

A quadtree tile is:

- **an image** when at least one of its WMTS tiles arrived;
- **no data** when none of its tiles exist (beyond the layer's bounds or
  limits, beyond the Mercator limit) or all of them are missing (404);
- **failed** (asked for again later) when any of them failed, so a
  partial picture is never cached in memory as complete;
- **cancelled** at once when the tile leaves the view.

## Tests

Besides the parts above (10a), 10b adds: the shipped catalogue and each
catalogue format refusing the other's file; the service address for the
terms; `fetchForRegion()` (one WMTS tile pinned when lined up, four from
the unaligned grid, stored ones only pinned, no data, failures, no disk
cache); a whole region downloaded through `RegionDownload` and drawn with
the server gone, and a forbidden one refused before any request
(`tests/wmts`); the dialog against the mock server, KVP and RESTful, with
its choices, the catalogue's credit, a typed date and a WMS address told
apart (`tests/ui`); and F1 in the dialog (`tests/ui`, `tests/help`).

10a's:

- `tests/wmts`: seven real capabilities documents (basemap.at, EMODnet,
  NASA GIBS in both CRSs, PDOK, swisstopo, USGS ArcGIS; contact details
  removed, large ones trimmed) and synthetic odd ones (`tests/data/wmts`);
  CRS spellings; pixel sizes and corners; the tile windows above; limits
  and bounds; GetTile URLs and templates; sources from the real documents;
  JSON; and, against a mock server (`tests/support/mockwmts.h`, three tile
  matrix sets drawing the mock WMS's picture of the Earth), each set drawn
  right through KVP and RESTful, the cache per WMTS tile and by dimension,
  404, server errors, offline, cancellation and the registry.
- `tests/globe`: golden images of a WMTS overlay at 50 % on the globe, the
  Mercator and Miller maps and halfway through the morph; and a WMTS layer
  in each tile matrix set drawing the same picture as the WMS layer: the
  same pixels when lined up with the quadtree, at most 0.4 % differing
  along the pattern's edges otherwise.
