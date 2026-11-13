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

With [terrain](terrain.md) switched on, zooming and moving follow the
ground, and the camera stops above mountains.

## The flat map

**View → 2D Map** (Ctrl+M) turns the globe into a flat map, and back. On
the map the left button drags it, the wheel zooms, and the right button
does nothing (a map isn't turned or tilted). **View → Map Projection**
chooses how it is flattened:

- **Mercator** (on the WGS 84 ellipsoid): directions are true, so it is
  the projection of sea charts; areas grow towards the poles, and it stops
  at about 85° north and south.
- **Web Mercator**: the spherical version used by web maps; nearly the
  same picture.
- **Miller Cylindrical**: a compromise that shows the whole world, poles
  nearly included, with less stretching near the poles.

The map wraps round east to west, so you can keep dragging.

## The status bar

At the bottom right:

- the **pointer's position** in latitude and longitude, and, with an
  elevation layer, the **height of the ground** there (above sea level, or
  above the WGS 84 ellipsoid with View → Heights Above the Ellipsoid);
- the **eye height**: how high the camera is above the ellipsoid;
- the **base pack**'s state (checked when the program starts; it says if
  the built-in data is missing or damaged).

**View → Scale Bar** shows a ground distance at the centre of the view.

## Credits on screen

The bottom left corner of the view names the sources of the layers drawn,
as their providers ask. They change as layers are switched on and off.
