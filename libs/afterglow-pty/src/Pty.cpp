// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowPty/Pty.h>

#include <cerrno>
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
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_xpixel = static_cast<unsigned short>(pw);
    ws.ws_ypixel = static_cast<unsigned short>(ph);
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
        bool overridden = false;
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

    int master = -1;
    const pid_t pid = forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) {
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

    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    fcntl(master, F_SETFD, FD_CLOEXEC);
    return std::unique_ptr<Pty>(new Pty(master, pid, listener));
}

Pty::Pty(int masterFd, pid_t pid, IPtyListener& listener)
    : m_master(masterFd)
    , m_wakeFd(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC))
    , m_pid(pid)
    , m_listener(listener)
{
    m_thread = std::thread([this] { run(); });
}

Pty::~Pty()
{
    m_stop.store(true);
    wake();
    if (m_thread.joinable()) {
        m_thread.join();
    }
    if (m_pid > 0) {
        int status = 0;
        if (waitpid(m_pid, &status, WNOHANG) == 0) {
            kill(m_pid, SIGHUP);
            // Give the child a moment to leave on SIGHUP before forcing it.
            for (int i = 0; i < 20; ++i) {
                if (waitpid(m_pid, &status, WNOHANG) != 0) {
                    break;
                }
                usleep(5000);
                if (i == 19) {
                    kill(m_pid, SIGKILL);
                    waitpid(m_pid, &status, 0);
                }
            }
        }
    }
    close(m_master);
    close(m_wakeFd);
}

void Pty::write(std::string_view bytes)
{
    if (bytes.empty()) {
        return;
    }
    {
        std::lock_guard lock(m_writeMutex);
        m_pending.append(bytes);
    }
    wake();
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
    const auto own = std::filesystem::read_symlink("/proc/" + std::to_string(m_pid) + "/cwd", ec);
    return ec ? std::string() : own.string();
}

std::string Pty::foregroundProcess() const
{
    const pid_t group = tcgetpgrp(m_master);
    if (group <= 0 || group == m_pid) {
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
    while (!m_pending.empty()) {
        const ssize_t n = ::write(m_master, m_pending.data(), m_pending.size());
        if (n > 0) {
            m_pending.erase(0, std::size_t(n));
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break; // EAGAIN: wait for POLLOUT; other errors surface as POLLHUP
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

        if (closed) {
            int status = 0;
            if (waitpid(m_pid, &status, WNOHANG) == m_pid) {
                m_pid = -1;
                m_listener.ptyClosed(status);
                return;
            }
            continue;
        }

        if (fds[0].revents & POLLOUT) {
            wantWrite = flushPending();
        }
        if (fds[0].revents & POLLIN) {
            // Drain everything available so one wake parses one large batch.
            for (;;) {
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
        } else if (fds[0].revents & (POLLHUP | POLLERR)) {
            closed = true;
        }
    }
}

} // namespace AfterglowPty
