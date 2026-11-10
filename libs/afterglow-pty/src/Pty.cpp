// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowPty/Pty.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <pwd.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

extern char** environ;

namespace AfterglowPty {

void setVariable(SpawnOptions& options, std::string name, std::string value)
{
    std::erase_if(options.environment, [&name](const auto& entry) { return entry.first == name; });
    std::erase(options.unsetEnvironment, name);
    options.environment.emplace_back(std::move(name), std::move(value));
}

void unsetVariable(SpawnOptions& options, std::string name)
{
    std::erase_if(options.environment, [&name](const auto& entry) { return entry.first == name; });
    std::erase(options.unsetEnvironment, name);
    options.unsetEnvironment.push_back(std::move(name));
}

std::optional<ProcessStat> parseProcessStat(std::string_view stat)
{
    const std::size_t open = stat.find('(');
    const std::size_t close = stat.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close < open) return std::nullopt;
    std::istringstream fields{std::string(stat.substr(close + 1))};
    ProcessStat parsed;
    if (!(fields >> parsed.state >> parsed.parent >> parsed.group >> parsed.session >> parsed.terminal
          >> parsed.foregroundGroup))
        return std::nullopt;
    return parsed;
}

std::optional<ProcessStat> processStat(pid_t pid)
{
    // The whole file: a line read would stop at a new line in the name.
    std::ifstream file("/proc/" + std::to_string(pid) + "/stat");
    if (!file) return std::nullopt;
    std::ostringstream whole;
    whole << file.rdbuf();
    return parseProcessStat(whole.str());
}

std::string Pty::defaultShell(const SpawnOptions& options)
{
    const auto set = std::find_if(options.environment.rbegin(), options.environment.rend(),
                                  [](const auto& kv) { return kv.first == "SHELL"; });
    if (set != options.environment.rend()) {
        if (!set->second.empty()) return set->second;
    } else if (options.inheritEnvironment
               && std::find(options.unsetEnvironment.begin(), options.unsetEnvironment.end(), "SHELL")
                   == options.unsetEnvironment.end()) {
        // The parent's SHELL only when the child gets the parent's environment:
        // the mux daemon's is not the session's.
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
enum ExecStage : int { TakeTerminal, EnterDirectory, RunProgram };

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
    const auto stat = processStat(pid);
    return stat && stat->session == session;
}

} // namespace

std::unique_ptr<Pty> Pty::spawn(SpawnOptions options, IPtyListener& listener, std::string* error)
{
    // Everything the child needs is built before fork(): after fork() in a
    // threaded process only async-signal-safe calls are allowed.
    std::vector<std::string> args = std::move(options.argv);
    if (args.empty()) {
        args.push_back(defaultShell(options));
    }
    // The path search would try each directory itself, and execve on a
    // directory fails with EACCES: execvp's answer is ENOENT.
    if (args.front().empty()) {
        if (error) *error = std::string("cannot run a program with an empty name: ") + std::strerror(ENOENT);
        return nullptr;
    }
    std::vector<char*> argv;
    for (std::string& a : args) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    // One entry per name, the last value given: programs disagree on which
    // of two entries counts (getenv takes the first, Bash the last). Sets,
    // not scans: a mux session's environment is the client's, whole.
    std::vector<const std::pair<std::string, std::string>*> overrides;
    std::unordered_set<std::string_view> replaced(options.unsetEnvironment.begin(), options.unsetEnvironment.end());
    {
        std::unordered_set<std::string_view> given;
        for (auto it = options.environment.rbegin(); it != options.environment.rend(); ++it) {
            if (given.insert(it->first).second) overrides.push_back(&*it);
        }
        replaced.insert(given.begin(), given.end());
    }
    std::reverse(overrides.begin(), overrides.end());

    std::vector<std::string> envStore;
    for (char** e = environ; options.inheritEnvironment && *e; ++e) {
        const std::string_view entry(*e);
        const auto eq = entry.find('=');
        if (!replaced.contains(entry.substr(0, eq))) {
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
    const int childUmask = options.umask;
    // Worked out here: the child only calls setrlimit.
    std::optional<rlimit> childFileLimit;
    if (rlimit limit{}; options.fileLimit >= 0 && getrlimit(RLIMIT_NOFILE, &limit) == 0) {
        limit.rlim_cur = limit.rlim_max == RLIM_INFINITY ? rlim_t(options.fileLimit) : std::min(rlim_t(options.fileLimit), limit.rlim_max);
        childFileLimit = limit;
    }
    const winsize ws = makeWinsize(options.cols, options.rows, options.pixelWidth, options.pixelHeight);

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
    // The child's stdin and stdout pipes, when it has them: -1 otherwise.
    int inputPipe[2] = {-1, -1};
    int outputPipe[2] = {-1, -1};
    int master = -1;
    int slave = -1;
    auto closeAll = [&] {
        for (int fd : {execPipe[0], execPipe[1], inputPipe[0], inputPipe[1], outputPipe[0], outputPipe[1], master, slave, wakeFd})
            if (fd >= 0) close(fd);
    };
    if ((options.input && pipe2(inputPipe, O_CLOEXEC) != 0) || (options.captureOutput && pipe2(outputPipe, O_CLOEXEC) != 0)) {
        const int code = errno;
        closeAll();
        if (error) *error = std::string("stdio pipe failed: ") + std::strerror(code);
        return nullptr;
    }
    // Both ends close on exec from the start (forkpty's do not): a program
    // that another thread starts while this one waits for exec must not
    // hold the terminal open, or the shell gets no hangup when its pane closes.
    const bool opened = [&] {
        master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
        if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) return false;
        // TIOCGPTPEER opens this master's own peer. A path from ptsname can
        // name another terminal when /dev/pts is a different mount (a
        // container). Any failure falls back to the path, as glibc's openpty
        // does: an error that the path has too (EMFILE) comes from its open.
#ifdef TIOCGPTPEER
        slave = ioctl(master, TIOCGPTPEER, O_RDWR | O_NOCTTY | O_CLOEXEC);
#endif
        if (slave < 0) {
            char name[128];
            if (const int failure = ptsname_r(master, name, sizeof name); failure != 0) {
                errno = failure;
                return false;
            }
            slave = open(name, O_RDWR | O_NOCTTY | O_CLOEXEC);
        }
        return slave >= 0 && ioctl(slave, TIOCSWINSZ, &ws) == 0;
    }();
    if (!opened) {
        const int code = errno;
        closeAll();
        if (error) *error = std::string("cannot open a terminal: ") + std::strerror(code);
        return nullptr;
    }
    // Above the standard streams, which the child points at the terminal
    // before it takes its pipes. The terminal too: dup2 onto itself would
    // keep its close-on-exec.
    for (int* descriptor : {&execPipe[0], &execPipe[1], &inputPipe[0], &inputPipe[1], &outputPipe[0], &outputPipe[1], &slave}) {
        if (*descriptor < 0 || *descriptor >= 3) continue;
        const int moved = fcntl(*descriptor, F_DUPFD_CLOEXEC, 3);
        if (moved < 0) {
            const int code = errno;
            closeAll();
            if (error) *error = std::string("pipe setup failed: ") + std::strerror(code);
            return nullptr;
        }
        close(*descriptor);
        *descriptor = moved;
    }
    // No handler of the parent may run in the child between fork and the
    // reset below: it would act on the parent's state (posix_spawn blocks
    // signals the same way). The child clears the mask itself.
    sigset_t blockAll;
    sigset_t parentMask;
    sigfillset(&blockAll);
    pthread_sigmask(SIG_SETMASK, &blockAll, &parentMask);
    const pid_t pid = fork();
    const int forkError = errno;
    if (pid != 0) pthread_sigmask(SIG_SETMASK, &parentMask, nullptr);
    if (pid < 0) {
        closeAll();
        if (error) *error = std::string("fork failed: ") + std::strerror(forkError);
        return nullptr;
    }

    if (pid == 0) {
        // Child. Signals the launcher ignored stay ignored across exec: give
        // the program the defaults it expects. The handlers go first: a
        // signal that waited in the mask would run the parent's handler here.
        struct sigaction fallback {};
        fallback.sa_handler = SIG_DFL;
        for (int sig = 1; sig < NSIG; ++sig) {
            if (sig != SIGKILL && sig != SIGSTOP) sigaction(sig, &fallback, nullptr);
        }
        sigset_t all;
        sigemptyset(&all);
        sigprocmask(SIG_SETMASK, &all, nullptr);
        auto failed = [&](int stage) {
            const int report[2] = {stage, errno};
            ssize_t sent;
            do { sent = ::write(execPipe[1], report, sizeof report); } while (sent < 0 && errno == EINTR);
            _exit(127);
        };
        // A session of its own, with the terminal as its controlling
        // terminal and its standard streams (login_tty).
        if (setsid() < 0 || ioctl(slave, TIOCSCTTY, 0) != 0) failed(TakeTerminal);
        for (int stream : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
            if (dup2(slave, stream) < 0) failed(TakeTerminal);
        }
        // Canonical-mode programs (cat, read, password prompts) erase whole
        // UTF-8 characters only with IUTF8, which a new pty lacks.
        if (termios t{}; tcgetattr(STDIN_FILENO, &t) == 0) {
            t.c_iflag |= IUTF8;
            tcsetattr(STDIN_FILENO, TCSANOW, &t);
        }
        close(execPipe[0]);
        // The pipes take the place of the terminal; it stays the controlling
        // terminal (/dev/tty) and standard error.
        if (inputPipe[0] >= 0) dup2(inputPipe[0], STDIN_FILENO);
        if (outputPipe[1] >= 0) dup2(outputPipe[1], STDOUT_FILENO);
        if (execPipe[1] > 3) close_range(3, unsigned(execPipe[1] - 1), 0);
        close_range(unsigned(execPipe[1] + 1), ~0U, 0);
        if (childUmask >= 0) ::umask(mode_t(childUmask));
        if (childFileLimit) setrlimit(RLIMIT_NOFILE, &*childFileLimit);
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
    for (int fd : {inputPipe[0], outputPipe[1], slave})
        if (fd >= 0) close(fd);
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
    result->m_outputFd = outputPipe[0];
    if (inputPipe[1] >= 0) {
        if (options.input->empty()) {
            close(inputPipe[1]); // at its end at once
        } else {
            result->m_inputFd = inputPipe[1];
            result->m_input = std::move(*options.input);
        }
    }
    for (int fd : {result->m_inputFd, result->m_outputFd}) {
        if (fd < 0) continue;
        const int pipeFlags = fcntl(fd, F_GETFL);
        if (pipeFlags < 0 || fcntl(fd, F_SETFL, pipeFlags | O_NONBLOCK) < 0) {
            if (error) *error = std::string("pipe setup failed: ") + std::strerror(errno);
            return nullptr;
        }
    }
    if (received != 0) {
        if (error) {
            const std::string reason = std::strerror(received == sizeof report ? report[1] : readError);
            const int stage = received == sizeof report ? report[0] : RunProgram;
            *error = stage == TakeTerminal ? "cannot open a terminal: " + reason
                : stage == EnterDirectory  ? "cannot enter directory " + cwd + ": " + reason
                                           : "cannot run " + args.front() + ": " + reason;
        }
        return nullptr;
    }
    const int flags = fcntl(master, F_GETFL);
    if (flags < 0 || fcntl(master, F_SETFL, flags | O_NONBLOCK) < 0) {
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
    if (m_inputFd >= 0) close(m_inputFd);
    if (m_outputFd >= 0) close(m_outputFd);
    if (const pid_t child = m_pid.load(); child > 0) Reaper::instance().adopt(child);
}

bool Pty::write(std::string_view bytes)
{
    {
        std::lock_guard lock(m_writeMutex);
        if (!m_accepting.load()) return false;
        if (bytes.size() > MaxPendingInput - m_pendingBytes) {
            m_refused = true;
            return false;
        }
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
    // spawn() makes the child a session leader: its PID is the session ID.
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
    bool pending = false;
    bool room = false;
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
        pending = !m_pending.empty();
        // Half the queue, not one byte: a writer that waits for room sends
        // its next part into a queue that can take it. A queue that takes
        // nothing more (EIO above) says so too: the writer stops waiting.
        if (m_refused && (!m_accepting.load() || m_pendingBytes <= MaxPendingInput / 2)) {
            m_refused = false;
            room = true;
        }
    }
    if (room) m_listener.ptyWritable();
    return pending;
}

void Pty::feedInput()
{
    // Bound work per poll tick, as flushPending does: a child that reads as
    // fast as it is fed must not starve stop() or its own output.
    for (int writes = 0; writes < 16 && m_inputOffset < m_input.size() && !m_stop.load(); ++writes) {
        const std::size_t amount = std::min<std::size_t>(65536, m_input.size() - m_inputOffset);
        const ssize_t n = ::write(m_inputFd, m_input.data() + m_inputOffset, amount);
        if (n > 0) {
            m_inputOffset += std::size_t(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n < 0 && errno == EPIPE) {
            // The child closed its stdin before it read everything: what is
            // left goes nowhere. This thread blocks SIGPIPE, so the signal
            // waits for it alone: take it.
            sigset_t broken;
            sigemptyset(&broken);
            sigaddset(&broken, SIGPIPE);
            const timespec now{0, 0};
            while (sigtimedwait(&broken, nullptr, &now) < 0 && errno == EINTR) {}
        }
        closeInput();
        return;
    }
    if (m_inputOffset == m_input.size()) closeInput();
}

void Pty::closeInput()
{
    if (m_inputFd < 0) return;
    close(m_inputFd);
    m_inputFd = -1;
    std::string().swap(m_input);
    m_inputOffset = 0;
}

bool Pty::readOutput(std::vector<char>& buffer)
{
    // A finite batch, as for the terminal: a child that writes without end
    // must not starve stop(). A full batch means more may wait, not the end:
    // closing the pipe then would cut the output and end the child with SIGPIPE.
    for (int reads = 0; reads < 16 && !m_stop.load(); ++reads) {
        const ssize_t n = ::read(m_outputFd, buffer.data(), buffer.size());
        if (n > 0) {
            m_listener.ptyOutput(std::string_view(buffer.data(), std::size_t(n)));
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        // Its end, or an error.
        close(m_outputFd);
        m_outputFd = -1;
        return false;
    }
    return true;
}

void Pty::run()
{
    // A child that closes its stdin pipe early makes the write fail with
    // EPIPE: the signal must not end the process (feedInput takes it).
    sigset_t pipeSignal;
    sigemptyset(&pipeSignal);
    sigaddset(&pipeSignal, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipeSignal, nullptr);
    std::vector<char> buf(std::size_t(1) << 16);
    bool wantWrite = false;
    bool closed = false; // nothing more is read from the terminal
    bool exited = false; // the child has exited (pidfd)
    // What the child wrote to its stdout before it exited is in the pipe
    // now, at most the pipe's capacity: a write past it would have waited
    // for this thread. Read it to the pipe's end. A background job that kept
    // the pipe is not waited for: the drain stops once the pipe is empty, or
    // after a bounded number of batches while the job writes on.
    auto drainOutput = [&] {
        for (int batches = 0; m_outputFd >= 0 && batches < 64 && !m_stop.load(); ++batches) {
            pollfd ready{m_outputFd, POLLIN, 0};
            if (poll(&ready, 1, 0) <= 0 || !readOutput(buf)) break;
        }
    };

    while (!m_stop.load()) {
        pollfd fds[6] = {
            // A hung-up fd reports POLLHUP even with no events requested, so
            // drop it from the set once closed instead of spinning on it.
            {closed ? -1 : m_master, short(POLLIN | (wantWrite ? POLLOUT : 0)), 0},
            {m_wakeFd, POLLIN, 0},
            {exited ? -1 : m_pidFd, POLLIN, 0},
            {m_execFd, POLLIN, 0},
            // The pipes stay in the set until the child is reaped: a child
            // whose standard streams are the pipes can drop its last terminal
            // fd (the master hangs up) and still read and write them. Their
            // own end or error closes them, so they cannot spin.
            {m_inputFd, POLLOUT, 0},
            {m_outputFd, POLLIN, 0},
        };
        // Without a pidfd, look for the child's exit every 100 ms once the
        // terminal hung up. So too once the pidfd fired but the child could not
        // be reaped yet (a tracer such as gdb or strace waits for it first):
        // the pidfd stays readable and is out of the set.
        const int timeout = closed && (m_pidFd < 0 || exited) ? 100 : -1;
        if (poll(fds, 6, timeout) < 0) {
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
            drainOutput();
            closeInput();
            m_listener.ptyClosed(reaped ? status : -1);
            return;
        }
        if (fds[4].revents) feedInput(); // POLLOUT, or POLLERR once the child closed its end
        if (fds[5].revents) readOutput(buf);

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
            // A writer that waits for room waits no more: the child side is
            // gone, while the child may run on for long.
            bool refused = false;
            {
                std::lock_guard lock(m_writeMutex);
                refused = std::exchange(m_refused, false);
            }
            if (refused) m_listener.ptyWritable();
            int status = 0;
            const pid_t child = m_pid.load();
            const pid_t reaped = waitChild(child, &status, WNOHANG);
            if (reaped == child || (reaped < 0 && errno == ECHILD)) {
                m_pid = -1;
                drainOutput();
                // A background job that holds the stdin pipe sees its end.
                closeInput();
                m_listener.ptyClosed(reaped == child ? status : -1);
                return;
            }
        }
    }
    m_accepting.store(false);
}

} // namespace AfterglowPty
