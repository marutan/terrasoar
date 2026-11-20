# KML and KMZ

KML is the file format of Google Earth and many other programs: places,
paths, areas, pictures laid over the ground, organised in folders. A KMZ
is the same in a zip archive, with its icons and pictures inside. Terra
Soar shows them on the globe and the flat maps.

## Opening a file

- **File → Open KML or KMZ…**, or
- drag files onto the window, or
- open them from your file manager with Open With → Terra Soar.

The file appears at the top of the Layers panel as a collection named
after its document. Each of its folders is a collection inside it, in the
same order, and the places of each folder are a layer called **Places**.
Tick and untick them as any layer; folders the file hides start
unticked. If the file has its own view, Terra Soar flies there.

## What is drawn

- **Paths and areas** in the colours and widths the file gives them.
  Areas are filled and outlined as their style says.
- **Places** (points) with their icons, in the colours, sizes and
  angles the file gives, and their names beside them. Icons inside a KMZ
  or next to the file show at once; icons from the web are fetched and
  kept, so they show offline next time. Until then, and for icons that
  can't be found, a yellow pushpin stands in.
- Where places crowd together, some icons and names are left out so
  the rest stay readable; zoom in to see them all.
- **Heights.** Paths and areas the file puts above the ground are drawn
  in the air, at a height above sea level or above the terrain, with
  walls down to the ground where the file asks for them ("extruded"),
  so buildings, flight paths and fences stand up. Places in the air can
  stand on a line down to the ground. On the flat maps they lie flat.
  Hills and mountains in front of them don't hide them.
- Paths and areas crossing the 180° meridian are drawn the short way
  round, on the globe and on every map.

## What a place is

Click a place, path or area (without dragging) to see its **balloon**:
its name, description and the data the file has for it, in the file's
own layout when it has one. Links in it open in your web browser;
pictures from the web are not loaded. **Fly there** at the bottom takes
you to it.

Right-click a folder or a Places layer and choose **Fly There** to go to
its view, or to see all of it.

## Things to know

- Elements Terra Soar doesn't show yet are skipped; the status bar says
  how many.
- A file that isn't valid KML is refused with the line and column where
  it goes wrong.
- More is coming in this phase:
  pictures over the ground, links to online KML, time, and creating and
  editing your own KML.
