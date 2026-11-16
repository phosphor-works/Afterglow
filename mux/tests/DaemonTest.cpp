// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// Each test runs its own aglow-mux (TestDaemon.h), so no test reaches the
// daemon of the user.

#include "TestDaemon.h"

#include "MuxLayout.h"

#include <atomic>
#include <cstring>
#include <tuple>

using namespace MuxTest;

namespace {

/// Checks that the daemon of @p daemon removed its sockets and their locks.
void expectSocketsGone(const TestDaemon& daemon)
{
    for (const std::string& path : {daemon.socket(), daemon.stateSocket()}) {
        EXPECT_FALSE(exists(path)) << path;
        EXPECT_FALSE(exists(path + ".lock")) << path;
    }
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
    // The answer names the session and its first pane, a new one each time.
    std::int64_t lastPane = 0;
    for (const char* expected : {"1", "2"}) {
        const auto created = daemon.newSession(daemon.options(Waiting));
        ASSERT_TRUE(created) << daemon.lastError();
        EXPECT_EQ((*created)["name"].toString(), expected);
        const auto pane = (*created)["pane"].toInteger();
        ASSERT_TRUE(pane) << Json::write(*created);
        EXPECT_GT(*pane, lastPane);
        lastPane = *pane;
    }
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "work"))) << daemon.lastError();
    IpcConnection::Error error;
    EXPECT_FALSE(daemon.newSession(daemon.options(Waiting, "work"), &error));
    EXPECT_EQ(error.code, Error::SessionExists);
    EXPECT_EQ(names(daemon.sessions()), (std::vector<std::string>{"keep", "1", "2", "work"}));
    for (const auto& session : daemon.sessions()) {
        EXPECT_FALSE(session.attached);
        EXPECT_EQ(session.panes, 1);
    }

    ASSERT_TRUE(daemon.call(Fn::KillSession, Json::Array{"1"}));
    EXPECT_FALSE(daemon.call(Fn::KillSession, Json::Array{"1"}, &error));
    EXPECT_EQ(error.code, Error::NoSuchSession);
    // A number never comes back: a kill that listed "1" cannot end a new session.
    const auto again = daemon.newSession(daemon.options(Waiting));
    ASSERT_TRUE(again);
    EXPECT_EQ((*again)["name"].toString(), "3");
    // A number that a name holds is skipped.
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "4")));
    const auto skipped = daemon.newSession(daemon.options(Waiting));
    ASSERT_TRUE(skipped);
    EXPECT_EQ((*skipped)["name"].toString(), "5");
}

TEST(MuxDaemon, RefusesBadCalls)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    IpcConnection::Error error;
    EXPECT_FALSE(daemon.call("noSuchFn", {}, &error));
    EXPECT_EQ(error.code, IpcErrorCode::NoSuchFn);
    EXPECT_FALSE(daemon.call(Fn::ListSessions, {}, &error, "noSuchTarget"));
    EXPECT_EQ(error.code, IpcErrorCode::NoSuchTarget);
    EXPECT_FALSE(daemon.call(Fn::NewSession, Json::Array{ProtocolVersion + 1, toJson(daemon.options(Waiting))}, &error));
    EXPECT_EQ(error.code, Error::VersionMismatch);
    EXPECT_NE(error.message.find(std::to_string(ProtocolVersion + 1)), std::string::npos) << error.message;
    // The number of arguments, and their types. A protocol that is no
    // number is another protocol.
    const std::tuple<const char*, Json::Array, const char*> rows[] = {
        {Fn::NewSession, Json::Array{ProtocolVersion}, IpcErrorCode::InvalidArg},
        {Fn::NewSession, Json::Array{"2", toJson(daemon.options(Waiting))}, Error::VersionMismatch},
        {Fn::KillSession, Json::Array{}, IpcErrorCode::InvalidArg},
        {Fn::KillSession, Json::Array{"a", "b"}, IpcErrorCode::InvalidArg},
        {Fn::KillSession, Json::Array{1}, IpcErrorCode::InvalidArg},
    };
    for (const auto& [fn, args, code] : rows) {
        SCOPED_TRACE(std::string(fn) + " " + Json::write(Json::Value(args)));
        EXPECT_FALSE(daemon.call(fn, args, &error));
        EXPECT_EQ(error.code, code) << error.message;
    }
    Json::Object bad = toJson(daemon.options(Waiting)).toObject();
    for (auto& [key, value] : bad) {
        if (key == "name") value = "no spaces";
    }
    EXPECT_FALSE(daemon.call(Fn::NewSession, Json::Array{ProtocolVersion, std::move(bad)}, &error));
    EXPECT_EQ(error.code, IpcErrorCode::InvalidArg);
    EXPECT_FALSE(daemon.newSession(daemon.options({"/no/such/program"}), &error));
    EXPECT_EQ(error.code, Error::SpawnFailed);
    // A request of another type than "call".
    RawClient raw(daemon.socket());
    ASSERT_TRUE(raw.connected());
    ASSERT_TRUE(raw.send(R"({"type":"subscribe","id":7,"target":"mux","signal":"x"})" "\n"));
    const auto reply = Json::parse(raw.line());
    ASSERT_TRUE(reply);
    EXPECT_EQ((*reply)["code"].toString(), IpcErrorCode::MalformedRequest) << Json::write(*reply);
    EXPECT_EQ(names(daemon.sessions()), std::vector<std::string>{"keep"});
}

// A session pane's AFTERGLOW_SOCKET names the daemon: `aglow run` there, and
// a control client that selected no session, get an answer that says why
// they cannot act (MuxControl.* for one that selected a session).
TEST(MuxDaemon, ControlAndCommandCallsFailWithAReason)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    for (const auto& [target, message] : std::vector<std::pair<std::string, std::string>>{
             {"control", "no session is selected: afterglow ctl --session NAME, or afterglow ctl in a session pane, selects one"},
             {AfterglowProto::Run::Target, "only a client in a session pane reaches the mux daemon for the commands of scripts"}}) {
        SCOPED_TRACE(target);
        IpcConnection::Error error;
        EXPECT_FALSE(daemon.call("listPanes", {}, &error, target));
        EXPECT_EQ(error.code, IpcErrorCode::CallFailed);
        EXPECT_EQ(error.message, message);
        // Not only calls: `afterglow ctl watch` subscribes.
        RawClient raw(daemon.socket());
        EXPECT_TRUE(raw.connected());
        if (!raw.connected() || !raw.send(R"({"type":"subscribe","id":3,"target":")" + target + R"(","signal":"x"})" "\n")) {
            ADD_FAILURE() << "cannot subscribe";
            continue;
        }
        const auto reply = Json::parse(raw.line());
        if (!reply) {
            ADD_FAILURE() << "no reply";
            continue;
        }
        EXPECT_EQ((*reply)["code"].toString(), IpcErrorCode::CallFailed) << Json::write(*reply);
        EXPECT_EQ((*reply)["message"].toString(), message) << Json::write(*reply);
    }
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
    EXPECT_TRUE(daemon.alive());
    ASSERT_TRUE(daemon.call(Fn::KillSession, Json::Array{"stays"}));
    EXPECT_EQ(daemon.waitExit(), 0) << daemon.errors();
    // It removed its sockets and their locks.
    expectSocketsGone(daemon);
}

TEST(MuxDaemon, ADaemonThatNobodyReachesEnds)
{
    TestDaemon daemon;
    ASSERT_GT(daemon.spawnDaemon(), 0);
    // The start wait (Daemon::StartWaitMs) is ten seconds. A daemon that is
    // still there fails the test, and the fixture kills it.
    const auto status = daemon.waitExit(15s);
    ASSERT_TRUE(status) << "the daemon still runs " << daemon.errors();
    EXPECT_EQ(*status, 0) << daemon.errors();
}

TEST(MuxDaemon, ASecondDaemonRefusesToStart)
{
    // The race of two daemons that start at once is the lock's, which
    // Proto.IpcServer.TheLockAloneDecidesWhoListens covers.
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    // Its report says that the socket is held, with a byte of its own: the
    // client waits for the holder then, which may not listen yet.
    int report[2] = {-1, -1};
    ASSERT_EQ(::pipe2(report, O_CLOEXEC), 0);
    const pid_t second = daemon.spawnDaemon(false, true, report[1]);
    ::close(report[1]);
    const std::optional<int> status = second > 0 ? daemon.waitExit(5s, true) : std::nullopt;
    // Read only from a daemon that ended: one that still runs holds the
    // write end, and the read would wait for it.
    const std::string reported = status ? readFile("/proc/self/fd/" + std::to_string(report[0])) : std::string();
    ::close(report[0]);
    ASSERT_GT(second, 0);
    ASSERT_TRUE(status) << "the second daemon still runs " << daemon.errors();
    EXPECT_EQ(*status, 1) << daemon.errors();
    EXPECT_EQ(reported, std::string(1, AfterglowMux::ReportHeld) + "aglow-mux: another aglow-mux listens on " + daemon.socket() + "\n");
    EXPECT_NE(daemon.errors().find("another aglow-mux listens on " + daemon.socket()), std::string::npos) << daemon.errors();
    // The first one still serves, on both sockets.
    EXPECT_EQ(names(daemon.sessions()), std::vector<std::string>{"keep"});
    daemon.callOn(daemon.stateSocket());
    EXPECT_EQ(names(daemon.sessions()), std::vector<std::string>{"keep"});
}

namespace {

/// A daemon that @p signal ends: it ends its sessions first, and their
/// programs with them, and then itself with the signal. The program ignores
/// the hangup, so only the daemon's end (the PTY's reaper) ends it: a
/// daemon that the signal killed outright would leave it, and its sockets.
void endsWith(int signal)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string pidFile = daemon.dir() + "/pid";
    ASSERT_TRUE(daemon.newSession(daemon.options({"/bin/sh", "-c", "trap '' HUP; echo $$ > '" + pidFile + "'; exec sleep 60"})));
    ASSERT_TRUE(waitFor([&] { return readFile(pidFile).find('\n') != std::string::npos; }));
    const pid_t program = pid_t(std::strtol(readFile(pidFile).c_str(), nullptr, 10));
    ASSERT_GT(program, 0);
    ASSERT_EQ(::kill(daemon.pid(), signal), 0);
    EXPECT_EQ(daemon.waitExit(), 128 + signal) << daemon.errors();
    expectSocketsGone(daemon);
    // Gone, or a zombie that waits for its reaper.
    EXPECT_TRUE(waitFor([&] { return !running(program); }));
    // A program that a failure left behind ends here.
    if (running(program)) ::kill(program, SIGKILL);
}

} // namespace

TEST(MuxDaemon, SigtermSigintAndSighupEndTheSessionsAndTheirPrograms)
{
    for (const int signal : {SIGTERM, SIGINT, SIGHUP}) {
        SCOPED_TRACE(::strsignal(signal));
        endsWith(signal);
    }
}

// As under nohup: a hangup that the daemon got as ignored stays ignored, and
// its sessions stay. SIGTERM still ends it, after its sessions.
TEST(MuxDaemon, AnIgnoredHangupStaysIgnored)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start(true, true)) << daemon.errors();
    ::kill(daemon.pid(), SIGHUP);
    std::this_thread::sleep_for(300ms);
    EXPECT_TRUE(daemon.alive()) << daemon.errors();
    EXPECT_EQ(names(daemon.sessions()), std::vector<std::string>{"keep"});
    ::kill(daemon.pid(), SIGTERM);
    EXPECT_EQ(daemon.waitExit(), 128 + SIGTERM) << daemon.errors();
    expectSocketsGone(daemon);
}

// The variables of the client reach the pane, from the session's environment
// and from the launch of the pane alike. The daemon sets the AFTERGLOW_
// variables that lead to it over either, and removes AFTERGLOW_OVERLAY.
TEST(MuxDaemon, TheDaemonSetsTheAfterglowVariablesOverTheClients)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    // Each place alone: a failure of the first does not skip the second.
    const auto setsOver = [&daemon](bool inLaunch) {
        const std::string name = inLaunch ? "launch" : "session";
        SCOPED_TRACE(name);
        const std::string out = daemon.dir() + "/env-" + name;
        SessionOptions o = daemon.options(writesEnvironment(out), name);
        auto& variables = inLaunch ? o.launch.environment : o.environment;
        variables.emplace_back("FROM_CLIENT", "yes");
        variables.emplace_back("AFTERGLOW_PANE_ID", "999");
        variables.emplace_back("AFTERGLOW_SESSION", "other");
        variables.emplace_back("AFTERGLOW_OVERLAY", "1");
        variables.emplace_back("AFTERGLOW_SOCKET", "/elsewhere");
        const auto created = daemon.newSession(o);
        ASSERT_TRUE(created) << daemon.lastError();
        const auto pane = (*created)["pane"].toInteger();
        ASSERT_TRUE(pane) << Json::write(*created);
        ASSERT_TRUE(waitFor([&] { return !readFile(out).empty(); }));
        const std::string env = "\n" + readFile(out);
        EXPECT_NE(env.find("\nFROM_CLIENT=yes\n"), std::string::npos) << env;
        EXPECT_NE(env.find("\nAFTERGLOW_SESSION=" + name + "\n"), std::string::npos) << env;
        EXPECT_EQ(env.find("\nAFTERGLOW_SESSION=other\n"), std::string::npos) << env;
        // The daemon's runtime socket, which serves the control target of
        // the session once a client selects it (MuxControl.*), and refuses
        // a client that selected none (ControlAndCommandCallsFailWithAReason).
        EXPECT_NE(env.find("\nAFTERGLOW_SOCKET=" + daemon.socket() + "\n"), std::string::npos) << env;
        EXPECT_EQ(env.find("\nAFTERGLOW_SOCKET=/elsewhere\n"), std::string::npos) << env;
        // The daemon's own pane ID, not the client's: that of the answer.
        EXPECT_NE(env.find("\nAFTERGLOW_PANE_ID=" + std::to_string(*pane) + "\n"), std::string::npos) << env;
        EXPECT_EQ(env.find("\nAFTERGLOW_OVERLAY="), std::string::npos) << env;
    };
    setsOver(true);
    setsOver(false);
}

// A session of another host takes the environment of the process that made
// it (the proxy, in the user's ssh login there), with the client's variables
// over it, and runs the user's shell as a login shell in HOME.
TEST(MuxDaemon, ASessionCanTakeTheEnvironmentOfItsCaller)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    // A shell that says how it ran: its arguments and its directory.
    const std::string home = daemon.dir() + "/home";
    const std::string shell = daemon.dir() + "/shell";
    const std::string out = daemon.dir() + "/ran";
    {
        std::ofstream script(shell);
        script << "#!/bin/sh\nprintf '%s\\n' \"$*\" \"$PWD\" \"$PATH\" \"$FROM_CLIENT\" > '" << out
               << ".part' && mv '" << out << ".part' '" << out << "'\nexec sleep 60\n";
    }
    ASSERT_EQ(::chmod(shell.c_str(), 0700), 0);
    if (::access(shell.c_str(), X_OK) != 0) GTEST_SKIP() << daemon.dir() << " runs no program";
    // The caller is this process: its environment from its start, which
    // /proc shows, PATH among it. That is all of the developer's
    // environment, which TestDaemon::cleanEnvironment() never sees: the
    // shell of this session runs only printf, mv and sleep, nothing that
    // looks up a socket, a display or a bus.
    SessionOptions o = daemon.options({}, "far");
    o.environment = {{"FROM_CLIENT", "yes"}, {"SHELL", shell}, {"HOME", home}};
    o.launch.workingDirectory.clear();
    o.callerEnvironment = true;
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return !readFile(out).empty(); }));
    const char* path = std::getenv("PATH");
    EXPECT_EQ(readFile(out), "-l\n" + home + "\n" + (path ? path : "") + "\nyes\n");
}

TEST(MuxDaemon, ThePaneEnvironmentLeavesOutTheDaemonsOwn)
{
    // As a daemon in a systemd unit has INVOCATION_ID, which the session has
    // not. Not an AFTERGLOW_ name: the daemon removes some of those itself,
    // which would pass this test without the rule.
    TestDaemon daemon;
    daemon.daemonOnly("MUX_TEST_DAEMON_ONLY", "1");
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string out = daemon.dir() + "/env";
    ASSERT_TRUE(daemon.newSession(daemon.options(writesEnvironment(out))));
    ASSERT_TRUE(waitFor([&] { return !readFile(out).empty(); }));
    const std::string env = "\n" + readFile(out);
    EXPECT_NE(env.find("\nPATH="), std::string::npos) << env;
    EXPECT_EQ(env.find("\nMUX_TEST_DAEMON_ONLY="), std::string::npos) << env;
}

// The programs get the umask of the client and the descriptor limit that the
// daemon had before it raised its own: the first pane of a session, a pane
// that a window adds, and a pane of a session that panes broke out to.
TEST(MuxDaemon, ThePaneGetsTheUmaskOfTheClientAndTheOriginalFileLimit)
{
    rlimit limit{};
    ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &limit), 0);
    if (limit.rlim_max <= 256) GTEST_SKIP() << "the hard limit leaves no room below it";
    // A mask that the test's process does not have: the daemon gets the
    // test's, and a daemon that gave its own would pass with it.
    const mode_t own = ::umask(0);
    ::umask(own);
    const mode_t mask = own == 027 ? 037 : 027;
    TestDaemon daemon;
    // The daemon raises 256 to the hard limit for itself.
    daemon.startFileLimit(256);
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const auto writesLimits = [](const std::string& out) {
        return std::vector<std::string>{"/bin/sh", "-c", "{ umask; ulimit -n; } > '" + out + ".part' && mv '" + out + ".part' '" + out + "'; exec sleep 60"};
    };
    const auto expectLimits = [mask](const std::string& out) {
        ASSERT_TRUE(waitFor([&] { return !readFile(out).empty(); })) << out;
        std::istringstream lines(readFile(out));
        std::string text;
        long files = 0;
        lines >> text >> files;
        EXPECT_EQ(std::strtol(text.c_str(), nullptr, 8), long(mask)) << out << ": " << text;
        EXPECT_EQ(files, 256) << out;
    };
    const auto newPane = [&](const std::string& session, const std::string& out) -> std::optional<std::uint32_t> {
        Launch launch;
        launch.argv = writesLimits(out);
        launch.workingDirectory = daemon.dir();
        const auto result = daemon.call(Fn::NewPane, Json::Array{ProtocolVersion, session, toJson(launch), toJson(PaneSize{})});
        return result ? paneIdFrom((*result)["pane"]) : std::nullopt;
    };
    const std::string out = daemon.dir() + "/limits";
    SessionOptions o = daemon.options(writesLimits(out), "s");
    o.umask = int(mask);
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    {
        SCOPED_TRACE("the first pane");
        expectLimits(out);
    }
    const auto added = newPane("s", out + "-added");
    ASSERT_TRUE(added) << daemon.lastError();
    {
        SCOPED_TRACE("a pane that a window adds");
        expectLimits(out + "-added");
    }
    Layout moved;
    moved.addTab(*added);
    const auto broke = daemon.call(Fn::BreakOut, Json::Array{ProtocolVersion, "s", toJson(moved)});
    ASSERT_TRUE(broke) << daemon.lastError();
    ASSERT_TRUE(newPane((*broke)["name"].toString(), out + "-moved")) << daemon.lastError();
    {
        SCOPED_TRACE("a pane of a session that panes broke out to");
        expectLimits(out + "-moved");
    }
}

TEST(MuxDaemon, ListsTheJobsOfASession)
{
    if (!pythonFound()) GTEST_SKIP() << "python3 is not installed";
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    // A shell that waits: no job.
    const auto first = daemon.sessions();
    ASSERT_EQ(first.size(), 1u) << daemon.lastError();
    EXPECT_TRUE(first.front().jobs.empty());
    // A command launch counts as running while it has the foreground. The
    // program asks the daemon itself, through AFTERGLOW_SOCKET: from inside
    // its own pane, it leads the foreground, and is no job of that pane. A
    // child of it in its group (a shell escape of an editor) is not the
    // program: the program is a job to ask about.
    const std::string out = daemon.dir() + "/inside";
    const std::string child = daemon.dir() + "/child";
    const std::string script = R"(import json, os, socket, sys, time
def ask(path):
    s = socket.socket(socket.AF_UNIX)
    s.connect(os.environ['AFTERGLOW_SOCKET'])
    s.sendall(b'{"type":"call","id":1,"target":"mux","fn":"listSessions","args":[]}\n')
    data = b''
    while not data.endswith(b'\n'):
        more = s.recv(4096)
        if not more:
            sys.exit(1)
        data += more
    with open(path + '.part', 'wb') as f:
        f.write(data)
    os.rename(path + '.part', path)
ask(sys.argv[1])
if os.fork() == 0:
    ask(sys.argv[2])
    os._exit(0)
os.wait()
time.sleep(60)
)";
    SessionOptions o = daemon.options({"python3", "-I", "-c", script, out, child}, "job");
    o.launch.launchedProgram = "sleeper";
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    /// The jobs of the session "job" in the reply in @p file.
    const auto jobsIn = [](const std::string& file) -> std::optional<std::vector<std::string>> {
        const auto reply = Json::parse(readFile(file));
        if (!reply) return std::nullopt;
        for (const Json::Value& item : (*reply)["result"].toArray()) {
            const auto info = sessionInfo(item);
            if (info && info->name == "job") return info->jobs;
        }
        return std::nullopt;
    };
    ASSERT_TRUE(waitFor([&] { return !readFile(out).empty() && !readFile(child).empty(); })) << daemon.errors();
    EXPECT_EQ(jobsIn(out), std::vector<std::string>{}) << readFile(out);
    EXPECT_EQ(jobsIn(child), std::vector<std::string>{"sleeper"}) << readFile(child);
    // From outside the pane, the launch is its job.
    bool seen = false;
    for (const auto& session : daemon.sessions()) {
        if (session.name != "job") continue;
        seen = true;
        EXPECT_EQ(session.jobs, std::vector<std::string>{"sleeper"});
    }
    EXPECT_TRUE(seen) << daemon.lastError();
}

TEST(MuxDaemon, ListensOnTheStateDirectoryToo)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    for (const std::string& directory : {daemon.runtime() + "/aglow-mux", daemon.stateDirectory(), daemon.stateHome()}) {
        struct stat info{};
        if (::lstat(directory.c_str(), &info) != 0) {
            ADD_FAILURE() << "no " << directory;
            continue;
        }
        EXPECT_TRUE(S_ISDIR(info.st_mode)) << directory;
        EXPECT_EQ(info.st_mode & 0777, 0700u) << directory;
    }
    struct stat info{};
    ASSERT_EQ(::lstat(daemon.stateSocket().c_str(), &info), 0) << daemon.errors();
    EXPECT_TRUE(S_ISSOCK(info.st_mode));
    daemon.callOn(daemon.stateSocket());
    EXPECT_EQ(names(daemon.sessions()), std::vector<std::string>{"keep"});
    ASSERT_TRUE(daemon.call(Fn::KillSession, Json::Array{"keep"}));
    EXPECT_EQ(daemon.waitExit(), 0) << daemon.errors();
    EXPECT_FALSE(exists(daemon.stateSocket()));
}

// A directory that is open to others, or a link, is no directory for the
// socket: the daemon says why and serves on the other.
namespace {

/// One case of SkipsADirectoryThatIsNotPrivate: a failed check ends this
/// case only.
void skipsDirectory(bool link)
{
    {
        TestDaemon daemon;
        const std::string directory = daemon.runtime() + "/aglow-mux";
        if (link) {
            ASSERT_EQ(::mkdir((daemon.dir() + "/elsewhere").c_str(), 0700), 0);
            ASSERT_EQ(::symlink((daemon.dir() + "/elsewhere").c_str(), directory.c_str()), 0);
        } else {
            ASSERT_EQ(::mkdir(directory.c_str(), 0700), 0);
            ASSERT_EQ(::chmod(directory.c_str(), 0755), 0);
        }
        daemon.callOn(daemon.stateSocket());
        ASSERT_TRUE(daemon.start()) << daemon.errors();
        EXPECT_NE(daemon.errors().find(directory + " must be a directory of this user with mode 0700"), std::string::npos)
            << daemon.errors();
        EXPECT_FALSE(exists(daemon.socket()));
        EXPECT_FALSE(exists(daemon.dir() + "/elsewhere/socket"));
        // The panes name the socket that the daemon has.
        const std::string out = daemon.dir() + "/env";
        ASSERT_TRUE(daemon.newSession(daemon.options(writesEnvironment(out)))) << daemon.lastError();
        ASSERT_TRUE(waitFor([&] { return !readFile(out).empty(); }));
        EXPECT_NE(readFile(out).find("\nAFTERGLOW_SOCKET=" + daemon.stateSocket() + "\n"), std::string::npos) << readFile(out);
    }
}

} // namespace

TEST(MuxDaemon, SkipsADirectoryThatIsNotPrivate)
{
    for (const bool link : {false, true}) {
        SCOPED_TRACE(link ? "a link" : "a directory open to others");
        skipsDirectory(link);
    }
}

// logind removes the runtime directory at the last logout. The sessions
// that start after that name the socket that clients can still reach.
TEST(MuxDaemon, PanesNameASocketThatStillExists)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_EQ(::unlink(daemon.socket().c_str()), 0);
    daemon.callOn(daemon.stateSocket());
    const std::string out = daemon.dir() + "/env";
    ASSERT_TRUE(daemon.newSession(daemon.options(writesEnvironment(out)))) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return !readFile(out).empty(); }));
    EXPECT_NE(readFile(out).find("\nAFTERGLOW_SOCKET=" + daemon.stateSocket() + "\n"), std::string::npos) << readFile(out);
}

// A client that sends calls without a pause and reads no answer does not
// keep the daemon from the others (Daemon::MaxLinesPerRead).
TEST(MuxDaemon, APipeliningClientDoesNotStarveTheOthers)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    RawClient flood(daemon.socket());
    ASSERT_TRUE(flood.connected());
    // Calls in batches, faster than the daemon answers them, until the
    // other call got its answer, or for longer than its wait: a daemon that
    // read the flood to its end would never get to the other call.
    std::atomic<bool> done{false};
    std::atomic<int> sent{0};
    std::jthread writer([&] {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        int next = 1;
        while (!done && std::chrono::steady_clock::now() < deadline) {
            std::string batch;
            for (int i = 0; i < 100; ++i, ++next) batch += R"({"type":"call","id":)" + std::to_string(next) + R"(,"target":"mux","fn":"version","args":[]})" "\n";
            if (!flood.send(batch)) break;
            sent = next - 1;
        }
    });
    // The other call goes once the flood runs: else it could get its
    // answer before the first batch, and the test would show nothing.
    ASSERT_TRUE(waitFor([&] { return sent > 0; }, 3s));
    IpcConnection connection;
    IpcConnection::Error error;
    ASSERT_TRUE(connection.connect(daemon.socket(), &error, 1000)) << error.message;
    // The call's own wait bounds it: a daemon that answered the flood first
    // would leave it without an answer.
    const auto list = connection.call(Target, Fn::ListSessions, {}, 1000, &error);
    done = true;
    writer.join();
    EXPECT_TRUE(list) << error.message;
    // Each call gets its answer, in order: the lines that wait in the
    // daemon's buffer after the socket went quiet are answered too.
    ASSERT_GT(sent.load(), 0);
    for (int i = 1; i <= sent; ++i) {
        const auto reply = Json::parse(flood.line());
        ASSERT_TRUE(reply) << "no answer to call " << i;
        ASSERT_EQ((*reply)["type"].toString(), "reply") << Json::write(*reply);
        ASSERT_EQ((*reply)["id"].toInteger(), i) << Json::write(*reply);
    }
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

    // The status report (DSR 5).
    const std::string status = daemon.dir() + "/status";
    ASSERT_TRUE(daemon.newSession(daemon.options(ask("\\x1b[5n", "n", status))));
    ASSERT_TRUE(waitFor([&] { return ::access(status.c_str(), F_OK) == 0; }));
    EXPECT_EQ(readFile(status), "\x1b[0n");

    // Which drag and drop types the terminal takes (OSC 72 t=q), as a window answers.
    const std::string dnd = daemon.dir() + "/dnd";
    ASSERT_TRUE(daemon.newSession(daemon.options(ask("\\x1b]72;t=q:i=3;\\x1b\\\\", "\\x1b\\\\", dnd))));
    ASSERT_TRUE(waitFor([&] { return ::access(dnd.c_str(), F_OK) == 0; }));
    EXPECT_EQ(readFile(dnd), "\x1b]72;t=q:i=3;\x1b\\");

    // The desktop notifications of a window (OSC 99 p=?), and none open (p=alive).
    const std::string notifications = daemon.dir() + "/notifications";
    o = daemon.options(ask("\\x1b]99;i=n:p=?;\\x1b\\\\\\x1b]99;i=n:p=alive;\\x1b\\\\", "p=alive;\\x1b\\\\", notifications));
    o.notificationCapabilities = "a=focus:p=title,body,alive";
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return ::access(notifications.c_str(), F_OK) == 0; }));
    EXPECT_EQ(readFile(notifications), "\x1b]99;i=n:p=?;a=focus:p=title,body,alive\x1b\\\x1b]99;i=n:p=alive;\x1b\\");
}

// A session that panes broke out to has the terminal values of their first
// session: its new panes answer as those of the first one.
TEST(MuxDaemon, ASessionThatPanesBrokeOutToHasTheTerminalValuesOfTheFirst)
{
    if (!pythonFound()) GTEST_SKIP() << "python3 is not installed";
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    SessionOptions o = daemon.options(Waiting, "s");
    o.terminal.defaultBackground = {0x12, 0x34, 0x56};
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    Launch launch;
    launch.argv = Waiting;
    launch.workingDirectory = daemon.dir();
    const auto added = daemon.call(Fn::NewPane, Json::Array{ProtocolVersion, "s", toJson(launch), toJson(PaneSize{})});
    ASSERT_TRUE(added) << daemon.lastError();
    const auto id = paneIdFrom((*added)["pane"]);
    ASSERT_TRUE(id) << Json::write(*added);
    Layout moved;
    moved.addTab(*id);
    const auto broke = daemon.call(Fn::BreakOut, Json::Array{ProtocolVersion, "s", toJson(moved)});
    ASSERT_TRUE(broke) << daemon.lastError();
    // The client's background color (OSC 11), which no window answers.
    const std::string color = daemon.dir() + "/color";
    launch.argv = ask("\\x1b]11;?\\x1b\\\\", "\\x1b\\\\", color);
    ASSERT_TRUE(daemon.call(Fn::NewPane, Json::Array{ProtocolVersion, (*broke)["name"].toString(), toJson(launch), toJson(PaneSize{})}))
        << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return ::access(color.c_str(), F_OK) == 0; }));
    EXPECT_NE(readFile(color).find("rgb:1212/3434/5656"), std::string::npos) << readFile(color);
}

TEST(MuxDaemon, RefusesClipboardDragsAndTransfersWhileDetached)
{
    if (!pythonFound()) GTEST_SKIP() << "python3 is not installed";
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    // OSC 52 read: an empty answer, as when the user denies it.
    const std::string clipboard = daemon.dir() + "/clipboard";
    ASSERT_TRUE(daemon.newSession(daemon.options(ask("\\x1b]52;c;?\\x1b\\\\", "\\x1b\\\\", clipboard))));
    ASSERT_TRUE(waitFor([&] { return ::access(clipboard.c_str(), F_OK) == 0; }));
    EXPECT_EQ(readFile(clipboard), "\x1b]52;c;\x1b\\");
    // OSC 5522 write: EPERM, and the text goes nowhere.
    const std::string write = daemon.dir() + "/write";
    ASSERT_TRUE(daemon.newSession(daemon.options(
        ask("\\x1b]5522;type=write:id=w\\x1b\\\\\\x1b]5522;type=wdata:mime=dGV4dC9wbGFpbg==;aGk=\\x1b\\\\\\x1b]5522;type=wdata\\x1b\\\\",
            "\\x1b\\\\", write))));
    ASSERT_TRUE(waitFor([&] { return ::access(write.c_str(), F_OK) == 0; }));
    EXPECT_EQ(readFile(write), "\x1b]5522;type=write:status=EPERM:id=w\x1b\\");
    // A file transfer: EPERM, in base64 without padding.
    const std::string transfer = daemon.dir() + "/transfer";
    ASSERT_TRUE(daemon.newSession(daemon.options(ask("\\x1b]5113;ac=send;id=t1\\x1b\\\\", "\\x1b\\\\", transfer))));
    ASSERT_TRUE(waitFor([&] { return ::access(transfer.c_str(), F_OK) == 0; }));
    EXPECT_EQ(readFile(transfer), "\x1b]5113;ac=status;id=t1;st=RVBFUk0\x1b\\");
    // A quiet transfer (q=2) gets no status: the DA1 after it answers first.
    const std::string quiet = daemon.dir() + "/quiet";
    ASSERT_TRUE(daemon.newSession(daemon.options(ask("\\x1b]5113;ac=send;id=t2;q=2\\x1b\\\\\\x1b[c", "c", quiet))));
    ASSERT_TRUE(waitFor([&] { return ::access(quiet.c_str(), F_OK) == 0; }));
    EXPECT_TRUE(readFile(quiet).starts_with("\x1b[?")) << readFile(quiet);
    // A drag offer: no button is held without a window, EPERM as in one.
    const std::string drag = daemon.dir() + "/drag";
    ASSERT_TRUE(daemon.newSession(daemon.options(ask("\\x1b]72;t=o:i=5:o=1;text/plain\\x1b\\\\", "\\x1b\\\\", drag))));
    ASSERT_TRUE(waitFor([&] { return ::access(drag.c_str(), F_OK) == 0; }));
    EXPECT_EQ(readFile(drag), "\x1b]72;i=5:m=0:t=E;EPERM\x1b\\");
}
