// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowRender/GridRenderer.h>

#include <AfterglowRender/FontSet.h>
#include <AfterglowRender/GlyphCache.h>
#include <AfterglowRender/GpuDevice.h>
#include <AfterglowRender/PostChain.h>
#include <AfterglowRender/Snapshot.h>

#include "Sprites.h"

#include <AfterglowVt/Unicode.h>

#include <hb.h>

#include <QColor>
#include <QFile>
#include <QWindow>
#include <rhi/qrhi.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace AfterglowRender {

using AfterglowVt::Attr::Bold;
using AfterglowVt::Attr::Italic;
using AfterglowVt::Attr::Overline;
using AfterglowVt::Attr::Strikethrough;
using AfterglowVt::UnderlineStyle;

namespace {

StyleSlot slotFor(std::uint16_t attrs)
{
    const bool b = attrs & Bold;
    const bool i = attrs & Italic;
    return b ? (i ? StyleSlot::BoldItalic : StyleSlot::Bold) : (i ? StyleSlot::Italic : StyleSlot::Regular);
}

std::uint8_t alphaOf(std::uint32_t rgba)
{
    return std::uint8_t(rgba >> 24);
}

std::uint32_t withAlpha(std::uint32_t rgba, std::uint8_t a)
{
    return (rgba & 0x00FFFFFFu) | (std::uint32_t(a) << 24);
}

QShader loadShader(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        throw std::runtime_error("missing shader " + path.toStdString());
    }
    return QShader::fromSerialized(f.readAll());
}

} // namespace

// --- FrameBuilder ----------------------------------------------------------

FrameBuilder::FrameBuilder()
    : m_hbBuffer(hb_buffer_create())
{
}

FrameBuilder::~FrameBuilder()
{
    hb_buffer_destroy(m_hbBuffer);
}

std::size_t FrameBuilder::ShapeKeyHash::operator()(const ShapeKey& k) const noexcept
{
    return std::hash<std::u32string>{}(k.text) ^ (std::size_t(k.face) * 0x9E3779B97F4A7C15ull) ^ std::size_t(k.slot);
}

const std::vector<FrameBuilder::ShapedGlyph>& FrameBuilder::shape(FontSet& fonts, int face, int slot,
                                                                  const std::u32string& text)
{
    ShapeKey key{face, slot, text};
    if (auto it = m_shapeCache.find(key); it != m_shapeCache.end()) {
        return it->second;
    }
    if (m_shapeCache.size() > 16384) {
        m_shapeCache.clear();
    }

    hb_buffer_clear_contents(m_hbBuffer);
    hb_buffer_add_utf32(m_hbBuffer, reinterpret_cast<const std::uint32_t*>(text.data()), int(text.size()), 0,
                        int(text.size()));
    hb_buffer_guess_segment_properties(m_hbBuffer);
    // Terminals lay text out left to right in cells; no bidi reordering here.
    hb_buffer_set_direction(m_hbBuffer, HB_DIRECTION_LTR);
    hb_buffer_set_cluster_level(m_hbBuffer, HB_BUFFER_CLUSTER_LEVEL_MONOTONE_GRAPHEMES);
    const auto& features = fonts.features();
    hb_shape(fonts.face(face).hb, m_hbBuffer, features.data(), unsigned(features.size()));

    unsigned count = 0;
    const hb_glyph_info_t* info = hb_buffer_get_glyph_infos(m_hbBuffer, &count);
    const hb_glyph_position_t* pos = hb_buffer_get_glyph_positions(m_hbBuffer, &count);

    std::vector<ShapedGlyph> glyphs;
    glyphs.reserve(count);
    float pen = 0;
    float clusterPen = 0;
    std::uint32_t cluster = ~0u;
    for (unsigned i = 0; i < count; ++i) {
        if (info[i].cluster != cluster) {
            cluster = info[i].cluster;
            clusterPen = pen;
        }
        glyphs.push_back({info[i].codepoint, info[i].cluster, pen - clusterPen + float(pos[i].x_offset) / 64.0f,
                          float(pos[i].y_offset) / 64.0f});
        pen += float(pos[i].x_advance) / 64.0f;
    }
    return m_shapeCache.emplace(std::move(key), std::move(glyphs)).first->second;
}

bool FrameBuilder::buildPane(const PaneSnapshot& s, const PaneParams& p, FontSet& fonts, GlyphCache& glyphs,
                             std::vector<Instance>& out, std::size_t& textStart)
{
    const FontMetrics& m = fonts.metrics();
    const int cw = m.cellWidth;
    const int ch = m.cellHeight;
    const float ox = float(p.contentOrigin.x());
    const float oy = float(p.contentOrigin.y()) - p.scrollShift;
    const int drawRows = s.rows + s.extraRows;
    const float dpr = float(fonts.devicePixelRatio());

    auto solid = [&out](float x, float y, float w, float h, std::uint32_t color) {
        out.push_back({x, y, w, h, 0, 0, 0, 0, color, std::uint32_t(InstanceKind::Solid)});
    };

    solid(float(p.rect.x()), float(p.rect.y()), float(p.rect.width()), float(p.rect.height()),
          (s.defaultBg & 0x00FFFFFFu) | (std::uint32_t(p.backgroundAlpha) << 24));

    const bool cursorInGrid = s.cursorX >= 0 && s.cursorX < s.cols && s.cursorY >= 0 && s.cursorY < s.rows;
    const bool drawCursor = s.cursorVisible && cursorInGrid && (!p.focused || !s.cursorBlink || p.cursorBlinkOn);
    const int cursorSpan = drawCursor && s.at(s.cursorX, s.cursorY).width == 2 ? 2 : 1;
    const bool blockCursor = drawCursor && p.focused && s.cursorShape == AfterglowVt::CursorShape::Block;
    // Where the cursor is drawn: its cell, or a point on its glide there.
    const QPointF cellPos(s.cursorX * cw, s.cursorY * ch);
    const QPointF cursorPos = p.cursorPosition.value_or(cellPos);
    const float cursorX = ox + float(cursorPos.x());
    const float cursorY = oy + float(cursorPos.y());
    // Text under a block cursor takes the background colour once it has arrived.
    const bool cursorSettled = (cursorPos - cellPos).manhattanLength() < 0.5;
    const bool recolorUnderCursor = blockCursor && cursorSettled;

    // Backgrounds, merged into horizontal runs.
    for (int y = 0; y < drawRows; ++y) {
        const float rowTop = oy + float(y * ch);
        int x = 0;
        while (x < s.cols) {
            const std::uint32_t bg = s.at(x, y).bg;
            if (alphaOf(bg) == 0) {
                ++x;
                continue;
            }
            int end = x + 1;
            while (end < s.cols && s.at(end, y).bg == bg) {
                ++end;
            }
            solid(ox + float(x * cw), rowTop, float((end - x) * cw), float(ch), bg);
            x = end;
        }
    }
    if (blockCursor) {
        solid(cursorX, cursorY, float(cursorSpan * cw), float(ch), s.cursorColor);
    }
    textStart = out.size();

    // Text, after every background so overhangs are not covered.
    for (int y = 0; y < drawRows; ++y) {
        const float baseline = oy + float(y * ch + m.baseline);
        int x = 0;
        while (x < s.cols) {
            const SnapshotCell& c = s.at(x, y);
            if (c.width == 0 || c.cp == 0) {
                ++x;
                continue;
            }
            if (c.grapheme == 0 && Sprites::isSprite(c.cp)) {
                if (alphaOf(c.fg) != 0) {
                    const GlyphEntry* e = glyphs.sprite(fonts, c.cp);
                    if (!e) {
                        return false;
                    }
                    if (!e->empty) {
                        std::uint32_t color = c.fg;
                        if (recolorUnderCursor && y == s.cursorY && x >= s.cursorX && x < s.cursorX + cursorSpan) {
                            color = s.defaultBg;
                        }
                        const float sx = ox + float(x * cw);
                        out.push_back({sx, baseline - float(e->top), float(e->width), float(e->height), float(e->u),
                                       float(e->v), float(e->u + e->width), float(e->v + e->height), color,
                                       std::uint32_t(InstanceKind::GrayGlyph)});
                    }
                }
                ++x;
                continue;
            }

            auto faceOf = [&](const SnapshotCell& cell) {
                bool color = cell.width == 2 && AfterglowVt::Unicode::props(cell.cp).extendedPictographic;
                if (!color && cell.grapheme) {
                    const std::u32string& extra = s.graphemes[cell.grapheme - 1];
                    color = extra.find(AfterglowVt::Unicode::VariationSelector16) != std::u32string::npos;
                }
                return fonts.faceFor(cell.cp, slotFor(cell.attrs), color);
            };

            const StyleSlot slot = slotFor(c.attrs);
            const int face = faceOf(c);

            m_runText.clear();
            m_runColumns.clear();
            int end = x;
            while (end < s.cols) {
                const SnapshotCell& n = s.at(end, y);
                if (n.width == 0) {
                    ++end;
                    continue;
                }
                if (n.cp == 0 || slotFor(n.attrs) != slot || (n.grapheme == 0 && Sprites::isSprite(n.cp))
                    || (end != x && faceOf(n) != face)) {
                    break;
                }
                m_runText.push_back(n.cp);
                m_runColumns.push_back(end);
                if (n.grapheme) {
                    for (char32_t e : s.graphemes[n.grapheme - 1]) {
                        m_runText.push_back(e);
                        m_runColumns.push_back(end);
                    }
                }
                ++end;
            }

            const auto& shaped = shape(fonts, face, int(slot), m_runText);
            for (const ShapedGlyph& g : shaped) {
                const int col = m_runColumns[std::min<std::size_t>(g.cluster, m_runColumns.size() - 1)];
                const SnapshotCell& cell = s.at(col, y);
                std::uint32_t color = cell.fg;
                if (alphaOf(color) == 0) {
                    continue;
                }
                if (recolorUnderCursor && y == s.cursorY && col >= s.cursorX && col < s.cursorX + cursorSpan) {
                    color = s.defaultBg;
                }
                const GlyphEntry* e = glyphs.glyph(fonts, face, g.glyphId, std::max<int>(1, cell.width));
                if (!e) {
                    return false;
                }
                if (e->empty) {
                    continue;
                }
                const float cellX = ox + float(col * cw);
                if (e->color) {
                    out.push_back({cellX + float(e->left), baseline - float(e->top), float(e->width), float(e->height),
                                   float(e->u), float(e->v), float(e->u + e->width), float(e->v + e->height),
                                   withAlpha(0xFFFFFFu, alphaOf(color)), std::uint32_t(InstanceKind::ColorGlyph)});
                } else {
                    const float gx = std::round(cellX + g.xOffset + float(e->left));
                    const float gy = std::round(baseline - float(e->top) - g.yOffset);
                    out.push_back({gx, gy, float(e->width), float(e->height), float(e->u), float(e->v),
                                   float(e->u + e->width), float(e->v + e->height), color,
                                   std::uint32_t(InstanceKind::GrayGlyph)});
                }
            }
            x = end;
        }
    }

    // Decorations.
    const float th = float(m.underlineThickness);
    for (int y = 0; y < drawRows; ++y) {
        const float rowTop = oy + float(y * ch);
        int x = 0;
        while (x < s.cols) {
            const SnapshotCell& c = s.at(x, y);
            const UnderlineStyle ul = c.underline;
            if (ul == UnderlineStyle::None) {
                ++x;
                continue;
            }
            int end = x + 1;
            while (end < s.cols && s.at(end, y).underline == ul && s.at(end, y).underlineColor == c.underlineColor) {
                ++end;
            }
            const float rx = ox + float(x * cw);
            const float rw = float((end - x) * cw);
            const float uy = rowTop + float(m.underlinePosition);
            const std::uint32_t color = c.underlineColor;
            switch (ul) {
            case UnderlineStyle::Single:
                solid(rx, std::round(uy - th / 2), rw, th, color);
                break;
            case UnderlineStyle::Double: {
                const float top = std::min(std::round(uy - th), rowTop + float(ch) - 3 * th);
                solid(rx, top, rw, th, color);
                solid(rx, top + 2 * th, rw, th, color);
                break;
            }
            case UnderlineStyle::Curly: {
                const float h = std::max(3 * th, std::round(float(ch) * 0.15f));
                const float top = std::min(std::round(uy - h / 2), rowTop + float(ch) - h);
                out.push_back({rx, top, rw, h, th, float(cw), 0, 0, color, std::uint32_t(InstanceKind::Undercurl)});
                break;
            }
            case UnderlineStyle::Dotted:
                out.push_back({rx, std::round(uy - th / 2), rw, th, std::max(1.0f, th), 0, 0, 0, color,
                               std::uint32_t(InstanceKind::Dotted)});
                break;
            case UnderlineStyle::Dashed:
                out.push_back({rx, std::round(uy - th / 2), rw, th, std::max(2.0f, float(cw) / 2), 0, 0, 0, color,
                               std::uint32_t(InstanceKind::Dashed)});
                break;
            case UnderlineStyle::None:
                break;
            }
            x = end;
        }
        for (int cx = 0; cx < s.cols; ++cx) {
            const SnapshotCell& c = s.at(cx, y);
            if ((c.attrs & Strikethrough) && alphaOf(c.fg) != 0) {
                solid(ox + float(cx * cw), std::round(rowTop + float(m.strikethroughPosition) - th / 2), float(cw), th,
                      c.fg);
            }
            if ((c.attrs & Overline) && alphaOf(c.fg) != 0) {
                solid(ox + float(cx * cw), rowTop, float(cw), th, c.fg);
            }
        }
    }

    // Non-block cursors draw over the text.
    if (drawCursor && !blockCursor) {
        const float x0 = cursorX;
        const float y0 = cursorY;
        const float w = float(cursorSpan * cw);
        const float t = std::max(1.0f, std::round(dpr));
        if (!p.focused) {
            solid(x0, y0, w, t, s.cursorColor);
            solid(x0, y0 + float(ch) - t, w, t, s.cursorColor);
            solid(x0, y0, t, float(ch), s.cursorColor);
            solid(x0 + w - t, y0, t, float(ch), s.cursorColor);
        } else if (s.cursorShape == AfterglowVt::CursorShape::Bar) {
            solid(x0, y0, std::max(t, std::round(float(cw) * 0.12f)), float(ch), s.cursorColor);
        } else {
            const float h = std::max(t, std::round(float(ch) * 0.08f));
            solid(x0, y0 + float(ch) - h, w, h, s.cursorColor);
        }
    }
    return true;
}

// --- WindowRenderer --------------------------------------------------------

namespace {

QRhiGraphicsPipeline::TargetBlend premultipliedBlend()
{
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::One;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    return blend;
}

QRhiVertexInputLayout instanceLayout()
{
    QRhiVertexInputLayout layout;
    layout.setBindings({{sizeof(Instance), QRhiVertexInputBinding::PerInstance}});
    layout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float4, offsetof(Instance, x)},
        {0, 1, QRhiVertexInputAttribute::Float4, offsetof(Instance, u0)},
        {0, 2, QRhiVertexInputAttribute::UNormByte4, offsetof(Instance, color)},
        {0, 3, QRhiVertexInputAttribute::UInt, offsetof(Instance, kind)},
    });
    return layout;
}

QColor toColor(std::uint32_t rgba)
{
    return QColor::fromRgb(int(rgba & 0xFF), int((rgba >> 8) & 0xFF), int((rgba >> 16) & 0xFF), int(rgba >> 24));
}

/// The clear colour premultiplied, as everything is drawn premultiplied.
QColor premultipliedClear(std::uint32_t rgba)
{
    const int a = int(rgba >> 24);
    return QColor::fromRgb(int(rgba & 0xFF) * a / 255, int((rgba >> 8) & 0xFF) * a / 255,
                           int((rgba >> 16) & 0xFF) * a / 255, a);
}

} // namespace

WindowRenderer::WindowRenderer(GpuDevice& device, QWindow* window)
    : m_device(device)
    , m_window(window)
{
    m_clock.start();
}

WindowRenderer::~WindowRenderer()
{
    releaseSwapChain();
}

FontResources& WindowRenderer::fonts()
{
    return m_device.fontResources(m_window->devicePixelRatio());
}

QSize WindowRenderer::pixelSize() const
{
    return m_hasSwapChain ? m_swapChain->currentPixelSize() : QSize();
}

bool WindowRenderer::ensureSwapChain()
{
    QRhi* rhi = m_device.rhi(m_window);
    if (!m_swapChain) {
        m_swapChain.reset(rhi->newSwapChain());
        m_swapChain->setWindow(m_window);
        if (m_translucent) {
            m_swapChain->setFlags(QRhiSwapChain::SurfaceHasPreMulAlpha);
        }
        m_renderPass.reset(m_swapChain->newCompatibleRenderPassDescriptor());
        m_swapChain->setRenderPassDescriptor(m_renderPass.get());
    }
    const QSize surface = m_swapChain->surfacePixelSize();
    if (surface.isEmpty()) {
        return false;
    }
    if (!m_hasSwapChain || m_swapChain->currentPixelSize() != surface) {
        m_hasSwapChain = m_swapChain->createOrResize();
    }
    return m_hasSwapChain;
}

void WindowRenderer::releaseSwapChain()
{
    m_post.reset();
    m_postPipelines = {};
    m_postPipelinePass = nullptr;
    m_swapPipelines = {};
    m_imageBindings.clear();
    m_bindings.reset();
    m_uniforms.reset();
    m_instances.reset();
    if (m_swapChain) {
        m_swapChain->destroy();
    }
    m_swapChain.reset();
    m_renderPass.reset();
    m_hasSwapChain = false;
    m_boundTextureGeneration = 0;
}

void WindowRenderer::setTranslucent(bool translucent)
{
    if (translucent == m_translucent) {
        return;
    }
    m_translucent = translucent;
    // The flag only takes effect on a new swapchain.
    if (m_swapChain) {
        releaseSwapChain();
    }
}

void WindowRenderer::setShaderChains(std::vector<ShaderProgramPtr> pane, std::vector<ShaderProgramPtr> window)
{
    if (pane.empty() && window.empty()) {
        m_post.reset();
        m_postPipelines = {};
        m_postPipelinePass = nullptr;
        return;
    }
    if (!m_post) {
        m_post = std::make_unique<PostChain>(m_device);
    }
    m_post->setChains(std::move(pane), std::move(window));
}

bool WindowRenderer::animating() const
{
    return m_post && m_post->animated();
}

void WindowRenderer::ensureBindings(FontResources& res)
{
    if (m_bindings && m_boundTextureGeneration == res.textureGeneration) {
        return;
    }
    QRhi* rhi = m_device.rhi();
    const bool fresh = !m_bindings;
    if (fresh) {
        m_bindings.reset(rhi->newShaderResourceBindings());
    }
    const auto stages = QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage;
    m_bindings->setBindings({
        QRhiShaderResourceBinding::uniformBuffer(0, stages, m_uniforms.get()),
        QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, res.grayTexture,
                                                  m_device.atlasSampler()),
        QRhiShaderResourceBinding::sampledTexture(2, QRhiShaderResourceBinding::FragmentStage, res.colorTexture,
                                                  m_device.atlasSampler()),
    });
    if (fresh) {
        m_bindings->create();
    } else {
        // Same layout, new textures: the pipelines stay valid.
        m_bindings->updateResources();
    }
    m_boundTextureGeneration = res.textureGeneration;
}

void WindowRenderer::initResources(FontResources& res)
{
    QRhi* rhi = m_device.rhi();
    m_uniforms.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, 32));
    m_uniforms->create();
    m_instances.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, sizeof(Instance) * 8192));
    m_instances->create();
    ensureBindings(res);
}

std::unique_ptr<QRhiGraphicsPipeline> WindowRenderer::makeGridPipeline(QRhiRenderPassDescriptor* pass)
{
    std::unique_ptr<QRhiGraphicsPipeline> p(m_device.rhi()->newGraphicsPipeline());
    p->setTargetBlends({premultipliedBlend()});
    p->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    p->setFlags(QRhiGraphicsPipeline::UsesScissor);
    p->setShaderStages({
        {QRhiShaderStage::Vertex, loadShader(QStringLiteral(":/afterglow/shaders/grid.vert.qsb"))},
        {QRhiShaderStage::Fragment, loadShader(QStringLiteral(":/afterglow/shaders/grid.frag.qsb"))},
    });
    p->setVertexInputLayout(instanceLayout());
    p->setShaderResourceBindings(m_bindings.get());
    p->setRenderPassDescriptor(pass);
    if (!p->create()) {
        throw std::runtime_error("failed to create the grid pipeline");
    }
    return p;
}

std::unique_ptr<QRhiGraphicsPipeline> WindowRenderer::makeImagePipeline(QRhiRenderPassDescriptor* pass,
                                                                        QRhiShaderResourceBindings* layout)
{
    std::unique_ptr<QRhiGraphicsPipeline> p(m_device.rhi()->newGraphicsPipeline());
    p->setTargetBlends({premultipliedBlend()});
    p->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    p->setFlags(QRhiGraphicsPipeline::UsesScissor);
    p->setShaderStages({
        {QRhiShaderStage::Vertex, loadShader(QStringLiteral(":/afterglow/shaders/grid.vert.qsb"))},
        {QRhiShaderStage::Fragment, loadShader(QStringLiteral(":/afterglow/shaders/image.frag.qsb"))},
    });
    p->setVertexInputLayout(instanceLayout());
    // Any image binding set has the same layout, so the first one serves.
    p->setShaderResourceBindings(layout);
    p->setRenderPassDescriptor(pass);
    if (!p->create()) {
        throw std::runtime_error("failed to create the image pipeline");
    }
    return p;
}

QRhiShaderResourceBindings* WindowRenderer::imageBindings(QRhiTexture* texture, std::uint64_t serial)
{
    ImageBinding& b = m_imageBindings[serial];
    if (!b.srb) {
        b.srb.reset(m_device.rhi()->newShaderResourceBindings());
        b.srb->setBindings({
            QRhiShaderResourceBinding::uniformBuffer(
                0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage, m_uniforms.get()),
            QRhiShaderResourceBinding::sampledTexture(1, QRhiShaderResourceBinding::FragmentStage, texture,
                                                      m_device.imageSampler()),
        });
        b.srb->create();
    }
    b.lastUsed = m_frameCounter;
    return b.srb.get();
}

void WindowRenderer::render(const std::vector<Pane>& panes,
                            const std::vector<std::pair<QRect, std::uint32_t>>& overlays, std::uint32_t clearColor,
                            QRhiTexture* overlayTexture, std::uint64_t overlaySerial)
{
    if (!m_hasSwapChain) {
        return;
    }
    QRhi* rhi = m_device.rhi();
    FontResources& res = fonts();
    if (m_fontEpoch != m_device.fontEpoch()) {
        m_builder.clearShapeCache();
        m_fontEpoch = m_device.fontEpoch();
    }

    // Build all instances first; a full atlas resets once and rebuilds.
    for (int attempt = 0; attempt < 2; ++attempt) {
        m_frame.clear();
        m_ranges.clear();
        bool ok = true;
        for (const Pane& pane : panes) {
            const auto first = std::uint32_t(m_frame.size());
            std::size_t textStart = 0;
            if (!m_builder.buildPane(*pane.snapshot, pane.params, *res.fonts, res.glyphs, m_frame, textStart)) {
                ok = false;
                break;
            }
            m_ranges.push_back({first, std::uint32_t(textStart), std::uint32_t(m_frame.size()), pane.params.rect, {}});
        }
        if (ok) {
            break;
        }
        res.glyphs.reset();
    }
    // Image quads go after all grid instances; each is drawn with its own texture.
    for (std::size_t i = 0; i < m_ranges.size(); ++i) {
        const Pane& pane = panes[i];
        const QPoint origin = pane.params.contentOrigin;
        for (const AfterglowVt::VisiblePlacement& img : pane.snapshot->images) {
            const float fw = float(img.frame->width);
            const float fh = float(img.frame->height);
            const auto index = std::uint32_t(m_frame.size());
            m_frame.push_back({float(origin.x() + img.x), float(origin.y() + img.y), float(img.width),
                               float(img.height), float(img.srcX) / fw, float(img.srcY) / fh,
                               float(img.srcX + img.srcW) / fw, float(img.srcY + img.srcH) / fh,
                               packRgba(255, 255, 255), std::uint32_t(InstanceKind::Solid)});
            m_ranges[i].images.push_back({index, img.frame, img.z < 0});
        }
    }
    const auto overlayFirst = std::uint32_t(m_frame.size());
    const QSize framePx = m_swapChain->currentPixelSize();
    for (const auto& [rect, color] : overlays) {
        m_frame.push_back({float(rect.x()), float(rect.y()), float(rect.width()), float(rect.height()), 0, 0, 0, 0,
                           color, std::uint32_t(InstanceKind::Solid)});
    }
    const auto overlayEnd = std::uint32_t(m_frame.size());
    // The chrome quad goes last; it is drawn with the image pipeline.
    std::uint32_t chromeInstance = 0;
    if (overlayTexture) {
        chromeInstance = std::uint32_t(m_frame.size());
        m_frame.push_back({0, 0, float(framePx.width()), float(framePx.height()), 0, 0, 1, 1, packRgba(255, 255, 255),
                           1u});
    }

    QRhi::FrameOpResult r = rhi->beginFrame(m_swapChain.get());
    if (r == QRhi::FrameOpSwapChainOutOfDate) {
        if (!ensureSwapChain()) {
            return;
        }
        r = rhi->beginFrame(m_swapChain.get());
    }
    if (r != QRhi::FrameOpSuccess) {
        return;
    }

    QRhiResourceUpdateBatch* updates = rhi->nextResourceUpdateBatch();
    m_device.prepareAtlases(res, updates);
    // Image textures upload in this frame's batch, before the pass begins.
    ++m_frameCounter;
    m_pendingImages.clear();
    for (const DrawRange& range : m_ranges) {
        for (const ImageDraw& draw : range.images) {
            if (!m_pendingImages.count(draw.frame.get())) {
                std::uint64_t serial = 0;
                QRhiTexture* tex = m_device.imageTexture(draw.frame, updates, serial);
                m_pendingImages.emplace(draw.frame.get(), std::make_pair(tex, serial));
            }
        }
    }
    if (!m_uniforms) {
        initResources(res);
    }
    ensureBindings(res);

    const QSize px = m_swapChain->currentPixelSize();
    const float uniforms[8] = {
        float(px.width()),
        float(px.height()),
        float(res.glyphs.grayPage().size()),
        float(res.glyphs.grayPage().size()),
        float(res.glyphs.colorPage().size()),
        float(res.glyphs.colorPage().size()),
        0.0f,
        0.0f,
    };
    updates->updateDynamicBuffer(m_uniforms.get(), 0, sizeof uniforms, uniforms);

    const auto bytes = quint32(m_frame.size() * sizeof(Instance));
    if (bytes > m_instances->size()) {
        m_instances.reset(rhi->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::VertexBuffer, bytes + bytes / 2));
        m_instances->create();
    }
    if (bytes > 0) {
        updates->updateDynamicBuffer(m_instances.get(), 0, bytes, m_frame.data());
    }

    // With shaders, the grid goes to an offscreen texture first.
    const bool post = m_post && m_post->active();
    QRhiRenderTarget* gridTarget = m_swapChain->currentFrameRenderTarget();
    QRhiRenderPassDescriptor* gridPass = m_renderPass.get();
    PipelineSet* gridPipelines = &m_swapPipelines;
    if (post) {
        gridTarget = m_post->prepare(px);
        gridPass = m_post->renderPass();
        if (m_postPipelinePass != gridPass) {
            m_postPipelines = {};
            m_postPipelinePass = gridPass;
        }
        gridPipelines = &m_postPipelines;
    }
    if (!gridPipelines->grid) {
        gridPipelines->grid = makeGridPipeline(gridPass);
    }

    const QColor clear = premultipliedClear(clearColor);
    QRhiCommandBuffer* cb = m_swapChain->currentFrameCommandBuffer();
    const QRhiCommandBuffer::VertexInput input(m_instances.get(), 0);
    // QRhi scissors use a bottom-left origin.
    auto scissor = [&px](const QRect& rc) {
        const QRect c = rc.intersected(QRect(QPoint(0, 0), px));
        return QRhiScissor(c.x(), px.height() - (c.y() + c.height()), c.width(), c.height());
    };
    auto bindGrid = [&] {
        cb->setGraphicsPipeline(gridPipelines->grid.get());
        cb->setViewport({0, 0, float(px.width()), float(px.height())});
        cb->setShaderResources(m_bindings.get());
        cb->setVertexInput(0, 1, &input);
    };
    auto drawImages = [&](const DrawRange& range, bool below) {
        bool bound = false;
        for (const ImageDraw& draw : range.images) {
            if (draw.belowText != below) {
                continue;
            }
            const auto it = m_pendingImages.find(draw.frame.get());
            if (it == m_pendingImages.end()) {
                continue;
            }
            QRhiShaderResourceBindings* srb = imageBindings(it->second.first, it->second.second);
            if (!gridPipelines->image) {
                gridPipelines->image = makeImagePipeline(gridPass, srb);
            }
            cb->setGraphicsPipeline(gridPipelines->image.get());
            cb->setViewport({0, 0, float(px.width()), float(px.height())});
            cb->setScissor(scissor(range.scissor));
            cb->setShaderResources(srb);
            cb->setVertexInput(0, 1, &input);
            cb->draw(4, 1, 0, draw.instance);
            bound = true;
        }
        if (bound) {
            bindGrid();
        }
    };
    auto drawChrome = [&] {
        if (!overlayTexture) {
            return;
        }
        // High bit keeps chrome serials apart from image texture serials.
        QRhiShaderResourceBindings* srb = imageBindings(overlayTexture, overlaySerial | (1ull << 63));
        if (!m_swapPipelines.image) {
            m_swapPipelines.image = makeImagePipeline(m_renderPass.get(), srb);
        }
        cb->setGraphicsPipeline(m_swapPipelines.image.get());
        cb->setViewport({0, 0, float(px.width()), float(px.height())});
        cb->setScissor(scissor(QRect(QPoint(0, 0), px)));
        cb->setShaderResources(srb);
        cb->setVertexInput(0, 1, &input);
        cb->draw(4, 1, 0, chromeInstance);
    };

    // Grid pass: backgrounds, images below the text, text, images above.
    cb->beginPass(gridTarget, clear, {1.0f, 0}, updates);
    bindGrid();
    for (const DrawRange& range : m_ranges) {
        if (range.scissor.isEmpty()) {
            continue;
        }
        cb->setScissor(scissor(range.scissor));
        if (range.textStart > range.first) {
            cb->draw(4, range.textStart - range.first, 0, range.first);
        }
        drawImages(range, true);
        cb->setScissor(scissor(range.scissor));
        if (range.end > range.textStart) {
            cb->draw(4, range.end - range.textStart, 0, range.textStart);
        }
        drawImages(range, false);
    }
    if (overlayEnd > overlayFirst) {
        cb->setScissor(scissor(QRect(QPoint(0, 0), px)));
        cb->draw(4, overlayEnd - overlayFirst, 0, overlayFirst);
    }

    if (post) {
        cb->endPass();
        // Shader inputs: per pane, and the focused pane's view for the window.
        const float now = float(m_clock.nsecsElapsed()) / 1e9f;
        const ShaderFrameTime time{now, m_lastFrameTime > 0 ? now - m_lastFrameTime : 0.0f, m_shaderFrame++};
        m_lastFrameTime = now;
        const FontMetrics& m = res.fonts->metrics();
        std::vector<ShaderRegionState> regions;
        ShaderRegionState windowState;
        windowState.rect = QRect(QPoint(0, 0), px);
        for (const Pane& pane : panes) {
            const PaneSnapshot& snap = *pane.snapshot;
            ShaderRegionState st;
            st.rect = pane.params.rect;
            st.snapshot = &snap;
            st.cellSize = QSizeF(m.cellWidth, m.cellHeight);
            st.mouse = m_mouse;
            const int span = (snap.cursorX < snap.cols && snap.cursorY < snap.rows
                              && snap.at(snap.cursorX, snap.cursorY).width == 2)
                ? 2
                : 1;
            QRectF cursor(pane.params.contentOrigin.x() + snap.cursorX * m.cellWidth,
                          pane.params.contentOrigin.y() + snap.cursorY * m.cellHeight, span * m.cellWidth,
                          m.cellHeight);
            const int style = !pane.params.focused                                         ? 1
                              : snap.cursorShape == AfterglowVt::CursorShape::Bar          ? 2
                              : snap.cursorShape == AfterglowVt::CursorShape::Underline    ? 3
                                                                                           : 0;
            CursorTrack& track = m_cursorTracks[&snap];
            const QColor color = toColor(snap.cursorColor);
            if (track.seenFrame == 0) {
                track.cursor = track.previous = cursor;
                track.color = track.previousColor = color;
                track.style = track.previousStyle = style;
                track.focused = pane.params.focused;
            }
            if (cursor != track.cursor || color != track.color) {
                track.previous = track.cursor;
                track.previousColor = track.color;
                track.previousStyle = track.style;
                track.cursor = cursor;
                track.color = color;
                track.changeTime = now;
            }
            track.style = style;
            if (pane.params.focused != track.focused) {
                track.focused = pane.params.focused;
                track.focusTime = now;
            }
            track.seenFrame = m_frameCounter;
            st.cursor = track.cursor;
            st.previousCursor = track.previous;
            st.cursorColor = track.color;
            st.previousCursorColor = track.previousColor;
            st.cursorStyle = track.style;
            st.previousCursorStyle = track.previousStyle;
            st.cursorVisible = snap.cursorVisible;
            st.cursorChangeTime = track.changeTime;
            st.focused = track.focused;
            st.focusChangeTime = track.focusTime;
            regions.push_back(st);
            if (pane.params.focused || windowState.snapshot == nullptr) {
                const QRect whole = windowState.rect;
                windowState = st;
                windowState.rect = whole;
            }
        }
        std::erase_if(m_cursorTracks, [this](const auto& kv) { return kv.second.seenFrame != m_frameCounter; });
        m_post->record(cb, regions, windowState, time, m_swapChain->currentFrameRenderTarget(), m_renderPass.get());
    }
    drawChrome();
    cb->endPass();
    rhi->endFrame(m_swapChain.get());

    // Bindings of textures not drawn for a while (deleted or scrolled away).
    std::erase_if(m_imageBindings, [this](const auto& kv) { return kv.second.lastUsed + 120 < m_frameCounter; });
    m_device.pruneImageTextures();
}

} // namespace AfterglowRender
