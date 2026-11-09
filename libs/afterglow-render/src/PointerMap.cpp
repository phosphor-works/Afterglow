// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "PointerMap.h"

#include <AfterglowRender/GpuDevice.h>
#include <AfterglowRender/RenderFrame.h>

#include <PhosphorShaders/ShaderBindings.h>

#include <QFile>
#include <QImage>
#include <QLoggingCategory>
#include <rhi/qrhi.h>
#include <rhi/qshaderdescription.h>

#include <algorithm>
#include <cmath>
#include <cstring>

Q_LOGGING_CATEGORY(lcPointerMap, "afterglow.render.pointermap")

namespace AfterglowRender {

namespace {
constexpr std::size_t MaxGrids = 32;
/// Grids evaluated in one frame, each with its own slot of uniforms. More
/// wait for the next frame.
constexpr int Slots = 8;
/// The target's clear value: a grid point that a map discards keeps it, and
/// a lookup there leaves the pointer where it is.
constexpr float Discarded = -1024.0f;
/// The marks of readbacks cycle below this: the clear colour holds them as
/// half floats, which are exact for integers up to 2048.
constexpr int Canaries = 2048;
} // namespace

struct PointerMaps::Readback
{
    QRhiReadbackResult result;
    bool done = false;
    struct Band
    {
        std::weak_ptr<Grid> grid;
        int row = 0;
    };
    std::vector<Band> bands;
    /// The row below the bands, which no band draws, keeps the clear colour:
    /// its blue is this readback's mark. Data without it is not this
    /// readback's (a readback that a frame of another window took before
    /// the GPU wrote it): its grids are evaluated again.
    float canary = 0;
    int canaryRow = 0;
};

PointerMaps::PointerMaps(GpuDevice& device)
    : m_device(device)
{
}

PointerMaps::~PointerMaps()
{
    // Qt writes a readback's result when a later frame begins. One still in
    // flight finishes here, or, when that fails (a lost device), is left to
    // Qt: its callback reaches only the result itself.
    const bool waiting = std::any_of(m_readbacks.begin(), m_readbacks.end(), [](const auto& r) { return !r->done; });
    QRhi* rhi = m_device.rhi();
    if (waiting && rhi && !rhi->isRecordingFrame()) rhi->finish();
    for (auto& readback : m_readbacks) {
        if (!readback->done) (void)readback.release();
    }
}

void PointerMaps::clear()
{
    const bool shown = (m_window && !m_window->shown.empty())
        || std::any_of(m_panes.begin(), m_panes.end(), [](const auto& kv) { return !kv.second.shown.empty(); });
    m_grids.clear();
    m_window.reset();
    m_panes.clear();
    m_programs.clear();
    m_failedPrograms.clear();
    // New packs can work where the old ones could not.
    m_failed = false;
    if (shown) ++m_serial;
}

bool PointerMaps::ensureResources()
{
    if (m_failed) return false;
    if (m_uniforms) return true;
    QRhi* rhi = m_device.rhi();
    if (!rhi) return false;
    auto fail = [this](const char* what) {
        qCWarning(lcPointerMap) << "Pointer maps are off:" << what;
        m_failed = true;
        m_uniforms.reset();
        return false;
    };
    // Float targets, for a map that reads back to a fraction of a pixel.
    if (!rhi->isTextureFormatSupported(QRhiTexture::RGBA32F)) return fail("the GPU cannot draw into float textures");
    QFile vertex(QStringLiteral(":/afterglow/shaders/post.vert.qsb"));
    if (!vertex.open(QIODevice::ReadOnly)) return fail("no post.vert.qsb");
    m_vertex = QShader::fromSerialized(vertex.readAll());
    m_uniformStride = rhi->ubufAligned(int(sizeof(TerminalUniforms)));
    m_zerosSize = int(std::max(sizeof(TerminalSceneUniforms), sizeof(TerminalPaneUniforms)));
    m_uniforms.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, quint32(m_uniformStride * Slots)));
    m_zeros.reset(rhi->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::UniformBuffer, quint32(m_zerosSize)));
    m_dummy.reset(rhi->newTexture(QRhiTexture::RGBA8, QSize(1, 1)));
    m_sampler.reset(rhi->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                    QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    if (!m_uniforms->create() || !m_zeros->create() || !m_dummy->create() || !m_sampler->create())
        return fail("its buffers could not be made");
    m_uploaded = false;
    if (!ensureTarget(MinPoints + 1)) return fail("its target could not be made");
    return true;
}

bool PointerMaps::ensureTarget(int rows)
{
    if (m_target && m_target->pixelSize().height() >= rows) return true;
    QRhi* rhi = m_device.rhi();
    // One target for the grids of a frame, one band each, grown to the most
    // rows that a frame has needed.
    const QSize size(MaxPoints, std::max(rows, m_target ? m_target->pixelSize().height() : 0));
    std::unique_ptr<QRhiTexture> texture(rhi->newTexture(QRhiTexture::RGBA32F, size, 1,
                                                         QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
    if (!texture->create()) return false;
    std::unique_ptr<QRhiTextureRenderTarget> target(rhi->newTextureRenderTarget({texture.get()}));
    // One descriptor for every target: they have the same format, so a
    // pipeline made for it draws into any of them.
    if (!m_pass) m_pass.reset(target->newCompatibleRenderPassDescriptor());
    target->setRenderPassDescriptor(m_pass.get());
    if (!target->create()) return false;
    m_renderTarget = std::move(target);
    m_target = std::move(texture);
    return true;
}

PointerMaps::Program* PointerMaps::program(const ShaderProgramPtr& shader)
{
    // A program given up after its pipeline was made has it still.
    if (m_failedPrograms.contains(shader.get())) return nullptr;
    if (auto it = m_programs.find(shader.get()); it != m_programs.end()) return &it->second.second;
    auto fail = [&](const char* why) -> Program* {
        failProgram(shader.get(), why);
        return nullptr;
    };
    QRhi* rhi = m_device.rhi();
    // The resources that the map's stage declares: its parameters in the
    // uniform block, the contract's other blocks as zeros, and one
    // transparent pixel for each sampler. A map reads only the canvas size
    // and the parameters.
    const QShaderDescription description = shader->pointerShader().description();
    if (!description.storageBlocks().isEmpty() || !description.separateImages().isEmpty()
        || !description.separateSamplers().isEmpty())
        return fail("its stage uses resources that a map cannot have");
    const auto stage = QRhiShaderResourceBinding::FragmentStage | QRhiShaderResourceBinding::VertexStage;
    Program result;
    for (int slot = 0; slot < Slots; ++slot) {
        std::vector<QRhiShaderResourceBinding> entries;
        for (const auto& block : description.uniformBlocks()) {
            if (block.binding == PhosphorShaders::Bindings::kUniformBlock) {
                if (block.size > int(sizeof(TerminalUniforms))) return fail("its uniform block is larger than the contract's");
                entries.push_back(QRhiShaderResourceBinding::uniformBuffer(block.binding, stage, m_uniforms.get(),
                                                                           quint32(slot * m_uniformStride), quint32(block.size)));
            } else {
                if (block.size > m_zerosSize) return fail("it declares a uniform block of its own");
                entries.push_back(QRhiShaderResourceBinding::uniformBuffer(block.binding, stage, m_zeros.get(), 0,
                                                                           quint32(block.size)));
            }
        }
        for (const auto& sampler : description.combinedImageSamplers())
            entries.push_back(QRhiShaderResourceBinding::sampledTexture(sampler.binding, stage, m_dummy.get(), m_sampler.get()));
        std::unique_ptr<QRhiShaderResourceBindings> bindings(rhi->newShaderResourceBindings());
        bindings->setBindings(entries.begin(), entries.end());
        if (!bindings->create()) return fail("its bindings could not be made");
        result.bindings.push_back(std::move(bindings));
    }
    result.pipeline.reset(rhi->newGraphicsPipeline());
    result.pipeline->setTopology(QRhiGraphicsPipeline::Triangles);
    result.pipeline->setFlags(QRhiGraphicsPipeline::UsesScissor);
    result.pipeline->setShaderStages({{QRhiShaderStage::Vertex, m_vertex}, {QRhiShaderStage::Fragment, shader->pointerShader()}});
    result.pipeline->setVertexInputLayout({});
    result.pipeline->setShaderResourceBindings(result.bindings.front().get());
    result.pipeline->setRenderPassDescriptor(m_pass.get());
    if (!result.pipeline->create()) return fail("its pipeline could not be made");
    auto& entry = m_programs[shader.get()];
    entry = {shader, std::move(result)};
    return &entry.second;
}

PointerMaps::GridPtr PointerMaps::grid(const ShaderProgramPtr& program, const QSize& canvas,
                                       const ShaderParameterValues* overrides)
{
    // The key: the parameters with their live values, as fill() gives them.
    ShaderParameterValues values;
    values.params = program->params();
    values.colors = program->colors();
    if (overrides) values.merge(*overrides);
    const std::array<float, 32>& params = values.params;
    const std::array<float, 64>& colors = values.colors;
    ++m_clock;
    for (const GridPtr& g : m_grids) {
        if (g->program == program && g->canvas == canvas && g->params == params && g->colors == colors) {
            g->used = m_clock;
            return g;
        }
    }
    auto g = std::make_shared<Grid>();
    g->program = program;
    g->canvas = canvas;
    g->params = params;
    g->colors = colors;
    g->columns = std::clamp((canvas.width() + Spacing - 1) / Spacing, MinPoints, MaxPoints);
    g->rows = std::clamp((canvas.height() + Spacing - 1) / Spacing, MinPoints, MaxPoints);
    g->queued = true;
    g->used = m_clock;
    if (m_grids.size() >= MaxGrids) {
        // The least recently asked for of those that no region holds. Those
        // that regions hold stay, however many: dropping one would only
        // evaluate it again in the next frame, and in every frame after.
        auto oldest = m_grids.end();
        for (auto it = m_grids.begin(); it != m_grids.end(); ++it) {
            if (it->use_count() == 1 && (oldest == m_grids.end() || (*it)->used < (*oldest)->used)) oldest = it;
        }
        if (oldest != m_grids.end()) m_grids.erase(oldest);
    }
    m_grids.push_back(g);
    return g;
}

void PointerMaps::settle(Region& region)
{
    // A region shows its new maps once all of them have arrived, and its
    // last ones until then.
    if (!std::all_of(region.wanted.begin(), region.wanted.end(), [](const GridPtr& g) { return g->ready; })) return;
    if (region.shown == region.wanted && region.rect == region.wantedRect) return;
    const bool moved = !region.shown.empty() || !region.wanted.empty();
    region.shown = region.wanted;
    region.rect = region.wantedRect;
    if (moved) ++m_serial;
}

void PointerMaps::show(std::optional<std::uint32_t> pane, const QRect& rect, const std::vector<ShaderProgramPtr>& chain,
                       const ShaderParameterMap& parameters)
{
    Region& region = pane ? m_panes[*pane] : m_window ? *m_window : m_window.emplace();
    region.seenFrame = m_frame;
    std::vector<GridPtr> wanted;
    if (!rect.isEmpty()) {
        for (const ShaderProgramPtr& program : chain) {
            if (!program || !program->hasPointerMap() || m_failedPrograms.contains(program.get())) continue;
            const auto values = parameters.constFind(program->instanceId());
            wanted.push_back(grid(program, rect.size(), values == parameters.cend() ? nullptr : &*values));
        }
    }
    region.wanted = std::move(wanted);
    region.wantedRect = rect;
    settle(region);
}

void PointerMaps::endRegions()
{
    bool shown = false;
    std::erase_if(m_panes, [&](const auto& kv) {
        if (kv.second.seenFrame == m_frame) return false;
        shown |= !kv.second.shown.empty();
        return true;
    });
    if (shown) ++m_serial;
    ++m_frame;
}

void PointerMaps::failProgram(const ShaderProgram* program, const char* why)
{
    if (!m_failedPrograms.insert(program).second) return;
    qCWarning(lcPointerMap) << "The pointer map of" << program->id() << "is off:" << why;
    for (const GridPtr& g : m_grids) {
        if (g->program.get() == program) g->queued = false;
    }
    auto drop = [&](Region& region) {
        std::erase_if(region.wanted, [program](const GridPtr& g) { return g->program.get() == program; });
        if (std::erase_if(region.shown, [program](const GridPtr& g) { return g->program.get() == program; })) ++m_serial;
        settle(region);
    };
    if (m_window) drop(*m_window);
    for (auto& [id, region] : m_panes) drop(region);
}

void PointerMaps::collect()
{
    bool arrived = false;
    for (auto& readback : m_readbacks) {
        if (!readback->done) continue;
        const QByteArray& data = readback->result.data;
        const auto* values = reinterpret_cast<const float*>(data.constData());
        const std::size_t available = std::size_t(data.size()) / (sizeof(float) * 4);
        const std::size_t stride = std::size_t(readback->result.pixelSize.width());
        const std::size_t canary = std::size_t(readback->canaryRow) * stride;
        auto marked = [&](std::size_t at) { return values[at * 4] == Discarded && values[at * 4 + 2] == readback->canary; };
        const bool ours = stride > 0 && canary + stride <= available && marked(canary) && marked(canary + stride - 1);
        for (const auto& band : readback->bands) {
            const GridPtr g = band.grid.lock();
            // A grid of a program given up meanwhile, maybe by another band here.
            if (!g || m_failedPrograms.contains(g->program.get())) continue;
            const std::size_t first = std::size_t(band.row) * stride;
            const std::size_t columns = std::size_t(g->columns);
            const std::size_t points = std::size_t(g->rows) * columns;
            if (!ours || columns > stride || first + std::size_t(g->rows) * stride > available) {
                // Not this readback's data: evaluate the grid again, a few
                // times, before the map is given up.
                if (++g->attempts < 3) g->queued = true;
                else failProgram(g->program.get(), "its readbacks did not arrive");
                continue;
            }
            g->values.resize(points * 2);
            for (std::size_t y = 0; y < std::size_t(g->rows); ++y) {
                for (std::size_t x = 0; x < columns; ++x) {
                    const std::size_t from = (first + y * stride + x) * 4, to = (y * columns + x) * 2;
                    g->values[to] = values[from];
                    g->values[to + 1] = values[from + 1];
                }
            }
            g->ready = true;
            arrived = true;
        }
    }
    std::erase_if(m_readbacks, [](const auto& r) { return r->done; });
    if (!arrived) return;
    if (m_window) settle(*m_window);
    for (auto& [id, region] : m_panes) settle(region);
}

void PointerMaps::record(RenderFrame& frame)
{
    std::vector<GridPtr> queue;
    int rows = 0;
    for (const GridPtr& g : m_grids) {
        if (!g->queued) continue;
        if (int(queue.size()) == Slots || rows + g->rows > 2 * MaxPoints) break;
        queue.push_back(g);
        rows += g->rows;
    }
    if (queue.empty()) return;
    auto drop = [&queue] {
        for (const GridPtr& g : queue) g->queued = false;
    };
    if (!ensureResources()) {
        // Without a device yet the grids wait; without resources they never come.
        if (m_failed) drop();
        return;
    }
    std::vector<std::pair<GridPtr, Program*>> draws;
    for (const GridPtr& g : queue) {
        if (Program* p = program(g->program)) draws.emplace_back(g, p);
        else g->queued = false;
    }
    if (draws.empty()) return;
    // The row below the bands stays the clear colour (Readback::canary).
    if (!ensureTarget(rows + 1)) {
        qCWarning(lcPointerMap) << "Pointer maps are off: their target could not be made";
        m_failed = true;
        drop();
        return;
    }
    QRhi* rhi = m_device.rhi();
    QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
    QRhiResourceUpdateBatch* read = updates ? rhi->nextResourceUpdateBatch() : nullptr;
    if (!read) {
        // The batch pool is spent this frame: the grids wait for the next.
        if (updates) updates->release();
        return;
    }
    if (!std::exchange(m_uploaded, true)) {
        QImage pixel(1, 1, QImage::Format_RGBA8888_Premultiplied);
        pixel.fill(Qt::transparent);
        updates->uploadTexture(m_dummy.get(), pixel);
        updates->uploadStaticBuffer(m_zeros.get(), QByteArray(m_zerosSize, '\0').constData());
    }
    const QSize targetSize = m_target->pixelSize();
    auto readback = std::make_unique<Readback>();
    m_canary = m_canary % (Canaries - 1) + 1;
    readback->canary = float(m_canary);
    readback->canaryRow = rows;
    int row = 0;
    for (std::size_t i = 0; i < draws.size(); ++i) {
        const Grid& g = *draws[i].first;
        // The canvas size and the parameters; the grid's band is the canvas
        // that pTerminal's coordinates are drawn on. The rest stays zero: a
        // map reads only these.
        TerminalUniforms u;
        std::memset(&u, 0, sizeof u);
        for (int m = 0; m < 4; ++m) u.base.qt_Matrix[m * 5] = 1.0f;
        u.base.qt_Opacity = 1.0f;
        u.base.iResolution[0] = float(g.canvas.width());
        u.base.iResolution[1] = float(g.canvas.height());
        static_assert(sizeof g.params == sizeof u.base.customParams && sizeof g.colors == sizeof u.base.customColors);
        std::memcpy(u.base.customParams, g.params.data(), sizeof u.base.customParams);
        std::memcpy(u.base.customColors, g.colors.data(), sizeof u.base.customColors);
        u.terminal.uCanvas[1] = float(row);
        u.terminal.uCanvas[2] = float(g.columns);
        u.terminal.uCanvas[3] = float(g.rows);
        u.terminal.uTargetSize[0] = float(targetSize.width());
        u.terminal.uTargetSize[1] = float(targetSize.height());
        updates->updateDynamicBuffer(m_uniforms.get(), quint32(int(i) * m_uniformStride), sizeof u, &u);
        readback->bands.push_back({draws[i].first, row});
        row += g.rows;
    }
    QRhiCommandBuffer* cb = frame.commands();
    frame.beginPass(m_renderTarget.get(), QColor::fromRgbF(Discarded, Discarded, readback->canary, 1.0f), {1.0f, 0}, updates);
    cb->setViewport({0, 0, float(targetSize.width()), float(targetSize.height())});
    row = 0;
    for (std::size_t i = 0; i < draws.size(); ++i) {
        const int rowsOf = draws[i].first->rows;
        cb->setGraphicsPipeline(draws[i].second->pipeline.get());
        cb->setScissor(scissorFor(QRect(0, row, draws[i].first->columns, rowsOf), targetSize));
        cb->setShaderResources(draws[i].second->bindings[i].get());
        cb->draw(3);
        row += rowsOf;
    }
    Readback* raw = readback.get();
    raw->result.completed = [raw] { raw->done = true; };
    // Only the bands and the mark: the target keeps the most rows that a
    // frame has needed.
    QRhiReadbackDescription description(m_target.get());
    description.setRect(QRect(0, 0, targetSize.width(), rows + 1));
    read->readBackTexture(description, &raw->result);
    frame.endPass(read);
    for (const auto& [g, p] : draws) g->queued = false;
    m_readbacks.push_back(std::move(readback));
}

bool PointerMaps::pending() const
{
    // A readback that completed needs no frame: the next lookup or frame
    // takes it (collect()).
    return std::any_of(m_readbacks.begin(), m_readbacks.end(), [](const auto& r) { return !r->done; })
        || (!m_failed && std::any_of(m_grids.begin(), m_grids.end(), [](const GridPtr& g) { return g->queued; }));
}

QPointF PointerMaps::Grid::unwarp(const QPointF& point) const
{
    // Grid points sit at the centres of their cells: bilinear between the
    // four around the point. Past the outer ones, and outside the canvas,
    // the edge cells carry on in a line, so a point outside stays outside.
    const double gx = point.x() / canvas.width() * columns - 0.5;
    const double gy = point.y() / canvas.height() * rows - 0.5;
    const int x0 = std::clamp(int(std::floor(gx)), 0, columns - 2);
    const int y0 = std::clamp(int(std::floor(gy)), 0, rows - 2);
    const double fx = gx - x0, fy = gy - y0;
    auto at = [this](int x, int y, int c) {
        return double(values[(std::size_t(y) * std::size_t(columns) + std::size_t(x)) * 2 + std::size_t(c)]);
    };
    if (at(x0, y0, 0) <= Discarded / 2 || at(x0 + 1, y0, 0) <= Discarded / 2 || at(x0, y0 + 1, 0) <= Discarded / 2
        || at(x0 + 1, y0 + 1, 0) <= Discarded / 2)
        return point;
    auto mix = [&](int c) {
        const double top = at(x0, y0, c) * (1 - fx) + at(x0 + 1, y0, c) * fx;
        const double bottom = at(x0, y0 + 1, c) * (1 - fx) + at(x0 + 1, y0 + 1, c) * fx;
        return top * (1 - fy) + bottom * fy;
    };
    const double u = mix(0), v = mix(1);
    if (!std::isfinite(u) || !std::isfinite(v)) return point;
    return QPointF(u * canvas.width(), v * canvas.height());
}

QPointF PointerMaps::unwarp(std::optional<std::uint32_t> pane, const QPointF& point) const
{
    const Region* region = nullptr;
    if (!pane) {
        region = m_window ? &*m_window : nullptr;
    } else if (const auto it = m_panes.find(*pane); it != m_panes.end()) {
        region = &it->second;
    }
    if (!region || region->shown.empty()) return point;
    const QPointF origin(region->rect.topLeft());
    QPointF p = point - origin;
    for (auto it = region->shown.rbegin(); it != region->shown.rend(); ++it) p = (*it)->unwarp(p);
    return p + origin;
}

} // namespace AfterglowRender
