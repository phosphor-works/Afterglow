// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowRender/afterglowrender_export.h>

#include <QImage>
#include <QRect>

#include <chrono>
#include <cstdint>
#include <memory>
#include <limits>
#include <unordered_map>
#include <vector>

typedef struct FT_Bitmap_ FT_Bitmap;

namespace AfterglowRender {

class FontSet;

/// One atlas texture's CPU side: a shelf packer over an image that grows by
/// doubling, keeping existing glyph coordinates valid.
class AFTERGLOWRENDER_EXPORT AtlasPage
{
public:
    /// A square page of @p initialSize that grows to @p maxSize.
    AtlasPage(QImage::Format format, int initialSize, int maxSize);
    /// A page of exactly @p size that never grows, for one entry too large
    /// for a shared page.
    AtlasPage(QImage::Format format, QSize size);

    /// Reserves a w x h region (plus padding). Returns false when the page is
    /// at its maximum size and full.
    bool allocate(int w, int h, int& x, int& y);

    QImage& image() noexcept { return m_image; }
    const QImage& image() const noexcept { return m_image; }
    int size() const noexcept { return m_image.width(); }

    /// Regions changed since the last takeDirty(); empty when nothing
    /// changed. Kept apart so an upload covers the new glyphs, not the
    /// rectangle around every shelf they landed on.
    std::vector<QRect> takeDirty();
    void markDirty(const QRect& r);

private:
    struct Shelf
    {
        int y;
        int height;
        int x;
    };
    bool grow();

    QImage::Format m_format;
    int m_maxSize;
    QImage m_image;
    std::vector<Shelf> m_shelves;
    int m_nextY = 0;
    std::vector<QRect> m_dirty;
};

struct GlyphEntry
{
    bool color = false;
    bool empty = true; ///< nothing to draw (space, zero-size bitmap)
    std::int16_t left = 0; ///< from the pen position
    std::int16_t top = 0; ///< from the baseline, upwards
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t u = 0; ///< atlas position in pixels
    std::uint16_t v = 0;
    std::uint32_t page = 0;
    bool subpixel = false; ///< RGB coverage in the color atlas, with opaque alpha
};

/// Rasterised glyphs, icons and sprites, keyed by face, glyph (or character),
/// cell span, scale and coverage mode (grayscale entries for subpixel fonts
/// are kept apart).
///
/// Lookups never return null. An entry the atlas cannot hold (wider or taller
/// than the GPU allows, or scaled to nothing) is empty: nothing is drawn.
class AFTERGLOWRENDER_EXPORT GlyphCache
{
public:
    GlyphCache();

    /// Looks up or rasterises a glyph. @p cellSpan is the cluster's width in
    /// cells, used to fit colour bitmaps. Allocates additional pages as needed; allocation failures throw.
    /// @p scale16 is the size in sixteenths of the base size (16 is 1:1),
    /// for scaled text (OSC 66, double-size lines).
    const GlyphEntry* glyph(FontSet& fonts, int face, std::uint32_t glyphId, int cellSpan, int scale16 = 16,
                            bool allowSubpixel = true);

    /// An icon (a Nerd Font symbol in the Private Use Area) fitted to
    /// @p cellSpan cells: sized to the line, shrunk if too wide, and centred.
    /// The entry's left is from the start of its cells and its top from the
    /// baseline, as for other glyphs.
    const GlyphEntry* icon(FontSet& fonts, int face, std::uint32_t glyphId, int cellSpan, bool allowSubpixel = true);

    /// A procedurally drawn character (see Sprites.h), @p sx by @p sy cells
    /// large, allocating atlas pages as needed. The entry's top is the
    /// baseline, so it lands on the cell's top edge.
    const GlyphEntry* sprite(FontSet& fonts, char32_t cp, int sx = 1, int sy = 1);

    void reset();
    /// Bumped by reset(): glyph entries from before are gone.
    std::uint64_t epoch() const noexcept { return m_epoch; }

    AtlasPage& grayPage(std::size_t page = 0) noexcept { return *m_gray[page]; }
    AtlasPage& colorPage(std::size_t page = 0) noexcept { return *m_color[page]; }
    std::size_t grayPageCount() const noexcept { return m_gray.size(); }
    std::size_t colorPageCount() const noexcept { return m_color.size(); }
    /// Called once per render of any window that shares the cache. Over the
    /// byte budget, a page no render has used for StaleAfter discards the
    /// whole cache (every page and entry) and bumps epoch().
    void beginFrame(std::size_t glyphBound, int textureLimit);
    void touchPage(bool color, std::size_t page);
    std::size_t bytes() const noexcept;
    static constexpr std::chrono::milliseconds StaleAfter{1000};

private:
    void rasterize(FontSet& fonts, int face, std::uint32_t glyphId, int cellSpan, int scale16, bool allowSubpixel,
                   GlyphEntry& out);
    void rasterizeIcon(FontSet& fonts, int face, std::uint32_t glyphId, int cellSpan, bool allowSubpixel, GlyphEntry& out);
    /// Stores FreeType coverage; @p scale resizes it first (bitmap-only fonts
    /// at a scaled size).
    void storeBitmap(const FT_Bitmap& bitmap, const FontSet& fonts, int left, int top, GlyphEntry& out, float scale = 1.0f);
    /// Copies an 8-bit coverage bitmap into the gray atlas.
    void storeGray(const unsigned char* pixels, int width, int height, int pitch, int left, int top, GlyphEntry& out);
    /// Whether one entry of @p width by @p height fits on a GPU texture.
    bool fits(int width, int height) const noexcept { return width < m_textureLimit && height < m_textureLimit; }

    AtlasPage& allocate(bool color, int width, int height, int& x, int& y, std::uint32_t& page);
    std::vector<std::unique_ptr<AtlasPage>> m_gray;
    std::vector<std::unique_ptr<AtlasPage>> m_color;
    std::vector<std::uint64_t> m_grayUsed, m_colorUsed;
    /// When each page was last used by a finished render.
    std::vector<std::chrono::steady_clock::time_point> m_grayUsedAt, m_colorUsedAt;
    std::uint64_t m_frame = 0;
    std::size_t m_pageBound = std::numeric_limits<std::size_t>::max();
    int m_textureLimit = 4096;
    std::unordered_map<std::uint64_t, GlyphEntry> m_entries;
    /// Separately rasterised grayscale coverage for translucent or post-processed text.
    std::unordered_map<std::uint64_t, GlyphEntry> m_grayFallbackEntries;
    std::uint64_t m_epoch = 1;
};

} // namespace AfterglowRender
