# Adding WMS layers

A Web Map Service (WMS) is a server that draws maps on request: satellite
images, street maps, weather, ocean depths, geology and much more.
Thousands of agencies and projects run one. Terra Soar shows a WMS layer
like any other: on the globe and the flat maps, blended with your other
layers at the opacity you choose, and saved with your project.

## Adding a layer

1. Choose **File → Add WMS Layer…**, or **Add Layer → WMS Layer…** at the
   bottom of the Layers panel.
2. In **Service**, pick one of the listed services, or type the address of
   any WMS server, for example `https://ows.terrestris.de/osm/service`.
   The address the server's documentation gives for "GetCapabilities"
   works too.
3. Press **Connect**. The server's layers appear in a tree; groups open to
   show the layers inside them.
4. Choose a layer. Below the list you can pick its **Style**, if the
   server offers more than one, and the image **Format**. Terra Soar
   suggests PNG for layers drawn over others (it keeps the background
   clear) and JPEG for layers that cover everything (smaller and faster).
5. Read the lines under the list: which coordinate system will be used,
   who to credit, and the server's own terms. Then press **Add**.

The layer goes to the top of the Layers panel (or into the collection you
had selected). Move it, hide it or change its opacity like any layer.

## The listed services

The list in the dialog comes with Terra Soar and is updated online, so
new services can appear without installing a new version. It contains
services that charge no fees and allow general use; check each one's
terms (the link under its description) before using its maps in your own
work.

| Service | What it shows |
| --- | --- |
| NASA GIBS | Satellite images of the whole Earth, daily and historical |
| OpenStreetMap (terrestris) | A street map of the world |
| GEBCO | Ocean depths and land heights. Not for navigation |
| USGS The National Map | Topographic maps of the United States |
| NOAA nowCOAST | Weather, ocean and coastal observations and forecasts, mostly for the United States |

## Credits

Most services ask to be credited. While a WMS layer is on screen its
credit is shown in the bottom-right corner of the view, as for the
built-in layers. If you publish a picture made with a WMS layer, keep the
credit in it.

## Saved tiles and working offline

Every tile Terra Soar downloads is kept on your computer (up to 256 MB per
layer; the least recently used go first). Places you have looked at come
back at once next time, without asking the server, for as long as the
server says its maps stay valid (a week when it doesn't say).

When a server can't be reached, for example without an internet
connection, Terra Soar shows the tiles it has saved, even old ones, and
coarser tiles where it has no fine ones. The layer gets a **warning icon**
in the Layers panel; hold the pointer over it to see which server can't be
reached. When the server is back, the next time you move the view the
layer catches up and the icon goes.

If a server answers with an error (for example because a layer was
withdrawn), the layer gets an **error icon** with the server's message,
and the message also appears in the status bar.

The saved tiles are in your cache folder: on Linux
`~/.cache/Terra Soar/terrasoar/wms`, on Windows
`%LOCALAPPDATA%\Terra Soar\terrasoar\cache\wms`. You can delete that
folder at any time; it fills up again as you browse.

## Things to know

- **Coordinate systems.** Terra Soar asks for maps in plain latitude and
  longitude (EPSG:4326 or CRS:84). If a server only draws Web Mercator
  (EPSG:3857, the system of web maps), Terra Soar asks for that and
  reshapes the images to fit; the dialog says so. Such layers have no
  data within about 5° of the poles, which Web Mercator can't show. Layers
  only available in other systems (national grids, UTM) can't be added
  yet.
- **Old servers.** Terra Soar speaks WMS 1.3.0 and 1.1.1 and asks for
  1.3.0 first; if a server refuses, it asks again in 1.1.1.
- **Several layers of one server.** Add them one at a time; each gets its
  own opacity and place in the tree.
- **Proxies.** Terra Soar uses your system's proxy settings (on Linux the
  `http_proxy`, `https_proxy` and `no_proxy` environment variables).
- **Speed.** Each tile is a request to the server, at most six at once.
  Busy or distant servers make layers fill in slowly the first time;
  saved tiles are fast afterwards.
- **Projects.** A project remembers the server's address and your
  choices, not the server's list of layers; opening it on another
  computer draws the same layer without connecting first.
