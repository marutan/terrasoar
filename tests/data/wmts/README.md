# WMTS capabilities documents

Fixtures for `tests/wmts`. No test fetches anything from the network.

## Real-world documents

Fetched on 2026-11-18. Each `<ows:ServiceContact>` block was removed (it
names people); nothing else was changed, except where a document was
trimmed as noted (the first layers kept, with only the tile matrix sets
they use).

| File | Service | Address | Notes |
| --- | --- | --- | --- |
| `basemapat.xml` | basemap.at (Austria) | `https://basemap.at/wmts/1.0.0/WMTSCapabilities.xml` | KVP and RESTful; two RESTful hosts; CRS written `urn:ogc:def:crs:EPSG:6.18.3:3857` |
| `emodnet-bathymetry.xml` | EMODnet Bathymetry | `https://ows.emodnet-bathymetry.eu/wmts/1.0.0/WMTSCapabilities.xml` | RESTful only; `inspire_quad` in EPSG:4326 with the corner latitude first; polar and LAEA sets; limits. Trimmed to the first 6 of 20 layers |
| `gibs-epsg4326.xml` | NASA GIBS | `https://gibs.earthdata.nasa.gov/wmts/epsg4326/best/1.0.0/WMTSCapabilities.xml` | RESTful only; CRS84 sets of 512-pixel tiles reaching past the world's edge; a Time dimension and several templates per layer. Trimmed to the first 12 of 1,330 layers |
| `gibs-epsg3857.xml` | NASA GIBS | `https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/1.0.0/WMTSCapabilities.xml` | The same in Web Mercator; CRS written `urn:ogc:def:crs:EPSG:6.18:3:3857`. Trimmed to the first 12 layers |
| `pdok.xml` | PDOK BRT-Achtergrondkaart (the Netherlands) | `https://service.pdok.nl/brt/achtergrondkaart/wmts/v2_0?request=GetCapabilities&service=WMTS` | Sets in EPSG:28992 (RD New), EPSG:25831 and EPSG:3857; matrix identifiers with leading zeros |
| `swisstopo.xml` | swisstopo / geo.admin.ch | `https://wmts.geo.admin.ch/EPSG/3857/1.0.0/WMTSCapabilities.xml` | A Time dimension in the template; per-layer sets with different depths. Trimmed to the first 8 of 688 layers |
| `usgs-arcgis.xml` | USGS The National Map topographic base map | `https://basemap.nationalmap.gov/arcgis/rest/services/USGSTopo/MapServer/WMTS/1.0.0/WMTSCapabilities.xml` | ArcGIS Server; format `image/jpgpng`; `default028mm` and `GoogleMapsCompatible` sets |

## Synthetic documents

Written for the tests, to cover valid but unusual cases compactly.

| File | Covers |
| --- | --- |
| `synthetic-kvp-limits.xml` | KVP only, with a RESTful Get listed first; limits listing some matrices (and a broken entry); a default style not listed first, with a legend; an extra dimension with a unit; a FeatureInfo template (ignored); an EPSG:4326 set whose corners are written longitude first, matrices out of order and one broken; a layer only in British National Grid (reprojected through PROJ); a layer only in IGNF:LAMB93 (not an EPSG code: can't be drawn); a link to a missing set; Web Mercator as EPSG:900913; two AccessConstraints; a layer without an identifier |
| `exception.xml` | An OWS ExceptionReport instead of capabilities |
| `error-page.html` | An HTML error page (the same as `wms/error-page.html`) |
| `truncated.xml` | The first 3,000 bytes of `basemapat.xml` |
