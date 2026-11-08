// SPDX-License-Identifier: GPL-2.0-or-later
#include "ts/render/glrenderdevice.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include <QCoreApplication>
#include <QOpenGLContext>
#include <QOpenGLFunctions_3_3_Core>
#include <QSurfaceFormat>

#include "ts/gl/glprobe.h"

namespace ts
{

/** The OpenGL 3.3 core entry points, resolved for one context. */
class GlFunctions : public QOpenGLFunctions_3_3_Core
{
};

namespace
{

// --- Enum mapping
// -------------------------------------------------------------

struct GlVertexFormat {
  GLint components;
  GLenum type;
  GLboolean normalized;
};

GlVertexFormat glVertexFormat(VertexFormat format)
{
  switch (format) {
  case VertexFormat::Float:
    return {1, GL_FLOAT, GL_FALSE};
  case VertexFormat::Float2:
    return {2, GL_FLOAT, GL_FALSE};
  case VertexFormat::Float3:
    return {3, GL_FLOAT, GL_FALSE};
  case VertexFormat::Float4:
    return {4, GL_FLOAT, GL_FALSE};
  case VertexFormat::UNorm8x4:
    return {4, GL_UNSIGNED_BYTE, GL_TRUE};
  }
  return {4, GL_FLOAT, GL_FALSE};
}

GLenum glTopology(PrimitiveTopology topology)
{
  switch (topology) {
  case PrimitiveTopology::Triangles:
    return GL_TRIANGLES;
  case PrimitiveTopology::TriangleStrip:
    return GL_TRIANGLE_STRIP;
  case PrimitiveTopology::Lines:
    return GL_LINES;
  case PrimitiveTopology::LineStrip:
    return GL_LINE_STRIP;
  case PrimitiveTopology::Points:
    return GL_POINTS;
  }
  return GL_TRIANGLES;
}

GLenum glCompare(CompareOp op)
{
  switch (op) {
  case CompareOp::Never:
    return GL_NEVER;
  case CompareOp::Less:
    return GL_LESS;
  case CompareOp::LessOrEqual:
    return GL_LEQUAL;
  case CompareOp::Equal:
    return GL_EQUAL;
  case CompareOp::Greater:
    return GL_GREATER;
  case CompareOp::GreaterOrEqual:
    return GL_GEQUAL;
  case CompareOp::Always:
    return GL_ALWAYS;
  }
  return GL_LEQUAL;
}

GLenum glBufferUsage(BufferUsage usage)
{
  return usage == BufferUsage::Static ? GL_STATIC_DRAW : GL_DYNAMIC_DRAW;
}

const void* glOffset(std::size_t offsetBytes)
{
  // OpenGL takes buffer offsets as pointers.
  // NOLINTNEXTLINE(performance-no-int-to-ptr): GL's offset-as-pointer API
  return reinterpret_cast<const void*>(offsetBytes);
}

QString glString(GlFunctions& gl, GLenum name)
{
  const GLubyte* value = gl.glGetString(name);
  return value ? QString::fromLatin1(reinterpret_cast<const char*>(value))
               : QString();
}

// --- Resources
// ----------------------------------------------------------------

class GlBuffer : public GpuBuffer
{
public:
  GlBuffer(GlFunctions& gl, BufferType type, BufferUsage usage,
           std::size_t sizeBytes, const void* data)
      : GpuBuffer(type, sizeBytes), m_gl(gl)
  {
    m_gl.glGenBuffers(1, &m_id);
    // GL_COPY_WRITE_BUFFER binds without touching vertex array state.
    m_gl.glBindBuffer(GL_COPY_WRITE_BUFFER, m_id);
    m_gl.glBufferData(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(sizeBytes),
                      data, glBufferUsage(usage));
    m_gl.glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
  }

  ~GlBuffer() override { m_gl.glDeleteBuffers(1, &m_id); }

  GlBuffer(const GlBuffer&) = delete;
  GlBuffer& operator=(const GlBuffer&) = delete;
  GlBuffer(GlBuffer&&) = delete;
  GlBuffer& operator=(GlBuffer&&) = delete;

  void update(std::size_t offsetBytes, const void* data,
              std::size_t sizeBytes) override
  {
    Q_ASSERT(offsetBytes + sizeBytes <= this->sizeBytes());
    m_gl.glBindBuffer(GL_COPY_WRITE_BUFFER, m_id);
    m_gl.glBufferSubData(GL_COPY_WRITE_BUFFER,
                         static_cast<GLintptr>(offsetBytes),
                         static_cast<GLsizeiptr>(sizeBytes), data);
    m_gl.glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
  }

  GLuint id() const { return m_id; }

private:
  GlFunctions& m_gl;
  GLuint m_id = 0;
};

class GlTexture : public GpuTexture
{
public:
  GlTexture(GlFunctions& gl, const TextureDesc& desc)
      : GpuTexture(desc), m_gl(gl)
  {
    m_gl.glGenTextures(1, &m_id);
    m_gl.glBindTexture(GL_TEXTURE_2D, m_id);
    const bool rgba = desc.format == TextureFormat::Rgba8;
    m_gl.glTexImage2D(GL_TEXTURE_2D, 0, rgba ? GL_RGBA8 : GL_R8, desc.width,
                      desc.height, 0, rgba ? GL_RGBA : GL_RED, GL_UNSIGNED_BYTE,
                      nullptr);
    const GLint mag =
        desc.filter == TextureFilter::Linear ? GL_LINEAR : GL_NEAREST;
    GLint min = mag;
    if (desc.mipmaps) {
      min = desc.filter == TextureFilter::Linear ? GL_LINEAR_MIPMAP_LINEAR
                                                 : GL_NEAREST_MIPMAP_NEAREST;
    } else {
      m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }
    m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, mag);
    m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min);
    const GLint wrap =
        desc.wrap == TextureWrap::Repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE;
    m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
    m_gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
    m_gl.glBindTexture(GL_TEXTURE_2D, 0);
  }

  ~GlTexture() override { m_gl.glDeleteTextures(1, &m_id); }

  GlTexture(const GlTexture&) = delete;
  GlTexture& operator=(const GlTexture&) = delete;
  GlTexture(GlTexture&&) = delete;
  GlTexture& operator=(GlTexture&&) = delete;

  void upload(const QImage& image) override
  {
    const TextureDesc& d = desc();
    if (image.width() != d.width || image.height() != d.height) {
      qWarning("GlTexture::upload: image is %dx%d, texture %dx%d",
               image.width(), image.height(), d.width, d.height);
      return;
    }
    const bool rgba = d.format == TextureFormat::Rgba8;
    const QImage pixels = image.convertToFormat(
        rgba ? QImage::Format_RGBA8888 : QImage::Format_Grayscale8);
    const int bytesPerPixel = rgba ? 4 : 1;
    m_gl.glBindTexture(GL_TEXTURE_2D, m_id);
    m_gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    m_gl.glPixelStorei(GL_UNPACK_ROW_LENGTH,
                       static_cast<GLint>(pixels.bytesPerLine()) /
                           bytesPerPixel);
    m_gl.glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, d.width, d.height,
                         rgba ? GL_RGBA : GL_RED, GL_UNSIGNED_BYTE,
                         pixels.constBits());
    m_gl.glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    m_gl.glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (d.mipmaps) {
      m_gl.glGenerateMipmap(GL_TEXTURE_2D);
    }
    m_gl.glBindTexture(GL_TEXTURE_2D, 0);
  }

  GLuint id() const { return m_id; }

private:
  GlFunctions& m_gl;
  GLuint m_id = 0;
};

class GlShaderProgram : public ShaderProgram
{
public:
  GlShaderProgram(GlFunctions& gl, GLuint id) : m_gl(gl), m_id(id) {}

  ~GlShaderProgram() override { m_gl.glDeleteProgram(m_id); }

  GlShaderProgram(const GlShaderProgram&) = delete;
  GlShaderProgram& operator=(const GlShaderProgram&) = delete;
  GlShaderProgram(GlShaderProgram&&) = delete;
  GlShaderProgram& operator=(GlShaderProgram&&) = delete;

  GLuint id() const { return m_id; }

private:
  GlFunctions& m_gl;
  GLuint m_id;
};

class GlPipeline : public Pipeline
{
public:
  GlPipeline(GlFunctions& gl, PipelineDesc desc)
      : Pipeline(std::move(desc)), m_gl(gl)
  {
    m_gl.glGenVertexArrays(1, &m_vertexArray);
  }

  ~GlPipeline() override { m_gl.glDeleteVertexArrays(1, &m_vertexArray); }

  GlPipeline(const GlPipeline&) = delete;
  GlPipeline& operator=(const GlPipeline&) = delete;
  GlPipeline(GlPipeline&&) = delete;
  GlPipeline& operator=(GlPipeline&&) = delete;

  GLuint vertexArray() const { return m_vertexArray; }

private:
  GlFunctions& m_gl;
  GLuint m_vertexArray = 0;
};

class GlRenderTarget : public RenderTarget
{
public:
  GlRenderTarget(GlFunctions& gl, int width, int height)
      : RenderTarget(QSize(width, height)), m_gl(gl)
  {
    m_gl.glGenRenderbuffers(1, &m_color);
    m_gl.glBindRenderbuffer(GL_RENDERBUFFER, m_color);
    m_gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    m_gl.glGenRenderbuffers(1, &m_depth);
    m_gl.glBindRenderbuffer(GL_RENDERBUFFER, m_depth);
    m_gl.glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width,
                               height);
    m_gl.glBindRenderbuffer(GL_RENDERBUFFER, 0);

    GLint previous = 0;
    m_gl.glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous);
    m_gl.glGenFramebuffers(1, &m_framebuffer);
    m_gl.glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
    m_gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_RENDERBUFFER, m_color);
    m_gl.glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                   GL_RENDERBUFFER, m_depth);
    // Known only once the attachments exist:
    // NOLINTNEXTLINE(cppcoreguidelines-prefer-member-initializer)
    m_complete = m_gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
                 GL_FRAMEBUFFER_COMPLETE;
    m_gl.glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
  }

  ~GlRenderTarget() override
  {
    m_gl.glDeleteFramebuffers(1, &m_framebuffer);
    m_gl.glDeleteRenderbuffers(1, &m_depth);
    m_gl.glDeleteRenderbuffers(1, &m_color);
  }

  GlRenderTarget(const GlRenderTarget&) = delete;
  GlRenderTarget& operator=(const GlRenderTarget&) = delete;
  GlRenderTarget(GlRenderTarget&&) = delete;
  GlRenderTarget& operator=(GlRenderTarget&&) = delete;

  bool isComplete() const { return m_complete; }
  GLuint framebuffer() const { return m_framebuffer; }

private:
  GlFunctions& m_gl;
  GLuint m_color = 0;
  GLuint m_depth = 0;
  GLuint m_framebuffer = 0;
  bool m_complete = false;
};

/** Compiles one stage; returns 0 and appends to @p log on failure. */
GLuint compileStage(GlFunctions& gl, GLenum stage, const QByteArray& source,
                    QString* log)
{
  const QByteArray full = QByteArrayLiteral("#version 330 core\n") + source;
  const char* text = full.constData();
  const GLuint shader = gl.glCreateShader(stage);
  gl.glShaderSource(shader, 1, &text, nullptr);
  gl.glCompileShader(shader);
  GLint ok = GL_FALSE;
  gl.glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (ok == GL_TRUE) {
    return shader;
  }
  GLint length = 0;
  gl.glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &length);
  QByteArray message(std::max(length, 1), '\0');
  gl.glGetShaderInfoLog(shader, length, nullptr, message.data());
  if (log) {
    *log += QString::fromUtf8(message.constData());
  }
  gl.glDeleteShader(shader);
  return 0;
}

std::unique_ptr<GlRenderDevice> failCreate(QString* errorString,
                                           const QString& message)
{
  if (errorString) {
    *errorString = message;
  }
  return nullptr;
}

} // namespace

// --- Base classes
// -------------------------------------------------------------

GpuBuffer::GpuBuffer(BufferType type, std::size_t sizeBytes)
    : m_type(type), m_sizeBytes(sizeBytes)
{
}

GpuBuffer::~GpuBuffer() = default;

GpuTexture::GpuTexture(const TextureDesc& desc) : m_desc(desc) {}

GpuTexture::~GpuTexture() = default;

ShaderProgram::~ShaderProgram() = default;

Pipeline::Pipeline(PipelineDesc desc) : m_desc(std::move(desc)) {}

Pipeline::~Pipeline() = default;

RenderTarget::RenderTarget(QSize sizePx)
    : m_width(sizePx.width()), m_height(sizePx.height())
{
}

RenderTarget::~RenderTarget() = default;

RenderDevice::~RenderDevice() = default;

// --- GlRenderDevice
// -----------------------------------------------------------

/** Bindings of the current pass. */
struct GlRenderDevice::State {
  /** One vertex buffer slot. */
  struct VertexSlot {
    const GlBuffer* buffer = nullptr;
    std::size_t offsetBytes = 0;
  };

  GLuint defaultFramebuffer = 0;
  QSize defaultSizePx;
  const GlPipeline* pipeline = nullptr;
  std::vector<VertexSlot> vertexSlots;
  const GlBuffer* indexBuffer = nullptr;
  IndexFormat indexFormat = IndexFormat::UInt32;
  bool vertexInputsDirty = true;
};

std::unique_ptr<GlRenderDevice> GlRenderDevice::create(QOpenGLContext* context,
                                                       QString* errorString)
{
  if (!context || QOpenGLContext::currentContext() != context) {
    return failCreate(errorString, QCoreApplication::translate(
                                       "ts::GlRenderDevice",
                                       "The OpenGL context is not current."));
  }
  const QSurfaceFormat format = context->format();
  if (context->isOpenGLES() || format.version() < qMakePair(3, 3) ||
      format.profile() == QSurfaceFormat::CompatibilityProfile) {
    return failCreate(errorString,
                      QCoreApplication::translate(
                          "ts::GlRenderDevice",
                          "OpenGL 3.3 core profile is needed; this context is "
                          "OpenGL %1.%2.")
                          .arg(format.majorVersion())
                          .arg(format.minorVersion()));
  }
  std::unique_ptr<GlFunctions> gl = std::make_unique<GlFunctions>();
  if (!gl->initializeOpenGLFunctions()) {
    return failCreate(errorString,
                      QCoreApplication::translate(
                          "ts::GlRenderDevice",
                          "The OpenGL 3.3 functions could not be loaded."));
  }
  RenderDeviceInfo info;
  info.backend = QStringLiteral("OpenGL 3.3 core");
  info.vendor = glString(*gl, GL_VENDOR);
  info.renderer = glString(*gl, GL_RENDERER);
  info.version = glString(*gl, GL_VERSION);
  info.softwareRenderer = isSoftwareRenderer(info.renderer);
  GLint maxTextureSize = 0;
  gl->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTextureSize);
  info.maxTextureSize = maxTextureSize;
  // The constructor is private, so std::make_unique can't call it.
  return std::unique_ptr<GlRenderDevice>(
      new GlRenderDevice(std::move(gl), std::move(info)));
}

GlRenderDevice::GlRenderDevice(std::unique_ptr<GlFunctions> functions,
                               RenderDeviceInfo info)
    : m_gl(std::move(functions)), m_state(std::make_unique<State>()),
      m_info(std::move(info))
{
}

GlRenderDevice::~GlRenderDevice() = default;

void GlRenderDevice::setDefaultFramebuffer(unsigned int framebufferId,
                                           QSize sizePx)
{
  m_state->defaultFramebuffer = framebufferId;
  m_state->defaultSizePx = sizePx;
}

RenderDeviceInfo GlRenderDevice::info() const
{
  return m_info;
}

std::unique_ptr<GpuBuffer> GlRenderDevice::createBuffer(BufferType type,
                                                        BufferUsage usage,
                                                        std::size_t sizeBytes,
                                                        const void* data)
{
  if (sizeBytes == 0) {
    return nullptr;
  }
  return std::make_unique<GlBuffer>(*m_gl, type, usage, sizeBytes, data);
}

std::unique_ptr<GpuTexture>
GlRenderDevice::createTexture(const TextureDesc& desc)
{
  if (desc.width <= 0 || desc.height <= 0 ||
      desc.width > m_info.maxTextureSize ||
      desc.height > m_info.maxTextureSize) {
    return nullptr;
  }
  return std::make_unique<GlTexture>(*m_gl, desc);
}

std::unique_ptr<ShaderProgram>
GlRenderDevice::createShaderProgram(const ShaderDesc& desc,
                                    QString* errorString)
{
  QString log;
  const GLuint vertex =
      compileStage(*m_gl, GL_VERTEX_SHADER, desc.vertexSource, &log);
  const GLuint fragment =
      compileStage(*m_gl, GL_FRAGMENT_SHADER, desc.fragmentSource, &log);
  if (vertex == 0 || fragment == 0) {
    m_gl->glDeleteShader(vertex);
    m_gl->glDeleteShader(fragment);
    if (errorString) {
      *errorString = desc.label + QStringLiteral(": ") + log;
    }
    return nullptr;
  }
  const GLuint program = m_gl->glCreateProgram();
  m_gl->glAttachShader(program, vertex);
  m_gl->glAttachShader(program, fragment);
  m_gl->glLinkProgram(program);
  m_gl->glDeleteShader(vertex);
  m_gl->glDeleteShader(fragment);
  GLint ok = GL_FALSE;
  m_gl->glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (ok != GL_TRUE) {
    GLint length = 0;
    m_gl->glGetProgramiv(program, GL_INFO_LOG_LENGTH, &length);
    QByteArray message(std::max(length, 1), '\0');
    m_gl->glGetProgramInfoLog(program, length, nullptr, message.data());
    m_gl->glDeleteProgram(program);
    if (errorString) {
      *errorString =
          desc.label + QStringLiteral(": ") + QString::fromUtf8(message);
    }
    return nullptr;
  }
  for (const UniformBlockBinding& block : desc.uniformBlocks) {
    const GLuint index =
        m_gl->glGetUniformBlockIndex(program, block.name.constData());
    if (index != GL_INVALID_INDEX) {
      m_gl->glUniformBlockBinding(program, index,
                                  static_cast<GLuint>(block.binding));
    }
  }
  if (!desc.samplers.empty()) {
    m_gl->glUseProgram(program);
    for (const SamplerBinding& sampler : desc.samplers) {
      const GLint location =
          m_gl->glGetUniformLocation(program, sampler.name.constData());
      if (location >= 0) {
        m_gl->glUniform1i(location, sampler.unit);
      }
    }
    m_gl->glUseProgram(0);
  }
  return std::make_unique<GlShaderProgram>(*m_gl, program);
}

std::unique_ptr<Pipeline>
GlRenderDevice::createPipeline(const PipelineDesc& desc)
{
  if (!dynamic_cast<const GlShaderProgram*>(desc.shader)) {
    return nullptr;
  }
  for (const VertexAttribute& attribute : desc.attributes) {
    if (attribute.bufferSlot < 0 ||
        static_cast<std::size_t>(attribute.bufferSlot) >=
            desc.bufferLayouts.size()) {
      return nullptr;
    }
  }
  return std::make_unique<GlPipeline>(*m_gl, desc);
}

std::unique_ptr<RenderTarget> GlRenderDevice::createRenderTarget(int width,
                                                                 int height)
{
  if (width <= 0 || height <= 0) {
    return nullptr;
  }
  std::unique_ptr<GlRenderTarget> target =
      std::make_unique<GlRenderTarget>(*m_gl, width, height);
  if (!target->isComplete()) {
    return nullptr;
  }
  return target;
}

void GlRenderDevice::beginPass(const PassDesc& pass)
{
  const GlRenderTarget* target =
      dynamic_cast<const GlRenderTarget*>(pass.target);
  const GLuint framebuffer =
      target ? target->framebuffer() : m_state->defaultFramebuffer;
  const int heightPx =
      target ? target->height() : m_state->defaultSizePx.height();
  m_gl->glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
  // QRect has its origin at the top left, OpenGL at the bottom left.
  const QRect& v = pass.viewportPx;
  m_gl->glViewport(v.x(), heightPx - v.y() - v.height(), v.width(), v.height());
  m_gl->glDisable(GL_SCISSOR_TEST);
  GLbitfield mask = 0;
  if (pass.clearColor) {
    m_gl->glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    m_gl->glClearColor(pass.clearColorRgba[0], pass.clearColorRgba[1],
                       pass.clearColorRgba[2], pass.clearColorRgba[3]);
    mask |= GL_COLOR_BUFFER_BIT;
  }
  if (pass.clearDepth) {
    m_gl->glDepthMask(GL_TRUE);
    m_gl->glClearDepth(static_cast<GLdouble>(pass.clearDepthValue));
    mask |= GL_DEPTH_BUFFER_BIT;
  }
  if (mask != 0) {
    m_gl->glClear(mask);
  }
  m_state->pipeline = nullptr;
  m_state->vertexSlots.clear();
  m_state->indexBuffer = nullptr;
  m_state->vertexInputsDirty = true;
}

void GlRenderDevice::setPipeline(const Pipeline& pipeline)
{
  const GlPipeline* gl = dynamic_cast<const GlPipeline*>(&pipeline);
  if (!gl) {
    return;
  }
  const PipelineDesc& desc = gl->desc();
  const GlShaderProgram* shader =
      dynamic_cast<const GlShaderProgram*>(desc.shader);
  m_gl->glUseProgram(shader ? shader->id() : 0);
  m_gl->glBindVertexArray(gl->vertexArray());

  if (desc.depthTest) {
    m_gl->glEnable(GL_DEPTH_TEST);
    m_gl->glDepthFunc(glCompare(desc.depthCompare));
    m_gl->glDepthMask(desc.depthWrite ? GL_TRUE : GL_FALSE);
  } else {
    m_gl->glDisable(GL_DEPTH_TEST);
    m_gl->glDepthMask(GL_FALSE);
  }
  if (desc.cullMode == CullMode::None) {
    m_gl->glDisable(GL_CULL_FACE);
  } else {
    m_gl->glEnable(GL_CULL_FACE);
    m_gl->glFrontFace(GL_CCW);
    m_gl->glCullFace(desc.cullMode == CullMode::Back ? GL_BACK : GL_FRONT);
  }
  if (desc.blend == BlendMode::PremultipliedAlpha) {
    m_gl->glEnable(GL_BLEND);
    m_gl->glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  } else {
    m_gl->glDisable(GL_BLEND);
  }
  m_state->pipeline = gl;
  m_state->vertexInputsDirty = true;
}

void GlRenderDevice::setVertexBuffer(int slot, const GpuBuffer& buffer,
                                     std::size_t offsetBytes)
{
  if (slot < 0) {
    return;
  }
  const std::size_t index = static_cast<std::size_t>(slot);
  if (m_state->vertexSlots.size() <= index) {
    m_state->vertexSlots.resize(index + 1);
  }
  m_state->vertexSlots[index] = {dynamic_cast<const GlBuffer*>(&buffer),
                                 offsetBytes};
  m_state->vertexInputsDirty = true;
}

void GlRenderDevice::setIndexBuffer(const GpuBuffer& buffer, IndexFormat format)
{
  m_state->indexBuffer = dynamic_cast<const GlBuffer*>(&buffer);
  m_state->indexFormat = format;
  m_state->vertexInputsDirty = true;
}

void GlRenderDevice::setUniformBuffer(int binding, const GpuBuffer& buffer,
                                      std::size_t offsetBytes,
                                      std::size_t sizeBytes)
{
  const GlBuffer* gl = dynamic_cast<const GlBuffer*>(&buffer);
  if (!gl || binding < 0) {
    return;
  }
  m_gl->glBindBufferRange(GL_UNIFORM_BUFFER, static_cast<GLuint>(binding),
                          gl->id(), static_cast<GLintptr>(offsetBytes),
                          static_cast<GLsizeiptr>(sizeBytes));
}

void GlRenderDevice::setTexture(int unit, const GpuTexture& texture)
{
  const GlTexture* gl = dynamic_cast<const GlTexture*>(&texture);
  if (!gl || unit < 0) {
    return;
  }
  m_gl->glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
  m_gl->glBindTexture(GL_TEXTURE_2D, gl->id());
  m_gl->glActiveTexture(GL_TEXTURE0);
}

void GlRenderDevice::bindVertexInputs()
{
  if (!m_state->vertexInputsDirty || !m_state->pipeline) {
    return;
  }
  // The pipeline's vertex array holds the attribute pointers; they are
  // re-specified whenever the bound buffers change.
  const PipelineDesc& desc = m_state->pipeline->desc();
  for (const VertexAttribute& attribute : desc.attributes) {
    const std::size_t slot = static_cast<std::size_t>(attribute.bufferSlot);
    const GLuint location = static_cast<GLuint>(attribute.location);
    if (slot >= m_state->vertexSlots.size() ||
        !m_state->vertexSlots[slot].buffer) {
      m_gl->glDisableVertexAttribArray(location);
      continue;
    }
    const State::VertexSlot& bound = m_state->vertexSlots[slot];
    const VertexBufferLayout& layout = desc.bufferLayouts[slot];
    const GlVertexFormat format = glVertexFormat(attribute.format);
    m_gl->glBindBuffer(GL_ARRAY_BUFFER, bound.buffer->id());
    m_gl->glVertexAttribPointer(
        location, format.components, format.type, format.normalized,
        static_cast<GLsizei>(layout.strideBytes),
        glOffset(bound.offsetBytes + attribute.offsetBytes));
    m_gl->glVertexAttribDivisor(location, layout.perInstance ? 1U : 0U);
    m_gl->glEnableVertexAttribArray(location);
  }
  m_gl->glBindBuffer(GL_ARRAY_BUFFER, 0);
  m_gl->glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,
                     m_state->indexBuffer ? m_state->indexBuffer->id() : 0);
  m_state->vertexInputsDirty = false;
}

void GlRenderDevice::draw(int vertexCount, int firstVertex, int instances)
{
  if (!m_state->pipeline || vertexCount <= 0 || instances <= 0) {
    return;
  }
  bindVertexInputs();
  m_gl->glDrawArraysInstanced(glTopology(m_state->pipeline->desc().topology),
                              firstVertex, vertexCount, instances);
}

void GlRenderDevice::drawIndexed(int indexCount, int firstIndex, int instances)
{
  if (!m_state->pipeline || !m_state->indexBuffer || indexCount <= 0 ||
      instances <= 0) {
    return;
  }
  bindVertexInputs();
  const bool wide = m_state->indexFormat == IndexFormat::UInt32;
  const std::size_t indexSize = wide ? 4 : 2;
  m_gl->glDrawElementsInstanced(
      glTopology(m_state->pipeline->desc().topology), indexCount,
      wide ? GL_UNSIGNED_INT : GL_UNSIGNED_SHORT,
      glOffset(static_cast<std::size_t>(firstIndex) * indexSize), instances);
}

void GlRenderDevice::endPass()
{
  // Leave the state other OpenGL code (QPainter, Qt's compositor) expects.
  m_gl->glBindVertexArray(0);
  m_gl->glUseProgram(0);
  m_gl->glDisable(GL_DEPTH_TEST);
  m_gl->glDepthMask(GL_TRUE);
  m_gl->glDisable(GL_CULL_FACE);
  m_gl->glDisable(GL_BLEND);
  m_gl->glBindFramebuffer(GL_FRAMEBUFFER, m_state->defaultFramebuffer);
  m_state->pipeline = nullptr;
  m_state->vertexSlots.clear();
  m_state->indexBuffer = nullptr;
}

QImage GlRenderDevice::readPixels(const RenderTarget& target)
{
  const GlRenderTarget* gl = dynamic_cast<const GlRenderTarget*>(&target);
  if (!gl) {
    return QImage();
  }
  const int width = gl->width();
  const int height = gl->height();
  QImage bottomUp(width, height, QImage::Format_RGBA8888);
  m_gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, gl->framebuffer());
  m_gl->glPixelStorei(GL_PACK_ALIGNMENT, 4);
  m_gl->glPixelStorei(GL_PACK_ROW_LENGTH,
                      static_cast<GLint>(bottomUp.bytesPerLine() / 4));
  m_gl->glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE,
                     bottomUp.bits());
  m_gl->glPixelStorei(GL_PACK_ROW_LENGTH, 0);
  m_gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, m_state->defaultFramebuffer);
  // OpenGL's first row is the bottom one.
  QImage image(width, height, QImage::Format_RGBA8888);
  const std::size_t rowBytes = static_cast<std::size_t>(width) * 4;
  for (int y = 0; y < height; ++y) {
    std::memcpy(image.scanLine(y), bottomUp.constScanLine(height - 1 - y),
                rowBytes);
  }
  return image;
}

QString GlRenderDevice::takeErrors()
{
  QString errors;
  // glGetError returns one flag per call until all are cleared; stop after
  // a few in case a broken context keeps reporting.
  for (int i = 0; i < 16; ++i) {
    const GLenum error = m_gl->glGetError();
    if (error == GL_NO_ERROR) {
      break;
    }
    if (!errors.isEmpty()) {
      errors += QStringLiteral(", ");
    }
    errors += QStringLiteral("0x%1").arg(error, 4, 16, QLatin1Char('0'));
  }
  return errors;
}

std::size_t GlRenderDevice::uniformBufferAlignment() const
{
  GLint alignment = 0;
  m_gl->glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &alignment);
  return std::max<std::size_t>(16, static_cast<std::size_t>(alignment));
}

} // namespace ts
