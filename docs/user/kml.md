# KML and KMZ

KML is the file format of Google Earth and many other programs: places,
paths, areas, pictures laid over the ground, organised in folders. A KMZ
is the same in a zip archive, with its icons and pictures inside. Terra
Soar shows them on the globe and the flat maps.

## Opening a file

- **Local Sources → Open KML or KMZ…**, or
- drag files onto the window, or
- open them from your file manager with Open With → Terra Soar.

The file appears at the top of the Layers panel as a collection named
after its document. Each of its folders is a collection inside it, in the
same order, and the places of each folder are a layer called **Places**.
Tick and untick them as any layer; folders the file hides start
unticked. If the file has its own view, Terra Soar flies there.

Large files (tens of thousands of places) take a moment to read; Terra
Soar keeps going meanwhile.

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
- **Pictures on the ground** (ground overlays: old maps, weather,
  plans) are laid over the globe and the maps, turned and stretched as
  the file says. Each is its own entry in the Layers panel, so you can
  untick it, change its opacity or drag it above or below other imagery.
- **Pictures on the screen** (screen overlays: legends, logos) stay at
  their place on the screen whatever you look at. Each is its own entry
  too.
- Paths and areas crossing the 180° meridian are drawn the short way
  round, on the globe and on every map.

## Time

Places with dates show when the program's clock (the time bar, View →
Time Bar) is at their time: a place with one date from then on, a place
with a period during it. When a file has dates the time bar offers them,
so stepping it goes from one change to the next. Tracks (a path with a
time at each point, from GPS loggers and flight trackers) show a marker
where they were at the clock's time; play the clock to watch it move.

## Links, regions and lists

- **Network links** bring in more KML, from a file or the web. Each is an
  entry in the Layers panel holding what it brought. Some fetch again on
  their own, every so often or when you stop moving the view (to show
  what is in sight); right-click one and choose **Refresh** to fetch it
  now. Without a connection, the copy from last time is shown.
- **Regions**: some files show parts only when you are close enough
  (large files of maps or photos, "superoverlays", load more detail as
  you zoom in). Those parts come and go as you move.
- **Folders that let you choose one** (radio folders): ticking one of
  their entries unticks the others. Some folders are listed as one entry
  without their contents, as the file asks.

## What a place is

Click a place, path or area (without dragging) to see its **balloon**:
its name, description and the data the file has for it, in the file's
own layout when it has one. Links in it open in your web browser;
pictures from the web are not loaded. **Fly there** at the bottom takes
you to it.

Double-click a folder, a Places layer or a picture in the Layers panel
(or right-click it and choose **Fly There**) to go to its view, or to see
all of it. To rename it, select it and press F2.

## Saving

- Right-click a KML entry in the Layers panel and choose **Save As KML
  or KMZ…**: the whole file for its top entry, or just that folder (or
  picture or link) for an entry inside it. What Terra Soar doesn't show
  or change is saved as it was, so files from other programs lose
  nothing.
- A **KMZ** holds the icons and pictures the KML uses, so it works on
  another computer. If any of them can't be found, you are told which.
- In the **Elevation Profile** window, **Save as KML…** saves the line.
- In **Offline Regions**, **Export as KML…** saves the selected region's
  area.

## Drawing and editing

**KML → New KML Document** starts a document of your own at the top of
the Layers panel. To change a file you opened, select it and choose
**KML → Edit This Document** (or right-click it).

Then, from the **KML** menu:

- **Add Placemark**: click where it goes.
- **Add Path**: click each point; double-click (or press Enter) to
  finish.
- **Add Polygon**: click each corner; double-click to finish. **Add Hole
  to Polygon** draws a hole in the selected polygon the same way.
- **Add Ground Overlay…**: choose a picture, then drag the rectangle it
  covers.
- **Add Folder**. New things go into the folder selected in the Layers
  panel.

Click a place, path or area of a document you are editing to select it:
its points show as small squares.

- Drag a square to move that point (a placemark moves with its point).
- Drag one of the small dots between two points to add a point there.
- Right-click a square to delete that point.
- Drag a ground overlay's corners to stretch it.

**KML → Properties…** (Alt+Enter) changes the selected thing's name,
description (in HTML, with its balloon shown as it will look), the view
Fly There goes to (**Set from the View**), its style (icon, colours,
line width, fill, label size: for it alone, or a **shared** style, which
changes everything that uses it), its height (on the ground, above the
ground or above sea level, with walls down to the ground if you like),
when it shows (always, from a moment on, or during a period) and its own
data fields.

**Cut**, **Copy**, **Paste** and **Delete** work on the selected thing;
you can also paste KML copied in Google Earth. Drag folders in the
Layers panel to move them into other folders. **Undo** and **Redo**
(Ctrl+Z, Ctrl+Shift+Z) take back any change.

**KML → Save KML Document** saves it to its file (or asks where, for a
new one); **Save KML Document As…** saves a copy, as KML or as a KMZ with
its pictures. What Terra Soar doesn't change in a file you edit stays as
it was.

### In projects

A project remembers the KML files you opened and the documents you
edit. If a document you edit has unsaved changes when you save the
project, you are asked whether to save them to its file first; if not,
the project keeps them. A document you never saved is kept in the
project.

## Things to know

- Elements Terra Soar doesn't show yet are skipped; the status bar says
  how many.
- A file that isn't valid KML is refused with the line and column where
  it goes wrong.
- Not shown: photo overlays, 3D models, tours, and sounds.
- Paths and areas in the air, and icons, are drawn over the view: a
  hill in front of them doesn't hide them.
