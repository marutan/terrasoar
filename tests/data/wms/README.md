# WMS capabilities documents

Fixtures for `tests/wms`. No test fetches anything from the network.

## Real-world documents

Fetched on 2026-11-11 with `SERVICE=WMS&REQUEST=GetCapabilities&VERSION=…`.
Each `<ContactInformation>` block was removed (it names people); nothing
else was changed, except where a document was trimmed as noted.

| File | Service | Version | Notes |
| --- | --- | --- | --- |
| `terrestris-130.xml` | terrestris OpenStreetMap WMS, `https://ows.terrestris.de/osm/service` | 1.3.0 | MapProxy; dozens of CRS; MaxWidth/MaxHeight |
| `terrestris-111.xml` | the same | 1.1.1 | DOCTYPE with an internal subset |
| `gebco-130.xml` | GEBCO, `https://wms.gebco.net/mapserv` | 1.3.0 | MapServer |
| `usgs-130.xml` | USGS The National Map topographic base map, `https://basemap.nationalmap.gov/arcgis/services/USGSTopo/MapServer/WMSServer` | 1.3.0 | ArcGIS Server; CRS:84 and EPSG:102100 |
| `eox-111.xml` | EOX Maps, `https://tiles.maps.eox.at/wms` | 1.1.1 | An empty `<SRS>`; EPSG:900913; GetMap URL ending in "?" |
| `nowcoast-130.xml` | NOAA nowCOAST, `https://nowcoast.noaa.gov/geoserver/ows` | 1.3.0 | GeoServer; child layers inherit EPSG:4326 from the root only. Trimmed to the root's first 12 layers |
| `gibs-4326-130.xml` | NASA GIBS, `https://gibs.earthdata.nasa.gov/wms/epsg4326/best/wms.cgi` | 1.3.0 | MapServer; LayerLimit; a lower-case `epsg:4326`. Trimmed from 1,600 layers to the first 30 below the root |

## Synthetic documents

Written for the tests, to cover valid but unusual cases compactly.

| File | Covers |
| --- | --- |
| `synthetic-111-inheritance.xml` | Inheritance of SRS, styles, bounding boxes and attribution; several SRS codes in one element and an empty one; ScaleHint; a group without a name; noSubsets and fixed sizes; a GetMap URL with its own query (`map=`) |
| `synthetic-130-axes.xml` | A byte-order mark; a latitude-first EPSG:4326 BoundingBox as the only box; lower-case and empty CRS elements; a Web Mercator-only layer (EPSG:900913); scale limits; LayerLimit; a MaxWidth below the tile size |
| `synthetic-111-time.xml` | 1.1.1 `Dimension` declared at the root with its `Extent`, inherited by one child and given other values by another (`TIME` in upper case, `elevation`) |
| `exception-111.xml` | A ServiceExceptionReport instead of capabilities (MapServer's wording) |
| `error-page.html` | An HTML error page |
| `truncated-130.xml` | The first 3,000 bytes of `terrestris-130.xml` |

## GetFeatureInfo answers (`featureinfo/`)

Written for the tests in the layouts the servers use (field names and
structure as GeoServer 2.x, MapServer 8 and ArcGIS Server 11 answer;
values made up or from their documentation's examples):

| File | Covers |
| --- | --- |
| `geoserver.json`, `geoserver-empty.json` | GeoJSON feature collection with numbers, null, a list; and with no features |
| `geoserver-gml.xml` | GML 2 `featureMember` with a geometry to leave out and a `fid` |
| `mapserver-gml.xml` | MapServer's `msGMLOutput` (`<x_layer>`, `<gml:name>`, `<x_feature>`) |
| `arcgis-fields.xml`, `arcgis-collection.xml` | ArcGIS's `FIELDS` attributes and `FeatureInfoCollection` fields |
| `geoserver.html`, `prose.html` | An HTML table with a header row and a caption, a script; a page of prose with entities and an image |
| `geoserver-plain.txt`, `mapserver-plain.txt`, `nothing-plain.txt` | Plain-text layouts, and "nothing found" |
| `exception.xml` | A service exception |
