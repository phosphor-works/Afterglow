// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Graphics.h>

#include "Base64.h"
#include <AfterglowVt/Screen.h>
#include <AfterglowVt/Terminal.h>

#include <zlib.h>

#include <algorithm>
#include <charconv>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <filesystem>
#include <cmath>
#include <unordered_set>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace AfterglowVt {

namespace {
#include "PlaceholderDiacritics.inc"
} // namespace

int placeholderDiacriticValue(char32_t cp)
{
    const auto* end = std::end(PlaceholderDiacritics);
    const auto* it = std::lower_bound(std::begin(PlaceholderDiacritics), end, cp);
    return (it != end && *it == cp) ? int(it - std::begin(PlaceholderDiacritics)) : -1;
}

namespace {

constexpr std::uint32_t MaxDimension = 10000;
constexpr std::size_t MaxPayload = GraphicsStore::DefaultQuota;
constexpr std::size_t MaxControl = 4096;
constexpr int MaxDepth = 32; ///< relative placement chain links
/// Frame edits (a=f on a frame, a=c) copy a whole frame for a command of a
/// few bytes: their copying is limited per second, with a burst for one
/// large edit.
constexpr double WorkBytesPerSecond = double(std::size_t(1) << 30);
constexpr double WorkBurst = double(std::size_t(1) << 30);

bool inflateZlib(const std::string& in, std::string& out)
{
    z_stream zs{};
    if (inflateInit(&zs) != Z_OK) {
        return false;
    }
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
    zs.avail_in = uInt(in.size());
    char buf[65536];
    int rc = Z_OK;
    while (rc == Z_OK) {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof buf;
        rc = inflate(&zs, Z_NO_FLUSH);
        const auto produced = sizeof buf - zs.avail_out;
        if (produced > MaxPayload - out.size()) { rc = Z_DATA_ERROR; break; }
        out.append(buf, produced);
    }
    inflateEnd(&zs);
    return rc == Z_STREAM_END;
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

/// Copies a rectangle of @p src into @p dst at (dx, dy), clipped.
void blit(ImageFrame& dst, const ImageFrame& src, int sx, int sy, int w, int h, int dx, int dy, bool overwrite)
{
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

/// The stable row of a non-virtual root placement as a signed absolute
/// index: negative once the row has left the scrollback, while the image's
/// lower rows can still be on screen.
long long anchorIndex(const Placement& p, const Screen& s)
{
    return static_cast<long long>(p.row) - static_cast<long long>(s.rowsDropped());
}

constexpr std::string_view TemporaryTag = "tty-graphics-protocol";

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

bool readPayload(int fd, std::uint32_t offset, std::uint32_t size, std::string& data, std::string& error)
{
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0
        || std::uint64_t(offset) > std::uint64_t(st.st_size)) {
        error = "EBADF:invalid image file extent";
        return false;
    }
    const std::uint64_t remaining = std::uint64_t(st.st_size) - offset;
    const std::uint64_t want = size ? std::min<std::uint64_t>(size, remaining) : remaining;
    if (want > MaxPayload || size > MaxPayload) { error = "EFBIG:image payload too large"; return false; }
    data.resize(std::size_t(want));
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = pread(fd, data.data() + done, std::min<std::size_t>(65536, data.size() - done), off_t(offset) + off_t(done));
        if (n > 0) done += std::size_t(n);
        else if (n < 0 && errno == EINTR) continue;
        else if (n == 0) { data.resize(done); break; }
        else { error = "EBADF:cannot read image payload"; data.clear(); return false; }
    }
    return true;
}

bool insideDirectory(const std::filesystem::path& path, const std::filesystem::path& directory)
{
    auto p = path.begin();
    for (auto d = directory.begin(); d != directory.end(); ++d, ++p)
        if (p == path.end() || *p != *d) return false;
    return p != path.end();
}

/// Whether a resolved path lies in a directory where the terminal may delete
/// a t=t file: /tmp, /dev/shm or $TMPDIR, each resolved, so a trailing slash
/// or a symbolic link in the setting still matches.
bool temporaryLocation(const std::filesystem::path& path)
{
    std::error_code ec;
    std::vector<std::filesystem::path> dirs{"/tmp", "/dev/shm"};
    if (auto tmp = std::filesystem::temp_directory_path(ec); !ec && !tmp.empty()) dirs.push_back(std::move(tmp));
    for (const auto& dir : dirs) {
        std::error_code resolved;
        const auto real = std::filesystem::canonical(dir, resolved);
        if (!resolved && insideDirectory(path, real)) return true;
    }
    return false;
}

} // namespace

std::size_t Image::byteSize() const
{
    std::size_t total = 0;
    for (const FramePtr& f : frames) {
        total += f->rgba.size();
    }
    return total;
}

// --- placeholders -----------------------------------------------------------

std::optional<PlaceholderCell> PlaceholderDecoder::next(const Screen& screen, const Row& row, int x)
{
    const Cell& cell = row.at(x);
    if (cell.codepoint() != PlaceholderCodepoint) {
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
    const int r = extra.size() > 0 ? placeholderDiacriticValue(extra[0]) : -1;
    const int c = extra.size() > 1 ? placeholderDiacriticValue(extra[1]) : -1;
    const int high = extra.size() > 2 ? placeholderDiacriticValue(extra[2]) : -1;
    const bool continues = m_previous && (out.imageId & 0xFFFFFF) == (m_last.imageId & 0xFFFFFF)
        && out.placementId == m_last.placementId;
    if (high >= 0) {
        out.imageId |= std::uint32_t(high) << 24;
    } else if (continues) {
        out.imageId = m_last.imageId;
    }
    out.row = r >= 0 ? r : continues ? m_last.row : 0;
    out.col = c >= 0 ? c : continues ? m_last.col + 1 : 0;
    m_previous = true;
    m_last = out;
    return out;
}

// --- command parsing --------------------------------------------------------

struct GraphicsStore::Command
{
    char action = 't';
    char medium = 'd';
    char del = 'a';
    char compression = 0;
    std::uint32_t format = 32;
    std::uint32_t s = 0; // width, or animation state for a=a
    std::uint32_t v = 0; // height, or loop count for a=a
    std::uint32_t size = 0; // S
    std::uint32_t offset = 0; // O
    std::uint32_t id = 0; // i
    std::uint32_t number = 0; // I
    std::uint32_t placement = 0; // p
    int more = 0;
    int quiet = 0;
    int x = 0;
    int y = 0;
    std::uint32_t ux = 0; // x and y unsigned, for the id range of d=r
    std::uint32_t uy = 0;
    int w = 0;
    int h = 0;
    int X = 0;
    std::int64_t Y = 0; // 64-bit for a frame's background colour; Yi elsewhere
    int Yi = 0;
    int c = 0;
    int r = 0;
    int C = 0;
    std::int32_t z = 0;
    int U = 0;
    std::uint32_t P = 0, Q = 0;
    int H = 0, V = 0;
    // Which keys the command carried, to tell a new command from a chunk.
    bool hasAction = false;
    bool hasId = false;
    bool hasNumber = false;

    static Command parse(std::string_view keys)
    {
        Command cmd;
        std::size_t pos = 0;
        while (pos < keys.size()) {
            const auto comma = keys.find(',', pos);
            const std::string_view kv = keys.substr(pos, comma == std::string_view::npos ? comma : comma - pos);
            pos = comma == std::string_view::npos ? keys.size() : comma + 1;
            if (kv.size() < 3 || kv[1] != '=') {
                continue;
            }
            const char key = kv[0];
            const std::string_view value = kv.substr(2);
            if (key == 'a' || key == 't' || key == 'd' || key == 'o') {
                const char ch = value.empty() ? 0 : value[0];
                (key == 'a' ? cmd.action : key == 't' ? cmd.medium : key == 'd' ? cmd.del : cmd.compression) = ch;
                cmd.hasAction = cmd.hasAction || key == 'a';
                continue;
            }
            std::int64_t n = 0;
            if (std::from_chars(value.data(), value.data() + value.size(), n).ec != std::errc{}) {
                continue;
            }
            const auto u = std::uint32_t(std::clamp<std::int64_t>(n, 0, 0xFFFFFFFF));
            const auto i = int(std::clamp<std::int64_t>(n, -0x7FFFFFFF, 0x7FFFFFFF));
            switch (key) {
            case 'f': cmd.format = u; break;
            case 's': cmd.s = u; break;
            case 'v': cmd.v = u; break;
            case 'S': cmd.size = u; break;
            case 'O': cmd.offset = u; break;
            case 'i': cmd.id = u; cmd.hasId = true; break;
            case 'I': cmd.number = u; cmd.hasNumber = true; break;
            case 'p': cmd.placement = u; break;
            case 'm': cmd.more = i; break;
            case 'q': cmd.quiet = i; break;
            case 'x': cmd.x = i; cmd.ux = u; break;
            case 'y': cmd.y = i; cmd.uy = u; break;
            case 'w': cmd.w = i; break;
            case 'h': cmd.h = i; break;
            case 'X': cmd.X = i; break;
            case 'Y': cmd.Y = n; cmd.Yi = i; break;
            case 'c': cmd.c = i; break;
            case 'r': cmd.r = i; break;
            case 'C': cmd.C = i; break;
            case 'z': cmd.z = i; break;
            case 'U': cmd.U = i; break;
            case 'P': cmd.P = u; break;
            case 'Q': cmd.Q = u; break;
            case 'H': cmd.H = i; break;
            case 'V': cmd.V = i; break;
            default: break;
            }
        }
        return cmd;
    }
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
        const auto image = m_store.m_images.find(p.parentImage);
        if (image == m_store.m_images.end() || image->second.isSixel) return std::nullopt;
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
    dispatch(payload, ctx);
    settle();
}

void GraphicsStore::dispatch(std::string_view payload, Context& ctx)
{
    const auto semi = payload.find(';');
    const std::string_view keys = payload.substr(0, semi);
    const std::string_view data = semi == std::string_view::npos ? std::string_view{} : payload.substr(semi + 1);
    const Command cmd = Command::parse(keys);
    pruneDead(ctx.screen, ctx.cellWidth, ctx.cellHeight);

    if (m_pending.active) {
        // Chunks after the first carry only m= and q=. A command that names
        // another action or image is new: the client that left this transfer
        // unfinished is gone, and its tail must not swallow the next client's
        // first command.
        const Command first = Command::parse(m_pending.controlData);
        const bool another = (cmd.hasAction && cmd.action != first.action) || (cmd.hasId && cmd.id != first.id)
            || (cmd.hasNumber && cmd.number != first.number);
        if (another) {
            m_pending = Pending{};
        } else {
            // Reject the entire transmission, including tails after an oversized chunk.
            if (!m_pending.failed) {
                if (data.size() > ((MaxPayload - m_pending.data.size() + 2) / 3) * 4) {
                    m_pending.failed = true;
                    m_pending.data.clear();
                    respond(first, ctx, "EFBIG:image payload too large", true);
                } else {
                    detail::base64DecodeAppend(m_pending.data, data, detail::Base64Check::Lenient);
                    if (m_pending.data.size() > MaxPayload) {
                        m_pending.failed = true;
                        m_pending.data.clear();
                        respond(first, ctx, "EFBIG:image payload too large", true);
                    }
                }
            }
            if (cmd.more == 1) return;
            std::string all = std::move(m_pending.data);
            const bool failed = m_pending.failed;
            m_pending = Pending{};
            if (!failed) execute(first, std::move(all), ctx);
            return;
        }
    }

    const bool chunked = cmd.more == 1 && (cmd.action == 't' || cmd.action == 'T' || cmd.action == 'q' || cmd.action == 'f');
    const bool oversized = keys.size() > MaxControl || data.size() > ((MaxPayload + 2) / 3) * 4;
    if (oversized) {
        respond(cmd, ctx, "EFBIG:image payload too large", true);
        if (chunked) {
            m_pending.controlData.assign(keys.substr(0, MaxControl));
            m_pending.active = true;
            m_pending.failed = true;
        }
        return;
    }
    std::string decoded = detail::base64Decode(data);
    if (decoded.size() > MaxPayload) {
        respond(cmd, ctx, "EFBIG:image payload too large", true);
        if (chunked) {
            m_pending.controlData.assign(keys);
            m_pending.active = true;
            m_pending.failed = true;
        }
        return;
    }
    if (chunked) {
        m_pending.controlData.assign(keys);
        m_pending.data = std::move(decoded);
        m_pending.active = true;
        return;
    }
    execute(cmd, std::move(decoded), ctx);
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
    std::string out = "\x1b_G";
    char buf[64];
    int n = 0;
    // i= only for a known id: a command that gives I= alone gets the id
    // filled in once its image is found, and no i=0 otherwise.
    if (cmd.id) {
        n = std::snprintf(buf, sizeof buf, "i=%u", cmd.id);
        out.append(buf, std::size_t(n));
    }
    if (cmd.number) {
        n = std::snprintf(buf, sizeof buf, cmd.id ? ",I=%u" : "I=%u", cmd.number);
        out.append(buf, std::size_t(n));
    }
    if (cmd.placement) {
        n = std::snprintf(buf, sizeof buf, ",p=%u", cmd.placement);
        out.append(buf, std::size_t(n));
    }
    out += ';';
    out += status;
    out += "\x1b\\";
    ctx.reply(out);
}

// --- transmission -----------------------------------------------------------

bool GraphicsStore::loadPayload(const Command& cmd, std::string& data, std::string& error)
{
    const bool fromFile = cmd.medium == 'f' || cmd.medium == 't' || cmd.medium == 's';
    if (fromFile && !m_fileTransfer) {
        error = "EPERM:file transmission is turned off";
        return false;
    }
    switch (cmd.medium) {
    case 'd':
        break;
    case 'f':
    case 't': {
        if (data.size() > 4096 || data.find('\0') != std::string::npos) {
            error = "EBADF:not a readable regular file"; return false;
        }
        std::error_code ec;
        const auto canonical = std::filesystem::canonical(data, ec);
        if (ec || forbiddenFile(canonical)) { error = "EBADF:not a readable regular file"; return false; }
        // Pin the parent directory and validate the actual opened file. O_NONBLOCK
        // prevents a replaced FIFO from hanging before fstat can reject it.
        const int parent = open(canonical.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (parent < 0) { error = "EBADF:cannot open file"; return false; }
        const auto actualParent = std::filesystem::read_symlink("/proc/self/fd/" + std::to_string(parent), ec);
        if (ec || forbiddenFile(actualParent)) { close(parent); error = "EBADF:not a readable regular file"; return false; }
        const auto name = canonical.filename();
        const int fd = openat(parent, name.c_str(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
        struct stat opened{};
        if (fd < 0 || fstat(fd, &opened) != 0 || !S_ISREG(opened.st_mode)) {
            if (fd >= 0) close(fd);
            close(parent); error = "EBADF:not a readable regular file"; return false;
        }
        const bool success = readPayload(fd, cmd.offset, cmd.size, data, error);
        // A temporary file is the terminal's to delete whether or not it
        // could use it; only the file that was read, though.
        if (cmd.medium == 't' && name.string().find(TemporaryTag) != std::string::npos
            && temporaryLocation(actualParent / name)) {
            struct stat current{};
            if (fstatat(parent, name.c_str(), &current, AT_SYMLINK_NOFOLLOW) == 0
                && current.st_dev == opened.st_dev && current.st_ino == opened.st_ino)
                unlinkat(parent, name.c_str(), 0);
        }
        close(fd);
        close(parent);
        if (!success) return false;
        break;
    }
    case 's': {
        const std::string name = data;
        if (name.size() > 4096 || name.find('\0') != std::string::npos) { error = "EBADF:invalid shared memory name"; return false; }
        // O_NONBLOCK: a FIFO planted in /dev/shm must not hang the open before
        // readPayload's fstat rejects it.
        const int fd = shm_open(name.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0);
        if (fd < 0) { error = "EBADF:cannot open shared memory"; return false; }
        // pread is safe if the sender truncates the object concurrently; mmap
        // would instead deliver SIGBUS to the terminal process.
        const bool success = readPayload(fd, cmd.offset, cmd.size, data, error);
        close(fd);
        // Only an object named for the protocol is the terminal's to delete.
        // Any other name stays: a command must not delete an unrelated object
        // by naming it, and almost any object decodes as a tiny image.
        if (name.find(TemporaryTag) != std::string::npos) shm_unlink(name.c_str());
        if (!success) return false;
        break;
    }
    default:
        error = "EINVAL:unknown transmission medium";
        return false;
    }
    if (cmd.compression == 'z') {
        std::string inflated;
        if (!inflateZlib(data, inflated)) {
            error = "EINVAL:zlib decompression failed";
            return false;
        }
        data = std::move(inflated);
    }
    return true;
}

void GraphicsStore::releaseMedium(const Command& cmd, const std::string& data) const
{
    if (!m_fileTransfer || data.size() > 4096 || data.find('\0') != std::string::npos
        || data.find(TemporaryTag) == std::string::npos) {
        return;
    }
    if (cmd.medium == 's') {
        shm_unlink(data.c_str());
        return;
    }
    if (cmd.medium != 't') return;
    std::error_code ec;
    const auto canonical = std::filesystem::canonical(data, ec);
    if (ec || canonical.filename().string().find(TemporaryTag) == std::string::npos) return;
    const int parent = open(canonical.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent < 0) return;
    const auto actualParent = std::filesystem::read_symlink("/proc/self/fd/" + std::to_string(parent), ec);
    const auto name = canonical.filename();
    struct stat st{};
    if (!ec && temporaryLocation(actualParent / name) && fstatat(parent, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0
        && S_ISREG(st.st_mode)) {
        unlinkat(parent, name.c_str(), 0);
    }
    close(parent);
}

bool GraphicsStore::decodePixels(const Command& cmd, std::string data, Context& ctx, ImageFrame& out,
                                 std::string& error)
{
    if (!loadPayload(cmd, data, error)) {
        return false;
    }
    if (cmd.format == 100) {
        if (!ctx.host.decodePng(data, out.width, out.height, out.rgba)) {
            error = "EBADPNG:failed to decode PNG data";
            return false;
        }
    } else if (cmd.format == 24 || cmd.format == 32) {
        const std::size_t bpp = cmd.format / 8;
        if (cmd.s == 0 || cmd.v == 0 || cmd.s > MaxDimension || cmd.v > MaxDimension) {
            error = "EINVAL:missing or invalid image size";
            return false;
        }
        const std::size_t expected = std::size_t(cmd.s) * cmd.v * bpp;
        if (std::size_t(cmd.s) * cmd.v * 4 > MaxPayload) { error = "EFBIG:image payload too large"; return false; }
        if (data.size() < expected) {
            error = "ENODATA:insufficient image data";
            return false;
        }
        out.width = cmd.s;
        out.height = cmd.v;
        if (bpp == 4) {
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
    } else {
        error = "EINVAL:unknown format";
        return false;
    }
    if (out.width == 0 || out.height == 0 || out.width > MaxDimension || out.height > MaxDimension) {
        error = "EINVAL:invalid image dimensions";
        return false;
    }
    const auto expectedBytes = std::size_t(out.width) * out.height * 4;
    if (expectedBytes > MaxPayload || out.rgba.size() != expectedBytes) {
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
    if (frame.rgba.size() > m_quota) {
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
    const bool replaces = cmd.id && findImage(Command{.id = cmd.id});
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
    if (auto existing = m_images.find(id); existing != m_images.end() && existing->second.isSixel) {
        // Kitty may explicitly claim any ID, including an internal image's.
        // Keep that image and its anchors, relocating it before replacement.
        const auto replacement = allocateSixelId();
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

    Image img;
    img.id = id;
    img.number = cmd.number;
    img.anonymous = cmdIn.id == 0 && cmdIn.number == 0;
    img.width = frame.width;
    img.height = frame.height;
    img.frames.push_back(std::make_shared<const ImageFrame>(std::move(frame)));
    img.gaps.push_back(40);
    img.sequence = ++m_sequence;
    m_images[id] = std::move(img);
    enforceQuota(id);

    if (cmd.action == 'T') {
        Command pc = cmd;
        pc.id = id;
        if (cmd.id == 0 && cmd.number == 0) pc.quiet = 2;
        place(pc, ctx); // Placement errors must not be hidden by an earlier OK.
    } else {
        respond(cmd, ctx, "OK", false);
    }
}

Image* GraphicsStore::findImage(const Command& cmd)
{
    if (cmd.id) {
        auto it = m_images.find(cmd.id);
        return it == m_images.end() || it->second.isSixel ? nullptr : &it->second;
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

void GraphicsStore::place(const Command& cmdIn, Context& ctx)
{
    Image* img = findImage(cmdIn);
    if (!img) {
        respond(cmdIn, ctx, "ENOENT:no such image", true);
        return;
    }
    Command cmd = cmdIn;
    cmd.id = img->id; // a command that gave only I= learns the id
    Screen& scr = ctx.screen;
    Placement pl;
    pl.imageId = img->id;
    pl.placementId = cmd.placement ? cmd.placement : m_nextPlacementId++;
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
            if (++depth > MaxDepth) { respond(cmd, ctx, "ETOODEEP:relative placement depth exceeds 32", true); return; }
        }
        // Reparenting also lengthens existing descendant chains: walk them
        // level by level from this placement, through a map of children.
        std::unordered_map<std::uint64_t, std::vector<std::uint64_t>> children;
        for (const Placement& p : m_placements) {
            if (p.parentImage) children[placementKey(p.parentImage, p.parentPlacement)].push_back(placementKey(p.imageId, p.placementId));
        }
        std::vector<std::uint64_t> level{key};
        for (int below = 0; !level.empty(); ++below) {
            if (depth + below > MaxDepth) { respond(cmd, ctx, "ETOODEEP:descendant depth exceeds 32", true); return; }
            std::vector<std::uint64_t> next;
            for (const auto k : level) {
                if (const auto it = children.find(k); it != children.end()) next.insert(next.end(), it->second.begin(), it->second.end());
            }
            level = std::move(next);
        }
    }
    const bool replacing = std::any_of(m_placements.begin(), m_placements.end(),
                                       [&](const Placement& p) { return placementKey(p.imageId, p.placementId) == key; });
    if (!replacing && m_placements.size() >= MaxPlacements) { respond(cmd, ctx, "ENOSPC:too many placements", true); return; }
    std::erase_if(m_placements, [&](const Placement& p) { return placementKey(p.imageId, p.placementId) == key; });
    pl.row = scr.rowsDropped() + scr.historySize() + std::size_t(scr.cursor.y);
    pl.col = scr.cursor.x;
    pl.offsetX = std::clamp(cmd.X, 0, 1 << 28);
    pl.offsetY = int(std::clamp<std::int64_t>(cmd.Y, 0, 1 << 28));
    pl.srcX = std::max(0, cmd.x);
    pl.srcY = std::max(0, cmd.y);
    pl.srcW = std::max(0, cmd.w);
    pl.srcH = std::max(0, cmd.h);
    pl.cols = std::max(0, cmd.c);
    pl.rows = std::max(0, cmd.r);
    pl.z = cmd.z;
    pl.isVirtual = cmd.U == 1;
    m_placements.push_back(pl);

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

std::uint32_t GraphicsStore::allocateSixelId()
{
    std::uint32_t id;
    do {
        id = m_nextSixelId++;
        if (m_nextSixelId >= (1u << 31)) m_nextSixelId = 3u << 29;
    } while (m_images.contains(id));
    return id;
}

void GraphicsStore::addSixel(ImageFrame frame, const Screen& screen, int row, int col, int cellWidth, int cellHeight,
                             std::uint32_t displayWidth, std::uint32_t displayHeight)
{
    if (cellWidth <= 0 || cellHeight <= 0) {
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
    img.id = allocateSixelId();
    img.isSixel = true;
    img.width = frame.width;
    img.height = frame.height;
    img.frames.push_back(std::make_shared<const ImageFrame>(std::move(frame)));
    img.gaps.push_back(0);
    img.sequence = ++m_sequence;
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
    if (std::string_view("aincpqxyzr").find(kind) == std::string_view::npos) {
        // Includes f/F (animation frames), which are not supported.
        respond(cmd, ctx, "EINVAL:unsupported delete target", true);
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
            r.cols = std::max(1LL, (static_cast<long long>(size.width) + p.offsetX + ctx.cellWidth - 1) / ctx.cellWidth);
            r.rows = std::max(1LL, (static_cast<long long>(size.height) + p.offsetY + ctx.cellHeight - 1) / ctx.cellHeight);
        }
        return r;
    };
    auto covers = [&](std::size_t index, long long row, long long col) {
        const auto r = cellRect(index);
        return r && row >= r->row && row < r->row + r->rows && col >= r->col && col < r->col + r->cols;
    };
    const Image* numbered = kind == 'n' ? findImage(Command{.number = cmd.number}) : nullptr;

    // Decide first, then erase: the decisions read the placements.
    std::vector<char> doomed(m_placements.size(), 0);
    std::vector<std::uint32_t> touched;
    for (std::size_t i = 0; i < m_placements.size(); ++i) {
        const Placement& p = m_placements[i];
        const auto image = m_images.find(p.imageId);
        if (image == m_images.end() || image->second.isSixel) {
            continue;
        }
        bool match = false;
        switch (kind) {
        case 'a': {
            // Visible on the screen: any of its rows reaches the active area.
            const auto r = cellRect(i);
            match = r && r->row + r->rows > 0;
            break;
        }
        case 'i':
            match = p.imageId == cmd.id && (cmd.placement == 0 || p.placementId == cmd.placement);
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
            match = p.z == cmd.z;
            break;
        case 'r':
            match = p.imageId >= cmd.ux && p.imageId <= cmd.uy;
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
        if (kind == 'i' && findImage(Command{.id = cmd.id})) {
            touched.push_back(cmd.id);
        } else if (kind == 'n' && numbered) {
            touched.push_back(numbered->id);
        } else if (kind == 'r') {
            for (const auto& [id, img] : m_images) {
                if (!img.isSixel && id >= cmd.ux && id <= cmd.uy) {
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
    const std::uint32_t gap = cmd.z < 0 ? 0 : cmd.z > 0 ? std::uint32_t(cmd.z) : 40;
    const bool edits = cmd.r > 0 && std::size_t(cmd.r) <= img->frames.size();

    if (data.empty() && cmd.medium == 'd') {
        // Only frame metadata: set the gap of frame r.
        if (edits && cmd.z != 0) {
            img->gaps[std::size_t(cmd.r - 1)] = gap;
        }
        respond(cmd, ctx, "OK", false);
        return;
    }

    // Limits first, so a refused frame reads and decodes nothing.
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
        releaseMedium(cmd, data);
        respond(cmd, ctx, "EBUSY:too much frame editing, try again later", true);
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

    auto canvas = std::make_shared<ImageFrame>();
    if (edits) {
        *canvas = *img->frames[std::size_t(cmd.r - 1)];
    } else if (cmd.c > 0 && std::size_t(cmd.c) <= img->frames.size()) {
        *canvas = *img->frames[std::size_t(cmd.c - 1)];
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
        // z=0 leaves an edited frame's gap alone, as for a=a and metadata.
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

void GraphicsStore::animate(const Command& cmdIn, Context& ctx)
{
    Image* img = findImage(cmdIn);
    if (!img) {
        respond(cmdIn, ctx, "ENOENT:no such image", true);
        return;
    }
    Command cmd = cmdIn;
    cmd.id = img->id;
    switch (cmd.s) {
    case 1:
        img->animation = Image::Animation::Stopped;
        break;
    case 2:
        img->animation = Image::Animation::Loading;
        img->frameShownAtMs = 0; // the next tick starts the clock
        break;
    case 3:
        img->animation = Image::Animation::Running;
        img->frameShownAtMs = 0;
        break;
    default:
        break;
    }
    if (cmd.v == 1) {
        img->loopsLeft = 0;
    } else if (cmd.v > 1) {
        img->loopsLeft = cmd.v - 1;
    }
    if (cmd.c > 0 && std::size_t(cmd.c) <= img->frames.size()) {
        img->currentFrame = std::size_t(cmd.c - 1);
        img->frameShownAtMs = 0;
    }
    if (cmd.r > 0 && std::size_t(cmd.r) <= img->frames.size() && cmd.z != 0) {
        img->gaps[std::size_t(cmd.r - 1)] = cmd.z < 0 ? 0 : std::uint32_t(cmd.z);
    }
    respond(cmd, ctx, "OK", false);
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
    const std::size_t n = img->frames.size();
    if (cmd.r < 1 || cmd.c < 1 || std::size_t(cmd.r) > n || std::size_t(cmd.c) > n) {
        respond(cmd, ctx, "ENOENT:no such frame", true);
        return;
    }
    const FramePtr& target = img->frames[std::size_t(cmd.c - 1)];
    if (!chargeWork(target->rgba.size())) {
        respond(cmd, ctx, "EBUSY:too much frame editing, try again later", true);
        return;
    }
    auto dst = std::make_shared<ImageFrame>(*target);
    blit(*dst, *img->frames[std::size_t(cmd.r - 1)], cmd.x, cmd.y, cmd.w, cmd.h, cmd.X, cmd.Yi, cmd.C == 1);
    img->frames[std::size_t(cmd.c - 1)] = std::move(dst);
    respond(cmd, ctx, "OK", false);
}

bool GraphicsStore::advanceAnimations(std::uint64_t nowMs, std::uint64_t& nextDeadlineMs)
{
    bool changed = false;
    nextDeadlineMs = 0;
    // An image with no placement shows nothing: its animation does not run
    // and asks for no frames until one appears.
    std::unordered_set<std::uint32_t> placed;
    for (const Placement& p : m_placements) {
        placed.insert(p.imageId);
    }
    auto due = [&](std::uint64_t at) { nextDeadlineMs = nextDeadlineMs == 0 ? at : std::min(nextDeadlineMs, at); };
    for (auto& [id, img] : m_images) {
        if (img.animation == Image::Animation::Stopped || img.frames.size() < 2 || !placed.contains(id)) {
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
                if (img.loopsLeft == 1) {
                    img.animation = Image::Animation::Stopped;
                    settled = true;
                    break;
                }
                if (img.loopsLeft > 1) {
                    --img.loopsLeft;
                }
                img.currentFrame = 0;
            } else {
                ++img.currentFrame;
            }
            // Catch up without drifting, but never fall far behind real time.
            img.frameShownAtMs = (nowMs - img.frameShownAtMs > 1000) ? nowMs : img.frameShownAtMs + gap;
            changed = true;
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

// --- queries and maintenance -----------------------------------------------

std::vector<VisiblePlacement> GraphicsStore::visible(const Screen& screen, int cw, int ch, int extraRows) const
{
    std::vector<VisiblePlacement> out;
    if (cw <= 0 || ch <= 0) {
        return out;
    }
    Origins origins(*this, screen, Origins::Rows::Viewport, extraRows);
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
        const long long px = col * cw + p.offsetX;
        const long long py = (abs - top) * ch + p.offsetY;
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
        out.push_back(std::move(v));
    }
    std::stable_sort(out.begin(), out.end(), [](const VisiblePlacement& a, const VisiblePlacement& b) { return a.z < b.z; });
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
    if (img == m_images.end() || img->second.frames.empty()) {
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
    // The placement's grid in cells. A side left out follows from the other
    // and the image's shape, as for a direct placement; with neither, the
    // image's own size.
    constexpr double limit = double(1 << 28);
    int cols = pl->cols;
    int rows = pl->rows;
    if (cols <= 0 && rows <= 0) {
        cols = int((image.width + std::uint32_t(cw) - 1) / std::uint32_t(cw));
        rows = int((image.height + std::uint32_t(ch) - 1) / std::uint32_t(ch));
    } else if (rows <= 0) {
        rows = int(std::clamp(std::ceil(double(cols) * cw * imgH / imgW / ch), 1.0, limit));
    } else if (cols <= 0) {
        cols = int(std::clamp(std::ceil(double(rows) * ch * imgW / imgH / cw), 1.0, limit));
    }
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
    v.z = pl->z;
    return v;
}

long long GraphicsStore::rowsCovered(const Placement& p, int cellWidth, int cellHeight) const
{
    const auto it = m_images.find(p.imageId);
    if (it == m_images.end() || cellWidth <= 0 || cellHeight <= 0) {
        return 1;
    }
    const DisplaySize size = displaySize(p, it->second, cellWidth, cellHeight);
    return std::max(1LL, (static_cast<long long>(size.height) + p.offsetY + cellHeight - 1) / cellHeight);
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
            if (image == m_images.end() || image->second.isSixel) return false;
            return p.parentPlacement ? present.contains(placementKey(p.parentImage, p.parentPlacement))
                                     : imagesShown.contains(p.parentImage);
        };
        const auto before = m_placements.size();
        std::erase_if(m_placements, [&](const Placement& p) { return p.parentImage && !hasParent(p); });
        if (m_placements.size() == before) break;
    }
}

void GraphicsStore::clearPlacements(const Screen& screen, bool includeHistory, int cellWidth, int cellHeight)
{
    if (includeHistory) {
        m_placements.clear();
    } else {
        const long long history = static_cast<long long>(screen.historySize());
        const long long total = static_cast<long long>(screen.totalRows());
        std::erase_if(m_placements, [&](const Placement& p) {
            if (p.parentImage || p.isVirtual) return false;
            const long long top = anchorIndex(p, screen);
            const long long bottom = top + rowsCovered(p, cellWidth, cellHeight);
            return bottom > history || bottom <= 0 || top >= total;
        });
    }
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

void GraphicsStore::scrollRegion(const RegionScroll& scroll)
{
    std::vector<char> gone(m_placements.size(), 0);
    bool changed = false;
    for (std::size_t i = 0; i < m_placements.size(); ++i) {
        Placement& p = m_placements[i];
        if (p.parentImage || p.isVirtual) continue;
        const SelectionPoint anchor{p.row, p.col};
        if (!scroll.contains(anchor)) continue;
        changed = true;
        if (const auto moved = scroll.map(anchor)) p.row = moved->row;
        else gone[i] = 1;
    }
    if (!changed) return;
    eraseFlagged(m_placements, gone);
    settle();
}

void GraphicsStore::clear()
{
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
    if (total <= m_quota && m_images.size() <= MaxImages) {
        return;
    }
    // One pass: rank every image once, unused before used and older before
    // newer, and take victims from the front until the limits hold.
    std::unordered_set<std::uint32_t> used;
    for (const Placement& p : m_placements) {
        used.insert(p.imageId);
    }
    struct Candidate
    {
        bool used;
        std::uint64_t sequence;
        std::uint32_t id;
        std::size_t bytes;
    };
    std::vector<Candidate> order;
    order.reserve(m_images.size());
    for (const auto& [id, img] : m_images) {
        if (id != keep) order.push_back({used.contains(id), img.sequence, id, img.byteSize()});
    }
    std::sort(order.begin(), order.end(), [](const Candidate& a, const Candidate& b) {
        return a.used != b.used ? !a.used : a.sequence < b.sequence;
    });
    std::unordered_set<std::uint32_t> evicted;
    std::size_t count = m_images.size();
    for (const Candidate& c : order) {
        if (total <= m_quota && count <= MaxImages) break;
        total -= c.bytes;
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
        if (!used.contains(it->first) && (victim == m_images.end() || it->second.sequence < victim->second.sequence)) {
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
    m_virtual.clear();
    for (std::size_t i = 0; i < m_placements.size(); ++i) {
        const Placement& p = m_placements[i];
        if (p.isVirtual) {
            m_virtual.try_emplace(placementKey(p.imageId, p.placementId), i);
            m_virtual.try_emplace(placementKey(p.imageId, 0), i);
        }
    }
}

bool GraphicsStore::chargeWork(std::size_t bytes)
{
    const std::int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                 std::chrono::steady_clock::now().time_since_epoch())
                                 .count();
    if (m_workBytes < 0) {
        m_workBytes = WorkBurst;
    } else {
        m_workBytes = std::min(WorkBurst, m_workBytes + double(now - m_workStampNs) * 1e-9 * WorkBytesPerSecond);
    }
    m_workStampNs = now;
    if (double(bytes) > m_workBytes) {
        return false;
    }
    m_workBytes -= double(bytes);
    return true;
}

} // namespace AfterglowVt
