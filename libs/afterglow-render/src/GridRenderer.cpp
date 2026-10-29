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
constexpr std::uint16_t BlinkAttr = AfterglowVt::Attr::Blink;
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
                          float(pos[i].y_offset) / 64.0f, pen + float(pos[i].x_offset) / 64.0f});
        pen += float(pos[i].x_advance) / 64.0f;
    }
    return m_shapeCache.emplace(std::move(key), std::move(glyphs)).first->second;
}

namespace {

/// A symbol from a Private Use Area (Nerd Font icons), other than the
/// powerline and box shapes drawn as sprites.
bool isIcon(char32_t cp)
{
    const bool pua = (cp >= 0xE000 && cp <= 0xF8FF) || (cp >= 0xF0000 && cp <= 0xFFFFD) || (cp >= 0x100000 && cp <= 0x10FFFD);
    return pua && !Sprites::isSprite(cp);
}

} // namespace

bool FrameBuilder::buildPane(const PaneSnapshot& s, const PaneParams& p, FontSet& fonts, GlyphCache& glyphs,
                             std::vector<Instance>& out, Ranges& ranges)
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

    // Blinking text (SGR 5) disappears in the off phase; its background stays.
    const bool hideBlinking = !p.textBlinkOn && s.hasBlinkingText;
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
    bool recolorUnderCursor = blockCursor && cursorSettled;
    // The cursor box: a cell, a doubled cell on a double-size line, or the
    // whole of a multicell block.
    float curX = cursorX;
    float curY = cursorY;
    float curW = float(cursorSpan * cw);
    float curH = float(ch);
    if (cursorInGrid) {
        const SnapshotCell& cc = s.at(s.cursorX, s.cursorY);
        if (s.lineSize(s.cursorY) != AfterglowVt::LineSize::Single) {
            curX = ox + float(2 * s.cursorX * cw);
            curW *= 2;
            recolorUnderCursor = false;
        } else if (cc.mcScale != 0) {
            curX = ox + float((s.cursorX - cc.mcDx) * cw);
            curY = oy + float((s.cursorY - cc.mcDy) * ch);
            curW = float(cc.mcScale * cc.mcWidth * cw);
            curH = float(cc.mcScale * ch);
            recolorUnderCursor = false;
        }
    }

    // Pushes a glyph quad clipped vertically to [top, bottom), its texture
    // coordinates cut to match (double-height halves).
    auto pushClipped = [&out](Instance inst, float top, float bottom) {
        if (inst.h <= 0 || inst.y >= bottom || inst.y + inst.h <= top) {
            return;
        }
        const float v0 = inst.v0;
        const float span = inst.v1 - inst.v0;
        const float y0 = inst.y;
        const float h0 = inst.h;
        if (inst.y < top) {
            inst.v0 = v0 + span * (top - y0) / h0;
            inst.h -= top - inst.y;
            inst.y = top;
        }
        if (inst.y + inst.h > bottom) {
            inst.v1 = v0 + span * (bottom - y0) / h0;
            inst.h = bottom - inst.y;
        }
        out.push_back(inst);
    };
    constexpr float NoClip = 1e9f;

    auto isColorText = [&s](const SnapshotCell& cell, int width) {
        bool color = width == 2 && AfterglowVt::Unicode::props(cell.cp).extendedPictographic;
        if (!color && cell.grapheme) {
            const std::u32string& extra = s.graphemes[cell.grapheme - 1];
            color = extra.find(AfterglowVt::Unicode::VariationSelector16) != std::u32string::npos;
        }
        return color;
    };
    auto cellText = [&s](const SnapshotCell& cell) {
        std::u32string text(1, cell.cp);
        if (cell.grapheme) {
            text += s.graphemes[cell.grapheme - 1];
        }
        return text;
    };
    // Draws text at @p scale16 sixteenths of the base size with its pen at
    // (@p x, @p baseline), squashed vertically by @p vstretch and clipped.
    auto drawScaled = [&](int face, StyleSlot slot, const std::u32string& text, float x, float baseline, int scale16,
                          float vstretch, std::uint32_t color, int cellSpan, float clipTop, float clipBottom) -> bool {
        if (alphaOf(color) == 0) {
            return true;
        }
        const float f = float(scale16) / 16.0f;
        const auto& shaped = shape(fonts, face, int(slot), text);
        for (const ShapedGlyph& g : shaped) {
            const GlyphEntry* e = glyphs.glyph(fonts, face, g.glyphId, cellSpan, scale16);
            if (!e) {
                return false;
            }
            if (e->empty) {
                continue;
            }
            Instance inst{};
            inst.u0 = float(e->u);
            inst.v0 = float(e->v);
            inst.u1 = float(e->u + e->width);
            inst.v1 = float(e->v + e->height);
            inst.w = float(e->width);
            inst.h = float(e->height) * vstretch;
            if (e->color) {
                inst.x = x + float(e->left);
                inst.y = baseline - float(e->top) * vstretch;
                inst.color = withAlpha(0xFFFFFFu, alphaOf(color));
                inst.kind = std::uint32_t(InstanceKind::ColorGlyph);
            } else {
                inst.x = std::round(x + g.penX * f + float(e->left));
                inst.y = std::round(baseline - (float(e->top) + g.yOffset * f) * vstretch);
                inst.color = color;
                inst.kind = std::uint32_t(InstanceKind::GrayGlyph);
            }
            pushClipped(inst, clipTop, clipBottom);
        }
        return true;
    };

    // Backgrounds, merged into horizontal runs.
    for (int y = 0; y < drawRows; ++y) {
        const float rowTop = oy + float(y * ch);
        if (s.lineSize(y) != AfterglowVt::LineSize::Single) {
            // Double-size line: the left half of the cells, twice as wide.
            for (int x = 0; x < s.cols / 2; ++x) {
                const std::uint32_t bg = s.at(x, y).bg;
                if (alphaOf(bg) != 0) {
                    solid(ox + float(2 * x * cw), rowTop, float(2 * cw), float(ch), bg);
                }
            }
            continue;
        }
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
    ranges.textStart = out.size();

    // Text, after every background so overhangs are not covered.
    for (int y = 0; y < drawRows; ++y) {
        const float baseline = oy + float(y * ch + m.baseline);
        const float rowTop = oy + float(y * ch);
        const AfterglowVt::LineSize lineSize = s.lineSize(y);
        if (lineSize != AfterglowVt::LineSize::Single) {
            // DECDWL/DECDHL: each cell twice as wide; double height shows
            // the top or bottom half of characters twice as tall.
            const bool tall = lineSize != AfterglowVt::LineSize::DoubleWidth;
            const float bandTop = lineSize == AfterglowVt::LineSize::DoubleBottom ? rowTop - float(ch) : rowTop;
            for (int x = 0; x < s.cols / 2; ++x) {
                const SnapshotCell& c = s.at(x, y);
                if (c.width == 0 || c.cp == 0 || alphaOf(c.fg) == 0 || (hideBlinking && (c.attrs & BlinkAttr))) {
                    continue;
                }
                const float cx = ox + float(2 * x * cw);
                if (c.grapheme == 0 && Sprites::isSprite(c.cp)) {
                    const GlyphEntry* e = glyphs.sprite(fonts, c.cp, 2, tall ? 2 : 1);
                    if (!e) {
                        return false;
                    }
                    if (!e->empty) {
                        pushClipped({cx, tall ? bandTop : rowTop, float(e->width), float(e->height), float(e->u),
                                     float(e->v), float(e->u + e->width), float(e->v + e->height), c.fg,
                                     std::uint32_t(InstanceKind::GrayGlyph)},
                                    rowTop, rowTop + float(ch));
                    }
                    continue;
                }
                const StyleSlot slot = slotFor(c.attrs);
                const int face = fonts.faceFor(c.cp, slot, isColorText(c, c.width));
                const float lineBaseline = tall ? bandTop + float(2 * m.baseline) : baseline;
                if (!drawScaled(face, slot, cellText(c), cx, lineBaseline, 32, tall ? 1.0f : 0.5f, c.fg,
                                std::max<int>(1, c.width), rowTop, rowTop + float(ch))) {
                    return false;
                }
            }
            continue;
        }
        int x = 0;
        while (x < s.cols) {
            const SnapshotCell& c = s.at(x, y);
            if (c.width == 0 || c.cp == 0 || (hideBlinking && (c.attrs & BlinkAttr))) {
                ++x;
                continue;
            }
            if (c.mcScale != 0) {
                // A multicell block draws once: from its top-left cell, or
                // from its first visible row when the top has scrolled off.
                if (c.mcDx == 0 && (c.mcDy == 0 || y == 0)) {
                    const int num = c.mcFraction >> 4;
                    const int den = c.mcFraction & 0xF;
                    const float frac = den > num && num > 0 ? float(num) / float(den) : 1.0f;
                    const float f = float(c.mcScale) * frac;
                    const float bx = ox + float(x * cw);
                    const float by = rowTop - float(c.mcDy * ch);
                    const float bw = float(c.mcScale * c.mcWidth * cw);
                    const float bh = float(c.mcScale * ch);
                    const float aw = bw * frac;
                    const float ah = bh * frac;
                    const int halign = c.mcAlign & 3;
                    const int valign = c.mcAlign >> 2;
                    const float ax = bx + (halign == 1 ? bw - aw : halign == 2 ? (bw - aw) / 2 : 0.0f);
                    const float ay = by + (valign == 1 ? bh - ah : valign == 2 ? (bh - ah) / 2 : 0.0f);
                    const StyleSlot slot = slotFor(c.attrs);
                    const bool color = isColorText(c, c.mcWidth >= 2 ? 2 : 1);
                    const int face = fonts.faceFor(c.cp, slot, color);
                    const int scale16 = std::max(1, int(std::lround(f * 16.0f)));
                    if (c.grapheme == 0 && Sprites::isSprite(c.cp) && frac == 1.0f) {
                        const GlyphEntry* e = glyphs.sprite(fonts, c.cp, c.mcScale * c.mcWidth, c.mcScale);
                        if (!e) {
                            return false;
                        }
                        if (!e->empty && alphaOf(c.fg) != 0) {
                            out.push_back({bx, by, float(e->width), float(e->height), float(e->u), float(e->v),
                                           float(e->u + e->width), float(e->v + e->height), c.fg,
                                           std::uint32_t(InstanceKind::GrayGlyph)});
                        }
                    } else if (!drawScaled(face, slot, cellText(c), ax, ay + float(m.baseline) * f, scale16, 1.0f, c.fg,
                                           c.mcWidth, -NoClip, NoClip)) {
                        return false;
                    }
                }
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
                if (n.cp == 0 || n.mcScale != 0 || (hideBlinking && (n.attrs & BlinkAttr)) || slotFor(n.attrs) != slot || (n.grapheme == 0 && Sprites::isSprite(n.cp))
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
                // Icons (Nerd Font symbols) are fitted to the grid: two cells
                // when the next one is blank, as prompts expect, else one.
                const bool icon = cell.grapheme == 0 && isIcon(cell.cp);
                int span = std::max<int>(1, cell.width);
                if (icon && span == 1 && col + 1 < s.cols) {
                    const SnapshotCell& next = s.at(col + 1, y);
                    if ((next.cp == 0 || next.cp == U' ') && next.width == 1 && next.mcScale == 0) {
                        span = 2;
                    }
                }
                const GlyphEntry* e = icon ? glyphs.icon(fonts, face, g.glyphId, span)
                                           : glyphs.glyph(fonts, face, g.glyphId, span);
                if (!e) {
                    return false;
                }
                if (e->empty) {
                    continue;
                }
                const float cellX = ox + float(col * cw);
                if (icon && !e->color) {
                    out.push_back({std::round(cellX + float(e->left)), std::round(baseline - float(e->top)), float(e->width),
                                   float(e->height), float(e->u), float(e->v), float(e->u + e->width),
                                   float(e->v + e->height), color, std::uint32_t(InstanceKind::GrayGlyph)});
                } else if (e->color) {
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
        const AfterglowVt::LineSize lineSize = s.lineSize(y);
        if (lineSize == AfterglowVt::LineSize::DoubleTop) {
            continue; // decorations belong to the bottom half
        }
        const int xs = lineSize == AfterglowVt::LineSize::Single ? 1 : 2;
        const int cols = lineSize == AfterglowVt::LineSize::Single ? s.cols : s.cols / 2;
        int x = 0;
        while (x < cols) {
            const SnapshotCell& c = s.at(x, y);
            const UnderlineStyle ul = c.mcScale != 0 ? UnderlineStyle::None : c.underline;
            if (ul == UnderlineStyle::None) {
                ++x;
                continue;
            }
            int end = x + 1;
            while (end < cols && s.at(end, y).mcScale == 0 && s.at(end, y).underline == ul
                   && s.at(end, y).underlineColor == c.underlineColor) {
                ++end;
            }
            const float rx = ox + float(x * xs * cw);
            const float rw = float((end - x) * xs * cw);
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
        for (int cx = 0; cx < cols && xs == 1; ++cx) {
            const SnapshotCell& c = s.at(cx, y);
            if (c.mcScale != 0) {
                continue;
            }
            const bool hovered = (p.hoveredLink != 0 && c.link == p.hoveredLink)
                || (y == p.hoverRow && cx >= p.hoverColStart && cx <= p.hoverColEnd);
            if (hovered && c.underline == UnderlineStyle::None) {
                solid(ox + float(cx * cw), std::round(rowTop + float(m.underlinePosition) - th / 2), float(cw), th,
                      c.fg);
            }
            if ((c.attrs & Strikethrough) && alphaOf(c.fg) != 0) {
                solid(ox + float(cx * cw), std::round(rowTop + float(m.strikethroughPosition) - th / 2), float(cw), th,
                      c.fg);
            }
            if ((c.attrs & Overline) && alphaOf(c.fg) != 0) {
                solid(ox + float(cx * cw), rowTop, float(cw), th, c.fg);
            }
        }
    }

    ranges.cursorStart = out.size();
    ranges.cursorBelowText = blockCursor;
    if (blockCursor) {
        solid(curX, curY, curW, curH, s.cursorColor);
    }
    if (drawCursor && !blockCursor) {
        const float x0 = curX;
        const float y0 = curY;
        const float w = curW;
        const float hh = curH;
        const float t = std::max(1.0f, std::round(dpr));
        if (!p.focused) {
            solid(x0, y0, w, t, s.cursorColor);
            solid(x0, y0 + hh - t, w, t, s.cursorColor);
            solid(x0, y0, t, hh, s.cursorColor);
            solid(x0 + w - t, y0, t, hh, s.cursorColor);
        } else if (s.cursorShape == AfterglowVt::CursorShape::Bar) {
            solid(x0, y0, std::max(t, std::round(float(cw) * 0.12f)), hh, s.cursorColor);
        } else {
            const float h = std::max(t, std::round(float(ch) * 0.08f));
            solid(x0, y0 + hh - h, w, h, s.cursorColor);
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
    return m_device.fontResources(m_window ? m_window->devicePixelRatio() : m_devicePixelRatio);
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
    // GPU resources only: the shader chains stay, and their textures and
    // pipelines are made again on the next frame.
    if (m_post) {
        m_post->release();
    }
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

void WindowRenderer::setShaderChains(ShaderChains chains)
{
    if (chains.empty()) {
        m_post.reset();
        m_postPipelines = {};
        m_postPipelinePass = nullptr;
        return;
    }
    if (!m_post) {
        m_post = std::make_unique<PostChain>(m_device);
    }
    m_post->setChains(std::move(chains));
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

std::unique_ptr<QRhiGraphicsPipeline> WindowRenderer::makeGridPipeline(QRhiRenderPassDescriptor* pass, bool blend)
{
    std::unique_ptr<QRhiGraphicsPipeline> p(m_device.rhi()->newGraphicsPipeline());
    p->setTargetBlends({blend ? premultipliedBlend() : QRhiGraphicsPipeline::TargetBlend{}});
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
    Target target;
    target.size = m_swapChain->currentPixelSize();
    target.begin = [this, rhi]() -> QRhiCommandBuffer* {
        QRhi::FrameOpResult r = rhi->beginFrame(m_swapChain.get());
        if (r == QRhi::FrameOpSwapChainOutOfDate) {
            if (!ensureSwapChain()) {
                return nullptr;
            }
            r = rhi->beginFrame(m_swapChain.get());
        }
        return r == QRhi::FrameOpSuccess ? m_swapChain->currentFrameCommandBuffer() : nullptr;
    };
    target.renderTarget = [this] { return m_swapChain->currentFrameRenderTarget(); };
    target.renderPass = m_renderPass.get();
    target.pipelines = &m_swapPipelines;
    target.end = [this, rhi] { rhi->endFrame(m_swapChain.get()); };
    renderTo(target, panes, overlays, clearColor, overlayTexture, overlaySerial);
}

QImage WindowRenderer::renderToImage(const QSize& size, const std::vector<Pane>& panes,
                                     const std::vector<std::pair<QRect, std::uint32_t>>& overlays,
                                     std::uint32_t clearColor)
{
    QRhi* rhi = m_device.rhi(m_window);
    if (!m_offscreenTexture || m_offscreenTexture->pixelSize() != size) {
        m_offscreenPipelines = {};
        m_offscreenTarget.reset();
        m_offscreenPass.reset();
        m_offscreenTexture.reset(
            rhi->newTexture(QRhiTexture::RGBA8, size, 1, QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        m_offscreenTexture->create();
        m_offscreenTarget.reset(rhi->newTextureRenderTarget({m_offscreenTexture.get()}));
        m_offscreenPass.reset(m_offscreenTarget->newCompatibleRenderPassDescriptor());
        m_offscreenTarget->setRenderPassDescriptor(m_offscreenPass.get());
        m_offscreenTarget->create();
    }
    QRhiReadbackResult readback;
    QRhiCommandBuffer* cb = nullptr;
    Target target;
    target.size = size;
    target.begin = [rhi, &cb]() -> QRhiCommandBuffer* {
        return rhi->beginOffscreenFrame(&cb) == QRhi::FrameOpSuccess ? cb : nullptr;
    };
    target.renderTarget = [this] { return m_offscreenTarget.get(); };
    target.renderPass = m_offscreenPass.get();
    target.pipelines = &m_offscreenPipelines;
    target.finalUpdates = [this, rhi, &readback] {
        QRhiResourceUpdateBatch* u = rhi->nextResourceUpdateBatch();
        u->readBackTexture(QRhiReadbackDescription(m_offscreenTexture.get()), &readback);
        return u;
    };
    // An offscreen frame completes, readbacks included, when it ends.
    target.end = [rhi] { rhi->endOffscreenFrame(); };
    renderTo(target, panes, overlays, clearColor, nullptr, 0);
    if (readback.data.isEmpty()) {
        return {};
    }
    return QImage(reinterpret_cast<const uchar*>(readback.data.constData()), readback.pixelSize.width(),
                  readback.pixelSize.height(), QImage::Format_RGBA8888_Premultiplied)
        .copy();
}

void WindowRenderer::renderTo(const Target& target, const std::vector<Pane>& panes,
                              const std::vector<std::pair<QRect, std::uint32_t>>& overlays, std::uint32_t clearColor,
                              QRhiTexture* overlayTexture, std::uint64_t overlaySerial)
{
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
            const FrameBuilder::PaneParams& pp = pane.params;
            PaneCache& cache = m_paneCache[pane.snapshot];
            const bool reusable = cache.snapshotVersion == pane.snapshot->version && cache.fontEpoch == m_fontEpoch
                && cache.glyphEpoch == res.glyphs.epoch() && cache.fonts == res.fonts.get() && cache.rect == pp.rect
                && cache.origin == pp.contentOrigin && cache.focused == pp.focused && cache.blinkOn == pp.cursorBlinkOn && cache.textBlinkOn == pp.textBlinkOn
                && cache.cursorPosition == pp.cursorPosition && cache.scrollShift == pp.scrollShift
                && cache.backgroundAlpha == pp.backgroundAlpha && cache.hoveredLink == pp.hoveredLink
                && cache.hoverRow == pp.hoverRow && cache.hoverColStart == pp.hoverColStart
                && cache.hoverColEnd == pp.hoverColEnd;
            if (!reusable) {
                m_scratch.clear();
                FrameBuilder::Ranges ranges;
                if (!m_builder.buildPane(*pane.snapshot, pp, *res.fonts, res.glyphs, m_scratch, ranges)) {
                    ok = false;
                    break;
                }
                cache.instances.swap(m_scratch);
                cache.ranges = ranges;
                cache.snapshotVersion = pane.snapshot->version;
                cache.fontEpoch = m_fontEpoch;
                cache.glyphEpoch = res.glyphs.epoch();
                cache.fonts = res.fonts.get();
                cache.rect = pp.rect;
                cache.origin = pp.contentOrigin;
                cache.focused = pp.focused;
                cache.blinkOn = pp.cursorBlinkOn;
                cache.textBlinkOn = pp.textBlinkOn;
                cache.cursorPosition = pp.cursorPosition;
                cache.scrollShift = pp.scrollShift;
                cache.backgroundAlpha = pp.backgroundAlpha;
                cache.hoveredLink = pp.hoveredLink;
                cache.hoverRow = pp.hoverRow;
                cache.hoverColStart = pp.hoverColStart;
                cache.hoverColEnd = pp.hoverColEnd;
            }
            cache.seenFrame = m_frameCounter + 1;
            m_frame.insert(m_frame.end(), cache.instances.begin(), cache.instances.end());
            m_ranges.push_back({first, first + std::uint32_t(cache.ranges.textStart),
                                first + std::uint32_t(cache.ranges.cursorStart), std::uint32_t(m_frame.size()),
                                cache.ranges.cursorBelowText, pp.rect, {}});
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
    const QSize framePx = target.size;
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

    QRhiCommandBuffer* cb = target.begin();
    if (!cb) {
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

    const QSize px = target.size;
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
    QRhiRenderTarget* gridTarget = target.renderTarget();
    QRhiRenderPassDescriptor* gridPass = target.renderPass;
    PipelineSet* gridPipelines = target.pipelines;
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
        gridPipelines->grid = makeGridPipeline(gridPass, true);
        gridPipelines->fill = makeGridPipeline(gridPass, false);
    }

    const QColor clear = premultipliedClear(clearColor);
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
        if (!target.pipelines->image) {
            target.pipelines->image = makeImagePipeline(target.renderPass, srb);
        }
        cb->setGraphicsPipeline(target.pipelines->image.get());
        cb->setViewport({0, 0, float(px.width()), float(px.height())});
        cb->setScissor(scissor(QRect(QPoint(0, 0), px)));
        cb->setShaderResources(srb);
        cb->setVertexInput(0, 1, &input);
        cb->draw(4, 1, 0, chromeInstance);
    };

    // Shader inputs: per pane, and the focused pane's view for the window.
    ShaderFrameTime time;
    std::vector<ShaderRegionState> regions;
    ShaderRegionState windowState;
    if (post) {
        const float now = m_clockOverride.value_or(float(m_clock.nsecsElapsed()) / 1e9f);
        time = ShaderFrameTime{now, m_lastFrameTime > 0 ? now - m_lastFrameTime : 0.0f, m_shaderFrame++};
        m_lastFrameTime = now;
        const FontMetrics& m = res.fonts->metrics();
                windowState.rect = QRect(QPoint(0, 0), px);
        for (const Pane& pane : panes) {
            const PaneSnapshot& snap = *pane.snapshot;
            ShaderRegionState st;
            st.rect = pane.params.rect;
            st.snapshot = &snap;
            st.cellSize = QSizeF(m.cellWidth, m.cellHeight);
            st.backgroundOpacity = float(pane.params.backgroundAlpha) / 255.0f;
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
            const auto wall = std::chrono::steady_clock::now();
            for (std::size_t i = 0; i < st.signals.size(); ++i) {
                const FrameBuilder::PaneParams::Signal& sig = pane.params.signals[i];
                const float age = sig.at ? std::chrono::duration<float>(wall - *sig.at).count() : 1e6f;
                st.signals[i] = {sig.value, now - age};
            }
            regions.push_back(st);
            if (pane.params.focused || windowState.snapshot == nullptr) {
                const QRect whole = windowState.rect;
                windowState = st;
                windowState.rect = whole;
            }
        }
        std::erase_if(m_cursorTracks, [this](const auto& kv) { return kv.second.seenFrame != m_frameCounter; });
    }

    auto drawBackgrounds = [&](const DrawRange& range) {
        // The pane's default background comes first and replaces what is
        // under it: blended over the translucent clear colour, its alpha
        // would compound (0.9 over 0.9 is 0.99).
        cb->setGraphicsPipeline(gridPipelines->fill.get());
        cb->draw(4, 1, 0, range.first);
        cb->setGraphicsPipeline(gridPipelines->grid.get());
        if (range.textStart > range.first + 1) {
            cb->draw(4, range.textStart - range.first - 1, 0, range.first + 1);
        }
    };
    auto drawSpan = [&](std::uint32_t from, std::uint32_t to) {
        if (to > from) {
            cb->draw(4, to - from, 0, from);
        }
    };
    auto drawOverlays = [&] {
        if (overlayEnd > overlayFirst) {
            cb->setScissor(scissor(QRect(QPoint(0, 0), px)));
            cb->draw(4, overlayEnd - overlayFirst, 0, overlayFirst);
        }
    };

    if (post && m_post->layered()) {
        // Each layer into its own texture; the layer chains then run on them
        // and the composite puts them together in the grid target.
        const struct
        {
            PostChain::Layer layer;
            std::function<void(const DrawRange&)> draw;
        } layers[] = {
            {PostChain::Layer::Background,
             [&](const DrawRange& range) {
                 drawBackgrounds(range);
                 drawImages(range, true);
             }},
            {PostChain::Layer::Text,
             [&](const DrawRange& range) {
                 drawSpan(range.textStart, range.cursorStart);
                 drawImages(range, false);
             }},
            {PostChain::Layer::Cursor, [&](const DrawRange& range) { drawSpan(range.cursorStart, range.end); }},
        };
        bool first = true;
        for (const auto& [layer, draw] : layers) {
            cb->beginPass(m_post->layerTarget(layer), Qt::transparent, {1.0f, 0}, first ? updates : nullptr);
            first = false;
            bindGrid();
            for (const DrawRange& range : m_ranges) {
                if (!range.scissor.isEmpty()) {
                    cb->setScissor(scissor(range.scissor));
                    draw(range);
                }
            }
            cb->endPass();
        }
        std::vector<bool> cursorBelowText;
        for (const DrawRange& range : m_ranges) {
            cursorBelowText.push_back(range.cursorBelowText);
        }
        m_post->composite(cb, regions, cursorBelowText, time, clear);
        bindGrid();
        drawOverlays();
        cb->endPass();
    } else {
        // One pass: backgrounds, a block cursor, images below the text, text,
        // other cursors, images above.
        cb->beginPass(gridTarget, clear, {1.0f, 0}, updates);
        bindGrid();
        for (const DrawRange& range : m_ranges) {
            if (range.scissor.isEmpty()) {
                continue;
            }
            cb->setScissor(scissor(range.scissor));
            drawBackgrounds(range);
            if (range.cursorBelowText) {
                drawSpan(range.cursorStart, range.end);
            }
            drawImages(range, true);
            cb->setScissor(scissor(range.scissor));
            drawSpan(range.textStart, range.cursorStart);
            if (!range.cursorBelowText) {
                drawSpan(range.cursorStart, range.end);
            }
            drawImages(range, false);
        }
        drawOverlays();
        if (post) {
            cb->endPass();
        }
    }
    if (post) {
        m_post->record(cb, regions, windowState, time, target.renderTarget(), target.renderPass);
    }
    drawChrome();
    cb->endPass(target.finalUpdates ? target.finalUpdates() : nullptr);
    target.end();

    // Panes not drawn this frame (closed, or in another tab) drop their cache.
    std::erase_if(m_paneCache, [this](const auto& kv) { return kv.second.seenFrame != m_frameCounter; });
    // Bindings of textures not drawn for a while (deleted or scrolled away).
    std::erase_if(m_imageBindings, [this](const auto& kv) { return kv.second.lastUsed + 120 < m_frameCounter; });
    m_device.pruneImageTextures();
}

} // namespace AfterglowRender
