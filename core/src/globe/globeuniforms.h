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
  /** 1 / log2(far + 1), device pixel ratio, tan(vertical fov / 2), unused. */
  Float4 depthParams{};
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
  /** Graticule colour (rgb). */
  Float4 gridColor{};
  /** Line width (device px), graticule opacity, unused, unused. */
  Float4 gridParams{};
};

static_assert(sizeof(FrameBlock) == 128, "std140 layout of Frame");
static_assert(sizeof(GlobeBlock) == 160, "std140 layout of Globe");

/** Binding of the "Frame" block. */
inline constexpr int frameBinding = 0;
/** Binding of the "Globe" block. */
inline constexpr int globeBinding = 1;
/** Binding of the tile renderer's per-tile "Tile" block. */
inline constexpr int tileBinding = 2;

inline const char* const frameBlockGlsl = R"(
layout(std140) uniform Frame {
  mat4 viewProjection;
  vec4 eyeHigh;
  vec4 eyeLow;
  vec4 viewport;
  vec4 depthParams;
};
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
