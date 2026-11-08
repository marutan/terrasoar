// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/globe/tilerenderer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

#include "ts/camera/camera.h"
#include "ts/maths/splitdouble.h"
#include "ts/providers/tileprovider.h"

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

/** Uniform block "Tile" (std140): the tile centre as high/low floats. */
struct TileBlock {
  std::array<float, 4> centreHigh{};
  std::array<float, 4> centreLow{};
};

static_assert(sizeof(TileBlock) == 32, "std140 layout of Tile");

constexpr int imageryUnit = 0;

const char* const tileBlockGlsl = R"(
layout(std140) uniform Tile {
  vec4 centreHigh;
  vec4 centreLow;
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

const char* const tileFragmentGlsl = R"(
uniform sampler2D imagery;
in vec2 vUv;
in float vDepthW;
in vec3 vNormal;
out vec4 fragColor;
void main()
{
  vec4 texel = texture(imagery, vUv);
  vec3 n = normalize(vNormal);
  float diffuse = max(dot(n, lightDir.xyz), 0.0);
  vec3 rgb = texel.rgb * (0.55 + 0.45 * diffuse);
  fragColor = vec4(rgb * texel.a, texel.a);
  gl_FragDepth = clamp(log2(1.0 + max(vDepthW, 0.0)) * depthParams.x, 0.0, 1.0);
}
)";

std::size_t alignUp(std::size_t value, std::size_t alignment)
{
  return (value + alignment - 1) / alignment * alignment;
}

} // namespace

struct TileRenderer::GpuTile {
  std::unique_ptr<GpuBuffer> vertices;
  std::unique_ptr<GpuTexture> texture;
  TileBlock uniforms;
  std::size_t bytes = 0;
  std::uint64_t lastUsedFrame = 0;
};

TileRenderer::TileRenderer(RenderDevice& device,
                           const TileRendererSettings& settings)
    : m_device(device), m_settings(settings), m_selector(settings.selection)
{
}

TileRenderer::~TileRenderer()
{
  // Stop the workers before the GPU resources go.
  m_loader.reset();
}

bool TileRenderer::initialise(QString* errorString)
{
  ShaderDesc desc;
  desc.label = QStringLiteral("tile");
  desc.vertexSource = QByteArray(frameBlockGlsl) + globeBlockGlsl +
                      tileBlockGlsl + tileVertexGlsl;
  desc.fragmentSource = QByteArray(frameBlockGlsl) + globeBlockGlsl +
                        tileBlockGlsl + tileFragmentGlsl;
  desc.uniformBlocks = {{QByteArrayLiteral("Frame"), frameBinding},
                        {QByteArrayLiteral("Globe"), globeBinding},
                        {QByteArrayLiteral("Tile"), tileBinding}};
  desc.samplers = {{QByteArrayLiteral("imagery"), imageryUnit}};
  m_shader = m_device.createShaderProgram(desc, errorString);
  if (!m_shader) {
    return false;
  }

  PipelineDesc pipeline;
  pipeline.shader = m_shader.get();
  pipeline.bufferLayouts = {{sizeof(TileVertex), false}};
  pipeline.attributes = {{0, VertexFormat::Float3, 0, 0},
                         {1, VertexFormat::Float2, 0, 12}};
  pipeline.topology = PrimitiveTopology::Triangles;
  // Back faces are the parts of a tile beyond the limb.
  pipeline.cullMode = CullMode::Back;
  pipeline.depthTest = true;
  pipeline.depthWrite = true;
  pipeline.depthCompare = CompareOp::LessOrEqual;
  pipeline.blend = BlendMode::PremultipliedAlpha;
  m_pipeline = m_device.createPipeline(pipeline);

  const int segments = m_settings.loader.meshSegments;
  const std::vector<std::uint16_t> indices = tileMeshIndices(segments);
  m_indexCount = static_cast<int>(indices.size());
  m_indices = m_device.createBuffer(BufferType::Index, BufferUsage::Static,
                                    indices.size() * sizeof(std::uint16_t),
                                    indices.data());
  m_tileUniformStride =
      alignUp(sizeof(TileBlock), m_device.uniformBufferAlignment());
  if (!m_pipeline || !m_indices) {
    if (errorString) {
      *errorString = QStringLiteral("tiles: could not create GPU resources");
    }
    return false;
  }
  return true;
}

void TileRenderer::setImageryProvider(std::shared_ptr<ImageryProvider> provider)
{
  m_loader.reset();
  m_tiles.clear();
  m_render.clear();
  m_gpuBytes = 0;
  m_provider = std::move(provider);
  if (!m_provider) {
    return;
  }
  TileSelectionSettings selection = m_settings.selection;
  selection.tileSizePx = m_provider->tileSizePx();
  selection.maximumLevel =
      std::min(selection.maximumLevel, m_provider->maximumLevel());
  m_selector.setSettings(selection);
  m_loader = std::make_unique<TileLoader>(m_provider, m_settings.loader);
  m_loader->setTilesReadyCallback(m_callback);
}

std::shared_ptr<ImageryProvider> TileRenderer::imageryProvider() const
{
  return m_provider;
}

void TileRenderer::setTilesReadyCallback(std::function<void()> callback)
{
  m_callback = std::move(callback);
  if (m_loader) {
    m_loader->setTilesReadyCallback(m_callback);
  }
}

bool TileRenderer::isResident(TileKey key) const
{
  return m_tiles.count(key) != 0;
}

bool TileRenderer::isBusy() const
{
  return m_loader && m_loader->outstanding() > 0;
}

void TileRenderer::upload(const LoadedTile& tile)
{
  std::unique_ptr<GpuTile> gpu = std::make_unique<GpuTile>();
  const std::vector<TileVertex>& vertices = tile.mesh.vertices;
  if (vertices.empty()) {
    return;
  }
  gpu->vertices = m_device.createBuffer(BufferType::Vertex, BufferUsage::Static,
                                        vertices.size() * sizeof(TileVertex),
                                        vertices.data());
  gpu->bytes = vertices.size() * sizeof(TileVertex);
  if (!tile.image.isNull()) {
    TextureDesc desc;
    desc.width = tile.image.width();
    desc.height = tile.image.height();
    desc.filter = TextureFilter::Linear;
    desc.wrap = TextureWrap::ClampToEdge;
    desc.mipmaps = true;
    gpu->texture = m_device.createTexture(desc);
    if (gpu->texture) {
      gpu->texture->upload(tile.image);
      // RGBA8 plus a third for the mipmaps.
      gpu->bytes += static_cast<std::size_t>(desc.width) *
                    static_cast<std::size_t>(desc.height) * 4 * 4 / 3;
    }
  }
  const SplitVec3 centre = splitVec3(tile.mesh.centreEcef);
  gpu->uniforms.centreHigh = {centre.highX, centre.highY, centre.highZ, 0.0F};
  gpu->uniforms.centreLow = {centre.lowX, centre.lowY, centre.lowZ, 0.0F};
  gpu->lastUsedFrame = m_frame;
  m_gpuBytes += gpu->bytes;
  const std::unordered_map<TileKey, std::unique_ptr<GpuTile>,
                           TileKeyHash>::iterator existing =
      m_tiles.find(tile.key);
  if (existing != m_tiles.end()) {
    m_gpuBytes -= existing->second->bytes;
  }
  m_tiles[tile.key] = std::move(gpu);
}

void TileRenderer::evict()
{
  if (m_gpuBytes <= m_settings.gpuBytes) {
    return;
  }
  // Least recently drawn first; never what this frame draws.
  std::vector<std::pair<std::uint64_t, TileKey>> candidates;
  for (const std::pair<const TileKey, std::unique_ptr<GpuTile>>& entry :
       m_tiles) {
    if (entry.second->lastUsedFrame < m_frame) {
      candidates.emplace_back(entry.second->lastUsedFrame, entry.first);
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const std::pair<std::uint64_t, TileKey>& a,
               const std::pair<std::uint64_t, TileKey>& b) {
              return a.first < b.first;
            });
  for (const std::pair<std::uint64_t, TileKey>& candidate : candidates) {
    if (m_gpuBytes <= m_settings.gpuBytes) {
      break;
    }
    const std::unordered_map<TileKey, std::unique_ptr<GpuTile>,
                             TileKeyHash>::iterator it =
        m_tiles.find(candidate.second);
    m_gpuBytes -= it->second->bytes;
    m_tiles.erase(it);
  }
}

void TileRenderer::prepare(const Camera& camera)
{
  ++m_frame;
  m_statistics = TileStatistics();
  m_render.clear();
  if (!m_loader) {
    return;
  }
  const std::vector<LoadedTile> loaded =
      m_loader->takeLoaded(m_settings.uploadsPerFrame);
  for (const LoadedTile& tile : loaded) {
    upload(tile);
  }
  const TileSelection selection = m_selector.select(
      camera, [this](TileKey key) { return m_tiles.count(key) != 0; });
  m_render = selection.render;
  for (const TileKey& key : m_render) {
    m_tiles.at(key)->lastUsedFrame = m_frame;
  }
  m_loader->setRequests(selection.load);
  evict();

  m_statistics.visited = selection.visited;
  m_statistics.requested = static_cast<int>(selection.load.size());
  m_statistics.uploaded = static_cast<int>(loaded.size());
  m_statistics.resident = static_cast<int>(m_tiles.size());
  m_statistics.gpuBytes = m_gpuBytes;
}

void TileRenderer::draw()
{
  std::vector<const GpuTile*> drawn;
  drawn.reserve(m_render.size());
  for (const TileKey& key : m_render) {
    const GpuTile* tile = m_tiles.at(key).get();
    if (tile->texture) {
      drawn.push_back(tile);
      m_statistics.deepestLevel =
          std::max(m_statistics.deepestLevel, key.level);
    }
  }
  m_statistics.rendered = static_cast<int>(drawn.size());
  if (drawn.empty() || !m_pipeline) {
    return;
  }
  const std::size_t needed = drawn.size() * m_tileUniformStride;
  if (needed > m_tileUniformCapacity) {
    m_tileUniformCapacity = std::max(needed, 2 * m_tileUniformCapacity);
    m_tileUniforms =
        m_device.createBuffer(BufferType::Uniform, BufferUsage::Dynamic,
                              m_tileUniformCapacity, nullptr);
  }
  if (!m_tileUniforms) {
    return;
  }
  std::vector<std::uint8_t> staging(needed);
  for (std::size_t i = 0; i < drawn.size(); ++i) {
    std::memcpy(staging.data() + i * m_tileUniformStride, &drawn[i]->uniforms,
                sizeof(TileBlock));
  }
  m_tileUniforms->update(0, staging.data(), staging.size());

  m_device.setPipeline(*m_pipeline);
  m_device.setIndexBuffer(*m_indices, IndexFormat::UInt16);
  for (std::size_t i = 0; i < drawn.size(); ++i) {
    m_device.setVertexBuffer(0, *drawn[i]->vertices, 0);
    m_device.setTexture(imageryUnit, *drawn[i]->texture);
    m_device.setUniformBuffer(tileBinding, *m_tileUniforms,
                              i * m_tileUniformStride, sizeof(TileBlock));
    m_device.drawIndexed(m_indexCount, 0, 1);
  }
}

} // namespace ts
