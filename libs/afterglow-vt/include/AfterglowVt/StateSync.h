// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

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
// A frame always describes the newest state, so a writer that cannot send
// (a full channel) waits and sends one frame later that covers everything.
//
// What a frame carries: both screens (their rows, the history with the
// stable id of each row, the cursor), the styles and links of their cells,
// the modes, the colors, the cursor shape, the title and the working
// directory. Not yet: extra cursors, OSC 133 marks and semantic regions,
// program status, shell contexts and images.

namespace AfterglowVt {

class Terminal;

inline constexpr std::string_view StateCodec = "afterglow.state.v1";

class AFTERGLOWVT_EXPORT StateWriter
{
public:
    /// The next frame for @p terminal: what changed since the last frame,
    /// everything for the first one. Empty when nothing changed. Visible
    /// rows, then history rows, past about @p maxBytes wait for the next
    /// frames, though each frame carries at least one row of each; the
    /// small sections always go.
    std::string update(const Terminal& terminal, std::size_t maxBytes);
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
        /// A hash of each visible row as the replica has it; 0 for a row
        /// that it does not have.
        std::vector<std::uint64_t> rowHashes;
        std::unordered_map<std::uint32_t, Style> styles; ///< the definitions that the replica has
        std::unordered_map<std::uint32_t, std::pair<std::string, std::string>> links;
        bool historyPending = false;
        bool rowsPending = false; ///< changed visible rows wait for a later frame
    };

    /// The visible rows of screen @p which that changed, while @p out stays
    /// within about @p maxBytes. True when the screen started over (a reset
    /// section).
    bool writeRows(std::string& out, const Terminal& terminal, int which, ScreenState& state, std::size_t maxBytes);
    /// The history rows that the replica lacks, while @p out stays within
    /// about @p maxBytes.
    void writeHistory(std::string& out, const Terminal& terminal, int which, ScreenState& state, std::size_t maxBytes);

    ScreenState m_screens[2];
    std::string m_modes;
    std::string m_colors;
    std::string m_text;
    std::string m_cursor;
    bool m_started = false;
};

class AFTERGLOWVT_EXPORT StateReader
{
public:
    /// Applies @p frame to @p replica, a terminal that only this reader
    /// changes. False with @p error for a frame that breaks the codec or does
    /// not fit what the replica has: the replica then needs a frame from a
    /// writer that was reset.
    bool apply(Terminal& replica, std::string_view frame, std::string* error);

private:
    struct ScreenState
    {
        std::uint64_t next = 0; ///< the stable id after the newest history row
        /// The oldest history row that the writer sent, which the replica
        /// may have dropped past its own limit: the next older row is one less.
        std::uint64_t from = 0;
        std::unordered_map<std::uint32_t, Style> styles; ///< the writer's definitions, with the writer's link ids
        std::unordered_map<std::uint32_t, std::pair<std::string, std::string>> links;
        std::unordered_map<std::uint32_t, std::uint32_t> linkCache; ///< the writer's link id to the replica's
    };

    ScreenState m_screens[2];
};

} // namespace AfterglowVt
