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

#include "ts/globe/vectorrenderer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <unordered_set>
#include <utility>

#include "ts/camera/camera.h"
#include "ts/maths/angles.h"
#include "ts/maths/splitdouble.h"
#include "ts/projection/projection.h"
#include "ts/providers/tileprovider.h"

#include "globeuniforms.h"

namespace ts
{

namespace
{

using globe_detail::Float4;
using globe_detail::frameBinding;
using globe_detail::frameBlockGlsl;
using globe_detail::globeBinding;
using globe_detail::globeBlockGlsl;
using globe_detail::morphGlsl;
using globe_detail::offMapGlsl;
using globe_detail::overlayDepthGlsl;
using globe_detail::vectorBinding;

/** Slack around a draw's clip rectangle, degrees: no cracks between tiles. */
constexpr double clipSlackDeg = 1e-5;

/** Uniform block "Vector" (std140): one per draw. */
struct VectorBlock {
  Float4 centreHigh{};
  Float4 centreLow{};
  /** West, south, east, north, degrees from the mesh's centre. */
  Float4 clipRect{};
  /** Premultiplied by the layer's opacity. */
  Float4 fillColor{};
  /** Premultiplied by the layer's opacity. */
  Float4 outlineColor{};
  /** Outline width (logical px), near plane (m), the mesh's centre's
   *  longitude and latitude (degrees). */
  Float4 params{};
  /**
   * The map reference point (the mesh's centre, held inside the
   * projection's domain) relative to the centre of the view, in metres on
   * the map's plane (xy); the latitude (z) and longitude (w) of the
   * mesh's centre minus the reference point's, radians.
   */
  Float4 mapOrigin{};
  /** The projection's reference values for the reference point. */
  Float4 projR0{};
  Float4 projR1{};
  Float4 projR2{};
  Float4 projR3{};
};

static_assert(sizeof(VectorBlock) == 176, "std140 layout of Vector");

const char* const vectorBlockGlsl = R"(
layout(std140) uniform Vector {
  vec4 centreHigh;
  vec4 centreLow;
  vec4 clipRect;
  vec4 fillColor;
  vec4 outlineColor;
  vec4 params;
  vec4 mapOrigin;
  vec4 projR0;
  vec4 projR1;
  vec4 projR2;
  vec4 projR3;
};
)";

// For vertex shaders, after vectorBlockGlsl, projectionGlsl() and
// morphGlsl.
const char* const vectorVertexHelpersGlsl = R"(
// Eye-relative position of a vertex given as an offset from the centre.
vec3 relativeToEye(vec3 offset)
{
  return (centreHigh.xyz - eyeHigh.xyz) + (centreLow.xyz - eyeLow.xyz)
       + offset;
}

// The same between the globe and the flat map; lonLat is the vertex's
// offset from the centre in degrees.
vec3 morphed(vec3 relative, vec2 lonLat)
{
  if (morph.x <= 0.0) {
    return relative;
  }
  return tsMorph(relative, radians(lonLat) + mapOrigin.wz, mapOrigin, projR0,
                 projR1, projR2, projR3);
}

// Positive where the ellipsoid faces the eye at that point (on this side
// of the horizon).
// The vertex raised onto the terrain (when shown), relative to the eye.
vec3 onTerrain(vec3 offset, vec2 terrainIn)
{
  vec3 up = normalize((centreHigh.xyz + offset)
                      * vec3(ellipsoid.z, ellipsoid.z, ellipsoid.w));
  return relativeToEye(offset) + up * tsTerrainHeight(vec3(terrainIn, 0.0));
}

float facing(vec3 offset, vec3 relative)
{
  vec3 normal = (centreHigh.xyz + offset)
              * vec3(ellipsoid.z, ellipsoid.z, ellipsoid.w);
  return dot(normalize(normal), -normalize(relative));
}

)";

// For fragment shaders, after vectorBlockGlsl and overlayDepthGlsl.
const char* const vectorFragmentHelpersGlsl = R"(
// Outside the draw's clip rectangle, or (on the map) the projection's own
// domain; lonLat is degrees from the mesh's centre (params.zw).
bool outsideClip(vec2 lonLat)
{
  return lonLat.x < clipRect.x || lonLat.y < clipRect.y
      || lonLat.x > clipRect.z || lonLat.y > clipRect.w
      || tsOffMap(lonLat + params.zw);
}

// True for what the globe hides: on the globe the far side of the
// ellipsoid. On the map and during the morph the depth test does it.
bool hidden(float facing)
{
  return morph.x <= 0.0 && facing < 0.0;
}
)";

const char* const fillVertexGlsl = R"(
layout(location = 0) in vec3 offset;
layout(location = 1) in vec2 lonLat;
layout(location = 2) in vec2 terrainIn;
out vec2 vLonLat;
out float vFacing;
out float vDepthW;
void main()
{
  vec3 relative = onTerrain(offset, terrainIn);
  vLonLat = lonLat;
  vFacing = facing(offset, relative);
  gl_Position = viewProjection
              * vec4(tsMapRelief(morphed(relative, lonLat),
                                 vec3(terrainIn, 0.0)), 1.0);
  vDepthW = gl_Position.w;
}
)";

const char* const fillFragmentGlsl = R"(
in vec2 vLonLat;
in float vFacing;
in float vDepthW;
out vec4 fragColor;
void main()
{
  if (hidden(vFacing) || outsideClip(vLonLat)) {
    discard;
  }
  fragColor = fillColor * (1.0 - tsMapHaze(vDepthW));
  gl_FragDepth = tsOverlayDepth(vDepthW);
}
)";

// One instance per segment, drawn as a 4-vertex strip around it in screen
// space: the outline keeps its width in pixels at every distance.
const char* const lineVertexGlsl = R"(
layout(location = 0) in vec3 aOffset;
layout(location = 1) in vec2 aLonLat;
layout(location = 2) in vec3 bOffset;
layout(location = 3) in vec2 bLonLat;
layout(location = 4) in vec2 aTerrain;
layout(location = 5) in vec2 bTerrain;
out vec2 vLonLat;
out float vFacing;
out float vDepthW;
flat out vec2 vA;
flat out vec2 vB;
flat out float vHalfWidth;

vec2 toScreen(vec4 clip)
{
  return (clip.xy / clip.w * 0.5 + 0.5) * viewport.xy;
}

void main()
{
  vec3 relA = onTerrain(aOffset, aTerrain);
  vec3 relB = onTerrain(bOffset, bTerrain);
  vec4 clipA = viewProjection
             * vec4(tsMapRelief(morphed(relA, aLonLat), vec3(aTerrain, 0.0)),
                    1.0);
  vec4 clipB = viewProjection
             * vec4(tsMapRelief(morphed(relB, bLonLat), vec3(bTerrain, 0.0)),
                    1.0);
  float nearW = params.y;
  if (clipA.w < nearW && clipB.w < nearW) {
    gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // behind the eye: off screen
    return;
  }
  // Move an end behind the near plane onto it.
  if (clipA.w < nearW) {
    clipA = mix(clipA, clipB, (nearW - clipA.w) / (clipB.w - clipA.w));
  } else if (clipB.w < nearW) {
    clipB = mix(clipB, clipA, (nearW - clipB.w) / (clipA.w - clipB.w));
  }
  vec2 sa = toScreen(clipA);
  vec2 sb = toScreen(clipB);
  vec2 along = sb - sa;
  float len = length(along);
  vec2 dir = len > 1e-6 ? along / len : vec2(1.0, 0.0);
  vec2 normal = vec2(-dir.y, dir.x);
  float halfWidth = 0.5 * params.x * depthParams.y;
  float extent = halfWidth + 1.0;
  bool atB = gl_VertexID >= 2;
  float side = (gl_VertexID == 1 || gl_VertexID == 3) ? 1.0 : -1.0;
  vec2 corner = (atB ? sb + dir * extent : sa - dir * extent)
              + normal * side * extent;
  vec4 clip = atB ? clipB : clipA;
  // Depth plays no part (no depth test); z = 0 keeps the quad clear of
  // the near and far planes.
  gl_Position = vec4((corner / viewport.xy * 2.0 - 1.0) * clip.w, 0.0, clip.w);
  vLonLat = atB ? bLonLat : aLonLat;
  vFacing = atB ? facing(bOffset, relB) : facing(aOffset, relA);
  vDepthW = clip.w;
  vA = sa;
  vB = sb;
  vHalfWidth = halfWidth;
}
)";

const char* const lineFragmentGlsl = R"(
in vec2 vLonLat;
in float vFacing;
in float vDepthW;
flat in vec2 vA;
flat in vec2 vB;
flat in float vHalfWidth;
out vec4 fragColor;
void main()
{
  if (hidden(vFacing) || outsideClip(vLonLat)) {
    discard;
  }
  // Distance from the pixel centre to the segment (a capsule: round ends
  // join neighbouring segments without gaps).
  vec2 p = gl_FragCoord.xy;
  vec2 ab = vB - vA;
  float t = clamp(dot(p - vA, ab) / max(dot(ab, ab), 1e-12), 0.0, 1.0);
  float distance = length(p - (vA + t * ab));
  float coverage = clamp(vHalfWidth + 0.5 - distance, 0.0, 1.0);
  if (coverage <= 0.0) {
    discard;
  }
  fragColor = outlineColor * coverage * (1.0 - tsMapHaze(vDepthW));
  gl_FragDepth = tsOverlayDepth(vDepthW);
}
)";

std::size_t alignUp(std::size_t value, std::size_t alignment)
{
  return (value + alignment - 1) / alignment * alignment;
}

} // namespace

/** One stored tile's mesh on the GPU. */
struct VectorRenderer::GpuMesh {
  TileKey key;
  std::unique_ptr<GpuBuffer> fill;
  int32_t fillVertices = 0;
  std::unique_ptr<GpuBuffer> segments;
  int32_t segmentCount = 0;
  /** The mesh's runs and the features' styles they use. */
  std::vector<VectorRun> fillRuns;
  std::vector<VectorRun> segmentRuns;
  std::vector<FeatureStyle> styles;
  Float4 centreHigh{};
  Float4 centreLow{};
  double centreLonDeg = 0.0;
  double centreLatDeg = 0.0;
  std::size_t bytes = 0;
  std::uint64_t lastUsedFrame = 0;
};

/** One feature provider: its loader, meshes and this frame's draws. */
struct VectorRenderer::Source {
  std::shared_ptr<FeatureProvider> provider;
  std::unique_ptr<VectorTileLoader> loader;
  TileSelector selector;
  std::unordered_map<TileKey, std::unique_ptr<GpuMesh>, TileKeyHash> meshes;
  std::size_t bytes = 0;
  std::vector<DrawItem> items;
};

VectorRenderer::VectorRenderer(RenderDevice& device,
                               const VectorRendererSettings& settings)
    : m_device(device), m_settings(settings)
{
}

std::vector<TileKey>
VectorRenderer::drawnTiles(const FeatureProvider* provider) const
{
  std::vector<TileKey> keys;
  for (const std::unique_ptr<Source>& source : m_sources) {
    if (source->provider.get() != provider) {
      continue;
    }
    for (const DrawItem& item : source->items) {
      if (item.mesh != nullptr &&
          std::find(keys.begin(), keys.end(), item.mesh->key) == keys.end()) {
        keys.push_back(item.mesh->key);
      }
    }
  }
  return keys;
}

VectorRenderer::~VectorRenderer()
{
  // Stop the workers before the GPU resources go.
  for (const std::unique_ptr<Source>& source : m_sources) {
    source->loader.reset();
  }
}

bool VectorRenderer::initialise(QString* errorString)
{
  const std::vector<UniformBlockBinding> blocks = {
      {QByteArrayLiteral("Frame"), frameBinding},
      {QByteArrayLiteral("Globe"), globeBinding},
      {QByteArrayLiteral("Vector"), vectorBinding}};
  const QByteArray blockGlsl =
      QByteArray(frameBlockGlsl) + globeBlockGlsl + vectorBlockGlsl;
  const QByteArray vertexPrefix =
      blockGlsl + projectionGlsl() + morphGlsl + vectorVertexHelpersGlsl;
  const QByteArray fragmentPrefix = blockGlsl + projectionDomainGlsl() +
                                    offMapGlsl + overlayDepthGlsl +
                                    vectorFragmentHelpersGlsl;

  ShaderDesc fill;
  fill.label = QStringLiteral("vector fill");
  fill.vertexSource = vertexPrefix + fillVertexGlsl;
  fill.fragmentSource = fragmentPrefix + fillFragmentGlsl;
  fill.uniformBlocks = blocks;
  m_fillShader = m_device.createShaderProgram(fill, errorString);

  ShaderDesc line;
  line.label = QStringLiteral("vector line");
  line.vertexSource = vertexPrefix + lineVertexGlsl;
  line.fragmentSource = fragmentPrefix + lineFragmentGlsl;
  line.uniformBlocks = blocks;
  m_lineShader = m_device.createShaderProgram(line, errorString);
  if (!m_fillShader || !m_lineShader) {
    return false;
  }

  PipelineDesc fillPipeline;
  fillPipeline.shader = m_fillShader.get();
  fillPipeline.bufferLayouts = {{sizeof(VectorVertex), false}};
  fillPipeline.attributes = {{0, VertexFormat::Float3, 0, 0},
                             {1, VertexFormat::Float2, 0, 12},
                             {2, VertexFormat::Float2, 0, 20}};
  fillPipeline.blend = BlendMode::PremultipliedAlpha;
  m_fillPipeline = m_device.createPipeline(fillPipeline);

  PipelineDesc linePipeline;
  linePipeline.shader = m_lineShader.get();
  linePipeline.topology = PrimitiveTopology::TriangleStrip;
  linePipeline.bufferLayouts = {{sizeof(VectorSegment), true}};
  linePipeline.attributes = {
      {0, VertexFormat::Float3, 0, 0},  {1, VertexFormat::Float2, 0, 12},
      {2, VertexFormat::Float3, 0, 28}, {3, VertexFormat::Float2, 0, 40},
      {4, VertexFormat::Float2, 0, 20}, {5, VertexFormat::Float2, 0, 48}};
  linePipeline.blend = BlendMode::PremultipliedAlpha;
  m_linePipeline = m_device.createPipeline(linePipeline);

  // On the map and during the morph: tested against the tiles' depth (see
  // tsOverlayDepth()).
  fillPipeline.depthTest = true;
  fillPipeline.depthCompare = CompareOp::LessOrEqual;
  m_fillMapPipeline = m_device.createPipeline(fillPipeline);
  linePipeline.depthTest = true;
  linePipeline.depthCompare = CompareOp::LessOrEqual;
  m_lineMapPipeline = m_device.createPipeline(linePipeline);

  m_uniformStride =
      alignUp(sizeof(VectorBlock), m_device.uniformBufferAlignment());
  if (!m_fillPipeline || !m_linePipeline || !m_fillMapPipeline ||
      !m_lineMapPipeline) {
    if (errorString) {
      *errorString = QStringLiteral("vectors: could not create pipelines");
    }
    return false;
  }
  return true;
}

void VectorRenderer::setElevationProvider(
    std::shared_ptr<ElevationProvider> provider)
{
  if (provider == m_elevation) {
    return;
  }
  m_elevation = std::move(provider);
  // Every mesh again, with (or without) heights.
  std::vector<VectorLayerDraw> draws;
  draws.reserve(m_layers.size());
  for (const std::pair<Source*, VectorLayerDraw>& layer : m_layers) {
    draws.push_back(layer.second);
  }
  m_layers.clear();
  for (std::unique_ptr<Source>& source : m_sources) {
    source->loader.reset();
  }
  m_sources.clear();
  setLayers(draws);
}

void VectorRenderer::setTerrainHeightMargin(double metres)
{
  m_terrainMarginM = metres;
  for (const std::unique_ptr<Source>& source : m_sources) {
    TileSelectionSettings selection = source->selector.settings();
    selection.terrainHeightM = metres;
    source->selector.setSettings(selection);
  }
}

void VectorRenderer::setLayers(const std::vector<VectorLayerDraw>& layers)
{
  std::vector<std::unique_ptr<Source>> sources;
  std::vector<std::pair<Source*, VectorLayerDraw>> newLayers;
  for (const VectorLayerDraw& draw : layers) {
    if (draw.kind != VectorLayerDraw::Kind::Features || !draw.provider) {
      newLayers.emplace_back(nullptr, draw);
      continue;
    }
    Source* source = nullptr;
    for (const std::unique_ptr<Source>& kept : sources) {
      source = kept->provider == draw.provider ? kept.get() : source;
    }
    if (!source) {
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
        created->loader = std::make_unique<VectorTileLoader>(
            draw.provider, m_settings.loader, m_elevation);
        created->loader->setTilesReadyCallback(m_callback);
        TileSelectionSettings selection = m_settings.selection;
        selection.maximumLevel =
            std::min(selection.maximumLevel, draw.provider->maximumLevel());
        selection.terrainHeightM = m_terrainMarginM;
        created->selector.setSettings(selection);
        sources.push_back(std::move(created));
      }
      source = sources.back().get();
    }
    newLayers.emplace_back(source, draw);
  }
  for (std::unique_ptr<Source>& unused : m_sources) {
    if (unused) {
      unused->loader.reset();
    }
  }
  m_sources = std::move(sources);
  m_layers = std::move(newLayers);
}

void VectorRenderer::setTilesReadyCallback(std::function<void()> callback)
{
  m_callback = std::move(callback);
  for (const std::unique_ptr<Source>& source : m_sources) {
    source->loader->setTilesReadyCallback(m_callback);
  }
}

bool VectorRenderer::isBusy() const
{
  return std::any_of(m_sources.begin(), m_sources.end(),
                     [](const std::unique_ptr<Source>& source) {
                       return source->loader->outstanding() > 0;
                     });
}

std::unique_ptr<VectorRenderer::GpuMesh>
VectorRenderer::createGpuMesh(const VectorTileMesh& mesh) const
{
  std::unique_ptr<GpuMesh> gpu = std::make_unique<GpuMesh>();
  gpu->key = mesh.key;
  gpu->lastUsedFrame = m_frame;
  const SplitVec3 centre = splitVec3(mesh.centreEcef);
  gpu->centreHigh = {centre.highX, centre.highY, centre.highZ, 0.0F};
  gpu->centreLow = {centre.lowX, centre.lowY, centre.lowZ, 0.0F};
  gpu->centreLonDeg = mesh.centreLonDeg;
  gpu->centreLatDeg = mesh.centreLatDeg;
  if (!mesh.triangles.empty()) {
    gpu->fill = m_device.createBuffer(
        BufferType::Vertex, BufferUsage::Static,
        mesh.triangles.size() * sizeof(VectorVertex), mesh.triangles.data());
    gpu->fillVertices =
        gpu->fill ? static_cast<int32_t>(mesh.triangles.size()) : 0;
  }
  if (!mesh.segments.empty()) {
    gpu->segments = m_device.createBuffer(
        BufferType::Vertex, BufferUsage::Static,
        mesh.segments.size() * sizeof(VectorSegment), mesh.segments.data());
    gpu->segmentCount =
        gpu->segments ? static_cast<int32_t>(mesh.segments.size()) : 0;
  }
  gpu->fillRuns = mesh.fillRuns;
  gpu->segmentRuns = mesh.segmentRuns;
  gpu->styles = mesh.styles;
  // A mesh made without runs: all in the layer's style.
  if (gpu->fillRuns.empty() && gpu->fillVertices > 0) {
    gpu->fillRuns.push_back(VectorRun{0, gpu->fillVertices, 0, false});
  }
  if (gpu->segmentRuns.empty() && gpu->segmentCount > 0) {
    gpu->segmentRuns.push_back(VectorRun{0, gpu->segmentCount, 0, false});
  }
  gpu->bytes = mesh.bytes();
  return gpu;
}

void VectorRenderer::upload()
{
  int32_t budget = m_settings.uploadsPerFrame;
  bool progress = true;
  // Round robin, so one busy source can't starve the others.
  while (budget > 0 && progress) {
    progress = false;
    for (const std::unique_ptr<Source>& source : m_sources) {
      for (const VectorTileMesh& mesh :
           source->loader->takeLoaded(budget > 0 ? 1 : 0)) {
        std::unique_ptr<GpuMesh>& slot = source->meshes[mesh.key];
        if (slot) {
          source->bytes -= slot->bytes;
        }
        slot = createGpuMesh(mesh);
        source->bytes += slot->bytes;
        --budget;
        ++m_statistics.uploaded;
        progress = true;
      }
    }
  }
}

void VectorRenderer::addItem(Source& source, const Camera& camera,
                             const GpuMesh* mesh, const GeoRect& clip)
{
  if (camera.morph() <= 0.0) {
    source.items.push_back(DrawItem{mesh, clip, 0.0});
    return;
  }
  // On the map: once for each copy of the world the piece shows in.
  for (const double shiftM : camera.mapFrame().copyShiftsM(
           clip.westDeg, clip.eastDeg, camera.morph() < 1.0)) {
    source.items.push_back(DrawItem{mesh, clip, shiftM});
  }
}

void VectorRenderer::select(Source& source, const Camera& camera) const
{
  source.items.clear();
  const FeatureProvider& provider = *source.provider;
  const TileSelection selection =
      source.selector.select(camera, [&](TileKey key) {
        return source.meshes.count(provider.storedKey(key)) != 0;
      });

  // Drawn tiles grouped by the stored tile that serves them.
  std::vector<TileKey> stored;
  std::unordered_map<TileKey, std::vector<TileKey>, TileKeyHash> served;
  std::vector<TileKey> requests;
  std::unordered_set<TileKey, TileKeyHash> requested;
  for (const TileKey& key : selection.render) {
    const TileKey s = provider.storedKey(key);
    if (source.meshes.count(s) == 0) {
      if (requested.insert(s).second) {
        requests.push_back(s);
      }
      continue;
    }
    std::vector<TileKey>& keys = served[s];
    if (keys.empty()) {
      stored.push_back(s);
    }
    keys.push_back(key);
  }
  for (const TileKey& s : stored) {
    GpuMesh* mesh = source.meshes.at(s).get();
    mesh->lastUsedFrame = m_frame;
    // A stored tile is drawn whole, unless tiles of another level of
    // detail are drawn inside it: then only over the tiles it serves.
    const bool shared =
        std::any_of(stored.begin(), stored.end(), [&s](TileKey other) {
          return other != s && s.contains(other);
        });
    if (!shared) {
      addItem(source, camera, mesh, s.bounds());
    } else {
      for (const TileKey& key : served.at(s)) {
        addItem(source, camera, mesh, key.bounds());
      }
    }
  }
  for (const TileLoadRequest& request : selection.load) {
    const TileKey s = provider.storedKey(request.key);
    if (source.meshes.count(s) == 0 && requested.insert(s).second) {
      requests.push_back(s);
    }
  }
  std::stable_sort(requests.begin(), requests.end(),
                   [](TileKey a, TileKey b) { return a.level < b.level; });
  source.loader->setRequests(requests);
}

void VectorRenderer::evict()
{
  std::size_t total = 0;
  for (const std::unique_ptr<Source>& source : m_sources) {
    total += source->bytes;
  }
  if (total <= m_settings.gpuBytes) {
    return;
  }
  std::vector<std::pair<std::uint64_t, GpuMesh*>> candidates;
  std::vector<Source*> owners;
  for (const std::unique_ptr<Source>& source : m_sources) {
    for (const std::pair<const TileKey, std::unique_ptr<GpuMesh>>& mesh :
         source->meshes) {
      if (mesh.second->lastUsedFrame < m_frame) {
        candidates.emplace_back(mesh.second->lastUsedFrame, mesh.second.get());
        owners.push_back(source.get());
      }
    }
  }
  std::vector<std::size_t> order(candidates.size());
  for (std::size_t i = 0; i < order.size(); ++i) {
    order[i] = i;
  }
  std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
    return candidates[a].first < candidates[b].first;
  });
  for (const std::size_t i : order) {
    if (total <= m_settings.gpuBytes) {
      break;
    }
    Source& source = *owners[i];
    const TileKey key = candidates[i].second->key;
    const std::size_t bytes = candidates[i].second->bytes;
    source.bytes -= bytes;
    total -= bytes;
    source.meshes.erase(key);
  }
}

namespace
{

/** Where one draw of a mesh goes: its mesh's centre and its clip. */
struct Placement {
  Float4 centreHigh{};
  Float4 centreLow{};
  double lonDeg = 0.0;
  double latDeg = 0.0;
  GeoRect clip;
  double mapShiftM = 0.0;
};

/**
 * The block's placement for @p place: the clip rectangle, and on the map
 * (@p frame not null) the reference point and projection values.
 */
VectorBlock placedBlock(const Placement& place, const MapFrame* frame)
{
  VectorBlock values;
  values.centreHigh = place.centreHigh;
  values.centreLow = place.centreLow;
  const double lon = place.lonDeg;
  const double lat = place.latDeg;
  values.clipRect = {
      static_cast<float>(place.clip.westDeg - lon - clipSlackDeg),
      static_cast<float>(place.clip.southDeg - lat - clipSlackDeg),
      static_cast<float>(place.clip.eastDeg - lon + clipSlackDeg),
      static_cast<float>(place.clip.northDeg - lat + clipSlackDeg)};
  if (frame != nullptr) {
    const Projection& projection = *frame->projection;
    if (!projection.repeats()) {
      // Nothing beyond the edge of a map that ends at its domain:
      // there it would be squashed onto the edge, and lines and
      // fills would show along it. Relative to the mesh's centre.
      const double limit = projection.maximumLonOffsetDeg();
      const double domainLon =
          normaliseLonDeg(projection.centralMeridianDeg() - lon);
      values.clipRect = {
          std::max(values.clipRect[0], static_cast<float>(domainLon - limit)),
          std::max(values.clipRect[1],
                   static_cast<float>(projection.minimumLatDeg() - lat)),
          std::min(values.clipRect[2], static_cast<float>(domainLon + limit)),
          std::min(values.clipRect[3],
                   static_cast<float>(projection.maximumLatDeg() - lat))};
    }
    // The reference point: the mesh's centre, held inside the domain.
    const Geodetic inside = projection.clampToDomain(lat, lon);
    const double referenceLatDeg = inside.latDeg;
    const double referenceLonDeg = inside.lonDeg;
    const double dLatRad = degToRad(lat - referenceLatDeg);
    const double dLonRad = degToRad(normaliseLonDeg(lon - referenceLonDeg));
    const MapPoint offset = frame->offsetOf(referenceLatDeg, referenceLonDeg);
    // offsetOf() takes the copy nearest the view's centre; the copies
    // of this piece were counted from the middle of its clip rectangle.
    const double pieceXM =
        frame
            ->offsetOf(projection.forward(frame->centreLatDeg,
                                          place.clip.centreLonDeg()))
            .xM;
    double originXM = offset.xM;
    const double halfWorldM = 0.5 * frame->worldWidthM;
    if (originXM - pieceXM > halfWorldM) {
      originXM -= frame->worldWidthM;
    } else if (originXM - pieceXM < -halfWorldM) {
      originXM += frame->worldWidthM;
    }
    // Its vertices reach the clip rectangle (and a little past it).
    const double halfSpanRad =
        degToRad(std::max({std::abs(place.clip.westDeg - lon),
                           std::abs(place.clip.eastDeg - lon),
                           std::abs(place.clip.southDeg - lat),
                           std::abs(place.clip.northDeg - lat)}) +
                 clipSlackDeg) +
        std::max(std::abs(dLatRad), std::abs(dLonRad));
    const ProjectionShaderReference reference = projection.shaderReferenceFor(
        referenceLatDeg, referenceLonDeg, halfSpanRad);
    values.mapOrigin = {
        static_cast<float>((originXM + place.mapShiftM) * frame->planeScale),
        static_cast<float>(offset.yM * frame->planeScale),
        static_cast<float>(dLatRad), static_cast<float>(dLonRad)};
    values.projR0 = reference.r0;
    values.projR1 = reference.r1;
    values.projR2 = reference.r2;
    values.projR3 = reference.r3;
  }
  return values;
}

/** How a run looks: the layer's style where the feature's leaves it. */
struct RunLook {
  QColor fill;
  QColor outline;
  double widthPx = 0.0;
  bool showsFill = false;
  bool showsLine = false;
};

RunLook runLook(const std::vector<FeatureStyle>& styles, VectorRun run,
                const VectorStyle& layer)
{
  const FeatureStyle none;
  const FeatureStyle& own =
      run.style > 0 && static_cast<std::size_t>(run.style) <= styles.size()
          ? styles.at(static_cast<std::size_t>(run.style) - 1)
          : none;
  RunLook look;
  look.fill = own.fill.value_or(layer.fill);
  look.outline = own.outline.value_or(layer.outline);
  look.widthPx = own.outlineWidthPx.value_or(layer.outlineWidthPx);
  if (run.points) {
    // Dots in the outline's colour (the fill's without one).
    if (!own.outline && !layer.hasOutline()) {
      look.outline = layer.fill;
    }
    look.widthPx = own.pointSizePx.value_or(layer.pointSizePx);
  }
  look.showsFill = look.fill.isValid() && look.fill.alpha() > 0;
  look.showsLine =
      look.outline.isValid() && look.outline.alpha() > 0 && look.widthPx > 0.0;
  return look;
}

} // namespace

void VectorRenderer::writeUniforms(const Camera& camera)
{
  std::vector<std::uint8_t> staging;
  m_layerUniformStart.assign(m_layers.size(), 0);
  std::size_t block = 0;
  const bool onMap = camera.morph() > 0.0;
  const MapFrame frame = onMap ? camera.mapFrame() : MapFrame();
  for (std::size_t i = 0; i < m_layers.size(); ++i) {
    m_layerUniformStart[i] = block;
    const Source* source = m_layers[i].first;
    if (!source) {
      continue;
    }
    const VectorLayerDraw& draw = m_layers[i].second;
    for (const DrawItem& item : source->items) {
      const GpuMesh& mesh = *item.mesh;
      VectorBlock values = placedBlock(
          Placement{mesh.centreHigh, mesh.centreLow, mesh.centreLonDeg,
                    mesh.centreLatDeg, item.clip, item.mapShiftM},
          onMap ? &frame : nullptr);
      // One block per run: the same place, the run's colours and width;
      // fill runs first, then segment runs (drawLayer() counts so).
      std::vector<VectorRun> runs = mesh.fillRuns;
      runs.insert(runs.end(), mesh.segmentRuns.begin(), mesh.segmentRuns.end());
      for (const VectorRun run : runs) {
        const RunLook look = runLook(mesh.styles, run, draw.style);
        values.fillColor = premultipliedRgba(look.fill, draw.alpha);
        values.outlineColor = premultipliedRgba(look.outline, draw.alpha);
        values.params = {static_cast<float>(look.widthPx),
                         static_cast<float>(camera.nearM()),
                         static_cast<float>(mesh.centreLonDeg),
                         static_cast<float>(mesh.centreLatDeg)};
        staging.resize((block + 1) * m_uniformStride);
        std::memcpy(staging.data() + block * m_uniformStride, &values,
                    sizeof(values));
        ++block;
      }
    }
  }
  if (staging.empty()) {
    return;
  }
  if (staging.size() > m_uniformCapacity) {
    m_uniformCapacity = std::max(staging.size(), 2 * m_uniformCapacity);
    m_uniforms = m_device.createBuffer(
        BufferType::Uniform, BufferUsage::Dynamic, m_uniformCapacity, nullptr);
  }
  if (m_uniforms) {
    m_uniforms->update(0, staging.data(), staging.size());
  }
}

void VectorRenderer::prepare(const Camera& camera)
{
  ++m_frame;
  m_morph = camera.morph();
  m_statistics = VectorStatistics();
  upload();
  for (const std::unique_ptr<Source>& source : m_sources) {
    select(*source, camera);
  }
  evict();
  writeUniforms(camera);
  for (const std::unique_ptr<Source>& source : m_sources) {
    m_statistics.resident += static_cast<int32_t>(source->meshes.size());
    m_statistics.gpuBytes += source->bytes;
  }
}

void VectorRenderer::drawLayer(std::size_t index)
{
  if (index >= m_layers.size() || !m_layers[index].first || !m_uniforms) {
    return;
  }
  const Source& source = *m_layers[index].first;
  const VectorStyle& style = m_layers[index].second.style;
  // The blocks follow writeUniforms(): per item, its fill runs, then its
  // segment runs.
  std::size_t block = m_layerUniformStart[index];
  std::vector<std::size_t> firstBlock;
  firstBlock.reserve(source.items.size());
  for (const DrawItem& item : source.items) {
    firstBlock.push_back(block);
    block += item.mesh->fillRuns.size() + item.mesh->segmentRuns.size();
  }
  drawFills(source, style, firstBlock);
  drawSegments(source, style, firstBlock);
  m_statistics.tilesDrawn += static_cast<int32_t>(source.items.size());
}

void VectorRenderer::drawFills(const Source& source, const VectorStyle& style,
                               const std::vector<std::size_t>& firstBlock)
{
  bool pipeline = false;
  for (std::size_t i = 0; i < source.items.size(); ++i) {
    const GpuMesh& mesh = *source.items[i].mesh;
    for (std::size_t r = 0; r < mesh.fillRuns.size() && mesh.fill; ++r) {
      const VectorRun run = mesh.fillRuns[r];
      if (!runLook(mesh.styles, run, style).showsFill) {
        continue;
      }
      if (!pipeline) {
        m_device.setPipeline(m_morph > 0.0 ? *m_fillMapPipeline
                                           : *m_fillPipeline);
        pipeline = true;
      }
      m_device.setVertexBuffer(0, *mesh.fill, 0);
      m_device.setUniformBuffer(vectorBinding, *m_uniforms,
                                (firstBlock[i] + r) * m_uniformStride,
                                sizeof(VectorBlock));
      m_device.draw(run.count, run.first, 1);
      ++m_statistics.drawCalls;
      m_statistics.triangles += run.count / 3;
    }
  }
}

void VectorRenderer::drawSegments(const Source& source,
                                  const VectorStyle& style,
                                  const std::vector<std::size_t>& firstBlock)
{
  bool pipeline = false;
  for (std::size_t i = 0; i < source.items.size(); ++i) {
    const GpuMesh& mesh = *source.items[i].mesh;
    for (std::size_t r = 0; r < mesh.segmentRuns.size() && mesh.segments; ++r) {
      const VectorRun run = mesh.segmentRuns[r];
      if (!runLook(mesh.styles, run, style).showsLine) {
        continue;
      }
      if (!pipeline) {
        m_device.setPipeline(m_morph > 0.0 ? *m_lineMapPipeline
                                           : *m_linePipeline);
        pipeline = true;
      }
      // Instanced: the run's segments start at its offset in the buffer.
      m_device.setVertexBuffer(0, *mesh.segments,
                               static_cast<std::size_t>(run.first) *
                                   sizeof(VectorSegment));
      m_device.setUniformBuffer(vectorBinding, *m_uniforms,
                                (firstBlock[i] + mesh.fillRuns.size() + r) *
                                    m_uniformStride,
                                sizeof(VectorBlock));
      m_device.draw(4, 0, run.count);
      ++m_statistics.drawCalls;
      m_statistics.segments += run.count;
    }
  }
}

} // namespace ts
