// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// The "commands" socket target, the plain C++ run client of afterglow-proto
// against the real phosphor-ipc router, and `aglow run` itself, also as
// `afterglow run` runs it, the command lines of `afterglow ctl` and
// `afterglow --help`, and `afterglow ctl overlay` against a target of the
// test's own. Every test listens on a socket of its own, in a
// runtime directory of its own, or points AFTERGLOW_SOCKET where nothing
// listens: the session that runs the tests may run inside an afterglow pane,
// whose AFTERGLOW_SOCKET these tests must never reach.

#include "app/InstanceSocket.h"
#include "app/ScriptSettings.h"
#include "config/Settings.h"
#include "control/CommandSocket.h"
#include "control/Control.h"
#include "scripting/ScriptControl.h"
#include "scripting/ScriptHost.h"
#include "shell/Aglow.h"

#include <AfterglowProto/Fd.h>
#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/Json.h>
#include <AfterglowProto/Run.h>

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QThread>

#include <atomic>
#include <functional>
#include <optional>
#include <thread>

#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

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

/// The parts of 1 MiB that the flood and spew commands write.
constexpr qint64 Part = 1048576;
/// The parts that fill the window of a connection: each line is a little
/// over a part, and a line goes while less than the window waits.
constexpr qint64 WindowParts = CommandSocket::AckWindow / Part;
static_assert(CommandSocket::AckWindow % Part == 0);

const QByteArray Script = R"(
local flooded = 0
local results = {}
return {
    metadata = { commands = {
        greet = { summary = "Greet", usage = "[NAME] [STATUS]", palette = "Greet" },
        read = { summary = "Echo stdin", stdin = true },
        plain = { summary = "Ignore stdin" },
        wait = { summary = "Wait MS", usage = "MS" },
        where = { summary = "Print the pane, directory and source" },
        flood = { summary = "Write MIB parts of 1 MiB, each awaited", usage = "MIB" },
        count = { summary = "Print how many flood parts have gone" },
        spew = { summary = "Write MIB parts of 1 MiB at once, then exit 3", usage = "MIB" },
        twin = { summary = "Write 5 MiB, then nothing, each with a result" },
        results = { summary = "Print the results of twin" },
    } },
    on_result = function(ev)
        table.insert(results, ev.request_id .. "=" .. tostring(ev.ok))
    end,
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
        elseif command == "spew" then
            local part = string.rep("z", 1048576)
            local effects = {}
            for _ = 1, tonumber(ev.args[1]) do
                table.insert(effects, afterglow.output(ev, part))
            end
            table.insert(effects, afterglow.exit(ev, 3))
            return effects
        elseif command == "twin" then
            return {
                afterglow.request("big", afterglow.output(ev, string.rep("y", 5 * 1048576))),
                afterglow.request("empty", afterglow.output(ev, "")),
            }
        elseif command == "results" then
            return afterglow.output(ev, table.concat(results, " "))
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
    /// Where start() listens: m_socketName in the test's runtime directory.
    QString socketPath() const { return m_runtime + QLatin1Char('/') + m_socketName; }

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
        // In Application's order: the socket's slot for the settings runs first.
        m_commands = std::make_unique<CommandSocket>(*m_control, *m_scripts, *m_settings, m_instance->router());
        followScriptSettings(*m_scripts, *m_settings);
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

    /// The last of @p frames, the answer that until() waited for.
    static QJsonObject last(const QList<QJsonObject>& frames)
    {
        if (frames.isEmpty()) ADD_FAILURE() << "no answer";
        return frames.isEmpty() ? QJsonObject{} : frames.last();
    }

    QJsonObject call(QLocalSocket& client, const QString& fn, const QJsonArray& args = {})
    {
        return last(until(client, send(client, fn, args)));
    }

    QStringList schemaNames(QLocalSocket& client, const char* kind)
    {
        QStringList names;
        const QJsonObject schema = last(until(client, send(client, {}, {}, QStringLiteral("schema"))));
        for (const QJsonValue& f : schema.value(QStringLiteral("result")).toObject().value(QLatin1String(kind)).toArray())
            names << f.toObject().value(QStringLiteral("name")).toString();
        return names;
    }

    /// What a client read until the answers to @p ids came.
    struct Drained
    {
        QString out;
        QString err;
        QHash<qint64, QJsonObject> answers;
    };

    /// Reads, from now without a limit, until the answers to @p ids came.
    /// With @p acknowledge it acknowledges the event lines as CONTROL.md
    /// says: once the runs of the connection have AckBytes together, each
    /// run's count. An ack of each run on its own, at AckBytes, would stall
    /// many small runs that fill the window together.
    Drained drain(QLocalSocket& client, const QList<qint64>& ids, bool acknowledge, int timeoutMs = 20000)
    {
        client.setReadBufferSize(0);
        Drained got;
        QHash<QString, qint64> unacknowledged;
        qint64 total = 0;
        const bool done = pumpUntil([&] {
            while (client.canReadLine()) {
                const QByteArray line = client.readLine();
                const QJsonObject frame = QJsonDocument::fromJson(line).object();
                if (frame.value(QStringLiteral("type")) == QLatin1String("event")) {
                    const QJsonArray args = frame.value(QStringLiteral("args")).toArray();
                    (frame.value(QStringLiteral("signal")) == QLatin1String("stdout") ? got.out : got.err) += args.at(1).toString();
                    const QString run = args.at(0).toString();
                    unacknowledged[run] += line.size();
                    total += line.size();
                    if (acknowledge && total >= qint64(AfterglowProto::Run::AckBytes)) {
                        for (auto count = unacknowledged.cbegin(); count != unacknowledged.cend(); ++count)
                            send(client, QStringLiteral("ack"), {count.key(), double(count.value())});
                        unacknowledged.clear();
                        total = 0;
                    }
                } else if (const qint64 id = frame.value(QStringLiteral("id")).toInteger(); ids.contains(id)) {
                    got.answers.insert(id, frame);
                }
            }
            return got.answers.size() == ids.size();
        }, timeoutMs);
        EXPECT_TRUE(done) << "answers for " << got.answers.size() << " of " << ids.size() << " calls";
        return got;
    }

    static int exitOf(const QJsonObject& answer) { return answer.value(QStringLiteral("result")).toObject().value(QStringLiteral("exit")).toInt(-1); }

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
    QString m_socketName = QStringLiteral("afterglow-commands-test.sock");
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
    ASSERT_EQ(list.size(), 10);
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

    const QStringList functions = schemaNames(client, "functions");
    for (const char* name : {"list", "input", "run", "ack", "cancel", "cmds.greet", "cmds.read", "cmds.plain", "cmds.wait", "cmds.where"})
        EXPECT_TRUE(functions.contains(QLatin1String(name))) << name;
    EXPECT_EQ(schemaNames(client, "signals"), (QStringList{QStringLiteral("stdout"), QStringLiteral("stderr")}));

    // A command's entry runs it, with a run ID of the terminal's.
    const auto frames = until(client, send(client, QStringLiteral("cmds.greet"), QJsonArray{QJsonValue(QJsonArray{QStringLiteral("schema"), QStringLiteral("4")})}));
    EXPECT_EQ(output(frames, "stdout"), QStringLiteral("hello schema\n"));
    EXPECT_EQ(exitOf(last(frames)), 4);

    // A command that a script adds or drops changes the schema. The events stay.
    QFile file(scripts() + QStringLiteral("/cmds.luau"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("return {metadata = {commands = {only = {summary = 'Only'}}}, run_command = function() end}");
    file.close();
    m_scripts->reload();
    ASSERT_TRUE(waitFor([&] {
        QCoreApplication::processEvents();
        const QStringList names = schemaNames(client, "functions");
        return names.contains(QStringLiteral("cmds.only")) && !names.contains(QStringLiteral("cmds.greet"));
    }));
    EXPECT_EQ(schemaNames(client, "signals"), (QStringList{QStringLiteral("stdout"), QStringLiteral("stderr")}));
    // With no command at all, the events are still there.
    ASSERT_TRUE(QFile::remove(scripts() + QStringLiteral("/cmds.luau")));
    m_scripts->reload();
    ASSERT_TRUE(waitFor([&] {
        QCoreApplication::processEvents();
        return !schemaNames(client, "functions").contains(QStringLiteral("cmds.only"));
    }));
    EXPECT_EQ(schemaNames(client, "signals"), (QStringList{QStringLiteral("stdout"), QStringLiteral("stderr")}));
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
    EXPECT_EQ(last(frames).value(QStringLiteral("type")), QStringLiteral("reply"));
    frames += until(client, run);
    EXPECT_EQ(output(frames, "stdout"), QStringLiteral("start\ndone\n"));
    for (const QJsonObject& f : std::as_const(frames)) {
        if (f.value(QStringLiteral("type")) != QLatin1String("event")) continue;
        EXPECT_FALSE(f.contains(QStringLiteral("subscriptionId")));
        EXPECT_EQ(f.value(QStringLiteral("target")), QStringLiteral("commands"));
        EXPECT_EQ(f.value(QStringLiteral("args")).toArray().at(0), QStringLiteral("r1"));
    }
    EXPECT_EQ(exitOf(last(frames)), 0);
    // The other client got nothing.
    EXPECT_FALSE(next(other, 200));

    const auto unknown = call(client, QStringLiteral("run"), {QStringLiteral("r2"), QStringLiteral("cmds"), QStringLiteral("nope"),
                                                              QJsonArray{}, QString(), 0});
    EXPECT_EQ(unknown.value(QStringLiteral("code")), QLatin1String(CommandSocket::NoSuchCommand));
    const auto badPane = call(client, QStringLiteral("run"), {QStringLiteral("r3"), QStringLiteral("cmds"), QStringLiteral("greet"),
                                                              QJsonArray{}, QString(), 99});
    EXPECT_EQ(badPane.value(QStringLiteral("message")), QStringLiteral("no pane 99"));
    // The directory is the client's: empty or absolute.
    const auto relative = call(client, QStringLiteral("run"), {QStringLiteral("r4"), QStringLiteral("cmds"), QStringLiteral("where"),
                                                               QJsonArray{}, QStringLiteral("work"), 0});
    EXPECT_EQ(relative.value(QStringLiteral("type")), QStringLiteral("error"));
    EXPECT_TRUE(relative.value(QStringLiteral("message")).toString().contains(QStringLiteral("absolute"))) << qPrintable(relative.value(QStringLiteral("message")).toString());
}

TEST_F(CommandSocketTest, StdinOverTheLimitIsRefusedNotCut)
{
    ASSERT_TRUE(start(R"({"Scripts": {"commandStdinLimit": 1}})"));
    QLocalSocket client;
    ASSERT_TRUE(connect(client));
    auto input = [&](const QString& run, const QString& text, bool eof) {
        return call(client, QStringLiteral("input"), {run, text, eof});
    };
    // Exactly at the limit of 1 KiB, in UTF-8 bytes, is taken whole.
    EXPECT_TRUE(input(QStringLiteral("in"), QString(1000, QLatin1Char('a')), false).value(QStringLiteral("result")).toBool());
    EXPECT_TRUE(input(QStringLiteral("in"), QString(24, QLatin1Char('b')), false).value(QStringLiteral("result")).toBool());
    EXPECT_TRUE(input(QStringLiteral("in"), QString(), true).value(QStringLiteral("result")).toBool());
    EXPECT_EQ(input(QStringLiteral("in"), QStringLiteral("k"), true).value(QStringLiteral("type")), QStringLiteral("error"));
    const auto frames = until(client, send(client, QStringLiteral("run"), {QStringLiteral("in"), QStringLiteral("cmds"),
                                                                           QStringLiteral("read"), QJsonArray{}, QString(), 0}));
    EXPECT_EQ(output(frames, "stdout"), QString(1000, QLatin1Char('a')) + QString(24, QLatin1Char('b')));
    EXPECT_EQ(m_commands->waitingInputBytes(), 0);

    // One byte more is refused with its own code, and the run's input goes.
    EXPECT_TRUE(input(QStringLiteral("over"), QString(512, QChar(0xe9)), false).value(QStringLiteral("result")).toBool());
    EXPECT_EQ(m_commands->waitingInputBytes(), 1024);
    const QJsonObject over = input(QStringLiteral("over"), QStringLiteral("x"), false);
    EXPECT_EQ(over.value(QStringLiteral("type")), QStringLiteral("error"));
    EXPECT_EQ(over.value(QStringLiteral("code")), QLatin1String(CommandSocket::InputTooLarge));
    EXPECT_EQ(over.value(QStringLiteral("message")), QStringLiteral("Standard input is over 1 KiB (Scripts.commandStdinLimit)"));
    EXPECT_EQ(m_commands->waitingInputs(), 0);
    EXPECT_EQ(m_commands->waitingInputBytes(), 0);

    // Input to a run that has started is refused.
    const qint64 waiting = send(client, QStringLiteral("run"), {QStringLiteral("w"), QStringLiteral("cmds"), QStringLiteral("wait"),
                                                              QJsonArray{QStringLiteral("60000")}, QString(), 0});
    ASSERT_TRUE(waitFor([&] { return client.canReadLine(); })); // its first output
    const QJsonObject started = input(QStringLiteral("w"), QStringLiteral("x"), false);
    EXPECT_EQ(started.value(QStringLiteral("type")), QStringLiteral("error"));
    EXPECT_TRUE(started.value(QStringLiteral("message")).toString().contains(QStringLiteral("has started")))
        << qPrintable(started.value(QStringLiteral("message")).toString());
    send(client, QStringLiteral("cancel"), {QStringLiteral("w")});
    EXPECT_EQ(exitOf(last(until(client, waiting))), 130);

    // A command without stdin gets none, also when the client sent some.
    EXPECT_TRUE(input(QStringLiteral("p"), QStringLiteral("x"), true).value(QStringLiteral("result")).toBool());
    const auto plain = until(client, send(client, QStringLiteral("run"), {QStringLiteral("p"), QStringLiteral("cmds"),
                                                                          QStringLiteral("plain"), QJsonArray{}, QString(), 0}));
    EXPECT_EQ(output(plain, "stdout"), QStringLiteral("plain nil\n"));

    // A limit of 0 refuses any input, and takes none.
    m_settings->write(QStringLiteral("Scripts"), QStringLiteral("commandStdinLimit"), 0);
    EXPECT_EQ(input(QStringLiteral("z"), QStringLiteral("x"), true).value(QStringLiteral("code")), QLatin1String(CommandSocket::InputTooLarge));
    EXPECT_TRUE(input(QStringLiteral("e"), QString(), true).value(QStringLiteral("result")).toBool());
}

TEST_F(CommandSocketTest, ARunIdBelongsToItsConnection)
{
    ASSERT_TRUE(start());
    QLocalSocket client, other;
    ASSERT_TRUE(connect(client));
    ASSERT_TRUE(connect(other));
    // Long enough that only the cancel ends it.
    const qint64 first = send(client, QStringLiteral("run"), {QStringLiteral("same"), QStringLiteral("cmds"), QStringLiteral("wait"),
                                                            QJsonArray{QStringLiteral("60000")}, QString(), 0});
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
    EXPECT_EQ(exitOf(last(frames)), 130);
    EXPECT_FALSE(output(frames, "stdout").contains(QStringLiteral("done")));
    // An ack names a run of this connection, and a count of bytes.
    EXPECT_EQ(call(client, QStringLiteral("ack"), {QStringLiteral("same"), 10}).value(QStringLiteral("type")), QStringLiteral("error"));
    EXPECT_EQ(call(client, QStringLiteral("cancel"), {QStringLiteral("same")}).value(QStringLiteral("type")), QStringLiteral("error"));
    const qint64 held = send(client, QStringLiteral("run"), {QStringLiteral("held"), QStringLiteral("cmds"), QStringLiteral("wait"),
                                                           QJsonArray{QStringLiteral("60000")}, QString(), 0});
    ASSERT_TRUE(waitFor([&] { return client.canReadLine(); }));
    const QJsonObject negative = call(client, QStringLiteral("ack"), {QStringLiteral("held"), -1});
    EXPECT_EQ(negative.value(QStringLiteral("message")), QStringLiteral("ack takes a number of bytes"));
    EXPECT_EQ(call(other, QStringLiteral("ack"), {QStringLiteral("held"), 10}).value(QStringLiteral("type")), QStringLiteral("error"));
    send(client, QStringLiteral("cancel"), {QStringLiteral("held")});
    EXPECT_EQ(exitOf(last(until(client, held))), 130);
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
    for (const char* group : {"Control", "Scripts"}) {
        const qint64 run = send(client, QStringLiteral("run"), {QLatin1String(group), QStringLiteral("cmds"), QStringLiteral("wait"),
                                                              QJsonArray{QStringLiteral("60000")}, QString(), 0});
        ASSERT_TRUE(waitFor([&] { return client.canReadLine(); }));
        // Turning either off ends the target and fails the run that waits,
        // with the reason, before the scripts stop.
        m_settings->write(QLatin1String(group), QStringLiteral("enabled"), false);
        EXPECT_FALSE(m_commands->enabled());
        const QJsonObject failed = last(until(client, run));
        EXPECT_EQ(failed.value(QStringLiteral("type")), QStringLiteral("error"));
        EXPECT_TRUE(failed.value(QStringLiteral("message")).toString().contains(QStringLiteral("commands are off")))
            << qPrintable(failed.value(QStringLiteral("message")).toString());
        EXPECT_EQ(call(client, QStringLiteral("list")).value(QStringLiteral("code")), QStringLiteral("NO_SUCH_TARGET"));
        EXPECT_EQ(m_scripts->isEnabled(), QLatin1String(group) == QLatin1String("Control"));
        m_settings->write(QLatin1String(group), QStringLiteral("enabled"), true);
        EXPECT_TRUE(m_commands->enabled());
        EXPECT_TRUE(m_scripts->isEnabled());
    }
}

TEST_F(CommandSocketTest, ASlowReaderHoldsTheRunBackAndKeepsItsConnection)
{
    ASSERT_TRUE(start(R"({"Scripts": {"commandOutputLimit": 65536}})"));
    QLocalSocket client, other;
    ASSERT_TRUE(connect(client));
    ASSERT_TRUE(connect(other));
    // The client reads nothing more: what the terminal sends piles up in the
    // router, which would close the connection past 16 MiB.
    client.setReadBufferSize(64 * 1024);
    const qint64 run = send(client, QStringLiteral("run"), {QStringLiteral("flood"), QStringLiteral("cmds"), QStringLiteral("flood"),
                                                          QJsonArray{QStringLiteral("20")}, QString(), 0});
    // WindowParts lines of a part and a little fill the window. The next part waits.
    ASSERT_TRUE(waitFor([this] { return m_commands->waitingOutputBytes() > 0; }));
    EXPECT_EQ(client.state(), QLocalSocket::ConnectedState);
    const int parts = output(until(other, send(other, QStringLiteral("run"), {QStringLiteral("c"), QStringLiteral("cmds"),
                                                                            QStringLiteral("count"), QJsonArray{}, QString(), 0})),
                             "stdout").toInt();
    EXPECT_EQ(parts, WindowParts);
    // Reading and acknowledging, as aglow run does, lets it go on.
    const Drained got = drain(client, {run}, true);
    EXPECT_EQ(got.out.size(), 20 * 1048576 + 5);
    EXPECT_EQ(exitOf(got.answers.value(run)), 0);
    EXPECT_EQ(client.state(), QLocalSocket::ConnectedState);
    EXPECT_EQ(m_commands->waitingOutputBytes(), 0);
}

TEST_F(CommandSocketTest, TheRunsOfAConnectionShareItsWindow)
{
    ASSERT_TRUE(start());
    QLocalSocket client, other;
    ASSERT_TRUE(connect(client));
    ASSERT_TRUE(connect(other));
    client.setReadBufferSize(64 * 1024);
    const qint64 a = send(client, QStringLiteral("run"), {QStringLiteral("a"), QStringLiteral("cmds"), QStringLiteral("flood"),
                                                        QJsonArray{QStringLiteral("6")}, QString(), 0});
    const qint64 b = send(client, QStringLiteral("run"), {QStringLiteral("b"), QStringLiteral("cmds"), QStringLiteral("flood"),
                                                        QJsonArray{QStringLiteral("6")}, QString(), 0});
    // Both runs have a part that waits: the window of the connection is full.
    ASSERT_TRUE(waitFor([this] { return m_commands->waitingOutputBytes() >= 2 * 2 * 1048576; }));
    const int parts = output(until(other, send(other, QStringLiteral("run"), {QStringLiteral("c"), QStringLiteral("cmds"),
                                                                            QStringLiteral("count"), QJsonArray{}, QString(), 0})),
                             "stdout").toInt();
    EXPECT_EQ(parts, WindowParts); // not WindowParts for each run
    const Drained got = drain(client, {a, b}, true);
    EXPECT_EQ(got.out.size(), 12 * 1048576 + 10);
    EXPECT_EQ(exitOf(got.answers.value(a)), 0);
    EXPECT_EQ(exitOf(got.answers.value(b)), 0);
}

TEST_F(CommandSocketTest, ManySmallRunsFillTheWindowAndTheClientAcknowledgesTheirTotal)
{
    // Two scripts, as one runs at most 16 commands at once.
    const QByteArray quarter = R"(
return {
    metadata = { commands = { quarter = { summary = "Write 250 KiB, wait MS, then write done", usage = "MS" } } },
    run_command = function(command, ev)
        assert(afterglow.await(afterglow.output(ev, string.rep("q", 250 * 1024))))
        afterglow.sleep(tonumber(ev.args[1]))
        return afterglow.output(ev, "done\n")
    end,
}
)";
    for (const char* id : {"wide1", "wide2"}) {
        QFile file(scripts() + QStringLiteral("/%1.luau").arg(QLatin1String(id)));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        file.write(quarter);
    }
    ASSERT_TRUE(start());
    QLocalSocket client;
    ASSERT_TRUE(connect(client));
    // Twenty runs of a line each just under 256 KiB: seventeen lines pass
    // 4 MiB, and no run alone has 256 KiB to acknowledge. Each run stays,
    // with its share of the window, until its "done" goes.
    QList<qint64> runs;
    for (int i = 0; i < 20; ++i) {
        runs << send(client, QStringLiteral("run"), {QStringLiteral("q%1").arg(i), i % 2 ? QStringLiteral("wide2") : QStringLiteral("wide1"),
                                                   QStringLiteral("quarter"), QJsonArray{QStringLiteral("500")}, QString(), 0});
    }
    ASSERT_TRUE(waitFor([this] { return m_commands->waitingOutputBytes() > 0; }));
    const Drained got = drain(client, runs, true, 10000);
    EXPECT_EQ(got.out.size(), 20 * (250 * 1024 + 5));
    for (const qint64 run : std::as_const(runs)) EXPECT_EQ(exitOf(got.answers.value(run)), 0);
    EXPECT_EQ(m_commands->waitingOutputBytes(), 0);
}

TEST_F(CommandSocketTest, ACancelDropsTheOutputThatWaits)
{
    ASSERT_TRUE(start(R"({"Scripts": {"commandOutputLimit": 65536}})"));
    QLocalSocket client;
    ASSERT_TRUE(connect(client));
    client.setReadBufferSize(64 * 1024);
    // A run that waits for its output to go. Without the drop, its reply
    // would wait for acknowledgments that never come.
    const qint64 flood = send(client, QStringLiteral("run"), {QStringLiteral("f"), QStringLiteral("cmds"), QStringLiteral("flood"),
                                                            QJsonArray{QStringLiteral("20")}, QString(), 0});
    ASSERT_TRUE(waitFor([this] { return m_commands->waitingOutputBytes() > 0; }));
    send(client, QStringLiteral("cancel"), {QStringLiteral("f")});
    Drained got = drain(client, {flood}, false);
    EXPECT_EQ(exitOf(got.answers.value(flood)), 130);
    EXPECT_EQ(got.out.size(), WindowParts * Part);
    EXPECT_EQ(m_commands->waitingOutputBytes(), 0);

    // A run that ended, with its output waiting, keeps its own status.
    client.setReadBufferSize(64 * 1024);
    const qint64 spew = send(client, QStringLiteral("run"), {QStringLiteral("s"), QStringLiteral("cmds"), QStringLiteral("spew"),
                                                           QJsonArray{QStringLiteral("8")}, QString(), 0});
    ASSERT_TRUE(waitFor([this] { return m_commands->waitingOutputBytes() >= 4 * 2 * 1048576; }));
    send(client, QStringLiteral("cancel"), {QStringLiteral("s")});
    got = drain(client, {spew}, false);
    EXPECT_EQ(exitOf(got.answers.value(spew)), 3);
    EXPECT_EQ(got.out.size(), WindowParts * Part);
    EXPECT_EQ(m_commands->waitingOutputBytes(), 0);
}

TEST_F(CommandSocketTest, AnEmptyWriteKeepsTheResultOfTheWriteBefore)
{
    ASSERT_TRUE(start());
    QLocalSocket client;
    ASSERT_TRUE(connect(client));
    const Drained got = drain(client, {send(client, QStringLiteral("run"), {QStringLiteral("t"), QStringLiteral("cmds"),
                                                                          QStringLiteral("twin"), QJsonArray{}, QString(), 0})},
                              true);
    EXPECT_EQ(got.out.size(), 5 * 1048576);
    // Both results come, in the order of the writes.
    QString results;
    ASSERT_TRUE(waitFor([&] {
        results = output(until(client, send(client, QStringLiteral("run"), {QStringLiteral("r") + QString::number(m_id),
                                                                          QStringLiteral("cmds"), QStringLiteral("results"),
                                                                          QJsonArray{}, QString(), 0})),
                         "stdout");
        return results.contains(QStringLiteral("empty"));
    })) << qPrintable(results);
    EXPECT_EQ(results, QStringLiteral("big=true empty=true"));
}

TEST_F(CommandSocketTest, WaitingOutputHasACapForTheWholeProcess)
{
    ASSERT_TRUE(start(R"({"Scripts": {"commandOutputLimit": 65536}})"));
    m_commands->setMaxWaitingOutputBytes(3 * 1048576);
    QLocalSocket client;
    ASSERT_TRUE(connect(client));
    client.setReadBufferSize(64 * 1024);
    // Four parts go, the fifth waits, and the sixth would pass the cap: the
    // run ends with status 1 and says why, before its reply.
    const qint64 spew = send(client, QStringLiteral("run"), {QStringLiteral("s"), QStringLiteral("cmds"), QStringLiteral("spew"),
                                                           QJsonArray{QStringLiteral("8")}, QString(), 0});
    const Drained got = drain(client, {spew}, true);
    EXPECT_EQ(exitOf(got.answers.value(spew)), 1);
    EXPECT_EQ(got.out.size(), 5 * 1048576);
    EXPECT_TRUE(got.err.contains(QStringLiteral("over 3 MiB"))) << qPrintable(got.err);
    EXPECT_EQ(m_commands->waitingOutputBytes(), 0);
}

TEST_F(CommandSocketTest, AClientWithoutAcknowledgmentsThatReadsNothingIsClosed)
{
    ASSERT_TRUE(start(R"({"Scripts": {"commandRunLimit": 1, "commandOutputLimit": 65536}})"));
    QLocalSocket client, other;
    ASSERT_TRUE(connect(client));
    ASSERT_TRUE(connect(other));
    // A call of the schema entry sends without a window: the router closes
    // the connection once 16 MiB wait for it, and the run ends.
    client.setReadBufferSize(64 * 1024);
    send(client, QStringLiteral("cmds.flood"), QJsonArray{QJsonValue(QJsonArray{QStringLiteral("20")})});
    ASSERT_TRUE(waitFor([&] {
        const auto reply = call(other, QStringLiteral("run"), {QStringLiteral("g") + QString::number(m_id), QStringLiteral("cmds"),
                                                               QStringLiteral("greet"), QJsonArray{}, QString(), 0});
        return reply.value(QStringLiteral("type")) == QLatin1String("reply");
    }, 20000));
    const int parts = output(until(other, send(other, QStringLiteral("run"), {QStringLiteral("c"), QStringLiteral("cmds"),
                                                                            QStringLiteral("count"), QJsonArray{}, QString(), 0})),
                             "stdout").toInt();
    EXPECT_GE(parts, 15);
    EXPECT_LT(parts, 20);
    EXPECT_EQ(m_commands->waitingOutputBytes(), 0);
}

TEST_F(CommandSocketTest, WaitingStdinHasACapForTheWholeProcess)
{
    ASSERT_TRUE(start(R"({"Scripts": {"commandStdinLimit": 4096}})"));
    constexpr qint64 Limit = 4096 * 1024;
    QLocalSocket client, other;
    ASSERT_TRUE(connect(client));
    ASSERT_TRUE(connect(other));
    // Each run takes its limit, in parts that fit in a request line. False
    // after a part was refused, for the test to stop there.
    auto fill = [&](QLocalSocket& socket, const QString& run) {
        for (qint64 sent = 0; sent < Limit;) {
            const qint64 size = std::min<qint64>(1000000, Limit - sent);
            const QJsonObject reply = call(socket, QStringLiteral("input"), {run, QString(size, QLatin1Char('x')), false});
            if (reply.value(QStringLiteral("type")) != QLatin1String("reply")) {
                ADD_FAILURE() << qPrintable(run) << ": " << qPrintable(reply.value(QStringLiteral("message")).toString());
                return false;
            }
            sent += size;
        }
        return true;
    };
    // Fifteen runs on one connection and one on another: exactly the cap.
    for (int run = 0; run < 15; ++run) ASSERT_TRUE(fill(client, QStringLiteral("a%1").arg(run)));
    ASSERT_TRUE(fill(other, QStringLiteral("b0")));
    EXPECT_EQ(m_commands->waitingInputBytes(), CommandSocket::MaxWaitingInputBytes);
    const QJsonObject over = call(other, QStringLiteral("input"), {QStringLiteral("b1"), QStringLiteral("x"), false});
    EXPECT_EQ(over.value(QStringLiteral("type")), QStringLiteral("error"));
    EXPECT_TRUE(over.value(QStringLiteral("message")).toString().contains(QStringLiteral("at most 64 MiB")));
    // The refused run left nothing that waits.
    EXPECT_EQ(m_commands->waitingInputs(), 16);
    // Sixteen runs of one connection can wait, not seventeen.
    EXPECT_EQ(call(client, QStringLiteral("input"), {QStringLiteral("a15"), QString(), false}).value(QStringLiteral("type")),
              QStringLiteral("reply"));
    const QJsonObject many = call(client, QStringLiteral("input"), {QStringLiteral("a16"), QString(), false});
    EXPECT_TRUE(many.value(QStringLiteral("message")).toString().contains(QStringLiteral("16 runs wait"))) << qPrintable(many.value(QStringLiteral("message")).toString());
    // A cancel and a closed connection give the room back.
    call(client, QStringLiteral("cancel"), {QStringLiteral("a0")});
    EXPECT_EQ(m_commands->waitingInputBytes(), 15 * Limit);
    client.disconnectFromServer();
    ASSERT_TRUE(waitFor([&] { return m_commands->waitingInputBytes() == Limit; }));
    other.disconnectFromServer();
    ASSERT_TRUE(waitFor([&] { return m_commands->waitingInputBytes() == 0; }));
}

TEST_F(CommandSocketTest, AScriptCommandActionRunsWithoutAPane)
{
    ASSERT_TRUE(start());
    QStringList started;
    m_control->setScriptCommandRunner([&started](const QString& command, quint32 pane, const QString& source) {
        started << QStringLiteral("%1 %2 %3").arg(command).arg(pane).arg(source);
        return QString();
    });
    // No pane has the focus: as for aglow run, the command runs with none.
    EXPECT_EQ(m_control->runAction(0, QStringLiteral("script:cmds.where")), QString());
    // A script cannot start one: its run would look like one from afterglow ctl.
    EXPECT_EQ(m_control->runScriptAction(0, QStringLiteral("script:cmds.where")), QStringLiteral("scripts cannot run script commands"));
    EXPECT_EQ(started, (QStringList{QStringLiteral("cmds.where 0 control")}));
    // A pane that is not there, and a target without a command, are errors.
    EXPECT_EQ(m_control->runAction(5, QStringLiteral("script:cmds.where")), QStringLiteral("no pane 5"));
    EXPECT_FALSE(m_control->runAction(0, QStringLiteral("script:cmds")).isEmpty());
    EXPECT_FALSE(m_control->runAction(0, QStringLiteral("script:cmds.")).isEmpty());
    EXPECT_EQ(started.size(), 1);
    // A keybind action needs its pane.
    EXPECT_EQ(m_control->runAction(0, QStringLiteral("new_tab")), QStringLiteral("no pane 0"));
    m_control->setScriptCommandRunner({}); // it holds this test's list
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
        ASSERT_EQ(list->toArray().size(), 10u);
        EXPECT_EQ(list->toArray()[0]["command"].toString(), "count");

        // Stdin before the run, then a deferred reply after the run's events.
        const auto taken = ipc.call("commands", "input", {"id1", "from stdin \x01 é", true}, 5000, &error);
        ASSERT_TRUE(taken) << error.message;
        EXPECT_TRUE(taken->toBool());
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
        const auto waited = IpcConnection::answer(frame, &error);
        ASSERT_TRUE(waited) << error.message;
        EXPECT_EQ((*waited)["exit"].toInteger(), 0);

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

/// `aglow run`, the binary, against this test's socket. m_throughAfterglow
/// makes it `afterglow run`, which runs the aglow next to it.
class RunBinaryTest : public CommandSocketTest
{
protected:
    void SetUp() override
    {
#ifndef AFTERGLOW_AGLOW
        GTEST_SKIP() << "this build has no aglow (AFTERGLOW_BUILD_AGLOW=OFF)";
#else
        CommandSocketTest::SetUp();
#endif
    }

    /// The aglow of this build.
    static QString aglow()
    {
#ifdef AFTERGLOW_AGLOW
        return QStringLiteral(AFTERGLOW_AGLOW);
#else
        return {};
#endif
    }

    /// The program and its first arguments.
    QStringList command() const
    {
        if (m_throughAfterglow) return {m_afterglow, QStringLiteral("run")};
        return {aglow(), QStringLiteral("run")};
    }

    /// Starts @p process with @p args after the command.
    void start(QProcess& process, const QStringList& args) const
    {
        const QStringList words = command();
        process.start(words.first(), words.mid(1) + args);
    }
    using CommandSocketTest::start;

    struct Result
    {
        int status = -1; ///< as a shell reports it: 128 + the signal that ended the process
        int signal = 0; ///< the signal that ended the process, else 0
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
        start(process, args);
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
        // QProcess gives the signal that ended a process as its exit code.
        const int signal = process.exitStatus() == QProcess::CrashExit ? process.exitCode() : 0;
        return {signal ? 128 + signal : process.exitCode(), signal, QString::fromUtf8(process.readAllStandardOutput()),
                QString::fromUtf8(process.readAllStandardError())};
    }

    bool m_throughAfterglow = false;
    /// The afterglow that m_throughAfterglow runs.
    QString m_afterglow = QStringLiteral(AFTERGLOW_BINARY);
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
    EXPECT_TRUE(r.out.startsWith(QStringLiteral("Usage: aglow run"))) << qPrintable(r.out);
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

TEST_F(RunBinaryTest, ASignalCancelsTheRunAndEndsTheClient)
{
    ASSERT_TRUE(start(R"({"Scripts": {"commandRunLimit": 1}})"));
    // The client ends by the signal, after the cancel: a shell loop of runs
    // stops at Ctrl+C, and the shell reports 130 or 143.
    for (const auto& [signal, status] : {std::pair{SIGINT, 130}, std::pair{SIGTERM, 143}}) {
        const Result r = run({QStringLiteral("cmds"), QStringLiteral("wait"), QStringLiteral("60000")}, {}, false, {},
                             [&, signal = signal](QProcess& process) {
                                 ASSERT_TRUE(pumpUntil([&] { return process.canReadLine(); }));
                                 ::kill(pid_t(process.processId()), signal);
                             });
        EXPECT_EQ(r.signal, signal);
        EXPECT_EQ(r.status, status);
        EXPECT_EQ(r.out, QStringLiteral("start\n"));
        // The run ended in the terminal too: with a limit of one, another
        // starts. The client sends its cancel without waiting for the
        // terminal, so the next pass waits until the run has ended there.
        ASSERT_TRUE(pumpUntil([&] { return run({QStringLiteral("cmds"), QStringLiteral("greet")}).status == 0; }));
    }
}

TEST_F(RunBinaryTest, AcknowledgesOutputPastTheWindow)
{
    ASSERT_TRUE(start(R"({"Scripts": {"commandOutputLimit": 65536}})"));
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
    // Less than the limit of 1 MiB: past it, the run would fail first.
    const Result r = run({QStringLiteral("cmds"), QStringLiteral("read")}, QByteArray(600 * 1024, 'x'), true, {},
                         [this](QProcess& process) {
                             ASSERT_TRUE(pumpUntil([this] { return m_commands->waitingInputBytes() > 0; }));
                             ::kill(pid_t(process.processId()), SIGTERM);
                         });
    EXPECT_EQ(r.signal, SIGTERM);
    EXPECT_EQ(r.status, 143);
    ASSERT_TRUE(pumpUntil([this] { return m_commands->waitingInputBytes() == 0; }));
}

TEST_F(RunBinaryTest, AClosedStdoutEndsTheRunAndTheClientBySigpipe)
{
    ASSERT_TRUE(start(R"({"Scripts": {"commandRunLimit": 1, "commandOutputLimit": 65536}})"));
    // head reads one byte, then closes the pipe that the client writes to.
    QProcess reader, process;
    reader.setStandardOutputFile(QProcess::nullDevice());
    process.setProcessEnvironment(environment());
    process.setStandardOutputProcess(&reader);
    // As a shell starts it: the test process may ignore SIGPIPE.
    process.setChildProcessModifier([] { ::signal(SIGPIPE, SIG_DFL); });
    start(process, {QStringLiteral("cmds"), QStringLiteral("flood"), QStringLiteral("20")});
    reader.start(QStringLiteral("head"), {QStringLiteral("-c"), QStringLiteral("1")});
    ASSERT_TRUE(process.waitForStarted());
    process.closeWriteChannel();
    ASSERT_TRUE(pumpUntil([&] { return process.state() == QProcess::NotRunning; }, 15000));
    EXPECT_EQ(process.exitStatus(), QProcess::CrashExit);
    EXPECT_EQ(process.exitCode(), SIGPIPE);
    reader.waitForFinished();
    // The run ended in the terminal too: with a limit of one, another starts.
    ASSERT_TRUE(pumpUntil([&] { return run({QStringLiteral("cmds"), QStringLiteral("greet")}).status == 0; }));
}

TEST_F(RunBinaryTest, StdinOverTheLimitIsAUsageError)
{
    ASSERT_TRUE(start(R"({"Scripts": {"commandStdinLimit": 1}})"));
    Result r = run({QStringLiteral("cmds"), QStringLiteral("read")}, QByteArray(1025, 'x'));
    EXPECT_EQ(r.status, 2);
    EXPECT_TRUE(r.err.contains(QStringLiteral("Standard input is over 1 KiB (Scripts.commandStdinLimit)"))) << qPrintable(r.err);
    EXPECT_TRUE(r.out.isEmpty()) << qPrintable(r.out);
    // Exactly the limit is taken whole.
    r = run({QStringLiteral("cmds"), QStringLiteral("read")}, QByteArray(1024, 'x'));
    EXPECT_EQ(r.status, 0) << qPrintable(r.err);
    EXPECT_EQ(r.out, QString(1024, QLatin1Char('x')));
    EXPECT_EQ(m_commands->waitingInputs(), 0);
}

TEST_F(RunBinaryTest, AnInheritedIgnoredSigintStaysIgnored)
{
    ASSERT_TRUE(start());
    QProcess process;
    process.setProcessEnvironment(environment());
    process.setChildProcessModifier([] { ::signal(SIGINT, SIG_IGN); });
    start(process, {QStringLiteral("cmds"), QStringLiteral("wait"), QStringLiteral("60000")});
    ASSERT_TRUE(process.waitForStarted());
    process.closeWriteChannel();
    ASSERT_TRUE(pumpUntil([&] { return process.canReadLine(); }));
    ::kill(pid_t(process.processId()), SIGINT);
    EXPECT_FALSE(pumpUntil([&] { return process.state() == QProcess::NotRunning; }, 500));
    ::kill(pid_t(process.processId()), SIGTERM);
    ASSERT_TRUE(pumpUntil([&] { return process.state() == QProcess::NotRunning; }));
    EXPECT_EQ(process.exitStatus(), QProcess::CrashExit);
    EXPECT_EQ(process.exitCode(), SIGTERM);
}

TEST_F(RunBinaryTest, ArgumentsOverOneRequestAreAUsageError)
{
    ASSERT_TRUE(start());
    // 100 arguments within their length, about 1.2 MiB of UTF-8 together:
    // over one request, and far below the kernel's limit on argv for exec.
    QStringList args{QStringLiteral("cmds"), QStringLiteral("greet")};
    for (int i = 0; i < 100; ++i) args << QString(4000, QChar(0x4e00));
    Result r = run(args);
    EXPECT_EQ(r.status, 2);
    EXPECT_TRUE(r.err.contains(QStringLiteral("make a request over 1 MiB"))) << qPrintable(r.err);
    // One argument more than the terminal takes.
    QStringList many{QStringLiteral("cmds"), QStringLiteral("greet")};
    for (int i = 0; i < 257; ++i) many << QStringLiteral("x");
    r = run(many);
    EXPECT_EQ(r.status, 2);
    EXPECT_TRUE(r.err.contains(QStringLiteral("256 arguments"))) << qPrintable(r.err);
}

TEST_F(RunBinaryTest, SaysWhenCommandsAreOff)
{
    ASSERT_TRUE(start(R"({"Scripts": {"enabled": false}})"));
    const Result r = run({QStringLiteral("cmds"), QStringLiteral("greet")});
    EXPECT_EQ(r.status, 1);
    EXPECT_TRUE(r.err.contains(QStringLiteral("Scripts.enabled is off"))) << qPrintable(r.err);
}

TEST_F(RunBinaryTest, AfterglowRunRunsAglowRun)
{
    ASSERT_TRUE(start(R"({"Scripts": {"commandRunLimit": 1}})"));
    m_throughAfterglow = true;
    // The status, the output and the arguments pass through the exec.
    Result r = run({QStringLiteral("cmds"), QStringLiteral("greet"), QStringLiteral("you"), QStringLiteral("7")});
    EXPECT_EQ(r.status, 7);
    EXPECT_EQ(r.out, QStringLiteral("hello you\n"));
    EXPECT_EQ(r.err, QStringLiteral("note\n"));
    r = run({QStringLiteral("cmds"), QStringLiteral("read")}, "abc");
    EXPECT_EQ(r.out, QStringLiteral("abc"));
    // Usage and messages name afterglow run, the command that ran aglow.
    r = run({QStringLiteral("--help")});
    EXPECT_EQ(r.status, 0);
    EXPECT_TRUE(r.out.startsWith(QStringLiteral("Usage: afterglow run "))) << qPrintable(r.out);
    r = run({QStringLiteral("nope"), QStringLiteral("greet")});
    EXPECT_EQ(r.status, 2);
    EXPECT_TRUE(r.err.startsWith(QStringLiteral("afterglow run: no script nope with commands (afterglow run lists them)")))
        << qPrintable(r.err);
    r = run({QStringLiteral("--complete")});
    EXPECT_EQ(r.out, QStringLiteral("cmds\n"));
    // The exec keeps the process: a signal reaches aglow, which cancels the run and ends by it.
    r = run({QStringLiteral("cmds"), QStringLiteral("wait"), QStringLiteral("60000")}, {}, false, {}, [&](QProcess& process) {
        ASSERT_TRUE(pumpUntil([&] { return process.canReadLine(); }));
        // The process runs aglow itself, not a client inside afterglow.
        EXPECT_EQ(QFileInfo(QStringLiteral("/proc/%1/exe").arg(process.processId())).canonicalFilePath(),
                  QFileInfo(aglow()).canonicalFilePath());
        ::kill(pid_t(process.processId()), SIGINT);
    });
    EXPECT_EQ(r.signal, SIGINT);
    EXPECT_EQ(r.status, 130);
    ASSERT_TRUE(pumpUntil([&] { return run({QStringLiteral("cmds"), QStringLiteral("greet")}).status == 0; }));
}

TEST_F(RunBinaryTest, AfterglowRunSaysWhenItCannotRunAglow)
{
    // An afterglow alone in a directory: a hard link beside the build's, or
    // a copy on another file system. It runs the aglow beside it, never one
    // on PATH, and ends before it looks for a socket.
    const QFileInfo binary(QStringLiteral(AFTERGLOW_BINARY));
    QTemporaryDir lone(binary.absolutePath() + QStringLiteral("/afterglow-exec-test-XXXXXX"));
    ASSERT_TRUE(lone.isValid());
    m_afterglow = lone.filePath(QStringLiteral("afterglow"));
    if (::link(QFile::encodeName(binary.filePath()).constData(), QFile::encodeName(m_afterglow).constData()) != 0) {
        ASSERT_TRUE(QFile::copy(binary.filePath(), m_afterglow));
    }
    m_throughAfterglow = true;
    // The path in the message: beside afterglow's own executable, links resolved.
    const QString missing = QFileInfo(lone.path()).canonicalFilePath() + QStringLiteral("/aglow");
    Result r = run({QStringLiteral("cmds"), QStringLiteral("greet")});
    EXPECT_EQ(r.status, 127);
    EXPECT_TRUE(r.err.startsWith(QStringLiteral("afterglow run: cannot run %1: ").arg(missing))) << qPrintable(r.err);
    EXPECT_TRUE(r.out.isEmpty()) << qPrintable(r.out);
    // An aglow that is there and cannot run, as it has no execute permission.
    QFile file(lone.filePath(QStringLiteral("aglow")));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("#!/bin/sh\nexit 0\n");
    file.close();
    ASSERT_TRUE(file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner));
    r = run({QStringLiteral("cmds"), QStringLiteral("greet")});
    EXPECT_EQ(r.status, 126);
    EXPECT_TRUE(r.err.startsWith(QStringLiteral("afterglow run: cannot run %1: ").arg(missing))) << qPrintable(r.err);
}

TEST_F(RunBinaryTest, FindsTheSocketOfTheDisplayWithoutAfterglowSocket)
{
    // The main socket of a display, where the terminal listens, in the
    // test's runtime directory: never the user's.
    Saved waylandDisplay("WAYLAND_DISPLAY");
    qputenv("WAYLAND_DISPLAY", "agtest-1");
    const QFileInfo shared(InstanceSocket::sharedPath());
    ASSERT_EQ(shared.path(), m_runtime);
    m_socketName = shared.fileName();
    ASSERT_TRUE(start());
    QProcessEnvironment display = environment();
    display.remove(QStringLiteral("AFTERGLOW_SOCKET"));
    display.insert(QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("agtest-1"));
    Result r = run({QStringLiteral("cmds"), QStringLiteral("greet"), QStringLiteral("you")}, {}, false, display);
    EXPECT_EQ(r.status, 0) << qPrintable(r.err);
    EXPECT_EQ(r.out, QStringLiteral("hello you\n"));
    // Wayland reads a relative display in XDG_RUNTIME_DIR: both spellings name one socket.
    display.insert(QStringLiteral("WAYLAND_DISPLAY"), m_runtime + QStringLiteral("/agtest-1"));
    r = run({QStringLiteral("cmds"), QStringLiteral("greet"), QStringLiteral("you")}, {}, false, display);
    EXPECT_EQ(r.status, 0) << qPrintable(r.err);
    // Another display has no afterglow.
    display.insert(QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("agtest-2"));
    r = run({QStringLiteral("cmds"), QStringLiteral("greet")}, {}, false, display);
    EXPECT_EQ(r.status, 1);
    EXPECT_TRUE(r.err.contains(QStringLiteral("no running Afterglow on this display"))) << qPrintable(r.err);
    // $AFTERGLOW_SOCKET comes first, whatever the display.
    QProcessEnvironment own = display;
    own.insert(QStringLiteral("AFTERGLOW_SOCKET"), socketPath());
    r = run({QStringLiteral("cmds"), QStringLiteral("greet"), QStringLiteral("you")}, {}, false, own);
    EXPECT_EQ(r.status, 0) << qPrintable(r.err);
    EXPECT_EQ(r.out, QStringLiteral("hello you\n"));
    // Without a runtime directory there is no display socket.
    display.remove(QStringLiteral("XDG_RUNTIME_DIR"));
    r = run({QStringLiteral("cmds"), QStringLiteral("greet")}, {}, false, display);
    EXPECT_EQ(r.status, 1);
    EXPECT_TRUE(r.err.contains(QStringLiteral("XDG_RUNTIME_DIR is not set"))) << qPrintable(r.err);
}

// A build without aglow: afterglow run says so, with the status of a
// command that is not there.
TEST(AfterglowRunWithoutAglowTest, SaysThatTheBuildHasNoAglow)
{
#ifdef AFTERGLOW_AGLOW
    GTEST_SKIP() << "this build has aglow";
#else
    // A socket that no afterglow listens on, should it look for one.
    QTemporaryDir runtime;
    ASSERT_TRUE(runtime.isValid());
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime.path());
    env.insert(QStringLiteral("AFTERGLOW_SOCKET"), runtime.filePath(QStringLiteral("none.sock")));
    env.remove(QStringLiteral("AFTERGLOW_PANE_ID"));
    QProcess process;
    process.setProcessEnvironment(env);
    process.start(QStringLiteral(AFTERGLOW_BINARY), {QStringLiteral("run"), QStringLiteral("cmds"), QStringLiteral("greet")});
    ASSERT_TRUE(process.waitForFinished(15000));
    EXPECT_EQ(process.exitStatus(), QProcess::NormalExit);
    EXPECT_EQ(process.exitCode(), 127);
    const QString err = QString::fromUtf8(process.readAllStandardError());
    EXPECT_TRUE(err.startsWith(QStringLiteral("afterglow run: this build of Afterglow has no aglow"))) << qPrintable(err);
#endif
}

/// What a program of this build did in isolate().
struct Isolated
{
    int status = -1; ///< -1 when it did not exit by itself
    QString out;
    QString err;
};

/// Runs @p program with @p args and @p input on stdin, in an environment
/// that reaches no Afterglow: a runtime directory of its own, a socket there
/// where nothing listens, no pane and no display.
Isolated isolate(const QString& program, const QStringList& args, const QByteArray& input = {})
{
    // Under /tmp: TMPDIR can be too long for a socket path.
    QTemporaryDir runtime(QStringLiteral("/tmp/afterglow-ctl-XXXXXX"));
    if (!runtime.isValid()) {
        ADD_FAILURE() << "no runtime directory";
        return {};
    }
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime.path());
    env.insert(QStringLiteral("AFTERGLOW_SOCKET"), runtime.filePath(QStringLiteral("none.sock")));
    for (const QString& name : {QStringLiteral("AFTERGLOW_PANE_ID"), QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("DISPLAY")})
        env.remove(name);
    QProcess process;
    process.setProcessEnvironment(env);
    process.start(program, args);
    if (!process.waitForStarted()) {
        ADD_FAILURE() << "cannot start " << qPrintable(program);
        return {};
    }
    process.write(input);
    process.closeWriteChannel();
    if (!process.waitForFinished(15000)) {
        process.kill();
        process.waitForFinished();
        ADD_FAILURE() << qPrintable(args.join(QLatin1Char(' '))) << " did not end";
        return {};
    }
    return {process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1, QString::fromUtf8(process.readAllStandardOutput()),
            QString::fromUtf8(process.readAllStandardError())};
}

// Usage errors and refused input end with status 2 before a connection: no
// message says that no Afterglow listens.
TEST(CtlBinaryTest, UsageErrorsExitWithTwoBeforeAnyConnection)
{
    const QString ctl = QStringLiteral("ctl");
    for (const auto& [args, message] :
         {std::pair{QStringList{ctl, QStringLiteral("focus"), QStringLiteral("--pane"), QStringLiteral("0")},
                    QStringLiteral("--pane takes a pane ID from 1")},
          std::pair{QStringList{ctl, QStringLiteral("focus"), QStringLiteral("--pane")}, QStringLiteral("--pane needs a pane ID")},
          std::pair{QStringList{ctl, QStringLiteral("split"), QStringLiteral("--left"), QStringLiteral("--right")},
                    QStringLiteral("split takes one direction")},
          std::pair{QStringList{ctl, QStringLiteral("new-tab"), QStringLiteral("--cwd")}, QStringLiteral("--cwd needs a value")},
          std::pair{QStringList{ctl, QStringLiteral("move-pane"), QStringLiteral("+3"), QStringLiteral("right")},
                    QStringLiteral("move-pane needs TARGET and PLACEMENT")},
          std::pair{QStringList{ctl, QStringLiteral("overlay"), QStringLiteral("-n")}, QStringLiteral("overlay needs a command after --")},
          std::pair{QStringList{ctl, QStringLiteral("overlay"), QStringLiteral("--capture"), QStringLiteral("stderr"), QStringLiteral("--"),
                                QStringLiteral("fzf")},
                    QStringLiteral("--capture takes stdout, file or none")}}) {
        const Isolated r = isolate(QStringLiteral(AFTERGLOW_BINARY), args);
        EXPECT_EQ(r.status, 2) << qPrintable(args.join(QLatin1Char(' '))) << qPrintable(r.err);
        EXPECT_TRUE(r.err.startsWith(QStringLiteral("afterglow ctl: %1\n\nUsage: afterglow ctl ").arg(message))) << qPrintable(r.err);
        EXPECT_TRUE(r.out.isEmpty()) << qPrintable(r.out);
    }
    // Standard input is read before the pane is looked up.
    const Isolated r = isolate(QStringLiteral(AFTERGLOW_BINARY), {ctl, QStringLiteral("send-text"), QStringLiteral("--stdin")}, "abc\xff");
    EXPECT_EQ(r.status, 2) << qPrintable(r.err);
    EXPECT_EQ(r.err, QStringLiteral("afterglow ctl: standard input is not UTF-8 text\n"));
    // So is the list of overlay, with the limit of an overlay.
    const QStringList overlay{ctl, QStringLiteral("overlay"), QStringLiteral("--"), QStringLiteral("fzf")};
    const Isolated bad = isolate(QStringLiteral(AFTERGLOW_BINARY), overlay, "abc\xff");
    EXPECT_EQ(bad.status, 2) << qPrintable(bad.err);
    EXPECT_EQ(bad.err, QStringLiteral("afterglow ctl: standard input is not UTF-8 text\n"));
    const Isolated big = isolate(QStringLiteral(AFTERGLOW_BINARY), overlay, QByteArray(4 * 1024 * 1024 + 1, 'x'));
    EXPECT_EQ(big.status, 2) << qPrintable(big.err);
    EXPECT_EQ(big.err, QStringLiteral("afterglow ctl: standard input over 4 MiB, the most that an overlay takes\n"));
}

// A current directory that was removed has no path: a relative directory
// cannot be made absolute, and ctl says so before it connects.
TEST(CtlBinaryTest, ARelativeDirectoryNeedsACurrentDirectory)
{
    QTemporaryDir parent;
    ASSERT_TRUE(parent.isValid());
    const QString script = QStringLiteral("cd \"$1\" && mkdir gone && cd gone && rmdir ../gone && shift && exec \"$@\"");
    const QString ctl = QStringLiteral("ctl");
    for (const QStringList& command :
         {QStringList{ctl, QStringLiteral("new-tab"), QStringLiteral("--cwd"), QStringLiteral("sub")},
          QStringList{ctl, QStringLiteral("new-window"), QStringLiteral("--cwd"), QStringLiteral(".")},
          QStringList{ctl, QStringLiteral("open-directory"), QStringLiteral("sub")},
          // The overlay's default directory is the current one.
          QStringList{ctl, QStringLiteral("overlay"), QStringLiteral("-n"), QStringLiteral("--"), QStringLiteral("true")}}) {
        const Isolated r = isolate(QStringLiteral("/bin/sh"),
                                   QStringList{QStringLiteral("-c"), script, QStringLiteral("sh"), parent.path(),
                                               QStringLiteral(AFTERGLOW_BINARY)}
                                       + command);
        EXPECT_EQ(r.status, 1) << qPrintable(command.join(QLatin1Char(' '))) << qPrintable(r.err);
        EXPECT_EQ(r.err, QStringLiteral("afterglow ctl: the current directory was removed, so give an absolute directory\n"))
            << qPrintable(command.join(QLatin1Char(' ')));
    }
    // An absolute directory needs none.
    const Isolated absolute = isolate(QStringLiteral("/bin/sh"),
                                      {QStringLiteral("-c"), script, QStringLiteral("sh"), parent.path(),
                                       QStringLiteral(AFTERGLOW_BINARY), ctl, QStringLiteral("new-window"), QStringLiteral("--cwd"),
                                       QStringLiteral("/")});
    EXPECT_FALSE(absolute.err.contains(QLatin1String("current directory"))) << qPrintable(absolute.err);
}

TEST(CtlBinaryTest, SaysThatNoAfterglowListens)
{
    // A valid command line connects, also with a pane given, and finds nobody.
    const QString ctl = QStringLiteral("ctl");
    for (const QStringList& args :
         {QStringList{ctl, QStringLiteral("focus"), QStringLiteral("--pane"), QStringLiteral("3")}, QStringList{ctl, QStringLiteral("ls")}}) {
        const Isolated r = isolate(QStringLiteral(AFTERGLOW_BINARY), args);
        EXPECT_EQ(r.status, 1) << qPrintable(args.join(QLatin1Char(' ')));
        EXPECT_TRUE(r.err.startsWith(QStringLiteral("afterglow ctl: no Afterglow at $AFTERGLOW_SOCKET ("))) << qPrintable(r.err);
    }
}

/// What `afterglow ctl` did against a control target of the test's own,
/// which answers each call with @p reply and the call's id, and the calls
/// it got. Afterglow's overlay needs a window: the client needs only the
/// reply. @p input goes to standard input, else @p inputFd is it.
struct AgainstTarget
{
    Isolated outcome;
    QList<QJsonObject> calls;
};

AgainstTarget ctlAgainst(const QJsonObject& reply, const QStringList& args, const QByteArray& input = {}, int inputFd = -1)
{
    AgainstTarget done;
    // Under /tmp: TMPDIR can be too long for a socket path.
    QTemporaryDir runtime(QStringLiteral("/tmp/afterglow-ctl-XXXXXX"));
    if (!runtime.isValid()) {
        ADD_FAILURE() << "no runtime directory";
        return done;
    }
    QLocalServer server;
    const QString path = runtime.filePath(QStringLiteral("control.sock"));
    if (!server.listen(path)) {
        ADD_FAILURE() << "cannot listen on " << qPrintable(path);
        return done;
    }
    QObject::connect(&server, &QLocalServer::newConnection, &server, [&server, &done, reply] {
        QLocalSocket* peer = server.nextPendingConnection();
        QObject::connect(peer, &QLocalSocket::readyRead, peer, [peer, &done, reply] {
            while (peer->canReadLine()) {
                const QJsonObject call = QJsonDocument::fromJson(peer->readLine()).object();
                done.calls.append(call);
                QJsonObject answer = reply;
                answer.insert(QStringLiteral("id"), call.value(QStringLiteral("id")));
                peer->write(QJsonDocument(answer).toJson(QJsonDocument::Compact) + '\n');
            }
        });
    });
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime.path());
    env.insert(QStringLiteral("AFTERGLOW_SOCKET"), path);
    for (const QString& name : {QStringLiteral("AFTERGLOW_PANE_ID"), QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("DISPLAY")})
        env.remove(name);
    QProcess process;
    process.setProcessEnvironment(env);
    process.setWorkingDirectory(runtime.path());
    // In the child, over the pipe that QProcess made: only dup2 runs there.
    if (inputFd >= 0) process.setChildProcessModifier([inputFd] { ::dup2(inputFd, STDIN_FILENO); });
    process.start(QStringLiteral(AFTERGLOW_BINARY), QStringList{QStringLiteral("ctl"), QStringLiteral("overlay")} + args);
    if (!process.waitForStarted()) {
        ADD_FAILURE() << "cannot start afterglow ctl";
        return done;
    }
    process.write(input);
    process.closeWriteChannel();
    // The target answers on this thread.
    if (!pumpUntil([&process] { return process.state() == QProcess::NotRunning; }, 15000)) {
        process.kill();
        process.waitForFinished();
        ADD_FAILURE() << qPrintable(args.join(QLatin1Char(' '))) << " did not end";
        return done;
    }
    done.outcome = {process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1,
                    QString::fromUtf8(process.readAllStandardOutput()), QString::fromUtf8(process.readAllStandardError())};
    return done;
}

QJsonObject overlayResult(const QJsonObject& result)
{
    return {{QStringLiteral("type"), QStringLiteral("reply")}, {QStringLiteral("result"), result}};
}

/// The options object of the overlay call that @p calls hold, the last
/// call, after the parts of its standard input.
QJsonObject overlayOptions(const QList<QJsonObject>& calls)
{
    if (calls.isEmpty() || calls.last().value(QStringLiteral("fn")).toString() != QLatin1String("overlay")) {
        ADD_FAILURE() << calls.size() << " calls, the last not overlay";
        return {};
    }
    return calls.last().value(QStringLiteral("args")).toArray().at(2).toObject();
}

/// The parts of standard input that came before the overlay call, joined:
/// nothing when the call names none.
std::optional<QString> overlayInput(const QList<QJsonObject>& calls)
{
    const QJsonObject options = overlayOptions(calls);
    if (!options.contains(QStringLiteral("input"))) return std::nullopt;
    const QString name = options.value(QStringLiteral("input")).toString();
    QString input;
    bool ended = false;
    for (const QJsonObject& call : calls) {
        if (call.value(QStringLiteral("fn")).toString() != QLatin1String("overlayInput")) continue;
        const QJsonArray args = call.value(QStringLiteral("args")).toArray();
        EXPECT_EQ(args.at(0).toString(), name);
        EXPECT_FALSE(ended) << "a part after the end";
        input += args.at(1).toString();
        ended = args.at(2).toBool();
    }
    EXPECT_TRUE(ended);
    return input;
}

/// The longest request line of @p calls, as ctl wrote it.
qsizetype longestCall(const QList<QJsonObject>& calls)
{
    qsizetype longest = 0;
    for (const QJsonObject& call : calls) longest = std::max(longest, QJsonDocument(call).toJson(QJsonDocument::Compact).size() + 1);
    return longest;
}

TEST(CtlOverlayTest, PrintsTheResultAndExitsWithTheProgramsStatus)
{
    const QStringList args{QStringLiteral("--pane"), QStringLiteral("3"), QStringLiteral("--title"), QStringLiteral("Pick"),
                           QStringLiteral("--"), QStringLiteral("fzf"), QStringLiteral("--multi")};
    AgainstTarget r = ctlAgainst(overlayResult({{QStringLiteral("exit_code"), 7}, {QStringLiteral("stdout"), QStringLiteral("picked\n")},
                                                {QStringLiteral("truncated"), false}}),
                                 args, "a\nb\n");
    EXPECT_EQ(r.outcome.status, 7) << qPrintable(r.outcome.err);
    EXPECT_EQ(r.outcome.out, QStringLiteral("picked\n"));
    EXPECT_EQ(r.outcome.err, QString());
    // One part of input, then the call.
    ASSERT_EQ(r.calls.size(), 2);
    const QJsonObject call = r.calls.last();
    EXPECT_EQ(call.value(QStringLiteral("target")).toString(), QStringLiteral("control"));
    EXPECT_EQ(call.value(QStringLiteral("fn")).toString(), QStringLiteral("overlay"));
    const QJsonArray callArgs = call.value(QStringLiteral("args")).toArray();
    EXPECT_EQ(callArgs.at(0).toInteger(), 3);
    EXPECT_EQ(callArgs.at(1).toArray(), (QJsonArray{QStringLiteral("fzf"), QStringLiteral("--multi")}));
    const QJsonObject options = callArgs.at(2).toObject();
    EXPECT_EQ(options.value(QStringLiteral("capture")).toString(), QStringLiteral("stdout"));
    EXPECT_EQ(options.value(QStringLiteral("title")).toString(), QStringLiteral("Pick"));
    EXPECT_EQ(overlayInput(r.calls), QStringLiteral("a\nb\n"));
    // The directory of ctl, which a relative --cwd starts from too.
    const QString cwd = options.value(QStringLiteral("cwd")).toString();
    EXPECT_TRUE(QDir::isAbsolutePath(cwd)) << qPrintable(cwd);
    r = ctlAgainst(overlayResult({}), {QStringLiteral("--pane"), QStringLiteral("3"), QStringLiteral("--cwd"), QStringLiteral("sub"),
                                       QStringLiteral("--"), QStringLiteral("fzf")});
    EXPECT_TRUE(overlayOptions(r.calls).value(QStringLiteral("cwd")).toString().endsWith(QStringLiteral("/sub")));
    // A name that starts with ':' is relative too, not a Qt resource.
    r = ctlAgainst(overlayResult({}), {QStringLiteral("--pane"), QStringLiteral("3"), QStringLiteral("--cwd"), QStringLiteral(":sub"),
                                       QStringLiteral("--"), QStringLiteral("fzf")});
    const QString colon = overlayOptions(r.calls).value(QStringLiteral("cwd")).toString();
    EXPECT_TRUE(colon.startsWith(QLatin1Char('/')) && colon.endsWith(QStringLiteral("/:sub"))) << qPrintable(colon);

    // A cut result says so. One without stdout (Control.allowReadText off)
    // prints nothing, not even that, and a status that no shell gives is 1.
    r = ctlAgainst(overlayResult({{QStringLiteral("exit_code"), 0}, {QStringLiteral("stdout"), QStringLiteral("x")},
                                  {QStringLiteral("truncated"), true}}),
                   args);
    EXPECT_EQ(r.outcome.status, 0);
    EXPECT_EQ(r.outcome.out, QStringLiteral("x"));
    EXPECT_EQ(r.outcome.err, QStringLiteral("afterglow ctl: the result was cut at 1 MiB\n"));
    r = ctlAgainst(overlayResult({{QStringLiteral("exit_code"), 3}, {QStringLiteral("truncated"), true}}), args);
    EXPECT_EQ(r.outcome.status, 3);
    EXPECT_TRUE(r.outcome.out.isEmpty()) << qPrintable(r.outcome.out);
    EXPECT_TRUE(r.outcome.err.isEmpty()) << qPrintable(r.outcome.err);
    for (const int status : {-1, 256}) {
        r = ctlAgainst(overlayResult({{QStringLiteral("exit_code"), status}, {QStringLiteral("stdout"), QString()}}), args);
        EXPECT_EQ(r.outcome.status, 1) << status;
    }
    // A cancel is an error of the call.
    r = ctlAgainst({{QStringLiteral("type"), QStringLiteral("error")}, {QStringLiteral("code"), QStringLiteral("cancelled")},
                    {QStringLiteral("message"), QStringLiteral("Control.enabled is off")}},
                   args);
    EXPECT_EQ(r.outcome.status, 1);
    EXPECT_EQ(r.outcome.err, QStringLiteral("afterglow ctl: Control.enabled is off\n"));
    EXPECT_TRUE(r.outcome.out.isEmpty()) << qPrintable(r.outcome.out);
}

TEST(CtlOverlayTest, SendsStandardInputUnlessItIsATerminalOrNIsGiven)
{
    const QJsonObject done = overlayResult({{QStringLiteral("exit_code"), 0}});
    const QStringList command{QStringLiteral("--"), QStringLiteral("fzf")};
    for (const QString& flag : {QStringLiteral("-n"), QStringLiteral("--no-stdin")}) {
        const AgainstTarget r = ctlAgainst(done, QStringList{QStringLiteral("--pane"), QStringLiteral("3"), flag} + command, "a\n");
        EXPECT_EQ(r.outcome.status, 0) << qPrintable(r.outcome.err);
        EXPECT_EQ(overlayInput(r.calls), std::nullopt) << qPrintable(flag);
        EXPECT_EQ(r.calls.size(), 1);
    }
    // A terminal is no list: the call goes at once, without stdin. Both
    // ends are opened with O_NOCTTY: neither becomes the controlling
    // terminal of the test or of afterglow.
    const AfterglowProto::UniqueFd master(posix_openpt(O_RDWR | O_NOCTTY));
    ASSERT_TRUE(master.valid());
    ASSERT_EQ(grantpt(master.get()), 0);
    ASSERT_EQ(unlockpt(master.get()), 0);
    const char* name = ptsname(master.get());
    ASSERT_NE(name, nullptr);
    const AfterglowProto::UniqueFd terminal(::open(name, O_RDONLY | O_NOCTTY | O_CLOEXEC));
    ASSERT_TRUE(terminal.valid());
    AgainstTarget r = ctlAgainst(done, QStringList{QStringLiteral("--pane"), QStringLiteral("3")} + command, {}, terminal.get());
    EXPECT_EQ(r.outcome.status, 0) << qPrintable(r.outcome.err);
    EXPECT_EQ(overlayInput(r.calls), std::nullopt);
    // A non-blocking pipe that is still empty when ctl reads it: ctl waits
    // for the list instead of failing on EAGAIN.
    int ends[2] = {-1, -1};
    ASSERT_EQ(::pipe2(ends, O_CLOEXEC), 0);
    const AfterglowProto::UniqueFd readEnd(ends[0]);
    AfterglowProto::UniqueFd writeEnd(ends[1]);
    ASSERT_EQ(::fcntl(readEnd.get(), F_SETFL, ::fcntl(readEnd.get(), F_GETFL) | O_NONBLOCK), 0);
    std::thread writer([fd = writeEnd.release()] {
        // Late enough that ctl finds the pipe empty first.
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const char list[] = "late\n";
        [[maybe_unused]] const auto written = ::write(fd, list, sizeof(list) - 1);
        ::close(fd);
    });
    r = ctlAgainst(done, QStringList{QStringLiteral("--pane"), QStringLiteral("3")} + command, {}, readEnd.get());
    writer.join();
    EXPECT_EQ(r.outcome.status, 0) << qPrintable(r.outcome.err);
    EXPECT_EQ(overlayInput(r.calls), QStringLiteral("late\n"));
    // The overlay's limit itself goes, in parts that each fit in a request.
    // So does a list of control characters, which JSON writes as 6 bytes each.
    for (const char byte : {'x', '\x01'}) {
        const QByteArray list(4 * 1024 * 1024, byte);
        r = ctlAgainst(done, QStringList{QStringLiteral("--pane"), QStringLiteral("3")} + command, list);
        EXPECT_EQ(r.outcome.status, 0) << qPrintable(r.outcome.err);
        EXPECT_TRUE(overlayInput(r.calls) == QString::fromLatin1(list)) << int(byte);
        EXPECT_GT(r.calls.size(), 2);
        EXPECT_LE(longestCall(r.calls), 1024 * 1024);
    }
}

// The help and the version need no desktop: afterglow prints them before it
// starts Qt's GUI, without which it would abort.
TEST(AfterglowHelpTest, HelpAndVersionNeedNoDesktop)
{
    Isolated r = isolate(QStringLiteral(AFTERGLOW_BINARY), {QStringLiteral("--help")});
    EXPECT_EQ(r.status, 0) << qPrintable(r.err);
    EXPECT_TRUE(r.out.contains(QStringLiteral("afterglow ctl ..."))) << qPrintable(r.out);
    // With the summaries of aglow's own usage, when the build has aglow.
#ifdef AFTERGLOW_AGLOW
    constexpr bool hasAglow = true;
#else
    constexpr bool hasAglow = false;
#endif
    for (const Aglow::Command& command : Aglow::Commands) {
        const QRegularExpression line(QStringLiteral("\n  afterglow %1 \\.\\.\\. +%2\\.\n")
                                          .arg(QLatin1String(command.name), QRegularExpression::escape(QLatin1String(command.summary))));
        EXPECT_EQ(r.out.contains(line), hasAglow) << command.name << "\n" << qPrintable(r.out);
    }
    r = isolate(QStringLiteral(AFTERGLOW_BINARY), {QStringLiteral("--version")});
    EXPECT_EQ(r.status, 0) << qPrintable(r.err);
    EXPECT_FALSE(r.out.trimmed().isEmpty());
#ifdef AFTERGLOW_AGLOW
    r = isolate(QStringLiteral(AFTERGLOW_AGLOW), {QStringLiteral("--help")});
    EXPECT_EQ(r.status, 0) << qPrintable(r.err);
    for (const Aglow::Command& command : Aglow::Commands) {
        const QRegularExpression line(
            QStringLiteral("\n  %1 +%2\n").arg(QLatin1String(command.name), QRegularExpression::escape(QLatin1String(command.summary))));
        EXPECT_TRUE(r.out.contains(line)) << command.name << "\n" << qPrintable(r.out);
    }
#endif
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
