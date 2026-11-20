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
  now.
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

Right-click a folder or a Places layer and choose **Fly There** to go to
its view, or to see all of it.

## Things to know

- Elements Terra Soar doesn't show yet are skipped; the status bar says
  how many.
- A file that isn't valid KML is refused with the line and column where
  it goes wrong.
- Creating and editing your own KML, and saving to KML and KMZ, come
  later in this phase.
- Not shown: photo overlays, 3D models, tours, and sounds.
- Paths and areas in the air, and icons, are drawn over the view: a
  hill in front of them doesn't hide them.
