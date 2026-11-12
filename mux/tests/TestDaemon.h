// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// The test daemon of the mux tests: each test runs its own afterglow-mux, with
// a runtime and a state directory of its own under /tmp and no AFTERGLOW_*
// variables, so no test reaches the daemon of the user.

#pragma once

#include "MuxProtocol.h"

#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/IpcServer.h>
#include <AfterglowProto/Run.h>
#include <AfterglowProto/Socket.h>
#include <AfterglowProto/Zlib.h>
#include <AfterglowPty/CloseFrom.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

// Named, not anonymous: each test file uses some of these. Nothing here is
// global, so a test file of another suite can include it too.
namespace MuxTest {

using namespace AfterglowMux;
using namespace std::chrono_literals;
namespace Json = AfterglowProto::Json;
using AfterglowProto::IpcConnection;
namespace IpcErrorCode = AfterglowProto::IpcErrorCode;

inline std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

/// Waits up to @p wait (five seconds by default) for @p done.
inline bool waitFor(const std::function<bool()>& done, std::chrono::milliseconds wait = 5s)
{
    const auto deadline = std::chrono::steady_clock::now() + wait;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return done();
}

inline bool pythonFound()
{
    return std::system("command -v python3 >/dev/null 2>&1") == 0;
}

inline bool exists(const std::string& path)
{
    struct stat info{};
    return ::lstat(path.c_str(), &info) == 0;
}

/// A program that keeps its session open until the test ends it.
inline const std::vector<std::string> Waiting = {"/bin/sh", "-c", "exec sleep 60"};

/// A program that writes its environment to @p file, then waits.
inline std::vector<std::string> writesEnvironment(const std::string& file)
{
    return {"/bin/sh", "-c", "env > '" + file + ".part' && mv '" + file + ".part' '" + file + "'; exec sleep 60"};
}

class TestDaemon
{
public:
    TestDaemon()
    {
        // Under /tmp: TMPDIR can be too long for a socket path.
        char dir[] = "/tmp/afterglow-mux-XXXXXX";
        if (!::mkdtemp(dir)) {
            ADD_FAILURE() << "cannot create a directory under /tmp: " << std::strerror(errno);
            return;
        }
        m_dir = dir;
        for (const std::string& sub : {runtime(), m_dir + "/home"}) {
            if (::mkdir(sub.c_str(), 0700) != 0) ADD_FAILURE() << "cannot create " << sub << ": " << std::strerror(errno);
        }
        m_environment = cleanEnvironment();
        m_callSocket = socket();
    }
    ~TestDaemon()
    {
        for (const pid_t pid : {m_pid, m_other}) {
            if (pid <= 0) continue;
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
        }
        std::error_code ignored;
        if (!m_dir.empty()) std::filesystem::remove_all(m_dir, ignored);
    }

    const std::string& dir() const { return m_dir; }
    std::string runtime() const { return m_dir + "/run"; }
    std::string socket() const { return runtime() + "/afterglow-mux/socket"; }
    /// XDG_STATE_HOME of the daemon and of the clients that find it.
    std::string stateHome() const { return m_dir + "/state"; }
    std::string stateDirectory() const { return stateHome() + "/afterglow-mux"; }
    std::string stateSocket() const { return stateDirectory() + "/" + AfterglowProto::muxStateSocketName(); }
    pid_t pid() const { return m_pid; }
    /// The socket that call() connects to.
    void callOn(std::string path) { m_callSocket = std::move(path); }

    /// The environment of this process, without the variables that lead to
    /// the user's Afterglow, and with the test's directories.
    std::vector<std::pair<std::string, std::string>> cleanEnvironment() const
    {
        std::vector<std::pair<std::string, std::string>> env;
        for (char** e = environ; *e; ++e) {
            const std::string entry(*e);
            const auto eq = entry.find('=');
            const std::string name = entry.substr(0, eq);
            if (name.starts_with("AFTERGLOW_") || name == "XDG_RUNTIME_DIR" || name == "XDG_STATE_HOME" || name == "HOME" || name == "TMUX"
                || name == "TMUX_PANE") {
                continue;
            }
            env.emplace_back(name, eq == std::string::npos ? "" : entry.substr(eq + 1));
        }
        env.emplace_back("XDG_RUNTIME_DIR", runtime());
        // The daemon listens under the state directory too: never the user's.
        env.emplace_back("XDG_STATE_HOME", stateHome());
        env.emplace_back("HOME", m_dir + "/home");
        return env;
    }

    /// Starts the daemon and waits until it answers. With @p keep, it gets a
    /// session named "keep" that runs until the test ends: without a session
    /// and a connection, the daemon ends.
    bool start(bool keep = true, bool ignoreHangup = false)
    {
        m_pid = spawnDaemon(ignoreHangup);
        if (m_pid <= 0) return false;
        const bool answers = waitFor([this] {
            IpcConnection connection;
            IpcConnection::Error error;
            return connection.connect(m_callSocket, &error, 100);
        });
        return answers && (!keep || newSession(options(Waiting, "keep")));
    }

    /// The soft RLIMIT_NOFILE that the daemon starts with, below the hard one.
    void startFileLimit(rlim_t soft) { m_startFileLimit = soft; }

    /// A variable that the daemon has and the sessions have not.
    void daemonOnly(std::string name, std::string value) { m_daemonOnly.emplace_back(std::move(name), std::move(value)); }

    /// Starts a daemon that this test kills at its end whatever happens: the
    /// test's daemon, or with @p other a second one. With @p ignoreHangup
    /// it starts with SIGHUP ignored, as under nohup. Its stdin is /dev/null
    /// and its stdout and stderr go to daemon.err: it holds no pipe of
    /// ctest, and it ends with this process (PR_SET_PDEATHSIG).
    /// @p report: the write end of a pipe that the daemon gets as its
    /// report descriptor (AfterglowMux::ReportFdVariable), as from
    /// `afterglow mux new`; -1 for none.
    pid_t spawnDaemon(bool ignoreHangup = false, bool other = false, int report = -1)
    {
        std::vector<std::string> store;
        for (const auto& [name, value] : m_environment) store.push_back(name + "=" + value);
        for (const auto& [name, value] : m_daemonOnly) store.push_back(name + "=" + value);
        if (report >= 0) store.push_back(std::string(AfterglowMux::ReportFdVariable) + "=3");
        std::vector<char*> envp;
        for (auto& entry : store) envp.push_back(entry.data());
        envp.push_back(nullptr);
        const std::string err = m_dir + "/daemon.err";
        const std::string script = std::string("trap '' HUP; exec '") + AFTERGLOW_MUX + "'";
        std::vector<char*> argv;
        if (ignoreHangup) argv = {const_cast<char*>("/bin/sh"), const_cast<char*>("-c"), const_cast<char*>(script.c_str()), nullptr};
        else argv = {const_cast<char*>(AFTERGLOW_MUX), nullptr};
        const pid_t parent = ::getpid();
        const pid_t pid = ::fork();
        if (pid < 0) return -1;
        if (pid == 0) {
            ::prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (::getppid() != parent) ::_exit(127);
            const int in = ::open("/dev/null", O_RDONLY);
            const int out = ::open(err.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
            if (in < 0 || out < 0) ::_exit(127);
            if (rlimit limit{}; m_startFileLimit > 0 && ::getrlimit(RLIMIT_NOFILE, &limit) == 0) {
                limit.rlim_cur = m_startFileLimit;
                ::setrlimit(RLIMIT_NOFILE, &limit);
            }
            if (report >= 0 && report != 3) {
                ::dup2(report, 3);
            } else if (report == 3) {
                ::fcntl(3, F_SETFD, 0);
            }
            ::dup2(in, 0);
            ::dup2(out, 1);
            ::dup2(out, 2);
            AfterglowPty::closeFrom(report >= 0 ? 4 : 3);
            ::execve(argv[0], argv.data(), envp.data());
            ::_exit(127);
        }
        (other ? m_other : m_pid) = pid;
        return pid;
    }

    std::string errors() const { return readFile(m_dir + "/daemon.err"); }

    /// Whether the daemon still runs. It is not reaped here: a later
    /// waitExit() still gets its status.
    bool alive() const
    {
        siginfo_t info{};
        return m_pid > 0 && ::waitid(P_PID, id_t(m_pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == 0;
    }

    /// The exit status, 128 + the signal for one that ended it, once the
    /// daemon ended within @p timeout; else nullopt, and it still runs.
    std::optional<int> waitExit(std::chrono::milliseconds timeout = 5s, bool other = false)
    {
        pid_t& pid = other ? m_other : m_pid;
        // No daemon: waitpid(-1) would reap any child of the test.
        if (pid <= 0) {
            ADD_FAILURE() << "no daemon to wait for";
            return std::nullopt;
        }
        int status = 0;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (::waitpid(pid, &status, WNOHANG) != pid) {
            if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
            std::this_thread::sleep_for(20ms);
        }
        pid = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }

    /// A call on its own connection, as `afterglow mux` makes them.
    std::optional<Json::Value> call(const std::string& fn, Json::Array args, IpcConnection::Error* error = nullptr,
                                    const std::string& target = Target)
    {
        IpcConnection connection;
        IpcConnection::Error& e = error ? *error : m_lastError;
        e = {};
        if (!connection.connect(m_callSocket, &e, 2000)) return std::nullopt;
        auto result = connection.call(target, fn, std::move(args), 5000, &e);
        if (&e != &m_lastError) m_lastError = e;
        return result;
    }

    /// Why the last call failed.
    std::string lastError() const { return m_lastError.code + ": " + m_lastError.message + " " + errors(); }

    SessionOptions options(std::vector<std::string> argv, std::string name = {}) const
    {
        SessionOptions o;
        o.name = std::move(name);
        o.environment = m_environment;
        o.launch.argv = std::move(argv);
        o.launch.workingDirectory = m_dir;
        return o;
    }

    std::optional<Json::Value> newSession(const SessionOptions& o, IpcConnection::Error* error = nullptr)
    {
        return call(Fn::NewSession, Json::Array{ProtocolVersion, toJson(o)}, error);
    }

    std::vector<SessionInfo> sessions()
    {
        std::vector<SessionInfo> list;
        const auto result = call(Fn::ListSessions, {});
        if (!result) return list;
        for (const Json::Value& item : result->toArray()) {
            auto info = sessionInfo(item);
            EXPECT_TRUE(info) << Json::write(item);
            if (info) list.push_back(std::move(*info));
        }
        return list;
    }

    /// The sockets of this daemon, as a client in the daemon's environment
    /// looks them up: never the user's.
    AfterglowProto::MuxSockets sockets() const
    {
        return AfterglowProto::muxSockets([this](const char* name) -> const char* {
            for (const auto& [key, value] : m_environment) {
                if (key == name) return value.c_str();
            }
            return nullptr;
        });
    }

private:
    std::string m_dir;
    std::vector<std::pair<std::string, std::string>> m_environment;
    std::vector<std::pair<std::string, std::string>> m_daemonOnly;
    std::string m_callSocket;
    rlim_t m_startFileLimit = 0;
    pid_t m_pid = -1;
    pid_t m_other = -1;
    IpcConnection::Error m_lastError;
};

/// A raw connection to @p path, for lines that IpcConnection does not send.
/// Its sends and reads time out after five seconds.
class RawClient
{
public:
    explicit RawClient(const std::string& path)
    {
        m_fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::snprintf(address.sun_path, sizeof address.sun_path, "%s", path.c_str());
        const timeval timeout{5, 0};
        ::setsockopt(m_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
        if (::connect(m_fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
            ::close(m_fd);
            m_fd = -1;
        }
    }
    ~RawClient()
    {
        if (m_fd >= 0) ::close(m_fd);
    }
    bool connected() const { return m_fd >= 0; }
    bool send(std::string_view text)
    {
        while (!text.empty()) {
            const ssize_t n = ::send(m_fd, text.data(), text.size(), MSG_NOSIGNAL);
            if (n <= 0) return false;
            text.remove_prefix(std::size_t(n));
        }
        return true;
    }
    /// The next line, without its '\n'; empty after five seconds or at the end.
    std::string line()
    {
        while (true) {
            if (const auto end = m_buffer.find('\n'); end != std::string::npos) {
                std::string line = m_buffer.substr(0, end);
                m_buffer.erase(0, end + 1);
                return line;
            }
            pollfd p{m_fd, POLLIN, 0};
            if (::poll(&p, 1, 5000) <= 0) return {};
            char buffer[4096];
            const ssize_t n = ::read(m_fd, buffer, sizeof buffer);
            if (n <= 0) return {};
            m_buffer.append(buffer, std::size_t(n));
        }
    }

private:
    int m_fd = -1;
    std::string m_buffer;
};

inline std::vector<std::string> names(const std::vector<SessionInfo>& sessions)
{
    std::vector<std::string> list;
    for (const auto& s : sessions) list.push_back(s.name);
    return list;
}

/// A session that runs an interactive sh, with a plain prompt.
inline bool shellSession(TestDaemon& daemon, const std::string& name)
{
    SessionOptions o = daemon.options({"/bin/sh"}, name);
    o.launch.environment.emplace_back("PS1", "$ ");
    o.launch.environment.emplace_back("ENV", "/dev/null");
    return bool(daemon.newSession(o));
}

/// The session @p name of the daemon, if it has one.
inline std::optional<SessionInfo> sessionNamed(TestDaemon& daemon, const std::string& name)
{
    for (SessionInfo& s : daemon.sessions()) {
        if (s.name == name) return std::move(s);
    }
    return std::nullopt;
}

/// A Python program that puts the terminal in raw mode, writes @p query
/// and saves what the terminal answers, up to @p end, in @p file. With
/// @p afterByte, it writes "go?" once in raw mode (which drops the input
/// that came before) and waits for one byte of input before it asks: a
/// window that attaches first then gets the query. It waits for the answer
/// longer than a test's own waits, so that the test sets the time limit.
inline std::vector<std::string> ask(const std::string& query, const std::string& end, const std::string& file, bool afterByte = false)
{
    const std::string script = R"(import os, select, sys, tty
fd = sys.stdin.fileno()
tty.setraw(fd)
if sys.argv[4] == '1':
    os.write(1, b'go?')
    os.read(fd, 1)
os.write(1, sys.argv[1].encode().decode('unicode_escape').encode('latin-1'))
end = sys.argv[2].encode().decode('unicode_escape').encode('latin-1')
data = b''
while not data.endswith(end):
    ready, _, _ = select.select([fd], [], [], 30)
    if not ready:
        break
    data += os.read(fd, 4096)
with open(sys.argv[3] + '.part', 'wb') as f:
    f.write(data)
os.rename(sys.argv[3] + '.part', sys.argv[3])
)";
    return {"python3", "-I", "-c", script, query, end, file, afterByte ? "1" : "0"};
}

/// A directory of its own under the temporary directory, removed however
/// the test ends. Empty when it cannot be made.
struct TempDir
{
    std::filesystem::path path;
    TempDir()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "afterglow-mux-test-XXXXXX").string();
        if (::mkdtemp(pattern.data())) path = pattern;
    }
    ~TempDir()
    {
        std::error_code ignored;
        if (!path.empty()) std::filesystem::remove_all(path, ignored);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    /// An executable script named @p name that runs @p body. Empty where
    /// the directory runs no program (a file system mounted noexec).
    std::string script(const std::string& name, const std::string& body) const
    {
        const std::string file = (path / name).string();
        std::ofstream(file) << "#!/bin/sh\n" << body << "\n";
        return ::chmod(file.c_str(), 0700) == 0 && ::access(file.c_str(), X_OK) == 0 ? file : std::string();
    }
};

/// Whether the kernel lists the children of a thread, which the tests of
/// the image decoder's processes read (CONFIG_PROC_CHILDREN).
inline bool childrenListed()
{
    return std::filesystem::exists("/proc/self/task/" + std::to_string(::gettid()) + "/children");
}

/// A grey PNG of @p width by @p height, with the CRCs that libpng checks.
inline std::string greyPng(std::uint32_t width, std::uint32_t height)
{
    const auto be = [](std::uint32_t v) {
        return std::string{char(v >> 24), char(v >> 16), char(v >> 8), char(v)};
    };
    const auto chunk = [&be](std::string_view type, const std::string& data) {
        const std::string body = std::string(type) + data;
        return be(std::uint32_t(data.size())) + body + be(AfterglowProto::crc32Of(body));
    };
    std::string rows;
    rows.reserve(std::size_t(height) * (width + 1));
    for (std::uint32_t y = 0; y < height; ++y) {
        rows += '\0';
        rows.append(width, char(y));
    }
    const std::string header = be(width) + be(height) + std::string{8, 0, 0, 0, 0};
    return std::string("\x89PNG\r\n\x1a\n", 8) + chunk("IHDR", header) + chunk("IDAT", AfterglowProto::zlibCompress(rows))
        + chunk("IEND", {});
}

} // namespace MuxTest
