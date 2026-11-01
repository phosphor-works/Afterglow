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
#include <stdexcept>
#include <utility>

namespace AfterglowRender {

namespace {
constexpr int Padding = 1;
/// More dirty rectangles than this on one page merge with their nearest one.
constexpr std::size_t MaxDirtyRects = 32;

/// Sets @p ft to @p scale times the base size for as long as it lives, then
/// back to the base size (HarfBuzz reads advances from the face at its
/// current size), also when storing the glyph throws.
struct SizeGuard
{
    SizeGuard(FT_Face face, float base, float scale, bool active)
        : ft(face), base(base), active(active)
    {
        if (active) FT_Set_Char_Size(ft, 0, FT_F26Dot6(std::lround(base * scale * 64.0f)), 72, 72);
    }
    ~SizeGuard()
    {
        if (active) FT_Set_Char_Size(ft, 0, FT_F26Dot6(std::lround(base * 64.0f)), 72, 72);
    }
    SizeGuard(const SizeGuard&) = delete;
    SizeGuard& operator=(const SizeGuard&) = delete;
    FT_Face ft;
    float base;
    bool active;
};
}

// --- AtlasPage -------------------------------------------------------------

AtlasPage::AtlasPage(QImage::Format format, int initialSize, int maxSize)
    : m_format(format)
    , m_maxSize(maxSize)
    , m_image(initialSize, initialSize, format)
{
    if (m_image.isNull()) throw std::bad_alloc();
    m_image.fill(0);
}

AtlasPage::AtlasPage(QImage::Format format, QSize size)
    : m_format(format)
    , m_maxSize(0)
    , m_image(size, format)
{
    if (m_image.isNull()) throw std::bad_alloc();
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
    const int size = std::min(m_maxSize, m_image.width() * 2);
    QImage bigger(size, size, m_format);
    if (bigger.isNull()) throw std::bad_alloc();
    bigger.fill(0);
    for (int row = 0; row < m_image.height(); ++row) {
        std::memcpy(bigger.scanLine(row), m_image.constScanLine(row), std::size_t(m_image.bytesPerLine()));
    }
    m_image = std::move(bigger);
    m_dirty.assign(1, m_image.rect());
    return true;
}

void AtlasPage::markDirty(const QRect& r)
{
    // A region that covers others (the whole page after a texture is made
    // again) replaces them, so no pixel uploads twice.
    std::erase_if(m_dirty, [&r](const QRect& dirty) { return r.contains(dirty); });
    for (QRect& dirty : m_dirty) {
        if (dirty.contains(r)) return;
        // Glyphs on one shelf share its top edge and sit a padding apart,
        // with different heights: one rectangle covers the shelf's new ones.
        // The columns between them, uploaded before, are cheap to send again.
        if (dirty.y() == r.y()) {
            dirty = dirty.united(r);
            return;
        }
    }
    if (m_dirty.size() < MaxDirtyRects) {
        m_dirty.push_back(r);
        return;
    }
    // Full: grow the rectangle that grows least, not one box around every shelf.
    const auto added = [&r](const QRect& dirty) {
        const QRect united = dirty.united(r);
        return std::int64_t(united.width()) * united.height() - std::int64_t(dirty.width()) * dirty.height();
    };
    auto nearest = std::min_element(m_dirty.begin(), m_dirty.end(),
                                    [&](const QRect& a, const QRect& b) { return added(a) < added(b); });
    *nearest = nearest->united(r);
}

std::vector<QRect> AtlasPage::takeDirty()
{
    return std::exchange(m_dirty, {});
}

// --- GlyphCache ------------------------------------------------------------

GlyphCache::GlyphCache() { reset(); }

void GlyphCache::reset()
{
    ++m_epoch;
    m_entries.clear();
    m_grayFallbackEntries.clear();
    m_gray.clear(); m_color.clear();
    m_gray.emplace_back(std::make_unique<AtlasPage>(QImage::Format_Grayscale8, 1024, 4096));
    m_color.emplace_back(std::make_unique<AtlasPage>(QImage::Format_ARGB32_Premultiplied, 512, 4096));
    m_grayUsed.assign(1, m_frame); m_colorUsed.assign(1, m_frame);
    m_grayUsedAt.assign(1, m_now); m_colorUsedAt.assign(1, m_now);
}

std::size_t GlyphCache::bytes() const noexcept
{
    std::size_t result = 0;
    for (const auto& page : m_gray) result += page->image().sizeInBytes();
    for (const auto& page : m_color) result += page->image().sizeInBytes();
    return result;
}

void GlyphCache::beginFrame(std::size_t glyphBound, int textureLimit, std::chrono::steady_clock::time_point now)
{
    // Staleness is measured in time, not in renders: every window at this
    // pixel ratio shares the cache, so one window's pages go unused for many
    // renders of the others while that window is still on screen.
    m_now = now;
    for (std::size_t i = 0; i < m_grayUsed.size(); ++i) if (m_grayUsed[i] == m_frame) m_grayUsedAt[i] = now;
    for (std::size_t i = 0; i < m_colorUsed.size(); ++i) if (m_colorUsed[i] == m_frame) m_colorUsedAt[i] = now;
    ++m_frame;
    m_grayUsed.front() = m_colorUsed.front() = m_frame;
    m_grayUsedAt.front() = m_colorUsedAt.front() = now;
    // Over budget, a page that no render has used for a while discards the
    // whole cache, every page and entry; the working set is rasterised again.
    const auto stale = [now](auto last) { return now - last > StaleAfter; };
    if (bytes() > 64u * 1024u * 1024u
        && (std::any_of(m_grayUsedAt.begin(), m_grayUsedAt.end(), stale)
            || std::any_of(m_colorUsedAt.begin(), m_colorUsedAt.end(), stale))) reset();
    m_pageBound = m_gray.size() + m_color.size() + glyphBound;
    m_textureLimit = textureLimit;
}

void GlyphCache::touchPage(bool color, std::size_t page)
{
    (color ? m_colorUsed : m_grayUsed).at(page) = m_frame;
}

AtlasPage& GlyphCache::allocate(bool color, int width, int height, int& x, int& y, std::uint32_t& index)
{
    auto& pages = color ? m_color : m_gray;
    for (std::size_t i = 0; i < pages.size(); ++i) {
        if (pages[i]->allocate(width, height, x, y)) {
            index = std::uint32_t(i); touchPage(color, i); return *pages[i];
        }
    }
    // Callers check fits() first: an entry the GPU cannot hold is drawn as nothing.
    if (!fits(width, height) || m_gray.size() + m_color.size() >= m_pageBound)
        throw std::runtime_error("glyph atlas exceeds the device or viewport working set");
    const auto format = color ? QImage::Format_ARGB32_Premultiplied : QImage::Format_Grayscale8;
    if (std::max(width, height) + Padding > 4096) {
        // One entry larger than a shared page (a big multicell sprite): a page
        // of its own size, not a square of its longer side.
        pages.emplace_back(std::make_unique<AtlasPage>(format, QSize(width + Padding, height + Padding)));
    } else {
        const int initial = std::max(512, std::max(width, height) + Padding);
        pages.emplace_back(std::make_unique<AtlasPage>(format, initial, std::min(4096, m_textureLimit)));
    }
    (color ? m_colorUsed : m_grayUsed).push_back(m_frame);
    (color ? m_colorUsedAt : m_grayUsedAt).push_back(m_now);
    index = std::uint32_t(pages.size() - 1);
    if (!pages.back()->allocate(width, height, x, y)) throw std::runtime_error("glyph atlas allocation failed");
    return *pages.back();
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
    rasterize(fonts, face, glyphId, cellSpan, scale16, allowSubpixel, entry);
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
    rasterizeIcon(fonts, face, glyphId, cellSpan, allowSubpixel, entry);
    return &entries.emplace(key, entry).first->second;
}

void GlyphCache::rasterizeIcon(FontSet& fonts, int faceIdx, std::uint32_t glyphId, int cellSpan, bool allowSubpixel, GlyphEntry& out)
{
    const Face& face = fonts.face(faceIdx);
    if (!face.scalable || face.color) {
        rasterize(fonts, faceIdx, glyphId, cellSpan, 16, allowSubpixel, out);
        return;
    }
    FT_Face ft = face.ft;
    const FontMetrics& m = fonts.metrics();
    if (FT_Load_Glyph(ft, glyphId, FT_LOAD_NO_HINTING) != 0) {
        return; // missing glyph: draw nothing
    }
    const float gw = float(ft->glyph->metrics.width) / 64.0f;
    const float gh = float(ft->glyph->metrics.height) / 64.0f;
    if (gw <= 0 || gh <= 0) {
        return;
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
    const SizeGuard guard(ft, fonts.pixelSize(), scale, true);
    if (FT_Load_Glyph(ft, glyphId, fonts.loadFlags()) == 0
        && FT_Render_Glyph(ft->glyph, FT_Render_Mode(fonts.renderMode(allowSubpixel))) == 0) {
        const FT_Bitmap& bm = ft->glyph->bitmap;
        if (bm.width > 0 && bm.rows > 0) {
            const int w = int(bm.width) / (bm.pixel_mode == FT_PIXEL_MODE_LCD ? 3 : 1);
            const int h = int(bm.rows);
            const int left = int(std::lround((boxW - float(w)) / 2.0f));
            const int top = m.baseline - int(std::lround((boxH - float(h)) / 2.0f));
            storeBitmap(bm, fonts, left, top, out);
        }
    }
}

void GlyphCache::storeGray(const unsigned char* pixels, int width, int height, int pitch, int left, int top,
                           GlyphEntry& out)
{
    if (!fits(width, height)) {
        out = GlyphEntry{};
        return;
    }
    int x = 0;
    int y = 0;
    std::uint32_t pageIndex = 0;
    auto& atlas = allocate(false, width, height, x, y, pageIndex);
    QImage& img = atlas.image();
    for (int row = 0; row < height; ++row) {
        std::memcpy(img.scanLine(y + row) + x, pixels + std::ptrdiff_t(row) * pitch, std::size_t(width));
    }
    atlas.markDirty(QRect(x, y, width, height));
    out = GlyphEntry{false, false, std::int16_t(left), std::int16_t(top), std::uint16_t(width), std::uint16_t(height),
                     std::uint16_t(x), std::uint16_t(y), pageIndex};
}

const GlyphEntry* GlyphCache::sprite(FontSet& fonts, char32_t cp, int sx, int sy)
{
    constexpr std::uint64_t SpriteFace = 0xFFFF;
    sx = std::clamp(sx, 1, 49);
    sy = std::clamp(sy, 1, 7);
    const std::uint64_t key = (std::uint64_t(sx - 1) << 32) | (std::uint64_t(sy - 1) << 56) | (SpriteFace << 40) | std::uint64_t(cp);
    if (auto it = m_entries.find(key); it != m_entries.end()) {
        return &it->second;
    }
    const FontMetrics& m = fonts.metrics();
    GlyphEntry entry;
    // A canvas the GPU cannot hold (a 7x7 multicell block of a wide
    // character at a large size) draws nothing.
    if (!fits(m.cellWidth * sx, m.cellHeight * sy)) {
        return &m_entries.emplace(key, entry).first->second;
    }
    QImage canvas(m.cellWidth * sx, m.cellHeight * sy, QImage::Format_Alpha8);
    if (canvas.isNull()) throw std::bad_alloc();
    if (Sprites::draw(cp, canvas, m.underlineThickness * std::min(sx, sy))) {
        storeGray(canvas.constBits(), canvas.width(), canvas.height(), int(canvas.bytesPerLine()), 0, m.baseline * sy, entry);
    }
    return &m_entries.emplace(key, entry).first->second;
}

void GlyphCache::rasterize(FontSet& fonts, int faceIdx, std::uint32_t glyphId, int cellSpan, int scale16, bool allowSubpixel, GlyphEntry& out)
{
    const Face& face = fonts.face(faceIdx);
    FT_Face ft = face.ft;
    // Scaled text: rasterise at the larger size, then back to the base size.
    const float scale = float(scale16) / 16.0f;
    const SizeGuard guard(ft, fonts.pixelSize(), scale, scale16 != 16 && face.scalable);
    FT_Int32 flags = fonts.loadFlags();
    if (face.color) {
        flags |= FT_LOAD_COLOR;
    }
    if (FT_Load_Glyph(ft, glyphId, flags) != 0) {
        return; // missing glyph: draw nothing
    }
    FT_GlyphSlot slot = ft->glyph;
    // Color outline renderers need normal mode; LCD/MONO are coverage modes
    // for uncolored outlines. Embedded color strikes already bypass rendering.
    const auto mode = face.color ? FT_RENDER_MODE_NORMAL : FT_Render_Mode(fonts.renderMode(allowSubpixel));
    if (slot->format != FT_GLYPH_FORMAT_BITMAP && FT_Render_Glyph(slot, mode) != 0) {
        return;
    }
    const FT_Bitmap& bm = slot->bitmap;
    if (bm.width == 0 || bm.rows == 0) {
        return;
    }

    const FontMetrics& m = fonts.metrics();

    if (bm.pixel_mode == FT_PIXEL_MODE_BGRA) {
        QImage src(bm.buffer, int(bm.width), int(bm.rows), bm.pitch, QImage::Format_ARGB32_Premultiplied);
        int left = slot->bitmap_left;
        int top = slot->bitmap_top;
        // At least one pixel: a tiny OSC 66 fraction in a small cell rounds
        // the box to nothing.
        const int boxW = std::max(1, int(std::lround(float(m.cellWidth * std::max(1, cellSpan)) * scale)));
        const int boxH = std::max(1, int(std::lround(float(m.cellHeight) * scale)));
        QImage img;
        if (!face.scalable || src.width() > boxW || src.height() > boxH) {
            // Scaling can still give an empty or null image (an aspect ratio
            // that truncates one side to 0): nothing to draw.
            img = src.scaled(boxW, boxH, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            if (img.isNull() || img.width() == 0 || img.height() == 0) return;
            left = (boxW - img.width()) / 2;
            top = int(std::lround(float(m.baseline) * scale)) - (boxH - img.height()) / 2;
        } else {
            img = src.copy();
            if (img.isNull()) throw std::bad_alloc();
        }
        if (!fits(img.width(), img.height())) return;
        int x = 0;
        int y = 0;
        std::uint32_t pageIndex = 0;
        auto& atlas = allocate(true, img.width(), img.height(), x, y, pageIndex);
        QPainter p(&atlas.image());
        p.setCompositionMode(QPainter::CompositionMode_Source);
        p.drawImage(x, y, img);
        p.end();
        atlas.markDirty(QRect(x, y, img.width(), img.height()));
        out = GlyphEntry{true, false, std::int16_t(left), std::int16_t(top), std::uint16_t(img.width()),
                         std::uint16_t(img.height()), std::uint16_t(x), std::uint16_t(y), pageIndex};
        return;
    }

    // A bitmap-only font cannot rasterise at the scaled size: its strike is
    // scaled instead, keeping its pixels sharp.
    storeBitmap(bm, fonts, slot->bitmap_left, slot->bitmap_top, out, face.scalable ? 1.0f : scale);
}

void GlyphCache::storeBitmap(const FT_Bitmap& bm, const FontSet& fonts, int left, int top, GlyphEntry& out, float scale)
{
    const int w = int(bm.width) / (bm.pixel_mode == FT_PIXEL_MODE_LCD ? 3 : 1);
    const int h = int(bm.rows);
    if (w == 0 || h == 0) {
        return;
    }
    if (bm.pixel_mode == FT_PIXEL_MODE_LCD) {
        if (!fits(w, h)) return;
        int x = 0, y = 0;
        std::uint32_t pageIndex = 0;
        auto& atlas = allocate(true, w, h, x, y, pageIndex);
        const bool bgr = fonts.spec().antialiasing == Antialiasing::Bgr;
        for (int row = 0; row < h; ++row) {
            const auto* src = bm.buffer + std::ptrdiff_t(row) * bm.pitch;
            auto* dst = reinterpret_cast<QRgb*>(atlas.image().scanLine(y + row)) + x;
            for (int col = 0; col < w; ++col) {
                dst[col] = qRgb(src[col * 3 + (bgr ? 2 : 0)], src[col * 3 + 1], src[col * 3 + (bgr ? 0 : 2)]);
            }
        }
        atlas.markDirty(QRect(x, y, w, h));
        out = GlyphEntry{false, false, std::int16_t(left), std::int16_t(top), std::uint16_t(w), std::uint16_t(h),
                         std::uint16_t(x), std::uint16_t(y), pageIndex, true};
        return;
    }
    if (bm.pixel_mode == FT_PIXEL_MODE_GRAY && bm.num_grays == 256
        && fonts.spec().antialiasing != Antialiasing::None && scale == 1.0f) {
        storeGray(bm.buffer, w, h, bm.pitch, left, top, out);
        return;
    }
    // Unpack embedded monochrome/low-depth strikes as well as outline MONO
    // output. AA-off thresholds gray strikes too; color emoji stay untouched.
    QImage mask(w, h, QImage::Format_Grayscale8);
    if (mask.isNull()) throw std::bad_alloc();
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
            default: return;
            }
            dst[col] = fonts.spec().antialiasing == Antialiasing::None ? (coverage >= 128 ? 255 : 0) : coverage;
        }
    }
    if (scale != 1.0f) {
        const int sw = std::max(1, int(std::lround(float(w) * scale)));
        const int sh = std::max(1, int(std::lround(float(h) * scale)));
        mask = mask.scaled(sw, sh, Qt::IgnoreAspectRatio, Qt::FastTransformation);
        if (mask.isNull()) throw std::bad_alloc();
        left = int(std::lround(float(left) * scale));
        top = int(std::lround(float(top) * scale));
    }
    storeGray(mask.constBits(), mask.width(), mask.height(), int(mask.bytesPerLine()), left, top, out);
}

} // namespace AfterglowRender
