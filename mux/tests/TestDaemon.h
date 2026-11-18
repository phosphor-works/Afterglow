// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// The test daemon of the mux tests: each test runs its own aglow-mux, with
// a runtime and a state directory of its own under /tmp and only the
// variables of an allow list (TestDaemon::cleanEnvironment()), so no test
// reaches the daemon, the Afterglow, the bus or the shell history of the user.

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
#include <array>
#include <cerrno>
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
#include <string_view>
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
        if (done())
            return true;
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

/// A program that keeps its session open until the test ends it: longer
/// than any test runs, so that no session ends by itself. The end of the
/// daemon (~TestDaemon) hangs it up.
inline const std::vector<std::string> Waiting = {"/bin/sh", "-c", "exec sleep 3600"};

/// A program that writes its environment to @p file, then waits as Waiting.
inline std::vector<std::string> writesEnvironment(const std::string& file)
{
    return {"/bin/sh", "-c", "env > '" + file + ".part' && mv '" + file + ".part' '" + file + "'; exec sleep 3600"};
}

/// The start of the name of the directory of each test daemon, before the
/// process ID of the test that owns it.
inline constexpr const char* DirectoryPrefix = "aglow-mux-";

/// Removes the directories of test daemons whose test is gone: one that
/// CTest killed, or that crashed, ran no ~TestDaemon. Only those of this
/// user, whose owner process no longer runs.
inline void removeDeadDirectories()
{
    std::error_code error;
    for (std::filesystem::directory_iterator it("/tmp", error), end; !error && it != end; it.increment(error)) {
        const std::filesystem::directory_entry& entry = *it;
        const std::string name = entry.path().filename().string();
        if (!name.starts_with(DirectoryPrefix))
            continue;
        const std::string rest = name.substr(std::strlen(DirectoryPrefix));
        const std::size_t dash = rest.find('-');
        if (dash == 0 || dash == std::string::npos || rest.find_first_not_of("0123456789") != dash)
            continue;
        const long owner = std::strtol(rest.c_str(), nullptr, 10);
        struct stat info{};
        if (owner <= 0 || ::lstat(entry.path().c_str(), &info) != 0 || !S_ISDIR(info.st_mode)
            || info.st_uid != ::getuid())
            continue;
        if (::kill(pid_t(owner), 0) == 0 || errno != ESRCH)
            continue;
        std::error_code ignored;
        std::filesystem::remove_all(entry.path(), ignored);
    }
}

class TestDaemon
{
public:
    TestDaemon()
    {
        removeDeadDirectories();
        // Under /tmp: TMPDIR can be too long for a socket path. The ID of
        // this process names the owner, for removeDeadDirectories().
        std::string dir = std::string("/tmp/") + DirectoryPrefix + std::to_string(::getpid()) + "-XXXXXX";
        if (!::mkdtemp(dir.data())) {
            ADD_FAILURE() << "cannot create a directory under /tmp: " << std::strerror(errno);
            return;
        }
        m_dir = dir;
        for (const std::string& sub : {runtime(), m_dir + "/home"}) {
            if (::mkdir(sub.c_str(), 0700) != 0)
                ADD_FAILURE() << "cannot create " << sub << ": " << std::strerror(errno);
        }
        m_environment = cleanEnvironment();
        m_callSocket = socket();
    }
    ~TestDaemon()
    {
        for (const pid_t pid : {m_pid, m_other}) {
            if (pid <= 0)
                continue;
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
        }
        std::error_code ignored;
        if (!m_dir.empty())
            std::filesystem::remove_all(m_dir, ignored);
    }

    const std::string& dir() const
    {
        return m_dir;
    }
    std::string runtime() const
    {
        return m_dir + "/run";
    }
    std::string socket() const
    {
        return runtime() + "/aglow-mux/socket";
    }
    /// XDG_STATE_HOME of the daemon and of the clients that find it.
    std::string stateHome() const
    {
        return m_dir + "/state";
    }
    std::string stateDirectory() const
    {
        return stateHome() + "/aglow-mux";
    }
    std::string stateSocket() const
    {
        return stateDirectory() + "/" + AfterglowProto::muxStateSocketName();
    }
    pid_t pid() const
    {
        return m_pid;
    }
    /// The socket that call() connects to.
    void callOn(std::string path)
    {
        m_callSocket = std::move(path);
    }

    /// The variables of this process that the daemon and its sessions get:
    /// an allow list, so that no variable leads to the user's Afterglow, mux
    /// daemon, bus, display or tmux (AFTERGLOW_*, AGLOW_MUX_REPORT_FD,
    /// DBUS_SESSION_BUS_ADDRESS, WAYLAND_DISPLAY, TMUX), no shell of a test
    /// reads or writes the user's history or startup files (HISTFILE,
    /// PROMPT_COMMAND, ENV, BASH_ENV), and a failure prints no secret into
    /// the log. Then the test's directories, and /bin/sh as the login shell.
    static bool allowed(std::string_view name)
    {
        static constexpr std::array<std::string_view, 8> Names{"PATH", "LANG",    "LANGUAGE", "TZ",
                                                               "USER", "LOGNAME", "TMPDIR",   "LD_LIBRARY_PATH"};
        // The options of the sanitizers (tools/sanitizers): ASAN_OPTIONS and the others.
        return std::find(Names.begin(), Names.end(), name) != Names.end() || name.starts_with("LC_")
            || (name.ends_with("SAN_OPTIONS") && name.size() > 11);
    }
    std::vector<std::pair<std::string, std::string>> cleanEnvironment() const
    {
        std::vector<std::pair<std::string, std::string>> env;
        for (char** e = environ; *e; ++e) {
            const std::string entry(*e);
            const auto eq = entry.find('=');
            if (eq == std::string::npos)
                continue;
            const std::string name = entry.substr(0, eq);
            if (allowed(name))
                env.emplace_back(name, entry.substr(eq + 1));
        }
        env.emplace_back("XDG_RUNTIME_DIR", runtime());
        // The daemon listens under the state directory too: never the user's.
        env.emplace_back("XDG_STATE_HOME", stateHome());
        env.emplace_back("HOME", m_dir + "/home");
        // Not the user's login shell, which reads the user's startup files.
        env.emplace_back("SHELL", "/bin/sh");
        return env;
    }

    /// Starts the daemon and waits until it answers. With @p keep, it gets a
    /// session named "keep" that runs until the test ends: without a session
    /// and a connection, the daemon ends.
    bool start(bool keep = true, bool ignoreHangup = false)
    {
        if (spawnDaemon(ignoreHangup) <= 0)
            return false;
        const bool answers = waitFor([this] {
            IpcConnection connection;
            IpcConnection::Error error;
            return connection.connect(m_callSocket, &error, 100);
        });
        return answers && (!keep || newSession(options(Waiting, "keep")));
    }

    /// The soft RLIMIT_NOFILE that the daemon starts with, below the hard one.
    void startFileLimit(rlim_t soft)
    {
        m_startFileLimit = soft;
    }

    /// A variable that the daemon has and the sessions have not, in place
    /// of one of the same name.
    void daemonOnly(std::string name, std::string value)
    {
        m_daemonOnly.emplace_back(std::move(name), std::move(value));
    }

    /// Starts a daemon that this test kills at its end whatever happens: the
    /// test's daemon, or with @p other a second one. With @p ignoreHangup
    /// it starts with SIGHUP ignored, as under nohup. Its stdin is /dev/null
    /// and its stdout and stderr go to daemon.err: it holds no pipe of
    /// ctest, and it ends with the thread that started it, the test's main
    /// thread (PR_SET_PDEATHSIG). -1 when it cannot start, or when the test
    /// has no directory: the daemon would get XDG_RUNTIME_DIR=/run.
    /// @p report: the write end of a pipe that the daemon gets as its
    /// report descriptor (AfterglowMux::ReportFdVariable), as from
    /// `afterglow mux new`; -1 for none.
    pid_t spawnDaemon(bool ignoreHangup = false, bool other = false, int report = -1)
    {
        if (m_dir.empty()) {
            ADD_FAILURE() << "the test has no directory for a daemon";
            return -1;
        }
        // A second daemon in one place would leave the first to nobody.
        if ((other ? m_other : m_pid) > 0) {
            ADD_FAILURE() << "a daemon of this test runs already";
            return -1;
        }
        std::vector<std::string> store;
        for (const auto& [name, value] : m_environment) {
            const bool replaced = std::any_of(m_daemonOnly.begin(), m_daemonOnly.end(), [&](const auto& only) {
                return only.first == name;
            });
            if (!replaced)
                store.push_back(name + "=" + value);
        }
        for (const auto& [name, value] : m_daemonOnly)
            store.push_back(name + "=" + value);
        if (report >= 0)
            store.push_back(std::string(AfterglowMux::ReportFdVariable) + "=3");
        std::vector<char*> envp;
        for (auto& entry : store)
            envp.push_back(entry.data());
        envp.push_back(nullptr);
        const std::string err = m_dir + "/daemon.err";
        const std::string script = std::string("trap '' HUP; exec '") + AFTERGLOW_MUX + "'";
        std::vector<char*> argv;
        if (ignoreHangup)
            argv = {const_cast<char*>("/bin/sh"), const_cast<char*>("-c"), const_cast<char*>(script.c_str()), nullptr};
        else
            argv = {const_cast<char*>(AFTERGLOW_MUX), nullptr};
        const pid_t parent = ::getpid();
        const pid_t pid = ::fork();
        if (pid < 0)
            return -1;
        if (pid == 0) {
            ::prctl(PR_SET_PDEATHSIG, SIGKILL);
            if (::getppid() != parent)
                ::_exit(127);
            // The report goes to 3 first, from above 10: the files opened
            // below then never take 3, and the report is never one of them.
            if (report >= 0) {
                const int high = ::fcntl(report, F_DUPFD, 10);
                if (high < 0 || ::dup2(high, 3) != 3)
                    ::_exit(127);
            }
            const int in = ::open("/dev/null", O_RDONLY);
            const int out = ::open(err.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
            if (in < 0 || out < 0)
                ::_exit(127);
            if (rlimit limit{}; m_startFileLimit > 0 && ::getrlimit(RLIMIT_NOFILE, &limit) == 0) {
                limit.rlim_cur = m_startFileLimit;
                ::setrlimit(RLIMIT_NOFILE, &limit);
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

    std::string errors() const
    {
        return readFile(m_dir + "/daemon.err");
    }

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
            if (std::chrono::steady_clock::now() >= deadline)
                return std::nullopt;
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
        std::optional<Json::Value> result;
        if (connection.connect(m_callSocket, &e, 2000))
            result = connection.call(target, fn, std::move(args), 5000, &e);
        // lastError() describes this call, also when it got no connection.
        if (&e != &m_lastError)
            m_lastError = e;
        return result;
    }

    /// Why the last call failed.
    std::string lastError() const
    {
        return m_lastError.code + ": " + m_lastError.message + " " + errors();
    }

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

    /// The sessions of the daemon. A call that fails is a failure of the
    /// test: a daemon that crashed has no sessions either, and a check that
    /// a session went would pass.
    std::vector<SessionInfo> sessions()
    {
        std::vector<SessionInfo> list;
        const auto result = call(Fn::ListSessions, {});
        if (!result) {
            ADD_FAILURE() << "listSessions failed: " << lastError();
            return list;
        }
        if (!result->isArray()) {
            ADD_FAILURE() << "listSessions answered no list: " << Json::write(*result);
            return list;
        }
        for (const Json::Value& item : result->toArray()) {
            auto info = sessionInfo(item);
            EXPECT_TRUE(info) << Json::write(item);
            if (info)
                list.push_back(std::move(*info));
        }
        return list;
    }

    /// The sockets of this daemon, as a client in the daemon's environment
    /// looks them up: never the user's.
    AfterglowProto::MuxSockets sockets() const
    {
        return AfterglowProto::muxSockets([this](const char* name) -> const char* {
            for (const auto& [key, value] : m_environment) {
                if (key == name)
                    return value.c_str();
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
        if (m_fd >= 0)
            ::close(m_fd);
    }
    bool connected() const
    {
        return m_fd >= 0;
    }
    bool send(std::string_view text)
    {
        while (!text.empty()) {
            const ssize_t n = ::send(m_fd, text.data(), text.size(), MSG_NOSIGNAL);
            if (n <= 0)
                return false;
            text.remove_prefix(std::size_t(n));
        }
        return true;
    }
    /// Ends this side of the connection, as a client that sent its last
    /// call does: the daemon reads the end, and can still answer.
    bool endSending()
    {
        return ::shutdown(m_fd, SHUT_WR) == 0;
    }
    /// A line() read the end of the connection.
    bool ended() const
    {
        return m_ended;
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
            if (::poll(&p, 1, 5000) <= 0)
                return {};
            char buffer[4096];
            const ssize_t n = ::read(m_fd, buffer, sizeof buffer);
            if (n == 0)
                m_ended = true;
            if (n <= 0)
                return {};
            m_buffer.append(buffer, std::size_t(n));
        }
    }

private:
    int m_fd = -1;
    std::string m_buffer;
    bool m_ended = false;
};

inline std::vector<std::string> names(const std::vector<SessionInfo>& sessions)
{
    std::vector<std::string> list;
    for (const auto& s : sessions)
        list.push_back(s.name);
    return list;
}

/// The options of a session that runs an interactive sh, with a plain
/// prompt and no startup file.
inline SessionOptions shellOptions(const TestDaemon& daemon, const std::string& name)
{
    SessionOptions o = daemon.options({"/bin/sh"}, name);
    o.launch.environment.emplace_back("PS1", "$ ");
    o.launch.environment.emplace_back("ENV", "/dev/null");
    return o;
}

/// A session that runs an interactive sh, with a plain prompt.
inline bool shellSession(TestDaemon& daemon, const std::string& name)
{
    return bool(daemon.newSession(shellOptions(daemon, name)));
}

/// The session @p name of the daemon, if it has one.
inline std::optional<SessionInfo> sessionNamed(TestDaemon& daemon, const std::string& name)
{
    for (SessionInfo& s : daemon.sessions()) {
        if (s.name == name)
            return std::move(s);
    }
    return std::nullopt;
}

/// The lowest ID of a pane that session @p name holds, as the daemon's
/// paneSession call says, up to @p last; 0 for none. The daemon numbers its
/// panes from 1, so a test needs no guess of which ID a session's pane got.
inline std::uint32_t firstPaneOf(TestDaemon& daemon, const std::string& name, std::uint32_t last = 64)
{
    for (std::uint32_t id = 1; id <= last; ++id) {
        IpcConnection::Error error;
        const auto session = daemon.call(Fn::PaneSession, Json::Array{std::int64_t(id)}, &error);
        if (session && session->toString() == name)
            return id;
    }
    return 0;
}

/// Whether process @p pid runs: neither gone nor a zombie, which kill(pid, 0)
/// still finds.
inline bool running(pid_t pid)
{
    const std::string stat = readFile("/proc/" + std::to_string(pid) + "/stat");
    return !stat.empty() && stat.find(") Z ") == std::string::npos;
}

/// A Python program that puts the terminal in raw mode, writes @p query
/// and saves what the terminal answers, up to @p end, in @p file. With
/// @p afterByte, it writes "go?" once in raw mode (which drops the input
/// that came before) and waits for one byte of input before it asks: a
/// window that attaches first then gets the query. It waits for the answer
/// longer than a test's own waits, so that the test sets the time limit.
inline std::vector<std::string> ask(const std::string& query, const std::string& end, const std::string& file,
                                    bool afterByte = false)
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
        std::string pattern = (std::filesystem::temp_directory_path() / "aglow-mux-test-XXXXXX").string();
        if (::mkdtemp(pattern.data()))
            path = pattern;
    }
    ~TempDir()
    {
        std::error_code ignored;
        if (!path.empty())
            std::filesystem::remove_all(path, ignored);
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
    return std::string("\x89PNG\r\n\x1a\n", 8) + chunk("IHDR", header)
        + chunk("IDAT", AfterglowProto::zlibCompress(rows)) + chunk("IEND", {});
}

} // namespace MuxTest
