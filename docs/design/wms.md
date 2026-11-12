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
power of two that fits). `ts::getMapUrl()` builds it:

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
tile:

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

## The catalogue

`data/wms-catalogue.json` lists open services for the dialog:

```json
{"format": "terrasoar-wms-catalogue", "version": 1,
 "services": [{"title": "…", "url": "…", "description": "…",
               "attribution": "…", "terms": "…"}]}
```

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
