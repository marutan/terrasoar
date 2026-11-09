// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/globe/globerenderer.h"

#include <algorithm>
#include <cmath>

#include "ts/camera/camera.h"
#include "ts/geo/wgs84.h"
#include "ts/globe/tilerenderer.h"
#include "ts/maths/mat4.h"
#include "ts/maths/splitdouble.h"

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

/** One marker instance in the vertex buffer. */
struct MarkerInstance {
  /** High xyz, low xyz, radius in logical px. */
  std::array<float, 7> values{};
  /** RGBA, read as normalised floats. */
  std::array<std::uint8_t, 4> rgba{};
};

static_assert(sizeof(MarkerInstance) == 32, "marker instance stride");

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
  // 1 % farther than the surface: a tile's flat triangles lie a little
  // below the curved surface and must still pass the depth test.
  float w = 1.01 * hit.tM * dot(hit.d, camForward.xyz);
  gl_FragDepth = clamp(log2(1.0 + max(w, 0.0)) * depthParams.x, 0.0, 1.0);
}
)";

// The graticule alone, over whatever is drawn already (no depth test).
const char* const graticuleFragmentGlsl = R"(
layout(std140) uniform Graticule {
  vec4 graticuleColor;  // premultiplied
  vec4 graticuleParams; // line width (device px), opacity
};
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
out vec2 vOffsetPx;
out float vRadiusPx;
out vec4 vColor;
out float vDepthW;
void main()
{
  // Relative to the eye: the high parts cancel exactly near the eye.
  vec3 offset = (positionHigh - eyeHigh.xyz) + (positionLow - eyeLow.xyz);
  vec4 clip = viewProjection * vec4(offset, 1.0);
  float radius = radiusPx * depthParams.y;
  float extent = radius + 1.0;
  clip.xy += corner * extent * 2.0 * viewport.zw * clip.w;
  vOffsetPx = corner * extent;
  vRadiusPx = radius;
  vColor = color;
  // A marker is a flat disc facing the viewer at its point's depth; the
  // ground around the point can be nearer by up to a few times the disc's
  // size in metres (more at grazing angles). Bring the depth that much
  // nearer so the ground doesn't cut the disc.
  float metresPerPx = 2.0 * depthParams.z * viewport.w * clip.w;
  vDepthW = max(clip.w - 4.0 * extent * metresPerPx, 0.5 * clip.w);
  gl_Position = clip;
}
)";

const char* const markerFragmentGlsl = R"(
in vec2 vOffsetPx;
in float vRadiusPx;
in vec4 vColor;
in float vDepthW;
out vec4 fragColor;
void main()
{
  float r = length(vOffsetPx);
  float alpha = clamp(vRadiusPx + 0.5 - r, 0.0, 1.0) * vColor.a;
  if (alpha <= 0.0) {
    discard;
  }
  // A dark rim one pixel wide keeps the marker visible on any background.
  float fill = clamp(vRadiusPx - 0.5 - r, 0.0, 1.0);
  vec3 rgb = vColor.rgb * fill;
  fragColor = vec4(rgb * alpha, alpha);
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
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
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
  int markerCount = 0;
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
                         traceGlsl + globeFragmentGlsl;
  globe.uniformBlocks = blocks;
  r->globeShader = m_device.createShaderProgram(globe, errorString);

  ShaderDesc graticule;
  graticule.label = QStringLiteral("graticule");
  graticule.vertexSource = globe.vertexSource;
  graticule.fragmentSource = QByteArray(frameBlockGlsl) + globeBlockGlsl +
                             traceGlsl + graticuleFragmentGlsl;
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
  if (!r->globeShader || !r->graticuleShader || !r->markerShader) {
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
  markerPipeline.attributes = {{0, VertexFormat::Float2, 0, 0},
                               {1, VertexFormat::Float3, 1, 0},
                               {2, VertexFormat::Float3, 1, 12},
                               {3, VertexFormat::Float, 1, 24},
                               {4, VertexFormat::UNorm8x4, 1, 28}};
  markerPipeline.depthTest = true;
  markerPipeline.depthWrite = false;
  markerPipeline.depthCompare = CompareOp::LessOrEqual;
  markerPipeline.blend = BlendMode::PremultipliedAlpha;
  r->markerPipeline = m_device.createPipeline(markerPipeline);

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
      !r->markerCorners || !r->uniforms) {
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

void GlobeRenderer::uploadMarkers()
{
  if (!m_markersDirty) {
    return;
  }
  Resources& r = *m_resources;
  std::vector<MarkerInstance> instances;
  instances.reserve(m_markers.size());
  for (const GlobeMarker& marker : m_markers) {
    const SplitVec3 split = splitVec3(marker.positionEcef);
    instances.push_back(MarkerInstance{{split.highX, split.highY, split.highZ,
                                        split.lowX, split.lowY, split.lowZ,
                                        static_cast<float>(marker.radiusPx)},
                                       marker.rgba});
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
  r.markerCount = static_cast<int>(instances.size());
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
  m_device.setPipeline(*r.graticulePipeline);
  m_device.setUniformBuffer(graticuleBinding, *r.graticuleUniforms,
                            index * r.graticuleStride, sizeof(GraticuleBlock));
  m_device.draw(3, 0, 1);
}

void GlobeRenderer::render(const Camera& camera, const PassDesc& pass,
                           double devicePixelRatio)
{
  if (!m_resources) {
    return;
  }
  Resources& r = *m_resources;

  uploadMarkers();

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
  frame.depthParams = {static_cast<float>(1.0 / std::log2(camera.farM() + 1.0)),
                       static_cast<float>(devicePixelRatio),
                       static_cast<float>(camera.tanHalfFovY()), 0.0F};

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

  m_device.setPipeline(*r.globePipeline);
  m_device.draw(3, 0, 1);
  if (m_style.graticule) {
    drawGraticule(0);
  }
  if (m_tiles) {
    const std::function<void()> rebind = [this, &r]() {
      m_device.setUniformBuffer(frameBinding, *r.uniforms, 0, sizeof(frame));
      m_device.setUniformBuffer(globeBinding, *r.uniforms, r.globeOffset,
                                sizeof(globe));
    };
    m_tiles->draw(clearPass, rebind);
  }
  // Vector layers over all imagery, in tree order.
  for (std::size_t i = 0; i < m_vectorLayers.size(); ++i) {
    if (m_vectorLayers[i].kind == VectorLayerDraw::Kind::Graticule) {
      drawGraticule(graticuleIndex[i]);
    } else if (m_vectors) {
      m_vectors->drawLayer(i);
    }
  }

  if (r.markerCount > 0 && r.markerInstances) {
    m_device.setPipeline(*r.markerPipeline);
    m_device.setVertexBuffer(0, *r.markerCorners, 0);
    m_device.setVertexBuffer(1, *r.markerInstances, 0);
    m_device.draw(4, 0, r.markerCount);
  }
  m_device.endPass();
}

} // namespace ts
