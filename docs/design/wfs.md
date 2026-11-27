# WFS

How a feature type of a Web Feature Service becomes vector features on
the globe and the maps: capabilities, the source and its levels, requests
and paging, reading GeoJSON and GML with their axis orders, the cache and
offline regions, per-feature styles and picking. Code:
`core/include/ts/wfs/` and `core/include/ts/vector/stylerules.h`. The
vector pipeline it feeds is in [vector-rendering.md](vector-rendering.md);
for users, [adding-wfs-layers.md](../user/adding-wfs-layers.md).

## From an address to features

```
Add WFS dialog (GUI thread)                VectorTileLoader workers
  GetCapabilities (WFS 2.0.0)                WfsFeatureProvider::loadFeatures
  parseWfsCapabilities()                       stored tile at storedLevel()
  pick a feature type, a colour                each page: disk cache / GetFeature
  wfsSourceFor() → WfsSource → layer           parseWfsFeatures() → Features
    definition (type vector,                    style rules → Feature::style
    source {"kind": "wfs", …})                 VectorMesher → runs by style
```

A WFS layer is a vector layer whose source is `{"kind": "wfs", …}`
(`ts::WfsSource::toJson()`); the registry makes a `WfsFeatureProvider`
for it with the WMS network client and cache settings, shared by every
layer with the same source. Opening a project asks for no capabilities.

## Capabilities

`ts::parseWfsCapabilities()` reads WFS 2.0.0 (and 1.1.0) by local names:
the service's title, abstract, fees and access constraints; the
provider's name (the default credit); GetFeature's GET address and
`outputFormat` values; the `ImplementsResultPaging` and `CountDefault`
constraints; and each feature type's name, title, abstract, default and
other CRSs, WGS 84 bounding box and own output formats.
`ServiceContact` names people and is not read (and was removed from the
fixtures). An HTML page, an exception report (its message), or a WMS or
WMTS document (pointing at the right dialog) is refused with a sentence.

## The source

| Member | Meaning |
| --- | --- |
| `url`, `service` | GetFeature's address; the address the user gave (for the catalogue's terms) |
| `version` | `2.0.0` (or `1.1.0`) |
| `typeName` | The feature type's qualified name |
| `outputFormat` | `chooseWfsOutputFormat()`: GeoJSON first, then GML 3.2, 3.1, 2 |
| `paging`, `pageSize`, `maximumPages` | STARTINDEX paging; features per request (the server's CountDefault, at most 1 000); pages per tile (5) |
| `bounds` | The type's WGS 84 box; tiles outside it ask nothing |
| `minimumLevel`, `maximumLevel` | The levels features are stored at (below) |
| `title`, `attribution`, `styleRules` | Name, credit (the catalogue's wording, else the provider's name), style rules |

**Levels.** Features are asked for by the bounding box of a *stored*
tile: `storedLevel(level)` is −1 below `minimumLevel` (nothing shown:
the layer appears when zoomed in that far), the level itself up to
`maximumLevel`, and `maximumLevel` beyond (deeper tiles share their
ancestor's features, which WFS sends at full resolution anyway).
`minimumLevel` is the first level whose tile covers the type's larger
side (`wfsMinimumLevel()`: the Netherlands at level 4, a world layer at
0), so zoomed out the whole type comes in a tile or two; `maximumLevel`
is four deeper. Vector tiles are selected as 1 024 pixels wide
([vector-rendering.md](vector-rendering.md)), so a stored tile is asked
for when it fills about that much of the view.

## Requests and paging

`getFeatureUrl()` asks for `TYPENAMES`, `SRSNAME` and the `BBOX` in
`urn:ogc:def:crs:EPSG::4326`, latitude first as WFS 2.0 defines it
(`BBOX=south,west,north,east,urn:…`), the `OUTPUTFORMAT`, `COUNT` and
`STARTINDEX` (`TYPENAME` and `MAXFEATURES` for 1.1.0). A page that comes
back full asks for the next, up to `maximumPages`; a tile whose pages run
out first says so in the layer's status ("zoom in to see them all"),
and deeper tiles, being smaller, get the rest.

The server decides what intersects the box; a feature spanning several
tiles comes in each, and the renderer clips each draw to its tile, so it
is drawn once ([vector-rendering.md](vector-rendering.md#drawing)).

## Reading features

`parseWfsFeatures()`:

- **GeoJSON** is read longitude first whatever its `crs` member says:
  GeoServer (EMODnet) names `urn:ogc:def:crs:EPSG::4326` and still writes
  longitude first, and PDOK names CRS84 (both checked against the live
  services when the fixtures were taken). Points, lines, polygons, their
  Multi forms and geometry collections; a MultiPolygon becomes one
  feature per polygon with the same id and attributes. `numberReturned`
  and `numberMatched` (or GeoServer's `totalFeatures`) are kept.
- **GML** 3.2, 3.1 and 2 (`member`, `featureMember`, `featureMembers`):
  each member's simple properties become attributes, its geometry
  property the geometry: Point, LineString, Curve (segments joined),
  Polygon and Surface (exterior and interior, or GML 2's outer and inner
  boundaries), LinearRing and the Multi and composite forms; `pos`,
  `posList` with `srsDimension`, and GML 2 `coordinates`. The axis order
  comes from the nearest `srsName`:

  | srsName | Order |
  | --- | --- |
  | `urn:ogc:def:crs:EPSG::4326`, `http://www.opengis.net/def/crs/EPSG/0/4326` | Latitude first |
  | `EPSG:4326`, `http://www.opengis.net/gml/srs/epsg.xml#4326`, CRS84 | Longitude first |
  | none (the collection's envelope, else what was asked) | Latitude first |

## The cache and offline

Each page is fetched through a `CachedTileFetcher` (the disk cache,
revalidation, the offline state and status messages of WMS,
[network-and-cache.md](network-and-cache.md)) under the stored tile's key
with the page number in the row's high bits (rows of stored levels are
below 2²⁰), so one cache file holds a source's pages. Bodies are checked by
parsing them before they are kept. `WfsFeatureProvider` is a
`RegionTileSource`: an offline region fetches and pins every page of each
stored tile its area needs (a stored page is read from the disk to see
whether another follows), and the New Offline Region dialog offers WFS
layers with the WMS, WMTS and XYZ ones.

## Per-feature styles

A source's `styleRules` (`ts::StyleRule`) give features a style by their
attributes: `{"attribute": "class", "equals": "motorway", "outline":
"#e8503aff", "outlineWidthPx": 3}`, or `"min"`/`"max"` for numbers; a rule
without `"attribute"` matches every feature. The first matching rule
wins; a feature no rule matches keeps the style its data gives it in the
simplestyle properties GeoJSON files use (`stroke`, `stroke-width`,
`stroke-opacity`, `fill`, `fill-opacity`, `marker-color`, `marker-size`),
else its layer's. Rules are part of the source (and its id), as they
change what the shared meshes hold. The mesher groups each tile's output
in runs by style and the renderer draws each run with its own colours
([vector-rendering.md](vector-rendering.md#per-feature-styles)).

## Picking

The provider keeps the features of the last 64 stored tiles it loaded;
`featuresAt()` returns those under a point: polygons containing it (holes
excluded), lines and points within a tolerance (four pixels at the view's
scale, longitude scaled by the latitude), each feature once. What Is Here
lists them with the WMS layers' answers ([wms.md](wms.md#getfeatureinfo)).

## The catalogue

`data/wfs-catalogue.json` (format `terrasoar-wfs-catalogue`, the WMS
catalogue's fields) is built in and fetched from the main branch
(`ts::wfsCatalogueUrl`, or `TS_WFS_CATALOGUE_URL`): PDOK's administrative
boundaries, EMODnet Bathymetry and MSC GeoMet, all open WFS 2.0 services.

## Tests

`tests/wfs`: three real capabilities documents (PDOK on MapServer,
EMODnet on GeoServer, MSC GeoMet trimmed to 8 types; contact details
removed) and broken ones; request building (BBOX axis order, paging,
1.1.0); output format choice and minimum levels; JSON round trips; real
GetFeature answers in GeoJSON and GML 3.2 from EMODnet giving the same
features, PDOK's GML polygon, every axis order, GML 2, curves and 3D
positions, exceptions; style rules; meshing in runs with points; and the
provider against the mock WFS (`tests/support/mockwfs.h`): levels,
paging and its limit, GeoJSON and GML agreeing, the disk cache and
offline use, picking polygons, lines and points, style rules on load,
offline regions, the registry, concurrency. `tests/ui`: the Add Web
Feature Service (WFS) Layer dialog and a WFS layer loaded, drawn and
picked.
