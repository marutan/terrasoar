# Adding WFS layers

A Web Feature Service (WFS) serves map data as **features** rather than
pictures: boundaries, roads, contour lines, weather stations, each with
its own attributes (a name, a code, a depth). Terra Soar draws them as
points, lines and areas over the imagery, on the globe and the flat maps,
lying on the terrain when it is shown, and tells you what a feature is
when you click it.

## Adding a layer

1. Choose **File → Add WFS Layer…**, or **Add Layer → WFS Layer…** at the
   bottom of the Layers panel.
2. In **Service**, pick one of the listed services, or type the address of
   any WFS, for example
   `https://service.pdok.nl/kadaster/bestuurlijkegebieden/wfs/v1_0`.
3. Press **Connect**. The server's **feature types** (its layers of
   features) are listed; hold the pointer over one to read its
   description.
4. Choose one, and a **Colour**: lines and points are drawn in it, areas
   in a see-through wash of it.
5. Read the lines below: the format the features come in, from how close
   they show, who to credit and the server's terms. Then press **Add**.

The layer goes into the Layers panel with the other vector layers, which
are always drawn above the imagery.

## How the features load

Terra Soar asks the server only for the features in view, a piece of the
map at a time, and keeps them on your computer like the tiles of other
web layers: places you have seen come back at once, also without a
network, and areas can be kept for offline use (see
[Using Terra Soar offline](offline-use.md)).

- **Zoomed out**, a layer covering a small area (one country) shows as
  soon as that area fills about a tile; a layer covering the world shows
  at once. A layer that would need too many features at that scale shows
  them only as you zoom in: the layer's status says "zoom in to see them
  all" while some are missing.
- **Large layers** come in pages of up to 1,000 features, at most five
  pages per piece of the map.
- **Colours from the data.** Features that carry their own colours (the
  `stroke` and `fill` properties many GeoJSON files use) are drawn in
  them.

## What a feature is

Click a feature (without dragging): the **What Is Here** window lists
the features under the pointer, with all their attributes, for each WFS
layer, together with what the WMS layers there say (see
[What is here](adding-wms-layers.md#what-is-here)). Lines and points are
found within a few pixels of the click.

## The listed services

| Service | What it has |
| --- | --- |
| PDOK administrative boundaries | The Dutch municipalities, provinces and the country, with names and codes |
| EMODnet Bathymetry | Depth contours, survey areas and the names of the European seas. Not for navigation |
| MSC GeoMet | Weather observations, air quality and forecast outlines for Canada |

## Things to know

- Terra Soar reads WFS 2.0 (and 1.1), in GeoJSON or GML. Services that
  offer only other formats can't be added.
- Features are asked for in latitude and longitude (WGS 84); servers
  convert them from their own coordinate systems.
- Credits are shown in the corner of the view while the layer is visible.
