// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/Cell.h>
#include <AfterglowVt/Style.h>
#include <AfterglowVt/afterglowvt_export.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string_view>
#include <vector>

namespace AfterglowVt {

enum class Charset : std::uint8_t { Ascii, DecSpecialGraphics };

struct Cursor
{
    int x = 0;
    int y = 0;
    /// The cursor sits past the right margin; the next print wraps first.
    bool pendingWrap = false;
    Style pen;
    /// Interned id of @ref pen. The cursor owns one reference to it.
    std::uint32_t penId = StyleTable::DefaultId;
};

struct SavedCursor
{
    int x = 0;
    int y = 0;
    bool pendingWrap = false;
    bool originMode = false;
    Style pen;
    Charset charsets[4] = {};
    int gl = 0;
};

/// Inclusive scroll / margin rectangle in active-area coordinates.
struct Region
{
    int top = 0;
    int bottom = 0;
    int left = 0;
    int right = 0;
};

/// A grid of rows: scrollback history followed by the active area.
///
/// All cell mutation goes through Screen so that style reference counts,
/// grapheme side tables and wide-character integrity stay consistent.
class AFTERGLOWVT_EXPORT Screen
{
public:
    /// @param maxScrollback rows of history to keep; 0 disables history (alternate screen).
    Screen(int cols, int rows, std::size_t maxScrollback);
    ~Screen();

    Screen(const Screen&) = delete;
    Screen& operator=(const Screen&) = delete;

    int cols() const noexcept { return m_cols; }
    int rows() const noexcept { return m_rowCount; }

    std::size_t historySize() const noexcept { return m_rows.size() - std::size_t(m_rowCount); }
    std::size_t totalRows() const noexcept { return m_rows.size(); }

    Row& row(int y) noexcept { return m_rows[historySize() + std::size_t(y)]; }
    const Row& row(int y) const noexcept { return m_rows[historySize() + std::size_t(y)]; }
    /// Row by absolute index: 0 is the oldest history row.
    const Row& absoluteRow(std::size_t index) const noexcept { return m_rows[index]; }

    StyleTable& styles() noexcept { return m_styles; }
    const StyleTable& styles() const noexcept { return m_styles; }

    Cursor cursor;
    SavedCursor savedCursor;
    bool hasSavedCursor = false;

    void setPen(const Style& style);

    // Cell mutation --------------------------------------------------------

    /// Writes a cell, retaining @p styleId. A width-2 write also fills the tail
    /// at x + 1. Any wide character partially overwritten is cleared.
    void writeCell(Row& row, int x, char32_t cp, std::uint32_t styleId, int width);
    void appendGrapheme(Row& row, int x, char32_t cp);
    std::u32string_view graphemeExtra(const Row& row, int x) const;

    /// Blanks [x0, x1) with @p fillStyleId (background colour erase).
    void eraseCells(Row& row, int x0, int x1, std::uint32_t fillStyleId);
    void eraseRow(Row& row, std::uint32_t fillStyleId);

    /// Inserts @p n blank cells at @p x, shifting [x, right] to the right.
    void insertCells(Row& row, int x, int right, int n, std::uint32_t fillStyleId);
    /// Deletes @p n cells at @p x, shifting [x + n, right] to the left.
    void deleteCells(Row& row, int x, int right, int n, std::uint32_t fillStyleId);

    /// Scrolls the region's content up by @p n. With @p intoHistory, a region
    /// covering the whole screen sends the rows leaving the top into scrollback
    /// (only line feeds do this; SU and DL never do).
    void scrollUp(const Region& region, int n, std::uint32_t fillStyleId, bool intoHistory = false);
    void scrollDown(const Region& region, int n, std::uint32_t fillStyleId);

    void clearHistory();

    /// Resizes the grid. With @p reflow, soft-wrapped lines are rewrapped to the
    /// new width and the cursor follows its character.
    ///
    /// @p promptStart is the absolute row where the shell prompt under the
    /// cursor begins, when the shell redraws its prompt on resize. Rows from
    /// there to the cursor are not reflowed: they are blanked and keep their
    /// row count, so the shell's relative cursor movement (computed at the old
    /// width) lands back on the prompt start when it repaints.
    ///
    /// @p trackedRows, if given, holds absolute row indices of things anchored
    /// to rows (image placements). They are rewritten to where those rows end
    /// up, or to TrackedRowGone if the row no longer exists.
    void resize(int cols, int rows, bool reflow, std::optional<std::size_t> promptStart = std::nullopt,
                std::vector<std::size_t>* trackedRows = nullptr);
    static constexpr std::size_t TrackedRowGone = std::size_t(-1);

    // Viewport -------------------------------------------------------------

    /// Rows scrolled back from the live bottom. 0 shows the active area.
    std::size_t viewportOffset() const noexcept { return m_viewportOffset; }
    void scrollViewport(long delta) noexcept;
    void resetViewport() noexcept { m_viewportOffset = 0; }
    /// Absolute index of the first row shown in the viewport.
    std::size_t viewportTop() const noexcept { return historySize() - m_viewportOffset; }

    // Stable row ids ---------------------------------------------------------

    /// Rows dropped off the top of history so far. A row's stable id is
    /// rowsDropped() + its absolute index; it survives scrolling and history
    /// trimming (resize reflow starts a new numbering, see layoutEpoch()).
    std::uint64_t rowsDropped() const noexcept { return m_rowsDropped; }
    /// Changes whenever a resize rebuilds the rows; stable ids from an older
    /// epoch no longer refer to the same content.
    std::uint64_t layoutEpoch() const noexcept { return m_layoutEpoch; }
    /// Absolute index for a stable id, or -1 if that row is gone.
    long long absoluteIndexOf(std::uint64_t stableId) const noexcept
    {
        if (stableId < m_rowsDropped || stableId - m_rowsDropped >= m_rows.size()) {
            return -1;
        }
        return static_cast<long long>(stableId - m_rowsDropped);
    }

private:
    void releaseRow(Row& row) noexcept;
    void clearCell(Row& row, int x, std::uint32_t fillStyleId);
    void breakWideAt(Row& row, int x, std::uint32_t fillStyleId);
    void moveCells(Row& dst, int dstX, Row& src, int srcX, int n);
    void copyRowSpan(Row& dst, const Row& src, int left, int right);
    void trimHistory();
    void resizeNoReflow(int cols, int rows, std::vector<std::size_t>* tracked);
    void resizeReflow(int cols, int rows, std::optional<std::size_t> promptStart, std::vector<std::size_t>* tracked);

    std::deque<Row> m_rows;
    int m_cols;
    int m_rowCount;
    std::size_t m_maxScrollback;
    std::size_t m_viewportOffset = 0;
    std::uint64_t m_rowsDropped = 0;
    std::uint64_t m_layoutEpoch = 0;
    StyleTable m_styles;
};

} // namespace AfterglowVt
