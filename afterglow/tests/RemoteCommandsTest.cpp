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
#include "app/InstanceSocket.h"
#include "app/ScriptSettings.h"
#include "config/Settings.h"
#include "control/CommandSocket.h"
#include "control/Control.h"
#include "control/ControlSocket.h"
#include "control/RemoteSockets.h"
#include "scripting/ScriptControl.h"
#include "scripting/ScriptHost.h"

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
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>

#include <cstring>
#include <functional>
#include <memory>
#include <optional>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace Afterglow;
namespace Remote = AfterglowProto::Remote;

namespace {

const QByteArray Script = R"(
return {
    metadata = { commands = {
        here = { summary = "Local only" },
        there = { summary = "From remote hosts too", usage = "[WORD]", remote = true },
    } },
    run_command = function(command, ev)
        return afterglow.output(ev, command .. " " .. ev.source .. " " .. tostring(ev.remote and ev.remote.host)
            .. " " .. tostring(ev.pane and ev.pane.id) .. " " .. (ev.args[1] or "-") .. "\n")
    end,
}
)";

/// A fake ssh for the probe tests. It writes each run's words to
/// FAKE_SSH_LOG, separated by 0x1f, one run on each line, and acts as the
/// FAKE_* variables say: the probe's report, the exit status of -O check
/// and -O forward, and a session that waits for the file FAKE_RELEASE.
const QByteArray FakeSsh = R"(#!/bin/sh
{ for a in "$@"; do printf '%s\037' "$a"; done; printf '\n'; } >> "$FAKE_SSH_LOG"
for a in "$@"; do
    case "$a" in -G) printf 'host prod\nhostname prod.example\nuser me\nport 22\ncontrolpath none\n'; exit 0;; esac
done
op=; prev=; last=
for a in "$@"; do
    [ "$prev" = "-O" ] && op=$a
    prev=$a; last=$a
done
case "$op" in
    check) exit "${FAKE_CHECK:-0}";;
    forward) exit "${FAKE_FORWARD:-0}";;
    ?*) exit 0;;
esac
case "$last" in
    "sh -c "*)
        printf 'runtime=/run/user/1000\nhome=/home/me\n'
        case "${FAKE_AGLOW:-same}" in
            same) printf 'aglow=aglow 0.0.0 (protocol %s)\ndir=%s\n' "$FAKE_PROTOCOL" "${FAKE_DIR:-/run/user/1000/afterglow}";;
            other) printf 'aglow=aglow 9.9.9 (protocol 99)\ndir=%s\n' "${FAKE_DIR:-/run/user/1000/afterglow}";;
        esac
        exit 0;;
esac
: > "$FAKE_RELEASE.started"
while [ ! -e "$FAKE_RELEASE" ]; do sleep 0.02; done
exit 7
)";

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

/// Connecting to the Unix socket at @p path: 0, or the errno of the failure.
int connectError(const QString& path)
{
    const QByteArray name = QFile::encodeName(path);
    sockaddr_un address{};
    if (std::size_t(name.size()) >= sizeof address.sun_path) return ENAMETOOLONG;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, name.constData(), std::size_t(name.size()));
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
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
        m_remote.reset();
        m_controlSocket.reset();
        m_commands.reset();
        m_scripts.reset();
        m_instance.reset();
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
                                                   [this](quint32 pane, const Remote::Answer& answer) {
                                                       if (m_pane && pane == m_pane->id()) m_pane->setRemoteSocket(answer);
                                                       else if (pane != FakePane) return false;
                                                       m_answers.insert(pane, answer);
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
        process->start(QStringLiteral("sleep"), {QStringLiteral("60")});
        EXPECT_TRUE(process->waitForStarted());
        return process;
    }

    RemoteSockets::Request request(qint64 pid, const QString& directory = QStringLiteral("/run/user/1000/afterglow")) const
    {
        return {FakePane, pid, QStringLiteral("box"), directory, AfterglowProto::ProtocolVersion,
                {QStringLiteral("-F"), QStringLiteral("/cfg")}, QStringLiteral("prod")};
    }

    /// Calls @p fn of @p target on the socket at @p path. The answer, or
    /// the event frames and then the answer of a run.
    QList<QJsonObject> callOn(const QString& path, const QString& target, const QString& fn, const QJsonArray& args)
    {
        QLocalSocket client;
        client.connectToServer(path);
        EXPECT_TRUE(pumpUntil([&] { return client.state() == QLocalSocket::ConnectedState; }));
        const QJsonObject frame{{QStringLiteral("type"), QStringLiteral("call")}, {QStringLiteral("id"), 1},
                                {QStringLiteral("target"), target}, {QStringLiteral("fn"), fn}, {QStringLiteral("args"), args}};
        client.write(QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n');
        QList<QJsonObject> frames;
        pumpUntil([&] {
            while (client.canReadLine()) frames << QJsonDocument::fromJson(client.readLine()).object();
            return !frames.isEmpty() && frames.last().value(QStringLiteral("type")) != QLatin1String("event");
        });
        return frames;
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
        for (auto it = fake.cbegin(); it != fake.cend(); ++it) env.insert(it.key(), it.value());
        process->setProcessEnvironment(env);
        QFile::remove(release());
        QFile::remove(release() + QStringLiteral(".started"));
        QFile::remove(log());
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
    QTemporaryDir m_dir{QStringLiteral("/tmp/agrc-XXXXXX")};
    QString m_runtime;
    std::unique_ptr<SettingsStore> m_settings;
    std::unique_ptr<Control> m_control;
    std::unique_ptr<InstanceSocket> m_instance;
    std::unique_ptr<ScriptHost> m_scripts;
    std::unique_ptr<CommandSocket> m_commands;
    std::unique_ptr<ControlSocket> m_controlSocket;
    std::unique_ptr<RemoteSockets> m_remote;
    QHash<quint32, Remote::Answer> m_answers;
    Pane* m_pane = nullptr;
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
    EXPECT_EQ(m_answers.value(FakePane), (Remote::Answer{opened.path.toStdString(), Remote::Error::Off}));

    // No control call reaches the terminal through it.
    QJsonObject answer = callOn(opened.socket, QStringLiteral("control"), QStringLiteral("listPanes"), {}).value(0);
    EXPECT_EQ(answer.value(QStringLiteral("code")).toString(), QStringLiteral("NO_SUCH_TARGET"));
    answer = callOn(opened.socket, QStringLiteral("commands"), QStringLiteral("list"), {}).value(0);
    EXPECT_EQ(answer.value(QStringLiteral("code")).toString(), QStringLiteral("NO_SUCH_TARGET"));
    // The commands that declare remote, and no others.
    answer = callOn(opened.socket, QStringLiteral("remote"), QStringLiteral("list"), {}).value(0);
    const QJsonArray listed = answer.value(QStringLiteral("result")).toArray();
    ASSERT_EQ(listed.size(), 1);
    EXPECT_EQ(listed[0].toObject().value(QStringLiteral("command")).toString(), QStringLiteral("there"));
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
    QThread::msleep(200);
    EXPECT_TRUE(runsWith(QStringLiteral("cancel")).isEmpty());
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

    // 104 bytes and no more: the limit of the BSDs and macOS.
    const QString fits = QLatin1Char('/') + QString(104 - 1 - 1 - 16 - 5, QLatin1Char('d'));
    ASSERT_EQ(m_remote->open(request(watched->processId(), fits), opened), QString());
    EXPECT_TRUE(opened.error.isEmpty());
    EXPECT_EQ(opened.path.size(), 104);
    ASSERT_EQ(m_remote->open(request(watched->processId(), fits + QLatin1Char('d')), opened), QString());
    EXPECT_EQ(opened.error, QStringLiteral("length"));
    // The newest session of the pane answers.
    EXPECT_EQ(m_answers.value(FakePane), (Remote::Answer{{}, Remote::Error::Length}));
    // A directory that would break the reply or ssh's -R is refused.
    for (const QString& bad : {QStringLiteral("relative"), QStringLiteral("/a:b"), QStringLiteral("/a\nb")})
        EXPECT_FALSE(m_remote->open(request(watched->processId(), bad), opened).isEmpty()) << qPrintable(bad);
}

TEST_F(RemoteCommandsTest, TheSettingClosesTheSessionsAndRefusesNewOnes)
{
    ASSERT_TRUE(start());
    const auto watched = sleeper();
    RemoteSockets::Opened opened;
    ASSERT_EQ(m_remote->open(request(watched->processId()), opened), QString());
    ASSERT_TRUE(m_settings->write(QStringLiteral("Terminal"), QStringLiteral("sshRemoteCommands"), false));
    EXPECT_TRUE(m_remote->tokens().isEmpty());
    EXPECT_FALSE(exists(opened.socket));
    EXPECT_FALSE(m_remote->enabled());
    EXPECT_FALSE(m_remote->open(request(watched->processId()), opened).isEmpty());
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

    const auto checks = runsWith(QStringLiteral("-O"), QStringLiteral("check"));
    ASSERT_EQ(checks.size(), 1);
    EXPECT_TRUE(checks.first().contains(shared));
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
            && cancels.first().contains(shared) && !cancels.first().contains(QStringLiteral("-L"));
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

    // A requirement off: no probe at all.
    ASSERT_TRUE(m_settings->write(QStringLiteral("Terminal"), QStringLiteral("sshRemoteCommands"), false));
    process = helper({QStringLiteral("prod")});
    EXPECT_TRUE(runsWith(QStringLiteral("-T")).isEmpty());
    EXPECT_EQ(runs().size(), 2); // ssh -G and the session
    EXPECT_EQ(finish(*process), 7);
    ASSERT_TRUE(m_settings->write(QStringLiteral("Terminal"), QStringLiteral("sshRemoteCommands"), true));
    ASSERT_TRUE(m_settings->write(QStringLiteral("Control"), QStringLiteral("enabled"), false));
    process = helper({QStringLiteral("prod")});
    EXPECT_TRUE(runsWith(QStringLiteral("-T")).isEmpty());
    EXPECT_EQ(finish(*process), 7);
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
    QThread::msleep(200);
    EXPECT_TRUE(runsWith(QStringLiteral("cancel")).isEmpty());

    const QString longDirectory = QStringLiteral("/home/") + QString(90, QLatin1Char('d'));
    process = helper({QStringLiteral("prod")}, {{QStringLiteral("FAKE_DIR"), longDirectory}});
    EXPECT_EQ(m_answers.value(FakePane), (Remote::Answer{{}, Remote::Error::Length}));
    EXPECT_TRUE(runsWith(QStringLiteral("-O"), QStringLiteral("forward")).isEmpty());
    EXPECT_EQ(finish(*process), 7);
    EXPECT_TRUE(m_stderr.isEmpty()) << qPrintable(m_stderr);
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
        QDir().mkpath(m_dir);
        const QString keygen = QStandardPaths::findExecutable(QStringLiteral("ssh-keygen"));
        if (m_sshd.isEmpty() || keygen.isEmpty() || QStandardPaths::findExecutable(QStringLiteral("ssh")).isEmpty()) {
            m_missing = QStringLiteral("sshd, ssh or ssh-keygen is not installed");
            return;
        }
        for (const QString& key : {QStringLiteral("host_key"), QStringLiteral("client_key")}) {
            if (QProcess::execute(keygen, {QStringLiteral("-q"), QStringLiteral("-t"), QStringLiteral("ed25519"), QStringLiteral("-N"),
                                           QString(), QStringLiteral("-f"), path(key)})
                != 0) {
                m_missing = QStringLiteral("ssh-keygen failed");
                return;
            }
        }
        QFile::copy(path(QStringLiteral("client_key.pub")), path(QStringLiteral("authorized_keys")));
        for (const QString& dir : {QStringLiteral("remote-home"), QStringLiteral("remote-run"), QStringLiteral("c")}) {
            QDir().mkpath(path(dir));
            QFile::setPermissions(path(dir), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        }
        QFile hostKey(path(QStringLiteral("host_key.pub")));
        if (!hostKey.open(QIODevice::ReadOnly)) {
            m_missing = QStringLiteral("no host key");
            return;
        }
        const QByteArray publicKey = hostKey.readAll().trimmed();
        QByteArray knownHosts;
        QByteArray client = "IdentityFile " + path(QStringLiteral("client_key")).toUtf8() + "\n"
            "IdentitiesOnly yes\nIdentityAgent none\nUserKnownHostsFile " + path(QStringLiteral("known_hosts")).toUtf8() + "\n"
            "GlobalKnownHostsFile /dev/null\nStrictHostKeyChecking yes\nUpdateHostKeys no\nBatchMode yes\n"
            "ControlMaster auto\nControlPath " + path(QStringLiteral("c/%C")).toUtf8() + "\nControlPersist 30\n"
            "ForwardAgent no\nLogLevel ERROR\nUser " + qgetenv("USER") + "\n";
        for (const bool allow : {true, false}) {
            const int port = freePort();
            const QString name = allow ? QStringLiteral("allow") : QStringLiteral("refuse");
            QFile config(path(name + QStringLiteral(".sshd_config")));
            if (!config.open(QIODevice::WriteOnly)) {
                m_missing = QStringLiteral("cannot write the sshd config");
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
            process->setStandardOutputFile(path(name + QStringLiteral(".log")));
            process->setStandardErrorFile(path(name + QStringLiteral(".log")));
            process->start(m_sshd, {QStringLiteral("-D"), QStringLiteral("-e"), QStringLiteral("-f"), config.fileName()});
            if (!process->waitForStarted()) {
                m_missing = QStringLiteral("sshd did not start");
                return;
            }
            m_servers.push_back(std::move(process));
            knownHosts += "[127.0.0.1]:" + QByteArray::number(port) + " " + publicKey + "\n";
            client += "Host " + name.toUtf8() + "\n  HostName 127.0.0.1\n  Port " + QByteArray::number(port) + "\n";
            if (!waitForPort(port)) m_missing = QStringLiteral("sshd did not listen");
        }
        QFile known(path(QStringLiteral("known_hosts")));
        if (!known.open(QIODevice::WriteOnly)) m_missing = QStringLiteral("cannot write known_hosts");
        known.write(knownHosts);
        // Host-specific lines first: ssh takes the first value of each option.
        QFile config(clientConfig());
        if (!config.open(QIODevice::WriteOnly)) m_missing = QStringLiteral("cannot write the ssh config");
        const qsizetype hosts = client.indexOf("Host ");
        config.write(client.mid(hosts) + "Host *\n" + client.left(hosts));
    }

    ~Sshd()
    {
        for (const char* host : {"allow", "refuse"}) ssh({QStringLiteral("-O"), QStringLiteral("exit"), QLatin1String(host)});
        for (const auto& server : m_servers) {
            server->terminate();
            if (!server->waitForFinished(3000)) server->kill();
            server->waitForFinished();
        }
    }

    /// Why the test cannot run here, else empty.
    QString missing() const { return m_missing; }
    QString path(const QString& name) const { return m_dir + QLatin1Char('/') + name; }
    QString clientConfig() const { return path(QStringLiteral("ssh_config")); }
    QString remoteRun() const { return path(QStringLiteral("remote-run")); }

    struct Result
    {
        int status = -1;
        QString err;
    };
    /// ssh with the test's own config, and never an agent.
    Result ssh(const QStringList& args) const
    {
        QProcess process;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.remove(QStringLiteral("SSH_AUTH_SOCK"));
        process.setProcessEnvironment(env);
        process.setStandardInputFile(QProcess::nullDevice());
        process.start(QStringLiteral("ssh"), QStringList{QStringLiteral("-F"), clientConfig()} + args);
        process.waitForFinished(20000);
        return {process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1,
                QString::fromUtf8(process.readAllStandardError())};
    }

private:
    static int freePort()
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address);
        socklen_t length = sizeof address;
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length);
        ::close(fd);
        return ntohs(address.sin_port);
    }

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
    Pane pane{AfterglowVt::TerminalOptions{}};
    m_pane = &pane;
    AfterglowPty::SpawnOptions options;
    options.argv = {AFTERGLOW_BINARY, "ssh", "-F", sshd.clientConfig().toStdString(), "-t", "allow",
                    "aglow run far there one; aglow run far here; sleep 1"};
    options.workingDirectory = m_dir.path().toStdString();
    options.environment = {{"HOME", m_dir.filePath(QStringLiteral("home")).toStdString()}};
    options.unsetEnvironment = {"SSH_AUTH_SOCK", "TMUX"};
    QString error;
    ASSERT_TRUE(pane.start(options, QStringLiteral("xterm-256color"), 120, 30, 8, 16, &error)) << qPrintable(error);
    const auto screen = [&] { return pane.text(AfterglowVt::Terminal::TextScope::Screen); };
    const auto sshdLog = [&] {
        QFile file(sshd.path(QStringLiteral("allow.log")));
        return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
    };
    EXPECT_TRUE(pumpUntil([&] { return screen().contains(QLatin1String("no command here in far")); }, 30000))
        << qPrintable(screen()) << qPrintable(sshdLog());
    // ev.remote.host is the destination as afterglow ssh named it.
    EXPECT_TRUE(screen().contains(QStringLiteral("there remote allow %1 one").arg(pane.id()))) << qPrintable(screen());
    // The forward lived in the remote directory, and the session cancels it at its end.
    const QStringList sockets = QDir(sshd.remoteRun() + QStringLiteral("/afterglow")).entryList({QStringLiteral("*.sock")}, QDir::System);
    ASSERT_EQ(sockets.size(), 1);
    EXPECT_TRUE(pumpUntil([&] { return m_remote->tokens().isEmpty(); }, 30000));
    EXPECT_TRUE(pumpUntil([&] { return connectError(sshd.remoteRun() + QStringLiteral("/afterglow/") + sockets.first()) == ECONNREFUSED; }));
    m_pane = nullptr;
#endif
}

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
