# Local imagery and RPF

Imagery from a raster file on disk: a GeoTIFF, JPEG 2000, or the Raster
Product Format's charts and imagery (CADRG, CIB) through their `A.TOC`.
Code: `core/include/ts/providers/gdalimageryprovider.h`. The ARC zones
the RPF products are made in are map projections of their own
([projections.md](projections.md#the-arc-zones-of-rpf)).

## The provider

`GdalImageryProvider` is the imagery layer source
`{"kind": "file", "path": …}` (File → Add Imagery File…, or Imagery
File… under Add Layer). GDAL opens the file:

- **Parts.** A file with subdatasets is all of them, drawn in the order
  GDAL lists them, later over earlier; otherwise the file is its one
  part. An RPF `A.TOC` has one subdataset per product, scale and zone
  (the ONC charts of zone 2, say), each a mosaic of its frames.
- **Polar zones.** GDAL before 3.13 places an `A.TOC`'s polar entries
  wrongly (the origin in degrees, the pixels in metres), though each
  frame on its own rightly; so a projected entry is read as its frames
  (`framesIfPolar()`).
- **Extent and depth.** A part's extent is found by taking its edges to
  latitude and longitude (all longitudes, up to the pole, if a pole is
  inside it). The deepest level is the one whose texels are no larger
  than the finest part's pixels (a projected raster's metres over
  111.32 km a degree).
- **A tile.** Each of its 256 × 256 pixel centres is taken into each
  part's pixel coordinates (PROJ, through GDAL); the window they cover is
  read once, at most 512 pixels a side: where the tile is coarser than
  the raster GDAL decimates (from overviews if there are any; averaged,
  or nearest for a palette), and the samples are bilinear in that buffer,
  premultiplied. Paletted rasters (CADRG) take their palette's colours and
  alpha, so a frame's transparent index (216) shows what is below; grey
  and RGB(A) rasters their bands by colour interpretation, opaque but
  where a band's no-data value is. A tile with nothing from any part is
  no data.
- **Threads.** GDAL handles are per thread: a part keeps a pool of
  readers, each with its dataset and transform, opened when a loader
  thread needs one and kept for the next load.

## Not pixel for pixel

The roadmap's test for RPF asks for a CADRG frame drawn without
resampling in its own zone. The renderer draws all imagery through the
quadtree's latitude/longitude tiles, so the provider resamples the chart
into them, at the chart's own resolution at the deepest level. In a
non-polar zone that is close to the chart's pixels but not exactly them;
near the poles a geographic tile is very narrow on the ground, so a
polar chart is softer than its frames. Drawing frames on their own grid
would need a drawing path that bypasses the quadtree; it waits for
Phase 22's local imagery.

## Test charts

`tests/data/rpf` holds two synthetic CADRG charts (GNC, 1:5M) of a
checker of eight colours, made by `tools/fixtures/make_rpf_fixtures.cpp`
with GDAL's CADRG writer (GDAL 3.13 or later; any GDAL reads them): one
frame in zone 2 (2° to 3.5° E, 34.5° to 36° N) and one in zone 9, the
north polar zone (0° to 10° E, 85° to 86.5° N). The tests check where
each square is drawn and its colour, that the frame's pixel spacing is
the ARC zone's (`arc::pixelSizeDeg()`), and golden images of both
charts on the globe and in their zones.
