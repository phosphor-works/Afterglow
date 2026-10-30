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

QRhiScissor scissorFor(const QRect& rc, const QSize& px)
{
    // QRhi scissors use a bottom-left origin.
    const QRect c = rc.intersected(QRect(QPoint(0, 0), px));
    return QRhiScissor(c.x(), px.height() - (c.y() + c.height()), c.width(), c.height());
}

struct CompositeUniforms
{
    float framebufferSize[2];
    std::int32_t cursorBelowText;
    std::int32_t pad;
};

} // namespace

PostChain::PostChain(GpuDevice& device)
    : m_device(device)
{
    QFile f(QStringLiteral(":/afterglow/shaders/post.vert.qsb"));
    if (!f.open(QIODevice::ReadOnly)) {
        throw std::runtime_error("missing post.vert.qsb");
    }
    m_vertex = QShader::fromSerialized(f.readAll());
    QFile composite(QStringLiteral(":/afterglow/shaders/layers.frag.qsb"));
    if (!composite.open(QIODevice::ReadOnly)) {
        throw std::runtime_error("missing layers.frag.qsb");
    }
    m_compositeShader = QShader::fromSerialized(composite.readAll());
}

PostChain::~PostChain()
{
    release();
}

bool PostChain::animated() const noexcept
{
    for (const auto* chain : {&m_chains.background, &m_chains.text, &m_chains.cursor, &m_chains.pane, &m_chains.window}) {
        for (const ShaderProgramPtr& p : *chain) {
            if (p->animated()) {
                return true;
            }
        }
    }
    return false;
}

void PostChain::setChains(ShaderChains chains)
{
    if (chains.highDynamicRange() != m_chains.highDynamicRange()) {
        release(); // Texture formats and render-pass compatibility changed.
    }
    m_pipelines.clear();
    m_bindings.clear(); // Audio opt-in may change the texture at binding 3.
    m_chains = std::move(chains);
    m_historyValid = false;
}

void PostChain::release()
{
    m_pipelines.clear();
    m_composite.reset();
    m_bindings.clear();
    m_compositeBindings.clear();
    m_ubos.clear();
    m_compositeUbos.clear();
    m_historyTarget.reset();
    m_previousText.reset();
    m_historyRegions.clear();
    m_historyValid = false;
    for (auto& t : m_layerTargets) {
        t.reset();
    }
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
    const auto format = m_chains.highDynamicRange() ? QRhiTexture::RGBA16F : QRhiTexture::RGBA8;
    // Layer textures come and go with the layer chains.
    const bool layersReady = layered() == bool(m_textures[FirstLayerTexture]);
    const bool historyReady = m_chains.textHistory() == bool(m_previousText);
    if (size == m_size && m_textures[0] && layersReady && historyReady) {
        return m_gridTarget.get();
    }
    m_bindings.clear();
    m_compositeBindings.clear();
    m_pipelines.clear();
    m_composite.reset();
    m_historyTarget.reset();
    m_previousText.reset();
    m_historyValid = false;
    for (auto& t : m_layerTargets) {
        t.reset();
    }
    for (int i = FirstLayerTexture; i < TextureCount; ++i) {
        m_textures[i].reset();
    }
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
        m_textures[i].reset(rhi->newTexture(format, size, 1,
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
    if (layered()) {
        // Cleared like the grid target, so the grid's pipelines draw into them.
        for (int i = 0; i <= LayerCount; ++i) {
            auto& tex = m_textures[FirstLayerTexture + i];
            tex.reset(rhi->newTexture(format, size, 1,
                                     QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
            tex->create();
            m_layerTargets[i].reset(rhi->newTextureRenderTarget({tex.get()}));
            m_layerTargets[i]->setRenderPassDescriptor(m_gridPass.get());
            m_layerTargets[i]->create();
        }
    }
    if (m_chains.textHistory()) {
        m_previousText.reset(rhi->newTexture(format, size, 1, QRhiTexture::RenderTarget));
        m_previousText->create();
        m_historyTarget.reset(rhi->newTextureRenderTarget({m_previousText.get()}));
        m_historyTarget->setRenderPassDescriptor(m_gridPass.get());
        m_historyTarget->create();
    }
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
            QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage,
                                                      m_previousText ? m_previousText.get() : m_textures[texture].get(),
                                                      m_sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(3, QRhiShaderResourceBinding::FragmentStage,
                                                      m_chains.audioReactive() ? m_device.audioTexture() : m_textures[texture].get(),
                                                      m_device.atlasSampler()),
        });
        slot->create();
    }
    return slot.get();
}

QRhiGraphicsPipeline* PostChain::pipeline(const ShaderProgram& program, QRhiRenderPassDescriptor* pass)
{
    std::unique_ptr<QRhiGraphicsPipeline>& slot = m_pipelines[{&program, pass}];
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

QRhiShaderResourceBindings* PostChain::compositeBindings(int ubo, int background, int text, int cursor)
{
    while (int(m_compositeUbos.size()) <= ubo) {
        auto* b = m_device.rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(CompositeUniforms));
        b->create();
        m_compositeUbos.emplace_back(b);
    }
    auto& slot = m_compositeBindings[{ubo, background, text, cursor}];
    if (!slot) {
        const auto stage = QRhiShaderResourceBinding::FragmentStage;
        slot.reset(m_device.rhi()->newShaderResourceBindings());
        slot->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, stage, m_compositeUbos[std::size_t(ubo)].get()),
            QRhiShaderResourceBinding::sampledTexture(1, stage, m_textures[background].get(), m_sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(2, stage, m_textures[text].get(), m_sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(3, stage, m_textures[cursor].get(), m_sampler.get()),
        });
        slot->create();
    }
    return slot.get();
}

QRhiGraphicsPipeline* PostChain::compositePipeline()
{
    if (m_composite) {
        return m_composite.get();
    }
    const int first = FirstLayerTexture;
    m_composite.reset(m_device.rhi()->newGraphicsPipeline());
    m_composite->setTopology(QRhiGraphicsPipeline::Triangles);
    m_composite->setFlags(QRhiGraphicsPipeline::UsesScissor);
    m_composite->setShaderStages({{QRhiShaderStage::Vertex, m_vertex}, {QRhiShaderStage::Fragment, m_compositeShader}});
    m_composite->setVertexInputLayout({});
    m_composite->setShaderResourceBindings(compositeBindings(0, first, first + 1, first + 2));
    m_composite->setRenderPassDescriptor(m_gridPass.get());
    if (!m_composite->create()) {
        throw std::runtime_error("failed to create the layer composite pipeline");
    }
    return m_composite.get();
}

QRhiTextureRenderTarget* PostChain::layerTarget(Layer layer)
{
    const int i = int(layer);
    m_layerTexture[i] = FirstLayerTexture + i;
    return m_layerTargets[i].get();
}

int PostChain::runLayerChain(QRhiCommandBuffer* cb, const std::vector<ShaderProgramPtr>& chain, int input,
                             const std::vector<ShaderRegionState>& panes, const ShaderFrameTime& time, int& ubo)
{
    QRhi* rhi = m_device.rhi();
    int current = input;
    for (const ShaderProgramPtr& program : chain) {
        // The layer textures not holding a layer: one is always free.
        int out = FirstLayerTexture;
        while (out == m_layerTexture[0] || out == m_layerTexture[1] || out == m_layerTexture[2]) {
            ++out;
        }
        QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
        const int firstUbo = ubo;
        for (const ShaderRegionState& pane : panes) {
            TerminalShaderUniforms u;
            fill(u, pane, time, *program);
            updates->updateDynamicBuffer(uniformBuffer(ubo++), 0, sizeof u, &u);
        }
        // Outside the panes the layer stays transparent.
        cb->beginPass(m_layerTargets[out - FirstLayerTexture].get(), Qt::transparent, {1.0f, 0}, updates);
        cb->setGraphicsPipeline(pipeline(*program, m_gridPass.get()));
        cb->setViewport({0, 0, float(m_size.width()), float(m_size.height())});
        for (std::size_t i = 0; i < panes.size(); ++i) {
            cb->setScissor(scissorFor(panes[i].rect, m_size));
            cb->setShaderResources(bindings(firstUbo + int(i), current));
            cb->draw(3);
        }
        cb->endPass();
        for (int& t : m_layerTexture) {
            if (t == current) {
                t = out;
            }
        }
        current = out;
    }
    return current;
}

void PostChain::composite(QRhiCommandBuffer* cb, const std::vector<ShaderRegionState>& panes,
                          const std::vector<bool>& cursorBelowText, const ShaderFrameTime& time, const QColor& clear)
{
    if (m_previousText) {
        std::vector<HistoryRegion> regions;
        for (const auto& pane : panes) {
            regions.emplace_back(pane.snapshot, pane.rect, pane.snapshot ? pane.snapshot->defaultBg : 0,
                                 pane.snapshot ? pane.snapshot->defaultFg : 0, pane.cellSize);
        }
        // Never carry another tab's text, or text in its old position, into
        // newly exposed panes. Clock resets also start a fresh history.
        if (!m_historyValid || regions != m_historyRegions || time.time < m_historyTime) {
            cb->beginPass(m_historyTarget.get(), Qt::transparent, {1.0f, 0});
            cb->endPass();
        }
        m_historyRegions = std::move(regions);
        m_historyTime = time.time;
    }
    int ubo = m_nextUbo;
    const int background = runLayerChain(cb, m_chains.background, m_layerTexture[0], panes, time, ubo);
    const int text = runLayerChain(cb, m_chains.text, m_layerTexture[1], panes, time, ubo);
    const int cursor = runLayerChain(cb, m_chains.cursor, m_layerTexture[2], panes, time, ubo);
    m_nextUbo = ubo;

    QRhiResourceUpdateBatch* updates = m_device.rhi()->nextResourceUpdateBatch();
    for (std::size_t i = 0; i < panes.size(); ++i) {
        compositeBindings(int(i), background, text, cursor); // makes the buffer
        const CompositeUniforms u{{float(m_size.width()), float(m_size.height())},
                                  i < cursorBelowText.size() && cursorBelowText[i] ? 1 : 0, 0};
        updates->updateDynamicBuffer(m_compositeUbos[i].get(), 0, sizeof u, &u);
    }
    cb->beginPass(m_gridTarget.get(), clear, {1.0f, 0}, updates);
    cb->setGraphicsPipeline(compositePipeline());
    cb->setViewport({0, 0, float(m_size.width()), float(m_size.height())});
    for (std::size_t i = 0; i < panes.size(); ++i) {
        cb->setScissor(scissorFor(panes[i].rect, m_size));
        cb->setShaderResources(compositeBindings(int(i), background, text, cursor));
        cb->draw(3);
    }
}

void PostChain::finishFrame(QRhiCommandBuffer* cb)
{
    if (m_previousText) {
        auto* updates = m_device.rhi()->nextResourceUpdateBatch();
        updates->copyTexture(m_previousText.get(), m_textures[m_layerTexture[int(Layer::Text)]].get());
        cb->resourceUpdate(updates);
        m_historyValid = true;
    }
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
    u.iBackgroundOpacity = s.backgroundOpacity;
    std::copy(s.scrollback.begin(), s.scrollback.end(), u.iScrollback);
    if (program.requiresAudio()) {
        std::copy(m_device.audioLevels().begin(), m_device.audioLevels().end(), u.iAudioLevels);
        u.iAudioSpectrumSize = m_device.audioSpectrumSize();
    }
    std::memcpy(u.customColors, program.colors().data(), sizeof u.customColors);
    for (std::size_t i = 0; i < s.signals.size(); ++i) {
        u.iSignal[i][0] = s.signals[i][0];
        u.iSignal[i][1] = s.signals[i][1];
    }
}

void PostChain::record(QRhiCommandBuffer* cb, const std::vector<ShaderRegionState>& panes,
                       const ShaderRegionState& window, const ShaderFrameTime& time, QRhiRenderTarget* swapTarget,
                       QRhiRenderPassDescriptor* swapPass)
{
    QRhi* rhi = m_device.rhi();
    const QSize px = m_size;
    int current = 0; // texture holding the latest image
    int ubo = m_nextUbo;

    // Pane chains: each step copies the input, then reshades every pane region.
    for (const ShaderProgramPtr& program : m_chains.pane) {
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
        cb->setGraphicsPipeline(pipeline(*program, m_renderPass.get()));
        cb->setViewport({0, 0, float(px.width()), float(px.height())});
        for (std::size_t i = 0; i < panes.size(); ++i) {
            cb->setScissor(scissorFor(panes[i].rect, px));
            cb->setShaderResources(bindings(firstUbo + int(i), current));
            cb->draw(3);
        }
        cb->endPass();
        current = out;
    }

    // Window chain; the last step (or a passthrough) writes to the swapchain.
    std::vector<ShaderProgramPtr> steps = m_chains.window;
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
        cb->setGraphicsPipeline(pipeline(program, last ? swapPass : m_renderPass.get()));
        cb->setViewport({0, 0, float(px.width()), float(px.height())});
        cb->setScissor(scissorFor(QRect(QPoint(0, 0), px), px));
        cb->setShaderResources(bindings(ubo, current));
        cb->draw(3);
        ++ubo;
        if (!last) {
            cb->endPass();
            current = out;
        }
    }
    m_nextUbo = 0; // the frame's passes are all recorded
}

} // namespace AfterglowRender
