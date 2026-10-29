// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowRender/Snapshot.h>

#include <utility>

namespace AfterglowRender {

using namespace AfterglowVt;

void PaneSnapshot::capture(const Terminal& t, const Selection* selection, const SearchHighlight& search)
{
    const Screen& scr = t.screen();
    const Modes& modes = t.modes();
    cols = scr.cols();
    rows = scr.rows();
    cells.resize(std::size_t(cols) * std::size_t(rows));
    graphemes.clear();

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
    // Matches overlapping the viewport, found once instead of per cell.
    std::vector<int> visibleMatches;
    if (search.matches) {
        const std::uint64_t first = scr.rowsDropped() + top;
        const std::uint64_t last = first + std::uint64_t(rows) - 1;
        for (int i = 0; i < int(search.matches->size()); ++i) {
            const SearchMatch& m = (*search.matches)[std::size_t(i)];
            if (m.end.row >= first && m.start.row <= last) {
                visibleMatches.push_back(i);
            }
        }
    }
    for (int y = 0; y < rows; ++y) {
        const Row& row = scr.absoluteRow(top + std::size_t(y));
        const std::uint64_t rowId = scr.rowsDropped() + top + std::size_t(y);
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
            out.grapheme = 0;
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
    images = t.graphics().visible(scr, t.cellWidth(), t.cellHeight());
}

} // namespace AfterglowRender
