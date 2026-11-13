// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * Uniform blocks shared by the globe's shaders (internal to core).
 */
#ifndef TS_GLOBE_GLOBEUNIFORMS_H
#define TS_GLOBE_GLOBEUNIFORMS_H

#include <array>

namespace ts::globe_detail
{

using Float4 = std::array<float, 4>;

/** Uniform block "Frame" (std140), shared by every shader. Binding 0. */
struct FrameBlock {
  /** Projection × view rotation: eye-relative ECEF offsets to clip space. */
  std::array<float, 16> viewProjection{};
  /** Eye position, high floats (xyz). */
  Float4 eyeHigh{};
  /** Eye position, low floats (xyz). */
  Float4 eyeLow{};
  /** Viewport width, height (device px), 1/width, 1/height. */
  Float4 viewport{};
  /**
   * 1 / log2(far + 1), device pixel ratio, tan(vertical fov / 2), and the
   * factor overlays' distances are multiplied by for their depth (0.9;
   * 0.99 with terrain; see tsOverlayDepth()).
   */
  Float4 depthParams{};
  /**
   * Morph factor (0 globe, 1 flat map), metres on the map's plane per map
   * metre, the projection's index for tsProject(), unused.
   */
  Float4 morph{};
  /** Direction of map east on the plane (xyz), ECEF. */
  Float4 planeEast{};
  /** Direction of map north on the plane (xyz), ECEF. */
  Float4 planeNorth{};
  /** The centre of the map (the camera's target) relative to the eye. */
  Float4 planeOrigin{};
  /**
   * Terrain: how much of it is drawn (0 when off, 1 on the globe, falling
   * to 0 as the view becomes the flat map), the vertical exaggeration,
   * the strength of the map's hillshade (0 for none; it fades in with the
   * morph), and the hillshade's slope factor. See docs/design/terrain.md.
   */
  Float4 terrain{};
};

/** Uniform block "Globe" (std140). Binding 1. */
struct GlobeBlock {
  /** View direction (xyz), unused (w). */
  Float4 camForward{};
  /** Screen right (xyz), tan(horizontal fov / 2) (w). */
  Float4 camRight{};
  /** Screen up (xyz), tan(vertical fov / 2) (w). */
  Float4 camUp{};
  /** Eye / (a, a, b) (xyz); |that|² − 1 computed in double (w). */
  Float4 eyeScaled{};
  /** a, a / b, 1 / a², 1 / b². */
  Float4 ellipsoid{};
  /** Eye in metres (xyz; float, only for normals). */
  Float4 eyeMetres{};
  /** Unit direction towards the light (xyz). */
  Float4 lightDir{};
  /** Surface colour (rgb). */
  Float4 surfaceColor{};
  /** Unused (the graticule has its own pass and block). */
  Float4 gridColor{};
  /** Unused (the graticule has its own pass and block). */
  Float4 gridParams{};
};

static_assert(sizeof(FrameBlock) == 208, "std140 layout of Frame");
static_assert(sizeof(GlobeBlock) == 160, "std140 layout of Globe");

/** Binding of the "Frame" block. */
inline constexpr int frameBinding = 0;
/** Binding of the "Globe" block. */
inline constexpr int globeBinding = 1;
/** Binding of the tile renderer's per-tile "Tile" block. */
inline constexpr int tileBinding = 2;
/** Binding of the vector renderer's per-draw "Vector" block. */
inline constexpr int vectorBinding = 4;
/** Binding of the graticule pass's "Graticule" block. */
inline constexpr int graticuleBinding = 5;

inline const char* const frameBlockGlsl = R"(
layout(std140) uniform Frame {
  mat4 viewProjection;
  vec4 eyeHigh;
  vec4 eyeLow;
  vec4 viewport;
  vec4 depthParams;
  vec4 morph;
  vec4 planeEast;
  vec4 planeNorth;
  vec4 planeOrigin;
  vec4 terrain;
};

// The height a vertex is drawn at above the ellipsoid: its sea level plus
// its height above sea level (the sea floor flattened) exaggerated, less a
// skirt's drop; none when terrain is off. t: height above the ellipsoid,
// sea level, drop (TileVertex::terrain).
float tsTerrainHeight(vec3 t)
{
  return terrain.x * (t.y + (max(t.x - t.y, 0.0) - t.z) * terrain.y);
}

// The normal of the terrain at a point whose ellipsoid normal is up, from
// its slopes towards east and north (metres per metre).
vec3 tsTerrainNormal(vec3 up, vec2 slope)
{
  vec3 east = vec3(-up.y, up.x, 0.0);
  float length2 = dot(east, east);
  if (terrain.x <= 0.0 || length2 < 1e-12) {
    return up;
  }
  east *= inversesqrt(length2);
  vec3 north = cross(up, east);
  float k = terrain.x * terrain.y;
  return normalize(up - k * (slope.x * east + slope.y * north));
}
)";

/**
 * tsMorph(): a vertex's position relative to the eye, between its place
 * on the globe (@c relative) and its place on the flat map. The map
 * position comes from the vertex's offset in longitude and latitude from
 * a reference point (a tile's centre) through the projection's GLSL, and
 * the reference point's own place on the plane (@c mapOrigin, computed in
 * double). Goes after frameBlockGlsl and projectionGlsl(). See
 * docs/design/morph.md.
 */
inline const char* const morphGlsl = R"(
vec3 tsMorph(vec3 relative, vec2 dLonLatRad, vec4 mapOrigin, vec4 r0, vec4 r1)
{
  if (morph.x <= 0.0) {
    return relative;
  }
  vec2 plane = mapOrigin.xy
             + morph.y * tsProject(int(morph.z), dLonLatRad, r0, r1);
  vec3 onMap = planeOrigin.xyz + planeEast.xyz * plane.x
             + planeNorth.xyz * plane.y;
  return mix(relative, onMap, morph.x);
}
)";

/**
 * tsOverlayDepth(): the depth of things drawn on the surface while it is
 * a map or morphing, or carries terrain. They are tested against the
 * tiles' depth so that the unrolling globe, or a mountain, hides what is
 * behind it, but brought nearer by a fraction of their distance
 * (FrameBlock::depthParams w: a tenth, or a hundredth with terrain, where
 * a tenth would let them show through mountains) and 10 m, because their
 * triangles differ from the tiles'. Goes after frameBlockGlsl.
 */
inline const char* const overlayDepthGlsl = R"(
float tsOverlayDepth(float w)
{
  return clamp(log2(1.0 + max(depthParams.w * w - 10.0, 0.0)) * depthParams.x,
               0.0, 1.0);
}
)";

/** The graticule pass's block; binding graticuleBinding. */
inline const char* const graticuleBlockGlsl = R"(
layout(std140) uniform Graticule {
  vec4 graticuleColor;  // premultiplied
  vec4 graticuleParams; // line width (device px), opacity
};
)";

/** Grid lines for the graticule, from a coordinate in degrees. */
inline const char* const gridLinesGlsl = R"(
// Coverage of grid lines every spacingDeg at coordinate coordDeg, faded out
// where the lines get closer than about minGapPx pixels.
float gridLines(float coordDeg, float spacingDeg, float widthPx, float minGapPx)
{
  float cells = coordDeg / spacingDeg;
  float distance = abs(cells - floor(cells + 0.5));
  float cellsPerPx = max(fwidth(cells), 1e-9);
  float halfWidth = 0.5 * widthPx * cellsPerPx;
  float line = 1.0 - smoothstep(halfWidth - 0.5 * cellsPerPx,
                                halfWidth + 0.5 * cellsPerPx, distance);
  return line * (1.0 - smoothstep(0.5 / minGapPx, 1.0 / minGapPx, cellsPerPx));
}
)";

inline const char* const globeBlockGlsl = R"(
layout(std140) uniform Globe {
  vec4 camForward;
  vec4 camRight;
  vec4 camUp;
  vec4 eyeScaled;
  vec4 ellipsoid;
  vec4 eyeMetres;
  vec4 lightDir;
  vec4 surfaceColor;
  vec4 gridColor;
  vec4 gridParams;
};
)";

} // namespace ts::globe_detail

#endif // TS_GLOBE_GLOBEUNIFORMS_H
