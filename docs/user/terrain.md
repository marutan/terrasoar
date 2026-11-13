# Terrain

Terra Soar can draw the land in relief: mountains rise, valleys sink,
and imagery, shorelines and the graticule lie on the ground. The heights
come with the program (from NOAA's ETOPO 2022, a height every 4.9 km), so
terrain works offline.

## Switching it on

The layer list has a **Terrain** layer at the bottom, switched off in a
new project. Tick it to raise the land; untick it for the smooth globe
again. Switching it on or off is immediate, since its heights are loaded
all along.

**View → Terrain Exaggeration** draws the terrain 1, 1.5, 2, 3 or 5 times
as high as it is. At 4.9 km between heights, mountains are rounded and
lower than they are (Everest's summit is averaged with its valleys), so a
little exaggeration (2×) brings out the relief. The choice is saved with
the project.

## What to expect

- **The sea floor is not shown.** Where the land is below sea level the
  terrain stays at sea level, so oceans and lakes are flat. That also
  flattens land below sea level, such as the Dead Sea's shore.
- **On the flat map the terrain is flat**, and it flattens gradually while
  the view changes between globe and map.
- **Clicking picks the ground**: zooming towards the pointer and dragging
  the globe hold the point of the terrain under the pointer, and the
  pointer's position in the status bar is on the terrain.
- **The camera stays above ground**: zooming in towards a mountain stops
  20 m above it.
- **Heights are relative to the ellipsoid** inside the program, but come
  from (and will be shown as) heights above sea level; see
  [the design notes](../design/vertical-datums.md) if that matters to you.

More detailed heights, your own elevation files and web terrain services,
a height readout and profiles come later (roadmap Phase 7d).
