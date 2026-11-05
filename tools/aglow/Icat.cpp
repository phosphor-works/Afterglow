// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// aglow icat: kitten icat's command line over the graphics command writer of
// afterglow-proto. PNG goes as it is; other formats, and images that must be
// scaled, go as RGBA with zlib. A GIF goes as animation frames.

#include "Cli.h"
#include "Commands.h"
#include "Image.h"
#include "Tty.h"

#include <AfterglowProto/FdDelta.h>
#include <AfterglowProto/Graphics.h>
#include <AfterglowProto/Placeholder.h>
#include <AfterglowProto/Random.h>
#include <AfterglowProto/TransferClient.h>
#include <AfterglowProto/Zlib.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace Aglow {

namespace {

using AfterglowProto::GraphicsCommand;

// The name that tells a terminal it may delete a file or shared memory object after reading it.
constexpr std::string_view TemporaryTag = "tty-graphics-protocol";
constexpr int Unlimited = INT_MAX;
// kitten's z-index origin for "--1": below the cell backgrounds.
constexpr std::int64_t BelowBackground = -1073741824;

const Cli::Command command {
    "icat",
    "image_file_or_directory ...",
    "Show images in the terminal, with kitty's graphics protocol.",
    "Shows PNG, JPEG, GIF, BMP, PSD, TGA, HDR, PIC and PNM images. A directory shows every image in it. Image "
    "data on standard input is shown too. An animated GIF plays in the terminal.",
    {
        { "--align", Cli::Option::Kind::Choice, "center", { "center", "left", "right" },
          "Horizontal alignment of the image." },
        { "--place", Cli::Option::Kind::String, "", {},
          "Where on the screen to show the image, as WIDTHxHEIGHT@LEFTxTOP in cells, from the top left corner at "
          "0x0. The image is scaled to fit the rectangle and aligned in it with --align. The cursor stays where it "
          "was." },
        { "--scale-up", Cli::Option::Kind::Flag, "", {},
          "Scale images that are smaller than the area up to fill as much of it as they can. The area comes from "
          "--place or --fit." },
        { "--fit", Cli::Option::Kind::Choice, "width", { "width", "height", "both", "none" },
          "Without --place, how the image is scaled to the screen: to fit its width, its height, both, or not at "
          "all." },
        { "--background", Cli::Option::Kind::String, "none", {},
          "A background colour, as #rrggbb or a name. Transparent images are drawn over it." },
        { "--mirror", Cli::Option::Kind::Choice, "none", { "none", "horizontal", "vertical", "both" },
          "Mirror the image about a horizontal or vertical axis, or both." },
        { "--clear", Cli::Option::Kind::Flag, "", {}, "Remove every image that is on the screen." },
        { "--clear-all", Cli::Option::Kind::Flag, "", {}, "Remove every image from the screen and the scrollback." },
        { "--transfer-mode", Cli::Option::Kind::Choice, "detect", { "detect", "file", "stream", "memory" },
          "How images reach the terminal: a temporary file, shared memory, or the data in escape codes. detect "
          "asks the terminal, and uses a file or shared memory only when the terminal can read them, that is "
          "when it runs on this computer." },
        { "--detect-support", Cli::Option::Kind::Flag, "", {},
          "Ask the terminal whether it shows images. Exits with 1 when it does not, else prints the transfer mode "
          "on standard error and exits with 0." },
        { "--detection-timeout", Cli::Option::Kind::Float, "10", {},
          "Seconds to wait for the terminal's answer." },
        { "--use-window-size", Cli::Option::Kind::String, "", {},
          "Use this window size and do not ask the terminal: COLUMNS,ROWS,WIDTH,HEIGHT, the last two in pixels." },
        { "--print-window-size", Cli::Option::Kind::Flag, "", {},
          "Print the window size as WIDTHxHEIGHT in pixels, and exit." },
        { "--stdin", Cli::Option::Kind::Choice, "detect", { "detect", "yes", "no" },
          "Read image data from standard input. detect reads it when standard input is not a terminal." },
        { "--z-index -z", Cli::Option::Kind::String, "0", {},
          "The z-index of the image. Text is drawn over an image with a negative one. With two dashes, as --1, "
          "the image goes under the cell backgrounds too." },
        { "--loop -l", Cli::Option::Kind::Int, "-1", {},
          "How many times an animation plays. A negative number plays it forever, 0 shows its first frame only." },
        { "--hold", Cli::Option::Kind::Flag, "", {}, "Wait for a key press before exiting." },
        { "--unicode-placeholder", Cli::Option::Kind::Flag, "", {},
          "Show images through Unicode placeholder characters, which full screen programs, terminal multiplexers "
          "and editors keep as text." },
        { "--passthrough", Cli::Option::Kind::Choice, "detect", { "detect", "tmux", "none" },
          "Wrap the graphics commands so that they pass through tmux. detect does it inside tmux. This implies "
          "--unicode-placeholder." },
        { "--image-id", Cli::Option::Kind::Int, "0", {},
          "The graphics protocol id of the image. More images take the ids that follow it. 0 picks one when it is "
          "needed." },
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
    return ec == std::errc() && end == text.data() + text.size();
}

std::optional<Place> parsePlace(const std::string& spec)
{
    Place place;
    int consumed = 0;
    if (std::sscanf(spec.c_str(), "%dx%d@%dx%d%n", &place.width, &place.height, &place.left, &place.top, &consumed) != 4
        || std::size_t(consumed) != spec.size() || place.width < 1 || place.height < 1 || place.left < 0 || place.top < 0) {
        return std::nullopt;
    }
    return place;
}

std::optional<std::array<std::uint8_t, 3>> parseColour(std::string text, bool& valid)
{
    valid = true;
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if (text.empty() || text == "none") {
        return std::nullopt;
    }
    static const std::pair<const char*, std::array<std::uint8_t, 3>> names[] = {
        { "black", { 0, 0, 0 } }, { "white", { 255, 255, 255 } }, { "red", { 255, 0, 0 } }, { "green", { 0, 128, 0 } },
        { "blue", { 0, 0, 255 } }, { "yellow", { 255, 255, 0 } }, { "cyan", { 0, 255, 255 } },
        { "magenta", { 255, 0, 255 } }, { "gray", { 128, 128, 128 } }, { "grey", { 128, 128, 128 } },
    };
    for (const auto& [name, rgb] : names) {
        if (text == name) {
            return rgb;
        }
    }
    unsigned r = 0;
    unsigned g = 0;
    unsigned b = 0;
    int consumed = 0;
    if (text.size() == 7 && std::sscanf(text.c_str(), "#%2x%2x%2x%n", &r, &g, &b, &consumed) == 3 && consumed == 7) {
        return std::array<std::uint8_t, 3> { std::uint8_t(r), std::uint8_t(g), std::uint8_t(b) };
    }
    if (text.size() == 4 && std::sscanf(text.c_str(), "#%1x%1x%1x%n", &r, &g, &b, &consumed) == 3 && consumed == 4) {
        return std::array<std::uint8_t, 3> { std::uint8_t(r * 17), std::uint8_t(g * 17), std::uint8_t(b * 17) };
    }
    if (std::sscanf(text.c_str(), "rgb:%2x/%2x/%2x%n", &r, &g, &b, &consumed) == 3 && std::size_t(consumed) == text.size()) {
        return std::array<std::uint8_t, 3> { std::uint8_t(r), std::uint8_t(g), std::uint8_t(b) };
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
    static const char* known[] = { "png", "jpg", "jpeg", "gif", "bmp", "psd", "tga", "hdr", "pic", "pnm", "ppm", "pgm", "pbm" };
    return std::any_of(std::begin(known), std::end(known), [&](const char* k) { return ext == k; });
}

void walk(const std::string& path, std::vector<std::string>& out)
{
    DIR* dir = ::opendir(path.c_str());
    if (!dir) {
        return;
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
            walk(child, out);
        } else if (hasImageExtension(name)) {
            out.push_back(child);
        }
    }
}

// A graphics command as it leaves aglow: wrapped for tmux when it must pass through it.
std::string wrap(const std::string& commands, bool tmux)
{
    if (!tmux) {
        return commands;
    }
    std::string out;
    std::size_t start = 0;
    while (start < commands.size()) {
        auto end = commands.find("\x1b\\", start + 1);
        end = end == std::string::npos ? commands.size() : end + 2;
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
        // A placeholder id needs its high byte and two middle bytes set, so
        // that programs that keep only 256 colours or no third diacritic
        // cannot mix it up with another (as kitten picks them).
        if (id != 0 && (!placeholder || ((id & 0xFF000000u) && (id & 0x00FFFF00u)))) {
            return id;
        }
    }
}

// Writes @p bytes to a new temporary file whose name tells the terminal to
// delete it after reading. Empty on failure.
std::string temporaryFile(std::string_view bytes)
{
    for (const char* dir : { "/dev/shm", "/tmp" }) {
        std::string name = std::string(dir) + "/" + std::string(TemporaryTag) + "-aglow-XXXXXX";
        const int fd = ::mkostemp(name.data(), O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        const bool ok = AfterglowProto::writeAll(fd, bytes);
        ::close(fd);
        if (ok) {
            return name;
        }
        ::unlink(name.c_str());
    }
    return {};
}

// A shared memory object with @p bytes, which the terminal unlinks after reading it.
std::string sharedMemory(std::string_view bytes)
{
    const auto suffix = AfterglowProto::randomHex(8);
    if (!suffix) {
        return {};
    }
    const std::string name = "/" + std::string(TemporaryTag) + "-aglow-" + *suffix;
    const int fd = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        return {};
    }
    const bool ok = ::ftruncate(fd, off_t(bytes.size())) == 0 && AfterglowProto::writeAll(fd, bytes);
    ::close(fd);
    if (!ok) {
        ::shm_unlink(name.c_str());
        return {};
    }
    return name;
}

class Icat
{
public:
    Icat(const Settings& settings, const Screen& screen, int out)
        : m_s(settings)
        , m_screen(screen)
        , m_out(out)
    {
    }

    // Shows one image; an error message on failure.
    std::string show(const std::string& name, std::string_view bytes, std::uint32_t id);

private:
    bool transmit(GraphicsCommand command, std::string_view data, std::string& error);
    void emit(const std::string& bytes) { AfterglowProto::writeAll(m_out, bytes); }

    const Settings& m_s;
    const Screen& m_screen;
    int m_out;
};

bool Icat::transmit(GraphicsCommand command, std::string_view data, std::string& error)
{
    std::string payload(data);
    if (m_s.medium != Medium::Stream) {
        const auto name = m_s.medium == Medium::Memory ? sharedMemory(data) : temporaryFile(data);
        if (name.empty()) {
            error = std::string("cannot write the image data for the terminal: ") + std::strerror(errno);
            return false;
        }
        command.medium = m_s.medium == Medium::Memory ? 's' : 't';
        command.size = std::uint32_t(data.size());
        payload = name;
    }
    emit(wrap(AfterglowProto::writeGraphics(command, payload), m_s.tmux));
    return true;
}

std::string Icat::show(const std::string& name, std::string_view bytes, std::uint32_t id)
{
    int width = 0;
    int height = 0;
    if (!imageSize(bytes, width, height)) {
        return "not an image that aglow can read";
    }
    // The area the image may take, in pixels.
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
    int canvasWidth = width;
    int canvasHeight = height;
    if (m_s.scaleUp && (width < available || height < availableHeight) && (available != Unlimited || availableHeight != Unlimited)) {
        const double ratio = std::min(double(available) / width, double(availableHeight) / height);
        canvasWidth = std::max(1, int(width * ratio));
        canvasHeight = std::max(1, int(height * ratio));
    }
    if (canvasWidth > available) {
        canvasHeight = std::max(1, int(std::floor(double(canvasHeight) * double(available) / canvasWidth)));
        canvasWidth = int(available);
    }
    if (canvasHeight > availableHeight) {
        canvasWidth = std::max(1, int(std::floor(double(canvasWidth) * double(availableHeight) / canvasHeight)));
        canvasHeight = int(availableHeight);
    }
    const bool scaled = canvasWidth != width || canvasHeight != height;
    std::optional<Image> image;
    std::string error;
    const bool png = isPng(bytes);
    // A PNG that needs no change goes as it is: the terminal decodes it.
    if (!png || scaled || m_s.mirrorH || m_s.mirrorV || m_s.background || bytes.starts_with("GIF8")) {
        image = decodeImage(bytes, error);
        if (!image) {
            return "cannot decode the image: " + error;
        }
        if (m_s.mirrorH || m_s.mirrorV) {
            mirrorImage(*image, m_s.mirrorH, m_s.mirrorV);
        }
        if (m_s.background) {
            flattenImage(*image, (*m_s.background)[0], (*m_s.background)[1], (*m_s.background)[2]);
        }
        resizeImage(*image, canvasWidth, canvasHeight);
        if (m_s.loops == 0 && image->frames.size() > 1) {
            image->frames.resize(1);
        }
    }
    // Where the image goes, in cells.
    const int cellWidth = m_screen.cellWidth();
    const int cellHeight = m_screen.cellHeight();
    const int widthCells = (canvasWidth + cellWidth - 1) / cellWidth;
    const int heightCells = (canvasHeight + cellHeight - 1) / cellHeight;
    const int extra = canvasWidth % cellWidth;
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
    if (m_s.placeholder && std::size_t(std::max(widthCells, heightCells)) >= AfterglowProto::PlaceholderDiacriticCount) {
        return "the image is too large for Unicode placeholders, which allow " + std::to_string(AfterglowProto::PlaceholderDiacriticCount) + " cells at most";
    }
    const bool animated = image && image->frames.size() > 1;
    std::uint32_t number = 0;
    if (id == 0 && m_s.placeholder) {
        id = randomId(true);
    } else if (id == 0 && animated) {
        number = randomId(false);
    }
    emit("\r");
    if (!m_s.placeholder) {
        if (moveBy > 0) {
            emit("\x1b[" + std::to_string(moveBy) + "C");
        }
        if (moveX > 0) {
            emit("\x1b[" + std::to_string(moveY) + ";" + std::to_string(moveX) + "H");
        }
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
    if (!image) {
        first.format = 100;
        if (!transmit(first, bytes, error)) {
            return error;
        }
    } else {
        const bool opaque = imageOpaque(*image);
        auto pixels = [&](const Image::Frame& frame) {
            if (!opaque) {
                return frame.rgba;
            }
            std::string rgb;
            rgb.reserve(frame.rgba.size() / 4 * 3);
            for (std::size_t i = 0; i < frame.rgba.size(); i += 4) {
                rgb.append(frame.rgba, i, 3);
            }
            return rgb;
        };
        GraphicsCommand control;
        control.action = 'a';
        control.hasAction = true;
        control.quiet = 2;
        if (id) {
            control.id = id;
            control.hasId = true;
        } else {
            control.number = number;
            control.hasNumber = true;
        }
        for (std::size_t n = 0; n < image->frames.size(); ++n) {
            const auto& frame = image->frames[n];
            GraphicsCommand command = n == 0 ? first : GraphicsCommand {};
            if (n > 0) {
                command.action = 'f';
                command.hasAction = true;
                command.quiet = 2;
                command.id = first.id;
                command.hasId = first.hasId;
                command.number = first.number;
                command.hasNumber = first.hasNumber;
                command.z = frame.delayMs;
                // Each frame is the whole canvas: it replaces the one before.
                command.X = 1;
            }
            command.format = opaque ? 24 : 32;
            command.s = std::uint32_t(image->width);
            command.v = std::uint32_t(image->height);
            std::string data = pixels(frame);
            if (m_s.medium == Medium::Stream) {
                // Over the stream, zlib saves most of the bytes of raw pixels.
                data = AfterglowProto::zlibCompress(data);
                if (data.empty()) {
                    return "out of memory";
                }
                command.compression = 'z';
            }
            if (!transmit(command, data, error)) {
                return error;
            }
            if (animated && n == 0) {
                GraphicsCommand gap = control;
                gap.r = 1;
                gap.z = frame.delayMs;
                if (m_s.loops < 0) {
                    gap.v = 1;
                } else if (m_s.loops > 0) {
                    gap.v = std::uint32_t(std::min<long long>(m_s.loops + 1, UINT32_MAX));
                }
                emit(wrap(AfterglowProto::writeGraphics(gap, {}), m_s.tmux));
            } else if (animated && n == 1) {
                // Loading: the frames that arrived play while the rest load.
                GraphicsCommand loading = control;
                loading.s = 2;
                emit(wrap(AfterglowProto::writeGraphics(loading, {}), m_s.tmux));
            }
        }
    }
    if (m_s.placeholder) {
        std::string text = "\x1b[38:2:" + std::to_string((id >> 16) & 255) + ":" + std::to_string((id >> 8) & 255) + ":"
            + std::to_string(id & 255) + "m";
        std::string restore = "\x1b[39m";
        if (moveY > 0) {
            text += "\x1b" "7\x1b[" + std::to_string(moveY) + ";0H";
            restore += "\x1b" "8";
        }
        auto utf8 = [](char32_t cp) {
            std::string out;
            if (cp < 0x80) {
                out += char(cp);
            } else if (cp < 0x800) {
                out += char(0xC0 | (cp >> 6));
                out += char(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                out += char(0xE0 | (cp >> 12));
                out += char(0x80 | ((cp >> 6) & 0x3F));
                out += char(0x80 | (cp & 0x3F));
            } else {
                out += char(0xF0 | (cp >> 18));
                out += char(0x80 | ((cp >> 12) & 0x3F));
                out += char(0x80 | ((cp >> 6) & 0x3F));
                out += char(0x80 | (cp & 0x3F));
            }
            return out;
        };
        const auto high = utf8(AfterglowProto::PlaceholderDiacritics[(id >> 24) & 255]);
        for (int row = 0; row < heightCells; ++row) {
            if (moveX > 0) {
                text += "\x1b[" + std::to_string(moveX - 1) + "C";
            } else if (moveBy > 0) {
                text += std::string(std::size_t(moveBy), ' ');
            }
            for (int column = 0; column < widthCells; ++column) {
                text += utf8(AfterglowProto::PlaceholderChar) + utf8(AfterglowProto::PlaceholderDiacritics[row])
                    + utf8(AfterglowProto::PlaceholderDiacritics[column]) + high;
            }
            if (row < heightCells - 1) {
                text += "\n\r";
            }
        }
        emit(text + restore);
    }
    if (animated) {
        GraphicsCommand normal;
        normal.action = 'a';
        normal.hasAction = true;
        normal.quiet = 2;
        normal.id = first.id;
        normal.hasId = first.hasId;
        normal.number = first.number;
        normal.hasNumber = first.hasNumber;
        normal.s = 3;
        emit(wrap(AfterglowProto::writeGraphics(normal, {}), m_s.tmux));
    }
    if (moveX == 0 && !m_s.noNewline) {
        emit("\n");
    }
    (void)name;
    return {};
}

// Asks the terminal which media it reads: a=q with a pixel through each, then
// DA1. A terminal answers in order, so DA1 first means no graphics at all.
bool detect(Tty& tty, long long timeoutMs, bool& direct, bool& file, bool& memory)
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
    const auto fileName = temporaryFile(pixel);
    if (!fileName.empty()) {
        query.id = 32;
        query.medium = 't';
        out += AfterglowProto::writeGraphics(query, fileName);
    }
    const auto memoryName = sharedMemory(pixel);
    if (!memoryName.empty()) {
        query.id = 33;
        query.medium = 's';
        query.size = 3;
        out += AfterglowProto::writeGraphics(query, memoryName);
    }
    tty.write(out + "\x1b[c");
    const long long deadline = nowMs() + timeoutMs;
    bool answered = false;
    while (!answered && nowMs() < deadline) {
        std::vector<Input> inputs;
        if (!tty.turn(100, inputs)) {
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
                answered = true;
            } else if (input.type == Input::Type::Interrupt) {
                answered = true;
            }
        }
    }
    // A terminal that read them removed them. Others did not.
    if (!fileName.empty()) {
        ::unlink(fileName.c_str());
    }
    if (!memoryName.empty()) {
        ::shm_unlink(memoryName.c_str());
    }
    return answered;
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
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
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
            report("icat", "Invalid --place specification: " + parsed->value("place"));
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
    settings.noNewline = parsed->flag("no-trailing-newline");
    const auto passthrough = parsed->value("passthrough");
    settings.tmux = passthrough == "tmux" || (passthrough == "detect" && std::getenv("TMUX") && *std::getenv("TMUX"));
    settings.placeholder = parsed->flag("unicode-placeholder") || settings.tmux;
    // The window size: from the options, else from the terminal.
    Screen screen;
    if (const auto spec = parsed->value("use-window-size"); !spec.empty()) {
        int consumed = 0;
        if (std::sscanf(spec.c_str(), "%d,%d,%d,%d%n", &screen.columns, &screen.rows, &screen.width, &screen.height, &consumed) != 4
            || std::size_t(consumed) != spec.size() || screen.columns < 1 || screen.rows < 1 || screen.width < screen.columns
            || screen.height < screen.rows) {
            report("icat", "Invalid size specification: " + spec);
            return 1;
        }
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
    const int out = STDOUT_FILENO;
    if (parsed->flag("clear-all")) {
        GraphicsCommand clear;
        clear.action = 'd';
        clear.hasAction = true;
        clear.del = 'r';
        clear.ux = 1;
        clear.uy = UINT32_MAX;
        AfterglowProto::writeAll(out, wrap(AfterglowProto::writeGraphics(clear, {}), settings.tmux));
    } else if (parsed->flag("clear")) {
        GraphicsCommand clear;
        clear.action = 'd';
        clear.hasAction = true;
        clear.del = 'a';
        AfterglowProto::writeAll(out, wrap(AfterglowProto::writeGraphics(clear, {}), settings.tmux));
    }
    if (screen.width == 0 || screen.height == 0) {
        report("icat", "The terminal does not report its size in pixels: use a terminal that does, or --use-window-size");
        return 1;
    }
    // The images, in order: standard input first, as kitten takes them.
    std::vector<std::string> sources;
    const auto stdinMode = parsed->value("stdin");
    if (stdinMode == "yes" || (stdinMode == "detect" && !::isatty(STDIN_FILENO))) {
        sources.emplace_back();
    }
    for (const auto& arg : parsed->args) {
        std::string path = arg;
        if (path.starts_with("file://")) {
            path.erase(0, 7);
        }
        struct stat st { };
        if (::stat(path.c_str(), &st) != 0) {
            report("icat", "Cannot read " + arg + ": " + std::strerror(errno));
            return 1;
        }
        if (S_ISDIR(st.st_mode)) {
            walk(path, sources);
        } else {
            sources.push_back(path);
        }
    }
    if (settings.place && sources.size() > 1) {
        report("icat", "The --place option can only be used with a single image, not " + std::to_string(sources.size()));
        return 1;
    }
    const auto mode = parsed->value("transfer-mode");
    const bool detecting = parsed->flag("detect-support");
    Tty tty;
    const bool needTty = (!settings.tmux && (mode == "detect" || detecting)) || parsed->flag("hold");
    if (needTty && !tty.open(error)) {
        report("icat", error);
        return 1;
    }
    settings.medium = mode == "file" ? Medium::File : mode == "memory" ? Medium::Memory : Medium::Stream;
    if (!settings.tmux && (mode == "detect" || detecting)) {
        bool direct = false;
        bool file = false;
        bool memory = false;
        detect(tty, (long long)(parsed->real("detection-timeout") * 1000), direct, file, memory);
        if (!direct) {
            tty.restore();
            report("icat", "This terminal does not support the graphics protocol. A terminal multiplexer such as tmux "
                           "or screen may be in the way.");
            return 1;
        }
        if (detecting) {
            tty.restore();
            std::fprintf(stderr, "%s\n", memory ? "memory" : file ? "files" : "stream");
            return 0;
        }
        if (mode == "detect") {
            settings.medium = memory ? Medium::Memory : file ? Medium::File : Medium::Stream;
        }
    }
    if (settings.tmux) {
        settings.medium = Medium::Stream;
        if (!allowTmuxPassthrough()) {
            report("icat", "Could not turn on tmux passthrough (tmux set -p allow-passthrough on)");
        }
    }
    Icat icat(settings, screen, out);
    auto id = std::uint32_t(parsed->integer("image-id"));
    int code = 0;
    for (const auto& source : sources) {
        std::string bytes;
        if (source.empty()) {
            if (!readAll(STDIN_FILENO, bytes)) {
                report("icat", "Could not read standard input: " + std::string(std::strerror(errno)));
                code = 1;
                continue;
            }
        } else {
            const int fd = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
            const bool ok = fd >= 0 && readAll(fd, bytes);
            if (fd >= 0) {
                ::close(fd);
            }
            if (!ok) {
                report("icat", "Could not open " + source + ": " + std::strerror(errno));
                code = 1;
                continue;
            }
        }
        const auto failure = icat.show(source, bytes, id);
        if (!failure.empty()) {
            report("icat", (source.empty() ? std::string("<stdin>") : source) + ": " + failure);
            code = 1;
        }
        if (id) {
            ++id;
        }
    }
    if (parsed->flag("hold")) {
        for (bool done = false; !done;) {
            std::vector<Input> inputs;
            if (!tty.turn(-1, inputs) || tty.takeSignal()) {
                break;
            }
            for (const auto& input : inputs) {
                done |= input.type != Input::Type::Csi;
            }
        }
    }
    tty.restore();
    return code;
}

} // namespace Aglow
