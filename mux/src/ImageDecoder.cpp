// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ImageDecoder.h"

#include "StbImage.h"

#include <AfterglowProto/Fd.h>
#include <AfterglowProto/Graphics.h>
#include <AfterglowVt/Graphics.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <new>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

namespace AfterglowMux {

namespace {

// The window takes no more (decodeBoundedImage).
constexpr std::size_t MaxBytes = AfterglowVt::GraphicsStore::DefaultQuota;

struct Header
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

bool fits(std::uint64_t width, std::uint64_t height)
{
    return width > 0 && height > 0 && width <= AfterglowProto::GraphicsMaxSide && height <= AfterglowProto::GraphicsMaxSide
        && width * height * 4 <= MaxBytes;
}

// The signature, then IHDR as the first chunk: Apple's CgBI chunk comes
// before IHDR, and Qt does not read CgBI files either.
bool isPng(std::string_view data)
{
    return data.size() >= 16 && data.starts_with("\x89PNG\r\n\x1a\n") && data.substr(12, 4) == "IHDR";
}

// The formats that the window takes too (imageFormatAllowed), by their
// signatures. WebP is not among them: stb_image does not read it.
bool allowed(std::string_view data, ImageFormats formats)
{
    if (isPng(data)) return true;
    if (formats == ImageFormats::Png) return false;
    return data.starts_with("\xff\xd8\xff") || data.starts_with("GIF87a") || data.starts_with("GIF89a")
        || (data.size() >= 14 && data.starts_with("BM"));
}

// The decoder, killed and reaped however decodeImage() ends.
class Child
{
public:
    explicit Child(pid_t pid)
        : m_pid(pid)
    {
    }
    ~Child()
    {
        if (m_pid > 0) finish(true);
    }
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    /// Its wait status, after SIGKILL when @p kill.
    int finish(bool kill)
    {
        if (kill) ::kill(m_pid, SIGKILL);
        int status = 0;
        while (::waitpid(m_pid, &status, 0) < 0 && errno == EINTR) {
        }
        m_pid = -1;
        return status;
    }

private:
    pid_t m_pid;
};

bool readUntil(int fd, void* data, std::size_t size, std::chrono::steady_clock::time_point deadline)
{
    auto* p = static_cast<char*>(data);
    while (size > 0) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return false;
        pollfd ready{fd, POLLIN, 0};
        const int polled = ::poll(&ready, 1, int(left.count()));
        if (polled < 0 && errno == EINTR) continue;
        if (polled <= 0) return false;
        const ssize_t n = ::read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        size -= std::size_t(n);
    }
    return true;
}

bool writeAll(int fd, const void* data, std::size_t size)
{
    const auto* p = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t n = ::write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        size -= std::size_t(n);
    }
    return true;
}

// The posix_spawn objects, destroyed however the spawn ends.
struct Spawn
{
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    Spawn()
    {
        ::posix_spawn_file_actions_init(&actions);
        ::posix_spawnattr_init(&attributes);
    }
    ~Spawn()
    {
        ::posix_spawnattr_destroy(&attributes);
        ::posix_spawn_file_actions_destroy(&actions);
    }
    Spawn(const Spawn&) = delete;
    Spawn& operator=(const Spawn&) = delete;
};

// The address space of this process, from /proc; 0 when it cannot be read.
std::uint64_t addressSpace()
{
    AfterglowProto::UniqueFd fd(::open("/proc/self/statm", O_RDONLY | O_CLOEXEC));
    if (!fd.valid()) return 0;
    char text[128];
    const ssize_t n = ::read(fd.get(), text, sizeof text - 1);
    if (n <= 0) return 0;
    text[n] = '\0';
    const long page = ::sysconf(_SC_PAGESIZE);
    return std::strtoull(text, nullptr, 10) * std::uint64_t(page > 0 ? page : 4096);
}

void limit(int resource, rlim_t value)
{
    rlimit current{};
    if (::getrlimit(resource, &current) != 0) return;
    rlimit next{std::min(value, current.rlim_max), std::min(value, current.rlim_max)};
    ::setrlimit(resource, &next);
}

} // namespace

bool decodeImage(std::string_view data, ImageFormats formats, std::uint32_t& width, std::uint32_t& height,
                 std::vector<std::uint8_t>& rgba, const char* decoder)
{
    if (data.size() > MaxBytes || !allowed(data, formats)) return false;
    // The image goes in a memory file: the decoder reads it whole before it
    // writes, and a write to a pipe of a decoder that ended would raise SIGPIPE.
    AfterglowProto::UniqueFd input(::memfd_create("afterglow-mux-image", MFD_CLOEXEC));
    if (!input.valid() || !writeAll(input.get(), data.data(), data.size()) || ::lseek(input.get(), 0, SEEK_SET) != 0) return false;
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC) != 0) return false;
    AfterglowProto::UniqueFd reader(fds[0]);
    AfterglowProto::UniqueFd writer(fds[1]);

    pid_t pid = -1;
    {
        Spawn spawn;
        ::posix_spawn_file_actions_adddup2(&spawn.actions, input.get(), STDIN_FILENO);
        ::posix_spawn_file_actions_adddup2(&spawn.actions, writer.get(), STDOUT_FILENO);
        ::posix_spawn_file_actions_addopen(&spawn.actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
        // The daemon blocks the signals of its signalfd and ignores SIGPIPE.
        // Both would reach the decoder through the exec.
        sigset_t none;
        ::sigemptyset(&none);
        sigset_t pipe;
        ::sigemptyset(&pipe);
        ::sigaddset(&pipe, SIGPIPE);
        ::posix_spawnattr_setsigmask(&spawn.attributes, &none);
        ::posix_spawnattr_setsigdefault(&spawn.attributes, &pipe);
        ::posix_spawnattr_setflags(&spawn.attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
        std::string name = "afterglow-mux";
        std::string argument(DecodeImageArgument);
        char* argv[] = {name.data(), argument.data(), nullptr};
        char* envp[] = {nullptr};
        if (::posix_spawn(&pid, decoder, &spawn.actions, &spawn.attributes, argv, envp) != 0) return false;
    }
    Child child(pid);
    writer.reset();
    input.reset();

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(DecodeWaitMs);
    Header header;
    if (!readUntil(reader.get(), &header, sizeof header, deadline) || !fits(header.width, header.height)) return false;
    std::vector<std::uint8_t> pixels(std::size_t(header.width) * header.height * 4);
    if (!readUntil(reader.get(), pixels.data(), pixels.size(), deadline)) return false;
    const int status = child.finish(false);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return false;
    width = header.width;
    height = header.height;
    rgba = std::move(pixels);
    return true;
}

int runImageDecoder()
{
    // Only the standard streams of the daemon reach the decoder, and it ends
    // with the daemon's thread that waits for it.
    ::close_range(3, ~0U, 0);
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() == 1) return 1;
    limit(RLIMIT_CORE, 0);
    // A few seconds to read and to look at the header; the size sets the rest.
    limit(RLIMIT_CPU, 5);
    try {
        std::string data;
        char buffer[65536];
        for (;;) {
            const ssize_t n = ::read(STDIN_FILENO, buffer, sizeof buffer);
            if (n < 0 && errno == EINTR) continue;
            if (n < 0) return 1;
            if (n == 0) break;
            data.append(buffer, std::size_t(n));
            if (data.size() > MaxBytes) return 1;
        }
        int width = 0;
        int height = 0;
        if (Aglow::Stb::probe(data, width, height) != Aglow::Stb::Probe::Image || !fits(std::uint64_t(width), std::uint64_t(height)))
            return 1;
        // As aglow: stb_image needs a few times the decoded image while it
        // works (16-bit PNG, a copy of the compressed data). Past this, its
        // allocations fail, whatever a corrupt file asks for. It decodes at
        // well over 16 MiB a second.
        const std::uint64_t frameBytes = std::uint64_t(width) * std::uint64_t(height) * 4;
        if (const std::uint64_t base = addressSpace())
            limit(RLIMIT_AS, rlim_t(base + 8 * frameBytes + data.size() + (std::uint64_t(64) << 20)));
        limit(RLIMIT_CPU, rlim_t(5 + (frameBytes >> 24)));
        bool written = false;
        std::string error;
        std::string warning;
        // The first frame of an animation, as the window shows it.
        Aglow::Stb::decode(data, [&](const unsigned char* pixels, int w, int h, int) {
            if (fits(std::uint64_t(w), std::uint64_t(h))) {
                const Header header{std::uint32_t(w), std::uint32_t(h)};
                written = writeAll(STDOUT_FILENO, &header, sizeof header)
                    && writeAll(STDOUT_FILENO, pixels, std::size_t(w) * std::size_t(h) * 4);
            }
            return false;
        }, error, warning);
        return written ? 0 : 1;
    } catch (const std::bad_alloc&) {
        return 1;
    }
}

} // namespace AfterglowMux
