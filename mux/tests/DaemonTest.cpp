// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// Each test runs its own afterglow-mux (TestDaemon.h), so no test reaches the
// daemon of the user.

#include "TestDaemon.h"

using namespace MuxTest;

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
    // The number of arguments, and their types.
    for (const auto& [fn, args] : {std::pair{Fn::NewSession, Json::Array{ProtocolVersion}},
                                   std::pair{Fn::NewSession, Json::Array{"2", toJson(daemon.options(Waiting))}},
                                   std::pair{Fn::KillSession, Json::Array{}}, std::pair{Fn::KillSession, Json::Array{"a", "b"}},
                                   std::pair{Fn::KillSession, Json::Array{1}}}) {
        EXPECT_FALSE(daemon.call(fn, args, &error)) << fn;
        EXPECT_TRUE(error.code == IpcErrorCode::InvalidArg || error.code == Error::VersionMismatch) << fn << " " << error.code;
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
             {AfterglowProto::Run::Target, "a session pane takes no script commands: the mux daemon runs no scripts"}}) {
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
    for (const std::string& path : {daemon.socket(), daemon.stateSocket()}) {
        EXPECT_FALSE(exists(path)) << path;
        EXPECT_FALSE(exists(path + ".lock")) << path;
    }
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
    EXPECT_EQ(reported, std::string(1, AfterglowMux::ReportHeld) + "afterglow-mux: another afterglow-mux listens on " + daemon.socket() + "\n");
    EXPECT_NE(daemon.errors().find("another afterglow-mux listens on " + daemon.socket()), std::string::npos) << daemon.errors();
    // The first one still serves, on both sockets.
    EXPECT_EQ(names(daemon.sessions()), std::vector<std::string>{"keep"});
    daemon.callOn(daemon.stateSocket());
    EXPECT_EQ(names(daemon.sessions()), std::vector<std::string>{"keep"});
}

TEST(MuxDaemon, SigtermEndsTheSessionsAndTheirPrograms)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string pidFile = daemon.dir() + "/pid";
    ASSERT_TRUE(daemon.newSession(daemon.options({"/bin/sh", "-c", "echo $$ > '" + pidFile + "'; exec sleep 60"})));
    ASSERT_TRUE(waitFor([&] { return readFile(pidFile).find('\n') != std::string::npos; }));
    const pid_t program = pid_t(std::strtol(readFile(pidFile).c_str(), nullptr, 10));
    ASSERT_GT(program, 0);
    ::kill(daemon.pid(), SIGTERM);
    // The daemon ends with the signal, after its sessions.
    EXPECT_EQ(daemon.waitExit(), 128 + SIGTERM) << daemon.errors();
    // The hangup ended the program: it is gone, or a zombie that waits for its reaper.
    EXPECT_TRUE(waitFor([&] {
        const std::string stat = readFile("/proc/" + std::to_string(program) + "/stat");
        return stat.empty() || stat.find(") Z ") != std::string::npos;
    }));
}

// As under nohup: a hangup that the daemon got as ignored stays ignored, and
// its sessions stay. SIGTERM still ends it.
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
}

TEST(MuxDaemon, ThePaneGetsTheSessionEnvironmentOnly)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string out = daemon.dir() + "/env";
    SessionOptions o = daemon.options(writesEnvironment(out), "env");
    o.launch.environment.emplace_back("FROM_CLIENT", "yes");
    o.launch.environment.emplace_back("AFTERGLOW_PANE_ID", "999");
    o.launch.environment.emplace_back("AFTERGLOW_OVERLAY", "1");
    o.launch.environment.emplace_back("AFTERGLOW_SOCKET", "/elsewhere");
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return !readFile(out).empty(); }));
    const std::string env = "\n" + readFile(out);
    EXPECT_NE(env.find("\nFROM_CLIENT=yes\n"), std::string::npos);
    EXPECT_NE(env.find("\nAFTERGLOW_SESSION=env\n"), std::string::npos);
    // The daemon's runtime socket, which answers control calls with
    // CALL_FAILED (ControlAndCommandCallsFailWithAReason).
    EXPECT_NE(env.find("\nAFTERGLOW_SOCKET=" + daemon.socket() + "\n"), std::string::npos) << env;
    // The daemon's own pane ID, not the client's.
    EXPECT_EQ(env.find("\nAFTERGLOW_PANE_ID=999\n"), std::string::npos);
    EXPECT_NE(env.find("\nAFTERGLOW_PANE_ID="), std::string::npos);
    EXPECT_EQ(env.find("\nAFTERGLOW_OVERLAY="), std::string::npos);
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
    EXPECT_NE(readFile(out).find("PATH="), std::string::npos);
    EXPECT_EQ(readFile(out).find("MUX_TEST_DAEMON_ONLY"), std::string::npos);
}

// The programs get the umask of the client and the descriptor limit that the
// daemon had before it raised its own.
TEST(MuxDaemon, ThePaneGetsTheUmaskOfTheClientAndTheOriginalFileLimit)
{
    rlimit limit{};
    ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &limit), 0);
    if (limit.rlim_max <= 256) GTEST_SKIP() << "the hard limit leaves no room below it";
    TestDaemon daemon;
    // The daemon raises 256 to the hard limit for itself.
    daemon.startFileLimit(256);
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string out = daemon.dir() + "/limits";
    SessionOptions o =
        daemon.options({"/bin/sh", "-c", "{ umask; ulimit -n; } > '" + out + ".part' && mv '" + out + ".part' '" + out + "'; exec sleep 60"});
    o.umask = 027;
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return !readFile(out).empty(); }));
    std::istringstream lines(readFile(out));
    std::string mask;
    long files = 0;
    lines >> mask >> files;
    EXPECT_EQ(std::strtol(mask.c_str(), nullptr, 8), 027) << mask;
    EXPECT_EQ(files, 256);
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
        data += s.recv(4096)
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
    for (const std::string& directory : {daemon.runtime() + "/afterglow-mux", daemon.stateDirectory(), daemon.stateHome()}) {
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
        const std::string directory = daemon.runtime() + "/afterglow-mux";
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
// keep the daemon from the others.
TEST(MuxDaemon, APipeliningClientDoesNotStarveTheOthers)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    RawClient flood(daemon.socket());
    ASSERT_TRUE(flood.connected());
    std::string calls;
    for (int i = 1; i <= 2000; ++i) calls += R"({"type":"call","id":)" + std::to_string(i) + R"(,"target":"mux","fn":"version","args":[]})" "\n";
    std::jthread writer([&] { flood.send(calls); });
    IpcConnection connection;
    IpcConnection::Error error;
    ASSERT_TRUE(connection.connect(daemon.socket(), &error, 1000)) << error.message;
    // The call's own wait bounds it: a daemon that answered the flood first
    // would leave it without an answer.
    const auto list = connection.call(Target, Fn::ListSessions, {}, 1000, &error);
    EXPECT_TRUE(list) << error.message;
    writer.join();
    // Each call gets its answer, in order: the lines that wait in the
    // daemon's buffer after the socket went quiet are answered too.
    for (int i = 1; i <= 2000; ++i) {
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
