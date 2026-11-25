// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

// `afterglow mux` and `afterglow ctl --session`, against daemons that they
// start in a directory of the test: XDG_RUNTIME_DIR, XDG_STATE_HOME,
// XDG_CONFIG_HOME and HOME under /tmp, so the daemon never takes the user
// manager (that needs /run/user/UID) and no command reaches the user's
// daemon or configuration. `afterglow mux COMMAND --ssh` reaches the daemon
// of a host that a fake ssh plays, in other directories of the test, through
// `aglow-mux proxy`; no test runs the user's ssh. Also the values of the
// settings that the protocol carries. Standard input is /dev/null, or a few
// bytes that a test gives, never a terminal.

#include "SavedEnvironment.h"
#include "config/Settings.h"

#include "MuxConnect.h"
#include "MuxProtocol.h"

#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/IpcServer.h>
#include <PhosphorChannel/Stream.h>

#include <gtest/gtest.h>

#include <QFile>

#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

using namespace std::chrono_literals;
namespace Json = AfterglowProto::Json;

namespace {

struct Result
{
    /// The exit status, 128 + a signal, or -1 when it did not end in time
    /// or did not start (a failure of the test says which).
    int status = -1;
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
    if (error != std::errc() || end != text.data() + text.size() || pid <= 0)
        return std::nullopt;
    return pid;
}

std::string readFile(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

/// Waits up to @p limit for @p done.
template<typename Done>
bool waitFor(Done done, std::chrono::milliseconds limit = 5s)
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (done())
            return true;
        std::this_thread::sleep_for(20ms);
    }
    return done();
}

/// Whether the environment of process @p pid has one of @p markers, whole
/// entries such as "XDG_RUNTIME_DIR=/tmp/x/run". Only processes of this user
/// show their environment.
bool hasMarker(pid_t pid, const std::vector<std::string>& markers)
{
    const std::string env = readFile("/proc/" + std::to_string(pid) + "/environ");
    std::size_t at = 0;
    for (std::size_t end; (end = env.find('\0', at)) != std::string::npos; at = end + 1) {
        for (const std::string& marker : markers) {
            if (env.compare(at, end - at, marker) == 0)
                return true;
        }
    }
    return false;
}

/// The processes other than this one whose environment has one of @p markers.
std::vector<pid_t> processesWith(const std::vector<std::string>& markers)
{
    std::vector<pid_t> found;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator("/proc", error)) {
        const std::optional<pid_t> pid = parsePid(entry.path().filename().string());
        if (pid && *pid != ::getpid() && hasMarker(*pid, markers))
            found.push_back(*pid);
    }
    return found;
}

/// SIGKILLs the processes with @p markers until none is left, or for at most
/// five seconds: a program can start another between a scan and its kill.
/// Each kill goes through a pidfd, opened before the environment is read
/// again, so that a process ID that a new process took meanwhile gets none.
void killProcessesWith(const std::vector<std::string>& markers)
{
    waitFor([&markers] {
        const std::vector<pid_t> found = processesWith(markers);
        for (const pid_t pid : found) {
            const int fd = int(::syscall(SYS_pidfd_open, pid, 0));
            if (fd < 0)
                continue;
            if (hasMarker(pid, markers))
                ::syscall(SYS_pidfd_send_signal, fd, SIGKILL, nullptr, 0);
            ::close(fd);
        }
        return found.empty();
    });
}

/// The marker of the processes of a test directory @p dir: its own runtime
/// directory, and the one of the host that its fake ssh plays.
std::vector<std::string> markersOf(const std::string& dir)
{
    return {"XDG_RUNTIME_DIR=" + dir + "/run", "XDG_RUNTIME_DIR=" + dir + "/far/run"};
}

/// Whether process @p pid runs: kill(pid, 0) finds it, or finds it as
/// another user's.
bool alive(pid_t pid)
{
    return ::kill(pid, 0) == 0 || errno == EPERM;
}

/// A phosphor-ipc server of the test on @p path, which answers each request
/// with the line that @p answer gives, on a thread of its own until it goes.
class FakeServer
{
public:
    using Answer = std::function<std::string(const AfterglowProto::IpcRequest&)>;

    FakeServer(const std::string& path, Answer answer)
        : m_answer(std::move(answer))
    {
        m_listening = m_listener.listen(path, &m_error) == AfterglowProto::IpcListener::Listen::Listening;
        if (m_listening)
            m_thread = std::jthread([this](std::stop_token stop) {
                serve(stop);
            });
    }

    bool listening() const
    {
        return m_listening;
    }
    const std::string& error() const
    {
        return m_error;
    }

private:
    void serve(std::stop_token stop)
    {
        std::vector<std::unique_ptr<AfterglowProto::IpcServerConnection>> connections;
        while (!stop.stop_requested()) {
            pollfd p{m_listener.fd(), POLLIN, 0};
            ::poll(&p, 1, 20);
            for (int fd = m_listener.accept(); fd >= 0; fd = m_listener.accept()) {
                connections.push_back(std::make_unique<AfterglowProto::IpcServerConnection>(fd));
            }
            for (auto& connection : connections) {
                AfterglowProto::IpcRequest request;
                for (AfterglowProto::IpcServerConnection::Read read;
                     (read = connection->nextRequest(request)) != AfterglowProto::IpcServerConnection::Read::None;) {
                    if (read == AfterglowProto::IpcServerConnection::Read::Request)
                        connection->send(m_answer(request));
                }
                connection->flush();
            }
        }
    }

    AfterglowProto::IpcListener m_listener;
    Answer m_answer;
    bool m_listening = false;
    std::string m_error;
    std::jthread m_thread; ///< last: it stops and joins before the rest goes
};

class MuxCommandTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        removeAbandonedAfterglows();
        removeAbandonedRuns();
        // Under /tmp: TMPDIR can be too long for a socket path. The name
        // holds the pid of this process: a later test removes what this one
        // leaves when ctest kills it before its TearDown.
        std::string dir = "/tmp/" + std::string(RunPrefix) + std::to_string(::getpid()) + "-XXXXXX";
        ASSERT_TRUE(::mkdtemp(dir.data())) << std::strerror(errno);
        m_dir = dir;
        for (const char* sub : {"/run", "/home", "/config", "/config/phosphor-works",
                                "/config/phosphor-works/afterglow", "/far", "/far/run", "/no-ssh"}) {
            ASSERT_EQ(::mkdir((m_dir + sub).c_str(), 0700), 0) << sub << ": " << std::strerror(errno);
        }
        for (char** e = environ; *e; ++e) {
            const std::string_view entry(*e);
            const std::string_view name = entry.substr(0, entry.find('='));
            // The user's ssh agent and connections, and the terminfo and
            // shell of the computer, stay out: the sessions get the test's.
            // So does the number of a daemon's report pipe, which a daemon
            // that the test starts would take for its own.
            if (name.starts_with("AFTERGLOW_") || name == "TMUX" || name == "TMUX_PANE" || name.starts_with("XDG_")
                || name == "HOME" || name == "WAYLAND_DISPLAY" || name == "DISPLAY" || name == "QT_QPA_PLATFORM"
                || name.starts_with("SSH_") || name == "TERMINFO_DIRS" || name == "SHELL"
                || name == AfterglowMux::ReportFdVariable) {
                continue;
            }
            m_environment.emplace_back(entry);
        }
        m_environment.push_back("XDG_RUNTIME_DIR=" + runtime());
        m_environment.push_back("XDG_STATE_HOME=" + state());
        m_environment.push_back("XDG_CONFIG_HOME=" + m_dir + "/config");
        m_environment.push_back("HOME=" + m_dir + "/home");
        m_environment.push_back("SHELL=/bin/sh");
        // An ssh that fails, first on PATH until fakeSsh() puts its own
        // before it: a command that reaches ssh without a fake never runs
        // the user's.
        // On a noexec mount, a lookup on PATH passes over a script it cannot
        // run, and would find the user's ssh.
        if (!writeScript(m_dir + "/no-ssh/ssh", "echo 'ssh: no fake ssh in this test' >&2\nexit 255\n")) {
            GTEST_SKIP() << "cannot run a script in " << m_dir;
        }
        setVariable("PATH", m_dir + "/no-ssh:" + variable("PATH"));
        // The panes that a test does not give a command, as split and
        // new-tab make them, end with this test too.
        ASSERT_TRUE(writeScript(testShell(), untilTheTestEnds() + "\n"));
        ASSERT_TRUE(writeConfig("{" + shellSetting() + "}"));
    }

    void TearDown() override
    {
        // After a SetUp that failed before it made the directory: no marker.
        if (m_dir.empty())
            return;
        if (m_displayFd >= 0)
            ::close(m_displayFd);
        // The daemon runs in a session of its own, and is no child of this
        // test: whatever failed, it and its programs go, found by the
        // runtime directory in their environment, which only they have.
        killProcessesWith(markersOf(m_dir));
        EXPECT_TRUE(waitFor([this] {
            return processes().empty();
        }));
        std::error_code ignored;
        std::filesystem::remove_all(m_dir, ignored);
        for (const auto& fake : m_fakes)
            std::filesystem::remove_all(fake, ignored);
    }

    std::string runtime() const
    {
        return m_dir + "/run";
    }
    std::string state() const
    {
        return m_dir + "/state";
    }
    /// XDG_RUNTIME_DIR of the host that fakeSsh() plays.
    std::string farRuntime() const
    {
        return m_dir + "/far/run";
    }
    std::string sshLog() const
    {
        return m_dir + "/ssh.log";
    }
    std::string testShell() const
    {
        return m_dir + "/test-shell";
    }

    /// A loop for sh that runs until this test process ends: a test that
    /// crashes before its TearDown leaves no program behind for longer.
    std::string untilTheTestEnds() const
    {
        return "while kill -0 " + std::to_string(::getpid()) + " 2>/dev/null; do sleep 0.2; done";
    }

    /// `-- COMMAND` of a session that runs until this test process ends.
    std::vector<std::string> waitsForTheTest() const
    {
        return {"--", "sh", "-c", untilTheTestEnds()};
    }

    /// `new --detach` with a session that waitsForTheTest(), named @p name
    /// when it is not empty.
    std::vector<std::string> newWaiting(const std::string& name = {}) const
    {
        std::vector<std::string> args{"new", "--detach"};
        if (!name.empty()) {
            args.emplace_back("--name");
            args.push_back(name);
        }
        const auto command = waitsForTheTest();
        args.insert(args.end(), command.begin(), command.end());
        return args;
    }

    /// The "shell" member of Terminal: testShell(), for the configurations
    /// that the tests write.
    std::string shellSetting() const
    {
        return R"("Terminal": {"shell": ")" + testShell() + R"("})";
    }

    /// Writes the configuration of afterglow, @p json.
    bool writeConfig(const std::string& json) const
    {
        std::ofstream config(m_dir + "/config/phosphor-works/afterglow/config.json", std::ios::trunc);
        config << json;
        return bool(config.flush());
    }

    /// An sh script at @p path with @p body, which this process can run.
    static bool writeScript(const std::string& path, const std::string& body)
    {
        {
            std::ofstream script(path, std::ios::trunc);
            script << "#!/bin/sh\n" << body;
            if (!script.flush())
                return false;
        }
        return ::chmod(path.c_str(), 0700) == 0 && ::access(path.c_str(), X_OK) == 0;
    }

    /// The value of @p name in the test's environment, or empty.
    std::string variable(const std::string& name) const
    {
        for (const std::string& entry : m_environment) {
            if (entry.starts_with(name + "="))
                return entry.substr(name.size() + 1);
        }
        return {};
    }

    /// Sets @p name to @p value in the test's environment.
    void setVariable(const std::string& name, const std::string& value)
    {
        for (std::string& entry : m_environment) {
            if (entry.starts_with(name + "=")) {
                entry = name + "=" + value;
                return;
            }
        }
        m_environment.push_back(name + "=" + value);
    }

    /// The processes of this user whose environment has the test's runtime
    /// directory or that of its fake host: the daemons that the test started
    /// and their programs.
    std::vector<pid_t> processes() const
    {
        return processesWith(markersOf(m_dir));
    }

    /// The daemons among processes(). A daemon whose binary a build replaced
    /// meanwhile still counts.
    std::vector<pid_t> daemons() const
    {
        std::vector<pid_t> found;
        for (const pid_t pid : processes()) {
            std::error_code error;
            const std::string name =
                std::filesystem::read_symlink("/proc/" + std::to_string(pid) + "/exe", error).filename().string();
            if (name == "aglow-mux" || name == "aglow-mux (deleted)")
                found.push_back(pid);
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

    /// @p binary with @p words, in the test's environment, for at most 15 s
    /// in all. Its standard input is @p input, at most 4 KiB, else
    /// /dev/null.
    Result run(const std::vector<std::string>& words, const std::string& binary, const std::string& input = {}) const
    {
        Result result;
        if (input.size() > 4096) {
            ADD_FAILURE() << "the input of a run is at most 4 KiB, which a pipe takes whole";
            return result;
        }
        std::vector<std::string> argv{binary};
        argv.insert(argv.end(), words.begin(), words.end());
        std::vector<char*> argp;
        for (const auto& a : argv)
            argp.push_back(const_cast<char*>(a.c_str()));
        argp.push_back(nullptr);
        std::vector<char*> envp;
        for (const auto& e : m_environment)
            envp.push_back(const_cast<char*>(e.c_str()));
        envp.push_back(nullptr);
        int out[2] = {-1, -1}, err[2] = {-1, -1}, in[2] = {-1, -1};
        const auto closeAll = [&] {
            for (int* fd : {&out[0], &out[1], &err[0], &err[1], &in[0], &in[1]}) {
                if (*fd >= 0)
                    ::close(*fd);
                *fd = -1;
            }
        };
        if (::pipe2(out, O_CLOEXEC) != 0 || ::pipe2(err, O_CLOEXEC) != 0
            || (!input.empty() && ::pipe2(in, O_CLOEXEC) != 0)) {
            ADD_FAILURE() << "pipe2: " << std::strerror(errno);
            closeAll();
            return result;
        }
        if (!input.empty()) {
            // Whole before the fork, while this process holds the read end
            // too: a child that ends without reading it cannot make the
            // write raise SIGPIPE here.
            const ssize_t written = ::write(in[1], input.data(), input.size());
            ::close(in[1]);
            in[1] = -1;
            if (written != ssize_t(input.size())) {
                ADD_FAILURE() << "cannot write the input: " << std::strerror(errno);
                closeAll();
                return result;
            }
        }
        const pid_t pid = ::fork();
        if (pid < 0) {
            ADD_FAILURE() << "fork: " << std::strerror(errno);
            closeAll();
            return result;
        }
        if (pid == 0) {
            // Never the test's own standard input, which can be the user's
            // terminal.
            const int null = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
            if (null < 0)
                ::_exit(127);
            ::dup2(input.empty() ? null : in[0], 0);
            ::dup2(out[1], 1);
            ::dup2(err[1], 2);
            ::execve(argp[0], argp.data(), envp.data());
            ::_exit(127);
        }
        for (int* fd : {&out[1], &err[1], &in[0]}) {
            if (*fd >= 0)
                ::close(*fd);
            *fd = -1;
        }
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        pollfd fds[2] = {{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
        std::string* text[2] = {&result.out, &result.err};
        int open = 2;
        while (open > 0) {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0)
                break;
            const int ready = ::poll(fds, 2, int(left.count()));
            if (ready < 0 && errno == EINTR)
                continue;
            if (ready <= 0)
                break;
            for (int i = 0; i < 2; ++i) {
                if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP)))
                    continue;
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
        closeAll();
        // The streams can end before the process does: the wait has the
        // same deadline.
        int status = 0;
        pid_t reaped = 0;
        while ((reaped = ::waitpid(pid, &status, WNOHANG)) == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(10ms);
        }
        if (reaped != pid) {
            // It did not end: the test fails, and the process goes.
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &status, 0);
            ADD_FAILURE() << binary << " " << (words.empty() ? std::string() : words.front()) << " did not end in 15 s";
            return result;
        }
        result.status = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        return result;
    }

    /// A directory with an ssh for `afterglow mux COMMAND --ssh`, first on
    /// PATH from now on: `ssh -G -- HOST` prints a configuration, and any
    /// other run runs @p remote (by default the command after "--" and the
    /// destination) as the host "far" would: in directories of its own
    /// (farRuntime(), its own state directory), with REMOTE_MARK and without
    /// LOCAL_ONLY, which this computer's commands have. So the proxy runs
    /// on this computer and reaches the daemon of that host. With @p mux,
    /// the build's aglow-mux is on its PATH; without, no aglow-mux is. Each
    /// run of the fake adds its arguments as a line to sshLog().
    void fakeSsh(bool mux, const std::string& remote = "exec /bin/sh -c \"$*\"")
    {
        const std::string directory = m_dir + "/ssh-bin";
        ASSERT_EQ(::mkdir(directory.c_str(), 0700), 0) << std::strerror(errno);
        // The test's own PATH after them: /usr/bin and /bin hold no env or
        // mkdir on every system (NixOS).
        const char* own = std::getenv("PATH");
        const std::string path = mux
            ? directory + ":" + std::filesystem::path(AFTERGLOW_BINARY).parent_path().string() + ":" + (own ? own : "")
            : directory;
        // The values come in the environment: no quoting of paths here.
        setVariable("FAKE_SSH_LOG", sshLog());
        setVariable("FAKE_HOST_PATH", path);
        setVariable("FAKE_HOST_RUNTIME", farRuntime());
        setVariable("FAKE_HOST_STATE", m_dir + "/far/state");
        setVariable("LOCAL_ONLY", "this-computer");
        ASSERT_TRUE(writeScript(
            directory + "/ssh",
            "printf '%s\\n' \"$*\" >> \"$FAKE_SSH_LOG\"\n"
            "if [ \"$1\" = -G ]; then printf 'host %s\\nhostname %s\\nuser me\\nport 22\\n' \"$3\" \"$3\"; exit 0; fi\n"
            "while [ \"$#\" -gt 0 ] && [ \"$1\" != -- ]; do shift; done\n"
            "shift 2\n"
            "unset LOCAL_ONLY\n"
            "export REMOTE_MARK=from-host PATH=\"$FAKE_HOST_PATH\" XDG_RUNTIME_DIR=\"$FAKE_HOST_RUNTIME\" "
            "XDG_STATE_HOME=\"$FAKE_HOST_STATE\"\n"
                + remote + "\n"));
        setVariable("PATH", directory + ":" + variable("PATH"));
    }

    /// infocmp on this computer and tic on the host, in the directory of
    /// fakeSsh(true), which is on both PATHs: infocmp prints an entry for
    /// afterglow, and tic writes it where -o says. The terminfo install of
    /// `afterglow mux new --ssh` then needs no tool of the test computer.
    void fakeTerminfoTools()
    {
        const std::string directory = m_dir + "/ssh-bin";
        ASSERT_TRUE(
            writeScript(directory + "/infocmp",
                        "printf '#\\tReconstructed via infocmp from file: fake\\nafterglow|fake,\\n\\tam,\\n'\n"));
        ASSERT_TRUE(writeScript(directory + "/tic",
                                "out=\n"
                                "while [ \"$#\" -gt 0 ]; do [ \"$1\" = -o ] && out=$2; shift; done\n"
                                "cat > /dev/null\n"
                                "mkdir -p \"$out/a\" && : > \"$out/a/afterglow\"\n"));
    }

    /// A socket at WAYLAND_DISPLAY in the test's runtime directory that
    /// takes no connection: `afterglow mux attach` finds a display, and no
    /// window could show on it.
    void fakeDisplay()
    {
        const std::string path = runtime() + "/wayland-test";
        m_displayFd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        ASSERT_GE(m_displayFd, 0) << std::strerror(errno);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        ASSERT_LT(path.size(), sizeof address.sun_path);
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        ASSERT_EQ(::bind(m_displayFd, reinterpret_cast<const sockaddr*>(&address), sizeof address), 0)
            << std::strerror(errno);
        setVariable("WAYLAND_DISPLAY", "wayland-test");
    }

    /// An afterglow whose aglow-mux is @p script: a link to the build's
    /// afterglow in a directory next to it (the same file system), with a
    /// script of that name. afterglow finds the daemon next to its own path.
    /// The directory goes with the test; its name holds the pid of this
    /// process, so a later test removes it when this one was killed first.
    std::string afterglowWithDaemon(const std::string& script)
    {
        const std::filesystem::path binary(AFTERGLOW_BINARY);
        std::string pattern =
            (binary.parent_path() / ("muxcommand-" + std::to_string(::getpid()) + "-XXXXXX")).string();
        if (!::mkdtemp(pattern.data())) {
            ADD_FAILURE() << std::strerror(errno);
            return {};
        }
        m_fakes.emplace_back(pattern);
        const std::filesystem::path directory(pattern);
        std::error_code error;
        std::filesystem::create_hard_link(binary, directory / "afterglow", error);
        if (error)
            std::filesystem::copy_file(binary, directory / "afterglow", error);
        if (error) {
            ADD_FAILURE() << error.message();
            return {};
        }
        std::ofstream(directory / "aglow-mux") << script;
        if (::chmod((directory / "aglow-mux").c_str(), 0700) != 0) {
            ADD_FAILURE() << std::strerror(errno);
            return {};
        }
        return (directory / "afterglow").string();
    }

    /// What `afterglow mux ls` says of session @p name: "attached",
    /// "detached", or empty for a session that it does not list.
    std::string stateOf(const std::string& name) const
    {
        std::istringstream lines(mux({"ls"}).out);
        for (std::string line; std::getline(lines, line);) {
            std::istringstream words(line);
            std::string first, second;
            words >> first >> second;
            if (first == name)
                return second;
        }
        return {};
    }

    /// A window of session @p name, as the daemon sees one: the attach call,
    /// and the stream on its connection, which shows the session until the
    /// daemon ends it. Its delete closes the stream. Nullptr on a failure.
    std::unique_ptr<PhosphorChannel::Stream> attachWindow(const std::string& name) const
    {
        AfterglowProto::IpcConnection connection;
        AfterglowProto::IpcConnection::Error error;
        if (!connection.connect(runtime() + "/aglow-mux/socket", &error, 2000)) {
            ADD_FAILURE() << "no daemon: " << error.message;
            return nullptr;
        }
        const auto result =
            connection.call(AfterglowMux::Target, AfterglowMux::Fn::Attach,
                            Json::Array{AfterglowMux::ProtocolVersion, name, Json::Object{}}, 5000, &error);
        if (!result) {
            ADD_FAILURE() << "attach failed: " << error.code << " " << error.message;
            return nullptr;
        }
        const int fd = connection.release(&error);
        if (fd < 0) {
            ADD_FAILURE() << "no stream: " << error.message;
            return nullptr;
        }
        // The window writes first, as a window does: the daemon starts the
        // stream when its hello comes.
        auto stream = std::make_unique<PhosphorChannel::Stream>(fd);
        stream->pump(0);
        return stream;
    }

    /// The directories of afterglowWithDaemon() whose test process is gone:
    /// a ctest timeout kills a test before its TearDown. One of a test that
    /// still runs, in parallel, stays.
    static void removeAbandonedAfterglows()
    {
        std::error_code error;
        for (const auto& entry :
             std::filesystem::directory_iterator(std::filesystem::path(AFTERGLOW_BINARY).parent_path(), error)) {
            const std::string name = entry.path().filename().string();
            constexpr std::string_view Prefix = "muxcommand-";
            if (!name.starts_with(Prefix))
                continue;
            // Without a valid pid: no process can own it.
            const std::optional<pid_t> pid =
                parsePid(std::string_view(name).substr(Prefix.size(), name.find('-', Prefix.size()) - Prefix.size()));
            std::error_code ignored;
            if (!pid || !alive(*pid))
                std::filesystem::remove_all(entry.path(), ignored);
        }
    }

    /// The test directories in /tmp whose test process is gone, with the
    /// daemons and the programs that they left.
    static void removeAbandonedRuns()
    {
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator("/tmp", error)) {
            const std::string name = entry.path().filename().string();
            if (!name.starts_with(RunPrefix))
                continue;
            const std::size_t start = RunPrefix.size();
            const std::optional<pid_t> pid =
                parsePid(std::string_view(name).substr(start, name.find('-', start) - start));
            if (pid && alive(*pid))
                continue;
            killProcessesWith(markersOf(entry.path().string()));
            std::error_code ignored;
            std::filesystem::remove_all(entry.path(), ignored);
        }
    }

    static constexpr std::string_view RunPrefix = "aglow-muxcommand-";

    std::string m_dir;
    std::vector<std::string> m_environment;
    std::vector<std::filesystem::path> m_fakes;
    int m_displayFd = -1; ///< fakeDisplay()
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
    EXPECT_NE(listed.out.find("NAME  STATE     PANES  AGENT\n1     detached      1  no\n"), std::string::npos)
        << listed.out;

    // A program runs and stdin is no terminal: no question, a refusal.
    const Result refused = mux({"kill", "1"});
    EXPECT_EQ(refused.status, 1);
    EXPECT_EQ(refused.err, "afterglow mux kill: session '1' runs sh: give --force to end it\n");
    EXPECT_NE(mux({"ls"}).out.find("detached"), std::string::npos);

    const Result killed = mux({"kill", "--force", "1"});
    EXPECT_EQ(killed.status, 0) << killed.err;
    // The daemon of a session of its own ends with its last session.
    EXPECT_TRUE(waitFor([this] {
        return daemons().empty();
    }));
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
    setVariable("AFTERGLOW_TEST_BINARY", AFTERGLOW_BINARY);
    ASSERT_TRUE(writeScript(wrapper, "trap '' CHLD\nexec \"$AFTERGLOW_TEST_BINARY\" \"$@\"\n"));
    const Result r = mux(newWaiting(), wrapper);
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

TEST_F(MuxCommandTest, UsageErrorsAndCommandsThatNeedADisplay)
{
    const std::string nameRule = std::string("the name must be ") + AfterglowMux::SessionNameRule + "\n";
    for (const auto& [args, status, message] : std::vector<std::tuple<std::vector<std::string>, int, std::string>>{
             {{"new", "--name"}, 2, "afterglow mux: --name needs a value\n"},
             {{"new", "--detach", "--cwd"}, 2, "afterglow mux: --cwd needs a value\n"},
             {{"new", "--detach", "--name", "-x"}, 2, "afterglow mux new: " + nameRule},
             {{"new", "--detach", "--cwd", "/no/such/directory"},
              1,
              "afterglow mux new: no directory /no/such/directory\n"},
             // The test has no Wayland display: no window, and no session for it.
             {{"new"}, 1, "afterglow mux new: no Wayland display shows a window here: "},
             {{"attach"}, 1, "afterglow mux attach: no Wayland display shows a window here: "},
             {{"attach", "bad name"}, 2, "afterglow mux attach: the name must be "},
             // An empty word is a name too, which the rule refuses.
             {{"attach", ""}, 2, "afterglow mux attach: " + nameRule},
             {{"attach", "a", "b"}, 2, "afterglow mux: unknown argument of attach: b\n"},
             // No display: refused before ssh runs.
             {{"attach", "--ssh", "host"}, 1, "afterglow mux attach: no Wayland display shows a window here: "},
             {{"detach"}, 2, "afterglow mux: detach takes the name of a session, outside a session pane\n"},
             {{"detach", "a", "b"}, 2, "afterglow mux: unknown argument of detach: b\n"},
             {{"detach", "bad name"}, 2, "afterglow mux detach: the name must be "},
             {{"detach", ""}, 2, "afterglow mux detach: " + nameRule},
             // A pane of this computer names no session of another host.
             {{"detach", "--ssh", "host"}, 2, "afterglow mux: detach --ssh takes the name of a session\n"},
             // No daemon: no session to detach, and none starts.
             {{"detach", "work"}, 1, "afterglow mux detach: no sessions\n"},
             {{"new", "--ssh", "host"}, 1, "afterglow mux new: no Wayland display shows a window here: "},
             {{"kill", "--ssh", "host"}, 2, "afterglow mux: kill takes the name of a session\n"},
             {{"kill", ""}, 2, "afterglow mux kill: " + nameRule},
             {{"ls", "x"}, 2, "afterglow mux: ls takes no arguments but --ssh DESTINATION\n"},
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
    const Result created = mux(newWaiting("now"));
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

// `afterglow mux COMMAND --ssh` reaches the daemon of another host through
// `aglow-mux proxy`, which ssh runs there. A fake ssh plays the host, whose
// daemon has directories of its own: what this computer's daemon has, the
// host's does not.
TEST_F(MuxCommandTest, ListsDetachesAndKillsTheSessionsOfAnotherHost)
{
    ASSERT_NO_FATAL_FAILURE(fakeSsh(true));
    // No daemon there, and none starts.
    const Result empty = mux({"ls", "--ssh", "far"});
    EXPECT_EQ(empty.status, 0) << empty.err;
    EXPECT_EQ(empty.err, "afterglow mux ls: no sessions\n");
    EXPECT_TRUE(daemons().empty());
    // A session there.
    std::vector<std::string> args{"new", "--ssh", "far", "--detach", "--name", "there"};
    for (const std::string& word : waitsForTheTest())
        args.push_back(word);
    const Result made = mux(args);
    ASSERT_EQ(made.status, 0) << made.err;
    EXPECT_EQ(made.out, "there\n");
    // The host's daemon, which this computer does not have.
    ASSERT_EQ(daemons().size(), 1u);
    EXPECT_FALSE(processesWith({"XDG_RUNTIME_DIR=" + farRuntime()}).empty());
    EXPECT_TRUE(processesWith({"XDG_RUNTIME_DIR=" + runtime()}).empty());
    const Result local = mux({"ls"});
    EXPECT_EQ(local.status, 0);
    EXPECT_EQ(local.err, "afterglow mux ls: no sessions\n");
    const Result listed = mux({"ls", "--ssh", "far"});
    EXPECT_EQ(listed.status, 0) << listed.err;
    EXPECT_NE(listed.out.find("there  detached      1"), std::string::npos) << listed.out;
    const Result detached = mux({"detach", "there", "--ssh", "far"});
    EXPECT_EQ(detached.status, 1);
    EXPECT_EQ(detached.err, "afterglow mux detach: no window shows session 'there'\n");
    // Only the host's session goes: a local kill finds none.
    const Result notHere = mux({"kill", "--force", "there"});
    EXPECT_EQ(notHere.status, 1);
    EXPECT_EQ(notHere.err, "afterglow mux kill: no sessions\n");
    const Result killed = mux({"kill", "--ssh", "far", "--force", "there"});
    EXPECT_EQ(killed.status, 0) << killed.err;
    EXPECT_TRUE(waitFor([this] {
        return daemons().empty();
    }));
    // Usage errors.
    EXPECT_EQ(mux({"ls", "--ssh"}).status, 2);
    EXPECT_EQ(mux({"ls", "--ssh", "a", "--ssh", "b"}).status, 2);
    EXPECT_EQ(mux({"detach", "--ssh", "far"}).status, 2);
    // Destinations that ssh would read as options, or as none.
    for (const std::string& destination : {std::string("-oProxyCommand=x"), std::string()}) {
        const Result notHost = mux({"ls", "--ssh", destination});
        EXPECT_EQ(notHost.status, 1) << destination;
        EXPECT_EQ(notHost.err, "afterglow mux ls: '" + destination + "' is no ssh destination\n");
    }
}

// A session of another host runs with that host's environment, which the
// proxy there has, and the terminal's variables over it: not with this
// computer's.
TEST_F(MuxCommandTest, ANewSessionOfAnotherHostHasThatHostsEnvironment)
{
    ASSERT_NO_FATAL_FAILURE(fakeSsh(true));
    const std::string out = m_dir + "/env";
    const Result made = mux({"new", "--detach", "--ssh", "far", "--name", "far1", "--", "sh", "-c",
                             "env > '" + out + ".part' && mv '" + out + ".part' '" + out + "'; " + untilTheTestEnds()});
    ASSERT_EQ(made.status, 0) << made.err;
    EXPECT_EQ(made.out, "far1\n");
    ASSERT_TRUE(waitFor([&] {
        return !readFile(out).empty();
    }));
    const std::string env = "\n" + readFile(out);
    EXPECT_NE(env.find("\nREMOTE_MARK=from-host\n"), std::string::npos) << env;
    EXPECT_NE(env.find("\nXDG_RUNTIME_DIR=" + farRuntime() + "\n"), std::string::npos) << env;
    // A variable that only this computer has.
    EXPECT_EQ(env.find("\nLOCAL_ONLY="), std::string::npos) << env;
    EXPECT_NE(env.find("\nCOLORTERM=truecolor\n"), std::string::npos) << env;
    EXPECT_NE(env.find("\nAFTERGLOW_SESSION=far1\n"), std::string::npos) << env;
    // afterglow's own entry where the host takes it, else one that it has.
    EXPECT_TRUE(env.find("\nTERM=afterglow\n") != std::string::npos
                || env.find("\nTERM=xterm-256color\n") != std::string::npos)
        << env;
    EXPECT_EQ(env.find("\nTERMINFO_DIRS="), std::string::npos) << env;
    // The directory is the host's: this computer does not check it.
    const Result relative = mux({"new", "--detach", "--ssh", "far", "--cwd", "here"});
    EXPECT_EQ(relative.status, 1);
    EXPECT_EQ(relative.err, "afterglow mux new: give an absolute directory of far with --cwd\n");
}

// Terminal.sshTerminfo fallback gives the session of another host
// xterm-256color, and installs nothing there.
TEST_F(MuxCommandTest, ANewSessionOfAnotherHostFallsBackWithoutAnInstall)
{
    ASSERT_TRUE(writeConfig(R"({"Terminal": {"term": "afterglow", "sshTerminfo": "fallback", "shell": ")" + testShell()
                            + R"("}})"));
    ASSERT_NO_FATAL_FAILURE(fakeSsh(true));
    const std::string out = m_dir + "/env";
    const Result made = mux({"new", "--detach", "--ssh", "far", "--name", "fb", "--", "sh", "-c",
                             "env > '" + out + ".part' && mv '" + out + ".part' '" + out + "'; " + untilTheTestEnds()});
    ASSERT_EQ(made.status, 0) << made.err;
    // No word of an entry that this computer lacks: none was looked for.
    EXPECT_TRUE(made.err.empty()) << made.err;
    ASSERT_TRUE(waitFor([&] {
        return !readFile(out).empty();
    }));
    const std::string env = "\n" + readFile(out);
    EXPECT_NE(env.find("\nTERM=xterm-256color\n"), std::string::npos) << env;
    const std::string log = readFile(sshLog());
    EXPECT_EQ(log.find("tic -x"), std::string::npos) << log;
    EXPECT_EQ(log.find("infocmp"), std::string::npos) << log;
}

// Terminal.term afterglow puts the entry on the host over the connection of
// the route, with what ssh -G read for the route: ssh -G runs once. The
// session gets TERM=afterglow, and no TERMINFO_DIRS of this computer.
TEST_F(MuxCommandTest, ANewSessionOfAnotherHostInstallsTheEntryWithOneSshG)
{
    ASSERT_TRUE(writeConfig(R"({"Terminal": {"term": "afterglow", "sshTerminfo": "install", "shell": ")" + testShell()
                            + R"("}})"));
    ASSERT_NO_FATAL_FAILURE(fakeSsh(true));
    ASSERT_NO_FATAL_FAILURE(fakeTerminfoTools());
    const std::string out = m_dir + "/env";
    const Result made = mux({"new", "--detach", "--ssh", "far", "--name", "inst", "--", "sh", "-c",
                             "env > '" + out + ".part' && mv '" + out + ".part' '" + out + "'; " + untilTheTestEnds()});
    ASSERT_EQ(made.status, 0) << made.err;
    EXPECT_TRUE(made.err.empty()) << made.err;
    EXPECT_TRUE(std::filesystem::exists(m_dir + "/home/.terminfo/a/afterglow"));
    const std::string log = readFile(sshLog());
    EXPECT_NE(log.find("tic -x"), std::string::npos) << log;
    int resolves = 0;
    std::istringstream lines(log);
    for (std::string line; std::getline(lines, line);)
        resolves += line.starts_with("-G ") ? 1 : 0;
    EXPECT_EQ(resolves, 1) << log;
    ASSERT_TRUE(waitFor([&] {
        return !readFile(out).empty();
    }));
    const std::string env = "\n" + readFile(out);
    EXPECT_NE(env.find("\nTERM=afterglow\n"), std::string::npos) << env;
    EXPECT_EQ(env.find("\nTERMINFO_DIRS="), std::string::npos) << env;
}

// ssh cannot read the destination (ssh -G fails, as for a config with an
// error): the command says so, and tries no connection.
TEST_F(MuxCommandTest, ADestinationThatSshCannotReadSaysSo)
{
    const std::string directory = m_dir + "/broken-ssh";
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    setVariable("FAKE_SSH_LOG", sshLog());
    ASSERT_TRUE(writeScript(directory + "/ssh", "printf '%s\\n' \"$*\" >> \"$FAKE_SSH_LOG\"\nexit 255\n"));
    setVariable("PATH", directory + ":" + variable("PATH"));
    for (const std::vector<std::string>& args : {std::vector<std::string>{"ls", "--ssh", "far"},
                                                 std::vector<std::string>{"new", "--detach", "--ssh", "far"}}) {
        const Result r = mux(args);
        EXPECT_EQ(r.status, 1) << args.front();
        EXPECT_EQ(r.err, "afterglow mux " + args.front() + ": ssh cannot read the destination 'far' (ssh -G)\n");
    }
    const std::string log = readFile(sshLog());
    std::istringstream lines(log);
    for (std::string line; std::getline(lines, line);)
        EXPECT_TRUE(line.starts_with("-G ")) << line;
    EXPECT_FALSE(log.empty());
    EXPECT_TRUE(daemons().empty());
}

// The host's shell has no aglow-mux to run: the message has its words.
TEST_F(MuxCommandTest, AHostWithoutAglowMuxSaysSo)
{
    ASSERT_NO_FATAL_FAILURE(fakeSsh(false));
    const Result r = mux({"ls", "--ssh", "far"});
    EXPECT_EQ(r.status, 1);
    EXPECT_TRUE(r.err.starts_with("afterglow mux ls: far cannot run aglow-mux: ")) << r.err;
    EXPECT_NE(r.err.find("not found"), std::string::npos) << r.err;
    EXPECT_NE(r.err.find(": install it there"), std::string::npos) << r.err;
}

// What the host writes on standard error reaches the terminal: as text, not
// as the sequences that it holds.
TEST_F(MuxCommandTest, AHostCannotWriteEscapesThroughSsh)
{
    ASSERT_NO_FATAL_FAILURE(fakeSsh(true, "printf '\\033]0;evil\\007\\n' >&2; exit 1"));
    const Result r = mux({"ls", "--ssh", "far"});
    EXPECT_EQ(r.status, 1);
    EXPECT_EQ(r.err.find('\x1b'), std::string::npos) << r.err;
    EXPECT_EQ(r.err.find('\x07'), std::string::npos) << r.err;
    EXPECT_NE(r.err.find("]0;evil"), std::string::npos) << r.err;
}

// A login file there that writes on standard output puts its text before the
// answers of aglow-mux: the message names the cause.
TEST_F(MuxCommandTest, ALoginThatWritesOnStdoutSaysSo)
{
    ASSERT_NO_FATAL_FAILURE(fakeSsh(true, "echo hello; exec /bin/sh -c \"$*\""));
    const Result r = mux({"ls", "--ssh", "far"});
    EXPECT_EQ(r.status, 1);
    EXPECT_EQ(r.err,
              "afterglow mux ls: far wrote text that is not from aglow-mux: a login file of the shell there, such as "
              ".bashrc, "
              ".zshenv or config.fish, writes on standard output when the shell is not interactive\n");
}

// attach to a session of another host signs in from the terminal first,
// which the window cannot: with no daemon there, it says so and opens no
// window. The display is a socket of the test that takes no connection.
TEST_F(MuxCommandTest, AttachToAnotherHostWithoutADaemonSaysSo)
{
    ASSERT_NO_FATAL_FAILURE(fakeSsh(true));
    ASSERT_NO_FATAL_FAILURE(fakeDisplay());
    const Result r = mux({"attach", "work", "--ssh", "far"});
    EXPECT_EQ(r.status, 1);
    EXPECT_EQ(r.err, "afterglow mux attach: no mux daemon runs on far\n");
    // The proxy ran there, without --start: no daemon starts for an attach.
    const std::string log = readFile(sshLog());
    EXPECT_NE(log.find("aglow-mux proxy\n"), std::string::npos) << log;
    EXPECT_TRUE(daemons().empty());
}

// The proxy carries one connection to the daemon of its host on its
// standard streams, and starts that daemon with --start.
TEST_F(MuxCommandTest, TheProxyStartsTheDaemonAndCarriesAConnection)
{
    const std::string proxy = (std::filesystem::path(AFTERGLOW_BINARY).parent_path() / "aglow-mux").string();
    const std::string version = R"({"type":"call","id":1,"target":"mux","fn":"version","args":[]})"
                                "\n";
    const Result none = run({"proxy"}, proxy, version);
    EXPECT_EQ(none.status, AfterglowMux::ProxyNoDaemon) << none.err;
    EXPECT_TRUE(none.out.empty()) << none.out;
    EXPECT_EQ(run({"proxy", "--other"}, proxy).status, 2);
    const Result started = run({"proxy", "--start"}, proxy, version);
    EXPECT_EQ(started.status, 0) << started.err;
    EXPECT_NE(started.out.find("\"protocol\":" + std::to_string(AfterglowMux::ProtocolVersion)), std::string::npos)
        << started.out;
    EXPECT_EQ(daemons().size(), 1u);
}

// `afterglow ctl --session` acts on a session that no window shows, with the
// daemon's pane IDs; so does `afterglow ctl` in a pane of the session.
TEST_F(MuxCommandTest, CtlActsOnASessionWithoutAWindow)
{
    ASSERT_TRUE(writeConfig(R"({"Control": {"allowReadText": true}, )" + shellSetting() + "}"));
    ASSERT_EQ(
        mux({"new", "--detach", "--name", "work", "--", "sh", "-c", "echo hel''lo; " + untilTheTestEnds()}).status, 0);

    const Result listed = ctl({"--session", "work", "ls"});
    EXPECT_EQ(listed.status, 0) << listed.err;
    // The first pane of the daemon, focused. Its program marks no prompts:
    // its shell is none.
    EXPECT_EQ(listed.out.rfind("PANE  WIN  TAB  SHELL        TITLE", 0), 0u) << listed.out;
    const std::size_t first = listed.out.find("\n1*");
    ASSERT_NE(first, std::string::npos) << listed.out;
    EXPECT_NE(listed.out.substr(first, listed.out.find('\n', first + 1) - first).find(" none "), std::string::npos)
        << listed.out;
    EXPECT_TRUE(waitFor([&] {
        return ctl({"--session", "work", "get-text"}).out.find("hello") != std::string::npos;
    }));
    const Result split = ctl({"--session", "work", "split", "--down"});
    EXPECT_EQ(split.status, 0) << split.err;
    EXPECT_EQ(split.out, "2\n");
    const Result json = ctl({"--session", "work", "ls", "--json"});
    EXPECT_EQ(json.status, 0) << json.err;
    EXPECT_NE(json.out.find("\"id\": 1"), std::string::npos) << json.out;
    EXPECT_NE(json.out.find("\"id\": 2"), std::string::npos) << json.out;
    // One pane of the two has the focus.
    const std::size_t focused = json.out.find("\"focused\": true");
    EXPECT_NE(focused, std::string::npos) << json.out;
    EXPECT_EQ(json.out.find("\"focused\": true", focused + 1), std::string::npos) << json.out;

    // What only a window does, a session without one refuses.
    const Result titled = ctl({"--session", "work", "set-title", "t"});
    EXPECT_EQ(titled.status, 1);
    EXPECT_EQ(titled.err, "afterglow ctl: no window shows session 'work', and only a window can do setTitle\n");
    const Result none = ctl({"--session", "nope", "ls"});
    EXPECT_EQ(none.status, 1);
    EXPECT_EQ(none.err, "afterglow ctl: no session named 'nope'\n");
    EXPECT_EQ(ctl({"--session", "bad name", "ls"}).status, 2);

    // In a pane of the session: its AFTERGLOW_SOCKET names the daemon, and a
    // stale AFTERGLOW_SESSION changes nothing. The command acts on the pane
    // of AFTERGLOW_PANE_ID, the first, whatever has the focus.
    m_environment.push_back("AFTERGLOW_SOCKET=" + runtime() + "/aglow-mux/socket");
    m_environment.push_back("AFTERGLOW_SESSION=before");
    m_environment.push_back("AFTERGLOW_PANE_ID=1");
    const Result inPane = ctl({"ls", "--json"});
    EXPECT_EQ(inPane.status, 0) << inPane.err;
    EXPECT_NE(inPane.out.find("\"id\": 1"), std::string::npos) << inPane.out;
    EXPECT_NE(inPane.out.find("\"id\": 2"), std::string::npos) << inPane.out;
    const Result closed = ctl({"close"});
    EXPECT_EQ(closed.status, 0) << closed.err;
    const Result one = ctl({"--session", "work", "ls", "--json"});
    EXPECT_EQ(one.status, 0) << one.err;
    EXPECT_EQ(one.out.find("\"id\": 1"), std::string::npos) << one.out;
    EXPECT_NE(one.out.find("\"id\": 2"), std::string::npos) << one.out;
}

// `new --agent` makes an agent session, which ls marks in its AGENT column:
// yes for it, no for another session, and no row ends in blanks.
TEST_F(MuxCommandTest, NewAgentMakesASessionThatLsMarks)
{
    std::vector<std::string> agent{"new", "--detach", "--agent", "--name", "agent"};
    const auto command = waitsForTheTest();
    agent.insert(agent.end(), command.begin(), command.end());
    const Result made = mux(agent);
    ASSERT_EQ(made.status, 0) << made.err;
    EXPECT_EQ(made.out, "agent\n");
    ASSERT_EQ(mux(newWaiting("plain")).status, 0);
    const Result listed = mux({"ls"});
    EXPECT_EQ(listed.status, 0) << listed.err;
    EXPECT_NE(listed.out.find("agent  detached      1  yes\n"), std::string::npos) << listed.out;
    EXPECT_NE(listed.out.find("plain  detached      1  no\n"), std::string::npos) << listed.out;
    EXPECT_EQ(listed.out.find(" \n"), std::string::npos) << listed.out;
}

// The session has the Control settings of the `afterglow mux new` that made
// it, which the daemon follows for `afterglow ctl --session`: by default no
// pane text, and none of the control target with Control.enabled off.
TEST_F(MuxCommandTest, CtlFollowsTheControlSettingsOfTheSession)
{
    ASSERT_EQ(mux(newWaiting("plain")).status, 0);
    const Result text = ctl({"--session", "plain", "get-text"});
    EXPECT_EQ(text.status, 1);
    EXPECT_EQ(text.err, "afterglow ctl: reading pane text is off (Control.allowReadText)\n");

    ASSERT_TRUE(writeConfig(R"({"Control": {"enabled": false}, )" + shellSetting() + "}"));
    ASSERT_EQ(mux(newWaiting("off")).status, 0);
    // The settings of ctl itself change nothing: the session's count.
    ASSERT_TRUE(writeConfig("{" + shellSetting() + "}"));
    const Result off = ctl({"--session", "off", "ls"});
    EXPECT_EQ(off.status, 1);
    EXPECT_EQ(off.err, "afterglow ctl: Control.enabled is off in the session\n");
    EXPECT_EQ(ctl({"--session", "plain", "ls"}).status, 0);
}

// A new tab of a session starts in the directory that ctl gives, with ~ as
// ctl's home, as a window expands it.
TEST_F(MuxCommandTest, CtlNewTabExpandsTheHomeOfItsDirectory)
{
    ASSERT_EQ(::mkdir((m_dir + "/home/src").c_str(), 0700), 0);
    ASSERT_EQ(mux(newWaiting("work")).status, 0);
    const Result tab = ctl({"--session", "work", "new-tab", "--cwd", "~/src"});
    EXPECT_EQ(tab.status, 0) << tab.err;
    EXPECT_EQ(tab.out, "2\n");
    const std::string expected = "\"cwd\": \"" + std::filesystem::canonical(m_dir + "/home/src").string() + "\"";
    std::string listed;
    EXPECT_TRUE(waitFor([&] {
        listed = ctl({"--session", "work", "ls", "--json"}).out;
        return listed.find(expected) != std::string::npos;
    })) << listed;
}

// A pane that `afterglow ctl --session` starts runs the shell of the
// session's environment, not the SHELL of the ctl process.
TEST_F(MuxCommandTest, CtlStartsTheShellOfTheSession)
{
    // No Terminal.shell: a new pane runs the login shell.
    ASSERT_TRUE(writeConfig("{}"));
    const std::string sessionShell = m_dir + "/session-shell";
    const std::string ctlShell = m_dir + "/ctl-shell";
    ASSERT_TRUE(writeScript(sessionShell, ": > '" + sessionShell + ".ran'\n" + untilTheTestEnds() + "\n"));
    ASSERT_TRUE(writeScript(ctlShell, ": > '" + ctlShell + ".ran'\n" + untilTheTestEnds() + "\n"));
    setVariable("SHELL", sessionShell);
    ASSERT_EQ(mux(newWaiting("shells")).status, 0);
    setVariable("SHELL", ctlShell);
    setVariable("ZDOTDIR", m_dir + "/zdotdir");
    const Result split = ctl({"--session", "shells", "split"});
    EXPECT_EQ(split.status, 0) << split.err;
    EXPECT_EQ(split.out, "2\n");
    EXPECT_TRUE(waitFor([&] {
        return std::filesystem::exists(sessionShell + ".ran");
    }));
    EXPECT_FALSE(std::filesystem::exists(ctlShell + ".ran"));
}

// How `afterglow ctl` finds the session when it fails: no daemon, a session
// pane without its pane ID, a socket that is not the daemon's, and a socket
// that is gone.
TEST_F(MuxCommandTest, CtlSaysWhyItFindsNoSession)
{
    const Result noDaemon = ctl({"--session", "work", "ls"});
    EXPECT_EQ(noDaemon.status, 1);
    EXPECT_EQ(noDaemon.err, "afterglow ctl: no mux daemon runs, so there is no session 'work'\n");

    ASSERT_EQ(mux(newWaiting("work")).status, 0);
    const std::vector<std::string> plain = m_environment;
    m_environment.push_back("AFTERGLOW_SOCKET=" + runtime() + "/aglow-mux/socket");
    m_environment.push_back("AFTERGLOW_SESSION=work");
    const Result noPane = ctl({"ls"});
    EXPECT_EQ(noPane.status, 1);
    EXPECT_EQ(noPane.err, "afterglow ctl: $AFTERGLOW_PANE_ID names no pane of the mux daemon\n");

    // An Afterglow's socket, which has no mux target.
    const std::string other = m_dir + "/other.sock";
    FakeServer afterglow(other, [](const AfterglowProto::IpcRequest& request) {
        return AfterglowProto::ipcError(request.id, AfterglowProto::IpcErrorCode::NoSuchTarget,
                                        "no target '" + request.target + "'");
    });
    ASSERT_TRUE(afterglow.listening()) << afterglow.error();
    m_environment = plain;
    m_environment.push_back("AFTERGLOW_SOCKET=" + other);
    m_environment.push_back("AFTERGLOW_SESSION=work");
    m_environment.push_back("AFTERGLOW_PANE_ID=1");
    const Result notDaemon = ctl({"ls"});
    EXPECT_EQ(notDaemon.status, 1);
    EXPECT_EQ(notDaemon.err,
              "afterglow ctl: $AFTERGLOW_SOCKET is not the socket of the mux daemon: clear $AFTERGLOW_SESSION\n");

    // No lookup of another socket: the message says what is wrong.
    m_environment = plain;
    m_environment.push_back("AFTERGLOW_SOCKET=" + runtime() + "/aglow-mux/gone");
    m_environment.push_back("AFTERGLOW_SESSION=work");
    m_environment.push_back("AFTERGLOW_PANE_ID=1");
    const Result gone = ctl({"ls"});
    EXPECT_EQ(gone.status, 1);
    EXPECT_TRUE(gone.err.starts_with(
        "afterglow ctl: the socket of the mux daemon of this session pane is gone: $AFTERGLOW_SOCKET names no file ("))
        << gone.err;
}

// detach ends the window of a session: of the session that it names, else in
// a session pane of the session that holds the pane. A connection of the test
// plays the window. A name goes before the pane, and a pane ID without the
// name of its session is no session pane.
TEST_F(MuxCommandTest, DetachEndsTheWindowOfTheNamedSessionOrOfThePane)
{
    for (const char* name : {"now", "other"}) {
        const Result created = mux(newWaiting(name));
        ASSERT_EQ(created.status, 0) << created.err;
    }
    auto window = attachWindow("now");
    ASSERT_TRUE(window);
    EXPECT_TRUE(waitFor([this] {
        return stateOf("now") == "attached";
    }));
    // In the first pane of the daemon (ID 1), which session "now" holds.
    m_environment.push_back("AFTERGLOW_SESSION=now");
    m_environment.push_back("AFTERGLOW_PANE_ID=1");
    const Result named = mux({"detach", "other"});
    EXPECT_EQ(named.status, 1);
    EXPECT_EQ(named.err, "afterglow mux detach: no window shows session 'other'\n");
    EXPECT_EQ(stateOf("now"), "attached");
    const Result ofPane = mux({"detach"});
    EXPECT_EQ(ofPane.status, 0) << ofPane.err;
    EXPECT_TRUE(ofPane.err.empty()) << ofPane.err;
    EXPECT_EQ(stateOf("now"), "detached");
    const Result again = mux({"detach"});
    EXPECT_EQ(again.status, 1);
    EXPECT_EQ(again.err, "afterglow mux detach: no window shows session 'now'\n");
    window.reset();
    // The pane ID alone.
    m_environment.erase(m_environment.end() - 2);
    const Result usage = mux({"detach"});
    EXPECT_EQ(usage.status, 2);
    // The usage text follows the message.
    EXPECT_TRUE(
        usage.err.starts_with("afterglow mux: detach takes the name of a session, outside a session pane\n\nUsage:"))
        << usage.err;
    // By name, outside a session pane.
    m_environment.pop_back();
    window = attachWindow("now");
    ASSERT_TRUE(window);
    EXPECT_TRUE(waitFor([this] {
        return stateOf("now") == "attached";
    }));
    const Result byName = mux({"detach", "now"});
    EXPECT_EQ(byName.status, 0) << byName.err;
    EXPECT_EQ(stateOf("now"), "detached");
}

// A platform other than Wayland, which afterglow would refuse after the
// session was made: refused first, with no session.
TEST_F(MuxCommandTest, AWindowNeedsTheWaylandPlatform)
{
    m_environment.push_back("QT_QPA_PLATFORM=offscreen");
    const Result r = mux({"new"});
    EXPECT_EQ(r.status, 1) << r.err;
    EXPECT_EQ(r.err,
              "afterglow mux new: QT_QPA_PLATFORM is offscreen, and Afterglow shows windows only on Wayland: give "
              "--detach\n");
    EXPECT_TRUE(daemons().empty());
}

// A daemon of a session of its own has /dev/null as its stderr: it reports
// why it cannot start through a pipe, and `afterglow mux new` prints that.
TEST_F(MuxCommandTest, SaysWhyTheDaemonDidNotStart)
{
    for (const std::string& directory : {runtime() + "/aglow-mux", state() + "/aglow-mux"}) {
        std::filesystem::create_directories(directory);
        ASSERT_EQ(::chmod(directory.c_str(), 0755), 0);
    }
    const Result r = mux(newWaiting());
    EXPECT_EQ(r.status, 1);
    EXPECT_EQ(r.err,
              "afterglow mux new: aglow-mux: no socket to listen on: " + runtime()
                  + "/aglow-mux must be a directory of this user with mode 0700 and " + state()
                  + "/aglow-mux must be a directory of this user with mode 0700\n");
    EXPECT_TRUE(waitFor([this] {
        return daemons().empty();
    }));
}

// A daemon that ends before it listens and says nothing (a signal, a crash)
// ends the report pipe without the byte that a listening daemon writes: the
// client fails at once, and does not wait out its start time for a socket.
TEST_F(MuxCommandTest, ADaemonThatEndsBeforeItListensFailsAtOnce)
{
    // An afterglow whose aglow-mux ends at once.
    const std::string afterglow = afterglowWithDaemon("#!/bin/sh\nkill -KILL $$\n");
    ASSERT_FALSE(afterglow.empty());
    const auto before = std::chrono::steady_clock::now();
    const Result r = mux(newWaiting(), afterglow);
    EXPECT_EQ(r.status, 1) << r.err;
    EXPECT_EQ(r.err, "afterglow mux new: aglow-mux ended before it listened\n");
    // At once, not after the start wait of five seconds: a second of it
    // left is room for a slow test computer.
    EXPECT_LT(std::chrono::steady_clock::now() - before, 4s);
}

// Two commands start a daemon at the same time: the second daemon finds the
// lock of the first, which may not listen yet. It reports that the socket is
// held, and its client waits for the first daemon, not only one look.
TEST_F(MuxCommandTest, ADaemonThatFindsTheSocketHeldLeavesTheClientToWaitForTheHolder)
{
    // Its aglow-mux reports the held socket and ends; the daemon of the
    // build listens a second later, as the holder that was slow to bind. Its
    // path comes in the environment: no quoting of it here.
    setVariable("AFTERGLOW_TEST_DAEMON",
                (std::filesystem::path(AFTERGLOW_BINARY).parent_path() / "aglow-mux").string());
    const std::string afterglow = afterglowWithDaemon(
        "#!/bin/sh\nprintf '\\001aglow-mux: another aglow-mux listens on %s\\n' \"$XDG_RUNTIME_DIR/aglow-mux/socket\" "
        ">&3\n"
        "exec 3>&-\n(sleep 1; unset "
        + std::string(AfterglowMux::ReportFdVariable) + "; exec \"$AFTERGLOW_TEST_DAEMON\") &\nexit 1\n");
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
    const std::string directory = runtime() + "/aglow-mux";
    ASSERT_EQ(::mkdir(directory.c_str(), 0700), 0);
    FakeServer server(directory + "/socket", [](const AfterglowProto::IpcRequest& request) {
        return AfterglowProto::ipcReply(request.id.value_or(0), Json::Object{{"version", "0"}, {"protocol", 99}});
    });
    ASSERT_TRUE(server.listening()) << server.error();
    for (const std::vector<std::string>& args :
         {std::vector<std::string>{"ls"}, std::vector<std::string>{"kill", "--force", "1"},
          std::vector<std::string>{"new", "--detach"}}) {
        const Result r = mux(args);
        EXPECT_EQ(r.status, 1) << args.front();
        EXPECT_NE(r.err.find("aglow-mux speaks protocol 99, and this program speaks "
                             + std::to_string(AfterglowMux::ProtocolVersion)
                             + ": end its sessions with the afterglow that started it, or wait until they end\n"),
                  std::string::npos)
            << r.err;
    }
    EXPECT_TRUE(daemons().empty());
}

// A daemon whose list of sessions the client cannot read: the command says
// so, and lists nothing.
TEST_F(MuxCommandTest, SaysWhenTheListOfTheDaemonDoesNotRead)
{
    const std::string directory = runtime() + "/aglow-mux";
    ASSERT_EQ(::mkdir(directory.c_str(), 0700), 0);
    std::atomic<bool> notAList{true};
    FakeServer server(directory + "/socket", [&notAList](const AfterglowProto::IpcRequest& request) {
        if (request.fn == AfterglowMux::Fn::ListSessions) {
            return AfterglowProto::ipcReply(request.id.value_or(0),
                                            notAList ? Json::Value(42)
                                                     : Json::Value(Json::Array{Json::Object{{"name", ""}}}));
        }
        return AfterglowProto::ipcReply(request.id.value_or(0),
                                        Json::Object{{"version", "0"}, {"protocol", AfterglowMux::ProtocolVersion}});
    });
    ASSERT_TRUE(server.listening()) << server.error();
    const Result notList = mux({"ls"});
    EXPECT_EQ(notList.status, 1);
    EXPECT_EQ(notList.err, "afterglow mux ls: the daemon's list of sessions is not a list\n");
    EXPECT_TRUE(notList.out.empty()) << notList.out;
    notAList = false;
    const Result unnamed = mux({"ls"});
    EXPECT_EQ(unnamed.status, 1);
    EXPECT_EQ(unnamed.err, "afterglow mux ls: the daemon sent a session that this afterglow cannot read\n");
}

// An afterglow with no aglow-mux next to it starts no daemon: the message
// names where it looked.
TEST_F(MuxCommandTest, SaysWhenNoAglowMuxIsNextToAfterglow)
{
    const std::string afterglow = afterglowWithDaemon("#!/bin/sh\nexit 0\n");
    ASSERT_FALSE(afterglow.empty());
    const std::string daemon = (std::filesystem::path(afterglow).parent_path() / "aglow-mux").string();
    ASSERT_TRUE(std::filesystem::remove(daemon));
    const Result r = mux(newWaiting(), afterglow);
    EXPECT_EQ(r.status, 1);
    EXPECT_EQ(r.err, "afterglow mux new: cannot find aglow-mux next to afterglow: " + daemon + "\n");
    EXPECT_TRUE(daemons().empty());
}

// The bounds of the protocol hold every value of the settings that it
// carries: the largest and the smallest that the configuration takes, in the
// options of a new session and in the settings that a window sends.
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
    // More variables to update than a window may send: the configuration
    // keeps the most that the daemon takes.
    std::string variables;
    for (int i = 0; i < 300; ++i)
        variables += (i ? ",\"V" : "\"V") + std::to_string(i) + "\"";
    for (
        const auto& [json, upper] :
        {std::pair{
             R"({"Terminal": {"scrollback": 1000000000}, "Colors": {"minimumContrast": 100}, "Graphics": {"quota": 1000000}, )"
             R"("Control": {"eventInterval": 1000000}, "Mux": {"keptNotifications": 1000000, "updateEnvironment": [)"
                 + variables + "]}}",
             true},
         std::pair{
             std::string(
                 R"({"Terminal": {"scrollback": -5}, "Colors": {"minimumContrast": -1}, "Graphics": {"quota": 0}, )"
                 R"("Control": {"eventInterval": -5}, "Mux": {"keptNotifications": -5}})"),
             false}}) {
        const std::string path = m_dir + "/config.json";
        std::ofstream(path, std::ios::trunc) << json;
        Afterglow::SettingsStore store(QFile::decodeName(path.c_str()));
        const Afterglow::Settings& settings = store.settings();
        AfterglowMux::SessionOptions options;
        options.terminal = settings.terminal;
        options.control = {settings.control.enabled,
                           settings.control.allowReadText,
                           {},
                           settings.control.eventInterval};
        options.keptNotifications = settings.mux.keptNotifications;
        // The configuration took the values, at its bounds.
        const AfterglowVt::TerminalOptions defaults;
        EXPECT_NE(options.terminal.maxScrollback, defaults.maxScrollback) << json;
        EXPECT_NE(options.terminal.graphicsQuota, defaults.graphicsQuota) << json;
        // 100 takes the upper bound; -1 the lower one, which is the default.
        EXPECT_DOUBLE_EQ(options.terminal.minimumContrast, upper ? 21.0 : 1.0) << json;
        EXPECT_EQ(options.control.eventInterval, upper ? int(AfterglowMux::MaxEventInterval) : 0) << json;
        EXPECT_EQ(options.keptNotifications, upper ? int(AfterglowMux::MaxKeptNotifications) : 0) << json;
        std::string error;
        EXPECT_TRUE(AfterglowMux::sessionOptions(AfterglowMux::toJson(options), &error)) << json << ": " << error;

        // As a window sends them (sessionSettings in MuxSessions.cpp), with
        // every variable unset.
        AfterglowMux::SessionSettings sent;
        sent.terminal = options.terminal;
        sent.control = options.control;
        sent.keptNotifications = options.keptNotifications;
        for (const QString& name : settings.mux.updateEnvironment)
            sent.environment.emplace_back(name.toStdString(), std::nullopt);
        if (upper) {
            EXPECT_EQ(sent.environment.size(), AfterglowMux::MaxUpdatedVariables);
        }
        error.clear();
        const auto taken = AfterglowMux::sessionSettings(AfterglowMux::toJson(sent), &error);
        EXPECT_TRUE(taken) << json << ": " << error;
        if (taken) {
            EXPECT_EQ(taken->control.eventInterval, options.control.eventInterval);
            EXPECT_EQ(taken->keptNotifications, options.keptNotifications);
            EXPECT_EQ(taken->environment.size(), sent.environment.size());
        }
    }
}

// A session that no window shows has the cell size of the font of the
// settings: its terminal answers a program's query of it (CSI 16 t), and
// places Sixel and iTerm2 images, which it drops without one.
TEST_F(MuxCommandTest, ADetachedSessionHasTheCellSizeOfTheFont)
{
    if (std::system("command -v python3 >/dev/null 2>&1") != 0)
        GTEST_SKIP() << "python3 is not installed";
    const std::string out = m_dir + "/cell";
    const std::string script = R"(import os, sys, time, tty
tty.setraw(0)
os.write(1, b'\x1b[16t')
r = b''
while not r.endswith(b't'):
    r += os.read(0, 1)
with open(sys.argv[1] + '.part', 'wb') as f:
    f.write(r)
os.rename(sys.argv[1] + '.part', sys.argv[1])
while True:
    try:
        os.kill(int(sys.argv[2]), 0)
    except OSError:
        break
    time.sleep(0.2)
)";
    ASSERT_EQ(mux({"new", "--detach", "--", "python3", "-I", "-c", script, out, std::to_string(::getpid())}).status, 0);
    ASSERT_TRUE(waitFor([&] {
        return !readFile(out).empty();
    }));
    // ESC [ 6 ; height ; width t
    const std::string reply = readFile(out);
    int height = 0;
    int width = 0;
    ASSERT_EQ(std::sscanf(reply.c_str(), "\x1b[6;%d;%dt", &height, &width), 2) << reply;
    EXPECT_GT(width, 0);
    EXPECT_GT(height, width);
}

// `afterglow ctl --session NAME watch` subscribes to the events of the session
// in the daemon, also while no window shows it, and prints them. When the
// session ends, the watch ends with status 1 and says why, where it once
// waited for ever (timeout's 124).
TEST_F(MuxCommandTest, CtlWatchPrintsTheEventsOfASession)
{
    ASSERT_EQ(mux(newWaiting("seen")).status, 0);
    const std::string printed = m_dir + "/watch.out";
    // The paths come in the environment: no quoting of them here.
    setVariable("AFTERGLOW_TEST_BINARY", AFTERGLOW_BINARY);
    setVariable("WATCH_OUT", printed);
    Result watched;
    std::thread watch([this, &watched] {
        watched = run({"-c", "exec timeout 12 \"$AFTERGLOW_TEST_BINARY\" ctl --session seen watch > \"$WATCH_OUT\""},
                      "/bin/sh");
    });
    // New tabs until the watch printed their events: then it subscribed.
    const auto seen = [&printed] {
        const std::string text = readFile(printed);
        return text.find(R"(,"event":"paneCreated"})") != std::string::npos
            && text.find(R"(,"event":"focusChanged"})") != std::string::npos;
    };
    for (int i = 0; i < 20 && !seen(); ++i) {
        EXPECT_EQ(ctl({"--session", "seen", "new-tab"}).status, 0);
        waitFor(seen, 500ms);
    }
    EXPECT_TRUE(seen()) << readFile(printed);
    // The session ends while the watch runs.
    EXPECT_EQ(mux({"kill", "--force", "seen"}).status, 0);
    watch.join();
    EXPECT_EQ(watched.status, 1) << watched.err;
    EXPECT_NE(watched.err.find("session 'seen' ended"), std::string::npos) << watched.err;
    // textChanged only with --text-changes: the new tabs drew their
    // prompts, and the watch above had none of it.
    EXPECT_EQ(readFile(printed).find(R"("event":"textChanged")"), std::string::npos) << readFile(printed);
    // A watch that comes after the end finds no session.
    const Result after = ctl({"--session", "seen", "watch"});
    EXPECT_EQ(after.status, 1) << after.err;
    EXPECT_NE(after.err.find("'seen'"), std::string::npos) << after.err;
}

// With --text-changes, the watch also prints the text changes of a session's
// panes, which come with the output of their programs.
TEST_F(MuxCommandTest, CtlWatchPrintsTextChangesWhenAsked)
{
    ASSERT_EQ(mux(newWaiting("typed")).status, 0);
    const std::string printed = m_dir + "/watch-text.out";
    setVariable("AFTERGLOW_TEST_BINARY", AFTERGLOW_BINARY);
    setVariable("WATCH_OUT", printed);
    Result watched;
    std::thread watch([this, &watched] {
        watched = run({"-c",
                       "exec timeout 12 \"$AFTERGLOW_TEST_BINARY\" ctl --session typed watch --text-changes "
                       "> \"$WATCH_OUT\""},
                      "/bin/sh");
    });
    // Output until the watch printed its change: then it subscribed.
    const auto seen = [&printed] {
        return readFile(printed).find(R"(,"event":"textChanged"})") != std::string::npos;
    };
    for (int i = 0; i < 20 && !seen(); ++i) {
        EXPECT_EQ(ctl({"--session", "typed", "send-text", "x"}).status, 0);
        waitFor(seen, 500ms);
    }
    EXPECT_TRUE(seen()) << readFile(printed);
    EXPECT_EQ(mux({"kill", "--force", "typed"}).status, 0);
    watch.join();
    EXPECT_EQ(watched.status, 1) << watched.err;
    // An option that watch does not take is a usage error.
    EXPECT_EQ(ctl({"--session", "typed", "watch", "--text"}).status, 2);
}
