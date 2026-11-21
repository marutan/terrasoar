// SPDX-License-Identifier: GPL-2.0-or-later
/*
  Terra Soar - A virtual globe

  Copyright (C) 2026 Peter Howkins

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include "ts/globe/globerenderer.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

#include "ts/camera/camera.h"
#include "ts/geo/geodetic.h"
#include "ts/geo/wgs84.h"
#include "ts/globe/tilerenderer.h"
#include "ts/maths/angles.h"
#include "ts/maths/mat4.h"
#include "ts/maths/splitdouble.h"
#include "ts/projection/projection.h"

#include "globeuniforms.h"

namespace ts
{

namespace
{

using globe_detail::Float4;
using globe_detail::frameBinding;
using globe_detail::FrameBlock;
using globe_detail::frameBlockGlsl;
using globe_detail::globeBinding;
using globe_detail::GlobeBlock;
using globe_detail::globeBlockGlsl;
using globe_detail::graticuleBinding;
using globe_detail::graticuleBlockGlsl;
using globe_detail::gridLinesGlsl;

/** One marker instance in the vertex buffer. */
struct MarkerInstance {
  /** High xyz, low xyz, radius in logical px. */
  std::array<float, 7> values{};
  /** RGBA, read as normalised floats. */
  std::array<std::uint8_t, 4> rgba{};
  /**
   * The heading's direction (xyz, ECEF, unit; zero: up the screen), and
   * the shape (MarkerShape).
   */
  std::array<float, 4> heading{};
};

static_assert(sizeof(MarkerInstance) == 48, "marker instance stride");

/** One segment of a space line in the vertex buffer. */
struct LineInstance {
  /** Start: high xyz, low xyz. */
  std::array<float, 6> a{};
  /** End: high xyz, low xyz. */
  std::array<float, 6> b{};
  /** Along at the start and end, width in logical px, dashed (0 or 1). */
  std::array<float, 4> params{};
  /** Colour at the start, RGBA, read as normalised floats. */
  std::array<std::uint8_t, 4> rgbaA{};
  /** Colour at the end. */
  std::array<std::uint8_t, 4> rgbaB{};
  /**
   * Timed lines: times at the start and end (seconds after the lines'
   * time origin), and how far back they are shown (negative: untimed).
   */
  std::array<float, 4> times{0.0F, 0.0F, -1.0F, 0.0F};
};

static_assert(sizeof(LineInstance) == 88, "line instance stride");

// A triangle covering the viewport; each pixel gets its view ray.
const char* const globeVertexGlsl = R"(
out vec3 vRay;
void main()
{
  vec2 ndc = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
  vRay = camForward.xyz + ndc.x * camRight.w * camRight.xyz
       + ndc.y * camUp.w * camUp.xyz;
  gl_Position = vec4(ndc, 0.0, 1.0);
}
)";

// The ray through each pixel against the ellipsoid, and the graticule's
// lines; shared by the surface and graticule passes.
const char* const traceGlsl = R"(
in vec3 vRay;

struct SurfaceHit {
  vec3 d;         // unit view ray
  vec3 n;         // surface normal
  float tM;       // distance along d, metres
  float coverage; // of this pixel by the globe (edge antialiasing)
  float latDeg;
  float lonDeg;
};

SurfaceHit traceSurface()
{
  SurfaceHit hit;
  vec3 d = normalize(vRay);
  // Ray against the unit sphere in the frame scaled by (1/a, 1/a, 1/b):
  // |e + tau D|^2 = 1 with D = d (1, 1, a/b) and t = a tau metres.
  vec3 D = d * vec3(1.0, 1.0, ellipsoid.y);
  float qa = dot(D, D);
  float qb = dot(eyeScaled.xyz, D);
  float qc = eyeScaled.w;
  float disc = qb * qb - qa * qc;
  float root = sqrt(max(disc, 0.0));
  float q = -(qb + (qb >= 0.0 ? root : -root));
  float tau0 = q / qa;
  float tau1 = q != 0.0 ? qc / q : tau0;
  float tauNear = min(tau0, tau1);
  float tau = tauNear >= 0.0 ? tauNear : max(tau0, tau1);
  bool isHit = tau >= 0.0;
  // Off the globe, continue from the ray's closest approach so the
  // derivatives below stay smooth across the edge.
  if (!isHit || disc < 0.0) {
    tau = max(-qb / qa, 0.0);
  }
  // Edge antialiasing: disc changes linearly across the silhouette.
  hit.coverage = isHit ? clamp(0.5 + disc / max(fwidth(disc), 1e-30), 0.0, 1.0)
                       : 0.0;
  hit.d = d;
  hit.tM = tau * ellipsoid.x;
  vec3 p = eyeMetres.xyz + hit.tM * d;
  hit.n = normalize(p * vec3(ellipsoid.z, ellipsoid.z, ellipsoid.w));
  // The normal's latitude is the geodetic latitude.
  hit.latDeg = degrees(asin(clamp(hit.n.z, -1.0, 1.0)));
  hit.lonDeg = degrees(atan(hit.n.y, hit.n.x));
  return hit;
}

// Longitude lines; longitude jumps by 360 at the antimeridian, so take
// whichever of (-180, 180] and [0, 360) is smooth here.
float lonLines(float lonDeg, float spacingDeg, float widthPx, float minGapPx)
{
  float wrapped = mod(lonDeg + 360.0, 360.0);
  float a = gridLines(lonDeg, spacingDeg, widthPx, minGapPx);
  float b = gridLines(wrapped, spacingDeg, widthPx, minGapPx);
  return fwidth(lonDeg) <= fwidth(wrapped) ? a : b;
}

// The graticule's coverage: 10° lines, fainter 1° lines, the equator and
// prime meridian a little wider.
float graticule(SurfaceHit hit, float widthPx)
{
  float grid = 0.7 * max(gridLines(hit.latDeg, 10.0, widthPx, 4.0),
                         lonLines(hit.lonDeg, 10.0, widthPx, 4.0));
  grid = max(grid, 0.35 * max(gridLines(hit.latDeg, 1.0, widthPx, 12.0),
                              lonLines(hit.lonDeg, 1.0, widthPx, 12.0)));
  float axes = max(gridLines(hit.latDeg, 180.0, 1.5 * widthPx, 4.0),
                   lonLines(hit.lonDeg, 360.0, 1.5 * widthPx, 4.0));
  return max(grid, axes);
}
)";

const char* const globeFragmentGlsl = R"(
out vec4 fragColor;
void main()
{
  SurfaceHit hit = traceSurface();
  if (hit.coverage <= 0.0) {
    discard;
  }
  float diffuse = max(dot(hit.n, lightDir.xyz), 0.0);
  float facing = clamp(dot(hit.n, -hit.d), 0.0, 1.0);
  vec3 color = surfaceColor.rgb * (0.30 + 0.70 * diffuse)
             * (0.85 + 0.15 * facing);
  fragColor = vec4(color * hit.coverage, hit.coverage);
  // Farther than the surface: a tile's flat triangles lie a little below
  // the curved surface and must still pass the depth test. Along the ray
  // that gap grows as the ray meets the surface at a shallower angle
  // (sag / cos of the angle from the vertical), which is where the tiles
  // looking towards the horizon would otherwise lose to the ellipsoid.
  // Not beyond 100 times the sag at the limb, where it is unbounded.
  float sagAlongRayM = camForward.w / max(facing, 0.01);
  float w = (1.01 * hit.tM + sagAlongRayM) * dot(hit.d, camForward.xyz);
  gl_FragDepth = clamp(log2(1.0 + max(w, 0.0)) * depthParams.x, 0.0, 1.0);
}
)";

// The graticule alone, over whatever is drawn already (no depth test).
const char* const graticuleFragmentGlsl = R"(
out vec4 fragColor;
void main()
{
  SurfaceHit hit = traceSurface();
  float amount = graticule(hit, graticuleParams.x) * graticuleParams.y
               * hit.coverage;
  if (amount <= 0.0) {
    discard;
  }
  fragColor = graticuleColor * amount;
}
)";

/** Uniform block "Graticule" (std140). */
struct GraticuleBlock {
  Float4 color{};
  Float4 params{};
};

static_assert(sizeof(GraticuleBlock) == 32, "std140 layout of Graticule");

const char* const markerVertexGlsl = R"(
layout(location = 0) in vec2 corner;
layout(location = 1) in vec3 positionHigh;
layout(location = 2) in vec3 positionLow;
layout(location = 3) in float radiusPx;
layout(location = 4) in vec4 color;
layout(location = 5) in vec4 heading;
out vec2 vOffsetPx;
out float vRadiusPx;
out vec4 vColor;
out float vDepthW;
flat out float vShape;
void main()
{
  // Relative to the eye: the high parts cancel exactly near the eye.
  vec3 offset = (positionHigh - eyeHigh.xyz) + (positionLow - eyeLow.xyz);
  vec4 clip = viewProjection * vec4(offset, 1.0);
  float radius = radiusPx * depthParams.y;
  float extent = radius + 1.0;
  // Aircraft turn to their heading on the screen: towards a point a
  // little along it (in pixels: y up the screen).
  vec2 up = vec2(0.0, 1.0);
  if (dot(heading.xyz, heading.xyz) > 0.0) {
    vec4 ahead = viewProjection
               * vec4(offset + heading.xyz * max(0.001 * clip.w, 1.0), 1.0);
    vec2 d = (ahead.xy / ahead.w - clip.xy / clip.w) * viewport.xy;
    if (ahead.w > 0.0 && dot(d, d) > 1e-12) {
      up = normalize(d);
    }
  }
  vec2 right = vec2(up.y, -up.x);
  vec2 offsetPx = corner * extent;
  vec2 screenPx = right * offsetPx.x + up * offsetPx.y;
  clip.xy += screenPx * 2.0 * viewport.zw * clip.w;
  vOffsetPx = offsetPx;
  vRadiusPx = radius;
  vColor = color;
  vShape = heading.w;
  // A marker is a flat disc facing the viewer at its point's depth; the
  // ground around the point can be nearer by up to a few times the disc's
  // size in metres (more at grazing angles). Bring the depth that much
  // nearer so the ground doesn't cut the disc.
  float metresPerPx = 2.0 * depthParams.z * viewport.w * clip.w;
  vDepthW = max(clip.w - 4.0 * extent * metresPerPx, 0.5 * clip.w);
  // The depth is written in the fragment shader; z = 0 keeps markers in
  // space beyond the far plane (satellites past the Earth's edge) from
  // being clipped. Those behind the Earth get the far depth and lose to it.
  gl_Position = vec4(clip.xy, 0.0, clip.w);
}
)";

const char* const markerFragmentGlsl = R"(
in vec2 vOffsetPx;
in float vRadiusPx;
in vec4 vColor;
in float vDepthW;
flat in float vShape;
out vec4 fragColor;

// An aircraft from above, nose up, in units of its half length: fuselage,
// swept wings and tailplane.
float aircraft(vec2 q)
{
  float x = abs(q.x);
  bool fuselage = x <= 0.11 && q.y >= -0.86
                && (q.y <= 0.78 || length(q - vec2(0.0, 0.78)) <= 0.11);
  bool wings = x <= 0.95 && q.y <= 0.24 - 0.36 * x
             && q.y >= 0.02 - 0.26 * x;
  bool tail = x <= 0.36 && q.y <= -0.56 - 0.3 * x
            && q.y >= -0.80 - 0.12 * x;
  return (fuselage || wings || tail) ? 1.0 : 0.0;
}

// The part of the pixel at p (pixels) inside the aircraft, grown by grow
// pixels, from four samples.
float aircraftCoverage(vec2 p, float grow)
{
  float r = max(vRadiusPx, 1.0);
  float scale = r / (r + grow);
  float sum = 0.0;
  sum += aircraft((p + vec2(-0.25, -0.25)) / r * scale);
  sum += aircraft((p + vec2(0.25, -0.25)) / r * scale);
  sum += aircraft((p + vec2(-0.25, 0.25)) / r * scale);
  sum += aircraft((p + vec2(0.25, 0.25)) / r * scale);
  return 0.25 * sum;
}

void main()
{
  float alpha;
  float fill;
  if (vShape > 0.5) {
    fill = aircraftCoverage(vOffsetPx, 0.0);
    alpha = max(fill, aircraftCoverage(vOffsetPx, 1.2)) * vColor.a;
  } else {
    float r = length(vOffsetPx);
    alpha = clamp(vRadiusPx + 0.5 - r, 0.0, 1.0) * vColor.a;
    fill = clamp(vRadiusPx - 0.5 - r, 0.0, 1.0);
  }
  if (alpha <= 0.0) {
    discard;
  }
  // A dark rim one pixel wide keeps the marker visible on any background.
  vec3 rgb = vColor.rgb * fill;
  fragColor = vec4(rgb * alpha, alpha);
  gl_FragDepth = clamp(log2(1.0 + vDepthW) * depthParams.x, 0.0, 1.0);
}
)";

// A segment as a quad in screen space, a capsule in the fragment shader
// (round ends join the segments of a line without gaps), at its points'
// depth brought nearer by a few pixels' worth, as markers are, so lines on
// the ground aren't cut by it.
const char* const lineVertexGlsl = R"(
layout(location = 0) in vec2 corner;
layout(location = 1) in vec3 aHigh;
layout(location = 2) in vec3 aLow;
layout(location = 3) in vec3 bHigh;
layout(location = 4) in vec3 bLow;
layout(location = 5) in vec4 params;
layout(location = 6) in vec4 colorA;
layout(location = 7) in vec4 colorB;
layout(location = 8) in vec4 times;
out vec4 vColor;
out float vAlong;
out float vDepthW;
flat out vec2 vA;
flat out vec2 vB;
flat out float vHalfWidth;
flat out float vDashed;
flat out vec3 vTimes;

vec2 toScreen(vec4 clip)
{
  return (clip.xy / clip.w * 0.5 + 0.5) * viewport.xy;
}

void main()
{
  vec3 relA = (aHigh - eyeHigh.xyz) + (aLow - eyeLow.xyz);
  vec3 relB = (bHigh - eyeHigh.xyz) + (bLow - eyeLow.xyz);
  vec4 clipA = viewProjection * vec4(relA, 1.0);
  vec4 clipB = viewProjection * vec4(relB, 1.0);
  float nearW = 1.0;
  // Timed: segments wholly after the time, or before the span, aren't
  // drawn (morph.w is the time, after the lines' origin).
  bool hidden = times.z >= 0.0
              && (times.x > morph.w || times.y < morph.w - times.z);
  if ((clipA.w < nearW && clipB.w < nearW) || hidden) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
    return;
  }
  float alongA = params.x;
  float alongB = params.y;
  if (clipA.w < nearW) {
    float f = (nearW - clipA.w) / (clipB.w - clipA.w);
    clipA = mix(clipA, clipB, f);
    alongA = mix(alongA, alongB, f);
  } else if (clipB.w < nearW) {
    float f = (nearW - clipB.w) / (clipA.w - clipB.w);
    clipB = mix(clipB, clipA, f);
    alongB = mix(alongB, alongA, f);
  }
  vec2 sa = toScreen(clipA);
  vec2 sb = toScreen(clipB);
  vec2 d = sb - sa;
  float len = length(d);
  vec2 dir = len > 1e-6 ? d / len : vec2(1.0, 0.0);
  vec2 normal = vec2(-dir.y, dir.x);
  float halfWidth = 0.5 * params.z * depthParams.y;
  float extent = halfWidth + 1.0;
  bool atB = corner.x > 0.0;
  vec2 screen = (atB ? sb + dir * extent : sa - dir * extent)
              + normal * corner.y * extent;
  vec4 clip = atB ? clipB : clipA;
  float metresPerPx = 2.0 * depthParams.z * viewport.w * clip.w;
  vDepthW = max(clip.w - 4.0 * extent * metresPerPx, 0.5 * clip.w);
  gl_Position = vec4((screen / viewport.xy * 2.0 - 1.0) * clip.w,
                     0.0, clip.w);
  vColor = atB ? colorB : colorA;
  vAlong = atB ? alongB : alongA;
  vA = sa;
  vB = sb;
  vHalfWidth = halfWidth;
  vDashed = params.w;
  vTimes = times.xyz;
}
)";

const char* const lineFragmentGlsl = R"(
in vec4 vColor;
in float vAlong;
in float vDepthW;
flat in vec2 vA;
flat in vec2 vB;
flat in float vHalfWidth;
flat in float vDashed;
flat in vec3 vTimes;
out vec4 fragColor;
void main()
{
  if (vDashed > 0.5 && fract(vAlong) > 0.5) {
    discard;
  }
  vec2 p = gl_FragCoord.xy;
  vec2 ab = vB - vA;
  float along = dot(p - vA, ab) / max(dot(ab, ab), 1e-12);
  float t = clamp(along, 0.0, 1.0);
  if (vTimes.z >= 0.0) {
    // A trail: cut where the time, and the span before it, fall. Past
    // the ends (the round caps) the ends' times hold.
    float time = mix(vTimes.x, vTimes.y, along);
    if (time > morph.w || time < morph.w - vTimes.z) {
      discard;
    }
  }
  float distance = length(p - (vA + t * ab));
  float coverage = clamp(vHalfWidth + 0.5 - distance, 0.0, 1.0) * vColor.a;
  if (coverage <= 0.0) {
    discard;
  }
  fragColor = vec4(vColor.rgb * coverage, coverage);
  gl_FragDepth = clamp(log2(1.0 + vDepthW) * depthParams.x, 0.0, 1.0);
}
)";

Float4 float4(const Vec3& v, double w)
{
  return Float4{static_cast<float>(v.x), static_cast<float>(v.y),
                static_cast<float>(v.z), static_cast<float>(w)};
}

Float4 color4(std::array<float, 3> rgb, float a)
{
  return Float4{rgb[0], rgb[1], rgb[2], a};
}

std::array<float, 16> floatMatrix(const Mat4& m)
{
  std::array<float, 16> result{};
  std::size_t index = 0;
  for (int32_t column = 0; column < 4; ++column) {
    for (int32_t row = 0; row < 4; ++row) {
      result.at(index++) = static_cast<float>(m.at(row, column));
    }
  }
  return result;
}

double smoothStep(double edge0, double edge1, double x)
{
  const double t = std::clamp((x - edge0) / (edge1 - edge0), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

std::size_t alignUp(std::size_t value, std::size_t alignment)
{
  return (value + alignment - 1) / alignment * alignment;
}

/** The graticule is computed from float positions (about 0.5 m steps), so
 * it fades out below a few kilometres, where that would show. */
constexpr double graticuleFadeStartM = 2000.0;
constexpr double graticuleFadeEndM = 500.0;

} // namespace

struct GlobeRenderer::Resources {
  std::unique_ptr<ShaderProgram> globeShader;
  std::unique_ptr<ShaderProgram> graticuleShader;
  std::unique_ptr<Pipeline> graticulePipeline;
  std::unique_ptr<GpuBuffer> graticuleUniforms;
  std::size_t graticuleStride = 0;
  std::size_t graticuleCapacity = 0;
  std::unique_ptr<ShaderProgram> markerShader;
  std::unique_ptr<Pipeline> globePipeline;
  std::unique_ptr<Pipeline> markerPipeline;
  std::unique_ptr<GpuBuffer> uniforms;
  std::unique_ptr<GpuBuffer> markerCorners;
  std::unique_ptr<GpuBuffer> markerInstances;
  std::size_t globeOffset = 0;
  std::size_t markerCapacity = 0;
  int32_t markerCount = 0;
  std::unique_ptr<ShaderProgram> lineShader;
  std::unique_ptr<Pipeline> linePipeline;
  /** One buffer of segments for each group of space lines. */
  struct LineBuffer {
    std::unique_ptr<GpuBuffer> instances;
    std::size_t capacity = 0;
    int32_t count = 0;
  };
  std::array<LineBuffer, GlobeRenderer::spaceLineGroups> lines;
};

GlobeRenderer::GlobeRenderer(RenderDevice& device)
    : GlobeRenderer(device, TileRendererSettings())
{
}

GlobeRenderer::GlobeRenderer(RenderDevice& device,
                             const TileRendererSettings& tileSettings)
    : m_device(device),
      m_tileSettings(std::make_unique<TileRendererSettings>(tileSettings))
{
}

GlobeRenderer::~GlobeRenderer() = default;

bool GlobeRenderer::isInitialised() const
{
  return m_resources != nullptr;
}

bool GlobeRenderer::initialise(QString* errorString)
{
  std::unique_ptr<Resources> r = std::make_unique<Resources>();
  const std::vector<UniformBlockBinding> blocks = {
      {QByteArrayLiteral("Frame"), frameBinding},
      {QByteArrayLiteral("Globe"), globeBinding}};

  ShaderDesc globe;
  globe.label = QStringLiteral("globe");
  globe.vertexSource =
      QByteArray(frameBlockGlsl) + globeBlockGlsl + globeVertexGlsl;
  globe.fragmentSource = QByteArray(frameBlockGlsl) + globeBlockGlsl +
                         gridLinesGlsl + traceGlsl + globeFragmentGlsl;
  globe.uniformBlocks = blocks;
  r->globeShader = m_device.createShaderProgram(globe, errorString);

  ShaderDesc graticule;
  graticule.label = QStringLiteral("graticule");
  graticule.vertexSource = globe.vertexSource;
  graticule.fragmentSource = QByteArray(frameBlockGlsl) + globeBlockGlsl +
                             graticuleBlockGlsl + gridLinesGlsl + traceGlsl +
                             graticuleFragmentGlsl;
  graticule.uniformBlocks = {
      {QByteArrayLiteral("Frame"), frameBinding},
      {QByteArrayLiteral("Globe"), globeBinding},
      {QByteArrayLiteral("Graticule"), graticuleBinding}};
  r->graticuleShader = m_device.createShaderProgram(graticule, errorString);

  ShaderDesc marker;
  marker.label = QStringLiteral("marker");
  marker.vertexSource = QByteArray(frameBlockGlsl) + markerVertexGlsl;
  marker.fragmentSource = QByteArray(frameBlockGlsl) + markerFragmentGlsl;
  marker.uniformBlocks = {{QByteArrayLiteral("Frame"), frameBinding}};
  r->markerShader = m_device.createShaderProgram(marker, errorString);
  ShaderDesc line;
  line.label = QStringLiteral("space line");
  line.vertexSource = QByteArray(frameBlockGlsl) + lineVertexGlsl;
  line.fragmentSource = QByteArray(frameBlockGlsl) + lineFragmentGlsl;
  line.uniformBlocks = {{QByteArrayLiteral("Frame"), frameBinding}};
  r->lineShader = m_device.createShaderProgram(line, errorString);
  if (!r->globeShader || !r->graticuleShader || !r->markerShader ||
      !r->lineShader) {
    return false;
  }

  PipelineDesc graticulePipeline;
  graticulePipeline.shader = r->graticuleShader.get();
  graticulePipeline.blend = BlendMode::PremultipliedAlpha;
  r->graticulePipeline = m_device.createPipeline(graticulePipeline);
  r->graticuleStride =
      alignUp(sizeof(GraticuleBlock), m_device.uniformBufferAlignment());

  PipelineDesc globePipeline;
  globePipeline.shader = r->globeShader.get();
  globePipeline.topology = PrimitiveTopology::Triangles;
  globePipeline.depthTest = true;
  globePipeline.depthWrite = true;
  globePipeline.depthCompare = CompareOp::Always;
  globePipeline.blend = BlendMode::PremultipliedAlpha;
  r->globePipeline = m_device.createPipeline(globePipeline);

  PipelineDesc markerPipeline;
  markerPipeline.shader = r->markerShader.get();
  markerPipeline.topology = PrimitiveTopology::TriangleStrip;
  markerPipeline.bufferLayouts = {{8, false}, {sizeof(MarkerInstance), true}};
  markerPipeline.attributes = {
      {0, VertexFormat::Float2, 0, 0},    {1, VertexFormat::Float3, 1, 0},
      {2, VertexFormat::Float3, 1, 12},   {3, VertexFormat::Float, 1, 24},
      {4, VertexFormat::UNorm8x4, 1, 28}, {5, VertexFormat::Float4, 1, 32}};
  markerPipeline.depthTest = true;
  markerPipeline.depthWrite = false;
  markerPipeline.depthCompare = CompareOp::LessOrEqual;
  markerPipeline.blend = BlendMode::PremultipliedAlpha;
  r->markerPipeline = m_device.createPipeline(markerPipeline);

  PipelineDesc linePipeline = markerPipeline;
  linePipeline.shader = r->lineShader.get();
  linePipeline.bufferLayouts = {{8, false}, {sizeof(LineInstance), true}};
  linePipeline.attributes = {
      {0, VertexFormat::Float2, 0, 0},    {1, VertexFormat::Float3, 1, 0},
      {2, VertexFormat::Float3, 1, 12},   {3, VertexFormat::Float3, 1, 24},
      {4, VertexFormat::Float3, 1, 36},   {5, VertexFormat::Float4, 1, 48},
      {6, VertexFormat::UNorm8x4, 1, 64}, {7, VertexFormat::UNorm8x4, 1, 68},
      {8, VertexFormat::Float4, 1, 72}};
  r->linePipeline = m_device.createPipeline(linePipeline);

  const std::array<float, 8> corners = {-1.0F, -1.0F, 1.0F, -1.0F,
                                        -1.0F, 1.0F,  1.0F, 1.0F};
  r->markerCorners = m_device.createBuffer(
      BufferType::Vertex, BufferUsage::Static, sizeof(corners), corners.data());
  r->globeOffset =
      alignUp(sizeof(FrameBlock), m_device.uniformBufferAlignment());
  r->uniforms =
      m_device.createBuffer(BufferType::Uniform, BufferUsage::Dynamic,
                            r->globeOffset + sizeof(GlobeBlock), nullptr);
  if (!r->globePipeline || !r->graticulePipeline || !r->markerPipeline ||
      !r->linePipeline || !r->markerCorners || !r->uniforms) {
    if (errorString) {
      *errorString = QStringLiteral("globe: could not create GPU resources");
    }
    return false;
  }
  std::unique_ptr<TileRenderer> tiles =
      std::make_unique<TileRenderer>(m_device, *m_tileSettings);
  if (!tiles->initialise(errorString)) {
    return false;
  }
  std::unique_ptr<VectorRenderer> vectors =
      std::make_unique<VectorRenderer>(m_device);
  if (!vectors->initialise(errorString)) {
    return false;
  }
  vectors->setLayers(m_vectorLayers);
  m_tiles = std::move(tiles);
  m_vectors = std::move(vectors);
  m_resources = std::move(r);
  m_markersDirty = true;
  for (SpaceLineGroup& group : m_spaceLineGroups) {
    group.dirty = true;
  }
  return true;
}

void GlobeRenderer::setStyle(const GlobeStyle& style)
{
  m_style = style;
}

void GlobeRenderer::setImageryProvider(
    std::shared_ptr<ImageryProvider> provider)
{
  if (m_tiles) {
    m_tiles->setImageryProvider(std::move(provider));
  }
}

void GlobeRenderer::setImageryLayers(
    const std::vector<ImageryLayerDraw>& layers,
    const std::vector<double>& groupAlphas)
{
  if (m_tiles) {
    m_tiles->setLayers(layers, groupAlphas);
  }
}

void GlobeRenderer::setGroupCompositing(bool enabled)
{
  if (m_tiles) {
    m_tiles->setGroupCompositing(enabled);
  }
}

void GlobeRenderer::setTilesReadyCallback(std::function<void()> callback)
{
  if (m_vectors) {
    m_vectors->setTilesReadyCallback(callback);
  }
  if (m_tiles) {
    m_tiles->setTilesReadyCallback(std::move(callback));
  }
}

bool GlobeRenderer::isBusy() const
{
  return (m_tiles && m_tiles->isBusy()) || (m_vectors && m_vectors->isBusy());
}

void GlobeRenderer::setVectorLayers(const std::vector<VectorLayerDraw>& layers)
{
  m_vectorLayers = layers;
  if (m_vectors) {
    m_vectors->setLayers(m_vectorLayers);
  }
}

VectorStatistics GlobeRenderer::vectorStatistics() const
{
  return m_vectors ? m_vectors->statistics() : VectorStatistics();
}

TileStatistics GlobeRenderer::tileStatistics() const
{
  return m_tiles ? m_tiles->statistics() : TileStatistics();
}

void GlobeRenderer::setMarkers(const std::vector<GlobeMarker>& markers)
{
  m_markers = markers;
  m_markersDirty = true;
}

void GlobeRenderer::setSpaceLines(std::vector<SpaceLine> lines, int32_t group)
{
  SpaceLineGroup& g = m_spaceLineGroups.at(
      static_cast<std::size_t>(std::clamp(group, 0, spaceLineGroups - 1)));
  g.lines = std::move(lines);
  g.dirty = true;
}

void GlobeRenderer::setSpaceLineTime(double timeS)
{
  m_spaceLineTimeS = timeS;
  // Float times stay precise to a few milliseconds within about a day of
  // their origin; farther, place them all again from a new one.
  if (std::fabs(timeS - m_spaceLineOriginS) > 1.0e5) {
    m_spaceLineOriginS = timeS;
    for (SpaceLineGroup& group : m_spaceLineGroups) {
      group.dirty = true;
    }
  }
}

int32_t GlobeRenderer::spaceSegmentsDrawn() const
{
  int32_t count = 0;
  if (m_resources) {
    for (const Resources::LineBuffer& buffer : m_resources->lines) {
      count += buffer.count;
    }
  }
  return count;
}

namespace
{

/** Where a line's point is drawn: on the globe, or moving to the map. */
struct PlacedPoint {
  Vec3 position;
  /** False where it is off the map. */
  bool shown = true;
  /** Its map position (morphing), offset from the centre, metres. */
  MapPoint map;
};

std::array<std::uint8_t, 4> withAlpha(std::array<std::uint8_t, 4> rgba,
                                      float alpha)
{
  rgba[3] = static_cast<std::uint8_t>(std::lround(
      std::clamp(static_cast<float>(rgba[3]) * alpha, 0.0F, 255.0F)));
  return rgba;
}

LineInstance lineInstance(const Vec3& a, const Vec3& b,
                          std::array<float, 4> params,
                          std::array<std::uint8_t, 4> rgbaA,
                          std::array<std::uint8_t, 4> rgbaB,
                          std::array<float, 4> times)
{
  const SplitVec3 sa = splitVec3(a);
  const SplitVec3 sb = splitVec3(b);
  LineInstance instance;
  instance.a = {sa.highX, sa.highY, sa.highZ, sa.lowX, sa.lowY, sa.lowZ};
  instance.b = {sb.highX, sb.highY, sb.highZ, sb.lowX, sb.lowY, sb.lowZ};
  instance.params = params;
  instance.rgbaA = rgbaA;
  instance.rgbaB = rgbaB;
  instance.times = times;
  return instance;
}

/** Places the points of @p line for @p camera (morphing to the map). */
std::vector<PlacedPoint> placeLine(const SpaceLine& line, const Camera& camera,
                                   const MapFrame& frame)
{
  std::vector<PlacedPoint> placed;
  placed.reserve(line.points.size());
  const double morph = camera.morph();
  for (const SpaceLinePoint& p : line.points) {
    PlacedPoint point;
    point.position = p.positionEcef;
    if (morph > 0.0) {
      point.shown = frame.projection->contains(p.latDeg, p.lonDeg);
      point.map = frame.offsetOf(p.latDeg, p.lonDeg);
      const Vec3 onGlobe = p.positionEcef - camera.eyeEcef();
      const Vec3 onMap = camera.mapPlanePoint(point.map);
      point.position =
          camera.eyeEcef() + onGlobe * (1.0 - morph) + onMap * morph;
    }
    placed.push_back(point);
  }
  return placed;
}

/**
 * Returns true if the segment from @p a to @p b crosses where the map
 * wraps around or jumps (its ends far apart on the map).
 */
bool breaksOnMap(const PlacedPoint& a, const PlacedPoint& b,
                 const MapFrame& frame)
{
  if (!a.shown || !b.shown) {
    return true;
  }
  const double dx = std::fabs(a.map.xM - b.map.xM);
  const double dy = std::fabs(a.map.yM - b.map.yM);
  if (frame.worldWidthM > 0.0) {
    return dx > 0.5 * frame.worldWidthM;
  }
  // Maps that don't repeat: a jump of a quarter of the map's size is an
  // edge (a polar map's rim, an interrupted map's lobes).
  const MapRect bounds = frame.projection->bounds();
  return dx > 0.25 * bounds.widthM() || dy > 0.25 * bounds.heightM();
}

/**
 * Where a segment crossing the map's seam meets it on each side: the
 * point at the seam's longitude, placed by each end's side.
 */
std::pair<PlacedPoint, PlacedPoint>
seamPoints(const SpaceLinePoint& a, const SpaceLinePoint& b,
           const Camera& camera, const MapFrame& frame, double& fraction)
{
  const double seam = normaliseLonDeg(frame.centreLonDeg + 180.0);
  double lonB = b.lonDeg;
  double delta = normaliseLonDeg(lonB - a.lonDeg);
  lonB = a.lonDeg + delta;
  double seamA = seam;
  while (seamA < std::min(a.lonDeg, lonB)) {
    seamA += 360.0;
  }
  while (seamA > std::max(a.lonDeg, lonB)) {
    seamA -= 360.0;
  }
  const double f =
      delta != 0.0 ? std::clamp((seamA - a.lonDeg) / delta, 0.0, 1.0) : 0.5;
  fraction = f;
  const double latDeg = a.latDeg + f * (b.latDeg - a.latDeg);
  const double nudge = delta > 0.0 ? 1e-7 : -1e-7;
  const Vec3 space =
      a.positionEcef + (b.positionEcef - a.positionEcef) * f - camera.eyeEcef();
  std::pair<PlacedPoint, PlacedPoint> ends;
  ends.first.map = frame.offsetOf(latDeg, seamA - nudge);
  ends.second.map = frame.offsetOf(latDeg, seamA + nudge);
  const double morph = camera.morph();
  ends.first.position = camera.eyeEcef() + space * (1.0 - morph) +
                        camera.mapPlanePoint(ends.first.map) * morph;
  ends.second.position = camera.eyeEcef() + space * (1.0 - morph) +
                         camera.mapPlanePoint(ends.second.map) * morph;
  return ends;
}

/** Appends @p line's segments for @p camera (broken where a map wraps). */
void appendLine(const SpaceLine& line, const Camera& camera,
                const MapFrame& frame, double timeOriginS,
                std::vector<LineInstance>& instances)
{
  const std::size_t n = line.points.size();
  if (n < 2) {
    return;
  }
  const bool morphed = camera.morph() > 0.0;
  const std::vector<PlacedPoint> placed = placeLine(line, camera, frame);
  const std::size_t segments = line.closed ? n : n - 1;
  for (std::size_t i = 0; i < segments; ++i) {
    const std::size_t j = (i + 1) % n;
    const SpaceLinePoint& a = line.points[i];
    const SpaceLinePoint& b = line.points[j];
    const std::array<float, 4> params{a.along, b.along, line.widthPx,
                                      line.dashed ? 1.0F : 0.0F};
    const std::array<std::uint8_t, 4> rgbaA =
        withAlpha(line.pointColours ? a.rgba : line.rgba, a.alpha);
    const std::array<std::uint8_t, 4> rgbaB =
        withAlpha(line.pointColours ? b.rgba : line.rgba, b.alpha);
    std::array<float, 4> times{0.0F, 0.0F, -1.0F, 0.0F};
    if (line.timed) {
      times = {static_cast<float>(a.timeS - timeOriginS),
               static_cast<float>(b.timeS - timeOriginS),
               static_cast<float>(std::min(line.shownS, 1.0e30)), 0.0F};
    }
    if (!morphed || !breaksOnMap(placed[i], placed[j], frame)) {
      instances.push_back(lineInstance(placed[i].position, placed[j].position,
                                       params, rgbaA, rgbaB, times));
      continue;
    }
    // Across the seam of a repeating map: to the edge on each side.
    if (placed[i].shown && placed[j].shown && frame.worldWidthM > 0.0) {
      double f = 0.5;
      const std::pair<PlacedPoint, PlacedPoint> seam =
          seamPoints(a, b, camera, frame, f);
      std::array<float, 4> first = times;
      std::array<float, 4> second = times;
      if (line.timed) {
        const float seamTime =
            static_cast<float>(a.timeS + (b.timeS - a.timeS) * f - timeOriginS);
        first[1] = seamTime;
        second[0] = seamTime;
      }
      instances.push_back(lineInstance(placed[i].position, seam.first.position,
                                       params, rgbaA, rgbaB, first));
      instances.push_back(lineInstance(seam.second.position, placed[j].position,
                                       params, rgbaA, rgbaB, second));
    }
  }
}

/** Places @p point (ECEF) for @p camera, morphing to the map. */
Vec3 placeOnMap(const Vec3& point, const Geodetic& where, const Camera& camera,
                const MapFrame& frame)
{
  const Vec3 onGlobe = point - camera.eyeEcef();
  const Vec3 onMap =
      camera.mapPlanePoint(frame.offsetOf(where.latDeg, where.lonDeg));
  return camera.eyeEcef() + onGlobe * (1.0 - camera.morph()) +
         onMap * camera.morph();
}

/**
 * Returns @p marker's instance for @p camera (on the map and during the
 * morph, placed there): its position, and an aircraft's heading as drawn.
 * Nothing if it is off a map that ends at its domain.
 */
std::optional<MarkerInstance> markerInstance(const GlobeMarker& marker,
                                             const Camera& camera,
                                             const MapFrame& frame)
{
  const bool morphed = camera.morph() > 0.0;
  const bool aircraft = marker.shape == MarkerShape::Aircraft;
  const bool turned = aircraft && std::isfinite(marker.headingDeg);
  Vec3 position = marker.positionEcef;
  Vec3 heading{};
  const Geodetic geodetic =
      morphed || turned ? ecefToGeodetic(marker.positionEcef) : Geodetic();
  if (turned) {
    const LocalFrame local = localFrame(geodetic.latDeg, geodetic.lonDeg);
    const double h = degToRad(marker.headingDeg);
    heading = local.northEcef * std::cos(h) + local.eastEcef * std::sin(h);
  }
  if (morphed) {
    if (!frame.projection->contains(geodetic.latDeg, geodetic.lonDeg)) {
      // Off a map that ends at its domain (it would sit on the edge).
      return std::nullopt;
    }
    position = placeOnMap(marker.positionEcef, geodetic, camera, frame);
    if (turned) {
      // The heading as it is drawn: towards a point a kilometre on.
      const Vec3 aheadEcef = marker.positionEcef + heading * 1000.0;
      const Vec3 d =
          placeOnMap(aheadEcef, ecefToGeodetic(aheadEcef), camera, frame) -
          position;
      heading = length(d) > 0.0 ? d * (1.0 / length(d)) : Vec3{};
    }
  }
  const SplitVec3 split = splitVec3(position);
  return MarkerInstance{
      {split.highX, split.highY, split.highZ, split.lowX, split.lowY,
       split.lowZ, static_cast<float>(marker.radiusPx)},
      marker.rgba,
      {static_cast<float>(heading.x), static_cast<float>(heading.y),
       static_cast<float>(heading.z), aircraft ? 1.0F : 0.0F}};
}

} // namespace

void GlobeRenderer::uploadSpaceLines(const Camera& camera)
{
  const bool morphed = camera.morph() > 0.0;
  const MapFrame frame = morphed ? camera.mapFrame() : MapFrame();
  // On the map and during the morph lines move with the view: placed
  // again when it moves, not while it is still (a trail playing back).
  const std::array<double, 6> view = {
      camera.eyeEcef().x,         camera.eyeEcef().y,
      camera.eyeEcef().z,         camera.morph(),
      camera.pose().targetLatDeg, camera.pose().targetLonDeg};
  const bool viewMoved =
      morphed &&
      (view != m_spaceLinesView || frame.projection != m_spaceLinesProjection);
  const bool everything = viewMoved || morphed != m_spaceLinesMorphed;
  m_spaceLinesMorphed = morphed;
  m_spaceLinesView = view;
  m_spaceLinesProjection = frame.projection;
  Resources& r = *m_resources;
  for (std::size_t g = 0; g < m_spaceLineGroups.size(); ++g) {
    SpaceLineGroup& group = m_spaceLineGroups.at(g);
    if (!group.dirty && !everything) {
      continue;
    }
    ++m_spaceLineUploads;
    std::vector<LineInstance> instances;
    for (const SpaceLine& line : group.lines) {
      if (!morphed || !line.globeOnly) {
        appendLine(line, camera, frame, m_spaceLineOriginS, instances);
      }
    }
    Resources::LineBuffer& buffer = r.lines.at(g);
    if (instances.size() > buffer.capacity) {
      buffer.capacity = std::max<std::size_t>(instances.size(), 64);
      buffer.instances = m_device.createBuffer(
          BufferType::Vertex, BufferUsage::Dynamic,
          buffer.capacity * sizeof(LineInstance), nullptr);
    }
    if (!instances.empty() && buffer.instances) {
      buffer.instances->update(0, instances.data(),
                               instances.size() * sizeof(LineInstance));
    }
    buffer.count = static_cast<int32_t>(instances.size());
    group.dirty = false;
  }
}

void GlobeRenderer::drawSpaceLines()
{
  const Resources& r = *m_resources;
  for (const Resources::LineBuffer& buffer : r.lines) {
    if (buffer.count > 0 && buffer.instances) {
      m_device.setPipeline(*r.linePipeline);
      m_device.setVertexBuffer(0, *r.markerCorners, 0);
      m_device.setVertexBuffer(1, *buffer.instances, 0);
      m_device.draw(4, 0, buffer.count);
    }
  }
}

void GlobeRenderer::uploadMarkers(const Camera& camera)
{
  // On the map and during the morph markers move with the camera, so
  // they are placed again every frame (they are few).
  const bool morphed = camera.morph() > 0.0;
  if (!m_markersDirty && !morphed && !m_markersMorphed) {
    return;
  }
  m_markersMorphed = morphed;
  Resources& r = *m_resources;
  std::vector<MarkerInstance> instances;
  instances.reserve(m_markers.size());
  const MapFrame frame = morphed ? camera.mapFrame() : MapFrame();
  for (const GlobeMarker& marker : m_markers) {
    const std::optional<MarkerInstance> instance =
        markerInstance(marker, camera, frame);
    if (instance) {
      instances.push_back(*instance);
    }
  }
  if (instances.size() > r.markerCapacity) {
    r.markerCapacity = std::max<std::size_t>(instances.size(), 16);
    r.markerInstances = m_device.createBuffer(
        BufferType::Vertex, BufferUsage::Dynamic,
        r.markerCapacity * sizeof(MarkerInstance), nullptr);
  }
  if (!instances.empty() && r.markerInstances) {
    r.markerInstances->update(0, instances.data(),
                              instances.size() * sizeof(MarkerInstance));
  }
  r.markerCount = static_cast<int32_t>(instances.size());
  m_markersDirty = false;
}

std::vector<std::size_t>
GlobeRenderer::writeGraticuleUniforms(double fade, double devicePixelRatio)
{
  Resources& r = *m_resources;
  // One Graticule block for the style's graticule (first), then one per
  // graticule layer; returns each layer's block (0 for other layers).
  std::vector<GraticuleBlock> graticules;
  graticules.push_back(GraticuleBlock{
      color4(m_style.graticuleRgb, 1.0F),
      {static_cast<float>(static_cast<double>(m_style.graticuleWidthPx) *
                          devicePixelRatio),
       static_cast<float>(m_style.graticule ? fade : 0.0), 0.0F, 0.0F}});
  std::vector<std::size_t> indices(m_vectorLayers.size(), 0);
  for (std::size_t i = 0; i < m_vectorLayers.size(); ++i) {
    const VectorLayerDraw& layer = m_vectorLayers[i];
    if (layer.kind != VectorLayerDraw::Kind::Graticule) {
      continue;
    }
    indices[i] = graticules.size();
    graticules.push_back(GraticuleBlock{
        premultipliedRgba(layer.style.outline, layer.alpha),
        {static_cast<float>(layer.style.outlineWidthPx * devicePixelRatio),
         static_cast<float>(fade), 0.0F, 0.0F}});
  }
  const std::size_t bytes = graticules.size() * r.graticuleStride;
  if (bytes > r.graticuleCapacity) {
    r.graticuleCapacity = bytes;
    r.graticuleUniforms =
        m_device.createBuffer(BufferType::Uniform, BufferUsage::Dynamic,
                              r.graticuleCapacity, nullptr);
  }
  if (r.graticuleUniforms) {
    for (std::size_t i = 0; i < graticules.size(); ++i) {
      r.graticuleUniforms->update(i * r.graticuleStride, &graticules[i],
                                  sizeof(GraticuleBlock));
    }
  }
  return indices;
}

void GlobeRenderer::drawGraticule(std::size_t index)
{
  const Resources& r = *m_resources;
  if (!r.graticuleUniforms) {
    return;
  }
  if (m_morph > 0.0 || terrainShown()) {
    // Not the ellipsoid: on the tiles, wherever they are now.
    m_device.setUniformBuffer(graticuleBinding, *r.graticuleUniforms,
                              index * r.graticuleStride,
                              sizeof(GraticuleBlock));
    m_tiles->drawGraticule();
    return;
  }
  m_device.setPipeline(*r.graticulePipeline);
  m_device.setUniformBuffer(graticuleBinding, *r.graticuleUniforms,
                            index * r.graticuleStride, sizeof(GraticuleBlock));
  m_device.draw(3, 0, 1);
}

void GlobeRenderer::setElevationProvider(
    std::shared_ptr<ElevationProvider> provider)
{
  if (m_vectors) {
    m_vectors->setElevationProvider(provider);
  }
  if (m_tiles) {
    m_tiles->setElevationProvider(std::move(provider));
  }
}

void GlobeRenderer::setTerrain(bool visible, double exaggeration)
{
  if (m_tiles) {
    m_tiles->setTerrain(visible, exaggeration);
  }
  if (m_vectors) {
    m_vectors->setTerrainHeightMargin(terrainShown() ? 9000.0 * exaggeration
                                                     : 0.0);
  }
}

void GlobeRenderer::setHillshade(double strength)
{
  m_hillshade = std::clamp(strength, 0.0, 1.0);
}

bool GlobeRenderer::terrainShown() const
{
  return m_tiles && m_tiles->terrainShown();
}

std::optional<double> GlobeRenderer::terrainHeightM(double latDeg,
                                                    double lonDeg)
{
  if (!m_tiles) {
    return 0.0;
  }
  return m_tiles->terrainHeightM(latDeg, lonDeg);
}

std::optional<GroundHeight> GlobeRenderer::groundHeight(double latDeg,
                                                        double lonDeg)
{
  return m_tiles ? m_tiles->groundHeight(latDeg, lonDeg)
                 : std::optional<GroundHeight>();
}

void GlobeRenderer::render(const Camera& camera, const PassDesc& pass,
                           double devicePixelRatio)
{
  if (!m_resources) {
    return;
  }
  Resources& r = *m_resources;
  m_morph = camera.morph();

  uploadMarkers(camera);
  uploadSpaceLines(camera);

  const Vec3& eye = camera.eyeEcef();
  const SplitVec3 eyeSplit = splitVec3(eye);
  const double widthPx = std::max(pass.viewportPx.width(), 1);
  const double heightPx = std::max(pass.viewportPx.height(), 1);

  FrameBlock frame;
  frame.viewProjection = floatMatrix(camera.viewProjection());
  frame.eyeHigh = {eyeSplit.highX, eyeSplit.highY, eyeSplit.highZ, 0.0F};
  frame.eyeLow = {eyeSplit.lowX, eyeSplit.lowY, eyeSplit.lowZ, 0.0F};
  frame.viewport = {static_cast<float>(widthPx), static_cast<float>(heightPx),
                    static_cast<float>(1.0 / widthPx),
                    static_cast<float>(1.0 / heightPx)};
  const bool terrain = terrainShown();
  frame.depthParams = {static_cast<float>(1.0 / std::log2(camera.farM() + 1.0)),
                       static_cast<float>(devicePixelRatio),
                       static_cast<float>(camera.tanHalfFovY()),
                       terrain ? 0.99F : 0.9F};
  // The terrain flattens as the view becomes the map.
  // The hillshade replaces it on the map.
  frame.terrain = {terrain ? static_cast<float>(1.0 - m_morph) : 0.0F,
                   static_cast<float>(m_tiles ? m_tiles->exaggeration() : 1.0),
                   terrain ? static_cast<float>(m_hillshade) : 0.0F,
                   static_cast<float>(hillshadeSlopeFactor)};
  // The timed lines' time (morph.w), after their origin.
  frame.morph.at(3) = static_cast<float>(m_spaceLineTimeS - m_spaceLineOriginS);
  if (m_morph > 0.0) {
    const MapFrame map = camera.mapFrame();
    const LocalFrame local =
        localFrame(camera.pose().targetLatDeg, camera.pose().targetLonDeg);
    frame.morph = {
        static_cast<float>(m_morph), static_cast<float>(map.planeScale),
        static_cast<float>(std::max(projectionIndex(map.projection), 0)),
        static_cast<float>(m_spaceLineTimeS - m_spaceLineOriginS)};
    frame.planeEast = float4(local.eastEcef, 0.0);
    frame.planeNorth = float4(local.northEcef, 0.0);
    frame.planeOrigin = float4(camera.targetEcef() - eye, 0.0);
    if (map.projection != nullptr && map.projection->hasOwnDomain()) {
      const std::array<float, 8> domain =
          map.projection->glslDomainParameters();
      frame.domain = {Float4{domain[0], domain[1], domain[2], domain[3]},
                      Float4{domain[4], domain[5], domain[6], domain[7]}};
    }
  }

  const double a = wgs84::semiMajorAxisM;
  const double b = wgs84::semiMinorAxisM;
  const Vec3 eyeScaled{eye.x / a, eye.y / a, eye.z / b};
  const Vec3 light = normalized(-camera.forwardEcef() + camera.upEcef() * 0.5 -
                                camera.rightEcef() * 0.35);
  // The graticule is computed from float positions (about 0.5 m steps), so
  // it fades out close to the ground.
  const double graticuleFade = smoothStep(
      graticuleFadeEndM, graticuleFadeStartM, camera.eyeGeodetic().heightM);

  GlobeBlock globe;
  // The tiles' sag (camForward.w) is set once they are chosen, below.
  globe.camForward = float4(camera.forwardEcef(), 0.0);
  globe.camRight = float4(camera.rightEcef(), camera.tanHalfFovX());
  globe.camUp = float4(camera.upEcef(), camera.tanHalfFovY());
  // The constant term in double: near the surface it is tiny (3e-7 at
  // 1 m), and computing it in float from eyeScaled would lose it all.
  globe.eyeScaled = float4(eyeScaled, lengthSquared(eyeScaled) - 1.0);
  globe.ellipsoid = {static_cast<float>(a), static_cast<float>(a / b),
                     static_cast<float>(1.0 / (a * a)),
                     static_cast<float>(1.0 / (b * b))};
  globe.eyeMetres = float4(eye, 0.0);
  globe.lightDir = float4(light, 0.0);
  globe.surfaceColor = color4(m_style.surfaceRgb, 1.0F);
  globe.gridColor = color4(m_style.graticuleRgb, 1.0F);

  const std::vector<std::size_t> graticuleIndex =
      writeGraticuleUniforms(graticuleFade, devicePixelRatio);

  if (m_tiles) {
    m_tiles->prepare(camera);
    globe.camForward.at(3) = static_cast<float>(m_tiles->surfaceSagM());
  }
  if (m_vectors) {
    m_vectors->prepare(camera);
  }
  r.uniforms->update(0, &frame, sizeof(frame));
  r.uniforms->update(r.globeOffset, &globe, sizeof(globe));

  PassDesc clearPass = pass;
  clearPass.clearColor = true;
  clearPass.clearColorRgba = color4(m_style.backgroundRgb, 1.0F);
  clearPass.clearDepth = true;
  clearPass.clearDepthValue = 1.0F;
  m_device.beginPass(clearPass);
  m_device.setUniformBuffer(frameBinding, *r.uniforms, 0, sizeof(frame));
  m_device.setUniformBuffer(globeBinding, *r.uniforms, r.globeOffset,
                            sizeof(globe));

  // On the map and during the morph the surface is the tiles themselves,
  // drawn with the first imagery layers unless the style's graticule goes
  // in between.
  // With terrain the ray-cast ellipsoid would cut through it: the tiles
  // are the surface, as on the map.
  const bool onTiles = m_morph > 0.0 || terrain;
  const bool baseFirst = onTiles && m_style.graticule;
  if (baseFirst) {
    m_tiles->drawBase();
  } else if (!onTiles) {
    m_device.setPipeline(*r.globePipeline);
    m_device.draw(3, 0, 1);
  }
  if (m_style.graticule) {
    drawGraticule(0);
  }
  if (m_tiles) {
    const std::function<void()> rebind = [this, &r]() {
      m_device.setUniformBuffer(frameBinding, *r.uniforms, 0, sizeof(frame));
      m_device.setUniformBuffer(globeBinding, *r.uniforms, r.globeOffset,
                                sizeof(globe));
    };
    m_tiles->draw(clearPass, rebind, !baseFirst);
  }
  // Vector layers over all imagery, in tree order.
  for (std::size_t i = 0; i < m_vectorLayers.size(); ++i) {
    if (m_vectorLayers[i].kind == VectorLayerDraw::Kind::Graticule) {
      drawGraticule(graticuleIndex[i]);
    } else if (m_vectors) {
      m_vectors->drawLayer(i);
    }
  }

  drawSpaceLines();
  if (r.markerCount > 0 && r.markerInstances) {
    m_device.setPipeline(*r.markerPipeline);
    m_device.setVertexBuffer(0, *r.markerCorners, 0);
    m_device.setVertexBuffer(1, *r.markerInstances, 0);
    m_device.draw(4, 0, r.markerCount);
  }
  m_device.endPass();
}

} // namespace ts
