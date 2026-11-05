// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// The "commands" socket target, the plain C++ run client of afterglow-proto
// against the real phosphor-ipc router, and `afterglow run` itself. Every
// test listens on a socket of its own, in a runtime directory of its own: the
// session that runs the tests may run inside an afterglow pane, whose
// AFTERGLOW_SOCKET these tests must never reach.

#include "app/InstanceSocket.h"
#include "config/Settings.h"
#include "control/CommandSocket.h"
#include "control/Control.h"
#include "scripting/ScriptControl.h"
#include "scripting/ScriptHost.h"

#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/Json.h>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <functional>
#include <optional>
#include <thread>

#include <csignal>

using namespace Afterglow;
using AfterglowProto::IpcConnection;
namespace Json = AfterglowProto::Json;

namespace {

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
        if (wasSet) qputenv(name, value);
        else qunsetenv(name);
    }
    const char* name;
    bool wasSet;
    QByteArray value;
};

const QByteArray Script = R"(
local flooded = 0
return {
    metadata = { commands = {
        greet = { summary = "Greet", usage = "[NAME] [STATUS]", palette = "Greet" },
        read = { summary = "Echo stdin", stdin = true },
        plain = { summary = "Ignore stdin" },
        wait = { summary = "Wait MS", usage = "MS" },
        where = { summary = "Print the pane, directory and source" },
        flood = { summary = "Write MIB parts of 1 MiB, each awaited", usage = "MIB" },
        count = { summary = "Print how many flood parts have gone" },
    } },
    run_command = function(command, ev)
        if command == "greet" then
            return {
                afterglow.output(ev, "hello " .. (ev.args[1] or "world") .. "\n"),
                afterglow.output_error(ev, "note\n"),
                afterglow.exit(ev, tonumber(ev.args[2]) or 0),
            }
        elseif command == "read" then
            return afterglow.output(ev, tostring(ev.stdin))
        elseif command == "plain" then
            return afterglow.output(ev, "plain " .. tostring(ev.stdin) .. "\n")
        elseif command == "wait" then
            afterglow.await(afterglow.output(ev, "start\n"))
            afterglow.sleep(tonumber(ev.args[1]) or 100)
            return afterglow.output(ev, "done\n")
        elseif command == "flood" then
            local part = string.rep("x", 1048576)
            for _ = 1, tonumber(ev.args[1]) do
                assert(afterglow.await(afterglow.output(ev, part)))
                flooded += 1
            end
            return afterglow.output(ev, "done\n")
        elseif command == "count" then
            return afterglow.output(ev, tostring(flooded))
        elseif command == "where" then
            return afterglow.output(ev, tostring(ev.pane and ev.pane.id) .. " " .. ev.cwd .. " " .. ev.source .. "\n")
        end
    end,
}
)";

/// The server runs on this thread: wait with its event loop running.
bool pumpUntil(const std::function<bool()>& done, int timeoutMs = 5000)
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

/// The client of afterglow-proto blocks, so it runs on a thread of its own
/// while this thread runs the router.
void onClientThread(const std::function<void()>& work)
{
    std::atomic<bool> done = false;
    std::thread thread([&] {
        work();
        done = true;
    });
    const bool finished = pumpUntil([&] { return done.load(); }, 15000);
    thread.join();
    EXPECT_TRUE(finished);
}

class CommandSocketTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(m_dir.isValid());
        qputenv("XDG_CONFIG_HOME", m_dir.filePath(QStringLiteral("config")).toUtf8());
        qputenv("XDG_DATA_HOME", m_dir.filePath(QStringLiteral("data")).toUtf8());
        m_runtime = m_dir.filePath(QStringLiteral("runtime"));
        ASSERT_TRUE(QDir().mkpath(m_runtime));
        qputenv("XDG_RUNTIME_DIR", m_runtime.toUtf8());
        qunsetenv("AFTERGLOW_SOCKET");
        qunsetenv("AFTERGLOW_PANE_ID");
        ASSERT_TRUE(QDir().mkpath(scripts()));
        QFile file(scripts() + QStringLiteral("/cmds.luau"));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(Script);
    }

    void TearDown() override
    {
        m_commands.reset();
        m_scripts.reset();
        m_instance.reset();
    }

    QString scripts() const { return m_dir.filePath(QStringLiteral("scripts")); }
    QString socketPath() const { return m_runtime + QStringLiteral("/afterglow-commands-test.sock"); }

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
        m_scripts = std::make_unique<ScriptHost>(scriptCallbacks(*m_control));
        m_scripts->setStateDirectory(m_dir.filePath(QStringLiteral("state")));
        m_scripts->setSearchPaths({scripts()});
        const Settings& s = m_settings->settings();
        m_scripts->setCommandLimits({s.scriptCommandTimeout, s.scriptCommandStdinLimit, s.scriptCommandRunLimit, s.scriptCommandOutputLimit});
        m_scripts->setEnabled(true);
        m_commands = std::make_unique<CommandSocket>(*m_control, *m_scripts, *m_settings, m_instance->router());
        return true;
    }

    static bool waitFor(const std::function<bool()>& done, int timeoutMs = 5000) { return pumpUntil(done, timeoutMs); }

    bool connect(QLocalSocket& client)
    {
        client.connectToServer(socketPath());
        return waitFor([&client] { return client.state() == QLocalSocket::ConnectedState; });
    }

    qint64 send(QLocalSocket& client, const QString& fn, const QJsonArray& args, const QString& type = QStringLiteral("call"))
    {
        const qint64 id = ++m_id;
        QJsonObject frame{{QStringLiteral("type"), type}, {QStringLiteral("id"), id},
                          {QStringLiteral("target"), QLatin1String(CommandSocket::Target)}};
        if (type == QLatin1String("call")) {
            frame.insert(QStringLiteral("fn"), fn);
            frame.insert(QStringLiteral("args"), args);
        }
        client.write(QJsonDocument(frame).toJson(QJsonDocument::Compact) + '\n');
        return id;
    }

    static std::optional<QJsonObject> next(QLocalSocket& client, int timeoutMs = 5000)
    {
        if (!waitFor([&client] { return client.canReadLine(); }, timeoutMs)) return std::nullopt;
        return QJsonDocument::fromJson(client.readLine()).object();
    }

    /// Frames until the answer to @p id, which comes last.
    static QList<QJsonObject> until(QLocalSocket& client, qint64 id, int timeoutMs = 5000)
    {
        QList<QJsonObject> frames;
        while (const auto frame = next(client, timeoutMs)) {
            frames.append(*frame);
            if (frame->value(QStringLiteral("id")).toInteger() == id && frame->value(QStringLiteral("type")) != QLatin1String("event"))
                break;
        }
        return frames;
    }

    QJsonObject call(QLocalSocket& client, const QString& fn, const QJsonArray& args = {})
    {
        const auto frames = until(client, send(client, fn, args));
        return frames.isEmpty() ? QJsonObject{} : frames.last();
    }

    static QString output(const QList<QJsonObject>& frames, const char* signal)
    {
        QString out;
        for (const QJsonObject& f : frames) {
            if (f.value(QStringLiteral("type")) == QLatin1String("event") && f.value(QStringLiteral("signal")) == QLatin1String(signal))
                out += f.value(QStringLiteral("args")).toArray().at(1).toString();
        }
        return out;
    }

    Saved m_configHome{"XDG_CONFIG_HOME"};
    Saved m_dataHome{"XDG_DATA_HOME"};
    Saved m_runtimeDir{"XDG_RUNTIME_DIR"};
    Saved m_socketVar{"AFTERGLOW_SOCKET"};
    Saved m_paneVar{"AFTERGLOW_PANE_ID"};
    QTemporaryDir m_dir;
    QString m_runtime;
    std::unique_ptr<SettingsStore> m_settings;
    std::unique_ptr<Control> m_control;
    std::unique_ptr<InstanceSocket> m_instance;
    std::unique_ptr<ScriptHost> m_scripts;
    std::unique_ptr<CommandSocket> m_commands;
    qint64 m_id = 0;
};

TEST_F(CommandSocketTest, ListsTheCommandsAndEachInTheSchema)
{
    ASSERT_TRUE(start());
    ASSERT_TRUE(m_commands->enabled());
    QLocalSocket client;
    ASSERT_TRUE(connect(client));
    const QJsonArray list = call(client, QStringLiteral("list")).value(QStringLiteral("result")).toArray();
    ASSERT_EQ(list.size(), 7);
    auto entry = [&list](const char* name) {
        for (const QJsonValue& v : list)
            if (v.toObject().value(QStringLiteral("command")) == QLatin1String(name)) return v.toObject();
        return QJsonObject{};
    };
    const QJsonObject greet = entry("greet");
    EXPECT_EQ(greet.value(QStringLiteral("script")), QStringLiteral("cmds"));
    EXPECT_EQ(greet.value(QStringLiteral("command")), QStringLiteral("greet"));
    EXPECT_EQ(greet.value(QStringLiteral("usage")), QStringLiteral("[NAME] [STATUS]"));
    EXPECT_EQ(greet.value(QStringLiteral("palette")), QStringLiteral("Greet"));
    EXPECT_TRUE(entry("read").value(QStringLiteral("stdin")).toBool());

    const QJsonObject schema = until(client, send(client, {}, {}, QStringLiteral("schema"))).last();
    QStringList functions, signalNames;
    for (const QJsonValue& f : schema.value(QStringLiteral("result")).toObject().value(QStringLiteral("functions")).toArray())
        functions << f.toObject().value(QStringLiteral("name")).toString();
    for (const QJsonValue& s : schema.value(QStringLiteral("result")).toObject().value(QStringLiteral("signals")).toArray())
        signalNames << s.toObject().value(QStringLiteral("name")).toString();
    for (const char* name : {"list", "input", "run", "ack", "cancel", "cmds.greet", "cmds.read", "cmds.plain", "cmds.wait", "cmds.where"})
        EXPECT_TRUE(functions.contains(QLatin1String(name))) << name;
    EXPECT_EQ(signalNames, (QStringList{QStringLiteral("stdout"), QStringLiteral("stderr")}));

    // A command's entry runs it, with a run ID of the terminal's.
    const auto frames = until(client, send(client, QStringLiteral("cmds.greet"), QJsonArray{QJsonValue(QJsonArray{QStringLiteral("schema"), QStringLiteral("4")})}));
    EXPECT_EQ(output(frames, "stdout"), QStringLiteral("hello schema\n"));
    EXPECT_EQ(frames.last().value(QStringLiteral("result")).toObject().value(QStringLiteral("exit")).toInt(), 4);

    // A command that a script adds or drops changes the schema.
    QFile file(scripts() + QStringLiteral("/cmds.luau"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("return {metadata = {commands = {only = {summary = 'Only'}}}, run_command = function() end}");
    file.close();
    m_scripts->reload();
    ASSERT_TRUE(waitFor([&] {
        QCoreApplication::processEvents();
        const auto result = until(client, send(client, {}, {}, QStringLiteral("schema"))).last();
        QStringList names;
        for (const QJsonValue& f : result.value(QStringLiteral("result")).toObject().value(QStringLiteral("functions")).toArray())
            names << f.toObject().value(QStringLiteral("name")).toString();
        return names.contains(QStringLiteral("cmds.only")) && !names.contains(QStringLiteral("cmds.greet"));
    }));
}

TEST_F(CommandSocketTest, ARunRepliesWhenItEndsAndItsEventsGoToTheCallerOnly)
{
    ASSERT_TRUE(start());
    QLocalSocket client, other;
    ASSERT_TRUE(connect(client));
    ASSERT_TRUE(connect(other));
    const qint64 run = send(client, QStringLiteral("run"), {QStringLiteral("r1"), QStringLiteral("cmds"), QStringLiteral("wait"),
                                                          QJsonArray{QStringLiteral("100")}, QStringLiteral("/work"), 0});
    // A list call on the same connection answers while the run waits.
    const qint64 list = send(client, QStringLiteral("list"), {});
    QList<QJsonObject> frames = until(client, list);
    ASSERT_FALSE(frames.isEmpty());
    EXPECT_EQ(frames.last().value(QStringLiteral("type")), QStringLiteral("reply"));
    frames += until(client, run);
    ASSERT_FALSE(frames.isEmpty());
    EXPECT_EQ(output(frames, "stdout"), QStringLiteral("start\ndone\n"));
    for (const QJsonObject& f : std::as_const(frames)) {
        if (f.value(QStringLiteral("type")) != QLatin1String("event")) continue;
        EXPECT_FALSE(f.contains(QStringLiteral("subscriptionId")));
        EXPECT_EQ(f.value(QStringLiteral("target")), QStringLiteral("commands"));
        EXPECT_EQ(f.value(QStringLiteral("args")).toArray().at(0), QStringLiteral("r1"));
    }
    EXPECT_EQ(frames.last().value(QStringLiteral("result")).toObject().value(QStringLiteral("exit")).toInt(), 0);
    // The other client got nothing.
    EXPECT_FALSE(next(other, 200));

    const auto unknown = call(client, QStringLiteral("run"), {QStringLiteral("r2"), QStringLiteral("cmds"), QStringLiteral("nope"),
                                                              QJsonArray{}, QString(), 0});
    EXPECT_EQ(unknown.value(QStringLiteral("code")), QLatin1String(CommandSocket::NoSuchCommand));
    const auto badPane = call(client, QStringLiteral("run"), {QStringLiteral("r3"), QStringLiteral("cmds"), QStringLiteral("greet"),
                                                              QJsonArray{}, QString(), 99});
    EXPECT_EQ(badPane.value(QStringLiteral("message")), QStringLiteral("no pane 99"));
}

TEST_F(CommandSocketTest, StdinComesBeforeTheRunAndStopsAtTheLimit)
{
    ASSERT_TRUE(start(R"({"Scripts": {"commandStdinLimit": 8}})"));
    QLocalSocket client;
    ASSERT_TRUE(connect(client));
    EXPECT_TRUE(call(client, QStringLiteral("input"), {QStringLiteral("in"), QStringLiteral("abc"), false})
                    .value(QStringLiteral("result")).toBool());
    // At the limit the terminal takes no more, and keeps what fits.
    EXPECT_FALSE(call(client, QStringLiteral("input"), {QStringLiteral("in"), QStringLiteral("defghij"), false})
                     .value(QStringLiteral("result")).toBool());
    EXPECT_EQ(call(client, QStringLiteral("input"), {QStringLiteral("in"), QStringLiteral("k"), true}).value(QStringLiteral("type")),
              QStringLiteral("error"));
    const auto frames = until(client, send(client, QStringLiteral("run"), {QStringLiteral("in"), QStringLiteral("cmds"),
                                                                           QStringLiteral("read"), QJsonArray{}, QString(), 0}));
    EXPECT_EQ(output(frames, "stdout"), QStringLiteral("abcdefgh"));
    // Input after the run started is refused, and a command without stdin gets none.
    EXPECT_EQ(call(client, QStringLiteral("input"), {QStringLiteral("p"), QStringLiteral("x"), true}).value(QStringLiteral("result")),
              false);
    const auto plain = until(client, send(client, QStringLiteral("run"), {QStringLiteral("p"), QStringLiteral("cmds"),
                                                                          QStringLiteral("plain"), QJsonArray{}, QString(), 0}));
    EXPECT_EQ(output(plain, "stdout"), QStringLiteral("plain nil\n"));
}

TEST_F(CommandSocketTest, ARunIdBelongsToItsConnection)
{
    ASSERT_TRUE(start());
    QLocalSocket client, other;
    ASSERT_TRUE(connect(client));
    ASSERT_TRUE(connect(other));
    const qint64 first = send(client, QStringLiteral("run"), {QStringLiteral("same"), QStringLiteral("cmds"), QStringLiteral("wait"),
                                                            QJsonArray{QStringLiteral("300")}, QString(), 0});
    ASSERT_TRUE(waitFor([&] { return client.canReadLine(); })); // its first output
    const auto again = call(client, QStringLiteral("run"), {QStringLiteral("same"), QStringLiteral("cmds"), QStringLiteral("greet"),
                                                            QJsonArray{}, QString(), 0});
    EXPECT_EQ(again.value(QStringLiteral("type")), QStringLiteral("error"));
    EXPECT_TRUE(again.value(QStringLiteral("message")).toString().contains(QStringLiteral("in use")));
    // Another connection has run IDs of its own.
    const auto elsewhere = until(other, send(other, QStringLiteral("run"), {QStringLiteral("same"), QStringLiteral("cmds"),
                                                                           QStringLiteral("greet"), QJsonArray{}, QString(), 0}));
    EXPECT_EQ(output(elsewhere, "stdout"), QStringLiteral("hello world\n"));
    // The first run is cancelled: its reply has status 130.
    send(client, QStringLiteral("cancel"), {QStringLiteral("same")});
    const auto frames = until(client, first);
    EXPECT_EQ(frames.last().value(QStringLiteral("result")).toObject().value(QStringLiteral("exit")).toInt(), 130);
    EXPECT_FALSE(output(frames, "stdout").contains(QStringLiteral("done")));
}

TEST_F(CommandSocketTest, AClosedConnectionDropsItsStdinAndCancelsItsRuns)
{
    ASSERT_TRUE(start(R"({"Scripts": {"commandRunLimit": 1}})"));
    {
        QLocalSocket client;
        ASSERT_TRUE(connect(client));
        call(client, QStringLiteral("input"), {QStringLiteral("x"), QStringLiteral("abc"), false});
        EXPECT_EQ(m_commands->waitingInputs(), 1);
        send(client, QStringLiteral("run"), {QStringLiteral("long"), QStringLiteral("cmds"), QStringLiteral("wait"),
                                            QJsonArray{QStringLiteral("60000")}, QString(), 0});
        ASSERT_TRUE(waitFor([&] { return client.canReadLine(); }));
        client.disconnectFromServer();
    }
    ASSERT_TRUE(waitFor([&] { return m_commands->waitingInputs() == 0; }));
    // With a limit of one run, a new run starts only once the old one ended.
    QLocalSocket next;
    ASSERT_TRUE(connect(next));
    ASSERT_TRUE(waitFor([&] {
        const auto reply = call(next, QStringLiteral("run"), {QStringLiteral("n") + QString::number(m_id), QStringLiteral("cmds"),
                                                              QStringLiteral("greet"), QJsonArray{}, QString(), 0});
        return reply.value(QStringLiteral("type")) == QLatin1String("reply");
    }));
}

TEST_F(CommandSocketTest, FollowsControlAndScriptsEnabled)
{
    ASSERT_TRUE(start(R"({"Control": {"enabled": false}})"));
    EXPECT_FALSE(m_commands->enabled());
    EXPECT_FALSE(m_instance->router().listTargets().contains(QStringLiteral("commands")));
    m_settings->write(QStringLiteral("Control"), QStringLiteral("enabled"), true);
    EXPECT_TRUE(m_commands->enabled());
    QLocalSocket client;
    ASSERT_TRUE(connect(client));
    const qint64 run = send(client, QStringLiteral("run"), {QStringLiteral("r"), QStringLiteral("cmds"), QStringLiteral("wait"),
                                                          QJsonArray{QStringLiteral("60000")}, QString(), 0});
    ASSERT_TRUE(waitFor([&] { return client.canReadLine(); }));
    // Turning scripts off ends the target and fails the run that waits.
    m_settings->write(QStringLiteral("Scripts"), QStringLiteral("enabled"), false);
    EXPECT_FALSE(m_commands->enabled());
    const auto frames = until(client, run);
    ASSERT_FALSE(frames.isEmpty());
    EXPECT_EQ(frames.last().value(QStringLiteral("type")), QStringLiteral("error"));
    EXPECT_EQ(call(client, QStringLiteral("list")).value(QStringLiteral("code")), QStringLiteral("NO_SUCH_TARGET"));
}

TEST_F(CommandSocketTest, ASlowReaderHoldsTheRunBackAndKeepsItsConnection)
{
    ASSERT_TRUE(start(R"({"Scripts": {"commandOutputLimit": 67108864}})"));
    QLocalSocket client, other;
    ASSERT_TRUE(connect(client));
    ASSERT_TRUE(connect(other));
    // The client reads nothing more: what the terminal sends piles up in the
    // router, which would close the connection past 16 MiB.
    client.setReadBufferSize(64 * 1024);
    const qint64 run = send(client, QStringLiteral("run"), {QStringLiteral("flood"), QStringLiteral("cmds"), QStringLiteral("flood"),
                                                          QJsonArray{QStringLiteral("20")}, QString(), 0});
    pumpUntil([] { return false; }, 1500);
    EXPECT_EQ(client.state(), QLocalSocket::ConnectedState);
    // The handler's await waits: it has sent about the window, not 20 MiB.
    const int parts = output(until(other, send(other, QStringLiteral("run"), {QStringLiteral("c"), QStringLiteral("cmds"),
                                                                            QStringLiteral("count"), QJsonArray{}, QString(), 0})),
                             "stdout").toInt();
    EXPECT_GE(parts, 3);
    EXPECT_LE(parts, 6);
    // Reading and acknowledging, as afterglow run does, lets it go on.
    client.setReadBufferSize(0);
    qint64 total = 0, unacknowledged = 0;
    QJsonObject reply;
    ASSERT_TRUE(pumpUntil([&] {
        while (client.canReadLine()) {
            const QByteArray line = client.readLine();
            const QJsonObject frame = QJsonDocument::fromJson(line).object();
            if (frame.value(QStringLiteral("type")) == QLatin1String("event")) {
                total += frame.value(QStringLiteral("args")).toArray().at(1).toString().size();
                unacknowledged += line.size();
                if (unacknowledged >= 256 * 1024) {
                    send(client, QStringLiteral("ack"), {QStringLiteral("flood"), double(unacknowledged)});
                    unacknowledged = 0;
                }
            } else if (frame.value(QStringLiteral("id")).toInteger() == run) {
                reply = frame;
                return true;
            }
        }
        return false;
    }, 20000));
    EXPECT_EQ(total, 20 * 1048576 + 5);
    EXPECT_EQ(reply.value(QStringLiteral("result")).toObject().value(QStringLiteral("exit")).toInt(), 0);
    EXPECT_EQ(client.state(), QLocalSocket::ConnectedState);
}

TEST_F(CommandSocketTest, WaitingStdinHasACapForTheWholeProcess)
{
    ASSERT_TRUE(start(R"({"Scripts": {"commandStdinLimit": 16777216}})"));
    QLocalSocket client;
    ASSERT_TRUE(connect(client));
    const QString part(1000000, QLatin1Char('x'));
    for (int run = 0; run < 4; ++run) {
        bool more = true;
        while (more) {
            const QJsonObject reply = call(client, QStringLiteral("input"), {QStringLiteral("in%1").arg(run), part, false});
            ASSERT_EQ(reply.value(QStringLiteral("type")), QStringLiteral("reply"));
            more = reply.value(QStringLiteral("result")).toBool();
        }
    }
    EXPECT_EQ(m_commands->waitingInputBytes(), CommandSocket::MaxWaitingInputBytes);
    const QJsonObject over = call(client, QStringLiteral("input"), {QStringLiteral("in4"), QStringLiteral("x"), false});
    EXPECT_EQ(over.value(QStringLiteral("type")), QStringLiteral("error"));
    EXPECT_TRUE(over.value(QStringLiteral("message")).toString().contains(QStringLiteral("at most 64 MiB")));
    // A cancel and a closed connection give the room back.
    call(client, QStringLiteral("cancel"), {QStringLiteral("in0")});
    EXPECT_EQ(m_commands->waitingInputBytes(), 3 * 16777216);
    client.disconnectFromServer();
    ASSERT_TRUE(waitFor([&] { return m_commands->waitingInputBytes() == 0; }));
}

TEST_F(CommandSocketTest, TheProtoClientSpeaksTheRoutersWireFormat)
{
    ASSERT_TRUE(start());
    const std::string path = socketPath().toStdString();
    onClientThread([&] {
        IpcConnection ipc;
        IpcConnection::Error error;
        ASSERT_TRUE(ipc.connect(path, &error)) << error.message;
        const auto list = ipc.call("commands", "list", {}, 5000, &error);
        ASSERT_TRUE(list) << error.message;
        ASSERT_EQ(list->toArray().size(), 7u);
        EXPECT_EQ(list->toArray()[0]["command"].toString(), "count");

        // Stdin before the run, then a deferred reply after the run's events.
        const auto more = ipc.call("commands", "input", {"id1", "from stdin \x01 é", true}, 5000, &error);
        ASSERT_TRUE(more) << error.message;
        EXPECT_FALSE(more->toBool());
        const auto run = ipc.send("commands", "run", {"id1", "cmds", "read", Json::Array{}, "/", 0}, &error);
        ASSERT_TRUE(run);
        std::string out;
        Json::Value frame;
        while (ipc.next(frame, 5000) == IpcConnection::Read::Frame) {
            if (IpcConnection::answers(frame, *run)) break;
            ASSERT_EQ(frame["type"].toString(), "event");
            EXPECT_EQ(frame["signal"].toString(), "stdout");
            EXPECT_EQ(frame["args"].toArray()[0].toString(), "id1");
            out += frame["args"].toArray()[1].toString();
        }
        EXPECT_EQ(out, "from stdin \x01 é");
        const auto result = IpcConnection::answer(frame, &error);
        ASSERT_TRUE(result);
        EXPECT_EQ((*result)["exit"].toInteger(), 0);

        // A call answers while a deferred one waits; its events wait for next().
        const auto waiting = ipc.send("commands", "run", {"id2", "cmds", "wait", Json::Array{"50"}, "/", 0}, &error);
        ASSERT_TRUE(waiting);
        const auto listed = ipc.call("commands", "list", {}, 5000, &error);
        EXPECT_TRUE(listed) << error.message;
        int events = 0;
        while (ipc.next(frame, 5000) == IpcConnection::Read::Frame && !IpcConnection::answers(frame, *waiting)) ++events;
        EXPECT_EQ(events, 2); // start and done
        EXPECT_EQ((*IpcConnection::answer(frame, &error))["exit"].toInteger(), 0);

        // Errors carry the router's or the target's code.
        EXPECT_FALSE(ipc.call("commands", "run", {"id3", "cmds", "nope", Json::Array{}, "/", 0}, 5000, &error));
        EXPECT_EQ(error.code, "NO_SUCH_COMMAND");
        EXPECT_FALSE(ipc.call("commands", "nothing", {}, 5000, &error));
        EXPECT_EQ(error.code, "NO_SUCH_FN");
        EXPECT_FALSE(ipc.call("nowhere", "list", {}, 5000, &error));
        EXPECT_EQ(error.code, "NO_SUCH_TARGET");
        // A request over the router's line limit is not sent.
        EXPECT_FALSE(ipc.send("commands", "input", {"id4", std::string(IpcConnection::MaxRequestBytes, 'x'), true}, &error));
    });
}

/// `afterglow run`, the binary, against this test's socket.
class RunBinaryTest : public CommandSocketTest
{
protected:
    struct Result
    {
        int status = -1;
        QString out;
        QString err;
    };

    QProcessEnvironment environment() const
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("XDG_RUNTIME_DIR"), m_runtime);
        env.insert(QStringLiteral("AFTERGLOW_SOCKET"), socketPath());
        env.remove(QStringLiteral("AFTERGLOW_PANE_ID"));
        return env;
    }

    /// Runs the binary with @p input on stdin, closed after it unless @p keepOpen.
    Result run(const QStringList& args, const QByteArray& input = {}, bool keepOpen = false,
               const QProcessEnvironment& env = {}, const std::function<void(QProcess&)>& during = {})
    {
        QProcess process;
        process.setProcessEnvironment(env.isEmpty() ? environment() : env);
        process.setWorkingDirectory(m_dir.path());
        process.start(QStringLiteral(AFTERGLOW_BINARY), QStringList{QStringLiteral("run")} + args);
        EXPECT_TRUE(process.waitForStarted());
        process.write(input);
        if (!keepOpen) process.closeWriteChannel();
        if (during) during(process);
        const bool ended = pumpUntil([&] { return process.state() == QProcess::NotRunning; }, 15000);
        EXPECT_TRUE(ended) << qPrintable(args.join(QLatin1Char(' ')));
        if (!ended) {
            process.kill();
            process.waitForFinished();
        }
        return {process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1,
                QString::fromUtf8(process.readAllStandardOutput()), QString::fromUtf8(process.readAllStandardError())};
    }
};

TEST_F(RunBinaryTest, PassesOutputAndExitStatusThrough)
{
    ASSERT_TRUE(start());
    Result r = run({QStringLiteral("cmds"), QStringLiteral("greet"), QStringLiteral("you"), QStringLiteral("7")});
    EXPECT_EQ(r.status, 7);
    EXPECT_EQ(r.out, QStringLiteral("hello you\n"));
    EXPECT_EQ(r.err, QStringLiteral("note\n"));
    // Arguments after the command are the command's, options too.
    r = run({QStringLiteral("cmds"), QStringLiteral("greet"), QStringLiteral("--help")});
    EXPECT_EQ(r.out, QStringLiteral("hello --help\n"));
    // The client's directory, and no pane when none has the focus.
    r = run({QStringLiteral("cmds"), QStringLiteral("where")});
    EXPECT_EQ(r.out, QStringLiteral("nil %1 shell\n").arg(m_dir.path()));
}

TEST_F(RunBinaryTest, ListsHelpsAndCompletes)
{
    ASSERT_TRUE(start());
    Result r = run({});
    EXPECT_EQ(r.status, 0);
    EXPECT_TRUE(r.out.startsWith(QStringLiteral("cmds\n"))) << qPrintable(r.out);
    EXPECT_TRUE(r.out.contains(QStringLiteral("greet [NAME] [STATUS]"))) << qPrintable(r.out);
    r = run({QStringLiteral("cmds"), QStringLiteral("--help")});
    EXPECT_EQ(r.status, 0);
    EXPECT_TRUE(r.out.contains(QStringLiteral("wait MS"))) << qPrintable(r.out);
    r = run({QStringLiteral("--complete")});
    EXPECT_EQ(r.out, QStringLiteral("cmds\n"));
    r = run({QStringLiteral("--complete"), QStringLiteral("cmds")});
    EXPECT_TRUE(r.out.contains(QStringLiteral("\ngreet\tGreet\nplain\tIgnore stdin\nread\tEcho stdin\n"))) << qPrintable(r.out);
    r = run({QStringLiteral("--help")});
    EXPECT_EQ(r.status, 0);
    EXPECT_TRUE(r.out.startsWith(QStringLiteral("usage: afterglow run")));
}

TEST_F(RunBinaryTest, UsageErrorsAndUnknownCommandsExitWithTwo)
{
    ASSERT_TRUE(start());
    for (const QStringList& args : {QStringList{QStringLiteral("cmds"), QStringLiteral("nope")},
                                    QStringList{QStringLiteral("nope"), QStringLiteral("greet")},
                                    QStringList{QStringLiteral("--bogus")}, QStringList{QStringLiteral("cmds")},
                                    QStringList{QStringLiteral("--pane"), QStringLiteral("x"), QStringLiteral("cmds"), QStringLiteral("greet")},
                                    QStringList{QStringLiteral("-n")}}) {
        const Result r = run(args);
        EXPECT_EQ(r.status, 2) << qPrintable(args.join(QLatin1Char(' '))) << qPrintable(r.err);
        EXPECT_FALSE(r.err.isEmpty());
    }
}

TEST_F(RunBinaryTest, ReadsStdinOnlyForACommandThatTakesIt)
{
    ASSERT_TRUE(start());
    Result r = run({QStringLiteral("cmds"), QStringLiteral("read")}, "abc");
    EXPECT_EQ(r.out, QStringLiteral("abc"));
    // Several parts: control characters take six bytes each once written.
    QByteArray big;
    for (int i = 0; i < 100000; ++i) big += "line\x01\xc3\xa9\n";
    r = run({QStringLiteral("cmds"), QStringLiteral("read")}, big);
    EXPECT_EQ(r.status, 0);
    EXPECT_EQ(r.out.toUtf8(), big);
    // -n passes none.
    r = run({QStringLiteral("-n"), QStringLiteral("cmds"), QStringLiteral("read")}, "abc");
    EXPECT_EQ(r.out, QStringLiteral("nil"));
    // A command without stdin does not wait for a pipe that stays open.
    r = run({QStringLiteral("cmds"), QStringLiteral("plain")}, "never read", true);
    EXPECT_EQ(r.status, 0);
    EXPECT_EQ(r.out, QStringLiteral("plain nil\n"));
}

TEST_F(RunBinaryTest, ChoosesThePaneAsCtlDoes)
{
    ASSERT_TRUE(start());
    // The pane goes to the terminal, which has none here: it names the one asked for.
    Result r = run({QStringLiteral("--pane"), QStringLiteral("5"), QStringLiteral("cmds"), QStringLiteral("where")});
    EXPECT_EQ(r.status, 1);
    EXPECT_TRUE(r.err.contains(QStringLiteral("no pane 5"))) << qPrintable(r.err);
    QProcessEnvironment inPane = environment();
    inPane.insert(QStringLiteral("AFTERGLOW_PANE_ID"), QStringLiteral("6"));
    r = run({QStringLiteral("cmds"), QStringLiteral("where")}, {}, false, inPane);
    EXPECT_TRUE(r.err.contains(QStringLiteral("no pane 6"))) << qPrintable(r.err);
    r = run({QStringLiteral("--pane"), QStringLiteral("5"), QStringLiteral("cmds"), QStringLiteral("where")}, {}, false, inPane);
    EXPECT_TRUE(r.err.contains(QStringLiteral("no pane 5"))) << qPrintable(r.err);
    // A pane of an afterglow without a socket is an error, as in ctl.
    QProcessEnvironment orphan = inPane;
    orphan.remove(QStringLiteral("AFTERGLOW_SOCKET"));
    r = run({QStringLiteral("cmds"), QStringLiteral("where")}, {}, false, orphan);
    EXPECT_EQ(r.status, 1);
    EXPECT_TRUE(r.err.contains(QStringLiteral("has no socket"))) << qPrintable(r.err);
}

TEST_F(RunBinaryTest, ASignalCancelsTheRun)
{
    ASSERT_TRUE(start(R"({"Scripts": {"commandRunLimit": 1}})"));
    for (const auto& [signal, status] : {std::pair{SIGINT, 130}, std::pair{SIGTERM, 143}}) {
        const Result r = run({QStringLiteral("cmds"), QStringLiteral("wait"), QStringLiteral("60000")}, {}, false, {},
                             [&, signal = signal](QProcess& process) {
                                 ASSERT_TRUE(pumpUntil([&] { return process.canReadLine(); }));
                                 ::kill(pid_t(process.processId()), signal);
                             });
        EXPECT_EQ(r.status, status);
        EXPECT_EQ(r.out, QStringLiteral("start\n"));
    }
    // The run ended in the terminal too: with a limit of one, another starts.
    ASSERT_TRUE(pumpUntil([&] { return run({QStringLiteral("cmds"), QStringLiteral("greet")}).status == 0; }));
}

TEST_F(RunBinaryTest, AcknowledgesOutputPastTheWindow)
{
    ASSERT_TRUE(start(R"({"Scripts": {"commandOutputLimit": 67108864}})"));
    const Result r = run({QStringLiteral("cmds"), QStringLiteral("flood"), QStringLiteral("20")});
    EXPECT_EQ(r.status, 0) << qPrintable(r.err);
    EXPECT_EQ(r.out.size(), 20 * 1048576 + 5);
    EXPECT_TRUE(r.out.endsWith(QStringLiteral("done\n")));
}

TEST_F(RunBinaryTest, ASignalEndsAWaitForStdin)
{
    ASSERT_TRUE(start());
    // Stdin stays open with no end. Once a part of it has reached the
    // terminal, the client waits for more, and a signal must end that wait.
    const Result r = run({QStringLiteral("cmds"), QStringLiteral("read")}, QByteArray(1200 * 1024, 'x'), true, {},
                         [this](QProcess& process) {
                             ASSERT_TRUE(pumpUntil([this] { return m_commands->waitingInputBytes() > 0; }));
                             ::kill(pid_t(process.processId()), SIGTERM);
                         });
    EXPECT_EQ(r.status, 143);
}

TEST_F(RunBinaryTest, AnInheritedIgnoredSigintStaysIgnored)
{
    ASSERT_TRUE(start());
    QProcess process;
    process.setProcessEnvironment(environment());
    process.setChildProcessModifier([] { ::signal(SIGINT, SIG_IGN); });
    process.start(QStringLiteral(AFTERGLOW_BINARY), {QStringLiteral("run"), QStringLiteral("cmds"), QStringLiteral("wait"), QStringLiteral("60000")});
    ASSERT_TRUE(process.waitForStarted());
    process.closeWriteChannel();
    ASSERT_TRUE(pumpUntil([&] { return process.canReadLine(); }));
    ::kill(pid_t(process.processId()), SIGINT);
    EXPECT_FALSE(pumpUntil([&] { return process.state() == QProcess::NotRunning; }, 500));
    ::kill(pid_t(process.processId()), SIGTERM);
    ASSERT_TRUE(pumpUntil([&] { return process.state() == QProcess::NotRunning; }));
    EXPECT_EQ(process.exitCode(), 143);
}

TEST_F(RunBinaryTest, ArgumentsOverOneRequestAreAUsageError)
{
    ASSERT_TRUE(start());
    QStringList args{QStringLiteral("cmds"), QStringLiteral("greet")};
    for (int i = 0; i < 300; ++i) args << QString(4000, QLatin1Char('x'));
    const Result r = run(args);
    EXPECT_EQ(r.status, 2);
    EXPECT_TRUE(r.err.contains(QStringLiteral("too long"))) << qPrintable(r.err);
}

TEST_F(RunBinaryTest, SaysWhenCommandsAreOff)
{
    ASSERT_TRUE(start(R"({"Scripts": {"enabled": false}})"));
    const Result r = run({QStringLiteral("cmds"), QStringLiteral("greet")});
    EXPECT_EQ(r.status, 1);
    EXPECT_TRUE(r.err.contains(QStringLiteral("Scripts.enabled is off"))) << qPrintable(r.err);
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
