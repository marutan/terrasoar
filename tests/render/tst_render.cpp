// SPDX-License-Identifier: GPL-2.0-or-later
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>

#include <QImage>
#include <QtTest>

#include "offscreengl.h"
#include "ts/render/framepacer.h"
#include "ts/render/glrenderdevice.h"
#include "ts/render/renderdevice.h"

namespace
{

constexpr int sizePx = 64;

/** Colour from a uniform block; position from attribute 0. */
const char* const solidVertexGlsl = R"(
layout(location = 0) in vec2 position;
void main() { gl_Position = vec4(position, 0.0, 1.0); }
)";

const char* const solidFragmentGlsl = R"(
layout(std140) uniform Solid { vec4 color; float depth; };
out vec4 fragColor;
void main() { fragColor = color; gl_FragDepth = depth; }
)";

/** Position from attribute 0, colour per vertex, offset per instance. */
const char* const instancedVertexGlsl = R"(
layout(location = 0) in vec2 position;
layout(location = 1) in vec4 color;
layout(location = 2) in vec2 offset;
out vec4 vColor;
void main() { vColor = color; gl_Position = vec4(position + offset, 0.0, 1.0); }
)";

const char* const instancedFragmentGlsl = R"(
in vec4 vColor;
out vec4 fragColor;
void main() { fragColor = vColor; }
)";

const char* const texturedVertexGlsl = R"(
layout(location = 0) in vec2 position;
out vec2 vUv;
void main() { vUv = position * 0.5 + 0.5; gl_Position = vec4(position, 0.0, 1.0); }
)";

const char* const texturedFragmentGlsl = R"(
uniform sampler2D image;
in vec2 vUv;
out vec4 fragColor;
void main() { fragColor = texture(image, vUv); }
)";

/** Two triangles covering clip space. */
constexpr std::array<float, 12> fullScreen = {-1.0F, -1.0F, 1.0F,  -1.0F,
                                              1.0F,  1.0F,  -1.0F, -1.0F,
                                              1.0F,  1.0F,  -1.0F, 1.0F};

/** std140 layout of the Solid block. */
struct SolidBlock {
  std::array<float, 4> color{};
  float depth = 0.5F;
  std::array<float, 3> padding{};
};

QRgb pixel(const QImage& image, int x, int y)
{
  return image.pixel(x, y) & 0x00ffffffU;
}

} // namespace

/**
 * GPU tests for ts/render: the OpenGL 3.3 backend draws what the
 * RenderDevice interface describes. Render-thread classes, so there are no
 * concurrency tests; each test uses one context.
 */
class TestRender : public QObject
{
  Q_OBJECT

private:
  std::unique_ptr<ts::ShaderProgram> solidShader(QString* error = nullptr)
  {
    ts::ShaderDesc desc;
    desc.label = QStringLiteral("solid");
    desc.vertexSource = solidVertexGlsl;
    desc.fragmentSource = solidFragmentGlsl;
    desc.uniformBlocks = {{QByteArrayLiteral("Solid"), 3}};
    return m_gl->device().createShaderProgram(desc, error);
  }

  ts::PipelineDesc solidPipeline(const ts::ShaderProgram& shader) const
  {
    ts::PipelineDesc desc;
    desc.shader = &shader;
    desc.bufferLayouts = {{8, false}};
    desc.attributes = {{0, ts::VertexFormat::Float2, 0, 0}};
    return desc;
  }

  /** Draws a full-screen quad of @p rgba at @p depth. */
  void drawSolid(const ts::Pipeline& pipeline, std::array<float, 4> rgba,
                 float depth)
  {
    ts::RenderDevice& device = m_gl->device();
    SolidBlock block;
    block.color = rgba;
    block.depth = depth;
    std::unique_ptr<ts::GpuBuffer> uniforms =
        device.createBuffer(ts::BufferType::Uniform, ts::BufferUsage::Dynamic,
                            sizeof(block), &block);
    device.setPipeline(pipeline);
    device.setVertexBuffer(0, *m_quad, 0);
    device.setUniformBuffer(3, *uniforms, 0, sizeof(block));
    device.draw(6, 0, 1);
  }

  ts::PassDesc pass(std::array<float, 4> clear = {0.0F, 0.0F, 0.0F, 1.0F}) const
  {
    ts::PassDesc desc;
    desc.target = m_target.get();
    desc.viewportPx = QRect(0, 0, sizePx, sizePx);
    desc.clearColorRgba = clear;
    return desc;
  }

  std::unique_ptr<ts::test::OffscreenGl> m_gl;
  std::unique_ptr<ts::RenderTarget> m_target;
  std::unique_ptr<ts::GpuBuffer> m_quad;

  // NOLINTNEXTLINE(readability-redundant-access-specifiers): Qt's slots marker
private slots:
  void init()
  {
    m_gl = std::make_unique<ts::test::OffscreenGl>();
    if (m_gl->isValid()) {
      m_target = m_gl->device().createRenderTarget(sizePx, sizePx);
      m_quad = m_gl->device().createBuffer(
          ts::BufferType::Vertex, ts::BufferUsage::Static, sizeof(fullScreen),
          fullScreen.data());
    }
  }

  void cleanup()
  {
    if (m_gl->isValid()) {
      m_gl->makeCurrent();
      QCOMPARE(m_gl->device().takeErrors(), QString());
    }
    m_quad.reset();
    m_target.reset();
    m_gl.reset();
  }

  void deviceInfo()
  {
    TS_TEST_REQUIRE_GL(*m_gl);
    const ts::RenderDeviceInfo info = m_gl->device().info();
    QCOMPARE(info.backend, QStringLiteral("OpenGL 3.3 core"));
    QVERIFY(!info.renderer.isEmpty());
    QVERIFY(!info.version.isEmpty());
    QVERIFY(info.maxTextureSize >= 1024);
    QVERIFY(m_gl->device().uniformBufferAlignment() >= 16);
  }

  void clearsTarget()
  {
    TS_TEST_REQUIRE_GL(*m_gl);
    QVERIFY(m_target);
    m_gl->device().beginPass(pass({1.0F, 0.0F, 0.0F, 1.0F}));
    m_gl->device().endPass();
    const QImage image = m_gl->device().readPixels(*m_target);
    QCOMPARE(image.size(), QSize(sizePx, sizePx));
    QCOMPARE(pixel(image, 0, 0), qRgb(255, 0, 0) & 0x00ffffffU);
    QCOMPARE(pixel(image, sizePx - 1, sizePx - 1),
             qRgb(255, 0, 0) & 0x00ffffffU);
  }

  void viewportOriginIsTopLeft()
  {
    // A full-screen draw into the top-left quarter viewport colours only
    // the top-left quarter of the image.
    TS_TEST_REQUIRE_GL(*m_gl);
    std::unique_ptr<ts::ShaderProgram> shader = solidShader();
    QVERIFY(shader);
    std::unique_ptr<ts::Pipeline> pipeline =
        m_gl->device().createPipeline(solidPipeline(*shader));
    QVERIFY(pipeline);
    ts::PassDesc desc = pass();
    desc.viewportPx = QRect(0, 0, sizePx / 2, sizePx / 2);
    m_gl->device().beginPass(desc);
    drawSolid(*pipeline, {0.0F, 1.0F, 0.0F, 1.0F}, 0.5F);
    m_gl->device().endPass();
    const QImage image = m_gl->device().readPixels(*m_target);
    QCOMPARE(pixel(image, 4, 4), 0x0000ff00U);
    QCOMPARE(pixel(image, sizePx - 4, 4), 0U);
    QCOMPARE(pixel(image, 4, sizePx - 4), 0U);
  }

  void indexedInstancedDraw()
  {
    // Two instances of a small quad, offset per instance, coloured per
    // vertex (UNorm8x4), drawn from a 16-bit index buffer.
    TS_TEST_REQUIRE_GL(*m_gl);
    ts::RenderDevice& device = m_gl->device();
    ts::ShaderDesc shaderDesc;
    shaderDesc.label = QStringLiteral("instanced");
    shaderDesc.vertexSource = instancedVertexGlsl;
    shaderDesc.fragmentSource = instancedFragmentGlsl;
    QString error;
    std::unique_ptr<ts::ShaderProgram> shader =
        device.createShaderProgram(shaderDesc, &error);
    QVERIFY2(shader, qPrintable(error));

    struct Vertex {
      std::array<float, 2> position;
      std::array<std::uint8_t, 4> rgba;
    };
    const std::array<Vertex, 4> vertices = {
        Vertex{{-0.25F, -0.25F}, {0, 0, 255, 255}},
        Vertex{{0.25F, -0.25F}, {0, 0, 255, 255}},
        Vertex{{0.25F, 0.25F}, {0, 0, 255, 255}},
        Vertex{{-0.25F, 0.25F}, {0, 0, 255, 255}}};
    const std::array<std::uint16_t, 6> indices = {0, 1, 2, 0, 2, 3};
    const std::array<float, 4> offsets = {-0.5F, 0.5F, 0.5F, -0.5F};
    std::unique_ptr<ts::GpuBuffer> vertexBuffer =
        device.createBuffer(ts::BufferType::Vertex, ts::BufferUsage::Static,
                            sizeof(vertices), vertices.data());
    std::unique_ptr<ts::GpuBuffer> indexBuffer =
        device.createBuffer(ts::BufferType::Index, ts::BufferUsage::Static,
                            sizeof(indices), indices.data());
    std::unique_ptr<ts::GpuBuffer> offsetBuffer =
        device.createBuffer(ts::BufferType::Vertex, ts::BufferUsage::Static,
                            sizeof(offsets), offsets.data());
    ts::PipelineDesc desc;
    desc.shader = shader.get();
    desc.bufferLayouts = {{sizeof(Vertex), false}, {8, true}};
    desc.attributes = {{0, ts::VertexFormat::Float2, 0, 0},
                       {1, ts::VertexFormat::UNorm8x4, 0, 8},
                       {2, ts::VertexFormat::Float2, 1, 0}};
    std::unique_ptr<ts::Pipeline> pipeline = device.createPipeline(desc);
    QVERIFY(pipeline);

    device.beginPass(pass());
    device.setPipeline(*pipeline);
    device.setVertexBuffer(0, *vertexBuffer, 0);
    device.setVertexBuffer(1, *offsetBuffer, 0);
    device.setIndexBuffer(*indexBuffer, ts::IndexFormat::UInt16);
    device.drawIndexed(6, 0, 2);
    device.endPass();
    const QImage image = device.readPixels(*m_target);
    // Instance 0 at (−0.5, +0.5): top left; instance 1 at (+0.5, −0.5):
    // bottom right. The other two quarters stay black.
    const int quarter = sizePx / 4;
    QCOMPARE(pixel(image, quarter, quarter), 0x000000ffU);
    QCOMPARE(pixel(image, 3 * quarter, 3 * quarter), 0x000000ffU);
    QCOMPARE(pixel(image, 3 * quarter, quarter), 0U);
    QCOMPARE(pixel(image, quarter, 3 * quarter), 0U);
  }

  void uniformBufferOffset()
  {
    // Two blocks in one buffer; binding the second (at an aligned offset)
    // draws its colour.
    TS_TEST_REQUIRE_GL(*m_gl);
    ts::RenderDevice& device = m_gl->device();
    std::unique_ptr<ts::ShaderProgram> shader = solidShader();
    std::unique_ptr<ts::Pipeline> pipeline =
        device.createPipeline(solidPipeline(*shader));
    const std::size_t offset =
        std::max(sizeof(SolidBlock), device.uniformBufferAlignment());
    std::unique_ptr<ts::GpuBuffer> uniforms =
        device.createBuffer(ts::BufferType::Uniform, ts::BufferUsage::Dynamic,
                            offset + sizeof(SolidBlock), nullptr);
    SolidBlock first;
    first.color = {1.0F, 0.0F, 0.0F, 1.0F};
    SolidBlock second;
    second.color = {0.0F, 1.0F, 1.0F, 1.0F};
    uniforms->update(0, &first, sizeof(first));
    uniforms->update(offset, &second, sizeof(second));
    device.beginPass(pass());
    device.setPipeline(*pipeline);
    device.setVertexBuffer(0, *m_quad, 0);
    device.setUniformBuffer(3, *uniforms, offset, sizeof(SolidBlock));
    device.draw(6, 0, 1);
    device.endPass();
    QCOMPARE(pixel(device.readPixels(*m_target), sizePx / 2, sizePx / 2),
             0x0000ffffU);
  }

  void textureRowZeroIsBottom()
  {
    // Image row 0 is texture coordinate t = 0, which the quad maps to the
    // bottom of the target (OpenGL convention). A 2 × 2 texture with
    // nearest filtering shows each texel in one quarter.
    TS_TEST_REQUIRE_GL(*m_gl);
    ts::RenderDevice& device = m_gl->device();
    ts::ShaderDesc shaderDesc;
    shaderDesc.label = QStringLiteral("textured");
    shaderDesc.vertexSource = texturedVertexGlsl;
    shaderDesc.fragmentSource = texturedFragmentGlsl;
    shaderDesc.samplers = {{QByteArrayLiteral("image"), 2}};
    std::unique_ptr<ts::ShaderProgram> shader =
        device.createShaderProgram(shaderDesc, nullptr);
    QVERIFY(shader);
    ts::PipelineDesc desc = solidPipeline(*shader);
    std::unique_ptr<ts::Pipeline> pipeline = device.createPipeline(desc);
    ts::TextureDesc textureDesc;
    textureDesc.width = 2;
    textureDesc.height = 2;
    textureDesc.filter = ts::TextureFilter::Nearest;
    std::unique_ptr<ts::GpuTexture> texture = device.createTexture(textureDesc);
    QVERIFY(texture);
    QImage image(2, 2, QImage::Format_RGB32);
    image.setPixel(0, 0, qRgb(255, 0, 0));
    image.setPixel(1, 0, qRgb(0, 255, 0));
    image.setPixel(0, 1, qRgb(0, 0, 255));
    image.setPixel(1, 1, qRgb(255, 255, 255));
    texture->upload(image);

    device.beginPass(pass());
    device.setPipeline(*pipeline);
    device.setVertexBuffer(0, *m_quad, 0);
    device.setTexture(2, *texture);
    device.draw(6, 0, 1);
    device.endPass();
    const QImage result = device.readPixels(*m_target);
    const int q = sizePx / 4;
    QCOMPARE(pixel(result, q, 3 * q), 0x00ff0000U);     // bottom left: row 0
    QCOMPARE(pixel(result, 3 * q, 3 * q), 0x0000ff00U); // bottom right
    QCOMPARE(pixel(result, q, q), 0x000000ffU);         // top left: row 1
    QCOMPARE(pixel(result, 3 * q, q), 0x00ffffffU);
  }

  void depthTest()
  {
    // A near red quad, then a far green one with depth test Less: red
    // stays. Then an equally near blue with LessOrEqual: blue wins.
    TS_TEST_REQUIRE_GL(*m_gl);
    ts::RenderDevice& device = m_gl->device();
    std::unique_ptr<ts::ShaderProgram> shader = solidShader();
    ts::PipelineDesc desc = solidPipeline(*shader);
    desc.depthTest = true;
    desc.depthWrite = true;
    desc.depthCompare = ts::CompareOp::Less;
    std::unique_ptr<ts::Pipeline> less = device.createPipeline(desc);
    desc.depthCompare = ts::CompareOp::LessOrEqual;
    std::unique_ptr<ts::Pipeline> lessOrEqual = device.createPipeline(desc);
    device.beginPass(pass());
    drawSolid(*less, {1.0F, 0.0F, 0.0F, 1.0F}, 0.25F);
    drawSolid(*less, {0.0F, 1.0F, 0.0F, 1.0F}, 0.75F);
    device.endPass();
    QCOMPARE(pixel(device.readPixels(*m_target), 8, 8), 0x00ff0000U);
    device.beginPass(pass());
    drawSolid(*less, {1.0F, 0.0F, 0.0F, 1.0F}, 0.25F);
    drawSolid(*lessOrEqual, {0.0F, 0.0F, 1.0F, 1.0F}, 0.25F);
    device.endPass();
    QCOMPARE(pixel(device.readPixels(*m_target), 8, 8), 0x000000ffU);
  }

  void premultipliedBlend()
  {
    // White at alpha 0.5 (premultiplied: 0.5, 0.5, 0.5, 0.5) over black.
    TS_TEST_REQUIRE_GL(*m_gl);
    ts::RenderDevice& device = m_gl->device();
    std::unique_ptr<ts::ShaderProgram> shader = solidShader();
    ts::PipelineDesc desc = solidPipeline(*shader);
    desc.blend = ts::BlendMode::PremultipliedAlpha;
    std::unique_ptr<ts::Pipeline> pipeline = device.createPipeline(desc);
    device.beginPass(pass());
    drawSolid(*pipeline, {0.5F, 0.5F, 0.5F, 0.5F}, 0.5F);
    device.endPass();
    const QColor c = QColor(device.readPixels(*m_target).pixel(8, 8));
    QVERIFY(std::abs(c.red() - 128) <= 1);
  }

  void shaderErrorsAreReported()
  {
    TS_TEST_REQUIRE_GL(*m_gl);
    ts::ShaderDesc desc;
    desc.label = QStringLiteral("broken");
    desc.vertexSource = "void main() { gl_Position = undefinedThing; }";
    desc.fragmentSource = instancedFragmentGlsl;
    QString error;
    QVERIFY(!m_gl->device().createShaderProgram(desc, &error));
    QVERIFY2(error.startsWith(QStringLiteral("broken: ")), qPrintable(error));
    QVERIFY(error.size() > 8);
  }

  void invalidDescriptionsAreRefused()
  {
    TS_TEST_REQUIRE_GL(*m_gl);
    ts::RenderDevice& device = m_gl->device();
    std::unique_ptr<ts::ShaderProgram> shader = solidShader();
    ts::PipelineDesc desc = solidPipeline(*shader);
    desc.attributes[0].bufferSlot = 5;
    QVERIFY(!device.createPipeline(desc));
    QVERIFY(!device.createPipeline(ts::PipelineDesc{}));
    QVERIFY(!device.createRenderTarget(0, 10));
    QVERIFY(!device.createBuffer(ts::BufferType::Vertex,
                                 ts::BufferUsage::Static, 0, nullptr));
    ts::TextureDesc huge;
    huge.width = device.info().maxTextureSize + 1;
    huge.height = 1;
    QVERIFY(!device.createTexture(huge));
  }

  void framePacer()
  {
    // No GPU needed: frames start at most once per refresh interval.
    ts::FramePacer pacer;
    QCOMPARE(pacer.minimumIntervalS(), 1.0 / 60.0);
    QCOMPARE(pacer.delayS(0.0), 0.0);
    pacer.setRefreshRateHz(100.0);
    pacer.frameStarted(10.0);
    QVERIFY(std::abs(pacer.delayS(10.0) - 0.01) < 1e-12);
    QVERIFY(std::abs(pacer.delayS(10.004) - 0.006) < 1e-12);
    QCOMPARE(pacer.delayS(10.02), 0.0);
    // Unknown or bogus rates fall back to 60 Hz.
    pacer.setRefreshRateHz(0.0);
    QCOMPARE(pacer.minimumIntervalS(), 1.0 / 60.0);
    pacer.setRefreshRateHz(5000.0);
    QCOMPARE(pacer.minimumIntervalS(), 1.0 / 60.0);
  }

  void rejectsMissingContext()
  {
    // A device needs a current context; null is refused with a message.
    QString error;
    QVERIFY(!ts::GlRenderDevice::create(nullptr, &error));
    QVERIFY(!error.isEmpty());
  }
};

QTEST_MAIN(TestRender)
#include "tst_render.moc"
