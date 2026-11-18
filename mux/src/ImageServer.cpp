// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ImageServer.h"

#include "ImageDecoder.h"
#include "ImageWire.h"

#include <AfterglowImage/Decode.h>
#include <AfterglowProto/DecodeFilter.h>
#include <AfterglowProto/DecodeProcess.h>
#include <AfterglowProto/DecodeRecord.h>
#include <AfterglowProto/Limits.h>
#include <AfterglowPty/CloseFrom.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// A sanitizer build: what a sanitizer reports in a decoding process goes to
// the daemon's standard error. Otherwise nothing of a decoding process does.
// GCC has no macro for UBSan alone: mux/CMakeLists.txt defines this from the
// flags.
#if defined(AFTERGLOW_MUX_SANITIZED)
#elif defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define AFTERGLOW_MUX_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer)             \
    || __has_feature(undefined_behavior_sanitizer)
#define AFTERGLOW_MUX_SANITIZED 1
#endif
#endif

namespace AfterglowMux {

using namespace decoding;

namespace {

// ---------------------------------------------------------- the process ---

/// The exit status of a decoding process that cannot limit its system calls
/// (AfterglowProto::allowOnlyDecoding: a kernel without seccomp filters, or
/// a processor whose system calls the filter does not know): it decodes
/// nothing, and the server says so.
constexpr int NoFilterStatus = 3;
/// The exit status of a decoding process that did not decode its image: a
/// file that is no image of its formats, too large, corrupt, or that needs
/// more memory than its limit. The daemon shows no image, and that is all.
/// Any other status but 0 is a failure that the server reports: also the 1
/// with which a sanitizer ends a process.
constexpr int RefusedStatus = 4;

/// The server's lines on standard error: the journal of the daemon, or the
/// log of the window that started it.
void report(std::chrono::steady_clock::time_point& last, const std::string& text)
{
    if (reportDue(last))
        std::fprintf(stderr, "afterglow-image: %s\n", text.c_str());
}

/// A decoding process: reads the image from @p input, a memory file, and
/// writes a Frame record (AfterglowProto/DecodeRecord.h) with the pixels of
/// its first frame to @p output, or of
/// each frame for ImageFormats::CommonFrames, at most GraphicsMaxFrames and
/// MaxBytes of pixels together. Of an APNG for ImageFormats::Png, its
/// default image, as kitty shows it. It ends with the server @p server, and
/// exits at once: no handler of the server runs in it.
[[noreturn]] void decodeProcess(int input, int output, ImageFormats formats, pid_t server)
{
    // Standard error is the daemon's journal, or a terminal: code that a
    // corrupt file took over writes neither. A sanitizer build keeps it, for
    // the reports.
#ifdef AFTERGLOW_MUX_SANITIZED
    constexpr bool keepStandardError = true;
#else
    constexpr bool keepStandardError = false;
#endif
    if (!AfterglowProto::startDecodingProcess(server, input, output, keepStandardError))
        ::_exit(RefusedStatus);

    struct stat file{};
    if (::fstat(STDIN_FILENO, &file) != 0 || file.st_size <= 0 || std::uint64_t(file.st_size) > MaxBytes)
        ::_exit(RefusedStatus);
    void* mapped = ::mmap(nullptr, std::size_t(file.st_size), PROT_READ, MAP_PRIVATE, STDIN_FILENO, 0);
    if (mapped == MAP_FAILED)
        ::_exit(RefusedStatus);
    const std::string_view data(static_cast<const char*>(mapped), std::size_t(file.st_size));
    // The daemon checked the signature: a decoding process checks again what
    // it reads.
    if (!allowed(data, formats))
        ::_exit(RefusedStatus);
    // As aglow. The mapped file is in the address space already.
    const std::uint64_t base = AfterglowProto::addressSpaceOr(std::uint64_t(file.st_size));
    const auto memoryFor = [base](std::uint64_t frameBytes) {
        return rlim_t(base + AfterglowProto::decodeMemory(frameBytes));
    };
    const auto secondsFor = [](std::uint64_t frameBytes) {
        return AfterglowProto::decodeSeconds(frameBytes);
    };
    // The limits of the largest image, and the filter, before anything of
    // the file is parsed: the header too comes from the program.
    AfterglowProto::limit(RLIMIT_AS, memoryFor(MaxBytes), memoryFor(MaxBytes));
    AfterglowProto::limit(RLIMIT_CPU, secondsFor(MaxBytes), secondsFor(MaxBytes) + 1);
    // Standard error is /dev/null here, but in a sanitizer build.
    if (!AfterglowProto::allowOnlyDecoding())
        ::_exit(NoFilterStatus);
    int width = 0;
    int height = 0;
    if (AfterglowImage::probe(data, width, height) != AfterglowImage::Probe::Image
        || !fits(std::uint64_t(width), std::uint64_t(height)))
        ::_exit(RefusedStatus);
    // Then those of this image.
    const std::uint64_t frameBytes = std::uint64_t(width) * std::uint64_t(height) * 4;
    AfterglowProto::limit(RLIMIT_AS, memoryFor(frameBytes), memoryFor(frameBytes));
    AfterglowProto::limit(RLIMIT_CPU, secondsFor(frameBytes), secondsFor(frameBytes) + 1);
    // The pixels of one frame at a time: an animation needs no more memory,
    // only more CPU time, so its later frames can run into the CPU limit.
    // The daemon keeps the frames that came.
    bool written = false;
    std::size_t frames = 0;
    std::uint64_t bytes = 0;
    try {
        std::string error;
        std::string warning;
        const AfterglowImage::PngFrames png = formats == ImageFormats::Png ? AfterglowImage::PngFrames::DefaultImage
                                                                           : AfterglowImage::PngFrames::Animation;
        AfterglowImage::decode(
            data,
            [&](const unsigned char* pixels, int w, int h, int delayMs) {
                const std::uint64_t size = std::uint64_t(w) * std::uint64_t(h) * 4;
                if (!fits(std::uint64_t(w), std::uint64_t(h)) || bytes + size > MaxBytes)
                    return false;
                if (!AfterglowProto::writeDecodeFrame(STDOUT_FILENO, std::uint32_t(w), std::uint32_t(h),
                                                      std::max(0, delayMs), pixels))
                    return false;
                written = true;
                bytes += size;
                return formats == ImageFormats::CommonFrames && ++frames < AfterglowProto::GraphicsMaxFrames;
            },
            error, warning, png);
    } catch (const std::bad_alloc&) {
    }
    ::_exit(written ? 0 : RefusedStatus);
}

/// Receives a request: the formats, and the input and the output of a
/// decoding process. False when the daemon closed its end.
bool receiveRequest(int socket, ImageFormats& formats, AfterglowProto::UniqueFd& input,
                    AfterglowProto::UniqueFd& output)
{
    std::uint8_t byte = 0;
    iovec io{&byte, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(2 * sizeof(int))] = {};
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof control;
    ssize_t n = -1;
    do {
        n = ::recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
    } while (n < 0 && errno == EINTR);
    if (n <= 0)
        return false;
    for (cmsghdr* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS)
            continue;
        const std::size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        std::array<int, 2> fds{-1, -1};
        std::memcpy(fds.data(), CMSG_DATA(c), std::min(count, fds.size()) * sizeof(int));
        input.reset(fds[0]);
        output.reset(fds[1]);
    }
    if (byte == std::uint8_t(ImageFormats::Png) || byte == std::uint8_t(ImageFormats::CommonFrames)) {
        formats = ImageFormats(byte);
    } else {
        // No formats of this version: the caller answers with no process.
        input.reset();
        output.reset();
    }
    return true;
}

/// Answers a request with the pidfd of its process, or with none.
void answer(int socket, int pidfd)
{
    std::uint8_t byte = pidfd >= 0 ? 1 : 0;
    iovec io{&byte, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    if (pidfd >= 0) {
        message.msg_control = control;
        message.msg_controllen = sizeof control;
        cmsghdr* c = CMSG_FIRSTHDR(&message);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(c), &pidfd, sizeof(int));
    }
    while (::sendmsg(socket, &message, MSG_NOSIGNAL) < 0 && errno == EINTR) { }
}

/// The decoding processes that ended. One that a signal other than the
/// daemon's SIGKILL ended (a crash, the CPU limit), and one that exited with
/// a status that it does not give itself (a sanitizer's report), go to the
/// journal.
void reap(std::chrono::steady_clock::time_point& lastReport)
{
    int status = 0;
    while (::waitpid(-1, &status, WNOHANG) > 0) {
        // SIGKILL is the daemon's own end of a process, and SIGPIPE that of
        // one whose pane closed before it wrote.
        // The frames of an animation that came before the end still show.
        if (WIFSIGNALED(status) && WTERMSIG(status) == SIGXCPU)
            report(lastReport,
                   "an image decoder ran out of CPU time: the image, or the rest of its frames, does not show");
        else if (WIFSIGNALED(status) && WTERMSIG(status) != SIGKILL && WTERMSIG(status) != SIGPIPE)
            report(lastReport,
                   "an image decoder ended with signal " + std::to_string(WTERMSIG(status))
                       + ": the image, or the rest of its frames, does not show");
        if (!WIFEXITED(status))
            continue;
        const int code = WEXITSTATUS(status);
        if (code == NoFilterStatus)
            report(lastReport,
                   "the image decoder cannot limit its system calls (no seccomp, or a processor that it does not "
                   "know): images do not show");
        else if (code != 0 && code != RefusedStatus)
            report(lastReport,
                   "an image decoder failed with status " + std::to_string(code)
                       + ": the image, or the rest of its frames, does not show");
    }
}

} // namespace

// ----------------------------------------------------------- the server ---

int runImageServer(std::string_view parent, std::string_view version)
{
    // Its own name in ps and the journal, which /proc/self/exe would make
    // "exe". The decoding processes take it too. It ends when the client's
    // end of its socket closes, however the client ends: no PDEATHSIG, which
    // follows the thread that started it, and a restart runs on a pane's.
    ::prctl(PR_SET_NAME, "afterglow-image");
    if (std::to_string(::getppid()) != parent)
        return 1;
    // A client of another version would read the frames wrong: it reports
    // the status instead.
    if (version != DecodeWireVersion)
        return OtherVersionStatus;
    // A daemon that started without standard error gives the server none: a
    // descriptor that it receives would land there.
    if (::fcntl(STDERR_FILENO, F_GETFD) < 0 && ::open("/dev/null", O_WRONLY | O_CLOEXEC) != STDERR_FILENO)
        return 1;
    // Standard input is the socket of the daemon, standard error the
    // daemon's. Nothing else of the daemon reaches the server.
    AfterglowPty::closeFrom(STDERR_FILENO + 1);
    sigset_t children;
    ::sigemptyset(&children);
    ::sigaddset(&children, SIGCHLD);
    ::sigprocmask(SIG_BLOCK, &children, nullptr);
    AfterglowProto::UniqueFd ended(::signalfd(-1, &children, SFD_NONBLOCK | SFD_CLOEXEC));
    if (!ended.valid())
        return 1;
    const pid_t self = ::getpid();
    std::chrono::steady_clock::time_point lastReport;
    for (;;) {
        std::array<pollfd, 2> ready{{{STDIN_FILENO, POLLIN, 0}, {ended.get(), POLLIN, 0}}};
        if (::poll(ready.data(), ready.size(), -1) < 0) {
            if (errno == EINTR)
                continue;
            return 1;
        }
        if (ready[1].revents) {
            signalfd_siginfo info{};
            while (::read(ended.get(), &info, sizeof info) == ssize_t(sizeof info)) { }
            reap(lastReport);
        }
        if (!ready[0].revents)
            continue;
        ImageFormats formats = ImageFormats::Png;
        AfterglowProto::UniqueFd input;
        AfterglowProto::UniqueFd output;
        if (!receiveRequest(STDIN_FILENO, formats, input, output))
            return 0;
        if (!input.valid() || !output.valid()) {
            answer(STDIN_FILENO, -1);
            continue;
        }
        const pid_t pid = ::fork();
        if (pid == 0)
            decodeProcess(input.get(), output.get(), formats, self);
        // The process is not reaped before reap() runs: its pidfd names it.
        AfterglowProto::UniqueFd process(pid > 0 ? pidfdOpen(pid) : -1);
        if (pid < 0) {
            report(lastReport,
                   std::string("the image decoder cannot start a process: ") + std::strerror(errno)
                       + ": the image does not show");
        } else if (!process.valid()) {
            // A kernel older than Linux 5.3 has no pidfd_open().
            report(lastReport,
                   std::string("the image decoder cannot watch its process (pidfd_open: ") + std::strerror(errno)
                       + "): images do not show");
            ::kill(pid, SIGKILL);
        }
        answer(STDIN_FILENO, process.get());
    }
}

} // namespace AfterglowMux
