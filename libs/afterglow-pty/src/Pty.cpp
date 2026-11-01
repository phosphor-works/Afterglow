// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowPty/Pty.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <pwd.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char** environ;

namespace AfterglowPty {

std::string Pty::defaultShell(const SpawnOptions& options)
{
    const auto set = std::find_if(options.environment.rbegin(), options.environment.rend(),
                                  [](const auto& kv) { return kv.first == "SHELL"; });
    if (set != options.environment.rend()) {
        if (!set->second.empty()) return set->second;
    } else if (std::find(options.unsetEnvironment.begin(), options.unsetEnvironment.end(), "SHELL")
               == options.unsetEnvironment.end()) {
        if (const char* s = std::getenv("SHELL"); s && *s) return s;
    }
    if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_shell && *pw->pw_shell) {
        return pw->pw_shell;
    }
    return "/bin/sh";
}

namespace {

pid_t waitChild(pid_t child, int* status, int flags)
{
    pid_t result;
    do { result = waitpid(child, status, flags); } while (result < 0 && errno == EINTR);
    return result;
}

winsize makeWinsize(int cols, int rows, int pw, int ph)
{
    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(std::clamp(cols, 1, 65535));
    ws.ws_row = static_cast<unsigned short>(std::clamp(rows, 1, 65535));
    ws.ws_xpixel = static_cast<unsigned short>(std::clamp(pw, 0, 65535));
    ws.ws_ypixel = static_cast<unsigned short>(std::clamp(ph, 0, 65535));
    return ws;
}

// What the child reports through the exec pipe before it gives up.
enum ExecStage : int { EnterDirectory, RunProgram };

// How long spawn() waits for exec on the caller's thread. A child stuck in
// chdir or execve on a hung mount must not freeze the GUI.
constexpr int ExecWaitMs = 2000;

// Reaps the children of closed terminals off the owner's thread. A shell that
// saves its history on SIGHUP, or one stuck on a hung mount, must not freeze
// the GUI while its pane closes.
class Reaper
{
public:
    static Reaper& instance()
    {
        static Reaper reaper;
        return reaper;
    }

    void adopt(pid_t child)
    {
        kill(child, SIGHUP);
        std::unique_lock lock(m_mutex);
        m_children.push_back({child, Clock::now() + Grace});
        if (!m_thread.joinable()) {
            try {
                m_thread = std::thread([this] { run(); });
            } catch (const std::system_error&) {
                m_children.pop_back();
                lock.unlock();
                reapHere(child);
                return;
            }
        }
        m_wake.notify_one();
    }

    // At exit, children in their grace period get the rest of it, then
    // SIGKILL. None is waited for after that.
    ~Reaper()
    {
        {
            std::lock_guard lock(m_mutex);
            m_exiting = true;
        }
        m_wake.notify_one();
        if (m_thread.joinable()) m_thread.join();
    }

private:
    using Clock = std::chrono::steady_clock;
    // A child's time to leave on SIGHUP before SIGKILL.
    static constexpr auto Grace = std::chrono::milliseconds(100);
    static constexpr auto Interval = std::chrono::milliseconds(5);

    struct Child
    {
        pid_t pid;
        Clock::time_point killAt;
        bool killed = false;
    };

    static void reapHere(pid_t child)
    {
        int status = 0;
        for (auto end = Clock::now() + Grace; Clock::now() < end;) {
            if (waitChild(child, &status, WNOHANG) != 0) return;
            std::this_thread::sleep_for(Interval);
        }
        kill(child, SIGKILL);
        waitChild(child, &status, 0);
    }

    void run()
    {
        std::unique_lock lock(m_mutex);
        while (true) {
            const auto now = Clock::now();
            std::erase_if(m_children, [now](Child& c) {
                int status = 0;
                if (waitChild(c.pid, &status, WNOHANG) != 0) return true;
                if (!c.killed && now >= c.killAt) {
                    kill(c.pid, SIGKILL);
                    c.killed = true;
                }
                return false;
            });
            const bool graceful = std::any_of(m_children.begin(), m_children.end(), [](const Child& c) { return !c.killed; });
            // A child that survives SIGKILL (stuck in the kernel) is left to init at exit.
            if (m_exiting && !graceful) return;
            if (m_children.empty()) {
                m_wake.wait(lock, [this] { return m_exiting || !m_children.empty(); });
            } else {
                m_wake.wait_for(lock, graceful ? Interval : Grace);
            }
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::vector<Child> m_children;
    bool m_exiting = false;
    std::thread m_thread;
};

// /proc/PID/cwd when it is a directory that exists. A removed directory reads
// as "PATH (deleted)", and a new pane cannot start there.
std::optional<std::string> directoryOf(pid_t pid)
{
    std::error_code ec;
    const auto link = std::filesystem::read_symlink("/proc/" + std::to_string(pid) + "/cwd", ec);
    if (ec || !std::filesystem::is_directory(link, ec)) return std::nullopt;
    return link.string();
}

// Whether PID belongs to SESSION. The foreground group's leader may have
// exited, and its PID may now be another process's.
bool inSession(pid_t pid, pid_t session)
{
    std::ifstream stat("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    std::getline(stat, line);
    // The command name in parentheses can hold spaces: fields follow the last ')'.
    const auto name = line.rfind(')');
    if (name == std::string::npos) return false;
    std::istringstream fields(line.substr(name + 1));
    char state = 0;
    long parent = 0, group = 0, owner = 0;
    fields >> state >> parent >> group >> owner;
    return fields && owner == session;
}

} // namespace

std::unique_ptr<Pty> Pty::spawn(const SpawnOptions& options, IPtyListener& listener, std::string* error)
{
    // Everything the child needs is built before fork(): after fork() in a
    // threaded process only async-signal-safe calls are allowed.
    std::vector<std::string> args = options.argv;
    if (args.empty()) {
        args.push_back(defaultShell(options));
    }
    std::vector<char*> argv;
    for (std::string& a : args) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    // One entry per name, the last value given: programs disagree on which
    // of two entries counts (getenv takes the first, Bash the last).
    std::vector<const std::pair<std::string, std::string>*> overrides;
    for (auto it = options.environment.rbegin(); it != options.environment.rend(); ++it) {
        if (std::none_of(overrides.begin(), overrides.end(), [&](const auto* kv) { return kv->first == it->first; }))
            overrides.push_back(&*it);
    }
    std::reverse(overrides.begin(), overrides.end());

    std::vector<std::string> envStore;
    for (char** e = environ; *e; ++e) {
        const std::string_view entry(*e);
        const auto eq = entry.find('=');
        const std::string_view key = entry.substr(0, eq);
        const bool overridden =
            std::find(options.unsetEnvironment.begin(), options.unsetEnvironment.end(), key) != options.unsetEnvironment.end()
            || std::any_of(overrides.begin(), overrides.end(), [key](const auto* kv) { return kv->first == key; });
        if (!overridden) {
            envStore.emplace_back(entry);
        }
    }
    for (const auto* kv : overrides) {
        envStore.push_back(kv->first + "=" + kv->second);
    }
    std::vector<char*> envp;
    for (std::string& e : envStore) {
        envp.push_back(e.data());
    }
    envp.push_back(nullptr);

    // execvpe would search the parent's PATH rather than the supplied envp.
    // Prepare candidates before fork; relative entries resolve after child chdir.
    std::vector<std::string> executables;
    if (args.front().find('/') != std::string::npos) executables.push_back(args.front());
    else {
        std::string path = "/bin:/usr/bin";
        for (const auto& entry : envStore) if (entry.starts_with("PATH=")) path = entry.substr(5);
        std::size_t start = 0;
        do {
            const auto end = path.find(':', start);
            const auto directory = path.substr(start, end == std::string::npos ? end : end - start);
            executables.push_back((directory.empty() ? "." : directory) + '/' + args.front());
            if (end == std::string::npos) break;
            start = end + 1;
        } while (start <= path.size());
    }
    std::vector<char*> shellArgv{const_cast<char*>("/bin/sh"), nullptr};
    for (std::size_t i = 1; i < args.size(); ++i) shellArgv.push_back(args[i].data());
    shellArgv.push_back(nullptr);

    const std::string cwd = options.workingDirectory;
    winsize ws = makeWinsize(options.cols, options.rows, options.pixelWidth, options.pixelHeight);

    const int wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeFd < 0) {
        if (error) *error = std::string("eventfd failed: ") + std::strerror(errno);
        return nullptr;
    }
    int execPipe[2];
    if (pipe2(execPipe, O_CLOEXEC) != 0) {
        if (error) *error = std::string("exec pipe failed: ") + std::strerror(errno);
        close(wakeFd);
        return nullptr;
    }
    for (int& descriptor : execPipe) {
        if (descriptor >= 3) continue;
        const int moved = fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
        if (moved < 0) {
            const int code = errno;
            close(execPipe[0]); close(execPipe[1]); close(wakeFd);
            if (error) *error = std::string("exec pipe setup failed: ") + std::strerror(code);
            return nullptr;
        }
        close(descriptor);
        descriptor = moved;
    }
    int master = -1;
    const pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) {
        const int forkError = errno;
        close(wakeFd);
        close(execPipe[0]);
        close(execPipe[1]);
        errno = forkError;
        if (error) {
            *error = std::string("forkpty failed: ") + std::strerror(errno);
        }
        return nullptr;
    }

    if (pid == 0) {
        // Child. Signals the launcher ignored stay ignored across exec: give
        // the program the defaults it expects.
        sigset_t all;
        sigemptyset(&all);
        sigprocmask(SIG_SETMASK, &all, nullptr);
        struct sigaction fallback {};
        fallback.sa_handler = SIG_DFL;
        for (int sig = 1; sig < NSIG; ++sig) {
            if (sig != SIGKILL && sig != SIGSTOP) sigaction(sig, &fallback, nullptr);
        }
        // Canonical-mode programs (cat, read, password prompts) erase whole
        // UTF-8 characters only with IUTF8, which a new pty lacks.
        if (termios t{}; tcgetattr(STDIN_FILENO, &t) == 0) {
            t.c_iflag |= IUTF8;
            tcsetattr(STDIN_FILENO, TCSANOW, &t);
        }
        close(execPipe[0]);
        if (execPipe[1] > 3) close_range(3, unsigned(execPipe[1] - 1), 0);
        close_range(unsigned(execPipe[1] + 1), ~0U, 0);
        auto failed = [&](int stage) {
            const int report[2] = {stage, errno};
            ssize_t sent;
            do { sent = ::write(execPipe[1], report, sizeof report); } while (sent < 0 && errno == EINTR);
            _exit(127);
        };
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) failed(EnterDirectory);
        bool denied = false;
        for (auto& executable : executables) {
            execve(executable.c_str(), argv.data(), envp.data());
            if (errno == ENOEXEC) {
                shellArgv[1] = executable.data();
                execve("/bin/sh", shellArgv.data(), envp.data());
                failed(RunProgram);
            }
            // The search goes on past these, as execvp's does.
            if (errno == EACCES) denied = true;
            else if (errno != ENOENT && errno != ENOTDIR && errno != ESTALE && errno != ENODEV && errno != ETIMEDOUT)
                failed(RunProgram);
        }
        if (denied) errno = EACCES;
        failed(RunProgram);
    }

    close(execPipe[1]);
    int pidFd = -1;
#ifdef SYS_pidfd_open
    pidFd = int(syscall(SYS_pidfd_open, pid, 0));
#endif
    // The pipe closes at exec, or carries the stage and errno of a failure.
    pollfd execReport{execPipe[0], POLLIN, 0};
    int ready;
    do { ready = poll(&execReport, 1, ExecWaitMs); } while (ready < 0 && errno == EINTR);
    int report[2] = {0, 0};
    ssize_t received = 0;
    int readError = 0;
    if (ready != 0) {
        do { received = ::read(execPipe[0], report, sizeof report); } while (received < 0 && errno == EINTR);
        readError = errno;
        close(execPipe[0]);
        execPipe[0] = -1;
    }
    auto result = std::unique_ptr<Pty>(new Pty(master, wakeFd, pidFd, pid, listener));
    // Still in exec: the reader thread takes the report, and a failed child
    // reaches the owner as an exit with status 127.
    result->m_execFd = execPipe[0];
    if (received != 0) {
        if (error) {
            const std::string reason = std::strerror(received == sizeof report ? report[1] : readError);
            *error = received == sizeof report && report[0] == EnterDirectory
                ? "cannot enter directory " + cwd + ": " + reason
                : "cannot run " + args.front() + ": " + reason;
        }
        return nullptr;
    }
    const int flags = fcntl(master, F_GETFL);
    if (flags < 0 || fcntl(master, F_SETFL, flags | O_NONBLOCK) < 0 || fcntl(master, F_SETFD, FD_CLOEXEC) < 0) {
        if (error) *error = std::string("pty setup failed: ") + std::strerror(errno);
        return nullptr;
    }
    if (!options.deferStart && !result->start(error)) return nullptr;
    return result;
}

Pty::Pty(int masterFd, int wakeFd, int pidFd, pid_t pid, IPtyListener& listener)
    : m_master(masterFd), m_wakeFd(wakeFd), m_pidFd(pidFd), m_pid(pid), m_listener(listener)
{
}

bool Pty::start(std::string* error)
{
    if (m_stop.load()) {
        if (error) *error = "PTY has been stopped";
        return false;
    }
    if (m_thread.joinable()) return true;
    try {
        m_thread = std::thread([this] { run(); });
    } catch (const std::system_error& e) {
        if (error) *error = e.what();
        stop();
        return false;
    }
    return true;
}

void Pty::stop()
{
    m_accepting.store(false);
    m_stop.store(true);
    wake();
    if (m_thread.joinable()) m_thread.join();
    std::lock_guard lock(m_writeMutex);
    m_pending.clear();
    m_pendingBytes = m_pendingOffset = 0;
}

Pty::~Pty()
{
    stop();
    // Closing the master hangs up the terminal, which also sends the shell SIGHUP.
    close(m_master);
    close(m_wakeFd);
    if (m_pidFd >= 0) close(m_pidFd);
    if (m_execFd >= 0) close(m_execFd);
    if (const pid_t child = m_pid.load(); child > 0) Reaper::instance().adopt(child);
}

bool Pty::write(std::string_view bytes)
{
    constexpr std::size_t MaxPending = 32u * 1024 * 1024;
    {
        std::lock_guard lock(m_writeMutex);
        if (!m_accepting.load() || bytes.size() > MaxPending - m_pendingBytes) return false;
        if (bytes.empty()) return true;
        if (!m_pending.empty() && m_pending.back().size() + bytes.size() <= 65536)
            m_pending.back().append(bytes);
        else
            m_pending.emplace_back(bytes);
        m_pendingBytes += bytes.size();
    }
    wake();
    return true;
}

std::size_t Pty::pendingInput() const
{
    std::lock_guard lock(m_writeMutex);
    return m_pendingBytes;
}

void Pty::resize(int cols, int rows, int pixelWidth, int pixelHeight)
{
    winsize ws = makeWinsize(cols, rows, pixelWidth, pixelHeight);
    ioctl(m_master, TIOCSWINSZ, &ws);
}

std::string Pty::currentWorkingDirectory() const
{
    // forkpty makes the child a session leader: its PID is the session ID.
    const pid_t child = m_pid.load();
    if (child <= 0) return {};
    if (const pid_t group = tcgetpgrp(m_master); group > 0 && group != child && inSession(group, child)) {
        if (auto directory = directoryOf(group)) return *directory;
    }
    return directoryOf(child).value_or(std::string());
}

std::string Pty::foregroundProcess() const
{
    const pid_t group = tcgetpgrp(m_master);
    if (group <= 0 || group == m_pid.load()) {
        return {};
    }
    std::ifstream comm("/proc/" + std::to_string(group) + "/comm");
    std::string name;
    std::getline(comm, name);
    return name.empty() ? std::string("a program") : name;
}

void Pty::wake()
{
    const std::uint64_t one = 1;
    [[maybe_unused]] const auto n = ::write(m_wakeFd, &one, sizeof one);
}

bool Pty::flushPending()
{
    std::lock_guard lock(m_writeMutex);
    // Bound work per poll tick so shutdown and output remain responsive.
    for (int attempts = 0; attempts < 16 && !m_pending.empty() && !m_stop.load(); ++attempts) {
        const auto& front = m_pending.front();
        const std::size_t amount = std::min<std::size_t>(65536, front.size() - m_pendingOffset);
        const ssize_t n = ::write(m_master, front.data() + m_pendingOffset, amount);
        if (n > 0) {
            m_pendingOffset += std::size_t(n);
            m_pendingBytes -= std::size_t(n);
            if (m_pendingOffset == front.size()) { m_pending.pop_front(); m_pendingOffset = 0; }
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                // The child side is gone (EIO). Input stops here; output the
                // child wrote before it left can still be read.
                m_accepting.store(false);
                m_pending.clear();
                m_pendingBytes = m_pendingOffset = 0;
            }
            break;
        }
    }
    return !m_pending.empty();
}

void Pty::run()
{
    std::vector<char> buf(std::size_t(1) << 16);
    bool wantWrite = false;
    bool closed = false; // nothing more is read from the terminal
    bool exited = false; // the child has exited (pidfd)

    while (!m_stop.load()) {
        pollfd fds[4] = {
            // A hung-up fd reports POLLHUP even with no events requested, so
            // drop it from the set once closed instead of spinning on it.
            {closed ? -1 : m_master, short(POLLIN | (wantWrite ? POLLOUT : 0)), 0},
            {m_wakeFd, POLLIN, 0},
            {exited ? -1 : m_pidFd, POLLIN, 0},
            {m_execFd, POLLIN, 0},
        };
        // Without a pidfd, look for the child's exit every 100 ms once the
        // terminal hung up. So too once the pidfd fired but the child could not
        // be reaped yet (a tracer such as gdb or strace waits for it first):
        // the pidfd stays readable and is out of the set.
        const int timeout = closed && (m_pidFd < 0 || exited) ? 100 : -1;
        if (poll(fds, 4, timeout) < 0) {
            if (errno == EINTR) {
                continue;
            }
            // The thread cannot wait on the terminal any more: report it
            // closed, so the pane does not stay on a dead terminal.
            m_accepting.store(false);
            int status = 0;
            const pid_t child = m_pid.load();
            const bool reaped = waitChild(child, &status, WNOHANG) == child;
            if (reaped) m_pid = -1;
            m_listener.ptyClosed(reaped ? status : -1);
            return;
        }

        if (fds[3].revents) {
            // A late exec failure: the child exits with 127, which the reap reports.
            int report[2];
            ssize_t n;
            do { n = ::read(m_execFd, report, sizeof report); } while (n < 0 && errno == EINTR);
            close(m_execFd);
            m_execFd = -1;
        }
        if (fds[1].revents & POLLIN) {
            std::uint64_t v;
            [[maybe_unused]] const auto n = ::read(m_wakeFd, &v, sizeof v);
            if (!closed) {
                wantWrite = flushPending();
            }
        }
        if (fds[2].revents & POLLIN) {
            exited = true;
        }

        if (!closed) {
            if (fds[0].revents & POLLOUT) {
                wantWrite = flushPending();
            }
            // After the child exits, take what it wrote before it left. A
            // background job may still hold the terminal open: its later
            // output is not waited for.
            if (fds[0].revents & POLLIN || exited) {
                // A finite batch prevents continuous output starving stop().
                for (int reads = 0; reads < 16 && !m_stop.load(); ++reads) {
                    const ssize_t n = ::read(m_master, buf.data(), buf.size());
                    if (n > 0) {
                        m_listener.ptyData(std::string_view(buf.data(), std::size_t(n)));
                        continue;
                    }
                    if (n < 0 && errno == EINTR) {
                        continue;
                    }
                    if (n < 0 && errno == EAGAIN) {
                        break;
                    }
                    closed = true; // EIO or EOF: the child side is gone
                    break;
                }
            } else if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                closed = true;
            }
            if (exited) closed = true;
        }

        if (closed) {
            m_accepting.store(false);
            int status = 0;
            const pid_t child = m_pid.load();
            const pid_t reaped = waitChild(child, &status, WNOHANG);
            if (reaped == child || (reaped < 0 && errno == ECHILD)) {
                m_pid = -1;
                m_listener.ptyClosed(reaped == child ? status : -1);
                return;
            }
        }
    }
    m_accepting.store(false);
}

} // namespace AfterglowPty
