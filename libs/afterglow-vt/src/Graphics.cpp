// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Graphics.h>

#include <AfterglowProto/Base64.h>
#include <AfterglowProto/Graphics.h>
#include <AfterglowProto/Placeholder.h>
#include <AfterglowVt/Screen.h>
#include <AfterglowVt/Terminal.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cmath>
#include <filesystem>
#include <type_traits>
#include <unordered_set>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace AfterglowVt {

namespace {

constexpr std::uint32_t MaxDimension = AfterglowProto::GraphicsMaxSide;
constexpr std::size_t MaxPayload = GraphicsStore::DefaultQuota;
constexpr std::size_t MaxControl = 4096;
constexpr int MaxDepth = 32; ///< relative placement chain links
/// The gap of a new frame without z=, as in kitty. The root frame has none.
constexpr std::uint32_t DefaultFrameGap = 40;
/// The one answer to every failure to read a file or shared memory object.
/// The client can be remote: the answer must not tell it whether a file it
/// cannot read exists, what kind it is or how long it is.
constexpr std::string_view FileError = "EBADF:failed to read image file";

/// The palette indices of f=8 (index) and f=16 (index and alpha).
constexpr bool isIndexedFormat(std::uint32_t format)
{
    return format == 8 || format == 16;
}

/// Pixels that the command gives byte by byte, with s and v: RGB, RGBA, and
/// the indexed formats.
constexpr bool isRawFormat(std::uint32_t format)
{
    return format == 24 || format == 32 || isIndexedFormat(format);
}

/// The bytes that a transfer in chunks keeps. decodePixels() uses only the
/// s by v pixels of uncompressed raw data, so the buffer stops there, not at
/// the payload limit. Other data has the payload limit.
std::size_t chunkedKeep(const AfterglowProto::GraphicsCommand& cmd)
{
    if (cmd.medium != 'd' || cmd.compression != 0 || !isRawFormat(cmd.format) || cmd.s == 0 || cmd.v == 0
        || cmd.s > MaxDimension || cmd.v > MaxDimension) {
        return MaxPayload;
    }
    return std::min(MaxPayload, std::size_t(cmd.s) * cmd.v * (cmd.format / 8));
}

/// Cuts @p data to @p keep bytes. resize() keeps the capacity, so a chunk
/// far longer than the image gives its memory back, which would otherwise
/// stay until the transfer ends. Shorter cuts keep it: a copy on every
/// chunk of a large image would cost more than it saves. GraphicsChunk
/// base64 bytes decode to fewer, so one chunk of slack never copies.
void keepOnly(std::string& data, std::size_t keep)
{
    if (data.size() <= keep) return;
    data.resize(keep);
    if (data.capacity() > 2 * keep + AfterglowProto::GraphicsChunk) std::string(data).swap(data);
}

/// The answer to a command over the work budget. It asks for a retry only
/// when the command can be sent again as it was: not when the refusal
/// deleted its medium (@p released), the temporary file of a t=t command or
/// the tagged shared memory object of a t=s command.
std::string busyError(bool released, std::string_view what)
{
    std::string error = "EBUSY:too much ";
    error += what;
    if (!released) error += ", try again later";
    return error;
}

void blendOver(std::uint8_t* dst, const std::uint8_t* src)
{
    const unsigned a = src[3];
    if (a == 255) {
        std::copy_n(src, 4, dst);
        return;
    }
    if (a == 0) {
        return;
    }
    const unsigned inv = 255 - a;
    const unsigned outA = a + dst[3] * inv / 255;
    for (int c = 0; c < 3; ++c) {
        const unsigned v = outA ? (src[c] * a + dst[c] * dst[3] * inv / 255) / outA : 0;
        dst[c] = std::uint8_t(std::min(255u, v));
    }
    dst[3] = std::uint8_t(outA);
}

/// Copies a rectangle of @p src into @p dst at (dx, dy), clipped. A width or
/// height of 0 reaches the edge of @p src.
void blit(ImageFrame& dst, const ImageFrame& src, int sx, int sy, int w, int h, int dx, int dy, bool overwrite)
{
    // frame() and compose() refuse indexed images: they have no RGBA to copy.
    if (dst.indexed() || src.indexed()) {
        return;
    }
    const auto width = w > 0 ? std::int64_t(w) : std::int64_t(src.width) - sx;
    const auto height = h > 0 ? std::int64_t(h) : std::int64_t(src.height) - sy;
    const auto firstRow = std::max({std::int64_t(0), -std::int64_t(sy), -std::int64_t(dy)});
    const auto lastRow = std::min({height, std::int64_t(src.height) - sy, std::int64_t(dst.height) - dy});
    const auto firstCol = std::max({std::int64_t(0), -std::int64_t(sx), -std::int64_t(dx)});
    const auto lastCol = std::min({width, std::int64_t(src.width) - sx, std::int64_t(dst.width) - dx});
    for (auto row = firstRow; row < lastRow; ++row) {
        for (auto col = firstCol; col < lastCol; ++col) {
            const auto* sp = &src.rgba[(std::size_t(sy + row) * src.width + std::size_t(sx + col)) * 4];
            auto* dp = &dst.rgba[(std::size_t(dy + row) * dst.width + std::size_t(dx + col)) * 4];
            if (overwrite) std::copy_n(sp, 4, dp);
            else blendOver(dp, sp);
        }
    }
}

struct DisplaySize
{
    int width;
    int height;
};

/// The part of the image a placement shows, clipped to the image.
struct SourceRect
{
    std::int64_t x, y, width, height;
};

SourceRect sourceRect(const Placement& pl, const Image& img)
{
    const auto x = std::min<std::int64_t>(pl.srcX, img.width);
    const auto y = std::min<std::int64_t>(pl.srcY, img.height);
    const auto restW = std::int64_t(img.width) - x;
    const auto restH = std::int64_t(img.height) - y;
    return {x, y, pl.srcW > 0 ? std::min<std::int64_t>(pl.srcW, restW) : restW,
            pl.srcH > 0 ? std::min<std::int64_t>(pl.srcH, restH) : restH};
}

DisplaySize displaySize(const Placement& pl, const Image& img, int cw, int ch)
{
    const SourceRect src = sourceRect(pl, img);
    const auto srcW = pl.displayWidth ? std::int64_t(pl.displayWidth) : src.width;
    const auto srcH = pl.displayHeight ? std::int64_t(pl.displayHeight) : src.height;
    constexpr auto limit = std::int64_t(1) << 28;
    std::int64_t w = srcW, h = srcH;
    if (pl.cols > 0) w = std::int64_t(pl.cols) * cw;
    if (pl.rows > 0) h = std::int64_t(pl.rows) * ch;
    if (pl.cols > 0 && pl.rows <= 0) h = srcW > 0 ? std::int64_t(std::clamp(double(srcH) * double(w) / double(srcW), 0.0, double(limit))) : 0;
    if (pl.rows > 0 && pl.cols <= 0) w = srcH > 0 ? std::int64_t(std::clamp(double(srcW) * double(h) / double(srcH), 0.0, double(limit))) : 0;
    return {int(std::clamp(w, std::int64_t(0), limit)), int(std::clamp(h, std::int64_t(0), limit))};
}

std::uint64_t placementKey(std::uint32_t image, std::uint32_t placement)
{
    return (std::uint64_t(image) << 32) | placement;
}

/// A placement's pixel offset inside its first cell. It stays below the cell
/// size, as in kitty, also when the cell size changes after the placement.
int cellOffset(int offset, int cell)
{
    return cell > 0 ? std::clamp(offset, 0, cell - 1) : std::clamp(offset, 0, 1 << 28);
}

/// The stable row of a non-virtual root placement as a signed absolute
/// index: negative once the row has left the scrollback, while the image's
/// lower rows can still be on screen.
long long anchorIndex(const Placement& p, const Screen& s)
{
    return static_cast<long long>(p.row) - static_cast<long long>(s.rowsDropped());
}

bool forbiddenFile(const std::filesystem::path& path)
{
    const std::string name = path.string();
    // /dev/shm holds plain files: the protocol names it as a temporary directory.
    if (name.starts_with("/dev/shm/")) return false;
    for (const char* deny : {"/proc", "/sys", "/dev"}) {
        if (name == deny || name.starts_with(std::string(deny) + '/')) return true;
    }
    return false;
}

enum class ReadResult { Done, Failed, Busy };

/// Reads @p size bytes (0: the rest of the file) from @p offset. Past
/// @p limit, the read stops there when @p cut is set (raw pixels, of which
/// no more are used) and fails otherwise. The length comes from fstat before
/// anything is read, and @p charge must accept it: Busy when it does not.
///
/// The read runs on the thread that feeds the terminal, under the pane's
/// lock. The budget bounds how many bytes it reads, not how long a stalled
/// mount (a hard NFS mount, a FUSE server that does not answer) keeps that
/// thread waiting: the pane stops until the read returns. Turning file
/// transmission off is the remedy for a user with such mounts.
template<typename Charge>
ReadResult readPayload(int fd, std::uint32_t offset, std::uint32_t size, std::size_t limit, bool cut,
                       std::string& data, Charge&& charge)
{
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0
        || std::uint64_t(offset) > std::uint64_t(st.st_size)) {
        return ReadResult::Failed;
    }
    const std::uint64_t remaining = std::uint64_t(st.st_size) - offset;
    std::uint64_t want = size ? std::min<std::uint64_t>(size, remaining) : remaining;
    if (cut) {
        want = std::min<std::uint64_t>(want, limit);
    } else if (want > limit || size > limit) {
        return ReadResult::Failed;
    }
    if (!charge(std::size_t(want))) {
        return ReadResult::Busy;
    }
    data.resize(std::size_t(want));
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = pread(fd, data.data() + done, std::min<std::size_t>(65536, data.size() - done), off_t(offset) + off_t(done));
        if (n > 0) done += std::size_t(n);
        else if (n < 0 && errno == EINTR) continue;
        else if (n == 0) { data.resize(done); break; }
        else { data.clear(); return ReadResult::Failed; }
    }
    return ReadResult::Done;
}

bool insideDirectory(const std::filesystem::path& path, const std::filesystem::path& directory)
{
    auto p = path.begin();
    for (auto d = directory.begin(); d != directory.end(); ++d, ++p)
        if (p == path.end() || *p != *d) return false;
    return p != path.end();
}

/// Whether the protocol hands the file at resolved @p path to the terminal to
/// delete: it lies in /tmp, /dev/shm or the programs' $TMPDIR, @p programs
/// or else the terminal's own (each resolved, so a trailing slash or a
/// symbolic link in the setting still matches), and its path below that
/// directory carries the tag. This is stricter than kitty, which looks for
/// the tag in the whole path: with a $TMPDIR whose own name holds the tag, a
/// command could name any regular file in it for the terminal to delete.
bool temporaryFile(const std::filesystem::path& path, const std::string& programs)
{
    std::error_code ec;
    std::vector<std::filesystem::path> dirs{"/tmp", "/dev/shm"};
    if (!programs.empty()) {
        dirs.emplace_back(programs);
    } else if (auto tmp = std::filesystem::temp_directory_path(ec); !ec && !tmp.empty()) {
        dirs.push_back(std::move(tmp));
    }
    for (const auto& dir : dirs) {
        std::error_code resolved;
        const auto real = std::filesystem::canonical(dir, resolved);
        if (!resolved && insideDirectory(path, real)
            && path.lexically_relative(real).string().find(AfterglowProto::GraphicsTemporaryTag) != std::string::npos) {
            return true;
        }
    }
    return false;
}

/// Deletes @p name in the directory @p parent, whose real path is
/// @p parentPath, when the protocol hands it to the terminal: a regular file
/// in a temporary directory, with the tag in its path (temporaryFile(), with
/// @p programs). With @p opened, only when it is still the file that was
/// read. Returns whether it deleted the file.
bool deleteTemporaryFile(int parent, const std::filesystem::path& parentPath, const std::filesystem::path& name,
                         const struct stat* opened, const std::string& programs)
{
    if (!temporaryFile(parentPath / name, programs)) return false;
    struct stat current{};
    if (fstatat(parent, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(current.st_mode)) return false;
    if (opened && (current.st_dev != opened->st_dev || current.st_ino != opened->st_ino)) return false;
    return unlinkat(parent, name.c_str(), 0) == 0;
}

} // namespace

std::size_t Image::byteSize() const
{
    std::size_t total = 0;
    for (const FramePtr& f : frames) {
        total += f->byteSize();
    }
    return total;
}

// --- placeholders -----------------------------------------------------------

std::optional<PlaceholderCell> PlaceholderDecoder::next(const Screen& screen, const Row& row, int x)
{
    const Cell& cell = row.at(x);
    if (cell.codepoint() != AfterglowProto::PlaceholderChar) {
        m_previous = false;
        return std::nullopt;
    }
    auto colorId = [](Color c) -> std::uint32_t {
        return c.kind() == Color::Kind::Rgb ? (std::uint32_t(c.r()) << 16) | (std::uint32_t(c.g()) << 8) | c.b()
            : c.kind() == Color::Kind::Palette ? c.index() : 0;
    };
    const Style& style = screen.styles().get(cell.styleId());
    PlaceholderCell out;
    out.imageId = colorId(style.fg);
    out.placementId = colorId(style.underlineColor);
    const std::u32string_view extra = screen.graphemeExtra(row, x);
    const int r = extra.size() > 0 ? AfterglowProto::placeholderDiacriticValue(extra[0]) : -1;
    const int c = extra.size() > 1 ? AfterglowProto::placeholderDiacriticValue(extra[1]) : -1;
    const int high = extra.size() > 2 ? AfterglowProto::placeholderDiacriticValue(extra[2]) : -1;
    // kitty's rule (screen_render_line_graphics): a diacritic that is there
    // must agree with the run, or the cell starts a new one.
    const bool continues = m_previous && (out.imageId & 0xFFFFFF) == (m_last.imageId & 0xFFFFFF)
        && out.placementId == m_last.placementId && (r < 0 || r == m_last.row) && (c < 0 || c == m_last.col + 1)
        && (high < 0 || std::uint32_t(high) == m_last.imageId >> 24);
    if (continues) {
        out.imageId = m_last.imageId;
        out.row = m_last.row;
        out.col = m_last.col + 1;
    } else {
        if (high >= 0) out.imageId |= std::uint32_t(high) << 24;
        out.row = std::max(r, 0);
        out.col = std::max(c, 0);
    }
    m_previous = true;
    m_last = out;
    return out;
}

// --- command parsing --------------------------------------------------------

// The keys are read by the protocol library, which clients share.
struct GraphicsStore::Command : AfterglowProto::GraphicsCommand
{
    static Command parse(std::string_view keys) { return Command{AfterglowProto::GraphicsCommand::parse(keys)}; }
};

// --- origins ----------------------------------------------------------------

/// Where placements start, for one query or one command. Built once, so a
/// chain of relative placements and the placeholder scan cost one pass each
/// instead of one per placement.
class GraphicsStore::Origins
{
public:
    /// Rows where displayed placeholders count for virtual placements: the
    /// viewport (drawing) or the active area (protocol commands, which must
    /// not depend on how far the user has scrolled back).
    enum class Rows { Viewport, Active };

    Origins(const GraphicsStore& store, const Screen& screen, Rows rows, int extraRows = 0)
        : m_store(store)
        , m_screen(screen)
        , m_rows(rows)
        , m_extraRows(std::clamp(extraRows, 0, screen.rows()))
        , m_state(store.m_placements.size(), Unknown)
        , m_origin(store.m_placements.size())
    {
        const auto& placements = store.m_placements;
        m_byKey.reserve(placements.size());
        for (std::size_t i = 0; i < placements.size(); ++i) {
            m_byKey.try_emplace(placementKey(placements[i].imageId, placements[i].placementId), i);
            m_firstOfImage.try_emplace(placements[i].imageId, i);
        }
    }

    /// The placement a child hangs from, if it exists.
    std::optional<std::size_t> parentOf(const Placement& p) const
    {
        if (!p.parentImage) return std::nullopt;
        // A parent is named by id: internal and anonymous images have none.
        const auto image = m_store.m_images.find(p.parentImage);
        if (image == m_store.m_images.end() || image->second.isSixel || image->second.anonymous) return std::nullopt;
        if (!p.parentPlacement) {
            const auto it = m_firstOfImage.find(p.parentImage);
            return it == m_firstOfImage.end() ? std::nullopt : std::optional(it->second);
        }
        const auto it = m_byKey.find(placementKey(p.parentImage, p.parentPlacement));
        return it == m_byKey.end() ? std::nullopt : std::optional(it->second);
    }

    /// The cell placement @p index starts at: its column and its signed
    /// absolute row. nullopt when it has none: a virtual placement with no
    /// placeholders in the rows scanned, or a child of a placement without one.
    std::optional<std::pair<long long, long long>> of(std::size_t index)
    {
        if (m_state[index] == Known) return m_origin[index];
        if (m_state[index] == Visiting) return std::nullopt; // a cycle; place() refuses them
        m_state[index] = Visiting;
        const Placement& p = m_store.m_placements[index];
        std::optional<std::pair<long long, long long>> at;
        if (p.parentImage) {
            if (const auto parent = parentOf(p)) {
                at = of(*parent);
                if (at) {
                    at->first += p.relativeX;
                    at->second += p.relativeY;
                }
            }
        } else if (!p.isVirtual) {
            const long long row = anchorIndex(p, m_screen);
            if (row < static_cast<long long>(m_screen.totalRows())) at = std::pair<long long, long long>(p.col, row);
        } else {
            scanPlaceholders();
            // Cells without a placement colour show any virtual placement of the image.
            for (const std::uint32_t placement : {p.placementId, 0u}) {
                const auto it = m_placeholders.find(placementKey(p.imageId, placement));
                if (it == m_placeholders.end()) continue;
                if (!at) at = it->second;
                else at = std::pair(std::min(at->first, it->second.first), std::min(at->second, it->second.second));
            }
        }
        m_state[index] = Known;
        m_origin[index] = at;
        return at;
    }

private:
    enum State : std::uint8_t { Unknown, Visiting, Known };

    /// One pass over the rows: for each (image, placement colour), the
    /// top-left corner its displayed placeholders imply, implied diacritics
    /// included.
    void scanPlaceholders()
    {
        if (m_scanned) return;
        m_scanned = true;
        const Screen& s = m_screen;
        const std::size_t active = s.historySize() + std::size_t(s.rows());
        const std::size_t first = m_rows == Rows::Viewport ? s.viewportTop() : s.historySize();
        const std::size_t last = m_rows == Rows::Viewport ? std::min(active, first + std::size_t(s.rows() + m_extraRows)) : active;
        PlaceholderDecoder decoder;
        for (std::size_t y = first; y < last; ++y) {
            const Row& row = s.absoluteRow(y);
            decoder.reset();
            for (int x = 0; x < s.cols(); ++x) {
                const auto cell = decoder.next(s, row, x);
                if (!cell) continue;
                const long long px = x - cell->col;
                const long long py = static_cast<long long>(y) - cell->row;
                auto [it, fresh] = m_placeholders.try_emplace(placementKey(cell->imageId, cell->placementId), px, py);
                if (!fresh) it->second = {std::min(it->second.first, px), std::min(it->second.second, py)};
            }
        }
    }

    const GraphicsStore& m_store;
    const Screen& m_screen;
    Rows m_rows;
    int m_extraRows;
    std::unordered_map<std::uint64_t, std::size_t> m_byKey;
    std::unordered_map<std::uint32_t, std::size_t> m_firstOfImage;
    std::unordered_map<std::uint64_t, std::pair<long long, long long>> m_placeholders;
    bool m_scanned = false;
    std::vector<State> m_state;
    std::vector<std::optional<std::pair<long long, long long>>> m_origin;
};

// --- dispatch ---------------------------------------------------------------

void GraphicsStore::handle(std::string_view payload, Context& ctx)
{
    ++m_changes;
    if (dispatch(payload, ctx)) {
        settle();
    }
}

bool GraphicsStore::dispatch(std::string_view payload, Context& ctx)
{
    const auto semi = payload.find(';');
    const std::string_view keys = payload.substr(0, semi);
    const std::string_view data = semi == std::string_view::npos ? std::string_view{} : payload.substr(semi + 1);
    const Command cmd = Command::parse(keys);

    // An id with a number is refused before anything else, as in kitty, and
    // a feature query is answered at once: neither touches an upload in
    // progress.
    if (cmd.id && cmd.number) {
        // Refused before the payload is decoded, but a t=t or t=s name is
        // still the terminal's to delete, as for any other refusal. A name
        // fits in a few KiB of base64: longer data names nothing.
        if ((cmd.medium == 't' || cmd.medium == 's') && data.size() <= 8192) {
            releaseMedium(cmd, AfterglowProto::base64Decode(data));
        }
        respond(cmd, ctx, "EINVAL:both an image id and an image number", true);
        return false;
    }
    if (cmd.action == 'q' && cmd.feature) {
        featureQuery(cmd, ctx);
        return false;
    }

    if (m_pending.active) {
        // Chunks after the first carry only m= and q= (and a=f for a frame).
        // A command that names another action or image is new: the client
        // that left this transfer unfinished is gone, and its tail must not
        // swallow the next client's first command.
        const bool another = (cmd.hasAction && cmd.action != m_pending.first.action)
            || (cmd.hasId && cmd.id != m_pending.first.id) || (cmd.hasNumber && cmd.number != m_pending.first.number);
        if (another) {
            m_pending = Pending{};
        } else {
            // The last q= a chunk gave decides the replies, as in kitty.
            if (cmd.quiet) m_pending.first.quiet = cmd.quiet;
            const Command first{m_pending.first};
            // Reject the entire transmission, including tails after an oversized chunk.
            const std::size_t keep = chunkedKeep(first);
            if (!m_pending.failed) {
                if (data.size() > ((MaxPayload - m_pending.received + 2) / 3) * 4) {
                    m_pending.failed = true;
                    std::string().swap(m_pending.data);
                    respond(first, ctx, "EFBIG:image payload too large", true);
                } else {
                    // The payload limit counts every byte sent, also those
                    // past what the buffer keeps.
                    const std::size_t before = m_pending.data.size();
                    AfterglowProto::base64DecodeAppend(m_pending.data, data, AfterglowProto::Base64Check::Lenient);
                    m_pending.received += m_pending.data.size() - before;
                    if (m_pending.received > MaxPayload) {
                        m_pending.failed = true;
                        std::string().swap(m_pending.data);
                        respond(first, ctx, "EFBIG:image payload too large", true);
                    } else {
                        keepOnly(m_pending.data, keep);
                    }
                }
            }
            // A chunk that is not the last only adds to the buffer: it
            // changes no image or placement, so there is nothing to prune
            // or settle until the last one.
            if (cmd.more == 1) return false;
            std::string all = std::move(m_pending.data);
            const bool failed = m_pending.failed;
            m_pending = Pending{};
            pruneDead(ctx.screen, ctx.cellWidth, ctx.cellHeight);
            if (!failed) execute(first, std::move(all), ctx);
            return true;
        }
    }
    pruneDead(ctx.screen, ctx.cellWidth, ctx.cellHeight);

    const bool chunked = cmd.more == 1 && (cmd.action == 't' || cmd.action == 'T' || cmd.action == 'q' || cmd.action == 'f');
    const bool oversized = keys.size() > MaxControl || data.size() > ((MaxPayload + 2) / 3) * 4;
    if (oversized) {
        respond(cmd, ctx, "EFBIG:image payload too large", true);
        if (chunked) {
            m_pending.first = cmd;
            m_pending.active = true;
            m_pending.failed = true;
        }
        return true;
    }
    std::string decoded = AfterglowProto::base64Decode(data);
    if (decoded.size() > MaxPayload) {
        respond(cmd, ctx, "EFBIG:image payload too large", true);
        if (chunked) {
            m_pending.first = cmd;
            m_pending.active = true;
            m_pending.failed = true;
        }
        return true;
    }
    if (chunked) {
        m_pending.first = cmd;
        m_pending.data = std::move(decoded);
        m_pending.received = m_pending.data.size();
        keepOnly(m_pending.data, chunkedKeep(cmd));
        m_pending.active = true;
        return true;
    }
    execute(cmd, std::move(decoded), ctx);
    return true;
}

void GraphicsStore::execute(const Command& cmd, std::string data, Context& ctx)
{
    switch (cmd.action) {
    case 't':
    case 'T':
    case 'q':
        transmit(cmd, std::move(data), ctx);
        break;
    case 'p':
        place(cmd, ctx);
        break;
    case 'd':
        remove(cmd, ctx);
        break;
    case 'f':
        frame(cmd, std::move(data), ctx);
        break;
    case 'a':
        animate(cmd, ctx);
        break;
    case 'c':
        compose(cmd, ctx);
        break;
    default:
        releaseMedium(cmd, data);
        respond(cmd, ctx, "EINVAL:unknown action", true);
        break;
    }
}

void GraphicsStore::respond(const Command& cmd, Context& ctx, std::string_view status, bool isError)
{
    if (cmd.id == 0 && cmd.number == 0) {
        return; // the protocol answers only commands that name an image
    }
    if (cmd.quiet >= 2 || (cmd.quiet == 1 && !isError)) {
        return;
    }
    AfterglowProto::GraphicsReply reply{cmd.id, cmd.number, cmd.placement, std::string(status)};
    if ((cmd.action == 'f' || cmd.action == 'a') && cmd.r > 0) {
        reply.keys = "r=" + std::to_string(cmd.r);
    }
    ctx.reply(reply.write());
}

void GraphicsStore::featureQuery(const Command& cmd, Context& ctx)
{
    // The keys kitty's supported_keys_for_query() lists for each action.
    // Usage hints (N=) and the indexed formats (f=8, f=16) belong to the
    // transmission, so frames do not report them. Keys with their default
    // value are left out.
    if (!cmd.id || cmd.quiet >= 1) {
        return; // a query names its id; quiet leaves nothing to say about an OK
    }
    std::string keys;
    auto add = [&keys](char key, auto value) {
        keys += ',';
        keys += key;
        keys += '=';
        if constexpr (std::is_same_v<decltype(value), char>) keys += value;
        else keys += std::to_string(value);
    };
    auto placementKeys = [&] {
        if (cmd.placement) add('p', cmd.placement);
        if (cmd.ux) add('x', cmd.ux);
        if (cmd.uy) add('y', cmd.uy);
        if (cmd.w > 0) add('w', cmd.w);
        if (cmd.h > 0) add('h', cmd.h);
        if (cmd.X > 0) add('X', cmd.X);
        if (cmd.Yi > 0) add('Y', cmd.Yi);
        if (cmd.c > 0) add('c', cmd.c);
        if (cmd.r > 0) add('r', cmd.r);
        if (cmd.C == 1) add('C', 1);
        if (cmd.U == 1) add('U', 1);
        if (cmd.z) add('z', cmd.z);
        if (cmd.P) add('P', cmd.P);
        if (cmd.Q) add('Q', cmd.Q);
        if (cmd.H) add('H', cmd.H);
        if (cmd.V) add('V', cmd.V);
    };
    const char e = cmd.feature;
    switch (e) {
    case 't':
    case 'T':
    case 'q':
    case 'f':
        add('e', e);
        if (cmd.medium == 'f' || cmd.medium == 't' || cmd.medium == 's') add('t', cmd.medium);
        if (cmd.compression == 'z') add('o', 'z');
        if (cmd.format == 24 || cmd.format == 100 || (e != 'f' && isIndexedFormat(cmd.format))) {
            add('f', cmd.format);
        }
        if (cmd.s && cmd.s <= MaxDimension) add('s', cmd.s);
        if (cmd.v && cmd.v <= MaxDimension) add('v', cmd.v);
        if (cmd.size) add('S', cmd.size);
        if (cmd.offset) add('O', cmd.offset);
        if (e != 'f' && cmd.hints == 1) add('N', 1);
        if (e == 'f') {
            if (cmd.ux) add('x', cmd.ux);
            if (cmd.uy) add('y', cmd.uy);
            if (cmd.c > 0) add('c', cmd.c);
            if (cmd.r > 0) add('r', cmd.r);
            if (cmd.z) add('z', cmd.z);
            if (cmd.X == 1) add('X', 1);
            if (cmd.Y > 0 && cmd.Y <= 0xFFFFFFFF) add('Y', cmd.Y);
        }
        if (e == 'T') placementKeys();
        break;
    case 'p':
        add('e', e);
        placementKeys();
        break;
    case 'd':
        add('e', e);
        if (cmd.del != 'a' && std::string_view("AiInNcCfFpPqQxXyYzZrR").find(cmd.del) != std::string_view::npos) {
            add('d', cmd.del);
        }
        if (cmd.placement) add('p', cmd.placement);
        if (cmd.ux) add('x', cmd.ux);
        if (cmd.uy) add('y', cmd.uy);
        if (cmd.z) add('z', cmd.z);
        break;
    case 'a':
        add('e', e);
        if (cmd.s >= 1 && cmd.s <= 3) add('s', cmd.s);
        if (cmd.v) add('v', cmd.v);
        if (cmd.r > 0) add('r', cmd.r);
        if (cmd.c > 0) add('c', cmd.c);
        if (cmd.z) add('z', cmd.z);
        break;
    case 'c':
        add('e', e);
        if (cmd.r > 0) add('r', cmd.r);
        if (cmd.c > 0) add('c', cmd.c);
        if (cmd.ux) add('x', cmd.ux);
        if (cmd.uy) add('y', cmd.uy);
        if (cmd.w > 0) add('w', cmd.w);
        if (cmd.h > 0) add('h', cmd.h);
        if (cmd.X > 0) add('X', cmd.X);
        if (cmd.Yi > 0) add('Y', cmd.Yi);
        if (cmd.C == 1) add('C', 1);
        break;
    default:
        break; // an action this store does not know: the reply has no e=
    }
    AfterglowProto::GraphicsReply reply{cmd.id, 0, 0, "OK"};
    if (!keys.empty()) reply.keys = keys.substr(1);
    ctx.reply(reply.write());
}

// --- transmission -----------------------------------------------------------

bool GraphicsStore::loadPayload(const Command& cmd, std::string& data, std::size_t readLimit, std::size_t inflateLimit,
                                std::string& error)
{
    const bool cut = cmd.compression == 0 && isRawFormat(cmd.format);
    const bool fromFile = cmd.medium == 'f' || cmd.medium == 't' || cmd.medium == 's';
    if (fromFile && !m_fileTransfer) {
        error = "EPERM:file transmission is turned off";
        return false;
    }
    auto fail = [&error] {
        error = FileError;
        return false;
    };
    // A refusal by the budget is not a failure to read: the file was opened
    // and found regular, which a read that succeeds tells the client too.
    bool released = false;
    auto finish = [&error, &fail, &released](ReadResult result) {
        if (result == ReadResult::Done) return true;
        if (result == ReadResult::Busy) {
            error = busyError(released, "image data read");
            return false;
        }
        return fail();
    };
    auto charge = [this](std::size_t bytes) { return chargeWork(bytes); };
    switch (cmd.medium) {
    case 'd':
        break;
    case 'f':
    case 't': {
        if (data.size() > 4096 || data.find('\0') != std::string::npos) return fail();
        std::error_code ec;
        const auto canonical = std::filesystem::canonical(data, ec);
        if (ec || forbiddenFile(canonical)) return fail();
        // Pin the parent directory and validate the actual opened file.
        const int parent = open(canonical.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent < 0) return fail();
        const auto actualParent = std::filesystem::read_symlink("/proc/self/fd/" + std::to_string(parent), ec);
        if (ec || forbiddenFile(actualParent)) { close(parent); return fail(); }
        const auto name = canonical.filename();
        // The type is checked before the open, which has effects of its own:
        // opening a FIFO lets its writer go on. It is checked again on what
        // was opened, in case the name was replaced in between, and
        // O_NONBLOCK keeps a FIFO put there from hanging the open.
        struct stat named{};
        if (fstatat(parent, name.c_str(), &named, AT_SYMLINK_NOFOLLOW) != 0 || !S_ISREG(named.st_mode)) {
            close(parent);
            return fail();
        }
        const int fd = openat(parent, name.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        struct stat opened{};
        if (fd < 0 || fstat(fd, &opened) != 0 || !S_ISREG(opened.st_mode)) {
            if (fd >= 0) close(fd);
            close(parent);
            return fail();
        }
        const ReadResult result = readPayload(fd, cmd.offset, cmd.size, readLimit, cut, data, charge);
        // A temporary file is the terminal's to delete whether or not it
        // could use it, as in kitty, so that refused ones do not pile up;
        // only the file that was opened, though.
        if (cmd.medium == 't') released = deleteTemporaryFile(parent, actualParent, name, &opened, m_temporaryDirectory);
        close(fd);
        close(parent);
        if (!finish(result)) return false;
        break;
    }
    case 's': {
        const std::string name = data;
        if (name.size() > 4096 || name.find('\0') != std::string::npos) return fail();
        // O_NONBLOCK: a FIFO planted in /dev/shm must not hang the open before
        // readPayload's fstat rejects it.
        const int fd = shm_open(name.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0);
        if (fd < 0) return fail();
        // pread is safe if the sender truncates the object concurrently; mmap
        // would instead deliver SIGBUS to the terminal process.
        const ReadResult result = readPayload(fd, cmd.offset, cmd.size, readLimit, cut, data, charge);
        close(fd);
        // Only an object named for the protocol is the terminal's to delete.
        // Any other name stays: a command must not delete an unrelated object
        // by naming it, and almost any object decodes as a tiny image.
        if (name.find(AfterglowProto::GraphicsTemporaryTag) != std::string::npos) {
            released = shm_unlink(name.c_str()) == 0;
        }
        if (!finish(result)) return false;
        break;
    }
    default:
        error = "EINVAL:unknown transmission medium";
        return false;
    }
    if (cmd.compression == 'z') {
        std::string inflated;
        if (!AfterglowProto::graphicsInflate(data, inflated, inflateLimit)) {
            error = "EINVAL:zlib decompression failed";
            return false;
        }
        data = std::move(inflated);
    }
    return true;
}

bool GraphicsStore::releaseMedium(const Command& cmd, const std::string& data) const
{
    // The same rules as loadPayload's: the name a t=s command gives, and the
    // real path of a t=t file below its temporary directory, must carry the tag.
    if (!m_fileTransfer || data.size() > 4096 || data.find('\0') != std::string::npos) {
        return false;
    }
    if (cmd.medium == 's') {
        return data.find(AfterglowProto::GraphicsTemporaryTag) != std::string::npos && shm_unlink(data.c_str()) == 0;
    }
    if (cmd.medium != 't') return false;
    std::error_code ec;
    const auto canonical = std::filesystem::canonical(data, ec);
    if (ec) return false;
    const int parent = open(canonical.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0) return false;
    const auto actualParent = std::filesystem::read_symlink("/proc/self/fd/" + std::to_string(parent), ec);
    const bool released = !ec && deleteTemporaryFile(parent, actualParent, canonical.filename(), nullptr, m_temporaryDirectory);
    close(parent);
    return released;
}

bool GraphicsStore::decodePixels(const Command& cmd, std::string data, Context& ctx, ImageFrame& out,
                                 std::string& error)
{
    // What the command declares is checked before anything is read or
    // inflated: a few hundred KiB of o=z data can expand to any size, so a
    // raw image may inflate only to the size it declares.
    //
    // PNG and o=z bytes have the limit of inline data in every medium. The
    // quota applies to the decoded image (transmit() checks it), so a PNG
    // file longer than the quota whose pixels fit is kept.
    //
    // The size limits count 4 bytes for each pixel, also for an indexed
    // image, which keeps 1 or 2: a renderer draws every image as RGBA.
    const bool raw = isRawFormat(cmd.format);
    const std::size_t bpp = cmd.format / 8;
    std::size_t readLimit = MaxPayload;
    std::size_t inflateLimit = std::min(m_quota, MaxPayload);
    if (cmd.compression != 0 && cmd.compression != 'z') {
        error = "EINVAL:unknown compression";
    } else if (!raw && cmd.format != 100) {
        error = "EINVAL:unknown format";
    } else if (raw && (cmd.s == 0 || cmd.v == 0 || cmd.s > MaxDimension || cmd.v > MaxDimension)) {
        error = "EINVAL:missing or invalid image size";
    } else if (raw && std::size_t(cmd.s) * cmd.v * 4 > MaxPayload) {
        error = "EFBIG:image payload too large";
    } else if (raw && std::size_t(cmd.s) * cmd.v * 4 > m_quota) {
        error = "ENOSPC:image larger than the storage quota";
    } else if (raw) {
        // Raw pixels past the image are never used, so they are not read.
        inflateLimit = std::size_t(cmd.s) * cmd.v * bpp;
        if (cmd.compression == 0) readLimit = inflateLimit;
    }
    if (!error.empty()) {
        releaseMedium(cmd, data);
        return false;
    }
    if (!loadPayload(cmd, data, readLimit, inflateLimit, error)) {
        return false;
    }
    if (!raw) {
        if (!ctx.host.decodePng(data, out.width, out.height, out.rgba)) {
            error = "EBADPNG:failed to decode PNG data";
            return false;
        }
    } else {
        const std::size_t expected = std::size_t(cmd.s) * cmd.v * bpp;
        if (data.size() < expected) {
            // A file shorter than the client claimed is a read failure: its
            // length is not the client's to learn.
            error = cmd.medium == 'd' ? std::string_view("ENODATA:insufficient image data") : FileError;
            return false;
        }
        out.width = cmd.s;
        out.height = cmd.v;
        if (isIndexedFormat(cmd.format)) {
            out.format = cmd.format == 8 ? PixelFormat::Indexed : PixelFormat::IndexedAlpha;
            out.indices.assign(data.begin(), data.begin() + std::ptrdiff_t(expected));
        } else if (bpp == 4) {
            out.rgba.assign(data.begin(), data.begin() + std::ptrdiff_t(expected));
        } else {
            out.rgba.resize(std::size_t(cmd.s) * cmd.v * 4);
            for (std::size_t i = 0, j = 0; i < expected; i += 3, j += 4) {
                out.rgba[j] = std::uint8_t(data[i]);
                out.rgba[j + 1] = std::uint8_t(data[i + 1]);
                out.rgba[j + 2] = std::uint8_t(data[i + 2]);
                out.rgba[j + 3] = 255;
            }
        }
    }
    if (out.width == 0 || out.height == 0 || out.width > MaxDimension || out.height > MaxDimension) {
        error = "EINVAL:invalid image dimensions";
        return false;
    }
    const std::size_t pixels = std::size_t(out.width) * out.height;
    const std::size_t kept = out.pixels().size();
    if (pixels * 4 > MaxPayload || kept != pixels * bytesPerPixel(out.format)) {
        error = "EINVAL:invalid decoded image extent";
        return false;
    }
    return true;
}

void GraphicsStore::transmit(const Command& cmdIn, std::string data, Context& ctx)
{
    Command cmd = cmdIn;
    ImageFrame frame;
    std::string error;
    if (!decodePixels(cmd, std::move(data), ctx, frame, error)) {
        respond(cmd, ctx, error, true);
        return;
    }
    if (frame.byteSize() > m_quota) {
        respond(cmd, ctx, "ENOSPC:image larger than the storage quota", true);
        return;
    }
    if (cmd.action == 'q') {
        respond(cmd, ctx, "OK", false);
        return;
    }
    // At the image limit, an image nothing shows makes room; only when every
    // image is on screen is the new one refused. This comes after the decode,
    // so a command that fails costs the client no image.
    const bool replaces = cmd.id && findImage(Command{{.id = cmd.id}});
    if (!replaces && m_images.size() >= MaxImages && !evictUnusedImage()) {
        respond(cmd, ctx, "ENOSPC:too many images", true);
        return;
    }

    std::uint32_t id = cmd.id;
    if (id == 0) {
        // An image number asks us to choose the id; with neither, the image
        // is anonymous and only ever displayed by this command.
        do {
            id = m_nextAutoId++;
            if (m_nextAutoId == 0) {
                m_nextAutoId = 1u << 31;
            }
        } while (m_images.count(id));
        if (cmd.number) {
            cmd.id = id;
        }
    }
    if (auto existing = m_images.find(id);
        existing != m_images.end() && (existing->second.isSixel || existing->second.anonymous)) {
        // Kitty may explicitly claim any ID, including that of an internal or
        // anonymous image, which no command can name. Keep that image and its
        // anchors, relocating it before replacement.
        const auto replacement = allocateInternalId();
        auto node = m_images.extract(existing);
        node.key() = replacement;
        node.mapped().id = replacement;
        m_images.insert(std::move(node));
        for (auto& placement : m_placements) {
            if (placement.imageId == id) placement.imageId = replacement;
        }
    }
    std::erase_if(m_placements, [id](const Placement& p) { return p.imageId == id; });
    pruneOrphans();
    const auto previous = m_images.find(id);
    const std::uint64_t order = previous != m_images.end() ? previous->second.order : 0;

    Image img;
    img.id = id;
    img.number = cmd.number;
    img.anonymous = cmdIn.id == 0 && cmdIn.number == 0;
    img.transient = (cmd.hints & 1) != 0;
    img.width = frame.width;
    img.height = frame.height;
    img.frames.push_back(std::make_shared<const ImageFrame>(std::move(frame)));
    img.gaps.push_back(0); // the root frame has no gap until a=a gives it one
    img.sequence = ++m_sequence;
    img.order = order ? order : img.sequence;
    m_images[id] = std::move(img);
    enforceQuota(id);

    if (cmd.action == 'T') {
        Command pc = cmd;
        pc.id = id;
        if (cmd.id == 0 && cmd.number == 0) pc.quiet = 2;
        // The image itself: an anonymous one is not found by its id.
        place(pc, ctx, &m_images.at(id)); // Placement errors must not be hidden by an earlier OK.
    } else {
        respond(cmd, ctx, "OK", false);
    }
}

Image* GraphicsStore::findImage(const Command& cmd)
{
    if (cmd.id) {
        // An anonymous image has an id only inside the store: in kitty it has
        // none, so no command reaches it by guessing.
        auto it = m_images.find(cmd.id);
        return it == m_images.end() || it->second.isSixel || it->second.anonymous ? nullptr : &it->second;
    }
    if (cmd.number) {
        Image* newest = nullptr;
        for (auto& [id, img] : m_images) {
            if (!img.isSixel && img.number == cmd.number && (!newest || img.sequence > newest->sequence)) {
                newest = &img;
            }
        }
        return newest;
    }
    return nullptr;
}

// --- placement --------------------------------------------------------------

namespace {

/// Erases the placements flagged in @p doomed, keeping the others in order.
void eraseFlagged(std::vector<Placement>& placements, const std::vector<char>& doomed)
{
    std::size_t kept = 0;
    for (std::size_t i = 0; i < placements.size(); ++i) {
        if (doomed[i]) continue;
        if (kept != i) placements[kept] = placements[i];
        ++kept;
    }
    placements.resize(kept);
}

} // namespace

void GraphicsStore::place(const Command& cmdIn, Context& ctx, Image* image)
{
    Image* img = image ? image : findImage(cmdIn);
    if (!img) {
        respond(cmdIn, ctx, "ENOENT:no such image", true);
        return;
    }
    Command cmd = cmdIn;
    cmd.id = img->id; // a command that gave only I= learns the id
    Screen& scr = ctx.screen;
    Placement pl;
    pl.imageId = img->id;
    pl.placementId = cmd.placement ? cmd.placement : allocatePlacementId(pl.imageId);
    pl.parentImage = cmd.P;
    pl.parentPlacement = cmd.Q;
    pl.relativeX = cmd.H;
    pl.relativeY = cmd.V;
    const auto key = placementKey(pl.imageId, pl.placementId);
    if (cmd.P || cmd.Q) {
        if (cmd.U) { respond(cmd, ctx, "EINVAL:virtual placement cannot be relative", true); return; }
        const Origins links(*this, scr, Origins::Rows::Active);
        const auto parent = links.parentOf(pl);
        if (!parent) { respond(cmd, ctx, "ENOPARENT:no such parent placement", true); return; }
        pl.parentPlacement = m_placements[*parent].placementId; // Resolve an omitted Q once.
        int depth = 0;
        for (auto up = parent; up; up = links.parentOf(m_placements[*up])) {
            const Placement& ancestor = m_placements[*up];
            if (ancestor.imageId == pl.imageId && ancestor.placementId == pl.placementId) {
                respond(cmd, ctx, "ECYCLE:relative placement cycle", true); return;
            }
            if (++depth > MaxDepth) {
                respond(cmd, ctx, "ETOODEEP:relative placement depth exceeds " + std::to_string(MaxDepth), true);
                return;
            }
        }
        // Reparenting also lengthens existing descendant chains: walk them
        // level by level from this placement, through a map of children.
        std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> children;
        for (const Placement& p : m_placements) {
            if (p.parentImage) children[placementKey(p.parentImage, p.parentPlacement)].push_back(placementKey(p.imageId, p.placementId));
        }
        std::vector<std::uint64_t> level{key};
        for (int below = 0; !level.empty(); ++below) {
            if (depth + below > MaxDepth) {
                respond(cmd, ctx, "ETOODEEP:descendant depth exceeds " + std::to_string(MaxDepth), true);
                return;
            }
            std::vector<std::uint64_t> next;
            for (const auto k : level) {
                if (const auto it = children.find(k); it != children.end()) next.insert(next.end(), it->second.begin(), it->second.end());
            }
            level = std::move(next);
        }
    }
    // A placement that is replaced keeps its slot, and so its place in the
    // drawing order among placements at its z (kitty's).
    const auto replaced = std::find_if(m_placements.begin(), m_placements.end(),
                                       [&](const Placement& p) { return placementKey(p.imageId, p.placementId) == key; });
    if (replaced == m_placements.end() && m_placements.size() >= MaxPlacements) {
        respond(cmd, ctx, "ENOSPC:too many placements", true);
        return;
    }
    pl.row = scr.rowsDropped() + scr.historySize() + std::size_t(scr.cursor.y);
    pl.col = scr.cursor.x;
    // The protocol keeps the offsets inside the cell: a larger one would move
    // the image, and the cursor with it, by any number of rows.
    pl.offsetX = cellOffset(cmd.X, ctx.cellWidth);
    pl.offsetY = cellOffset(int(std::clamp<std::int64_t>(cmd.Y, 0, 1 << 28)), ctx.cellHeight);
    pl.srcX = std::max(0, cmd.x);
    pl.srcY = std::max(0, cmd.y);
    pl.srcW = std::max(0, cmd.w);
    pl.srcH = std::max(0, cmd.h);
    pl.cols = std::max(0, cmd.c);
    pl.rows = std::max(0, cmd.r);
    pl.z = cmd.z;
    pl.isVirtual = cmd.U == 1;
    if (replaced != m_placements.end()) *replaced = pl;
    else m_placements.push_back(pl);

    if (!pl.isVirtual && !pl.parentImage && cmd.C == 0 && ctx.cellWidth > 0 && ctx.cellHeight > 0) {
        // The cursor moves past the image: down to its last row, right of its last column.
        const DisplaySize size = displaySize(pl, *img, ctx.cellWidth, ctx.cellHeight);
        const long long cellsWide = (static_cast<long long>(size.width) + pl.offsetX + ctx.cellWidth - 1) / ctx.cellWidth;
        const long long cellsHigh = (static_cast<long long>(size.height) + pl.offsetY + ctx.cellHeight - 1) / ctx.cellHeight;
        const long long targetX = pl.col + cellsWide;
        const bool wraps = targetX >= scr.cols();
        // The size comes from the application; bound the line feeds to
        // reaching the bottom plus one screen of scrolling, as kitty does.
        const long long requested = std::max(0LL, cellsHigh - 1) + (wraps ? 1 : 0);
        const long long limit = (scr.rows() - 1 - scr.cursor.y) + scr.rows();
        for (long long i = 0; i < std::min(requested, limit); ++i) {
            ctx.lineFeed();
        }
        scr.cursor.pendingWrap = false;
        scr.cursor.x = wraps ? 0 : int(targetX);
    }
    respond(cmd, ctx, "OK", false);
}

std::uint32_t GraphicsStore::allocatePlacementId(std::uint32_t imageId)
{
    // A client may name any p= in this range too: a chosen id skips those in
    // use, so that a placement without p= never replaces one with it. 0 is
    // never chosen, since p=0 means "no placement id".
    std::uint32_t id;
    do {
        id = m_nextPlacementId++;
        if (m_nextPlacementId == 0) m_nextPlacementId = 1u << 31;
    } while (std::any_of(m_placements.begin(), m_placements.end(),
                         [&](const Placement& p) { return p.imageId == imageId && p.placementId == id; }));
    return id;
}

std::uint32_t GraphicsStore::allocateInternalId()
{
    std::uint32_t id;
    do {
        id = m_nextInternalId++;
        if (m_nextInternalId >= (1u << 31)) m_nextInternalId = 3u << 29;
    } while (m_images.contains(id));
    return id;
}

void GraphicsStore::addSixel(ImageFrame frame, const Screen& screen, int row, int col, int cellWidth, int cellHeight,
                             std::uint32_t displayWidth, std::uint32_t displayHeight)
{
    if (cellWidth <= 0 || cellHeight <= 0) {
        return;
    }
    // Kept, it would evict every other image and still not fit.
    if (frame.byteSize() > m_quota) {
        return;
    }
    pruneDead(screen, cellWidth, cellHeight);
    const std::uint32_t width = displayWidth ? displayWidth : frame.width;
    const std::uint32_t height = displayHeight ? displayHeight : frame.height;
    const int cellsWide = int((width + std::uint32_t(cellWidth) - 1) / std::uint32_t(cellWidth));
    const int cellsHigh = int((height + std::uint32_t(cellHeight) - 1) / std::uint32_t(cellHeight));
    const std::uint64_t topRow = screen.rowsDropped() + screen.historySize() + std::uint64_t(std::max(0, row));

    // A new image over an older one replaces it where it covers it whole.
    std::vector<std::uint32_t> covered;
    std::erase_if(m_placements, [&](const Placement& p) {
        const auto it = m_images.find(p.imageId);
        if (it == m_images.end() || !it->second.isSixel) {
            return false;
        }
        const std::uint32_t pw = p.displayWidth ? p.displayWidth : it->second.width;
        const std::uint32_t ph = p.displayHeight ? p.displayHeight : it->second.height;
        const int w = int((pw + std::uint32_t(cellWidth) - 1) / std::uint32_t(cellWidth));
        const int h = int((ph + std::uint32_t(cellHeight) - 1) / std::uint32_t(cellHeight));
        const bool inside = p.row >= topRow && p.row + std::uint64_t(h) <= topRow + std::uint64_t(cellsHigh)
            && p.col >= col && p.col + w <= col + cellsWide;
        if (inside) {
            covered.push_back(p.imageId);
        }
        return inside;
    });
    for (std::uint32_t id : covered) {
        dropUnusedImage(id);
    }
    if (m_placements.size() >= MaxPlacements) {
        // Room comes from the oldest internal image; kitty placements belong
        // to their program. With none, the new image is not kept.
        const auto oldest = std::find_if(m_placements.begin(), m_placements.end(), [&](const Placement& p) {
            const auto it = m_images.find(p.imageId);
            return it != m_images.end() && it->second.isSixel;
        });
        if (oldest == m_placements.end()) {
            settle();
            return;
        }
        m_placements.erase(oldest);
    }

    Image img;
    img.id = allocateInternalId();
    img.isSixel = true;
    img.width = frame.width;
    img.height = frame.height;
    img.frames.push_back(std::make_shared<const ImageFrame>(std::move(frame)));
    img.gaps.push_back(0);
    img.sequence = ++m_sequence;
    img.order = img.sequence;
    const std::uint32_t id = img.id;
    m_images[id] = std::move(img);

    Placement pl;
    pl.imageId = id;
    pl.placementId = 1;
    pl.row = topRow;
    pl.col = col;
    pl.z = -1; // under text, over cell backgrounds: text written on it stays readable
    pl.displayWidth = displayWidth;
    pl.displayHeight = displayHeight;
    m_placements.push_back(pl);
    enforceQuota(id);
    settle();
}

void GraphicsStore::remove(const Command& cmd, Context& ctx)
{
    const Screen& scr = ctx.screen;
    const char d = cmd.del ? cmd.del : 'a';
    const bool freeImages = d >= 'A' && d <= 'Z';
    const char kind = char(d | 0x20);
    if (std::string_view("aincpqxyzrf").find(kind) == std::string_view::npos) {
        respond(cmd, ctx, "EINVAL:unsupported delete target", true);
        return;
    }
    if (kind == 'f') {
        deleteFrame(cmd, freeImages);
        return;
    }

    // Cells relative to the top of the screen; a placement without a
    // position (an undisplayed virtual parent's child) covers none.
    Origins origins(*this, scr, Origins::Rows::Active);
    struct Rect
    {
        long long row, col, rows, cols;
    };
    auto cellRect = [&](std::size_t index) -> std::optional<Rect> {
        const Placement& p = m_placements[index];
        if (p.isVirtual) return std::nullopt; // drawn only where placeholders are
        const auto at = origins.of(index);
        if (!at) return std::nullopt;
        Rect r{at->second - static_cast<long long>(scr.historySize()), at->first, 1, 1};
        const auto it = m_images.find(p.imageId);
        if (it != m_images.end() && ctx.cellWidth > 0 && ctx.cellHeight > 0) {
            const DisplaySize size = displaySize(p, it->second, ctx.cellWidth, ctx.cellHeight);
            const long long ox = cellOffset(p.offsetX, ctx.cellWidth), oy = cellOffset(p.offsetY, ctx.cellHeight);
            r.cols = std::max(1LL, (static_cast<long long>(size.width) + ox + ctx.cellWidth - 1) / ctx.cellWidth);
            r.rows = std::max(1LL, (static_cast<long long>(size.height) + oy + ctx.cellHeight - 1) / ctx.cellHeight);
        }
        return r;
    };
    auto covers = [&](std::size_t index, long long row, long long col) {
        const auto r = cellRect(index);
        return r && row >= r->row && row < r->row + r->rows && col >= r->col && col < r->col + r->cols;
    };
    const Image* numbered = kind == 'n' ? findImage(Command{{.number = cmd.number}}) : nullptr;

    // Decide first, then erase: the decisions read the placements.
    std::vector<char> doomed(m_placements.size(), 0);
    std::vector<std::uint32_t> touched;
    for (std::size_t i = 0; i < m_placements.size(); ++i) {
        const Placement& p = m_placements[i];
        const auto image = m_images.find(p.imageId);
        if (image == m_images.end() || image->second.isSixel) {
            continue;
        }
        // Ids name only images that a client gave one (kitty's client ids).
        const bool named = !image->second.anonymous;
        bool match = false;
        switch (kind) {
        case 'a': {
            // Visible on the screen: any of its rows reaches the active area.
            const auto r = cellRect(i);
            match = r && r->row + r->rows > 0;
            break;
        }
        case 'i':
            match = named && p.imageId == cmd.id && (cmd.placement == 0 || p.placementId == cmd.placement);
            break;
        case 'n':
            match = numbered && p.imageId == numbered->id && (cmd.placement == 0 || p.placementId == cmd.placement);
            break;
        case 'c':
            match = covers(i, scr.cursor.y, scr.cursor.x);
            break;
        case 'p':
            match = covers(i, cmd.y - 1LL, cmd.x - 1LL);
            break;
        case 'q':
            match = p.z == cmd.z && covers(i, cmd.y - 1LL, cmd.x - 1LL);
            break;
        case 'x': {
            const auto r = cellRect(i);
            match = r && cmd.x - 1LL >= r->col && cmd.x - 1LL < r->col + r->cols;
            break;
        }
        case 'y': {
            const auto r = cellRect(i);
            match = r && cmd.y - 1LL >= r->row && cmd.y - 1LL < r->row + r->rows;
            break;
        }
        case 'z':
            match = !p.isVirtual && p.z == cmd.z; // drawn only where placeholders are, as for the others
            break;
        case 'r':
            match = named && p.imageId >= cmd.ux && p.imageId <= cmd.uy;
            break;
        default:
            break;
        }
        if (match) {
            doomed[i] = 1;
            touched.push_back(p.imageId);
        }
    }
    eraseFlagged(m_placements, doomed);
    if (freeImages) {
        if (kind == 'i' && findImage(Command{{.id = cmd.id}})) {
            touched.push_back(cmd.id);
        } else if (kind == 'n' && numbered) {
            touched.push_back(numbered->id);
        } else if (kind == 'r') {
            for (const auto& [id, img] : m_images) {
                if (!img.isSixel && !img.anonymous && id >= cmd.ux && id <= cmd.uy) {
                    touched.push_back(id);
                }
            }
        }
    }

    pruneOrphans();
    if (freeImages) {
        for (std::uint32_t id : touched) {
            dropUnusedImage(id);
        }
    }
}

void GraphicsStore::deleteFrame(const Command& cmd, bool freeImage)
{
    // kitty's handle_delete_frame_command. Frames here are whole images, so
    // no other frame depends on the one that goes.
    Image* img = findImage(cmd);
    if (!img) {
        return;
    }
    const std::size_t n = img->frames.size();
    if (n <= 1) {
        // d=F on an image of one frame deletes the image; d=f keeps it.
        if (freeImage) {
            const std::uint32_t id = img->id;
            std::erase_if(m_placements, [id](const Placement& p) { return p.imageId == id; });
            m_images.erase(id);
            pruneOrphans();
        }
        return;
    }
    const std::size_t victim = std::clamp<std::size_t>(cmd.r > 0 ? std::size_t(cmd.r) : 1, 1, n) - 1;
    img->frames.erase(img->frames.begin() + std::ptrdiff_t(victim));
    img->gaps.erase(img->gaps.begin() + std::ptrdiff_t(victim));
    if (img->currentFrame >= img->frames.size()) {
        img->currentFrame = img->frames.size() - 1;
        img->frameShownAtMs = 0;
    } else if (img->currentFrame > victim) {
        --img->currentFrame;
    } else if (img->currentFrame == victim) {
        img->frameShownAtMs = 0; // another frame shows from the next tick
    }
}

void GraphicsStore::dropUnusedImage(std::uint32_t id)
{
    const bool used = std::any_of(m_placements.begin(), m_placements.end(),
                                  [id](const Placement& p) { return p.imageId == id; });
    if (!used) {
        m_images.erase(id);
    }
}

// --- animation --------------------------------------------------------------

void GraphicsStore::frame(const Command& cmdIn, std::string data, Context& ctx)
{
    Image* img = findImage(cmdIn);
    if (!img) {
        releaseMedium(cmdIn, data);
        respond(cmdIn, ctx, "ENOENT:no such image", true);
        return;
    }
    Command cmd = cmdIn;
    cmd.id = img->id;
    const std::uint32_t gap = cmd.z < 0 ? 0 : cmd.z > 0 ? std::uint32_t(cmd.z) : DefaultFrameGap;
    const bool edits = cmd.r > 0 && std::size_t(cmd.r) <= img->frames.size();
    // The reply names the frame written: r= past the last frame adds one.
    const int base = cmd.c;
    cmd.r = edits ? cmd.r : int(std::min<std::size_t>(img->frames.size() + 1, INT_MAX));

    // Indexed images have no animation in the protocol: their colours
    // come from the palette when drawn, which frame composition cannot use.
    if (img->frames.front()->indexed() || isIndexedFormat(cmd.format)) {
        releaseMedium(cmd, data);
        respond(cmd, ctx, "EINVAL:indexed images have no frames", true);
        return;
    }

    // Limits first, so a refused frame reads and decodes nothing.
    if (!edits && base > 0 && std::size_t(base) > img->frames.size()) {
        releaseMedium(cmd, data);
        respond(cmd, ctx, "EINVAL:no such base frame", true);
        return;
    }
    const std::size_t canvasBytes = std::size_t(img->width) * img->height * 4;
    if (!edits && img->frames.size() >= MaxFrames) {
        releaseMedium(cmd, data);
        respond(cmd, ctx, "ENOSPC:too many image frames", true);
        return;
    }
    if (!edits && img->byteSize() + canvasBytes > m_quota) {
        releaseMedium(cmd, data);
        respond(cmd, ctx, "ENOSPC:image larger than the storage quota", true);
        return;
    }
    if (!chargeWork(canvasBytes)) {
        const bool released = releaseMedium(cmd, data);
        respond(cmd, ctx, busyError(released, "frame editing"), true);
        return;
    }
    Command pix = cmd;
    if (pix.s == 0 && pix.v == 0 && pix.format != 100) {
        pix.s = img->width;
        pix.v = img->height;
    }
    ImageFrame patch;
    std::string error;
    if (!decodePixels(pix, std::move(data), ctx, patch, error)) {
        respond(cmd, ctx, error, true);
        return;
    }
    if (patch.width > img->width || patch.height > img->height) {
        respond(cmd, ctx, "EINVAL:frame larger than the image", true);
        return;
    }

    auto canvas = std::make_shared<ImageFrame>();
    if (edits) {
        *canvas = *img->frames[std::size_t(cmd.r - 1)];
    } else if (base > 0) {
        *canvas = *img->frames[std::size_t(base - 1)];
    } else {
        canvas->width = img->width;
        canvas->height = img->height;
        const auto bg = std::uint32_t(cmd.Y);
        const std::uint8_t px[4] = {std::uint8_t(bg >> 24), std::uint8_t(bg >> 16), std::uint8_t(bg >> 8),
                                    std::uint8_t(bg)};
        canvas->rgba.resize(canvasBytes);
        for (std::size_t i = 0; i < canvas->rgba.size(); i += 4) {
            std::copy_n(px, 4, &canvas->rgba[i]);
        }
    }
    blit(*canvas, patch, 0, 0, int(patch.width), int(patch.height), cmd.x, cmd.y, cmd.X == 1);

    if (edits) {
        img->frames[std::size_t(cmd.r - 1)] = std::move(canvas);
        // z=0 leaves an edited frame's gap alone, as for a=a.
        if (cmd.z != 0) {
            img->gaps[std::size_t(cmd.r - 1)] = gap;
        }
    } else {
        img->frames.push_back(std::move(canvas));
        img->gaps.push_back(gap);
    }
    enforceQuota(img->id);
    respond(cmd, ctx, "OK", false);
}

void GraphicsStore::animate(const Command& cmd, Context& ctx)
{
    Image* img = findImage(cmd);
    if (!img) {
        respond(cmd, ctx, "ENOENT:no such image", true);
        return;
    }
    // kitty's handle_animation_control_command, which sends no reply when
    // the image is found: an OK would reach a shell script as stray input.
    if (cmd.r > 0 && std::size_t(cmd.r) <= img->frames.size() && cmd.z != 0) {
        img->gaps[std::size_t(cmd.r - 1)] = cmd.z < 0 ? 0 : std::uint32_t(cmd.z);
    }
    if (cmd.c > 0 && std::size_t(cmd.c) <= img->frames.size()) {
        ++img->frameCommands;
        if (std::size_t(cmd.c - 1) != img->currentFrame) {
            img->currentFrame = std::size_t(cmd.c - 1);
            img->frameShownAtMs = 0; // the next tick starts the clock
        }
    }
    if (cmd.s != 0) {
        ++img->stateCommands;
        const Image::Animation before = img->animation;
        switch (cmd.s) {
        case 1: img->animation = Image::Animation::Stopped; break;
        case 2: img->animation = Image::Animation::Loading; break;
        case 3: img->animation = Image::Animation::Running; break;
        default: break;
        }
        // A running animation keeps its clock when only its mode changes.
        if (img->animation != Image::Animation::Stopped && before == Image::Animation::Stopped) {
            img->frameShownAtMs = 0;
        }
        img->loopsDone = 0;
    }
    if (cmd.v != 0) {
        img->maxLoops = cmd.v - 1;
    }
}

void GraphicsStore::compose(const Command& cmdIn, Context& ctx)
{
    Image* img = findImage(cmdIn);
    if (!img) {
        respond(cmdIn, ctx, "ENOENT:no such image", true);
        return;
    }
    Command cmd = cmdIn;
    cmd.id = img->id;
    if (img->frames.front()->indexed()) {
        respond(cmd, ctx, "EINVAL:indexed images have no frames", true);
        return;
    }
    const std::size_t n = img->frames.size();
    if (cmd.r < 1 || cmd.c < 1 || std::size_t(cmd.r) > n || std::size_t(cmd.c) > n) {
        respond(cmd, ctx, "ENOENT:no such frame", true);
        return;
    }
    // x, y place the rectangle in the destination frame (c=), X, Y in the
    // source frame (r=). The bounds are kitty's: whole rectangles, inside
    // the image, not overlapping within one frame.
    const std::int64_t width = cmd.w ? cmd.w : std::int64_t(img->width);
    const std::int64_t height = cmd.h ? cmd.h : std::int64_t(img->height);
    const std::int64_t dx = cmd.x, dy = cmd.y, sx = cmd.X, sy = cmd.Yi;
    if (width <= 0 || height <= 0 || dx < 0 || dy < 0 || dx + width > img->width || dy + height > img->height) {
        respond(cmd, ctx, "EINVAL:destination rectangle out of bounds", true);
        return;
    }
    if (sx < 0 || sy < 0 || sx + width > img->width || sy + height > img->height) {
        respond(cmd, ctx, "EINVAL:source rectangle out of bounds", true);
        return;
    }
    if (cmd.r == cmd.c && std::max(sx, dx) < std::min(sx, dx) + width && std::max(sy, dy) < std::min(sy, dy) + height) {
        respond(cmd, ctx, "EINVAL:source and destination overlap in one frame", true);
        return;
    }
    const FramePtr& target = img->frames[std::size_t(cmd.c - 1)];
    if (!chargeWork(target->rgba.size())) {
        respond(cmd, ctx, busyError(false, "frame editing"), true);
        return;
    }
    auto dst = std::make_shared<ImageFrame>(*target);
    blit(*dst, *img->frames[std::size_t(cmd.r - 1)], int(sx), int(sy), int(width), int(height), int(dx), int(dy),
         cmd.C == 1);
    img->frames[std::size_t(cmd.c - 1)] = std::move(dst);
    respond(cmd, ctx, "OK", false);
}

bool GraphicsStore::advanceAnimations(const Screen& screen, int cellWidth, int cellHeight, std::uint64_t nowMs,
                                      std::uint64_t& nextDeadlineMs)
{
    bool changed = false;
    nextDeadlineMs = 0;
    // kitty's image_is_animatable(): running or loading, with frames to
    // move through and loops left.
    auto animatable = [](const Image& img) {
        return img.animation != Image::Animation::Stopped && img.frames.size() >= 2
            && (img.maxLoops == 0 || img.loopsDone < img.maxLoops);
    };
    if (std::none_of(m_images.begin(), m_images.end(), [&](const auto& entry) { return animatable(entry.second); })) {
        return false;
    }
    // Only an image drawn in the viewport animates (kitty's is_drawn). One in
    // the scrollback, or a virtual placement without placeholder cells in
    // view, asks for no frames until it is shown again.
    const std::unordered_set<std::uint32_t> shown = shownImages(screen, cellWidth, cellHeight);
    auto due = [&](std::uint64_t at) { nextDeadlineMs = nextDeadlineMs == 0 ? at : std::min(nextDeadlineMs, at); };
    for (auto& [id, img] : m_images) {
        if (!animatable(img) || !shown.contains(id)) {
            continue;
        }
        if (img.frameShownAtMs == 0) {
            img.frameShownAtMs = nowMs;
        }
        // Bounded so frames whose gaps are all zero cannot spin forever.
        bool settled = false;
        for (std::size_t guard = 0; guard <= img.frames.size(); ++guard) {
            const std::uint32_t gap = img.gaps[img.currentFrame];
            if (gap != 0 && nowMs < img.frameShownAtMs + gap) {
                due(img.frameShownAtMs + gap);
                settled = true;
                break;
            }
            const bool last = img.currentFrame + 1 >= img.frames.size();
            if (last) {
                if (img.animation == Image::Animation::Loading) {
                    settled = true;
                    break; // wait on the last frame until more arrive
                }
                // The state stays: a later v= can give the animation more loops.
                if (img.loopsDone < UINT32_MAX) ++img.loopsDone;
                if (img.maxLoops != 0 && img.loopsDone >= img.maxLoops) {
                    settled = true;
                    break;
                }
                img.currentFrame = 0;
            } else {
                ++img.currentFrame;
            }
            // Catch up without drifting, but never fall far behind real time.
            img.frameShownAtMs = (nowMs - img.frameShownAtMs > 1000) ? nowMs : img.frameShownAtMs + gap;
            changed = true;
            ++m_changes;
        }
        if (!settled) {
            // More than a whole cycle behind (a late tick): show the current
            // frame from now rather than stop asking for ticks.
            img.frameShownAtMs = nowMs;
            if (const std::uint32_t gap = img.gaps[img.currentFrame]; gap != 0) {
                due(nowMs + gap);
            }
        }
    }
    return changed;
}

std::unordered_set<std::uint32_t> GraphicsStore::shownImages(const Screen& screen, int cellWidth, int cellHeight) const
{
    // A row more than the screen: smooth scrolling shows part of the next one.
    constexpr int ExtraRows = 1;
    std::unordered_set<std::uint32_t> out;
    for (const VisiblePlacement& v : visible(screen, cellWidth, cellHeight, ExtraRows)) {
        out.insert(v.imageId);
    }
    if (m_virtual.empty() || cellWidth <= 0 || cellHeight <= 0) {
        return out;
    }
    const std::size_t active = screen.historySize() + std::size_t(screen.rows());
    const std::size_t first = screen.viewportTop();
    const std::size_t last = std::min(active, first + std::size_t(screen.rows() + ExtraRows));
    PlaceholderDecoder decoder;
    for (std::size_t y = first; y < last; ++y) {
        const Row& row = screen.absoluteRow(y);
        decoder.reset();
        for (int x = 0; x < screen.cols(); ++x) {
            if (const auto cell = decoder.next(screen, row, x);
                cell && m_virtual.contains(placementKey(cell->imageId, cell->placementId))) {
                out.insert(cell->imageId);
            }
        }
    }
    return out;
}

// --- queries and maintenance -----------------------------------------------

std::vector<VisiblePlacement> GraphicsStore::visible(const Screen& screen, int cw, int ch, int extraRows) const
{
    std::vector<VisiblePlacement> out;
    if (cw <= 0 || ch <= 0) {
        return out;
    }
    Origins origins(*this, screen, Origins::Rows::Viewport, extraRows);
    // Each with its image's order, for kitty's drawing order.
    std::vector<std::pair<std::uint64_t, VisiblePlacement>> drawn;
    const long long top = (long long)screen.viewportTop();
    const long long viewH = static_cast<long long>(screen.rows() + std::clamp(extraRows, 0, screen.rows())) * ch;
    for (std::size_t i = 0; i < m_placements.size(); ++i) {
        const Placement& p = m_placements[i];
        if (p.isVirtual) {
            continue;
        }
        const auto it = m_images.find(p.imageId);
        if (it == m_images.end() || it->second.frames.empty()) {
            continue;
        }
        const auto position = origins.of(i);
        if (!position) continue;
        const auto [col, abs] = *position;
        const Image& img = it->second;
        const SourceRect src = sourceRect(p, img);
        const DisplaySize size = displaySize(p, img, cw, ch);
        if (src.width <= 0 || src.height <= 0 || size.width <= 0 || size.height <= 0) {
            continue;
        }
        const long long px = col * cw + cellOffset(p.offsetX, cw);
        const long long py = (abs - top) * ch + cellOffset(p.offsetY, ch);
        // The view can be taller than an int counts pixels: such positions
        // are off the drawn area anyway.
        if (px < -size.width || px >= static_cast<long long>(screen.cols()) * cw || py < -size.height || py >= viewH
            || px > INT_MAX || py > INT_MAX) {
            continue;
        }
        VisiblePlacement v;
        v.x = int(px);
        v.y = int(py);
        v.width = size.width;
        v.height = size.height;
        v.frame = img.frames[std::min(img.currentFrame, img.frames.size() - 1)];
        v.imageId = img.id;
        v.srcX = int(src.x);
        v.srcY = int(src.y);
        v.srcW = int(src.width);
        v.srcH = int(src.height);
        v.z = p.z;
        drawn.emplace_back(img.order, std::move(v));
    }
    // Stable: placements of one image keep the order they were made in.
    std::stable_sort(drawn.begin(), drawn.end(), [](const auto& a, const auto& b) {
        return a.second.z != b.second.z ? a.second.z < b.second.z : a.first < b.first;
    });
    out.reserve(drawn.size());
    for (auto& entry : drawn) {
        out.push_back(std::move(entry.second));
    }
    return out;
}

std::optional<VisiblePlacement> GraphicsStore::placeholderSlice(std::uint32_t imageId, std::uint32_t placementId,
                                                                int row, int col, int cellX, int cellY, int cw,
                                                                int ch) const
{
    if (cw <= 0 || ch <= 0) {
        return std::nullopt;
    }
    const auto img = m_images.find(imageId);
    if (img == m_images.end() || img->second.frames.empty() || img->second.anonymous) {
        return std::nullopt;
    }
    auto matches = [&](const Placement& p) {
        return p.isVirtual && p.imageId == imageId && (placementId == 0 || p.placementId == placementId);
    };
    const auto indexed = m_virtual.find(placementKey(imageId, placementId));
    if (indexed == m_virtual.end()) {
        return std::nullopt;
    }
    const Placement* pl = nullptr;
    if (indexed->second < m_placements.size() && matches(m_placements[indexed->second])) {
        pl = &m_placements[indexed->second];
    } else {
        const auto it = std::find_if(m_placements.begin(), m_placements.end(), matches);
        pl = it == m_placements.end() ? nullptr : &*it;
    }
    if (!pl) {
        return std::nullopt;
    }
    const Image& image = img->second;
    const double imgW = std::max(1.0, double(image.width));
    const double imgH = std::max(1.0, double(image.height));
    // The placement's grid in cells. A side left out is the image's own size
    // in cells, whatever the other side is, as in kitty (grman_put_cell_image).
    const int cols = pl->cols > 0 ? pl->cols : int((image.width + std::uint32_t(cw) - 1) / std::uint32_t(cw));
    const int rows = pl->rows > 0 ? pl->rows : int((image.height + std::uint32_t(ch) - 1) / std::uint32_t(ch));
    if (row < 0 || col < 0 || row >= rows || col >= cols) {
        return std::nullopt;
    }
    // The image fits the grid with its aspect ratio kept, centred.
    const double gridW = double(cols) * cw;
    const double gridH = double(rows) * ch;
    const double scale = (imgW * gridH > imgH * gridW) ? gridW / imgW : gridH / imgH;
    const double offX = (gridW - imgW * scale) / 2;
    const double offY = (gridH - imgH * scale) / 2;
    // This cell's part of the grid, clipped to the image.
    const double gx0 = std::max(double(col) * cw, offX);
    const double gy0 = std::max(double(row) * ch, offY);
    const double gx1 = std::min((double(col) + 1) * cw, offX + imgW * scale);
    const double gy1 = std::min((double(row) + 1) * ch, offY + imgH * scale);
    if (gx1 <= gx0 || gy1 <= gy0) {
        return std::nullopt;
    }
    VisiblePlacement v;
    v.frame = image.frames[std::min(image.currentFrame, image.frames.size() - 1)];
    v.imageId = imageId;
    v.x = cellX + int(std::lround(gx0 - double(col) * cw));
    v.y = cellY + int(std::lround(gy0 - double(row) * ch));
    v.width = int(std::lround(gx1 - gx0));
    v.height = int(std::lround(gy1 - gy0));
    v.srcX = int(std::lround((gx0 - offX) / scale));
    v.srcY = int(std::lround((gy0 - offY) / scale));
    v.srcW = std::max(1, int(std::lround((gx1 - gx0) / scale)));
    v.srcH = std::max(1, int(std::lround((gy1 - gy0) / scale)));
    v.z = -1; // under the text and the cursor, as kitty draws placeholder images
    return v;
}

long long GraphicsStore::rowsCovered(const Placement& p, int cellWidth, int cellHeight) const
{
    const auto it = m_images.find(p.imageId);
    if (it == m_images.end() || cellWidth <= 0 || cellHeight <= 0) {
        return 1;
    }
    const DisplaySize size = displaySize(p, it->second, cellWidth, cellHeight);
    return std::max(1LL, (static_cast<long long>(size.height) + cellOffset(p.offsetY, cellHeight) + cellHeight - 1)
                             / cellHeight);
}

void GraphicsStore::pruneDead(const Screen& screen, int cellWidth, int cellHeight)
{
    // An image whose top row left the scrollback still shows its lower rows;
    // it goes only when the last of them has left too. Until then it counts
    // toward the limits, and after that nothing can show it again.
    const auto before = m_placements.size();
    std::erase_if(m_placements, [&](const Placement& p) {
        return !p.parentImage && !p.isVirtual && anchorIndex(p, screen) + rowsCovered(p, cellWidth, cellHeight) <= 0;
    });
    if (m_placements.size() != before) {
        pruneOrphans();
    }
}

void GraphicsStore::pruneOrphans()
{
    // A child goes with its parent placement; its image stays (a lowercase
    // delete keeps image data) unless nothing can name it, see settle().
    // Erase after collecting keys: erase_if moves elements while predicates run.
    for (;;) {
        std::unordered_set<std::uint64_t> present;
        std::unordered_set<std::uint32_t> imagesShown;
        for (const auto& p : m_placements) {
            present.insert(placementKey(p.imageId, p.placementId));
            imagesShown.insert(p.imageId);
        }
        auto hasParent = [&](const Placement& p) {
            const auto image = m_images.find(p.parentImage);
            if (image == m_images.end() || image->second.isSixel || image->second.anonymous) return false;
            return p.parentPlacement ? present.contains(placementKey(p.parentImage, p.parentPlacement))
                                     : imagesShown.contains(p.parentImage);
        };
        const auto before = m_placements.size();
        std::erase_if(m_placements, [&](const Placement& p) { return p.parentImage && !hasParent(p); });
        if (m_placements.size() == before) break;
    }
}

void GraphicsStore::clearPlacements(const Screen& screen, int cellWidth, int cellHeight)
{
    const long long history = static_cast<long long>(screen.historySize());
    const long long total = static_cast<long long>(screen.totalRows());
    std::erase_if(m_placements, [&](const Placement& p) {
        if (p.parentImage || p.isVirtual) return false;
        const long long top = anchorIndex(p, screen);
        const long long bottom = top + rowsCovered(p, cellWidth, cellHeight);
        return bottom > history || bottom <= 0 || top >= total;
    });
    settle();
}

void GraphicsStore::eraseRows(int firstRow, int lastRow, const Screen& screen, int cellWidth, int cellHeight)
{
    // The erased screen rows, as buffer rows: the top of the screen is the
    // first row after the history.
    const long long history = static_cast<long long>(screen.historySize());
    const long long eraseTop = history + firstRow;
    const long long eraseBottom = history + lastRow;
    std::erase_if(m_placements, [&](const Placement& p) {
        if (p.parentImage || p.isVirtual) return false;
        const long long top = anchorIndex(p, screen);
        const long long bottom = top + rowsCovered(p, cellWidth, cellHeight);
        return top <= eraseBottom && bottom > eraseTop;
    });
    settle();
}

std::vector<std::size_t> GraphicsStore::anchorRows(const Screen& screen) const
{
    std::vector<std::size_t> out;
    out.reserve(m_placements.size());
    const long long total = static_cast<long long>(screen.totalRows());
    for (const Placement& p : m_placements) {
        const long long abs = anchorIndex(p, screen);
        const bool anchored = !p.parentImage && !p.isVirtual && abs >= 0 && abs < total;
        out.push_back(anchored ? std::size_t(abs) : Screen::TrackedRowGone);
    }
    return out;
}

std::vector<int> GraphicsStore::anchorColumns() const
{
    std::vector<int> out;
    out.reserve(m_placements.size());
    for (const Placement& p : m_placements) {
        out.push_back(p.parentImage || p.isVirtual ? 0 : p.col);
    }
    return out;
}

void GraphicsStore::retarget(const Screen& screen, const std::vector<std::size_t>& rows, const std::vector<int>* columns,
                             int cellWidth, int cellHeight)
{
    if (rows.size() != m_placements.size() || (columns && columns->size() != rows.size())) {
        return;
    }
    std::vector<char> gone(m_placements.size(), 0);
    for (std::size_t i = 0; i < m_placements.size(); ++i) {
        Placement& p = m_placements[i];
        if (p.parentImage || p.isVirtual) continue;
        if (rows[i] == Screen::TrackedRowGone) {
            // Without a reflow the row ids still hold: an image whose top
            // row left the buffer keeps its anchor while its lower rows show.
            // With no cell size it counts one row, so it goes.
            const long long top = anchorIndex(p, screen);
            gone[i] = top >= 0 || top + rowsCovered(p, cellWidth, cellHeight) <= 0;
            continue;
        }
        p.row = screen.rowsDropped() + rows[i];
        if (columns) p.col = (*columns)[i];
    }
    eraseFlagged(m_placements, gone);
    settle();
}

void GraphicsStore::scrollRegion(const RegionScroll& scroll, const Screen& screen, int cw, int ch)
{
    const std::uint64_t lastRow =
        screen.rowsDropped() + screen.historySize() + std::uint64_t(std::max(0, screen.rows() - 1));
    const bool cutBelow = scroll.delta > 0 || scroll.bottom < lastRow;
    const long long regionRows = static_cast<long long>(scroll.bottom - scroll.top) + 1;
    const long long regionPixels = regionRows * ch;
    // Cuts @p p, whose top-left cell is @p rel rows below the region's top
    // after the move, to the rows still inside the region. Returns the row
    // that cell moves to, or nullopt when nothing of @p p remains.
    auto clip = [&](Placement& p, long long rel) -> std::optional<long long> {
        const auto img = m_images.find(p.imageId);
        const DisplaySize size = img != m_images.end() && cw > 0 && ch > 0 ? displaySize(p, img->second, cw, ch)
                                                                           : DisplaySize{0, 0};
        const SourceRect src = img != m_images.end() ? sourceRect(p, img->second) : SourceRect{};
        if (size.width <= 0 || size.height <= 0 || src.width <= 0 || src.height <= 0) {
            // Nothing to measure its rows by: it is its top-left cell.
            if (rel < 0 || rel >= regionRows) return std::nullopt;
            return rel;
        }
        const long long y0 = rel * ch + cellOffset(p.offsetY, ch);
        const long long cutTop = std::max(0LL, -y0);
        const long long cutBottom = cutBelow ? std::max(0LL, y0 + size.height - regionPixels) : 0;
        const long long kept = size.height - cutTop - cutBottom;
        if (kept <= 0) return std::nullopt;
        long long anchorRel = rel;
        if (rel < 0) {
            // The top-left cell left the top: the first row still shown becomes it.
            const long long top = y0 + cutTop;
            anchorRel = top / ch;
            p.offsetY = int(top - anchorRel * ch);
        }
        if (cutTop == 0 && cutBottom == 0) return anchorRel;
        // The source shrinks in proportion. Rounding down keeps at least one
        // source row, and at most a fraction of one more than is shown.
        const long long srcTop = cutTop * src.height / size.height;
        const long long srcBottom = cutBottom * src.height / size.height;
        p.srcY = int(src.y + srcTop);
        p.srcH = int(src.height - srcTop - srcBottom);
        // The display size is now explicit; displaySize() takes it as the
        // aspect for whichever side the cells still give. A row count stays
        // only while the kept part is whole rows.
        p.displayWidth = std::uint32_t(size.width);
        p.displayHeight = std::uint32_t(kept);
        if (p.rows > 0) p.rows = kept % ch == 0 ? int(kept / ch) : 0;
        return anchorRel;
    };

    enum class State : std::uint8_t { Unknown, Visiting, Still, Moved, Gone };
    std::vector<State> state(m_placements.size(), State::Unknown);
    // For a moved placement: its top-left row relative to the region's top,
    // after the move and before the cut, and where the cut left it.
    std::vector<long long> movedRow(m_placements.size(), 0);
    std::vector<long long> keptRow(m_placements.size(), 0);
    bool changed = false;
    bool children = false;
    for (std::size_t i = 0; i < m_placements.size(); ++i) {
        Placement& p = m_placements[i];
        if (p.parentImage) {
            children = true;
            continue;
        }
        const SelectionPoint anchor{p.row, p.col};
        if (p.isVirtual || !scroll.contains(anchor)) {
            state[i] = State::Still;
            continue;
        }
        changed = true;
        movedRow[i] = static_cast<long long>(p.row - scroll.top) + scroll.delta;
        const auto kept = clip(p, movedRow[i]);
        if (!kept) {
            state[i] = State::Gone;
            continue;
        }
        keptRow[i] = *kept;
        p.row = scroll.top + std::uint64_t(*kept);
        state[i] = State::Moved;
    }
    if (!changed) return;
    if (children) {
        // A child moves with its parent's cells and is cut like a root image.
        // Its origin stays where the move took it, so its offset changes by
        // as much as its parent's origin moved down in the cut. A child of a
        // placement that went goes with it.
        Origins origins(*this, screen, Origins::Rows::Active);
        auto resolve = [&](auto& self, std::size_t i) -> State {
            if (state[i] == State::Visiting) return State::Still; // a cycle; place() refuses them
            if (state[i] != State::Unknown) return state[i];
            state[i] = State::Visiting;
            Placement& p = m_placements[i];
            const auto parent = origins.parentOf(p);
            const State up = parent ? self(self, *parent) : State::Still;
            if (up != State::Moved) {
                state[i] = up == State::Gone ? State::Gone : State::Still;
                return state[i];
            }
            movedRow[i] = movedRow[*parent] + p.relativeY;
            const auto kept = clip(p, movedRow[i]);
            if (!kept) {
                state[i] = State::Gone;
                return state[i];
            }
            keptRow[i] = *kept;
            p.relativeY = int(std::clamp<long long>(*kept - keptRow[*parent], INT_MIN, INT_MAX));
            state[i] = State::Moved;
            return state[i];
        };
        for (std::size_t i = 0; i < m_placements.size(); ++i) {
            if (m_placements[i].parentImage) resolve(resolve, i);
        }
    }
    std::vector<char> gone(m_placements.size(), 0);
    for (std::size_t i = 0; i < m_placements.size(); ++i) gone[i] = state[i] == State::Gone;
    eraseFlagged(m_placements, gone);
    settle();
}

void GraphicsStore::clear()
{
    ++m_changes;
    m_images.clear();
    m_placements.clear();
    m_virtual.clear();
    m_pending = Pending{};
}

void GraphicsStore::setQuota(std::size_t bytes)
{
    m_quota = bytes;
    enforceQuota();
    settle();
}

std::size_t GraphicsStore::totalBytes() const
{
    std::size_t total = 0;
    for (const auto& [id, img] : m_images) {
        total += img.byteSize();
    }
    return total;
}

void GraphicsStore::enforceQuota(std::uint32_t keep)
{
    std::size_t total = totalBytes();
    std::size_t frames = 0;
    for (const auto& [id, img] : m_images) {
        frames += img.frames.size();
    }
    if (total <= m_quota && m_images.size() <= MaxImages && frames <= MaxStoreFrames) {
        return;
    }
    // One pass: rank every image once, unused before used, transient before
    // the rest among the unused, older before newer, and take victims from
    // the front until the limits hold.
    std::unordered_set<std::uint32_t> used;
    for (const Placement& p : m_placements) {
        used.insert(p.imageId);
    }
    struct Candidate
    {
        bool used;
        bool transient;
        std::uint64_t sequence;
        std::uint32_t id;
        std::size_t bytes;
        std::size_t frames;
    };
    std::vector<Candidate> order;
    order.reserve(m_images.size());
    for (const auto& [id, img] : m_images) {
        if (id != keep) order.push_back({used.contains(id), img.transient, img.sequence, id, img.byteSize(), img.frames.size()});
    }
    std::sort(order.begin(), order.end(), [](const Candidate& a, const Candidate& b) {
        if (a.used != b.used) return !a.used;
        if (!a.used && a.transient != b.transient) return a.transient;
        return a.sequence < b.sequence;
    });
    std::unordered_set<std::uint32_t> evicted;
    std::size_t count = m_images.size();
    for (const Candidate& c : order) {
        if (total <= m_quota && count <= MaxImages && frames <= MaxStoreFrames) break;
        total -= c.bytes;
        frames -= c.frames;
        --count;
        evicted.insert(c.id);
    }
    if (evicted.empty()) {
        return;
    }
    std::erase_if(m_placements, [&](const Placement& p) { return evicted.contains(p.imageId); });
    for (const std::uint32_t id : evicted) {
        m_images.erase(id);
    }
    pruneOrphans();
}

bool GraphicsStore::evictUnusedImage()
{
    std::unordered_set<std::uint32_t> used;
    for (const Placement& p : m_placements) {
        used.insert(p.imageId);
    }
    auto victim = m_images.end();
    for (auto it = m_images.begin(); it != m_images.end(); ++it) {
        if (used.contains(it->first)) {
            continue;
        }
        if (victim == m_images.end() || it->second.transient > victim->second.transient
            || (it->second.transient == victim->second.transient && it->second.sequence < victim->second.sequence)) {
            victim = it;
        }
    }
    if (victim == m_images.end()) {
        return false;
    }
    m_images.erase(victim);
    return true;
}

void GraphicsStore::settle()
{
    ++m_changes;
    pruneOrphans();
    // Internal and anonymous images go with their last placement: nothing can
    // name them to show them again. Images with an id or a number stay.
    std::unordered_set<std::uint32_t> used;
    for (const Placement& p : m_placements) {
        used.insert(p.imageId);
    }
    std::erase_if(m_images, [&](const auto& entry) {
        return (entry.second.isSixel || entry.second.anonymous) && !used.contains(entry.first);
    });
    indexVirtual();
}

void GraphicsStore::indexVirtual()
{
    m_virtual.clear();
    for (std::size_t i = 0; i < m_placements.size(); ++i) {
        const Placement& p = m_placements[i];
        if (p.isVirtual) {
            m_virtual.try_emplace(placementKey(p.imageId, p.placementId), i);
            m_virtual.try_emplace(placementKey(p.imageId, 0), i);
        }
    }
}

void GraphicsStore::setWorkBudget(std::size_t burst, std::size_t bytesPerSecond)
{
    m_workBurst = double(burst);
    m_workRate = double(bytesPerSecond);
    m_workBytes = -1;
}

bool GraphicsStore::chargeWork(std::size_t bytes)
{
    const std::int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    if (m_workBytes < 0) {
        m_workBytes = m_workBurst;
    } else {
        m_workBytes = std::min(m_workBurst, m_workBytes + double(now - m_workStampNs) * 1e-9 * m_workRate);
    }
    m_workStampNs = now;
    if (double(bytes) > m_workBytes) {
        return false;
    }
    m_workBytes -= double(bytes);
    return true;
}

} // namespace AfterglowVt
