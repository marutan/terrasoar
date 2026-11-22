# Adding XYZ tile layers

Most web maps, OpenStreetMap among them, are served as plain tiles named
by zoom level, column and row: an address such as
`https://tile.openstreetmap.org/{z}/{x}/{y}.png`. These are called XYZ
tiles, or TMS tiles when the rows are counted from the south. They have no
capabilities document to browse: you give Terra Soar the address and it
does the rest. An XYZ layer is drawn, blended, saved with your project and
kept offline like a [WMS](adding-wms-layers.md) or
[WMTS](adding-wmts-layers.md) layer.

## Adding a layer

1. Choose **File → Add XYZ Tile Layer…**, or **Add Layer → XYZ Tile
   Layer…** at the bottom of the Layers panel.
2. In **Service**, pick one of the listed services: that fills in
   everything below, with its description and a link to its terms. Or
   leave it and fill in the fields yourself:
   - **Name**: what the Layers panel calls it.
   - **Address**: the tile address, with `{z}` for the zoom level, `{x}`
     for the column and `{y}` for the row. Some services put them in
     another order (`…/tile/{z}/{y}/{x}`), which is fine. `{-y}` is a row
     counted from the south.
   - **Subdomains for {s}**: if the address contains `{s}`, the names it
     stands for, separated by commas (often `a, b, c`). Each tile always
     comes from the same one.
   - **Rows are counted from the south (TMS)**: tick it for TMS services,
     whose row 0 is at the bottom of the map. If a layer comes out upside
     down, this is the box to change.
   - **Zoom levels**: the first and last levels the service has. Past the
     last, its finest tiles are enlarged; services usually say how deep
     they go.
   - **Credit**: whom to credit, in the words the provider asks for.
3. If something in the address can't be used (a `{key}` the program
   doesn't know, a missing `{y}`), the line under the fields says what.
   Otherwise press **Add**.

The layer goes to the top of the Layers panel (or into the collection you
had selected).

## The listed services

The list comes with Terra Soar and is updated online. Check each
service's terms (the link under its description) before using its maps in
your own work.

| Service | What it shows |
| --- | --- |
| OpenStreetMap | The standard OpenStreetMap map |
| OpenTopoMap | A topographic map with contours and hill shading, from OpenStreetMap and SRTM |
| USGS The National Map: imagery | Aerial and satellite imagery of the United States |
| NASA GIBS: Blue Marble with bathymetry | The whole Earth with shaded relief and sea depths |
| EMODnet Bathymetry | Depths of the European seas and the oceans. Not for navigation |
| Google Maps | Google's road map |
| Google Maps - Satellite | Google's satellite and aerial imagery |
| Google Maps - Hybrid | Google's imagery with roads, places and borders |

OpenStreetMap's and OpenTopoMap's tile servers are run by volunteers on
donated resources. Their terms allow looking at the map, not downloading
areas of it: Terra Soar won't keep them as offline regions.

The Google Maps entries use Google's own tile server, which is not a
published service: Google's terms allow its maps only through Google's
own products, so read them before using these layers, and expect the
address to stop working without notice. Terra Soar won't keep them as
offline regions either.

## Credits, saved tiles and working offline

While an XYZ layer is on screen its credit is shown in the bottom-right
corner of the view. Its tiles are saved on your computer, come back at
once from places you have looked at, and are shown when the server can't
be reached, as for WMS layers (see
[Adding WMS layers](adding-wms-layers.md#saved-tiles-and-working-offline)).
To keep an area for certain, download it as an offline region (see
[Using Terra Soar offline](offline-use.md)), where the service's terms
allow it.

## Things to know

- **Web Mercator.** XYZ tiles are always in Web Mercator, the system of
  web maps, which can't show the last 5° around the poles: there is no
  data there. Terra Soar reprojects them for the globe and every map
  projection.
- **Keys.** Services that need an API key in the address aren't supported
  yet.
- **Speed.** Each tile is a request, at most six at once, shared with your
  WMS and WMTS layers.
