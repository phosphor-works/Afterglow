// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowRender/GpuDevice.h>
#include <AfterglowRender/PostChain.h>
#include <AfterglowRender/Snapshot.h>

#include <QDateTime>
#include <QFile>
#include <rhi/qrhi.h>

#include <cstring>
#include <stdexcept>

namespace AfterglowRender {

namespace {

void setRgb(float* out, std::uint32_t rgba)
{
    out[0] = float(rgba & 0xFF) / 255.0f;
    out[1] = float((rgba >> 8) & 0xFF) / 255.0f;
    out[2] = float((rgba >> 16) & 0xFF) / 255.0f;
    out[3] = 1.0f;
}

void setColor(float* out, const QColor& c)
{
    out[0] = float(c.redF());
    out[1] = float(c.greenF());
    out[2] = float(c.blueF());
    out[3] = float(c.alphaF());
}

/// A cursor rectangle in region coordinates, y-up, as (left, top, width, height).
void setCursor(float* out, const QRectF& c, const QRect& region)
{
    out[0] = float(c.x() - region.x());
    out[1] = float((region.y() + region.height()) - c.y());
    out[2] = float(c.width());
    out[3] = float(c.height());
}

} // namespace

PostChain::PostChain(GpuDevice& device)
    : m_device(device)
{
    QFile f(QStringLiteral(":/afterglow/shaders/post.vert.qsb"));
    if (!f.open(QIODevice::ReadOnly)) {
        throw std::runtime_error("missing post.vert.qsb");
    }
    m_vertex = QShader::fromSerialized(f.readAll());
}

PostChain::~PostChain()
{
    release();
}

bool PostChain::animated() const noexcept
{
    for (const auto* chain : {&m_pane, &m_window}) {
        for (const ShaderProgramPtr& p : *chain) {
            if (p->animated()) {
                return true;
            }
        }
    }
    return false;
}

void PostChain::setChains(std::vector<ShaderProgramPtr> pane, std::vector<ShaderProgramPtr> window)
{
    m_pipelines.clear();
    m_pane = std::move(pane);
    m_window = std::move(window);
}

void PostChain::release()
{
    m_pipelines.clear();
    m_bindings.clear();
    m_ubos.clear();
    m_gridTarget.reset();
    m_gridPass.reset();
    for (auto& t : m_targets) {
        t.reset();
    }
    m_renderPass.reset();
    for (auto& t : m_textures) {
        t.reset();
    }
    m_sampler.reset();
    m_size = QSize();
}

QRhiTextureRenderTarget* PostChain::prepare(const QSize& size)
{
    QRhi* rhi = m_device.rhi();
    if (size == m_size && m_textures[0]) {
        return m_gridTarget.get();
    }
    m_bindings.clear();
    m_pipelines.clear();
    m_gridTarget.reset();
    m_gridPass.reset();
    for (auto& t : m_targets) {
        t.reset();
    }
    m_renderPass.reset();
    if (!m_sampler) {
        m_sampler.reset(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                        QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        m_sampler->create();
    }
    for (int i = 0; i < 2; ++i) {
        m_textures[i].reset(rhi->newTexture(QRhiTexture::RGBA8, size, 1,
                                            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        m_textures[i]->create();
        // Pane passes only redraw pane regions; the rest is the copied input.
        m_targets[i].reset(rhi->newTextureRenderTarget({m_textures[i].get()},
                                                       QRhiTextureRenderTarget::PreserveColorContents));
        if (!m_renderPass) {
            m_renderPass.reset(m_targets[i]->newCompatibleRenderPassDescriptor());
        }
        m_targets[i]->setRenderPassDescriptor(m_renderPass.get());
        m_targets[i]->create();
    }
    m_gridTarget.reset(rhi->newTextureRenderTarget({m_textures[0].get()}));
    m_gridPass.reset(m_gridTarget->newCompatibleRenderPassDescriptor());
    m_gridTarget->setRenderPassDescriptor(m_gridPass.get());
    m_gridTarget->create();
    m_size = size;
    return m_gridTarget.get();
}

QRhiBuffer* PostChain::uniformBuffer(int index)
{
    while (int(m_ubos.size()) <= index) {
        auto* b = m_device.rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                            sizeof(TerminalShaderUniforms));
        b->create();
        m_ubos.emplace_back(b);
    }
    return m_ubos[std::size_t(index)].get();
}

QRhiShaderResourceBindings* PostChain::bindings(int ubo, int texture)
{
    auto& slot = m_bindings[{ubo, texture}];
    if (!slot) {
        slot.reset(m_device.rhi()->newShaderResourceBindings());
        slot->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, QRhiShaderResourceBinding::FragmentStage, uniformBuffer(ubo)),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage,
                                                      m_textures[texture].get(), m_sampler.get()),
        });
        slot->create();
    }
    return slot.get();
}

QRhiGraphicsPipeline* PostChain::pipeline(const ShaderProgram& program, QRhiRenderPassDescriptor* pass,
                                          bool toSwapchain)
{
    Pipelines& p = m_pipelines[&program];
    std::unique_ptr<QRhiGraphicsPipeline>& slot = toSwapchain ? p.swapchain : p.offscreen;
    if (slot) {
        return slot.get();
    }
    slot.reset(m_device.rhi()->newGraphicsPipeline());
    slot->setTopology(QRhiGraphicsPipeline::Triangles);
    slot->setFlags(QRhiGraphicsPipeline::UsesScissor);
    slot->setShaderStages({{QRhiShaderStage::Vertex, m_vertex}, {QRhiShaderStage::Fragment, program.shader()}});
    slot->setVertexInputLayout({});
    slot->setShaderResourceBindings(bindings(0, 0));
    slot->setRenderPassDescriptor(pass);
    if (!slot->create()) {
        throw std::runtime_error("failed to create the pipeline for shader " + program.id().toStdString());
    }
    return slot.get();
}

void PostChain::fill(TerminalShaderUniforms& u, const ShaderRegionState& s, const ShaderFrameTime& t,
                     const ShaderProgram& program) const
{
    std::memset(&u, 0, sizeof u);
    const QRect& r = s.rect;
    u.iResolution[0] = float(r.width());
    u.iResolution[1] = float(r.height());
    u.iResolution[2] = 1.0f;
    u.iTime = t.time;
    u.iTimeDelta = t.delta;
    u.iFrameRate = t.delta > 0 ? 1.0f / t.delta : 60.0f;
    u.iFrame = t.frame;
    u.iChannelResolution[0][0] = float(r.width());
    u.iChannelResolution[0][1] = float(r.height());
    u.iChannelResolution[0][2] = 1.0f;
    u.iMouse[0] = float(s.mouse.x() - r.x());
    u.iMouse[1] = float((r.y() + r.height()) - s.mouse.y());
    const QDateTime now = QDateTime::currentDateTime();
    u.iDate[0] = float(now.date().year());
    u.iDate[1] = float(now.date().month() - 1);
    u.iDate[2] = float(now.date().day());
    u.iDate[3] = float(now.time().msecsSinceStartOfDay()) / 1000.0f;
    u.iSampleRate = 44100.0f;

    setCursor(u.iCurrentCursor, s.cursor, r);
    setCursor(u.iPreviousCursor, s.previousCursor, r);
    setColor(u.iCurrentCursorColor, s.cursorColor);
    setColor(u.iPreviousCursorColor, s.previousCursorColor);
    u.iCurrentCursorStyle = s.cursorStyle;
    u.iPreviousCursorStyle = s.previousCursorStyle;
    u.iCursorVisible = s.cursorVisible ? 1 : 0;
    u.iTimeCursorChange = s.cursorChangeTime;
    u.iTimeFocus = s.focusChangeTime;
    u.iFocus = s.focused ? 1 : 0;

    if (const PaneSnapshot* snap = s.snapshot) {
        for (int i = 0; i < 256; ++i) {
            setRgb(u.iPalette[i], snap->palette[std::size_t(i)]);
        }
        setRgb(u.iBackgroundColor, snap->defaultBg);
        setRgb(u.iForegroundColor, snap->defaultFg);
        setRgb(u.iCursorColor, snap->cursorColor);
        setRgb(u.iCursorText, snap->defaultBg);
        setRgb(u.iSelectionForegroundColor, snap->defaultBg);
        setRgb(u.iSelectionBackgroundColor, snap->defaultFg);
    }

    u.iRegion[0] = float(r.x());
    u.iRegion[1] = float(r.y());
    u.iRegion[2] = float(r.width());
    u.iRegion[3] = float(r.height());
    u.iFramebufferSize[0] = float(m_size.width());
    u.iFramebufferSize[1] = float(m_size.height());
    u.iCellSize[0] = float(s.cellSize.width());
    u.iCellSize[1] = float(s.cellSize.height());
    std::memcpy(u.customParams, program.params().data(), sizeof u.customParams);
    std::memcpy(u.customColors, program.colors().data(), sizeof u.customColors);
}

void PostChain::record(QRhiCommandBuffer* cb, const std::vector<ShaderRegionState>& panes,
                       const ShaderRegionState& window, const ShaderFrameTime& time, QRhiRenderTarget* swapTarget,
                       QRhiRenderPassDescriptor* swapPass)
{
    QRhi* rhi = m_device.rhi();
    const QSize px = m_size;
    auto scissor = [&px](const QRect& rc) {
        const QRect c = rc.intersected(QRect(QPoint(0, 0), px));
        return QRhiScissor(c.x(), px.height() - (c.y() + c.height()), c.width(), c.height());
    };
    int current = 0; // texture holding the latest image
    int ubo = 0;

    // Pane chains: each step copies the input, then reshades every pane region.
    for (const ShaderProgramPtr& program : m_pane) {
        const int out = 1 - current;
        QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
        updates->copyTexture(m_textures[out].get(), m_textures[current].get());
        const int firstUbo = ubo;
        for (const ShaderRegionState& pane : panes) {
            TerminalShaderUniforms u;
            fill(u, pane, time, *program);
            updates->updateDynamicBuffer(uniformBuffer(ubo++), 0, sizeof u, &u);
        }
        cb->beginPass(m_targets[out].get(), Qt::black, {1.0f, 0}, updates);
        cb->setGraphicsPipeline(pipeline(*program, m_renderPass.get(), false));
        cb->setViewport({0, 0, float(px.width()), float(px.height())});
        for (std::size_t i = 0; i < panes.size(); ++i) {
            cb->setScissor(scissor(panes[i].rect));
            cb->setShaderResources(bindings(firstUbo + int(i), current));
            cb->draw(3);
        }
        cb->endPass();
        current = out;
    }

    // Window chain; the last step (or a passthrough) writes to the swapchain.
    std::vector<ShaderProgramPtr> steps = m_window;
    if (steps.empty()) {
        steps.push_back(ShaderProgram::passthrough());
    }
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const ShaderProgram& program = *steps[i];
        const bool last = i + 1 == steps.size();
        const int out = 1 - current;
        QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
        TerminalShaderUniforms u;
        fill(u, window, time, program);
        updates->updateDynamicBuffer(uniformBuffer(ubo), 0, sizeof u, &u);
        cb->beginPass(last ? swapTarget : m_targets[out].get(), Qt::black, {1.0f, 0}, updates);
        cb->setGraphicsPipeline(pipeline(program, last ? swapPass : m_renderPass.get(), last));
        cb->setViewport({0, 0, float(px.width()), float(px.height())});
        cb->setScissor(scissor(QRect(QPoint(0, 0), px)));
        cb->setShaderResources(bindings(ubo, current));
        cb->draw(3);
        ++ubo;
        if (!last) {
            cb->endPass();
            current = out;
        }
    }
}

} // namespace AfterglowRender
