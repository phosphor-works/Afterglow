// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/Graphics.h>
#include <AfterglowVt/Selection.h>
#include <AfterglowVt/Style.h>
#include <AfterglowVt/afterglowvt_export.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// The state of a terminal, carried to a replica in another process: the
// codec afterglow.state.v1 of the mux. The daemon parses the program's
// output into its terminal. A StateWriter turns what changed into a frame,
// and a StateReader applies the frame to a replica Terminal in the window,
// which never sees the program's bytes. The window then draws, selects and
// searches the replica as it does a terminal of its own.
//
// A writer remembers what each frame told the replica: every frame that
// update() or updateHeld() returns must reach the reader, in order, or the
// writer must be reset() for a new replica. A frame that waits for room in
// its channel waits whole, and goes before the next one. A replica that
// nobody sees can take frames without the screens (updateHeld()), which
// catch up at the next update().
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
// program status, the shell contexts, the region scrolls since the last
// frame, the cell size, and the images with their placements: the pixels of
// each frame go once, in parts, and an image shows once all of its frames
// came.

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
    /// images are in the budget too: the pixels of their frames, their
    /// records, their placements and the frames that went, each of which
    /// goes past it by its own bytes at most when the frame has nothing else.
    /// The other sections are not in the budget, and go whole: together they
    /// are under 1 MiB (the bounds of the status records, the contexts, the
    /// semantic regions, the extra cursors, the title and the scrolls).
    /// @p scrolls: the region scrolls since the last update(), held frames
    /// included, which the first update() leaves out: a new replica keeps
    /// nothing that they move.
    std::string update(const Terminal& terminal, std::size_t maxBytes, const RegionScrolls* scrolls = nullptr);
    /// The next frame for a replica that nobody sees: update() without the
    /// screens. It carries what a host shows of a terminal that it does not
    /// draw, when it changed: the modes, the colors, the title and the
    /// working directory, the OSC 133 marks, the program status, the shell
    /// contexts and the extra cursors. The rows, the history, the cursors,
    /// the cell size and the images wait, and so do the region scrolls: the
    /// caller keeps them for the next update(), which sends the screens as
    /// they are then. Empty when none of those sections changed.
    std::string updateHeld(const Terminal& terminal);
    /// The most history rows that the replica keeps: the writer sends no
    /// older ones. 0 sends none.
    void setReplicaHistory(std::size_t rows) { m_replicaHistory = rows; }
    /// The writer starts over, as a new one does: for a new replica. The
    /// next frame carries everything that it carries (an updateHeld() all
    /// but the screens), and the next update() the screens and the images.
    void reset();
    /// Visible or history rows, or the pixels, records, placements or
    /// dropped frames of images, that wait for a later frame.
    bool pending() const;
    /// The rows of the screens that wait for a later frame: pending()
    /// without history and images, which a program that floods its pane
    /// keeps waiting while it writes.
    bool rowsPending() const;
    /// The next update() takes the rows of the screens and of the history
    /// as the terminal has them then: textMarked() says when the frames
    /// carried them. A mark replaces the one before it.
    void markText()
    {
        m_mark = TextMark();
        m_mark.wanted = true;
    }
    /// The frames carried the text that markText() took: the rows of both
    /// screens, also those that scrolled into the history since, and the
    /// history rows of the terminal at the mark that the replica keeps
    /// (setReplicaHistory). What a read of the replica's text needs. Rows
    /// that scrolled in after the mark may still wait. Older history that
    /// the replica never had (a new link's) goes only once those caught up:
    /// a program that floods its pane faster than the frames go keeps such
    /// a read waiting until it stops, or until its rows pass what the
    /// replica keeps. False until an update() took the mark.
    bool textMarked() const;
    /// The terminal changed in an updateHeld() since the last update(), or
    /// no update() ran yet: the screens of the replica are behind until the
    /// next update().
    bool screensHeld() const { return m_screensHeld || !m_screensStarted; }

private:
    /// update() and updateHeld(): @p held leaves the screens for later.
    std::string write(const Terminal& terminal, std::size_t maxBytes, const RegionScrolls* scrolls, bool held);
    /// Ends what the writer holds of the images that went from the
    /// terminal: the version whose pixels go, and those whose records wait.
    /// Also while the screens are held: their frames are free at once.
    void releaseGoneImages(const Terminal& terminal);

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
    /// Before the images of a frame: what the replica must drop, and the
    /// frames that are new. False when nothing of the images is to send:
    /// neither store changed (GraphicsStore::changes()) and nothing waits.
    bool prepareImages(std::string& out, const Terminal& terminal, bool first);
    /// The pixels of the frames of one image after another, in parts within
    /// @p budget. @p shown: only those of an image that shows on the active
    /// screen, which go before the history; the others go after it.
    void writePixels(std::string& out, const Terminal& terminal, Budget& budget, bool shown);
    /// The records of the images whose frames the replica has, those that
    /// went, and the placements of the images that the replica has.
    void writeRecords(std::string& out, const Terminal& terminal, Budget& budget);
    /// The frames that the replica got a part of and no image holds any
    /// more: before any frame starts, as the replica builds one at a time.
    void writeDrops(std::string& out, Budget& budget);
    /// Whether the pixels of all of @p image's frames went.
    bool complete(const Image& image) const;
    /// Ends the transfer: the frame that the replica builds of it goes.
    void endTransfer();
    /// Whether an item of @p bytes fits the frame: within @p budget, or,
    /// @p first of its section, as the first of a frame that has nothing
    /// else, so that the images move at any budget.
    bool fits(const std::string& out, const Budget& budget, std::size_t bytes, bool first) const;

    /// A frame of an image as the replica has it, by its address while it
    /// lives: a frame is immutable once an image holds it. The writer keeps
    /// it alive only while it sends it (Transfer).
    struct SentFrame
    {
        std::weak_ptr<const ImageFrame> frame;
        std::uint64_t id = 0;
        std::size_t sent = 0; ///< its bytes that went
    };
    std::unordered_map<const ImageFrame*, SentFrame> m_frames;
    std::uint64_t m_nextFrameId = 1;
    /// A version of an image whose frames go, one image at a time: the
    /// replica builds one frame at a time. It holds the frames, so that the
    /// version finishes, and shows, also when the program replaces it while
    /// its pixels go: an image that a program sends again faster than its
    /// pixels go still shows, at the pace that the channel takes.
    struct ImageVersion
    {
        int which = 0;
        Image image;
        bool recorded = false; ///< the replica has its record
    };
    std::optional<ImageVersion> m_transfer;
    /// Versions whose pixels went and whose records wait for room.
    std::vector<ImageVersion> m_done;
    /// The ids of frames that the replica drops, which wait for room.
    std::vector<std::uint64_t> m_drops;
    /// A frame that the replica builds is among m_drops: no frame starts
    /// before they went.
    bool m_dropsBuilding = false;
    /// The record of each image of a screen as the replica has it.
    std::unordered_map<std::uint32_t, std::string> m_images[2];
    std::string m_placements[2]; ///< the placements of each screen as the replica has them
    /// The images that show on the active screen, at the last prepareImages().
    std::unordered_set<std::uint32_t> m_shown;
    std::uint64_t m_storeChanges[2] = {~std::uint64_t(0), ~std::uint64_t(0)};
    bool m_imagesPending = false; ///< pixels, records, placements or drops wait for a later frame
    bool m_imagesWent = false; ///< this frame has an image section
    bool m_storesChanged = false; ///< a store changed since the last frame
    bool m_recordsWaiting = false; ///< a record or the placements wait for room, or a frame's pixels completed

    ScreenState m_screens[2];
    /// markText(): the rows of each screen that the text needs, in stable
    /// ids, from floor to total: the history, then the visible rows.
    struct TextMark
    {
        bool wanted = false; ///< the next update() takes it
        bool taken = false;
        std::uint64_t total[2] = {0, 0};
        std::uint64_t floor[2] = {0, 0};
    };
    TextMark m_mark;
    std::string m_modes;
    std::string m_colors;
    std::string m_text;
    std::string m_cursor;
    std::string m_shell;
    std::string m_status;
    std::string m_contexts;
    std::string m_extraCursors;
    std::string m_cellSize;
    std::uint64_t m_generation = 0; ///< Terminal::generation() at the last frame
    std::uint64_t m_statusSerial = 0; ///< ProgramStatus::serial() at the last frame
    std::size_t m_replicaHistory = ~std::size_t(0);
    bool m_started = false; ///< a frame went
    bool m_screensStarted = false; ///< an update() went: the screens of the replica started
    bool m_screensHeld = false; ///< the terminal changed in an updateHeld() since the last update()
};

class AFTERGLOWVT_EXPORT StateReader
{
public:
    /// Applies @p frame to @p replica, a terminal that only this reader
    /// changes. False with @p error for a frame that breaks the codec or does
    /// not fit what the replica has: the replica then needs the frames of a
    /// writer that was reset, whose first update() starts the screens and
    /// the images over (its held frames before it carry neither). The
    /// frames of images that the reader keeps are bounded by four times what
    /// a store holds (GraphicsStore::MaxQuota and MaxStoreFrames): what both
    /// screens show, and what comes for them. The replica's host hears of
    /// the scrolls, the status and the contexts of a frame once all of it
    /// applied, and of none for a frame that fails.
    bool apply(Terminal& replica, std::string_view frame, std::string* error);
    /// The last frame that applied changed the visible rows, started a
    /// screen over, or changed the images or the placements of the screen
    /// that shows: output of the program, not history that catches up.
    bool screenChanged() const { return m_screenChanged; }
    /// The frames of images that the writer sent and did not drop yet.
    std::size_t frames() const { return m_frames.size(); }
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

    /// A frame of an image that comes in parts (FrameTag). The writer
    /// finishes each frame, or drops it, before it starts the next.
    struct ReceivedFrame
    {
        std::shared_ptr<ImageFrame> frame;
        std::size_t size = 0;
        std::size_t received = 0;
        bool complete() const { return received == size; }
    };
    /// What the writer last sent of an image's animation. The replica runs
    /// the animation on its own clock: it takes the state or the current
    /// frame only for a command of the program (Image::stateCommands and
    /// frameCommands), and starts over for an image sent again (sequence).
    struct Animation
    {
        std::uint32_t stateCommands = 0;
        std::uint32_t frameCommands = 0;
        std::uint64_t sequence = 0;
    };

    ScreenState m_screens[2];
    Epoch m_epochs[2];
    bool m_screenChanged = false;
    std::unordered_map<std::uint64_t, ReceivedFrame> m_frames;
    std::size_t m_frameBytes = 0; ///< the pixels of m_frames
    std::uint64_t m_building = 0; ///< the frame whose parts still come, or 0
    std::unordered_map<std::uint32_t, Animation> m_animations[2];
};

} // namespace AfterglowVt
