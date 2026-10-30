// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <AfterglowRender/Snapshot.h>
#include <fribidi.h>
#include <algorithm>
#include <numeric>

namespace AfterglowRender {

int PaneSnapshot::visualColumn(int logical, int row) const
{
    if (logicalToVisual.empty() || logical < 0 || logical >= cols || row < 0 || row >= rows + extraRows) return logical;
    return logicalToVisual[std::size_t(row) * cols + logical];
}

int PaneSnapshot::logicalColumn(int visual, int row) const
{
    if (logicalToVisual.empty() || visual < 0 || visual >= cols || row < 0 || row >= rows + extraRows) return visual;
    return at(visual, row).logicalColumn;
}

void PaneSnapshot::applyBidi(const AfterglowVt::Terminal& terminal)
{
    using namespace AfterglowVt;
    const auto mode = terminal.options().bidi;
    if (mode == BidiMode::Off || terminal.modes().altScreen
        || terminal.modes().mouseTracking != MouseTracking::None || cols <= 0) return;
    const auto& screen = terminal.screen();
    const auto top = screen.viewportTop();
    const auto bottom = top + std::size_t(rows + extraRows);
    // Bound paragraph work independently of retained scrollback. An oversized
    // paragraph keeps its application grid, including all of its visible rows.
    constexpr std::size_t MaxCharacters = 65536;
    const std::size_t maxLines = std::max(std::size_t(1), MaxCharacters / std::size_t(cols));
    logicalToVisual.resize(cells.size());
    for (std::size_t i = 0; i < cells.size(); ++i) logicalToVisual[i] = int(i % std::size_t(cols));
    struct Group { int column, width; FriBidiStrIndex first; };
    struct Line { std::size_t row; FriBidiStrIndex first, length; std::vector<Group> groups; };
    for (std::size_t visible = top; visible < bottom;) {
        auto start = visible;
        while (start > 0 && screen.absoluteRow(start - 1).wrapped && visible - start < maxLines) --start;
        auto end = visible + 1;
        while (end < screen.totalRows() && screen.absoluteRow(end - 1).wrapped && end - start <= maxLines) ++end;
        if ((start && screen.absoluteRow(start - 1).wrapped) || end - start > maxLines) {
            // Every subsequent captured row in this connected paragraph is
            // also left in logical order, without scanning unbounded history.
            while (visible < bottom && screen.absoluteRow(visible).wrapped) ++visible;
            if (visible < bottom) ++visible;
            continue;
        }
        std::vector<FriBidiChar> characters;
        std::vector<Line> lines;
        bool grid = false;
        for (auto rowIndex = start; rowIndex < end && !grid; ++rowIndex) {
            const auto& row = screen.absoluteRow(rowIndex);
            if (row.lineSize != LineSize::Single) { grid = true; break; }
            const bool captured = rowIndex >= top && rowIndex < bottom;
            const int sy = captured ? int(rowIndex - top) : -1;
            if (captured && std::any_of(images.begin(), images.end(), [&](const auto& image) {
                return image.y < (sy + 1) * terminal.cellHeight()
                    && image.y + image.height > sy * terminal.cellHeight();
            })) { grid = true; break; }
            Line line{rowIndex, FriBidiStrIndex(characters.size()), 0, {}};
            for (int col = 0; col < cols;) {
                const auto& source = row.at(col);
                const auto* cell = captured ? &at(col, sy) : nullptr;
                if ((cell && cell->mcScale) || source.isMulticell()) { grid = true; break; }
                const int width = std::clamp(cell ? int(cell->width) : source.width(), 1, cols - col);
                const char32_t cp = cell ? cell->cp : source.isEmpty() ? 0 : source.codepoint();
                line.groups.push_back({col, width, FriBidiStrIndex(characters.size())});
                characters.push_back(cp ? FriBidiChar(cp) : FriBidiChar(' '));
                const std::u32string_view extra = cell
                    ? (cell->grapheme ? std::u32string_view(graphemes[cell->grapheme - 1]) : std::u32string_view{})
                    : screen.graphemeExtra(row, col);
                for (char32_t value : extra) characters.push_back(FriBidiChar(value));
                if (characters.size() > MaxCharacters) { grid = true; break; }
                col += width;
            }
            line.length = FriBidiStrIndex(characters.size()) - line.first;
            lines.push_back(std::move(line));
        }
        visible = std::min(end, bottom);
        if (grid || characters.empty()) continue;
        const auto count = FriBidiStrIndex(characters.size());
        std::vector<FriBidiCharType> types(characters.size());
        std::vector<FriBidiBracketType> brackets(characters.size());
        std::vector<FriBidiLevel> levels(characters.size());
        fribidi_get_bidi_types(characters.data(), count, types.data());
        fribidi_get_bracket_types(characters.data(), count, types.data(), brackets.data());
        FriBidiParType direction = mode == BidiMode::LeftToRight ? FRIBIDI_PAR_LTR
            : mode == BidiMode::RightToLeft ? FRIBIDI_PAR_RTL : FRIBIDI_PAR_ON;
        if (!fribidi_get_par_embedding_levels_ex(types.data(), brackets.data(), count, &direction, levels.data())) continue;
        std::vector<FriBidiStrIndex> map(characters.size());
        std::iota(map.begin(), map.end(), 0);
        for (const auto& line : lines) {
            if (line.row < top || line.row >= bottom) continue;
            if (!fribidi_reorder_line(0, types.data(), line.length, line.first, direction, levels.data(), nullptr, map.data())) continue;
            const auto offset = (line.row - top) * std::size_t(cols);
            std::vector<SnapshotCell> reordered;
            reordered.reserve(std::size_t(cols));
            std::vector<bool> used(line.groups.size());
            for (auto i = line.first; i < line.first + line.length; ++i) {
                const auto logical = map[std::size_t(i)];
                const auto it = std::upper_bound(line.groups.begin(), line.groups.end(), logical,
                    [](auto index, const Group& group) { return index < group.first; });
                if (it == line.groups.begin()) continue;
                const auto groupIndex = std::size_t(it - line.groups.begin() - 1);
                if (used[groupIndex]) continue;
                used[groupIndex] = true;
                const auto& group = line.groups[groupIndex];
                for (int dx = 0; dx < group.width; ++dx) {
                    auto cell = cells[offset + group.column + dx];
                    cell.logicalColumn = group.column + dx;
                    cell.bidiLevel = levels[std::size_t(group.first)];
                    logicalToVisual[offset + group.column + dx] = int(reordered.size());
                    reordered.push_back(cell);
                }
            }
            if (reordered.size() == std::size_t(cols)) std::copy(reordered.begin(), reordered.end(), cells.begin() + offset);
        }
    }
    cursorX = visualColumn(cursorX, cursorY);
    for (auto& cursor : extraCursors) cursor.x = visualColumn(cursor.x, cursor.y);
}
}
