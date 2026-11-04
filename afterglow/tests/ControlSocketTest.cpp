// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "config/Settings.h"
#include "control/Control.h"
#include "control/ControlSocket.h"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QDir>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <optional>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace Afterglow;

namespace {

class ControlSocketTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_dir.isValid());
        qputenv("XDG_CONFIG_HOME", m_dir.filePath(QStringLiteral("config")).toUtf8());
        qputenv("XDG_DATA_HOME", m_dir.filePath(QStringLiteral("data")).toUtf8());
        // The socket looks for dead sockets there: never in the user's own.
        m_runtime = m_dir.filePath(QStringLiteral("runtime"));
        ASSERT_TRUE(QDir().mkpath(m_runtime));
        m_previousRuntime = qgetenv("XDG_RUNTIME_DIR");
        qputenv("XDG_RUNTIME_DIR", m_runtime.toUtf8());
    }

    void TearDown() override
    {
        m_socket.reset();
        qputenv("XDG_RUNTIME_DIR", m_previousRuntime);
    }

    void start(const QByteArray& config = "{}")
    {
        const QString path = m_dir.filePath(QStringLiteral("config.json"));
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(config);
        file.close();
        m_settings = std::make_unique<SettingsStore>(path);
        m_control = std::make_unique<Control>(*m_settings, [](const QString&, const QStringList&, bool) { return nullptr; });
        m_socket = std::make_unique<ControlSocket>(*m_control, *m_settings, socketPath());
    }

    QString socketPath() const { return m_dir.filePath(QStringLiteral("control.sock")); }

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

    void subscribe(QLocalSocket& client, const QString& event)
    {
        const qint64 id = ++m_id;
        request(client, {{QStringLiteral("type"), QStringLiteral("subscribe")}, {QStringLiteral("id"), id},
                         {QStringLiteral("target"), QStringLiteral("control")}, {QStringLiteral("signal"), event}});
        const auto frame = next(client);
        ASSERT_TRUE(frame.has_value());
        ASSERT_EQ(frame->value(QStringLiteral("type")).toString(), QStringLiteral("reply")) << qPrintable(event);
        m_events.insert(id, event);
    }

    /// The events that arrive within @p ms, as "name:arg1:arg2".
    QStringList events(QLocalSocket& client, int ms)
    {
        QStringList out;
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < ms) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
            while (client.canReadLine()) {
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

    QTemporaryDir m_dir;
    QString m_runtime;
    QByteArray m_previousRuntime;
    std::unique_ptr<SettingsStore> m_settings;
    std::unique_ptr<Control> m_control;
    std::unique_ptr<ControlSocket> m_socket;
    qint64 m_id = 0;
    QHash<qint64, QString> m_events;
};

TEST_F(ControlSocketTest, CallsReplyWithResultsOrErrors)
{
    start();
    ASSERT_TRUE(m_socket->listening());
    EXPECT_EQ(ControlSocket::processPath(), socketPath());
    // Only the owner can connect.
    EXPECT_EQ(QFileInfo(socketPath()).permissions() & (QFile::ReadGroup | QFile::WriteGroup | QFile::ReadOther | QFile::WriteOther),
              QFile::Permissions{});
    QLocalSocket client;
    ASSERT_TRUE(connect(client));

    QJsonObject panes = call(client, QStringLiteral("listPanes"));
    EXPECT_EQ(panes.value(QStringLiteral("type")).toString(), QStringLiteral("reply"));
    EXPECT_TRUE(panes.value(QStringLiteral("result")).toObject().value(QStringLiteral("panes")).toArray().isEmpty());

    const QJsonObject focused = call(client, QStringLiteral("focusedPane"));
    EXPECT_EQ(focused.value(QStringLiteral("result")).toObject().value(QStringLiteral("error")).toString(),
              QStringLiteral("no focused pane"));

    const QJsonObject text = call(client, QStringLiteral("sendText"), {7, QStringLiteral("x")});
    EXPECT_TRUE(text.value(QStringLiteral("result")).toObject().value(QStringLiteral("error")).toString().contains(QLatin1Char('7')));

    const QJsonObject scope = call(client, QStringLiteral("getText"), {1, QStringLiteral("everything")});
    EXPECT_TRUE(scope.value(QStringLiteral("result")).toObject().value(QStringLiteral("error")).toString().startsWith(
        QStringLiteral("scope is")));

    // Reading pane text is off by default, and the check comes first.
    const QJsonObject copy = call(client, QStringLiteral("runAction"), {1, QStringLiteral("copy")});
    EXPECT_TRUE(copy.value(QStringLiteral("result")).toObject().value(QStringLiteral("error")).toString().contains(
        QStringLiteral("Control.allowReadText")));

    const QJsonObject projects = call(client, QStringLiteral("projects"));
    EXPECT_TRUE(projects.value(QStringLiteral("result")).toObject().value(QStringLiteral("projects")).isArray());

    // Only the API is callable, not the QObject's own slots.
    const QJsonObject unknown = call(client, QStringLiteral("deleteLater"));
    EXPECT_EQ(unknown.value(QStringLiteral("type")).toString(), QStringLiteral("error"));
    EXPECT_EQ(unknown.value(QStringLiteral("code")).toString(), QStringLiteral("NO_SUCH_FN"));
}

TEST_F(ControlSocketTest, FollowsControlEnabled)
{
    start(R"({"Control": {"enabled": false}})");
    EXPECT_FALSE(m_socket->listening());
    EXPECT_FALSE(QFileInfo::exists(socketPath()));
    // Panes still name this process, so that afterglow ctl in them fails
    // instead of reaching another one.
    EXPECT_EQ(ControlSocket::processPath(), socketPath());

    m_settings->write(QStringLiteral("Control"), QStringLiteral("enabled"), true);
    ASSERT_TRUE(m_socket->listening());
    QLocalSocket client;
    ASSERT_TRUE(connect(client));

    m_settings->write(QStringLiteral("Control"), QStringLiteral("enabled"), false);
    EXPECT_FALSE(m_socket->listening());
    EXPECT_FALSE(QFileInfo::exists(socketPath()));
    EXPECT_TRUE(waitFor([&client] { return client.state() == QLocalSocket::UnconnectedState; }));
}

TEST_F(ControlSocketTest, MergesFastStateEventsOfAPane)
{
    start(R"({"Control": {"eventInterval": 400}})");
    QLocalSocket client;
    ASSERT_TRUE(connect(client));
    subscribe(client, QStringLiteral("titleChanged"));
    subscribe(client, QStringLiteral("bell"));

    // The first goes at once. The rest of the interval merges into the newest.
    Q_EMIT m_control->titleChanged(1, QStringLiteral("a"));
    Q_EMIT m_control->titleChanged(1, QStringLiteral("b"));
    Q_EMIT m_control->titleChanged(1, QStringLiteral("c"));
    // Another pane has its own interval.
    Q_EMIT m_control->titleChanged(2, QStringLiteral("x"));
    for (int i = 0; i < 50; ++i) Q_EMIT m_control->bell(1);
    EXPECT_EQ(events(client, 100), (QStringList{QStringLiteral("titleChanged:1:a"), QStringLiteral("titleChanged:2:x"),
                                               QStringLiteral("bell:1")}));
    // Events that wait for the same moment have no order between them.
    QStringList merged = events(client, 600);
    merged.sort();
    EXPECT_EQ(merged, (QStringList{QStringLiteral("bell:1"), QStringLiteral("titleChanged:1:c")}));

    // With nothing left to send, the timer stops.
    QTimer* due = m_socket->findChild<QTimer*>();
    ASSERT_NE(due, nullptr);
    EXPECT_TRUE(waitFor([due] { return !due->isActive(); }, 2000));
    // An event after a quiet interval goes at once again.
    Q_EMIT m_control->titleChanged(1, QStringLiteral("d"));
    EXPECT_EQ(events(client, 100), QStringList{QStringLiteral("titleChanged:1:d")});
}

TEST_F(ControlSocketTest, ClosingPaneSendsWhatWaitsFirst)
{
    start(R"({"Control": {"eventInterval": 5000}})");
    QLocalSocket client;
    ASSERT_TRUE(connect(client));
    subscribe(client, QStringLiteral("titleChanged"));
    subscribe(client, QStringLiteral("userVarChanged"));
    subscribe(client, QStringLiteral("paneClosed"));

    Q_EMIT m_control->titleChanged(3, QStringLiteral("old"));
    Q_EMIT m_control->titleChanged(3, QStringLiteral("last"));
    // User variables merge by name.
    Q_EMIT m_control->userVarChanged(3, QStringLiteral("a"), QStringLiteral("1"));
    Q_EMIT m_control->userVarChanged(3, QStringLiteral("b"), QStringLiteral("2"));
    Q_EMIT m_control->paneClosed(3);
    const QStringList got = events(client, 100);
    ASSERT_EQ(got.size(), 5) << qPrintable(got.join(QLatin1Char(' ')));
    EXPECT_EQ(got[0], QStringLiteral("titleChanged:3:old"));
    // Values stay empty without Control.allowReadText.
    EXPECT_TRUE(got.contains(QStringLiteral("userVarChanged:3:a:")));
    EXPECT_TRUE(got.contains(QStringLiteral("userVarChanged:3:b:")));
    EXPECT_TRUE(got.contains(QStringLiteral("titleChanged:3:last")));
    EXPECT_EQ(got.last(), QStringLiteral("paneClosed:3"));
    EXPECT_FALSE(m_socket->findChild<QTimer*>()->isActive());
}

TEST_F(ControlSocketTest, ZeroIntervalSendsEveryChangeAndCommandsFollowReadAccess)
{
    start(R"({"Control": {"eventInterval": 0, "allowReadText": true}})");
    QLocalSocket client;
    ASSERT_TRUE(connect(client));
    subscribe(client, QStringLiteral("titleChanged"));
    subscribe(client, QStringLiteral("commandStarted"));

    Q_EMIT m_control->titleChanged(1, QStringLiteral("a"));
    Q_EMIT m_control->titleChanged(1, QStringLiteral("b"));
    Q_EMIT m_control->commandStarted(1, QStringLiteral("make"));
    m_settings->write(QStringLiteral("Control"), QStringLiteral("allowReadText"), false);
    Q_EMIT m_control->commandStarted(1, QStringLiteral("secret"));
    EXPECT_EQ(events(client, 60), (QStringList{QStringLiteral("titleChanged:1:a"), QStringLiteral("titleChanged:1:b"),
                                               QStringLiteral("commandStarted:1:make"), QStringLiteral("commandStarted:1:")}));
}

/// A socket file with no listener, as a process that died leaves it.
bool makeStaleSocket(const QString& path)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const QByteArray name = QFile::encodeName(path);
    std::memcpy(address.sun_path, name.constData(), std::min(sizeof(address.sun_path) - 1, std::size_t(name.size())));
    const bool bound = ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
    ::close(fd);
    return bound;
}

TEST_F(ControlSocketTest, RemovesDeadSocketsOfOtherProcesses)
{
    const QString& runtime = m_runtime;
    const QString dead = runtime + QStringLiteral("/afterglow-wayland-9-4242.sock");
    const QString other = runtime + QStringLiteral("/other.sock");
    const QString file = runtime + QStringLiteral("/afterglow-notes.sock");
    const QString live = runtime + QStringLiteral("/afterglow-wayland-9.sock");
    ASSERT_TRUE(makeStaleSocket(dead));
    ASSERT_TRUE(makeStaleSocket(other));
    QFile note(file);
    ASSERT_TRUE(note.open(QIODevice::WriteOnly));
    note.close();
    QLocalServer server;
    ASSERT_TRUE(server.listen(live));

    start();
    EXPECT_FALSE(QFileInfo::exists(dead));
    // Not Afterglow's, not a socket, or alive.
    EXPECT_TRUE(QFileInfo::exists(other));
    EXPECT_TRUE(QFileInfo::exists(file));
    EXPECT_TRUE(QFileInfo::exists(live));
    EXPECT_TRUE(server.isListening());
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
