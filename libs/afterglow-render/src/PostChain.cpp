// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowRender/RenderFrame.h>

#include <AfterglowRender/GpuDevice.h>
#include <AfterglowRender/PostChain.h>
#include <AfterglowRender/Snapshot.h>

#include <PhosphorShaders/ShaderBindings.h>

#include <QFile>
#include <QLoggingCategory>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

Q_LOGGING_CATEGORY(lcRender, "afterglow.render", QtInfoMsg)

namespace AfterglowRender {

namespace {

int meshSamples(const ShaderProgram& program, QRhi* rhi)
{
    const int requested = program.geometry().samples;
    return program.meshPass() && rhi->supportedSampleCounts().contains(requested) ? requested : 1;
}

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

/// A rectangle in canvas coordinates, as (x, y, width, height).
void setRect(float* out, const QRectF& c, const QRect& canvas)
{
    out[0] = float(c.x() - canvas.x());
    out[1] = float(c.y() - canvas.y());
    out[2] = float(c.width());
    out[3] = float(c.height());
}

/// Seconds since @p when on the shader clock, in double until the result;
/// events that never happened (stamped -1e6) read as 1e6.
float age(double now, double when)
{
    return float(std::clamp(now - when, 0.0, 1e6));
}

/// uPreviousText in terminal_history.glsl: the first binding above Phosphor's table.
constexpr int PreviousTextBinding = PhosphorShaders::Bindings::kExtraBase;
/// TerminalScene in terminal_scene.glsl: object matrices for mesh passes.
constexpr int SceneBinding = PreviousTextBinding + 1;
constexpr int PanesBinding = SceneBinding + 1;
constexpr int TabImageBinding = PanesBinding + 1;
constexpr int TabAtlasBinding = TabImageBinding + 1;

/// Shrinks the window-sized tab image into an atlas tile: each tile pixel
/// averages a 3x3 grid across the source pixels it covers.
const ShaderProgram& shrinkProgram()
{
    static const std::shared_ptr<ShaderProgram> program = [] {
        ShaderProgramSource source;
        source.id = QStringLiteral("overview-shrink");
        source.code = "vec4 pTerminal(vec2 uv) {\n"
                      "    vec2 tilePixel = 1.0 / iResolution;\n"
                      "    vec4 sum = vec4(0.0);\n"
                      "    for (int y = -1; y <= 1; ++y)\n"
                      "        for (int x = -1; x <= 1; ++x) sum += terminalTexel(uv + vec2(x, y) * tilePixel / 3.0);\n"
                      "    return sum / 9.0;\n"
                      "}\n";
        QString error;
        auto compiled = ShaderProgram::compile(source, &error);
        if (!compiled) throw std::runtime_error("overview shrink shader failed: " + error.toStdString());
        return compiled;
    }();
    return *program;
}
static_assert(PhosphorShaders::Bindings::kUserTextureCount == MaxShaderImages);

/// Image @p i of @p program, or the region's override for it.
const QImage& imageFor(const ShaderProgram& program, const ShaderParameterValues* overrides, int i)
{
    if (overrides && (overrides->imageMask & (1u << i))) return overrides->images[std::size_t(i)];
    return program.images()[std::size_t(i)];
}

struct CompositeUniforms
{
    float framebufferSize[2];
    std::int32_t cursorBelowText;
    std::int32_t pad;
};

} // namespace

// Textures precede their dependants so targets/pass descriptors die first.
struct PostChain::BufferState {
    std::unique_ptr<QRhiTexture> textures[2];
    std::unique_ptr<QRhiTexture> depths[2];
    std::unique_ptr<QRhiRenderBuffer> hardwareDepth;
    std::unique_ptr<QRhiTexture> multisampleColor;
    std::unique_ptr<QRhiTexture> multisampleAuxiliary;
    std::unique_ptr<QRhiSampler> sampler;
    QRhiRenderPassDescriptor* pass = nullptr; ///< shared, owned by PostChain::m_bufferPasses
    std::unique_ptr<QRhiTextureRenderTarget> targets[2];
    QRhiBuffer* vertices = nullptr; ///< shared, owned by PostChain::m_meshVertices
    std::vector<HistoryRegion> regions;
    QSize size;
    QSize wanted; // the size the pass asks for; `size` is smaller when the budget is short
    int read = 0;
    int frame = 0;
    double time = 0;
    bool blocked = false;
    /// What the budget log last said about this pass, carried to the state
    /// that replaces it so a moving rectangle logs only a change of outcome.
    enum class Logged { None, Shrunk, Blocked } logged = Logged::None;
    std::uint64_t bytes = 0, used = 0;
};

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

bool PostChain::showing(const std::vector<ShaderProgramPtr>& chain) const noexcept
{
    return &chain == eventChain();
}

const std::vector<ShaderProgramPtr>* PostChain::eventChain() const noexcept
{
    if (m_overviewShowing && !m_chains.overview.empty()) return &m_chains.overview;
    if (m_transitionShowing && !m_chains.tabTransition.empty()) return &m_chains.tabTransition;
    if (m_paneEffectsShowing && !m_chains.paneEffects.empty()) return &m_chains.paneEffects;
    return nullptr;
}

bool PostChain::audioReactive() const
{
    if (m_chains.anyAlwaysOn([](const ShaderProgram& p) { return p.requiresAudio(); })) return true;
    // Pane effects run under the overview and a tab switch too (record()).
    for (const auto* chain : {&m_chains.overview, &m_chains.tabTransition, &m_chains.paneEffects}) {
        const bool runs = chain == &m_chains.paneEffects ? m_paneEffectsShowing : showing(*chain);
        if (runs && ShaderChains::anyIn(*chain, [](const ShaderProgram& p) { return p.requiresAudio(); })) return true;
    }
    return false;
}

bool PostChain::highDynamicRange() const
{
    return anyRunning([](const ShaderProgram& p) { return p.requiresHdr(); })
        || (m_paneEffectsShowing && ShaderChains::anyIn(m_chains.paneEffects, [](const ShaderProgram& p) { return p.requiresHdr(); }));
}

bool PostChain::textHistory() const
{
    return anyRunning([](const ShaderProgram& p) { return p.readsPreviousText(); })
        || (m_paneEffectsShowing && ShaderChains::anyIn(m_chains.paneEffects, [](const ShaderProgram& p) { return p.readsPreviousText(); }));
}

bool PostChain::layered() const
{
    return !m_chains.background.empty() || !m_chains.text.empty() || !m_chains.cursor.empty() || textHistory();
}

void PostChain::setChains(ShaderChains chains)
{
    // A change of texture format (HDR) is seen and handled by prepare().
    m_pipelines.clear();
    m_bindings.clear(); // they bind textures released below
    m_buffers.clear();
    m_meshVertices.clear();
    m_bufferInstances.clear();
    m_localTextures.clear();
    m_parameterTextures.clear();
    m_emptyTexture.reset();
    m_bufferBytes = 0;
    m_regionBudgetWarned = m_imageBudgetWarned = false;
    m_chains = std::move(chains);
    m_historyValid = false;
}

void PostChain::releaseTabImages()
{
    m_tabImageTarget.reset();
    m_tabAtlasTarget.reset();
    m_tabAtlasClearTarget.reset();
    m_tabImage.reset();
    m_tabAtlas.reset();
}

void PostChain::releaseTextures()
{
    // Bindings and copies refer to the textures; targets go before them.
    m_bindings.clear();
    m_compositeBindings.clear();
    m_buffers.clear();
    m_meshVertices.clear();
    m_bufferInstances.clear();
    m_localTextures.clear();
    m_parameterTextures.clear();
    m_bufferBytes = 0;
    releaseTabImages();
    m_historyTarget.reset();
    m_previousText.reset();
    m_historyRegions.clear();
    m_historyValid = false;
    for (auto& t : m_layerTargets) t.reset();
    for (auto& target : m_windowTargets) target.reset();
    m_gridTarget.reset();
    for (auto& t : m_targets) t.reset();
    for (auto& t : m_textures) t.reset();
    m_size = QSize();
}

void PostChain::releaseIdle()
{
    releaseTextures();
}

void PostChain::forgetPass(const QRhiRenderPassDescriptor* pass)
{
    std::erase_if(m_pipelines, [pass](const auto& item) { return item.first.second == pass; });
}

void PostChain::release()
{
    releaseTextures();
    m_pipelines.clear();
    m_composite.reset();
    m_bufferPasses.clear();
    m_emptyTexture.reset();
    m_ubos.clear();
    m_sceneUbos.clear();
    m_panesUbo.reset();
    m_compositeUbos.clear();
    m_gridPass.reset();
    m_renderPass.reset();
    m_sampler.reset();
    m_format = -1;
    ++m_passGeneration;
}

QRhiTextureRenderTarget* PostChain::prepare(const QSize& size)
{
    QRhi* rhi = m_device.rhi();
    ++m_frame;
    m_bindings.clear();
    m_tabAtlasCleared = false;
    std::erase_if(m_localTextures, [this](const auto& item) { return item.second.used + 1 < m_frame; });
    std::erase_if(m_parameterTextures, [this](const auto& item) { return item.second.used + 1 < m_frame; });
    bool expired = false;
    for (auto it = m_buffers.begin(); it != m_buffers.end();) {
        if (it->second[0] && it->second[0]->used + 1 < m_frame) {
            for (const auto& pass : it->second) if (pass) m_bufferBytes -= pass->bytes;
            it = m_buffers.erase(it); expired = true;
        } else ++it;
    }
    if (expired) {
        // Pipelines stay: the descriptors they were made for are shared and live on.
        m_bufferInstances.clear();
        for (const auto& [key, passes] : m_buffers) {
            if (std::any_of(passes.begin(), passes.end(), [](const auto& pass) { return pass && !pass->blocked; }))
                m_bufferInstances.emplace(std::get<0>(key), std::get<1>(key), std::get<2>(key));
        }
    }
    const auto format = highDynamicRange() ? QRhiTexture::RGBA16F : QRhiTexture::RGBA8;
    if (int(format) != m_format) {
        // Texture formats and render-pass compatibility change: start over.
        release();
        m_format = int(format);
    }
    // The tab image (overview cards, the tab a switch leaves, a closing pane)
    // lives while frames use it, and goes after one that did not.
    if (!std::exchange(m_tabImageUsed, false) || size != m_size) releaseTabImages();
    // Layer textures come and go with the layer chains.
    const bool layersReady = layered() == bool(m_textures[FirstLayerTexture]);
    const bool historyReady = textHistory() == bool(m_previousText);
    if (size == m_size && m_textures[0] && layersReady && historyReady) {
        return m_gridTarget.get();
    }
    // A new size or set of layers: new textures and targets. The render-pass
    // descriptors hold no size, so they and the pipelines made for them stay.
    releaseTextures();
    if (!m_sampler) {
        m_sampler.reset(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                        QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
        if (!m_sampler->create()) throw std::runtime_error("shader sampler allocation failed");
    }
    for (int i = 0; i < 2; ++i) {
        m_textures[i].reset(rhi->newTexture(format, size, 1,
                                            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!m_textures[i]->create()) throw std::runtime_error("shader texture allocation failed");
        // Pane passes only redraw pane regions; the rest is the copied input.
        m_targets[i].reset(rhi->newTextureRenderTarget({m_textures[i].get()},
                                                       QRhiTextureRenderTarget::PreserveColorContents));
        if (!m_renderPass) {
            m_renderPass.reset(m_targets[i]->newCompatibleRenderPassDescriptor());
        }
        m_targets[i]->setRenderPassDescriptor(m_renderPass.get());
        if (!m_targets[i]->create()) throw std::runtime_error("shader target allocation failed");
    }
    m_gridTarget.reset(rhi->newTextureRenderTarget({m_textures[0].get()}));
    if (!m_gridPass) {
        m_gridPass.reset(m_gridTarget->newCompatibleRenderPassDescriptor());
        ++m_passGeneration;
    }
    m_gridTarget->setRenderPassDescriptor(m_gridPass.get());
    if (!m_gridTarget->create()) throw std::runtime_error("grid target allocation failed");
    // Vulkan load operations belong to the render-pass descriptor. Reuse the
    // clearing grid descriptor, not the preserving pane-pass descriptor.
    for (int i = 0; i < 2; ++i) {
        m_windowTargets[i].reset(rhi->newTextureRenderTarget({m_textures[i].get()}));
        m_windowTargets[i]->setRenderPassDescriptor(m_gridPass.get());
        if (!m_windowTargets[i]->create()) throw std::runtime_error("window shader target allocation failed");
    }
    if (layered()) {
        // Cleared like the grid target, so the grid's pipelines draw into them.
        for (int i = 0; i <= LayerCount; ++i) {
            auto& tex = m_textures[FirstLayerTexture + i];
            tex.reset(rhi->newTexture(format, size, 1,
                                     QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
            if (!tex->create()) throw std::runtime_error("layer texture allocation failed");
            m_layerTargets[i].reset(rhi->newTextureRenderTarget({tex.get()}));
            m_layerTargets[i]->setRenderPassDescriptor(m_gridPass.get());
            if (!m_layerTargets[i]->create()) throw std::runtime_error("layer target allocation failed");
        }
    }
    if (textHistory()) {
        m_previousText.reset(rhi->newTexture(format, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        if (!m_previousText->create()) throw std::runtime_error("text history allocation failed");
        m_historyTarget.reset(rhi->newTextureRenderTarget({m_previousText.get()}));
        m_historyTarget->setRenderPassDescriptor(m_gridPass.get());
        if (!m_historyTarget->create()) throw std::runtime_error("text history target allocation failed");
    }
    m_size = size;
    return m_gridTarget.get();
}

QRhiBuffer* PostChain::uniformBuffer(int index)
{
    while (int(m_ubos.size()) <= index) {
        std::unique_ptr<QRhiBuffer> b(m_device.rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                                                sizeof(TerminalUniforms)));
        if (!b->create()) throw std::runtime_error("shader uniform buffer allocation failed");
        m_ubos.push_back(std::move(b));
    }
    return m_ubos[std::size_t(index)].get();
}

QRhiBuffer* PostChain::panesBuffer()
{
    if (!m_panesUbo) {
        m_panesUbo.reset(m_device.rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(TerminalPaneUniforms)));
        if (!m_panesUbo->create()) throw std::runtime_error("pane uniform buffer allocation failed");
    }
    return m_panesUbo.get();
}

void PostChain::uploadPanes(QRhiResourceUpdateBatch* updates, const TerminalPaneUniforms& panes)
{
    updates->updateDynamicBuffer(panesBuffer(), 0, sizeof panes, &panes);
}

QRhiTextureRenderTarget* PostChain::tabImageTarget()
{
    m_tabImageUsed = true;
    if (!m_tabImageTarget) {
        QRhi* rhi = m_device.rhi();
        m_tabImage.reset(rhi->newTexture(QRhiTexture::Format(m_format), m_size, 1, QRhiTexture::RenderTarget));
        if (!m_tabImage->create()) throw std::runtime_error("overview tab allocation failed");
        m_tabImageTarget.reset(rhi->newTextureRenderTarget({m_tabImage.get()}));
        m_tabImageTarget->setRenderPassDescriptor(m_gridPass.get());
        if (!m_tabImageTarget->create()) throw std::runtime_error("overview tab target failed");
    }
    return m_tabImageTarget.get();
}

void PostChain::shrinkTabToAtlas(RenderFrame& frame, const QRect& tile)
{
    if (!m_tabImage || tile.isEmpty()) return;
    QRhi* rhi = m_device.rhi();
    if (!m_tabAtlas) {
        // Only overview cards use the atlas, so only they allocate it.
        m_tabAtlas.reset(rhi->newTexture(QRhiTexture::Format(m_format), m_size, 1, QRhiTexture::RenderTarget));
        if (!m_tabAtlas->create()) throw std::runtime_error("overview atlas allocation failed");
        m_tabAtlasTarget.reset(rhi->newTextureRenderTarget({m_tabAtlas.get()},
                                                           QRhiTextureRenderTarget::PreserveColorContents));
        m_tabAtlasTarget->setRenderPassDescriptor(m_renderPass.get());
        m_tabAtlasClearTarget.reset(rhi->newTextureRenderTarget({m_tabAtlas.get()}));
        m_tabAtlasClearTarget->setRenderPassDescriptor(m_gridPass.get());
        if (!m_tabAtlasTarget->create() || !m_tabAtlasClearTarget->create())
            throw std::runtime_error("overview atlas target failed");
    }
    // The frame's first tile clears the atlas, so the parts no tile covers
    // (division remainders, bilinear taps at card edges) read transparent.
    const bool clear = !std::exchange(m_tabAtlasCleared, true);
    auto* cb = frame.commands();
    const ShaderProgram& program = shrinkProgram();
    const int ubo = m_nextUbo++;
    TerminalUniforms u{};
    u.base.iResolution[0] = float(tile.width());
    u.base.iResolution[1] = float(tile.height());
    u.terminal.uCanvas[0] = float(tile.x());
    u.terminal.uCanvas[1] = float(tile.y());
    u.terminal.uCanvas[2] = float(tile.width());
    u.terminal.uCanvas[3] = float(tile.height());
    u.terminal.uTargetSize[0] = float(m_size.width());
    u.terminal.uTargetSize[1] = float(m_size.height());
    QRhiResourceUpdateBatch* updates = frame.stage(rhi->nextResourceUpdateBatch());
    updates->updateDynamicBuffer(uniformBuffer(ubo), 0, sizeof u, &u);
    auto* resources = bindings(ubo, 0, nullptr, &program, nullptr, nullptr, m_tabImage.get());
    QRhiRenderPassDescriptor* pass = clear ? m_gridPass.get() : m_renderPass.get();
    frame.beginPass(clear ? m_tabAtlasClearTarget.get() : m_tabAtlasTarget.get(), Qt::transparent, {1.0f, 0}, updates);
    cb->setGraphicsPipeline(pipeline(program, pass));
    cb->setViewport({0, 0, float(m_size.width()), float(m_size.height())});
    cb->setScissor(scissorFor(tile, m_size));
    cb->setShaderResources(resources);
    cb->draw(3);
    frame.endPass();
}

QRhiBuffer* PostChain::sceneBuffer(int index)
{
    while (int(m_sceneUbos.size()) <= index) {
        std::unique_ptr<QRhiBuffer> b(m_device.rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer,
                                                                sizeof(TerminalSceneUniforms)));
        if (!b->create()) throw std::runtime_error("shader scene buffer allocation failed");
        m_sceneUbos.push_back(std::move(b));
    }
    return m_sceneUbos[std::size_t(index)].get();
}

bool PostChain::fitsInputBudget(std::uint64_t bytes)
{
    if (bytes > BufferBudget) return false;
    auto resident = [this] {
        std::uint64_t total = 0;
        for (const auto& [key, entry] : m_localTextures) total += entry.bytes;
        for (const auto& [key, entry] : m_parameterTextures) total += entry.bytes;
        return total;
    };
    if (resident() <= BufferBudget - bytes) return true;
    // Copies of rectangles that moved since the last frame are never read
    // again. Everything bound this frame is stamped with m_frame and stays.
    std::erase_if(m_localTextures, [this](const auto& item) { return item.second.texture && item.second.used < m_frame; });
    std::erase_if(m_parameterTextures, [this](const auto& item) { return item.second.texture && item.second.used < m_frame; });
    return resident() <= BufferBudget - bytes;
}

QRhiTexture* PostChain::localTexture(QRhiTexture* source, const QRect& rect, QRhiResourceUpdateBatch* updates)
{
    if (rect == QRect(QPoint(), source->pixelSize())) return source;
    if (rect.isEmpty() || !QRect(QPoint(), source->pixelSize()).contains(rect))
        throw std::runtime_error("shader region outside source texture");
    const auto key = std::make_tuple(source, rect.x(), rect.y(), rect.width(), rect.height());
    auto& cache = m_localTextures[key];
    cache.used = m_frame;
    if (!cache.texture) {
        const auto bytes = std::uint64_t(rect.width()) * rect.height() * (source->format() == QRhiTexture::RGBA16F ? 8 : 4);
        if (!fitsInputBudget(bytes)) {
            m_localTextures.erase(key);
            if (!std::exchange(m_regionBudgetWarned, true))
                qCWarning(lcRender, "Shader input texture budget exceeded (256 MiB); using transparent input");
            return parameterTexture({}, updates);
        }
        cache.bytes = bytes;
        cache.texture.reset(m_device.rhi()->newTexture(source->format(), rect.size()));
        if (!cache.texture->create()) throw std::runtime_error("shader region allocation failed");
    }
    QRhiTextureCopyDescription copy;
    copy.setSourceTopLeft(rect.topLeft()); copy.setPixelSize(rect.size());
    updates->copyTexture(cache.texture.get(), source, copy);
    return cache.texture.get();
}

QRhiTexture* PostChain::parameterTexture(const QImage& image, QRhiResourceUpdateBatch* updates)
{
    if (image.isNull()) {
        if (!m_emptyTexture) {
            m_emptyTexture.reset(m_device.rhi()->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
            if (!m_emptyTexture->create()) throw std::runtime_error("shader empty texture allocation failed");
            QImage empty(1, 1, QImage::Format_RGBA8888); empty.fill(Qt::transparent);
            updates->uploadTexture(m_emptyTexture.get(), empty);
        }
        return m_emptyTexture.get();
    }
    const auto key = image.cacheKey();
    auto& cache = m_parameterTextures[key];
    cache.used = m_frame;
    if (!cache.texture) {
        QImage pixels = image;
        const auto bytes = std::uint64_t(pixels.width()) * pixels.height() * 4;
        const int limit = m_device.rhi()->resourceLimit(QRhi::TextureSizeMax);
        if (pixels.width() > limit || pixels.height() > limit || !fitsInputBudget(bytes)) {
            m_parameterTextures.erase(key);
            if (!std::exchange(m_imageBudgetWarned, true))
                qCWarning(lcRender, "Shader image exceeds GPU size or shared 256 MiB input budget; using transparent image");
            return parameterTexture({}, updates);
        }
        cache.bytes = bytes;
        pixels = pixels.convertToFormat(QImage::Format_RGBA8888);
        if (pixels.isNull()) throw std::runtime_error("shader image conversion failed");
        cache.texture.reset(m_device.rhi()->newTexture(QRhiTexture::RGBA8, pixels.size()));
        if (!cache.texture->create()) throw std::runtime_error("shader image allocation failed");
        updates->uploadTexture(cache.texture.get(), pixels);
    }
    return cache.texture.get();
}

QRhiShaderResourceBindings* PostChain::bindings(int ubo, int texture, const ShaderRegionState* region,
    const ShaderProgram* program, const BufferInputs* buffers, QRhiResourceUpdateBatch* updates, QRhiTexture* source)
{
    auto slot = std::unique_ptr<QRhiShaderResourceBindings>(m_device.rhi()->newShaderResourceBindings());
    const auto stage = QRhiShaderResourceBinding::FragmentStage | QRhiShaderResourceBinding::VertexStage;
    auto* input = source ? source : m_textures[texture].get();
    if (region) input = localTexture(input, region->rect, updates);
    // Only a program that includes terminal_history.glsl samples the previous
    // text; the others get the input there and no second copy.
    auto* history = input;
    if (m_previousText && program && program->readsPreviousText())
        history = region ? localTexture(m_previousText.get(), region->rect, updates) : m_previousText.get();
    // Phosphor's binding table (PhosphorShaders::Bindings), with the canvas in
    // the consumer slot and the previous text in the first free one.
    namespace Bindings = PhosphorShaders::Bindings;
    std::vector<QRhiShaderResourceBinding>& entries = m_bindingEntries;
    entries.clear();
    entries.insert(entries.end(), {
        QRhiShaderResourceBinding::uniformBuffer(Bindings::kUniformBlock, stage, uniformBuffer(ubo)),
        QRhiShaderResourceBinding::sampledTexture(Bindings::kConsumer, stage, input, m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(Bindings::kAudioSpectrum, stage,
            m_chains.audioReactive() ? m_device.audioTexture() : input, m_device.atlasSampler()),
        QRhiShaderResourceBinding::sampledTexture(PreviousTextBinding, stage, history, m_sampler.get()),
        QRhiShaderResourceBinding::uniformBuffer(SceneBinding, stage, sceneBuffer(ubo)),
        QRhiShaderResourceBinding::uniformBuffer(PanesBinding, stage, panesBuffer()),
        // The shrink pass (an explicit source) renders into the atlas, so it binds neither.
        QRhiShaderResourceBinding::sampledTexture(TabImageBinding, stage,
            m_tabImage && !source ? m_tabImage.get() : input, m_sampler.get()),
        QRhiShaderResourceBinding::sampledTexture(TabAtlasBinding, stage,
            m_tabAtlas && !source ? m_tabAtlas.get() : input, m_sampler.get()),
    });
    for (int i = 0; i < MaxShaderBuffers; ++i)
        entries.push_back(QRhiShaderResourceBinding::sampledTexture(Bindings::kChannelBase + i, stage,
            buffers && buffers->textures[i] ? buffers->textures[i] : input,
            buffers && buffers->samplers[i] ? buffers->samplers[i] : m_sampler.get()));
    const ShaderParameterValues* overrides = nullptr;
    if (program && region) {
        if (auto it = region->shaderParameters.constFind(program->instanceId()); it != region->shaderParameters.cend())
            overrides = &*it;
    }
    for (int i = 0; i < MaxShaderImages; ++i) {
        QRhiTexture* image = input;
        if (updates) image = parameterTexture(program ? imageFor(*program, overrides, i) : QImage(), updates);
        entries.push_back(QRhiShaderResourceBinding::sampledTexture(Bindings::kUserTextureBase + i, stage, image, m_sampler.get()));
    }
    entries.push_back(QRhiShaderResourceBinding::sampledTexture(Bindings::kDepth, stage,
        buffers && buffers->depth ? buffers->depth : (updates ? parameterTexture({}, updates) : input), m_sampler.get()));
    slot->setBindings(entries.begin(), entries.end());
    if (!slot->create()) throw std::runtime_error("shader sampler bindings failed");
    auto* result = slot.get(); m_bindings.push_back(std::move(slot));
    return result;
}

QRhiRenderPassDescriptor* PostChain::bufferPass(QRhiTextureRenderTarget* target, int format, int samples,
                                                bool depthOutput, bool hardwareDepth)
{
    auto& pass = m_bufferPasses[{format, samples, depthOutput, hardwareDepth}];
    if (!pass) pass.reset(target->newCompatibleRenderPassDescriptor());
    return pass.get();
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
    slot->setShaderStages({{QRhiShaderStage::Vertex, program.vertexShader().isValid() ? program.vertexShader() : m_vertex}, {QRhiShaderStage::Fragment, program.shader()}});
    QRhiVertexInputLayout layout;
    if (program.meshPass()) {
        slot->setSampleCount(meshSamples(program, m_device.rhi()));
        slot->setDepthTest(true);
        slot->setDepthWrite(true);
        slot->setDepthOp(QRhiGraphicsPipeline::Less);
        if (!program.geometry().vertices.empty()) {
            layout.setBindings({{sizeof(ShaderMeshVertex)}});
            layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float3, offsetof(ShaderMeshVertex, position)},
                {0, 1, QRhiVertexInputAttribute::Float3, offsetof(ShaderMeshVertex, normal)},
                {0, 2, QRhiVertexInputAttribute::Float2, offsetof(ShaderMeshVertex, uv)}});
        }
    }
    slot->setVertexInputLayout(layout);
    if (program.depthOutput()) {
        const QRhiGraphicsPipeline::TargetBlend blends[2]{};
        slot->setTargetBlends(std::begin(blends), std::end(blends));
    }
    slot->setShaderResourceBindings(bindings(0, 0));
    slot->setRenderPassDescriptor(pass);
    if (!slot->create()) {
        throw std::runtime_error("failed to create the pipeline for shader " + program.id().toStdString());
    }
    return slot.get();
}

QRhiShaderResourceBindings* PostChain::compositeBindings(int ubo, int background, int text, int cursor, int other)
{
    while (int(m_compositeUbos.size()) <= ubo) {
        std::unique_ptr<QRhiBuffer> b(m_device.rhi()->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(CompositeUniforms)));
        if (!b->create()) throw std::runtime_error("layer composite buffer allocation failed");
        m_compositeUbos.push_back(std::move(b));
    }
    auto& slot = m_compositeBindings[{ubo, background, text, cursor, other}];
    if (!slot) {
        const auto stage = QRhiShaderResourceBinding::FragmentStage;
        slot.reset(m_device.rhi()->newShaderResourceBindings());
        slot->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(0, stage, m_compositeUbos[std::size_t(ubo)].get()),
            QRhiShaderResourceBinding::sampledTexture(1, stage, m_textures[background].get(), m_sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(2, stage, m_textures[text].get(), m_sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(3, stage, m_textures[cursor].get(), m_sampler.get()),
            QRhiShaderResourceBinding::sampledTexture(4, stage, m_textures[other].get(), m_sampler.get()),
        });
        if (!slot->create()) throw std::runtime_error("layer composite bindings failed");
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
    m_composite->setShaderResourceBindings(compositeBindings(0, first, first + 1, first + 2, first + 3));
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

std::vector<PostChain::BufferInputs> PostChain::runBuffers(RenderFrame& frame, const ShaderProgram& program,
    int scope, int step, int input, const std::vector<ShaderRegionState>& regions,
    const ShaderFrameTime& time, int& ubo)
{
    auto* cb = frame.commands();
    std::vector<BufferInputs> result(regions.size());
    if (program.buffers().empty()) return result;
    auto* rhi = m_device.rhi();
    const auto instance = std::make_tuple(&program, scope, step);
    const bool overflow = !m_bufferInstances.contains(instance) && m_bufferInstances.size() >= 32;
    if (!overflow) m_bufferInstances.insert(instance);
    for (std::size_t regionIndex = 0; regionIndex < regions.size(); ++regionIndex) {
        const auto& region = regions[regionIndex];
        if (region.rect.isEmpty()) continue;
        const bool sharedBackground = scope == 0 && program.windowBackground();
        const std::uint64_t identitySnapshot = sharedBackground || !region.snapshot ? 0 : region.snapshot->identity.value();
        auto& passes = m_buffers[{&program, scope, step, identitySnapshot}];
        auto& samples = result[regionIndex];
        const std::vector<HistoryRegion> identity{{identitySnapshot, region.rect,
            region.snapshot ? region.snapshot->defaultBg : 0, region.snapshot ? region.snapshot->defaultFg : 0,
            region.cellSize, region.backgroundOpacity}};
        // Initialise every pass before running any of them. Earlier passes
        // read later passes' previous frames; later passes read earlier current frames.
        for (std::size_t index = 0; index < program.buffers().size(); ++index) {
            const auto& settings = program.bufferSettings(index);
            auto& state = passes[index];
            const QSize wanted(std::max(1, qRound(region.rect.width() * settings.scale)),
                               std::max(1, qRound(region.rect.height() * settings.scale)));
            const auto& passProgram = *program.buffers()[index];
            const int sampleCount = meshSamples(passProgram, rhi);
            const std::uint64_t pixelBytes = (settings.halfFloat ? 8 : 4) * (2 + (sampleCount > 1 ? sampleCount : 0))
                    * (passProgram.depthOutput() ? 2 : 1)
                + (passProgram.meshPass() ? 8 * sampleCount : 0);
            // Imported vertices are shared by every pane that draws the pass,
            // so they count once: only the first state to need them pays.
            const bool meshVertices = passProgram.meshPass() && !passProgram.geometry().vertices.empty();
            const std::uint64_t fixedBytes = meshVertices && !m_meshVertices.contains(&passProgram)
                ? passProgram.geometry().vertices.size() * sizeof(ShaderMeshVertex) : 0;
            const auto bytesFor = [&](QSize size) { return std::uint64_t(size.width()) * size.height() * pixelBytes + fixedBytes; };
            // The size that fits in @p room bytes: both sides shrink by the same
            // factor, and a side below MinimumBufferSide blocks the pass.
            const auto fitted = [&](std::uint64_t room) {
                if (bytesFor(wanted) <= room) return wanted;
                const double fit = room > fixedBytes
                    ? std::sqrt(double(room - fixedBytes) / double(bytesFor(wanted) - fixedBytes)) : 0.0;
                const QSize size(int(wanted.width() * fit), int(wanted.height() * fit));
                return std::min(size.width(), size.height()) < MinimumBufferSide ? QSize() : size;
            };
            // A pass that was shrunk or blocked to fit grows back once the
            // budget has room: to full size, or by at least an eighth per side,
            // so it does not reallocate every frame for a few pixels.
            bool canGrow = false;
            if (state && state->wanted == wanted && state->size != wanted && !overflow) {
                const QSize room = fitted(BufferBudget - m_bufferBytes + state->bytes);
                canGrow = room == wanted || (!room.isEmpty() && (state->blocked
                    || (room.width() >= state->size.width() + state->size.width() / 8
                        && room.height() >= state->size.height() + state->size.height() / 8)));
            }
            const auto logged = state ? state->logged : BufferState::Logged::None;
            if (state && (state->wanted != wanted || canGrow)) {
                m_bufferBytes -= state->bytes;
                state.reset(); // the shared descriptor and the pipelines made for it stay
            }
            if (!state) {
                state = std::make_unique<BufferState>();
                state->wanted = state->size = wanted;
                const QSize size = fitted(overflow ? 0 : BufferBudget - m_bufferBytes);
                state->blocked = size.isEmpty();
                state->size = state->blocked ? QSize(1, 1) : size;
                // Log a change of outcome, not every new rectangle of an animation.
                state->logged = state->blocked ? BufferState::Logged::Blocked
                    : state->size != wanted ? BufferState::Logged::Shrunk : BufferState::Logged::None;
                if (state->logged != logged && overflow) {
                    qCWarning(lcRender, "More than 32 buffer effect occurrences in this window; %s uses a transparent buffer",
                              qPrintable(program.id()));
                } else if (state->logged != logged && state->logged == BufferState::Logged::Blocked) {
                    qCWarning(lcRender, "Shader buffer budget exceeded for %s; using a transparent buffer", qPrintable(program.id()));
                } else if (state->logged != logged && state->logged == BufferState::Logged::Shrunk) {
                    qCWarning(lcRender, "Shader buffer budget exceeded for %s; rendering it at %dx%d instead of %dx%d",
                              qPrintable(program.id()), state->size.width(), state->size.height(),
                              wanted.width(), wanted.height());
                }
                if (!state->blocked) {
                    state->bytes = bytesFor(state->size) - fixedBytes;
                    m_bufferBytes += bytesFor(state->size);
                }
                const auto filter = settings.nearest ? QRhiSampler::Nearest : QRhiSampler::Linear;
                const auto wrap = settings.wrap == ShaderWrap::Repeat ? QRhiSampler::Repeat
                    : settings.wrap == ShaderWrap::Mirror ? QRhiSampler::Mirror : QRhiSampler::ClampToEdge;
                state->sampler.reset(rhi->newSampler(filter, filter, QRhiSampler::None, wrap, wrap));
                if (!state->sampler->create()) throw std::runtime_error("shader buffer sampler failed");
                if (passProgram.meshPass()) {
                    state->hardwareDepth.reset(rhi->newRenderBuffer(QRhiRenderBuffer::DepthStencil, state->size, sampleCount));
                    if (!state->hardwareDepth->create()) throw std::runtime_error("mesh depth allocation failed");
                    if (!state->blocked && meshVertices) {
                        auto& shared = m_meshVertices[&passProgram];
                        if (!shared.buffer) {
                            shared.buffer.reset(rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer,
                                quint32(passProgram.geometry().vertices.size() * sizeof(ShaderMeshVertex))));
                            if (!shared.buffer->create()) throw std::runtime_error("mesh vertex allocation failed");
                        }
                        state->vertices = shared.buffer.get();
                    }
                }
                if (sampleCount > 1) {
                    const auto format = settings.halfFloat ? QRhiTexture::RGBA16F : QRhiTexture::RGBA8;
                    state->multisampleColor.reset(rhi->newTexture(format, state->size, sampleCount, QRhiTexture::RenderTarget));
                    if (!state->multisampleColor->create()) throw std::runtime_error("mesh multisample allocation failed");
                    if (passProgram.depthOutput()) {
                        state->multisampleAuxiliary.reset(rhi->newTexture(format, state->size, sampleCount, QRhiTexture::RenderTarget));
                        if (!state->multisampleAuxiliary->create()) throw std::runtime_error("mesh auxiliary multisample allocation failed");
                    }
                }
                for (int side = 0; side < 2; ++side) {
                    state->textures[side].reset(rhi->newTexture(settings.halfFloat ? QRhiTexture::RGBA16F : QRhiTexture::RGBA8,
                                                              state->size, 1, QRhiTexture::RenderTarget));
                    if (!state->textures[side]->create()) throw std::runtime_error("shader buffer allocation failed");
                    QRhiTextureRenderTargetDescription description(state->textures[side].get());
                    if (state->hardwareDepth) description.setDepthStencilBuffer(state->hardwareDepth.get());
                    if (program.buffers()[index]->depthOutput()) {
                        state->depths[side].reset(rhi->newTexture(settings.halfFloat ? QRhiTexture::RGBA16F : QRhiTexture::RGBA8,
                            state->size, 1, QRhiTexture::RenderTarget));
                        if (!state->depths[side]->create()) throw std::runtime_error("shader depth allocation failed");
                        description.setColorAttachments({state->textures[side].get(), state->depths[side].get()});
                    }
                    if (state->multisampleColor) {
                        QRhiColorAttachment color(state->multisampleColor.get());
                        color.setResolveTexture(state->textures[side].get());
                        if (state->multisampleAuxiliary) {
                            QRhiColorAttachment auxiliary(state->multisampleAuxiliary.get());
                            auxiliary.setResolveTexture(state->depths[side].get());
                            description.setColorAttachments({color, auxiliary});
                        } else description.setColorAttachments({color});
                    }
                    state->targets[side].reset(rhi->newTextureRenderTarget(description));
                    if (!state->pass) {
                        state->pass = bufferPass(state->targets[side].get(), settings.halfFloat ? 1 : 0, sampleCount,
                                                 passProgram.depthOutput(), state->hardwareDepth != nullptr);
                    }
                    state->targets[side]->setRenderPassDescriptor(state->pass);
                    if (!state->targets[side]->create()) throw std::runtime_error("shader buffer target failed");
                }
            }
            auto& buffer = *state;
            buffer.used = m_frame;
            const bool reset = buffer.frame == 0 || buffer.regions != identity || time.time < buffer.time;
            // The pass below clears the side it draws; only the side passes
            // read as "previous" needs clearing here.
            if (reset || !settings.feedback) {
                frame.beginPass(buffer.targets[buffer.read].get(), Qt::transparent, {1.f, 0});
                frame.endPass();
                if (reset) buffer.frame = 0;
            }
            buffer.regions = identity; buffer.time = time.time;
            samples.textures[index] = buffer.textures[buffer.read].get(); samples.samplers[index] = buffer.sampler.get();
            samples.sizes[index] = buffer.size;
        }
        for (std::size_t index = 0; index < program.buffers().size(); ++index) {
            auto& buffer = *passes[index];
            if (buffer.blocked) continue;
            const auto size = buffer.size;
            const double sx = double(size.width()) / region.rect.width(), sy = double(size.height()) / region.rect.height();
            auto scaled = region;
            scaled.rect = QRect(QPoint(), size);
            scaled.cellSize = QSizeF(region.cellSize.width() * sx, region.cellSize.height() * sy);
            auto localCursor = [&](const QRectF& cursor) { return QRectF((cursor.x() - region.rect.x()) * sx,
                (cursor.y() - region.rect.y()) * sy, cursor.width() * sx, cursor.height() * sy); };
            scaled.cursor = localCursor(region.cursor); scaled.previousCursor = localCursor(region.previousCursor);
            scaled.mouse = QPointF((region.mouse.x() - region.rect.x()) * sx, (region.mouse.y() - region.rect.y()) * sy);
            TerminalUniforms uniforms;
            fill(uniforms, scaled, time, program, &samples);
            // The canvas is the buffer. Keep the original window rectangle
            // separately so mesh cameras can span splits.
            uniforms.terminal.uTerminalRect[0] = region.rect.x();
            uniforms.terminal.uTerminalRect[1] = region.rect.y();
            uniforms.terminal.uTerminalRect[2] = region.rect.width();
            uniforms.terminal.uTerminalRect[3] = region.rect.height();
            uniforms.terminal.uTargetSize[0] = size.width(); uniforms.terminal.uTargetSize[1] = size.height();
            const bool inWindow = QRectF(QPointF(), QSizeF(m_size)).contains(region.mouse);
            uniforms.terminal.uWindowMouse[0] = inWindow ? float(region.mouse.x()) : -1.0f;
            uniforms.terminal.uWindowMouse[1] = inWindow ? float(region.mouse.y()) : -1.0f;
            uniforms.base.iFrame = buffer.frame;
            auto* updates = frame.stage(rhi->nextResourceUpdateBatch());
            updates->updateDynamicBuffer(uniformBuffer(ubo), 0, sizeof uniforms, &uniforms);
            const auto& objects = program.buffers()[index]->geometry().objects;
            if (!objects.empty()) {
                ShaderParameterValues values;
                values.params = program.params();
                if (auto it = region.shaderParameters.constFind(program.instanceId()); it != region.shaderParameters.cend())
                    values.merge(*it);
                TerminalSceneUniforms scene{};
                ShaderProgram::sceneMatrices(objects, values.params, time.time, scene);
                updates->updateDynamicBuffer(sceneBuffer(ubo), 0, sizeof scene, &scene);
            }
            if (index == 0) samples.depth = buffer.depths[buffer.read].get();
            auto* resources = bindings(ubo++, input, &region, &program, &samples, updates);
            const int out = 1 - buffer.read;
            if (buffer.vertices) {
                auto& shared = m_meshVertices[program.buffers()[index].get()];
                if (!shared.uploaded) {
                    updates->uploadStaticBuffer(buffer.vertices, program.buffers()[index]->geometry().vertices.data());
                    shared.uploaded = true;
                }
            }
            frame.beginPass(buffer.targets[out].get(), Qt::transparent, {1.f, 0}, updates);
            cb->setGraphicsPipeline(pipeline(*program.buffers()[index], buffer.pass));
            cb->setViewport({0, 0, float(size.width()), float(size.height())});
            cb->setScissor(scissorFor(QRect(QPoint(), size), size));
            cb->setShaderResources(resources);
            if (buffer.vertices) {
                const QRhiCommandBuffer::VertexInput binding(buffer.vertices, 0);
                cb->setVertexInput(0, 1, &binding);
            }
            const auto& geometry = program.buffers()[index]->geometry();
            if (geometry.objects.empty()) {
                cb->draw(program.buffers()[index]->vertexCount(), geometry.instances);
            } else {
                // One draw per object with vertices; the first instance is its index.
                for (std::size_t object = 0; object < geometry.objects.size(); ++object) {
                    const auto& node = geometry.objects[object];
                    if (node.vertexCount > 0)
                        cb->draw(quint32(node.vertexCount), 1, quint32(node.firstVertex), quint32(object));
                }
            }
            frame.endPass();
            buffer.read = out;
            if (buffer.frame < std::numeric_limits<int>::max()) ++buffer.frame;
            samples.textures[index] = buffer.textures[out].get();
            if (buffer.depths[out]) samples.depth = buffer.depths[out].get();
        }
    }
    return result;
}

int PostChain::runLayerChain(RenderFrame& frame, const std::vector<ShaderProgramPtr>& chain, int input,
                             const std::vector<ShaderRegionState>& panes, const ShaderFrameTime& time, int& ubo, int scope)
{
    auto* cb = frame.commands();
    QRhi* rhi = m_device.rhi();
    int current = input;
    int step = 0;
    for (const ShaderProgramPtr& program : chain) {
        // A scene background owns one window-sized buffer set. Split animation
        // changes only the scissors, never its camera, targets or mesh draw count.
        std::vector<ShaderRegionState> sharedRegions;
        const bool shared = scope == 0 && program->windowBackground() && !panes.empty();
        if (shared) {
            auto focused = std::find_if(panes.begin(), panes.end(), [](const auto& pane) { return pane.focused; });
            auto window = focused == panes.end() ? panes.front() : *focused;
            window.rect = QRect(QPoint(), m_size);
            sharedRegions.push_back(std::move(window));
        }
        const auto& regions = shared ? sharedRegions : panes;
        auto buffers = runBuffers(frame, *program, scope, step++, current, regions, time, ubo);
        // The layer textures not holding a layer: one is always free.
        int out = FirstLayerTexture;
        while (std::find(std::begin(m_layerTexture), std::end(m_layerTexture), out) != std::end(m_layerTexture)) {
            ++out;
        }
        QRhiResourceUpdateBatch* updates = frame.stage(rhi->nextResourceUpdateBatch());
        const int firstUbo = ubo;
        for (std::size_t i = 0; i < regions.size(); ++i) {
            TerminalUniforms u;
            fill(u, regions[i], time, *program, &buffers[i]);
            updates->updateDynamicBuffer(uniformBuffer(ubo++), 0, sizeof u, &u);
        }
        std::vector<QRhiShaderResourceBindings*> resources;
        for (std::size_t i = 0; i < regions.size(); ++i)
            resources.push_back(bindings(firstUbo + int(i), current, &regions[i], program.get(), &buffers[i], updates));
        // Outside the panes the layer stays transparent.
        frame.beginPass(m_layerTargets[out - FirstLayerTexture].get(), Qt::transparent, {1.0f, 0}, updates);
        cb->setGraphicsPipeline(pipeline(*program, m_gridPass.get()));
        cb->setViewport({0, 0, float(m_size.width()), float(m_size.height())});
        for (std::size_t i = 0; i < panes.size(); ++i) {
            cb->setScissor(scissorFor(panes[i].rect, m_size));
            cb->setShaderResources(resources[shared ? 0 : i]);
            cb->draw(program->vertexCount());
        }
        frame.endPass();
        for (int& t : m_layerTexture) {
            if (t == current) {
                t = out;
            }
        }
        current = out;
    }
    return current;
}

void PostChain::composite(RenderFrame& frame, const std::vector<ShaderRegionState>& panes,
                          const std::vector<bool>& cursorBelowText, const ShaderFrameTime& time, const QColor& clear, bool otherCursors)
{
    auto* cb = frame.commands();
    if (m_previousText) {
        std::vector<HistoryRegion> regions;
        for (const auto& pane : panes) {
            regions.emplace_back(pane.snapshot ? pane.snapshot->identity.value() : 0, pane.rect,
                                 pane.snapshot ? pane.snapshot->defaultBg : 0,
                                 pane.snapshot ? pane.snapshot->defaultFg : 0, pane.cellSize, pane.backgroundOpacity);
        }
        // Never carry another tab's text, or text in its old position, into
        // newly exposed panes. Clock resets also start a fresh history.
        if (!m_historyValid || regions != m_historyRegions || time.time < m_historyTime) {
            frame.beginPass(m_historyTarget.get(), Qt::transparent, {1.0f, 0});
            frame.endPass();
        }
        m_historyRegions = std::move(regions);
        m_historyTime = time.time;
    }
    int ubo = m_nextUbo;
    const int background = runLayerChain(frame, m_chains.background, m_layerTexture[0], panes, time, ubo, 0);
    const int text = runLayerChain(frame, m_chains.text, m_layerTexture[1], panes, time, ubo, 1);
    const int cursor = runLayerChain(frame, m_chains.cursor, m_layerTexture[2], panes, time, ubo, 2);
    const int other = otherCursors ? runLayerChain(frame, m_chains.cursor, m_layerTexture[3], panes, time, ubo, 5) : m_layerTexture[3];
    m_nextUbo = ubo;

    QRhiResourceUpdateBatch* updates = frame.stage(m_device.rhi()->nextResourceUpdateBatch());
    for (std::size_t i = 0; i < panes.size(); ++i) {
        compositeBindings(int(i), background, text, cursor, other); // makes the buffer
        const CompositeUniforms u{{float(m_size.width()), float(m_size.height())},
                                  i < cursorBelowText.size() && cursorBelowText[i] ? 1 : 0, 0};
        updates->updateDynamicBuffer(m_compositeUbos[i].get(), 0, sizeof u, &u);
    }
    frame.beginPass(m_gridTarget.get(), clear, {1.0f, 0}, updates);
    cb->setGraphicsPipeline(compositePipeline());
    cb->setViewport({0, 0, float(m_size.width()), float(m_size.height())});
    for (std::size_t i = 0; i < panes.size(); ++i) {
        cb->setScissor(scissorFor(panes[i].rect, m_size));
        cb->setShaderResources(compositeBindings(int(i), background, text, cursor, other));
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

void PostChain::fill(TerminalUniforms& u, const ShaderRegionState& s, const ShaderFrameTime& t,
                     const ShaderProgram& program, const BufferInputs* buffers) const
{
    std::memset(&u, 0, sizeof u);
    auto& base = u.base;
    auto& terminal = u.terminal;
    const QRect& r = s.rect;
    for (int i = 0; i < 4; ++i) base.qt_Matrix[i * 5] = 1.0f;
    base.qt_Opacity = 1.0f;
    const double wraps = std::floor(t.time / PhosphorShaders::kShaderTimeWrap);
    const double timeHi = wraps * PhosphorShaders::kShaderTimeWrap;
    base.iTimeHi = float(timeHi);
    base.iTime = float(t.time - timeHi);
    base.iTimeDelta = t.delta;
    base.iFrame = t.frame;
    base.iResolution[0] = float(r.width());
    base.iResolution[1] = float(r.height());
    const QPointF mouse = s.mouse - QPointF(r.topLeft());
    if (mouse.x() >= 0 && mouse.y() >= 0 && mouse.x() < r.width() && mouse.y() < r.height()) {
        base.iMouse[0] = float(mouse.x());
        base.iMouse[1] = float(mouse.y());
        base.iMouse[2] = float(mouse.x() / r.width());
        base.iMouse[3] = float(mouse.y() / r.height());
    } else {
        std::fill(std::begin(base.iMouse), std::end(base.iMouse), -1.0f);
    }
    std::copy(t.date.begin(), t.date.end(), base.iDate);
    std::memcpy(base.customParams, program.params().data(), sizeof base.customParams);
    std::memcpy(base.customColors, program.colors().data(), sizeof base.customColors);
    const ShaderParameterValues* overrides = nullptr;
    if (auto it = s.shaderParameters.constFind(program.instanceId()); it != s.shaderParameters.cend()) overrides = &*it;
    if (overrides) overrides->apply(base);
    if (buffers) {
        for (int i = 0; i < PhosphorShaders::Bindings::kChannelResolutionSlots; ++i) {
            base.iChannelResolution[i][0] = float(buffers->sizes[std::size_t(i)].width());
            base.iChannelResolution[i][1] = float(buffers->sizes[std::size_t(i)].height());
        }
    }
    for (int i = 0; i < MaxShaderImages; ++i) {
        const QImage& image = imageFor(program, overrides, i);
        base.iTextureResolution[i][0] = float(image.width());
        base.iTextureResolution[i][1] = float(image.height());
    }
    if (program.requiresAudio()) {
        std::copy(m_device.audioLevels().begin(), m_device.audioLevels().end(), terminal.uAudioLevels);
        base.iAudioSpectrumSize = m_device.audioSpectrumSize();
    }

    terminal.uCanvas[0] = float(r.x());
    terminal.uCanvas[1] = float(r.y());
    terminal.uCanvas[2] = float(r.width());
    terminal.uCanvas[3] = float(r.height());
    terminal.uTargetSize[0] = float(m_size.width());
    terminal.uTargetSize[1] = float(m_size.height());
    terminal.uCellSize[0] = float(s.cellSize.width());
    terminal.uCellSize[1] = float(s.cellSize.height());
    std::copy(std::begin(terminal.uCanvas), std::end(terminal.uCanvas), terminal.uTerminalRect);
    terminal.uWindowSize[0] = float(m_size.width());
    terminal.uWindowSize[1] = float(m_size.height());
    const bool inWindow = QRectF(QPointF(), QSizeF(m_size)).contains(s.mouse);
    terminal.uWindowMouse[0] = inWindow ? float(s.mouse.x()) : -1.0f;
    terminal.uWindowMouse[1] = inWindow ? float(s.mouse.y()) : -1.0f;
    setRect(terminal.uCursorRect, s.cursor, r);
    setRect(terminal.uPreviousCursorRect, s.previousCursor, r);
    setColor(terminal.uCursorColor, s.cursorColor);
    setColor(terminal.uPreviousCursorColor, s.previousCursorColor);
    terminal.uCursorState[0] = s.cursorStyle;
    terminal.uCursorState[1] = s.previousCursorStyle;
    terminal.uCursorState[2] = s.cursorVisible ? 1 : 0;
    terminal.uCursorState[3] = s.focused ? 1 : 0;
    terminal.uEventAges[0] = age(t.time, s.cursorChangeTime);
    terminal.uEventAges[1] = age(t.time, s.focusChangeTime);
    if (const PaneSnapshot* snap = s.snapshot) {
        for (int i = 0; i < 256; ++i) {
            setRgb(terminal.uPalette[i], snap->palette[std::size_t(i)]);
        }
        setRgb(terminal.uBackgroundColor, snap->defaultBg);
        setRgb(terminal.uForegroundColor, snap->defaultFg);
        setRgb(terminal.uThemeCursorColor, snap->cursorColor);
        setRgb(terminal.uThemeCursorTextColor, snap->cursorTextColor);
        setRgb(terminal.uSelectionForegroundColor, snap->selectionForegroundColor);
        setRgb(terminal.uSelectionBackgroundColor, snap->selectionBackgroundColor);
    }
    terminal.uBackgroundColor[3] = s.backgroundOpacity;
    std::copy(s.scrollback.begin(), s.scrollback.end(), terminal.uScrollback);
    for (std::size_t i = 0; i < s.signals.size(); ++i) {
        terminal.uSignal[i][0] = s.signals[i].value;
        terminal.uSignal[i][1] = age(t.time, s.signals[i].at);
    }
}

void PostChain::record(RenderFrame& frame, const std::vector<ShaderRegionState>& panes,
                       const ShaderRegionState& window, const ShaderFrameTime& time, QRhiRenderTarget* swapTarget,
                       QRhiRenderPassDescriptor* swapPass, const QColor& clear)
{
    auto* cb = frame.commands();
    QRhi* rhi = m_device.rhi();
    const QSize px = m_size;
    int current = 0; // texture holding the latest image
    int ubo = m_nextUbo;

    // Pane chains: each step copies the input, then reshades every pane region.
    int step = 0;
    for (const ShaderProgramPtr& program : m_chains.pane) {
        auto buffers = runBuffers(frame, *program, 3, step++, current, panes, time, ubo);
        const int out = 1 - current;
        QRhiResourceUpdateBatch* updates = frame.stage(rhi->nextResourceUpdateBatch());
        updates->copyTexture(m_textures[out].get(), m_textures[current].get());
        const int firstUbo = ubo;
        for (std::size_t i = 0; i < panes.size(); ++i) {
            TerminalUniforms u;
            fill(u, panes[i], time, *program, &buffers[i]);
            updates->updateDynamicBuffer(uniformBuffer(ubo++), 0, sizeof u, &u);
        }
        std::vector<QRhiShaderResourceBindings*> resources;
        for (std::size_t i = 0; i < panes.size(); ++i)
            resources.push_back(bindings(firstUbo + int(i), current, &panes[i], program.get(), &buffers[i], updates));
        frame.beginPass(m_targets[out].get(), Qt::black, {1.0f, 0}, updates);
        cb->setGraphicsPipeline(pipeline(*program, m_renderPass.get()));
        cb->setViewport({0, 0, float(px.width()), float(px.height())});
        for (std::size_t i = 0; i < panes.size(); ++i) {
            cb->setScissor(scissorFor(panes[i].rect, px));
            cb->setShaderResources(resources[i]);
            cb->draw(program->vertexCount());
        }
        frame.endPass();
        current = out;
    }

    // Window chain; the last step (or a passthrough) writes to the swapchain.
    // Pane effects (frames of panes away from here) run whenever they are
    // active, before the overview or a tab switch takes the window apart,
    // so those start and end on what the window showed.
    std::vector<ShaderProgramPtr> steps = m_chains.window;
    if (m_paneEffectsShowing) steps.insert(steps.end(), m_chains.paneEffects.begin(), m_chains.paneEffects.end());
    if (const auto* event = eventChain(); event && event != &m_chains.paneEffects)
        steps.insert(steps.end(), event->begin(), event->end());
    if (steps.empty()) {
        steps.push_back(ShaderProgram::passthrough());
    }
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const ShaderProgram& program = *steps[i];
        auto buffers = runBuffers(frame, program, 4, int(i), current, {window}, time, ubo);
        const bool last = i + 1 == steps.size();
        const int out = 1 - current;
        QRhiResourceUpdateBatch* updates = frame.stage(rhi->nextResourceUpdateBatch());
        TerminalUniforms u;
        fill(u, window, time, program, &buffers.front());
        updates->updateDynamicBuffer(uniformBuffer(ubo), 0, sizeof u, &u);
        auto* resources = bindings(ubo, current, &window, &program, &buffers.front(), updates);
        // A vertex stage that shrinks the quad shows the window's clear colour
        // around it, translucency included.
        frame.beginPass(last ? swapTarget : m_windowTargets[out].get(), clear, {1.0f, 0}, updates);
        cb->setGraphicsPipeline(pipeline(program, last ? swapPass : m_gridPass.get()));
        cb->setViewport({0, 0, float(px.width()), float(px.height())});
        cb->setScissor(scissorFor(QRect(QPoint(0, 0), px), px));
        cb->setShaderResources(resources);
        cb->draw(program.vertexCount());
        ++ubo;
        if (!last) {
            frame.endPass();
            current = out;
        }
    }
    m_nextUbo = 0; // the frame's passes are all recorded
}

} // namespace AfterglowRender
