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
an [elevation file or web terrain](terrain.md), or a solid colour (a tint
over what is below).

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
