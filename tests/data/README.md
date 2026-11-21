# Test data

Small sample files used by the tests. All are written for this project and
are under the same licence as the code, except where the table says
otherwise.

| File | Used by | Contents |
| --- | --- | --- |
| `sample.geojson` | `tests/deps` | Two features (a point and a 1°×1° square) read through GDAL/OGR |
| `sample.wkt` | `tests/deps` | A 10×10 square with a 2×2 hole (area 96), read through GEOS |
| `projects/v1-basic.terrasoar` | `tests/layers` | A project in format version 1 (a composite collection and the checkerboard). Every format version keeps a fixture here that must keep loading; see docs/design/layer-model.md |
| `projects/v2-map.terrasoar` | `tests/layers`, `tests/smoke` | A project in format version 2, saved in the 2D map view with Miller |
| `wms/*` | `tests/wms` | WMS capabilities documents: seven real ones (contact details removed) and synthetic odd ones; sources in `wms/README.md` |
| `wmts/*` | `tests/wmts` | WMTS capabilities documents: seven real ones (contact details removed, large ones trimmed) and synthetic odd ones; sources in `wmts/README.md` |
| `golden/*.png` | `tests/globe` | Golden images of the globe from fixed cameras, rendered on Mesa llvmpipe; see docs/testing.md to regenerate them |
| `rpf/*` | `tests/providers`, `tests/globe`, `tests/ui` | Two synthetic CADRG charts (a checker, GNC 1:5M) in ARC zones 2 and 9, written by `tools/fixtures/make_rpf_fixtures.cpp` with GDAL 3.13's CADRG writer; see docs/design/rpf.md |
| `geodtest-sample.dat` | `tests/geo` | 500 geodesics, every 20th line of C. F. F. Karney's `GeodTest-short.dat` (<https://doi.org/10.5281/zenodo.32156>, CC0 1.0); source and columns in the file header |
| `vmap0/VMAPLV0/*` | `tests/vmap0`, `tests/globe` | One tile (L/H: 30° W–15° W, 15°–30° N) of the SOAMAFR library of NGA's Vector Map Level 0, edition 5, as a small VPF database, clipped by `tools/fixtures/make_vmap0_fixture.py` from <https://geo.lib.umn.edu/gisdata/vmap0/soamafr.zip> (SHA-256 `41367cbb…f69d0fe`): every thematic coverage with that tile's features, without the features ESRI owns (first-order administrative boundaries, named administrative areas). A US government work (NGA, releasability unrestricted); see docs/data/vmap0.md |
