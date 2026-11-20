# WFS documents

Fixtures for `tests/wfs`. No test fetches anything from the network.

Fetched on 2026-11-20. Each `<ows:ServiceContact>` block was removed (it
names people); nothing else was changed, except where noted.

| File | Service | Notes |
| --- | --- | --- |
| `pdok-200.xml` | PDOK administrative boundaries, `https://service.pdok.nl/kadaster/bestuurlijkegebieden/wfs/v1_0` | WFS 2.0.0 (MapServer); three types; paging, CountDefault 1000; JSON offered as `application/json` and `application/json; subtype=geojson` |
| `emodnet-200.xml` | EMODnet Bathymetry, `https://ows.emodnet-bathymetry.eu/wfs` | WFS 2.0.0 (GeoServer); `FeatureType` elements with their own namespace declarations |
| `geomet-200.xml` | MSC GeoMet, `https://geo.weather.gc.ca/geomet` | WFS 2.0.0 (MapServer); trimmed from 95 feature types to the first 8 |
| `emodnet-features.json`, `emodnet-features.xml` | EMODnet `world:sea_names`, `COUNT=2`, BBOX around the North Sea | The same two features as GeoJSON (longitude first, though its `crs` names `urn:ogc:def:crs:EPSG::4326`) and GML 3.2 (latitude first) |
| `pdok-features.xml` | PDOK `Provinciegebied`, `COUNT=1` | GML 3.2 polygon (Zuid-Holland); its `posList`s thinned to every 40th point so the file is small |
