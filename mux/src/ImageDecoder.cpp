// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ImageDecoder.h"
#include "ImageWire.h"

#include <AfterglowProto/DecodeRecord.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace AfterglowMux {

using namespace decoding;

namespace {

/// A descriptor of at least 3: one of 0 to 2, in a daemon that started with
/// a standard stream closed, would land on a standard stream of the server.
AfterglowProto::UniqueFd aboveStandard(int fd)
{
    AfterglowProto::UniqueFd held(fd);
    if (fd < 0 || fd > STDERR_FILENO) return held;
    return AfterglowProto::UniqueFd(::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1));
}

} // namespace

// ---------------------------------------------------------- the daemon ---

void ImageDecoder::reportOnStandardError(const std::string& line)
{
    std::fprintf(stderr, "aglow-mux: %s\n", line.c_str());
}

ImageDecoder::ImageDecoder(std::string executable, int waitMs, Reporter report)
    : m_executable(std::move(executable))
    , m_report(std::move(report))
    , m_waitMs(waitMs)
{
    std::lock_guard lock(m_mutex);
    startLocked();
}

ImageDecoder::~ImageDecoder()
{
    std::lock_guard lock(m_mutex);
    stopLocked();
}

void ImageDecoder::reportOnce(std::chrono::steady_clock::time_point& last, const std::string& text)
{
    std::lock_guard lock(m_reportMutex);
    if (reportDue(last) && m_report) m_report(text);
}

bool ImageDecoder::startLocked()
{
    // A start that fails fails again at once: none for a minute, as for
    // servers that end before they answer.
    const auto failed = [this](const char* what, int code) {
        m_failedStart = std::chrono::steady_clock::now();
        reportOnce(m_lastStartReport, std::string("the image decoder does not start (") + what + "): " + std::strerror(code));
        return false;
    };
    int pair[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) != 0) return failed("socketpair", errno);
    AfterglowProto::UniqueFd daemon = aboveStandard(pair[0]);
    AfterglowProto::UniqueFd server = aboveStandard(pair[1]);
    if (!daemon.valid() || !server.valid()) return failed("fcntl", errno);

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (const int code = ::posix_spawn_file_actions_init(&actions); code != 0) return failed("posix_spawn", code);
    if (const int code = ::posix_spawnattr_init(&attributes); code != 0) {
        ::posix_spawn_file_actions_destroy(&actions);
        return failed("posix_spawn", code);
    }
    // The daemon blocks the signals of its signalfd and ignores SIGPIPE.
    // Both would reach the server through the exec, as would a SIGCHLD that
    // a host of this class ignores: the server reaps its processes.
    sigset_t none;
    ::sigemptyset(&none);
    sigset_t defaults;
    ::sigemptyset(&defaults);
    ::sigaddset(&defaults, SIGPIPE);
    ::sigaddset(&defaults, SIGXCPU);
    ::sigaddset(&defaults, SIGCHLD);
    const bool prepared = ::posix_spawn_file_actions_adddup2(&actions, server.get(), STDIN_FILENO) == 0
        && ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) == 0
        && ::posix_spawnattr_setsigmask(&attributes, &none) == 0 && ::posix_spawnattr_setsigdefault(&attributes, &defaults) == 0
        && ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF) == 0;
    std::string name = "aglow-mux";
    std::string argument(DecodeServerArgument);
    std::string parent = std::to_string(::getpid());
    std::string version(DecodeWireVersion);
    char* argv[] = {name.data(), argument.data(), parent.data(), version.data(), nullptr};
    // Only what a sanitizer build reads of the daemon's environment.
    std::vector<std::string> variables;
    for (const char* key : {"ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS"}) {
        if (const char* value = std::getenv(key)) variables.push_back(std::string(key) + '=' + value);
    }
    std::vector<char*> envp;
    for (std::string& variable : variables) envp.push_back(variable.data());
    envp.push_back(nullptr);
    pid_t pid = -1;
    const int code = prepared ? ::posix_spawn(&pid, m_executable.c_str(), &actions, &attributes, argv, envp.data()) : EINVAL;
    ::posix_spawnattr_destroy(&attributes);
    ::posix_spawn_file_actions_destroy(&actions);
    if (code != 0) return failed("posix_spawn", code);
    m_server = pid;
    m_socket = std::move(daemon);
    ++m_unanswered;
    return true;
}

void ImageDecoder::stopLocked()
{
    // The server ends with its socket, and its decoding processes with it.
    m_socket.reset();
    m_owedAnswers = 0;
    if (m_server <= 0) return;
    ::kill(m_server, SIGKILL);
    int status = 0;
    while (::waitpid(m_server, &status, 0) < 0 && errno == EINTR) {
    }
    m_server = -1;
    // A server that ended by itself said why. Another version reads the
    // frames otherwise: none starts again. One that does not take the
    // arguments of this version is another version too.
    const bool other = WIFEXITED(status) && (WEXITSTATUS(status) == OtherVersionStatus || WEXITSTATUS(status) == UsageStatus);
    if (other && !m_otherVersion) {
        m_otherVersion = true;
        reportOnce(m_lastStartReport, "the image decoder " + m_executable + " is of another version: images do not show until this program starts again");
    }
}

namespace {

/// Waits until @p socket is readable, by @p deadline. With @p cancel: also
/// until it is. 1 for the socket, 2 for the cancel, 0 for the deadline.
int waitForAnswer(int socket, std::chrono::steady_clock::time_point deadline, int cancel)
{
    for (;;) {
        // Once even past the deadline: an answer that came is no stuck server.
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        std::array<pollfd, 2> ready{{{socket, POLLIN, 0}, {cancel, POLLIN, 0}}};
        const int polled = ::poll(ready.data(), cancel >= 0 ? 2 : 1, int(std::max<long long>(left.count(), 0)));
        if (polled < 0 && errno == EINTR) continue;
        if (polled <= 0) return 0;
        if (cancel >= 0 && ready[1].revents) return 2;
        return 1;
    }
}

bool readable(int fd)
{
    pollfd ready{fd, POLLIN, 0};
    return fd >= 0 && ::poll(&ready, 1, 0) > 0;
}

} // namespace

void ImageDecoder::serverEndedLocked()
{
    // Reaped now: a server that crashed stays no zombie until the next decode.
    stopLocked();
    // A server that ended before it answered once can have been killed (the
    // OOM killer, a user) after it ran for hours: the next one starts. When
    // that one ends before it answers too, no server starts: a library of
    // the daemon is gone or changed, or it cannot run. Another start would
    // end the same way, at each decode: none for a minute.
    if (m_unanswered >= 2) {
        m_failedStart = std::chrono::steady_clock::now();
        reportOnce(m_lastStartReport, "the image decoder ends before it answers: images do not show for a minute");
    }
}

void ImageDecoder::stuckLocked()
{
    stopLocked();
    // As one that ended before it answered: two in a row, and none starts for
    // a minute.
    if (m_unanswered >= 2) {
        m_failedStart = std::chrono::steady_clock::now();
        reportOnce(m_lastStartReport, "the image decoder does not answer: images do not show for a minute");
    } else {
        reportOnce(m_lastReport, "the image decoder did not answer in " + std::to_string(m_waitMs) + " ms: it starts again");
    }
}

void ImageDecoder::oweLocked(std::chrono::steady_clock::time_point due)
{
    if (m_owedAnswers++ == 0) m_answerDue = due;
}

AfterglowProto::UniqueFd ImageDecoder::answerLocked()
{
    std::uint8_t ok = 0;
    iovec in{&ok, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
    msghdr reply{};
    reply.msg_iov = &in;
    reply.msg_iovlen = 1;
    reply.msg_control = control;
    reply.msg_controllen = sizeof control;
    ssize_t n = -1;
    do {
        n = ::recvmsg(m_socket.get(), &reply, MSG_CMSG_CLOEXEC);
    } while (n < 0 && errno == EINTR);
    if (n != 1) {
        serverEndedLocked();
        return {};
    }
    m_unanswered = 0;
    AfterglowProto::UniqueFd process;
    for (cmsghdr* r = CMSG_FIRSTHDR(&reply); r; r = CMSG_NXTHDR(&reply, r)) {
        if (r->cmsg_level != SOL_SOCKET || r->cmsg_type != SCM_RIGHTS || r->cmsg_len < CMSG_LEN(sizeof(int))) continue;
        int fd = -1;
        std::memcpy(&fd, CMSG_DATA(r), sizeof fd);
        process.reset(fd);
    }
    return process;
}

AfterglowProto::UniqueFd ImageDecoder::requestLocked(ImageFormats formats, int input, int output,
                                                     std::chrono::steady_clock::time_point deadline, int cancel,
                                                     bool& cancelled)
{
    if (!m_socket.valid()) return {};
    // A request that waited for the lock past its deadline, or for a pane
    // that closed meanwhile, goes no further: the server is fine.
    if (std::chrono::steady_clock::now() >= deadline) return {};
    if (readable(cancel)) {
        cancelled = true;
        return {};
    }
    // The answers that requests of closed panes, and of callers whose own
    // deadline came first, left: their processes have no one to read them.
    // The server answers as soon as it forked: one that owes an answer
    // m_waitMs after its request is stuck, and the next decode starts
    // another. A caller whose own deadline comes first leaves it be.
    while (m_owedAnswers > 0) {
        switch (waitForAnswer(m_socket.get(), std::min(deadline, m_answerDue), cancel)) {
        case 0:
            if (std::chrono::steady_clock::now() >= m_answerDue) stuckLocked();
            return {};
        case 2:
            cancelled = true;
            return {};
        default:
            break;
        }
        if (const AfterglowProto::UniqueFd stale = answerLocked(); stale.valid()) pidfdKill(stale.get());
        if (!m_socket.valid()) return {};
        // The server answers in order: the next one follows.
        if (--m_owedAnswers > 0) m_answerDue = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_waitMs);
    }
    // A drain that ended at the deadline leaves the server no time to
    // answer: it is fine, and the request goes no further.
    if (std::chrono::steady_clock::now() >= deadline) return {};
    std::uint8_t byte = std::uint8_t(formats);
    iovec io{&byte, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(2 * sizeof(int))] = {};
    msghdr message{};
    message.msg_iov = &io;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof control;
    cmsghdr* c = CMSG_FIRSTHDR(&message);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(2 * sizeof(int));
    const std::array<int, 2> fds{input, output};
    std::memcpy(CMSG_DATA(c), fds.data(), sizeof fds);
    ssize_t n = -1;
    do {
        n = ::sendmsg(m_socket.get(), &message, MSG_NOSIGNAL);
    } while (n < 0 && errno == EINTR);
    if (n != 1) {
        serverEndedLocked();
        return {};
    }
    const auto due = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_waitMs);
    // The caller's deadline began before the request, m_waitMs long: it
    // comes first. A server that is stuck shows at the next request, which
    // waits for the answer owed until m_answerDue.
    switch (waitForAnswer(m_socket.get(), deadline, cancel)) {
    case 0:
        // The answer comes to the next request, which ends its process. A
        // healthy server goes on, and so do the decodes of other panes.
        oweLocked(due);
        return {};
    case 2:
        // The pane closes: likewise.
        oweLocked(due);
        cancelled = true;
        return {};
    default:
        return answerLocked();
    }
}

bool ImageDecoder::decode(std::string_view data, ImageFormats formats, std::uint32_t& width, std::uint32_t& height,
                          std::vector<std::uint8_t>& rgba, int cancel)
{
    std::vector<DecodedFrame> frames;
    // One frame: the process of Png writes no more.
    if (formats == ImageFormats::CommonFrames || !run(data, formats, MaxBytes, frames, cancel)) return false;
    width = frames.front().width;
    height = frames.front().height;
    rgba = std::move(frames.front().rgba);
    return true;
}

bool ImageDecoder::decodeFrames(std::string_view data, std::size_t maxBytes, std::vector<DecodedFrame>& frames, int cancel)
{
    return run(data, ImageFormats::CommonFrames, std::min(maxBytes, MaxBytes), frames, cancel);
}

bool ImageDecoder::run(std::string_view data, ImageFormats formats, std::size_t maxBytes, std::vector<DecodedFrame>& frames,
                       int cancel)
{
    const int waitMs = m_waitMs;
    // A pane that closes decodes nothing more: its feed may hold images.
    if (data.empty() || data.size() > MaxBytes || !allowed(data, formats) || readable(cancel)) return false;
    // The image goes in a memory file, which the decoding process maps.
    AfterglowProto::UniqueFd input(::memfd_create("afterglow-image", MFD_CLOEXEC));
    if (!input.valid() || !AfterglowProto::writeAll(input.get(), data)) return false;
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC) != 0) return false;
    AfterglowProto::UniqueFd reader(fds[0]);
    AfterglowProto::UniqueFd writer(fds[1]);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    AfterglowProto::UniqueFd process;
    {
        // The server takes one request at a time: a decode waits for the
        // others, and still ends at its deadline and when its pane closes.
        std::unique_lock lock(m_mutex, std::defer_lock);
        while (!lock.try_lock_for(std::chrono::milliseconds(20))) {
            if (std::chrono::steady_clock::now() >= deadline || readable(cancel)) return false;
        }
        bool cancelled = false;
        for (int attempt = 0; attempt < 2 && !process.valid() && !cancelled; ++attempt) {
            if (!m_socket.valid()) {
                // The server ended: a crash, the OOM killer, one that got
                // stuck, or one that did not start. The new one runs the
                // executable again, with the libraries of now. One that did
                // not start waits a minute before it is tried again, and one
                // of another version never is.
                const auto now = std::chrono::steady_clock::now();
                if (m_otherVersion || now >= deadline || (m_failedStart && now - *m_failedStart < std::chrono::minutes(1))) return false;
                stopLocked();
                // A start that fails reports why itself: the line of a
                // restart would hold that back for a minute.
                if (!startLocked()) return false;
                // Not with the reports of failed starts, which this line
                // would hold back when the new server fails too.
                reportOnce(m_lastReport, "the image decoder is not running: it starts again");
            }
            process = requestLocked(formats, input.get(), writer.get(), deadline, cancel, cancelled);
            // A server that answers with no process could not fork: no
            // memory, or no process left under the user's limit. A request
            // past its deadline, or of a pane that closes, ends too.
            if (!process.valid() && m_socket.valid()) return false;
        }
        if (!process.valid()) return false;
    }
    // Only the decoding process holds them now: the pipe ends when it does.
    writer.reset();
    input.reset();
    // It has nothing to do once its pixels came, or when they do not: it
    // ends however this ends, and the server reaps it.
    struct Kill
    {
        int pidfd;
        ~Kill() { pidfdKill(pidfd); }
    } kill{process.get()};

    using AfterglowProto::DecodeRead;
    AfterglowProto::DecodeWait wait;
    wait.deadline = deadline;
    wait.cancel = cancel;
    // The frames that came before still show.
    const auto late = [this, waitMs, &frames](DecodeRead read) {
        if (read != DecodeRead::Late) return;
        reportOnce(m_lastReport, "an image decoder took more than " + std::to_string(waitMs) + " ms: "
                                     + (frames.empty() ? std::string("the image does not show")
                                                       : "the frames after the first " + std::to_string(frames.size()) + " do not show"));
    };
    // Frames until the process closes its end: a frame cut short, by its
    // end or the deadline, goes, and those before it stay. Each frame has
    // the size of the first, and they fit in maxBytes together. Its pixels
    // come through a small buffer: a frame of hundreds of megabytes is not
    // filled with zeros first, on the thread that waits for it.
    std::vector<std::uint8_t> chunk(std::size_t(1) << 18);
    std::size_t total = 0;
    while (frames.size() < AfterglowProto::GraphicsMaxFrames) {
        AfterglowProto::DecodeRecord record;
        DecodeRead read = AfterglowProto::readDecodeRecord(reader.get(), record, wait);
        late(read);
        if (read != DecodeRead::Done || record.kind != AfterglowProto::DecodeRecord::Kind::Frame || !fits(record.width, record.height))
            break;
        if (!frames.empty() && (record.width != frames.front().width || record.height != frames.front().height)) break;
        const std::size_t size = std::size_t(record.width) * record.height * 4;
        if (record.length != size || size > maxBytes - total) break;
        DecodedFrame frame;
        try {
            frame.rgba.reserve(size);
        } catch (const std::bad_alloc&) {
            break;
        }
        while (read == DecodeRead::Done && frame.rgba.size() < size) {
            const std::size_t part = std::min(chunk.size(), size - frame.rgba.size());
            read = AfterglowProto::readFromDecoder(reader.get(), chunk.data(), part, wait);
            if (read == DecodeRead::Done) frame.rgba.insert(frame.rgba.end(), chunk.begin(), chunk.begin() + std::ptrdiff_t(part));
        }
        late(read);
        if (read != DecodeRead::Done) break;
        frame.width = record.width;
        frame.height = record.height;
        frame.delayMs = std::uint32_t(std::max(0, record.delay));
        total += size;
        frames.push_back(std::move(frame));
    }
    if (frames.size() == 1) frames.front().delayMs = 0;
    return !frames.empty();
}

} // namespace AfterglowMux
