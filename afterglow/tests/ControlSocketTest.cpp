// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "app/InstanceSocket.h"
#include "config/Settings.h"
#include "control/Control.h"
#include "control/ControlSocket.h"
#include "control/Ctl.h"
#include "control/IpcClient.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <functional>
#include <optional>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace Afterglow;

namespace {

/// An environment variable as it was, put back at the end.
struct Saved
{
    explicit Saved(const char* name)
        : name(name)
        , wasSet(qEnvironmentVariableIsSet(name))
        , value(qgetenv(name))
    {
    }
    ~Saved()
    {
        if (wasSet) {
            qputenv(name, value);
        } else {
            qunsetenv(name);
        }
    }
    const char* name;
    bool wasSet;
    QByteArray value;
};

class ControlSocketTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_dir.isValid());
        qputenv("XDG_CONFIG_HOME", m_dir.filePath(QStringLiteral("config")).toUtf8());
        qputenv("XDG_DATA_HOME", m_dir.filePath(QStringLiteral("data")).toUtf8());
        // The sockets and their locks go there: never in the user's own.
        m_runtime = m_dir.filePath(QStringLiteral("runtime"));
        ASSERT_TRUE(QDir().mkpath(m_runtime));
        qputenv("XDG_RUNTIME_DIR", m_runtime.toUtf8());
    }

    void TearDown() override
    {
        m_socket.reset();
        m_instance.reset();
    }

    bool start(const QByteArray& config = "{}")
    {
        const QString path = m_dir.filePath(QStringLiteral("config.json"));
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly)) return false;
        file.write(config);
        file.close();
        m_settings = std::make_unique<SettingsStore>(path);
        m_control = std::make_unique<Control>(*m_settings, [](const QString&, const QStringList&, bool) { return nullptr; });
        m_instance = std::make_unique<InstanceSocket>();
        if (m_instance->listen(socketPath()) != InstanceSocket::Listen::Listening) return false;
        m_socket = std::make_unique<ControlSocket>(*m_control, *m_settings, m_instance->router());
        return true;
    }

    QString socketPath() const { return m_runtime + QStringLiteral("/afterglow-test.sock"); }

    QTimer* dueTimer() const { return m_socket->findChild<QTimer*>(QStringLiteral("eventIntervalTimer")); }

    /// The server runs on this thread: wait with its event loop running.
    static bool waitFor(const std::function<bool()>& done, int timeoutMs = 3000)
    {
        QElapsedTimer clock;
        clock.start();
        while (!done()) {
            if (clock.elapsed() > timeoutMs) return false;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            QThread::msleep(1);
        }
        return true;
    }

    bool connect(QLocalSocket& client)
    {
        client.connectToServer(socketPath());
        return waitFor([&client] { return client.state() == QLocalSocket::ConnectedState; });
    }

    static void request(QLocalSocket& client, const QJsonObject& frame)
    {
        client.write(QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n');
    }

    static std::optional<QJsonObject> next(QLocalSocket& client, int timeoutMs = 3000)
    {
        if (!waitFor([&client] { return client.canReadLine(); }, timeoutMs)) return std::nullopt;
        return QJsonDocument::fromJson(client.readLine()).object();
    }

    QJsonObject call(QLocalSocket& client, const QString& fn, const QJsonArray& args = {})
    {
        const qint64 id = ++m_id;
        request(client, {{QStringLiteral("type"), QStringLiteral("call")}, {QStringLiteral("id"), id},
                         {QStringLiteral("target"), QStringLiteral("control")}, {QStringLiteral("fn"), fn},
                         {QStringLiteral("args"), args}});
        const auto frame = next(client);
        EXPECT_TRUE(frame.has_value()) << qPrintable(fn);
        return frame.value_or(QJsonObject{});
    }

    bool subscribe(QLocalSocket& client, const QString& event)
    {
        const qint64 id = ++m_id;
        request(client, {{QStringLiteral("type"), QStringLiteral("subscribe")}, {QStringLiteral("id"), id},
                         {QStringLiteral("target"), QStringLiteral("control")}, {QStringLiteral("signal"), event}});
        const auto frame = next(client);
        if (!frame || frame->value(QStringLiteral("type")).toString() != QLatin1String("reply")) return false;
        m_events.insert(id, event);
        return true;
    }

    /// The events as "name:arg1:arg2", until @p count arrived or @p ms
    /// passed. A count of 0 waits the whole time, to see that none come.
    QStringList events(QLocalSocket& client, int count, int ms = 3000)
    {
        QStringList out;
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < ms && (count == 0 || out.size() < count)) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            while ((count == 0 || out.size() < count) && client.canReadLine()) {
                const QJsonObject frame = QJsonDocument::fromJson(client.readLine()).object();
                QString line = m_events.value(frame.value(QStringLiteral("subscriptionId")).toInteger());
                for (const QJsonValue& arg : frame.value(QStringLiteral("args")).toArray()) {
                    line += QLatin1Char(':') + (arg.isString() ? arg.toString() : QString::number(arg.toInteger()));
                }
                out.append(line);
            }
            QThread::msleep(1);
        }
        return out;
    }

    Saved m_configHome{"XDG_CONFIG_HOME"};
    Saved m_dataHome{"XDG_DATA_HOME"};
    Saved m_runtimeDir{"XDG_RUNTIME_DIR"};
    Saved m_display{"WAYLAND_DISPLAY"};
    QTemporaryDir m_dir;
    QString m_runtime;
    std::unique_ptr<SettingsStore> m_settings;
    std::unique_ptr<Control> m_control;
    std::unique_ptr<InstanceSocket> m_instance;
    std::unique_ptr<ControlSocket> m_socket;
    qint64 m_id = 0;
    QHash<qint64, QString> m_events;
};

TEST_F(ControlSocketTest, CallsReplyWithResultsOrErrors)
{
    ASSERT_TRUE(start());
    ASSERT_TRUE(m_socket->enabled());
    EXPECT_EQ(InstanceSocket::processPath(), socketPath());
    // Only the owner can connect.
    EXPECT_EQ(QFileInfo(socketPath()).permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther),
              QFile::Permissions{});
    QLocalSocket client;
    ASSERT_TRUE(connect(client));

    const QJsonObject panes = call(client, QStringLiteral("listPanes"));
    EXPECT_EQ(panes.value(QStringLiteral("type")).toString(), QStringLiteral("reply"));
    EXPECT_TRUE(panes.value(QStringLiteral("result")).isArray());
    EXPECT_TRUE(panes.value(QStringLiteral("result")).toArray().isEmpty());

    // A call that fails gets an error reply with Control's message, and no result.
    const auto failure = [this, &client](const QString& fn, const QJsonArray& args) {
        const QJsonObject frame = call(client, fn, args);
        EXPECT_EQ(frame.value(QStringLiteral("type")).toString(), QStringLiteral("error")) << qPrintable(fn);
        EXPECT_EQ(frame.value(QStringLiteral("code")).toString(), QStringLiteral("CALL_FAILED")) << qPrintable(fn);
        EXPECT_FALSE(frame.contains(QStringLiteral("result"))) << qPrintable(fn);
        return frame.value(QStringLiteral("message")).toString();
    };
    EXPECT_EQ(failure(QStringLiteral("focusedPane"), {}), QStringLiteral("no focused pane"));
    EXPECT_TRUE(failure(QStringLiteral("sendText"), {7, QStringLiteral("x")}).contains(QLatin1Char('7')));
    EXPECT_TRUE(failure(QStringLiteral("getText"), {1, QStringLiteral("everything")}).startsWith(QStringLiteral("scope is")));
    // Reading pane text is off by default, and the check comes first.
    EXPECT_TRUE(failure(QStringLiteral("runAction"), {1, QStringLiteral("copy")}).contains(QStringLiteral("Control.allowReadText")));
    // A pane ID that a conversion would wrap or round onto a real pane.
    for (const double bad : {-1.0, 4294967297.0, 1.5}) {
        EXPECT_TRUE(failure(QStringLiteral("close"), {bad}).startsWith(QStringLiteral("a pane ID is a whole number"))) << bad;
    }
    EXPECT_EQ(failure(QStringLiteral("close"), {4294967295.0}), QStringLiteral("no pane 4294967295"));

    // A call that succeeds after a failed one replies normally.
    const QJsonObject projects = call(client, QStringLiteral("projects"));
    EXPECT_EQ(projects.value(QStringLiteral("type")).toString(), QStringLiteral("reply"));
    EXPECT_TRUE(projects.value(QStringLiteral("result")).isArray());

    // Only the API is callable, not the QObject's own slots.
    const QJsonObject unknown = call(client, QStringLiteral("deleteLater"));
    EXPECT_EQ(unknown.value(QStringLiteral("type")).toString(), QStringLiteral("error"));
    EXPECT_EQ(unknown.value(QStringLiteral("code")).toString(), QStringLiteral("NO_SUCH_FN"));
}

TEST_F(ControlSocketTest, OverlayChecksItsArgumentsThenFailsWithCodesOfItsOwn)
{
    ASSERT_TRUE(start());
    QLocalSocket client;
    ASSERT_TRUE(connect(client));
    // The rules of the overlay effect, and a directory that exists: a
    // refusal before anything runs.
    const auto refused = [this, &client](const QJsonArray& args) {
        const QJsonObject frame = call(client, QStringLiteral("overlay"), args);
        EXPECT_EQ(frame.value(QStringLiteral("type")).toString(), QStringLiteral("error"));
        EXPECT_EQ(frame.value(QStringLiteral("code")).toString(), QStringLiteral("CALL_FAILED"));
        return frame.value(QStringLiteral("message")).toString();
    };
    EXPECT_TRUE(refused({7, QJsonArray{}, QJsonObject{}}).contains(QStringLiteral("argv")));
    EXPECT_TRUE(refused({7, QJsonArray{QStringLiteral("fzf")}, QJsonObject{{QStringLiteral("capture"), QStringLiteral("stderr")}}})
                    .startsWith(QStringLiteral("capture must be")));
    EXPECT_TRUE(refused({7, QJsonArray{QStringLiteral("fzf")}, QJsonObject{{QStringLiteral("shell"), true}}})
                    .contains(QStringLiteral("shell")));
    const QString missingDirectory = m_dir.filePath(QStringLiteral("missing"));
    EXPECT_EQ(refused({7, QJsonArray{QStringLiteral("fzf")}, QJsonObject{{QStringLiteral("cwd"), missingDirectory}}}),
              QStringLiteral("cwd must name an existing absolute directory"));
    // Qt takes a resource path for absolute, and its root for a directory:
    // the spec refuses it before the directory check.
    EXPECT_EQ(refused({7, QJsonArray{QStringLiteral("fzf")}, QJsonObject{{QStringLiteral("cwd"), QStringLiteral(":/")}}}),
              QStringLiteral("cwd must be an absolute path, ~ or a path that starts with ~/"));
    // Past the checks, a failure has its own code: here no pane, also for
    // the focused pane when there is none. ~ is the home directory, which
    // exists.
    const auto noPane = [this, &client](const QJsonArray& args) {
        const QJsonObject frame = call(client, QStringLiteral("overlay"), args);
        EXPECT_EQ(frame.value(QStringLiteral("type")).toString(), QStringLiteral("error"));
        EXPECT_EQ(frame.value(QStringLiteral("code")).toString(), QStringLiteral("no_pane"));
        return frame.value(QStringLiteral("message")).toString();
    };
    EXPECT_TRUE(noPane({7, QJsonArray{QStringLiteral("fzf")}, QJsonObject{}}).contains(QLatin1Char('7')));
    EXPECT_TRUE(noPane({7, QJsonArray{QStringLiteral("fzf")}, QJsonObject{{QStringLiteral("cwd"), QStringLiteral("~")}}})
                    .contains(QLatin1Char('7')));
    EXPECT_EQ(noPane({0, QJsonArray{QStringLiteral("fzf")}, QJsonObject{}}), QStringLiteral("no focused pane"));
}

/// Runs @p work on a thread of its own while this thread's event loop
/// serves the socket: the client blocks.
template<typename Work>
bool runBlocking(Work work, int timeoutMs = 20000)
{
    std::unique_ptr<QThread> thread(QThread::create(std::move(work)));
    thread->start();
    QElapsedTimer clock;
    clock.start();
    while (!thread->isFinished() && clock.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        QThread::msleep(1);
    }
    if (thread->wait(QDeadlineTimer(1000))) return true;
    // A hung client must fail this test, not abort the binary: a QThread
    // destroyed while it runs ends the process, and the work holds
    // references to the caller's locals.
    thread->terminate();
    thread->wait();
    return false;
}

TEST_F(ControlSocketTest, ClientCallsAndReadsTheServersErrors)
{
    ASSERT_TRUE(start());
    struct Outcome
    {
        bool connected = false;
        pid_t peer = 0;
        std::optional<QJsonValue> projects, part, tooLong, after;
        IpcClient::Error partError, tooLongError;
    } outcome;
    ASSERT_TRUE(runBlocking([&] {
        IpcClient client;
        IpcClient::Error error;
        outcome.connected = client.connectTo(socketPath(), 3000, &error);
        if (!outcome.connected) return;
        outcome.peer = client.peerPid();
        outcome.projects = client.call(QStringLiteral("control"), QStringLiteral("projects"), {}, 3000, &error);
        // The most text that a send-text part holds, each unit a control
        // character that JSON writes as six bytes, fits in a request.
        const QString part(CtlTextPart, QChar(0x01));
        outcome.part = client.call(QStringLiteral("control"), QStringLiteral("sendText"), {1, part}, 3000, &outcome.partError);
        // A request over the router's line limit is not sent: the connection
        // stays, where the router would have closed it.
        const QString tooLong(IpcClient::MaxLineBytes / 4, QChar(0x01));
        outcome.tooLong =
            client.call(QStringLiteral("control"), QStringLiteral("sendText"), {1, tooLong}, 3000, &outcome.tooLongError);
        outcome.after = client.call(QStringLiteral("control"), QStringLiteral("projects"), {}, 3000, &error);
    }));
    ASSERT_TRUE(outcome.connected);
    EXPECT_EQ(outcome.peer, getpid());
    ASSERT_TRUE(outcome.projects.has_value());
    EXPECT_TRUE(outcome.projects->isArray());
    EXPECT_FALSE(outcome.part.has_value());
    EXPECT_EQ(outcome.partError.code, QStringLiteral("CALL_FAILED")) << qPrintable(outcome.partError.message);
    EXPECT_FALSE(outcome.tooLong.has_value());
    EXPECT_TRUE(outcome.tooLongError.tooLong);
    EXPECT_TRUE(outcome.after.has_value());
}

TEST_F(ControlSocketTest, ClientTakesAnErrorWithNoIdAsTheAnswer)
{
    // A server that could not read a line answers without an id, and keeps
    // the connection: the call ends with that error, not at its timeout.
    QLocalServer server;
    const QString path = m_runtime + QStringLiteral("/fake.sock");
    ASSERT_TRUE(server.listen(path));
    QObject::connect(&server, &QLocalServer::newConnection, &server, [&server] {
        QLocalSocket* peer = server.nextPendingConnection();
        QObject::connect(peer, &QLocalSocket::readyRead, peer, [peer] {
            while (peer->canReadLine()) {
                peer->readLine();
                peer->write(R"({"type":"error","code":"MALFORMED_REQUEST","message":"unreadable"})" "\n");
            }
        });
    });
    std::optional<QJsonValue> result;
    IpcClient::Error error;
    qint64 took = 0;
    ASSERT_TRUE(runBlocking([&] {
        IpcClient client;
        if (!client.connectTo(path, 3000, &error)) return;
        QElapsedTimer clock;
        clock.start();
        result = client.call(QStringLiteral("control"), QStringLiteral("projects"), {}, 5000, &error);
        took = clock.elapsed();
    }));
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(error.code, QStringLiteral("MALFORMED_REQUEST"));
    EXPECT_EQ(error.message, QStringLiteral("unreadable"));
    EXPECT_FALSE(error.timedOut);
    EXPECT_LT(took, 4000);
}

TEST_F(ControlSocketTest, FollowsControlEnabled)
{
    ASSERT_TRUE(start(R"({"Control": {"enabled": false}})"));
    // The socket stays open for launches. Only the control target goes.
    EXPECT_FALSE(m_socket->enabled());
    EXPECT_TRUE(QFileInfo::exists(socketPath()));
    QLocalSocket client;
    ASSERT_TRUE(connect(client));
    QJsonObject off = call(client, QStringLiteral("listPanes"));
    EXPECT_EQ(off.value(QStringLiteral("code")).toString(), QStringLiteral("NO_SUCH_TARGET"));

    m_settings->write(QStringLiteral("Control"), QStringLiteral("enabled"), true);
    ASSERT_TRUE(m_socket->enabled());
    EXPECT_EQ(call(client, QStringLiteral("listPanes")).value(QStringLiteral("type")).toString(), QStringLiteral("reply"));
    ASSERT_TRUE(subscribe(client, QStringLiteral("titleChanged")));
    const qint64 subscription = m_id;

    // Turning control off ends the subscription, and the client hears it.
    m_settings->write(QStringLiteral("Control"), QStringLiteral("enabled"), false);
    EXPECT_FALSE(m_socket->enabled());
    const auto ended = next(client);
    ASSERT_TRUE(ended.has_value());
    EXPECT_EQ(ended->value(QStringLiteral("type")).toString(), QStringLiteral("error"));
    EXPECT_EQ(ended->value(QStringLiteral("id")).toInteger(), subscription);
    EXPECT_EQ(ended->value(QStringLiteral("code")).toString(), QStringLiteral("NO_SUCH_TARGET"));
    off = call(client, QStringLiteral("listPanes"));
    EXPECT_EQ(off.value(QStringLiteral("code")).toString(), QStringLiteral("NO_SUCH_TARGET"));

    // Back on, the old subscription stays ended: no event without a new one.
    m_settings->write(QStringLiteral("Control"), QStringLiteral("enabled"), true);
    Q_EMIT m_control->titleChanged(1, QStringLiteral("after"));
    EXPECT_TRUE(events(client, 0, 100).isEmpty());
}

TEST_F(ControlSocketTest, OneProcessListensOnAPath)
{
    ASSERT_TRUE(start());
    // Another process (here another socket) cannot take a held path.
    InstanceSocket second;
    EXPECT_EQ(second.listen(socketPath()), InstanceSocket::Listen::Held);
    EXPECT_FALSE(second.listening());
    EXPECT_EQ(InstanceSocket::processPath(), socketPath());
    // Gone, its file and its lock go with it, and the path is free.
    m_socket.reset();
    m_instance.reset();
    EXPECT_FALSE(QFileInfo::exists(socketPath()));
    EXPECT_FALSE(QFileInfo::exists(socketPath() + QStringLiteral(".lock")));
    EXPECT_TRUE(InstanceSocket::processPath().isEmpty());
    EXPECT_EQ(second.listen(socketPath()), InstanceSocket::Listen::Listening);
}

/// A socket file with no listener, as a process that died leaves it.
bool makeStaleSocket(const QString& path)
{
    const QByteArray name = QFile::encodeName(path);
    sockaddr_un address{};
    if (std::size_t(name.size()) >= sizeof(address.sun_path)) return false;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, name.constData(), std::size_t(name.size()));
    const bool bound = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    ::close(fd);
    return bound;
}

TEST_F(ControlSocketTest, TakesOverThePathOfACrashedProcess)
{
    // A crash leaves the socket and frees the lock: the next process gets the
    // path without a probe that two launches could both pass.
    ASSERT_TRUE(makeStaleSocket(socketPath()));
    ASSERT_TRUE(start());
    QLocalSocket client;
    EXPECT_TRUE(connect(client));
    // A live listener that holds no lock (another build) keeps its path.
    const QString other = m_runtime + QStringLiteral("/afterglow-other.sock");
    QLocalServer server;
    ASSERT_TRUE(server.listen(other));
    InstanceSocket second;
    EXPECT_EQ(second.listen(other), InstanceSocket::Listen::Held);
    EXPECT_TRUE(server.isListening());
}

TEST_F(ControlSocketTest, AHeldLockKeepsAStaleSocketFromTheNextLaunch)
{
    // Between its lock and its bind, a launch holds the path although the
    // file there still refuses connections: another launch must neither
    // take the path nor remove the file.
    ASSERT_TRUE(makeStaleSocket(socketPath()));
    const int held = ::open(QFile::encodeName(socketPath() + QStringLiteral(".lock")).constData(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    ASSERT_GE(held, 0);
    ASSERT_EQ(::flock(held, LOCK_EX | LOCK_NB), 0);
    InstanceSocket second;
    EXPECT_EQ(second.listen(socketPath()), InstanceSocket::Listen::Held);
    InstanceSocket::removeStaleSockets();
    EXPECT_TRUE(QFileInfo::exists(socketPath()));
    ::close(held);
    // Its holder gone, the path is free.
    EXPECT_EQ(second.listen(socketPath()), InstanceSocket::Listen::Listening);
}

TEST_F(ControlSocketTest, RemovesDeadSocketsOfOtherProcesses)
{
    const QString dead = m_runtime + QStringLiteral("/afterglow-wayland-9-4242.sock");
    const QString other = m_runtime + QStringLiteral("/other.sock");
    const QString file = m_runtime + QStringLiteral("/afterglow-notes.sock");
    const QString live = m_runtime + QStringLiteral("/afterglow-wayland-9.sock");
    const QString lockOnly = m_runtime + QStringLiteral("/afterglow-gone.sock.lock");
    ASSERT_TRUE(makeStaleSocket(dead));
    ASSERT_TRUE(makeStaleSocket(other));
    QFile note(file);
    ASSERT_TRUE(note.open(QIODevice::WriteOnly));
    note.close();
    QFile lock(lockOnly);
    ASSERT_TRUE(lock.open(QIODevice::WriteOnly));
    lock.close();
    QLocalServer server;
    ASSERT_TRUE(server.listen(live));
    // A process that listens holds its lock: the sweep leaves it alone.
    ASSERT_TRUE(start());

    InstanceSocket::removeStaleSockets();
    EXPECT_FALSE(QFileInfo::exists(dead));
    EXPECT_FALSE(QFileInfo::exists(lockOnly));
    // Not Afterglow's, not a socket, or alive.
    EXPECT_TRUE(QFileInfo::exists(other));
    EXPECT_TRUE(QFileInfo::exists(file));
    EXPECT_TRUE(QFileInfo::exists(live));
    EXPECT_TRUE(server.isListening());
    EXPECT_TRUE(QFileInfo::exists(socketPath()));
    EXPECT_TRUE(QFileInfo::exists(socketPath() + QStringLiteral(".lock")));
}

TEST_F(ControlSocketTest, SocketPathsFollowTheDisplay)
{
    const QString pid = QString::number(QCoreApplication::applicationPid());
    qputenv("WAYLAND_DISPLAY", "wayland-1");
    EXPECT_EQ(InstanceSocket::sharedPath(), m_runtime + QStringLiteral("/afterglow-wayland-1.sock"));
    EXPECT_EQ(InstanceSocket::ownPath(), m_runtime + QStringLiteral("/afterglow-wayland-1-%1.sock").arg(pid));
    // Two absolute displays with the same name are two displays.
    qputenv("WAYLAND_DISPLAY", "/tmp/nested/wayland-0");
    EXPECT_EQ(InstanceSocket::sharedPath(), m_runtime + QStringLiteral("/afterglow-tmp_nested_wayland-0.sock"));
    qputenv("WAYLAND_DISPLAY", "a b:c");
    EXPECT_EQ(InstanceSocket::sharedPath(), m_runtime + QStringLiteral("/afterglow-a_b_c.sock"));
    qunsetenv("WAYLAND_DISPLAY");
    EXPECT_EQ(InstanceSocket::sharedPath(), m_runtime + QStringLiteral("/afterglow-wayland-0.sock"));
    qputenv("XDG_RUNTIME_DIR", QByteArray());
    EXPECT_TRUE(InstanceSocket::sharedPath().isEmpty());
    EXPECT_TRUE(InstanceSocket::ownPath().isEmpty());
}

TEST_F(ControlSocketTest, MergesFastStateEventsOfAPane)
{
    ASSERT_TRUE(start(R"({"Control": {"eventInterval": 400}})"));
    QLocalSocket client;
    ASSERT_TRUE(connect(client));
    ASSERT_TRUE(subscribe(client, QStringLiteral("titleChanged")));
    ASSERT_TRUE(subscribe(client, QStringLiteral("bell")));

    // The first goes at once. The rest of the interval merges into the newest.
    Q_EMIT m_control->titleChanged(1, QStringLiteral("a"));
    Q_EMIT m_control->titleChanged(1, QStringLiteral("b"));
    Q_EMIT m_control->titleChanged(1, QStringLiteral("c"));
    // Another pane has its own interval.
    Q_EMIT m_control->titleChanged(2, QStringLiteral("x"));
    for (int i = 0; i < 50; ++i) Q_EMIT m_control->bell(1);
    EXPECT_EQ(events(client, 3), (QStringList{QStringLiteral("titleChanged:1:a"), QStringLiteral("titleChanged:2:x"),
                                             QStringLiteral("bell:1")}));
    // Events that wait for the same moment have no order between them.
    QStringList merged = events(client, 2);
    merged.sort();
    EXPECT_EQ(merged, (QStringList{QStringLiteral("bell:1"), QStringLiteral("titleChanged:1:c")}));

    // With nothing left to send, the timer stops.
    QTimer* due = dueTimer();
    ASSERT_NE(due, nullptr);
    EXPECT_TRUE(waitFor([due] { return !due->isActive(); }, 2000));
    // An event after a quiet interval goes at once again.
    Q_EMIT m_control->titleChanged(1, QStringLiteral("d"));
    EXPECT_EQ(events(client, 1), QStringList{QStringLiteral("titleChanged:1:d")});
}

TEST_F(ControlSocketTest, WhatWaitsGoesBeforeTheNextEventOfThePane)
{
    ASSERT_TRUE(start(R"({"Control": {"eventInterval": 5000}})"));
    QLocalSocket client;
    ASSERT_TRUE(connect(client));
    for (const char* event : {"titleChanged", "userVarChanged", "commandFinished", "paneClosed"}) {
        ASSERT_TRUE(subscribe(client, QLatin1String(event)));
    }
    Q_EMIT m_control->titleChanged(3, QStringLiteral("old"));
    Q_EMIT m_control->titleChanged(3, QStringLiteral("last"));
    // A client that reads the state at the end of a command sees it current.
    Q_EMIT m_control->commandFinished(3, QStringLiteral("make"), 0, 10);
    EXPECT_EQ(events(client, 3), (QStringList{QStringLiteral("titleChanged:3:old"), QStringLiteral("titleChanged:3:last"),
                                             QStringLiteral("commandFinished:3::0:10")}));
    // The title that went out still counts for the interval.
    Q_EMIT m_control->titleChanged(3, QStringLiteral("next"));
    EXPECT_TRUE(events(client, 0, 100).isEmpty());

    // User variables merge by name: a waits with its newest value, b is new
    // and goes at once.
    Q_EMIT m_control->userVarChanged(3, QStringLiteral("a"), QStringLiteral("1"));
    Q_EMIT m_control->userVarChanged(3, QStringLiteral("a"), QStringLiteral("2"));
    Q_EMIT m_control->userVarChanged(3, QStringLiteral("b"), QStringLiteral("3"));
    // Values stay empty without Control.allowReadText.
    EXPECT_EQ(events(client, 2), (QStringList{QStringLiteral("userVarChanged:3:a:"), QStringLiteral("userVarChanged:3:b:")}));
    Q_EMIT m_control->paneClosed(3);
    QStringList closing = events(client, 3);
    ASSERT_EQ(closing.size(), 3) << qPrintable(closing.join(QLatin1Char(' ')));
    EXPECT_EQ(closing.last(), QStringLiteral("paneClosed:3"));
    closing.removeLast();
    closing.sort();
    EXPECT_EQ(closing, (QStringList{QStringLiteral("titleChanged:3:next"), QStringLiteral("userVarChanged:3:a:")}));
    EXPECT_FALSE(dueTimer()->isActive());
}

TEST_F(ControlSocketTest, ReadAccessCountsWhenTheValueGoesOut)
{
    ASSERT_TRUE(start(R"({"Control": {"eventInterval": 5000, "allowReadText": true}})"));
    QLocalSocket client;
    ASSERT_TRUE(connect(client));
    ASSERT_TRUE(subscribe(client, QStringLiteral("userVarChanged")));
    ASSERT_TRUE(subscribe(client, QStringLiteral("paneClosed")));
    Q_EMIT m_control->userVarChanged(1, QStringLiteral("token"), QStringLiteral("first"));
    EXPECT_EQ(events(client, 1), QStringList{QStringLiteral("userVarChanged:1:token:first")});
    Q_EMIT m_control->userVarChanged(1, QStringLiteral("token"), QStringLiteral("secret"));
    // The value waits. The user takes read access away before it goes.
    m_settings->write(QStringLiteral("Control"), QStringLiteral("allowReadText"), false);
    Q_EMIT m_control->paneClosed(1);
    EXPECT_EQ(events(client, 2), (QStringList{QStringLiteral("userVarChanged:1:token:"), QStringLiteral("paneClosed:1")}));
}

TEST_F(ControlSocketTest, ZeroIntervalSendsEveryChangeAndCommandsFollowReadAccess)
{
    ASSERT_TRUE(start(R"({"Control": {"eventInterval": 0, "allowReadText": true}})"));
    QLocalSocket client;
    ASSERT_TRUE(connect(client));
    ASSERT_TRUE(subscribe(client, QStringLiteral("titleChanged")));
    ASSERT_TRUE(subscribe(client, QStringLiteral("commandStarted")));

    Q_EMIT m_control->titleChanged(1, QStringLiteral("a"));
    Q_EMIT m_control->titleChanged(1, QStringLiteral("b"));
    Q_EMIT m_control->commandStarted(1, QStringLiteral("make"));
    m_settings->write(QStringLiteral("Control"), QStringLiteral("allowReadText"), false);
    Q_EMIT m_control->commandStarted(1, QStringLiteral("secret"));
    EXPECT_EQ(events(client, 4), (QStringList{QStringLiteral("titleChanged:1:a"), QStringLiteral("titleChanged:1:b"),
                                             QStringLiteral("commandStarted:1:make"), QStringLiteral("commandStarted:1:")}));
}

TEST_F(ControlSocketTest, IntervalTurnedToZeroSendsWhatWaitsFirst)
{
    ASSERT_TRUE(start(R"({"Control": {"eventInterval": 5000}})"));
    QLocalSocket client;
    ASSERT_TRUE(connect(client));
    ASSERT_TRUE(subscribe(client, QStringLiteral("titleChanged")));
    Q_EMIT m_control->titleChanged(1, QStringLiteral("a"));
    Q_EMIT m_control->titleChanged(1, QStringLiteral("old"));
    m_settings->write(QStringLiteral("Control"), QStringLiteral("eventInterval"), 0);
    Q_EMIT m_control->titleChanged(1, QStringLiteral("new"));
    // The client ends on the newest title.
    EXPECT_EQ(events(client, 3), (QStringList{QStringLiteral("titleChanged:1:a"), QStringLiteral("titleChanged:1:old"),
                                             QStringLiteral("titleChanged:1:new")}));
    EXPECT_TRUE(events(client, 0, 100).isEmpty());
    EXPECT_FALSE(dueTimer()->isActive());
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
