// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Graphics.h>

#include "Base64.h"
#include <AfterglowVt/Screen.h>
#include <AfterglowVt/Terminal.h>

#include <zlib.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <cmath>
#include <fstream>

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
        out.append(buf, sizeof buf - zs.avail_out);
        if (out.size() > (std::size_t(MaxDimension) * MaxDimension * 4)) {
            rc = Z_DATA_ERROR;
        }
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
    w = w > 0 ? w : int(src.width) - sx;
    h = h > 0 ? h : int(src.height) - sy;
    for (int row = 0; row < h; ++row) {
        const int syy = sy + row;
        const int dyy = dy + row;
        if (syy < 0 || syy >= int(src.height) || dyy < 0 || dyy >= int(dst.height)) {
            continue;
        }
        for (int col = 0; col < w; ++col) {
            const int sxx = sx + col;
            const int dxx = dx + col;
            if (sxx < 0 || sxx >= int(src.width) || dxx < 0 || dxx >= int(dst.width)) {
                continue;
            }
            const std::uint8_t* sp = &src.rgba[(std::size_t(syy) * src.width + std::size_t(sxx)) * 4];
            std::uint8_t* dp = &dst.rgba[(std::size_t(dyy) * dst.width + std::size_t(dxx)) * 4];
            if (overwrite) {
                std::copy_n(sp, 4, dp);
            } else {
                blendOver(dp, sp);
            }
        }
    }
}

struct DisplaySize
{
    int width;
    int height;
};

DisplaySize displaySize(const Placement& pl, const Image& img, int cw, int ch)
{
    const int srcW = pl.srcW > 0 ? pl.srcW : std::max(0, int(img.width) - pl.srcX);
    const int srcH = pl.srcH > 0 ? pl.srcH : std::max(0, int(img.height) - pl.srcY);
    if (pl.cols > 0 && pl.rows > 0) {
        return {pl.cols * cw, pl.rows * ch};
    }
    if (pl.cols > 0) {
        const int w = pl.cols * cw;
        return {w, srcW > 0 ? srcH * w / srcW : 0};
    }
    if (pl.rows > 0) {
        const int h = pl.rows * ch;
        return {srcH > 0 ? srcW * h / srcH : 0, h};
    }
    return {srcW, srcH};
}

bool pathAllowed(const std::string& path)
{
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec).string();
    if (ec) {
        return false;
    }
    for (const char* deny : {"/proc/", "/sys/", "/dev/"}) {
        if (canonical.starts_with(deny)) {
            return false;
        }
    }
    struct stat st{};
    return ::stat(canonical.c_str(), &st) == 0 && S_ISREG(st.st_mode);
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
    int w = 0;
    int h = 0;
    int X = 0;
    std::int64_t Y = 0;
    int c = 0;
    int r = 0;
    int C = 0;
    std::int32_t z = 0;
    int U = 0;
    std::uint32_t P = 0, Q = 0;
    int H = 0, V = 0;

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
            case 'i': cmd.id = u; break;
            case 'I': cmd.number = u; break;
            case 'p': cmd.placement = u; break;
            case 'm': cmd.more = i; break;
            case 'q': cmd.quiet = i; break;
            case 'x': cmd.x = i; break;
            case 'y': cmd.y = i; break;
            case 'w': cmd.w = i; break;
            case 'h': cmd.h = i; break;
            case 'X': cmd.X = i; break;
            case 'Y': cmd.Y = n; break;
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

// --- dispatch ---------------------------------------------------------------

void GraphicsStore::handle(std::string_view payload, Context& ctx)
{
    const auto semi = payload.find(';');
    const std::string_view keys = payload.substr(0, semi);
    const std::string_view data = semi == std::string_view::npos ? std::string_view{} : payload.substr(semi + 1);
    const Command cmd = Command::parse(keys);

    if (m_pending.active) {
        // Continuation chunks carry only m (and q); the first chunk's keys apply.
        detail::base64DecodeAppend(m_pending.data, data, detail::Base64Check::Lenient);
        if (cmd.more == 1) {
            return;
        }
        Command first = Command::parse(m_pending.controlData);
        std::string all = std::move(m_pending.data);
        m_pending = Pending{};
        execute(first, std::move(all), ctx);
        return;
    }

    std::string decoded = detail::base64Decode(data);
    if (cmd.more == 1 && (cmd.action == 't' || cmd.action == 'T' || cmd.action == 'q' || cmd.action == 'f')) {
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
    int n = std::snprintf(buf, sizeof buf, "i=%u", cmd.id);
    out.append(buf, std::size_t(n));
    if (cmd.number) {
        n = std::snprintf(buf, sizeof buf, ",I=%u", cmd.number);
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
    switch (cmd.medium) {
    case 'd':
        break;
    case 'f':
    case 't': {
        const std::string path = data;
        if (!pathAllowed(path)) {
            error = "EBADF:not a readable regular file";
            return false;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error = "EBADF:cannot open file";
            return false;
        }
        in.seekg(std::streamoff(cmd.offset));
        if (cmd.size > 0) {
            data.resize(cmd.size);
            in.read(data.data(), std::streamsize(cmd.size));
            data.resize(std::size_t(in.gcount()));
        } else {
            data.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        // Temporary files are deleted after reading, but only ones that are
        // clearly meant for this protocol.
        if (cmd.medium == 't' && path.find("tty-graphics-protocol") != std::string::npos) {
            const std::string tmp = std::filesystem::temp_directory_path().string();
            if (path.starts_with(tmp) || path.starts_with("/tmp/") || path.starts_with("/dev/shm/")) {
                std::error_code ec;
                std::filesystem::remove(path, ec);
            }
        }
        break;
    }
    case 's': {
        const std::string name = data;
        const int fd = shm_open(name.c_str(), O_RDONLY, 0);
        if (fd < 0) {
            error = "EBADF:cannot open shared memory";
            return false;
        }
        struct stat st{};
        fstat(fd, &st);
        const std::size_t total = std::size_t(st.st_size);
        const std::size_t want = cmd.size ? std::min<std::size_t>(cmd.size, total - std::min<std::size_t>(cmd.offset, total)) : total;
        void* map = total ? mmap(nullptr, total, PROT_READ, MAP_SHARED, fd, 0) : MAP_FAILED;
        close(fd);
        shm_unlink(name.c_str());
        if (map == MAP_FAILED) {
            error = "EBADF:cannot map shared memory";
            return false;
        }
        data.assign(static_cast<const char*>(map) + std::min<std::size_t>(cmd.offset, total), want);
        munmap(map, total);
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
    if (cmd.action == 'q') {
        respond(cmd, ctx, "OK", false);
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
    std::erase_if(m_placements, [id](const Placement& p) { return p.imageId == id; });
    pruneOrphans();

    Image img;
    img.id = id;
    img.number = cmd.number;
    img.width = frame.width;
    img.height = frame.height;
    img.frames.push_back(std::make_shared<const ImageFrame>(std::move(frame)));
    img.sequence = ++m_sequence;
    m_images[id] = std::move(img);
    enforceQuota();

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
        return it == m_images.end() ? nullptr : &it->second;
    }
    if (cmd.number) {
        Image* newest = nullptr;
        for (auto& [id, img] : m_images) {
            if (img.number == cmd.number && (!newest || img.sequence > newest->sequence)) {
                newest = &img;
            }
        }
        return newest;
    }
    return nullptr;
}

// --- placement --------------------------------------------------------------

void GraphicsStore::place(const Command& cmd, Context& ctx)
{
    Image* img = findImage(cmd);
    if (!img) {
        respond(cmd, ctx, "ENOENT:no such image", true);
        return;
    }
    Screen& scr = ctx.screen;
    Placement pl;
    pl.imageId = img->id;
    pl.placementId = cmd.placement ? cmd.placement : m_nextPlacementId++;
    pl.parentImage = cmd.P;
    pl.parentPlacement = cmd.Q;
    pl.relativeX = cmd.H;
    pl.relativeY = cmd.V;
    if (cmd.P || cmd.Q) {
        if (cmd.U) { respond(cmd, ctx, "EINVAL:virtual placement cannot be relative", true); return; }
        const Placement* parent = parentOf(pl);
        if (!parent) { respond(cmd, ctx, "ENOPARENT:no such parent placement", true); return; }
        pl.parentPlacement = parent->placementId; // Resolve an omitted Q once.
        int depth = 0;
        while (parent) {
            if (parent->imageId == pl.imageId && parent->placementId == pl.placementId) {
                respond(cmd, ctx, "ECYCLE:relative placement cycle", true); return;
            }
            if (++depth > 32) { respond(cmd, ctx, "ETOODEEP:relative placement depth exceeds 32", true); return; }
            parent = parentOf(*parent);
        }
        // Reparenting also lengthens existing descendant chains.
        for (const auto& candidate : m_placements) {
            const Placement* ancestor = &candidate;
            for (int below = 0; ancestor && below <= 32; ++below) {
                if (ancestor->imageId == pl.imageId && ancestor->placementId == pl.placementId) {
                    if (depth + below > 32) { respond(cmd, ctx, "ETOODEEP:descendant depth exceeds 32", true); return; }
                    break;
                }
                ancestor = parentOf(*ancestor);
            }
        }
    }
    std::erase_if(m_placements, [&](const Placement& p) {
        return p.imageId == pl.imageId && p.placementId == pl.placementId;
    });
    pl.row = scr.rowsDropped() + scr.historySize() + std::size_t(scr.cursor.y);
    pl.col = scr.cursor.x;
    pl.offsetX = std::max(0, cmd.X);
    pl.offsetY = std::max(0, int(cmd.Y));
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

void GraphicsStore::addSixel(ImageFrame frame, const Screen& screen, int row, int col, int cellWidth, int cellHeight)
{
    if (cellWidth <= 0 || cellHeight <= 0) {
        return;
    }
    const int cellsWide = int((frame.width + std::uint32_t(cellWidth) - 1) / std::uint32_t(cellWidth));
    const int cellsHigh = int((frame.height + std::uint32_t(cellHeight) - 1) / std::uint32_t(cellHeight));
    const std::uint64_t topRow = screen.rowsDropped() + screen.historySize() + std::uint64_t(std::max(0, row));

    // A new image over an older one replaces it where it covers it whole.
    std::vector<std::uint32_t> covered;
    std::erase_if(m_placements, [&](const Placement& p) {
        if (!isSixelId(p.imageId)) {
            return false;
        }
        const auto it = m_images.find(p.imageId);
        if (it == m_images.end()) {
            return false;
        }
        const int w = int((it->second.width + std::uint32_t(cellWidth) - 1) / std::uint32_t(cellWidth));
        const int h = int((it->second.height + std::uint32_t(cellHeight) - 1) / std::uint32_t(cellHeight));
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

    Image img;
    img.id = m_nextSixelId++;
    if (m_nextSixelId >= (1u << 31)) {
        m_nextSixelId = 3u << 29;
    }
    img.width = frame.width;
    img.height = frame.height;
    img.frames.push_back(std::make_shared<const ImageFrame>(std::move(frame)));
    img.sequence = ++m_sequence;
    const std::uint32_t id = img.id;
    m_images[id] = std::move(img);

    Placement pl;
    pl.imageId = id;
    pl.placementId = 1;
    pl.row = topRow;
    pl.col = col;
    pl.z = -1; // under text, over cell backgrounds: text written on it stays readable
    m_placements.push_back(pl);
    enforceQuota();
}

void GraphicsStore::remove(const Command& cmd, Context& ctx)
{
    const Screen& scr = ctx.screen;
    const char d = cmd.del ? cmd.del : 'a';
    const bool freeImages = d >= 'A' && d <= 'Z';
    const char kind = char(d | 0x20);

    auto cellRect = [&](const Placement& p, int& row0, int& col0, int& rows, int& cols) {
        const auto position = origin(p, scr);
        const long long abs = position ? position->second : -1;
        row0 = abs < 0 ? -1 : int(std::clamp(abs - (long long)scr.historySize(), -1000000000LL, 1000000000LL));
        col0 = position ? int(std::clamp(position->first, -1000000000LL, 1000000000LL)) : -1000000000;
        const auto it = m_images.find(p.imageId);
        if (it == m_images.end() || ctx.cellWidth <= 0 || ctx.cellHeight <= 0) {
            rows = cols = 1;
            return;
        }
        const DisplaySize size = displaySize(p, it->second, ctx.cellWidth, ctx.cellHeight);
        cols = std::max(1, (size.width + p.offsetX + ctx.cellWidth - 1) / ctx.cellWidth);
        rows = std::max(1, (size.height + p.offsetY + ctx.cellHeight - 1) / ctx.cellHeight);
    };
    auto covers = [&](const Placement& p, int row, int col) {
        int r0, c0, rs, cs;
        cellRect(p, r0, c0, rs, cs);
        return row >= r0 && row < r0 + rs && col >= c0 && col < c0 + cs;
    };

    std::vector<std::uint32_t> touched;
    auto drop = [&](auto pred) {
        std::erase_if(m_placements, [&](const Placement& p) {
            if (!pred(p)) {
                return false;
            }
            touched.push_back(p.imageId);
            return true;
        });
    };

    switch (kind) {
    case 'a':
        drop([&](const Placement& p) {
            const auto at = origin(p, scr);
            return at && at->second >= (long long)scr.historySize();
        });
        break;
    case 'i':
        drop([&](const Placement& p) {
            return p.imageId == cmd.id && (cmd.placement == 0 || p.placementId == cmd.placement);
        });
        if (freeImages) {
            touched.push_back(cmd.id);
        }
        break;
    case 'n': {
        const Image* img = findImage(Command{.number = cmd.number});
        if (img) {
            const std::uint32_t id = img->id;
            drop([&](const Placement& p) {
                return p.imageId == id && (cmd.placement == 0 || p.placementId == cmd.placement);
            });
            if (freeImages) {
                touched.push_back(id);
            }
        }
        break;
    }
    case 'c':
        drop([&](const Placement& p) { return covers(p, scr.cursor.y, scr.cursor.x); });
        break;
    case 'p':
        drop([&](const Placement& p) { return covers(p, cmd.y - 1, cmd.x - 1); });
        break;
    case 'q':
        drop([&](const Placement& p) { return covers(p, cmd.y - 1, cmd.x - 1) && p.z == cmd.z; });
        break;
    case 'x':
        drop([&](const Placement& p) {
            int r0, c0, rs, cs;
            cellRect(p, r0, c0, rs, cs);
            return cmd.x - 1 >= c0 && cmd.x - 1 < c0 + cs;
        });
        break;
    case 'y':
        drop([&](const Placement& p) {
            int r0, c0, rs, cs;
            cellRect(p, r0, c0, rs, cs);
            return cmd.y - 1 >= r0 && cmd.y - 1 < r0 + rs;
        });
        break;
    case 'z':
        drop([&](const Placement& p) { return p.z == cmd.z; });
        break;
    case 'r':
        drop([&](const Placement& p) { return p.imageId >= std::uint32_t(cmd.x) && p.imageId <= std::uint32_t(cmd.y); });
        if (freeImages) {
            for (const auto& [id, img] : m_images) {
                if (id >= std::uint32_t(cmd.x) && id <= std::uint32_t(cmd.y)) {
                    touched.push_back(id);
                }
            }
        }
        break;
    default:
        break;
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

void GraphicsStore::frame(const Command& cmd, std::string data, Context& ctx)
{
    Image* img = findImage(cmd);
    if (!img) {
        respond(cmd, ctx, "ENOENT:no such image", true);
        return;
    }
    const std::uint32_t gap = cmd.z < 0 ? 0 : cmd.z > 0 ? std::uint32_t(cmd.z) : 40;

    if (data.empty() && cmd.medium == 'd') {
        // Only frame metadata: set the gap of frame r.
        if (cmd.r > 0 && std::size_t(cmd.r) <= img->frames.size() && cmd.z != 0) {
            auto edited = std::make_shared<ImageFrame>(*img->frames[std::size_t(cmd.r - 1)]);
            edited->gapMs = gap;
            img->frames[std::size_t(cmd.r - 1)] = std::move(edited);
        }
        respond(cmd, ctx, "OK", false);
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
    if (cmd.r > 0 && std::size_t(cmd.r) <= img->frames.size()) {
        *canvas = *img->frames[std::size_t(cmd.r - 1)];
    } else if (cmd.c > 0 && std::size_t(cmd.c) <= img->frames.size()) {
        *canvas = *img->frames[std::size_t(cmd.c - 1)];
    } else {
        canvas->width = img->width;
        canvas->height = img->height;
        const auto bg = std::uint32_t(cmd.Y);
        const std::uint8_t px[4] = {std::uint8_t(bg >> 24), std::uint8_t(bg >> 16), std::uint8_t(bg >> 8),
                                    std::uint8_t(bg)};
        canvas->rgba.resize(std::size_t(img->width) * img->height * 4);
        for (std::size_t i = 0; i < canvas->rgba.size(); i += 4) {
            std::copy_n(px, 4, &canvas->rgba[i]);
        }
    }
    canvas->gapMs = gap;
    blit(*canvas, patch, 0, 0, int(patch.width), int(patch.height), cmd.x, cmd.y, cmd.X == 1);

    if (cmd.r > 0 && std::size_t(cmd.r) <= img->frames.size()) {
        img->frames[std::size_t(cmd.r - 1)] = std::move(canvas);
    } else {
        img->frames.push_back(std::move(canvas));
    }
    enforceQuota();
    respond(cmd, ctx, "OK", false);
}

void GraphicsStore::animate(const Command& cmd, Context& ctx)
{
    Image* img = findImage(cmd);
    if (!img) {
        respond(cmd, ctx, "ENOENT:no such image", true);
        return;
    }
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
        auto edited = std::make_shared<ImageFrame>(*img->frames[std::size_t(cmd.r - 1)]);
        edited->gapMs = cmd.z < 0 ? 0 : std::uint32_t(cmd.z);
        img->frames[std::size_t(cmd.r - 1)] = std::move(edited);
    }
    respond(cmd, ctx, "OK", false);
}

void GraphicsStore::compose(const Command& cmd, Context& ctx)
{
    Image* img = findImage(cmd);
    if (!img) {
        respond(cmd, ctx, "ENOENT:no such image", true);
        return;
    }
    const std::size_t n = img->frames.size();
    if (cmd.r < 1 || cmd.c < 1 || std::size_t(cmd.r) > n || std::size_t(cmd.c) > n) {
        respond(cmd, ctx, "ENOENT:no such frame", true);
        return;
    }
    auto dst = std::make_shared<ImageFrame>(*img->frames[std::size_t(cmd.c - 1)]);
    blit(*dst, *img->frames[std::size_t(cmd.r - 1)], cmd.x, cmd.y, cmd.w, cmd.h, cmd.X, int(cmd.Y), cmd.C == 1);
    img->frames[std::size_t(cmd.c - 1)] = std::move(dst);
    respond(cmd, ctx, "OK", false);
}

bool GraphicsStore::advanceAnimations(std::uint64_t nowMs, std::uint64_t& nextDeadlineMs)
{
    bool changed = false;
    nextDeadlineMs = 0;
    for (auto& [id, img] : m_images) {
        if (img.animation == Image::Animation::Stopped || img.frames.size() < 2) {
            continue;
        }
        if (img.frameShownAtMs == 0) {
            img.frameShownAtMs = nowMs;
        }
        // Bounded so frames whose gaps are all zero cannot spin forever.
        for (std::size_t guard = 0; guard <= img.frames.size(); ++guard) {
            const std::uint32_t gap = img.frames[img.currentFrame]->gapMs;
            if (gap != 0 && nowMs < img.frameShownAtMs + gap) {
                const std::uint64_t due = img.frameShownAtMs + gap;
                nextDeadlineMs = nextDeadlineMs == 0 ? due : std::min(nextDeadlineMs, due);
                break;
            }
            const bool last = img.currentFrame + 1 >= img.frames.size();
            if (last) {
                if (img.animation == Image::Animation::Loading) {
                    break; // wait on the last frame until more arrive
                }
                if (img.loopsLeft == 1) {
                    img.animation = Image::Animation::Stopped;
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
    }
    return changed;
}

// --- queries and maintenance -----------------------------------------------

const Placement* GraphicsStore::parentOf(const Placement& p) const
{
    if (!p.parentImage) return nullptr;
    for (const auto& parent : m_placements)
        if (parent.imageId == p.parentImage && (!p.parentPlacement || parent.placementId == p.parentPlacement)) return &parent;
    return nullptr;
}

void GraphicsStore::pruneOrphans()
{
    // Erase after collecting keys: erase_if moves elements while predicates run.
    for (;;) {
        std::vector<std::pair<std::uint32_t, std::uint32_t>> gone;
        for (const auto& p : m_placements)
            if (p.parentImage && !parentOf(p)) gone.emplace_back(p.imageId, p.placementId);
        if (gone.empty()) break;
        std::erase_if(m_placements, [&](const Placement& p) {
            return std::find(gone.begin(), gone.end(), std::pair(p.imageId, p.placementId)) != gone.end();
        });
        for (const auto& [id, placement] : gone) dropUnusedImage(id);
    }
}

std::optional<std::pair<long long, long long>> GraphicsStore::origin(const Placement& p, const Screen& s, int depth) const
{
    if (depth > 32) return std::nullopt;
    if (p.parentImage) {
        const auto* parent = parentOf(p);
        auto at = parent ? origin(*parent, s, depth + 1) : std::nullopt;
        if (at) { at->first += p.relativeX; at->second += p.relativeY; }
        return at;
    }
    if (!p.isVirtual) {
        const auto row = s.absoluteIndexOf(p.row);
        if (row < 0) return std::nullopt;
        return std::pair<long long, long long>(p.col, row);
    }
    // Virtual placements have no fixed anchor: derive the origin of their
    // displayed placeholder images, including the implied diacritics.
    std::optional<std::pair<long long, long long>> at;
    auto colorId = [](Color c) -> std::uint32_t {
        return c.kind() == Color::Kind::Rgb ? (std::uint32_t(c.r()) << 16) | (std::uint32_t(c.g()) << 8) | c.b()
            : c.kind() == Color::Kind::Palette ? c.index() : 0;
    };
    for (auto y = s.viewportTop(); y < s.viewportTop() + std::size_t(s.rows()); ++y) {
        const auto& row = s.absoluteRow(y);
        bool previous = false;
        std::uint32_t prevImage = 0, prevPlacement = 0;
        int prevRow = 0, prevCol = 0;
        for (int x = 0; x < s.cols(); ++x) {
            const auto& cell = row.at(x);
            if (cell.codepoint() != PlaceholderCodepoint) { previous = false; continue; }
            const auto& style = s.styles().get(cell.styleId());
            auto image = colorId(style.fg);
            const auto placement = colorId(style.underlineColor);
            const auto extra = s.graphemeExtra(row, x);
            const bool continues = previous && (image & 0xffffff) == (prevImage & 0xffffff) && placement == prevPlacement;
            const int r = extra.size() > 0 ? placeholderDiacriticValue(extra[0]) : -1;
            const int c = extra.size() > 1 ? placeholderDiacriticValue(extra[1]) : -1;
            const int high = extra.size() > 2 ? placeholderDiacriticValue(extra[2]) : -1;
            if (high >= 0) image |= std::uint32_t(high) << 24;
            else if (continues) image = prevImage;
            const int gridRow = r >= 0 ? r : continues ? prevRow : 0;
            const int gridCol = c >= 0 ? c : continues ? prevCol + 1 : 0;
            if (image == p.imageId && (!placement || placement == p.placementId)) {
                const long long px = x - gridCol, py = static_cast<long long>(y) - gridRow;
                if (!at) at = std::pair(px, py);
                else { at->first = std::min(at->first, px); at->second = std::min(at->second, py); }
            }
            previous = true; prevImage = image; prevPlacement = placement; prevRow = gridRow; prevCol = gridCol;
        }
    }
    return at;
}

std::vector<VisiblePlacement> GraphicsStore::visible(const Screen& screen, int cw, int ch) const
{
    std::vector<VisiblePlacement> out;
    if (cw <= 0 || ch <= 0) {
        return out;
    }
    const long long top = (long long)screen.viewportTop();
    const int viewH = screen.rows() * ch;
    for (const Placement& p : m_placements) {
        if (p.isVirtual) {
            continue;
        }
        const auto it = m_images.find(p.imageId);
        if (it == m_images.end() || it->second.frames.empty()) {
            continue;
        }
        const auto position = origin(p, screen);
        if (!position) continue;
        const auto [col, abs] = *position;
        const Image& img = it->second;
        const DisplaySize size = displaySize(p, img, cw, ch);
        VisiblePlacement v;
        const long long px = col * cw + p.offsetX;
        const long long py = (abs - top) * ch + p.offsetY;
        if (px < -size.width || px >= static_cast<long long>(screen.cols()) * cw || py < -size.height || py >= viewH) continue;
        v.x = int(px);
        v.y = int(py);
        v.width = size.width;
        v.height = size.height;
        if (v.y + v.height <= 0 || v.y >= viewH || v.width <= 0 || v.height <= 0) {
            continue;
        }
        v.frame = img.frames[std::min(img.currentFrame, img.frames.size() - 1)];
        v.imageId = img.id;
        v.srcX = p.srcX;
        v.srcY = p.srcY;
        v.srcW = p.srcW > 0 ? p.srcW : int(img.width) - p.srcX;
        v.srcH = p.srcH > 0 ? p.srcH : int(img.height) - p.srcY;
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
    const Placement* pl = nullptr;
    for (const Placement& p : m_placements) {
        if (p.isVirtual && p.imageId == imageId && (placementId == 0 || p.placementId == placementId)) {
            pl = &p;
            break;
        }
    }
    if (!pl) {
        return std::nullopt;
    }
    const Image& image = img->second;
    // The placement's grid, from its size in cells or the image's own size.
    const int cols = pl->cols > 0 ? pl->cols : int((image.width + std::uint32_t(cw) - 1) / std::uint32_t(cw));
    const int rows = pl->rows > 0 ? pl->rows : int((image.height + std::uint32_t(ch) - 1) / std::uint32_t(ch));
    if (row < 0 || col < 0 || row >= rows || col >= cols) {
        return std::nullopt;
    }
    // The image fits the grid with its aspect ratio kept, centred.
    const double gridW = double(cols * cw);
    const double gridH = double(rows * ch);
    const double imgW = std::max(1.0, double(image.width));
    const double imgH = std::max(1.0, double(image.height));
    const double scale = (imgW * gridH > imgH * gridW) ? gridW / imgW : gridH / imgH;
    const double offX = (gridW - imgW * scale) / 2;
    const double offY = (gridH - imgH * scale) / 2;
    // This cell's part of the grid, clipped to the image.
    const double gx0 = std::max(double(col * cw), offX);
    const double gy0 = std::max(double(row * ch), offY);
    const double gx1 = std::min(double((col + 1) * cw), offX + imgW * scale);
    const double gy1 = std::min(double((row + 1) * ch), offY + imgH * scale);
    if (gx1 <= gx0 || gy1 <= gy0) {
        return std::nullopt;
    }
    VisiblePlacement v;
    v.frame = image.frames[std::min(image.currentFrame, image.frames.size() - 1)];
    v.imageId = imageId;
    v.x = cellX + int(std::lround(gx0 - col * cw));
    v.y = cellY + int(std::lround(gy0 - row * ch));
    v.width = int(std::lround(gx1 - gx0));
    v.height = int(std::lround(gy1 - gy0));
    v.srcX = int(std::lround((gx0 - offX) / scale));
    v.srcY = int(std::lround((gy0 - offY) / scale));
    v.srcW = std::max(1, int(std::lround((gx1 - gx0) / scale)));
    v.srcH = std::max(1, int(std::lround((gy1 - gy0) / scale)));
    v.z = pl->z;
    return v;
}

void GraphicsStore::clearPlacements(const Screen& screen, bool includeHistory)
{
    std::erase_if(m_placements, [&](const Placement& p) {
        const long long abs = screen.absoluteIndexOf(p.row);
        return includeHistory || (!p.parentImage && (abs < 0 || abs >= (long long)screen.historySize()));
    });
    pruneOrphans();
}

std::vector<std::size_t> GraphicsStore::anchorRows(const Screen& screen) const
{
    std::vector<std::size_t> out;
    out.reserve(m_placements.size());
    for (const Placement& p : m_placements) {
        const long long abs = screen.absoluteIndexOf(p.row);
        out.push_back(abs < 0 ? Screen::TrackedRowGone : std::size_t(abs));
    }
    return out;
}

void GraphicsStore::retarget(const Screen& screen, const std::vector<std::size_t>& rows)
{
    std::size_t i = 0;
    std::erase_if(m_placements, [&](Placement& p) {
        const std::size_t abs = rows[i++];
        if (p.parentImage || p.isVirtual) return false;
        if (abs == Screen::TrackedRowGone) {
            return true;
        }
        p.row = screen.rowsDropped() + abs;
        return false;
    });
    pruneOrphans();
}

void GraphicsStore::clear()
{
    m_images.clear();
    m_placements.clear();
    m_pending = Pending{};
}

std::size_t GraphicsStore::totalBytes() const
{
    std::size_t total = 0;
    for (const auto& [id, img] : m_images) {
        total += img.byteSize();
    }
    return total;
}

void GraphicsStore::enforceQuota()
{
    std::size_t total = totalBytes();
    while (total > m_quota && !m_images.empty()) {
        // Oldest image without placements first, then the oldest overall.
        auto victim = m_images.end();
        bool victimUnused = false;
        for (auto it = m_images.begin(); it != m_images.end(); ++it) {
            const bool unused = std::none_of(m_placements.begin(), m_placements.end(),
                                             [&](const Placement& p) { return p.imageId == it->first; });
            if (victim == m_images.end() || (unused && !victimUnused)
                || (unused == victimUnused && it->second.sequence < victim->second.sequence)) {
                victim = it;
                victimUnused = unused;
            }
        }
        total -= victim->second.byteSize();
        const std::uint32_t id = victim->first;
        std::erase_if(m_placements, [id](const Placement& p) { return p.imageId == id; });
        m_images.erase(victim);
        pruneOrphans();
        total = totalBytes();
    }
}

} // namespace AfterglowVt
