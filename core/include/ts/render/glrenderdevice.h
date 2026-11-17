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

/**
 * @file
 * The OpenGL 3.3 core implementation of RenderDevice.
 */
#ifndef TS_RENDER_GLRENDERDEVICE_H
#define TS_RENDER_GLRENDERDEVICE_H

#include <cstdint>
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
  void setDefaultFramebuffer(uint32_t framebufferId, QSize sizePx);

  RenderDeviceInfo info() const override;
  std::unique_ptr<GpuBuffer> createBuffer(BufferType type, BufferUsage usage,
                                          std::size_t sizeBytes,
                                          const void* data) override;
  std::unique_ptr<GpuTexture> createTexture(const TextureDesc& desc) override;
  std::unique_ptr<ShaderProgram>
  createShaderProgram(const ShaderDesc& desc, QString* errorString) override;
  std::unique_ptr<Pipeline> createPipeline(const PipelineDesc& desc) override;
  std::unique_ptr<RenderTarget> createRenderTarget(int32_t width,
                                                   int32_t height) override;
  void beginPass(const PassDesc& pass) override;
  void setPipeline(const Pipeline& pipeline) override;
  void setVertexBuffer(int32_t slot, const GpuBuffer& buffer,
                       std::size_t offsetBytes) override;
  void setIndexBuffer(const GpuBuffer& buffer, IndexFormat format) override;
  void setUniformBuffer(int32_t binding, const GpuBuffer& buffer,
                        std::size_t offsetBytes,
                        std::size_t sizeBytes) override;
  void setTexture(int32_t unit, const GpuTexture& texture) override;
  void setRenderTargetTexture(int32_t unit,
                              const RenderTarget& target) override;
  void draw(int32_t vertexCount, int32_t firstVertex,
            int32_t instances) override;
  void drawIndexed(int32_t indexCount, int32_t firstIndex,
                   int32_t instances) override;
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
