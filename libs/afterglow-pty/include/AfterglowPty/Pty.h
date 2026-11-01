// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowPty/afterglowpty_export.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/types.h>

namespace AfterglowPty {

struct SpawnOptions
{
    /// Program and arguments. Empty runs the user's shell.
    std::vector<std::string> argv;
    std::string workingDirectory;
    /// Variables set (or overridden) in the child's environment. The last
    /// value of a name wins.
    std::vector<std::pair<std::string, std::string>> environment;
    std::vector<std::string> unsetEnvironment;
    /// Delay callbacks until the owner has published the returned object.
    bool deferStart = false;
    int cols = 80;
    int rows = 24;
    int pixelWidth = 0;
    int pixelHeight = 0;
};

/// Receives pty events on the pty's I/O thread.
class IPtyListener
{
public:
    virtual ~IPtyListener() = default;
    virtual void ptyData(std::string_view bytes) = 0;
    /// The child exited, or the terminal can no longer be read. @p status is
    /// the waitpid() status, or -1 when it is unknown: the child was reaped
    /// elsewhere, or still ran when the terminal failed.
    virtual void ptyClosed(int status) = 0;
};

/// A child process on a pseudo-terminal, with a dedicated I/O thread that
/// reads output and drains queued input without blocking the caller. The
/// terminal closes when the child exits, even while a background job it
/// left behind still holds the terminal.
class AFTERGLOWPTY_EXPORT Pty
{
public:
    /// The shell the child gets: SHELL as the child's environment will have
    /// it (@p options' overrides and unsets first), then the account's login
    /// shell, falling back to /bin/sh.
    static std::string defaultShell(const SpawnOptions& options = {});
    static std::unique_ptr<Pty> spawn(const SpawnOptions& options, IPtyListener& listener, std::string* error = nullptr);

    /// Returns at once: the child gets SIGHUP and, if it stays, SIGKILL on
    /// a background thread.
    ~Pty();

    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;

    /// Queues bytes for the child. Thread-safe; never blocks on the child.
    /// Entire writes are accepted atomically, up to 32 MiB pending.
    [[nodiscard]] bool write(std::string_view bytes);
    /// Lifecycle operations belong to the owner thread. Cannot restart after stop.
    [[nodiscard]] bool start(std::string* error = nullptr);
    void stop();
    void resize(int cols, int rows, int pixelWidth, int pixelHeight);

    pid_t pid() const noexcept { return m_pid.load(); }
    /// Bytes queued for the child and not yet written. Thread-safe.
    std::size_t pendingInput() const;
    /// The foreground job's working directory, else the child's, read from
    /// /proc. Empty when neither is a directory that exists.
    std::string currentWorkingDirectory() const;
    /// The name of the job in the foreground when it is not the child itself
    /// (a program the shell is running); empty while the shell has the terminal.
    std::string foregroundProcess() const;

private:
    Pty(int masterFd, int wakeFd, int pidFd, pid_t pid, IPtyListener& listener);
    void run();
    void wake();
    bool flushPending();

    int m_master;
    int m_wakeFd;
    int m_pidFd; ///< readable when the child exits; -1 on kernels without pidfd_open
    int m_execFd = -1; ///< the exec report, when spawn() stopped waiting for it
    std::atomic<pid_t> m_pid;
    IPtyListener& m_listener;

    mutable std::mutex m_writeMutex;
    std::deque<std::string> m_pending;
    std::size_t m_pendingOffset = 0;
    std::size_t m_pendingBytes = 0;
    std::atomic<bool> m_accepting{true};

    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

} // namespace AfterglowPty
