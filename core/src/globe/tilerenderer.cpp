// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/globe/tilerenderer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_set>
#include <utility>

#include "ts/camera/camera.h"
#include "ts/maths/splitdouble.h"
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
  /** x: number of layers in this draw. */
  Float4 layerInfo{};
  /** Per layer: uv scale (xy) and offset (zw) into its image. */
  std::array<Float4, layersPerDraw> uvTransform{};
  /** Per layer opacity, four per vec4. */
  std::array<Float4, 2> layerAlpha{};
};

static_assert(sizeof(TileBlock) == 208, "std140 layout of Tile");

const char* const tileBlockGlsl = R"(
layout(std140) uniform Tile {
  vec4 centreHigh;
  vec4 centreLow;
  vec4 layerInfo;
  vec4 uvTransform[8];
  vec4 layerAlpha[2];
};
)";

const char* const tileVertexGlsl = R"(
layout(location = 0) in vec3 offset;
layout(location = 1) in vec2 uv;
out vec2 vUv;
out float vDepthW;
out vec3 vNormal;
void main()
{
  // Relative to the eye: centre (high/low) minus eye (high/low), plus the
  // vertex's float offset from the centre.
  vec3 position = (centreHigh.xyz - eyeHigh.xyz) + (centreLow.xyz - eyeLow.xyz)
                + offset;
  vec4 clip = viewProjection * vec4(position, 1.0);
  vUv = uv;
  vDepthW = clip.w;
  vNormal = (centreHigh.xyz + offset)
          * vec3(ellipsoid.z, ellipsoid.z, ellipsoid.w);
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
out vec4 fragColor;

// One layer's colour, premultiplied, at its opacity.
vec4 layerColor(sampler2D image, vec4 transform, float alpha)
{
  vec4 texel = texture(image, vUv * transform.xy + transform.zw);
  return vec4(texel.rgb * texel.a, texel.a) * alpha;
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
            lanes.at(static_cast<std::size_t>(i % 4)) +
            ");\n    result = c + result * (1.0 - c.a);\n  }\n";
  }
  glsl += R"(
  vec3 n = normalize(vNormal);
  float diffuse = max(dot(n, lightDir.xyz), 0.0);
  result.rgb *= 0.55 + 0.45 * diffuse;
  fragColor = result;
  gl_FragDepth = clamp(log2(1.0 + max(vDepthW, 0.0)) * depthParams.x, 0.0, 1.0);
}
)";
  return glsl;
}

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
};

struct TileRenderer::GpuImage {
  /** Null when the provider has no image for the tile. */
  std::unique_ptr<GpuTexture> texture;
  std::size_t bytes = 0;
  std::uint64_t lastUsedFrame = 0;
};

struct TileRenderer::Source {
  std::shared_ptr<ImageryProvider> provider;
  std::unique_ptr<TileLoader> loader;
  std::unordered_map<TileKey, GpuImage, TileKeyHash> images;
  std::size_t bytes = 0;
};

struct TileRenderer::DrawTile {
  TileKey key;
  const GpuMesh* mesh = nullptr;
  /** Per layer: the image (null: nothing to draw) and its uv transform. */
  std::vector<std::pair<const GpuTexture*, Float4>> layers;
};

TileRenderer::TileRenderer(RenderDevice& device,
                           const TileRendererSettings& settings)
    : m_device(device), m_settings(settings), m_selector(settings.selection)
{
  m_settings.loader.buildMeshes = false;
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
  desc.vertexSource = QByteArray(frameBlockGlsl) + globeBlockGlsl +
                      tileBlockGlsl + tileVertexGlsl;
  desc.fragmentSource = QByteArray(frameBlockGlsl) + globeBlockGlsl +
                        tileBlockGlsl + tileFragmentGlsl();
  desc.uniformBlocks = {{QByteArrayLiteral("Frame"), frameBinding},
                        {QByteArrayLiteral("Globe"), globeBinding},
                        {QByteArrayLiteral("Tile"), tileBinding}};
  for (int i = 0; i < layersPerDraw; ++i) {
    desc.samplers.push_back({"layer" + QByteArray::number(i), i});
  }
  m_shader = m_device.createShaderProgram(desc, errorString);

  ShaderDesc composite;
  composite.label = QStringLiteral("group composite");
  composite.vertexSource = compositeVertexGlsl;
  composite.fragmentSource = compositeFragmentGlsl;
  composite.uniformBlocks = {
      {QByteArrayLiteral("Composite"), compositeBinding}};
  composite.samplers = {{QByteArrayLiteral("groupImage"), 0}};
  m_compositeShader = m_device.createShaderProgram(composite, errorString);
  if (!m_shader || !m_compositeShader) {
    return false;
  }

  PipelineDesc pipeline;
  pipeline.shader = m_shader.get();
  pipeline.bufferLayouts = {{sizeof(TileVertex), false}};
  pipeline.attributes = {{0, VertexFormat::Float3, 0, 0},
                         {1, VertexFormat::Float2, 0, 12}};
  // Back faces are the parts of a tile beyond the limb.
  pipeline.cullMode = CullMode::Back;
  pipeline.depthTest = true;
  pipeline.depthWrite = true;
  // Equal passes: a tile is drawn again for each batch of layers.
  pipeline.depthCompare = CompareOp::LessOrEqual;
  pipeline.blend = BlendMode::PremultipliedAlpha;
  m_pipeline = m_device.createPipeline(pipeline);

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
  if (!m_pipeline || !m_compositePipeline || !m_indices || !m_emptyTexture ||
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
  TileSelectionSettings selection = m_settings.selection;
  selection.maximumLevel = std::min(selection.maximumLevel, deepest);
  selection.tileSizePx = tileSize;
  m_selector.setSettings(selection);
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
  for (const std::unique_ptr<Source>& source : m_sources) {
    source->loader->setTilesReadyCallback(m_callback);
  }
}

bool TileRenderer::isBusy() const
{
  if (m_meshesPending) {
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

void TileRenderer::buildMeshes(const std::vector<TileLoadRequest>& wanted)
{
  int built = 0;
  for (const TileLoadRequest& request : wanted) {
    if (built >= m_settings.meshBuildsPerFrame) {
      break;
    }
    const TileMesh mesh =
        buildTileMesh(request.key, m_settings.loader.meshSegments);
    std::unique_ptr<GpuMesh> gpu = std::make_unique<GpuMesh>();
    gpu->bytes = mesh.vertices.size() * sizeof(TileVertex);
    gpu->vertices =
        m_device.createBuffer(BufferType::Vertex, BufferUsage::Static,
                              gpu->bytes, mesh.vertices.data());
    const SplitVec3 centre = splitVec3(mesh.centreEcef);
    gpu->centreHigh = {centre.highX, centre.highY, centre.highZ, 0.0F};
    gpu->centreLow = {centre.lowX, centre.lowY, centre.lowZ, 0.0F};
    gpu->lastUsedFrame = m_frame;
    if (gpu->vertices) {
      m_meshBytes += gpu->bytes;
      m_meshes[request.key] = std::move(gpu);
    }
    ++built;
  }
  // Meshes built this frame let the next selection refine further.
  m_meshesPending = !wanted.empty();
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
      const Float4 transform = {
          scale, scale,
          static_cast<float>(tile.key.x - (key.x << levels)) * scale,
          static_cast<float>(tile.key.y - (key.y << levels)) * scale};
      tile.layers.emplace_back(texture, transform);
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
  if (m_layers.empty()) {
    m_meshesPending = false;
    return;
  }
  uploadImages();
  const TileSelection selection = m_selector.select(
      camera, [this](TileKey key) { return m_meshes.count(key) != 0; });
  m_drawTiles.reserve(selection.render.size());
  for (const TileKey& key : selection.render) {
    GpuMesh* mesh = m_meshes.at(key).get();
    mesh->lastUsedFrame = m_frame;
    m_drawTiles.push_back(DrawTile{key, mesh, {}});
    m_statistics.deepestLevel = std::max(m_statistics.deepestLevel, key.level);
  }
  buildMeshes(selection.load);
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

std::vector<bool> TileRenderer::writeBatchUniforms(LayerRange batch)
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
    block.layerInfo = {static_cast<float>(count), 0.0F, 0.0F, 0.0F};
    for (std::size_t slot = 0; slot < count; ++slot) {
      const std::size_t layerIndex = firstLayer + slot;
      const std::pair<const GpuTexture*, Float4>& binding =
          tile.layers[layerIndex];
      const double alpha =
          layerAlpha(m_layers[layerIndex], binding.first != nullptr);
      block.uvTransform.at(slot) = binding.second;
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

void TileRenderer::drawLayers(LayerRange layers)
{
  m_device.setPipeline(*m_pipeline);
  m_device.setIndexBuffer(*m_indices, IndexFormat::UInt16);
  const std::size_t first = layers.first;
  const std::size_t end = first + layers.count;
  const std::size_t perDraw = static_cast<std::size_t>(layersPerDraw);
  for (std::size_t batch = first; batch < end; batch += perDraw) {
    const std::size_t batchCount = std::min(perDraw, end - batch);
    const std::vector<bool> visible =
        writeBatchUniforms(LayerRange{batch, batchCount});
    for (std::size_t t = 0; t < visible.size(); ++t) {
      if (!visible[t]) {
        continue;
      }
      const DrawTile& tile = m_drawTiles[t];
      for (std::size_t slot = 0; slot < perDraw; ++slot) {
        const GpuTexture* texture =
            slot < batchCount ? tile.layers[batch + slot].first : nullptr;
        m_device.setTexture(static_cast<int>(slot),
                            texture ? *texture : *m_emptyTexture);
      }
      m_device.setVertexBuffer(0, *tile.mesh->vertices, 0);
      m_device.setUniformBuffer(tileBinding, *m_tileUniforms,
                                t * m_tileUniformStride, sizeof(TileBlock));
      m_device.drawIndexed(m_indexCount, 0, 1);
      ++m_statistics.drawCalls;
    }
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
                        const std::function<void()>& rebindUniforms)
{
  if (m_drawTiles.empty() || m_layers.empty() || !m_pipeline) {
    return;
  }
  const int count = static_cast<int>(m_layers.size());
  const bool groups = m_settings.groupCompositing;
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
                            static_cast<std::size_t>(end - first)});
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
                            static_cast<std::size_t>(end - first)});
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
