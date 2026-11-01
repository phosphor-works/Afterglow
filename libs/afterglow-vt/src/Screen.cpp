// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Screen.h>

#include <algorithm>
#include <vector>

namespace AfterglowVt {

namespace {
constexpr std::size_t MaxGraphemeExtra = 32;

bool isBlank(const Cell& c)
{
    return c.isEmpty() && c.styleId() == StyleTable::DefaultId && !c.hasGrapheme() && c.width() == 1;
}

bool rowIsBlank(const Row& row)
{
    return std::all_of(row.cells.begin(), row.cells.end(), isBlank);
}

// A blank cell that a compact row does not store. A spacer head is kept:
// reflow must not read its column as a blank cell of the line.
bool isDroppableBlank(const Cell& c)
{
    return isBlank(c) && !c.isSpacerHead();
}
} // namespace

Screen::Screen(int cols, int rows, std::size_t maxScrollback)
    : m_cols(std::clamp(cols, 2, 4096))
    , m_rowCount(std::clamp(rows, 1, 1048576 / m_cols))
    , m_maxScrollback(maxScrollback)
{
    for (int y = 0; y < m_rowCount; ++y) {
        m_rows.emplace_back(m_cols);
    }
}

Screen::~Screen() = default;

void Screen::setPen(const Style& style)
{
    const std::uint32_t id = m_styles.acquire(style);
    m_styles.release(cursor.penId);
    cursor.pen = style;
    cursor.penId = id;
}

// --- cell primitives -------------------------------------------------------

void Screen::clearCell(Row& row, int x, std::uint32_t fillStyleId)
{
    Cell& c = row.cells[std::size_t(x)];
    if (c.hasGrapheme() && row.graphemes) {
        row.graphemes->erase(std::uint16_t(x));
    }
    m_styles.release(c.styleId());
    c = Cell{};
    c.setStyleId(fillStyleId);
    m_styles.retain(fillStyleId);
}

void Screen::breakWideAt(Row& row, int x, std::uint32_t fillStyleId)
{
    if (x < 0 || x >= m_cols) {
        return;
    }
    const Cell& c = row.cells[std::size_t(x)];
    if (c.isTail() && x > 0) {
        clearCell(row, x - 1, fillStyleId);
        clearCell(row, x, fillStyleId);
    } else if (c.width() == 2 && x + 1 < m_cols) {
        clearCell(row, x + 1, fillStyleId);
        clearCell(row, x, fillStyleId);
    }
}

void Screen::writeCell(Row& row, int x, char32_t cp, std::uint32_t styleId, int width)
{
    if (row.cells[std::size_t(x)].isMulticell()) {
        eraseMulticellAt(row, x);
    }
    if (width == 2 && x + 1 < m_cols && row.cells[std::size_t(x + 1)].isMulticell()) {
        eraseMulticellAt(row, x + 1);
    }
    breakWideAt(row, x, StyleTable::DefaultId);
    if (width == 2) {
        breakWideAt(row, x + 1, StyleTable::DefaultId);
    }

    Cell& c = row.cells[std::size_t(x)];
    if (c.hasGrapheme() && row.graphemes) {
        row.graphemes->erase(std::uint16_t(x));
    }
    m_styles.release(c.styleId());
    c = Cell{};
    c.setCodepoint(cp);
    c.setStyleId(styleId);
    c.setWidth(width);
    m_styles.retain(styleId);

    if (width == 2 && x + 1 < m_cols) {
        clearCell(row, x + 1, styleId);
        Cell& tail = row.cells[std::size_t(x + 1)];
        tail.setWidth(0);
    }
}

void Screen::appendGrapheme(Row& row, int x, char32_t cp)
{
    if (!row.graphemes) {
        row.graphemes = std::make_unique<GraphemeMap>();
    }
    std::u32string& extra = (*row.graphemes)[std::uint16_t(x)];
    if (extra.size() < MaxGraphemeExtra) {
        extra.push_back(cp);
    }
    row.cells[std::size_t(x)].setHasGrapheme(true);
}

std::u32string_view Screen::graphemeExtra(const Row& row, int x) const
{
    if (!row.at(x).hasGrapheme() || !row.graphemes) {
        return {};
    }
    auto it = row.graphemes->find(std::uint16_t(x));
    return it == row.graphemes->end() ? std::u32string_view{} : std::u32string_view(it->second);
}

void Screen::eraseCells(Row& row, int x0, int x1, std::uint32_t fillStyleId)
{
    x0 = std::clamp(x0, 0, m_cols);
    x1 = std::clamp(x1, 0, m_cols);
    if (x0 >= x1) {
        return;
    }
    if (row.multicell) {
        // Any block the erase touches goes whole.
        for (int x = x0; x < x1; ++x) {
            if (row.cells[std::size_t(x)].isMulticell()) {
                eraseMulticellAt(row, x);
            }
        }
    }
    breakWideAt(row, x0, fillStyleId);
    if (x1 < m_cols && row.cells[std::size_t(x1)].isTail()) {
        clearCell(row, x1, fillStyleId);
    }
    // Bulk: release what was there, then take the fill style once for the run.
    Cell blank;
    blank.setStyleId(fillStyleId);
    for (int x = x0; x < x1; ++x) {
        Cell& c = row.cells[std::size_t(x)];
        if (c.styleId() != StyleTable::DefaultId) {
            m_styles.release(c.styleId());
        }
        c = blank;
    }
    m_styles.retain(fillStyleId, std::uint32_t(x1 - x0));
    if (row.graphemes) {
        if (x0 == 0 && x1 == m_cols) {
            row.graphemes.reset();
        } else {
            for (int x = x0; x < x1; ++x) {
                row.graphemes->erase(std::uint16_t(x));
            }
        }
    }
}

void Screen::eraseRow(Row& row, std::uint32_t fillStyleId)
{
    eraseCells(row, 0, m_cols, fillStyleId);
    row.graphemes.reset();
    row.multicell.reset();
    row.wrapped = false;
    row.promptStart = false;
    row.lineSize = LineSize::Single;
}

int Screen::visibleIndexOf(const Row& r) const noexcept
{
    for (int y = 0; y < m_rowCount; ++y) {
        if (&row(y) == &r) {
            return y;
        }
    }
    return -1;
}

const MulticellParams* Screen::multicellParams(const Row& r, int x) const
{
    const Cell& c = r.at(x);
    if (!c.isMulticell() || !r.multicell) {
        return nullptr;
    }
    const auto it = r.multicell->find(std::uint16_t(x - c.multicellDx()));
    return it == r.multicell->end() ? nullptr : &it->second;
}

void Screen::writeMulticell(int y, int x, char32_t cp, std::u32string_view extra, std::uint32_t styleId,
                            const MulticellParams& params)
{
    const int cols = params.columns();
    for (int dy = 0; dy < params.scale; ++dy) {
        Row& r = row(y + dy);
        for (int dx = 0; dx < cols; ++dx) {
            writeCell(r, x + dx, cp, styleId, 1); // erases blocks in the way
        }
    }
    // A block's text (kitty's w=N renders a whole string in N cells) may use
    // what is left of its rows' grapheme budget: as much as every cell of a
    // row could hold one by one. Each row keeps a copy, so the tightest row
    // sets the length, and scrollback stays bounded by rows x cols.
    std::size_t keep = extra.size();
    for (int dy = 0; dy < params.scale && keep > MaxGraphemeExtra; ++dy) {
        const Row& r = row(y + dy);
        std::size_t used = 0;
        if (r.graphemes) {
            for (const auto& [col, text] : *r.graphemes) {
                used += text.size();
            }
        }
        const std::size_t budget = std::size_t(m_cols) * MaxGraphemeExtra;
        keep = std::min(keep, std::max(MaxGraphemeExtra, budget > used ? budget - used : 0));
    }
    for (int dy = 0; dy < params.scale; ++dy) {
        Row& r = row(y + dy);
        for (int dx = 0; dx < cols; ++dx) {
            r.cells[std::size_t(x + dx)].setMulticell(dx, dy);
        }
        // Every row of the block knows its text and parameters, so the part
        // still on screen draws when the top has scrolled away.
        if (!r.multicell) {
            r.multicell = std::make_unique<MulticellMap>();
        }
        (*r.multicell)[std::uint16_t(x)] = params;
        if (!extra.empty()) {
            if (!r.graphemes) r.graphemes = std::make_unique<GraphemeMap>();
            (*r.graphemes)[std::uint16_t(x)] = std::u32string(extra.substr(0, keep));
            r.cells[std::size_t(x)].setHasGrapheme(true);
        }
    }
}

void Screen::eraseMulticell(int y, int x, bool spaces)
{
    Row& start = row(y);
    const Cell c = start.cells[std::size_t(x)];
    if (!c.isMulticell()) {
        return;
    }
    const int left = x - c.multicellDx();
    const int top = y - c.multicellDy();
    int rows = 0;
    int cols = 0;
    if (const MulticellParams* p = multicellParams(start, x)) {
        rows = p->scale;
        cols = p->columns();
    } else {
        // The parameters are lost: measure the block by its cells, down from
        // its top row and right from its left column.
        int yy = std::max(top, 0);
        for (; yy < m_rowCount; ++yy) {
            const Cell& below = row(yy).at(left);
            if (!below.isMulticell() || below.multicellDx() != 0 || below.multicellDy() != yy - top) {
                break;
            }
        }
        rows = yy - top;
        while (left + cols < m_cols) {
            const Cell& next = start.at(left + cols);
            if (!next.isMulticell() || next.multicellDx() != cols || next.multicellDy() != c.multicellDy()) {
                break;
            }
            ++cols;
        }
        rows = std::max(rows, c.multicellDy() + 1);
    }
    for (int dy = 0; dy < rows; ++dy) {
        const int yy = top + dy;
        if (yy < 0 || yy >= m_rowCount) {
            continue;
        }
        Row& r = row(yy);
        for (int dx = 0; dx < cols && left + dx < m_cols; ++dx) {
            Cell& cell = r.cells[std::size_t(left + dx)];
            if (cell.isMulticell() && cell.multicellDx() == dx && cell.multicellDy() == dy) {
                clearCell(r, left + dx, StyleTable::DefaultId);
                if (spaces) {
                    r.cells[std::size_t(left + dx)].setCodepoint(U' ');
                }
            }
        }
        if (r.multicell) {
            r.multicell->erase(std::uint16_t(left));
        }
    }
}

void Screen::eraseMulticellAt(Row& r, int x)
{
    if (const int y = visibleIndexOf(r); y >= 0) {
        eraseMulticell(y, x);
        return;
    }
    eraseMulticellInRow(r, x);
}

void Screen::eraseMulticellInRow(Row& r, int x)
{
    // Only this row's part of the block: the rest is elsewhere, or gone.
    const Cell c = r.cells[std::size_t(x)];
    const int left = x - c.multicellDx();
    const MulticellParams* p = multicellParams(r, x);
    const int cols = p ? p->columns() : c.multicellDx() + 1;
    for (int dx = 0; dx < cols && left + dx < int(r.cells.size()); ++dx) {
        if (r.cells[std::size_t(left + dx)].isMulticell()) {
            clearCell(r, left + dx, StyleTable::DefaultId);
        }
    }
    if (r.multicell) {
        r.multicell->erase(std::uint16_t(left));
    }
}

void Screen::eraseMulticellsCrossing(int top, int bottom, int left, int right, int split)
{
    std::vector<std::pair<int, int>> doomed;
    for (int y = std::max(top, 0); y <= std::min(bottom, m_rowCount - 1); ++y) {
        const Row& r = row(y);
        if (!r.multicell) {
            continue;
        }
        for (const auto& [col, params] : *r.multicell) {
            const Cell& c = r.at(col);
            const int blockTop = y - c.multicellDy();
            const int blockBottom = blockTop + params.scale - 1;
            const int blockRight = col + params.columns() - 1;
            const bool inside = blockTop >= top && blockBottom <= bottom && col >= left && blockRight <= right;
            const bool touches = blockRight >= left && col <= right;
            const bool cut = blockTop < split && blockBottom >= split;
            if (touches && (!inside || cut)) {
                doomed.emplace_back(y, col);
            }
        }
    }
    for (const auto& [y, x] : doomed) {
        eraseMulticell(y, x);
    }
}

void Screen::flattenMulticells()
{
    for (Row& r : m_rows) {
        if (!r.multicell) {
            continue;
        }
        for (std::size_t x = 0; x < r.cells.size(); ++x) {
            Cell& c = r.cells[x];
            if (!c.isMulticell()) {
                continue;
            }
            if (c.multicellDx() == 0 && c.multicellDy() == 0) {
                c.setCodepoint(c.codepoint()); // the text stays, as one plain cell
            } else {
                clearCell(r, int(x), StyleTable::DefaultId);
            }
        }
        r.multicell.reset();
    }
}

namespace {
void moveGrapheme(Row& row, int from, int to)
{
    if (!row.graphemes) {
        return;
    }
    row.graphemes->erase(std::uint16_t(to));
    auto node = row.graphemes->extract(std::uint16_t(from));
    if (!node.empty()) {
        node.key() = std::uint16_t(to);
        row.graphemes->insert(std::move(node));
    }
}
} // namespace

void Screen::insertCells(Row& row, int x, int right, int n, std::uint32_t fillStyleId)
{
    right = std::min(right, m_cols - 1);
    if (x > right || n <= 0) {
        return;
    }
    n = std::min(n, right - x + 1);
    if (row.multicell) {
        for (int i = x; i <= right; ++i) {
            if (row.cells[std::size_t(i)].isMulticell()) {
                eraseMulticellAt(row, i);
            }
        }
    }
    // A glyph that starts at x moves whole; only one that x cuts in half breaks.
    if (row.cells[std::size_t(x)].isTail()) {
        breakWideAt(row, x, fillStyleId);
    }
    breakWideAt(row, right, fillStyleId);

    for (int i = right - n + 1; i <= right; ++i) {
        clearCell(row, i, StyleTable::DefaultId);
    }
    for (int i = right; i >= x + n; --i) {
        m_styles.release(row.cells[std::size_t(i)].styleId());
        row.cells[std::size_t(i)] = row.cells[std::size_t(i - n)];
        m_styles.retain(row.cells[std::size_t(i)].styleId());
        moveGrapheme(row, i - n, i);
    }
    for (int i = x; i < x + n; ++i) {
        Cell& c = row.cells[std::size_t(i)];
        m_styles.release(c.styleId());
        c = Cell{};
        c.setStyleId(fillStyleId);
        m_styles.retain(fillStyleId);
    }
    // A head shifted onto the margin lost its tail.
    if (row.cells[std::size_t(right)].width() == 2) {
        clearCell(row, right, fillStyleId);
    }
}

void Screen::deleteCells(Row& row, int x, int right, int n, std::uint32_t fillStyleId)
{
    right = std::min(right, m_cols - 1);
    if (x > right || n <= 0) {
        return;
    }
    n = std::min(n, right - x + 1);
    if (row.multicell) {
        for (int i = x; i <= right; ++i) {
            if (row.cells[std::size_t(i)].isMulticell()) {
                eraseMulticellAt(row, i);
            }
        }
    }
    breakWideAt(row, x, fillStyleId);
    // The first cell kept moves to x: a glyph starting there moves whole, and
    // only a tail (its head deleted) is left without a partner.
    if (x + n <= right && row.cells[std::size_t(x + n)].isTail()) {
        breakWideAt(row, x + n, fillStyleId);
    }
    // Past the margin only a tail matters: its head inside is about to move.
    if (right + 1 < m_cols && row.cells[std::size_t(right + 1)].isTail()) {
        breakWideAt(row, right + 1, fillStyleId);
    }

    for (int i = x; i < x + n; ++i) {
        clearCell(row, i, StyleTable::DefaultId);
    }
    for (int i = x; i + n <= right; ++i) {
        m_styles.release(row.cells[std::size_t(i)].styleId());
        row.cells[std::size_t(i)] = row.cells[std::size_t(i + n)];
        // A spacer head belongs in the last column only; moved, it is a blank.
        row.cells[std::size_t(i)].setSpacerHead(false);
        m_styles.retain(row.cells[std::size_t(i)].styleId());
        moveGrapheme(row, i + n, i);
    }
    for (int i = right - n + 1; i <= right; ++i) {
        Cell& c = row.cells[std::size_t(i)];
        if (c.hasGrapheme() && row.graphemes) {
            row.graphemes->erase(std::uint16_t(i));
        }
        m_styles.release(c.styleId());
        c = Cell{};
        c.setStyleId(fillStyleId);
        m_styles.retain(fillStyleId);
    }
}

void Screen::copyRowSpan(Row& dst, const Row& src, int left, int right)
{
    breakWideAt(dst, left, StyleTable::DefaultId);
    breakWideAt(dst, right, StyleTable::DefaultId);
    for (int x = left; x <= right; ++x) {
        Cell& d = dst.cells[std::size_t(x)];
        if (d.hasGrapheme() && dst.graphemes) {
            dst.graphemes->erase(std::uint16_t(x));
        }
        m_styles.release(d.styleId());
        d = src.cells[std::size_t(x)];
        // A spacer head marks this row's own wrap; the copy keeps no such mark.
        d.setSpacerHead(false);
        m_styles.retain(d.styleId());
        if (d.hasGrapheme()) {
            const std::u32string_view extra = graphemeExtra(src, x);
            if (!dst.graphemes) {
                dst.graphemes = std::make_unique<GraphemeMap>();
            }
            (*dst.graphemes)[std::uint16_t(x)] = std::u32string(extra);
        }
    }
    if (dst.multicell) {
        std::erase_if(*dst.multicell, [left, right](const auto& kv) { return kv.first >= left && kv.first <= right; });
    }
    if (src.multicell) {
        for (const auto& [col, params] : *src.multicell) {
            if (col >= left && col <= right) {
                if (!dst.multicell) {
                    dst.multicell = std::make_unique<MulticellMap>();
                }
                (*dst.multicell)[col] = params;
            }
        }
    }
    if (dst.cells[std::size_t(left)].isTail()) {
        clearCell(dst, left, StyleTable::DefaultId);
    }
    if (dst.cells[std::size_t(right)].width() == 2 && right + 1 <= m_cols - 1
        && !dst.cells[std::size_t(right + 1)].isTail()) {
        clearCell(dst, right, StyleTable::DefaultId);
    }
}

void Screen::compact(Row& row)
{
    std::size_t keep = row.cells.size();
    while (keep > 0 && isDroppableBlank(row.cells[keep - 1])) {
        --keep;
    }
    if (keep < row.cells.size()) {
        row.cells.resize(keep);
        row.cells.shrink_to_fit();
    }
}

void Screen::expand(Row& row)
{
    if (row.cells.size() < std::size_t(m_cols)) {
        row.cells.reserve(std::size_t(m_cols)); // exactly; growth would overallocate
        row.cells.resize(std::size_t(m_cols));
    }
}

std::size_t Screen::historyCells() const noexcept
{
    std::size_t n = 0;
    for (std::size_t i = 0; i < historySize(); ++i) {
        n += m_rows[i].cells.size();
    }
    return n;
}

void Screen::releaseRow(Row& row) noexcept
{
    for (const Cell& c : row.cells) {
        m_styles.release(c.styleId());
    }
}

// --- scrolling -------------------------------------------------------------

bool Screen::scrollUp(const Region& r, int n, std::uint32_t fillStyleId, bool intoHistory)
{
    const int height = r.bottom - r.top + 1;
    n = std::clamp(n, 0, height);
    if (n == 0) {
        return false;
    }
    const bool fullWidth = r.left == 0 && r.right == m_cols - 1;

    if (intoHistory && fullWidth && r.top == 0 && r.bottom == m_rowCount - 1 && m_maxScrollback > 0) {
        for (int k = 0; k < n; ++k) {
            Row* freshPtr;
            if (historySize() >= m_maxScrollback) {
                // History is full: the oldest row's storage becomes the new line.
                Row recycled = std::move(m_rows.front());
                m_rows.pop_front();
                ++m_rowsDropped;
                releaseRow(recycled);
                recycled.cells.assign(std::size_t(m_cols), Cell{});
                recycled.graphemes.reset();
                recycled.multicell.reset();
                recycled.wrapped = false;
                recycled.promptStart = false;
                recycled.lineSize = LineSize::Single;
                freshPtr = &m_rows.emplace_back(std::move(recycled));
            } else {
                freshPtr = &m_rows.emplace_back(m_cols);
            }
            Row& fresh = *freshPtr;
            if (fillStyleId != StyleTable::DefaultId) {
                for (Cell& c : fresh.cells) {
                    c.setStyleId(fillStyleId);
                    m_styles.retain(fillStyleId);
                }
            }
            // The row that just left the top of the screen is history now.
            compact(m_rows[historySize() - 1]);
        }
        if (m_viewportOffset > 0) {
            m_viewportOffset += std::size_t(n);
        }
        trimHistory();
        return false;
    }

    if (fullWidth && r.top == 0 && r.bottom == m_rowCount - 1 && historySize() == 0) {
        // The whole screen with no history (the alternate screen): the top
        // rows leave and come back cleared at the bottom, rather than every
        // row moving up one. Nothing can cross the screen's own edges, and
        // the row ids stay with their content, so images scroll with it.
        for (int k = 0; k < n; ++k) {
            // Blocks leave with the row like rows that go into history: the
            // part below stays. Erased here, at the top, the row cannot reach
            // into the rows it lands next to at the bottom.
            Row& leaving = m_rows.front();
            if (leaving.multicell) {
                for (int x = 0; x < m_cols; ++x) {
                    if (leaving.cells[std::size_t(x)].isMulticell()) {
                        eraseMulticellInRow(leaving, x);
                    }
                }
            }
            m_rows.push_back(std::move(m_rows.front()));
            m_rows.pop_front();
            ++m_rowsDropped;
            eraseRow(m_rows.back(), fillStyleId);
        }
        return false;
    }

    // Blocks the region boundary cuts cannot move with it, and neither can
    // those the scroll splits between the rows leaving and the rows staying:
    // a part left behind would point outside the region.
    eraseMulticellsCrossing(r.top, r.bottom, r.left, r.right, r.top + n);
    if (fullWidth) {
        auto first = m_rows.begin() + std::ptrdiff_t(historySize()) + r.top;
        std::rotate(first, first + n, first + height);
        for (int y = r.bottom - n + 1; y <= r.bottom; ++y) {
            eraseRow(row(y), fillStyleId);
        }
        return true;
    }

    for (int y = r.top; y + n <= r.bottom; ++y) {
        copyRowSpan(row(y), row(y + n), r.left, r.right);
    }
    for (int y = r.bottom - n + 1; y <= r.bottom; ++y) {
        eraseCells(row(y), r.left, r.right + 1, fillStyleId);
    }
    return true;
}

bool Screen::scrollDown(const Region& r, int n, std::uint32_t fillStyleId)
{
    const int height = r.bottom - r.top + 1;
    n = std::clamp(n, 0, height);
    if (n == 0) {
        return false;
    }
    const bool fullWidth = r.left == 0 && r.right == m_cols - 1;

    eraseMulticellsCrossing(r.top, r.bottom, r.left, r.right, r.bottom - n + 1);
    if (fullWidth) {
        auto first = m_rows.begin() + std::ptrdiff_t(historySize()) + r.top;
        std::rotate(first, first + (height - n), first + height);
        for (int y = r.top; y < r.top + n; ++y) {
            eraseRow(row(y), fillStyleId);
        }
        return true;
    }

    for (int y = r.bottom; y - n >= r.top; --y) {
        copyRowSpan(row(y), row(y - n), r.left, r.right);
    }
    for (int y = r.top; y < r.top + n; ++y) {
        eraseCells(row(y), r.left, r.right + 1, fillStyleId);
    }
    return true;
}

void Screen::trimHistory()
{
    while (historySize() > m_maxScrollback) {
        releaseRow(m_rows.front());
        m_rows.pop_front();
        ++m_rowsDropped;
    }
    m_viewportOffset = std::min(m_viewportOffset, historySize());
}

void Screen::clearHistory()
{
    while (historySize() > 0) {
        releaseRow(m_rows.front());
        m_rows.pop_front();
        ++m_rowsDropped;
    }
    m_viewportOffset = 0;
}

void Screen::setMaxScrollback(std::size_t rows)
{
    m_maxScrollback = rows;
    trimHistory();
}

void Screen::scrollViewport(long delta) noexcept
{
    const long next = long(m_viewportOffset) + delta;
    m_viewportOffset = std::size_t(std::clamp(next, 0L, long(historySize())));
}

// --- resize ----------------------------------------------------------------

void Screen::resize(int cols, int rows, bool reflow, std::optional<std::size_t> promptStart,
                    std::vector<std::size_t>* trackedRows, std::vector<int>* trackedColumns)
{
    cols = std::clamp(cols, 2, 4096);
    rows = std::clamp(rows, 1, 1048576 / cols);
    if (trackedColumns && (!trackedRows || trackedColumns->size() != trackedRows->size())) trackedColumns = nullptr;
    if (cols == m_cols && rows == m_rowCount) {
        return;
    }
    if (cols != m_cols) {
        // Only a width change can move content between rows. A height change
        // keeps every stable id on its row, so selections and searches stay.
        ++m_layoutEpoch;
    }
    if (reflow && cols != m_cols) {
        // The saved cursor and the top of a scrolled-back viewport follow
        // their cells like the caller's positions; they go last in the list.
        std::vector<std::size_t> rowsTracked = trackedRows ? *trackedRows : std::vector<std::size_t>{};
        std::vector<int> columnsTracked = trackedColumns ? *trackedColumns : std::vector<int>(rowsTracked.size(), 0);
        const std::size_t own = rowsTracked.size();
        rowsTracked.push_back(hasSavedCursor ? historySize() + std::size_t(std::clamp(savedCursor.y, 0, m_rowCount - 1))
                                             : TrackedRowGone);
        // A pending wrap sits after the last character, not on it.
        columnsTracked.push_back(std::clamp(savedCursor.x, 0, m_cols - 1) + (savedCursor.pendingWrap ? 1 : 0));
        rowsTracked.push_back(m_viewportOffset > 0 ? viewportTop() : TrackedRowGone);
        columnsTracked.push_back(0);

        flattenMulticells(); // blocks cannot reflow; their text can
        resizeReflow(cols, rows, promptStart, &rowsTracked, &columnsTracked);

        if (const std::size_t top = rowsTracked[own + 1]; top != TrackedRowGone) {
            m_viewportOffset = top < historySize() ? historySize() - top : 0;
        }
        if (const std::size_t saved = rowsTracked[own]; saved != TrackedRowGone) {
            savedCursor.y = saved < historySize() ? 0 : int(saved - historySize());
            const int col = columnsTracked[own];
            savedCursor.pendingWrap = savedCursor.pendingWrap && col >= cols;
            savedCursor.x = std::min(col, cols - 1);
        } else {
            savedCursor.pendingWrap = false;
        }
        if (trackedRows) {
            rowsTracked.resize(own);
            *trackedRows = std::move(rowsTracked);
        }
        if (trackedColumns) {
            columnsTracked.resize(own);
            *trackedColumns = std::move(columnsTracked);
        }
    } else {
        resizeNoReflow(cols, rows, trackedRows, trackedColumns);
    }
    cursor.x = std::clamp(cursor.x, 0, m_cols - 1);
    cursor.y = std::clamp(cursor.y, 0, m_rowCount - 1);
    savedCursor.x = std::clamp(savedCursor.x, 0, m_cols - 1);
    savedCursor.y = std::clamp(savedCursor.y, 0, m_rowCount - 1);
}

void Screen::truncateRow(Row& row, int cols)
{
    if (row.cells.size() <= std::size_t(cols)) {
        return;
    }
    for (std::size_t x = std::size_t(cols); x < row.cells.size(); ++x) {
        clearCell(row, int(x), StyleTable::DefaultId);
    }
    row.cells.resize(std::size_t(cols));
    if (row.cells.back().width() == 2) {
        // The wide glyph's tail was clipped; discard its head and grapheme too.
        clearCell(row, cols - 1, StyleTable::DefaultId);
    }
}

void Screen::resizeNoReflow(int cols, int rows, std::vector<std::size_t>* tracked, std::vector<int>* trackedColumns)
{
    const std::uint64_t droppedBefore = m_rowsDropped;
    if (cols != m_cols) {
        // The cursor keeps its column; a wrap pending at the old edge is not
        // at the new one.
        cursor.pendingWrap = false;
        savedCursor.pendingWrap = false;
    }
    if (cols < m_cols) {
        // Blocks the new width would cut are erased.
        std::vector<std::pair<int, int>> doomed;
        for (int y = 0; y < m_rowCount; ++y) {
            const Row& r = row(y);
            if (r.multicell) {
                for (const auto& [col, params] : *r.multicell) {
                    if (col + params.columns() > cols) {
                        doomed.emplace_back(y, col);
                    }
                }
            }
        }
        for (const auto& [y, x] : doomed) {
            eraseMulticell(y, x);
        }
    }
    if (cols != m_cols) {
        const std::size_t history = historySize();
        for (std::size_t i = 0; i < m_rows.size(); ++i) {
            Row& r = m_rows[i];
            truncateRow(r, cols);
            // A spacer head marks the last column only: wider, it is a blank.
            if (cols > m_cols && r.cells.size() >= std::size_t(m_cols)) {
                r.cells[std::size_t(m_cols - 1)].setSpacerHead(false);
            }
            if (i >= history) {
                r.cells.resize(std::size_t(cols)); // the active area is always full width
            }
        }
        m_cols = cols;
    }

    if (rows < m_rowCount) {
        int excess = m_rowCount - rows;
        // Drop blank rows below the cursor first. Then push rows into history
        // only as far as the cursor's row needs to stay on the screen, and
        // drop the rest from the bottom, as reflow does: the cursor stays on
        // its own line.
        while (excess > 0 && m_rowCount - 1 > cursor.y && rowIsBlank(m_rows.back())) {
            m_rows.pop_back();
            --m_rowCount;
            --excess;
        }
        const int intoHistory = std::clamp(cursor.y + 1 - rows, 0, excess);
        for (int k = intoHistory; k < excess; ++k) {
            releaseRow(m_rows.back());
            m_rows.pop_back();
            --m_rowCount;
        }
        for (int k = 0; k < intoHistory; ++k) {
            compact(m_rows[historySize() + std::size_t(k)]); // top rows become history
        }
        m_rowCount -= intoHistory;
        cursor.y -= intoHistory;
        savedCursor.y -= intoHistory;
    } else if (rows > m_rowCount) {
        const int grow = rows - m_rowCount;
        const int pulled = int(std::min<std::size_t>(std::size_t(grow), historySize()));
        for (int k = 1; k <= pulled; ++k) {
            expand(m_rows[historySize() - std::size_t(k)]); // history rows return to the screen
        }
        m_rowCount += pulled;
        cursor.y += pulled;
        savedCursor.y += pulled;
        while (m_rowCount < rows) {
            m_rows.emplace_back(m_cols);
            ++m_rowCount;
        }
    }
    trimHistory();
    if (trackedColumns) for (auto& col : *trackedColumns) col = std::clamp(col, 0, cols);
    if (tracked) {
        const std::size_t dropped = std::size_t(m_rowsDropped - droppedBefore);
        for (std::size_t& t : *tracked) {
            t = (t == TrackedRowGone || t < dropped || t - dropped >= m_rows.size()) ? TrackedRowGone : t - dropped;
        }
    }
}

void Screen::resizeReflow(int cols, int rows, std::optional<std::size_t> promptStart, std::vector<std::size_t>* tracked, std::vector<int>* trackedColumns)
{
    const std::size_t cursorAbs = historySize() + std::size_t(cursor.y);
    // Reflow output above a redrawable prompt, preserving the prompt's row
    // count and cells. Shells may repaint only differences after SIGWINCH;
    // blanking their cached content makes an unchanged prompt disappear.
    const bool keepPrompt = promptStart && *promptStart <= cursorAbs;
    const std::size_t reflowEnd = keepPrompt ? *promptStart : m_rows.size();
    const std::size_t promptRows = keepPrompt ? cursorAbs - *promptStart + 1 : 0;
    const bool trackCursor = !keepPrompt;
    // A pending wrap puts the cursor after the last character printed: it
    // follows that position, and is pending again if the position lands
    // right after a full row.
    const bool wasPending = cursor.pendingWrap && trackCursor;
    bool cursorAtEdge = false;

    // Tracked rows are remembered as a cell offset within their logical line
    // and mapped back to rows while rewrapping. Rows of a kept prompt keep
    // their row offset.
    std::vector<std::size_t> trackedNew(tracked ? tracked->size() : 0, TrackedRowGone);
    std::vector<int> mappedColumns(trackedNew.size(), 0);
    std::vector<std::pair<std::size_t, std::size_t>> trackedOrder; // (abs row, index)
    std::vector<std::pair<std::size_t, std::size_t>> trackedInPrompt; // (prompt row, index)
    if (tracked) {
        for (std::size_t k = 0; k < tracked->size(); ++k) {
            const std::size_t t = (*tracked)[k];
            if (t == TrackedRowGone || t >= m_rows.size()) {
                continue;
            }
            if (t >= reflowEnd) {
                trackedInPrompt.emplace_back(t - reflowEnd, k);
            } else {
                trackedOrder.emplace_back(t, k);
            }
        }
        std::sort(trackedOrder.begin(), trackedOrder.end());
    }
    std::size_t trackedNext = 0;
    // Spacer heads are not cells of the line.
    auto trackedOffset = [&](const Row& r, std::size_t index, std::size_t lineCells) {
        const int col = trackedColumns ? std::clamp((*trackedColumns)[index], 0, m_cols) : 0;
        std::size_t offset = lineCells;
        for (int x = 0; x < col; ++x) if (!r.at(x).isSpacerHead()) ++offset;
        return offset;
    };

    for (std::size_t i = reflowEnd + promptRows; i < m_rows.size(); ++i) {
        releaseRow(m_rows[i]);
    }

    // Rewrap one logical line at a time, straight from the old rows into the
    // new ones. A line's cells are the cells of its rows without spacer heads
    // and without the blank cells that end the line. Cells keep their style
    // references, and graphemes move with their cells. The old rows of a
    // line are freed as soon as the line is rewrapped.
    std::deque<Row> out;
    std::size_t cursorRow = 0;
    int cursorCol = 0;
    // Rows that can store blank cells after their content. Only these need
    // compacting if they end in history.
    std::vector<std::size_t> loose;
    // The row being written. A finished row does not store the unwritten
    // cells after its content; active rows expand again below.
    std::vector<Cell> pending(static_cast<std::size_t>(cols));
    auto finishRow = [&] {
        std::size_t n = pending.size();
        while (n > 0 && pending[n - 1] == Cell{}) {
            --n;
        }
        if (n > 0 && isDroppableBlank(pending[n - 1])) {
            loose.push_back(out.size() - 1);
        }
        out.back().cells.assign(pending.begin(), pending.begin() + std::ptrdiff_t(n));
        std::fill(pending.begin(), pending.begin() + std::ptrdiff_t(n), Cell{});
    };
    std::vector<std::pair<std::size_t, std::size_t>> marks; // (cell offset, index) in the line
    std::size_t i = 0;
    while (i < reflowEnd) {
        Row& first = m_rows[i];
        // A compact hard line that still fits needs no cell reconstruction.
        // Its row moves with its allocation and side tables.
        if (i < historySize() && !first.wrapped && first.cells.size() <= std::size_t(cols)
            && first.lineSize == LineSize::Single) {
            for (; trackedNext < trackedOrder.size() && trackedOrder[trackedNext].first == i; ++trackedNext) {
                const auto index = trackedOrder[trackedNext].second;
                trackedNew[index] = out.size();
                mappedColumns[index] = int(std::min(std::size_t(cols), trackedOffset(first, index, 0)));
            }
            loose.push_back(out.size());
            out.push_back(std::move(first));
            ++i;
            continue;
        }

        const std::size_t lineStart = out.size();
        out.emplace_back();
        bool linePrompt = false;
        // DEC double width or height: kept when the line is one row that
        // still fits at half the new width; otherwise it becomes single size.
        const LineSize lineSize = first.lineSize;
        const bool singleRow = !first.wrapped || i + 1 == reflowEnd;
        std::size_t off = 0; // cells of the line read so far
        int col = 0;
        int headCol = -1; // a wide head that waits for its tail
        marks.clear();
        std::size_t nextMark = 0;
        bool cursorHere = false;
        bool cursorPlaced = false;
        std::size_t cursorOffset = 0;
        auto placeCursorIfHere = [&](std::size_t offset, int mappedCol) {
            while (nextMark < marks.size() && marks[nextMark].first <= offset) {
                const auto index = marks[nextMark++].second;
                trackedNew[index] = out.size() - 1;
                mappedColumns[index] = mappedCol;
            }
            if (cursorHere && !cursorPlaced && offset == cursorOffset) {
                cursorRow = out.size() - 1;
                cursorCol = mappedCol;
                cursorPlaced = true;
            }
        };
        // A wide head takes the tail that follows it in the line, or a new one.
        auto endWide = [&](const Cell* tail) {
            Cell& dst = pending[std::size_t(headCol + 1)];
            if (tail) {
                dst = *tail;
            } else {
                dst.setWidth(0);
                dst.setStyleId(pending[std::size_t(headCol)].styleId());
                m_styles.retain(dst.styleId());
            }
            headCol = -1;
        };
        // Places one cell of the line. The run copy below is the fast case.
        auto placeCell = [&](Row& src, int x, const Cell& c) {
            if (c.isTail()) {
                if (headCol >= 0) {
                    endWide(&c);
                }
                placeCursorIfHere(off++, col - 1);
                return;
            }
            if (headCol >= 0) {
                endWide(nullptr);
            }
            const int w = std::max(1, c.width());
            if (col + w > cols) {
                if (w == 2 && col < cols) {
                    pending[std::size_t(col)].setSpacerHead(true);
                }
                finishRow();
                out.back().wrapped = true;
                out.emplace_back();
                col = 0;
            }
            placeCursorIfHere(off++, col);
            pending[std::size_t(col)] = c;
            if (c.hasGrapheme() && src.graphemes) {
                // The map node moves to the new row; no string is copied.
                auto node = src.graphemes->extract(std::uint16_t(x));
                if (!node.empty()) {
                    Row& dst = out.back();
                    if (!dst.graphemes) {
                        dst.graphemes = std::make_unique<GraphemeMap>();
                    }
                    node.key() = std::uint16_t(col);
                    dst.graphemes->insert(std::move(node));
                }
            }
            if (w == 2) {
                headCol = col;
            }
            col += w;
        };

        std::size_t j = i;
        for (;; ++j) {
            Row& r = m_rows[j];
            const bool lineEnds = !r.wrapped || j + 1 == reflowEnd;
            const std::size_t marked = marks.size();
            for (; trackedNext < trackedOrder.size() && trackedOrder[trackedNext].first == j; ++trackedNext) {
                const auto index = trackedOrder[trackedNext].second;
                marks.emplace_back(trackedOffset(r, index, off), index);
            }
            std::sort(marks.begin() + std::ptrdiff_t(marked), marks.end());
            if (off == 0) {
                linePrompt = r.promptStart;
            }
            int end = m_cols;
            if (lineEnds) {
                while (end > 0 && isBlank(r.at(end - 1))) {
                    --end;
                }
            }
            if (trackCursor && j == cursorAbs) {
                cursorHere = true;
                cursorOffset = off + std::size_t(cursor.x) + (wasPending ? 1 : 0);
            }
            const Cell* cells = r.cells.data();
            const int stored = int(r.cells.size());
            int x = 0;
            while (x < end) {
                // Copy narrow plain cells in one run. A run stops at the end
                // of the new row, at a tracked position or after a wide head.
                int limit = headCol >= 0 ? 0 : std::min(end - x, cols - col);
                if (nextMark < marks.size()) {
                    limit = int(std::min(std::size_t(limit), marks[nextMark].first - off));
                }
                if (cursorHere && !cursorPlaced && cursorOffset >= off) {
                    limit = int(std::min(std::size_t(limit), cursorOffset - off));
                }
                int n = 0;
                while (n < limit && x + n < stored && cells[x + n].width() == 1 && !cells[x + n].hasGrapheme()
                       && !cells[x + n].isSpacerHead()) {
                    ++n;
                }
                if (x + n >= stored) {
                    n = limit; // cells past the stored ones read as blank
                }
                if (x < stored) {
                    std::copy(cells + x, cells + std::min(x + n, stored), pending.begin() + col);
                }
                x += n;
                col += n;
                off += std::size_t(n);
                if (x >= end) {
                    break;
                }
                const Cell c = r.at(x);
                if (c.isSpacerHead()) {
                    m_styles.release(c.styleId());
                } else {
                    placeCell(r, x, c);
                }
                ++x;
            }
            if (lineEnds) {
                break;
            }
        }
        if (headCol >= 0) {
            endWide(nullptr);
        }
        placeCursorIfHere(off, col);
        finishRow();
        while (nextMark < marks.size()) {
            const auto [offset, index] = marks[nextMark++];
            trackedNew[index] = out.size() - 1;
            mappedColumns[index] = std::min(cols, col + int(offset - off));
        }
        if (cursorHere && !cursorPlaced) {
            // Cursor past the end of the content: advance virtually.
            std::size_t extra = cursorOffset - std::min(cursorOffset, off);
            std::size_t r = out.size() - 1;
            std::size_t c = std::size_t(col) + extra;
            if (c == std::size_t(cols)) {
                // Right after content that exactly fills the row: stay on it.
                c = std::size_t(cols) - 1;
                cursorAtEdge = true;
            }
            while (c >= std::size_t(cols)) {
                c -= std::size_t(cols);
                ++r;
            }
            while (out.size() <= r) {
                out.back().wrapped = true;
                loose.push_back(out.size());
                out.emplace_back(cols);
            }
            cursorRow = r;
            cursorCol = int(c);
        }
        out[lineStart].promptStart = linePrompt;
        if (lineSize != LineSize::Single && singleRow && out.size() - lineStart == 1 && col <= cols / 2) {
            out[lineStart].lineSize = lineSize;
        }
        for (; i <= j; ++i) {
            m_rows[i] = Row{};
        }
    }
    const std::size_t promptRegionStart = out.size();
    if (keepPrompt) {
        // Preserve row positions so the shell's cursor-up repaint still lands
        // at the prompt start. Move ownership of styles and graphemes with the
        // cells; only content clipped by a narrower width is discarded.
        for (std::size_t k = 0; k < promptRows; ++k) {
            out.push_back(std::move(m_rows[reflowEnd + k]));
            Row& row = out.back();
            truncateRow(row, cols);
            row.cells.resize(std::size_t(cols));
            row.promptStart = k == 0;
        }
        cursorRow = out.size() - 1;
        cursorCol = std::min(cursor.x, cols - 1);
    }
    if (out.empty()) {
        out.emplace_back(cols);
    }

    for (const auto& [offset, k] : trackedInPrompt) {
        if (keepPrompt && offset < promptRows) {
            trackedNew[k] = promptRegionStart + offset;
            mappedColumns[k] = trackedColumns ? std::clamp((*trackedColumns)[k], 0, cols) : 0;
        }
    }
    if (trackedColumns) *trackedColumns = std::move(mappedColumns);

    // Trim blank rows below the cursor, pad to the screen height.
    while (out.size() > cursorRow + 1 && rowIsBlank(out.back()) && !out[out.size() - 2].wrapped) {
        out.pop_back();
    }
    while (out.size() < std::size_t(rows)) {
        out.emplace_back(cols);
    }
    // Keep the cursor inside the active area.
    while (out.size() - std::size_t(rows) > cursorRow && out.size() > std::size_t(rows)) {
        releaseRow(out.back());
        out.pop_back();
    }

    m_rows = std::move(out);
    m_cols = cols;
    m_rowCount = rows;
    cursor.y = int(cursorRow - (m_rows.size() - std::size_t(rows)));
    cursor.pendingWrap = wasPending && (cursorAtEdge || cursorCol >= cols);
    cursor.x = std::min(cursorCol, cols - 1);
    m_viewportOffset = 0; // resize() restores a scrolled-back position
    const std::uint64_t droppedBefore = m_rowsDropped;
    trimHistory();
    const std::size_t dropped = std::size_t(m_rowsDropped - droppedBefore);
    for (const std::size_t k : loose) {
        if (k >= dropped && k - dropped < historySize()) {
            compact(m_rows[k - dropped]);
        }
    }
    for (std::size_t i = std::max(promptRegionStart, dropped) - dropped; i < historySize(); ++i) {
        compact(m_rows[i]);
    }
    // A compact history row can enter the active area when the height grows
    // or preceding lines unwrap. Active rows must always store every column.
    for (std::size_t i = historySize(); i < m_rows.size(); ++i) {
        expand(m_rows[i]);
    }
    if (tracked) {
        for (std::size_t k = 0; k < trackedNew.size(); ++k) {
            const std::size_t t = trackedNew[k];
            (*tracked)[k] = (t == TrackedRowGone || t < dropped || t - dropped >= m_rows.size()) ? TrackedRowGone
                                                                                                : t - dropped;
        }
    }
}

} // namespace AfterglowVt
