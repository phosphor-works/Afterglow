// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ImageDecoder.h"

#include "StbImage.h"

#include <AfterglowProto/Limits.h>

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
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace AfterglowMux {

namespace {

static_assert(Aglow::Stb::MaxSide == int(AfterglowProto::GraphicsMaxSide), "stb_image and the store bound a side alike");

constexpr std::size_t MaxBytes = ImageDecoder::MaxBytes;

/// What a decoding process writes before the pixels.
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

// The formats that the window takes too (imageFormatAllowed in
// afterglow/src/ImageDecode.cpp), by their signatures. stb_image reads more
// (PNM, TGA, PSD, HDR, PIC): only these reach it. WebP is not among them:
// stb_image does not read it.
bool allowed(std::string_view data, ImageFormats formats)
{
    if (isPng(data)) return true;
    if (formats == ImageFormats::Png) return false;
    return data.starts_with("\xff\xd8\xff") || data.starts_with("GIF87a") || data.starts_with("GIF89a")
        || (data.size() >= 14 && data.starts_with("BM"));
}

// Through syscall(2): <sys/pidfd.h> of some glibc versions declares them
// without C linkage.
int pidfdOpen(pid_t pid)
{
    return int(::syscall(SYS_pidfd_open, pid, 0));
}

void pidfdKill(int pidfd)
{
    ::syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0);
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

enum class Read : std::uint8_t {
    Done,
    Ended, ///< the decoding process closed its end, or the read failed
    Late, ///< the deadline passed
    Cancelled,
};

Read readUntil(int fd, void* data, std::size_t size, std::chrono::steady_clock::time_point deadline, int cancel)
{
    auto* p = static_cast<char*>(data);
    while (size > 0) {
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return Read::Late;
        std::array<pollfd, 2> ready{{{fd, POLLIN, 0}, {cancel, POLLIN, 0}}};
        const int polled = ::poll(ready.data(), cancel >= 0 ? 2 : 1, int(left.count()));
        if (polled < 0 && errno == EINTR) continue;
        if (polled < 0) return Read::Ended;
        if (polled == 0) return Read::Late;
        if (cancel >= 0 && ready[1].revents) return Read::Cancelled;
        const ssize_t n = ::read(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return Read::Ended;
        p += n;
        size -= std::size_t(n);
    }
    return Read::Done;
}

/// A descriptor of at least 3: one of 0 to 2, in a daemon that started with
/// a standard stream closed, would land on a standard stream of the server.
AfterglowProto::UniqueFd aboveStandard(int fd)
{
    AfterglowProto::UniqueFd held(fd);
    if (fd < 0 || fd > STDERR_FILENO) return held;
    return AfterglowProto::UniqueFd(::fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1));
}

/// One line on standard error, the journal under systemd, at most once a
/// minute for each @p last: a program that sends broken images in a loop
/// fills no log.
void report(std::chrono::steady_clock::time_point& last, const std::string& text)
{
    const auto now = std::chrono::steady_clock::now();
    if (last != std::chrono::steady_clock::time_point{} && now - last < std::chrono::minutes(1)) return;
    last = now;
    std::fprintf(stderr, "afterglow-mux: %s\n", text.c_str());
}

// ---------------------------------------------------------- the process ---

#if defined(__x86_64__)
constexpr std::uint32_t NativeArch = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
constexpr std::uint32_t NativeArch = AUDIT_ARCH_AARCH64;
#else
constexpr std::uint32_t NativeArch = 0;
#endif

/// The exit status of a decoding process that cannot limit its system calls
/// (a kernel without seccomp filters): it decodes nothing, and the server
/// says so.
constexpr int NoFilterStatus = 3;

/// Past this, the decoding process makes only the system calls that
/// decoding needs: memory, a read of its file, a write of its pixels or of
/// a sanitizer's report, its end. Any other fails with ENOSYS, so that code
/// that a corrupt file took over can open, run or connect to nothing, and
/// read no terminal that standard error may be. A system call of another
/// architecture kills it. False when the filter cannot be set.
bool allowOnlyDecoding()
{
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) return false;
    if constexpr (NativeArch == 0) {
        return true;
    } else {
        constexpr std::uint32_t Allow = SECCOMP_RET_ALLOW;
        constexpr std::uint32_t Refuse = SECCOMP_RET_ERRNO | (ENOSYS & SECCOMP_RET_DATA);
        // The low word of the first argument: a descriptor (little endian,
        // as both architectures here are).
        constexpr std::uint32_t FirstArgument = offsetof(seccomp_data, args);
        std::vector<sock_filter> filter;
        filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)));
        filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, NativeArch, 1, 0));
        filter.push_back(BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS));
        filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)));
        // read() of standard input only.
        filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_read, 0, 4));
        filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, FirstArgument));
        filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, STDIN_FILENO, 0, 1));
        filter.push_back(BPF_STMT(BPF_RET | BPF_K, Allow));
        filter.push_back(BPF_STMT(BPF_RET | BPF_K, Refuse));
        // write() of standard output and standard error only.
        filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_write, 0, 5));
        filter.push_back(BPF_STMT(BPF_LD | BPF_W | BPF_ABS, FirstArgument));
        filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, STDOUT_FILENO, 1, 0));
        filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, STDERR_FILENO, 0, 1));
        filter.push_back(BPF_STMT(BPF_RET | BPF_K, Allow));
        filter.push_back(BPF_STMT(BPF_RET | BPF_K, Refuse));
        for (const long call : {SYS_close, SYS_mmap, SYS_munmap, SYS_mremap, SYS_mprotect, SYS_madvise, SYS_brk, SYS_futex,
                                SYS_getrandom, SYS_clock_gettime, SYS_rt_sigreturn, SYS_exit, SYS_exit_group}) {
            filter.push_back(BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, std::uint32_t(call), 0, 1));
            filter.push_back(BPF_STMT(BPF_RET | BPF_K, Allow));
        }
        filter.push_back(BPF_STMT(BPF_RET | BPF_K, Refuse));
        const sock_fprog program{static_cast<unsigned short>(filter.size()), filter.data()};
        return ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
    }
}

/// A decoding process: reads the image from @p input, a memory file, and
/// writes the header and the pixels of its first frame to @p output. It ends
/// with the server @p server, and exits at once: no handler of the server
/// runs in it.
[[noreturn]] void decodeProcess(int input, int output, ImageFormats formats, pid_t server)
{
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() != server) ::_exit(1);
    // The server blocks SIGCHLD for its signalfd: the limits' signals must
    // arrive here.
    sigset_t none;
    ::sigemptyset(&none);
    ::sigprocmask(SIG_SETMASK, &none, nullptr);
    if (::dup2(input, STDIN_FILENO) < 0 || ::dup2(output, STDOUT_FILENO) < 0) ::_exit(1);
    // Standard error stays the daemon's: what a sanitizer reports goes there.
    ::close_range(STDERR_FILENO + 1, ~0U, 0);
    // The kernel's OOM killer takes this process before the daemon. Before
    // PR_SET_DUMPABLE, which gives /proc/self to root.
    if (const int adjust = ::open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC); adjust >= 0) {
        (void)!::write(adjust, "1000", 4);
        ::close(adjust);
    }
    // No core file, also where systemd-coredump takes cores through a pipe,
    // which RLIMIT_CORE does not stop.
    AfterglowProto::limit(RLIMIT_CORE, 0, 0);
    ::prctl(PR_SET_DUMPABLE, 0);

    struct stat file{};
    if (::fstat(STDIN_FILENO, &file) != 0 || file.st_size <= 0 || std::uint64_t(file.st_size) > MaxBytes) ::_exit(1);
    void* mapped = ::mmap(nullptr, std::size_t(file.st_size), PROT_READ, MAP_PRIVATE, STDIN_FILENO, 0);
    if (mapped == MAP_FAILED) ::_exit(1);
    const std::string_view data(static_cast<const char*>(mapped), std::size_t(file.st_size));
    // The daemon checked the signature: a decoding process checks again what
    // it reads.
    if (!allowed(data, formats)) ::_exit(1);
    int width = 0;
    int height = 0;
    if (Aglow::Stb::probe(data, width, height) != Aglow::Stb::Probe::Image || !fits(std::uint64_t(width), std::uint64_t(height)))
        ::_exit(1);
    // As aglow: stb_image needs a few times the decoded image while it works
    // (16-bit PNG, a copy of the compressed data). Past this, its
    // allocations fail, whatever a corrupt file asks for. The mapped file is
    // in the address space already. Without /proc, the server's start, which
    // is far less, stands for it. It decodes at well over 16 MiB a second.
    const std::uint64_t frameBytes = std::uint64_t(width) * std::uint64_t(height) * 4;
    std::uint64_t base = AfterglowProto::addressSpace();
    if (base == 0) base = (std::uint64_t(256) << 20) + std::uint64_t(file.st_size);
    const auto memory = rlim_t(base + 8 * frameBytes + (std::uint64_t(64) << 20));
    AfterglowProto::limit(RLIMIT_AS, memory, memory);
    const auto seconds = rlim_t(5 + (frameBytes >> 24));
    AfterglowProto::limit(RLIMIT_CPU, seconds, seconds + 1);
    if (!allowOnlyDecoding()) ::_exit(NoFilterStatus);
    bool written = false;
    try {
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
    } catch (const std::bad_alloc&) {
        written = false;
    }
    ::_exit(written ? 0 : 1);
}

/// Receives a request: the formats, and the input and the output of a
/// decoding process. False when the daemon closed its end.
bool receiveRequest(int socket, ImageFormats& formats, AfterglowProto::UniqueFd& input, AfterglowProto::UniqueFd& output)
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
    if (n <= 0) return false;
    for (cmsghdr* c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        const std::size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        std::array<int, 2> fds{-1, -1};
        std::memcpy(fds.data(), CMSG_DATA(c), std::min(count, fds.size()) * sizeof(int));
        input.reset(fds[0]);
        output.reset(fds[1]);
    }
    formats = byte == std::uint8_t(ImageFormats::Png) ? ImageFormats::Png : ImageFormats::Common;
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
    while (::sendmsg(socket, &message, MSG_NOSIGNAL) < 0 && errno == EINTR) {
    }
}

/// The decoding processes that ended. One that a signal other than the
/// daemon's SIGKILL ended (a crash, the CPU limit) goes to the journal.
void reap(std::chrono::steady_clock::time_point& lastReport)
{
    int status = 0;
    while (::waitpid(-1, &status, WNOHANG) > 0) {
        if (WIFSIGNALED(status) && WTERMSIG(status) != SIGKILL)
            report(lastReport, "an image decoder ended with signal " + std::to_string(WTERMSIG(status)) + ": the image does not show");
        if (WIFEXITED(status) && WEXITSTATUS(status) == NoFilterStatus)
            report(lastReport, "the image decoder cannot limit its system calls (seccomp): images of sessions do not show");
    }
}

} // namespace

// ----------------------------------------------------------- the server ---

int runImageServer(std::string_view parent)
{
    // Its own name in ps and the journal, which /proc/self/exe would make
    // "exe". The decoding processes take it too. It ends when the daemon's
    // end of its socket closes, however the daemon ends: no PDEATHSIG, which
    // follows the thread that started it, and a restart runs on a pane's.
    ::prctl(PR_SET_NAME, "afterglow-image");
    if (std::to_string(::getppid()) != parent) return 1;
    // A daemon that started without standard error gives the server none: a
    // descriptor that it receives would land there.
    if (::fcntl(STDERR_FILENO, F_GETFD) < 0 && ::open("/dev/null", O_WRONLY | O_CLOEXEC) != STDERR_FILENO) return 1;
    // Standard input is the socket of the daemon, standard error the
    // daemon's. Nothing else of the daemon reaches the server.
    ::close_range(STDERR_FILENO + 1, ~0U, 0);
    sigset_t children;
    ::sigemptyset(&children);
    ::sigaddset(&children, SIGCHLD);
    ::sigprocmask(SIG_BLOCK, &children, nullptr);
    AfterglowProto::UniqueFd ended(::signalfd(-1, &children, SFD_NONBLOCK | SFD_CLOEXEC));
    if (!ended.valid()) return 1;
    const pid_t self = ::getpid();
    std::chrono::steady_clock::time_point lastReport;
    for (;;) {
        std::array<pollfd, 2> ready{{{STDIN_FILENO, POLLIN, 0}, {ended.get(), POLLIN, 0}}};
        if (::poll(ready.data(), ready.size(), -1) < 0) {
            if (errno == EINTR) continue;
            return 1;
        }
        if (ready[1].revents) {
            signalfd_siginfo info{};
            while (::read(ended.get(), &info, sizeof info) == ssize_t(sizeof info)) {
            }
            reap(lastReport);
        }
        if (!ready[0].revents) continue;
        ImageFormats formats = ImageFormats::Png;
        AfterglowProto::UniqueFd input;
        AfterglowProto::UniqueFd output;
        if (!receiveRequest(STDIN_FILENO, formats, input, output)) return 0;
        if (!input.valid() || !output.valid()) {
            answer(STDIN_FILENO, -1);
            continue;
        }
        const pid_t pid = ::fork();
        if (pid == 0) decodeProcess(input.get(), output.get(), formats, self);
        // The process is not reaped before reap() runs: its pidfd names it.
        AfterglowProto::UniqueFd process(pid > 0 ? pidfdOpen(pid) : -1);
        if (pid > 0 && !process.valid()) ::kill(pid, SIGKILL);
        answer(STDIN_FILENO, process.get());
    }
}

// ---------------------------------------------------------- the daemon ---

ImageDecoder::ImageDecoder(std::string executable, int waitMs)
    : m_executable(std::move(executable))
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

bool ImageDecoder::startLocked()
{
    int pair[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) != 0) return false;
    AfterglowProto::UniqueFd daemon = aboveStandard(pair[0]);
    AfterglowProto::UniqueFd server = aboveStandard(pair[1]);
    if (!daemon.valid() || !server.valid()) return false;

    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attributes;
    if (::posix_spawn_file_actions_init(&actions) != 0) return false;
    if (::posix_spawnattr_init(&attributes) != 0) {
        ::posix_spawn_file_actions_destroy(&actions);
        return false;
    }
    // The daemon blocks the signals of its signalfd and ignores SIGPIPE.
    // Both would reach the server through the exec, as would a SIGCHLD that
    // a host of this class ignores: the server reaps its processes.
    sigset_t none;
    ::sigemptyset(&none);
    sigset_t pipe;
    ::sigemptyset(&pipe);
    ::sigaddset(&pipe, SIGPIPE);
    ::sigaddset(&pipe, SIGXCPU);
    ::sigaddset(&pipe, SIGCHLD);
    const bool prepared = ::posix_spawn_file_actions_adddup2(&actions, server.get(), STDIN_FILENO) == 0
        && ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0) == 0
        && ::posix_spawnattr_setsigmask(&attributes, &none) == 0 && ::posix_spawnattr_setsigdefault(&attributes, &pipe) == 0
        && ::posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF) == 0;
    std::string name = "afterglow-mux";
    std::string argument(DecodeServerArgument);
    std::string parent = std::to_string(::getpid());
    char* argv[] = {name.data(), argument.data(), parent.data(), nullptr};
    // Only what a sanitizer build reads of the daemon's environment.
    std::vector<std::string> variables;
    for (const char* key : {"ASAN_OPTIONS", "UBSAN_OPTIONS", "LSAN_OPTIONS"}) {
        if (const char* value = std::getenv(key)) variables.push_back(std::string(key) + '=' + value);
    }
    std::vector<char*> envp;
    for (std::string& variable : variables) envp.push_back(variable.data());
    envp.push_back(nullptr);
    pid_t pid = -1;
    const bool spawned = prepared && ::posix_spawn(&pid, m_executable.c_str(), &actions, &attributes, argv, envp.data()) == 0;
    ::posix_spawnattr_destroy(&attributes);
    ::posix_spawn_file_actions_destroy(&actions);
    if (!spawned) return false;
    m_server = pid;
    m_socket = std::move(daemon);
    return true;
}

void ImageDecoder::stopLocked()
{
    // The server ends with its socket, and its decoding processes with it.
    m_socket.reset();
    m_owedAnswers = 0;
    if (m_server <= 0) return;
    ::kill(m_server, SIGKILL);
    while (::waitpid(m_server, nullptr, 0) < 0 && errno == EINTR) {
    }
    m_server = -1;
}

namespace {

/// Waits until @p socket is readable, by @p deadline. With @p cancel: also
/// until it is. 1 for the socket, 2 for the cancel, 0 for the deadline.
int waitForAnswer(int socket, std::chrono::steady_clock::time_point deadline, int cancel)
{
    for (;;) {
        const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return 0;
        std::array<pollfd, 2> ready{{{socket, POLLIN, 0}, {cancel, POLLIN, 0}}};
        const int polled = ::poll(ready.data(), cancel >= 0 ? 2 : 1, int(left.count()));
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
        m_socket.reset();
        return {};
    }
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
    // The answers that requests of closed panes left: their processes have
    // no one to read them. The server answers as soon as it forked: one that
    // does not by the deadline is stuck, and the next decode starts another.
    while (m_owedAnswers > 0) {
        if (waitForAnswer(m_socket.get(), deadline, -1) == 0) {
            stopLocked();
            return {};
        }
        if (const AfterglowProto::UniqueFd stale = answerLocked(); stale.valid()) pidfdKill(stale.get());
        if (!m_socket.valid()) return {};
        --m_owedAnswers;
    }
    // A request that waited for the lock past its deadline, or for a pane
    // that closed meanwhile, goes no further: the server is fine.
    if (std::chrono::steady_clock::now() >= deadline) return {};
    if (readable(cancel)) {
        cancelled = true;
        return {};
    }
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
        m_socket.reset();
        return {};
    }
    switch (waitForAnswer(m_socket.get(), deadline, cancel)) {
    case 0:
        stopLocked();
        return {};
    case 2:
        // The pane closes: its answer comes to the next request, which ends
        // its process. The server goes on for the other panes.
        ++m_owedAnswers;
        cancelled = true;
        return {};
    default:
        return answerLocked();
    }
}

bool ImageDecoder::decode(std::string_view data, ImageFormats formats, std::uint32_t& width, std::uint32_t& height,
                          std::vector<std::uint8_t>& rgba, int cancel)
{
    const int waitMs = m_waitMs;
    // A pane that closes decodes nothing more: its feed may hold images.
    if (data.empty() || data.size() > MaxBytes || !allowed(data, formats) || readable(cancel)) return false;
    // The image goes in a memory file, which the decoding process maps.
    AfterglowProto::UniqueFd input(::memfd_create("afterglow-image", MFD_CLOEXEC));
    if (!input.valid() || !writeAll(input.get(), data.data(), data.size())) return false;
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC) != 0) return false;
    AfterglowProto::UniqueFd reader(fds[0]);
    AfterglowProto::UniqueFd writer(fds[1]);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    AfterglowProto::UniqueFd process;
    {
        std::lock_guard lock(m_mutex);
        bool cancelled = false;
        for (int attempt = 0; attempt < 2 && !process.valid() && !cancelled; ++attempt) {
            if (!m_socket.valid()) {
                // The server ended: a crash, the OOM killer, or one that got
                // stuck. The new one has the libraries of now.
                if (std::chrono::steady_clock::now() >= deadline) return false;
                report(m_lastReport, "the image decoder is not running: it starts again");
                stopLocked();
                if (!startLocked()) return false;
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

    const auto late = [this, waitMs](Read read) {
        if (read != Read::Late) return;
        std::lock_guard lock(m_mutex);
        report(m_lastReport, "an image decoder took more than " + std::to_string(waitMs) + " ms: the image does not show");
    };
    Header header;
    Read read = readUntil(reader.get(), &header, sizeof header, deadline, cancel);
    late(read);
    if (read != Read::Done || !fits(header.width, header.height)) return false;
    std::vector<std::uint8_t> pixels;
    try {
        pixels.resize(std::size_t(header.width) * header.height * 4);
    } catch (const std::bad_alloc&) {
        return false;
    }
    read = readUntil(reader.get(), pixels.data(), pixels.size(), deadline, cancel);
    late(read);
    if (read != Read::Done) return false;
    width = header.width;
    height = header.height;
    rgba = std::move(pixels);
    return true;
}

} // namespace AfterglowMux
