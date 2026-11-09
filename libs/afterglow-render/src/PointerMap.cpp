// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PointerMap.h"

#include "ShaderBindings.h"

#include <AfterglowRender/GpuDevice.h>

#include <QFile>
#include <QLoggingCategory>
#include <QtCore/qfloat16.h>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

Q_LOGGING_CATEGORY(lcPointerMap, "afterglow.render.pointermap")

namespace AfterglowRender {

namespace {
constexpr std::size_t MaxGrids = 32;
} // namespace

PointerMaps::PointerMaps(GpuDevice& device)
    : m_device(device)
{
}

PointerMaps::~PointerMaps() = default;

void PointerMaps::clear()
{
    m_grids.clear();
    m_pipelines.clear();
}

bool PointerMaps::ensureResources()
{
    if (m_bindings) return true;
    if (m_failed) return false;
    QRhi* rhi = m_device.rhi();
    auto fail = [this](const char* what) {
        qCWarning(lcPointerMap) << "Pointer maps are off:" << what;
        m_failed = true;
        return false;
    };
    QFile vertex(QStringLiteral(":/afterglow/shaders/post.vert.qsb"));
    if (!vertex.open(QIODevice::ReadOnly)) return fail("no post.vert.qsb");
    m_vertex = QShader::fromSerialized(vertex.readAll());
    // Float targets, for a map that reads back to a fraction of a pixel.
    m_targetFormat = rhi->isTextureFormatSupported(QRhiTexture::RGBA32F) ? int(QRhiTexture::RGBA32F)
                                                                          : int(QRhiTexture::RGBA16F);
    m_uniforms.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(TerminalUniforms)));
    m_scene.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(TerminalSceneUniforms)));
    m_panes.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(TerminalPaneUniforms)));
    m_dummy.reset(rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
    m_sampler.reset(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                    QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    if (!m_uniforms->create() || !m_scene->create() || !m_panes->create() || !m_dummy->create() || !m_sampler->create())
        return fail("its buffers could not be made");
    // The whole binding table of a pack's program (PostChain::bindings), with
    // nothing to read: a map may read only the canvas size and parameters.
    namespace B = PhosphorShaders::Bindings;
    const auto stage = QRhiShaderResourceBinding::FragmentStage | QRhiShaderResourceBinding::VertexStage;
    auto texture = [&](int binding) { return QRhiShaderResourceBinding::sampledTexture(binding, stage, m_dummy.get(), m_sampler.get()); };
    std::vector<QRhiShaderResourceBinding> entries{
        QRhiShaderResourceBinding::uniformBuffer(B::kUniformBlock, stage, m_uniforms.get()),
        texture(B::kConsumer),
        texture(B::kAudioSpectrum),
        texture(Bindings::PreviousText),
        QRhiShaderResourceBinding::uniformBuffer(Bindings::Scene, stage, m_scene.get()),
        QRhiShaderResourceBinding::uniformBuffer(Bindings::Panes, stage, m_panes.get()),
        texture(Bindings::TabImage),
        texture(Bindings::TabAtlas),
        texture(B::kDepth),
    };
    for (int i = 0; i < MaxShaderBuffers; ++i) entries.push_back(texture(B::kChannelBase + i));
    for (int i = 0; i < MaxShaderImages; ++i) entries.push_back(texture(B::kUserTextureBase + i));
    m_bindings.reset(rhi->newShaderResourceBindings());
    m_bindings->setBindings(entries.begin(), entries.end());
    if (!m_bindings->create()) {
        m_bindings.reset();
        return fail("its bindings could not be made");
    }
    return true;
}

QRhiGraphicsPipeline* PointerMaps::pipeline(const ShaderProgram& program)
{
    if (auto it = m_pipelines.find(&program); it != m_pipelines.end()) return it->second.pipeline.get();
    std::unique_ptr<QRhiGraphicsPipeline> p(m_device.rhi()->newGraphicsPipeline());
    p->setTopology(QRhiGraphicsPipeline::Triangles);
    p->setShaderStages({{QRhiShaderStage::Vertex, m_vertex}, {QRhiShaderStage::Fragment, program.pointerShader()}});
    p->setVertexInputLayout({});
    p->setShaderResourceBindings(m_bindings.get());
    p->setRenderPassDescriptor(m_pass.get());
    if (!p->create()) {
        qCWarning(lcPointerMap) << "The pointer map of" << program.id() << "could not be made";
        return nullptr;
    }
    auto* raw = p.get();
    m_pipelines.emplace(&program, Pipeline{nullptr, std::move(p)});
    return raw;
}

bool PointerMaps::evaluate(Grid& grid, const TerminalUniforms& uniforms)
{
    QRhi* rhi = m_device.rhi();
    const QSize size(grid.columns, grid.rows);
    if (!m_target || m_target->pixelSize() != size) {
        m_renderTarget.reset();
        m_target.reset(rhi->newTexture(QRhiTexture::Format(m_targetFormat), size, 1,
                                       QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!m_target->create()) return false;
        m_renderTarget.reset(rhi->newTextureRenderTarget({m_target.get()}));
        // One descriptor for every target: they have the same format, so a
        // pipeline made for it draws into any of them.
        if (!m_pass) m_pass.reset(m_renderTarget->newCompatibleRenderPassDescriptor());
        m_renderTarget->setRenderPassDescriptor(m_pass.get());
        if (!m_renderTarget->create()) return false;
    }
    QRhiGraphicsPipeline* p = pipeline(*grid.program);
    if (!p) return false;
    m_pipelines[grid.program.get()].program = grid.program;
    QRhiCommandBuffer* cb = nullptr;
    if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) return false;
    QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
    updates->updateDynamicBuffer(m_uniforms.get(), 0, sizeof uniforms, &uniforms);
    cb->beginPass(m_renderTarget.get(), Qt::black, {1.0f, 0}, updates);
    cb->setGraphicsPipeline(p);
    cb->setViewport({0, 0, float(size.width()), float(size.height())});
    cb->setShaderResources(m_bindings.get());
    cb->draw(3);
    QRhiReadbackResult readback;
    QRhiResourceUpdateBatch* read = rhi->nextResourceUpdateBatch();
    read->readBackTexture(QRhiReadbackDescription(m_target.get()), &readback);
    cb->endPass(read);
    // An offscreen frame completes, readbacks included, when it ends.
    rhi->endOffscreenFrame();
    const std::size_t points = std::size_t(size.width()) * std::size_t(size.height());
    grid.values.resize(points * 2);
    if (m_targetFormat == int(QRhiTexture::RGBA32F)) {
        if (std::size_t(readback.data.size()) < points * 16) return false;
        const auto* data = reinterpret_cast<const float*>(readback.data.constData());
        for (std::size_t i = 0; i < points; ++i) {
            grid.values[i * 2] = data[i * 4];
            grid.values[i * 2 + 1] = data[i * 4 + 1];
        }
    } else {
        if (std::size_t(readback.data.size()) < points * 8) return false;
        const auto* data = reinterpret_cast<const qfloat16*>(readback.data.constData());
        for (std::size_t i = 0; i < points; ++i) {
            grid.values[i * 2] = float(data[i * 4]);
            grid.values[i * 2 + 1] = float(data[i * 4 + 1]);
        }
    }
    return true;
}

const PointerMaps::Grid* PointerMaps::grid(const ShaderProgramPtr& program, const QSize& canvas,
                                           const ShaderParameterValues* overrides)
{
    // The uniforms of the evaluation: the canvas size, the parameters with
    // their live values, and the grid as the canvas the map is drawn on.
    TerminalUniforms u;
    std::memset(&u, 0, sizeof u);
    for (int i = 0; i < 4; ++i) u.base.qt_Matrix[i * 5] = 1.0f;
    u.base.qt_Opacity = 1.0f;
    u.base.iResolution[0] = float(canvas.width());
    u.base.iResolution[1] = float(canvas.height());
    std::memcpy(u.base.customParams, program->params().data(), sizeof u.base.customParams);
    std::memcpy(u.base.customColors, program->colors().data(), sizeof u.base.customColors);
    if (overrides) overrides->apply(u.base);
    std::array<float, 32> params{};
    std::array<float, 64> colors{};
    static_assert(sizeof params == sizeof u.base.customParams && sizeof colors == sizeof u.base.customColors);
    std::memcpy(params.data(), u.base.customParams, sizeof params);
    std::memcpy(colors.data(), u.base.customColors, sizeof colors);
    ++m_clock;
    for (Grid& g : m_grids) {
        if (g.program == program && g.canvas == canvas && g.params == params && g.colors == colors) {
            g.used = m_clock;
            return &g;
        }
    }
    if (!ensureResources()) return nullptr;
    Grid g;
    g.program = program;
    g.canvas = canvas;
    g.params = params;
    g.colors = colors;
    g.columns = GridColumns;
    g.rows = std::clamp(int(std::lround(double(GridColumns) * canvas.height() / canvas.width())), 8, 4 * GridColumns);
    g.used = m_clock;
    u.terminal.uCanvas[2] = float(g.columns);
    u.terminal.uCanvas[3] = float(g.rows);
    u.terminal.uTargetSize[0] = float(g.columns);
    u.terminal.uTargetSize[1] = float(g.rows);
    u.terminal.uWindowSize[0] = float(canvas.width());
    u.terminal.uWindowSize[1] = float(canvas.height());
    u.terminal.uTerminalRect[2] = float(canvas.width());
    u.terminal.uTerminalRect[3] = float(canvas.height());
    if (!evaluate(g, u)) {
        qCWarning(lcPointerMap) << "The pointer map of" << program->id() << "could not be evaluated";
        return nullptr;
    }
    if (m_grids.size() >= MaxGrids) {
        m_grids.erase(std::min_element(m_grids.begin(), m_grids.end(),
                                       [](const Grid& a, const Grid& b) { return a.used < b.used; }));
    }
    m_grids.push_back(std::move(g));
    return &m_grids.back();
}

QPointF PointerMaps::unwarp(const ShaderProgramPtr& program, const QSize& canvas, const ShaderParameterValues* overrides,
                            const QPointF& point)
{
    if (!program || !program->hasPointerMap() || canvas.isEmpty()) return point;
    const Grid* g = grid(program, canvas, overrides);
    if (!g) return point;
    // Grid points sit at the centres of their cells: bilinear between the
    // four around the point, held at the outer ones.
    const double gx = std::clamp(point.x() / canvas.width() * g->columns - 0.5, 0.0, double(g->columns - 1));
    const double gy = std::clamp(point.y() / canvas.height() * g->rows - 0.5, 0.0, double(g->rows - 1));
    const int x0 = int(gx), y0 = int(gy);
    const int x1 = std::min(x0 + 1, g->columns - 1), y1 = std::min(y0 + 1, g->rows - 1);
    const double fx = gx - x0, fy = gy - y0;
    auto at = [g](int x, int y, int c) { return double(g->values[(std::size_t(y) * std::size_t(g->columns) + std::size_t(x)) * 2 + std::size_t(c)]); };
    auto mix = [&](int c) {
        const double top = at(x0, y0, c) * (1 - fx) + at(x1, y0, c) * fx;
        const double bottom = at(x0, y1, c) * (1 - fx) + at(x1, y1, c) * fx;
        return top * (1 - fy) + bottom * fy;
    };
    const double u = mix(0), v = mix(1);
    if (!std::isfinite(u) || !std::isfinite(v)) return point;
    return QPointF(u * canvas.width(), v * canvas.height());
}

} // namespace AfterglowRender
