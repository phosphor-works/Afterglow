// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowRender/FontSet.h>
#include <AfterglowRender/GlyphCache.h>

#include "Sprites.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <QPainter>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace AfterglowRender {

namespace {
constexpr int Padding = 1;
}

// --- AtlasPage -------------------------------------------------------------

AtlasPage::AtlasPage(QImage::Format format, int initialSize, int maxSize)
    : m_format(format)
    , m_maxSize(maxSize)
    , m_image(initialSize, initialSize, format)
{
    m_image.fill(0);
}

bool AtlasPage::allocate(int w, int h, int& x, int& y)
{
    const int pw = w + Padding;
    const int ph = h + Padding;
    for (;;) {
        Shelf* best = nullptr;
        for (Shelf& s : m_shelves) {
            if (s.height >= ph && s.height <= ph + ph / 3 + 2 && s.x + pw <= m_image.width()) {
                if (!best || s.height < best->height) {
                    best = &s;
                }
            }
        }
        if (best) {
            x = best->x;
            y = best->y;
            best->x += pw;
            return true;
        }
        if (m_nextY + ph <= m_image.height() && pw <= m_image.width()) {
            m_shelves.push_back({m_nextY, ph, pw});
            x = 0;
            y = m_nextY;
            m_nextY += ph;
            return true;
        }
        if (!grow()) {
            return false;
        }
    }
}

bool AtlasPage::grow()
{
    if (m_image.width() >= m_maxSize) {
        return false;
    }
    const int size = m_image.width() * 2;
    QImage bigger(size, size, m_format);
    bigger.fill(0);
    for (int row = 0; row < m_image.height(); ++row) {
        std::memcpy(bigger.scanLine(row), m_image.constScanLine(row), std::size_t(m_image.bytesPerLine()));
    }
    m_image = std::move(bigger);
    m_dirty = m_image.rect();
    ++m_layoutVersion;
    return true;
}

void AtlasPage::reset()
{
    m_image.fill(0);
    m_shelves.clear();
    m_nextY = 0;
    m_dirty = m_image.rect();
    ++m_layoutVersion;
}

QRect AtlasPage::takeDirty()
{
    const QRect r = m_dirty;
    m_dirty = QRect();
    return r;
}

// --- GlyphCache ------------------------------------------------------------

GlyphCache::GlyphCache()
    : m_gray(QImage::Format_Grayscale8, 1024, 4096)
    , m_color(QImage::Format_ARGB32_Premultiplied, 512, 4096)
{
}

void GlyphCache::reset()
{
    ++m_epoch;
    m_entries.clear();
    m_grayFallbackEntries.clear();
    m_gray.reset();
    m_color.reset();
}

const GlyphEntry* GlyphCache::glyph(FontSet& fonts, int face, std::uint32_t glyphId, int cellSpan, int scale16, bool allowSubpixel)
{
    scale16 = std::clamp(scale16, 1, 255);
    auto& entries = !allowSubpixel && fonts.subpixel() ? m_grayFallbackEntries : m_entries;
    const std::uint64_t key = (std::uint64_t(scale16 == 16 ? 0 : scale16) << 56) | (std::uint64_t(face & 0xFFFF) << 40)
        | (std::uint64_t(cellSpan & 0xFF) << 32) | glyphId;
    if (auto it = entries.find(key); it != entries.end()) {
        return &it->second;
    }
    GlyphEntry entry;
    if (!rasterize(fonts, face, glyphId, cellSpan, scale16, allowSubpixel, entry)) {
        return nullptr;
    }
    return &entries.emplace(key, entry).first->second;
}

const GlyphEntry* GlyphCache::icon(FontSet& fonts, int face, std::uint32_t glyphId, int cellSpan, bool allowSubpixel)
{
    // The span's top bit (no text cluster is 128 cells wide) keeps icons
    // apart from text glyphs.
    auto& entries = !allowSubpixel && fonts.subpixel() ? m_grayFallbackEntries : m_entries;
    const std::uint64_t key = (std::uint64_t(face & 0xFFFF) << 40) | (std::uint64_t((cellSpan & 0x7F) | 0x80) << 32) | glyphId;
    if (auto it = entries.find(key); it != entries.end()) {
        return &it->second;
    }
    GlyphEntry entry;
    if (!rasterizeIcon(fonts, face, glyphId, cellSpan, allowSubpixel, entry)) {
        return nullptr;
    }
    return &entries.emplace(key, entry).first->second;
}

bool GlyphCache::rasterizeIcon(FontSet& fonts, int faceIdx, std::uint32_t glyphId, int cellSpan, bool allowSubpixel, GlyphEntry& out)
{
    const Face& face = fonts.face(faceIdx);
    if (!face.scalable || face.color) {
        return rasterize(fonts, faceIdx, glyphId, cellSpan, 16, allowSubpixel, out);
    }
    FT_Face ft = face.ft;
    const FontMetrics& m = fonts.metrics();
    if (FT_Load_Glyph(ft, glyphId, FT_LOAD_NO_HINTING) != 0) {
        return true; // missing glyph: draw nothing
    }
    const float gw = float(ft->glyph->metrics.width) / 64.0f;
    const float gh = float(ft->glyph->metrics.height) / 64.0f;
    if (gw <= 0 || gh <= 0) {
        return true;
    }
    // The icon font's line maps onto our cell, so icons keep their sizes
    // relative to each other whichever Nerd Font supplies them; only one too
    // wide for its cells shrinks further.
    const float line = float(ft->size->metrics.ascender - ft->size->metrics.descender) / 64.0f;
    const float boxW = float(m.cellWidth * std::max(1, cellSpan));
    const float boxH = float(m.cellHeight);
    const float pad = std::max(1.0f, float(m.cellWidth) * 0.1f);
    float scale = line > 0 ? boxH * 0.9f / line : 1.0f;
    scale = std::min({scale, (boxW - pad) / gw, boxH * 0.95f / gh});
    const float base = fonts.pixelSize();
    FT_Set_Char_Size(ft, 0, FT_F26Dot6(std::lround(base * scale * 64.0f)), 72, 72);
    bool ok = true;
    if (FT_Load_Glyph(ft, glyphId, fonts.loadFlags()) == 0
        && FT_Render_Glyph(ft->glyph, FT_Render_Mode(fonts.renderMode(allowSubpixel))) == 0) {
        const FT_Bitmap& bm = ft->glyph->bitmap;
        if (bm.width > 0 && bm.rows > 0) {
            const int w = int(bm.width) / (bm.pixel_mode == FT_PIXEL_MODE_LCD ? 3 : 1);
            const int h = int(bm.rows);
            const int left = int(std::lround((boxW - float(w)) / 2.0f));
            const int top = m.baseline - int(std::lround((boxH - float(h)) / 2.0f));
            ok = storeBitmap(bm, fonts, left, top, out);
        }
    }
    FT_Set_Char_Size(ft, 0, FT_F26Dot6(std::lround(base * 64.0f)), 72, 72);
    return ok;
}

bool GlyphCache::storeGray(const unsigned char* pixels, int width, int height, int pitch, int left, int top,
                           GlyphEntry& out)
{
    int x = 0;
    int y = 0;
    if (!m_gray.allocate(width, height, x, y)) {
        return false;
    }
    QImage& img = m_gray.image();
    for (int row = 0; row < height; ++row) {
        std::memcpy(img.scanLine(y + row) + x, pixels + std::ptrdiff_t(row) * pitch, std::size_t(width));
    }
    m_gray.markDirty(QRect(x, y, width, height));
    out = GlyphEntry{false, false, std::int16_t(left), std::int16_t(top), std::uint16_t(width), std::uint16_t(height),
                     std::uint16_t(x), std::uint16_t(y)};
    return true;
}

const GlyphEntry* GlyphCache::sprite(FontSet& fonts, char32_t cp, int sx, int sy)
{
    constexpr std::uint64_t SpriteFace = 0xFFFF;
    sx = std::clamp(sx, 1, 7);
    sy = std::clamp(sy, 1, 7);
    const std::uint64_t key = (std::uint64_t((sx - 1) * 8 + (sy - 1)) << 56) | (SpriteFace << 40) | std::uint64_t(cp);
    if (auto it = m_entries.find(key); it != m_entries.end()) {
        return &it->second;
    }
    const FontMetrics& m = fonts.metrics();
    QImage canvas(m.cellWidth * sx, m.cellHeight * sy, QImage::Format_Alpha8);
    if (!Sprites::draw(cp, canvas, m.underlineThickness * std::min(sx, sy))) {
        return &m_entries.emplace(key, GlyphEntry{}).first->second;
    }
    int x = 0;
    int y = 0;
    if (!m_gray.allocate(canvas.width(), canvas.height(), x, y)) {
        return nullptr;
    }
    QImage& img = m_gray.image();
    for (int row = 0; row < canvas.height(); ++row) {
        std::memcpy(img.scanLine(y + row) + x, canvas.constScanLine(row), std::size_t(canvas.width()));
    }
    m_gray.markDirty(QRect(x, y, canvas.width(), canvas.height()));
    const GlyphEntry entry{false, false, 0, std::int16_t(m.baseline * sy), std::uint16_t(canvas.width()),
                           std::uint16_t(canvas.height()), std::uint16_t(x), std::uint16_t(y)};
    return &m_entries.emplace(key, entry).first->second;
}

bool GlyphCache::rasterize(FontSet& fonts, int faceIdx, std::uint32_t glyphId, int cellSpan, int scale16, bool allowSubpixel, GlyphEntry& out)
{
    const Face& face = fonts.face(faceIdx);
    FT_Face ft = face.ft;
    // Scaled text: rasterise at the larger size, then back to the base size
    // (HarfBuzz reads advances from the face at its current size).
    struct SizeGuard
    {
        FT_Face ft;
        float base;
        bool active;
        ~SizeGuard()
        {
            if (active) {
                FT_Set_Char_Size(ft, 0, FT_F26Dot6(std::lround(base * 64.0f)), 72, 72);
            }
        }
    } guard{ft, fonts.pixelSize(), scale16 != 16 && face.scalable};
    if (guard.active) {
        FT_Set_Char_Size(ft, 0, FT_F26Dot6(std::lround(fonts.pixelSize() * float(scale16) / 16.0f * 64.0f)), 72, 72);
    }
    const float scale = float(scale16) / 16.0f;
    FT_Int32 flags = fonts.loadFlags();
    if (face.color) {
        flags |= FT_LOAD_COLOR;
    }
    if (FT_Load_Glyph(ft, glyphId, flags) != 0) {
        return true; // missing glyph: draw nothing
    }
    FT_GlyphSlot slot = ft->glyph;
    // Color outline renderers need normal mode; LCD/MONO are coverage modes
    // for uncolored outlines. Embedded color strikes already bypass rendering.
    const auto mode = face.color ? FT_RENDER_MODE_NORMAL : FT_Render_Mode(fonts.renderMode(allowSubpixel));
    if (slot->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(slot, mode) != 0) {
        return true;
    }
    const FT_Bitmap& bm = slot->bitmap;
    if (bm.width == 0 || bm.rows == 0) {
        return true;
    }

    const FontMetrics& m = fonts.metrics();

    if (bm.pixel_mode == FT_PIXEL_MODE_BGRA) {
        QImage src(bm.buffer, int(bm.width), int(bm.rows), bm.pitch, QImage::Format_ARGB32_Premultiplied);
        int left = slot->bitmap_left;
        int top = slot->bitmap_top;
        const int boxW = int(std::lround(float(m.cellWidth * std::max(1, cellSpan)) * scale));
        const int boxH = int(std::lround(float(m.cellHeight) * scale));
        QImage img = src;
        if (!face.scalable || src.width() > boxW || src.height() > boxH) {
            img = src.scaled(boxW, boxH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            left = (boxW - img.width()) / 2;
            top = int(std::lround(float(m.baseline) * scale)) - (boxH - img.height()) / 2;
        } else {
            img = src.copy();
        }
        int x = 0;
        int y = 0;
        if (!m_color.allocate(img.width(), img.height(), x, y)) {
            return false;
        }
        QPainter p(&m_color.image());
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(x, y, img);
        p.end();
        m_color.markDirty(QRect(x, y, img.width(), img.height()));
        out = GlyphEntry{true, false, std::int16_t(left), std::int16_t(top), std::uint16_t(img.width()),
                         std::uint16_t(img.height()), std::uint16_t(x), std::uint16_t(y)};
        return true;
    }

    return storeBitmap(bm, fonts, slot->bitmap_left, slot->bitmap_top, out);
}

bool GlyphCache::storeBitmap(const FT_Bitmap& bm, const FontSet& fonts, int left, int top, GlyphEntry& out)
{
    const int w = int(bm.width) / (bm.pixel_mode == FT_PIXEL_MODE_LCD ? 3 : 1);
    const int h = int(bm.rows);
    if (w == 0 || h == 0) {
        return true;
    }
    if (bm.pixel_mode == FT_PIXEL_MODE_LCD) {
        int x = 0, y = 0;
        if (!m_color.allocate(w, h, x, y)) {
            return false;
        }
        const bool bgr = fonts.spec().antialiasing == Antialiasing::Bgr;
        for (int row = 0; row < h; ++row) {
            const auto* src = bm.buffer + std::ptrdiff_t(row) * bm.pitch;
            auto* dst = reinterpret_cast<QRgb*>(m_color.image().scanLine(y + row)) + x;
            for (int col = 0; col < w; ++col) {
                dst[col] = qRgb(src[col * 3 + (bgr ? 2 : 0)], src[col * 3 + 1], src[col * 3 + (bgr ? 0 : 2)]);
            }
        }
        m_color.markDirty(QRect(x, y, w, h));
        out = GlyphEntry{false, false, std::int16_t(left), std::int16_t(top), std::uint16_t(w), std::uint16_t(h),
                         std::uint16_t(x), std::uint16_t(y), true};
        return true;
    }
    if (bm.pixel_mode == FT_PIXEL_MODE_GRAY && bm.num_grays == 256
        && fonts.spec().antialiasing != Antialiasing::None) {
        return storeGray(bm.buffer, w, h, bm.pitch, left, top, out);
    }
    // Unpack embedded monochrome/low-depth strikes as well as outline MONO
    // output. AA-off thresholds gray strikes too; color emoji stay untouched.
    QImage mask(w, h, QImage::Format_Grayscale8);
    for (int row = 0; row < h; ++row) {
        const auto* src = bm.buffer + std::ptrdiff_t(row) * bm.pitch;
        auto* dst = mask.scanLine(row);
        for (int col = 0; col < w; ++col) {
            int coverage = 0;
            switch (bm.pixel_mode) {
            case FT_PIXEL_MODE_MONO: coverage = (src[col / 8] & (0x80 >> (col % 8))) ? 255 : 0; break;
            case FT_PIXEL_MODE_GRAY2: coverage = ((src[col / 4] >> (6 - 2 * (col % 4))) & 3) * 85; break;
            case FT_PIXEL_MODE_GRAY4: coverage = ((src[col / 2] >> (4 - 4 * (col % 2))) & 15) * 17; break;
            case FT_PIXEL_MODE_GRAY: coverage = int(src[col]) * 255 / std::max(1, int(bm.num_grays) - 1); break;
            default: return true;
            }
            dst[col] = fonts.spec().antialiasing == Antialiasing::None ? (coverage >= 128 ? 255 : 0) : coverage;
        }
    }
    return storeGray(mask.constBits(), w, h, mask.bytesPerLine(), left, top, out);
}

} // namespace AfterglowRender
