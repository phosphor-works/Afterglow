// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// `afterglow mux` itself, against daemons that it starts in a directory of
// the test: XDG_RUNTIME_DIR, XDG_STATE_HOME, XDG_CONFIG_HOME and HOME under
// /tmp, so the daemon never takes the user manager (that needs
// /run/user/UID) and no command reaches the user's daemon or configuration.
// Standard input is /dev/null, never a terminal.

#include "SavedEnvironment.h"
#include "config/Settings.h"

#include "MuxProtocol.h"

#include <AfterglowProto/IpcServer.h>

#include <gtest/gtest.h>

#include <QFile>

#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

using namespace std::chrono_literals;
namespace Json = AfterglowProto::Json;

namespace {

struct Result
{
    int status = -1; ///< the exit status, 128 + a signal, or -1 when it did not end in time
    std::string out;
    std::string err;
};

/// The process ID that @p text holds in decimal, or none: not a number,
/// not all of it, too large for pid_t, or not above 0 (kill() takes 0 and -1
/// for groups of processes).
std::optional<pid_t> parsePid(std::string_view text)
{
    pid_t pid = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), pid);
    if (error != std::errc() || end != text.data() + text.size() || pid <= 0) return std::nullopt;
    return pid;
}

std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

/// Waits up to five seconds for @p done.
template<typename Done>
bool waitFor(Done done)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done()) return true;
        std::this_thread::sleep_for(20ms);
    }
    return done();
}

class MuxCommandTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        removeAbandonedAfterglows();
        // Under /tmp: TMPDIR can be too long for a socket path.
        char dir[] = "/tmp/afterglow-muxcommand-XXXXXX";
        ASSERT_TRUE(::mkdtemp(dir)) << std::strerror(errno);
        m_dir = dir;
        for (const char* sub : {"/run", "/home", "/config"}) ASSERT_EQ(::mkdir((m_dir + sub).c_str(), 0700), 0);
        for (char** e = environ; *e; ++e) {
            const std::string_view entry(*e);
            const std::string_view name = entry.substr(0, entry.find('='));
            if (name.starts_with("AFTERGLOW_") || name == "TMUX" || name == "TMUX_PANE" || name.starts_with("XDG_")
                || name == "HOME" || name == "WAYLAND_DISPLAY" || name == "DISPLAY"
                || name == "QT_QPA_PLATFORM") {
                continue;
            }
            m_environment.emplace_back(entry);
        }
        m_environment.push_back("XDG_RUNTIME_DIR=" + runtime());
        m_environment.push_back("XDG_STATE_HOME=" + state());
        m_environment.push_back("XDG_CONFIG_HOME=" + m_dir + "/config");
        m_environment.push_back("HOME=" + m_dir + "/home");
    }

    void TearDown() override
    {
        // The daemon runs in a session of its own, and is no child of this
        // test: whatever failed, it and its programs go, found by the
        // runtime directory in their environment, which only they have.
        for (const pid_t pid : processes()) ::kill(pid, SIGKILL);
        EXPECT_TRUE(waitFor([this] { return processes().empty(); }));
        std::error_code ignored;
        if (!m_dir.empty()) std::filesystem::remove_all(m_dir, ignored);
        for (const auto& fake : m_fakes) std::filesystem::remove_all(fake, ignored);
    }

    std::string runtime() const { return m_dir + "/run"; }
    std::string state() const { return m_dir + "/state"; }

    /// `-- COMMAND` of a session that runs until this test process ends:
    /// a test that crashes before its TearDown leaves no session behind
    /// for longer than that.
    std::vector<std::string> waitsForTheTest() const
    {
        return {"--", "sh", "-c", "while kill -0 " + std::to_string(::getpid()) + " 2>/dev/null; do sleep 0.2; done"};
    }

    /// `new --detach` with a session that waitsForTheTest().
    std::vector<std::string> newWaiting() const
    {
        std::vector<std::string> args{"new", "--detach"};
        const auto command = waitsForTheTest();
        args.insert(args.end(), command.begin(), command.end());
        return args;
    }

    /// The processes of this user whose environment has the test's runtime
    /// directory: the daemons that the test started and their programs.
    std::vector<pid_t> processes() const
    {
        std::vector<pid_t> found;
        const std::string marker = "XDG_RUNTIME_DIR=" + runtime();
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
            const std::string name = entry.path().filename().string();
            const std::optional<pid_t> parsed = parsePid(name);
            if (!parsed || *parsed == ::getpid()) continue;
            const pid_t pid = *parsed;
            const std::string env = readFile(entry.path().string() + "/environ");
            std::size_t at = 0;
            for (std::size_t end; (end = env.find('\0', at)) != std::string::npos; at = end + 1) {
                if (env.compare(at, end - at, marker) == 0) {
                    found.push_back(pid);
                    break;
                }
            }
        }
        return found;
    }

    /// The daemons among processes().
    std::vector<pid_t> daemons() const
    {
        std::vector<pid_t> found;
        for (const pid_t pid : processes()) {
            std::error_code error;
            if (std::filesystem::read_symlink("/proc/" + std::to_string(pid) + "/exe", error).filename() == "afterglow-mux") found.push_back(pid);
        }
        return found;
    }

    /// Runs `afterglow mux ARGS`, with stdin at /dev/null, for at most 15 s.
    /// @p binary is the afterglow to run, by default the one of the build.
    Result mux(const std::vector<std::string>& args, const std::string& binary = AFTERGLOW_BINARY) const
    {
        std::vector<std::string> words{"mux"};
        words.insert(words.end(), args.begin(), args.end());
        return run(words, binary);
    }

    /// `afterglow ctl ARGS` in the test's environment.
    Result ctl(const std::vector<std::string>& args) const
    {
        std::vector<std::string> words{"ctl"};
        words.insert(words.end(), args.begin(), args.end());
        return run(words, AFTERGLOW_BINARY);
    }

    Result run(const std::vector<std::string>& words, const std::string& binary) const
    {
        std::vector<std::string> argv{binary};
        argv.insert(argv.end(), words.begin(), words.end());
        std::vector<char*> argp;
        for (const auto& a : argv) argp.push_back(const_cast<char*>(a.c_str()));
        argp.push_back(nullptr);
        std::vector<char*> envp;
        for (const auto& e : m_environment) envp.push_back(const_cast<char*>(e.c_str()));
        envp.push_back(nullptr);
        int out[2], err[2];
        if (::pipe2(out, O_CLOEXEC) != 0) return {};
        if (::pipe2(err, O_CLOEXEC) != 0) {
            ::close(out[0]);
            ::close(out[1]);
            return {};
        }
        const pid_t pid = ::fork();
        if (pid < 0) {
            for (const int fd : {out[0], out[1], err[0], err[1]}) ::close(fd);
            return {};
        }
        if (pid == 0) {
            const int null = ::open("/dev/null", O_RDONLY);
            ::dup2(null, 0);
            ::dup2(out[1], 1);
            ::dup2(err[1], 2);
            ::execve(argp[0], argp.data(), envp.data());
            ::_exit(127);
        }
        ::close(out[1]);
        ::close(err[1]);
        Result result;
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        pollfd fds[2] = {{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
        std::string* text[2] = {&result.out, &result.err};
        int open = 2;
        while (open > 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0) break;
            const int ready = ::poll(fds, 2, int(left.count()));
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0) break;
            for (int i = 0; i < 2; ++i) {
                if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP))) continue;
                char buffer[4096];
                const ssize_t n = ::read(fds[i].fd, buffer, sizeof buffer);
                if (n > 0) {
                    text[i]->append(buffer, std::size_t(n));
                } else {
                    fds[i].fd = -1;
                    --open;
                }
            }
        }
        ::close(out[0]);
        ::close(err[0]);
        int status = 0;
        if (open > 0) {
            // It did not end: the test fails, and the process goes.
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            return result;
        }
        ::waitpid(pid, &status, 0);
        result.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        return result;
    }

    /// An afterglow whose afterglow-mux is @p script: a link to the build's
    /// afterglow in a directory next to it (the same file system), with a
    /// script of that name. afterglow finds the daemon next to its own path.
    /// The directory goes with the test; its name holds the pid of this
    /// process, so a later test removes it when this one was killed first.
    std::string afterglowWithDaemon(const std::string& script)
    {
        const std::filesystem::path binary(AFTERGLOW_BINARY);
        std::string pattern = (binary.parent_path() / ("muxcommand-" + std::to_string(::getpid()) + "-XXXXXX")).string();
        if (!::mkdtemp(pattern.data())) {
            ADD_FAILURE() << std::strerror(errno);
            return {};
        }
        m_fakes.emplace_back(pattern);
        const std::filesystem::path directory(pattern);
        std::error_code error;
        std::filesystem::create_hard_link(binary, directory / "afterglow", error);
        if (error) std::filesystem::copy_file(binary, directory / "afterglow", error);
        if (error) {
            ADD_FAILURE() << error.message();
            return {};
        }
        std::ofstream(directory / "afterglow-mux") << script;
        if (::chmod((directory / "afterglow-mux").c_str(), 0700) != 0) {
            ADD_FAILURE() << std::strerror(errno);
            return {};
        }
        return (directory / "afterglow").string();
    }

    /// The directories of afterglowWithDaemon() whose test process is gone:
    /// a ctest timeout kills a test before its TearDown. One of a test that
    /// still runs, in parallel, stays.
    static void removeAbandonedAfterglows()
    {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(AFTERGLOW_BINARY).parent_path(), error)) {
            const std::string name = entry.path().filename().string();
            constexpr std::string_view Prefix = "muxcommand-";
            if (!name.starts_with(Prefix)) continue;
            // Without a valid pid: no process can own it.
            const std::optional<pid_t> pid = parsePid(std::string_view(name).substr(Prefix.size(), name.find('-', Prefix.size()) - Prefix.size()));
            const bool owned = pid && (::kill(*pid, 0) == 0 || errno == EPERM);
            std::error_code ignored;
            if (!owned) std::filesystem::remove_all(entry.path(), ignored);
        }
    }

    std::string m_dir;
    std::vector<std::string> m_environment;
    std::vector<std::filesystem::path> m_fakes;
};

} // namespace

TEST_F(MuxCommandTest, CreatesListsAndKillsASessionAndTheDaemonEndsWithIt)
{
    const Result created = mux(newWaiting());
    ASSERT_EQ(created.status, 0) << created.err;
    EXPECT_EQ(created.out, "1\n");
    ASSERT_EQ(daemons().size(), 1u);

    const Result listed = mux({"ls"});
    EXPECT_EQ(listed.status, 0) << listed.err;
    EXPECT_NE(listed.out.find("NAME  STATE     PANES\n1     detached      1\n"), std::string::npos) << listed.out;

    // A program runs and stdin is no terminal: no question, a refusal.
    const Result refused = mux({"kill", "1"});
    EXPECT_EQ(refused.status, 1);
    EXPECT_EQ(refused.err, "afterglow mux kill: session '1' runs sh: give --force to end it\n");
    EXPECT_NE(mux({"ls"}).out.find("detached"), std::string::npos);

    const Result killed = mux({"kill", "--force", "1"});
    EXPECT_EQ(killed.status, 0) << killed.err;
    // The daemon of a session of its own ends with its last session.
    EXPECT_TRUE(waitFor([this] { return daemons().empty(); }));
    const Result empty = mux({"ls"});
    EXPECT_EQ(empty.status, 0);
    EXPECT_EQ(empty.err, "afterglow mux ls: no sessions\n");
    EXPECT_TRUE(empty.out.empty());
}

// A SIGCHLD that the command got as ignored (inherited across exec) would make
// the kernel reap its children: the start of the daemon looked failed.
TEST_F(MuxCommandTest, ASigchldThatTheCommandGotAsIgnoredDoesNotStopTheStart)
{
    const std::string wrapper = m_dir + "/afterglow-ignoring-sigchld";
    // The binary's path comes in the environment: no quoting of it here.
    m_environment.push_back(std::string("AFTERGLOW_TEST_BINARY=") + AFTERGLOW_BINARY);
    std::ofstream(wrapper) << "#!/bin/sh\ntrap '' CHLD\nexec \"$AFTERGLOW_TEST_BINARY\" \"$@\"\n";
    ASSERT_EQ(::chmod(wrapper.c_str(), 0700), 0);
    const Result r = mux({"new", "--detach", "--", "/bin/sh", "-c", "exec sleep 60"}, wrapper);
    EXPECT_EQ(r.status, 0) << r.err;
    EXPECT_EQ(r.out, "1\n");
    EXPECT_EQ(daemons().size(), 1u);
    EXPECT_EQ(mux({"kill", "--force", "1"}).status, 0);
}

TEST_F(MuxCommandTest, WithoutADaemonThereAreNoSessions)
{
    const Result listed = mux({"ls"});
    EXPECT_EQ(listed.status, 0);
    EXPECT_EQ(listed.err, "afterglow mux ls: no sessions\n");
    const Result killed = mux({"kill", "--force", "work"});
    EXPECT_EQ(killed.status, 1);
    EXPECT_EQ(killed.err, "afterglow mux kill: no sessions\n");
    // Neither starts a daemon.
    EXPECT_TRUE(daemons().empty());
}

TEST_F(MuxCommandTest, UsageErrorsAndCommandsThatAreNotAvailable)
{
    for (const auto& [args, status, message] : std::vector<std::tuple<std::vector<std::string>, int, std::string>>{
             {{"new", "--name"}, 2, "afterglow mux: --name needs a value\n"},
             {{"new", "--detach", "--cwd"}, 2, "afterglow mux: --cwd needs a value\n"},
             {{"new", "--detach", "--name", "-x"}, 2,
              "afterglow mux new: the name must be 1 to 64 ASCII letters, digits, '_' or '-', with a letter, a digit or '_' first\n"},
             {{"new", "--detach", "--cwd", "/no/such/directory"}, 1, "afterglow mux new: no directory /no/such/directory\n"},
             // The test has no Wayland display: no window, and no session for it.
             {{"new"}, 1, "afterglow mux new: no Wayland display shows a window here: "},
             {{"attach"}, 1, "afterglow mux attach: no Wayland display shows a window here: "},
             {{"attach", "bad name"}, 2, "afterglow mux attach: the name must be "},
             {{"attach", "a", "b"}, 2, "afterglow mux: unknown argument of attach: b\n"},
             {{"attach", "--ssh", "host"}, 1, "afterglow mux attach: sessions on other hosts are not available yet\n"},
             {{"detach"}, 2, "afterglow mux: detach takes the name of a session, outside a session pane\n"},
             {{"detach", "a", "b"}, 2, "afterglow mux: unknown argument of detach: b\n"},
             {{"detach", "bad name"}, 2, "afterglow mux detach: the name must be "},
             {{"detach", "--ssh", "host"}, 1, "afterglow mux detach: sessions on other hosts are not available yet\n"},
             // No daemon: no session to detach, and none starts.
             {{"detach", "work"}, 1, "afterglow mux detach: no sessions\n"},
             {{"new", "--ssh", "host"}, 1, "afterglow mux new: sessions on other hosts are not available yet\n"},
             {{"ls", "--ssh", "host"}, 1, "afterglow mux ls: sessions on other hosts are not available yet\n"},
             {{"kill", "--ssh", "host"}, 1, "afterglow mux kill: sessions on other hosts are not available yet\n"},
             {{"ls", "x"}, 2, "afterglow mux: ls takes no arguments\n"},
             // Bytes that are not UTF-8 come back as '?', at once.
             {{"new", "--bad\xff\xfe"}, 2, "afterglow mux: unknown option of new: --bad??\n"},
             {{"\xff"}, 2, "afterglow mux: unknown command: ?\n"},
         }) {
        const Result r = mux(args);
        EXPECT_EQ(r.status, status) << args.front() << " " << r.err;
        EXPECT_TRUE(r.err.starts_with(message)) << r.err;
    }
    EXPECT_TRUE(daemons().empty());
}

// In a session pane, detach without a name finds the session of the pane by
// its ID: a pane that broke out of its window keeps the AFTERGLOW_SESSION of
// its first session.
TEST_F(MuxCommandTest, DetachInASessionPaneFindsTheSessionOfThePane)
{
    std::vector<std::string> args{"new", "--detach", "--name", "now"};
    for (const std::string& arg : waitsForTheTest()) args.push_back(arg);
    const Result created = mux(args);
    ASSERT_EQ(created.status, 0) << created.err;
    // The first pane of the daemon: ID 1.
    m_environment.push_back("AFTERGLOW_SESSION=before");
    m_environment.push_back("AFTERGLOW_PANE_ID=1");
    const Result detached = mux({"detach"});
    EXPECT_EQ(detached.status, 1);
    // The session of the pane, which no window shows.
    EXPECT_EQ(detached.err, "afterglow mux detach: no window shows session 'now'\n");
    m_environment.back() = "AFTERGLOW_PANE_ID=99";
    const Result none = mux({"detach"});
    EXPECT_EQ(none.status, 1);
    EXPECT_EQ(none.err, "afterglow mux detach: no session has pane 99\n");
    m_environment.back() = "AFTERGLOW_PANE_ID=x";
    const Result bad = mux({"detach"});
    EXPECT_EQ(bad.status, 1);
    EXPECT_EQ(bad.err, "afterglow mux detach: AFTERGLOW_PANE_ID names no pane of the mux daemon\n");
}

// `afterglow ctl --session` acts on a session that no window shows, with the
// daemon's pane IDs; so does `afterglow ctl` in a pane of the session.
TEST_F(MuxCommandTest, CtlActsOnASessionWithoutAWindow)
{
    ASSERT_EQ(::mkdir((m_dir + "/config/afterglow").c_str(), 0700), 0);
    {
        std::ofstream config(m_dir + "/config/afterglow/config.json");
        config << R"({"Control": {"allowReadText": true}, "Terminal": {"shell": "/bin/sh"}})";
    }
    std::vector<std::string> args{"new", "--detach", "--name", "work", "--", "sh", "-c",
                                  "echo hel''lo; while kill -0 " + std::to_string(::getpid()) + " 2>/dev/null; do sleep 0.2; done"};
    ASSERT_EQ(mux(args).status, 0);

    const Result listed = ctl({"--session", "work", "ls"});
    EXPECT_EQ(listed.status, 0) << listed.err;
    // The first pane of the daemon, focused.
    EXPECT_NE(listed.out.find("\n1*"), std::string::npos) << listed.out;
    EXPECT_TRUE(waitFor([&] { return ctl({"--session", "work", "get-text"}).out.find("hello") != std::string::npos; }));
    const Result split = ctl({"--session", "work", "split", "--down"});
    EXPECT_EQ(split.status, 0) << split.err;
    EXPECT_EQ(split.out, "2\n");
    const Result json = ctl({"--session", "work", "ls", "--json"});
    EXPECT_NE(json.out.find("\"id\": 2"), std::string::npos) << json.out;
    EXPECT_NE(json.out.find("\"focused\": true"), std::string::npos) << json.out;

    // What only a window does, a session without one refuses.
    const Result titled = ctl({"--session", "work", "set-title", "t"});
    EXPECT_EQ(titled.status, 1);
    EXPECT_EQ(titled.err, "afterglow ctl: no window shows session 'work', and only a window can do setTitle\n");
    const Result none = ctl({"--session", "nope", "ls"});
    EXPECT_EQ(none.status, 1);
    EXPECT_EQ(none.err, "afterglow ctl: no session named 'nope'\n");
    EXPECT_EQ(ctl({"--session", "bad name", "ls"}).status, 2);

    // In a pane of the session: its AFTERGLOW_SOCKET names the daemon, and a
    // stale AFTERGLOW_SESSION changes nothing.
    m_environment.push_back("AFTERGLOW_SOCKET=" + runtime() + "/afterglow-mux/socket");
    m_environment.push_back("AFTERGLOW_SESSION=before");
    m_environment.push_back("AFTERGLOW_PANE_ID=2");
    const Result inPane = ctl({"ls", "--json"});
    EXPECT_EQ(inPane.status, 0) << inPane.err;
    EXPECT_NE(inPane.out.find("\"id\": 1"), std::string::npos) << inPane.out;
    EXPECT_NE(inPane.out.find("\"id\": 2"), std::string::npos) << inPane.out;
    const Result closed = ctl({"close"});
    EXPECT_EQ(closed.status, 0) << closed.err;
    const Result one = ctl({"--session", "work", "ls", "--json"});
    EXPECT_EQ(one.out.find("\"id\": 2"), std::string::npos) << one.out;
}

// A platform other than Wayland, which afterglow would refuse after the
// session was made: refused first, with no session.
TEST_F(MuxCommandTest, AWindowNeedsTheWaylandPlatform)
{
    m_environment.push_back("QT_QPA_PLATFORM=offscreen");
    const Result r = mux({"new"});
    EXPECT_EQ(r.status, 1) << r.err;
    EXPECT_EQ(r.err, "afterglow mux new: QT_QPA_PLATFORM is offscreen, and Afterglow shows windows only on Wayland: give --detach\n");
    EXPECT_TRUE(daemons().empty());
}

// A daemon of a session of its own has /dev/null as its stderr: it reports
// why it cannot start through a pipe, and `afterglow mux new` prints that.
TEST_F(MuxCommandTest, SaysWhyTheDaemonDidNotStart)
{
    for (const std::string& directory : {runtime() + "/afterglow-mux", state() + "/afterglow-mux"}) {
        std::filesystem::create_directories(directory);
        ASSERT_EQ(::chmod(directory.c_str(), 0755), 0);
    }
    const Result r = mux(newWaiting());
    EXPECT_EQ(r.status, 1);
    EXPECT_EQ(r.err, "afterglow mux new: afterglow-mux: no socket to listen on: " + runtime()
                         + "/afterglow-mux must be a directory of this user with mode 0700 and " + state()
                         + "/afterglow-mux must be a directory of this user with mode 0700\n");
    EXPECT_TRUE(waitFor([this] { return daemons().empty(); }));
}

// A daemon that ends before it listens and says nothing (a signal, a crash)
// ends the report pipe without the byte that a listening daemon writes: the
// client fails at once, and does not wait out its start time for a socket.
TEST_F(MuxCommandTest, ADaemonThatEndsBeforeItListensFailsAtOnce)
{
    // An afterglow whose afterglow-mux ends at once.
    const std::string afterglow = afterglowWithDaemon("#!/bin/sh\nkill -KILL $$\n");
    ASSERT_FALSE(afterglow.empty());
    const auto before = std::chrono::steady_clock::now();
    const Result r = mux(newWaiting(), afterglow);
    EXPECT_EQ(r.status, 1) << r.err;
    EXPECT_EQ(r.err, "afterglow mux new: afterglow-mux ended before it listened\n");
    // At once, not after the start wait of five seconds: a second of it
    // left is room for a slow test computer.
    EXPECT_LT(std::chrono::steady_clock::now() - before, 4s);
}

// Two commands start a daemon at the same time: the second daemon finds the
// lock of the first, which may not listen yet. It reports that the socket is
// held, and its client waits for the first daemon, not only one look.
TEST_F(MuxCommandTest, ADaemonThatFindsTheSocketHeldLeavesTheClientToWaitForTheHolder)
{
    // Its afterglow-mux reports the held socket and ends; the daemon of the
    // build listens a second later, as the holder that was slow to bind.
    const std::string daemon = (std::filesystem::path(AFTERGLOW_BINARY).parent_path() / "afterglow-mux").string();
    const std::string afterglow = afterglowWithDaemon(
        "#!/bin/sh\nprintf '\\001afterglow-mux: another afterglow-mux listens on %s\\n' \"$XDG_RUNTIME_DIR/afterglow-mux/socket\" >&3\n"
        "exec 3>&-\n(sleep 1; unset AFTERGLOW_MUX_REPORT_FD; exec '" + daemon + "') &\nexit 1\n");
    ASSERT_FALSE(afterglow.empty());
    const Result r = mux(newWaiting(), afterglow);
    EXPECT_EQ(r.status, 0) << r.err;
    EXPECT_EQ(r.out, "1\n");
    EXPECT_EQ(daemons().size(), 1u);
}

// A daemon of another protocol keeps its sessions: the client says so, and
// does not start a second daemon.
TEST_F(MuxCommandTest, RefusesADaemonOfAnotherProtocol)
{
    const std::string directory = runtime() + "/afterglow-mux";
    ASSERT_EQ(::mkdir(directory.c_str(), 0700), 0);
    AfterglowProto::IpcListener listener;
    std::string error;
    ASSERT_EQ(listener.listen(directory + "/socket", &error), AfterglowProto::IpcListener::Listen::Listening) << error;
    std::atomic<bool> stop{false};
    std::jthread server([&] {
        std::vector<std::unique_ptr<AfterglowProto::IpcServerConnection>> connections;
        while (!stop) {
            pollfd p{listener.fd(), POLLIN, 0};
            ::poll(&p, 1, 20);
            for (int fd = listener.accept(); fd >= 0; fd = listener.accept()) {
                connections.push_back(std::make_unique<AfterglowProto::IpcServerConnection>(fd));
            }
            for (auto& connection : connections) {
                AfterglowProto::IpcRequest request;
                while (connection->nextRequest(request) != AfterglowProto::IpcServerConnection::Read::None) {
                    connection->send(AfterglowProto::ipcReply(request.id.value_or(0), Json::Object{{"version", "0"}, {"protocol", 99}}));
                }
                connection->flush();
            }
        }
    });
    for (const std::vector<std::string>& args : {std::vector<std::string>{"ls"}, std::vector<std::string>{"kill", "--force", "1"},
                                                 std::vector<std::string>{"new", "--detach"}}) {
        const Result r = mux(args);
        EXPECT_EQ(r.status, 1) << args.front();
        EXPECT_NE(r.err.find("afterglow-mux speaks protocol 99 and this afterglow speaks " + std::to_string(AfterglowMux::ProtocolVersion)
                             + ": end its sessions with the afterglow that started it, or wait until they end\n"),
                  std::string::npos)
            << r.err;
    }
    stop = true;
    EXPECT_TRUE(daemons().empty());
}

// The bounds of the protocol hold every value of the settings that it
// carries: the largest and the smallest that the configuration takes.
TEST_F(MuxCommandTest, TheProtocolTakesEveryValueOfTheSettings)
{
    // In this process: the store reads themes and kdeglobals from the base
    // directories, which are the test's, never the user's.
    using Afterglow::Test::Saved;
    const Saved home("HOME"), config("XDG_CONFIG_HOME"), data("XDG_DATA_HOME"), configDirs("XDG_CONFIG_DIRS"),
        dataDirs("XDG_DATA_DIRS"), stateHome("XDG_STATE_HOME"), cache("XDG_CACHE_HOME");
    const QByteArray base = QByteArray::fromStdString(m_dir);
    qputenv("HOME", base + "/home");
    qputenv("XDG_CONFIG_HOME", base + "/config");
    qputenv("XDG_DATA_HOME", base + "/home/.local/share");
    qputenv("XDG_CONFIG_DIRS", base + "/config-dirs");
    qputenv("XDG_DATA_DIRS", base + "/data-dirs");
    qputenv("XDG_STATE_HOME", base + "/state");
    qputenv("XDG_CACHE_HOME", base + "/home/.cache");
    for (const char* json : {R"({"Terminal": {"scrollback": 1000000000}, "Colors": {"minimumContrast": 100}, "Graphics": {"quota": 1000000}})",
                             R"({"Terminal": {"scrollback": -5}, "Colors": {"minimumContrast": -1}, "Graphics": {"quota": 0}})"}) {
        const std::string path = m_dir + "/config.json";
        std::ofstream(path) << json;
        Afterglow::SettingsStore store(QFile::decodeName(path.c_str()));
        AfterglowMux::SessionOptions options;
        options.terminal = store.settings().terminal;
        // The configuration took the values, at its bounds.
        const AfterglowVt::TerminalOptions defaults;
        EXPECT_NE(options.terminal.maxScrollback, defaults.maxScrollback) << json;
        EXPECT_NE(options.terminal.graphicsQuota, defaults.graphicsQuota) << json;
        // 100 takes the upper bound; -1 the lower one, which is the default.
        EXPECT_DOUBLE_EQ(options.terminal.minimumContrast, std::string_view(json).find("100") != std::string_view::npos ? 21.0 : 1.0) << json;
        std::string error;
        EXPECT_TRUE(AfterglowMux::sessionOptions(AfterglowMux::toJson(options), &error)) << json << ": " << error;
    }
}
