# Terrain

Terra Soar can draw the land in relief: mountains rise, valleys sink,
and imagery, shorelines and the graticule lie on the ground. The heights
come with the program (from NOAA's ETOPO 2022, a height every 4.9 km), so
terrain works offline.

## Switching it on

The layer list has a **Terrain** layer at the bottom, switched off in a
new project. Tick it to raise the land; untick it for the smooth globe
again. Switching it on or off is immediate, since its heights are loaded
all along.

**View → Terrain Exaggeration** draws the terrain 1, 1.5, 2, 3 or 5 times
as high as it is. At 4.9 km between heights, mountains are rounded and
lower than they are (Everest's summit is averaged with its valleys), so a
little exaggeration (2×) brings out the relief. The choice is saved with
the project.

## Your own elevation models

**File → Add Elevation File…** (or **Elevation File…** in the layer
list's add menu) adds a digital elevation model: a GeoTIFF or any other
raster GDAL reads, in latitude and longitude or a projected system such
as UTM. Terra Soar asks what its heights are measured from, suggesting
what the file says if it says:

- **Sea level, EGM2008**: most recent global DEMs (Copernicus DEM).
- **Sea level, EGM96**: SRTM, ALOS and older data.
- **WGS 84 ellipsoid**: heights from GNSS or lidar processing that
  weren't converted. National DEMs above a local sea level (in Britain,
  Ordnance Datum Newlyn) are within a metre or so of EGM2008.

The new layer goes above the other elevation layers and is switched on.
**Every visible elevation layer is used, the upper ones first**: the
file's heights where it has them, the built-in terrain around it, so a
detailed area sits in the global relief without gaps. Move a layer up or
down to change which wins where they overlap; untick it to leave it out.
The exaggeration is the topmost visible elevation layer's.

Large files show at their full detail when you zoom in. Zoomed far out,
a file without overviews isn't read (the built-in terrain shows instead);
`gdaladdo yourfile.tif` adds overviews, and Cloud Optimized GeoTIFFs have
them already.

## Terrain from the web

**File → Add Web Terrain…** (or **Web Terrain…** in the layer list's add
menu) adds heights from a server:

- **Terrain Tiles (AWS Open Data)**, ready to use: free worldwide heights
  from SRTM and other sources, about 5 m to 30 m apart where they are most
  detailed.
- **Terrain tiles of your own choosing**: an address with `{z}`, `{x}` and
  `{y}` in it, and whether the tiles use the Terrarium or the Terrain-RGB
  encoding (the provider's documentation says; services that need a key,
  such as MapTiler or Mapbox, take it in the address). Give the deepest
  zoom they have and the attribution the provider asks for: it is shown in
  the corner of the view while the layer is drawn.
- **A Cloud Optimized GeoTIFF** on a web server: only the parts in view
  are downloaded.

Web terrain works like the other elevation layers: it goes above them, and
the built-in terrain fills in wherever it has no heights (beyond 85° north
and south, or outside the area a service covers). Tiles are kept on your
computer like WMS tiles, so places you have seen work offline. If the
server can't be reached, the layer shows a warning and the built-in
heights stand in until it answers again.

## On the flat map

The terrain lies flat on the map, so its relief is shown by **hillshade**:
slopes facing the north-west are lighter, those facing away darker, as on
a printed relief map. It fades in as the globe turns into the map. Switch
it off with **View → Hillshade on the Map**; the choice is saved with the
project.

## Elevation profiles

**View → Elevation Profile…** shows the heights along a line:

1. Click the points of the line on the globe or map; double-click or
   press Enter to finish (Backspace removes the last point, Esc cancels).
2. The window charts the height above sea level against the distance
   along the line, and gives its length, lowest and highest point, and the
   total climb and descent.
3. Point at the chart to see where a height is: a marker follows on the
   globe.
4. **Save as CSV…** writes every point (distance, latitude, longitude,
   height above sea level and above the ellipsoid) for a spreadsheet.

**Draw Another Line** starts again. The heights come from the visible
elevation layers (your own files first), at a detail suited to the line's
length: 512 points along it, so a 5 km line shows detail every 10 m where
the data has it, a 500 km line every kilometre.

## Height under the pointer

Once there is an elevation layer (switched on or not), the status bar
shows the height of the ground under the pointer after its position:
above sea level, or above the WGS 84 ellipsoid (as a GNSS receiver gives
it) with **View → Heights Above the Ellipsoid**. The sea floor shows as
negative heights. The value comes from the heights loaded for the view,
so zoomed far out it is an average over several kilometres; zoom in for
the full detail of the data.

## What to expect

- **The sea floor is not shown.** Where the land is below sea level the
  terrain stays at sea level, so oceans and lakes are flat. That also
  flattens land below sea level, such as the Dead Sea's shore.
- **On the flat map the terrain is flat**, and it flattens gradually while
  the view changes between globe and map.
- **Clicking picks the ground**: zooming towards the pointer and dragging
  the globe hold the point of the terrain under the pointer, and the
  pointer's position in the status bar is on the terrain.
- **The camera stays above ground**: zooming in towards a mountain stops
  20 m above it.
- **Heights are relative to the ellipsoid** inside the program, but come
  from (and will be shown as) heights above sea level; see
  [the design notes](../design/vertical-datums.md) if that matters to you.

## More exact sea level

Heights above sea level depend on a model of where sea level is (the
geoid). Two data packs (**File → Data Packs…**) make them more exact:

- **EGM2008 geoid, 2.5′** (13 MB) replaces the built-in 5′ grid: heights
  above sea level within a centimetre or two of the full model, where the
  built-in grid can be half a metre off in high mountains.
- **OSGM15 geoid (Great Britain)** (1.5 MB) adds Ordnance Datum Newlyn,
  the datum of British maps and of OS and Environment Agency elevation
  data, to what elevation files can be measured from. Without it, choose
  EGM2008 for such files: within about half a metre.

The terrain and the height readout use them as soon as they are
installed.
