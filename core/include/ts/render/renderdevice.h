// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The renderer backend interface: GPU resources and draw submission.
 */
#ifndef TS_RENDER_RENDERDEVICE_H
#define TS_RENDER_RENDERDEVICE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <QByteArray>
#include <QImage>
#include <QRect>
#include <QSize>
#include <QString>

namespace ts
{

/** What a GPU buffer holds. */
enum class BufferType {
  /** Vertex attributes, per vertex or per instance. */
  Vertex,
  /** 16- or 32-bit vertex indices. */
  Index,
  /** A uniform block (std140 layout). */
  Uniform,
};

/** How often a buffer's contents change. */
enum class BufferUsage {
  /** Written once (or rarely). */
  Static,
  /** Rewritten every frame or so. */
  Dynamic,
};

/** Type of one vertex attribute in a buffer. */
enum class VertexFormat {
  /** One 32-bit float. */
  Float,
  /** Two 32-bit floats. */
  Float2,
  /** Three 32-bit floats. */
  Float3,
  /** Four 32-bit floats. */
  Float4,
  /** Four unsigned bytes, read as floats in [0, 1] (e.g. a colour). */
  UNorm8x4,
};

/** Size of the indices in an index buffer. */
enum class IndexFormat {
  /** 16-bit unsigned indices. */
  UInt16,
  /** 32-bit unsigned indices. */
  UInt32,
};

/** How vertices are assembled into primitives. */
enum class PrimitiveTopology {
  /** Every three vertices form a triangle. */
  Triangles,
  /** Each vertex after the first two forms a triangle with the previous two. */
  TriangleStrip,
  /** Every two vertices form a line. */
  Lines,
  /** Each vertex after the first continues the line. */
  LineStrip,
  /** Each vertex is a point. */
  Points,
};

/** Which triangles are discarded. Front faces are counter-clockwise. */
enum class CullMode {
  /** Draw all triangles. */
  None,
  /** Discard clockwise (back-facing) triangles. */
  Back,
  /** Discard counter-clockwise (front-facing) triangles. */
  Front,
};

/** Depth comparison: a fragment passes if `fragment OP stored` holds. */
enum class CompareOp {
  /** Never passes. */
  Never,
  /** Passes if nearer (smaller depth). */
  Less,
  /** Passes if nearer or equal. */
  LessOrEqual,
  /** Passes if equal. */
  Equal,
  /** Passes if farther. */
  Greater,
  /** Passes if farther or equal. */
  GreaterOrEqual,
  /** Always passes. */
  Always,
};

/** How a fragment's colour combines with the colour already there. */
enum class BlendMode {
  /** Replace. */
  None,
  /** Source colour already multiplied by its alpha (`src + dst (1 − αs)`). */
  PremultipliedAlpha,
};

/** Pixel format of a texture. */
enum class TextureFormat {
  /** 8-bit red, green, blue, alpha. */
  Rgba8,
  /** One 8-bit channel, read as red. */
  R8,
};

/** Texture filtering for magnification and minification. */
enum class TextureFilter {
  /** Nearest texel. */
  Nearest,
  /** Bilinear; trilinear between mipmap levels when the texture has them. */
  Linear,
};

/** What texture coordinates outside [0, 1] read. */
enum class TextureWrap {
  /** The edge texel. */
  ClampToEdge,
  /** The texture repeats. */
  Repeat,
};

/**
 * A GPU buffer: vertices, indices or uniforms.
 *
 * Created by RenderDevice::createBuffer(). Must be destroyed while its
 * device exists, on the render thread.
 *
 * @renderthread
 */
class GpuBuffer
{
public:
  /** Releases the GPU memory. */
  virtual ~GpuBuffer();

  GpuBuffer(const GpuBuffer&) = delete;
  GpuBuffer& operator=(const GpuBuffer&) = delete;
  GpuBuffer(GpuBuffer&&) = delete;
  GpuBuffer& operator=(GpuBuffer&&) = delete;

  /** Returns what the buffer holds. */
  BufferType type() const { return m_type; }

  /** Returns the size in bytes. */
  std::size_t sizeBytes() const { return m_sizeBytes; }

  /**
   * Replaces @p sizeBytes bytes at @p offsetBytes with @p data. The range
   * must lie inside the buffer.
   */
  virtual void update(std::size_t offsetBytes, const void* data,
                      std::size_t sizeBytes) = 0;

protected:
  /** For backends: a buffer of @p type and @p sizeBytes. */
  GpuBuffer(BufferType type, std::size_t sizeBytes);

private:
  BufferType m_type;
  std::size_t m_sizeBytes;
};

/** Size, format and sampling of a 2D texture. */
struct TextureDesc {
  /** Width in texels. */
  int width = 0;
  /** Height in texels. */
  int height = 0;
  /** Pixel format. */
  TextureFormat format = TextureFormat::Rgba8;
  /** Filtering. */
  TextureFilter filter = TextureFilter::Linear;
  /** Behaviour outside [0, 1]. */
  TextureWrap wrap = TextureWrap::ClampToEdge;
  /** True to keep a full mipmap chain, regenerated on every upload. */
  bool mipmaps = false;
};

/**
 * A 2D texture.
 *
 * @renderthread
 */
class GpuTexture
{
public:
  /** Releases the GPU memory. */
  virtual ~GpuTexture();

  GpuTexture(const GpuTexture&) = delete;
  GpuTexture& operator=(const GpuTexture&) = delete;
  GpuTexture(GpuTexture&&) = delete;
  GpuTexture& operator=(GpuTexture&&) = delete;

  /** Returns the size, format and sampling it was created with. */
  const TextureDesc& desc() const { return m_desc; }

  /**
   * Replaces the whole texture with @p image, which must have the
   * texture's size. Converted to the texture's format if needed; row 0 of
   * the image is texture coordinate t = 0.
   */
  virtual void upload(const QImage& image) = 0;

protected:
  /** For backends: a texture described by @p desc. */
  explicit GpuTexture(const TextureDesc& desc);

private:
  TextureDesc m_desc;
};

/** A uniform block in a shader and the binding point it reads from. */
struct UniformBlockBinding {
  /** Block name in the GLSL source. */
  QByteArray name;
  /** Binding point, as passed to RenderDevice::setUniformBuffer(). */
  int binding = 0;
};

/** A sampler in a shader and the texture unit it reads from. */
struct SamplerBinding {
  /** Sampler uniform name in the GLSL source. */
  QByteArray name;
  /** Texture unit, as passed to RenderDevice::setTexture(). */
  int unit = 0;
};

/**
 * Shader sources and their bindings.
 *
 * Sources are GLSL 3.30 core without the `#version` line, which the
 * backend adds. Vertex inputs use `layout(location = N)`; uniforms are in
 * std140 uniform blocks, so the same layout maps to a future backend's
 * uniform buffers (see docs/design/renderer-backend.md).
 */
struct ShaderDesc {
  /** Name for error messages. */
  QString label;
  /** Vertex shader source. */
  QByteArray vertexSource;
  /** Fragment shader source. */
  QByteArray fragmentSource;
  /** Uniform blocks to bind. */
  std::vector<UniformBlockBinding> uniformBlocks;
  /** Samplers to bind. */
  std::vector<SamplerBinding> samplers;
};

/**
 * A compiled and linked shader program.
 *
 * @renderthread
 */
class ShaderProgram
{
public:
  /** Releases the program. */
  virtual ~ShaderProgram();

  ShaderProgram(const ShaderProgram&) = delete;
  ShaderProgram& operator=(const ShaderProgram&) = delete;
  ShaderProgram(ShaderProgram&&) = delete;
  ShaderProgram& operator=(ShaderProgram&&) = delete;

protected:
  /** For backends. */
  ShaderProgram() = default;
};

/** Stride and step rate of one vertex buffer slot. */
struct VertexBufferLayout {
  /** Bytes from one vertex (or instance) to the next. */
  std::uint32_t strideBytes = 0;
  /** True if the slot advances per instance instead of per vertex. */
  bool perInstance = false;
};

/** One vertex shader input and where it comes from. */
struct VertexAttribute {
  /** `layout(location = N)` of the input. */
  int location = 0;
  /** Type in the buffer. */
  VertexFormat format = VertexFormat::Float3;
  /** Index into PipelineDesc::bufferLayouts. */
  int bufferSlot = 0;
  /** Offset from the start of the vertex in bytes. */
  std::uint32_t offsetBytes = 0;
};

/** Everything fixed about a draw except its buffers, textures and count. */
struct PipelineDesc {
  /** The program; must outlive the pipeline. */
  const ShaderProgram* shader = nullptr;
  /** One entry per vertex buffer slot. */
  std::vector<VertexBufferLayout> bufferLayouts;
  /** The vertex inputs. */
  std::vector<VertexAttribute> attributes;
  /** Primitive assembly. */
  PrimitiveTopology topology = PrimitiveTopology::Triangles;
  /** Face culling. */
  CullMode cullMode = CullMode::None;
  /** True to test fragments against the depth buffer. */
  bool depthTest = false;
  /** True to write depth (only with depthTest). */
  bool depthWrite = false;
  /** Depth comparison when depthTest is on. */
  CompareOp depthCompare = CompareOp::LessOrEqual;
  /** Colour blending. */
  BlendMode blend = BlendMode::None;
};

/**
 * A pipeline state object.
 *
 * @renderthread
 */
class Pipeline
{
public:
  /** Releases the pipeline. */
  virtual ~Pipeline();

  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;
  Pipeline(Pipeline&&) = delete;
  Pipeline& operator=(Pipeline&&) = delete;

  /** Returns the description it was created from. */
  const PipelineDesc& desc() const { return m_desc; }

protected:
  /** For backends: a pipeline described by @p desc. */
  explicit Pipeline(PipelineDesc desc);

private:
  PipelineDesc m_desc;
};

/**
 * An offscreen colour (RGBA 8-bit) and depth target: for drawing a
 * group of layers to composite it later, and for tests, golden images and
 * screenshots.
 *
 * @renderthread
 */
class RenderTarget
{
public:
  /** Releases the target. */
  virtual ~RenderTarget();

  RenderTarget(const RenderTarget&) = delete;
  RenderTarget& operator=(const RenderTarget&) = delete;
  RenderTarget(RenderTarget&&) = delete;
  RenderTarget& operator=(RenderTarget&&) = delete;

  /** Returns the width in pixels. */
  int width() const { return m_width; }

  /** Returns the height in pixels. */
  int height() const { return m_height; }

protected:
  /** For backends: a target of @p sizePx pixels. */
  explicit RenderTarget(QSize sizePx);

private:
  int m_width;
  int m_height;
};

/** Where a render pass draws and what it clears first. */
struct PassDesc {
  /** The offscreen target, or null for the surface's default framebuffer. */
  const RenderTarget* target = nullptr;
  /** Viewport in device pixels, origin at the top left. */
  QRect viewportPx;
  /** True to clear the colour to clearColor. */
  bool clearColor = true;
  /** Clear colour, linear RGBA in [0, 1], not premultiplied. */
  std::array<float, 4> clearColorRgba = {0.0F, 0.0F, 0.0F, 1.0F};
  /** True to clear the depth to clearDepth. */
  bool clearDepth = true;
  /** Depth clear value: 1 is the far plane. */
  float clearDepthValue = 1.0F;
};

/** What a device runs on. */
struct RenderDeviceInfo {
  /** Backend name, e.g. "OpenGL 3.3 core". */
  QString backend;
  /** Driver vendor string. */
  QString vendor;
  /** Renderer (GPU) string. */
  QString renderer;
  /** Driver version string. */
  QString version;
  /** True if the renderer is a software rasteriser (see isSoftwareRenderer). */
  bool softwareRenderer = false;
  /** Largest texture width or height the device supports. */
  int maxTextureSize = 0;
};

/**
 * The renderer backend: creates GPU resources and submits draws.
 *
 * Rendering code (the globe, later tiles and vectors) is written against
 * this interface only, so a second backend (Qt RHI for Vulkan, Metal and
 * Direct3D) can be added without changing it. The OpenGL 3.3 core
 * implementation is GlRenderDevice.
 *
 * A frame is a sequence of passes: beginPass(), then any number of
 * setPipeline(), set...() and draw calls, then endPass(). State set
 * between draws persists until changed or until the pass ends.
 *
 * Resources created by a device must be destroyed before it, on the same
 * thread.
 *
 * @renderthread
 */
class RenderDevice
{
public:
  /** Destroys the device. Its resources must be gone already. */
  virtual ~RenderDevice();

  RenderDevice(const RenderDevice&) = delete;
  RenderDevice& operator=(const RenderDevice&) = delete;
  RenderDevice(RenderDevice&&) = delete;
  RenderDevice& operator=(RenderDevice&&) = delete;

  /** Returns what the device runs on. */
  virtual RenderDeviceInfo info() const = 0;

  /**
   * Creates a buffer of @p sizeBytes bytes, filled from @p data if it is
   * not null. Returns null on failure.
   */
  virtual std::unique_ptr<GpuBuffer> createBuffer(BufferType type,
                                                  BufferUsage usage,
                                                  std::size_t sizeBytes,
                                                  const void* data) = 0;

  /** Creates a texture; its contents are undefined until uploaded. */
  virtual std::unique_ptr<GpuTexture>
  createTexture(const TextureDesc& desc) = 0;

  /**
   * Compiles and links @p desc. On failure returns null and, if
   * @p errorString is not null, sets it to the compiler's log.
   */
  virtual std::unique_ptr<ShaderProgram>
  createShaderProgram(const ShaderDesc& desc, QString* errorString) = 0;

  /** Creates a pipeline; returns null if @p desc is invalid. */
  virtual std::unique_ptr<Pipeline>
  createPipeline(const PipelineDesc& desc) = 0;

  /** Creates an offscreen target; returns null on failure. */
  virtual std::unique_ptr<RenderTarget> createRenderTarget(int width,
                                                           int height) = 0;

  /** Starts a pass: binds the target, sets the viewport and clears. */
  virtual void beginPass(const PassDesc& pass) = 0;

  /** Selects the pipeline for the following draws. */
  virtual void setPipeline(const Pipeline& pipeline) = 0;

  /** Binds @p buffer (a vertex buffer) to vertex buffer @p slot. */
  virtual void setVertexBuffer(int slot, const GpuBuffer& buffer,
                               std::size_t offsetBytes) = 0;

  /** Binds @p buffer as the index buffer. */
  virtual void setIndexBuffer(const GpuBuffer& buffer, IndexFormat format) = 0;

  /**
   * Binds @p sizeBytes bytes of @p buffer (a uniform buffer) from
   * @p offsetBytes to uniform block binding point @p binding.
   */
  virtual void setUniformBuffer(int binding, const GpuBuffer& buffer,
                                std::size_t offsetBytes,
                                std::size_t sizeBytes) = 0;

  /** Binds @p texture to texture unit @p unit. */
  virtual void setTexture(int unit, const GpuTexture& texture) = 0;

  /**
   * Binds the colour of @p target to texture unit @p unit (bilinear,
   * clamped), to draw what was rendered offscreen. The target must not be
   * the one being drawn into.
   */
  virtual void setRenderTargetTexture(int unit, const RenderTarget& target) = 0;

  /** Draws @p vertexCount vertices from @p firstVertex, @p instances times. */
  virtual void draw(int vertexCount, int firstVertex, int instances) = 0;

  /**
   * Draws @p indexCount indices from @p firstIndex of the index buffer,
   * @p instances times.
   */
  virtual void drawIndexed(int indexCount, int firstIndex, int instances) = 0;

  /** Ends the pass and leaves the device ready for other code to draw. */
  virtual void endPass() = 0;

  /**
   * Returns the contents of @p target as an image (top row first), or a
   * null image on failure. Waits for the GPU; for tests and screenshots.
   */
  virtual QImage readPixels(const RenderTarget& target) = 0;

  /**
   * Returns and clears the errors the backend has seen since the last call
   * (for OpenGL, glGetError()). Empty if there were none.
   */
  virtual QString takeErrors() = 0;

  /** Returns the uniform buffer offset alignment in bytes (at least 16). */
  virtual std::size_t uniformBufferAlignment() const = 0;

protected:
  /** For backends. */
  RenderDevice() = default;
};

} // namespace ts

#endif // TS_RENDER_RENDERDEVICE_H
