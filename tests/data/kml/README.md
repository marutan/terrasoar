# KML fixtures

Fixtures for `tests/kml` (and the display tests). Nothing is fetched from
the network.

## Written for the tests

| File | Covers |
| --- | --- |
| `everything.kml` | Every element Terra Soar reads: shared and inline styles, a StyleMap, ListStyle, nested folders, each geometry (with altitude modes, extrude and tessellate), ExtendedData (Data and SchemaData), LookAt, Camera, Region and Lod, TimeStamp, TimeSpan, gx:Track, GroundOverlay (LatLonBox with rotation, gx:LatLonQuad), ScreenOverlay, NetworkLink, an external style, and unknown elements (atom:author, PhotoOverlay) |
| `styles.kml` | The external style `everything.kml` refers to |
| `heights.kml` | Shapes above the ground near London: an extruded polygon at an absolute height, an extruded line relative to the ground with a 2.4° edge, a point on a stalk, and a draped line |
| `links.kml` | NetworkLinks to local files (one waiting for a Region near London), a radioFolder and a checkHideChildren folder |
| `broken.kml` | Malformed XML (an unclosed element) |
| `images/overlay.png` | The ground overlays' picture, 64 × 32: red top left, green top right, blue bottom left, white bottom right (to check which way round it is drawn) |
| `images/legend.png` | The screen overlay's picture: orange, 40 × 20 |
| `archive.kmz` | A KMZ: doc.kml with an icon and a ground overlay image inside the archive |

## Written by other programs (`written/`)

`everything.kml` translated by GDAL 3.8.4 (`GDALVectorTranslate`, as
`ogr2ogr` and QGIS's export do) with its two KML writers: `gdal-kml.kml`
(the KML driver) and `gdal-libkml.kml` (LIBKML). Made from our own
fixture, so Terra Soar's to use.

## From GDAL's tests (`gdal/`)

From GDAL's `autotest/ogr/data/kml` (https://github.com/OSGeo/gdal),
fetched on 2026-11-20, unchanged. GDAL's test data is under the MIT
licence. `samples.kml` is Google's "KML Samples" from the KML
documentation (Google, CC BY 4.0), as GDAL ships it.

| File | Covers |
| --- | --- |
| `samples.kml` | Google's KML tutorial samples (KML 2.1 namespace): styles, every geometry, extrude and altitude modes |
| `geometries.kml` | Every geometry kind |
| `gxtrack.kml`, `gxmultitrack.kml` | gx:Track and gx:MultiTrack |
| `placemark_with_kml_prefix.kml` | Elements with a `kml:` prefix |
| `kml_with_space_content_in_coordinates.kml`, `tab_separated_coord_triplet.kml` | Coordinates with odd whitespace |
| `truncated.kml` | A file cut short |
| `extended_data_without_schema_data.kml`, `test_schema.kml` | ExtendedData forms |
| `point_with_external_style.kml`, `style_of_point_with_external_style/style.kml` | A styleUrl into another file |

## Schemas (`schema/`)

For checking the KML Terra Soar writes with `xmllint --schema
schema/kml-and-gx.xsd` (the `validate()` test helper; `--nonet`, nothing
is fetched).

| File | What |
| --- | --- |
| `ogckml22.xsd`, `atom-author-link.xsd` | The OGC KML 2.2 schema from http://schemas.opengis.net/kml/2.2.0/ (OGC Software Notice), fetched on 2026-11-20. One change: the xAL import's `schemaLocation` points at `xAL.xsd` here instead of OASIS's site |
| `xAL.xsd` | OASIS xAL 2.0, which the KML schema imports, from http://docs.oasis-open.org/election/external/xAL.xsd |
| `terrasoar-gx.xsd` | Terra Soar's own schema for the `gx:` elements it writes (Track, coord, altitudeMode, LatLonQuad), written from Google's KML reference; not Google's schema |
| `kml-and-gx.xsd` | Imports the two, so a file using both can be checked |
