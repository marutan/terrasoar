# Adding WMTS layers

A Web Map Tile Service (WMTS) is a server that keeps maps already cut into
tiles: national base maps, aerial photographs, satellite imagery, ocean
depths. Because the tiles are ready-made, a WMTS layer usually fills in
faster than a [WMS](adding-wms-layers.md) one. Many national mapping
agencies and NASA publish their maps this way. Terra Soar shows a WMTS
layer like any other: on the globe and the flat maps, blended with your
other layers at the opacity you choose, and saved with your project.

## Adding a layer

1. Choose **Online Sources → Add Web Map Tile Service (WMTS) Layer…**, or
   **Add Layer → Web Map Tile Service (WMTS) Layer…** at the bottom of the
   Layers panel.
2. In **Service**, pick one of the listed services, or type the address of
   any WMTS server. Both kinds of address work:
   - a capabilities file, usually ending in `WMTSCapabilities.xml`, for
     example `https://basemap.at/wmts/1.0.0/WMTSCapabilities.xml`;
   - a service address, for example
     `https://service.pdok.nl/brt/achtergrondkaart/wmts/v2_0`; Terra Soar
     asks it for its capabilities.
3. Press **Connect**. The server's layers appear in a list; hold the
   pointer over one to read its description.
4. Choose a layer. Below the list you can pick:
   - its **Style**, if the server offers more than one;
   - the image **Format**: Terra Soar suggests PNG, which keeps the
     background clear for layers drawn over others; JPEG is smaller for
     layers that cover everything;
   - the **Tile matrix set**: the grid of tiles to use, if the layer comes
     in more than one. Terra Soar lists the ones it can draw and suggests
     the best: plain latitude and longitude first, then Web Mercator;
   - a value for each of the layer's **dimensions**, if it has any: most
     often a date (**Time**) for satellite imagery and forecasts, sometimes
     a height or depth. The server's default is filled in; pick another
     value from the list or type one.
5. Read the lines under the list: which tiles will be drawn, who to
   credit, the server's fees and terms, and a link to the layer's legend
   when it has one. Then press **Add**.

The layer goes to the top of the Layers panel (or into the collection you
had selected). Move it, hide it or change its opacity like any layer. To
see another date or style, add the layer again with the other choice: each
is a layer of its own.

## The listed services

The list in the dialog comes with Terra Soar and is updated online, so
new services can appear without installing a new version. It contains
services that charge no fees and allow general use; check each one's
terms (the link under its description) before using its maps in your own
work.

| Service | What it shows |
| --- | --- |
| NASA GIBS | Satellite images of the whole Earth, daily and historical (two entries: one lined up with the globe's tiles, one in Web Mercator) |
| basemap.at | The official base map of Austria, with aerial photographs |
| swisstopo | Maps and aerial photographs of Switzerland |
| PDOK BRT background map | The Dutch government's topographic background map |
| EMODnet Bathymetry | Depths of the European seas and the oceans. Not for navigation |
| USGS The National Map | Topographic maps of the United States |

## Credits

Most services ask to be credited. While a WMTS layer is on screen its
credit is shown in the bottom-right corner of the view, in the words the
provider asks for. If you publish a picture made with a WMTS layer, keep
the credit in it.

## Saved tiles and working offline

WMTS layers keep their tiles on your computer, come back at once from
places you have looked at, and show saved tiles when the server can't be
reached, exactly as WMS layers do (see
[Adding WMS layers](adding-wms-layers.md#saved-tiles-and-working-offline)).
Their tiles are saved in the same folder. A layer showing another date
keeps its tiles apart, so you never see one day's picture in place of
another's.

To keep an area for certain, download it as an offline region: see
[Using Terra Soar offline](offline-use.md). Some services' terms don't
allow that; Terra Soar then says so and doesn't download.

## Things to know

- **Coordinate systems.** Terra Soar draws tile matrix sets in plain
  latitude and longitude (EPSG:4326 or CRS:84), in Web Mercator
  (EPSG:3857, the system of web maps), and in national and regional grids
  such as the British National Grid, the Dutch RD New or LAEA Europe: any
  coordinate system with an EPSG code. When a layer comes in several, the
  dialog suggests latitude and longitude first, then Web Mercator, then the
  others; all look the same once drawn. Web Mercator layers have no data
  within about 5° of the poles, which Web Mercator can't show. A layer in a
  national grid is drawn only over the area the grid is made for (Great
  Britain for the British National Grid).
- **Detail.** Terra Soar uses the server's tiles as they are where they
  line up with its own, and cuts and combines them where they don't, so
  any tile grid works. Zoomed in past a layer's finest tiles, the finest
  are enlarged.
- **Speed.** Each tile is a request to the server, at most six at once,
  shared with your WMS layers. Saved tiles are fast afterwards.
- **Proxies.** Terra Soar uses your system's proxy settings, as for WMS.
- **Projects.** A project remembers the layer, your choices and the
  server's tile grid, not the whole list of layers; opening it on another
  computer draws the same layer without connecting first.
