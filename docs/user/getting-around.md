# Getting around

Terra Soar starts on a 3D globe looking at the Earth from space. Every
layer is drawn the same way on the globe and on the flat map, and the
view moves smoothly between them.

## The globe

| Mouse | |
| --- | --- |
| Drag (left button) | Move the globe; the point you grabbed stays under the pointer. Let go while moving and it glides on |
| Drag with the right or middle button, or Ctrl + left | Turn (left–right) and tilt (up–down) the view about its centre |
| Wheel | Zoom in or out towards the pointer |
| Double-click | Fly to that point, three times closer |

| Key | |
| --- | --- |
| Arrows or W A S D | Move forward, back, left, right |
| Page Up / Page Down, + / − | Zoom in / out |
| Q / E | Turn left / right |
| R / F | Tilt up / down |
| Home, View → Reset View | Fly back to the start view |

Keys move the view for as long as you hold them, at the same speed however
fast the computer draws.

## The navigation controls

The controls in the top-right corner of the view do the same with the
mouse, a touch screen or the keyboard, without needing to know the
buttons and keys:

- **The compass** shows where north is. Drag its ring round to turn the
  view; click the **N** to turn back to north up. The round arrows either
  side turn the view a step at a time, or for as long as you hold them.
- **The arrows** move the view: a click moves it a quarter of the way
  across; held, it keeps moving, faster the longer you hold.
- **The house** in the middle flies back to the start view (the same as
  Home).
- **+ and −** zoom in and out (a click halves or doubles the distance;
  held, they keep zooming). **The slider** between them shows how high you
  are (the eye's height on the globe, the scale on the map): click or
  drag it to fly to another height.
- **The tilt buttons** tilt up or down, and **the arc** beside them shows
  the tilt, from straight down to straight up: click or drag on it to
  tilt; double-click it to look straight down. On the globe you can tilt
  past the horizon and look up into the sky, for satellites or aircraft
  overhead: up to 85° the view circles the point in the middle; beyond
  that you stay where you are and only look further up.

The controls are half see-through until the pointer is over them, and
stay readable over snow, sea, space or a pale map. Their tooltips name the
keys that do the same.

**View → Navigation Controls** shows or hides them (hidden, a click there
goes to the globe or map), and **View → Navigation Controls Corner** puts
them in another corner. Terra Soar remembers both. With the keyboard, Tab
from the view into the controls, then use the arrow keys and Enter. They
grow with a larger system font, and are at least 9 mm across for fingers.
With the system's high-contrast setting on, they are drawn opaque in the
system's colours.

With [terrain](terrain.md) switched on, zooming and moving follow the
ground, and the camera stops above mountains.

## The flat map

**View → 2D Map** (Ctrl+M) turns the globe into a flat map, and back. The
map starts with north up, seen from straight above. On the map the left
button drags it and the wheel zooms as on the globe, and it can be turned
and tilted too: with the right button, Q / E and R / F, or the navigation
controls. A tilted map looks towards its horizon, up to 70°; far off it
fades into a haze, with the sky beyond, and with
[terrain](terrain.md) on the mountains stand up in relief (straight down
the map is flat, with its hillshade). The compass's **N** turns the map
back to north up, and Home or **View → Reset View** goes back to north up
and straight down. **View → Map Projection** chooses how it is flattened:

- **Mercator** (on the WGS 84 ellipsoid): directions are true, so it is
  the projection of sea charts; areas grow towards the poles, and it stops
  at about 85° north and south.
- **Web Mercator**: the spherical version used by web maps; nearly the
  same picture.
- **Miller Cylindrical**: a compromise that shows the whole world, poles
  nearly included, with less stretching near the poles.
- **Equidistant Cylindrical (Plate Carrée)**: latitude and longitude as a
  plain grid; distances along the meridians are true. It has a setting
  (below).
- **Gall Stereographic**: a compromise for world maps, poles included,
  less stretched than Mercator.
- **Gall–Peters (Gall Orthographic)**: areas are true everywhere, so
  countries can be compared by size; shapes are stretched north–south
  near the equator and squashed near the poles.

- **Transverse Mercator**: Mercator turned on its side, true to scale
  along a central meridian of your choice. Regional and national maps of
  places that stretch north–south. It shows 40° either side of that
  meridian.
- **Universal Transverse Mercator (UTM)**: Transverse Mercator on the
  middle of one of the sixty 6° zones used by military and survey maps
  worldwide.
- **Cassini–Soldner**: the older transverse projection of many national
  and cadastral surveys; 30° either side of its central meridian.
- **Rotated Mercator (Oblique Mercator)**: Mercator turned so that a
  line of your choice, not the equator, is true to scale: for long,
  narrow areas that don't run north–south, such as a coastline or a
  flight route. North is up at its centre. The map ends at its own edge:
  far from the line, where it would stretch without limit, and far along
  it.
- **Lambert Conformal Conic**: the standard projection of aeronautical
  charts and many national and continental maps; shapes are true, and
  scale is true along one or two parallels. It starts as the map of
  Europe used by the European Union (EPSG:3034). The map is a fan about
  the pole; it leaves out the far side of the Earth and the opposite
  hemisphere beyond 60° past the standard parallels.
- **American Polyconic**: the projection of older US topographic maps:
  every parallel is true to scale, as is the central meridian. It shows
  75° either side of that meridian.
- **Azimuthal Equidistant**: distances and directions from the centre
  are true, as on radio and great-circle range maps. It starts centred on
  the North Pole and shows nearly the whole Earth (the far side's edge is
  the point opposite the centre).
- **Gnomonic**: every great circle is a straight line, so the shortest
  routes plan as straight lines. It shows up to 80° from its centre.
- **Polar Stereographic (North)** and **(South)**: the map of the polar
  regions, as used for sea ice and Antarctic maps; shapes are true.
  They start as the standard Arctic (EPSG:3413) and Antarctic
  (EPSG:3031) maps.
- **Robinson**, **Mollweide** and **Winkel Tripel**: maps of the whole
  world in one piece, with curved edges. Robinson and Winkel Tripel are
  compromises (Winkel Tripel is the one many atlases use); Mollweide is
  equal-area, so countries can be compared by size. They are centred on
  Greenwich and have no settings.
- **ARC Zones (RPF)**: the zones of the ARC system that military raster
  charts (CADRG) and imagery (CIB) are made in, so that such data is
  shown pixel for pixel. Each zone is a band of latitude (0° to 32°,
  32° to 48° and so on, nine each side of the equator); by default the
  map follows the view, showing the zone its centre is in and changing
  zone as you move. **ARC North Polar Zone** and **South Polar Zone**
  show the polar caps beyond 80°. Add the charts themselves with Local
  Sources → Add Imagery File… ([imagery files](layers.md#imagery-files)).
- **UK National Grid (OSGB36)**: the Ordnance Survey's grid of Great
  Britain, as on OS maps. The status bar gives the grid reference under
  the pointer (such as TQ 30080 80729). Positions are converted from GPS
  coordinates to the grid's OSGB36 datum to within a few metres; with
  the **UK grids** data pack (File → Data Packs) installed, to about
  10 cm, the Ordnance Survey's own transformation (OSTN15).

The cylindrical maps wrap round east to west, so you can keep dragging.
The transverse ones end at the edge of the area they show: the view stops
there, and slides along the edge if you drag past it.

### Projection settings

Some projections have settings. **View → Map Projection → Projection
Settings…** changes them for the projection shown (it is greyed out for
the others), and **Restore Defaults** puts them back. Projects remember
the settings with the projection.

- **Equidistant Cylindrical**: the **standard parallel**, the latitude
  (north and south) along which east–west distances are true too. 0°,
  the default, is Plate Carrée; larger values make the map narrower.
- **Transverse Mercator**: the **central meridian** (the line down the
  middle, true to scale) and the **scale factor** along it (1; 0.9996 or
  0.9996012717 are common in national grids, to spread the error).
- **UTM**: the **zone**, 1 to 60 from 180° west (zone 30 covers Greenwich
  and the west of Europe, 31 the east of France, and so on).
- **Cassini–Soldner**: the **central meridian**.
- **Lambert Conformal Conic**: the two **standard parallels** (give the
  same latitude twice for one), the **origin latitude** and the
  **central meridian**.
- **American Polyconic**: the **central meridian** and the **origin
  latitude**.
- **Azimuthal Equidistant** and **Gnomonic**: the **centre** (latitude and
  longitude). The Azimuthal Equidistant also has **North Pole** and
  **South Pole** presets.
- **Polar Stereographic**: the **latitude of true scale** and the
  **central meridian** (the one straight down from the north pole, or up
  from the south pole).
- **ARC Zones**: the **zone** (0 to follow the view; 9 and 18 are the
  polar zones) and the **product scale** (1:1,000,000 for ONC charts,
  1:500,000 for TPC and so on), which sets the pixels' shape.
- **Rotated Mercator**: the **centre** (latitude and longitude), the
  **azimuth** of the centre line there (its direction, clockwise from
  north) and the **scale factor** along it. Or give two places under
  **Centre Line Through Two Places** and choose **Use These Places**: the
  line then runs through both, centred halfway between them.

## The status bar

At the bottom right:

- the **pointer's position** in latitude and longitude; on the UK
  National Grid map also its **grid reference**; and, with an elevation
  layer, the **height of the ground** there (above sea level, or above
  the WGS 84 ellipsoid with View → Heights Above the Ellipsoid);
- the **eye height**: how high the camera is above the ellipsoid;
- the **base pack**'s state (checked when the program starts; it says if
  the built-in data is missing or damaged).

**View → Scale Bar** shows a ground distance at the centre of the view.

## Credits on screen

The bottom right corner of the view names the sources of the layers drawn,
as their providers ask. They change as layers are switched on and off.
**View → Attributions** hides them and shows them again; they are shown
when the program starts. Many providers' terms ask for this credit
wherever their data is shown, so leave them on in images you publish.
