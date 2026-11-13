// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/globe/tilerenderer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <unordered_set>
#include <utility>

#include "ts/camera/camera.h"
#include "ts/elevation/heightgrid.h"
#include "ts/elevation/heighttileloader.h"
#include "ts/geo/wgs84.h"
#include "ts/maths/angles.h"
#include "ts/maths/splitdouble.h"
#include "ts/projection/cylindricalprojection.h"
#include "ts/projection/projection.h"
#include "ts/providers/tileprovider.h"
#include "ts/tiling/tilemesh.h"

#include "globeuniforms.h"

namespace ts
{

namespace
{

using globe_detail::frameBinding;
using globe_detail::frameBlockGlsl;
using globe_detail::globeBinding;
using globe_detail::globeBlockGlsl;
using globe_detail::graticuleBinding;
using globe_detail::graticuleBlockGlsl;
using globe_detail::gridLinesGlsl;
using globe_detail::morphGlsl;
using globe_detail::overlayDepthGlsl;
using globe_detail::tileBinding;

/** Layers blended in one draw (texture units 0 … 7). */
constexpr int layersPerDraw = 8;

/** Binding of the composite pass's block. */
constexpr int compositeBinding = 3;

/** How many levels above a tile its coarse stand-in is requested. */
constexpr int coarseLevels = 4;

using Float4 = std::array<float, 4>;

/** Uniform block "Tile" (std140). */
struct TileBlock {
  Float4 centreHigh{};
  Float4 centreLow{};
  /** x: number of layers in this draw; y: 1 to start from the surface
   *  colour (the base pass). */
  Float4 layerInfo{};
  /** Per layer: uv scale (xy) and offset (zw) into its image. */
  std::array<Float4, layersPerDraw> uvTransform{};
  /** Per layer opacity, four per vec4. */
  std::array<Float4, 2> layerAlpha{};
  /**
   * The map reference point (the tile's centre, held inside the
   * projection's latitude limits) relative to the centre of the view, in
   * metres on the map's plane (xy); the latitude of the tile's centre
   * minus the reference point's, radians (z).
   */
  Float4 mapOrigin{};
  /** The projection's reference values for the reference point. */
  Float4 projR0{};
  Float4 projR1{};
  /** Tile width and height, radians; centre longitude and latitude,
   *  degrees. */
  Float4 tileGeo{};
  /**
   * Per layer whose image is in Web Mercator (zero otherwise): the image
   * tile's height (x), how far its north edge is above the Mercator limit
   * (y, 0 unless it reaches past it), the latitude span the image covers
   * (z), all radians; 1 / that span in Mercator northing on the unit
   * sphere (w). See imageCoord() in the shader.
   */
  std::array<Float4, layersPerDraw> mercatorInfo{};
  /** Per such layer: Web Mercator's reference values at the image's
   *  (clipped) north edge (WebMercatorProjection::shaderReference()). */
  std::array<Float4, layersPerDraw> mercatorR0{};
  /** The second half of them. */
  std::array<Float4, layersPerDraw> mercatorR1{};
};

static_assert(sizeof(TileBlock) == 656, "std140 layout of Tile");

/** On the map a tile is drawn this much larger about its centre, so that
 *  rounding never leaves a crack between neighbours (their shared edge is
 *  computed from two different centres). A fortieth of a texel. */
constexpr const char* mapOverlapGlsl = "1.0001";

const char* const tileBlockGlsl = R"(
layout(std140) uniform Tile {
  vec4 centreHigh;
  vec4 centreLow;
  vec4 layerInfo;
  vec4 uvTransform[8];
  vec4 layerAlpha[2];
  vec4 mapOrigin;
  vec4 projR0;
  vec4 projR1;
  vec4 tileGeo;
  vec4 mercatorInfo[8];
  vec4 mercatorR0[8];
  vec4 mercatorR1[8];
};
)";

const char* const tileVertexGlsl = R"(
layout(location = 0) in vec3 offset;
layout(location = 1) in vec2 uv;
layout(location = 2) in vec3 terrainIn;
layout(location = 3) in vec2 slopeIn;
out vec2 vUv;
out float vDepthW;
out vec3 vNormal;
out vec3 vRelative;
out vec2 vLonLatDeg;
void main()
{
  // Relative to the eye: centre (high/low) minus eye (high/low), plus the
  // vertex's float offset from the centre.
  vec3 position = (centreHigh.xyz - eyeHigh.xyz) + (centreLow.xyz - eyeLow.xyz)
                + offset;
  // The terrain raises the vertex along the ellipsoid's normal.
  vec3 up = normalize((centreHigh.xyz + offset)
                      * vec3(ellipsoid.z, ellipsoid.z, ellipsoid.w));
  position += up * tsTerrainHeight(terrainIn);
  // The same vertex on the map, from its longitude and latitude.
  vec2 dLonLat = vec2(uv.x - 0.5, 0.5 - uv.y) * tileGeo.xy;
  vLonLatDeg = tileGeo.zw + degrees(dLonLat);
  if (morph.x > 0.0) {
    position = tsMorph(position,
                       dLonLat * MAP_OVERLAP + vec2(0.0, mapOrigin.z),
                       mapOrigin, projR0, projR1);
  }
  vec4 clip = viewProjection * vec4(position, 1.0);
  vUv = uv;
  vRelative = position;
  vDepthW = clip.w;
  vNormal = tsTerrainNormal(up, slopeIn);
  gl_Position = clip;
}
)";

/** The fragment shader, with the layer loop unrolled (GLSL 3.30 can only
 *  index sampler arrays with constants). */
QByteArray tileFragmentGlsl()
{
  QByteArray glsl;
  for (int i = 0; i < layersPerDraw; ++i) {
    glsl += "uniform sampler2D layer" + QByteArray::number(i) + ";\n";
  }
  glsl += R"(
in vec2 vUv;
in float vDepthW;
in vec3 vNormal;
in vec3 vRelative;
out vec4 fragColor;

// Where this fragment is in a layer's image. A Web Mercator image's rows
// are evenly spaced in northing, not latitude: the fragment's latitude,
// measured down from the image's north edge, becomes a northing with the
// projection's own difference formula (precise in small tiles), then a
// row. Outside the latitudes the image covers, inside is 0.
vec2 imageCoord(vec4 transform, vec4 info, vec4 r0, vec4 r1, out float inside)
{
  vec2 coord = vUv * transform.xy + transform.zw;
  inside = 1.0;
  if (info.z > 0.0) {
    float below = info.y - coord.y * info.x; // ≤ 0 inside the image
    inside = step(-info.z - 1e-7, below) * step(below, 1e-7);
    coord.y = -tsCylindricalDy(below, r0, r1) * info.w;
  }
  return coord;
}

// One layer's colour, premultiplied, at its opacity.
vec4 layerColor(sampler2D image, vec4 transform, float alpha, vec4 info,
                vec4 r0, vec4 r1)
{
  float inside;
  vec2 coord = imageCoord(transform, info, r0, r1, inside);
  vec4 texel = texture(image, coord);
  return vec4(texel.rgb * texel.a, texel.a) * alpha * inside;
}

void main()
{
  int count = int(layerInfo.x);
  vec4 result = vec4(0.0);
  vec4 c;
)";
  const std::array<const char*, 4> lanes = {"x", "y", "z", "w"};
  for (int i = 0; i < layersPerDraw; ++i) {
    const QByteArray n = QByteArray::number(i);
    glsl += "  if (count > " + n + ") {\n    c = layerColor(layer" + n +
            ", uvTransform[" + n + "], layerAlpha[" +
            QByteArray::number(i / 4) + "]." +
            lanes.at(static_cast<std::size_t>(i % 4)) + ", mercatorInfo[" + n +
            "], mercatorR0[" + n + "], mercatorR1[" + n +
            "]);\n    result = c + result * (1.0 - c.a);\n  }\n";
  }
  glsl += R"(
  vec3 n = normalize(vNormal);
  float diffuse = max(dot(n, lightDir.xyz), 0.0);
  // The flat map is not shaded.
  result.rgb *= mix(0.55 + 0.45 * diffuse, 1.0, morph.x);
  if (layerInfo.y > 0.5) {
    // Under the layers the bare surface, shaded as the ray-cast globe is.
    float facing = clamp(dot(n, -normalize(vRelative)), 0.0, 1.0);
    float shade = (0.30 + 0.70 * diffuse) * (0.85 + 0.15 * facing);
    result.rgb += surfaceColor.rgb * mix(shade, 1.0, morph.x)
                * (1.0 - result.a);
    result.a = 1.0;
  }
  fragColor = result;
  gl_FragDepth = clamp(log2(1.0 + max(vDepthW, 0.0)) * depthParams.x, 0.0, 1.0);
}
)";
  return glsl;
}

// The graticule on the tiles' own geometry, for the map and the morph
// (on the globe it is ray-cast, see GlobeRenderer).
const char* const tileGraticuleFragmentGlsl = R"(
in vec2 vLonLatDeg;
in float vDepthW;
out vec4 fragColor;
void main()
{
  float widthPx = graticuleParams.x;
  float lon = vLonLatDeg.x;
  float lat = vLonLatDeg.y;
  float grid = 0.7 * max(gridLines(lat, 10.0, widthPx, 4.0),
                         gridLines(lon, 10.0, widthPx, 4.0));
  grid = max(grid, 0.35 * max(gridLines(lat, 1.0, widthPx, 12.0),
                              gridLines(lon, 1.0, widthPx, 12.0)));
  float axes = max(gridLines(lat, 180.0, 1.5 * widthPx, 4.0),
                   gridLines(lon, 360.0, 1.5 * widthPx, 4.0));
  float amount = max(grid, axes) * graticuleParams.y;
  if (amount <= 0.0) {
    discard;
  }
  fragColor = graticuleColor * amount;
  gl_FragDepth = tsOverlayDepth(vDepthW);
}
)";

const char* const compositeVertexGlsl = R"(
out vec2 vUv;
void main()
{
  vec2 ndc = vec2(gl_VertexID == 1 ? 3.0 : -1.0, gl_VertexID == 2 ? 3.0 : -1.0);
  vUv = 0.5 * ndc + 0.5;
  gl_Position = vec4(ndc, 0.0, 1.0);
}
)";

const char* const compositeFragmentGlsl = R"(
layout(std140) uniform Composite { vec4 compositeParams; };
uniform sampler2D groupImage;
in vec2 vUv;
out vec4 fragColor;
void main()
{
  // The group image is premultiplied; fade it as a whole.
  fragColor = texture(groupImage, vUv) * compositeParams.x;
}
)";

std::size_t alignUp(std::size_t value, std::size_t alignment)
{
  return (value + alignment - 1) / alignment * alignment;
}

} // namespace

struct TileRenderer::GpuMesh {
  std::unique_ptr<GpuBuffer> vertices;
  Float4 centreHigh{};
  Float4 centreLow{};
  std::size_t bytes = 0;
  std::uint64_t lastUsedFrame = 0;
  /** Level of the heights it was built with; −1 for none. */
  int heightsLevel = -1;
  /** The heights it was built with (kept, to tell a replacement). */
  std::shared_ptr<const HeightTile> heights;
};

struct TileRenderer::GpuImage {
  /** Null when the provider has no image for the tile. */
  std::unique_ptr<GpuTexture> texture;
  std::size_t bytes = 0;
  std::uint64_t lastUsedFrame = 0;
};

struct TileRenderer::Source {
  std::shared_ptr<ImageryProvider> provider;
  /** The provider's images are in Web Mercator (reprojected when drawn). */
  bool webMercator = false;
  std::unique_ptr<TileLoader> loader;
  std::unordered_map<TileKey, GpuImage, TileKeyHash> images;
  std::size_t bytes = 0;
};

/** One layer's image for a drawn tile. */
struct TileRenderer::LayerImage {
  /** The image (null: nothing to draw). */
  const GpuTexture* texture = nullptr;
  /** Scale (xy) and offset (zw) from the tile's uv into the image. */
  Float4 transform{};
  /** For Web Mercator images, see TileBlock::mercatorInfo. */
  Float4 mercatorInfo{};
  Float4 mercatorR0{};
  Float4 mercatorR1{};
};

struct TileRenderer::DrawTile {
  TileKey key;
  const GpuMesh* mesh = nullptr;
  /** Per layer: its image. */
  std::vector<LayerImage> layers;
  /** The map uniforms of TileBlock (zero on the globe). */
  Float4 mapOrigin{};
  Float4 projR0{};
  Float4 projR1{};
  Float4 tileGeo{};
};

TileRenderer::TileRenderer(RenderDevice& device,
                           const TileRendererSettings& settings)
    : m_device(device), m_settings(settings),
      m_heights(settings.heightCacheBytes)
{
  m_settings.loader.buildMeshes = false;
  m_settings.selection.minimumMapLevel = m_settings.minimumMapLevel;
  m_settings.selection.meshSegments = m_settings.loader.meshSegments;
  m_selector.setSettings(m_settings.selection);
}

TileRenderer::~TileRenderer()
{
  // Stop the workers before the GPU resources go.
  for (const std::unique_ptr<Source>& source : m_sources) {
    source->loader.reset();
  }
}

bool TileRenderer::initialise(QString* errorString)
{
  ShaderDesc desc;
  desc.label = QStringLiteral("tile");
  const QByteArray vertexSource = QByteArray("#define MAP_OVERLAP ") +
                                  mapOverlapGlsl + "\n" + frameBlockGlsl +
                                  globeBlockGlsl + tileBlockGlsl +
                                  projectionGlsl() + morphGlsl + tileVertexGlsl;
  desc.vertexSource = vertexSource;
  desc.fragmentSource = QByteArray(frameBlockGlsl) + globeBlockGlsl +
                        tileBlockGlsl + projectionGlsl() + tileFragmentGlsl();
  desc.uniformBlocks = {{QByteArrayLiteral("Frame"), frameBinding},
                        {QByteArrayLiteral("Globe"), globeBinding},
                        {QByteArrayLiteral("Tile"), tileBinding}};
  for (int i = 0; i < layersPerDraw; ++i) {
    desc.samplers.push_back({"layer" + QByteArray::number(i), i});
  }
  m_shader = m_device.createShaderProgram(desc, errorString);

  ShaderDesc graticule;
  graticule.label = QStringLiteral("tile graticule");
  graticule.vertexSource = vertexSource;
  graticule.fragmentSource = QByteArray(frameBlockGlsl) + graticuleBlockGlsl +
                             gridLinesGlsl + overlayDepthGlsl +
                             tileGraticuleFragmentGlsl;
  graticule.uniformBlocks = {
      {QByteArrayLiteral("Frame"), frameBinding},
      {QByteArrayLiteral("Globe"), globeBinding},
      {QByteArrayLiteral("Tile"), tileBinding},
      {QByteArrayLiteral("Graticule"), graticuleBinding}};
  m_graticuleShader = m_device.createShaderProgram(graticule, errorString);

  ShaderDesc composite;
  composite.label = QStringLiteral("group composite");
  composite.vertexSource = compositeVertexGlsl;
  composite.fragmentSource = compositeFragmentGlsl;
  composite.uniformBlocks = {
      {QByteArrayLiteral("Composite"), compositeBinding}};
  composite.samplers = {{QByteArrayLiteral("groupImage"), 0}};
  m_compositeShader = m_device.createShaderProgram(composite, errorString);
  if (!m_shader || !m_compositeShader || !m_graticuleShader) {
    return false;
  }

  PipelineDesc pipeline;
  pipeline.shader = m_shader.get();
  pipeline.bufferLayouts = {{sizeof(TileVertex), false}};
  pipeline.attributes = {{0, VertexFormat::Float3, 0, 0},
                         {1, VertexFormat::Float2, 0, 12},
                         {2, VertexFormat::Float3, 0, 20},
                         {3, VertexFormat::Float2, 0, 32}};
  // Back faces are the parts of a tile beyond the limb.
  pipeline.cullMode = CullMode::Back;
  pipeline.depthTest = true;
  pipeline.depthWrite = true;
  // Equal passes: a tile is drawn again for each batch of layers.
  pipeline.depthCompare = CompareOp::LessOrEqual;
  pipeline.blend = BlendMode::PremultipliedAlpha;
  m_pipeline = m_device.createPipeline(pipeline);

  // Over the tiles drawn before it; see tsOverlayDepth().
  PipelineDesc graticulePipeline = pipeline;
  graticulePipeline.shader = m_graticuleShader.get();
  graticulePipeline.depthWrite = false;
  m_graticulePipeline = m_device.createPipeline(graticulePipeline);

  PipelineDesc compositePipeline;
  compositePipeline.shader = m_compositeShader.get();
  compositePipeline.blend = BlendMode::PremultipliedAlpha;
  m_compositePipeline = m_device.createPipeline(compositePipeline);

  const std::vector<std::uint16_t> indices =
      tileMeshIndices(m_settings.loader.meshSegments);
  m_indexCount = static_cast<int>(indices.size());
  m_indices = m_device.createBuffer(BufferType::Index, BufferUsage::Static,
                                    indices.size() * sizeof(std::uint16_t),
                                    indices.data());
  const std::vector<std::uint16_t> strips =
      tileMeshStripIndices(m_settings.loader.meshSegments);
  m_stripIndexCount = static_cast<int>(strips.size());
  m_stripIndices = m_device.createBuffer(BufferType::Index, BufferUsage::Static,
                                         strips.size() * sizeof(std::uint16_t),
                                         strips.data());
  TextureDesc empty;
  empty.width = 1;
  empty.height = 1;
  m_emptyTexture = m_device.createTexture(empty);
  if (m_emptyTexture) {
    QImage transparent(1, 1, QImage::Format_RGBA8888);
    transparent.fill(Qt::transparent);
    m_emptyTexture->upload(transparent);
  }
  m_compositeUniforms = m_device.createBuffer(
      BufferType::Uniform, BufferUsage::Dynamic, sizeof(Float4), nullptr);
  m_tileUniformStride =
      alignUp(sizeof(TileBlock), m_device.uniformBufferAlignment());
  if (!m_pipeline || !m_graticulePipeline || !m_compositePipeline ||
      !m_indices || !m_stripIndices || !m_emptyTexture ||
      !m_compositeUniforms) {
    if (errorString) {
      *errorString = QStringLiteral("tiles: could not create GPU resources");
    }
    return false;
  }
  return true;
}

void TileRenderer::setLayers(const std::vector<ImageryLayerDraw>& layers,
                             const std::vector<double>& groupAlphas)
{
  std::vector<std::unique_ptr<Source>> sources;
  std::vector<Layer> newLayers;
  newLayers.reserve(layers.size());
  for (const ImageryLayerDraw& draw : layers) {
    if (!draw.provider) {
      continue;
    }
    Source* source = nullptr;
    for (const std::unique_ptr<Source>& kept : sources) {
      if (kept->provider == draw.provider) {
        source = kept.get();
      }
    }
    if (!source) {
      // Reuse the existing source (and its tiles) for this provider.
      const std::vector<std::unique_ptr<Source>>::iterator existing =
          std::find_if(m_sources.begin(), m_sources.end(),
                       [&draw](const std::unique_ptr<Source>& s) {
                         return s && s->provider == draw.provider;
                       });
      if (existing != m_sources.end()) {
        sources.push_back(std::move(*existing));
      } else {
        std::unique_ptr<Source> created = std::make_unique<Source>();
        created->provider = draw.provider;
        created->webMercator = draw.provider->imageProjection() ==
                               TileImageProjection::WebMercator;
        created->loader =
            std::make_unique<TileLoader>(draw.provider, m_settings.loader);
        created->loader->setTilesReadyCallback(m_callback);
        sources.push_back(std::move(created));
      }
      source = sources.back().get();
    }
    newLayers.push_back(Layer{source, draw.alpha, draw.group});
  }
  // Sources nobody uses any more stop loading and free their tiles.
  for (std::unique_ptr<Source>& unused : m_sources) {
    if (unused) {
      unused->loader.reset();
    }
  }
  m_sources = std::move(sources);
  m_layers = std::move(newLayers);
  m_groupAlphas = groupAlphas;
  m_drawTiles.clear();

  int deepest = 0;
  int tileSize = 1;
  for (const std::unique_ptr<Source>& source : m_sources) {
    deepest = std::max(deepest, source->provider->maximumLevel());
    tileSize = std::max(tileSize, source->provider->tileSizePx());
  }
  m_imageryDeepestLevel = deepest;
  m_imageryTileSizePx = tileSize;
  applySelectionSettings();
}

void TileRenderer::applySelectionSettings()
{
  TileSelectionSettings selection = m_settings.selection;
  selection.minimumMapLevel = m_settings.minimumMapLevel;
  selection.meshSegments = m_settings.loader.meshSegments;
  int deepest = m_imageryDeepestLevel;
  int tileSize = m_imageryTileSizePx;
  if (terrainShown() && m_layers.empty()) {
    // Terrain alone: as deep as its data, and a little deeper (its tiles
    // are interpolated there) for the shape of the land.
    deepest = m_elevation->maximumLevel() + 3;
    tileSize = 256;
  }
  selection.maximumLevel = std::min(selection.maximumLevel, deepest);
  selection.tileSizePx = std::max(tileSize, 1);
  // The highest summit (8849 m) plus a margin, exaggerated.
  selection.terrainHeightM = terrainShown() ? 9000.0 * m_exaggeration : 0.0;
  m_selector.setSettings(selection);
}

void TileRenderer::setElevationProvider(
    std::shared_ptr<ElevationProvider> provider)
{
  if (provider == m_elevation) {
    return;
  }
  m_heightLoader.reset();
  m_elevation = std::move(provider);
  m_heights.clear();
  m_noHeights.clear();
  m_heightsRetry.clear();
  m_deepestHeights = 0;
  m_meshes.clear();
  m_meshBytes = 0;
  m_drawTiles.clear();
  if (m_elevation) {
    // Network sources want more requests in flight.
    m_heightLoader = std::make_unique<HeightTileLoader>(
        m_elevation, std::max(m_settings.heightThreads,
                              m_elevation->preferredLoaderThreads()));
    m_heightLoader->setTilesReadyCallback(m_callback);
  }
  applySelectionSettings();
}

void TileRenderer::setTerrain(bool visible, double exaggeration)
{
  m_terrainVisible = visible;
  m_exaggeration = std::clamp(exaggeration, 0.0, 100.0);
  applySelectionSettings();
}

bool TileRenderer::terrainShown() const
{
  return m_terrainVisible && m_elevation != nullptr;
}

std::shared_ptr<const HeightTile> TileRenderer::heightsFor(TileKey key,
                                                           bool exact)
{
  for (TileKey candidate = key;; candidate = candidate.parent()) {
    std::optional<std::shared_ptr<const HeightTile>> found =
        m_heights.find(candidate);
    if (found) {
      return *found;
    }
    if (exact || candidate.level == 0) {
      return nullptr;
    }
  }
}

std::optional<double> TileRenderer::terrainHeightM(double latDeg, double lonDeg)
{
  if (!terrainShown()) {
    return 0.0;
  }
  const Geodetic point{latDeg, lonDeg, 0.0};
  for (int level = m_deepestHeights; level >= 0; --level) {
    std::optional<std::shared_ptr<const HeightTile>> found =
        m_heights.find(TileKey::containing(level, point));
    if (!found || !*found) {
      continue;
    }
    const HeightTile& tile = **found;
    const double height = sampleHeightM(tile, latDeg, lonDeg).value_or(0.0);
    const double sea = sampleSeaLevelM(tile, latDeg, lonDeg).value_or(0.0);
    return sea + std::max(height - sea, 0.0) * m_exaggeration;
  }
  return std::nullopt;
}

std::optional<GroundHeight> TileRenderer::groundHeight(double latDeg,
                                                       double lonDeg)
{
  if (!m_elevation) {
    return std::nullopt;
  }
  const Geodetic point{latDeg, lonDeg, 0.0};
  for (int level = m_deepestHeights; level >= 0; --level) {
    std::optional<std::shared_ptr<const HeightTile>> found =
        m_heights.find(TileKey::containing(level, point));
    if (!found || !*found) {
      continue;
    }
    const HeightTile& tile = **found;
    const std::optional<double> height = sampleHeightM(tile, latDeg, lonDeg);
    if (!height) {
      continue;
    }
    GroundHeight ground;
    ground.ellipsoidalM = *height;
    ground.seaLevelM = sampleSeaLevelM(tile, latDeg, lonDeg).value_or(0.0);
    ground.level = level;
    return ground;
  }
  return std::nullopt;
}

void TileRenderer::takeHeights()
{
  if (!m_heightLoader) {
    return;
  }
  for (LoadedHeights& loaded : m_heightLoader->takeLoaded(256)) {
    if (!loaded.heights) {
      m_noHeights.insert(loaded.key);
      continue;
    }
    const std::size_t cost =
        (loaded.heights->heightsM.size() + loaded.heights->seaLevelM.size()) *
            sizeof(float) +
        sizeof(HeightTile);
    if (loaded.heights->incomplete) {
      // Shown as it is; asked for again later (see requestHeights()).
      m_heightsRetry[loaded.key] =
          std::chrono::steady_clock::now() + m_settings.heightsRetryDelay;
    } else {
      m_heightsRetry.erase(loaded.key);
    }
    m_deepestHeights = std::max(m_deepestHeights, loaded.key.level);
    m_heights.insert(
        loaded.key,
        std::make_shared<const HeightTile>(std::move(*loaded.heights)), cost);
  }
}

void TileRenderer::requestHeights(const TileSelection& selection)
{
  if (!m_heightLoader) {
    return;
  }
  // Drawn tiles first, then the ones about to be: the selection's order.
  std::vector<TileKey> wanted;
  const std::chrono::steady_clock::time_point now =
      std::chrono::steady_clock::now();
  const std::function<void(TileKey)> want = [&](TileKey key) {
    if (m_noHeights.count(key) != 0) {
      return;
    }
    if (!heightsFor(key, true)) {
      wanted.push_back(key);
      return;
    }
    // Incomplete heights, due to be asked for again.
    const std::unordered_map<TileKey, std::chrono::steady_clock::time_point,
                             TileKeyHash>::const_iterator retry =
        m_heightsRetry.find(key);
    if (retry != m_heightsRetry.end() && retry->second <= now) {
      wanted.push_back(key);
    }
  };
  std::unordered_set<TileKey, TileKeyHash> selected;
  for (const TileKey& key : selection.render) {
    want(key);
    selected.insert(key);
  }
  for (const TileLoadRequest& request : selection.load) {
    want(request.key);
    selected.insert(request.key);
  }
  // Incomplete heights no longer wanted are dropped rather than kept as
  // they are: picking and the readout use the finest heights there are,
  // and the complete ancestors are better than them. If they are wanted
  // again, they are loaded again.
  for (std::unordered_map<TileKey, std::chrono::steady_clock::time_point,
                          TileKeyHash>::iterator it = m_heightsRetry.begin();
       it != m_heightsRetry.end();) {
    if (selected.count(it->first) == 0) {
      m_heights.remove(it->first);
      it = m_heightsRetry.erase(it);
    } else {
      ++it;
    }
  }
  m_heightLoader->setRequests(wanted);
}

void TileRenderer::setImageryProvider(std::shared_ptr<ImageryProvider> provider)
{
  std::vector<ImageryLayerDraw> layers;
  if (provider) {
    layers.push_back(ImageryLayerDraw{std::move(provider), 1.0, -1});
  }
  setLayers(layers, {});
}

void TileRenderer::setGroupCompositing(bool enabled)
{
  m_settings.groupCompositing = enabled;
}

void TileRenderer::setTilesReadyCallback(std::function<void()> callback)
{
  m_callback = std::move(callback);
  if (m_heightLoader) {
    m_heightLoader->setTilesReadyCallback(m_callback);
  }
  for (const std::unique_ptr<Source>& source : m_sources) {
    source->loader->setTilesReadyCallback(m_callback);
  }
}

bool TileRenderer::isBusy() const
{
  if (m_meshesPending ||
      (m_heightLoader && m_heightLoader->outstanding() > 0)) {
    return true;
  }
  return std::any_of(m_sources.begin(), m_sources.end(),
                     [](const std::unique_ptr<Source>& source) {
                       return source->loader->outstanding() > 0;
                     });
}

void TileRenderer::uploadImages()
{
  int budget = m_settings.uploadsPerFrame;
  bool progress = true;
  // Round robin, so one busy source can't starve the others.
  while (budget > 0 && progress) {
    progress = false;
    for (const std::unique_ptr<Source>& source : m_sources) {
      if (budget <= 0) {
        break;
      }
      for (const LoadedTile& tile : source->loader->takeLoaded(1)) {
        GpuImage image;
        image.lastUsedFrame = m_frame;
        if (!tile.image.isNull()) {
          TextureDesc desc;
          desc.width = tile.image.width();
          desc.height = tile.image.height();
          desc.mipmaps = true;
          image.texture = m_device.createTexture(desc);
          if (image.texture) {
            image.texture->upload(tile.image);
            // RGBA8 plus a third for the mipmaps.
            image.bytes = static_cast<std::size_t>(desc.width) *
                          static_cast<std::size_t>(desc.height) * 4 * 4 / 3;
          }
        }
        const std::unordered_map<TileKey, GpuImage, TileKeyHash>::iterator
            replaced = source->images.find(tile.key);
        if (replaced != source->images.end()) {
          source->bytes -= replaced->second.bytes;
        }
        source->bytes += image.bytes;
        source->images[tile.key] = std::move(image);
        --budget;
        ++m_statistics.uploaded;
        progress = true;
      }
    }
  }
}

void TileRenderer::buildMeshes(const std::vector<TileLoadRequest>& wanted,
                               const std::vector<TileKey>& rebuild)
{
  int built = 0;
  // Meshes that now have their own heights, then new ones.
  for (const TileKey& key : rebuild) {
    if (built >= m_settings.meshBuildsPerFrame) {
      break;
    }
    uploadMesh(key);
    ++built;
  }
  for (const TileLoadRequest& request : wanted) {
    if (built >= m_settings.meshBuildsPerFrame) {
      break;
    }
    uploadMesh(request.key);
    ++built;
  }
  // Meshes built this frame let the next selection refine further.
  m_meshesPending = !wanted.empty() || !rebuild.empty();
}

void TileRenderer::uploadMesh(TileKey key)
{
  {
    // With terrain, the finest heights there are: the tile's own, or an
    // ancestor's until they arrive.
    const std::shared_ptr<const HeightTile> heights =
        m_elevation ? heightsFor(key, false) : nullptr;
    const TileMesh mesh =
        buildTileMesh(key, m_settings.loader.meshSegments, heights.get());
    std::unique_ptr<GpuMesh> gpu = std::make_unique<GpuMesh>();
    gpu->heightsLevel = heights ? heights->key.level : -1;
    gpu->heights = heights;
    gpu->bytes = mesh.vertices.size() * sizeof(TileVertex);
    gpu->vertices =
        m_device.createBuffer(BufferType::Vertex, BufferUsage::Static,
                              gpu->bytes, mesh.vertices.data());
    const SplitVec3 centre = splitVec3(mesh.centreEcef);
    gpu->centreHigh = {centre.highX, centre.highY, centre.highZ, 0.0F};
    gpu->centreLow = {centre.lowX, centre.lowY, centre.lowZ, 0.0F};
    gpu->lastUsedFrame = m_frame;
    if (gpu->vertices) {
      const std::unordered_map<TileKey, std::unique_ptr<GpuMesh>,
                               TileKeyHash>::iterator old = m_meshes.find(key);
      if (old != m_meshes.end()) {
        m_meshBytes -= old->second->bytes;
      }
      m_meshBytes += gpu->bytes;
      m_meshes[key] = std::move(gpu);
    }
  }
}

void TileRenderer::requestImages()
{
  // Coarse first: for every drawn tile a stand-in a few levels up, so
  // something shows quickly everywhere; then the tiles themselves, in
  // level order.
  std::vector<TileKey> drawn;
  drawn.reserve(m_drawTiles.size());
  for (const DrawTile& tile : m_drawTiles) {
    drawn.push_back(tile.key);
  }
  std::stable_sort(drawn.begin(), drawn.end(),
                   [](TileKey a, TileKey b) { return a.level < b.level; });
  for (const std::unique_ptr<Source>& source : m_sources) {
    const int deepest = source->provider->maximumLevel();
    std::vector<TileLoadRequest> requests;
    std::unordered_set<TileKey, TileKeyHash> seen;
    for (int pass = 0; pass < 2; ++pass) {
      for (const TileKey& key : drawn) {
        TileKey wanted = key;
        while (wanted.level > deepest) {
          wanted = wanted.parent();
        }
        if (pass == 0) {
          const int coarseLevel = std::max(0, wanted.level - coarseLevels);
          while (wanted.level > coarseLevel) {
            wanted = wanted.parent();
          }
        }
        if (source->images.count(wanted) == 0 && seen.insert(wanted).second) {
          requests.push_back(TileLoadRequest{wanted, 0.0});
        }
      }
    }
    m_statistics.requested += static_cast<int>(requests.size());
    source->loader->setRequests(requests);
  }
}

void TileRenderer::setMercatorImage(LayerImage& image, TileKey imageKey)
{
  // The image covers its tile's latitudes clipped to the Mercator limit,
  // rows evenly spaced in northing (see imageCoord() in the shader).
  static const WebMercatorProjection mercator;
  const GeoRect rect = imageKey.bounds();
  const double limit = mercator.maximumLatDeg();
  const double north = std::min(rect.northDeg, limit);
  const double south = std::max(rect.southDeg, -limit);
  if (!(south < north)) {
    return; // nothing of it is on the map; the provider has no image
  }
  const double spanM = mercator.northingM(north) - mercator.northingM(south);
  const ProjectionShaderReference reference =
      mercator.shaderReference(north, 0.0);
  image.mercatorInfo = {static_cast<float>(degToRad(rect.heightDeg())),
                        static_cast<float>(degToRad(rect.northDeg - north)),
                        static_cast<float>(degToRad(north - south)),
                        static_cast<float>(wgs84::semiMajorAxisM / spanM)};
  image.mercatorR0 = reference.r0;
  image.mercatorR1 = reference.r1;
}

void TileRenderer::bindImages()
{
  for (DrawTile& tile : m_drawTiles) {
    tile.layers.clear();
    for (const Layer& layer : m_layers) {
      Source& source = *layer.source;
      TileKey key = tile.key;
      while (key.level > source.provider->maximumLevel()) {
        key = key.parent();
      }
      const GpuTexture* texture = nullptr;
      for (;;) {
        const std::unordered_map<TileKey, GpuImage, TileKeyHash>::iterator it =
            source.images.find(key);
        if (it != source.images.end()) {
          // A tile known to have no image stays empty; no stand-in.
          texture = it->second.texture.get();
          it->second.lastUsedFrame = m_frame;
          break;
        }
        if (key.level == 0) {
          break;
        }
        key = key.parent();
      }
      // The drawn tile is a 2^d × 2^d part of the image's tile.
      const int levels = tile.key.level - key.level;
      const float scale = 1.0F / static_cast<float>(1 << levels);
      LayerImage image;
      image.texture = texture;
      image.transform = {
          scale, scale,
          static_cast<float>(tile.key.x - (key.x << levels)) * scale,
          static_cast<float>(tile.key.y - (key.y << levels)) * scale};
      if (source.webMercator) {
        setMercatorImage(image, key);
      }
      tile.layers.push_back(image);
    }
  }
}

void TileRenderer::evict()
{
  std::size_t total = m_meshBytes;
  for (const std::unique_ptr<Source>& source : m_sources) {
    total += source->bytes;
  }
  if (total <= m_settings.gpuBytes) {
    return;
  }
  // Least recently used first, meshes and images together; never anything
  // used this frame.
  struct Candidate {
    std::uint64_t lastUsed;
    int source; // −1 for a mesh
    TileKey key;
  };
  std::vector<Candidate> candidates;
  for (const std::pair<const TileKey, std::unique_ptr<GpuMesh>>& mesh :
       m_meshes) {
    if (mesh.second->lastUsedFrame < m_frame) {
      candidates.push_back({mesh.second->lastUsedFrame, -1, mesh.first});
    }
  }
  for (std::size_t s = 0; s < m_sources.size(); ++s) {
    for (const std::pair<const TileKey, GpuImage>& image :
         m_sources[s]->images) {
      if (image.second.lastUsedFrame < m_frame) {
        candidates.push_back(
            {image.second.lastUsedFrame, static_cast<int>(s), image.first});
      }
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) {
              return a.lastUsed < b.lastUsed;
            });
  for (const Candidate& candidate : candidates) {
    if (total <= m_settings.gpuBytes) {
      break;
    }
    if (candidate.source < 0) {
      const std::size_t bytes = m_meshes.at(candidate.key)->bytes;
      m_meshBytes -= bytes;
      total -= bytes;
      m_meshes.erase(candidate.key);
    } else {
      Source& source = *m_sources[static_cast<std::size_t>(candidate.source)];
      const std::size_t bytes = source.images.at(candidate.key).bytes;
      source.bytes -= bytes;
      total -= bytes;
      source.images.erase(candidate.key);
    }
  }
}

void TileRenderer::prepare(const Camera& camera)
{
  ++m_frame;
  m_statistics = TileStatistics();
  m_drawTiles.clear();
  m_morph = camera.morph();
  m_drawStrips = m_morph >= 1.0 && camera.mapProjection()->isCylindrical();
  // The map's surface and graticule are drawn on the tiles too, so it
  // needs them even without imagery.
  if (m_layers.empty() && m_morph <= 0.0 && !terrainShown()) {
    m_meshesPending = false;
    return;
  }
  uploadImages();
  takeHeights();
  const TileSelection selection = m_selector.select(
      camera, [this](TileKey key) { return m_meshes.count(key) != 0; });
  m_drawTiles.reserve(selection.render.size());
  const MapFrame frame = m_morph > 0.0 ? camera.mapFrame() : MapFrame();
  for (const TileKey& key : selection.render) {
    GpuMesh* mesh = m_meshes.at(key).get();
    mesh->lastUsedFrame = m_frame;
    m_statistics.deepestLevel = std::max(m_statistics.deepestLevel, key.level);
    if (m_morph <= 0.0) {
      m_drawTiles.push_back(DrawTile{key, mesh, {}, {}, {}, {}, {}});
      continue;
    }
    // On the map: where the tile's centre lies, once for each copy of the
    // world it shows in.
    const Projection& projection = *frame.projection;
    const GeoRect rect = key.bounds();
    const double referenceLatDeg =
        std::clamp(rect.centreLatDeg(), projection.minimumLatDeg(),
                   projection.maximumLatDeg());
    const MapPoint offset =
        frame.offsetOf(referenceLatDeg, rect.centreLonDeg());
    const ProjectionShaderReference reference =
        projection.shaderReference(referenceLatDeg, rect.centreLonDeg());
    DrawTile tile{key, mesh, {}, {}, reference.r0, reference.r1, {}};
    tile.tileGeo = {static_cast<float>(degToRad(rect.widthDeg())),
                    static_cast<float>(degToRad(rect.heightDeg())),
                    static_cast<float>(rect.centreLonDeg()),
                    static_cast<float>(rect.centreLatDeg())};
    for (const double shiftM :
         frame.copyShiftsM(rect.westDeg, rect.eastDeg, m_morph < 1.0)) {
      tile.mapOrigin = {
          static_cast<float>((offset.xM + shiftM) * frame.planeScale),
          static_cast<float>(offset.yM * frame.planeScale),
          static_cast<float>(degToRad(rect.centreLatDeg() - referenceLatDeg)),
          0.0F};
      m_drawTiles.push_back(tile);
    }
  }
  // Drawn meshes built with coarser heights than their own, once those
  // have arrived.
  std::vector<TileKey> rebuild;
  if (m_elevation) {
    for (const DrawTile& tile : m_drawTiles) {
      const std::shared_ptr<const HeightTile> own = heightsFor(tile.key, true);
      // Its own heights have arrived, or replaced incomplete ones.
      if (own && own != tile.mesh->heights &&
          std::find(rebuild.begin(), rebuild.end(), tile.key) ==
              rebuild.end()) {
        rebuild.push_back(tile.key);
      }
    }
  }
  requestHeights(selection);
  buildMeshes(selection.load, rebuild);
  if (!rebuild.empty()) {
    // A rebuilt mesh replaced the one the draw list points to.
    for (DrawTile& tile : m_drawTiles) {
      tile.mesh = m_meshes.at(tile.key).get();
    }
  }
  bindImages();
  requestImages();
  evict();

  m_statistics.rendered = static_cast<int>(m_drawTiles.size());
  m_statistics.visited = selection.visited;
  std::size_t bytes = m_meshBytes;
  for (const std::unique_ptr<Source>& source : m_sources) {
    m_statistics.resident += static_cast<int>(source->images.size());
    bytes += source->bytes;
  }
  m_statistics.gpuBytes = bytes;
}

double TileRenderer::layerAlpha(const Layer& layer, bool hasImage) const
{
  double alpha = hasImage ? layer.alpha : 0.0;
  if (!m_settings.groupCompositing && layer.group >= 0) {
    // Without compositing a group's opacity multiplies into its layers.
    alpha *= m_groupAlphas.at(static_cast<std::size_t>(layer.group));
  }
  return alpha;
}

std::vector<bool> TileRenderer::writeBatchUniforms(LayerRange batch, bool base)
{
  const std::size_t firstLayer = batch.first;
  const std::size_t count = batch.count;
  // One uniform block per tile for this batch.
  std::vector<std::uint8_t> staging(m_drawTiles.size() * m_tileUniformStride);
  std::vector<bool> visible(m_drawTiles.size(), false);
  for (std::size_t t = 0; t < m_drawTiles.size(); ++t) {
    const DrawTile& tile = m_drawTiles[t];
    TileBlock block;
    block.centreHigh = tile.mesh->centreHigh;
    block.centreLow = tile.mesh->centreLow;
    block.layerInfo = {static_cast<float>(count), base ? 1.0F : 0.0F, 0.0F,
                       0.0F};
    block.mapOrigin = tile.mapOrigin;
    block.projR0 = tile.projR0;
    block.projR1 = tile.projR1;
    block.tileGeo = tile.tileGeo;
    visible[t] = base;
    for (std::size_t slot = 0; slot < count; ++slot) {
      const std::size_t layerIndex = firstLayer + slot;
      const LayerImage& binding = tile.layers[layerIndex];
      const double alpha =
          layerAlpha(m_layers[layerIndex], binding.texture != nullptr);
      block.uvTransform.at(slot) = binding.transform;
      block.mercatorInfo.at(slot) = binding.mercatorInfo;
      block.mercatorR0.at(slot) = binding.mercatorR0;
      block.mercatorR1.at(slot) = binding.mercatorR1;
      block.layerAlpha.at(slot / 4).at(slot % 4) = static_cast<float>(alpha);
      visible[t] = visible[t] || alpha > 0.0;
    }
    std::memcpy(staging.data() + t * m_tileUniformStride, &block,
                sizeof(TileBlock));
  }
  if (staging.size() > m_tileUniformCapacity) {
    m_tileUniformCapacity = std::max(staging.size(), 2 * m_tileUniformCapacity);
    m_tileUniforms =
        m_device.createBuffer(BufferType::Uniform, BufferUsage::Dynamic,
                              m_tileUniformCapacity, nullptr);
  }
  if (!m_tileUniforms) {
    return {};
  }
  m_tileUniforms->update(0, staging.data(), staging.size());
  return visible;
}

void TileRenderer::setTileIndexBuffer()
{
  m_device.setIndexBuffer(m_drawStrips ? *m_stripIndices : *m_indices,
                          IndexFormat::UInt16);
}

int TileRenderer::tileIndexCount() const
{
  return m_drawStrips ? m_stripIndexCount : m_indexCount;
}

void TileRenderer::drawLayers(LayerRange layers, bool withBase)
{
  m_device.setPipeline(*m_pipeline);
  setTileIndexBuffer();
  const std::size_t first = layers.first;
  const std::size_t end = first + layers.count;
  const std::size_t perDraw = static_cast<std::size_t>(layersPerDraw);
  for (std::size_t batch = first; batch < end; batch += perDraw) {
    const std::size_t batchCount = std::min(perDraw, end - batch);
    // The surface colour goes under the first batch.
    const std::vector<bool> visible = writeBatchUniforms(
        LayerRange{batch, batchCount}, withBase && batch == first);
    for (std::size_t t = 0; t < visible.size(); ++t) {
      if (!visible[t]) {
        continue;
      }
      const DrawTile& tile = m_drawTiles[t];
      for (std::size_t slot = 0; slot < perDraw; ++slot) {
        const GpuTexture* texture =
            slot < batchCount ? tile.layers[batch + slot].texture : nullptr;
        m_device.setTexture(static_cast<int>(slot),
                            texture ? *texture : *m_emptyTexture);
      }
      m_device.setVertexBuffer(0, *tile.mesh->vertices, 0);
      m_device.setUniformBuffer(tileBinding, *m_tileUniforms,
                                t * m_tileUniformStride, sizeof(TileBlock));
      m_device.drawIndexed(tileIndexCount(), 0, 1);
      ++m_statistics.drawCalls;
    }
  }
}

void TileRenderer::drawPlain(const Pipeline& pipeline)
{
  if (m_drawTiles.empty() || m_morph <= 0.0) {
    return;
  }
  m_device.setPipeline(pipeline);
  setTileIndexBuffer();
  const std::vector<bool> visible = writeBatchUniforms(LayerRange{0, 0}, true);
  for (int slot = 0; slot < layersPerDraw; ++slot) {
    m_device.setTexture(slot, *m_emptyTexture);
  }
  for (std::size_t t = 0; t < visible.size(); ++t) {
    m_device.setVertexBuffer(0, *m_drawTiles[t].mesh->vertices, 0);
    m_device.setUniformBuffer(tileBinding, *m_tileUniforms,
                              t * m_tileUniformStride, sizeof(TileBlock));
    m_device.drawIndexed(tileIndexCount(), 0, 1);
    ++m_statistics.drawCalls;
  }
}

void TileRenderer::drawBase()
{
  if (m_pipeline) {
    drawPlain(*m_pipeline);
  }
}

void TileRenderer::drawGraticule()
{
  if (m_graticulePipeline) {
    drawPlain(*m_graticulePipeline);
  }
}

void TileRenderer::composite(double alpha)
{
  const Float4 params = {static_cast<float>(alpha), 0.0F, 0.0F, 0.0F};
  m_compositeUniforms->update(0, params.data(), sizeof(params));
  m_device.setPipeline(*m_compositePipeline);
  m_device.setRenderTargetTexture(0, *m_groupTarget);
  m_device.setUniformBuffer(compositeBinding, *m_compositeUniforms, 0,
                            sizeof(params));
  m_device.draw(3, 0, 1);
  ++m_statistics.groupsComposited;
}

void TileRenderer::draw(const PassDesc& mainPass,
                        const std::function<void()>& rebindUniforms,
                        bool withBase)
{
  if (m_drawTiles.empty() || !m_pipeline) {
    return;
  }
  // On the globe the ray-cast ellipsoid is the surface under the layers;
  // with terrain the tiles have to be (the ellipsoid is not where they are).
  bool baseWanted = withBase && (m_morph > 0.0 || terrainShown());
  const int count = static_cast<int>(m_layers.size());
  const bool groups = m_settings.groupCompositing;
  // A composite group is drawn on its own offscreen, so the surface can't
  // share its draws; nor can it with no layers at all.
  if (baseWanted && (count == 0 || (groups && m_layers.front().group >= 0))) {
    drawBase();
    baseWanted = false;
  }
  int first = 0;
  while (first < count) {
    const int group =
        groups ? m_layers[static_cast<std::size_t>(first)].group : -1;
    int end = first + 1;
    while (end < count &&
           (groups ? m_layers[static_cast<std::size_t>(end)].group : -1) ==
               group) {
      ++end;
    }
    if (group < 0) {
      drawLayers(LayerRange{static_cast<std::size_t>(first),
                            static_cast<std::size_t>(end - first)},
                 baseWanted);
      baseWanted = false;
      first = end;
      continue;
    }
    // The group alone into an offscreen target, then faded onto the scene
    // as one image.
    const QSize size = mainPass.viewportPx.size();
    if (!m_groupTarget || m_groupTarget->width() != size.width() ||
        m_groupTarget->height() != size.height()) {
      m_groupTarget = m_device.createRenderTarget(size.width(), size.height());
    }
    if (m_groupTarget) {
      m_device.endPass();
      PassDesc offscreen;
      offscreen.target = m_groupTarget.get();
      offscreen.viewportPx = QRect(QPoint(0, 0), size);
      offscreen.clearColorRgba = {0.0F, 0.0F, 0.0F, 0.0F};
      m_device.beginPass(offscreen);
      rebindUniforms();
      drawLayers(LayerRange{static_cast<std::size_t>(first),
                            static_cast<std::size_t>(end - first)},
                 false);
      m_device.endPass();
      PassDesc resume = mainPass;
      resume.clearColor = false;
      resume.clearDepth = false;
      m_device.beginPass(resume);
      rebindUniforms();
      composite(m_groupAlphas.at(static_cast<std::size_t>(group)));
    }
    first = end;
  }
}

} // namespace ts
