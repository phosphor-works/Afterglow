// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// Each test runs its own afterglow-mux, with a runtime directory of its own
// and no AFTERGLOW_* variables, so no test reaches the daemon of the user.

#include "MuxProtocol.h"

#include <AfterglowProto/Ipc.h>

#include <gtest/gtest.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

using namespace AfterglowMux;
using namespace std::chrono_literals;
namespace Json = AfterglowProto::Json;
using AfterglowProto::IpcConnection;

namespace {

std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

/// Waits up to five seconds for @p done.
bool waitFor(const std::function<bool()>& done)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return done();
}

bool pythonFound()
{
    return std::system("command -v python3 >/dev/null 2>&1") == 0;
}

/// A program that keeps its session open until the test ends it.
const std::vector<std::string> Waiting = {"/bin/sh", "-c", "exec sleep 60"};

class TestDaemon
{
public:
    TestDaemon()
    {
        // Under /tmp: TMPDIR can be too long for a socket path.
        char dir[] = "/tmp/afterglow-mux-XXXXXX";
        if (!::mkdtemp(dir)) return;
        m_dir = dir;
        ::mkdir(runtime().c_str(), 0700);
        m_environment = cleanEnvironment();
    }
    ~TestDaemon()
    {
        if (m_pid > 0) {
            ::kill(m_pid, SIGTERM);
            int status = 0;
            ::waitpid(m_pid, &status, 0);
        }
        std::error_code ignored;
        if (!m_dir.empty()) std::filesystem::remove_all(m_dir, ignored);
    }

    const std::string& dir() const { return m_dir; }
    std::string runtime() const { return m_dir + "/run"; }
    std::string socket() const { return runtime() + "/afterglow-mux.sock"; }
    pid_t pid() const { return m_pid; }

    /// The environment of this process, without the variables that lead to
    /// the user's Afterglow, and with the test's runtime directory.
    std::vector<std::pair<std::string, std::string>> cleanEnvironment() const
    {
        std::vector<std::pair<std::string, std::string>> env;
        for (char** e = environ; *e; ++e) {
            const std::string entry(*e);
            const auto eq = entry.find('=');
            const std::string name = entry.substr(0, eq);
            if (name.starts_with("AFTERGLOW_") || name == "XDG_RUNTIME_DIR" || name == "TMUX") continue;
            env.emplace_back(name, eq == std::string::npos ? "" : entry.substr(eq + 1));
        }
        env.emplace_back("XDG_RUNTIME_DIR", runtime());
        return env;
    }

    /// Starts the daemon and waits until it answers. With @p keep, it gets a
    /// session named "keep" that runs until the test ends: without a session
    /// and a connection, the daemon ends.
    bool start(bool keep = true)
    {
        pid_t pid = spawnDaemon();
        if (pid <= 0) return false;
        m_pid = pid;
        const bool answers = waitFor([this] {
            IpcConnection connection;
            IpcConnection::Error error;
            return connection.connect(socket(), &error, 100);
        });
        return answers && (!keep || newSession(options(Waiting, "keep")));
    }

    /// A variable that the daemon has and the sessions have not.
    void daemonOnly(std::string name, std::string value) { m_daemonOnly.emplace_back(std::move(name), std::move(value)); }

    pid_t spawnDaemon() const
    {
        std::vector<std::string> store;
        for (const auto& [name, value] : m_environment) store.push_back(name + "=" + value);
        for (const auto& [name, value] : m_daemonOnly) store.push_back(name + "=" + value);
        std::vector<char*> envp;
        for (auto& entry : store) envp.push_back(entry.data());
        envp.push_back(nullptr);
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        const std::string err = m_dir + "/daemon.err";
        posix_spawn_file_actions_addopen(&actions, 2, err.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        char* argv[] = {const_cast<char*>(AFTERGLOW_MUX), nullptr};
        pid_t pid = 0;
        const int result = posix_spawn(&pid, AFTERGLOW_MUX, &actions, nullptr, argv, envp.data());
        posix_spawn_file_actions_destroy(&actions);
        return result == 0 ? pid : -1;
    }

    std::string errors() const { return readFile(m_dir + "/daemon.err"); }

    /// The exit status, once the daemon ended in five seconds; else nullopt.
    std::optional<int> waitExit()
    {
        int status = 0;
        const bool ended = waitFor([&] { return ::waitpid(m_pid, &status, WNOHANG) == m_pid; });
        if (!ended) return std::nullopt;
        m_pid = -1;
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }

    /// A call on its own connection, as `afterglow mux` makes them.
    std::optional<Json::Value> call(const std::string& fn, Json::Array args, IpcConnection::Error* error = nullptr)
    {
        IpcConnection connection;
        IpcConnection::Error& e = error ? *error : m_lastError;
        e = {};
        if (!connection.connect(socket(), &e, 2000)) return std::nullopt;
        auto result = connection.call(Target, fn, std::move(args), 5000, &e);
        if (&e != &m_lastError) m_lastError = e;
        return result;
    }

    /// Why the last call failed.
    std::string lastError() const { return m_lastError.code + ": " + m_lastError.message + " " + errors(); }

    SessionOptions options(std::vector<std::string> argv, std::string name = {}) const
    {
        SessionOptions o;
        o.name = std::move(name);
        o.launch.argv = std::move(argv);
        o.launch.workingDirectory = m_dir;
        o.launch.environment = m_environment;
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
            if (auto info = sessionInfo(item)) list.push_back(std::move(*info));
        }
        return list;
    }

private:
    std::string m_dir;
    std::vector<std::pair<std::string, std::string>> m_environment;
    std::vector<std::pair<std::string, std::string>> m_daemonOnly;
    pid_t m_pid = -1;
    IpcConnection::Error m_lastError;
};

std::vector<std::string> names(const std::vector<SessionInfo>& sessions)
{
    std::vector<std::string> list;
    for (const auto& s : sessions) list.push_back(s.name);
    return list;
}

/// A Python program that puts the terminal in raw mode, writes @p query
/// and saves what the terminal answers, up to @p end, in @p file.
std::vector<std::string> ask(const std::string& query, const std::string& end, const std::string& file)
{
    const std::string script = R"(import os, select, sys, tty
fd = sys.stdin.fileno()
tty.setraw(fd)
os.write(1, sys.argv[1].encode().decode('unicode_escape').encode('latin-1'))
end = sys.argv[2].encode().decode('unicode_escape').encode('latin-1')
data = b''
while not data.endswith(end):
    ready, _, _ = select.select([fd], [], [], 5)
    if not ready:
        break
    data += os.read(fd, 4096)
with open(sys.argv[3] + '.part', 'wb') as f:
    f.write(data)
os.rename(sys.argv[3] + '.part', sys.argv[3])
)";
    return {"python3", "-c", script, query, end, file};
}

} // namespace

TEST(MuxDaemon, AnswersItsVersion)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const auto version = daemon.call(Fn::Version, {});
    ASSERT_TRUE(version);
    EXPECT_EQ((*version)["protocol"].toInteger(), ProtocolVersion);
    EXPECT_FALSE((*version)["version"].toString().empty());
}

TEST(MuxDaemon, CreatesListsAndKillsSessions)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    for (const char* expected : {"1", "2"}) {
        const auto created = daemon.newSession(daemon.options(Waiting));
        ASSERT_TRUE(created) << daemon.lastError();
        EXPECT_EQ((*created)["name"].toString(), expected);
    }
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "work"))) << daemon.lastError();
    IpcConnection::Error error;
    EXPECT_FALSE(daemon.newSession(daemon.options(Waiting, "work"), &error));
    EXPECT_EQ(error.code, Error::SessionExists);
    EXPECT_EQ(names(daemon.sessions()), (std::vector<std::string>{"keep", "1", "2", "work"}));
    for (const auto& session : daemon.sessions()) {
        EXPECT_FALSE(session.attached);
        EXPECT_EQ(session.windows, 1);
        EXPECT_EQ(session.panes, 1);
    }

    ASSERT_TRUE(daemon.call(Fn::KillSession, Json::Array{"1"}));
    EXPECT_FALSE(daemon.call(Fn::KillSession, Json::Array{"1"}, &error));
    EXPECT_EQ(error.code, Error::NoSuchSession);
    // The smallest free number comes back.
    const auto again = daemon.newSession(daemon.options(Waiting));
    ASSERT_TRUE(again);
    EXPECT_EQ((*again)["name"].toString(), "1");
}

TEST(MuxDaemon, RefusesBadCalls)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    IpcConnection::Error error;
    EXPECT_FALSE(daemon.call("noSuchFn", {}, &error));
    EXPECT_EQ(error.code, "NO_SUCH_FN");
    EXPECT_FALSE(daemon.call(Fn::NewSession, Json::Array{ProtocolVersion + 1, toJson(daemon.options(Waiting))}, &error));
    EXPECT_EQ(error.code, Error::VersionMismatch);
    EXPECT_NE(error.message.find(std::to_string(ProtocolVersion + 1)), std::string::npos) << error.message;
    Json::Object bad = toJson(daemon.options(Waiting)).toObject();
    for (auto& [key, value] : bad) {
        if (key == "name") value = "no spaces";
    }
    EXPECT_FALSE(daemon.call(Fn::NewSession, Json::Array{ProtocolVersion, std::move(bad)}, &error));
    EXPECT_EQ(error.code, "INVALID_ARG");
    EXPECT_FALSE(daemon.newSession(daemon.options({"/no/such/program"}), &error));
    EXPECT_EQ(error.code, Error::SpawnFailed);
    EXPECT_EQ(names(daemon.sessions()), std::vector<std::string>{"keep"});
}

TEST(MuxDaemon, ASessionEndsWithItsProgramAndTheDaemonWithItsLastSession)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start(false)) << daemon.errors();
    const std::string flag = daemon.dir() + "/go";
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "stays"))) << daemon.lastError();
    ASSERT_TRUE(daemon.newSession(daemon.options({"/bin/sh", "-c", "while [ ! -e '" + flag + "' ]; do sleep 0.05; done"}, "ends")));
    std::ofstream(flag).put('1');
    EXPECT_TRUE(waitFor([&] { return names(daemon.sessions()) == std::vector<std::string>{"stays"}; }));
    // Still a session: the daemon stays.
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(::waitpid(daemon.pid(), nullptr, WNOHANG), 0);
    ASSERT_TRUE(daemon.call(Fn::KillSession, Json::Array{"stays"}));
    EXPECT_EQ(daemon.waitExit(), 0) << daemon.errors();
    // It removed its socket.
    EXPECT_NE(::access(daemon.socket().c_str(), F_OK), 0);
}

TEST(MuxDaemon, ADaemonThatNobodyReachesEnds)
{
    TestDaemon daemon;
    const pid_t pid = daemon.spawnDaemon();
    ASSERT_GT(pid, 0);
    int status = 0;
    // The start wait is ten seconds.
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (::waitpid(pid, &status, WNOHANG) == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(100ms);
    ASSERT_TRUE(WIFEXITED(status)) << daemon.errors();
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(MuxDaemon, ASecondDaemonRefusesToStart)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const pid_t second = daemon.spawnDaemon();
    int status = 0;
    ASSERT_EQ(::waitpid(second, &status, 0), second);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 1);
    EXPECT_NE(daemon.errors().find("another server listens"), std::string::npos) << daemon.errors();
    // The first one still serves.
    EXPECT_EQ(names(daemon.sessions()), std::vector<std::string>{"keep"});
}

TEST(MuxDaemon, SigtermEndsTheSessionsAndTheirPrograms)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string pidFile = daemon.dir() + "/pid";
    ASSERT_TRUE(daemon.newSession(daemon.options({"/bin/sh", "-c", "echo $$ > '" + pidFile + "'; exec sleep 60"})));
    ASSERT_TRUE(waitFor([&] { return readFile(pidFile).find('\n') != std::string::npos; }));
    const pid_t program = std::stoi(readFile(pidFile));
    ::kill(daemon.pid(), SIGTERM);
    EXPECT_EQ(daemon.waitExit(), 0) << daemon.errors();
    // The hangup ended the program: it is gone, or a zombie that waits for its reaper.
    EXPECT_TRUE(waitFor([&] {
        const std::string stat = readFile("/proc/" + std::to_string(program) + "/stat");
        return stat.empty() || stat.find(") Z ") != std::string::npos;
    }));
}

TEST(MuxDaemon, ThePaneGetsTheSessionEnvironmentOnly)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string out = daemon.dir() + "/env";
    SessionOptions o = daemon.options({"/bin/sh", "-c", "env > '" + out + ".part' && mv '" + out + ".part' '" + out + "'; exec sleep 60"}, "env");
    o.launch.environment.emplace_back("FROM_CLIENT", "yes");
    o.launch.environment.emplace_back("AFTERGLOW_PANE_ID", "999");
    o.launch.environment.emplace_back("AFTERGLOW_OVERLAY", "1");
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return !readFile(out).empty(); }));
    const std::string env = "\n" + readFile(out);
    EXPECT_NE(env.find("\nFROM_CLIENT=yes\n"), std::string::npos);
    EXPECT_NE(env.find("\nAFTERGLOW_SESSION=env\n"), std::string::npos);
    EXPECT_NE(env.find("\nAFTERGLOW_SOCKET=" + daemon.socket() + "\n"), std::string::npos);
    // The daemon's own pane ID, not the client's.
    EXPECT_EQ(env.find("\nAFTERGLOW_PANE_ID=999\n"), std::string::npos);
    EXPECT_NE(env.find("\nAFTERGLOW_PANE_ID="), std::string::npos);
    EXPECT_EQ(env.find("\nAFTERGLOW_OVERLAY="), std::string::npos);
}

TEST(MuxDaemon, ThePaneEnvironmentLeavesOutTheDaemonsOwn)
{
    // As a daemon in a systemd unit has INVOCATION_ID, which the session has not.
    TestDaemon daemon;
    daemon.daemonOnly("AFTERGLOW_MUX_TEST_DAEMON_ONLY", "1");
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string out = daemon.dir() + "/env";
    ASSERT_TRUE(daemon.newSession(daemon.options({"/bin/sh", "-c", "env > '" + out + ".part' && mv '" + out + ".part' '" + out + "'; exec sleep 60"})));
    ASSERT_TRUE(waitFor([&] { return !readFile(out).empty(); }));
    EXPECT_NE(readFile(out).find("PATH="), std::string::npos);
    EXPECT_EQ(readFile(out).find("AFTERGLOW_MUX_TEST_DAEMON_ONLY"), std::string::npos);
}

TEST(MuxDaemon, AnswersTerminalQueriesWhileDetached)
{
    if (!pythonFound()) GTEST_SKIP() << "python3 is not installed";
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string da1 = daemon.dir() + "/da1";
    ASSERT_TRUE(daemon.newSession(daemon.options(ask("\\x1b[c", "c", da1)))) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return ::access(da1.c_str(), F_OK) == 0; })) << daemon.errors();
    EXPECT_TRUE(readFile(da1).starts_with("\x1b[?")) << readFile(da1);

    // The size that the client measured, in cells (XTWINOPS 18).
    const std::string size = daemon.dir() + "/size";
    SessionOptions o = daemon.options(ask("\\x1b[18t", "t", size));
    o.size = {101, 37, 8, 16};
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return ::access(size.c_str(), F_OK) == 0; }));
    EXPECT_EQ(readFile(size), "\x1b[8;37;101t");

    // The client's background color (OSC 11).
    const std::string color = daemon.dir() + "/color";
    o = daemon.options(ask("\\x1b]11;?\\x1b\\\\", "\\x1b\\\\", color));
    o.terminal.defaultBackground = {0x12, 0x34, 0x56};
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return ::access(color.c_str(), F_OK) == 0; }));
    EXPECT_NE(readFile(color).find("rgb:1212/3434/5656"), std::string::npos) << readFile(color);
}

TEST(MuxDaemon, RefusesClipboardReadsAndTransfersWhileDetached)
{
    if (!pythonFound()) GTEST_SKIP() << "python3 is not installed";
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    // OSC 52 read: an empty answer, as when the user denies it.
    const std::string clipboard = daemon.dir() + "/clipboard";
    ASSERT_TRUE(daemon.newSession(daemon.options(ask("\\x1b]52;c;?\\x1b\\\\", "\\x1b\\\\", clipboard))));
    ASSERT_TRUE(waitFor([&] { return ::access(clipboard.c_str(), F_OK) == 0; }));
    EXPECT_EQ(readFile(clipboard), "\x1b]52;c;\x1b\\");
    // A file transfer: EPERM, in base64 without padding.
    const std::string transfer = daemon.dir() + "/transfer";
    ASSERT_TRUE(daemon.newSession(daemon.options(ask("\\x1b]5113;ac=send;id=t1\\x1b\\\\", "\\x1b\\\\", transfer))));
    ASSERT_TRUE(waitFor([&] { return ::access(transfer.c_str(), F_OK) == 0; }));
    EXPECT_EQ(readFile(transfer), "\x1b]5113;ac=status;id=t1;st=RVBFUk0\x1b\\");
}
