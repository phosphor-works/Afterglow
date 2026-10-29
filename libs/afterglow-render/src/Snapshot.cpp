// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowRender/Snapshot.h>

#include <algorithm>
#include <unordered_map>
#include <utility>

namespace AfterglowRender {

using namespace AfterglowVt;

void PaneSnapshot::capture(const Terminal& t, const Selection* selection, const SearchHighlight& search, int extra)
{
    const Screen& scr = t.screen();
    const Modes& modes = t.modes();
    cols = scr.cols();
    rows = scr.rows();
    // Extra rows come from below the viewport, so only exist while scrolled back.
    extraRows = std::clamp(extra, 0, int(scr.viewportOffset()));
    const int captureRows = rows + extraRows;
    cells.resize(std::size_t(cols) * std::size_t(captureRows));
    graphemes.clear();
    links.clear();
    std::unordered_map<std::uint32_t, std::uint32_t> linkIndex; // style table link -> snapshot index

    Rgb defFg = t.defaultForeground();
    Rgb defBg = t.defaultBackground();
    if (modes.reverseVideo) {
        std::swap(defFg, defBg);
    }
    defaultFg = packRgba(defFg);
    defaultBg = packRgba(defBg);

    auto resolve = [&](Color c, Rgb fallback) -> Rgb {
        return c.isDefault() ? fallback : t.resolve(c, true);
    };

    const bool hasSelection = selection && selection->validFor(scr);
    const Selection::Range range = hasSelection ? selection->range(scr) : Selection::Range{};

    const std::size_t top = scr.viewportTop();
    const int cw = t.cellWidth();
    const int ch = t.cellHeight();
    images = t.graphics().visible(scr, cw, ch);
    // Matches overlapping the viewport, found once instead of per cell.
    std::vector<int> visibleMatches;
    if (search.matches) {
        const std::uint64_t first = scr.rowsDropped() + top;
        const std::uint64_t last = first + std::uint64_t(captureRows) - 1;
        for (int i = 0; i < int(search.matches->size()); ++i) {
            const SearchMatch& m = (*search.matches)[std::size_t(i)];
            if (m.end.row >= first && m.start.row <= last) {
                visibleMatches.push_back(i);
            }
        }
    }
    // Unicode placeholders continue from the placeholder to their left.
    auto colorId = [](Color c) -> std::uint32_t {
        return c.kind() == Color::Kind::Rgb ? (std::uint32_t(c.r()) << 16) | (std::uint32_t(c.g()) << 8) | c.b()
               : c.kind() == Color::Kind::Palette ? c.index()
                                                  : 0;
    };
    for (int y = 0; y < captureRows; ++y) {
        const Row& row = scr.absoluteRow(top + std::size_t(y));
        const std::uint64_t rowId = scr.rowsDropped() + top + std::size_t(y);
        bool prevValid = false;
        std::uint32_t prevImage = 0;
        std::uint32_t prevPlacement = 0;
        int prevRow = 0;
        int prevCol = 0;
        for (int x = 0; x < cols; ++x) {
            const Cell& c = row.cells[std::size_t(x)];
            const Style& st = scr.styles().get(c.styleId());
            SnapshotCell& out = cells[std::size_t(y) * std::size_t(cols) + std::size_t(x)];

            Rgb fg = resolve(st.fg, defFg);
            Rgb bg = resolve(st.bg, defBg);
            bool bgDefault = st.bg.isDefault();
            if (st.has(Attr::Inverse)) {
                std::swap(fg, bg);
                bgDefault = false;
            }
            std::uint32_t highlight = 0;
            for (int i : visibleMatches) {
                if (contains((*search.matches)[std::size_t(i)], rowId, x)) {
                    highlight = i == search.current ? search.currentColor : search.matchColor;
                    break;
                }
            }
            if (hasSelection && range.contains(rowId, x)) {
                std::swap(fg, bg);
                bgDefault = false;
            }
            std::uint8_t fgAlpha = 255;
            if (st.has(Attr::Faint)) {
                fgAlpha = 128;
            }
            if (st.has(Attr::Invisible)) {
                fgAlpha = 0;
            }

            out.cp = c.isEmpty() ? 0 : c.codepoint();
            out.fg = packRgba(fg, fgAlpha);
            out.bg = bgDefault ? 0 : packRgba(bg);
            if (highlight != 0) {
                // Matches read as dark text on the highlight colour.
                out.bg = highlight;
                out.fg = packRgba(0x10, 0x10, 0x10);
            }
            out.underlineColor = st.underlineColor.isDefault() ? out.fg : packRgba(resolve(st.underlineColor, fg));
            out.attrs = st.attrs;
            out.underline = st.underline;
            out.width = std::uint8_t(c.width());
            out.link = 0;
            if (st.link != 0) {
                auto [it, fresh] = linkIndex.try_emplace(st.link, 0);
                if (fresh) {
                    links.emplace_back(scr.styles().linkUri(st.link));
                    it->second = std::uint32_t(links.size());
                }
                out.link = it->second;
            }
            out.grapheme = 0;
            if (c.codepoint() == PlaceholderCodepoint && !c.isEmpty()) {
                // An image slice, not a glyph.
                const std::u32string_view extra = scr.graphemeExtra(row, x);
                std::uint32_t imageId = colorId(st.fg);
                const std::uint32_t placementId = colorId(st.underlineColor);
                const int r = extra.size() > 0 ? placeholderDiacriticValue(extra[0]) : -1;
                const int cIdx = extra.size() > 1 ? placeholderDiacriticValue(extra[1]) : -1;
                const int high = extra.size() > 2 ? placeholderDiacriticValue(extra[2]) : -1;
                const bool continues = prevValid && (imageId & 0xFFFFFF) == (prevImage & 0xFFFFFF)
                    && placementId == prevPlacement;
                if (high >= 0) {
                    imageId |= std::uint32_t(high) << 24;
                } else if (continues) {
                    imageId = prevImage;
                }
                const int cellRow = r >= 0 ? r : (continues ? prevRow : 0);
                const int cellCol = cIdx >= 0 ? cIdx : (continues ? prevCol + 1 : 0);
                if (auto slice = t.graphics().placeholderSlice(imageId, placementId, cellRow, cellCol, x * cw, y * ch,
                                                               cw, ch)) {
                    images.push_back(std::move(*slice));
                }
                prevValid = true;
                prevImage = imageId;
                prevPlacement = placementId;
                prevRow = cellRow;
                prevCol = cellCol;
                out.cp = 0;
                continue;
            }
            prevValid = false;
            if (c.hasGrapheme()) {
                const std::u32string_view extra = scr.graphemeExtra(row, x);
                if (!extra.empty()) {
                    graphemes.emplace_back(extra);
                    out.grapheme = std::uint32_t(graphemes.size());
                }
            }
        }
    }

    cursorVisible = modes.cursorVisible && scr.viewportOffset() == 0;
    cursorX = scr.cursor.x;
    cursorY = scr.cursor.y;
    cursorShape = t.cursorShape();
    cursorBlink = modes.cursorBlink;
    cursorColor = t.cursorColor() ? packRgba(*t.cursorColor()) : defaultFg;
    generation = t.generation();
    for (int i = 0; i < 256; ++i) {
        palette[std::size_t(i)] = packRgba(t.paletteColor(i));
    }
    images = t.graphics().visible(scr, t.cellWidth(), t.cellHeight());
}

} // namespace AfterglowRender
