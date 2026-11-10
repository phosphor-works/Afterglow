// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowPty/afterglowpty_export.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
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
    /// False: the child gets only the variables of environment, none of the
    /// parent's. The mux daemon runs a session's programs in the session's
    /// environment, which the client sent, not in its own.
    bool inheritEnvironment = true;
    /// Delay callbacks until the owner has published the returned object.
    bool deferStart = false;
    int cols = 80;
    int rows = 24;
    int pixelWidth = 0;
    int pixelHeight = 0;
    /// Standard input from a pipe that carries these bytes and then closes,
    /// in place of the terminal. Unset: the terminal.
    std::optional<std::string> input;
    /// Standard output to a pipe, which IPtyListener::ptyOutput reads, in
    /// place of the terminal. Either way the terminal stays the controlling
    /// terminal and standard error, so the program can still draw on
    /// /dev/tty: a list on stdin, the interface on /dev/tty, the answer on
    /// stdout.
    bool captureOutput = false;
    /// The file mode creation mask of the child: -1 keeps the parent's,
    /// else the child sets it before exec. The mux daemon's own mask is not
    /// the one of the user who started the session.
    int umask = -1;
    /// The soft RLIMIT_NOFILE of the child: -1 keeps the parent's, else the
    /// child sets its soft limit to this (at most the hard limit, which
    /// stays) before exec. The mux daemon raises its own soft limit, which
    /// programs that use select() must not get.
    long fileLimit = -1;
};

/// Sets @p name to @p value for the child, replacing every earlier set or unset of it.
AFTERGLOWPTY_EXPORT void setVariable(SpawnOptions& options, std::string name, std::string value);
/// Removes @p name from the child's environment, replacing every earlier set or unset of it.
AFTERGLOWPTY_EXPORT void unsetVariable(SpawnOptions& options, std::string name);

/// The fields of /proc/PID/stat after the command name that a terminal asks about.
struct ProcessStat
{
    char state = 0;
    long long parent = 0;
    long long group = 0;
    long long session = 0;
    long long terminal = 0;
    long long foregroundGroup = 0; ///< tpgid: the foreground process group of the controlling terminal
};

/// Parses the text of /proc/PID/stat. The command name, between the first
/// '(' and the last ')', can hold any byte, ')' and new lines included.
/// Empty when the text does not have the fields.
AFTERGLOWPTY_EXPORT std::optional<ProcessStat> parseProcessStat(std::string_view stat);
/// Reads the whole of /proc/@p pid/stat and parses it. Empty when the
/// process is gone or the file does not parse.
AFTERGLOWPTY_EXPORT std::optional<ProcessStat> processStat(pid_t pid);

/// Receives pty events on the pty's I/O thread.
class IPtyListener
{
public:
    virtual ~IPtyListener() = default;
    virtual void ptyData(std::string_view bytes) = 0;
    /// What the child wrote to its standard output, with
    /// SpawnOptions::captureOutput. What the child wrote before it exited
    /// arrives before ptyClosed. A background job that keeps the pipe after
    /// the child exits is not waited for.
    virtual void ptyOutput(std::string_view bytes) { (void)bytes; }
    /// The child exited, or the terminal can no longer be read. @p status is
    /// the waitpid() status, or -1 when it is unknown: the child was reaped
    /// elsewhere, or still ran when the terminal failed.
    virtual void ptyClosed(int status) = 0;
    /// Pty::write() refused bytes because its queue was full, and the queue
    /// has room again (at most half of Pty::MaxPendingInput waits), or
    /// write() refuses for good now (Pty::accepting()). On the I/O thread,
    /// without a lock of the Pty.
    virtual void ptyWritable() {}
};

/// A child process on a pseudo-terminal, with a dedicated I/O thread that
/// reads output and drains queued input without blocking the caller. The
/// terminal closes when the child exits, even while a background job it
/// left behind still holds the terminal.
class AFTERGLOWPTY_EXPORT Pty
{
public:
    /// The shell the child gets: SHELL as the child's environment will have
    /// it (@p options' overrides and unsets first, then the parent's unless
    /// the child does not inherit it), then the account's login shell,
    /// falling back to /bin/sh.
    static std::string defaultShell(const SpawnOptions& options = {});
    /// Takes @p options by value: move them in, and a large
    /// SpawnOptions::input is not copied.
    static std::unique_ptr<Pty> spawn(SpawnOptions options, IPtyListener& listener, std::string* error = nullptr);

    /// Returns at once: the child gets SIGHUP and, if it stays, SIGKILL on
    /// a background thread.
    ~Pty();

    Pty(const Pty&) = delete;
    Pty& operator=(const Pty&) = delete;

    /// The most input that waits for the child, in bytes.
    static constexpr std::size_t MaxPendingInput = std::size_t(32) << 20;

    /// Queues bytes for the child. Thread-safe; never blocks on the child.
    /// Entire writes are accepted atomically, up to MaxPendingInput pending.
    /// After a refusal for a full queue, the listener's ptyWritable() says
    /// when there is room again.
    [[nodiscard]] bool write(std::string_view bytes);
    /// Lifecycle operations belong to the owner thread. Cannot restart after stop.
    [[nodiscard]] bool start(std::string* error = nullptr);
    void stop();
    void resize(int cols, int rows, int pixelWidth, int pixelHeight);

    pid_t pid() const noexcept { return m_pid.load(); }
    /// Bytes queued for the child and not yet written. Thread-safe.
    std::size_t pendingInput() const;
    /// write() can still take bytes: false once the terminal's child side is
    /// gone or the Pty stopped, although the child may still run. Thread-safe.
    bool accepting() const noexcept { return m_accepting.load(); }
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
    /// Writes a batch of what is left of m_input to the child's stdin pipe;
    /// closes the pipe once it is all written or the child closed its end.
    void feedInput();
    /// Closes the child's stdin pipe and drops what it did not take.
    void closeInput();
    /// Reads a batch from the child's stdout pipe. False once the pipe is at
    /// its end or failed, and closed; true while more may come.
    bool readOutput(std::vector<char>& buffer);

    int m_master;
    int m_wakeFd;
    int m_pidFd; ///< readable when the child exits; -1 on kernels without pidfd_open
    int m_execFd = -1; ///< the exec report, when spawn() stopped waiting for it
    int m_inputFd = -1; ///< the write end of the child's stdin pipe, until it is fed
    std::string m_input; ///< what the stdin pipe carries
    std::size_t m_inputOffset = 0;
    int m_outputFd = -1; ///< the read end of the child's stdout pipe, until its end
    std::atomic<pid_t> m_pid;
    IPtyListener& m_listener;

    mutable std::mutex m_writeMutex;
    std::deque<std::string> m_pending;
    std::size_t m_pendingOffset = 0;
    std::size_t m_pendingBytes = 0;
    bool m_refused = false; ///< write() refused bytes since the queue last had room
    std::atomic<bool> m_accepting{true};

    std::atomic<bool> m_stop{false};
    std::thread m_thread;
};

} // namespace AfterglowPty
