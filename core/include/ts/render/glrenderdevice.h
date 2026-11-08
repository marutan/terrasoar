// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file
 * The OpenGL 3.3 core implementation of RenderDevice.
 */
#ifndef TS_RENDER_GLRENDERDEVICE_H
#define TS_RENDER_GLRENDERDEVICE_H

#include <memory>

#include <QSize>
#include <QString>

#include "ts/render/renderdevice.h"

class QOpenGLContext;

namespace ts
{

class GlFunctions;

/**
 * RenderDevice on OpenGL 3.3 core profile.
 *
 * Works on any context of version 3.3 or later with the core profile,
 * including Mesa llvmpipe. The context must be current whenever the device
 * or any of its resources is used or destroyed.
 *
 * Passes with a null target draw into the framebuffer set with
 * setDefaultFramebuffer(); for QOpenGLWidget that is
 * defaultFramebufferObject(), which can change between frames.
 *
 * @renderthread
 */
class GlRenderDevice : public RenderDevice
{
public:
  /**
   * Creates a device for @p context, which must be current. Returns null
   * (and sets @p errorString if it is not null) if the context is not
   * OpenGL 3.3 core or later.
   */
  static std::unique_ptr<GlRenderDevice> create(QOpenGLContext* context,
                                                QString* errorString);

  /** Destroys the device; the context must be current. */
  ~GlRenderDevice() override;

  GlRenderDevice(const GlRenderDevice&) = delete;
  GlRenderDevice& operator=(const GlRenderDevice&) = delete;
  GlRenderDevice(GlRenderDevice&&) = delete;
  GlRenderDevice& operator=(GlRenderDevice&&) = delete;

  /**
   * Sets the framebuffer object that passes without a target draw into,
   * and its size in device pixels.
   */
  void setDefaultFramebuffer(unsigned int framebufferId, QSize sizePx);

  RenderDeviceInfo info() const override;
  std::unique_ptr<GpuBuffer> createBuffer(BufferType type, BufferUsage usage,
                                          std::size_t sizeBytes,
                                          const void* data) override;
  std::unique_ptr<GpuTexture> createTexture(const TextureDesc& desc) override;
  std::unique_ptr<ShaderProgram>
  createShaderProgram(const ShaderDesc& desc, QString* errorString) override;
  std::unique_ptr<Pipeline> createPipeline(const PipelineDesc& desc) override;
  std::unique_ptr<RenderTarget> createRenderTarget(int width,
                                                   int height) override;
  void beginPass(const PassDesc& pass) override;
  void setPipeline(const Pipeline& pipeline) override;
  void setVertexBuffer(int slot, const GpuBuffer& buffer,
                       std::size_t offsetBytes) override;
  void setIndexBuffer(const GpuBuffer& buffer, IndexFormat format) override;
  void setUniformBuffer(int binding, const GpuBuffer& buffer,
                        std::size_t offsetBytes,
                        std::size_t sizeBytes) override;
  void setTexture(int unit, const GpuTexture& texture) override;
  void draw(int vertexCount, int firstVertex, int instances) override;
  void drawIndexed(int indexCount, int firstIndex, int instances) override;
  void endPass() override;
  QImage readPixels(const RenderTarget& target) override;
  QString takeErrors() override;
  std::size_t uniformBufferAlignment() const override;

private:
  struct State;

  GlRenderDevice(std::unique_ptr<GlFunctions> functions, RenderDeviceInfo info);
  void bindVertexInputs();

  std::unique_ptr<GlFunctions> m_gl;
  std::unique_ptr<State> m_state;
  RenderDeviceInfo m_info;
};

} // namespace ts

#endif // TS_RENDER_GLRENDERDEVICE_H
