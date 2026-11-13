// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// Package commands from remote hosts: the sockets that `afterglow ssh`
// forwards (RemoteSockets, and the "remote" target of CommandSocket), the
// probe of `afterglow ssh` with a fake ssh on PATH, and an acceptance test
// with sshd run as the user on 127.0.0.1, whose "remote" side is this
// machine with a HOME and XDG_RUNTIME_DIR of its own.
//
// Every test listens on sockets in a runtime directory of its own. It never
// reaches the user's Afterglow, ssh configuration, keys, agent or control
// sockets: the ssh runs of the acceptance test take the test's own -F file,
// keys and known hosts, with no agent.

#include "Pane.h"
#include "SavedEnvironment.h"
#include "app/InstanceSocket.h"
#include "app/ScriptSettings.h"
#include "config/Settings.h"
#include "control/CommandSocket.h"
#include "control/Control.h"
#include "control/ControlSocket.h"
#include "control/RemoteSockets.h"
#include "scripting/ScriptControl.h"
#include "scripting/ScriptHost.h"

#include <AfterglowProto/IpcServer.h>
#include <AfterglowProto/Lock.h>
#include <AfterglowProto/Remote.h>
#include <AfterglowProto/Version.h>

#include <gtest/gtest.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <atomic>
#include <csignal>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <pwd.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace Afterglow;
namespace Remote = AfterglowProto::Remote;

namespace {

const QByteArray Script = R"(
local slowDone = 0
return {
    metadata = { commands = {
        here = { summary = "Local only" },
        there = { summary = "From remote hosts too", usage = "[WORD]", remote = true },
        echo = { summary = "Print stdin and the directory", stdin = true, remote = true },
        slow = { summary = "Print start, then wait 2 s", remote = true },
        settle = { summary = "Wait 2 s, then print how many slow runs ended" },
    } },
    run_command = function(command, ev)
        if command == "echo" then
            return afterglow.output(ev, tostring(ev.stdin) .. " " .. tostring(ev.cwd) .. "\n")
        elseif command == "slow" then
            afterglow.await(afterglow.output(ev, "start\n"))
            -- A cancel resumes the handler early, with an error: only a
            -- sleep that ran its time counts.
            local _, err = afterglow.sleep(2000)
            if err then return end
            slowDone += 1
            return afterglow.output(ev, "done\n")
        elseif command == "settle" then
            afterglow.sleep(2000)
            return afterglow.output(ev, tostring(slowDone))
        end
        return afterglow.output(ev, command .. " " .. ev.source .. " " .. tostring(ev.remote and ev.remote.host)
            .. " " .. tostring(ev.pane and ev.pane.id) .. " " .. (ev.args[1] or "-") .. "\n")
    end,
}
)";

/// A fake ssh for the probe tests. It writes each run's words to
/// FAKE_SSH_LOG, separated by 0x1f, one run on each line, and acts as the
/// FAKE_* variables say: the probe's report, the exit status of -O check
/// and -O forward, an -O forward that ends only once the file
/// FAKE_FORWARD_GATE is there, and a session that waits for the file
/// FAKE_RELEASE.
/// ssh -G reports what real ssh does without a ControlPath, or with the
/// user's ControlPath FAKE_CONTROL_PATH, for the destination it is given,
/// with RequestTTY force (FAKE_TTY), as -tt would: the helper runs without
/// a terminal here. The helper's second ssh -G reads the user's files
/// through -F /dev/stdin, and its own Host * block last: the fake takes
/// that block's values, with %C as "fakehash", unless FAKE_USER sets
/// ControlPath none or ControlMaster no ("path-none", "master-no"), or
/// FAKE_CONTROL_PATH is set. A session whose test failed before it
/// released it ends when its parent has gone, or after 3000 waits of 20 ms:
/// it never outlives the test for long.
const QByteArray FakeSsh = R"sh(#!/bin/sh
{ for a in "$@"; do printf '%s\037' "$a"; done; printf '\n'; } >> "$FAKE_SSH_LOG"
op=; prev=; last=
for a in "$@"; do
    [ "$prev" = "-O" ] && op=$a
    prev=$a; last=$a
done
for a in "$@"; do
    case "$a" in -G)
        printf 'host %s\nhostname %s.example\nuser me\nport 22\ncontrolpersist no\nrequesttty %s\n' "$last" "$last" "${FAKE_TTY:-force}"
        [ -n "${FAKE_CONTROL_PATH:-}" ] && printf 'controlpath %s\n' "$FAKE_CONTROL_PATH"
        case " $* " in
            *" /dev/stdin "*)
                own=$(sed -n 's/^ *ControlPath \(.*\)$/\1/p; s/^ *ControlMaster \(.*\)$/master \1/p')
                case "${FAKE_USER:-}${FAKE_CONTROL_PATH:-}" in
                    path-none|/*) ;;
                    *) printf 'controlpath %s\n' "$(printf '%s\n' "$own" | sed -n '1s/%C/fakehash/p')";;
                esac
                case "${FAKE_USER:-}" in
                    master-no) printf 'controlmaster false\n';;
                    *) printf 'controlmaster %s\n' "$(printf '%s\n' "$own" | sed -n 's/^master //p')";;
                esac;;
            *) printf 'controlmaster false\n';;
        esac
        exit 0;;
    esac
done
case "$op" in
    check) exit "${FAKE_CHECK:-0}";;
    forward)
        n=0
        while [ -n "${FAKE_FORWARD_GATE:-}" ] && [ ! -e "$FAKE_FORWARD_GATE" ]; do
            n=$((n + 1)); [ "$n" -gt 3000 ] && exit 8
            sleep 0.02
        done
        exit "${FAKE_FORWARD:-0}";;
    ?*) exit 0;;
esac
case "$last" in
    "sh -c "*)
        printf 'runtime=/run/user/1000\nhome=/home/me\n'
        case "${FAKE_AGLOW:-same}" in
            same) printf 'aglow=aglow 0.0.0 (protocol %s)\ndir=%s\n' "$FAKE_PROTOCOL" "${FAKE_DIR:-/run/user/1000/afterglow}";;
            other) printf 'aglow=aglow 9.9.9 (protocol 99)\ndir=%s\n' "${FAKE_DIR:-/run/user/1000/afterglow}";;
            another) printf 'aglow=aglow 9.9.8 (protocol 98)\ndir=%s\n' "${FAKE_DIR:-/run/user/1000/afterglow}";;
        esac
        exit "${FAKE_PROBE_STATUS:-0}";;
esac
: > "$FAKE_RELEASE.started"
parent=$PPID; n=0
while [ ! -e "$FAKE_RELEASE" ]; do
    kill -0 "$parent" 2>/dev/null || exit 9
    n=$((n + 1)); [ "$n" -gt 3000 ] && exit 8
    sleep 0.02
done
exit 7
)sh";

using Test::Saved;

/// The server runs on this thread: wait with its event loop running.
bool pumpUntil(const std::function<bool()>& done, int timeoutMs = 10000)
{
    QElapsedTimer clock;
    clock.start();
    while (!done()) {
        if (clock.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(2);
    }
    return true;
}

bool exists(const QString& path)
{
    struct stat info{};
    return ::lstat(QFile::encodeName(path).constData(), &info) == 0;
}

/// For QProcess::setChildProcessModifier: the child gets SIGKILL when the
/// test binary's main thread ends, so that no sshd or fake ssh outlives a
/// test that a timeout or a crash ended (where no destructor runs).
void endWithTheTest()
{
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
}

/// Connecting to the Unix socket at @p path: 0, or the errno of the failure.
int connectError(const QString& path)
{
    const QByteArray name = QFile::encodeName(path);
    sockaddr_un address{};
    if (std::size_t(name.size()) >= sizeof address.sun_path) return ENAMETOOLONG;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, name.constData(), std::size_t(name.size()));
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return errno;
    const int result = ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address);
    const int error = result == 0 ? 0 : errno;
    ::close(fd);
    return error;
}

class RemoteCommandsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        // Short: socket paths hold 108 bytes.
        ASSERT_TRUE(m_dir.isValid());
        qputenv("XDG_CONFIG_HOME", m_dir.filePath(QStringLiteral("config")).toUtf8());
        qputenv("XDG_DATA_HOME", m_dir.filePath(QStringLiteral("data")).toUtf8());
        qputenv("XDG_STATE_HOME", m_dir.filePath(QStringLiteral("state")).toUtf8());
        qputenv("HOME", m_dir.filePath(QStringLiteral("home")).toUtf8());
        m_runtime = m_dir.filePath(QStringLiteral("r"));
        ASSERT_TRUE(QDir().mkpath(m_runtime));
        QFile::setPermissions(m_runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        qputenv("XDG_RUNTIME_DIR", m_runtime.toUtf8());
        qunsetenv("AFTERGLOW_SOCKET");
        qunsetenv("AFTERGLOW_PANE_ID");
        ASSERT_TRUE(QDir().mkpath(m_dir.filePath(QStringLiteral("scripts"))));
        QFile script(m_dir.filePath(QStringLiteral("scripts/far.luau")));
        ASSERT_TRUE(script.open(QIODevice::WriteOnly));
        script.write(Script);
        script.close();
        // The fake ssh, for the probe tests and for the cancels of RemoteSockets.
        ASSERT_TRUE(QDir().mkpath(m_dir.filePath(QStringLiteral("bin"))));
        QFile ssh(fakeSsh());
        ASSERT_TRUE(ssh.open(QIODevice::WriteOnly));
        ssh.write(FakeSsh);
        ssh.close();
        ssh.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }

    void TearDown() override
    {
        // The sessions answer to the pane as they close: it goes after them.
        m_remote.reset();
        m_controlSocket.reset();
        m_commands.reset();
        m_scripts.reset();
        m_instance.reset();
        m_pane.reset();
    }

    QString fakeSsh() const { return m_dir.filePath(QStringLiteral("bin/ssh")); }
    QString log() const { return m_dir.filePath(QStringLiteral("ssh.log")); }
    QString socketPath() const { return m_runtime + QStringLiteral("/afterglow-test.sock"); }

    bool start(bool remoteCommands = true)
    {
        const QString path = m_dir.filePath(QStringLiteral("config.json"));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return false;
        file.write(QJsonDocument(QJsonObject{{QStringLiteral("Terminal"),
                                              QJsonObject{{QStringLiteral("sshRemoteCommands"), remoteCommands}}}})
                       .toJson());
        file.close();
        m_settings = std::make_unique<SettingsStore>(path);
        m_control = std::make_unique<Control>(*m_settings, [](const QString&, const QStringList&, bool) { return nullptr; });
        m_instance = std::make_unique<InstanceSocket>();
        if (m_instance->listen(socketPath()) != InstanceSocket::Listen::Listening) return false;
        m_scripts = std::make_unique<ScriptHost>(scriptCallbacks(*m_control));
        m_scripts->setStateDirectory(m_dir.filePath(QStringLiteral("scriptstate")));
        m_scripts->setSearchPaths({m_dir.filePath(QStringLiteral("scripts"))});
        m_commands = std::make_unique<CommandSocket>(*m_control, *m_scripts, *m_settings, m_instance->router());
        m_controlSocket = std::make_unique<ControlSocket>(*m_control, *m_settings, m_instance->router());
        m_remote = std::make_unique<RemoteSockets>(*m_control, *m_scripts, *m_settings,
                                                   [this](quint32 pane, const Remote::Answer& answer,
                                                          const std::optional<Remote::Answer>& overlay) {
                                                       if (m_pane && pane == m_pane->id()) m_pane->setRemoteSocket(answer, overlay);
                                                       else if (pane != FakePane) return false;
                                                       m_answers.insert(pane, answer);
                                                       m_overlayAnswers.insert(pane, overlay);
                                                       return true;
                                                   });
        m_remote->setSsh(fakeSsh());
        m_controlSocket->setRemoteSockets(m_remote.get());
        followScriptSettings(*m_scripts, *m_settings);
        qputenv("FAKE_SSH_LOG", log().toUtf8());
        return true;
    }

    /// The runs of the fake ssh, each as its words.
    QList<QStringList> runs() const
    {
        QFile file(log());
        QList<QStringList> out;
        if (!file.open(QIODevice::ReadOnly)) return out;
        for (const QByteArray& line : file.readAll().split('\n')) {
            if (line.isEmpty()) continue;
            QStringList words = QString::fromUtf8(line).split(QChar(0x1f));
            words.removeLast(); // after the last separator
            out << words;
        }
        return out;
    }

    /// The runs whose words contain @p word, and the next after it.
    QList<QStringList> runsWith(const QString& word, const QString& next = {}) const
    {
        QList<QStringList> out;
        for (const QStringList& run : runs()) {
            const qsizetype at = run.indexOf(word);
            if (at >= 0 && (next.isEmpty() || run.value(at + 1) == next)) out << run;
        }
        return out;
    }

    /// A process for a session to watch: it ends when the test kills it.
    std::unique_ptr<QProcess> sleeper()
    {
        auto process = std::make_unique<QProcess>();
        process->setChildProcessModifier(endWithTheTest);
        process->start(QStringLiteral("sleep"), {QStringLiteral("60")});
        EXPECT_TRUE(process->waitForStarted());
        return process;
    }

    RemoteSockets::Request request(qint64 pid, const QString& directory = QStringLiteral("/run/user/1000/afterglow")) const
    {
        return {FakePane, pid, QStringLiteral("box"), directory, AfterglowProto::ProtocolVersion,
                {QStringLiteral("-F"), QStringLiteral("/cfg")}, QStringLiteral("prod")};
    }

    static bool connectTo(QLocalSocket& client, const QString& path)
    {
        client.connectToServer(path);
        return pumpUntil([&] { return client.state() == QLocalSocket::ConnectedState; });
    }

    /// Sends a frame of @p type (call, schema or subscribe) to @p target.
    /// Returns its id.
    qint64 send(QLocalSocket& client, const QString& target, const QString& fn, const QJsonArray& args,
                const QString& type = QStringLiteral("call"))
    {
        const qint64 id = ++m_id;
        QJsonObject frame{{QStringLiteral("type"), type}, {QStringLiteral("id"), id}, {QStringLiteral("target"), target}};
        if (type == QLatin1String("call")) {
            frame.insert(QStringLiteral("fn"), fn);
            frame.insert(QStringLiteral("args"), args);
        } else if (type == QLatin1String("subscribe")) {
            frame.insert(QStringLiteral("signal"), fn);
        }
        client.write(QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n');
        return id;
    }

    /// The frames until the answer to @p id, which comes last. Empty when
    /// it did not come.
    static QList<QJsonObject> until(QLocalSocket& client, qint64 id, int timeoutMs = 10000)
    {
        QList<QJsonObject> frames;
        const bool answered = pumpUntil([&] {
            while (client.canReadLine()) {
                frames << QJsonDocument::fromJson(client.readLine()).object();
                const QJsonObject& frame = frames.last();
                if (frame.value(QStringLiteral("id")).toInteger() == id && frame.value(QStringLiteral("type")) != QLatin1String("event"))
                    return true;
            }
            return false;
        }, timeoutMs);
        EXPECT_TRUE(answered) << "no answer to " << id;
        return answered ? frames : QList<QJsonObject>{};
    }

    /// The text of the stdout events in @p frames.
    static QString output(const QList<QJsonObject>& frames)
    {
        QString out;
        for (const QJsonObject& f : frames) {
            if (f.value(QStringLiteral("type")) == QLatin1String("event") && f.value(QStringLiteral("signal")) == QLatin1String("stdout"))
                out += f.value(QStringLiteral("args")).toArray().at(1).toString();
        }
        return out;
    }

    /// Calls @p fn of @p target on the socket at @p path. The answer, or
    /// the event frames and then the answer of a run.
    QList<QJsonObject> callOn(const QString& path, const QString& target, const QString& fn, const QJsonArray& args)
    {
        QLocalSocket client;
        EXPECT_TRUE(connectTo(client, path));
        return until(client, send(client, target, fn, args));
    }

    /// Whether a fake ssh of this test runs. QProcess::startDetached returns
    /// once the program has been executed, so a cancel that close() started
    /// runs until it has written its line to the log.
    bool fakeSshRuns() const
    {
        const QByteArray program = QFile::encodeName(fakeSsh());
        for (const QString& entry : QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            bool numeric = false;
            entry.toUInt(&numeric);
            if (!numeric) continue;
            QFile cmdline(QStringLiteral("/proc/%1/cmdline").arg(entry));
            if (cmdline.open(QIODevice::ReadOnly) && cmdline.readAll().split('\0').contains(program)) return true;
        }
        return false;
    }

    /// Asserts that no `ssh -O cancel` ran. The cancel of a session runs
    /// detached as the session closes: once no fake ssh of the test runs,
    /// the log holds every cancel there was.
    void expectNoCancel()
    {
        ASSERT_TRUE(pumpUntil([&] { return !fakeSshRuns(); }));
        EXPECT_TRUE(runsWith(QStringLiteral("cancel")).isEmpty());
    }

    /// `afterglow ssh ARGS` with the fake ssh, in pane FakePane of this
    /// test's socket. It runs until the test releases the session.
    std::unique_ptr<QProcess> helper(const QStringList& args, const QHash<QString, QString>& fake = {})
    {
        auto process = std::make_unique<QProcess>();
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("PATH"), m_dir.filePath(QStringLiteral("bin")) + QLatin1Char(':') + env.value(QStringLiteral("PATH")));
        env.insert(QStringLiteral("AFTERGLOW_SOCKET"), socketPath());
        env.insert(QStringLiteral("AFTERGLOW_PANE_ID"), QString::number(FakePane));
        env.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
        env.insert(QStringLiteral("FAKE_SSH_LOG"), log());
        env.insert(QStringLiteral("FAKE_RELEASE"), release());
        env.insert(QStringLiteral("FAKE_PROTOCOL"), QString::number(AfterglowProto::ProtocolVersion));
        // In tmux, the helper cannot trust AFTERGLOW_PANE_ID: the test may run in one.
        env.remove(QStringLiteral("TMUX"));
        // Nor in an overlay, unless the test asks for one.
        env.remove(QStringLiteral("AFTERGLOW_OVERLAY"));
        for (auto it = fake.cbegin(); it != fake.cend(); ++it) env.insert(it.key(), it.value());
        process->setProcessEnvironment(env);
        QFile::remove(release());
        QFile::remove(release() + QStringLiteral(".started"));
        QFile::remove(log());
        // A test binary that dies (a timeout, a crash) takes the helper with
        // it, and the fake session then ends with its parent.
        process->setChildProcessModifier(endWithTheTest);
        process->start(QStringLiteral(AFTERGLOW_BINARY), QStringList{QStringLiteral("ssh")} + args);
        EXPECT_TRUE(process->waitForStarted());
        EXPECT_TRUE(pumpUntil([&] { return exists(release() + QStringLiteral(".started")); }))
            << qPrintable(QString::fromUtf8(process->readAllStandardError()));
        return process;
    }

    /// Ends the session of @p process, and returns its exit status.
    int finish(QProcess& process)
    {
        QFile released(release());
        EXPECT_TRUE(released.open(QIODevice::WriteOnly));
        EXPECT_TRUE(pumpUntil([&] { return process.state() == QProcess::NotRunning; }));
        m_stderr = QString::fromUtf8(process.readAllStandardError());
        // The session ends when the process that opened it does.
        EXPECT_TRUE(pumpUntil([&] { return m_remote->tokens().isEmpty(); }));
        return process.exitCode();
    }

    QString release() const { return m_dir.filePath(QStringLiteral("release")); }

    static constexpr quint32 FakePane = 7;
    Saved m_configHome{"XDG_CONFIG_HOME"};
    Saved m_dataHome{"XDG_DATA_HOME"};
    Saved m_stateHome{"XDG_STATE_HOME"};
    Saved m_runtimeDir{"XDG_RUNTIME_DIR"};
    Saved m_home{"HOME"};
    Saved m_socketVar{"AFTERGLOW_SOCKET"};
    Saved m_paneVar{"AFTERGLOW_PANE_ID"};
    Saved m_sshLog{"FAKE_SSH_LOG"};
    QTemporaryDir m_dir{QStringLiteral("/tmp/agrc-XXXXXX")};
    QString m_runtime;
    qint64 m_id = 0;
    std::unique_ptr<SettingsStore> m_settings;
    std::unique_ptr<Control> m_control;
    std::unique_ptr<InstanceSocket> m_instance;
    std::unique_ptr<ScriptHost> m_scripts;
    std::unique_ptr<CommandSocket> m_commands;
    std::unique_ptr<ControlSocket> m_controlSocket;
    std::unique_ptr<RemoteSockets> m_remote;
    QHash<quint32, Remote::Answer> m_answers;
    QHash<quint32, std::optional<Remote::Answer>> m_overlayAnswers;
    /// A real pane, for the acceptance test. The fixture owns it: a failed
    /// assertion leaves it to TearDown, after the sessions that answer to it.
    std::unique_ptr<Pane> m_pane;
    QString m_stderr;
};

} // namespace

TEST_F(RemoteCommandsTest, TheRemoteTargetRunsOnlyRemoteCommandsOnTheSessionsPane)
{
    ASSERT_TRUE(start());
    m_scripts->setEnabled(true);
    const auto watched = sleeper();
    RemoteSockets::Opened opened;
    ASSERT_EQ(m_remote->open(request(watched->processId()), opened), QString());
    EXPECT_TRUE(opened.error.isEmpty());
    EXPECT_EQ(opened.path, QStringLiteral("/run/user/1000/afterglow/%1.sock").arg(opened.token));
    EXPECT_EQ(opened.token.size(), 16);
    EXPECT_EQ(opened.socket, m_runtime + QStringLiteral("/afterglow-remote-%1.sock").arg(opened.token));
    // Only the user can connect.
    EXPECT_EQ(QFileInfo(opened.socket).permissions() & (QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ReadGroup
                                                         | QFileDevice::WriteGroup),
              QFileDevice::Permissions());
    // The path is the answer once its forward came, not before.
    EXPECT_EQ(m_answers.value(FakePane), Remote::Answer{});
    EXPECT_EQ(m_remote->forwarded(opened.token), QString());
    EXPECT_EQ(m_answers.value(FakePane), (Remote::Answer{opened.path.toStdString(), Remote::Error::Off}));

    // No control call reaches the terminal through it.
    QJsonObject answer = callOn(opened.socket, QStringLiteral("control"), QStringLiteral("listPanes"), {}).value(0);
    EXPECT_EQ(answer.value(QStringLiteral("code")).toString(), QStringLiteral("NO_SUCH_TARGET"));
    answer = callOn(opened.socket, QStringLiteral("commands"), QStringLiteral("list"), {}).value(0);
    EXPECT_EQ(answer.value(QStringLiteral("code")).toString(), QStringLiteral("NO_SUCH_TARGET"));
    // The commands that declare remote, and no others.
    answer = callOn(opened.socket, QStringLiteral("remote"), QStringLiteral("list"), {}).value(0);
    QStringList listed;
    for (const QJsonValue& entry : answer.value(QStringLiteral("result")).toArray()) {
        listed << entry.toObject().value(QStringLiteral("command")).toString();
        EXPECT_EQ(entry.toObject().value(QStringLiteral("remote")), QJsonValue(true)) << qPrintable(listed.last());
    }
    listed.sort();
    EXPECT_EQ(listed, (QStringList{QStringLiteral("echo"), QStringLiteral("slow"), QStringLiteral("there")}));
    // The terminal's own socket lists them all, and says which are remote.
    QHash<QString, QJsonValue> remote;
    for (const QJsonValue& entry :
         callOn(socketPath(), QStringLiteral("commands"), QStringLiteral("list"), {}).value(0).value(QStringLiteral("result")).toArray())
        remote.insert(entry.toObject().value(QStringLiteral("command")).toString(), entry.toObject().value(QStringLiteral("remote")));
    EXPECT_EQ(remote.size(), 5);
    EXPECT_EQ(remote.value(QStringLiteral("here")), QJsonValue(false));
    EXPECT_EQ(remote.value(QStringLiteral("there")), QJsonValue(true));
    // A command's own entry is not there: list and run only.
    answer = callOn(opened.socket, QStringLiteral("remote"), QStringLiteral("far.there"), {QJsonArray{}}).value(0);
    EXPECT_EQ(answer.value(QStringLiteral("code")).toString(), QStringLiteral("NO_SUCH_FN"));

    const auto run = [&](const QString& command, double pane) {
        return callOn(opened.socket, QStringLiteral("remote"), QStringLiteral("run"),
                      {QStringLiteral("r1"), QStringLiteral("far"), command, QJsonArray{QStringLiteral("word")},
                       QStringLiteral("/home/me/src"), pane});
    };
    QList<QJsonObject> frames = run(QStringLiteral("there"), 0);
    ASSERT_EQ(frames.size(), 2);
    EXPECT_EQ(frames[0].value(QStringLiteral("args")).toArray().at(1).toString(), QStringLiteral("there remote box 7 word\n"));
    EXPECT_EQ(frames[1].value(QStringLiteral("result")).toObject().value(QStringLiteral("exit")).toInt(), 0);
    // A command without remote does not exist for a remote host.
    frames = run(QStringLiteral("here"), 0);
    EXPECT_EQ(frames.value(0).value(QStringLiteral("code")).toString(), QStringLiteral("NO_SUCH_COMMAND"));
    // The run acts on the session's pane, which the client may name, and on no other.
    frames = run(QStringLiteral("there"), FakePane);
    EXPECT_EQ(frames.value(0).value(QStringLiteral("args")).toArray().at(1).toString(), QStringLiteral("there remote box 7 word\n"));
    frames = run(QStringLiteral("there"), 9);
    EXPECT_TRUE(frames.value(0).value(QStringLiteral("message")).toString().contains(QLatin1String("pane of its ssh session")));
}

// The socket that a window gives the clients that a mux daemon passes it
// (MuxCommands): a client in a pane of a session of another host, on the
// commands target, and on the remote host of an ssh session in a session
// pane, on the remote target. Each gets only the commands marked remote, on
// the pane, with the host. One socket serves each pane, host and target
// until the pane closes, with no forward to cancel and no answer of the
// pane to change.
TEST_F(RemoteCommandsTest, AnEndpointServesTheRemoteCommandsOfItsPaneWithNoForward)
{
    ASSERT_TRUE(start());
    m_scripts->setEnabled(true);
    QString error;
    const QString commands = m_remote->endpoint(FakePane, QStringLiteral("box"), true, &error);
    ASSERT_FALSE(commands.isEmpty()) << error.toStdString();
    EXPECT_TRUE(commands.startsWith(m_runtime + QStringLiteral("/afterglow-remote-"))) << commands.toStdString();
    EXPECT_EQ(m_remote->endpoint(FakePane, QStringLiteral("box"), true, &error), commands);
    const QString remote = m_remote->endpoint(FakePane, QStringLiteral("box"), false, &error);
    ASSERT_FALSE(remote.isEmpty()) << error.toStdString();
    EXPECT_NE(remote, commands);
    EXPECT_NE(m_remote->endpoint(FakePane, QStringLiteral("other"), true, &error), commands);

    // Each on its own target, with the commands that declare remote.
    for (const auto& [socket, target, other] : {std::tuple{commands, QStringLiteral("commands"), QStringLiteral("remote")},
                                                std::tuple{remote, QStringLiteral("remote"), QStringLiteral("commands")}}) {
        SCOPED_TRACE(target.toStdString());
        EXPECT_EQ(callOn(socket, other, QStringLiteral("list"), {}).value(0).value(QStringLiteral("code")).toString(),
                  QStringLiteral("NO_SUCH_TARGET"));
        QStringList listed;
        for (const QJsonValue& entry : callOn(socket, target, QStringLiteral("list"), {}).value(0).value(QStringLiteral("result")).toArray())
            listed << entry.toObject().value(QStringLiteral("command")).toString();
        listed.sort();
        EXPECT_EQ(listed, (QStringList{QStringLiteral("echo"), QStringLiteral("slow"), QStringLiteral("there")}));
        const auto run = [&](const QString& command, double pane) {
            return callOn(socket, target, QStringLiteral("run"),
                          {QStringLiteral("r1"), QStringLiteral("far"), command, QJsonArray{QStringLiteral("word")},
                           QStringLiteral("/home/me/src"), pane});
        };
        QList<QJsonObject> frames = run(QStringLiteral("there"), 0);
        ASSERT_EQ(frames.size(), 2);
        EXPECT_EQ(frames[0].value(QStringLiteral("args")).toArray().at(1).toString(), QStringLiteral("there remote box 7 word\n"));
        frames = run(QStringLiteral("here"), 0);
        EXPECT_EQ(frames.value(0).value(QStringLiteral("code")).toString(), QStringLiteral("NO_SUCH_COMMAND"));
        frames = run(QStringLiteral("there"), 9);
        // The run acts on the pane, and on no other.
        EXPECT_TRUE(frames.value(0).value(QStringLiteral("message")).toString().endsWith(QLatin1String("pane, 7")) ||
                    frames.value(0).value(QStringLiteral("message")).toString().endsWith(QLatin1String("session, 7")))
            << frames.value(0).value(QStringLiteral("message")).toString().toStdString();
    }
    // A session of another host hears that its run acts on its own pane.
    const QList<QJsonObject> elsewhere = callOn(commands, QStringLiteral("commands"), QStringLiteral("run"),
                                                {QStringLiteral("r2"), QStringLiteral("far"), QStringLiteral("there"), QJsonArray{}, QString(), 9});
    EXPECT_TRUE(elsewhere.value(0).value(QStringLiteral("message")).toString().contains(QLatin1String("session of another host")));

    // The pane's answer to the query stays as it was: no ssh session of its own.
    EXPECT_FALSE(m_answers.contains(FakePane));
    m_remote->paneClosed(FakePane);
    EXPECT_TRUE(m_remote->tokens().isEmpty());
    EXPECT_FALSE(exists(commands));
    EXPECT_FALSE(exists(remote));
    expectNoCancel();
    // A host that is no name.
    EXPECT_TRUE(m_remote->endpoint(FakePane, QString(), true, &error).isEmpty());
    EXPECT_FALSE(error.isEmpty());
    // The setting off: no socket, and the reason.
    ASSERT_TRUE(m_settings->write(QStringLiteral("Terminal"), QStringLiteral("sshRemoteCommands"), false));
    EXPECT_TRUE(m_remote->endpoint(FakePane, QStringLiteral("box"), true, &error).isEmpty());
    EXPECT_TRUE(error.contains(QLatin1String("Terminal.sshRemoteCommands"))) << error.toStdString();
}

TEST_F(RemoteCommandsTest, TheRemoteTargetHasTheCallsOfARunAndNoMore)
{
    ASSERT_TRUE(start());
    m_scripts->setEnabled(true);
    const auto watched = sleeper();
    RemoteSockets::Opened opened;
    ASSERT_EQ(m_remote->open(request(watched->processId()), opened), QString());
    const QString target = QStringLiteral("remote");
    QLocalSocket client;
    ASSERT_TRUE(connectTo(client, opened.socket));

    // The schema has the calls and the events of a run, and no command's own entry.
    const QJsonObject schema = until(client, send(client, target, {}, {}, QStringLiteral("schema"))).value(0).value(QStringLiteral("result")).toObject();
    const auto names = [&schema](const char* kind) {
        QStringList out;
        for (const QJsonValue& entry : schema.value(QLatin1String(kind)).toArray()) out << entry.toObject().value(QStringLiteral("name")).toString();
        return out;
    };
    const QStringList functions = names("functions");
    for (const char* name : {"list", "input", "run", "ack", "cancel"}) EXPECT_TRUE(functions.contains(QLatin1String(name))) << name;
    for (const QString& name : functions) EXPECT_FALSE(name.startsWith(QLatin1String("far."))) << qPrintable(name);
    EXPECT_EQ(names("signals"), (QStringList{QStringLiteral("stdout"), QStringLiteral("stderr")}));
    // No control events through it.
    EXPECT_EQ(until(client, send(client, QStringLiteral("control"), QStringLiteral("titleChanged"), {}, QStringLiteral("subscribe")))
                  .value(0).value(QStringLiteral("code")).toString(),
              QStringLiteral("NO_SUCH_TARGET"));
    // A subscriber of the run events gets none of another connection's run.
    QLocalSocket listener;
    ASSERT_TRUE(connectTo(listener, opened.socket));
    EXPECT_EQ(until(listener, send(listener, target, QStringLiteral("stdout"), {}, QStringLiteral("subscribe"))).value(0).value(QStringLiteral("type")),
              QStringLiteral("reply"));

    // Input in parts, then the run that takes it. The directory is one of
    // the remote host, which the handler gets as text.
    for (const bool eof : {false, true}) {
        EXPECT_EQ(until(client, send(client, target, QStringLiteral("input"), {QStringLiteral("r1"), eof ? QStringLiteral("b") : QStringLiteral("a"), eof}))
                      .value(0).value(QStringLiteral("result")),
                  QJsonValue(true));
    }
    QList<QJsonObject> frames = until(client, send(client, target, QStringLiteral("run"),
                                                   {QStringLiteral("r1"), QStringLiteral("far"), QStringLiteral("echo"), QJsonArray{},
                                                    QStringLiteral("/nowhere/here/src"), 0}));
    EXPECT_EQ(output(frames), QStringLiteral("ab /nowhere/here/src\n"));
    EXPECT_EQ(frames.value(frames.size() - 1).value(QStringLiteral("result")).toObject().value(QStringLiteral("exit")).toInt(-1), 0);
    // The run's events came to its caller alone: the subscriber's next answer is all it reads.
    EXPECT_EQ(until(listener, send(listener, target, QStringLiteral("list"), {})).size(), 1);
    // A relative directory would name one of this machine.
    frames = until(client, send(client, target, QStringLiteral("run"),
                                {QStringLiteral("r2"), QStringLiteral("far"), QStringLiteral("echo"), QJsonArray{}, QStringLiteral("src"), 0}));
    EXPECT_TRUE(frames.value(0).value(QStringLiteral("message")).toString().contains(QLatin1String("cwd must be empty or an absolute path")));

    // A run that waits takes an ack, and a cancel ends it with 130.
    const qint64 run = send(client, target, QStringLiteral("run"),
                            {QStringLiteral("r3"), QStringLiteral("far"), QStringLiteral("slow"), QJsonArray{}, QString(), 0});
    QByteArray started;
    ASSERT_TRUE(pumpUntil([&] {
        if (!client.canReadLine()) return false;
        started = client.readLine();
        return true;
    }));
    const QJsonObject event = QJsonDocument::fromJson(started).object();
    ASSERT_EQ(event.value(QStringLiteral("type")).toString(), QStringLiteral("event"));
    EXPECT_EQ(event.value(QStringLiteral("args")).toArray(), (QJsonArray{QStringLiteral("r3"), QStringLiteral("start\n")}));
    EXPECT_EQ(until(client, send(client, target, QStringLiteral("ack"), {QStringLiteral("r3"), double(started.size())})).value(0).value(QStringLiteral("type")),
              QStringLiteral("reply"));
    const qint64 cancel = send(client, target, QStringLiteral("cancel"), {QStringLiteral("r3")});
    QHash<qint64, QJsonObject> answers;
    ASSERT_TRUE(pumpUntil([&] {
        while (client.canReadLine()) {
            const QJsonObject frame = QJsonDocument::fromJson(client.readLine()).object();
            answers.insert(frame.value(QStringLiteral("id")).toInteger(), frame);
        }
        return answers.contains(run) && answers.contains(cancel);
    }));
    EXPECT_EQ(answers.value(cancel).value(QStringLiteral("type")), QStringLiteral("reply"));
    EXPECT_EQ(answers.value(run).value(QStringLiteral("result")).toObject().value(QStringLiteral("exit")).toInt(-1), 130);

    // What open() takes from the client: at most 256 ssh options, and a
    // destination that ssh cannot take for an option.
    RemoteSockets::Request options = request(watched->processId());
    options.ssh = QStringList(256, QStringLiteral("-v"));
    EXPECT_EQ(m_remote->open(options, opened), QString());
    options.ssh << QStringLiteral("-v");
    EXPECT_EQ(m_remote->open(options, opened), QStringLiteral("ssh takes at most 256 options here"));
    for (const QString& destination : {QString(), QStringLiteral("-oProxyCommand=touch /tmp/x")}) {
        RemoteSockets::Request bad = request(watched->processId());
        bad.destination = destination;
        EXPECT_EQ(m_remote->open(bad, opened), QStringLiteral("the ssh destination must not be empty or start with '-'")) << qPrintable(destination);
    }
}

TEST_F(RemoteCommandsTest, TheSocketClosesWithItsPaneAndWithTheWatchedProcess)
{
    ASSERT_TRUE(start());
    const auto first = sleeper();
    RemoteSockets::Opened byPane;
    ASSERT_EQ(m_remote->open(request(first->processId()), byPane), QString());
    ASSERT_TRUE(exists(byPane.socket));
    m_remote->paneClosed(FakePane);
    EXPECT_TRUE(m_remote->tokens().isEmpty());
    EXPECT_FALSE(exists(byPane.socket));
    EXPECT_EQ(m_answers.value(FakePane), Remote::Answer{});
    // The forward goes with the options that reach the shared connection.
    const QStringList cancel{QStringLiteral("-F"), QStringLiteral("/cfg"), QStringLiteral("-O"), QStringLiteral("cancel"),
                             QStringLiteral("-R"), byPane.path + QLatin1Char(':') + byPane.socket, QStringLiteral("--"),
                             QStringLiteral("prod")};
    EXPECT_TRUE(pumpUntil([&] { return runs().contains(cancel); }));

    const auto second = sleeper();
    RemoteSockets::Opened byProcess;
    ASSERT_EQ(m_remote->open(request(second->processId()), byProcess), QString());
    ASSERT_TRUE(exists(byProcess.socket));
    const qint64 ended = second->processId();
    second->kill();
    second->waitForFinished();
    EXPECT_TRUE(pumpUntil([&] { return m_remote->tokens().isEmpty(); }));
    EXPECT_FALSE(exists(byProcess.socket));
    EXPECT_TRUE(pumpUntil([&] { return runsWith(byProcess.path + QLatin1Char(':') + byProcess.socket).size() == 1; }));

    // A process that has ended opens nothing.
    RemoteSockets::Opened none;
    EXPECT_TRUE(m_remote->open(request(ended), none).startsWith(QLatin1String("no process")));
    // Nor a pane that does not exist.
    RemoteSockets::Request elsewhere = request(first->processId());
    elsewhere.pane = 99;
    EXPECT_EQ(m_remote->open(elsewhere, none), QStringLiteral("no pane 99"));
    EXPECT_TRUE(m_remote->tokens().isEmpty());
}

TEST_F(RemoteCommandsTest, AFailedForwardClosesTheSocketAndGivesItsReason)
{
    ASSERT_TRUE(start());
    const auto watched = sleeper();
    RemoteSockets::Opened opened;
    ASSERT_EQ(m_remote->open(request(watched->processId()), opened), QString());
    EXPECT_FALSE(m_remote->failed(opened.token, QStringLiteral("version")).isEmpty());
    EXPECT_FALSE(m_remote->failed(QStringLiteral("0000000000000000"), QStringLiteral("forward")).isEmpty());
    EXPECT_EQ(m_remote->failed(opened.token, QStringLiteral("forward")), QString());
    EXPECT_FALSE(exists(opened.socket));
    EXPECT_EQ(m_answers.value(FakePane), (Remote::Answer{{}, Remote::Error::Forward}));
    // The session keeps its reason until the process ends, and cancels nothing.
    watched->kill();
    watched->waitForFinished();
    EXPECT_TRUE(pumpUntil([&] { return m_remote->tokens().isEmpty(); }));
    EXPECT_EQ(m_answers.value(FakePane), Remote::Answer{});
    expectNoCancel();
}

TEST_F(RemoteCommandsTest, AnSshSessionInTheOverlayAnswersThereAndLeavesThePanesAnswer)
{
    ASSERT_TRUE(start());
    const auto inPane = sleeper();
    const auto inOverlay = sleeper();
    RemoteSockets::Opened pane;
    ASSERT_EQ(m_remote->open(request(inPane->processId()), pane), QString());
    ASSERT_EQ(m_remote->forwarded(pane.token), QString());
    const Remote::Answer paneAnswer{pane.path.toStdString(), Remote::Error::Off};
    EXPECT_EQ(m_overlayAnswers.value(FakePane), std::nullopt) << "the overlay answers as the pane";
    // The newer session runs in the overlay: the pane keeps its own answer.
    RemoteSockets::Request overlay = request(inOverlay->processId());
    overlay.overlay = true;
    RemoteSockets::Opened opened;
    ASSERT_EQ(m_remote->open(overlay, opened), QString());
    ASSERT_EQ(m_remote->forwarded(opened.token), QString());
    EXPECT_EQ(m_answers.value(FakePane), paneAnswer);
    EXPECT_EQ(m_overlayAnswers.value(FakePane), std::optional(Remote::Answer{opened.path.toStdString(), Remote::Error::Off}));
    // Once it ends, the overlay answers as the pane again.
    inOverlay->kill();
    inOverlay->waitForFinished();
    EXPECT_TRUE(pumpUntil([&] { return m_remote->tokens().size() == 1; }));
    EXPECT_EQ(m_answers.value(FakePane), paneAnswer);
    EXPECT_EQ(m_overlayAnswers.value(FakePane), std::nullopt);
}

TEST_F(RemoteCommandsTest, AfterglowSshInAnOverlayOpensTheOverlaysSession)
{
    ASSERT_TRUE(start());
    auto process = helper({QStringLiteral("prod")}, {{QStringLiteral("AFTERGLOW_OVERLAY"), QStringLiteral("1")}});
    ASSERT_EQ(m_remote->tokens().size(), 1);
    const QString path = QStringLiteral("/run/user/1000/afterglow/%1.sock").arg(m_remote->tokens().first());
    EXPECT_EQ(m_overlayAnswers.value(FakePane), std::optional(Remote::Answer{path.toStdString(), Remote::Error::Off}));
    EXPECT_EQ(m_answers.value(FakePane), Remote::Answer{}) << "the pane has no session of its own";
    EXPECT_EQ(finish(*process), 7);
}

TEST_F(RemoteCommandsTest, AnotherProtocolOrALongPathGetsNoSocket)
{
    ASSERT_TRUE(start());
    const auto watched = sleeper();
    RemoteSockets::Request other = request(watched->processId());
    other.protocol = AfterglowProto::ProtocolVersion + 1;
    RemoteSockets::Opened opened;
    ASSERT_EQ(m_remote->open(other, opened), QString());
    EXPECT_EQ(opened.error, QStringLiteral("version"));
    EXPECT_TRUE(opened.socket.isEmpty());
    EXPECT_EQ(m_answers.value(FakePane), (Remote::Answer{{}, Remote::Error::Version}));

    // Remote::MaxPathBytes and no more: the BSDs and macOS take 104 bytes
    // with the NUL.
    const qsizetype limit = qsizetype(Remote::MaxPathBytes);
    const QString fits = QLatin1Char('/') + QString(limit - 1 - 1 - 16 - 5, QLatin1Char('d'));
    ASSERT_EQ(m_remote->open(request(watched->processId(), fits), opened), QString());
    EXPECT_TRUE(opened.error.isEmpty());
    EXPECT_EQ(opened.path.size(), limit);
    EXPECT_TRUE(exists(opened.socket));
    const QStringList before = QDir(m_runtime).entryList(QDir::System);
    ASSERT_EQ(m_remote->open(request(watched->processId(), fits + QLatin1Char('d')), opened), QString());
    EXPECT_EQ(opened.error, QStringLiteral("length"));
    // No socket for it, here or for the client to forward.
    EXPECT_TRUE(opened.socket.isEmpty());
    EXPECT_TRUE(m_remote->socket(opened.token).isEmpty());
    EXPECT_EQ(QDir(m_runtime).entryList(QDir::System), before);
    // The newest session of the pane answers.
    EXPECT_EQ(m_answers.value(FakePane), (Remote::Answer{{}, Remote::Error::Length}));
    // A directory that would break the reply or ssh's -R is refused.
    for (const QString& bad : {QStringLiteral("relative"), QStringLiteral("/a:b"), QStringLiteral("/a\nb")})
        EXPECT_FALSE(m_remote->open(request(watched->processId(), bad), opened).isEmpty()) << qPrintable(bad);
}

TEST_F(RemoteCommandsTest, EachSettingOfTheGateClosesTheSessionsAndRefusesNewOnes)
{
    // Off from the start: nothing opens until the setting turns on.
    ASSERT_TRUE(start(false));
    m_scripts->setEnabled(true);
    const auto watched = sleeper();
    RemoteSockets::Opened opened;
    EXPECT_FALSE(m_remote->enabled());
    EXPECT_FALSE(m_remote->open(request(watched->processId()), opened).isEmpty());
    EXPECT_TRUE(m_remote->tokens().isEmpty());
    ASSERT_TRUE(m_settings->write(QStringLiteral("Terminal"), QStringLiteral("sshRemoteCommands"), true));
    ASSERT_TRUE(m_remote->enabled());
    const auto settle = [&] {
        return output(callOn(socketPath(), QStringLiteral("commands"), QStringLiteral("run"),
                             {QStringLiteral("s1"), QStringLiteral("far"), QStringLiteral("settle"), QJsonArray{}, QString(), 0}));
    };
    // The count that shows an ended run: a slow run that runs its time counts.
    {
        ASSERT_EQ(m_remote->open(request(watched->processId()), opened), QString());
        const QList<QJsonObject> frames = callOn(opened.socket, QStringLiteral("remote"), QStringLiteral("run"),
                                                 {QStringLiteral("r0"), QStringLiteral("far"), QStringLiteral("slow"), QJsonArray{}, QString(), 0});
        ASSERT_EQ(output(frames), QStringLiteral("start\ndone\n"));
        ASSERT_EQ(settle(), QStringLiteral("1"));
        m_remote->paneClosed(FakePane);
    }

    for (const auto& [group, key] : {std::pair{"Terminal", "sshRemoteCommands"}, std::pair{"Control", "enabled"}, std::pair{"Scripts", "enabled"}}) {
        SCOPED_TRACE(QStringLiteral("%1.%2").arg(QLatin1String(group), QLatin1String(key)).toStdString());
        ASSERT_EQ(m_remote->open(request(watched->processId()), opened), QString());
        ASSERT_TRUE(exists(opened.socket));
        // The forward came: the pane answers with the path, which the setting must take back.
        ASSERT_EQ(m_remote->forwarded(opened.token), QString());
        ASSERT_EQ(m_answers.value(FakePane), (Remote::Answer{opened.path.toStdString(), Remote::Error::Off}));
        // A client with a run in flight.
        QLocalSocket client;
        ASSERT_TRUE(connectTo(client, opened.socket));
        const qint64 run = send(client, QStringLiteral("remote"), QStringLiteral("run"),
                                {QStringLiteral("r1"), QStringLiteral("far"), QStringLiteral("slow"), QJsonArray{}, QString(), 0});
        QList<QJsonObject> frames;
        ASSERT_TRUE(pumpUntil([&] {
            while (client.canReadLine()) frames << QJsonDocument::fromJson(client.readLine()).object();
            return output(frames) == QLatin1String("start\n");
        }));

        ASSERT_TRUE(m_settings->write(QString::fromLatin1(group), QString::fromLatin1(key), false));
        EXPECT_FALSE(m_remote->enabled());
        EXPECT_TRUE(m_remote->tokens().isEmpty());
        EXPECT_FALSE(exists(opened.socket));
        // The pane answers that there is no socket.
        EXPECT_EQ(m_answers.value(FakePane), Remote::Answer{});
        // The client gets the reason as the reply to its run, and then loses its connection.
        EXPECT_TRUE(pumpUntil([&] { return client.state() == QLocalSocket::UnconnectedState; }));
        while (client.canReadLine()) frames << QJsonDocument::fromJson(client.readLine()).object();
        ASSERT_FALSE(frames.isEmpty());
        EXPECT_EQ(frames.last().value(QStringLiteral("id")).toInteger(), run);
        EXPECT_EQ(frames.last().value(QStringLiteral("type")), QStringLiteral("error"));
        EXPECT_EQ(frames.last().value(QStringLiteral("message")),
                  QStringLiteral("remote commands are off (%1.%2) in the running afterglow").arg(QLatin1String(group), QLatin1String(key)));
        // The forward goes too.
        EXPECT_TRUE(pumpUntil([&] { return runsWith(opened.path + QLatin1Char(':') + opened.socket).size() == 1; }));
        // Nothing opens while it is off.
        RemoteSockets::Opened refused;
        EXPECT_FALSE(m_remote->open(request(watched->processId()), refused).isEmpty());
        EXPECT_TRUE(m_remote->tokens().isEmpty());

        ASSERT_TRUE(m_settings->write(QString::fromLatin1(group), QString::fromLatin1(key), true));
        EXPECT_TRUE(m_remote->enabled());
        // The scripts ended the run: a wait as long as its own, started
        // after it, ends after it would have, and sees it never finished.
        // Scripts.enabled reloads the script, whose count starts again.
        EXPECT_EQ(settle(), QLatin1String(group) == QLatin1String("Scripts") ? QStringLiteral("0") : QStringLiteral("1"));
    }

    // Terminal.sshIntegration only chooses what an integrated shell's ssh
    // runs: a session that `afterglow ssh` opened stays, and so do new ones.
    ASSERT_EQ(m_remote->open(request(watched->processId()), opened), QString());
    ASSERT_TRUE(m_settings->write(QStringLiteral("Terminal"), QStringLiteral("sshIntegration"), false));
    EXPECT_TRUE(m_remote->enabled());
    EXPECT_EQ(m_remote->tokens(), QStringList{opened.token});
    EXPECT_TRUE(exists(opened.socket));
    const QJsonObject listed = callOn(opened.socket, QStringLiteral("remote"), QStringLiteral("list"), {}).value(0);
    EXPECT_EQ(listed.value(QStringLiteral("type")).toString(), QStringLiteral("reply")) << qPrintable(listed.value(QStringLiteral("message")).toString());
    RemoteSockets::Opened another;
    EXPECT_EQ(m_remote->open(request(watched->processId()), another), QString());
}

TEST_F(RemoteCommandsTest, TheProbeForwardsASocketWhenTheHostHasTheSameAglow)
{
    ASSERT_TRUE(start());
    // The user's -L reaches the probe and the session, never a control command.
    auto process = helper({QStringLiteral("-L"), QStringLiteral("8080:localhost:80"), QStringLiteral("prod")});
    ASSERT_EQ(m_remote->tokens().size(), 1);
    const QString token = m_remote->tokens().first();
    const QString path = QStringLiteral("/run/user/1000/afterglow/%1.sock").arg(token);
    const QString socket = m_remote->socket(token);
    EXPECT_EQ(m_answers.value(FakePane).path, path.toStdString());

    const QString shared = QStringLiteral("ControlPath=") + m_runtime + QStringLiteral("/afterglow-ssh-%C");
    const auto probes = runsWith(QStringLiteral("-T"));
    ASSERT_EQ(probes.size(), 1);
    const QStringList probe = probes.first();
    EXPECT_TRUE(probe.contains(shared)) << qPrintable(probe.join(QLatin1Char(' ')));
    EXPECT_TRUE(probe.contains(QStringLiteral("8080:localhost:80")));
    EXPECT_EQ(probe.mid(probe.size() - 3, 2), (QStringList{QStringLiteral("--"), QStringLiteral("prod")}));
    // The directory step and the prune step.
    EXPECT_TRUE(probe.last().startsWith(QLatin1String("sh -c '")));
    EXPECT_TRUE(probe.last().contains(QLatin1String("umask 077 && mkdir -p")));
    EXPECT_TRUE(probe.last().contains(QLatin1String("aglow remote-prune \"$d\"")));

    // The control commands name the expanded path and no configuration:
    // the files' and the command line's forwards stay out of them.
    const QStringList control{QStringLiteral("-F"), QStringLiteral("none"), QStringLiteral("-S"),
                              m_runtime + QStringLiteral("/afterglow-ssh-fakehash")};
    const auto checks = runsWith(QStringLiteral("-O"), QStringLiteral("check"));
    ASSERT_EQ(checks.size(), 1);
    EXPECT_EQ(checks.first().mid(0, 4), control) << qPrintable(checks.first().join(QLatin1Char(' ')));
    EXPECT_FALSE(checks.first().contains(QStringLiteral("-L")));
    const auto forwards = runsWith(QStringLiteral("-O"), QStringLiteral("forward"));
    ASSERT_EQ(forwards.size(), 1);
    const QStringList forward = forwards.first();
    EXPECT_EQ(forward.mid(forward.indexOf(QStringLiteral("-O"))),
              (QStringList{QStringLiteral("-O"), QStringLiteral("forward"), QStringLiteral("-R"), path + QLatin1Char(':') + socket,
                           QStringLiteral("--"), QStringLiteral("prod")}));
    EXPECT_FALSE(forward.contains(QStringLiteral("-L")));
    // The session shares the connection, and never forwards with -R itself.
    const QStringList session = runs().last();
    EXPECT_EQ(session.last(), QStringLiteral("prod"));
    EXPECT_TRUE(session.contains(shared));
    EXPECT_FALSE(session.contains(QStringLiteral("-R")));

    EXPECT_EQ(finish(*process), 7);
    EXPECT_TRUE(m_stderr.isEmpty()) << qPrintable(m_stderr);
    EXPECT_FALSE(exists(socket));
    // The main process cancels the forward with the same -R value.
    EXPECT_TRUE(pumpUntil([&] {
        const auto cancels = runsWith(QStringLiteral("-O"), QStringLiteral("cancel"));
        return cancels.size() == 1 && cancels.first().contains(path + QLatin1Char(':') + socket)
            && cancels.first().mid(0, 4) == control && !cancels.first().contains(QStringLiteral("-L"));
    }));
}

TEST_F(RemoteCommandsTest, NoAglowNoMasterOrTheSettingOffForwardsNothingAndSaysNothing)
{
    ASSERT_TRUE(start());
    auto process = helper({QStringLiteral("prod")}, {{QStringLiteral("FAKE_AGLOW"), QStringLiteral("missing")}});
    EXPECT_TRUE(m_remote->tokens().isEmpty());
    EXPECT_EQ(runsWith(QStringLiteral("-T")).size(), 1);
    EXPECT_TRUE(runsWith(QStringLiteral("-O")).isEmpty());
    EXPECT_EQ(finish(*process), 7);
    EXPECT_TRUE(m_stderr.isEmpty()) << qPrintable(m_stderr);

    // No master after the probe.
    process = helper({QStringLiteral("prod")}, {{QStringLiteral("FAKE_CHECK"), QStringLiteral("255")}});
    EXPECT_TRUE(m_remote->tokens().isEmpty());
    EXPECT_EQ(runsWith(QStringLiteral("-O"), QStringLiteral("check")).size(), 1);
    EXPECT_TRUE(runsWith(QStringLiteral("-O"), QStringLiteral("forward")).isEmpty());
    EXPECT_FALSE(runs().last().contains(QStringLiteral("-R")));
    EXPECT_EQ(finish(*process), 7);
    EXPECT_TRUE(m_stderr.isEmpty()) << qPrintable(m_stderr);

    // A requirement off: no probe at all, and no message.
    for (const auto& [group, key] : {std::pair{"Terminal", "sshRemoteCommands"}, std::pair{"Control", "enabled"}, std::pair{"Scripts", "enabled"}}) {
        SCOPED_TRACE(QStringLiteral("%1.%2").arg(QLatin1String(group), QLatin1String(key)).toStdString());
        ASSERT_TRUE(m_settings->write(QString::fromLatin1(group), QString::fromLatin1(key), false));
        process = helper({QStringLiteral("prod")});
        EXPECT_TRUE(runsWith(QStringLiteral("-T")).isEmpty());
        EXPECT_EQ(runs().size(), 2); // ssh -G and the session
        EXPECT_EQ(finish(*process), 7);
        EXPECT_TRUE(m_stderr.isEmpty()) << qPrintable(m_stderr);
        ASSERT_TRUE(m_settings->write(QString::fromLatin1(group), QString::fromLatin1(key), true));
    }
}

TEST_F(RemoteCommandsTest, AnotherProtocolIsReportedOnceForEachHostAndVersion)
{
    ASSERT_TRUE(start());
    auto process = helper({QStringLiteral("prod")}, {{QStringLiteral("FAKE_AGLOW"), QStringLiteral("other")}});
    EXPECT_EQ(m_answers.value(FakePane), (Remote::Answer{{}, Remote::Error::Version}));
    EXPECT_TRUE(runsWith(QStringLiteral("-O"), QStringLiteral("forward")).isEmpty());
    EXPECT_EQ(finish(*process), 7);
    EXPECT_TRUE(m_stderr.contains(QLatin1String("aglow on prod speaks protocol 99"))) << qPrintable(m_stderr);
    EXPECT_EQ(m_stderr.count(QLatin1Char('\n')), 1);

    process = helper({QStringLiteral("prod")}, {{QStringLiteral("FAKE_AGLOW"), QStringLiteral("other")}});
    EXPECT_EQ(finish(*process), 7);
    EXPECT_TRUE(m_stderr.isEmpty()) << qPrintable(m_stderr);

    // Another version on the same host, and the same version on another
    // host, are each new: one line each.
    process = helper({QStringLiteral("prod")}, {{QStringLiteral("FAKE_AGLOW"), QStringLiteral("another")}});
    EXPECT_EQ(finish(*process), 7);
    EXPECT_TRUE(m_stderr.contains(QLatin1String("aglow on prod speaks protocol 98"))) << qPrintable(m_stderr);
    EXPECT_EQ(m_stderr.count(QLatin1Char('\n')), 1);
    process = helper({QStringLiteral("stage")}, {{QStringLiteral("FAKE_AGLOW"), QStringLiteral("other")}});
    EXPECT_EQ(finish(*process), 7);
    EXPECT_TRUE(m_stderr.contains(QLatin1String("aglow on stage speaks protocol 99"))) << qPrintable(m_stderr);
    EXPECT_EQ(m_stderr.count(QLatin1Char('\n')), 1);
    // Each stays known.
    for (const auto& [host, aglow] : {std::pair{"prod", "other"}, std::pair{"prod", "another"}, std::pair{"stage", "other"}}) {
        process = helper({QString::fromLatin1(host)}, {{QStringLiteral("FAKE_AGLOW"), QString::fromLatin1(aglow)}});
        EXPECT_EQ(finish(*process), 7);
        EXPECT_TRUE(m_stderr.isEmpty()) << host << " " << aglow << ": " << qPrintable(m_stderr);
    }
}

TEST_F(RemoteCommandsTest, ARefusedForwardOrALongPathLeavesTheReasonForTheQuery)
{
    ASSERT_TRUE(start());
    auto process = helper({QStringLiteral("prod")}, {{QStringLiteral("FAKE_FORWARD"), QStringLiteral("255")}});
    EXPECT_EQ(m_answers.value(FakePane), (Remote::Answer{{}, Remote::Error::Forward}));
    ASSERT_EQ(m_remote->tokens().size(), 1);
    EXPECT_TRUE(m_remote->socket(m_remote->tokens().first()).isEmpty());
    EXPECT_EQ(finish(*process), 7);
    // No message in the session, and no forward to cancel.
    EXPECT_TRUE(m_stderr.isEmpty()) << qPrintable(m_stderr);
    expectNoCancel();

    const QString longDirectory = QStringLiteral("/home/") + QString(90, QLatin1Char('d'));
    process = helper({QStringLiteral("prod")}, {{QStringLiteral("FAKE_DIR"), longDirectory}});
    EXPECT_EQ(m_answers.value(FakePane), (Remote::Answer{{}, Remote::Error::Length}));
    EXPECT_TRUE(runsWith(QStringLiteral("-O"), QStringLiteral("forward")).isEmpty());
    EXPECT_EQ(finish(*process), 7);
    EXPECT_TRUE(m_stderr.isEmpty()) << qPrintable(m_stderr);
}

TEST_F(RemoteCommandsTest, ARunWhoseHandlerTurnsTheSettingOffEndsWithItsSocket)
{
    ASSERT_TRUE(start());
    m_scripts->setEnabled(true);
    const auto watched = sleeper();
    RemoteSockets::Opened opened;
    ASSERT_EQ(m_remote->open(request(watched->processId()), opened), QString());
    ASSERT_EQ(m_remote->forwarded(opened.token), QString());
    // The setting goes off while the session's router runs the call of the
    // run: the router and its target must outlive that call. Freed too
    // early, they often still work in a normal build: the debug preset's
    // sanitizers show it every time (TESTING.md).
    bool turnedOff = false;
    QObject::connect(m_scripts.get(), &ScriptHost::effectApplied, m_scripts.get(), [&] {
        if (turnedOff) return;
        turnedOff = true;
        m_settings->write(QStringLiteral("Terminal"), QStringLiteral("sshRemoteCommands"), false);
    });
    QLocalSocket client;
    ASSERT_TRUE(connectTo(client, opened.socket));
    send(client, QStringLiteral("remote"), QStringLiteral("run"),
         {QStringLiteral("r1"), QStringLiteral("far"), QStringLiteral("there"), QJsonArray{}, QString(), 0});
    EXPECT_TRUE(pumpUntil([&] { return turnedOff && client.state() == QLocalSocket::UnconnectedState; }));
    EXPECT_TRUE(m_remote->tokens().isEmpty());
    EXPECT_FALSE(exists(opened.socket));
    EXPECT_FALSE(exists(opened.socket + QStringLiteral(".lock")));
    // The forward came after the session ended: the client must cancel it.
    EXPECT_FALSE(m_remote->forwarded(opened.token).isEmpty());
}

TEST_F(RemoteCommandsTest, ARunWhoseHandlerClosesItsSocketAsItStartsIsCancelled)
{
    ASSERT_TRUE(start());
    m_scripts->setEnabled(true);
    // One run of the script at a time: a run that was left running would refuse the next.
    ASSERT_TRUE(m_settings->write(QStringLiteral("Scripts"), QStringLiteral("commandRunLimit"), 1));
    const auto watched = sleeper();
    const auto runSlow = [&](bool turnOff) {
        RemoteSockets::Opened opened;
        EXPECT_EQ(m_remote->open(request(watched->processId()), opened), QString());
        EXPECT_EQ(m_remote->forwarded(opened.token), QString());
        bool turnedOff = !turnOff;
        // The first output of "slow" comes while startCommand runs: the
        // socket closes before the run has its handle.
        const QMetaObject::Connection hook =
            QObject::connect(m_scripts.get(), &ScriptHost::effectApplied, m_scripts.get(), [&] {
                if (turnedOff) return;
                turnedOff = true;
                m_settings->write(QStringLiteral("Terminal"), QStringLiteral("sshRemoteCommands"), false);
            });
        QLocalSocket client;
        EXPECT_TRUE(connectTo(client, opened.socket));
        const qint64 id = send(client, QStringLiteral("remote"), QStringLiteral("run"),
                               {QStringLiteral("r1"), QStringLiteral("far"), QStringLiteral("slow"), QJsonArray{}, QString(), 0});
        QList<QJsonObject> frames;
        const bool done = pumpUntil([&] {
            while (client.canReadLine()) frames << QJsonDocument::fromJson(client.readLine()).object();
            return turnOff ? client.state() == QLocalSocket::UnconnectedState : output(frames) == QLatin1String("start\n");
        });
        QObject::disconnect(hook);
        EXPECT_TRUE(done);
        for (const QJsonObject& frame : std::as_const(frames)) {
            if (frame.value(QStringLiteral("id")).toInteger() == id && frame.value(QStringLiteral("type")) == QLatin1String("error"))
                return frame.value(QStringLiteral("message")).toString();
        }
        return QString();
    };
    runSlow(true);
    ASSERT_TRUE(m_settings->write(QStringLiteral("Terminal"), QStringLiteral("sshRemoteCommands"), true));
    // The first run was cancelled: the second starts and prints.
    EXPECT_EQ(runSlow(false), QString());
}

TEST_F(RemoteCommandsTest, TheProbeIsNoMasterOfItsOwnForAUsersDashM)
{
    ASSERT_TRUE(start());
    // The user's ControlPath, on which a master runs: the probe is its
    // client, and the forward lives on it.
    const QString userPath = m_dir.filePath(QStringLiteral("user-cp"));
    auto process = helper({QStringLiteral("-M"), QStringLiteral("-vM"), QStringLiteral("prod")},
                          {{QStringLiteral("FAKE_CONTROL_PATH"), userPath}});
    const auto probes = runsWith(QStringLiteral("-T"));
    ASSERT_EQ(probes.size(), 1);
    EXPECT_FALSE(probes.first().contains(QStringLiteral("-M"))) << qPrintable(probes.first().join(QLatin1Char(' ')));
    EXPECT_TRUE(probes.first().contains(QStringLiteral("ControlMaster=no")));
    EXPECT_TRUE(probes.first().contains(QStringLiteral("-v")));
    EXPECT_EQ(m_remote->tokens().size(), 1);
    // The session keeps the user's -M.
    EXPECT_TRUE(runs().last().contains(QStringLiteral("-M")));
    EXPECT_EQ(finish(*process), 7);
    // ssh -G shows -M as ControlMaster yes: with no master on that path, the
    // probe would be a master that the session does not join.
    process = helper({QStringLiteral("-vM"), QStringLiteral("prod")},
                     {{QStringLiteral("FAKE_CONTROL_PATH"), userPath}, {QStringLiteral("FAKE_CHECK"), QStringLiteral("255")}});
    EXPECT_EQ(runsWith(QStringLiteral("-O"), QStringLiteral("check")).size(), 1);
    EXPECT_TRUE(runsWith(QStringLiteral("-T")).isEmpty());
    EXPECT_TRUE(m_remote->tokens().isEmpty());
    EXPECT_TRUE(runs().last().contains(QStringLiteral("-vM")));
    EXPECT_EQ(finish(*process), 7);
    EXPECT_TRUE(m_stderr.contains(QLatin1String("no remote commands in this session: no master runs on the ControlPath, "
                                                "which -M needs")))
        << qPrintable(m_stderr);
}

TEST_F(RemoteCommandsTest, ADashMWithoutAControlPathOfTheUsersGetsNoRemoteCommands)
{
    // ssh -M joins no master: the session would not share the helper's
    // connection, which holds the forward and ends after ControlPersist.
    ASSERT_TRUE(start());
    const QString shared = QStringLiteral("ControlPath=") + m_runtime + QStringLiteral("/afterglow-ssh-%C");
    auto process = helper({QStringLiteral("-M"), QStringLiteral("prod")});
    EXPECT_TRUE(runsWith(QStringLiteral("-T")).isEmpty());
    EXPECT_TRUE(m_remote->tokens().isEmpty());
    EXPECT_TRUE(runs().last().contains(QStringLiteral("-M")));
    EXPECT_FALSE(runs().last().contains(shared));
    EXPECT_EQ(finish(*process), 7);
    EXPECT_TRUE(m_stderr.isEmpty()) << qPrintable(m_stderr);
    process = helper({QStringLiteral("-vM"), QStringLiteral("prod")});
    EXPECT_EQ(finish(*process), 7);
    EXPECT_TRUE(m_stderr.contains(QLatin1String("no remote commands in this session: -M needs a "
                                                "ControlPath of your own")))
        << qPrintable(m_stderr);
}

TEST_F(RemoteCommandsTest, ALiveSocketHoldsItsLockAndAWaitingForwardAnswersNothing)
{
    ASSERT_TRUE(start());
    const auto watched = sleeper();
    RemoteSockets::Opened opened;
    ASSERT_EQ(m_remote->open(request(watched->processId()), opened), QString());
    // Another launch's removeStaleSockets passes it by: the lock is held.
    EXPECT_EQ(InstanceSocket::lock(opened.socket), AfterglowProto::LockHeld);
    // Until its forward came, the pane does not answer with a path that may not exist.
    EXPECT_EQ(m_answers.value(FakePane), Remote::Answer{});
    EXPECT_EQ(m_remote->forwarded(opened.token), QString());
    EXPECT_EQ(m_answers.value(FakePane).path, opened.path.toStdString());
    // A forwarded() that came late: the client cancelled the forward, and
    // its remoteSocketFailed comes after. The path goes from the answer.
    EXPECT_EQ(m_remote->failed(opened.token, QStringLiteral("forward")), QString());
    EXPECT_FALSE(exists(opened.socket));
    EXPECT_EQ(m_answers.value(FakePane), (Remote::Answer{{}, Remote::Error::Forward}));
    // A session without a socket has no forward to fail.
    RemoteSockets::Request other = request(watched->processId());
    other.protocol = AfterglowProto::ProtocolVersion + 1;
    RemoteSockets::Opened version;
    ASSERT_EQ(m_remote->open(other, version), QString());
    EXPECT_FALSE(m_remote->failed(version.token, QStringLiteral("forward")).isEmpty());
    m_remote->paneClosed(FakePane);
    EXPECT_FALSE(exists(opened.socket + QStringLiteral(".lock")));
}

TEST_F(RemoteCommandsTest, TheSocketOfASessionRefusesConnectionsPastItsCapAndLogsOnce)
{
    static int warnings = 0;
    warnings = 0;
    static QtMessageHandler previous = nullptr;
    previous = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& context, const QString& message) {
        if (type == QtWarningMsg && context.category && QLatin1String(context.category) == QLatin1String("afterglow.commands")
            && message.contains(QLatin1String("refuses more")))
            ++warnings;
        else if (previous)
            previous(type, context, message);
    });
    const auto restore = qScopeGuard([] { qInstallMessageHandler(previous); });
    ASSERT_TRUE(start());
    m_scripts->setEnabled(true);
    const auto watched = sleeper();
    RemoteSockets::Opened opened;
    ASSERT_EQ(m_remote->open(request(watched->processId()), opened), QString());
    // A reply on each shows that the router took the connection.
    const auto answered = [&](QLocalSocket& client) {
        const qint64 id = send(client, QStringLiteral("remote"), QStringLiteral("list"), {});
        bool reply = false;
        pumpUntil([&] {
            while (client.canReadLine()) {
                if (QJsonDocument::fromJson(client.readLine()).object().value(QStringLiteral("id")).toInteger() == id) reply = true;
            }
            return reply || client.state() == QLocalSocket::UnconnectedState;
        });
        return reply;
    };
    std::vector<std::unique_ptr<QLocalSocket>> clients;
    for (int i = 0; i < RemoteSockets::MaxConnectionsPerSession; ++i) {
        clients.push_back(std::make_unique<QLocalSocket>());
        ASSERT_TRUE(connectTo(*clients.back(), opened.socket));
        ASSERT_TRUE(answered(*clients.back())) << i;
    }
    // Past the cap, a connection closes before the router reads its request.
    for (int i = 0; i < 2; ++i) {
        QLocalSocket over;
        ASSERT_TRUE(connectTo(over, opened.socket));
        EXPECT_FALSE(answered(over));
        EXPECT_EQ(over.state(), QLocalSocket::UnconnectedState);
    }
    EXPECT_EQ(warnings, 1);
    // A connection that closes makes room.
    clients.front()->disconnectFromServer();
    QLocalSocket next;
    ASSERT_TRUE(pumpUntil([&] {
        next.abort();
        return connectTo(next, opened.socket) && answered(next);
    }));
    QLocalSocket over;
    ASSERT_TRUE(connectTo(over, opened.socket));
    EXPECT_FALSE(answered(over));
    // A full socket again, after one closed: logged again.
    EXPECT_EQ(warnings, 2);
}

TEST_F(RemoteCommandsTest, AHostNameIsShortTextAndAPaneHasSixteenSessionsAtMost)
{
    ASSERT_TRUE(start());
    const auto watched = sleeper();
    RemoteSockets::Opened opened;
    for (const QString& host : {QString(), QStringLiteral("a\x1b]0;b"), QStringLiteral("a\u009bb"), QString(256, QLatin1Char('h'))}) {
        RemoteSockets::Request bad = request(watched->processId());
        bad.host = host;
        EXPECT_FALSE(m_remote->open(bad, opened).isEmpty()) << qPrintable(host);
    }
    RemoteSockets::Request backslash = request(watched->processId(), QStringLiteral("/a\\b"));
    EXPECT_FALSE(m_remote->open(backslash, opened).isEmpty());
    EXPECT_TRUE(m_remote->tokens().isEmpty());

    // Sessions with no socket count too.
    RemoteSockets::Request other = request(watched->processId());
    other.protocol = AfterglowProto::ProtocolVersion + 1;
    for (int i = 0; i < RemoteSockets::MaxSessionsPerPane; ++i) ASSERT_EQ(m_remote->open(other, opened), QString()) << i;
    EXPECT_EQ(m_remote->open(other, opened), QStringLiteral("pane 7 has 16 remote sessions already"));
    m_remote->paneClosed(FakePane);
    EXPECT_EQ(m_remote->open(other, opened), QString());
}

TEST_F(RemoteCommandsTest, TheRemoteSessionsShareTheWaitingInputBudgetOfTheProcess)
{
    ASSERT_TRUE(start());
    // As Application makes it: the sessions count toward the budget of the process's own socket.
    RemoteSockets shared(*m_control, *m_scripts, *m_settings, [](quint32, const Remote::Answer&, const std::optional<Remote::Answer>&) { return true; },
                         m_commands->budget());
    shared.setSsh(fakeSsh());
    const auto watched = sleeper();
    RemoteSockets::Opened opened;
    ASSERT_EQ(shared.open(request(watched->processId()), opened), QString());
    QLocalSocket client;
    ASSERT_TRUE(connectTo(client, opened.socket));
    qint64 id = send(client, QStringLiteral("remote"), QStringLiteral("input"), {QStringLiteral("r1"), QStringLiteral("abc"), false});
    QList<QJsonObject> frames = until(client, id);
    ASSERT_FALSE(frames.isEmpty());
    EXPECT_EQ(frames.last().value(QStringLiteral("type")).toString(), QStringLiteral("reply"));
    // The process's own socket sees the remote input.
    EXPECT_EQ(m_commands->waitingInputBytes(), 3);
    // Full from the other sockets: the remote input is refused.
    m_commands->budget()->inputBytes = CommandSocket::MaxWaitingInputBytes;
    id = send(client, QStringLiteral("remote"), QStringLiteral("input"), {QStringLiteral("r2"), QStringLiteral("x"), true});
    frames = until(client, id);
    ASSERT_FALSE(frames.isEmpty());
    EXPECT_EQ(frames.last().value(QStringLiteral("type")).toString(), QStringLiteral("error"));
    m_commands->budget()->inputBytes = 3;
    // The session's close gives its share back.
    shared.paneClosed(FakePane);
    EXPECT_EQ(m_commands->waitingInputBytes(), 0);
}

TEST_F(RemoteCommandsTest, ControlPathNoneOrControlMasterNoKeepsTheUsersChoice)
{
    ASSERT_TRUE(start());
    const QString shared = QStringLiteral("ControlPath=") + m_runtime + QStringLiteral("/afterglow-ssh-%C");
    for (const QString& user : {QStringLiteral("path-none"), QStringLiteral("master-no")}) {
        SCOPED_TRACE(user.toStdString());
        auto process = helper({QStringLiteral("prod")}, {{QStringLiteral("FAKE_USER"), user}});
        // No probe, no shared connection of the helper's, no socket.
        EXPECT_TRUE(runsWith(QStringLiteral("-T")).isEmpty());
        EXPECT_TRUE(m_remote->tokens().isEmpty());
        EXPECT_FALSE(runs().last().contains(shared));
        EXPECT_EQ(finish(*process), 7);
        EXPECT_TRUE(m_stderr.isEmpty()) << qPrintable(m_stderr);
    }
    // With -v the helper says why.
    auto process = helper({QStringLiteral("-v"), QStringLiteral("prod")}, {{QStringLiteral("FAKE_USER"), QStringLiteral("path-none")}});
    EXPECT_EQ(finish(*process), 7);
    EXPECT_TRUE(m_stderr.contains(QLatin1String("no remote commands in this session: ControlPath is none"))) << qPrintable(m_stderr);
}

// A server that answers the control call with an error: -v gives its
// reason, not "does not answer". The mux daemon answers so for a session
// pane, and an Afterglow with Control.enabled off has no control target.
TEST_F(RemoteCommandsTest, WithVerboseAControlCallThatFailsSaysWhy)
{
    ASSERT_TRUE(start());
    struct Case
    {
        const char* code;
        std::string message;
        const char* expected;
    };
    const Case cases[] = {
        // The server's text, safe to print.
        {AfterglowProto::IpcErrorCode::CallFailed, "a session pane takes no control calls\x1b[2J",
         "no remote commands in this session: a session pane takes no control calls?[2J\n"},
        {AfterglowProto::IpcErrorCode::NoSuchTarget, "no target control",
         "no remote commands in this session: Afterglow has no control target: Control.enabled is off\n"},
        {AfterglowProto::IpcErrorCode::CallFailed, "", "no remote commands in this session: Afterglow does not answer\n"},
    };
    int index = 0;
    for (const Case& c : cases) {
        SCOPED_TRACE(c.expected);
        const std::string path = (m_runtime + QStringLiteral("/refuses-%1.sock").arg(index++)).toStdString();
        AfterglowProto::IpcListener listener;
        std::string error;
        ASSERT_EQ(listener.listen(path, &error), AfterglowProto::IpcListener::Listen::Listening) << error;
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
                        connection->send(AfterglowProto::ipcError(request.id, c.code, c.message));
                    }
                    connection->flush();
                }
            }
        });
        auto process = helper({QStringLiteral("-v"), QStringLiteral("prod")},
                              {{QStringLiteral("AFTERGLOW_SOCKET"), QString::fromStdString(path)}});
        EXPECT_EQ(finish(*process), 7);
        stop = true;
        EXPECT_TRUE(m_stderr.contains(QLatin1String(c.expected))) << qPrintable(m_stderr);
    }
}

TEST_F(RemoteCommandsTest, AForwardThatAfterglowConfirmsTooLateIsCancelledWithTheReasonForward)
{
    ASSERT_TRUE(start());
    // As ssh -O forward runs, the test opens its gate and then holds the
    // event loop until the helper gave up on remoteSocketForwarded and
    // cancelled the forward. Afterglow takes that call late, and the
    // remoteSocketFailed that the helper sends behind it after.
    const QString gate = m_dir.filePath(QStringLiteral("gate"));
    QTimer stall;
    QObject::connect(&stall, &QTimer::timeout, [&] {
        if (runsWith(QStringLiteral("-O"), QStringLiteral("forward")).isEmpty()) return;
        stall.stop();
        QFile open(gate);
        EXPECT_TRUE(open.open(QIODevice::WriteOnly));
        // Until the helper gave up on the call: it cancels the forward then.
        QElapsedTimer held;
        held.start();
        while (runsWith(QStringLiteral("-O"), QStringLiteral("cancel")).isEmpty() && held.elapsed() < 10000) QThread::msleep(10);
    });
    stall.start(5);
    auto process = helper({QStringLiteral("-v"), QStringLiteral("prod")}, {{QStringLiteral("FAKE_FORWARD_GATE"), gate}});
    ASSERT_FALSE(stall.isActive());
    ASSERT_EQ(m_remote->tokens().size(), 1);
    const QString token = m_remote->tokens().first();
    // The late confirmation answered with the path, and the failure after it
    // replaced the answer: the query tells aglow run why.
    EXPECT_TRUE(pumpUntil([&] { return m_answers.value(FakePane) == (Remote::Answer{{}, Remote::Error::Forward}); }));
    EXPECT_TRUE(m_remote->socket(token).isEmpty());
    // The helper cancelled the forward that it had added.
    const auto forwards = runsWith(QStringLiteral("-O"), QStringLiteral("forward"));
    ASSERT_EQ(forwards.size(), 1);
    const QString spec = forwards.first().value(forwards.first().indexOf(QStringLiteral("-R")) + 1);
    const auto cancels = runsWith(QStringLiteral("-O"), QStringLiteral("cancel"));
    ASSERT_EQ(cancels.size(), 1);
    EXPECT_TRUE(cancels.first().contains(spec)) << qPrintable(cancels.first().join(QLatin1Char(' ')));
    EXPECT_EQ(finish(*process), 7);
    EXPECT_TRUE(m_stderr.contains(QLatin1String("no remote commands in this session: Afterglow did not answer, and the forward is "
                                                "cancelled")))
        << qPrintable(m_stderr);
    // Afterglow cancels nothing more as the session ends.
    ASSERT_TRUE(pumpUntil([&] { return !fakeSshRuns(); }));
    EXPECT_EQ(runsWith(QStringLiteral("-O"), QStringLiteral("cancel")).size(), 1);
}

TEST_F(RemoteCommandsTest, AProbeThatRanIsNoFailureOfSshWhateverItsStatus)
{
    ASSERT_TRUE(start());
    // A command on the host that ends with 255 after the script ran.
    auto process = helper({QStringLiteral("prod")}, {{QStringLiteral("FAKE_PROBE_STATUS"), QStringLiteral("255")}});
    EXPECT_EQ(m_remote->tokens().size(), 1);
    EXPECT_EQ(finish(*process), 7);
    // No terminal on the host: nothing to ask the pane through, so no probe.
    process = helper({QStringLiteral("prod")}, {{QStringLiteral("FAKE_TTY"), QStringLiteral("false")}});
    EXPECT_TRUE(runsWith(QStringLiteral("-T")).isEmpty());
    EXPECT_TRUE(m_remote->tokens().isEmpty());
    EXPECT_EQ(finish(*process), 7);
}

// --- The acceptance test, with sshd run as the user ------------------------

namespace {

/// Two sshd of the test's own on 127.0.0.1, one that allows Unix socket
/// forwarding and one that does not, with keys and configs in a temporary
/// directory. Stopped and removed at the end, also when a test fails.
class Sshd
{
public:
    explicit Sshd(const QString& directory, const QString& remotePath)
        : m_dir(directory)
    {
        m_sshd = QStandardPaths::findExecutable(QStringLiteral("sshd"), {QStringLiteral("/usr/sbin"), QStringLiteral("/usr/bin"),
                                                                         QStringLiteral("/sbin")});
        if (m_sshd.isEmpty()) m_sshd = QStandardPaths::findExecutable(QStringLiteral("sshd"));
        const QString keygen = QStandardPaths::findExecutable(QStringLiteral("ssh-keygen"));
        if (m_sshd.isEmpty() || keygen.isEmpty() || QStandardPaths::findExecutable(QStringLiteral("ssh")).isEmpty()) {
            m_missing = QStringLiteral("sshd, ssh or ssh-keygen is not installed");
            return;
        }
        // From here on the tools are there: a failure is the harness's, and fails the test.
        if (!QDir().mkpath(m_dir)) {
            m_broken = QStringLiteral("cannot make %1").arg(m_dir);
            return;
        }
        const passwd* user = ::getpwuid(::getuid());
        if (!user || !user->pw_name) {
            m_broken = QStringLiteral("this user has no name in the password database");
            return;
        }
        for (const QString& key : {QStringLiteral("host_key"), QStringLiteral("client_key")}) {
            if (const Result keyed = run(keygen, {QStringLiteral("-q"), QStringLiteral("-t"), QStringLiteral("ed25519"), QStringLiteral("-N"),
                                                  QString(), QStringLiteral("-f"), path(key)});
                keyed.status != 0) {
                m_broken = QStringLiteral("ssh-keygen failed: ") + keyed.err;
                return;
            }
        }
        if (!QFile::copy(path(QStringLiteral("client_key.pub")), path(QStringLiteral("authorized_keys")))) {
            m_broken = QStringLiteral("cannot write authorized_keys");
            return;
        }
        for (const QString& dir : {QStringLiteral("remote-home"), QStringLiteral("remote-run"), QStringLiteral("c")}) {
            if (!QDir().mkpath(path(dir))
                || !QFile::setPermissions(path(dir), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner)) {
                m_broken = QStringLiteral("cannot make %1").arg(path(dir));
                return;
            }
        }
        QFile hostKey(path(QStringLiteral("host_key.pub")));
        if (!hostKey.open(QIODevice::ReadOnly)) {
            m_broken = QStringLiteral("no host key");
            return;
        }
        const QByteArray publicKey = hostKey.readAll().trimmed();
        QByteArray knownHosts;
        // The name from the password database, as ssh's own default: USER
        // can be unset or another user's.
        QByteArray client = "IdentityFile " + path(QStringLiteral("client_key")).toUtf8() + "\n"
            "IdentitiesOnly yes\nIdentityAgent none\nUserKnownHostsFile " + path(QStringLiteral("known_hosts")).toUtf8() + "\n"
            "GlobalKnownHostsFile /dev/null\nStrictHostKeyChecking yes\nUpdateHostKeys no\nBatchMode yes\n"
            "ControlMaster auto\nControlPath " + path(QStringLiteral("c/%C")).toUtf8() + "\nControlPersist 30\n"
            "ForwardAgent no\nLogLevel ERROR\nUser " + QByteArray(user->pw_name) + "\n";
        for (const bool allow : {true, false}) {
            // The port stays bound until sshd listens on it, so that no other
            // program gets it in between. sshd binds with SO_REUSEADDR, as
            // this socket does: Linux lets it while this one does not listen.
            const PortHold hold;
            if (!hold.valid()) {
                m_broken = QStringLiteral("no free port on 127.0.0.1");
                return;
            }
            const int port = hold.port();
            const QString name = allow ? QStringLiteral("allow") : QStringLiteral("refuse");
            QFile config(path(name + QStringLiteral(".sshd_config")));
            if (!config.open(QIODevice::WriteOnly)) {
                m_broken = QStringLiteral("cannot write the sshd config");
                return;
            }
            config.write("Port " + QByteArray::number(port) + "\nListenAddress 127.0.0.1\nHostKey " + path(QStringLiteral("host_key")).toUtf8()
                         + "\nPidFile " + path(name + QStringLiteral(".pid")).toUtf8() + "\nAuthorizedKeysFile "
                         + path(QStringLiteral("authorized_keys")).toUtf8()
                         + "\nStrictModes no\nUsePAM no\nPasswordAuthentication no\nKbdInteractiveAuthentication no\n"
                           "PubkeyAuthentication yes\nPermitUserEnvironment no\nAllowAgentForwarding no\nX11Forwarding no\n"
                           "AllowStreamLocalForwarding "
                         + (allow ? "yes" : "no") + "\nSetEnv HOME=" + path(QStringLiteral("remote-home")).toUtf8()
                         + " XDG_RUNTIME_DIR=" + path(QStringLiteral("remote-run")).toUtf8() + " PATH=" + remotePath.toUtf8()
                         + "\nLogLevel ERROR\n");
            config.close();
            auto process = std::make_unique<QProcess>();
            // One open of the log: two would each write from the start of it.
            process->setStandardOutputFile(QProcess::nullDevice());
            process->setStandardErrorFile(path(name + QStringLiteral(".log")));
            // The destructor stops it. When the test binary dies before
            // that, the kernel does.
            process->setChildProcessModifier(endWithTheTest);
            process->start(m_sshd,{QStringLiteral("-D"), QStringLiteral("-e"), QStringLiteral("-f"), config.fileName()});
            if (!process->waitForStarted()) {
                m_broken = QStringLiteral("sshd did not start");
                return;
            }
            m_servers.push_back(std::move(process));
            knownHosts += "[127.0.0.1]:" + QByteArray::number(port) + " " + publicKey + "\n";
            client += "Host " + name.toUtf8() + "\n  HostName 127.0.0.1\n  Port " + QByteArray::number(port) + "\n";
            if (!waitForPort(port)) {
                m_broken = QStringLiteral("sshd did not listen: ") + log(name);
                return;
            }
        }
        QFile known(path(QStringLiteral("known_hosts")));
        if (!known.open(QIODevice::WriteOnly) || known.write(knownHosts) != knownHosts.size()) {
            m_broken = QStringLiteral("cannot write known_hosts");
            return;
        }
        // Host-specific lines first: ssh takes the first value of each option.
        QFile config(clientConfig());
        const qsizetype hosts = client.indexOf("Host ");
        const QByteArray ordered = client.mid(hosts) + "Host *\n" + client.left(hosts);
        if (!config.open(QIODevice::WriteOnly) || config.write(ordered) != ordered.size()) m_broken = QStringLiteral("cannot write the ssh config");
    }

    ~Sshd()
    {
        if (m_missing.isEmpty()) {
            for (const char* host : {"allow", "refuse"}) ssh({QStringLiteral("-O"), QStringLiteral("exit"), QLatin1String(host)});
        }
        for (const auto& server : m_servers) {
            server->terminate();
            if (!server->waitForFinished(3000)) server->kill();
            server->waitForFinished();
        }
    }

    /// Why the test cannot run here: a tool is not installed. Else empty.
    QString missing() const { return m_missing; }
    /// What failed in the harness, with the tools there. Else empty.
    QString broken() const { return m_broken; }
    QString path(const QString& name) const { return m_dir + QLatin1Char('/') + name; }
    QString clientConfig() const { return path(QStringLiteral("ssh_config")); }
    QString remoteRun() const { return path(QStringLiteral("remote-run")); }
    /// The log of the sshd @p name (allow or refuse).
    QString log(const QString& name) const
    {
        QFile file(path(name + QStringLiteral(".log")));
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    }

    struct Result
    {
        int status = -1; ///< -1 when it did not exit by itself in time
        QString err;
    };
    /// ssh with the test's own config, and never an agent.
    Result ssh(const QStringList& args) const
    {
        return run(QStringLiteral("ssh"), QStringList{QStringLiteral("-F"), clientConfig()} + args);
    }

private:
    /// Runs @p program with no agent and no input. One that hangs is killed:
    /// its status is then -1, never the 0 of a QProcess that has not ended.
    static Result run(const QString& program, const QStringList& args)
    {
        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("SSH_AUTH_SOCK"));
        process.setProcessEnvironment(env);
        process.setStandardInputFile(QProcess::nullDevice());
        process.start(program, args);
        if (!process.waitForFinished(20000)) {
            process.kill();
            process.waitForFinished();
            return {-1, QStringLiteral("%1 %2 did not end in 20 s").arg(program, args.join(QLatin1Char(' ')))};
        }
        return {process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1,
                QString::fromUtf8(process.readAllStandardError())};
    }

    /// A port of 127.0.0.1 that this socket holds, bound and not listening.
    class PortHold
    {
    public:
        PortHold()
            : m_fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0))
        {
            if (m_fd < 0) return;
            const int on = 1;
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            socklen_t length = sizeof address;
            if (::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) == 0
                && ::bind(m_fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0
                && ::getsockname(m_fd, reinterpret_cast<sockaddr*>(&address), &length) == 0)
                m_port = ntohs(address.sin_port);
        }
        ~PortHold()
        {
            if (m_fd >= 0) ::close(m_fd);
        }
        PortHold(const PortHold&) = delete;
        PortHold& operator=(const PortHold&) = delete;
        bool valid() const { return m_port != 0; }
        int port() const { return m_port; }

    private:
        int m_fd = -1;
        int m_port = 0;
    };

    static bool waitForPort(int port)
    {
        return pumpUntil([port] {
            const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(quint16(port));
            const bool ok = ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
            ::close(fd);
            return ok;
        });
    }

    QString m_dir;
    QString m_sshd;
    QString m_missing;
    QString m_broken;
    std::vector<std::unique_ptr<QProcess>> m_servers;
};

} // namespace

TEST_F(RemoteCommandsTest, OpenSshForwardsAsTheDesignExpectsAndAglowRunWorksThroughIt)
{
#ifndef AFTERGLOW_AGLOW
    GTEST_SKIP() << "this build has no aglow (AFTERGLOW_BUILD_AGLOW=OFF)";
#else
    const QString aglowDirectory = QFileInfo(QStringLiteral(AFTERGLOW_AGLOW)).absolutePath();
    Sshd sshd(m_dir.filePath(QStringLiteral("ssh")), aglowDirectory + QStringLiteral(":/usr/bin:/bin"));
    if (!sshd.missing().isEmpty()) GTEST_SKIP() << qPrintable(sshd.missing());
    ASSERT_TRUE(sshd.broken().isEmpty()) << qPrintable(sshd.broken());
    ASSERT_TRUE(start());
    m_scripts->setEnabled(true);
    // The real ssh cancels the forward, with the options of the session: the test's -F.
    m_remote->setSsh(QStringLiteral("ssh"));

    // The OpenSSH behavior that the design depends on.
    QLocalServer local;
    const QString target = sshd.path(QStringLiteral("l.sock"));
    ASSERT_TRUE(local.listen(target));
    ASSERT_EQ(sshd.ssh({QStringLiteral("-MNf"), QStringLiteral("allow")}).status, 0);
    ASSERT_EQ(sshd.ssh({QStringLiteral("-MNf"), QStringLiteral("refuse")}).status, 0);
    // With -M, past the config's ControlMaster auto, a session joins no
    // master that runs on its ControlPath: it signs in on its own and shares
    // nothing. Thus afterglow ssh -M forwards no socket on a master of its
    // own.
    const Sshd::Result dashM = sshd.ssh({QStringLiteral("-M"), QStringLiteral("allow"), QStringLiteral("true")});
    EXPECT_EQ(dashM.status, 0) << qPrintable(dashM.err);
    EXPECT_TRUE(dashM.err.contains(QLatin1String("already exists, disabling multiplexing"))) << qPrintable(dashM.err);
    // A client of the master says nothing.
    const Sshd::Result client = sshd.ssh({QStringLiteral("allow"), QStringLiteral("true")});
    EXPECT_EQ(client.status, 0);
    EXPECT_TRUE(client.err.isEmpty()) << qPrintable(client.err);
    const QString directory = sshd.remoteRun() + QStringLiteral("/t");
    QDir().mkpath(directory);
    const QString forwarded = directory + QStringLiteral("/a.sock");
    const auto forward = [&](const QString& host, const QString& path, const QStringList& more = {}) {
        return sshd.ssh(more + QStringList{QStringLiteral("-O"), QStringLiteral("forward"), QStringLiteral("-R"),
                                           path + QLatin1Char(':') + target, host});
    };
    // A forward on the shared connection: mode 0600, and it reaches the local socket.
    ASSERT_EQ(forward(QStringLiteral("allow"), forwarded).status, 0);
    EXPECT_EQ(QFileInfo(forwarded).permissions() & ~(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadUser
                                                    | QFileDevice::WriteUser),
              QFileDevice::Permissions());
    EXPECT_EQ(connectError(forwarded), 0);
    EXPECT_TRUE(pumpUntil([&] { return local.hasPendingConnections(); }));
    while (local.hasPendingConnections()) delete local.nextPendingConnection();
    // A cancel refuses connections, and leaves the file.
    ASSERT_EQ(sshd.ssh({QStringLiteral("-O"), QStringLiteral("cancel"), QStringLiteral("-R"), forwarded + QLatin1Char(':') + target,
                        QStringLiteral("allow")})
                  .status,
              0);
    EXPECT_TRUE(pumpUntil([&] { return connectError(forwarded) == ECONNREFUSED; }));
    EXPECT_TRUE(exists(forwarded));
    // A stale file fails the forward, and the client's StreamLocalBindUnlink changes nothing.
    EXPECT_EQ(forward(QStringLiteral("allow"), forwarded).status, 255);
    EXPECT_EQ(forward(QStringLiteral("allow"), forwarded, {QStringLiteral("-o"), QStringLiteral("StreamLocalBindUnlink=yes")}).status,
              255);
    // A missing parent directory fails it.
    EXPECT_EQ(forward(QStringLiteral("allow"), directory + QStringLiteral("/missing/b.sock")).status, 255);
    // A server that refuses forwarding fails it with the same status.
    EXPECT_EQ(forward(QStringLiteral("refuse"), directory + QStringLiteral("/c.sock")).status, 255);
    // A path over Linux's 108 bytes fails.
    EXPECT_EQ(forward(QStringLiteral("allow"), directory + QLatin1Char('/') + QString(110, QLatin1Char('x'))).status, 255);
    // A session's own -R stays on the shared connection after the session.
    const QString own = directory + QStringLiteral("/own.sock");
    EXPECT_EQ(sshd.ssh({QStringLiteral("-R"), own + QLatin1Char(':') + target, QStringLiteral("allow"), QStringLiteral("true")}).status, 0);
    EXPECT_EQ(connectError(own), 0);
    // On a server that refuses it, ssh complains and the forward is not there.
    const Sshd::Result refused = sshd.ssh({QStringLiteral("-R"), directory + QStringLiteral("/d.sock:") + target,
                                           QStringLiteral("refuse"), QStringLiteral("true")});
    EXPECT_FALSE(refused.err.isEmpty());
    EXPECT_FALSE(exists(directory + QStringLiteral("/d.sock")));
    ASSERT_EQ(sshd.ssh({QStringLiteral("-O"), QStringLiteral("exit"), QStringLiteral("allow")}).status, 0);

    // A whole afterglow ssh session that runs aglow run on the "remote" side.
    m_pane = std::make_unique<Pane>(AfterglowVt::TerminalOptions{});
    Pane& pane = *m_pane;
    AfterglowPty::SpawnOptions options;
    options.argv = {AFTERGLOW_BINARY, "ssh", "-F", sshd.clientConfig().toStdString(), "-t", "allow",
                    "aglow run far there one; aglow run far here; sleep 1"};
    options.workingDirectory = m_dir.path().toStdString();
    options.environment = {{"HOME", m_dir.filePath(QStringLiteral("home")).toStdString()}};
    options.unsetEnvironment = {"SSH_AUTH_SOCK", "TMUX"};
    QString error;
    ASSERT_TRUE(pane.start(options, QStringLiteral("xterm-256color"), 120, 30, 8, 16, &error)) << qPrintable(error);
    const auto screen = [&] { return pane.text(AfterglowVt::Terminal::TextScope::Screen); };
    EXPECT_TRUE(pumpUntil([&] { return screen().contains(QLatin1String("no command here in far")); }, 30000))
        << qPrintable(screen()) << qPrintable(sshd.log(QStringLiteral("allow")));
    // ev.remote.host is the destination as afterglow ssh named it.
    EXPECT_TRUE(screen().contains(QStringLiteral("there remote allow %1 one").arg(pane.id()))) << qPrintable(screen());
    // The forward lived in the remote directory, which only the user can
    // enter, and only the user can connect to it.
    const QString remoteDirectory = sshd.remoteRun() + QStringLiteral("/afterglow");
    const auto mode = [](const QString& path) {
        struct stat info{};
        return ::lstat(QFile::encodeName(path).constData(), &info) == 0 ? int(info.st_mode) : -1;
    };
    EXPECT_EQ(mode(remoteDirectory), int(S_IFDIR | 0700));
    const QStringList sockets = QDir(remoteDirectory).entryList({QStringLiteral("*.sock")}, QDir::System);
    ASSERT_EQ(sockets.size(), 1);
    const QString forwardedSocket = remoteDirectory + QLatin1Char('/') + sockets.first();
    EXPECT_EQ(mode(forwardedSocket), int(S_IFSOCK | 0600));
    // The session cancels it at its end.
    EXPECT_TRUE(pumpUntil([&] { return m_remote->tokens().isEmpty(); }, 30000));
    EXPECT_TRUE(pumpUntil([&] { return connectError(forwardedSocket) == ECONNREFUSED; }));
#endif
}

int main(int argc, char** argv)
{
    // Test discovery runs this binary too, at build time and without the
    // test's environment: it must never reach the user's display or the
    // platform theme of the session.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
