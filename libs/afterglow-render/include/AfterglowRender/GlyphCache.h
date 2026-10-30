// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowRender/afterglowrender_export.h>

#include <QImage>
#include <QRect>

#include <cstdint>
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
    bool subpixel = false; ///< RGB coverage in the color atlas, with opaque alpha
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
    const GlyphEntry* glyph(FontSet& fonts, int face, std::uint32_t glyphId, int cellSpan, int scale16 = 16,
                            bool allowSubpixel = true);

    /// An icon (a Nerd Font symbol in the Private Use Area) fitted to
    /// @p cellSpan cells: sized to the line, shrunk if too wide, and centred.
    /// The entry's left is from the start of its cells and its top from the
    /// baseline, as for other glyphs.
    const GlyphEntry* icon(FontSet& fonts, int face, std::uint32_t glyphId, int cellSpan, bool allowSubpixel = true);

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
    bool rasterize(FontSet& fonts, int face, std::uint32_t glyphId, int cellSpan, int scale16, bool allowSubpixel,
                   GlyphEntry& out);
    bool rasterizeIcon(FontSet& fonts, int face, std::uint32_t glyphId, int cellSpan, bool allowSubpixel, GlyphEntry& out);
    bool storeBitmap(const FT_Bitmap& bitmap, const FontSet& fonts, int left, int top, GlyphEntry& out);
    /// Copies an 8-bit coverage bitmap into the gray atlas.
    bool storeGray(const unsigned char* pixels, int width, int height, int pitch, int left, int top, GlyphEntry& out);

    AtlasPage m_gray;
    AtlasPage m_color;
    std::unordered_map<std::uint64_t, GlyphEntry> m_entries;
    /// Separately rasterised grayscale coverage for translucent or post-processed text.
    std::unordered_map<std::uint64_t, GlyphEntry> m_grayFallbackEntries;
    std::uint64_t m_epoch = 1;
};

} // namespace AfterglowRender
