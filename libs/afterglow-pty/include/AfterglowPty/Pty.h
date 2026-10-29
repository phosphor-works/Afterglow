// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowPty/afterglowpty_export.h>

#include <atomic>
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
    /// Variables set (or overridden) in the child's environment.
    std::vector<std::pair<std::string, std::string>> environment;
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
    /// The child closed the terminal. @p status is the waitpid() status, or -1
    /// if the child had not been reaped yet.
    virtual void ptyClosed(int status) = 0;
};

/// A child process on a pseudo-terminal, with a dedicated I/O thread that
/// reads output and drains queued input without blocking the caller.
class AFTERGLOWPTY_EXPORT Pty
{
public:
    static std::unique_ptr<Pty> spawn(const SpawnOptions& options, IPtyListener& listener, std::string* error = nullptr);

    ~Pty();

    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;

    /// Queues bytes for the child. Thread-safe; never blocks on the child.
    void write(std::string_view bytes);
    void resize(int cols, int rows, int pixelWidth, int pixelHeight);

    pid_t pid() const noexcept { return m_pid; }
    /// The child's current working directory, read from /proc.
    std::string currentWorkingDirectory() const;
    /// The name of the job in the foreground when it is not the child itself
    /// (a program the shell is running); empty while the shell has the terminal.
    std::string foregroundProcess() const;

private:
    Pty(int masterFd, pid_t pid, IPtyListener& listener);
    void run();
    void wake();
    bool flushPending();

    int m_master;
    int m_wakeFd;
    pid_t m_pid;
    IPtyListener& m_listener;

    std::mutex m_writeMutex;
    std::string m_pending;

    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

} // namespace AfterglowPty
