# Renderer backend

Rendering code in Terra Soar never calls OpenGL directly. It talks to
`ts::RenderDevice` (`core/include/ts/render/renderdevice.h`), a small
backend interface with one implementation today, `ts::GlRenderDevice` on
OpenGL 3.3 core. This page describes the interface, the rules a backend
follows, and how a second backend (Qt RHI, for Vulkan, Metal and
Direct3D) would plug in.

## Why a backend interface

- **OpenGL 3.3 core is the baseline** because Mesa llvmpipe supports it,
  which keeps remote desktop and machines without a GPU usable
  (see the roadmap's performance requirements).
- **The interface is shaped like the modern APIs**: immutable pipeline
  state objects, uniform buffers instead of individual uniforms, explicit
  passes. OpenGL is the odd one out, so the OpenGL implementation does the
  translating, and a Vulkan/Metal/D3D backend (through Qt RHI) maps almost
  one to one.
- **Everything above it is backend-free**: the globe renderer, and from
  Phase 2 the tile renderer, build their frames from the same calls.

## The interface

| Object | Created by | Holds |
| --- | --- | --- |
| `GpuBuffer` | `createBuffer(type, usage, size, data)` | Vertices, 16/32-bit indices, or a std140 uniform block. `update()` rewrites a range |
| `GpuTexture` | `createTexture(TextureDesc)` | A 2D RGBA8 or R8 texture with its sampling (filter, wrap, mipmaps). `upload(QImage)` fills it; image row 0 is t = 0 |
| `ShaderProgram` | `createShaderProgram(ShaderDesc, &error)` | GLSL 3.30 sources (without `#version`), the uniform blocks' binding points and the samplers' texture units |
| `Pipeline` | `createPipeline(PipelineDesc)` | Shader, vertex buffer layouts (stride, per-vertex or per-instance), attributes, topology, culling, depth test/write/compare, blending |
| `RenderTarget` | `createRenderTarget(w, h)` | Offscreen RGBA8 colour + 24-bit depth, for tests and screenshots; `readPixels()` reads it back |

A frame is a list of passes:

```cpp
device.beginPass(pass);              // target (or the window), viewport, clears
device.setUniformBuffer(0, *frameUniforms, 0, sizeof(FrameBlock));
device.setPipeline(*pipeline);
device.setVertexBuffer(0, *vertices, 0);
device.setIndexBuffer(*indices, IndexFormat::UInt16);
device.drawIndexed(count, 0, instances);
device.endPass();                    // leaves state as Qt expects it
```

State set inside a pass (pipeline, buffers, textures) stays until changed
or until the pass ends. Viewports are in device pixels with the origin at
the top left, like `QRect`; the OpenGL backend flips them.

### Conventions every backend follows

- **Uniforms are std140 uniform blocks.** C++ mirrors them with
  `std::array<float, N>` members in vec4-sized steps and checks the size
  with `static_assert` (see `FrameBlock` in `globerenderer.cpp`). Offsets
  into one buffer are aligned to `uniformBufferAlignment()`.
- **Vertex inputs use `layout(location = N)`**; `VertexAttribute::location`
  is that N.
- **Front faces are counter-clockwise.** Clip-space depth is OpenGL's
  [−1, 1]; renderers that need precise depth write it themselves (see
  [camera-and-precision.md](camera-and-precision.md)).
- **Blending is premultiplied alpha** (`src + dst (1 − αs)`) or none.
- **Errors**: creation functions return null on bad input or failure
  (`createShaderProgram` also returns the compiler log); `takeErrors()`
  returns anything the driver reported since the last call. The tests
  check it after every case.
- **Threading**: a device and its resources are used only on the render
  thread, with the context current, and resources are destroyed before
  the device (`@renderthread`). Loading and decoding happen on workers
  (Phase 2); only the upload goes through the device.

## The OpenGL 3.3 implementation

`GlRenderDevice::create(context, &error)` needs a current context of
version 3.3 or later, core profile. It resolves the entry points through
`QOpenGLFunctions_3_3_Core`, which exists in both Qt 5 (Qt GUI) and Qt 6
(Qt OpenGL, which doesn't pull in widgets), so `core/` stays widget-free.

| Interface | OpenGL |
| --- | --- |
| Buffer upload | `glBufferData` through `GL_COPY_WRITE_BUFFER`, which touches no vertex array state |
| Pipeline | One vertex array object per pipeline; attribute pointers are re-specified when the bound buffers change (`glVertexAttribDivisor` for per-instance slots) |
| Uniform block binding | `glUniformBlockBinding` at link time (GLSL 3.30 has no `layout(binding)`) |
| Samplers | `glUniform1i` at link time |
| Draw | `glDrawArraysInstanced` / `glDrawElementsInstanced` |
| Render target | Framebuffer object with RGBA8 and `DEPTH_COMPONENT24` renderbuffers |
| Default framebuffer | Set per frame with `setDefaultFramebuffer(id, size)`; for `QOpenGLWidget` that is `defaultFramebufferObject()`, which can change |
| `endPass()` | Unbinds the vertex array and program, resets depth, culling and blending, rebinds the default framebuffer, so `QPainter` can draw on top (the debug overlay does) |

## How an RHI backend would plug in

Qt 6's RHI (`QRhi`) offers Vulkan, Metal, Direct3D 11/12 and OpenGL behind
one API, and its concepts match this interface:

| `RenderDevice` | `QRhi` |
| --- | --- |
| `GpuBuffer` | `QRhiBuffer` (Immutable/Dynamic, VertexBuffer/IndexBuffer/UniformBuffer) |
| `GpuTexture` + sampling | `QRhiTexture` + `QRhiSampler` |
| `ShaderProgram` | `QShader` pairs, compiled offline from the same GLSL by `qsb` (Vulkan-style GLSL 4.50 → SPIR-V, MSL, HLSL) |
| `Pipeline` | `QRhiGraphicsPipeline` + `QRhiShaderResourceBindings` |
| `beginPass` / `endPass` | `QRhiCommandBuffer::beginPass` / `endPass` |
| `setUniformBuffer`, `setTexture` | Shader resource bindings, per draw or dynamic offsets |

What a second backend needs:

1. **Shaders in two dialects.** `ShaderDesc` would gain a precompiled
   `QShader` (or a `.qsb` resource path) next to the GLSL 3.30 source.
   The shaders already use only uniform blocks and explicit locations, so
   the sources port mechanically (add `layout(binding = N)` and
   `#version 450`).
2. **Resource bindings per pipeline.** RHI fixes the binding layout in
   the pipeline; `PipelineDesc` would list its uniform blocks and textures
   (today they come from `ShaderDesc`).
3. **Clip-space depth per backend.** Vulkan, Metal and Direct3D use
   [0, 1] (and Vulkan flips y); RHI's OpenGL backend keeps [−1, 1]. The
   projection is multiplied by `QRhi::clipSpaceCorrMatrix()` to match.
   Logarithmic depth written by the fragment shaders is unaffected.
4. **No direct `QOpenGLWidget`.** The view would become a `QRhiWidget`
   (Qt 6.7+); Qt 5 builds keep the OpenGL backend.

RHI is Qt 6 only, so it is an option for later phases, not a replacement
for the OpenGL 3.3 path that Qt 5 and software rendering need.

## Tests

`tests/render` draws through the interface into a `RenderTarget` and reads
the pixels back: clearing, viewport orientation, indexed and instanced
draws with per-vertex colours, uniform buffer offsets, texture
orientation and filtering, depth comparison, premultiplied blending,
shader error reporting, refusal of invalid descriptions, and no driver
errors after any case. GPU tests skip without OpenGL 3.3 unless
`TS_REQUIRE_GL=1` (CI runs them on Mesa llvmpipe).
