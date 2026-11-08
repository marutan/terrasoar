# Test data

Small sample files used by the tests. All are written for this project and
are under the same licence as the code, except where the table says
otherwise.

| File | Used by | Contents |
| --- | --- | --- |
| `sample.geojson` | `tests/deps` | Two features (a point and a 1°×1° square) read through GDAL/OGR |
| `sample.wkt` | `tests/deps` | A 10×10 square with a 2×2 hole (area 96), read through GEOS |
| `geodtest-sample.dat` | `tests/geo` | 500 geodesics, every 20th line of C. F. F. Karney's `GeodTest-short.dat` (<https://doi.org/10.5281/zenodo.32156>, CC0 1.0); source and columns in the file header |
