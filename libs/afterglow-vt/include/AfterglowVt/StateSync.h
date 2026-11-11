// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/Selection.h>
#include <AfterglowVt/Style.h>
#include <AfterglowVt/afterglowvt_export.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// The state of a terminal, carried to a replica in another process: the
// codec afterglow.state.v1 of the mux. The daemon parses the program's
// output into its terminal. A StateWriter turns what changed into a frame,
// and a StateReader applies the frame to a replica Terminal in the window,
// which never sees the program's bytes. The window then draws, selects and
// searches the replica as it does a terminal of its own.
//
// A writer remembers what each frame told the replica: every frame that
// update() returns must reach the reader, in order, or the writer must be
// reset() for a new replica. A frame that waits for room in its channel waits
// whole, and goes before the next one.
//
// The replica numbers its rows as the writer does: the same stable id for
// each row, and a new layout epoch of its own whenever the writer's changes,
// also while history rows that the writer has are still on their way. Their
// places then hold blank rows. A selection, a search, a mark or a scroll
// thus names the same text at both ends.
//
// What a frame carries: both screens (their rows, the history with the
// stable id of each row, the cursor), the styles and links of their cells,
// the modes, the colors, the cursor shape, the title and the working
// directory, the extra cursors, the OSC 133 marks and semantic regions, the
// program status, the shell contexts, and the region scrolls since the last
// frame. Not yet: images.

namespace AfterglowVt {

class Terminal;

inline constexpr std::string_view StateCodec = "afterglow.state.v1";

/// The region scrolls of a terminal (ITerminalHost::regionScrolled) for the
/// next frame. The replica gets the rows that moved as rows, so its host
/// hears of each scroll from the frame, once the frame applied, and moves
/// what it keeps by stable row id (a selection, a search) with the text, as a
/// host of the terminal does.
class AFTERGLOWVT_EXPORT RegionScrolls
{
public:
    /// The most scrolls that a frame carries. A scroll that continues the
    /// last one (the same region, the same direction) adds to it.
    static constexpr std::size_t Max = 256;
    /// A scroll of the active screen, @p altScreen as the terminal has it
    /// when it calls back. Past Max, every row from the top of the highest
    /// region that scrolled to the bottom of the lowest, on each screen,
    /// counts as moved out: the replica's host lets go of what it kept there.
    void add(bool altScreen, const RegionScroll& scroll);
    void clear();
    bool empty() const { return m_entries.empty() && !m_lost; }

private:
    friend class StateWriter;
    struct Entry
    {
        bool altScreen = false;
        RegionScroll scroll;
    };
    /// The rows that the scrolls of one screen touched, for a loss.
    struct Span
    {
        bool any = false;
        std::uint64_t top = 0;
        std::uint64_t bottom = 0;
    };
    std::vector<Entry> m_entries;
    Span m_spans[2];
    bool m_lost = false;
};

class AFTERGLOWVT_EXPORT StateWriter
{
public:
    /// The next frame for @p terminal: what changed since the last frame,
    /// everything for the first one. Empty when nothing changed. Visible
    /// rows, then history rows, with the style and link definitions that
    /// they need, wait for the next frames past @p maxBytes. The first row
    /// of a frame goes past it by its own bytes at most: definitions too
    /// many for one frame go ahead of their row in frames of their own. The
    /// other sections are not in the budget, and go whole: together they
    /// are under 1 MiB (the bounds of the status records, the contexts, the
    /// semantic regions, the extra cursors, the title and the scrolls).
    /// @p scrolls: the region scrolls since the last frame, which the first
    /// frame leaves out: a new replica keeps nothing that they move.
    std::string update(const Terminal& terminal, std::size_t maxBytes, const RegionScrolls* scrolls = nullptr);
    /// The most history rows that the replica keeps: the writer sends no
    /// older ones. 0 sends none.
    void setReplicaHistory(std::size_t rows) { m_replicaHistory = rows; }
    /// The next frame carries everything again, as the first one does: for
    /// a new replica.
    void reset();
    /// Visible or history rows that wait for a later frame.
    bool pending() const;

private:
    struct ScreenState
    {
        bool synced = false;
        int cols = 0;
        int rows = 0;
        std::uint64_t epoch = 0;
        /// Stable ids of the replica's history: [from, to).
        std::uint64_t from = 0;
        std::uint64_t to = 0;
        std::uint64_t oldestSent = ~std::uint64_t(0); ///< the oldest id that the last history section named
        std::uint64_t totalSent = ~std::uint64_t(0); ///< the id of the first visible row that it named
        /// A hash of each visible row as the replica has it; 0 for a row
        /// that it does not have.
        std::vector<std::uint64_t> rowHashes;
        std::unordered_map<std::uint32_t, Style> styles; ///< the definitions that the replica has
        std::unordered_map<std::uint32_t, std::pair<std::string, std::string>> links;
        bool historyPending = false;
        bool rowsPending = false; ///< changed visible rows wait for a later frame
        int rowStart = 0; ///< the first row that the last frame left out, where the next one starts
    };

    /// What is left of a frame's budget for rows, across both screens.
    struct Budget
    {
        std::size_t max = 0;
        bool anyRow = false; ///< the frame carries a row
        bool full = false; ///< no more rows: definitions went ahead of one
    };
    /// The visible rows of screen @p which that changed, within @p budget.
    /// @p changed: the terminal changed since the last frame. True when the
    /// screen started over (a reset section).
    bool writeRows(std::string& out, const Terminal& terminal, int which, ScreenState& state, Budget& budget, bool changed);
    /// The history rows that the replica lacks, within @p budget.
    void writeHistory(std::string& out, const Terminal& terminal, int which, ScreenState& state, Budget& budget);

    ScreenState m_screens[2];
    std::string m_modes;
    std::string m_colors;
    std::string m_text;
    std::string m_cursor;
    std::string m_shell;
    std::string m_status;
    std::string m_contexts;
    std::string m_extraCursors;
    std::uint64_t m_generation = 0; ///< Terminal::generation() at the last frame
    std::uint64_t m_statusSerial = 0; ///< ProgramStatus::serial() at the last frame
    std::size_t m_replicaHistory = ~std::size_t(0);
    bool m_started = false;
};

class AFTERGLOWVT_EXPORT StateReader
{
public:
    /// Applies @p frame to @p replica, a terminal that only this reader
    /// changes. False with @p error for a frame that breaks the codec or does
    /// not fit what the replica has: the replica then needs a frame from a
    /// writer that was reset. The replica's host hears of the scrolls, the
    /// status and the contexts of a frame once all of it applied, and of
    /// none for a frame that fails.
    bool apply(Terminal& replica, std::string_view frame, std::string* error);
    /// The last frame that applied changed the visible rows or started a
    /// screen over: output of the program, not history that catches up.
    bool screenChanged() const { return m_screenChanged; }
    /// Every row of the primary screen from stable id @p first to @p last
    /// that the writer has came: none is a blank place of a row on its way,
    /// and none is an older row that the writer still sends (PrependTag).
    bool received(std::uint64_t first, std::uint64_t last) const
    {
        const ScreenState& st = m_screens[0];
        const bool blank = st.hole < st.next && first < st.next && last >= st.hole;
        const bool older = first < st.from && st.from > st.oldest;
        return !blank && !older;
    }

private:
    struct ScreenState
    {
        std::uint64_t next = 0; ///< the stable id of the first visible row: the writer's
        /// The ids in [hole, next) are blank rows in the history, in the
        /// places of rows that the writer has and did not send yet.
        std::uint64_t hole = 0;
        /// The oldest history row that the writer sent, which the replica
        /// may have dropped past its own limit: the next older row is one less.
        std::uint64_t from = 0;
        /// The oldest row that the writer sends at all (its AppendTag): rows in
        /// [oldest, from) are still to come.
        std::uint64_t oldest = 0;
        std::unordered_map<std::uint32_t, Style> styles; ///< the writer's definitions, with the writer's link ids
        std::unordered_map<std::uint32_t, std::pair<std::string, std::string>> links;
        std::unordered_map<std::uint32_t, std::uint32_t> linkCache; ///< the writer's link id to the replica's
        std::unordered_map<std::uint32_t, std::uint32_t> linkOwner; ///< the replica's link id to the writer's that holds it
    };
    /// The layout epoch of each of the writer's screens, as its last reset
    /// said, for as long as this reader lives: a screen that starts over in
    /// a new layout numbers its rows anew, and so must the replica's.
    struct Epoch
    {
        bool known = false;
        std::uint64_t epoch = 0;
    };

    ScreenState m_screens[2];
    Epoch m_epochs[2];
    bool m_screenChanged = false;
};

} // namespace AfterglowVt
