// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/Selection.h>
#include <AfterglowVt/afterglowvt_export.h>

#include <AfterglowProto/Graphics.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace AfterglowVt {

class ITerminalHost;
class Screen;
class StateReader;
struct Row;

/// One decoded placeholder cell: which image and placement it shows, and
/// which cell of the placement's grid.
struct PlaceholderCell
{
    std::uint32_t imageId = 0;
    std::uint32_t placementId = 0; ///< 0: any virtual placement of the image
    int row = 0;
    int col = 0;
};

/// Decodes kitty's Unicode placeholder cells (AfterglowProto/Placeholder.h)
/// of a row, left to right. The foreground colour carries the image id, the
/// underline colour the placement id, and up to three diacritics the grid
/// row, the grid column and the id's high byte. A cell continues the
/// placeholder just before it when the ids match and each diacritic it has
/// agrees (the same row, the next column, the same high byte); it then takes
/// the ones it leaves out from there. Any other cell starts at row 0,
/// column 0 for what it leaves out.
class AFTERGLOWVT_EXPORT PlaceholderDecoder
{
public:
    /// The placeholder at column @p x, or nullopt for any other cell, which
    /// also ends the run. Call it for each cell of a row in order.
    std::optional<PlaceholderCell> next(const Screen& screen, const Row& row, int x);
    /// Starts a new row.
    void reset() noexcept { m_previous = false; }

private:
    bool m_previous = false;
    PlaceholderCell m_last;
};

/// How a frame holds its pixels. Indexed frames (kitty f=8 and f=16) hold
/// palette indices, so an image follows the colour scheme: a renderer gives
/// each index the pane's colour before it draws the frame.
enum class PixelFormat : std::uint8_t {
    Rgba,
    Indexed, ///< one index for each pixel, opaque
    IndexedAlpha, ///< an index and an alpha for each pixel
};

/// The bytes that a frame of @p format keeps for each pixel.
constexpr std::size_t bytesPerPixel(PixelFormat format) noexcept
{
    return format == PixelFormat::Indexed ? 1 : format == PixelFormat::IndexedAlpha ? 2 : 4;
}

/// One frame of an image, immutable once published so renderers can hold it
/// without a lock.
struct ImageFrame
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PixelFormat format = PixelFormat::Rgba;
    std::vector<std::uint8_t> rgba; ///< Rgba frames only: R, G, B, A for each pixel
    /// Indexed and IndexedAlpha frames only: the index of each pixel, or for
    /// IndexedAlpha the index and then the alpha of each pixel.
    std::vector<std::uint8_t> indices;

    bool indexed() const noexcept { return format != PixelFormat::Rgba; }
    /// The bytes that the frame holds: rgba, or indices for an indexed frame.
    std::vector<std::uint8_t>& pixels() noexcept { return indexed() ? indices : rgba; }
    const std::vector<std::uint8_t>& pixels() const noexcept { return indexed() ? indices : rgba; }
    /// What the frame counts for in the image quota: its RGBA size. An
    /// indexed frame keeps fewer bytes, but it is drawn as RGBA, and the
    /// quota bounds what the images of a screen cost when they show.
    std::size_t byteSize() const noexcept { return indexed() ? std::size_t(width) * height * 4 : rgba.size(); }
};
using FramePtr = std::shared_ptr<const ImageFrame>;

struct Image
{
    enum class Animation : std::uint8_t { Stopped, Loading, Running };

    std::uint32_t id = 0;
    std::uint32_t number = 0;
    bool isSixel = false; ///< internally placed sixel/iTerm2 image, not a kitty protocol ID
    /// Sent with neither i= nor I=: no command can name it again, so it goes
    /// with its last placement, as internal images do.
    bool anonymous = false;
    /// Sent with the transient usage hint (N=1): when room is needed, it goes
    /// before the other images that nothing shows.
    bool transient = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<FramePtr> frames;
    /// Display time in milliseconds of each frame before the next; 0 skips
    /// the frame. Kept apart from the frames, so a timing change copies no
    /// pixels.
    std::vector<std::uint32_t> gaps;

    Animation animation = Animation::Stopped;
    /// Times the animation may wrap to the first frame before it stays on
    /// the last; 0 loops forever. a=a v= sets it, so a finished animation
    /// can go on.
    std::uint32_t maxLoops = 0;
    /// Wraps since a=a last set the state.
    std::uint32_t loopsDone = 0;
    std::size_t currentFrame = 0;
    std::uint64_t frameShownAtMs = 0;
    /// The a=a commands that set the state (s=) and the current frame (c=):
    /// a replica (StateSync.h) runs the animation on its own clock, and
    /// takes the state or the frame of the terminal that it mirrors only for
    /// a command, also one that changes no value there.
    std::uint32_t stateCommands = 0;
    std::uint32_t frameCommands = 0;
    std::uint64_t sequence = 0; ///< transmission order, for eviction
    /// When an image first had this id: placements at one z draw in this
    /// order, as in kitty. Sending the id again keeps it.
    std::uint64_t order = 0;

    std::size_t byteSize() const;
};

struct Placement
{
    std::uint32_t imageId = 0;
    std::uint32_t placementId = 0;
    std::uint64_t row = 0; ///< stable row id of the top-left cell
    int col = 0;
    int offsetX = 0; ///< pixels within the first cell
    int offsetY = 0;
    int srcX = 0;
    int srcY = 0;
    int srcW = 0; ///< 0: to the image edge
    int srcH = 0;
    int cols = 0; ///< requested size in cells, 0: from the pixel size
    int rows = 0;
    std::int32_t z = 0;
    std::uint32_t parentImage = 0, parentPlacement = 0;
    int relativeX = 0, relativeY = 0; ///< signed cell offsets from parent
    bool isVirtual = false; ///< for Unicode placeholders, not drawn directly
    /// Explicit display size in pixels: an iTerm2 image drawn larger than its
    /// stored frame (the renderer scales it), or a placement a region scroll
    /// cut. 0: the source rectangle's size.
    std::uint32_t displayWidth = 0;
    std::uint32_t displayHeight = 0;
};

/// What a renderer needs to draw one placement.
struct VisiblePlacement
{
    FramePtr frame; ///< can be indexed: give it colours before an RGBA upload
    std::uint32_t imageId = 0;
    /// Destination in pixels relative to cell (0, 0) of the viewport.
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    /// Source rectangle in pixels of the frame.
    int srcX = 0;
    int srcY = 0;
    int srcW = 0;
    int srcH = 0;
    std::int32_t z = 0;
};

/// Kitty graphics protocol state for one screen: stored images, their
/// placements, in-flight chunked transmissions and animation playback. It
/// also holds the screen's sixel and iTerm2 images (internal images).
class AFTERGLOWVT_EXPORT GraphicsStore
{
public:
    /// The terminal services a command needs.
    struct Context
    {
        Screen& screen;
        ITerminalHost& host;
        int cellWidth;
        int cellHeight;
        std::function<void(std::string_view)> reply;
        /// Moves the cursor down one line, scrolling at the bottom margin.
        std::function<void()> lineFeed;
    };

    static constexpr std::size_t DefaultQuota = AfterglowProto::GraphicsDefaultQuota;
    /// The largest quota that a terminal of the application takes
    /// (Graphics.quota): what a store of a session's terminal can hold.
    static constexpr std::size_t MaxQuota = std::size_t(4096) << 20;
    static_assert(sizeof(std::size_t) >= 8, "a 4 GiB quota needs a 64-bit size");
    static constexpr std::size_t MaxImages = 4096;
    static constexpr std::size_t MaxPlacements = 4096;
    /// Frame edits (a=f on a frame, a=c) copy a whole frame for a command of
    /// a few bytes, and a file read costs its length for a few bytes of path.
    /// Both run on the thread that feeds the terminal: their bytes are
    /// limited per second, with a burst for one large edit or read.
    static constexpr std::size_t DefaultWorkBurst = std::size_t(1) << 30;
    static constexpr std::size_t DefaultWorkBytesPerSecond = std::size_t(1) << 30;
    static constexpr std::size_t MaxFrames = AfterglowProto::GraphicsMaxFrames;
    /// The most frames of all the images of a store. The quota counts the
    /// pixels, and a frame of one pixel costs far more than its four bytes
    /// to keep, and to mirror in a replica: past this, images are evicted as
    /// past the quota.
    static constexpr std::size_t MaxStoreFrames = 16384;

    /// Handles an APC payload that starts with 'G'.
    void handle(std::string_view payload, Context& ctx);

    /// Visible placements of the viewport in kitty's order: by z, then by
    /// when the image got its id, then by when the placement was made (a
    /// placement that is replaced keeps its place).
    std::vector<VisiblePlacement> visible(const Screen& screen, int cellWidth, int cellHeight, int extraRows = 0) const;

    /// The slice of a virtual placement that the placeholder cell for grid
    /// position (@p row, @p col) shows, drawn at (@p cellX, @p cellY) under
    /// the text and the cursor (z -1, as in kitty). @p placementId 0 takes
    /// any virtual placement of the image. A side of the grid that the
    /// placement leaves out is the image's own size in cells.
    std::optional<VisiblePlacement> placeholderSlice(std::uint32_t imageId, std::uint32_t placementId, int row,
                                                     int col, int cellX, int cellY, int cellWidth,
                                                     int cellHeight) const;

    /// Advances the running animations of images drawn in @p screen's
    /// viewport at the cell size: a placement in the scrollback, or a virtual
    /// one without placeholder cells in view, asks for no frames. Returns
    /// true if any frame changed. @p nextDeadlineMs receives the next time a
    /// frame is due (0 for none), so the caller must call again when the
    /// viewport moves. The caller's clock is the only time source: an
    /// animation started by a command begins on the next call.
    bool advanceAnimations(const Screen& screen, int cellWidth, int cellHeight, std::uint64_t nowMs,
                           std::uint64_t& nextDeadlineMs);
    /// The images that @p screen's viewport draws at the cell size.
    std::unordered_set<std::uint32_t> shownImages(const Screen& screen, int cellWidth, int cellHeight) const;

    /// A clear of the screen (ED 2, DECCOLM): drops the placements that
    /// reach into the active area, and those whose rows have all left the
    /// scrollback. Virtual placements and relative children stay; a child
    /// goes with its parent. The cell size measures how many rows an image
    /// covers; 0 counts only its top row. The alternate screen's clear
    /// (1049, 1047) uses clear(), which drops the images too.
    void clearPlacements(const Screen& screen, int cellWidth = 0, int cellHeight = 0);

    /// An erase of part of the screen (EL, ED 0 and ED 1): drops the
    /// placements whose rows overlap the erased rows, inclusive of both ends,
    /// in visible screen rows (0 is the top). Virtual placements and relative
    /// children stay; a child goes with its parent. The cell size measures how
    /// many rows an image covers; 0 counts only its top row.
    void eraseRows(int firstRow, int lastRow, const Screen& screen, int cellWidth = 0, int cellHeight = 0);

    /// Reflow renumbers rows. anchorRows() and anchorColumns() give each
    /// placement's absolute row and column for Screen::resize() to track;
    /// retarget() applies the result. Both vectors hold one entry per
    /// placement, in order, or retarget() leaves the placements alone. A
    /// placement whose row is gone is dropped. Only when the row ids still
    /// hold (a resize without reflow) does the caller pass the cell size:
    /// a placement anchored above the buffer then stays while its lower rows
    /// are still in it.
    std::vector<std::size_t> anchorRows(const Screen& screen) const;
    std::vector<int> anchorColumns() const;
    void retarget(const Screen& screen, const std::vector<std::size_t>& rows, const std::vector<int>* columns = nullptr,
                  int cellWidth = 0, int cellHeight = 0);
    /// Moves the placements anchored in @p scroll's region with the rows the
    /// scroll moved. A placement keeps only its rows still inside the region
    /// (its source rectangle and size shrink) and goes when none remain.
    /// Rows below the screen's last row are not cut by an upward scroll: they
    /// come into view as it moves, as with a line feed into the history.
    /// The cell size measures the rows an image covers; 0 counts only its
    /// top row, which goes when it leaves. A relative child moves with its
    /// parent and is cut the same way; it goes with its parent. Virtual
    /// placements follow their placeholder cells, so they are not moved here.
    void scrollRegion(const RegionScroll& scroll, const Screen& screen, int cellWidth, int cellHeight);

    /// Drops every image, placement and unfinished upload: a reset, or the
    /// alternate screen's clear, after which no id names an old image.
    void clear();
    /// Sets the bytes of image data kept; a lower quota evicts at once.
    void setQuota(std::size_t bytes);
    /// Whether transmissions may read files, temporary files and shared
    /// memory (t=f, t=t, t=s). Off, they are refused and nothing is opened.
    void setFileTransfer(bool allowed) noexcept { m_fileTransfer = allowed; }
    /// The $TMPDIR of the programs (Terminal::setTemporaryDirectory).
    void setTemporaryDirectory(std::string directory) { m_temporaryDirectory = std::move(directory); }
    /// Sets the work budget (chargeWork()) and fills it: @p burst bytes at
    /// once, refilled at @p bytesPerSecond.
    void setWorkBudget(std::size_t burst, std::size_t bytesPerSecond);

    /// Stores an internal image (sixel, or an iTerm2 inline image) placed
    /// with its top-left at visible cell (@p col, @p row). Earlier internal
    /// images it covers entirely are dropped. @p displayWidth and
    /// @p displayHeight, when not 0, give the size in pixels to draw it at.
    /// An image larger than the quota is not stored, and replaces nothing.
    void addSixel(ImageFrame frame, const Screen& screen, int row, int col, int cellWidth, int cellHeight,
                  std::uint32_t displayWidth = 0, std::uint32_t displayHeight = 0);

    const std::map<std::uint32_t, Image>& images() const noexcept { return m_images; }
    const std::vector<Placement>& placements() const noexcept { return m_placements; }
    std::size_t totalBytes() const;
    /// Grows with each change that may have changed the images or the
    /// placements: a writer of the codec (StateSync.h) looks at a store
    /// again only when it grew.
    std::uint64_t changes() const noexcept { return m_changes; }

private:
    /// A replica takes the images and placements of the terminal that it
    /// mirrors (StateSync.h). The codec is part of this library: it keeps the
    /// invariants of the store itself (m_virtual through indexVirtual()).
    friend class StateReader;
    struct Command;
    class Origins;
    struct Pending
    {
        /// The first chunk's keys, parsed once. A later chunk's q= replaces
        /// its q, as in kitty.
        AfterglowProto::GraphicsCommand first;
        std::string data;
        /// Decoded bytes received, also those past what data keeps.
        std::size_t received = 0;
        bool active = false;
        bool failed = false;
    };

    /// False when the command changed nothing that needs settling: a chunk
    /// that only added to a pending transmission, a feature query, or a
    /// command refused before anything ran.
    bool dispatch(std::string_view payload, Context& ctx);
    void execute(const Command& cmd, std::string data, Context& ctx);
    /// Reads the payload from its medium and inflates o=z data, which may
    /// not grow past @p inflateLimit bytes. A file or shared memory object
    /// gives at most @p readLimit bytes: raw pixels past it are not read,
    /// and longer compressed or PNG data is refused. Every failure to read
    /// one gets the same error. The bytes to read count against the work
    /// budget, and a read the budget refuses gets EBUSY.
    bool loadPayload(const Command& cmd, std::string& data, std::size_t readLimit, std::size_t inflateLimit,
                     std::string& error);
    bool decodePixels(const Command& cmd, std::string data, Context& ctx, ImageFrame& out, std::string& error);
    void transmit(const Command& cmd, std::string data, Context& ctx);
    /// Places @p image, or the image the command names when it is null.
    void place(const Command& cmd, Context& ctx, Image* image = nullptr);
    void remove(const Command& cmd, Context& ctx);
    void frame(const Command& cmd, std::string data, Context& ctx);
    void animate(const Command& cmd, Context& ctx);
    void compose(const Command& cmd, Context& ctx);
    /// d=f and d=F: deletes frame r= (the last when past it) of an image.
    void deleteFrame(const Command& cmd, bool freeImage);
    /// A query with e= (a feature query): the keys of its action that the
    /// store supports, with no other effect.
    void featureQuery(const Command& cmd, Context& ctx);
    /// Answers a command that names an image; an a=f or a=a reply also
    /// names the frame (r=).
    void respond(const Command& cmd, Context& ctx, std::string_view status, bool isError);
    /// Deletes the file or shared memory a refused t=t or t=s command names,
    /// when the protocol hands it to the terminal, without reading it.
    /// Returns whether it deleted one.
    bool releaseMedium(const Command& cmd, const std::string& data) const;
    Image* findImage(const Command& cmd);
    /// Rows a placement covers at the given cell size (1 when unknown).
    long long rowsCovered(const Placement& p, int cellWidth, int cellHeight) const;
    /// Drops placements whose rows have all left the scrollback.
    void pruneDead(const Screen& screen, int cellWidth, int cellHeight);
    void pruneOrphans();
    /// Evicts images until the quota, the image limit and MaxStoreFrames
    /// hold, unused ones and older ones first. Image @p keep (being added
    /// to) is never evicted.
    void enforceQuota(std::uint32_t keep = 0);
    /// Evicts the oldest image without placements. False if there is none.
    bool evictUnusedImage();
    void dropUnusedImage(std::uint32_t id);
    /// After each change: drops orphans and the images nothing can name or
    /// show any more, and rebuilds the lookups that queries use.
    void settle();
    /// Rebuilds m_virtual from the placements.
    void indexVirtual();
    /// Charges @p bytes of frame copying, or of a file or shared memory read,
    /// against a budget that refills over time. Both run on the thread that
    /// feeds the terminal. False when the budget is spent.
    bool chargeWork(std::size_t bytes);
    /// A placement id for a command without p=, unused by image @p imageId.
    std::uint32_t allocatePlacementId(std::uint32_t imageId);
    /// An id for an internal image, or for an anonymous one that a command
    /// claims the id of: free, and preferably out of the way of clients.
    std::uint32_t allocateInternalId();

    std::map<std::uint32_t, Image> m_images;
    std::vector<Placement> m_placements;
    /// Virtual placements by (image, placement), and by (image, 0) for the
    /// first of an image: placeholder cells look them up once each.
    std::unordered_map<std::uint64_t, std::size_t> m_virtual;
    Pending m_pending;
    std::uint32_t m_nextAutoId = 1u << 31;
    std::uint32_t m_nextPlacementId = 1u << 31;
    std::uint32_t m_nextInternalId = 3u << 29; ///< preferred range; occupied IDs are skipped
    std::uint64_t m_sequence = 0;
    std::uint64_t m_changes = 0;
    std::size_t m_quota = DefaultQuota;
    bool m_fileTransfer = true;
    std::string m_temporaryDirectory;
    double m_workBytes = -1; ///< work budget left; below 0 before first use
    double m_workBurst = double(DefaultWorkBurst);
    double m_workRate = double(DefaultWorkBytesPerSecond);
    std::int64_t m_workStampNs = 0;
};

} // namespace AfterglowVt
