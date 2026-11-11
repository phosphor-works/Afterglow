// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "Image.h"

#include "Commands.h"
#include "StbImage.h"
#include "Tty.h"

#include <AfterglowProto/Fd.h>
#include <AfterglowProto/Graphics.h>
#include <AfterglowProto/Limits.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Aglow {

static_assert(std::uint32_t(Stb::MaxSide) == AfterglowProto::GraphicsMaxSide,
              "stb_image's STBI_MAX_DIMENSIONS must be the longest side that Afterglow takes");

namespace {

std::uint8_t at(std::string_view bytes, std::size_t index) { return std::uint8_t(bytes[index]); }

std::uint32_t big32(std::string_view bytes, std::size_t index)
{
    return std::uint32_t(at(bytes, index)) << 24 | std::uint32_t(at(bytes, index + 1)) << 16
        | std::uint32_t(at(bytes, index + 2)) << 8 | at(bytes, index + 3);
}

// The Orientation tag (0x0112) of the first IFD of a TIFF block, 1 without it.
int tiffOrientation(std::string_view tiff)
{
    if (tiff.size() < 8) {
        return 1;
    }
    bool little = false;
    if (tiff.starts_with("II")) {
        little = true;
    } else if (!tiff.starts_with("MM")) {
        return 1;
    }
    auto u16 = [&](std::size_t i) -> std::uint32_t {
        return little ? std::uint32_t(at(tiff, i)) | std::uint32_t(at(tiff, i + 1)) << 8
                      : std::uint32_t(at(tiff, i)) << 8 | std::uint32_t(at(tiff, i + 1));
    };
    auto u32 = [&](std::size_t i) -> std::uint32_t {
        return little ? u16(i) | u16(i + 2) << 16 : u16(i) << 16 | u16(i + 2);
    };
    if (u16(2) != 42) {
        return 1;
    }
    const std::size_t ifd = u32(4);
    if (ifd > tiff.size() - 2) {
        return 1;
    }
    const std::size_t entries = u16(ifd);
    for (std::size_t i = 0; i < entries; ++i) {
        const std::size_t entry = ifd + 2 + i * 12;
        if (entry > tiff.size() - 12) {
            break;
        }
        // Type 3 is SHORT: the value is in the first two bytes of the field.
        if (u16(entry) == 0x0112 && u16(entry + 2) == 3) {
            const auto value = u16(entry + 8);
            return value >= 1 && value <= 8 ? int(value) : 1;
        }
    }
    return 1;
}

// The EXIF orientation of a JPEG: 1 is upright, 5 to 8 swap its sides.
int jpegOrientation(std::string_view bytes)
{
    if (bytes.size() < 4 || at(bytes, 0) != 0xFF || at(bytes, 1) != 0xD8) {
        return 1;
    }
    std::size_t i = 2;
    while (i + 4 <= bytes.size()) {
        if (at(bytes, i) != 0xFF) {
            return 1;
        }
        const auto marker = at(bytes, i + 1);
        if (marker == 0xFF) {
            ++i; // a fill byte
            continue;
        }
        // EXIF comes before the image data, and these markers have no length.
        if (marker == 0xDA || marker == 0xD9) {
            return 1;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
            i += 2;
            continue;
        }
        const std::size_t length = std::size_t(at(bytes, i + 2)) << 8 | at(bytes, i + 3);
        if (length < 2 || length > bytes.size() - i - 2) {
            return 1;
        }
        const auto segment = bytes.substr(i + 4, length - 2);
        if (marker == 0xE1 && segment.starts_with(std::string_view("Exif\0\0", 6))) {
            return tiffOrientation(segment.substr(6));
        }
        i += 2 + length;
    }
    return 1;
}

// The frame turned upright: @p width and @p height are those of @p rgba, and
// they swap for orientations 5 to 8.
std::string orient(const unsigned char* rgba, int width, int height, int orientation)
{
    const std::size_t w = std::size_t(width);
    const std::size_t h = std::size_t(height);
    const bool swap = orientation >= 5;
    const std::size_t outWidth = swap ? h : w;
    const std::size_t outHeight = swap ? w : h;
    std::string out(w * h * 4, '\0');
    for (std::size_t y = 0; y < outHeight; ++y) {
        for (std::size_t x = 0; x < outWidth; ++x) {
            std::size_t sx = x;
            std::size_t sy = y;
            switch (orientation) {
            case 2: sx = w - 1 - x; break;
            case 3: sx = w - 1 - x; sy = h - 1 - y; break;
            case 4: sy = h - 1 - y; break;
            case 5: sx = y; sy = x; break;
            case 6: sx = y; sy = h - 1 - x; break;
            case 7: sx = w - 1 - y; sy = h - 1 - x; break;
            case 8: sx = w - 1 - y; sy = x; break;
            default: break;
            }
            std::memcpy(out.data() + (y * outWidth + x) * 4, rgba + (sy * w + sx) * 4, 4);
        }
    }
    return out;
}

// What the decoder child sends its parent, in this order: frames, then one of
// Warning and End, or Error, or TooMany.
enum class Kind : std::uint32_t { Frame, Warning, Error, TooMany, End };

struct Record
{
    Kind kind = Kind::End;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t delay = 0;
    std::uint64_t length = 0;
};

// A message from the child is never longer than this.
constexpr std::uint64_t MaxMessage = 4096;

bool send(int fd, const Record& record, std::string_view payload = {})
{
    return AfterglowProto::writeAll(fd, std::string_view(reinterpret_cast<const char*>(&record), sizeof record))
        && AfterglowProto::writeAll(fd, payload);
}

// The signals that Tty catches to stop a command. While the parent waits
// for the child, they are blocked, and ppoll() takes them in its mask only:
// a signal that arrives at any time ends the wait, where a read() that
// SA_RESTART restarts would wait for the child to end.
class SignalGate
{
public:
    SignalGate()
    {
        sigset_t stops;
        sigemptyset(&stops);
        for (const int number : { SIGINT, SIGTERM, SIGHUP }) {
            sigaddset(&stops, number);
        }
        ::sigprocmask(SIG_BLOCK, &stops, &m_waiting);
    }
    ~SignalGate() { ::sigprocmask(SIG_SETMASK, &m_waiting, nullptr); }
    SignalGate(const SignalGate&) = delete;
    SignalGate& operator=(const SignalGate&) = delete;
    /// The mask from before the gate, for ppoll().
    const sigset_t* waiting() const { return &m_waiting; }

private:
    sigset_t m_waiting;
};

// Reads @p size bytes from the child. False at its end, and with
// @p interrupted set when a signal that Tty caught stops the command.
bool readChild(int fd, void* data, std::size_t size, const SignalGate& gate, bool& interrupted)
{
    auto* p = static_cast<char*>(data);
    while (size > 0) {
        if (Tty::signalPending()) {
            interrupted = true;
            return false;
        }
        pollfd ready { fd, POLLIN, 0 };
        if (::ppoll(&ready, 1, nullptr, gate.waiting()) < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        const ssize_t n = ::read(fd, p, size);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        p += n;
        size -= std::size_t(n);
    }
    return true;
}

struct Limits
{
    int orientation = 1;
    int width = 0; ///< upright
    int height = 0;
    std::size_t maxFrames = 1;
    std::uint64_t memory = 0; ///< what the decoder may allocate, past the address space at the fork
    rlim_t seconds = 0; ///< of CPU time
};

[[noreturn]] void runChild(int fd, std::string_view bytes, const Limits& limits, pid_t parent)
{
    // The child ends with the parent, also when a signal kills the parent
    // before it can kill the child.
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() != parent) {
        ::_exit(1);
    }
    // The signals that the parent catches kill the child, so that Ctrl+C
    // stops a long decode, and SIGCONT no longer runs the parent's handler.
    // SIGPIPE ends the child when the parent stops reading.
    for (const int number : { SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGWINCH, SIGCONT }) {
        struct sigaction inherited { };
        if (::sigaction(number, nullptr, &inherited) == 0 && inherited.sa_handler != SIG_IGN) {
            std::signal(number, SIG_DFL);
        }
    }
    std::signal(SIGPIPE, SIG_DFL);
    AfterglowProto::limit(RLIMIT_CORE, 0, 0);
    AfterglowProto::limit(RLIMIT_CPU, limits.seconds, limits.seconds + 1);
    if (const auto base = AfterglowProto::addressSpace()) {
        AfterglowProto::limit(RLIMIT_AS, rlim_t(base + limits.memory), rlim_t(base + limits.memory));
    }
    int code = 0;
    try {
        std::size_t frames = 0;
        bool written = true;
        std::string error;
        std::string warning;
        const bool ok = Stb::decode(bytes, [&](const unsigned char* rgba, int width, int height, int delay) {
            if (++frames > limits.maxFrames) {
                written = send(fd, { Kind::TooMany });
                return false;
            }
            const std::size_t size = std::size_t(width) * std::size_t(height) * 4;
            Record record { Kind::Frame, limits.width, limits.height, delay, size };
            if (limits.orientation != 1) {
                written = send(fd, record, orient(rgba, width, height, limits.orientation));
            } else {
                record.width = width;
                record.height = height;
                written = send(fd, record, std::string_view(reinterpret_cast<const char*>(rgba), size));
            }
            return written;
        }, error, warning);
        if (!ok) {
            error.resize(std::min<std::size_t>(error.size(), MaxMessage));
            send(fd, { Kind::Error, 0, 0, 0, error.size() }, error);
        } else if (written) {
            if (!warning.empty()) {
                warning.resize(std::min<std::size_t>(warning.size(), MaxMessage));
                send(fd, { Kind::Warning, 0, 0, 0, warning.size() }, warning);
            }
            send(fd, { Kind::End });
        }
    } catch (...) {
        code = 1; // std::bad_alloc under the memory limit: the parent reads why from the exit
    }
    ::_exit(code);
}

// The decoder child, killed and reaped however decodeImage() ends, also by
// an exception.
class Child
{
public:
    explicit Child(pid_t pid)
        : m_pid(pid)
    {
    }
    ~Child()
    {
        if (m_pid > 0) {
            finish(true);
        }
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    /// Waits for the child to end, after SIGKILL when @p kill. Its wait status.
    int finish(bool kill)
    {
        if (kill) {
            ::kill(m_pid, SIGKILL);
        }
        int status = 0;
        while (::waitpid(m_pid, &status, 0) < 0 && errno == EINTR) {
        }
        m_pid = -1;
        return status;
    }

private:
    pid_t m_pid;
};

// Why the child ended before it sent its result.
std::string childFailure(int status)
{
    if (WIFSIGNALED(status)) {
        const int number = WTERMSIG(status);
        if (number == SIGXCPU || number == SIGKILL) {
            return "decoding takes longer than aglow allows";
        }
        if (number == SIGINT || number == SIGTERM || number == SIGHUP) {
            return "interrupted";
        }
        return "the decoder crashed on this file, which is corrupt";
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        return "the image decoder stopped early";
    }
    return "the image needs more memory to decode than aglow allows";
}

} // namespace

bool imageSize(std::string_view bytes, int& width, int& height, std::string& error)
{
    switch (Stb::probe(bytes, width, height)) {
    case Stb::Probe::Unknown:
        error = "not an image that aglow can read";
        return false;
    case Stb::Probe::TooLarge:
        error = width > 0 && height > 0
            ? "the image is " + std::to_string(width) + "x" + std::to_string(height) + " pixels, and a terminal takes no side longer than "
                + std::to_string(AfterglowProto::GraphicsMaxSide)
            : "the image has a side longer than " + std::to_string(AfterglowProto::GraphicsMaxSide)
                + " pixels, which is more than a terminal takes";
        return false;
    case Stb::Probe::Image:
        break;
    }
    if (jpegOrientation(bytes) >= 5) {
        std::swap(width, height);
    }
    return true;
}

bool imageSize(std::string_view bytes, int& width, int& height)
{
    std::string error;
    return imageSize(bytes, width, height, error);
}

bool isPng(std::string_view bytes)
{
    // The signature, then the first chunk: IHDR of 13 bytes. Apple's CgBI
    // chunk comes before IHDR, so a CgBI file is not a PNG to a terminal.
    if (bytes.size() < 33 || !bytes.starts_with("\x89PNG\r\n\x1a\n") || big32(bytes, 8) != 13
        || bytes.substr(12, 4) != "IHDR") {
        return false;
    }
    const auto width = big32(bytes, 16);
    const auto height = big32(bytes, 20);
    const int depth = at(bytes, 24);
    const int colour = at(bytes, 25);
    const bool low = depth == 1 || depth == 2 || depth == 4;
    bool valid = false;
    switch (colour) {
    case 0: valid = low || depth == 8 || depth == 16; break;
    case 3: valid = low || depth == 8; break;
    case 2:
    case 4:
    case 6: valid = depth == 8 || depth == 16; break;
    default: break;
    }
    return valid && width > 0 && height > 0 && width <= AfterglowProto::GraphicsMaxSide
        && height <= AfterglowProto::GraphicsMaxSide;
}

std::optional<Image> decodeImage(std::string_view bytes, std::size_t budget, std::string& error)
{
    Image image;
    if (!imageSize(bytes, image.width, image.height, error)) {
        return std::nullopt;
    }
    // Checked before any pixel is decoded: a small file can name a huge image.
    const std::uint64_t frameBytes = std::uint64_t(image.width) * std::uint64_t(image.height) * 4;
    if (frameBytes > budget) {
        error = "the image is " + std::to_string(image.width) + "x" + std::to_string(image.height) + " pixels and needs "
            + humanSize(frameBytes) + ", more than the " + humanSize(budget) + " that aglow allows for it";
        return std::nullopt;
    }
    Limits limits;
    limits.orientation = jpegOrientation(bytes);
    limits.width = image.width;
    limits.height = image.height;
    limits.maxFrames = std::size_t(std::max<std::uint64_t>(
        1, std::min<std::uint64_t>(AfterglowProto::GraphicsMaxFrames, budget / frameBytes)));
    // stb_image needs a few times the decoded image while it works (16-bit
    // PNG, float HDR, a copy of the compressed data). Past this, its
    // allocations fail, whatever a corrupt file asks for.
    limits.memory = 8 * frameBytes + bytes.size() + (std::uint64_t(64) << 20);
    // Seconds of CPU time: decoding runs at well over 16 MiB a second.
    limits.seconds = rlim_t(5 + (std::uint64_t(limits.maxFrames) * frameBytes >> 24));

    int fds[2] = { -1, -1 };
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        error = std::string("cannot start the image decoder: ") + std::strerror(errno);
        return std::nullopt;
    }
    AfterglowProto::UniqueFd reader(fds[0]);
    AfterglowProto::UniqueFd writer(fds[1]);
    const pid_t parent = ::getpid();
    const pid_t pid = ::fork();
    if (pid < 0) {
        error = std::string("cannot start the image decoder: ") + std::strerror(errno);
        return std::nullopt;
    }
    if (pid == 0) {
        reader.release();
        ::close(fds[0]);
        runChild(fds[1], bytes, limits, parent);
    }
    Child child(pid);
    writer.reset();

    bool finished = false;
    bool tooMany = false;
    bool interrupted = false;
    const SignalGate gate;
    auto readExact = [&](void* data, std::size_t size) { return readChild(reader.get(), data, size, gate, interrupted); };
    for (;;) {
        Record record;
        if (!readExact(&record, sizeof record)) {
            break;
        }
        if (record.kind == Kind::Frame) {
            if (record.width != image.width || record.height != image.height || record.length != frameBytes
                || image.m_frames.size() >= limits.maxFrames) {
                error = "the image is not what its header says";
                break;
            }
            std::string pixels(std::size_t(frameBytes), '\0');
            if (!readExact(pixels.data(), pixels.size())) {
                break;
            }
            image.m_frames.push_back(std::move(pixels));
            image.m_delays.push_back(record.delay);
            continue;
        }
        if (record.kind == Kind::TooMany) {
            tooMany = true;
            break;
        }
        if (record.kind == Kind::End) {
            finished = true;
            break;
        }
        if ((record.kind != Kind::Warning && record.kind != Kind::Error) || record.length > MaxMessage) {
            error = "the image decoder sent data that is not valid";
            break;
        }
        std::string text(std::size_t(record.length), '\0');
        if (!readExact(text.data(), text.size())) {
            break;
        }
        if (record.kind == Kind::Error) {
            error = text.empty() ? "the image data is corrupt" : text;
            break;
        }
        image.m_warning = text;
    }
    reader.reset();
    const int status = child.finish(!finished);
    if (interrupted) {
        error = "interrupted";
        return std::nullopt;
    }
    if (tooMany && !image.m_frames.empty()) {
        // An animation whose frames do not all fit shows its first frame only.
        image.m_frames.resize(1);
        image.m_delays.assign(1, 0);
        image.m_warning = limits.maxFrames == AfterglowProto::GraphicsMaxFrames
            ? "the animation has more than " + std::to_string(limits.maxFrames)
                + " frames, the most that a terminal keeps, so only its first frame shows"
            : "the frames of the animation do not all fit in the " + humanSize(budget)
                + " that aglow allows for it, so only its first frame shows";
        return image;
    }
    if (!finished || image.m_frames.empty()) {
        if (error.empty()) {
            error = image.m_frames.empty() && finished ? "the file holds no image" : childFailure(status);
        }
        return std::nullopt;
    }
    if (image.m_frames.size() > 1) {
        // Browsers show a GIF delay under 20 ms as 100 ms, as kitty does.
        for (auto& delay : image.m_delays) {
            delay = delay < 20 ? 100 : delay;
        }
    }
    return image;
}

Resizer::Resizer(int width, int height, int newWidth, int newHeight)
    : m_width(width)
    , m_height(height)
    , m_newWidth(std::max(1, newWidth))
    , m_newHeight(std::max(1, newHeight))
{
    if (identity()) {
        return;
    }
    m_across = taps(m_width, m_newWidth);
    m_down = taps(m_height, m_newHeight);
    for (const auto& tap : m_down) {
        m_window = std::max(m_window, tap.weights.size());
    }
}

// The weights of a separable resample along one axis, from @p from to @p to
// pixels. A triangle filter as wide as the scale averages every source pixel
// when shrinking and interpolates when growing.
std::vector<Resizer::Taps> Resizer::taps(int from, int to)
{
    const double scale = double(to) / double(from);
    const double support = scale < 1 ? 1 / scale : 1;
    std::vector<Taps> out(static_cast<std::size_t>(to));
    for (int d = 0; d < to; ++d) {
        const double center = (d + 0.5) / scale - 0.5;
        auto& tap = out[std::size_t(d)];
        tap.first = std::max(0, int(std::floor(center - support)));
        const int last = std::min(from - 1, int(std::ceil(center + support)));
        double total = 0;
        for (int s = tap.first; s <= last; ++s) {
            const double w = std::max(0.0, 1 - std::abs(s - center) / support);
            tap.weights.push_back(float(w));
            total += w;
        }
        if (total <= 0) {
            tap.first = std::clamp(int(std::lround(center)), 0, from - 1);
            tap.weights.assign(1, 1.0f);
            total = 1;
        }
        for (auto& w : tap.weights) {
            w = float(w / total);
        }
    }
    return out;
}

std::string Resizer::operator()(const std::string& rgba) const
{
    if (identity()) {
        return rgba;
    }
    const std::size_t newWidth = std::size_t(m_newWidth);
    const std::size_t rowFloats = newWidth * 4;
    // Premultiplied while filtering, so that transparent pixels lend no
    // colour to their neighbours. Across first, one source row at a time
    // into a window of the rows that the output row being made reads: the
    // taps only move down, so no row is filtered twice.
    std::vector<float> window(m_window * rowFloats);
    std::vector<int> held(m_window, -1);
    auto across = [&](int y) -> const float* {
        const std::size_t slot = std::size_t(y) % m_window;
        float* out = window.data() + slot * rowFloats;
        if (held[slot] == y) {
            return out;
        }
        held[slot] = y;
        const auto* row = reinterpret_cast<const std::uint8_t*>(rgba.data()) + std::size_t(y) * std::size_t(m_width) * 4;
        for (std::size_t x = 0; x < newWidth; ++x) {
            float acc[4] = { 0, 0, 0, 0 };
            const auto& tap = m_across[x];
            for (std::size_t k = 0; k < tap.weights.size(); ++k) {
                const auto* px = row + std::size_t(tap.first + int(k)) * 4;
                const float alpha = px[3] / 255.0f;
                for (int c = 0; c < 3; ++c) {
                    acc[c] += tap.weights[k] * px[c] * alpha;
                }
                acc[3] += tap.weights[k] * px[3];
            }
            std::copy(std::begin(acc), std::end(acc), out + x * 4);
        }
        return out;
    };
    std::string out(newWidth * std::size_t(m_newHeight) * 4, '\0');
    std::vector<const float*> rows;
    for (int y = 0; y < m_newHeight; ++y) {
        const auto& tap = m_down[std::size_t(y)];
        rows.clear();
        for (std::size_t k = 0; k < tap.weights.size(); ++k) {
            rows.push_back(across(tap.first + int(k)));
        }
        for (std::size_t x = 0; x < newWidth; ++x) {
            float acc[4] = { 0, 0, 0, 0 };
            for (std::size_t k = 0; k < tap.weights.size(); ++k) {
                const float* px = rows[k] + x * 4;
                for (int c = 0; c < 4; ++c) {
                    acc[c] += tap.weights[k] * px[c];
                }
            }
            const std::size_t i = (std::size_t(y) * newWidth + x) * 4;
            const float alpha = std::clamp(acc[3], 0.0f, 255.0f);
            for (int c = 0; c < 3; ++c) {
                const float value = alpha > 0 ? acc[c] * 255.0f / alpha : 0.0f;
                out[i + std::size_t(c)] = char(std::uint8_t(std::clamp(std::lround(value), 0L, 255L)));
            }
            out[i + 3] = char(std::uint8_t(std::lround(alpha)));
        }
    }
    return out;
}

std::string resizeFrame(const std::string& rgba, int width, int height, int newWidth, int newHeight)
{
    return Resizer(width, height, newWidth, newHeight)(rgba);
}

void mirrorFrame(std::string& rgba, int width, int height, bool horizontal, bool vertical)
{
    const std::size_t stride = std::size_t(width) * 4;
    if (horizontal) {
        for (int y = 0; y < height; ++y) {
            char* row = rgba.data() + std::size_t(y) * stride;
            for (int a = 0, b = width - 1; a < b; ++a, --b) {
                std::swap_ranges(row + std::size_t(a) * 4, row + std::size_t(a) * 4 + 4, row + std::size_t(b) * 4);
            }
        }
    }
    if (vertical) {
        for (int a = 0, b = height - 1; a < b; ++a, --b) {
            std::swap_ranges(rgba.data() + std::size_t(a) * stride, rgba.data() + std::size_t(a + 1) * stride,
                             rgba.data() + std::size_t(b) * stride);
        }
    }
}

void flattenFrame(std::string& rgba, std::uint8_t red, std::uint8_t green, std::uint8_t blue)
{
    const std::uint8_t colour[3] = { red, green, blue };
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const unsigned alpha = std::uint8_t(rgba[i + 3]);
        for (int c = 0; c < 3; ++c) {
            const unsigned value = std::uint8_t(rgba[i + std::size_t(c)]);
            rgba[i + std::size_t(c)] = char((value * alpha + colour[c] * (255 - alpha) + 127) / 255);
        }
        rgba[i + 3] = char(255);
    }
}

bool frameOpaque(const std::string& rgba)
{
    for (std::size_t i = 3; i < rgba.size(); i += 4) {
        if (std::uint8_t(rgba[i]) != 255) {
            return false;
        }
    }
    return true;
}

std::string dropAlpha(const std::string& rgba)
{
    std::string rgb;
    rgb.reserve(rgba.size() / 4 * 3);
    for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
        rgb.append(rgba, i, 3);
    }
    return rgb;
}

} // namespace Aglow
