// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Screen.h>

#include <algorithm>
#include <unordered_map>
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
        Cell& tail = row.cells[std::size_t(x + 1)];
        m_styles.release(tail.styleId());
        tail = Cell{};
        tail.setStyleId(styleId);
        tail.setWidth(0);
        m_styles.retain(styleId);
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
            (*r.graphemes)[std::uint16_t(x)] = std::u32string(extra.substr(0, 4095));
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
    const MulticellParams* p = multicellParams(start, x);
    const MulticellParams params = p ? *p : MulticellParams{std::uint8_t(c.multicellDy() + 1), std::uint8_t(c.multicellDx() + 1)};
    for (int dy = 0; dy < params.scale; ++dy) {
        const int yy = top + dy;
        if (yy < 0 || yy >= m_rowCount) {
            continue;
        }
        Row& r = row(yy);
        for (int dx = 0; dx < params.columns() && left + dx < m_cols; ++dx) {
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
    // A history row: only its own part of the block is here.
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

void Screen::eraseMulticellsCrossing(int top, int bottom, int left, int right)
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
            if (touches && !inside) {
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
    breakWideAt(row, x, fillStyleId);
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
    if (x + n <= right) {
        breakWideAt(row, x + n, fillStyleId);
    }
    breakWideAt(row, right + 1, fillStyleId);

    for (int i = x; i < x + n; ++i) {
        clearCell(row, i, StyleTable::DefaultId);
    }
    for (int i = x; i + n <= right; ++i) {
        m_styles.release(row.cells[std::size_t(i)].styleId());
        row.cells[std::size_t(i)] = row.cells[std::size_t(i + n)];
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
    while (keep > 0 && isBlank(row.cells[keep - 1])) {
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

void Screen::scrollUp(const Region& r, int n, std::uint32_t fillStyleId, bool intoHistory)
{
    const int height = r.bottom - r.top + 1;
    n = std::clamp(n, 0, height);
    if (n == 0) {
        return;
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
        return;
    }

    if (fullWidth && r.top == 0 && r.bottom == m_rowCount - 1 && historySize() == 0) {
        // The whole screen with no history (the alternate screen): the top
        // rows leave and come back cleared at the bottom, rather than every
        // row moving up one. Nothing can cross the screen's own edges, and
        // the row ids stay with their content, so images scroll with it.
        for (int k = 0; k < n; ++k) {
            m_rows.push_back(std::move(m_rows.front()));
            m_rows.pop_front();
            ++m_rowsDropped;
            eraseRow(m_rows.back(), fillStyleId);
        }
        return;
    }

    // Blocks the region boundary cuts cannot move with it.
    eraseMulticellsCrossing(r.top, r.bottom, r.left, r.right);
    if (fullWidth) {
        auto first = m_rows.begin() + std::ptrdiff_t(historySize()) + r.top;
        std::rotate(first, first + n, first + height);
        for (int y = r.bottom - n + 1; y <= r.bottom; ++y) {
            eraseRow(row(y), fillStyleId);
        }
        return;
    }

    for (int y = r.top; y + n <= r.bottom; ++y) {
        copyRowSpan(row(y), row(y + n), r.left, r.right);
    }
    for (int y = r.bottom - n + 1; y <= r.bottom; ++y) {
        eraseCells(row(y), r.left, r.right + 1, fillStyleId);
    }
}

void Screen::scrollDown(const Region& r, int n, std::uint32_t fillStyleId)
{
    const int height = r.bottom - r.top + 1;
    n = std::clamp(n, 0, height);
    if (n == 0) {
        return;
    }
    const bool fullWidth = r.left == 0 && r.right == m_cols - 1;

    eraseMulticellsCrossing(r.top, r.bottom, r.left, r.right);
    if (fullWidth) {
        auto first = m_rows.begin() + std::ptrdiff_t(historySize()) + r.top;
        std::rotate(first, first + (height - n), first + height);
        for (int y = r.top; y < r.top + n; ++y) {
            eraseRow(row(y), fillStyleId);
        }
        return;
    }

    for (int y = r.bottom; y - n >= r.top; --y) {
        copyRowSpan(row(y), row(y - n), r.left, r.right);
    }
    for (int y = r.top; y < r.top + n; ++y) {
        eraseCells(row(y), r.left, r.right + 1, fillStyleId);
    }
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
    ++m_layoutEpoch;
    if (reflow && m_maxScrollback > 0 && cols != m_cols) {
        flattenMulticells(); // blocks cannot reflow; their text can
        resizeReflow(cols, rows, promptStart, trackedRows, trackedColumns);
    } else {
        resizeNoReflow(cols, rows, trackedRows, trackedColumns);
    }
    cursor.x = std::clamp(cursor.x, 0, m_cols - 1);
    cursor.y = std::clamp(cursor.y, 0, m_rowCount - 1);
    cursor.pendingWrap = false;
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
    if (cols < m_cols) {
        // Blocks the new width or height would cut are erased.
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
            if (i >= history) {
                r.cells.resize(std::size_t(cols)); // the active area is always full width
            }
        }
        m_cols = cols;
    }

    if (rows < m_rowCount) {
        int excess = m_rowCount - rows;
        // Drop blank rows below the cursor first, then push rows into history.
        while (excess > 0 && m_rowCount - 1 > cursor.y && rowIsBlank(m_rows.back())) {
            m_rows.pop_back();
            --m_rowCount;
            --excess;
        }
        for (int k = 0; k < excess; ++k) {
            compact(m_rows[historySize() + std::size_t(k)]); // top rows become history
        }
        m_rowCount -= excess;
        cursor.y -= excess;
        savedCursor.y -= excess;
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
    struct Line
    {
        std::vector<Cell> cells;
        std::unordered_map<std::uint32_t, std::u32string> graphemes;
        bool promptStart = false;
    };

    const std::size_t cursorAbs = historySize() + std::size_t(cursor.y);
    // Reflow output above a redrawable prompt, preserving the prompt's row
    // count and cells. Shells may repaint only differences after SIGWINCH;
    // blanking their cached content makes an unchanged prompt disappear.
    const bool keepPrompt = promptStart && *promptStart <= cursorAbs;
    const std::size_t reflowEnd = keepPrompt ? *promptStart : m_rows.size();
    const std::size_t promptRows = keepPrompt ? cursorAbs - *promptStart + 1 : 0;
    const bool trackCursor = !keepPrompt;
    std::size_t cursorLine = 0;
    std::size_t cursorOffset = 0;

    // Tracked rows are remembered as (line, cell offset within the line) and
    // mapped back to rows after rewrapping.
    struct TrackedPos
    {
        enum Kind : std::uint8_t { Gone, InLine, InPrompt } kind = Gone;
        std::size_t line = 0;
        std::size_t offset = 0;
    };
    std::vector<TrackedPos> trackedPos(tracked ? tracked->size() : 0);
    std::vector<std::pair<std::size_t, std::size_t>> trackedOrder; // (abs row, index)
    if (tracked) {
        for (std::size_t k = 0; k < tracked->size(); ++k) {
            const std::size_t t = (*tracked)[k];
            if (t == TrackedRowGone || t >= m_rows.size()) {
                continue;
            }
            if (t >= reflowEnd) {
                trackedPos[k] = {TrackedPos::InPrompt, 0, t - reflowEnd};
            } else {
                trackedOrder.emplace_back(t, k);
            }
        }
        std::sort(trackedOrder.begin(), trackedOrder.end());
    }
    std::size_t trackedNext = 0;


    // 1. Unwrap rows into logical lines.
    std::vector<Line> lines;
    Line current;
    for (std::size_t i = reflowEnd + promptRows; i < m_rows.size(); ++i) {
        releaseRow(m_rows[i]);
    }
    for (std::size_t i = 0; i < reflowEnd; ++i) {
        Row& r = m_rows[i];
        const bool lineEnds = !r.wrapped || i + 1 == reflowEnd;
        while (trackedNext < trackedOrder.size() && trackedOrder[trackedNext].first == i) {
            const auto index = trackedOrder[trackedNext].second;
            const int col = trackedColumns ? std::clamp((*trackedColumns)[index], 0, m_cols) : 0;
            std::size_t offset = current.cells.size();
            for (int x = 0; x < col; ++x) if (!r.at(x).isSpacerHead()) ++offset;
            trackedPos[index] = {TrackedPos::InLine, lines.size(), offset};
            ++trackedNext;
        }
        if (current.cells.empty()) {
            current.promptStart = r.promptStart;
        }
        int keep = m_cols;
        if (lineEnds) {
            while (keep > 0 && isBlank(r.at(keep - 1))) {
                --keep;
            }
        }
        expand(r); // reading every column below
        if (trackCursor && i == cursorAbs) {
            cursorLine = lines.size();
            cursorOffset = current.cells.size() + std::size_t(cursor.x);
        }
        for (int x = 0; x < m_cols; ++x) {
            Cell& c = r.cells[std::size_t(x)];
            if (x >= keep || c.isSpacerHead()) {
                m_styles.release(c.styleId());
                continue;
            }
            if (c.hasGrapheme() && r.graphemes) {
                auto it = r.graphemes->find(std::uint16_t(x));
                if (it != r.graphemes->end()) {
                    current.graphemes.emplace(std::uint32_t(current.cells.size()), std::move(it->second));
                }
            }
            current.cells.push_back(c);
        }
        if (lineEnds) {
            lines.push_back(std::move(current));
            current = Line{};

        }
    }
    if (!current.cells.empty()) {
        lines.push_back(std::move(current));
    }

    // 2. Rewrap every line to the new width.
    std::deque<Row> out;
    std::vector<std::size_t> lineFirstRow;
    std::vector<std::size_t> trackedNew(trackedPos.size(), TrackedRowGone);
    std::vector<int> mappedColumns(trackedPos.size(), 0);
    std::vector<std::vector<std::pair<std::size_t, std::size_t>>> lineMarks(lines.size());
    for (std::size_t k = 0; k < trackedPos.size(); ++k)
        if (trackedPos[k].kind == TrackedPos::InLine && trackedPos[k].line < lines.size())
            lineMarks[trackedPos[k].line].emplace_back(trackedPos[k].offset, k);
    for (auto& marks : lineMarks) std::sort(marks.begin(), marks.end());
    std::size_t cursorRow = 0;
    int cursorCol = 0;
    for (std::size_t li = 0; li < lines.size(); ++li) {
        Line& line = lines[li];
        lineFirstRow.push_back(out.size());
        out.emplace_back(cols).promptStart = line.promptStart;
        int col = 0;
        bool cursorPlaced = false;
        std::size_t nextMark = 0;
        auto placeCursorIfHere = [&](std::size_t offset) {
            while (nextMark < lineMarks[li].size() && lineMarks[li][nextMark].first <= offset) {
                const auto index = lineMarks[li][nextMark++].second;
                trackedNew[index] = out.size() - 1;
                mappedColumns[index] = col;
            }
            if (trackCursor && li == cursorLine && !cursorPlaced && offset == cursorOffset) {
                cursorRow = out.size() - 1;
                cursorCol = col;
                cursorPlaced = true;
            }
        };
        for (std::size_t off = 0; off < line.cells.size(); ++off) {
            const Cell& c = line.cells[off];
            if (c.isTail()) {
                placeCursorIfHere(off);
                continue; // re-emitted with its head
            }
            const int w = std::max(1, c.width());
            if (col + w > cols) {
                if (w == 2 && col < cols) {
                    out.back().cells[std::size_t(col)].setSpacerHead(true);
                }
                out.back().wrapped = true;
                out.emplace_back(cols);
                col = 0;
            }
            placeCursorIfHere(off);
            Row& dst = out.back();
            dst.cells[std::size_t(col)] = c;
            if (c.hasGrapheme()) {
                auto it = line.graphemes.find(std::uint32_t(off));
                if (it != line.graphemes.end()) {
                    if (!dst.graphemes) {
                        dst.graphemes = std::make_unique<GraphemeMap>();
                    }
                    (*dst.graphemes)[std::uint16_t(col)] = std::move(it->second);
                }
            }
            if (w == 2) {
                const bool hasTail = off + 1 < line.cells.size() && line.cells[off + 1].isTail();
                if (hasTail) {
                    dst.cells[std::size_t(col + 1)] = line.cells[off + 1];
                } else {
                    dst.cells[std::size_t(col + 1)].setWidth(0);
                    dst.cells[std::size_t(col + 1)].setStyleId(c.styleId());
                    m_styles.retain(c.styleId());
                }
            }
            col += w;
        }
        placeCursorIfHere(line.cells.size());
        while (nextMark < lineMarks[li].size()) {
            const auto index = lineMarks[li][nextMark++].second;
            trackedNew[index] = out.size() - 1;
            mappedColumns[index] = std::min(cols, col + int(trackedPos[index].offset - line.cells.size()));
        }
        if (trackCursor && li == cursorLine && !cursorPlaced) {
            // Cursor past the end of the content: advance virtually.
            std::size_t extra = cursorOffset - std::min(cursorOffset, line.cells.size());
            std::size_t r = out.size() - 1;
            std::size_t c = std::size_t(col) + extra;
            if (c == std::size_t(cols)) {
                // Right after content that exactly fills the row: stay on it.
                c = std::size_t(cols) - 1;
            }
            while (c >= std::size_t(cols)) {
                c -= std::size_t(cols);
                ++r;
            }
            while (out.size() <= r) {
                out.back().wrapped = true;
                out.emplace_back(cols);
            }
            cursorRow = r;
            cursorCol = int(c);
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

    for (std::size_t k = 0; k < trackedPos.size(); ++k) {
        const auto& tp = trackedPos[k];
        if (tp.kind == TrackedPos::InPrompt && keepPrompt && tp.offset < promptRows) {
            trackedNew[k] = promptRegionStart + tp.offset;
            mappedColumns[k] = trackedColumns ? std::clamp((*trackedColumns)[k], 0, cols) : 0;
        }
    }
    if (trackedColumns) *trackedColumns = std::move(mappedColumns);

    // 3. Trim blank rows below the cursor, pad to the screen height.
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
    cursor.x = std::min(cursorCol, cols - 1);
    m_viewportOffset = 0;
    const std::uint64_t droppedBefore = m_rowsDropped;
    trimHistory();
    for (std::size_t i = 0; i < historySize(); ++i) {
        compact(m_rows[i]);
    }
    if (tracked) {
        const std::size_t dropped = std::size_t(m_rowsDropped - droppedBefore);
        for (std::size_t k = 0; k < trackedNew.size(); ++k) {
            const std::size_t t = trackedNew[k];
            (*tracked)[k] = (t == TrackedRowGone || t < dropped || t - dropped >= m_rows.size()) ? TrackedRowGone
                                                                                                : t - dropped;
        }
    }
}

} // namespace AfterglowVt
