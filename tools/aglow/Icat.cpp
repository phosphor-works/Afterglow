// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// aglow icat: kitten icat's command line over the graphics command writer of
// afterglow-proto. A PNG that needs no change goes as it is. Other formats,
// and images that must be scaled, mirrored or flattened, go as pixels: RGB
// when they are opaque, else RGBA, and with zlib when they go in the escape
// codes. An animated GIF or WebP goes as animation frames.

#include "Cli.h"
#include "Commands.h"
#include "Image.h"
#include "Tty.h"

#include <AfterglowProto/Color.h>
#include <AfterglowProto/DndUri.h>
#include <AfterglowProto/Fd.h>
#include <AfterglowProto/Graphics.h>
#include <AfterglowProto/Placeholder.h>
#include <AfterglowProto/Random.h>
#include <AfterglowProto/Text.h>
#include <AfterglowProto/Zlib.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <optional>
#include <poll.h>
#include <set>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

extern char** environ;

namespace Aglow {

namespace {

using AfterglowProto::GraphicsCommand;

constexpr long long Unlimited = LLONG_MAX;
// kitten's z-index origin for "--1": below the cell backgrounds.
constexpr std::int64_t BelowBackground = -1073741824;
// The largest file that aglow reads as an image: twice the image memory of
// the terminal, which an uncompressed BMP or a lossless PNG reaches.
constexpr std::uint64_t MaxInput = 2 * std::uint64_t(AfterglowProto::GraphicsDefaultQuota);
// The bytes of graphics commands that go out between two checks for a signal.
constexpr std::size_t WriteSlice = 64 * 1024;

const Cli::Command command {
    "icat",
    "image_file_or_directory ...",
    "Show images in the terminal, with kitty's graphics protocol.",
    "Shows PNG, JPEG, GIF, WebP and BMP images. A directory shows every image in it. Image data on standard input "
    "is shown too. An animated GIF or WebP plays in the terminal.",
    {
        { "--align", Cli::Option::Kind::Choice, "center", { "center", "left", "right" },
          "Horizontal alignment of the image." },
        { "--place", Cli::Option::Kind::String, "", {},
          "Where on the screen to show the image, as WIDTHxHEIGHT@LEFTxTOP in cells, from the top left corner at "
          "0x0. The image is scaled to fit the rectangle and aligned in it with --align. The cursor ends at the top "
          "left corner of the image, not on the line after it." },
        { "--scale-up", Cli::Option::Kind::Flag, "", {},
          "Scale images that are smaller than the area up to fill as much of it as they can. The area comes from "
          "--place or --fit." },
        { "--fit", Cli::Option::Kind::Choice, "width", { "width", "height", "both", "none" },
          "Without --place, how the image is scaled to the screen: to fit its width, its height, both, or not at "
          "all." },
        { "--background", Cli::Option::Kind::String, "none", {},
          "A background colour, as #rgb, #rrggbb, rgb:rr/gg/bb or an X11 colour name. Transparent images are drawn "
          "over it." },
        { "--mirror", Cli::Option::Kind::Choice, "none", { "none", "horizontal", "vertical", "both" },
          "Mirror the image about a horizontal or vertical axis, or both." },
        { "--clear", Cli::Option::Kind::Flag, "", {}, "Remove every image that is on the screen." },
        { "--clear-all", Cli::Option::Kind::Flag, "", {},
          "Remove every image from the screen and the scrollback, and free its data in the terminal." },
        { "--transfer-mode", Cli::Option::Kind::Choice, "detect", { "detect", "file", "stream", "memory" },
          "How images reach the terminal: a temporary file, shared memory, or the data in escape codes. detect "
          "asks the terminal, and uses a file or shared memory only when the terminal can read them, that is "
          "when it runs on this computer and standard output is the terminal. With standard output elsewhere, "
          "aglow removes its files and shared memory when it exits. Inside tmux, images always go in escape codes." },
        { "--detect-support", Cli::Option::Kind::Flag, "", {},
          "Ask the terminal whether it shows images. Exits with 1 when it does not, else prints the transfer mode "
          "(memory, file or stream) on standard error and exits with 0. Inside tmux, which answers in place of "
          "the terminal, it cannot ask and exits with 1." },
        { "--detection-timeout", Cli::Option::Kind::Float, "10", {}, "Seconds to wait for the terminal's answer.",
          0, 3600 },
        { "--use-window-size", Cli::Option::Kind::String, "", {},
          "Use this window size and do not ask the terminal: COLUMNS,ROWS,WIDTH,HEIGHT, the last two in pixels." },
        { "--print-window-size", Cli::Option::Kind::Flag, "", {},
          "Print the window size as WIDTHxHEIGHT in pixels, and exit." },
        { "--stdin", Cli::Option::Kind::Choice, "detect", { "detect", "yes", "no" },
          "Read image data from standard input. detect reads it when standard input is not a terminal, and "
          "shows nothing for it when it is empty." },
        { "--z-index -z", Cli::Option::Kind::String, "0", {},
          "The z-index of the image. Text is drawn over an image with a negative one. With two dashes, as --1, "
          "the image goes under the cell backgrounds too." },
        { "--loop -l", Cli::Option::Kind::Int, "-1", {},
          "How many times an animation plays. A negative number plays it forever. With 0, only the first frame "
          "shows." },
        { "--hold", Cli::Option::Kind::Flag, "", {}, "Wait for a key press before exiting." },
        { "--unicode-placeholder", Cli::Option::Kind::Flag, "", {},
          "Show images through Unicode placeholder characters, which full screen programs, terminal multiplexers "
          "and editors keep as text." },
        { "--passthrough", Cli::Option::Kind::Choice, "detect", { "detect", "tmux", "none" },
          "Wrap the graphics commands so that they pass through tmux. detect does it inside tmux. This implies "
          "--unicode-placeholder." },
        { "--image-id", Cli::Option::Kind::Int, "0", {},
          "The graphics protocol id of the image. More images take the ids that follow it. 0 picks one when it is "
          "needed.",
          0, 4294967295.0 },
        { "--engine", Cli::Option::Kind::String, "auto", {},
          "The engine that decodes images: auto or builtin, which are the same in aglow. aglow has no ImageMagick "
          "engine (magick)." },
        { "--silent", Cli::Option::Kind::Flag, "", {}, "Not used. kitten icat keeps it for compatibility, and so does aglow." },
        { "--no-trailing-newline -n", Cli::Option::Kind::Flag, "", {},
          "Leave the cursor after the image, not on the next line. Has no effect with --place." },
    },
};

struct Place
{
    int width = 0;
    int height = 0;
    int left = 0;
    int top = 0;
};

struct Screen
{
    int columns = 0;
    int rows = 0;
    int width = 0;
    int height = 0;
    int cellWidth() const { return std::max(1, width / std::max(1, columns)); }
    int cellHeight() const { return std::max(1, height / std::max(1, rows)); }
};

enum class Medium { Stream, File, Memory };

struct Settings
{
    std::optional<Place> place;
    std::string fit = "width";
    bool scaleUp = false;
    std::string align = "center";
    bool mirrorH = false;
    bool mirrorV = false;
    std::optional<std::array<std::uint8_t, 3>> background;
    std::int32_t z = 0;
    long long loops = -1;
    bool placeholder = false;
    bool tmux = false;
    bool noNewline = false;
    Medium medium = Medium::Stream;
};

bool parseInt(std::string_view text, int& value)
{
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    return !text.empty() && ec == std::errc() && end == text.data() + text.size();
}

// Numbers in decimal digits only, split at @p separators in their order: no
// sign, no spaces.
bool parseFields(std::string_view text, std::string_view separators, int* fields)
{
    for (std::size_t i = 0; i <= separators.size(); ++i) {
        const auto end = i < separators.size() ? text.find(separators[i]) : text.size();
        if (end == std::string_view::npos) {
            return false;
        }
        const auto field = text.substr(0, end);
        if (field.empty() || !std::all_of(field.begin(), field.end(), [](char c) { return c >= '0' && c <= '9'; })
            || !parseInt(field, fields[i])) {
            return false;
        }
        text.remove_prefix(std::min(text.size(), end + 1));
    }
    return true;
}

std::optional<Place> parsePlace(std::string_view spec)
{
    int fields[4] = {};
    if (!parseFields(spec, "x@x", fields) || fields[0] < 1 || fields[1] < 1) {
        return std::nullopt;
    }
    return Place { fields[0], fields[1], fields[2], fields[3] };
}

std::optional<std::array<std::uint8_t, 3>> parseColour(const std::string& text, bool& valid)
{
    valid = true;
    if (text.empty() || text == "none") {
        return std::nullopt;
    }
    std::array<std::uint8_t, 3> rgb {};
    if (AfterglowProto::parseColorSpec(text, rgb[0], rgb[1], rgb[2])) {
        return rgb;
    }
    valid = false;
    return std::nullopt;
}

bool hasImageExtension(const std::string& name)
{
    const auto dot = name.rfind('.');
    if (dot == std::string::npos) {
        return false;
    }
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    static const char* known[] = { "png", "jpg", "jpeg", "gif", "webp", "bmp" };
    return std::any_of(std::begin(known), std::end(known), [&](const char* k) { return ext == k; });
}

// The image files under @p path, in name order. A directory that a link leads
// back to is read once: @p seen holds the directories read so far. Only the
// directory that the command names must be readable (@p top): a subdirectory
// that cannot be read is left out, with the reason in @p skipped.
bool walk(const std::string& path, std::vector<std::string>& out, std::set<std::pair<dev_t, ino_t>>& seen,
          std::vector<std::string>& skipped, std::string& error, bool top = true)
{
    struct stat self { };
    if (::stat(path.c_str(), &self) != 0 || !seen.emplace(self.st_dev, self.st_ino).second) {
        return true;
    }
    DIR* dir = ::opendir(path.c_str());
    if (!dir) {
        const std::string reason = "Cannot read the directory " + printable(path) + ": " + std::strerror(errno);
        if (!top) {
            skipped.push_back(reason);
            return true;
        }
        error = reason;
        return false;
    }
    std::vector<std::string> names;
    while (const dirent* entry = ::readdir(dir)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") {
            names.push_back(name);
        }
    }
    ::closedir(dir);
    std::sort(names.begin(), names.end());
    for (const auto& name : names) {
        const auto child = path + "/" + name;
        struct stat st { };
        if (::stat(child.c_str(), &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            walk(child, out, seen, skipped, error, false);
        } else if (hasImageExtension(name)) {
            out.push_back(child);
        }
    }
    return true;
}

// Reads a whole image, up to MaxInput bytes. A pipe or a device is read
// after poll says it has data, so that @p stop can end a read that waits.
bool readSource(int fd, std::string& out, const std::function<bool()>& stop, std::string& error)
{
    struct stat st { };
    if (::fstat(fd, &st) != 0) {
        error = std::strerror(errno);
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        error = "it is a directory";
        return false;
    }
    const bool regular = S_ISREG(st.st_mode);
    if (regular && std::uint64_t(st.st_size) > MaxInput) {
        error = "it is larger than " + humanSize(MaxInput) + ", the most that aglow reads as an image";
        return false;
    }
    if (regular) {
        out.reserve(std::size_t(st.st_size));
    }
    char buffer[65536];
    for (;;) {
        if (!regular) {
            pollfd ready { fd, POLLIN, 0 };
            const int n = ::poll(&ready, 1, 100);
            if (n < 0 && errno != EINTR) {
                error = std::strerror(errno);
                return false;
            }
            if (stop && stop()) {
                error = "interrupted";
                return false;
            }
            if (n <= 0) {
                continue;
            }
        }
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            error = std::strerror(errno);
            return false;
        }
        if (n == 0) {
            return true;
        }
        if (out.size() + std::size_t(n) > MaxInput) {
            error = "it is larger than " + humanSize(MaxInput) + ", the most that aglow reads as an image";
            return false;
        }
        out.append(buffer, std::size_t(n));
    }
}

// Graphics commands as they leave aglow: wrapped for tmux when they must pass through it.
std::string wrap(std::string_view commands, bool tmux)
{
    if (!tmux) {
        return std::string(commands);
    }
    std::string out;
    std::size_t start = 0;
    while (start < commands.size()) {
        auto end = commands.find("\x1b\\", start + 1);
        end = end == std::string_view::npos ? commands.size() : end + 2;
        out += "\x1bPtmux;";
        for (std::size_t i = start; i < end; ++i) {
            out += commands[i];
            if (commands[i] == '\x1b') {
                out += '\x1b';
            }
        }
        out += "\x1b\\";
        start = end;
    }
    return out;
}

std::uint32_t randomId(bool placeholder)
{
    for (;;) {
        std::uint32_t id = 0;
        if (!AfterglowProto::randomBytes(&id, sizeof id)) {
            id = std::uint32_t(nowMs() * 2654435761u);
        }
        // A placeholder id needs its high byte set, and one of its two middle
        // bytes, so that programs that keep only 256 colours or no third
        // diacritic cannot mix it up with another (as kitten picks them).
        if (id != 0 && (!placeholder || ((id & 0xFF000000u) && (id & 0x00FFFF00u)))) {
            return id;
        }
    }
}

// Writes @p bytes to a new temporary file whose name tells the terminal to
// delete it after reading. Empty with @p error set on failure.
std::string temporaryFile(std::string_view bytes, std::string& error)
{
    for (const char* dir : { "/dev/shm", "/tmp" }) {
        std::string name = std::string(dir) + "/" + std::string(AfterglowProto::GraphicsTemporaryTag) + "-aglow-XXXXXX";
        const int fd = ::mkostemp(name.data(), O_CLOEXEC);
        if (fd < 0) {
            error = std::strerror(errno);
            continue;
        }
        const bool ok = AfterglowProto::writeAll(fd, bytes);
        error = std::strerror(errno);
        ::close(fd);
        if (ok) {
            return name;
        }
        ::unlink(name.c_str());
    }
    return {};
}

// A shared memory object with @p bytes, which the terminal unlinks after
// reading it. Empty with @p error set on failure.
std::string sharedMemory(std::string_view bytes, std::string& error)
{
    const auto suffix = AfterglowProto::randomHex(8);
    if (!suffix) {
        error = "no random bytes for its name";
        return {};
    }
    const std::string name = "/" + std::string(AfterglowProto::GraphicsTemporaryTag) + "-aglow-" + *suffix;
    const int fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        error = std::strerror(errno);
        return {};
    }
    const bool ok = ::ftruncate(fd, off_t(bytes.size())) == 0 && AfterglowProto::writeAll(fd, bytes);
    error = std::strerror(errno);
    ::close(fd);
    if (!ok) {
        ::shm_unlink(name.c_str());
        return {};
    }
    return name;
}

void removeTemporary(Medium medium, const std::string& name)
{
    if (medium == Medium::Memory) {
        ::shm_unlink(name.c_str());
    } else {
        ::unlink(name.c_str());
    }
}

// The keys of the last command in @p commands: whether it is a chunk with
// more to come (m=1), and whether it belongs to a frame (a=f).
void lastChunk(std::string_view commands, bool& open, bool& frame)
{
    open = frame = false;
    const auto start = commands.rfind("\x1b_G");
    if (start == std::string_view::npos) {
        return;
    }
    auto keys = commands.substr(start + 3);
    keys = keys.substr(0, std::min(keys.find(';'), keys.find('\x1b')));
    while (!keys.empty()) {
        const auto comma = keys.find(',');
        const auto key = keys.substr(0, comma);
        open |= key == "m=1";
        frame |= key == "a=f";
        keys.remove_prefix(comma == std::string_view::npos ? keys.size() : comma + 1);
    }
}

class Icat
{
public:
    Icat(const Settings& settings, const Screen& screen, int out, std::function<bool()> stop)
        : m_s(settings)
        , m_screen(screen)
        , m_out(out)
        , m_stop(std::move(stop))
    {
    }

    // Shows one image; an error message on failure. @p warning tells what
    // did not show, for an image that did.
    std::string show(std::string_view bytes, std::uint32_t id, std::string& warning);
    bool interrupted() const { return m_interrupted; }
    // The temporary files or shared memory objects sent so far.
    const std::vector<std::string>& temporaries() const { return m_temporaries; }

private:
    bool transmit(GraphicsCommand command, std::string_view data, std::string& error);
    // Writes text that is not a graphics command.
    bool emit(std::string_view bytes, std::string& error);
    // Writes graphics commands, in slices with a check for a signal between
    // them. A chunked transmission cut short gets a last empty chunk, so that
    // the terminal drops it and does not wait for the rest.
    bool emitGraphics(const std::string& commands, std::string& error);

    const Settings& m_s;
    const Screen& m_screen;
    int m_out;
    std::function<bool()> m_stop;
    bool m_interrupted = false;
    std::vector<std::string> m_temporaries;
};

bool Icat::emit(std::string_view bytes, std::string& error)
{
    if (!AfterglowProto::writeAll(m_out, bytes)) {
        error = std::string("cannot write to the terminal: ") + std::strerror(errno);
        return false;
    }
    return true;
}

bool Icat::emitGraphics(const std::string& commands, std::string& error)
{
    std::size_t start = 0;
    while (start < commands.size()) {
        std::size_t end = start;
        while (end < commands.size() && end - start < WriteSlice) {
            const auto stop = commands.find("\x1b\\", end);
            end = stop == std::string::npos ? commands.size() : stop + 2;
        }
        const std::string_view slice(commands.data() + start, end - start);
        if (!emit(wrap(slice, m_s.tmux), error)) {
            return false;
        }
        start = end;
        if (start < commands.size() && m_stop()) {
            m_interrupted = true;
            bool open = false;
            bool frame = false;
            lastChunk(slice, open, frame);
            if (open) {
                std::string ignored;
                emit(wrap(std::string("\x1b_G") + (frame ? "a=f," : "") + "m=0,q=2;\x1b\\", m_s.tmux), ignored);
            }
            error = "interrupted";
            return false;
        }
    }
    return true;
}

bool Icat::transmit(GraphicsCommand command, std::string_view data, std::string& error)
{
    std::string payload(data);
    std::string name;
    if (m_s.medium != Medium::Stream) {
        std::string reason;
        name = m_s.medium == Medium::Memory ? sharedMemory(data, reason) : temporaryFile(data, reason);
        if (name.empty()) {
            error = "cannot write the image data for the terminal: " + reason;
            return false;
        }
        command.medium = m_s.medium == Medium::Memory ? 's' : 't';
        command.size = std::uint32_t(data.size());
        payload = name;
    }
    if (!emitGraphics(AfterglowProto::writeGraphics(command, payload), error)) {
        // The terminal never got the name, so it cannot delete the data.
        if (!name.empty()) {
            removeTemporary(m_s.medium, name);
        }
        return false;
    }
    if (!name.empty()) {
        m_temporaries.push_back(name);
    }
    return true;
}

std::string Icat::show(std::string_view bytes, std::uint32_t id, std::string& warning)
{
    int width = 0;
    int height = 0;
    std::string error;
    if (!imageSize(bytes, width, height, error)) {
        return error;
    }
    // The area the image may take, in pixels, and never more than a
    // terminal takes on a side.
    long long available = Unlimited;
    long long availableHeight = Unlimited;
    if (m_s.place) {
        available = (long long)m_s.place->width * m_screen.cellWidth();
        availableHeight = (long long)m_s.place->height * m_screen.cellHeight();
    } else if (m_s.fit == "width") {
        available = m_screen.width;
    } else if (m_s.fit == "height") {
        availableHeight = m_screen.height;
    } else if (m_s.fit == "both") {
        available = m_screen.width;
        availableHeight = m_screen.height;
    }
    const bool bounded = available != Unlimited || availableHeight != Unlimited;
    const long long maxSide = AfterglowProto::GraphicsMaxSide;
    available = std::min(available, maxSide);
    availableHeight = std::min(availableHeight, maxSide);
    double canvasWidth = width;
    double canvasHeight = height;
    if (m_s.scaleUp && bounded && (width < available || height < availableHeight)) {
        const double ratio = std::min(double(available) / width, double(availableHeight) / height);
        if (ratio > 1) {
            canvasWidth = std::floor(width * ratio);
            canvasHeight = std::floor(height * ratio);
        }
    }
    if (canvasWidth > double(available)) {
        canvasHeight = std::floor(canvasHeight * double(available) / canvasWidth);
        canvasWidth = double(available);
    }
    if (canvasHeight > double(availableHeight)) {
        canvasWidth = std::floor(canvasWidth * double(availableHeight) / canvasHeight);
        canvasHeight = double(availableHeight);
    }
    // One frame within the image memory of the terminal.
    const double quotaPixels = double(AfterglowProto::GraphicsDefaultQuota / 4);
    if (canvasWidth * canvasHeight > quotaPixels) {
        const double shrink = std::sqrt(quotaPixels / (canvasWidth * canvasHeight));
        canvasWidth = std::floor(canvasWidth * shrink);
        canvasHeight = std::floor(canvasHeight * shrink);
    }
    const int outWidth = std::max(1, int(canvasWidth));
    const int outHeight = std::max(1, int(canvasHeight));
    const bool scaled = outWidth != width || outHeight != height;
    // Where the image goes, in cells.
    const int cellWidth = m_screen.cellWidth();
    const int cellHeight = m_screen.cellHeight();
    const int widthCells = (outWidth + cellWidth - 1) / cellWidth;
    const int heightCells = (outHeight + cellHeight - 1) / cellHeight;
    if (m_s.placeholder && std::size_t(std::max(widthCells, heightCells)) > AfterglowProto::PlaceholderDiacriticCount) {
        return "the image is too large for Unicode placeholders, which allow "
            + std::to_string(AfterglowProto::PlaceholderDiacriticCount) + " cells at most";
    }
    std::optional<Image> image;
    // A PNG that needs no change goes as it is: the terminal decodes it.
    if (!isPng(bytes) || scaled || m_s.mirrorH || m_s.mirrorV || m_s.background) {
        // Within the image memory of the terminal, checked before decoding.
        image = decodeImage(bytes, AfterglowProto::GraphicsDefaultQuota, error);
        if (!image) {
            if (m_stop()) {
                m_interrupted = true;
                return "interrupted";
            }
            return "cannot decode the image: " + error;
        }
        warning = image->warning();
    }
    int frames = !image ? 1 : m_s.loops == 0 ? 1 : image->frameCount();
    // The terminal keeps every frame at the size it is shown: an animation
    // that does not fit in its image memory shows its first frame.
    if (frames > 1 && std::uint64_t(frames) * std::uint64_t(outWidth) * std::uint64_t(outHeight) * 4 > AfterglowProto::GraphicsDefaultQuota) {
        frames = 1;
        warning = "the animation is too large for the image memory of the terminal at this size, so only its first frame shows";
    }
    const Resizer resize(image ? image->width : width, image ? image->height : height, outWidth, outHeight);
    // Each frame is changed and sent on its own: one copy at a time.
    auto prepare = [&](int index) {
        const std::string& source = image->frame(index);
        if (!m_s.mirrorH && !m_s.mirrorV && !m_s.background) {
            return resize(source);
        }
        std::string rgba = source;
        if (m_s.mirrorH || m_s.mirrorV) {
            mirrorFrame(rgba, image->width, image->height, m_s.mirrorH, m_s.mirrorV);
        }
        if (m_s.background) {
            flattenFrame(rgba, (*m_s.background)[0], (*m_s.background)[1], (*m_s.background)[2]);
        }
        if (resize.identity()) {
            return rgba;
        }
        return resize(rgba);
    };
    const int extra = outWidth % cellWidth;
    int pixelOffset = 0;
    if (extra) {
        pixelOffset = m_s.align == "left" ? 0 : m_s.align == "right" ? cellWidth - extra : (cellWidth - extra) / 2;
    }
    int moveBy = 0;
    int moveX = 0;
    int moveY = 0;
    if (!m_s.place) {
        if (m_s.align == "center") {
            moveBy = (m_screen.columns - widthCells) / 2;
        } else if (m_s.align == "right") {
            moveBy = m_screen.columns - widthCells;
        }
    } else {
        moveX = m_s.place->left + 1;
        moveY = m_s.place->top + 1;
        if (m_s.align == "center") {
            moveX += (m_s.place->width - widthCells) / 2;
        } else if (m_s.align == "right") {
            moveX += m_s.place->width - widthCells;
        }
    }
    const bool animated = frames > 1;
    std::uint32_t number = 0;
    if (id == 0 && m_s.placeholder) {
        id = randomId(true);
    } else if (id == 0 && animated) {
        number = randomId(false);
    }
    std::string move = "\r";
    if (!m_s.placeholder) {
        if (moveBy > 0) {
            move += "\x1b[" + std::to_string(moveBy) + "C";
        }
        if (moveX > 0) {
            move += "\x1b[" + std::to_string(moveY) + ";" + std::to_string(moveX) + "H";
        }
    }
    if (!emit(move, error)) {
        return error;
    }
    GraphicsCommand first;
    first.action = 'T';
    first.hasAction = true;
    first.quiet = 2;
    if (id) {
        first.id = id;
        first.hasId = true;
    }
    if (number) {
        first.number = number;
        first.hasNumber = true;
    }
    if (m_s.placeholder) {
        first.U = 1;
        first.c = widthCells;
        first.r = heightCells;
    }
    if (pixelOffset > 0) {
        first.X = pixelOffset;
    }
    first.z = m_s.z;
    if (m_s.place) {
        first.C = 1;
    }
    GraphicsCommand control;
    control.action = 'a';
    control.hasAction = true;
    control.quiet = 2;
    control.id = first.id;
    control.hasId = first.hasId;
    control.number = first.number;
    control.hasNumber = first.hasNumber;
    // A failure or an interruption after the first frame still ends the
    // image as a finished one: the frames that arrived, the placeholder text
    // and the newline. The first failure is the one reported.
    std::string failure;
    int sent = 0;
    if (!image) {
        first.format = 100;
        if (!transmit(first, bytes, error)) {
            return error;
        }
        sent = 1;
    } else {
        for (int n = 0; n < frames; ++n) {
            if (n > 0 && m_stop()) {
                m_interrupted = true;
                failure = "interrupted";
                break;
            }
            const int delay = image->delay(n);
            std::string data = prepare(n);
            // An opaque frame goes as RGB: a quarter fewer bytes.
            const bool opaque = frameOpaque(data);
            if (opaque) {
                data = dropAlpha(data);
            }
            GraphicsCommand command = n == 0 ? first : GraphicsCommand {};
            if (n > 0) {
                command.action = 'f';
                command.hasAction = true;
                command.quiet = 2;
                command.id = first.id;
                command.hasId = first.hasId;
                command.number = first.number;
                command.hasNumber = first.hasNumber;
                command.z = delay;
                // Each frame is the whole canvas: it replaces the one before.
                command.X = 1;
            }
            command.format = opaque ? 24 : 32;
            command.s = std::uint32_t(outWidth);
            command.v = std::uint32_t(outHeight);
            if (m_s.medium == Medium::Stream) {
                // Over the stream, zlib saves most of the bytes of raw pixels.
                data = AfterglowProto::zlibCompress(data);
                if (data.empty()) {
                    failure = "out of memory";
                    break;
                }
                command.compression = 'z';
            }
            if (!transmit(command, data, error)) {
                failure = error;
                break;
            }
            ++sent;
            if (animated && n == 0) {
                GraphicsCommand gap = control;
                gap.r = 1;
                gap.z = delay;
                if (m_s.loops < 0) {
                    gap.v = 1;
                } else if (m_s.loops > 0) {
                    // v counts the plays plus one, as kitty's loop count does.
                    gap.v = std::uint32_t(std::min<long long>(m_s.loops, UINT32_MAX - 1) + 1);
                }
                if (!emitGraphics(AfterglowProto::writeGraphics(gap, {}), error)) {
                    failure = error;
                    break;
                }
            } else if (animated && n == 1) {
                // Loading: the frames that arrived play while the rest load.
                GraphicsCommand loading = control;
                loading.s = 2;
                if (!emitGraphics(AfterglowProto::writeGraphics(loading, {}), error)) {
                    failure = error;
                    break;
                }
            }
        }
        if (sent == 0) {
            return failure;
        }
    }
    std::string tail;
    if (m_s.placeholder) {
        tail = "\x1b[38:2:" + std::to_string((id >> 16) & 255) + ":" + std::to_string((id >> 8) & 255) + ":"
            + std::to_string(id & 255) + "m";
        std::string restore = "\x1b[39m";
        if (moveY > 0) {
            // With --place the cursor ends at the top left corner of the
            // image, as after the graphics command with C=1.
            tail += "\x1b[" + std::to_string(moveY) + ";1H";
            restore += "\x1b[" + std::to_string(moveY) + ";" + std::to_string(moveX) + "H";
        }
        std::string high;
        AfterglowProto::appendUtf8(high, AfterglowProto::PlaceholderDiacritics[(id >> 24) & 255]);
        for (int row = 0; row < heightCells; ++row) {
            // CSI 0 C would move one column: column 1 takes no move.
            if (moveX > 1) {
                tail += "\x1b[" + std::to_string(moveX - 1) + "C";
            } else if (moveX == 0 && moveBy > 0) {
                tail += std::string(std::size_t(moveBy), ' ');
            }
            for (int column = 0; column < widthCells; ++column) {
                AfterglowProto::appendUtf8(tail, AfterglowProto::PlaceholderChar);
                AfterglowProto::appendUtf8(tail, AfterglowProto::PlaceholderDiacritics[row]);
                AfterglowProto::appendUtf8(tail, AfterglowProto::PlaceholderDiacritics[column]);
                tail += high;
            }
            if (row < heightCells - 1) {
                tail += "\n\r";
            }
        }
        tail += restore;
    }
    if (animated) {
        // Played as it is once all frames are there; an animation cut short
        // stops at its first frame rather than wait for frames that never come.
        GraphicsCommand state = control;
        state.s = failure.empty() ? 3 : 1;
        tail += wrap(AfterglowProto::writeGraphics(state, {}), m_s.tmux);
    }
    if (moveX == 0 && !m_s.noNewline) {
        tail += "\n";
    }
    if (!emit(tail, error) && failure.empty()) {
        failure = error;
    }
    return failure;
}

enum class Detection { Answered, Interrupted, Silent, Closed };

// Asks the terminal which media it reads: a=q with a pixel through each, then
// DA1. A terminal answers in order, so DA1 first means no graphics at all.
Detection detect(Tty& tty, long long timeoutMs, bool& direct, bool& file, bool& memory)
{
    direct = file = memory = false;
    const std::string pixel("\0\0\0", 3);
    GraphicsCommand query;
    query.action = 'q';
    query.hasAction = true;
    query.format = 24;
    query.s = 1;
    query.v = 1;
    query.id = 31;
    query.hasId = true;
    std::string out = AfterglowProto::writeGraphics(query, pixel);
    std::string ignored;
    const auto fileName = temporaryFile(pixel, ignored);
    if (!fileName.empty()) {
        query.id = 32;
        query.medium = 't';
        out += AfterglowProto::writeGraphics(query, fileName);
    }
    const auto memoryName = sharedMemory(pixel, ignored);
    if (!memoryName.empty()) {
        query.id = 33;
        query.medium = 's';
        query.size = 3;
        out += AfterglowProto::writeGraphics(query, memoryName);
    }
    tty.write(out);
    tty.queryAttributes();
    const long long deadline = nowMs() + timeoutMs;
    Detection result = Detection::Silent;
    while (result == Detection::Silent && nowMs() < deadline) {
        std::vector<Input> inputs;
        if (!tty.turn(100, inputs)) {
            result = Detection::Closed;
            break;
        }
        for (const auto& input : inputs) {
            if (input.type == Input::Type::Apc && input.data.starts_with('G')) {
                const auto reply = AfterglowProto::GraphicsReply::parse(std::string_view(input.data).substr(1));
                if (reply && reply->ok()) {
                    direct |= reply->id == 31;
                    file |= reply->id == 32;
                    memory |= reply->id == 33;
                }
            } else if (input.type == Input::Type::Csi && input.data.starts_with('?') && input.data.ends_with('c')) {
                result = Detection::Answered;
            } else if (input.type == Input::Type::Interrupt) {
                result = Detection::Interrupted;
            }
        }
        if (result == Detection::Silent && tty.takeSignal()) {
            result = Detection::Interrupted;
        }
    }
    // A terminal that read them removed them. Others did not.
    if (!fileName.empty()) {
        ::unlink(fileName.c_str());
    }
    if (!memoryName.empty()) {
        ::shm_unlink(memoryName.c_str());
    }
    return result;
}

// Waits until the terminal has read every command before a DA1 query, then
// removes the temporary files and shared memory objects that it left: a
// terminal deletes the ones it read, so the others it never will.
void removeUnread(Tty& tty, long long timeoutMs, Medium medium, const std::vector<std::string>& names)
{
    if (names.empty()) {
        return;
    }
    std::string error;
    if (::isatty(STDOUT_FILENO) && tty.open(error)) {
        tty.queryAttributes();
        const long long deadline = nowMs() + timeoutMs;
        bool answered = false;
        while (!answered && nowMs() < deadline) {
            std::vector<Input> inputs;
            if (!tty.turn(100, inputs)) {
                break;
            }
            for (const auto& input : inputs) {
                answered |= input.type == Input::Type::Interrupt
                    || (input.type == Input::Type::Csi && input.data.starts_with('?') && input.data.ends_with('c'));
            }
            answered |= tty.takeSignal() != 0;
        }
        tty.restore();
    }
    for (const auto& name : names) {
        removeTemporary(medium, name);
    }
}

bool allowTmuxPassthrough()
{
    const char* argv[] = { "tmux", "set", "-p", "allow-passthrough", "on", nullptr };
    pid_t pid = 0;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    const int result = ::posix_spawnp(&pid, "tmux", &actions, nullptr, const_cast<char**>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (result != 0) {
        return false;
    }
    int status = 0;
    pid_t waited = -1;
    while ((waited = ::waitpid(pid, &status, 0)) < 0 && errno == EINTR) {
    }
    return waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// A key that the user pressed, not a reply of the terminal: replies to
// queries are CSI with a private '?' parameter, and strings.
bool keyPress(const Input& input)
{
    switch (input.type) {
    case Input::Type::Text:
    case Input::Type::Interrupt:
    case Input::Type::Escape:
    case Input::Type::Key:
        return true;
    case Input::Type::Csi:
        return !input.data.starts_with('?');
    default:
        return false;
    }
}

} // namespace

int runIcat(int argc, char** argv)
{
    std::string error;
    bool help = false;
    const auto parsed = Cli::parse(command, argc, argv, error, help);
    if (!parsed) {
        if (help) {
            std::fputs(Cli::helpText(command).c_str(), stdout);
            return 0;
        }
        report("icat", error);
        return 1;
    }
    Settings settings;
    if (!parsed->value("place").empty()) {
        settings.place = parsePlace(parsed->value("place"));
        if (!settings.place) {
            report("icat", "Invalid --place specification " + parsed->value("place") + ". Give WIDTHxHEIGHT@LEFTxTOP.");
            return 1;
        }
    }
    settings.fit = parsed->value("fit");
    settings.scaleUp = parsed->flag("scale-up");
    settings.align = parsed->value("align");
    const auto mirror = parsed->value("mirror");
    settings.mirrorH = mirror == "horizontal" || mirror == "both";
    settings.mirrorV = mirror == "vertical" || mirror == "both";
    bool validColour = true;
    settings.background = parseColour(parsed->value("background"), validColour);
    if (!validColour) {
        report("icat", "Invalid value for --background: " + parsed->value("background"));
        return 1;
    }
    {
        std::string z = parsed->value("z-index");
        std::int64_t origin = 0;
        if (z.starts_with("--")) {
            origin = BelowBackground;
            z.erase(0, 1);
        }
        int value = 0;
        if (!parseInt(z, value)) {
            report("icat", "Invalid value for --z-index: " + parsed->value("z-index"));
            return 1;
        }
        settings.z = std::int32_t(std::clamp<std::int64_t>(std::int64_t(value) + origin, INT32_MIN, INT32_MAX));
    }
    settings.loops = parsed->integer("loop");
    if (const auto engine = parsed->value("engine"); engine != "auto" && engine != "builtin") {
        report("icat", "--engine must be auto or builtin. aglow decodes images itself and has no " + engine + " engine.");
        return 1;
    }
    settings.noNewline = parsed->flag("no-trailing-newline");
    const auto passthrough = parsed->value("passthrough");
    settings.tmux = passthrough == "tmux" || (passthrough == "detect" && std::getenv("TMUX") && *std::getenv("TMUX"));
    settings.placeholder = parsed->flag("unicode-placeholder") || settings.tmux;
    const auto mode = parsed->value("transfer-mode");
    const bool detecting = parsed->flag("detect-support");
    const long long timeoutMs = (long long)(parsed->real("detection-timeout") * 1000);
    const int out = STDOUT_FILENO;
    if (detecting && settings.tmux) {
        report("icat", "Inside tmux, aglow cannot ask the terminal whether it shows images, as tmux answers in its place.");
        return 1;
    }
    // A clear needs no window size, and with no image to show it is all there is to do.
    if (parsed->flag("clear-all") || parsed->flag("clear")) {
        GraphicsCommand clear;
        clear.action = 'd';
        clear.hasAction = true;
        clear.quiet = 2;
        if (parsed->flag("clear-all")) {
            // Every id, in capitals: the image data goes too.
            clear.del = 'R';
            clear.ux = 1;
            clear.uy = UINT32_MAX;
        } else {
            clear.del = 'a';
        }
        if (!AfterglowProto::writeAll(out, wrap(AfterglowProto::writeGraphics(clear, {}), settings.tmux))) {
            report("icat", std::string("Cannot write to the terminal: ") + std::strerror(errno));
            return 1;
        }
        const auto input = parsed->value("stdin");
        if (parsed->args.empty() && !detecting && (input == "no" || (input == "detect" && ::isatty(STDIN_FILENO)))) {
            return 0;
        }
    }
    // The window size: from the options, else from the terminal.
    Screen screen;
    if (const auto spec = parsed->value("use-window-size"); !spec.empty()) {
        int fields[4] = {};
        if (!parseFields(spec, ",,,", fields) || fields[0] < 1 || fields[1] < 1 || fields[2] < fields[0] || fields[3] < fields[1]) {
            report("icat", "Invalid size specification " + spec + ". Give COLUMNS,ROWS,WIDTH,HEIGHT.");
            return 1;
        }
        screen = { fields[0], fields[1], fields[2], fields[3] };
    } else {
        winsize size { };
        int fd = ::isatty(STDOUT_FILENO) ? STDOUT_FILENO : ::open("/dev/tty", O_RDONLY | O_CLOEXEC | O_NOCTTY);
        if (fd < 0 || ::ioctl(fd, TIOCGWINSZ, &size) != 0) {
            report("icat", std::string("Failed to query the window size: ") + std::strerror(errno));
            return 1;
        }
        if (fd != STDOUT_FILENO) {
            ::close(fd);
        }
        screen = { size.ws_col, size.ws_row, size.ws_xpixel, size.ws_ypixel };
    }
    if (parsed->flag("print-window-size")) {
        std::printf("%dx%d", screen.width, screen.height);
        return 0;
    }
    if (!detecting && (screen.width == 0 || screen.height == 0)) {
        report("icat", "The terminal does not report its size in pixels. Use a terminal that does, or --use-window-size.");
        return 1;
    }
    // The images, in order: standard input first, as kitten takes them. It
    // is read before the terminal is touched: a command piped into aglow can
    // itself write to the terminal.
    struct Source
    {
        std::string path; ///< empty for standard input
        std::string bytes; ///< standard input, read already
    };
    std::vector<Source> sources;
    const auto stdinMode = parsed->value("stdin");
    const bool stdinPiped = !::isatty(STDIN_FILENO);
    if (!detecting && (stdinMode == "yes" || (stdinMode == "detect" && stdinPiped))) {
        Source input;
        if (!readSource(STDIN_FILENO, input.bytes, {}, error)) {
            report("icat", "Could not read standard input: " + error);
            return 1;
        }
        // detect: an empty pipe, as xargs gives a command with no input, shows nothing.
        if (stdinMode == "yes" || !input.bytes.empty()) {
            sources.push_back(std::move(input));
        }
    }
    // A subdirectory that cannot be read fails the command at its end, after
    // the images that can be shown, as a file that cannot be read does.
    bool unread = false;
    for (const auto& arg : parsed->args) {
        std::string path = arg;
        if (path.starts_with("file:")) {
            const auto uris = AfterglowProto::parseDndUriList(arg);
            if (uris.size() != 1 || uris[0].kind != AfterglowProto::DndUri::Kind::File) {
                report("icat", "Cannot read " + printable(arg) + ". It is not a file of this computer.");
                return 1;
            }
            path = uris[0].path;
        }
        struct stat st { };
        if (::stat(path.c_str(), &st) != 0) {
            report("icat", "Cannot read " + printable(arg) + ": " + std::strerror(errno));
            return 1;
        }
        if (S_ISDIR(st.st_mode)) {
            std::vector<std::string> found;
            std::set<std::pair<dev_t, ino_t>> seen;
            std::vector<std::string> skipped;
            if (!walk(path, found, seen, skipped, error)) {
                report("icat", error);
                return 1;
            }
            for (const auto& reason : skipped) {
                report("icat", reason);
            }
            unread |= !skipped.empty();
            if (found.empty()) {
                report("icat", "The directory " + printable(arg) + " has no image files");
                return 1;
            }
            for (auto& name : found) {
                sources.push_back({ std::move(name), {} });
            }
        } else {
            sources.push_back({ path, {} });
        }
    }
    if (sources.empty() && !detecting) {
        // Nothing was piped in and nothing named: a usage error at a prompt,
        // and nothing to do for a command that xargs runs with no input.
        if (!stdinPiped) {
            report("icat", "No images to show. Give image files, or pipe image data into aglow icat.");
            return 1;
        }
        return 0;
    }
    if (settings.place && sources.size() > 1) {
        report("icat", "The --place option can only be used with a single image, not " + std::to_string(sources.size()));
        return 1;
    }
    Tty tty;
    settings.medium = mode == "file" ? Medium::File : mode == "memory" ? Medium::Memory : Medium::Stream;
    if (!settings.tmux && (mode == "detect" || detecting)) {
        if (!tty.open(error)) {
            report("icat", error);
            return 1;
        }
        bool direct = false;
        bool file = false;
        bool memory = false;
        const auto detection = detect(tty, timeoutMs, direct, file, memory);
        const bool restored = tty.restore();
        if (detection == Detection::Interrupted) {
            report("icat", "interrupted");
            return 130;
        }
        if (detection == Detection::Closed) {
            report("icat", "The terminal closed");
            return 1;
        }
        if (!restored) {
            report("icat", "Could not restore the terminal after asking it about images");
            return 1;
        }
        if (!direct) {
            report("icat", detection == Detection::Silent
                               ? "The terminal did not answer in time. A terminal multiplexer such as tmux or screen may be in the way."
                               : "This terminal does not support the graphics protocol. A terminal multiplexer such as tmux or "
                                 "screen may be in the way.");
            return 1;
        }
        if (detecting) {
            std::fprintf(stderr, "%s\n", memory ? "memory" : file ? "file" : "stream");
            return 0;
        }
        // The terminal reads files and shared memory only through the names
        // that reach it: with standard output elsewhere they would wait in
        // a file or a pipe, and nobody would delete them.
        if (::isatty(STDOUT_FILENO)) {
            settings.medium = memory ? Medium::Memory : file ? Medium::File : Medium::Stream;
        }
    }
    if (settings.tmux) {
        if (settings.medium != Medium::Stream) {
            report("icat", "Inside tmux, images go in escape codes, not through --transfer-mode=" + mode);
        }
        settings.medium = Medium::Stream;
        if (!allowTmuxPassthrough()) {
            report("icat", "Could not turn on tmux passthrough (tmux set -p allow-passthrough on)");
        }
    }
    // While the images go out, Ctrl+C is a signal, and it stops the sending
    // between two slices of commands, so that no command is cut in half.
    Tty::watchSignals();
    bool interrupted = false;
    auto stop = [&] { return interrupted = interrupted || tty.takeSignal() != 0; };
    Icat icat(settings, screen, out, stop);
    auto id = std::uint32_t(parsed->integer("image-id"));
    int code = unread ? 1 : 0;
    for (auto& source : sources) {
        if (stop()) {
            break;
        }
        const std::string name = source.path.empty() ? std::string("<stdin>") : printable(source.path);
        if (!source.path.empty()) {
            const int fd = ::open(source.path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0) {
                report("icat", "Could not open " + name + ": " + std::strerror(errno));
                code = 1;
                continue;
            }
            const bool ok = readSource(fd, source.bytes, stop, error);
            ::close(fd);
            if (!ok) {
                if (interrupted) {
                    break;
                }
                report("icat", "Could not read " + name + ": " + error);
                code = 1;
                continue;
            }
        }
        std::string warning;
        const auto failure = icat.show(source.bytes, id, warning);
        // One image at a time in memory.
        std::string().swap(source.bytes);
        if (!warning.empty()) {
            report("icat", name + ": " + warning);
        }
        if (icat.interrupted() || interrupted) {
            interrupted = true;
            break;
        }
        if (!failure.empty()) {
            report("icat", name + ": " + failure);
            code = 1;
        }
        if (id) {
            // The ids that follow, past UINT32_MAX back to 1: 0 is not an id.
            id = id == UINT32_MAX ? 1 : id + 1;
        }
    }
    if (settings.medium != Medium::Stream) {
        removeUnread(tty, timeoutMs, settings.medium, icat.temporaries());
    }
    if (interrupted) {
        report("icat", "interrupted");
        return 130;
    }
    if (parsed->flag("hold")) {
        if (!tty.open(error)) {
            report("icat", error);
            return 1;
        }
        for (bool done = false; !done;) {
            std::vector<Input> inputs;
            if (!tty.turn(-1, inputs) || tty.takeSignal()) {
                break;
            }
            done = std::any_of(inputs.begin(), inputs.end(), keyPress);
        }
        if (!tty.restore()) {
            report("icat", "Could not restore the terminal");
            code = 1;
        }
    }
    return code;
}

} // namespace Aglow
