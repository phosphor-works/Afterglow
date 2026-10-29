// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/afterglowvt_export.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace AfterglowVt {

/// Kitty's Unicode placeholder: text cells that show a slice of a virtual
/// placement, so images survive multiplexers like tmux.
inline constexpr char32_t PlaceholderCodepoint = 0x10EEEE;
/// The number a placeholder row/column diacritic encodes, or -1.
AFTERGLOWVT_EXPORT int placeholderDiacriticValue(char32_t cp);

class ITerminalHost;
class Screen;

/// One frame of an image, RGBA8, immutable once published so renderers can
/// hold it without a lock.
struct ImageFrame
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
    /// Display time in milliseconds before the next frame. 0 skips the frame.
    std::uint32_t gapMs = 40;
};
using FramePtr = std::shared_ptr<const ImageFrame>;

struct Image
{
    enum class Animation : std::uint8_t { Stopped, Loading, Running };

    std::uint32_t id = 0;
    std::uint32_t number = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<FramePtr> frames;

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
    bool isVirtual = false; ///< for Unicode placeholders, not drawn directly
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
/// placements, in-flight chunked transmissions and animation playback.
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

    /// Handles an APC payload that starts with 'G'.
    void handle(std::string_view payload, Context& ctx);

    /// Visible placements of the viewport, sorted by z then age.
    std::vector<VisiblePlacement> visible(const Screen& screen, int cellWidth, int cellHeight) const;

    /// The slice of a virtual placement that the placeholder cell for grid
    /// position (@p row, @p col) shows, drawn at (@p cellX, @p cellY).
    /// @p placementId 0 takes any virtual placement of the image.
    std::optional<VisiblePlacement> placeholderSlice(std::uint32_t imageId, std::uint32_t placementId, int row,
                                                     int col, int cellX, int cellY, int cellWidth,
                                                     int cellHeight) const;

    /// Advances running animations. Returns true if any frame changed.
    /// @p nextDeadlineMs receives the next time a frame is due (0 for none).
    /// The caller's clock is the only time source: an animation started by a
    /// command begins on the next call.
    bool advanceAnimations(std::uint64_t nowMs, std::uint64_t& nextDeadlineMs);

    /// Drops placements whose top row lies in the active area, plus history
    /// when @p includeHistory (ED 2 / ED 3 / RIS).
    void clearPlacements(const Screen& screen, bool includeHistory);

    /// Reflow renumbers rows. anchorRows() gives each placement's absolute
    /// row for Screen::resize() to track; retarget() applies the result.
    std::vector<std::size_t> anchorRows(const Screen& screen) const;
    void retarget(const Screen& screen, const std::vector<std::size_t>& rows);

    void clear();
    void setQuota(std::size_t bytes) { m_quota = bytes; }

    const std::map<std::uint32_t, Image>& images() const noexcept { return m_images; }
    const std::vector<Placement>& placements() const noexcept { return m_placements; }
    std::size_t totalBytes() const;

private:
    struct Command;
    struct Pending
    {
        std::string controlData; ///< the first chunk's keys
        std::string data;
        bool active = false;
    };

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
    Image* findImage(const Command& cmd);
    void enforceQuota();
    void dropUnusedImage(std::uint32_t id);

    std::map<std::uint32_t, Image> m_images;
    std::vector<Placement> m_placements;
    Pending m_pending;
    std::uint32_t m_nextAutoId = 1u << 31;
    std::uint32_t m_nextPlacementId = 1u << 31;
    std::uint64_t m_sequence = 0;
    std::size_t m_quota = DefaultQuota;
};

} // namespace AfterglowVt
