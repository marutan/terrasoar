# Layers and projects

The **Layers** list (on the left; View → Layers shows it again if closed)
holds everything the globe can show. Tick a layer to show it, untick it to
hide it. Drag layers to change their order: higher in the list is drawn
on top.

## Built-in layers

A new project starts with:

| Layer (top to bottom) | What |
| --- | --- |
| Graticule | Lines of latitude and longitude |
| Shoreline | Land outlines (GSHHG), with more detail as you zoom in |
| Blue Marble | NASA's picture of the Earth, July 2004 |
| Terrain | Relief from the built-in elevation data, off at first ([terrain](terrain.md)) |

**Add Layer** adds more: the built-in layers again (also Land, the
shoreline filled, and test patterns), a [WMS layer](adding-wms-layers.md),
an [imagery file](#imagery-files), an
[elevation file or web terrain](terrain.md), or a solid colour (a tint
over what is below).

## Imagery files

**Local Sources → Add Imagery File…** (or **Imagery File…** under **Add
Layer**) adds a georeferenced image from your computer: a GeoTIFF, JPEG
2000 or another raster that says where on the Earth it is. For military
charts and imagery in the Raster Product Format (CADRG, CIB), choose the
disc's or folder's `A.TOC`: every chart it lists is shown, and the layer
is named after the disc or folder. To see such charts as they were made,
use View → Map Projection → **ARC Zones (RPF)** ([getting
around](getting-around.md)). Images are fitted to the globe's tiles, so
they are a little softer than the file, most of all near the poles.

Vector layers (shoreline, graticule) are always drawn above imagery, in
their order in the list.

## Collections

**Add Collection** makes a folder for layers. Ticking a collection shows
or hides all of it; its opacity fades all of its layers. **Composite as
group** draws a collection's layers together first and fades the result
as one picture, so overlapping layers don't show through each other.

## Opacity and removing

Select a layer or collection and set its **Opacity** with the slider.
**Remove** deletes the selected entry (not its data: a WMS layer's saved
tiles stay until you clear them).

## Time

Some layers show a different picture for each time: daily satellite
images, forecasts, radar, or Blue Marble month by month once its pack is
installed. These follow the program's **clock**, which starts at the
current time.

When you add the first such layer, the **time slider** appears under the
view (**View → Time Slider** shows or hides it):

- Drag the **slider** to move through the times the layers have; the
  date next to it is the time shown. Each place on the slider is a time
  at least one layer has a picture for.
- **◀◀** and **▶▶** go to the previous and next time.
- **▶ Play** shows the times one after another, each for as long as the
  box beside it says (¼ s to 5 s), and stops at the last. Each time's
  tiles are fetched as it comes; times seen before come back from the
  saved tiles at once.

A layer shows the latest of its times not after the clock's, so layers
with daily and monthly pictures can be shown together. Right-click a layer
and use **Time** to have it follow the clock, follow only the clock's
month (for monthly pictures, in any year), or stay at one time of its own.

The clock's time is saved in projects.

## Projects

A project keeps the layer list (with each layer's settings, such as
opacity, exaggeration and hillshade) and the view.

- **File → Save Project** (Ctrl+S) / **Save Project As…** writes a
  `.terrasoar` file.
- **File → Open Project…** (Ctrl+O) replaces the layers and view with a
  saved project.

Project files are small text files. Elevation files are referred to by
their path, so keep them where they were, or open the project and add
them again.
