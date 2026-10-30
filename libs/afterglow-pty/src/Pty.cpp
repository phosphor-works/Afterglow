// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowPty/Pty.h>

#include <algorithm>
#include <cerrno>
#include <system_error>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <pwd.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace AfterglowPty {

std::string Pty::defaultShell()
{
    if (const char* s = std::getenv("SHELL"); s && *s) {
        return s;
    }
    if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_shell && *pw->pw_shell) {
        return pw->pw_shell;
    }
    return "/bin/sh";
}

namespace {

winsize makeWinsize(int cols, int rows, int pw, int ph)
{
    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(std::clamp(cols, 1, 65535));
    ws.ws_row = static_cast<unsigned short>(std::clamp(rows, 1, 65535));
    ws.ws_xpixel = static_cast<unsigned short>(std::clamp(pw, 0, 65535));
    ws.ws_ypixel = static_cast<unsigned short>(std::clamp(ph, 0, 65535));
    return ws;
}

} // namespace

std::unique_ptr<Pty> Pty::spawn(const SpawnOptions& options, IPtyListener& listener, std::string* error)
{
    // Everything the child needs is built before fork(): after fork() in a
    // threaded process only async-signal-safe calls are allowed.
    std::vector<std::string> args = options.argv;
    if (args.empty()) {
        args.push_back(defaultShell());
    }
    std::vector<char*> argv;
    for (std::string& a : args) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);

    std::vector<std::string> envStore;
    for (char** e = environ; *e; ++e) {
        const std::string_view entry(*e);
        const auto eq = entry.find('=');
        const std::string_view key = entry.substr(0, eq);
        bool overridden = std::find(options.unsetEnvironment.begin(), options.unsetEnvironment.end(), key) != options.unsetEnvironment.end();
        for (const auto& kv : options.environment) {
            if (kv.first == key) {
                overridden = true;
                break;
            }
        }
        if (!overridden) {
            envStore.emplace_back(entry);
        }
    }
    for (const auto& kv : options.environment) {
        envStore.push_back(kv.first + "=" + kv.second);
    }
    std::vector<char*> envp;
    for (std::string& e : envStore) {
        envp.push_back(e.data());
    }
    envp.push_back(nullptr);

    const std::string cwd = options.workingDirectory;
    winsize ws = makeWinsize(options.cols, options.rows, options.pixelWidth, options.pixelHeight);

    const int wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeFd < 0) {
        if (error) *error = std::string("eventfd failed: ") + std::strerror(errno);
        return nullptr;
    }
    int master = -1;
    const pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) {
        const int forkError = errno;
        close(wakeFd);
        errno = forkError;
        if (error) {
            *error = std::string("forkpty failed: ") + std::strerror(errno);
        }
        return nullptr;
    }

    if (pid == 0) {
        // Child.
        sigset_t all;
        sigemptyset(&all);
        sigprocmask(SIG_SETMASK, &all, nullptr);
        for (int sig : {SIGCHLD, SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGALRM, SIGPIPE}) {
            signal(sig, SIG_DFL);
        }
        close_range(3, ~0U, 0);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
            // Fall through: the shell starts in the inherited directory.
        }
        execvpe(argv[0], argv.data(), envp.data());
        const char msg[] = "afterglow: failed to start the shell\r\n";
        ::write(STDERR_FILENO, msg, sizeof msg - 1);
        _exit(127);
    }

    auto result = std::unique_ptr<Pty>(new Pty(master, wakeFd, pid, listener));
    const int flags = fcntl(master, F_GETFL);
    if (flags < 0 || fcntl(master, F_SETFL, flags | O_NONBLOCK) < 0 || fcntl(master, F_SETFD, FD_CLOEXEC) < 0) {
        if (error) *error = std::string("pty setup failed: ") + std::strerror(errno);
        return nullptr;
    }
    if (!options.deferStart && !result->start(error)) return nullptr;
    return result;
}

Pty::Pty(int masterFd, int wakeFd, pid_t pid, IPtyListener& listener)
    : m_master(masterFd), m_wakeFd(wakeFd), m_pid(pid), m_listener(listener)
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
    const pid_t child = m_pid.load();
    if (child > 0) {
        int status = 0;
        if (waitpid(child, &status, WNOHANG) == 0) {
            kill(child, SIGHUP);
            // Give the child a moment to leave on SIGHUP before forcing it.
            for (int i = 0; i < 20; ++i) {
                if (waitpid(child, &status, WNOHANG) != 0) {
                    break;
                }
                usleep(5000);
                if (i == 19) {
                    kill(child, SIGKILL);
                    waitpid(child, &status, 0);
                }
            }
        }
    }
    close(m_master);
    close(m_wakeFd);
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

void Pty::resize(int cols, int rows, int pixelWidth, int pixelHeight)
{
    winsize ws = makeWinsize(cols, rows, pixelWidth, pixelHeight);
    ioctl(m_master, TIOCSWINSZ, &ws);
}

std::string Pty::currentWorkingDirectory() const
{
    std::error_code ec;
    const auto p = std::filesystem::read_symlink("/proc/" + std::to_string(tcgetpgrp(m_master)) + "/cwd", ec);
    if (!ec) {
        return p.string();
    }
    const auto own = std::filesystem::read_symlink("/proc/" + std::to_string(m_pid.load()) + "/cwd", ec);
    return ec ? std::string() : own.string();
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
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) m_accepting.store(false);
            break;
        }
    }
    return !m_pending.empty();
}

void Pty::run()
{
    std::vector<char> buf(std::size_t(1) << 16);
    bool wantWrite = false;
    bool closed = false;

    while (!m_stop.load()) {
        pollfd fds[2] = {
            // A hung-up fd reports POLLHUP even with no events requested, so
            // drop it from the set once closed instead of spinning on it.
            {closed ? -1 : m_master, short(POLLIN | (wantWrite ? POLLOUT : 0)), 0},
            {m_wakeFd, POLLIN, 0},
        };
        const int timeout = closed ? 100 : -1;
        if (poll(fds, 2, timeout) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        if (fds[1].revents & POLLIN) {
            std::uint64_t v;
            [[maybe_unused]] const auto n = ::read(m_wakeFd, &v, sizeof v);
            if (!closed) {
                wantWrite = flushPending();
            }
        }

        if (!m_accepting.load()) closed = true;
        if (closed) {
            m_accepting.store(false);
            int status = 0;
            const pid_t child = m_pid.load();
            const pid_t reaped = waitpid(child, &status, WNOHANG);
            if (reaped == child || (reaped < 0 && errno == ECHILD)) {
                m_pid = -1;
                m_listener.ptyClosed(reaped == child ? status : -1);
                return;
            }
            continue;
        }

        if (fds[0].revents & POLLOUT) {
            wantWrite = flushPending();
        }
        if (fds[0].revents & POLLIN) {
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
        if (closed) m_accepting.store(false);
    }
    m_accepting.store(false);
}

} // namespace AfterglowPty
