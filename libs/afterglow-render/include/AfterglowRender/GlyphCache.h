// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowRender/afterglowrender_export.h>

#include <QImage>
#include <QRect>

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace AfterglowRender {

class FontSet;

/// One atlas texture's CPU side: a shelf packer over an image that grows by
/// doubling, keeping existing glyph coordinates valid.
class AFTERGLOWRENDER_EXPORT AtlasPage
{
public:
    AtlasPage(QImage::Format format, int initialSize, int maxSize);

    /// Reserves a w x h region (plus padding). Returns false when the page is
    /// at its maximum size and full.
    bool allocate(int w, int h, int& x, int& y);
    void reset();

    QImage& image() noexcept { return m_image; }
    const QImage& image() const noexcept { return m_image; }
    int size() const noexcept { return m_image.width(); }

    /// Region changed since the last takeDirty(); empty when nothing changed.
    QRect takeDirty();
    void markDirty(const QRect& r) { m_dirty = m_dirty.united(r); }
    /// Bumped when the image is reallocated (grown or reset): the GPU texture
    /// must be recreated and fully uploaded.
    std::uint64_t layoutVersion() const noexcept { return m_layoutVersion; }

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
    QRect m_dirty;
    std::uint64_t m_layoutVersion = 1;
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
};

/// Rasterised glyphs keyed by (face, glyph id, cell span).
class AFTERGLOWRENDER_EXPORT GlyphCache
{
public:
    GlyphCache();

    /// Looks up or rasterises a glyph. @p cellSpan is the cluster's width in
    /// cells, used to fit colour bitmaps. Returns nullptr if both atlases are
    /// full; the caller must then call reset() and rebuild its frame.
    /// @p scale16 is the size in sixteenths of the base size (16 is 1:1),
    /// for scaled text (OSC 66, double-size lines).
    const GlyphEntry* glyph(FontSet& fonts, int face, std::uint32_t glyphId, int cellSpan, int scale16 = 16);

    /// A procedurally drawn cell (see Sprites.h), or nullptr if the atlas is full.
    /// The entry's top is the baseline, so it lands on the cell's top edge.
    /// A procedurally drawn character, @p sx by @p sy cells large.
    const GlyphEntry* sprite(FontSet& fonts, char32_t cp, int sx = 1, int sy = 1);

    void reset();
    /// Bumped by reset(): glyph entries from before are gone.
    std::uint64_t epoch() const noexcept { return m_epoch; }

    AtlasPage& grayPage() noexcept { return m_gray; }
    AtlasPage& colorPage() noexcept { return m_color; }

private:
    bool rasterize(FontSet& fonts, int face, std::uint32_t glyphId, int cellSpan, int scale16, GlyphEntry& out);

    AtlasPage m_gray;
    AtlasPage m_color;
    std::unordered_map<std::uint64_t, GlyphEntry> m_entries;
    std::uint64_t m_epoch = 1;
};

} // namespace AfterglowRender
