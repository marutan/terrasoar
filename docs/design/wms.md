# WMS

How a layer from a Web Map Service becomes tiles on the globe and the
maps: reading capabilities, building GetMap requests (with the axis-order
rules), the Web Mercator fallback, and how the provider handles caching,
errors and being offline. Code: `core/include/ts/wms/`. The HTTP client and
the disk cache are in [network-and-cache.md](network-and-cache.md); the
dialog is described for users in
[adding-wms-layers.md](../user/adding-wms-layers.md).

## From an address to tiles

```
Add WMS dialog (GUI thread)                     loader threads (6)
  GetCapabilities 1.3.0 (1.1.1 if refused)        WmsImageryProvider::loadTileImage
  parseWmsCapabilities()                            disk cache: fresh? → image
  user picks layer, style, format                   GetMap for the tile (getMapUrl)
  wmsSourceFor() → WmsSource → layer definition     304 → refresh; 200 → store, image
                                                    failure → saved tile or "Failed"
```

A WMS layer is an ordinary imagery layer whose definition's source is
`{"kind": "wms", …}` (`ts::WmsSource::toJson()`). Everything needed to
draw it is in that object, so opening a project makes no capabilities
request. The registry creates a `WmsImageryProvider` for it, shared by
every layer with the same source (`WmsSource::id()`), like any other
provider ([providers.md](providers.md)).

## Capabilities

`ts::parseWmsCapabilities()` reads WMS 1.1.0, 1.1.1 and 1.3.0 documents
with `QXmlStreamReader`, matching elements by local name so the 1.3.0
namespace and the 1.1.1 DTD don't matter. It keeps what Terra Soar uses:

- Service: title, abstract, fees, access constraints, MaxWidth, MaxHeight,
  LayerLimit.
- GetMap: the image formats and the HTTP GET address.
- The layer tree: name (empty for a group that can't be requested),
  title, abstract, CRS, geographic bounds, bounding boxes, styles with
  legend URLs, the opaque/queryable/noSubsets/fixedWidth/fixedHeight
  attributes, scale limits and attribution.

**Inheritance** is applied while reading (WMS 1.3.0 §7.2.4.8, the same in
1.1.1): CRS and styles are added to the parent's; geographic and other
bounding boxes, scale limits, attribution and the attributes replace the
parent's when a layer gives its own. So each `WmsLayer` is complete.

**What servers get wrong**, and the parser accepts (each has a fixture in
`tests/data/wms`): lower-case codes (`epsg:4326`, NASA GIBS); empty
`<SRS></SRS>` (EOX); several codes in one SRS element (WMS 1.1.0 style);
EPSG:900913 and EPSG:102100 for Web Mercator; a GetMap address ending in
`?`; a DTD with an internal subset; a byte-order mark; an EPSG:4326
BoundingBox, latitude first, as the only extent. A geographic extent is
taken from `EX_GeographicBoundingBox` or `LatLonBoundingBox`, else from a
`CRS:84` or `EPSG:4326` BoundingBox with the axis order of the document's
version.

**What isn't capabilities** gives the user a sentence, not an XML error:
a `ServiceExceptionReport` passes the server's message on; an HTML page
says to check the address; a document without a GetMap address or named
layers says so.

## Requests

One GetMap per quadtree tile ([tiling.md](tiling.md)), 256 × 256 pixels
unless the server's MaxWidth or MaxHeight is smaller (then the largest
power of two that fits), or one per **block** of four (below).
`ts::getMapUrl()` builds it:

```
<GetMap address>?<its own query items>&SERVICE=WMS&VERSION=1.3.0&REQUEST=GetMap
  &LAYERS=a,b&STYLES=,s&CRS=EPSG:4326&BBOX=…&WIDTH=256&HEIGHT=256
  &FORMAT=image/png&TRANSPARENT=TRUE&EXCEPTIONS=XML
```

- Query items of the server's own address are kept (MapServer's `map=`);
  WMS parameters in it are dropped and set afresh.
- `STYLES` is always sent, with an empty entry for a default style.
- 1.1.1 uses `SRS` and `EXCEPTIONS=application/vnd.ogc.se_xml`.
- Tiles outside the layer's geographic bounds aren't requested (no data).

### Axis order

The rule that most WMS clients get wrong at least once
(`ts::bboxParameter()`):

| Version | CRS | BBOX |
| --- | --- | --- |
| 1.3.0 | EPSG:4326 | minLat,minLon,maxLat,maxLon (the EPSG definition's order) |
| 1.3.0 | CRS:84 | minLon,minLat,maxLon,maxLat |
| 1.1.1 | EPSG:4326 | minLon,minLat,maxLon,maxLat (1.1.1 always puts x first) |
| either | Web Mercator | minX,minY,maxX,maxY in metres |

Numbers are written in the shortest form that reads back exactly. With
1.3.0, CRS:84 is preferred over EPSG:4326 when the server offers both: the
same picture without the axis question.

### Choosing the CRS and format

`ts::chooseCrs()` takes, in this order, CRS:84 (1.3.0 only), EPSG:4326,
then Web Mercator under any of its codes (EPSG:3857, EPSG:900913,
EPSG:102100, EPSG:3785, EPSG:102113), and returns the code in its usual
spelling. A layer in none of these can't be added; the dialog says why.
With several layers in one source, only CRS all of them offer count.

`ts::chooseFormat()` takes PNG for overlays (it can be transparent; then
`TRANSPARENT=TRUE`) and JPEG for layers the server marks opaque (smaller),
falling back to the other and to GIF. The user can pick another image
format in the dialog.

Layers marked `noSubsets` or with a fixed size can't be cut into tiles and
are refused, as is a source with more layers than LayerLimit.

## The Web Mercator fallback

A server without EPSG:4326 or CRS:84 is asked for Web Mercator images:
the tile's rectangle clipped to the Mercator limit (±85.051 129°), its
corners projected with Phase 4's `WebMercatorProjection`. The image's
rows are then evenly spaced in northing, not latitude. Instead of
resampling on the CPU, the tile fragment shader reprojects per pixel
(`imageCoord()` in `tilerenderer.cpp`):

1. The fragment's latitude below the image's north edge comes from the
   tile's texture coordinate (exact within a tile, like everything else
   there).
2. The projection's own GLSL turns it into a northing difference,
   `tsCylindricalDy()` from [projections.md](projections.md), which stays
   precise in small tiles because nothing is subtracted.
3. Divided by the image's northing span (computed in double on the CPU),
   that is the image row.

Outside the latitudes the image covers, the layer is transparent: a Web
Mercator layer shows nothing within 5° of the poles. The same path serves
the globe, the maps and the morph, and stand-in images from coarser
levels, because only the texture coordinate changes.

`tests/globe` draws the same test pattern from an EPSG:4326 server and a
Web Mercator one and finds them the same except along the edges of the
pattern (at most 0.8 % of the pixels, near the limit).

## The provider

`ts::WmsImageryProvider::loadTileImage()` runs on a loader thread for each
tile. Steps 2–4 are `ts::CachedTileFetcher`, which web terrain tiles use
too ([terrain.md](terrain.md)); the provider adds the GetMap URL, decoding
the image, and the WMS-specific error messages:

1. No URL for the tile (outside the bounds, beyond the Mercator limit):
   **no data**.
2. In the disk cache and **fresh**: decoded and returned; no request.
3. The server is being **left alone** (below): a saved copy, stale or not,
   else **failed**.
4. Otherwise a GetMap, through the shared `NetworkClient`; with the saved
   copy's ETag or Last-Modified when there is one (revalidation).
   - **304 Not Modified**: the saved copy, with its freshness renewed.
   - **200 with an image**: stored (unless `no-store`) and returned.
   - **200 with something else**: WMS servers report errors this way. The
     ServiceException's message (or the content type) becomes the
     layer's error; a saved copy is shown if there is one.
   - **An HTTP error** after retries: the same.
   - **No answer** (connection failed, timeout): the layer is offline; a
     saved copy if there is one, else failed.
   - **Cancelled** (the tile left the view): given up at once.

**Blocks.** In a geographic CRS (EPSG:4326, CRS:84), a tile not fresh in
the cache comes with its three siblings: one GetMap for their parent's
area at twice the size (512 × 512), cut into the four, each stored in the
cache as a tile of its own (PNG, or JPEG at quality 92 for JPEG sources)
with the block's freshness but not its validators. A quarter of the
requests, for little more data where the view ends; WMS servers draw each
request, so requests are what cost them. The pixels are those of the
single tiles: each pixel of the block covers the same ground as one of a
tile (`providerFetchesBlocks` compares them exactly). A tile wanted while
another loader thread fetches its block waits for that block instead of
asking again. If the server refuses the larger image (an error, or not
an image of that size, as past its MaxWidth), the source goes back to
single tiles, starting with that one. Not in Web Mercator, whose rows
aren't evenly spaced in latitude, so a block's middle row isn't the edge
between its tiles; not for region downloads, which keep single tiles.
Stale pieces are fetched again rather than revalidated. Off with
`WebTileSettings::blocks` (tests of single tiles do that).

A **failed** tile isn't delivered to the renderer, which keeps showing a
coarser image where it has one; the loader asks again after 30 s if the
tile is still wanted ([tiling.md](tiling.md)). After three unreachable
requests in a row the provider leaves the server alone for 30 s and serves
only saved tiles, so a whole view of tiles doesn't each wait for its
timeout.

Network sources get six loader threads instead of two: they spend their
time waiting, and six matches the network client's limit.

### Status

The provider reports its status ([providers.md](providers.md)):

| State | When | Shown |
| --- | --- | --- |
| Ok | The last request succeeded (or none has been made) | Nothing |
| Offline | The server couldn't be reached | A warning icon on the layer; the tooltip names the server and says saved tiles are shown |
| Error | The server answered with an error, something that isn't an image, or far too much | An error icon with the server's message; the status bar shows it once |

A layer whose source can't even be created (an unusable CRS in a
hand-edited project) is marked Error at once.

## Time

Layers whose data changes with time (satellite days, forecasts, radar,
monthly climatologies) declare a TIME dimension; Phase 12 makes Terra
Soar's layers follow a **program clock**. Code: `core/include/ts/time/`.

**Capabilities.** `WmsLayer::dimensions` holds each `Dimension`: name
(lower case, as WMS compares names), units, default, the values as
written (`extent`), and `multipleValues`, `nearestValue` and `current`.
In 1.1.1 the values are in a separate `Extent` of the same name, which may
sit on a child of the layer declaring the `Dimension`; both are inherited,
a child's of the same name replacing its parent's.

**Values.** `ts::TimeExtent` reads an extent: a comma-separated list of
instants and `start/end/period` intervals (WMS 1.3.0 Annex C), ISO 8601
times of any precision (`2004`, `2004-07`, `2004-07-01`,
`…T12:00Z`, `…T12:00:00.000Z`, an offset or none, read as UTC), `current`
for now, periods with years, months, weeks, days, hours, minutes and
seconds. Intervals stay intervals (GIBS' daily imagery since 2000 is
one part, not 9 000 values): the latest time not after a given one is
found by stepping from an estimate. A time is written back the way the
extent writes its values, so a server listing dates gets a date.

**The clock.** `ts::ProgramClock`, one per main window, holds a UTC time,
the current time at start and saved in projects as `view.time` (optional,
so older files are unchanged). It doesn't run by itself; the time bar
moves it. Phases 14 (the sun) and 15 (satellites) use the same clock.

**A layer's time.** The source object keeps `"time"`
(`ts::LayerTime`): the dimension's name, its extent and default, and a
mode:

| Mode | The value requested |
| --- | --- |
| `clock` (default) | The latest of the layer's times not after the clock's; the earliest if the clock is before them all |
| `month` | The layer's time in the clock's month, whatever the year (Blue Marble's months, other monthly climatologies); offered for twelve monthly values of one year |
| `fixed` | `value`, whatever the clock |

`ts::sourceAtTime()` turns a source with `"time"` into the one drawn at a
given time: the value goes into the source's `dimensions`, which
`WmsSource` sends as `TIME` (and `ELEVATION`, or `DIM_<NAME>` for other
dimensions, WMS 1.3.0 C.3.2) and includes in `id()`. So each time is a
**source of its own**: its own provider, its own disk cache file, and
cached tiles of one day are never shown for another. Sources without
dimensions keep the ids they had, so existing caches stay valid. WMTS
sources work the same way with their `Time` dimension
([wmts.md](wmts.md)).

**Drawing.** `MainWindow::refreshLayers()` draws `stackAtTime()` of the
layer stack. When the clock moves, only the imagery is drawn again, and
only if some source's value changed (`clockMoved()`): layers that don't
change keep their providers and tiles, and a layer that does starts on
its new provider (from its disk cache if that time was seen before).

**The time bar** (`app/src/timebar.*`): a tool bar under the view, shown
when the first layer that follows the clock is added and from View →
Time Slider. Its slider spans the times of the visible layers that follow
the clock, joined (`TimeExtent::parse()` of their extents): one step per
time up to 2 000 of them, else 10 000 positions over the span, each
snapped to the latest time not after it. So every position is a time some
layer has. Back and forward go to the previous and next of those times;
Play steps forward at the chosen interval (¼ s to 5 s per time) and stops
at the last. A layer following the month of the year contributes the
twelve months of the clock's year.

**Choosing.** The Add WMS and Add WMTS dialogs offer, for a layer with
times, "Follow the program's clock" (chosen at first), "The clock's
month, any year" for monthly times, or "Always this time" with a list of
the latest values (`app/src/timemodebox.*`). The layer's context menu in
the Layers panel (**Time**) changes it later.

**Blue Marble's months.** NASA's Blue Marble exists for each month of
2004; there is no live web service for them (NASA's NEO WMS, which had
them, now redirects to a web page), so they come as a data pack,
`bluemarble-monthly`, whose twelve imagery datasets are *variants* of
`bluemarble` (`PackDataset::variantOf`, `variant` = `2004-01` …
[packs.md](packs.md#variants)). While it is installed, the built-in Blue
Marble layer gets the time settings `{"dimension": "month", "mode":
"month"}` over the installed months (`ProviderRegistry::basePackTime()`),
and the registry draws the variant its `month` value names. So the
seasons follow the clock; a fixed month can be chosen from the layer's
Time menu, and removing the pack brings back the July picture.

## GetFeatureInfo

**Capabilities.** `WmsCapabilities::getFeatureInfoUrl` and
`getFeatureInfoFormats` come from the `GetFeatureInfo` request's `DCPType`
and `Format`s. `wmsSourceFor()` gives a source a `featureInfoUrl` and an
`infoFormat` (kept as `"featureInfo": {"url", "format"}`, not part of
`id()`) when every chosen layer is `queryable`. `chooseInfoFormat()`
prefers GeoJSON (names and values exactly), then GML and the other XML
forms (ArcGIS's `vnd.esri.wms_featureinfo_xml`, `text/xml`), then HTML,
then plain text: nowCOAST answers in JSON, GEBCO in GML, the USGS in
GeoJSON, terrestris in XML.

**The request.** A click on the globe or the map (a press and release
within 4 px, reported after the double-click interval so a double click
isn't one; `GlobeView::pointClicked`) carries the point and the view's
degrees per pixel there. `MainWindow::identifyAt()` picks, for each
visible queryable WMS layer as drawn at the clock's time, the quadtree
tile at the view's scale containing the point (so the server judges
scale-dependent layers as for what is drawn), and `getFeatureInfoUrl()`
asks as if the point were clicked in that tile's GetMap image: the same
BBOX, size, CRS, styles and dimensions, `QUERY_LAYERS`, `INFO_FORMAT`,
`FEATURE_COUNT` (10), and the pixel as `I`/`J` (1.3.0) or `X`/`Y`
(1.1.1), rows even in latitude or, for Web Mercator, in northing.

**The answer.** `parseFeatureInfo()` reads, by the response's content type
(else the format asked for, else by sniffing):

| Answer | Read as |
| --- | --- |
| GeoJSON (GeoServer, QGIS Server, MapServer, ArcGIS) | Each feature's id and properties; numbers without needless decimals, nested values as compact JSON |
| GML `featureMember`/`featureMembers` (GeoServer) | Each member's simple elements; geometry (`*geom*`, `boundedBy`, `shape`) and nested elements left out; `fid` as the id |
| MapServer `msGMLOutput` | `<x_layer>` (its `gml:name`) and its `<x_feature>`s |
| ArcGIS `FeatureInfoResponse` | `<FIELDS name="value" …/>` attributes, or `FeatureInfoCollection`'s `Field`s |
| Other XML | Its text; a service exception is an error with its message |
| HTML | Each table with a header row: one feature per row; a two-column table: one feature of names and values; else the page's text. Scripts, styles and comments are dropped, entities decoded; nothing is loaded |
| Plain text | `name = value` lines grouped by GeoServer's and MapServer's headings and rules; geometry summaries left out; "no features" is nothing |

**The window** (`app/src/featureinfowindow.*`) asks every layer at once
through the shared network client and lists the answers as they come.
A new click cancels the requests of the last one. View → What Is Here on
Click turns clicking off.

## The catalogue

`data/wms-catalogue.json` lists open services for the dialog:

```json
{"format": "terrasoar-wms-catalogue", "version": 1,
 "services": [{"title": "…", "url": "…", "description": "…",
               "attribution": "…", "terms": "…",
               "allowBulkDownload": true}]}
```

`allowBulkDownload` (optional) says whether the service's terms allow
downloading regions for offline use; `false` makes the program refuse,
and without it the user is warned
([offline-regions.md](offline-regions.md#terms-of-use-allowbulkdownload)).

It is built into the program (`app/app.qrc`) and also fetched from the
repository's main branch (`ts::wmsCatalogueUrl`) when the dialog opens,
so services can be added or fixed without a release; the fetched copy is
kept in the cache directory and used next time. An entry is listed only
if the service states no fees and its terms of use allow general use;
each gives the credit the service asks for and a link to its terms.
`TS_WMS_CATALOGUE_URL` overrides the address (empty: don't fetch), for
tests and offline installations.

## Tests

`tests/wms` (no network: everything against a local mock server):

- Seven real capabilities documents (terrestris 1.3.0 and 1.1.1, GEBCO,
  USGS, EOX, nowCOAST, NASA GIBS; contact details removed, two trimmed):
  each parses and gives a requestable source in the expected CRS, and
  each server's quirk is checked. Synthetic documents for inheritance,
  axis order and the odd cases above; broken documents give the right
  message.
- TIME and other dimensions: 1.3.0 `Dimension` values (GIBS' monthly
  intervals with gaps, nowCOAST's 125 listed instants), 1.1.1 `Dimension`
  and `Extent` inherited and overridden (`synthetic-111-time.xml`), the
  GetMap parameter names, dimension values in JSON, requests and ids;
  `tests/time` covers time extents and the source drawn for each time.
- GetFeatureInfo: addresses and formats from real capabilities, the
  format chosen, sources of queryable layers only, the request for a
  point (BBOX, I/J or X/Y, Web Mercator rows, dimensions), and answers in
  every form above (`tests/data/wms/featureinfo`).
- The BBOX for every version and CRS, Web Mercator's edges, full GetMap
  and GetCapabilities URLs, CRS and format choice, sources from
  capabilities (several layers, layer limits, fixed sizes), JSON round
  trips, the catalogue.
- The provider: caching across restarts, revalidation with a fake clock,
  `no-store`, offline (stale copies, leaving the server alone, one status
  change), server errors of each kind and recovery, Web Mercator
  requests, cancellation of tiles leaving the view (connections dropped,
  nothing delivered), an idle loader while the server fails, the
  registry, many threads sharing a provider.

The mock WMS (`tests/support/mockwms.h`) draws its test pattern pixel by
pixel from each pixel's longitude and latitude, honouring the request's
version, CRS and axis order, so a correct client gets the same picture in
every CRS. `tests/globe` has golden images of it at 50 % on the globe, the
maps and half-way through the morph, from both kinds of server, and with
`TS_BASEPACK_DIR` over Blue Marble. `tests/ui` drives the dialog end to
end; `tests/smoke` runs the program on a project with a WMS layer, with
the server there and gone.
