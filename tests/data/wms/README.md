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
| `exception-111.xml` | A ServiceExceptionReport instead of capabilities (MapServer's wording) |
| `error-page.html` | An HTML error page |
| `truncated-130.xml` | The first 3,000 bytes of `terrestris-130.xml` |
