// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowRender/FontSet.h>
#include <AfterglowRender/GlyphCache.h>

#include "Sprites.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_BITMAP_H

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
    m_gray.reset();
    m_color.reset();
}

const GlyphEntry* GlyphCache::glyph(FontSet& fonts, int face, std::uint32_t glyphId, int cellSpan, int scale16)
{
    scale16 = std::clamp(scale16, 1, 255);
    const std::uint64_t key = (std::uint64_t(scale16 == 16 ? 0 : scale16) << 56) | (std::uint64_t(face & 0xFFFF) << 40)
        | (std::uint64_t(cellSpan & 0xFF) << 32) | glyphId;
    if (auto it = m_entries.find(key); it != m_entries.end()) {
        return &it->second;
    }
    GlyphEntry entry;
    if (!rasterize(fonts, face, glyphId, cellSpan, scale16, entry)) {
        return nullptr;
    }
    return &m_entries.emplace(key, entry).first->second;
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

bool GlyphCache::rasterize(FontSet& fonts, int faceIdx, std::uint32_t glyphId, int cellSpan, int scale16, GlyphEntry& out)
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
    FT_Int32 flags = FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT;
    if (face.color) {
        flags |= FT_LOAD_COLOR;
    }
    if (FT_Load_Glyph(ft, glyphId, flags) != 0) {
        return true; // missing glyph: draw nothing
    }
    FT_GlyphSlot slot = ft->glyph;
    if (slot->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(slot, FT_RENDER_MODE_LIGHT) != 0) {
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

    // Grayscale (convert mono or other depths first).
    FT_Bitmap converted;
    FT_Bitmap_Init(&converted);
    const FT_Bitmap* gray = &bm;
    if (bm.pixel_mode != FT_PIXEL_MODE_GRAY) {
        FT_Library lib = ft->glyph->library;
        if (FT_Bitmap_Convert(lib, &bm, &converted, 1) != 0) {
            return true;
        }
        if (converted.num_grays == 2) {
            for (unsigned i = 0; i < converted.rows * unsigned(std::abs(converted.pitch)); ++i) {
                converted.buffer[i] = converted.buffer[i] ? 255 : 0;
            }
        }
        gray = &converted;
    }

    const int w = int(gray->width);
    const int h = int(gray->rows);
    int x = 0;
    int y = 0;
    const bool ok = m_gray.allocate(w, h, x, y);
    if (ok) {
        QImage& img = m_gray.image();
        for (int row = 0; row < h; ++row) {
            std::memcpy(img.scanLine(y + row) + x, gray->buffer + std::ptrdiff_t(row) * gray->pitch, std::size_t(w));
        }
        m_gray.markDirty(QRect(x, y, w, h));
        out = GlyphEntry{false, false, std::int16_t(slot->bitmap_left), std::int16_t(slot->bitmap_top),
                         std::uint16_t(w), std::uint16_t(h), std::uint16_t(x), std::uint16_t(y)};
    }
    if (gray == &converted) {
        FT_Bitmap_Done(ft->glyph->library, &converted);
    }
    return ok;
}

} // namespace AfterglowRender
