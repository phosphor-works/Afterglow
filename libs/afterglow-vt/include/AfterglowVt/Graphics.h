// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/Selection.h>
#include <AfterglowVt/afterglowvt_export.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace AfterglowVt {

/// Kitty's Unicode placeholder: text cells that show a slice of a virtual
/// placement, so images survive multiplexers like tmux.
inline constexpr char32_t PlaceholderCodepoint = 0x10EEEE;
/// The number a placeholder row/column diacritic encodes, or -1.
AFTERGLOWVT_EXPORT int placeholderDiacriticValue(char32_t cp);

class ITerminalHost;
class Screen;
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

/// Decodes the Unicode placeholder cells of a row, left to right. The
/// foreground colour carries the image id, the underline colour the
/// placement id, and up to three diacritics the grid row, the grid column
/// and the id's high byte. A cell that leaves them out continues the
/// placeholder just before it.
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

/// One frame of an image, RGBA8, immutable once published so renderers can
/// hold it without a lock.
struct ImageFrame
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
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
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<FramePtr> frames;
    /// Display time in milliseconds of each frame before the next; 0 skips
    /// the frame. Kept apart from the frames, so a timing change copies no
    /// pixels.
    std::vector<std::uint32_t> gaps;

    Animation animation = Animation::Stopped;
    /// Loops still to play; 0 loops forever.
    std::uint32_t loopsLeft = 0;
    std::size_t currentFrame = 0;
    std::uint64_t frameShownAtMs = 0;
    std::uint64_t sequence = 0; ///< transmission order, for eviction

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
    /// Pixels to draw an internal image at, when that differs from its frame
    /// (iTerm2 sizes are scaled by the renderer, not stored scaled). 0: the
    /// frame's size.
    std::uint32_t displayWidth = 0;
    std::uint32_t displayHeight = 0;
};

/// What a renderer needs to draw one placement.
struct VisiblePlacement
{
    FramePtr frame;
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

    static constexpr std::size_t DefaultQuota = std::size_t(320) << 20;
    static constexpr std::size_t MaxImages = 4096;
    static constexpr std::size_t MaxPlacements = 4096;
    static constexpr std::size_t MaxFrames = 1024;

    /// Handles an APC payload that starts with 'G'.
    void handle(std::string_view payload, Context& ctx);

    /// Visible placements of the viewport, sorted by z then age.
    std::vector<VisiblePlacement> visible(const Screen& screen, int cellWidth, int cellHeight, int extraRows = 0) const;

    /// The slice of a virtual placement that the placeholder cell for grid
    /// position (@p row, @p col) shows, drawn at (@p cellX, @p cellY).
    /// @p placementId 0 takes any virtual placement of the image.
    std::optional<VisiblePlacement> placeholderSlice(std::uint32_t imageId, std::uint32_t placementId, int row,
                                                     int col, int cellX, int cellY, int cellWidth,
                                                     int cellHeight) const;

    /// Advances running animations of images that have a placement. Returns
    /// true if any frame changed. @p nextDeadlineMs receives the next time a
    /// frame is due (0 for none). The caller's clock is the only time source:
    /// an animation started by a command begins on the next call.
    bool advanceAnimations(std::uint64_t nowMs, std::uint64_t& nextDeadlineMs);

    /// Without @p includeHistory (ED 2): drops the placements that reach into
    /// the active area, and those whose rows have all left the scrollback.
    /// Virtual placements and relative children stay; a child goes with its
    /// parent. The cell size measures how many rows an image covers; 0 counts
    /// only its top row. With @p includeHistory (the alternate screen's
    /// lifecycle): drops every placement.
    void clearPlacements(const Screen& screen, bool includeHistory, int cellWidth = 0, int cellHeight = 0);

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
    /// top row, which goes when it leaves. Virtual placements follow their
    /// placeholder cells, and relative children their parents, so neither is
    /// moved here.
    void scrollRegion(const RegionScroll& scroll, const Screen& screen, int cellWidth, int cellHeight);

    void clear();
    /// Sets the bytes of image data kept; a lower quota evicts at once.
    void setQuota(std::size_t bytes);
    /// Whether transmissions may read files, temporary files and shared
    /// memory (t=f, t=t, t=s). Off, they are refused and nothing is opened.
    void setFileTransfer(bool allowed) noexcept { m_fileTransfer = allowed; }

    /// Stores an internal image (sixel, or an iTerm2 inline image) placed
    /// with its top-left at visible cell (@p col, @p row). Earlier internal
    /// images it covers entirely are dropped. @p displayWidth and
    /// @p displayHeight, when not 0, give the size in pixels to draw it at.
    void addSixel(ImageFrame frame, const Screen& screen, int row, int col, int cellWidth, int cellHeight,
                  std::uint32_t displayWidth = 0, std::uint32_t displayHeight = 0);

    const std::map<std::uint32_t, Image>& images() const noexcept { return m_images; }
    const std::vector<Placement>& placements() const noexcept { return m_placements; }
    std::size_t totalBytes() const;

private:
    struct Command;
    class Origins;
    struct Pending
    {
        std::string controlData; ///< the first chunk's keys
        std::string data;
        bool active = false;
        bool failed = false;
    };

    void dispatch(std::string_view payload, Context& ctx);
    void execute(const Command& cmd, std::string data, Context& ctx);
    bool loadPayload(const Command& cmd, std::string& data, std::string& error);
    bool decodePixels(const Command& cmd, std::string data, Context& ctx, ImageFrame& out, std::string& error);
    void transmit(const Command& cmd, std::string data, Context& ctx);
    void place(const Command& cmd, Context& ctx);
    void remove(const Command& cmd, Context& ctx);
    void frame(const Command& cmd, std::string data, Context& ctx);
    void animate(const Command& cmd, Context& ctx);
    void compose(const Command& cmd, Context& ctx);
    void respond(const Command& cmd, Context& ctx, std::string_view status, bool isError);
    /// Deletes the file or shared memory a refused t=t or t=s command names,
    /// when the protocol hands it to the terminal, without reading it.
    void releaseMedium(const Command& cmd, const std::string& data) const;
    Image* findImage(const Command& cmd);
    /// Rows a placement covers at the given cell size (1 when unknown).
    long long rowsCovered(const Placement& p, int cellWidth, int cellHeight) const;
    /// Drops placements whose rows have all left the scrollback.
    void pruneDead(const Screen& screen, int cellWidth, int cellHeight);
    void pruneOrphans();
    /// Evicts images until the quota and the image limit hold, unused ones
    /// and older ones first. Image @p keep (being added to) is never evicted.
    void enforceQuota(std::uint32_t keep = 0);
    /// Evicts the oldest image without placements. False if there is none.
    bool evictUnusedImage();
    void dropUnusedImage(std::uint32_t id);
    /// After each change: drops orphans and the images nothing can name or
    /// show any more, and rebuilds the lookups that queries use.
    void settle();
    /// Charges @p bytes of frame copying against a budget that refills over
    /// time. False when the budget is spent.
    bool chargeWork(std::size_t bytes);
    std::uint32_t allocateSixelId();

    std::map<std::uint32_t, Image> m_images;
    std::vector<Placement> m_placements;
    /// Virtual placements by (image, placement), and by (image, 0) for the
    /// first of an image: placeholder cells look them up once each.
    std::unordered_map<std::uint64_t, std::size_t> m_virtual;
    Pending m_pending;
    std::uint32_t m_nextAutoId = 1u << 31;
    std::uint32_t m_nextPlacementId = 1u << 31;
    std::uint32_t m_nextSixelId = 3u << 29; ///< preferred range; occupied IDs are skipped
    std::uint64_t m_sequence = 0;
    std::size_t m_quota = DefaultQuota;
    bool m_fileTransfer = true;
    double m_workBytes = -1; ///< frame-copy budget left; below 0 before first use
    std::int64_t m_workStampNs = 0;
};

} // namespace AfterglowVt
