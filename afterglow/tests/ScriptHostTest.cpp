// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "scripting/ScriptHost.h"
#include "scripting/OverlaySpec.h"
#include "scripting/ScriptTasks.h"
#include "scripting/ScriptState.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QThread>
#include <QSaveFile>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <signal.h>
#include <algorithm>
#include <deque>
#include <optional>
#include <ostream>
#include <cerrno>
#include <sys/stat.h>

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include "QStringPrinter.h"

using namespace Afterglow;

namespace {

// Runs the queued hops of a dispatch: results, effects that they apply, and
// the calls that those effects post in turn.
void pump(int turns = 6)
{
    for (int i = 0; i < turns; ++i) QCoreApplication::processEvents();
}

struct Recorder
{
    QList<QVariantMap> applied;
    QStringList owners; ///< of each owned effect, in order
    QList<QPair<QVariantMap, QString>> results;
};

class ScriptHostTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(system.isValid() && user.isValid() && stateDir.isValid() && files.isValid());
        ScriptHost::Callbacks c;
        for (quint32 pane : {7u, 8u, 9u}) {
            paneState.insert(pane, {{QStringLiteral("id"), pane}, {QStringLiteral("window"), pane == 9 ? 2 : 1},
                                   {QStringLiteral("tab_id"), pane == 9 ? 20 : 10}, {QStringLiteral("tab"), 0},
                                   {QStringLiteral("focused"), false}, {QStringLiteral("command"), QString()},
                                   {QStringLiteral("title"), QStringLiteral("fish")}});
        }
        c.paneInfo = [this](quint32 pane) { return paneState.value(pane); };
        c.state = [this] {
            ++snapshots;
            QVariantList panes;
            for (const auto& p : std::as_const(paneState)) panes.append(p);
            return QVariantMap{{QStringLiteral("panes"), panes}, {QStringLiteral("active_window"), 1},
                               {QStringLiteral("active_tab"), 10}, {QStringLiteral("active_pane"), 7},
                               {QStringLiteral("windows"), windows}, {QStringLiteral("tabs"), tabs}};
        };
        c.structuralAction = [](const QString& name) {
            return name.startsWith(QLatin1String("split_")) || name.startsWith(QLatin1String("close_"))
                || name == QLatin1String("new_tab") || name == QLatin1String("new_window");
        };
        c.apply = [this](const QVariantMap& e) {
            rec.applied.append(e);
            return applyHook ? applyHook(e) : ScriptHost::EffectResult{};
        };
        c.cleanup = [this](const QString& owner) { cleaned << owner; };
        c.closeRunWaits = [this](const QString& owner, const QString& prefix) {
            closedDialogs << prefix;
            // The window answers the dialog that it closes, as TerminalWindow
            // does: the owner's, with that prefix.
            if (pendingReply && pendingOwner == owner && pendingRequest.startsWith(prefix))
                std::exchange(pendingReply, {})({QStringLiteral("Dialog canceled"), {}, QStringLiteral("cancelled")});
        };
        c.notice = [this](quint32 pane, const QString& title, const QString& body) { notices.append({pane, title, body}); };
        c.failed = [this](const QString& owner) { failedOwners << owner; };
        c.applyOwned = [this, apply = c.apply](const QString& owner, const QVariantMap& effect, std::function<void(ScriptHost::EffectResult)> complete) {
            rec.owners.append(owner);
            if (effect.value(QStringLiteral("kind")) == QLatin1String("prompt")
                || effect.value(QStringLiteral("kind")) == QLatin1String("overlay")) {
                if (effect.value(QStringLiteral("kind")) == QLatin1String("overlay")) overlays.append(effect);
                pendingOwner = owner;
                pendingRequest = effect.value(QStringLiteral("request_id")).toString();
                pendingReply = std::move(complete);
                return ScriptHost::EffectResult{{}, {}, {}, true};
            }
            return apply(effect);
        };
        host = std::make_unique<ScriptHost>(c);
        // Beside the configuration in production, so outside the search path.
        host->setStateDirectory(stateDir.path());
        QObject::connect(host.get(), &ScriptHost::effectApplied, host.get(),
                         [this](const QString&, const QVariantMap& e, const QString& error) { rec.results.append({e, error}); });
        host->setSearchPaths({system.path(), user.path()});
    }

    static void write(const QTemporaryDir& dir, const QString& name, const QByteArray& source)
    {
        QFile f(dir.filePath(name));
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate)) << f.fileName().toStdString();
        // A short write would load a cut script.
        ASSERT_EQ(f.write(source), source.size()) << f.fileName().toStdString();
        ASSERT_TRUE(f.flush()) << f.fileName().toStdString();
    }

    void installPackage(const QString& name)
    {
        const QDir package(QStringLiteral(AFTERGLOW_EXAMPLE_PACKAGES) + "/" + name);
        QDirIterator sources(package.path(), QDir::Files, QDirIterator::Subdirectories);
        while (sources.hasNext()) {
            const QString source = sources.next();
            const QString target = user.filePath(name + "/" + package.relativeFilePath(source));
            ASSERT_TRUE(QDir().mkpath(QFileInfo(target).absolutePath()));
            ASSERT_TRUE(QFile::copy(source, target));
        }
    }

    /// Starts a command with no client and no arguments, as the palette does.
    bool run(const QString& token, quint32 pane)
    {
        return host->runCommand(token, pane, QStringLiteral("palette")).isEmpty();
    }

    /// What a run with a client got: its output, its errors and its status.
    struct Client
    {
        QString out;
        QString err;
        std::optional<int> status;
        bool gone = false; ///< the client left: writes are dropped, and the run is cancelled
        bool hold = false; ///< the client reads nothing: writes wait in held
        QList<std::function<void(bool)>> held;
    };
    /// A client that lives as long as the fixture. A run that is still live
    /// when a test ends, or returns at an ASSERT, writes into it when the host
    /// ends it.
    Client& newClient() { return clients.emplace_back(); }
    /// Starts @p command of @p script from a shell, for a client that @p client records.
    quint64 start(const QString& script, const QString& command, Client& client, QStringList args = {}, quint32 pane = 7,
                  std::optional<QString> input = std::nullopt, QString* error = nullptr, bool* unknown = nullptr,
                  const QString& cwd = QStringLiteral("/work"))
    {
        return startRequest({script, command, args, cwd, pane, input, QStringLiteral("shell"), {}}, client, error, unknown);
    }
    /// Starts @p request for a client that @p client records.
    quint64 startRequest(const ScriptHost::CommandRequest& request, Client& client, QString* error = nullptr, bool* unknown = nullptr)
    {
        ScriptHost::CommandSink sink;
        Client* const c = &client;
        auto writer = [c](QString Client::*to) {
            return [c, to](const QString& text, std::function<void(bool)> sent) {
                if (c->gone) {
                    if (sent) sent(false);
                    return false;
                }
                c->*to += text;
                if (c->hold) c->held.append(std::move(sent));
                else if (sent) sent(true);
                return true;
            };
        };
        sink.output = writer(&Client::out);
        sink.error = writer(&Client::err);
        sink.finished = [c](int status) { c->status = status; };
        return host->startCommand(request, std::move(sink), error, unknown);
    }

    /// The diagnostics of the first script. With none, the test fails: an
    /// empty map would read 0 for every count, which many checks expect.
    QVariantMap firstDiagnostic() const
    {
        const QVariantList all = host->diagnostics();
        if (all.isEmpty()) {
            ADD_FAILURE() << "No script diagnostics";
            return {};
        }
        return all.first().toMap();
    }

    QVariantList windows, tabs;
    QTemporaryDir system;
    QTemporaryDir user;
    QTemporaryDir stateDir;
    QTemporaryDir files; ///< files that tests change, outside the search path
    int snapshots = 0; ///< how often the host built a terminal snapshot
    Recorder rec;
    QMap<quint32, QVariantMap> paneState;
    std::function<ScriptHost::EffectResult(const QVariantMap&)> applyHook;
    QStringList cleaned;
    QStringList failedOwners;
    std::function<void(ScriptHost::EffectResult)> pendingReply;
    QString pendingOwner; ///< of the prompt that pendingReply answers
    QString pendingRequest; ///< of the prompt that pendingReply answers
    QStringList closedDialogs; ///< request ID prefixes
    QList<QVariantMap> overlays; ///< the overlay effects that reached the window
    struct Notice
    {
        quint32 pane;
        QString title;
        QString body;
    };
    QList<Notice> notices;
    std::deque<Client> clients; ///< before host: ~ScriptHost ends the live runs, which write into their clients
    std::unique_ptr<ScriptHost> host;
};

// A failed write stops the test, not only the helper. In a lambda it returns
// from the lambda: its callers check HasFatalFailure, or wrap their call too.
#define WRITE(...) ASSERT_NO_FATAL_FAILURE(write(__VA_ARGS__))

bool await(const std::function<bool()>& done, int timeout = 3000)
{
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return done();
}
} // namespace

TEST_F(ScriptHostTest, LoadsMetadataAndCommands)
{
    WRITE(user, QStringLiteral("hello.luau"), R"(
local s = {}
s.metadata = { id = "greeter", name = "Greeter", commands = {
    hi = { summary = "Say hi", usage = "[NAME]", palette = "Say hi" },
    ["read-list"] = { summary = "Read names", stdin = true },
} }
function s.run_command(command, ev) return nil end
return s
)");
    WRITE(user, QStringLiteral("afterglow.d.luau"), "this is not code");
    host->setEnabled(true);
    EXPECT_EQ(host->scriptIds(), QStringList{QStringLiteral("greeter")});
    // The declarations file is not loaded, so it is not even a failure.
    EXPECT_EQ(host->diagnostics().size(), 1);
    const auto commands = host->commands();
    ASSERT_EQ(commands.size(), 2);
    EXPECT_EQ(commands[0].token(), QStringLiteral("greeter.hi"));
    EXPECT_EQ(commands[0].summary, QStringLiteral("Say hi"));
    EXPECT_EQ(commands[0].usage, QStringLiteral("[NAME]"));
    EXPECT_EQ(commands[0].palette, QStringLiteral("Say hi"));
    EXPECT_FALSE(commands[0].input);
    EXPECT_EQ(commands[1].token(), QStringLiteral("greeter.read-list"));
    EXPECT_TRUE(commands[1].palette.isEmpty()); // in neither the palette nor the menu
    EXPECT_EQ(commands[1].title(), QStringLiteral("greeter read-list"));
    EXPECT_TRUE(commands[1].input);
}

TEST_F(ScriptHostTest, ManifestCommandsAreChecked)
{
    const QByteArray handler = " run_command = function() end }";
    const std::pair<const char*, const char*> cases[] = {
        {"{ ['bad id!'] = { summary = 'x' } }", "must be 1 to 64"},
        {"{ ['--help'] = { summary = 'x' } }", "must not start with -"},
        {"{ go = { summary = '   ' } }", "needs a summary"},
        {"{ go = { summary = 'x', palette = ' ' } }", "palette must not be blank"},
        {"{ go = { usage = 'X' } }", "needs a summary"},
        {"{ go = { summary = 'x', stdin = 'yes' } }", "stdin must be a boolean"},
        {"{ go = { summary = 'x', remote = 1 } }", "remote must be a boolean"},
        {"{ go = { summary = 'x', title = 'y' } }", "unknown field title"},
        {"{ go = { summary = string.rep('x', 513) } }", "at most 512"},
        {"{ go = { summary = 'a\\nb' } }", "no control characters"},
        {"{ go = { summary = 'a\\u{2028}b' } }", "line or paragraph separators"},
        {"{ go = { summary = 'a\\u{202E}b' } }", "bidirectional controls"},
        {"{ go = 'x' }", "must be a table of fields"},
        {"{ 'go' }", "must map command names"},
    };
    int i = 0;
    for (const auto& [commands, error] : cases) {
        const QString name = QStringLiteral("bad%1.luau").arg(i++);
        WRITE(user, name, "return { metadata = { commands = " + QByteArray(commands) + " }," + handler);
    }
    QByteArray most;
    for (int k = 0; k < MaxScriptCommands; ++k) most += "c" + QByteArray::number(k) + " = { summary = 'x' },";
    const QByteArray many = most + "extra = { summary = 'x' },";
    WRITE(user, QStringLiteral("many.luau"), "return { metadata = { commands = {" + many + "} }," + handler);
    WRITE(user, QStringLiteral("most.luau"), "return { metadata = { commands = {" + most + "} }," + handler);
    // An id that `aglow run` would read as an option.
    WRITE(user, QStringLiteral("-dash.luau"), "return {}");
    WRITE(user, QStringLiteral("nohandler.luau"), "return { metadata = { commands = { go = { summary = 'x' } } } }");
    host->setEnabled(true);
    // At the limit, a script loads.
    EXPECT_EQ(host->scriptIds(), QStringList{QStringLiteral("most")});
    EXPECT_EQ(host->commands().size(), MaxScriptCommands);
    // Each file's own error: a message that another file gave proves nothing.
    QHash<QString, QString> errors;
    for (const QVariant& d : host->diagnostics())
        errors.insert(QFileInfo(d.toMap().value(QStringLiteral("path")).toString()).fileName(), d.toMap().value(QStringLiteral("error")).toString());
    EXPECT_EQ(errors.value(QStringLiteral("-dash.luau")),
              QStringLiteral("The script id -dash must be 1 to 64 letters, digits, _ and -, and must not start with -"));
    i = 0;
    for (const auto& [commands, error] : cases) {
        const QString message = errors.value(QStringLiteral("bad%1.luau").arg(i++));
        EXPECT_TRUE(message.contains(QLatin1String(error))) << commands << " " << error << ": " << message.toStdString();
    }
    EXPECT_EQ(errors.value(QStringLiteral("many.luau")), QStringLiteral("A script has at most %1 commands").arg(MaxScriptCommands));
    EXPECT_EQ(errors.value(QStringLiteral("nohandler.luau")), QStringLiteral("A script with commands needs a run_command function"));
}

TEST_F(ScriptHostTest, UserScriptReplacesSystemScript)
{
    // The system file sorts last: the order of names alone would load it.
    WRITE(system, QStringLiteral("b.luau"), "return { metadata = { id = 'same', commands={sys={summary='sys',palette='sys'}} }, run_command = function() end }");
    WRITE(user, QStringLiteral("a.luau"), "return { metadata = { id = 'same', commands={user={summary='user',palette='user'}} }, run_command = function() end }");
    host->setEnabled(true);
    ASSERT_EQ(host->commands().size(), 1);
    EXPECT_EQ(host->commands()[0].token(), QStringLiteral("same.user"));
    QString superseded;
    for (const QVariant& d : host->diagnostics())
        if (d.toMap().value(QStringLiteral("status")) == QLatin1String("superseded")) superseded = d.toMap().value(QStringLiteral("path")).toString();
    EXPECT_EQ(superseded, system.filePath(QStringLiteral("b.luau")));
}

TEST_F(ScriptHostTest, BrokenScriptsAreSkipped)
{
    WRITE(user, QStringLiteral("syntax.luau"), "return {");
    WRITE(user, QStringLiteral("nothing.luau"), "return 42");
    WRITE(user, QStringLiteral("fine.luau"), "return {}");
    host->setEnabled(true);
    EXPECT_EQ(host->scriptIds(), QStringList{QStringLiteral("fine")});
}

TEST_F(ScriptHostTest, EventEffectsDefaultToTheEventPane)
{
    WRITE(user, QStringLiteral("n.luau"), R"(
return {
    on_command_finished = function(ev)
        if ev.exit_code ~= 0 and not ev.pane.focused then
            return afterglow.notify(ev.command, "failed after " .. ev.duration_ms .. " ms")
        end
    end,
}
)");
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_command_finished"), 7,
                   {{QStringLiteral("command"), QStringLiteral("make")}, {QStringLiteral("exit_code"), 2},
                    {QStringLiteral("duration_ms"), 1500}});
    host->dispatch(QStringLiteral("on_command_finished"), 7,
                   {{QStringLiteral("command"), QStringLiteral("ls")}, {QStringLiteral("exit_code"), 0},
                    {QStringLiteral("duration_ms"), 5}});
    ASSERT_EQ(rec.applied.size(), 1);
    const QVariantMap e = rec.applied[0];
    EXPECT_EQ(e.value(QStringLiteral("kind")), QStringLiteral("notify"));
    EXPECT_EQ(e.value(QStringLiteral("pane")).toUInt(), 7u);
    EXPECT_EQ(e.value(QStringLiteral("title")), QStringLiteral("make"));
    EXPECT_EQ(e.value(QStringLiteral("body")), QStringLiteral("failed after 1500 ms"));
}

TEST_F(ScriptHostTest, CommandsReturnEffectLists)
{
    WRITE(user, QStringLiteral("a.luau"), R"(
return {
    metadata = { commands={go={summary="go",palette="go"}} },
    run_command = function(command, ev)
        return {
            afterglow.split(ev.pane, "down", "htop"),
            afterglow.send_text(ev.pane, "echo " .. command .. "\n"),
            afterglow.log("ran " .. command),
            { kind = "format_disk" },
            afterglow.action(ev.pane, "script:a.go"),
            afterglow.split(ev.pane, "sideways"),
        }
    end,
}
)");
    host->setEnabled(true);
    EXPECT_FALSE(run(QStringLiteral("a.nope"), 7));
    ASSERT_TRUE(run(QStringLiteral("a.go"), 7));
    ASSERT_EQ(rec.applied.size(), 2); // log stays in the host; the rest are refused
    EXPECT_EQ(rec.applied[0].value(QStringLiteral("kind")), QStringLiteral("split"));
    EXPECT_EQ(rec.applied[0].value(QStringLiteral("command")).toStringList(), QStringList{QStringLiteral("htop")});
    EXPECT_EQ(rec.applied[0].value(QStringLiteral("direction")), QStringLiteral("down"));
    EXPECT_EQ(rec.applied[1].value(QStringLiteral("text")), QStringLiteral("echo go\n"));
    ASSERT_EQ(rec.results.size(), 6);
    EXPECT_TRUE(rec.results[2].second.isEmpty());
    for (int i = 3; i < 6; ++i) {
        EXPECT_FALSE(rec.results[i].second.isEmpty()) << i;
    }
}

TEST_F(ScriptHostTest, EffectsAndHelpersRefuseWhatTheyWouldMisread)
{
    WRITE(user, QStringLiteral("misread.luau"), R"(
return {
    on_bell = function(ev)
        return {
            afterglow.split(ev.pane, "right", ""),
            afterglow.split(ev.pane, "right", {"vi", 5}),
            {kind = "new_tab", pane = ev.pane.id, command = {program = "vi"}},
            afterglow.exec("", {"/bin/true"}),
            afterglow.request("big", afterglow.state_write({big = string.rep("x", 70000)}, 0)),
        }
    end,
    on_result = function(ev)
        return afterglow.log(ev.request_id .. ":" .. (ev.error and ev.error.code or "ok"))
    end,
    on_focus = function(ev)
        local results = {}
        for _, call in ipairs({
            function() return afterglow.request("id", nil) end,
            function() return afterglow.panes(ev.terminal, "tabs") end,
            function() return afterglow.pulse(ev.pane, nil) end,
        }) do
            local ok, err = pcall(call)
            table.insert(results, if ok then "accepted" else tostring(err))
        end
        return afterglow.log(table.concat(results, "|"))
    end,
}
)");
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_bell"), 7);
    ASSERT_EQ(rec.results.size(), 5);
    // "" is the default shell, as a string command is: not a program named "".
    EXPECT_TRUE(rec.results[0].second.isEmpty()) << rec.results[0].second.toStdString();
    EXPECT_EQ(rec.results[0].first.value(QStringLiteral("command")).toStringList(), QStringList{});
    // A number would turn into the argument "5", a table into the default shell.
    EXPECT_EQ(rec.results[1].second, QStringLiteral("command must be a string or a list of strings"));
    EXPECT_EQ(rec.results[2].second, QStringLiteral("command must be a string or a list of strings"));
    // The helper's request ID is the task name: the error names the name.
    EXPECT_EQ(rec.results[3].second, QStringLiteral("task name must contain 1 to 64 characters"));
    // A state over its limits is refused as an effect, before the store sees it.
    EXPECT_FALSE(rec.results[4].second.isEmpty());
    ASSERT_TRUE(await([&] {
        return std::any_of(rec.results.cbegin(), rec.results.cend(), [](const auto& r) { return r.first["message"] == "big:invalid_effect"; });
    }));
    rec.results.clear();
    host->dispatch(QStringLiteral("on_focus"), 7);
    ASSERT_EQ(rec.results.size(), 1);
    const QStringList errors = rec.results[0].first.value(QStringLiteral("message")).toString().split(QLatin1Char('|'));
    ASSERT_EQ(errors.size(), 3);
    EXPECT_TRUE(errors[0].contains("afterglow.request expects an effect")) << errors[0].toStdString();
    EXPECT_TRUE(errors[1].contains("afterglow.panes scope must be all, pane, tab or window")) << errors[1].toStdString();
    EXPECT_TRUE(errors[2].contains("afterglow.pulse needs an effect")) << errors[2].toStdString();
}

TEST_F(ScriptHostTest, PaneEffectsNeedAPane)
{
    WRITE(user, QStringLiteral("a.luau"), R"(
return {
    metadata = { commands={go={summary="go",palette="go"}} },
    run_command = function(command, ev) return afterglow.close(ev.pane) end,
}
)");
    host->setEnabled(true);
    ASSERT_TRUE(run(QStringLiteral("a.go"), 0));
    EXPECT_TRUE(rec.applied.isEmpty());
    ASSERT_EQ(rec.results.size(), 1);
    // Not "no pane 0", which the lookup of a pane that has gone gives.
    EXPECT_EQ(rec.results[0].second, QStringLiteral("close needs a pane"));
}

TEST_F(ScriptHostTest, StructuralEffectsAreRateLimited)
{
    WRITE(user, QStringLiteral("a.luau"), R"(
return {
    on_bell = function(ev)
        local out = {}
        for i = 1, 5 do out[i] = afterglow.new_tab(ev.pane) end
        for i = 6, 30 do out[i] = afterglow.set_title(ev.pane, "t" .. i) end
        return out
    end,
}
)");
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_bell"), 7);
    int tabs = 0;
    for (const QVariantMap& e : std::as_const(rec.applied)) {
        tabs += e.value(QStringLiteral("kind")) == QStringLiteral("new_tab");
    }
    EXPECT_EQ(tabs, ScriptHost::MaxStructuralPer10s);
    EXPECT_EQ(rec.applied.size(), ScriptHost::MaxEffectsPerSecond);
}

TEST_F(ScriptHostTest, RepeatedFailuresDisableAScript)
{
    WRITE(user, QStringLiteral("loop.luau"), R"(
return {
    metadata = { commands={spin={summary="spin",palette="spin"}} },
    on_bell = function(ev) while true do end end,
    run_command = function() error("nope") end,
}
)");
    host->setEnabled(true);
    ASSERT_EQ(host->commands().size(), 1);
    for (int i = 0; i < ScriptHost::MaxFailures - 1; ++i) {
        host->dispatch(QStringLiteral("on_bell"), 7); // times out
    }
    EXPECT_FALSE(host->isDisabled(QStringLiteral("loop")));
    run(QStringLiteral("loop.spin"), 7);
    EXPECT_TRUE(host->isDisabled(QStringLiteral("loop")));
    EXPECT_TRUE(host->commands().isEmpty());

    // Still disabled while the file is unchanged; an edit brings it back.
    host->reload();
    EXPECT_TRUE(host->isDisabled(QStringLiteral("loop")));
    WRITE(user, QStringLiteral("loop.luau"), "return { metadata = { commands={spin={summary='spin',palette='spin'}} }, run_command = function() end }");
    host->reload();
    EXPECT_FALSE(host->isDisabled(QStringLiteral("loop")));
    EXPECT_EQ(host->commands().size(), 1);
}

TEST_F(ScriptHostTest, ScriptsAreSandboxed)
{
    // Each write succeeds in a VM that was not sandboxed. Luau has no io,
    // os.execute or loadstring at all, so their absence would prove nothing.
    WRITE(user, QStringLiteral("s.luau"), R"(
return {
    on_bell = function(ev)
        local writes = {
            function() afterglow.notify = nil end,
            function() string.upper = nil end,
            function() _G.injected = true end,
            function() getmetatable("").__index = {} end,
        }
        local open = {}
        for i, write in ipairs(writes) do
            if pcall(write) then table.insert(open, tostring(i)) end
        end
        local intact = afterglow.notify ~= nil and string.upper ~= nil and rawget(_G, "injected") == nil
        return afterglow.log((#open == 0 and "none" or table.concat(open, ",")) .. " writable, " .. (intact and "intact" or "changed"))
    end,
}
)");
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_bell"), 7);
    ASSERT_EQ(rec.results.size(), 1);
    EXPECT_EQ(rec.results[0].first.value(QStringLiteral("message")), QStringLiteral("none writable, intact"));
}

TEST_F(ScriptHostTest, DisablingUnloads)
{
    WRITE(user, QStringLiteral("a.luau"), "return { metadata = { commands={x={summary='x',palette='x'}} }, run_command = function() end }");
    host->setEnabled(true);
    EXPECT_EQ(host->commands().size(), 1);
    host->setEnabled(false);
    EXPECT_TRUE(host->scriptIds().isEmpty());
    EXPECT_FALSE(run(QStringLiteral("a.x"), 7));
}

TEST_F(ScriptHostTest, ExamplesLoad)
{
    host->setSearchPaths({QStringLiteral(AFTERGLOW_EXAMPLE_SCRIPTS)});
    host->setEnabled(true);
    EXPECT_FALSE(host->scriptIds().isEmpty());
    EXPECT_EQ(host->scriptIds().size(), QDir(QStringLiteral(AFTERGLOW_EXAMPLE_SCRIPTS)).entryList({QStringLiteral("*.luau")}).size());
}

TEST_F(ScriptHostTest, SemanticRegionExampleCopiesSelectedOutput)
{
    QFile example(QStringLiteral(AFTERGLOW_EXAMPLE_SCRIPTS) + QStringLiteral("/semantic-regions.luau"));
    ASSERT_TRUE(example.open(QIODevice::ReadOnly));
    WRITE(user, "semantic-regions.luau", example.readAll());
    applyHook = [](const QVariantMap& effect) -> ScriptHost::EffectResult {
        if (effect["kind"] == QLatin1String("semantic_regions"))
            return {{}, {{"regions", QVariantList{QVariantMap{{"id", "42"}, {"kind", "output"}, {"available", true}}}}}, {}};
        if (effect["kind"] == QLatin1String("select")) return {{}, {{"id", "42"}}, {}};
        if (effect["kind"] == QLatin1String("read_region"))
            return {{}, {{"text", "command output\n"}, {"available", true}, {"truncated", false}}, {}};
        return {};
    };
    host->setEnabled(true);
    ASSERT_TRUE(run("semantic-regions.copy", 7));
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    ASSERT_EQ(rec.applied.size(), 4);
    EXPECT_EQ(rec.applied[1]["items"].toList().front().toMap()["id"].toString(), "42");
    EXPECT_EQ(rec.applied[2]["region"].toString(), "42");
    EXPECT_EQ(rec.applied[2]["max_bytes"].toInt(), 262144);
    EXPECT_EQ(rec.applied[3]["kind"].toString(), "copy");
    EXPECT_EQ(rec.applied[3]["text"].toString(), "command output\n");
    ASSERT_TRUE(run("semantic-regions.copy", 7));
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    EXPECT_EQ(rec.applied.size(), 8);
}

TEST_F(ScriptHostTest, SemanticRegionExampleReportsARefusedSelector)
{
    QFile example(QStringLiteral(AFTERGLOW_EXAMPLE_SCRIPTS) + QStringLiteral("/semantic-regions.luau"));
    ASSERT_TRUE(example.open(QIODevice::ReadOnly));
    WRITE(user, "semantic-regions.luau", example.readAll());
    QString selectCode = "busy";
    applyHook = [&](const QVariantMap& effect) -> ScriptHost::EffectResult {
        if (effect["kind"] == QLatin1String("semantic_regions"))
            return {{}, {{"regions", QVariantList{QVariantMap{{"id", "42"}, {"kind", "output"}, {"available", true}}}}}, {}};
        if (effect["kind"] == QLatin1String("select")) return {"Another dialog is open", {}, selectCode};
        return {};
    };
    host->setEnabled(true);
    ASSERT_TRUE(run("semantic-regions.copy", 7));
    pump();
    // The failure goes to stderr, which a run from the palette shows as a notice.
    ASSERT_EQ(rec.applied.size(), 2);
    EXPECT_EQ(rec.applied[0]["kind"].toString(), "semantic_regions");
    EXPECT_EQ(rec.applied[1]["kind"].toString(), "select");
    ASSERT_EQ(notices.size(), 1);
    EXPECT_EQ(notices[0].pane, 7u);
    EXPECT_EQ(notices[0].body, "Another dialog is open");
    EXPECT_EQ(notices[0].title, "Copy a shell semantic region");
    // Escape is not an error.
    selectCode = "cancelled";
    rec.applied.clear();
    ASSERT_TRUE(run("semantic-regions.copy", 7));
    pump();
    ASSERT_EQ(rec.applied.size(), 2);
    EXPECT_EQ(rec.applied[1]["kind"].toString(), "select");
    EXPECT_EQ(notices.size(), 1);
}

TEST_F(ScriptHostTest, SignalEffects)
{
    WRITE(user, QStringLiteral("s.luau"), R"(
return {
    on_bell = function(ev)
        return { afterglow.signal(ev.pane, 3, 0.5), afterglow.signal(nil, 2), { kind = "signal", channel = "x" } }
    end,
}
)");
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_bell"), 7);
    ASSERT_EQ(rec.applied.size(), 2);
    ASSERT_EQ(rec.results.size(), 3);
    EXPECT_FALSE(rec.results[2].second.isEmpty()); // channel "x"
    EXPECT_EQ(rec.applied[0].value(QStringLiteral("pane")).toUInt(), 7u);
    EXPECT_EQ(rec.applied[0].value(QStringLiteral("channel")).toInt(), 3);
    EXPECT_DOUBLE_EQ(rec.applied[0].value(QStringLiteral("value")).toDouble(), 0.5);
    EXPECT_DOUBLE_EQ(rec.applied[1].value(QStringLiteral("value")).toDouble(), 1.0); // the default
    EXPECT_EQ(rec.applied[1].value(QStringLiteral("pane")).toUInt(), 7u); // the event's pane
}

TEST_F(ScriptHostTest, GlasshouseMapsShellEventsToSignals)
{
    host->setSearchPaths({QStringLiteral(AFTERGLOW_GLASSHOUSE_SCRIPTS)});
    host->setEnabled(true);
    ASSERT_EQ(host->scriptIds(), QStringList{QStringLiteral("glasshouse")});
    auto signalsSent = [this] {
        QList<QPair<int, double>> out;
        for (const QVariantMap& e : std::as_const(rec.applied)) {
            if (e.value(QStringLiteral("kind")) == QStringLiteral("signal")) {
                out.append({e.value(QStringLiteral("channel")).toInt(), e.value(QStringLiteral("value")).toDouble()});
            }
        }
        rec.applied.clear();
        return out;
    };
    auto finished = [this](int code, int ms) {
        host->dispatch(QStringLiteral("on_command_finished"), 7,
                       {{QStringLiteral("command"), QStringLiteral("make")}, {QStringLiteral("exit_code"), code},
                        {QStringLiteral("duration_ms"), ms}});
    };
    using Sent = QList<QPair<int, double>>;
    host->dispatch(QStringLiteral("on_command_started"), 7, {{QStringLiteral("command"), QStringLiteral("make")}});
    EXPECT_EQ(signalsSent(), (Sent{{2, 1.0}}));
    finished(2, 300);
    EXPECT_EQ(signalsSent(), (Sent{{2, 0.0}, {0, 2.0}})); // cools, strikes
    finished(130, 300);
    EXPECT_EQ(signalsSent(), (Sent{{2, 0.0}})); // Ctrl+C is not a failure
    finished(0, 300);
    EXPECT_EQ(signalsSent(), (Sent{{2, 0.0}}));
    finished(0, 8000);
    EXPECT_EQ(signalsSent(), (Sent{{2, 0.0}, {1, 8.0}})); // glazed
    host->dispatch(QStringLiteral("on_status"), 7, {{QStringLiteral("state"), QStringLiteral("blocked")}, {QStringLiteral("text"), QString()}});
    EXPECT_EQ(signalsSent(), (Sent{{4, 2.0}}));
    host->dispatch(QStringLiteral("on_status"), 7, {{QStringLiteral("state"), QStringLiteral("done")}, {QStringLiteral("text"), QString()}});
    EXPECT_EQ(signalsSent(), (Sent{{4, 0.0}, {1, 1.0}}));
    host->dispatch(QStringLiteral("on_bell"), 7);
    EXPECT_EQ(signalsSent(), (Sent{{3, 1.0}}));
    EXPECT_EQ(host->commands().size(), 4);
    ASSERT_TRUE(run(QStringLiteral("glasshouse.strike"), 7));
    EXPECT_EQ(signalsSent(), (Sent{{0, 1.0}}));
}

TEST_F(ScriptHostTest, ContextualSceneSamplesTransitionsAndCleansUp)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("contextual-scene"));
    applyHook=[](const QVariantMap& e) -> ScriptHost::EffectResult {
        if(e.value("kind")=="scene_state") return {{}, {{"scene",QVariantMap{{"tab_id",10},{"history_normalized",.5},{"audio_available",true},{"overall",.75}}}}, {}};
        if(e.value("kind")=="shaders") return {{}, {{"shaders",QVariantList{QVariantMap{{"shader","phosphor-field"},{"instance","field"},{"layer","background"}}}}}, {}};
        return {};
    };
    host->setEnabled(true);
    ASSERT_TRUE(run("contextual-scene.toggle",7));
    QElapsedTimer timer;timer.start();
    bool updated=false;
    while(timer.elapsed()<1500 && !updated) {
        QCoreApplication::processEvents(); QThread::msleep(5);
        for(const auto& e:std::as_const(rec.applied)) if(e.value("kind")=="shader_parameters") {
            updated=true; EXPECT_NEAR(e.value("values").toMap().value("intensity").toDouble(),1.175,.001);
            EXPECT_EQ(e.value("scope"),"tab"); EXPECT_EQ(e.value("transition").toMap().value("duration_ms").toInt(),900);
        }
    }
    ASSERT_TRUE(updated);
    rec.applied.clear();
    ASSERT_TRUE(run("contextual-scene.toggle",7));
    bool reset=false;
    for(const auto& e:std::as_const(rec.applied)) if(e.value("kind")=="shader_parameters") {
        reset=true; EXPECT_TRUE(e.value("values").toMap().isEmpty()); EXPECT_EQ(e.value("scope"),"window");
    }
    EXPECT_TRUE(reset);
    host->setEnabled(false);
    for (const auto& result : rec.results) EXPECT_TRUE(result.second.isEmpty()) << result.second.toStdString();
}

TEST_F(ScriptHostTest, PhosphorReactsToShellCommandsAndStatus)
{
    host->setSearchPaths({QStringLiteral(AFTERGLOW_PHOSPHOR_SCRIPTS)});
    host->setEnabled(true);
    ASSERT_EQ(host->scriptIds(), QStringList{QStringLiteral("phosphor")});
    auto takeSignals = [this] {
        QList<QPair<int, double>> sent;
        for (const auto& e : std::as_const(rec.applied)) {
            if (e.value(QStringLiteral("kind")) == QStringLiteral("signals")) {
                for (const auto& v : e.value(QStringLiteral("values")).toList()) {
                    const auto channel = v.toMap();
                    sent.append({channel.value(QStringLiteral("channel")).toInt(), channel.value(QStringLiteral("value")).toDouble()});
                }
                EXPECT_EQ(e.value(QStringLiteral("pane")).toUInt(), 7u);
            }
        }
        rec.applied.clear();
        return sent;
    };
    auto finished = [this](int code, int ms) {
        host->dispatch(QStringLiteral("on_command_finished"), 7,
                       {{QStringLiteral("exit_code"), code}, {QStringLiteral("duration_ms"), ms}});
    };
    using Sent = QList<QPair<int, double>>;
    host->dispatch(QStringLiteral("on_command_started"), 7);
    EXPECT_EQ(takeSignals(), (Sent{{2, 1.0}}));
    finished(1, 2000);
    EXPECT_EQ(takeSignals(), (Sent{{2, 0.0}, {4, 0.0}, {0, 1.0}}));
    finished(0, 2000);
    EXPECT_EQ(takeSignals(), (Sent{{2, 0.0}, {4, 0.0}, {1, 2.0}}));
    finished(130, 2000);
    EXPECT_EQ(takeSignals(), (Sent{{2, 0.0}, {4, 0.0}}));
    finished(-1, 2000);
    EXPECT_EQ(takeSignals(), (Sent{{2, 0.0}, {4, 0.0}}));
    finished(0, 100);
    EXPECT_EQ(takeSignals(), (Sent{{2, 0.0}, {4, 0.0}}));
    host->dispatch(QStringLiteral("on_status"), 7, {{QStringLiteral("state"), QStringLiteral("blocked")}});
    EXPECT_EQ(takeSignals(), (Sent{{4, 2.0}}));
    host->dispatch(QStringLiteral("on_status"), 7, {{QStringLiteral("state"), QStringLiteral("done")}});
    EXPECT_EQ(takeSignals(), (Sent{{4, 0.0}, {1, 1.0}}));
}

TEST_F(ScriptHostTest, PhosphorFocusChangesDoNotSpendEffectBudgetOrRetargetHeat)
{
    host->setSearchPaths({QStringLiteral(AFTERGLOW_PHOSPHOR_SCRIPTS)});host->setEnabled(true);
    host->dispatch("on_command_started",7);
    ASSERT_EQ(rec.applied.size(),2);
    rec.applied.clear();
    rec.results.clear();
    // The demo has no on_focus on purpose: this pins that focus sends
    // nothing, also from a handler that someone adds back.
    for(int i=0;i<64;++i) host->dispatch("on_focus", i%2 ? 7 : 8);
    EXPECT_TRUE(rec.applied.isEmpty());
    EXPECT_TRUE(rec.results.isEmpty());
    host->dispatch("on_command_finished",7,{{"exit_code",0},{"duration_ms",2000}});
    EXPECT_EQ(rec.applied.size(),2);
    for(const auto& result:rec.results) EXPECT_TRUE(result.second.isEmpty()) << result.second.toStdString();
}

TEST_F(ScriptHostTest, PhosphorDemoCommandsAndStatus)
{
    host->setSearchPaths({QStringLiteral(AFTERGLOW_PHOSPHOR_SCRIPTS)});
    host->setEnabled(true);
    // Each command on its own: a failure of one does not hide the others.
    for (const auto& [command, channel] : std::initializer_list<std::pair<const char*, int>>{
             {"strike", 0}, {"complete", 1}, {"ring", 3}, {"heat", 2}}) {
        SCOPED_TRACE(command);
        EXPECT_TRUE(run(QStringLiteral("phosphor.") + QString::fromLatin1(command), 7));
        EXPECT_EQ(rec.applied.size(), channel == 2 ? 2 : 1);
        const auto values = rec.applied.value(0).value(QStringLiteral("values")).toList();
        EXPECT_EQ(values.size(), 1);
        const auto value = values.value(0).toMap();
        EXPECT_EQ(value.value(QStringLiteral("channel"), -1).toInt(), channel);
        EXPECT_DOUBLE_EQ(value.value(QStringLiteral("value")).toDouble(), 1.0);
        rec.applied.clear();
    }
    host->dispatch(QStringLiteral("on_status"), 7, {{QStringLiteral("state"), QStringLiteral("error")}});
    ASSERT_EQ(rec.applied.size(), 1);
    const auto values = rec.applied[0].value(QStringLiteral("values")).toList();
    ASSERT_EQ(values.size(), 2);
    EXPECT_EQ(values[0].toMap().value(QStringLiteral("channel")).toInt(), 4);
    EXPECT_DOUBLE_EQ(values[0].toMap().value(QStringLiteral("value")).toDouble(), 3.0);
    EXPECT_EQ(values[1].toMap().value(QStringLiteral("channel")).toInt(), 0);
}

TEST_F(ScriptHostTest, PhosphorUsesTabBatchesWithoutAPaneRegistry)
{
    host->setSearchPaths({QStringLiteral(AFTERGLOW_PHOSPHOR_SCRIPTS)});
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_user_var"), 7,
                   {{QStringLiteral("name"), QStringLiteral("phosphor")}, {QStringLiteral("value"), QStringLiteral("strike")}});
    ASSERT_EQ(rec.applied.size(), 1);
    EXPECT_EQ(rec.applied[0].value(QStringLiteral("pane")).toUInt(), 7u);
    EXPECT_EQ(rec.applied[0].value(QStringLiteral("scope")), QStringLiteral("tab"));
    rec.applied.clear();
    paneState.remove(8);
    host->dispatch(QStringLiteral("on_pane_closed"), 8);
    rec.applied.clear();
    ASSERT_TRUE(run(QStringLiteral("phosphor.complete"), 7));
    ASSERT_EQ(rec.applied.size(), 1);
    EXPECT_EQ(rec.applied[0].value(QStringLiteral("pane")).toUInt(), 7u);
    EXPECT_EQ(rec.applied[0].value(QStringLiteral("values")).toList().first().toMap().value(QStringLiteral("channel")).toInt(), 1);
}

TEST_F(ScriptHostTest, PhosphorReheatsBothTabsOfAPaneThatMoves)
{
    tabs = {QVariantMap{{"id", 10}, {"window", 1}, {"index", 0}, {"active", true}, {"active_pane", 7}},
            QVariantMap{{"id", 20}, {"window", 2}, {"index", 0}, {"active", true}, {"active_pane", 9}}};
    paneState[8]["command"] = "make";
    host->setSearchPaths({QStringLiteral(AFTERGLOW_PHOSPHOR_SCRIPTS)});
    host->setEnabled(true);
    pump();
    rec.applied.clear();
    // Pane 8 takes make along to tab 20: tab 10 cools, tab 20 charges.
    paneState[8]["tab_id"] = 20;
    paneState[8]["window"] = 2;
    host->dispatch(QStringLiteral("on_pane_moved"), 8);
    QMap<quint32, double> heat;
    QStringList requests;
    for (const auto& e : std::as_const(rec.applied)) {
        if (e["kind"] == "shaders") requests << e["request_id"].toString();
        if (e["kind"] != "signals") continue;
        for (const auto& v : e["values"].toList())
            if (v.toMap()["channel"].toInt() == 2) heat.insert(e["pane"].toUInt(), v.toMap()["value"].toDouble());
    }
    EXPECT_EQ(heat, (QMap<quint32, double>{{7u, 0.0}, {8u, 1.0}}));
    EXPECT_EQ(requests, (QStringList{"heat:7:0", "heat:8:1"}));
    // A move within the tab sends nothing.
    rec.applied.clear();
    host->dispatch(QStringLiteral("on_pane_moved"), 8);
    EXPECT_TRUE(rec.applied.isEmpty());
}

TEST_F(ScriptHostTest, PhosphorSendsTheHeatOfAClosedPanesTabToItsLivePane)
{
    tabs = {QVariantMap{{"id", 10}, {"window", 1}, {"index", 0}, {"active", true}, {"active_pane", 7}}};
    applyHook = [](const QVariantMap& e) -> ScriptHost::EffectResult {
        if (e["kind"] == "shaders")
            return {{}, {{"shaders", QVariantList{QVariantMap{{"shader", "phosphor-field"}, {"instance", "field-1"}}}}}, {}};
        return {};
    };
    host->setSearchPaths({QStringLiteral(AFTERGLOW_PHOSPHOR_SCRIPTS)});
    host->setEnabled(true);
    pump();
    rec.applied.clear();
    paneState.remove(8);
    // The result of the request has the closed pane as its event's pane.
    host->dispatch(QStringLiteral("on_pane_closed"), 8,
                   {{"pane", QVariantMap{{"id", 8}, {"tab_id", 10}, {"window", 1}}}});
    auto parameters = [&] {
        QList<QVariantMap> out;
        for (const auto& e : std::as_const(rec.applied)) if (e["kind"] == "shader_parameters") out << e;
        return out;
    };
    ASSERT_TRUE(await([&] { return !parameters().isEmpty(); }));
    pump();
    const auto sent = parameters();
    ASSERT_EQ(sent.size(), 1);
    EXPECT_EQ(sent[0]["pane"].toUInt(), 7u);
    EXPECT_EQ(sent[0]["instance"], "field-1");
    EXPECT_EQ(sent[0]["values"].toMap()["commandHeat"].toDouble(), 0.0);
    for (const auto& result : rec.results) EXPECT_TRUE(result.second.isEmpty()) << result.second.toStdString();
}

TEST_F(ScriptHostTest, PhosphorStaysChargedWhileAnotherPaneIsRunning)
{
    host->setSearchPaths({QStringLiteral(AFTERGLOW_PHOSPHOR_SCRIPTS)});
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_command_started"), 7);
    paneState[8].insert(QStringLiteral("command"), QStringLiteral("make"));
    host->dispatch(QStringLiteral("on_command_started"), 8);
    rec.applied.clear();
    host->dispatch(QStringLiteral("on_command_finished"), 7,
                   {{QStringLiteral("exit_code"), 0}, {QStringLiteral("duration_ms"), 100}});
    int charges = 0;
    for (const auto& e : rec.applied) {
        for (const auto& v : e.value(QStringLiteral("values")).toList()) {
            const auto value = v.toMap();
            if (value.value(QStringLiteral("channel")).toInt() == 2) {
                ++charges;
                EXPECT_DOUBLE_EQ(value.value(QStringLiteral("value")).toDouble(), 1.0);
            }
        }
    }
    EXPECT_EQ(charges, 1); // one batch updates both panes
}

TEST_F(ScriptHostTest, SnapshotsAreVersionedDetachedAndUseStableTabIds)
{
    WRITE(user, QStringLiteral("state.luau"), R"(
local s = {}
function s.on_bell(ev)
    assert(ev.terminal.version == 1)
    assert(ev.terminal.active_pane == 7 and ev.terminal.active_tab == 10)
    assert(#afterglow.panes(ev.terminal) == 3)
    assert(#afterglow.panes(ev.terminal, "tab") == 2)
    assert(#afterglow.panes(ev.terminal, "tab", 999) == 0)
    assert(#afterglow.panes(ev.terminal, "tab", {id=999}) == 0)
    assert(#afterglow.panes(ev.terminal, "tab", {id=8, tab_id=999}) == 2)
    local panes = afterglow.panes(ev.terminal, "tab", ev.pane)
    assert(#panes == 2 and panes[1].title == "fish")
    panes[1].title = "only this copy"
    return afterglow.copy(tostring(#panes))
end
return s
)");
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_bell"), 7);
    paneState[7].insert(QStringLiteral("tab"), 3); // reorder leaves identity intact
    paneState[8].insert(QStringLiteral("tab"), 3);
    host->dispatch(QStringLiteral("on_bell"), 7);
    ASSERT_EQ(rec.applied.size(), 2);
    // The second call's assert on panes[1].title is what shows the copy was detached.
    EXPECT_EQ(rec.applied[1].value(QStringLiteral("text")), QStringLiteral("2"));
}

TEST_F(ScriptHostTest, CreationResultsAreQueuedAndDeliveredOnlyToTheirOwner)
{
    WRITE(user, QStringLiteral("creator.luau"), R"(
return {
    on_bell = function(ev) return afterglow.request("split", afterglow.split(ev.pane)) end,
    on_result = function(ev)
        assert(ev.request_id == "split" and ev.kind == "split" and ev.ok and ev.error == nil)
        assert(ev.data.pane.id == 42 and ev.data.tab_id == 10)
        assert(#afterglow.panes(ev.terminal, "tab", ev.data.pane) == 3)
        return afterglow.set_title(ev.data.pane, "created")
    end,
}
)");
    WRITE(user, QStringLiteral("other.luau"), R"(
return { on_result = function() return afterglow.copy("wrong owner") end }
)");
    applyHook = [this](const QVariantMap& effect) -> ScriptHost::EffectResult {
        if (effect.value(QStringLiteral("kind")) != QLatin1String("split")) return {};
        auto pane = paneState[7];
        pane.insert(QStringLiteral("id"), 42);
        paneState.insert(42, pane);
        return {{}, {{QStringLiteral("pane"), pane}, {QStringLiteral("tab_id"), 10}}, {}};
    };
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_bell"), 7);
    ASSERT_EQ(rec.applied.size(), 1); // completion does not re-enter the handler
    QCoreApplication::processEvents();
    ASSERT_EQ(rec.applied.size(), 2);
    EXPECT_EQ(rec.applied[1].value(QStringLiteral("pane")).toUInt(), 42u);
    EXPECT_EQ(rec.applied[1].value(QStringLiteral("title")), QStringLiteral("created"));
}

TEST_F(ScriptHostTest, RefusalsAndFailuresHaveCorrelatedErrorCodes)
{
    WRITE(user, QStringLiteral("errors.luau"), R"(
return {
    on_bell = function(ev)
        return {
            afterglow.request("invalid_effect", {kind = "unknown"}),
            afterglow.request("no_pane", afterglow.focus(999)),
            afterglow.request("operation_failed", afterglow.close(ev.pane)),
        }
    end,
    on_result = function(ev)
        assert(not ev.ok and ev.error.code == ev.request_id and ev.error.message ~= "")
        return afterglow.copy(ev.request_id)
    end,
}
)");
    applyHook = [](const QVariantMap& effect) -> ScriptHost::EffectResult {
        return effect.value(QStringLiteral("kind")) == QLatin1String("close")
            ? ScriptHost::EffectResult{QStringLiteral("cannot close"), {}, {}} : ScriptHost::EffectResult{};
    };
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_bell"), 7);
    ASSERT_EQ(rec.applied.size(), 1);
    QCoreApplication::processEvents();
    ASSERT_EQ(rec.applied.size(), 4);
    EXPECT_EQ(rec.applied[1].value(QStringLiteral("text")), QStringLiteral("invalid_effect"));
    EXPECT_EQ(rec.applied[2].value(QStringLiteral("text")), QStringLiteral("no_pane"));
    EXPECT_EQ(rec.applied[3].value(QStringLiteral("text")), QStringLiteral("operation_failed"));
}

TEST_F(ScriptHostTest, PaneClosedBeforeCompletionCannotBeRetargeted)
{
    WRITE(user, QStringLiteral("stale.luau"), R"(
return {
    on_bell = function(ev) return afterglow.request("first", afterglow.set_title(ev.pane, "before")) end,
    on_result = function(ev)
        if ev.request_id == "first" then
            assert(ev.ok and ev.pane.id == 7 and ev.pane.title == nil)
            assert(#afterglow.panes(ev.terminal, "pane", 7) == 0)
            return afterglow.request("followup", afterglow.focus(ev.pane))
        end
        assert(not ev.ok and ev.error.code == "no_pane")
        return afterglow.copy("closed")
    end,
}
)");
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_bell"), 7);
    paneState.remove(7);
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    ASSERT_EQ(rec.applied.size(), 2);
    EXPECT_EQ(rec.applied[1].value(QStringLiteral("text")), QStringLiteral("closed"));
}

TEST_F(ScriptHostTest, ReloadAndUnloadCancelOldCompletions)
{
    const QByteArray source = R"(
return {
    on_bell = function(ev) return afterglow.request("r", afterglow.focus(ev.pane)) end,
    on_result = function() return afterglow.copy("unexpected") end,
}
)";
    WRITE(user, QStringLiteral("pending.luau"), source);
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_bell"), 7);
    WRITE(user, QStringLiteral("pending.luau"), source + "\n-- new instance\n");
    host->reload();
    QCoreApplication::processEvents();
    ASSERT_EQ(rec.applied.size(), 1);
    host->dispatch(QStringLiteral("on_bell"), 7);
    host->setEnabled(false);
    QCoreApplication::processEvents();
    ASSERT_EQ(rec.applied.size(), 2);
}

TEST_F(ScriptHostTest, GenericActionsShareTheStructuralBudget)
{
    WRITE(user, QStringLiteral("budget.luau"), R"(
return {
    on_bell = function(ev)
        return {
            afterglow.action(ev.pane, "split_left"),
            afterglow.new_tab(ev.pane),
            afterglow.action(ev.pane, "close_window"),
            afterglow.request("limited", afterglow.action(ev.pane, "new_window")),
        }
    end,
    on_result = function(ev)
        assert(not ev.ok and ev.error.code == "rate_limited")
        return afterglow.copy("limited")
    end,
}
)");
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_bell"), 7);
    ASSERT_EQ(rec.applied.size(), 3);
    QCoreApplication::processEvents();
    ASSERT_EQ(rec.applied.size(), 4);
    EXPECT_EQ(rec.applied.last().value(QStringLiteral("text")), QStringLiteral("limited"));
}

TEST_F(ScriptHostTest, SignalBatchesValidateEveryChannelBeforeApplying)
{
    WRITE(user, QStringLiteral("batch.luau"), R"(
return {
    on_bell = function(ev)
        return {
            afterglow.signals(ev.pane, {{channel=0,value=1}, {channel=8,value=1}}),
            afterglow.signals(ev.pane, {{channel=0,value=1}, {channel=0,value=2}}),
            afterglow.signals(ev.pane, {{channel=0,value=1}}, "bad"),
            afterglow.signals(ev.pane, {{channel=0,value=1e100}}),
            afterglow.signal(ev.pane, 0.5, 1),
            afterglow.signal(ev.pane, 1, 0/0),
            afterglow.signals(ev.pane, {{channel=0,value=1}, {channel=2,value=0}, {channel=4,value=0}}),
        }
    end,
}
)");
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_bell"), 7);
    ASSERT_EQ(rec.applied.size(), 1);
    ASSERT_EQ(rec.results.size(), 7);
    EXPECT_EQ(rec.applied[0].value(QStringLiteral("values")).toList().size(), 3);
    EXPECT_EQ(rec.applied[0].value(QStringLiteral("scope")), QStringLiteral("tab"));
}

TEST_F(ScriptHostTest, EightPanePhosphorUpdateUsesOneEffectForThreeChannels)
{
    for (quint32 id = 8; id < 15; ++id) {
        auto pane = paneState[7];
        pane.insert(QStringLiteral("id"), id);
        paneState.insert(id, pane);
    }
    host->setSearchPaths({QStringLiteral(AFTERGLOW_PHOSPHOR_SCRIPTS)});
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_command_finished"), 7,
                   {{QStringLiteral("exit_code"), 2}, {QStringLiteral("duration_ms"), 2000}});
    ASSERT_EQ(paneState.size(), 8);
    ASSERT_EQ(rec.applied.size(), 2); // one signal batch plus named-parameter discovery
    EXPECT_EQ(rec.applied[0].value(QStringLiteral("kind")), QStringLiteral("signals"));
    EXPECT_EQ(rec.applied[0].value(QStringLiteral("values")).toList().size(), 3);
    EXPECT_TRUE(rec.results[0].second.isEmpty());
}

TEST_F(ScriptHostTest, ResultFeedbackIsBoundedEvenForRejectedRequests)
{
    WRITE(user, QStringLiteral("loop.luau"), R"(
local function retry() return afterglow.request("again", afterglow.focus(999)) end
return { on_bell = retry, on_result = retry }
)");
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_bell"), 7);
    for (int i = 0; i <= ScriptHost::MaxResultsPerSecond; ++i) QCoreApplication::processEvents();
    EXPECT_TRUE(host->isDisabled(QStringLiteral("loop")));
    EXPECT_TRUE(rec.applied.isEmpty());
    EXPECT_EQ(rec.results.size(), ScriptHost::MaxResultsPerSecond + 1);
}

TEST_F(ScriptHostTest, OversizedEffectListsApplyNothing)
{
    WRITE(user, QStringLiteral("large.luau"), R"(
return { on_bell = function(ev)
    local effects = {}
    for i = 1, 65 do table.insert(effects, afterglow.set_title(ev.pane, "x")) end
    return effects
end }
)");
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_bell"), 7);
    EXPECT_TRUE(rec.applied.isEmpty());
    ASSERT_EQ(rec.results.size(), 1);
    EXPECT_FALSE(rec.results[0].second.isEmpty());
}

namespace {
// Whether the kernel lists the children of a thread: it needs
// CONFIG_PROC_CHILDREN. Without it, children() is always empty.
bool childrenListed()
{
    return QFile::exists(QStringLiteral("/proc/self/task/%1/children").arg(QCoreApplication::applicationPid()));
}
// The processes that this process started and has not reaped yet.
QSet<qint64> children()
{
    QSet<qint64> out;
    const QDir tasks("/proc/self/task");
    for (const QString& task : tasks.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile file(tasks.filePath(task + "/children"));
        if (!file.open(QIODevice::ReadOnly)) continue;
        for (const QByteArray& pid : file.readAll().split(' '))
            if (!pid.trimmed().isEmpty()) out.insert(pid.trimmed().toLongLong());
    }
    return out;
}
QVariantMap job(const QString& name, const QStringList& argv)
{
    return {{QStringLiteral("kind"), QStringLiteral("exec")}, {QStringLiteral("name"), name},
            {QStringLiteral("command"), argv}, {QStringLiteral("timeout_ms"), 1000}, {QStringLiteral("max_bytes"), 128}};
}
}

TEST(ScriptTasksTest, ProcessesReportSuccessFailureLimitsAndCancellationOnce)
{
    if (!childrenListed()) GTEST_SKIP() << "The kernel has no /proc/PID/task/TID/children";
    ScriptTasks tasks;
    QMap<QString, QList<ScriptHost::EffectResult>> results;
    const auto before = children();
    auto start = [&](QVariantMap effect) {
        auto name = effect.value(QStringLiteral("name")).toString();
        return tasks.apply(QStringLiteral("owner"), effect, [&, name](auto result) { results[name].append(result); }, {});
    };
    EXPECT_TRUE(start(job("ok", {"/bin/sh", "-c", "printf hello; printf warning >&2"})).deferred);
    EXPECT_TRUE(start(job("bad", {"/bin/sh", "-c", "exit 7"})).deferred);
    EXPECT_TRUE(start(job("spawn", {"/no/such/afterglow-test-command"})).deferred);
    auto timeout = job("timeout", {"/bin/sleep", "10"});
    timeout["timeout_ms"] = 30;
    EXPECT_TRUE(start(timeout).deferred);
    // The probe below sees the jobs: a sleep of 10 s runs now.
    ASSERT_FALSE(children().subtract(before).isEmpty());
    EXPECT_TRUE(start(job("limit", {"/bin/sh", "-c", "while :; do printf xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx; done"})).deferred);
    EXPECT_TRUE(start(job("cancel", {"/bin/sleep", "10"})).deferred);
    EXPECT_TRUE(tasks.apply("owner", {{"kind", "cancel"}, {"name", "cancel"}}, {}, {}).error.isEmpty());
    ASSERT_TRUE(await([&] { return results.size() == 6; }));
    EXPECT_EQ(results["ok"][0].data["stdout"], QStringLiteral("hello"));
    EXPECT_EQ(results["ok"][0].data["stderr"], QStringLiteral("warning"));
    EXPECT_TRUE(results["ok"][0].error.isEmpty());
    EXPECT_FALSE(results["ok"][0].data["truncated"].toBool());
    EXPECT_EQ(results["bad"][0].code, QStringLiteral("process_failed"));
    EXPECT_EQ(results["bad"][0].data["exit_code"], 7);
    EXPECT_EQ(results["spawn"][0].code, QStringLiteral("spawn_failed"));
    EXPECT_EQ(results["timeout"][0].code, QStringLiteral("timeout"));
    EXPECT_EQ(results["limit"][0].code, QStringLiteral("output_limit"));
    // Cut at max_bytes exactly: the process writes without end.
    EXPECT_EQ(results["limit"][0].data["stdout"].toString(), QString(128, QLatin1Char('x')));
    EXPECT_TRUE(results["limit"][0].data["truncated"].toBool());
    EXPECT_EQ(results["cancel"][0].code, QStringLiteral("cancelled"));
    // The killed processes report finished when they are reaped: that must
    // not complete them again.
    ASSERT_TRUE(await([&] { return children().subtract(before).isEmpty(); }));
    pump();
    for (const auto& values : results) EXPECT_EQ(values.size(), 1);
}

TEST(ScriptTasksTest, FileReadsAreBoundedAndRejectSpecialFiles)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QFile file(dir.filePath("input")); ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("abcdef"); file.close();
    const auto fifo = dir.filePath("fifo");
    ASSERT_EQ(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
    ScriptTasks tasks;
    QMap<QString, ScriptHost::EffectResult> results;
    for (const auto& name : {QStringLiteral("input"), QStringLiteral("fifo")}) {
        EXPECT_TRUE(tasks.apply("owner", {{"kind", "read_file"}, {"name", name}, {"path", dir.filePath(name)}, {"max_bytes", 3}},
            [&, name](auto result) { results.insert(name, result); }, {}).deferred);
    }
    ASSERT_TRUE(await([&] { return results.size() == 2; }));
    EXPECT_EQ(results["input"].data["text"], QStringLiteral("abc"));
    EXPECT_TRUE(results["input"].data["truncated"].toBool());
    EXPECT_EQ(results["fifo"].code, QStringLiteral("io_error"));
}

TEST(ScriptTasksTest, OwnerCleanupStopsTimersAndDropsAsyncCallbacks)
{
    if (!childrenListed()) GTEST_SKIP() << "The kernel has no /proc/PID/task/TID/children";
    ScriptTasks tasks;
    int callbacks = 0;
    const int repeatMs = 250;
    EXPECT_TRUE(tasks.apply("owner", {{"kind", "timer"}, {"name", "tick"}, {"delay_ms", 5}, {"repeat_ms", repeatMs}},
        {}, [&](auto, auto, auto) { ++callbacks; }).error.isEmpty());
    ASSERT_TRUE(await([&] { return callbacks == 1; }));
    const auto before = children();
    EXPECT_TRUE(tasks.apply("owner", job("job", {"/bin/sleep", "5"}), [&](auto) { ++callbacks; }, {}).deferred);
    // The probe sees the job, so its end below is a real reap.
    ASSERT_TRUE(await([&] { return !children().subtract(before).isEmpty(); }));
    // A slow start of the job can let the timer repeat before this: count
    // from here.
    const int fired = callbacks;
    tasks.clear("owner");
    // The killed job is reaped, and its end calls nothing.
    ASSERT_TRUE(await([&] { return children().subtract(before).isEmpty(); }));
    pump();
    EXPECT_EQ(callbacks, fired);
    // The timer stopped too: past its next repeat, it has not fired.
    await([] { return false; }, repeatMs + 50);
    EXPECT_EQ(callbacks, fired);
}

TEST(ScriptTasksTest, FileTasksRefuseRelativeResourceLongAndNulPaths)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ScriptTasks tasks;
    const QString tooLong = dir.path() + "/" + QString(4096, 'x');
    const QString withNul = dir.path() + QChar(0) + "watch";
    // ":/x" is a Qt resource, which QDir::isAbsolutePath takes for absolute.
    for (const QString& path : {QStringLiteral("relative"), QStringLiteral(":/x"), tooLong, withNul}) {
        for (const char* kind : {"watch_file", "read_file"}) {
            const auto result = tasks.apply("owner", {{"kind", QString::fromLatin1(kind)}, {"name", "watch"}, {"path", path}}, {}, {});
            EXPECT_EQ(result.code, "invalid_effect") << kind;
            EXPECT_TRUE(result.error.contains("at most 4096 characters")) << kind << ' ' << result.error.toStdString();
        }
    }
}

TEST(ScriptTasksTest, ExecRefusesARelativeResourceOrNulDirectory)
{
    ScriptTasks tasks;
    // Absolute, as for the file tasks, and not a Qt resource.
    for (const QString& cwd : {QStringLiteral("relative"), QStringLiteral(":/"), QStringLiteral("/tmp") + QChar(0) + "x"}) {
        const auto result =
            tasks.apply("owner", {{"kind", "exec"}, {"name", "run"}, {"command", QStringList{QStringLiteral("true")}}, {"cwd", cwd}}, {}, {});
        EXPECT_EQ(result.code, "invalid_effect") << cwd.toStdString();
        EXPECT_EQ(result.error, QStringLiteral("exec cwd must be an absolute path")) << cwd.toStdString();
    }
}

TEST(ScriptTasksTest, WatchSurvivesAtomicFileReplacement)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto path = dir.filePath("watch");
    { QFile f(path); ASSERT_TRUE(f.open(QIODevice::WriteOnly)); f.write("old"); }
    ScriptTasks tasks;
    int changes = 0;
    EXPECT_TRUE(tasks.apply("owner", {{"kind", "watch_file"}, {"name", "watch"}, {"path", path}}, {},
        [&](const QString& handler, const QVariantMap& event, quint64) {
            EXPECT_EQ(handler, QStringLiteral("on_file_changed"));
            EXPECT_TRUE(event["exists"].toBool());
            ++changes;
        }).error.isEmpty());
    for (int i = 1; i <= 2; ++i) {
        QSaveFile f(path); ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(QByteArray::number(i)); ASSERT_TRUE(f.commit());
        ASSERT_TRUE(await([&] { return changes == i; }));
    }
}

TEST_F(ScriptHostTest, LifecycleTimersJobsAndDynamicCommands)
{
    WRITE(user, "lifecycle.luau", R"(
return {
    on_load = function()
        return {afterglow.copy("loaded"), afterglow.timer("tick", 1),
                afterglow.exec("job", {"/bin/sh", "-c", "printf hello"}),
                afterglow.set_commands({dynamic = {summary="Dynamic",palette="Dynamic"}})}
    end,
    on_timer = function(ev) return afterglow.copy(ev.name) end,
    on_result = function(ev)
        assert(ev.ok)
        return afterglow.copy(ev.data.stdout)
    end,
    run_command = function() return afterglow.copy("command") end,
}
)");
    host->setEnabled(true);
    ASSERT_TRUE(await([&] { return rec.applied.size() == 3; }));
    QStringList text;
    for (const auto& e : rec.applied) text << e["text"].toString();
    EXPECT_TRUE(text.contains("loaded")); EXPECT_TRUE(text.contains("tick")); EXPECT_TRUE(text.contains("hello"));
    ASSERT_EQ(host->commands().size(), 1);
    EXPECT_EQ(host->commands()[0].token(), QStringLiteral("lifecycle.dynamic"));
    host->reload(); // unchanged source keeps its instance
    EXPECT_EQ(rec.applied.size(), 3);
    WRITE(user, "lifecycle.luau", "return {"); host->reload(); // broken edit retains last good
    EXPECT_EQ(host->commands().size(), 1);
    EXPECT_TRUE(run("lifecycle.dynamic", 7));
    EXPECT_EQ(rec.applied.last()["text"], QStringLiteral("command"));
    host->setEnabled(false);
    EXPECT_TRUE(host->commands().isEmpty());
}

TEST_F(ScriptHostTest, InvalidOwnedUiEffectsNeverReachTheWindow)
{
    WRITE(user, "invalidui.luau", R"(
return { on_bell = function(ev)
    return {
        afterglow.override(ev.pane, {backgroundOpacity=2}),
        afterglow.override(ev.pane, {cursorBlink=1}),
        afterglow.override(ev.pane, {shell="evil"}),
        afterglow.select(ev.pane, "Duplicate", {{id="x"},{id="x"}}),
        afterglow.prompt(ev.pane, "Too long", string.rep("x",4097)),
        afterglow.set_commands({["no-handler"] = {summary="x"}}),
        afterglow.override(ev.pane, {backgroundOpacity=0.5,cursorBlink=false}),
    }
end }
)");
    host->setEnabled(true); host->dispatch("on_bell", 7);
    ASSERT_EQ(rec.applied.size(), 1);
    EXPECT_EQ(rec.applied[0]["kind"], QStringLiteral("override"));
    // The prompt and set_commands never reach apply even when accepted, so check them here.
    EXPECT_FALSE(bool(pendingReply));
    EXPECT_TRUE(host->commands().isEmpty());
    ASSERT_EQ(rec.results.size(), 7);
    for (int i = 0; i < 6; ++i) EXPECT_FALSE(rec.results[i].second.isEmpty()) << i;
    EXPECT_TRUE(rec.results[6].second.isEmpty());
}


TEST_F(ScriptHostTest, DockEffectsAreCheckedBeforeTheyReachTheWindow)
{
    WRITE(user, "docks.luau", R"(
local run = {text="A", fg=1, bg="#10A0FF", bold=true, item="tab-1"}
return { on_bell = function(ev)
    return {
        afterglow.dock(ev.pane, {id="bar", edge="top", tab_bar=true, fill=3, lines={{run, {text="B"}}}}),
        afterglow.dock(ev.pane, {id="side", edge="left", size=20, lines={}}),
        afterglow.undock(ev.pane, "bar"),
        afterglow.dock(ev.pane, {id="x", edge="middle", lines={}}),
        afterglow.dock(ev.pane, {id="x", edge="top", size=9, lines={}}),
        afterglow.dock(ev.pane, {id="x", edge="top", size=0, lines={}}),
        afterglow.dock(ev.pane, {id="x", edge="bottom", tab_bar=true, lines={}}),
        afterglow.dock(ev.pane, {id="x", edge="top", lines={{{text="A", fg=256}}}}),
        afterglow.dock(ev.pane, {id="x", edge="top", lines={{{text="A", bg="red"}}}}),
        afterglow.dock(ev.pane, {id="x", edge="top", lines={{{text="A", blink=true}}}}),
        afterglow.dock(ev.pane, {id="x", edge="top", lines={{{text=5}}}}),
        afterglow.dock(ev.pane, {id="x", edge="top", lines={{{text="A", item=""}}}}),
        afterglow.dock(ev.pane, {id="x", edge="top", lines={{}, {}}}),
        afterglow.dock(ev.pane, {id="two words", edge="top", lines={}}),
        afterglow.dock(ev.pane, {id="x", edge="top", lines={{{text=string.rep("x", 16385)}}}}),
    }
end }
)");
    host->setEnabled(true); host->dispatch("on_bell", 7);
    // Each refusal for its own reason, so a case refused for another passes no test.
    const char* reasons[] = {"Dock edge must be", "1 to 8 rows", "1 to 8 rows", "Only a top dock", "Dock colors",
                             "Dock colors", "Unknown dock run field blink", "text must be a string", "A dock item",
                             "at most one line for each row", "A dock id", "exceed 16384 bytes"};
    ASSERT_EQ(rec.results.size(), 15);
    for (int i = 0; i < 3; ++i) EXPECT_TRUE(rec.results[i].second.isEmpty()) << i;
    for (int i = 3; i < 15; ++i)
        EXPECT_TRUE(rec.results[i].second.contains(QLatin1String(reasons[i - 3]))) << i << ' ' << rec.results[i].second.toStdString();
    ASSERT_EQ(rec.applied.size(), 3);
    // The window gets the checked form: colors as indices or lower-case hex.
    const QVariantMap bar = rec.applied[0];
    EXPECT_EQ(bar["edge"], QStringLiteral("top"));
    EXPECT_EQ(bar["size"].toInt(), 1);
    EXPECT_TRUE(bar["tab_bar"].toBool());
    EXPECT_EQ(bar["fill"].toInt(), 3);
    EXPECT_FALSE(rec.applied[1].contains("fill")); // absent stays absent: the theme's background
    const QVariantMap first = bar["lines"].toList().at(0).toList().at(0).toMap();
    EXPECT_EQ(first["fg"].toInt(), 1);
    EXPECT_EQ(first["bg"], QStringLiteral("#10a0ff"));
    EXPECT_EQ(first["item"], QStringLiteral("tab-1"));
    EXPECT_TRUE(rec.applied[1]["lines"].toList().isEmpty());
    EXPECT_EQ(rec.applied[2]["kind"], QStringLiteral("undock"));
}

TEST_F(ScriptHostTest, DockEffectsHaveABudgetOfTheirOwn)
{
    // Each budget in one handler call, so that the second of the budgets
    // cannot end between two calls.
    WRITE(user, "general-first.luau", R"(
return { on_bell = function(ev)
    local out = {}
    for i = 1, 20 do table.insert(out, afterglow.set_title(ev.pane, "t" .. i)) end
    for i = 1, 40 do table.insert(out, afterglow.dock(ev.pane, {id="bar", edge="top", lines={{{text=tostring(i)}}}})) end
    table.insert(out, afterglow.tab_menu(ev.pane))
    return out
end }
)");
    WRITE(user, "docks-first.luau", R"(
return { on_bell = function(ev)
    local out = {}
    for i = 1, 60 do table.insert(out, afterglow.dock(ev.pane, {id="bar", edge="top", lines={{{text=tostring(i)}}}})) end
    table.insert(out, afterglow.dock(ev.pane, {id="bar", edge="top", lines={{{text="over"}}}}))
    for i = 1, 3 do table.insert(out, afterglow.set_title(ev.pane, "t" .. i)) end
    return out
end }
)");
    host->setEnabled(true);
    auto count = [&](const QString& owner, const char* kind) {
        int n = 0;
        for (qsizetype i = 0; i < rec.applied.size(); ++i)
            n += rec.owners.value(i).startsWith(owner) && rec.applied[i]["kind"] == QLatin1String(kind);
        return n;
    };
    // The general budget spent: the docks still go out, and the tab menu,
    // which is not a dock, does not. The docks' budget spent first: the
    // general effects still go out.
    host->dispatch("on_bell", 7);
    ASSERT_EQ(rec.owners.size(), rec.applied.size());
    EXPECT_EQ(count("general-first", "dock"), 40);
    EXPECT_EQ(count("general-first", "set_title"), ScriptHost::MaxEffectsPerSecond);
    EXPECT_EQ(count("general-first", "tab_menu"), 0);
    EXPECT_EQ(count("docks-first", "dock"), ScriptHost::MaxDockEffectsPerSecond);
    EXPECT_EQ(count("docks-first", "set_title"), 3);
    // Past their own budget, docks are dropped as rate_limited.
    int refused = 0;
    for (const auto& result : std::as_const(rec.results))
        refused += result.first["kind"] == QLatin1String("dock") && result.second.contains(QLatin1String("Too many effects"));
    EXPECT_EQ(refused, 1);
}

TEST_F(ScriptHostTest, AScriptThatRequestsAllItsBudgetsAllowIsNotDisabledForItsResults)
{
    WRITE(user, "requests.luau", R"(
return { on_bell = function(ev)
    local out = {}
    for i = 1, 20 do table.insert(out, afterglow.request("t" .. i, afterglow.set_title(ev.pane, "t" .. i))) end
    for i = 1, 40 do
        table.insert(out, afterglow.request("d" .. i, afterglow.dock(ev.pane, {id="bar", edge="top", lines={{{text=tostring(i)}}}})))
    end
    return out
end, on_title = function(ev)
    local out = {}
    for i = 1, 20 do
        table.insert(out, afterglow.request("e" .. i, afterglow.dock(ev.pane, {id="bar", edge="top", lines={{{text="x" .. i}}}})))
    end
    return out
end, on_result = function(ev) end }
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    host->dispatch("on_title", 7);
    for (int i = 0; i < 4; ++i) QCoreApplication::processEvents();
    EXPECT_FALSE(host->isDisabled("requests"));
    EXPECT_EQ(rec.applied.size(), ScriptHost::MaxEffectsPerSecond + ScriptHost::MaxDockEffectsPerSecond);
}

TEST_F(ScriptHostTest, TheTabMenuEffectNamesItsPane)
{
    WRITE(user, "menu.luau", R"(
return { on_bell = function(ev) return { afterglow.tab_menu(ev.pane), afterglow.tab_menu(8) } end }
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_EQ(rec.applied.size(), 2);
    EXPECT_EQ(rec.applied[0]["kind"], QStringLiteral("tab_menu"));
    EXPECT_EQ(rec.applied[0]["pane"].toUInt(), 7u);
    EXPECT_EQ(rec.applied[1]["pane"].toUInt(), 8u);
}

TEST_F(ScriptHostTest, MoveTabNeedsAWholeIndexOfZeroOrMore)
{
    // The largest index passes on: only the window knows its tab count.
    WRITE(user, "move.luau", R"(
return { on_bell = function(ev)
    return { afterglow.move_tab(ev.pane, 2), afterglow.move_tab(8, 0), afterglow.move_tab(ev.pane, 2^31 - 1),
             afterglow.move_tab(ev.pane, -1), afterglow.move_tab(ev.pane, 1.5), afterglow.move_tab(ev.pane, "1"),
             afterglow.move_tab(ev.pane), afterglow.move_tab(ev.pane, 2^31), afterglow.move_tab(ev.pane, math.huge) }
end }
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_EQ(rec.applied.size(), 3);
    EXPECT_EQ(rec.applied[0]["kind"], QStringLiteral("move_tab"));
    EXPECT_EQ(rec.applied[0]["pane"].toUInt(), 7u);
    EXPECT_EQ(rec.applied[0]["index"].toInt(), 2);
    EXPECT_EQ(rec.applied[1]["pane"].toUInt(), 8u);
    EXPECT_EQ(rec.applied[1]["index"].toInt(), 0);
    EXPECT_EQ(rec.applied[2]["index"].toDouble(), 2147483647.0);
    const QString range = QStringLiteral("move_tab needs a tab index of 0 or more");
    const QString reasons[] = {range, range, QStringLiteral("move_tab: index must be a number"), range, range, range};
    ASSERT_EQ(rec.results.size(), 3 + int(std::size(reasons)));
    for (int i = 0; i < 3; ++i) EXPECT_TRUE(rec.results[i].second.isEmpty()) << i;
    for (int i = 3; i < rec.results.size(); ++i) EXPECT_EQ(rec.results[i].second, reasons[i - 3]) << i;
}

TEST_F(ScriptHostTest, DocksHaveAWindowOrAPaneScope)
{
    WRITE(user, "scope.luau", R"(
return { on_bell = function(ev)
    return { afterglow.dock(ev.pane, {id="a", edge="top", scope="pane", lines={{{text="x"}}}}),
             afterglow.dock(ev.pane, {id="b", edge="bottom"}),
             afterglow.undock(ev.pane, "a", "pane"),
             afterglow.undock(ev.pane, "b"),
             afterglow.dock(ev.pane, {id="c", edge="top", scope="pane", tab_bar=true}),
             afterglow.dock(ev.pane, {id="d", edge="top", scope="tab"}),
             afterglow.undock(ev.pane, "e", "screen") }
end }
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_EQ(rec.applied.size(), 4);
    EXPECT_EQ(rec.applied[0]["scope"], QStringLiteral("pane"));
    EXPECT_EQ(rec.applied[0]["edge"], QStringLiteral("top"));
    EXPECT_EQ(rec.applied[1]["scope"], QStringLiteral("window"));
    EXPECT_EQ(rec.applied[2]["kind"], QStringLiteral("undock"));
    EXPECT_EQ(rec.applied[2]["scope"], QStringLiteral("pane"));
    EXPECT_EQ(rec.applied[3]["scope"], QStringLiteral("window"));
    ASSERT_EQ(rec.results.size(), 7);
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(rec.results[i].second.isEmpty()) << i;
    EXPECT_EQ(rec.results[4].second, QStringLiteral("Only a window dock can replace the tab bar"));
    EXPECT_EQ(rec.results[5].second, QStringLiteral("Dock scope must be window or pane"));
    EXPECT_EQ(rec.results[6].second, QStringLiteral("Dock scope must be window or pane"));
}

TEST_F(ScriptHostTest, DockClicksReachOnlyTheDocksOwner)
{
    // Each script docks once, which names its owner, then answers clicks.
    const char* source = R"(
return {
    on_bell = function(ev) return afterglow.dock(ev.pane, {id="bar", edge="top", lines={}}) end,
    on_dock_click = function(ev) return afterglow.copy(ev.dock .. ":" .. ev.item .. ":" .. ev.button) end,
}
)";
    WRITE(user, "owner.luau", source);
    WRITE(user, "other.luau", source);
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_EQ(rec.owners.size(), 2);
    const QString owner = rec.owners[0];
    rec.applied.clear();
    host->dispatchTo(owner, "on_dock_click", 7, {{"dock", "bar"}, {"item", "tab-2"}, {"button", "middle"}, {"window", 1}});
    ASSERT_EQ(rec.applied.size(), 1);
    EXPECT_EQ(rec.applied[0]["text"], QStringLiteral("bar:tab-2:middle"));
    EXPECT_EQ(rec.owners.last(), owner);
}

TEST_F(ScriptHostTest, DockEffectsRefuseEachFieldOutsideItsForm)
{
    WRITE(user, "dockforms.luau", R"(
local function lines(count, runs, run)
    local out = {}
    for i = 1, count do
        local line = {}
        for j = 1, runs do line[j] = run end
        out[i] = line
    end
    return out
end
return { on_bell = function(ev)
    local d = function(spec) return afterglow.dock(ev.pane, spec) end
    return {
        d({id="ok", edge="left", lines=lines(256, 4, {text="x"})}),
        d({id="ok", edge="top", lines={{{}, {spacer=true, item="gap"}}}}),
        d({id="ok", edge="top", lines={{{text="A", item=string.rep("\u{1F600}", 64)}}}}),
        d({id="x", edge="top", size=0/0}),
        d({id="x", edge="top", size=math.huge}),
        d({id="x", edge="left", size=1e300}),
        d({id="x", edge="top", fill="#0x1234"}),
        d({id="x", edge="top", fill="#+abcde"}),
        d({id="x", edge="top", lines={{{text="A", bg="# abcde"}}}}),
        d({id="x", edge="top", lines={{{text="A", item="a\nb"}}}}),
        d({id="x", edge="top", lines={{{text="A", item=string.rep("i", 65)}}}}),
        d({id="x", edge="top", lines=lines(1, 129, {text="x"})}),
        d({id="x", edge="right", lines=lines(257, 1, {text="x"})}),
        d({id="x", edge="right", lines=lines(9, 128, {text="x"})}),
        d({id="x", edge="right", lines=lines(4, 128, {item=string.rep("i", 60)})}),
        d({id="x", edge="top", lines={{{text="A", spacer="yes"}}}}),
        d({id="x", edge="top", tab_bar="yes"}),
        d({id="x", edge="top", lines={{{spacer=true, text="ignored"}}}}),
        d({id="x", edge="top", line={{{text="A"}}}}),
        d({id=5, edge="top"}),
        {kind="undock", pane=ev.pane.id, id=true},
        d({id="x", lines={}}),
    }
end }
)");
    host->setEnabled(true); host->dispatch("on_bell", 7);
    const char* reasons[] = {"1 to 8 rows", "1 to 8 rows", "1 to 80 columns", "Dock colors", "Dock colors", "Dock colors",
                             "A dock item", "A dock item", "at most 128 runs", "at most 256 lines", "at most 1024 runs",
                             "exceed 16384 bytes", "spacer must be a boolean", "tab_bar must be a boolean",
                             "A spacer run has no text", "Unknown dock field line", "A dock id", "A dock id",
                             "Dock edge must be"};
    constexpr int accepted = 3;
    ASSERT_EQ(rec.results.size(), accepted + int(std::size(reasons)));
    for (int i = 0; i < accepted; ++i) EXPECT_TRUE(rec.results[i].second.isEmpty()) << i << ' ' << rec.results[i].second.toStdString();
    for (int i = accepted; i < rec.results.size(); ++i)
        EXPECT_TRUE(rec.results[i].second.contains(QLatin1String(reasons[i - accepted]))) << i << ' ' << rec.results[i].second.toStdString();
    ASSERT_EQ(rec.applied.size(), accepted);
    EXPECT_EQ(rec.applied[0]["lines"].toList().size(), 256);
    // An empty run is no error, and a spacer may be clickable.
    const QVariantList runs = rec.applied[1]["lines"].toList().at(0).toList();
    ASSERT_EQ(runs.size(), 2);
    EXPECT_TRUE(runs[0].toMap().isEmpty());
    EXPECT_EQ(runs[1].toMap()["item"], QStringLiteral("gap"));
}

TEST_F(ScriptHostTest, UndockReachesTheWindowAfterItsPaneClosed)
{
    WRITE(user, "closing.luau", R"(
return {
    on_bell = function(ev)
        return {afterglow.dock(ev.pane, {id="bar", edge="top"}), afterglow.dock(ev.pane, {id="title", edge="top", scope="pane"})}
    end,
    on_pane_closed = function(ev)
        return {afterglow.undock(ev.pane.id, "bar"), afterglow.undock(ev.pane.id, "title", "pane"), afterglow.focus(ev.pane.id)}
    end,
}
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 9);
    paneState.remove(9);
    rec.applied.clear();
    host->dispatch("on_pane_closed", 9, {{"pane", QVariantMap{{"id", 9}, {"tab_id", 20}, {"window", 2}}}});
    // A window area stays in its window when its pane goes: only the window
    // side can find it. A pane area closes with its pane: the undock still
    // goes there, with its scope, and the window side answers no_pane.
    ASSERT_EQ(rec.applied.size(), 2);
    EXPECT_EQ(rec.applied[0]["kind"], QStringLiteral("undock"));
    EXPECT_EQ(rec.applied[0]["pane"].toUInt(), 9u);
    EXPECT_EQ(rec.applied[0]["scope"], QStringLiteral("window"));
    EXPECT_EQ(rec.applied[1]["kind"], QStringLiteral("undock"));
    EXPECT_EQ(rec.applied[1]["pane"].toUInt(), 9u);
    EXPECT_EQ(rec.applied[1]["scope"], QStringLiteral("pane"));
    EXPECT_EQ(rec.applied[1]["id"], QStringLiteral("title"));
    EXPECT_TRUE(rec.results.last().second.contains("no pane 9")); // other pane effects are still refused
}

TEST_F(ScriptHostTest, DockClicksAreNotDroppedWhenEventsSpendTheCallbackBudget)
{
    WRITE(user, "clicks.luau", R"(
return {
    on_bell = function(ev) return afterglow.dock(ev.pane, {id="bar", edge="top"}) end,
    on_title = function() return nil end,
    on_dock_click = function(ev) return afterglow.copy(ev.item) end,
}
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_EQ(rec.owners.size(), 1);
    // A program that rewrites its title spends the budget of the second.
    for (int i = 0; i < ScriptHost::MaxCallbacksPerSecond; ++i) host->dispatch("on_title", 7);
    ASSERT_GT(firstDiagnostic()["dropped_callbacks"].toULongLong(), 0u);
    rec.applied.clear();
    host->dispatchTo(rec.owners[0], "on_dock_click", 7, {{"dock", "bar"}, {"item", "tab-2"}, {"button", "left"}, {"window", 1}});
    ASSERT_EQ(rec.applied.size(), 1);
    EXPECT_EQ(rec.applied[0]["text"], QStringLiteral("tab-2"));
    // An assistive technology's press can come without end: it counts, and
    // the spent budget drops it.
    const auto dropped = firstDiagnostic()["dropped_callbacks"].toULongLong();
    host->dispatchTo(rec.owners[0], "on_dock_click", 7, {{"dock", "bar"}, {"item", "tab-3"}, {"button", "left"}, {"window", 1}}, true);
    EXPECT_EQ(rec.applied.size(), 1);
    EXPECT_EQ(firstDiagnostic()["dropped_callbacks"].toULongLong(), dropped + 1);
}

TEST_F(ScriptHostTest, TimersAndWatchersHaveTheirOwnCallbackBudget)
{
    WRITE(user, "ticks.luau", R"(
return {
    on_bell = function(ev) return afterglow.dock(ev.pane, {id="bar", edge="top"}) end,
    on_timer = function() return nil end,
    on_file_changed = function() return nil end,
    on_key = function() return nil end,
}
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_EQ(rec.owners.size(), 1);
    const auto before = firstDiagnostic();
    for (int i = 0; i < ScriptHost::MaxCallbacksPerSecond + 6; ++i) host->dispatchTo(rec.owners[0], "on_timer", 0, {{"name", "t"}});
    auto stats = firstDiagnostic();
    EXPECT_EQ(stats["calls"].toULongLong() - before["calls"].toULongLong(), quint64(ScriptHost::MaxCallbacksPerSecond));
    EXPECT_EQ(stats["dropped_callbacks"].toULongLong() - before["dropped_callbacks"].toULongLong(), 6u);
    // Watchers share the timers' budget, which the second has spent.
    host->dispatchTo(rec.owners[0], "on_file_changed", 0, {{"name", "w"}});
    EXPECT_EQ(firstDiagnostic()["dropped_callbacks"].toULongLong(), stats["dropped_callbacks"].toULongLong() + 1);
    stats = firstDiagnostic();
    // Keys are not counted: a key mode must not lose its keys.
    for (int i = 0; i < ScriptHost::MaxCallbacksPerSecond + 6; ++i) host->dispatchTo(rec.owners[0], "on_key", 7, {{"key", "a"}});
    EXPECT_EQ(firstDiagnostic()["dropped_callbacks"].toULongLong(), stats["dropped_callbacks"].toULongLong());
}

TEST_F(ScriptHostTest, TimersFireWhenEventsSpendTheCallbackBudget)
{
    WRITE(user, "timers.luau", R"(
return {
    on_bell = function() return afterglow.timer("later", 50) end,
    on_title = function() return nil end,
    on_timer = function(ev) return afterglow.copy(ev.name) end,
}
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    // A title storm spends the second's budget while the timer runs.
    for (int i = 0; i < ScriptHost::MaxCallbacksPerSecond + 6; ++i) host->dispatch("on_title", 7);
    ASSERT_GT(firstDiagnostic()["dropped_callbacks"].toULongLong(), 0u);
    rec.applied.clear();
    ASSERT_TRUE(await([&] { return !rec.applied.isEmpty(); }));
    EXPECT_EQ(rec.applied[0]["text"], QStringLiteral("later"));
}

TEST_F(ScriptHostTest, RetiredInstanceDropsPendingUiResultsAndCleansOwnedResources)
{
    WRITE(user, "retire.luau", R"(
return {
    on_bell = function(ev) return afterglow.request("ui",afterglow.prompt(ev.pane,"Prompt")) end,
    on_result = function() return afterglow.copy("obsolete") end,
}
)");
    host->setEnabled(true); host->dispatch("on_bell", 7);
    ASSERT_TRUE(bool(pendingReply));
    EXPECT_TRUE(rec.applied.isEmpty());
    WRITE(user, "retire.luau", "return {}"); host->reload();
    ASSERT_EQ(cleaned.size(), 1);
    // The window still holds the prompt, and answers it late.
    ASSERT_TRUE(bool(pendingReply));
    pendingReply({{}, {{"text","late"}}, {}});
    QCoreApplication::processEvents();
    EXPECT_TRUE(rec.applied.isEmpty());
    host->setEnabled(false);
    EXPECT_EQ(cleaned.size(), 2);
}

TEST_F(ScriptHostTest, PromptResultsAreDeferredAndDeliveredToTheirOwner)
{
    WRITE(user, "prompt.luau", R"(
return {
    on_bell = function(ev) return afterglow.request("ui",afterglow.prompt(ev.pane,"Prompt")) end,
    on_result = function(ev)
        assert(ev.request_id=="ui" and ev.ok)
        return afterglow.copy(ev.data.text)
    end,
}
)");
    host->setEnabled(true); host->dispatch("on_bell", 7);
    ASSERT_TRUE(bool(pendingReply));
    QCoreApplication::processEvents();
    EXPECT_TRUE(rec.applied.isEmpty());
    pendingReply({{}, {{"text","accepted"}}, {}});
    QCoreApplication::processEvents();
    ASSERT_EQ(rec.applied.size(), 1);
    EXPECT_EQ(rec.applied[0]["text"], QStringLiteral("accepted"));
}


TEST_F(ScriptHostTest, BuildInspectorExtractsDiagnosticsIntoASelector)
{
    QFile example(QStringLiteral(AFTERGLOW_EXAMPLE_SCRIPTS) + QStringLiteral("/build-inspector.luau"));
    ASSERT_TRUE(example.open(QIODevice::ReadOnly));
    WRITE(user, "inspector.luau", example.readAll());
    applyHook = [](const QVariantMap& effect) -> ScriptHost::EffectResult {
        if (effect["kind"] == QLatin1String("semantic_regions"))
            return {{}, {{"regions", QVariantList{QVariantMap{{"id", "5"}, {"kind", "output"}, {"available", true}}}}}, {}};
        if (effect["kind"] == QLatin1String("read_region"))
            return {{}, {{"available",true},{"truncated",false},
                {"text","src/main.cpp:42:3: error: broken\nlib/other.cpp:7: warning\n"}} , {}};
        return {};
    };
    host->setEnabled(true);
    ASSERT_TRUE(run("build-inspector.inspect", 7));
    pump();
    ASSERT_EQ(rec.applied.size(), 3);
    EXPECT_EQ(rec.applied[1]["kind"], QStringLiteral("read_region"));
    EXPECT_EQ(rec.applied[1]["region"], QStringLiteral("5"));
    EXPECT_EQ(rec.applied[1]["max_bytes"].toDouble(), 262144.0);
    EXPECT_EQ(rec.applied[2]["kind"], QStringLiteral("select"));
    const auto items = rec.applied[2]["items"].toList();
    ASSERT_EQ(items.size(), 2);
    EXPECT_EQ(items[0].toMap()["id"], QStringLiteral("1"));
    EXPECT_TRUE(items[1].toMap()["title"].toString().startsWith("lib/other.cpp:7:"));
}

TEST_F(ScriptHostTest, ProjectHelpersPreserveExplicitIntentAndResults)
{
    WRITE(user, QStringLiteral("projects.luau"), R"(
return { on_bell = function(ev)
    return {afterglow.save_project(ev.pane, "dev", true), afterglow.open_project("dev", true), afterglow.list_projects(),
            afterglow.move_pane(ev.pane, 8, "left"), afterglow.detach_tab(ev.pane)}
end }
)");
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_bell"), 7);
    ASSERT_EQ(rec.applied.size(), 5);
    for (const auto& result : rec.results) EXPECT_TRUE(result.second.isEmpty()) << result.second.toStdString();
    EXPECT_EQ(rec.applied[0].value("pane").toUInt(), 7u);
    EXPECT_EQ(rec.applied[0].value("name").toString(), QStringLiteral("dev"));
    EXPECT_TRUE(rec.applied[0].value("overwrite").toBool());
    EXPECT_TRUE(rec.applied[1].value("new_instance").toBool());
    EXPECT_EQ(rec.applied[2].value("kind").toString(), QStringLiteral("list_projects"));
    EXPECT_EQ(rec.applied[3].value("target").toUInt(), 8u);
}
TEST_F(ScriptHostTest, ProjectLaunchesShareStructuralBudgetAndRejectNonBooleanFlags)
{
    WRITE(user, QStringLiteral("projects.luau"), R"(
return { on_bell = function(ev)
    local out = {{kind="open_project",name="dev",new_instance="yes"}, {kind="save_project",name="dev",overwrite=1}}
    for i = 1, 10 do table.insert(out, afterglow.open_project("dev")) end
    return out
end }
)");
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_bell"), 7);
    EXPECT_EQ(rec.applied.size(), ScriptHost::MaxStructuralPer10s);
    ASSERT_GE(rec.results.size(), 2);
    EXPECT_FALSE(rec.results[0].second.isEmpty());
    EXPECT_FALSE(rec.results[1].second.isEmpty());
}

TEST_F(ScriptHostTest, ProjectExampleChainsListingSelectionAndLaunch)
{
    host->setSearchPaths({QStringLiteral(AFTERGLOW_EXAMPLE_SCRIPTS)});
    applyHook = [](const QVariantMap& effect) -> ScriptHost::EffectResult {
        const auto kind = effect.value("kind").toString();
        if (kind == QLatin1String("list_projects")) return {{}, {{"names", QStringList{"dev"}}}, {}};
        if (kind == QLatin1String("select")) return {{}, {{"id", "dev"}}, {}};
        if (kind == QLatin1String("open_project")) return {{}, {{"window_id", 5}, {"reused", true}}, {}};
        return {};
    };
    host->setEnabled(true);
    // Its on_load lists the projects for their open-NAME commands.
    pump();
    rec.applied.clear();
    ASSERT_TRUE(run("projects.open", 7));
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    ASSERT_EQ(rec.applied.size(), 3);
    EXPECT_EQ(rec.applied[1].value("kind").toString(), QStringLiteral("select"));
    EXPECT_EQ(rec.applied[1].value("items").toList().first().toMap().value("title").toString(), QStringLiteral("dev"));
    EXPECT_EQ(rec.applied[2].value("kind").toString(), QStringLiteral("open_project"));
    EXPECT_EQ(rec.applied[2].value("name").toString(), QStringLiteral("dev"));
    for (const auto& result : rec.results) EXPECT_TRUE(result.second.isEmpty()) << result.second.toStdString();
}

TEST_F(ScriptHostTest, PackagesCacheModulesAndRetainLastGoodOnBrokenModuleEdits)
{
    QDir().mkpath(user.filePath("bundle"));
    WRITE(user,"bundle/package.json",R"({"id":"bundle","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"init.luau","counter":"counter.luau"}})");
    WRITE(user,"bundle/init.luau",R"(
local count=require("counter")
assert(count==require("counter"))
return {on_bell=function() return afterglow.copy(count.next()) end}
)");
    WRITE(user,"bundle/counter.luau","local i=0; return {next=function() i+=1; return tostring(i) end}");
    host->setEnabled(true);
    host->dispatch("on_bell",7); ASSERT_EQ(rec.applied.size(),1); EXPECT_EQ(rec.applied.last().value("text"),"1");
    WRITE(user,"bundle/counter.luau","broken syntax !"); host->reload();
    host->dispatch("on_bell",7); ASSERT_FALSE(rec.applied.isEmpty()); EXPECT_EQ(rec.applied.last().value("text"),"2");
    WRITE(user,"bundle/counter.luau","return {next=function() return 'fixed' end}"); host->reload();
    host->dispatch("on_bell",7); EXPECT_EQ(rec.applied.last().value("text"),"fixed");
}
TEST_F(ScriptHostTest, PackageMigrationAndInitializationAreTransactional)
{
    QDir().mkpath(user.filePath("bundle"));
    auto manifest=[&](int schema) { WRITE(user,"bundle/package.json",QString(R"({"id":"bundle","version":"1.0.0","api_version":1,"state_version":%1,"entry":"main","modules":{"main":"init.luau"},"config":{"label":"test"}})").arg(schema).toUtf8()); };
    ASSERT_NO_FATAL_FAILURE(manifest(1));
    WRITE(user,"bundle/init.luau",R"(
return {on_load=function(ev)
 assert(ev.package.config.label=="test")
 return afterglow.state_write({count=41},ev.storage.revision)
end,on_bell=function() return afterglow.copy("original") end}
)");
    host->setEnabled(true);
    ScriptState::Snapshot stored;
    ScriptState store(stateDir.path());
    ASSERT_TRUE(store.read("bundle",stored).isEmpty()); ASSERT_EQ(stored.version,1);
    ASSERT_NO_FATAL_FAILURE(manifest(2));
    WRITE(user,"bundle/init.luau",R"(
return {migrate=function(value,from,to) assert(from==1 and to==2); return {count=value.count+1} end,
on_load=function(ev) error("broken initialization") end}
)");
    host->reload();
    host->dispatch("on_bell",7); ASSERT_FALSE(rec.applied.isEmpty()); EXPECT_EQ(rec.applied.last().value("text"),"original");
    ASSERT_TRUE(store.read("bundle",stored).isEmpty()); EXPECT_EQ(stored.version,1);
    WRITE(user,"bundle/init.luau",R"(
return {migrate=function(value,from,to) return {count=value.count+1} end,
on_load=function(ev) assert(ev.storage.value.count==42 and ev.storage.revision==2) end,
on_bell=function() return afterglow.copy("migrated") end}
)");
    host->reload(); host->dispatch("on_bell",7); EXPECT_EQ(rec.applied.last().value("text"),"migrated");
    ASSERT_TRUE(store.read("bundle",stored).isEmpty()); EXPECT_EQ(stored.version,2); EXPECT_EQ(stored.value.value("count").toInt(),42);
    host->setEnabled(false); host->setEnabled(true);
    EXPECT_EQ(host->scriptIds(),QStringList{"bundle"});
}
TEST_F(ScriptHostTest, PackagesRejectMissingDependenciesAndCycles)
{
    QDir().mkpath(user.filePath("bundle/vendor/lib"));
    WRITE(user,"bundle/package.json",R"({"id":"bundle","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"init.luau"},"dependencies":{"lib":{"path":"vendor/lib","version":"1.0.0"}}})");
    WRITE(user,"bundle/init.luau","local lib=require('@lib'); return {on_bell=function() return afterglow.copy(lib.label) end}");
    host->setEnabled(true); EXPECT_TRUE(host->scriptIds().isEmpty());
    WRITE(user,"bundle/vendor/lib/package.json",R"({"id":"lib","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"lib.luau"}})");
    WRITE(user,"bundle/vendor/lib/lib.luau","return {label='dependency'}");
    host->reload(); host->dispatch("on_bell",7); ASSERT_FALSE(rec.applied.isEmpty()); EXPECT_EQ(rec.applied.last().value("text"),"dependency");
    WRITE(user,"bundle/vendor/lib/lib.luau","return require('main')");
    // The broken edit keeps the instance that works: this bell is its own.
    rec.applied.clear();
    host->reload(); host->dispatch("on_bell",7); ASSERT_EQ(rec.applied.size(),1); EXPECT_EQ(rec.applied.last().value("text"),"dependency");
    // The cycle is found, not a stack overflow that happens to fail the load too.
    bool reported = false;
    for (const auto& entry : host->diagnostics()) reported |= entry.toMap()["error"].toString().contains("cycle");
    EXPECT_TRUE(reported);
}

// In the one chunk of a package, an open block takes the end of its module's
// factory: the parser then fails at the end of the chunk, or in a later
// module. Each module parses alone first, so the real Luau error names the
// module that breaks, with the line of its own file. The messages are those
// of Luau's parser (Ast/src/Parser.cpp): parseElseBody matches the 'end' of
// an if to its 'then' (expectMatchAndConsumeFail, "at line N" when the lines
// differ), and parseChunk reports a token after the last statement with
// expectAndConsumeFail(Eof): "Expected <eof>, got 'end'".
TEST_F(ScriptHostTest, AParseErrorNamesTheModuleThatHasIt)
{
    QDir().mkpath(user.filePath("parsing"));
    // alpha sorts first and is not the last module of the chunk.
    WRITE(user, "parsing/package.json", R"({"id":"parsing","version":"1.0.0","api_version":1,"entry":"main",
        "modules":{"alpha":"alpha.luau","main":"init.luau","zeta":"zeta.luau"}})");
    WRITE(user, "parsing/init.luau", "local alpha = require('alpha')\nlocal zeta = require('zeta')\nreturn {}");
    WRITE(user, "parsing/zeta.luau", "return {}");
    WRITE(user, "parsing/alpha.luau", "local x = 1\nif x then\nreturn {}");
    auto error = [&] {
        for (const QVariant& d : host->diagnostics())
            if (QFileInfo(d.toMap().value(QStringLiteral("path")).toString()).dir().dirName() == QLatin1String("parsing"))
                return d.toMap().value(QStringLiteral("error")).toString();
        return QString();
    };
    host->setEnabled(true);
    EXPECT_TRUE(host->scriptIds().isEmpty());
    // The eof is on line 3, as the file has no line break at its end.
    EXPECT_EQ(error(), QStringLiteral("alpha.luau:3: Expected 'end' (to close 'then' at line 2), got <eof>"));
    WRITE(user, "parsing/alpha.luau", "local M = {}\nreturn M\nend");
    host->reload();
    EXPECT_TRUE(host->scriptIds().isEmpty());
    EXPECT_EQ(error(), QStringLiteral("alpha.luau:3: Expected <eof>, got 'end'"));
    // Fixed, the package loads.
    WRITE(user, "parsing/alpha.luau", "local M = {}\nreturn M");
    host->reload();
    EXPECT_EQ(host->scriptIds(), QStringList{QStringLiteral("parsing")});
}

TEST_F(ScriptHostTest, NavigatorLaunchesChosenLayoutAndPersistsFavoritesAcrossReload)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("project-navigator"));
    // A path of this test alone, under no symbolic link: realpath keeps it
    // as it is. It does not exist, and realpath -m takes it as it is.
    const QString root = QFileInfo(files.path()).canonicalFilePath() + "/navigator-project";
    ASSERT_FALSE(QFileInfo::exists(root));
    WRITE(user,"project-navigator/config.json",QJsonDocument(QJsonObject{{"roots",QJsonArray{root}},{"fd",false},{"zoxide",false}}).toJson());
    auto pane = paneState[7]; pane["cwd"]=root; paneState[7]=pane;
    applyHook = [root](const QVariantMap& e) -> ScriptHost::EffectResult {
        const auto kind=e.value("kind").toString();
        if (kind=="list_projects") return {{},{{"names",QStringList{"dev"}}},{}};
        if (kind=="select") {
            const auto title=e.value("title").toString();
            if (title.startsWith("Layout for")) return {{},{{"id","layout-1"}},{}};
            for (const auto& item : e.value("items").toList()) if (item.toMap().value("title").toString().contains(root)) return {{},{{"id",item.toMap().value("id")}},{}};
        }
        if (kind=="open_directory") return {{},{{"window_id",3},{"cwd",root},{"reused",false}},{}};
        return {};
    };
    host->setEnabled(true);
    ASSERT_EQ(host->scriptIds(),QStringList{"project-navigator"});
    ASSERT_TRUE(run("project-navigator.pick",7));
    // The picker waits for a realpath job for the root, then launches and saves the layout.
    ASSERT_TRUE(await([&]{ ScriptState::Snapshot s; return ScriptState(stateDir.path()).read("project-navigator", s).isEmpty()
                                  && s.value.value("templates").toMap().value(root) == "dev"; }));
    // Its run ends once the save returns: favorite then finds no busy run.
    pump();
    bool launched=false;
    for (const auto& e : rec.applied) if (e.value("kind")=="open_directory") { launched=true; EXPECT_EQ(e.value("layout"),"dev"); }
    EXPECT_TRUE(launched);
    ASSERT_TRUE(run("project-navigator.favorite",7));
    // The favorite waits for a realpath job first.
    ASSERT_TRUE(await([&]{ ScriptState::Snapshot s; return ScriptState(stateDir.path()).read("project-navigator", s).isEmpty() && s.value.value("favorites").toMap().contains(root); }));
    ScriptState::Snapshot stored; ScriptState store(stateDir.path());
    ASSERT_TRUE(store.read("project-navigator",stored).isEmpty());
    EXPECT_TRUE(stored.value.value("favorites").toMap().value(root).toBool());
    EXPECT_EQ(stored.value.value("templates").toMap().value(root),"dev");
    host->setEnabled(false); host->setEnabled(true);
    EXPECT_EQ(host->scriptIds(),QStringList{"project-navigator"});
    ASSERT_FALSE(host->diagnostics().isEmpty());
    EXPECT_EQ(firstDiagnostic().value("status"),"active");
    for (const auto& result : rec.results) EXPECT_TRUE(result.second.isEmpty()) << result.second.toStdString();
}

TEST_F(ScriptHostTest, NavigatorParsesBoundedProviderRecords)
{
    QDir().mkpath(user.filePath("providers"));
    ASSERT_TRUE(QFile::copy(QStringLiteral(AFTERGLOW_EXAMPLE_PACKAGES)+"/project-navigator/providers.luau",user.filePath("providers/providers.luau")));
    WRITE(user,"providers/package.json",R"({"id":"providers","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"main.luau","providers":"providers.luau"}})");
    WRITE(user,"providers/main.luau",R"(
local p=require("providers")
return {on_bell=function()
 local paths=p.parse("fd","/tmp/with spaces/.git\0/tmp/worktree/.git/\0/truncated","/home/test")
 assert(#paths==2 and paths[1]=="/tmp/with spaces" and paths[2]=="/tmp/worktree")
 local z=p.parse("zoxide","/tmp/one\n/tmp/two\npartial","/home/test")
 assert(#z==2 and z[2]=="/tmp/two")
 local jobs,roots=p.jobs({roots={"~/Projects"},fd=true,zoxide=true,max_depth=100},"/home/test")
 assert(#jobs==2 and roots[1]=="/home/test/Projects" and jobs[1].command[5]=="16")
 return afterglow.copy("parsed")
end}
)");
    host->setEnabled(true); host->dispatch("on_bell",7);
    ASSERT_EQ(rec.applied.size(),1); EXPECT_EQ(rec.applied[0].value("text"),"parsed");
}
TEST_F(ScriptHostTest, NavigatorMissingProviderAndCancelledPickerRemainUsable)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("project-navigator"));
    WRITE(user,"project-navigator/config.json",R"({"roots":["/tmp"],"fd":true,"zoxide":false})");
    QFile provider(user.filePath("project-navigator/providers.luau")); ASSERT_TRUE(provider.open(QIODevice::ReadOnly));
    auto source=provider.readAll(); provider.close();
    ASSERT_TRUE(source.contains("command={\"fd\""));
    source.replace("command={\"fd\"", "command={\"afterglow-nonexistent-fd-test\"");
    WRITE(user,"project-navigator/providers.luau",source);
    auto pane=paneState[7];pane["cwd"]="/tmp";paneState[7]=pane;
    bool selected=false;
    applyHook=[&](const QVariantMap& e) -> ScriptHost::EffectResult {
        if (e.value("kind")=="list_projects") return {{},{{"names",QStringList{}}},{}};
        if (e.value("kind")=="select") {
            selected=true;
            EXPECT_TRUE(e.value("title").toString().contains("fd-1 not started")) << e.value("title").toString().toStdString();
            EXPECT_FALSE(e.value("items").toList().isEmpty());
            return {"Dialog canceled",{},"cancelled"};
        }
        return {};
    };
    host->setEnabled(true); ASSERT_TRUE(run("project-navigator.pick",7));
    QElapsedTimer clock;clock.start();
    while (!selected && clock.elapsed()<3000) { QCoreApplication::processEvents(); QThread::msleep(5); }
    ASSERT_TRUE(selected);
    for (int i=0;i<5;++i) QCoreApplication::processEvents();
    ASSERT_TRUE(run("project-navigator.favorite",7));
    // The favorite waits for a realpath job first.
    ASSERT_TRUE(await([&]{ ScriptState::Snapshot s; return ScriptState(stateDir.path()).read("project-navigator", s).isEmpty() && s.value.value("favorites").toMap().contains("/tmp"); }));
    ScriptState::Snapshot stored;
    ASSERT_TRUE(ScriptState(stateDir.path()).read("project-navigator",stored).isEmpty());
    EXPECT_TRUE(stored.value.value("favorites").toMap().value("/tmp").toBool());
}

TEST_F(ScriptHostTest, NavigatorNamesAProviderThatTimedOut)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("project-navigator"));
    WRITE(user, "project-navigator/config.json", R"({"roots":["/tmp"],"fd":true,"zoxide":false})");
    // A provider that runs past its timeout returns no output at all.
    auto patch = [&](const QString& name, const QByteArray& from, const QByteArray& to) {
        QFile f(user.filePath("project-navigator/" + name));
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        auto source = f.readAll();
        f.close();
        ASSERT_TRUE(source.contains(from)) << from.toStdString();
        WRITE(user, "project-navigator/" + name, source.replace(from, to));
    };
    // An ASSERT in the lambda returns from it only. Unpatched, the provider would run fd over /tmp.
    ASSERT_NO_FATAL_FAILURE(patch("providers.luau", R"(command={"fd", )", R"(command={"/bin/sh", "-c", "exec sleep 5", )"));
    ASSERT_NO_FATAL_FAILURE(patch("init.luau", "timeout_ms=5000", "timeout_ms=100"));
    paneState[7]["cwd"] = "/tmp";
    QString title;
    applyHook = [&](const QVariantMap& e) -> ScriptHost::EffectResult {
        if (e.value("kind") == "list_projects") return {{}, {{"names", QStringList{"dev"}}}, {}};
        if (e.value("kind") == "select") {
            title = e.value("title").toString();
            return {"Dialog canceled", {}, "cancelled"};
        }
        return {};
    };
    host->setEnabled(true);
    ASSERT_TRUE(run("project-navigator.pick", 7));
    ASSERT_TRUE(await([&] { return !title.isEmpty(); }));
    EXPECT_TRUE(title.contains("(unavailable: fd-1 timed out)")) << title.toStdString();
}

TEST_F(ScriptHostTest, PackageReloadAndRemovalRetireJobsTimersWatchesAndUi)
{
    QDir().mkpath(user.filePath("lifecycle"));
    WRITE(user, "lifecycle/package.json", R"({"id":"lifecycle","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"init.luau"},
        "commands":{"dialog":{"summary":"dialog","palette":"dialog"}}})");
    WRITE(files, "watched", "original");
    WRITE(user, "lifecycle/init.luau", R"(
local generation
return {
 on_load=function(ev)
  generation=ev.package.config.generation
  return {afterglow.timer("tick",250,250),
   afterglow.watch_file("watch",ev.package.config.watch),
   afterglow.request("job",afterglow.exec("job",{"/bin/sh","-c",'echo $$ > "$1"; exec sleep 60',"probe",ev.package.config.pid})),
   afterglow.status(7,"generation "..generation)}
 end,
 run_command=function(_,ev) return afterglow.request("dialog",afterglow.prompt(ev.pane,"Pending")) end,
 on_timer=function() return afterglow.copy("tick-"..generation) end,
 on_file_changed=function() return afterglow.copy("watch-"..generation) end,
 on_result=function(ev) return afterglow.copy("late-"..generation) end,
}
)");
    auto config = [&](int generation) {
        QJsonObject value{{"generation",generation},{"pid",files.filePath("pid")},{"watch",files.filePath("watched")}};
        WRITE(user,"lifecycle/config.json",QJsonDocument(value).toJson());
    };
    auto pid = [&]() { QFile file(files.filePath("pid")); return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed().toLongLong() : 0; };
    auto wait = [](const std::function<bool()>& condition) {
        QElapsedTimer timer; timer.start();
        do { QCoreApplication::processEvents(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
            if (condition()) return true;
            QThread::msleep(5);
        } while (timer.elapsed()<3000);
        return false;
    };
    ASSERT_NO_FATAL_FAILURE(config(0)); host->setEnabled(true);
    ASSERT_TRUE(wait([&]{return pid()>0;}));
    qint64 previousPid=pid();
    for (int generation=1;generation<=5;++generation) {
        ASSERT_TRUE(run("lifecycle.dialog",7));
        auto obsoleteReply=std::move(pendingReply);
        ASSERT_TRUE(bool(obsoleteReply));
        ASSERT_NO_FATAL_FAILURE(config(generation)); host->reload();
        ASSERT_TRUE(wait([&]{return pid()>0 && pid()!=previousPid;}));
        ASSERT_TRUE(wait([&]{return ::kill(pid_t(previousPid),0)==-1 && errno==ESRCH;}));
        previousPid=pid();
        EXPECT_EQ(cleaned.size(),generation);
        obsoleteReply({{},{{"text","obsolete"}}, {}});
        rec.applied.clear();
        WRITE(files,"watched",QByteArray::number(generation));
        const auto marker=QStringLiteral("watch-%1").arg(generation);
        ASSERT_TRUE(wait([&]{return std::any_of(rec.applied.begin(),rec.applied.end(),[&](const auto& e){return e.value("text")==marker;});}));
        for (const auto& effect:rec.applied) {
            const auto text=effect.value("text").toString();
            EXPECT_TRUE(text==marker || text==QStringLiteral("tick-%1").arg(generation)) << text.toStdString();
        }
        EXPECT_EQ(host->commands().size(),1);
    }
    ASSERT_TRUE(QDir(user.filePath("lifecycle")).removeRecursively());
    host->reload();
    ASSERT_TRUE(wait([&]{return ::kill(pid_t(previousPid),0)==-1 && errno==ESRCH;}));
    EXPECT_EQ(cleaned.size(),6);
    EXPECT_TRUE(host->commands().isEmpty());
    EXPECT_TRUE(host->diagnostics().isEmpty());
    rec.applied.clear();
    WRITE(files,"watched","after removal");
    QElapsedTimer quiet;quiet.start();
    while(quiet.elapsed()<600) {QCoreApplication::processEvents();QThread::msleep(5);}
    EXPECT_TRUE(rec.applied.isEmpty());
}

TEST_F(ScriptHostTest, AutosaveIsOptInPersistsAndRestoresOnlySelectedProjects)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("project-autosave"));
    windows={QVariantMap{{"id",1},{"project","dev"},{"active_pane",7}}};
    applyHook=[](const QVariantMap& e)->ScriptHost::EffectResult {
        if(e.value("kind")=="list_projects") return {{},{{"names",QStringList{"dev","unrelated"}}},{}};
        if(e.value("kind")=="select") {
            EXPECT_EQ(e.value("items").toList().size(),1);
            EXPECT_EQ(e.value("items").toList().first().toMap().value("title"),"dev");
            return {{},{{"id","1"}},{}};
        }
        return {};
    };
    host->setEnabled(true);
    host->dispatch("on_timer",0,{{"name","autosave"}});pump(12);
    EXPECT_TRUE(rec.applied.isEmpty());
    ASSERT_TRUE(run("project-autosave.enable",7));pump(12);
    ScriptState::Snapshot value;
    ASSERT_TRUE(ScriptState(stateDir.path()).read("project-autosave",value).isEmpty());
    EXPECT_TRUE(value.value.value("enabled").toMap().value("dev").toBool());
    ASSERT_GE(rec.applied.size(),1);
    EXPECT_EQ(rec.applied.first().value("kind"),"save_project");
    EXPECT_TRUE(rec.applied.first().value("overwrite").toBool());
    host->setEnabled(false);host->setEnabled(true);rec.applied.clear();
    host->dispatch("on_timer",0,{{"name","autosave"}});pump(12);
    ASSERT_EQ(rec.applied.size(),1);EXPECT_EQ(rec.applied.first().value("name"),"dev");
    // Duplicate live instances of a named project cannot overwrite each other.
    windows.append(QVariantMap{{"id",2},{"project","dev"},{"active_pane",9}});
    rec.applied.clear();host->dispatch("on_timer",0,{{"name","autosave"}});pump(12);
    EXPECT_TRUE(rec.applied.isEmpty());windows.removeLast();
    ASSERT_TRUE(run("project-autosave.restore",7));pump(12);
    ASSERT_FALSE(rec.applied.isEmpty());
    ASSERT_EQ(rec.applied.last().value("kind"),"open_project");
    EXPECT_EQ(rec.applied.last().value("name"),"dev");
    EXPECT_FALSE(rec.applied.last().value("new_instance").toBool());
    ASSERT_TRUE(run("project-autosave.disable",7));pump(12);rec.applied.clear();
    host->dispatch("on_timer",0,{{"name","autosave"}});pump(12);EXPECT_TRUE(rec.applied.isEmpty());
    for(const auto& result:rec.results) EXPECT_TRUE(result.second.isEmpty()) << result.second.toStdString();
}

// The texts of a dock's first line, separators and spacers left out.
static QStringList dockTexts(const QVariantMap& dock)
{
    QStringList texts;
    for (const QVariant& value : dock.value("lines").toList().value(0).toList()) {
        const QString text = value.toMap().value("text").toString().trimmed();
        if (!text.isEmpty() && text != QString(QChar(0xE0B0)) && text != QString(QChar(0xE0B2))) texts << text;
    }
    return texts;
}

TEST_F(ScriptHostTest, TabLineDrawsATabLineAndPersistsToggle)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("tab-line"));
    WRITE(user,"tab-line/config.json",R"({"git":false,"segments":["cwd","command","exit"]})");
    paneState[7]["cwd"]="/tmp/repository";paneState[7]["context"]="";
    windows={QVariantMap{{"id",1},{"project","dev"},{"active_pane",7}}};
    tabs={QVariantMap{{"id",10},{"window",1},{"index",0},{"title","fish"},{"active",true},{"active_pane",7}},
          QVariantMap{{"id",11},{"window",1},{"index",1},{"title","vim"},{"active",false},{"active_pane",8}}};
    // As the timer calls it: its instance only, which the dock names.
    auto tick=[&]{host->dispatchTo(rec.owners.value(0),"on_timer",0,{{"name","refresh"}});pump();};
    host->setEnabled(true);pump();
    ASSERT_EQ(rec.owners.size(),1);
    tick();
    ASSERT_EQ(rec.applied.size(),1);
    const QVariantMap dock=rec.applied.last();
    EXPECT_EQ(dock.value("kind"),"dock");
    EXPECT_EQ(dock.value("pane").toUInt(),7u);
    EXPECT_EQ(dock.value("edge"),"top");
    EXPECT_TRUE(dock.value("tab_bar").toBool());
    EXPECT_EQ(dockTexts(dock),(QStringList{"dev","1 fish","2 vim","+","repository"}));
    QStringList items;
    for (const QVariant& run : dock.value("lines").toList().value(0).toList())
        if (run.toMap().contains("item")) items << run.toMap().value("item").toString();
    EXPECT_EQ(items,(QStringList{"tab:10","tab:11","new"}));
    // An unchanged line is not sent again.
    tick();EXPECT_EQ(rec.applied.size(),1);
    host->dispatch("on_command_finished",7,{{"exit_code",2}});pump();
    EXPECT_EQ(dockTexts(rec.applied.last()),(QStringList{"dev","1 fish","2 vim","+","repository","exit 2"}));
    // A click on a tab goes to it.
    const QString owner=rec.owners.last();
    host->dispatchTo(owner,"on_dock_click",7,{{"dock","tabline"},{"item","tab:11"},{"button","left"},{"window",1}});pump();
    EXPECT_EQ(rec.applied.last().value("kind"),"focus");
    EXPECT_EQ(rec.applied.last().value("pane").toUInt(),8u);
    // Off, the window gets its tab bar back, and the choice survives a restart.
    ASSERT_TRUE(run("tab-line.toggle",7));pump();
    EXPECT_EQ(rec.applied.last().value("kind"),"undock");
    EXPECT_EQ(rec.applied.last().value("id"),"tabline");
    // Cleared before the restart: on_load draws a line that is on at once.
    // The new instance applies no owned effect, so the test cannot learn its
    // owner: dispatch calls on_timer in every script that has it, here this one.
    host->setEnabled(false);rec.applied.clear();host->setEnabled(true);pump();
    host->dispatch("on_timer",0,{{"name","refresh"}});pump();
    EXPECT_TRUE(rec.applied.isEmpty()) << rec.applied.value(0).value("kind").toString().toStdString();
    for(const auto& result:rec.results) EXPECT_TRUE(result.second.isEmpty()) << result.second.toStdString();
}

TEST_F(ScriptHostTest, TabLineActsLikeATabBarAndFollowsItsDock)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("tab-line"));
    WRITE(user,"tab-line/config.json",R"({"git":false,"segments":["cwd"]})");
    paneState[7]["cwd"]="/tmp/repository";
    windows={QVariantMap{{"id",1},{"project",""},{"active_pane",7},{"active_tab",10},{"cols",200}}};
    auto setTabs=[&](int count) {
        tabs.clear();
        for (int i=0;i<count;++i)
            tabs.append(QVariantMap{{"id",10+i},{"window",1},{"index",i},{"title",QString("t%1").arg(i)},
                                    {"active",i==0},{"active_pane",i==0?7:(i==1?8:100+i)}});
    };
    setTabs(2);
    host->setEnabled(true);pump();
    ASSERT_EQ(rec.applied.size(),1);
    const QString owner=rec.owners.last();
    auto event=[&](const char* handler, QVariantMap ev) {
        ev.insert("dock","tabline");ev.insert("window",1);
        rec.applied.clear();
        host->dispatchTo(owner,handler,7,ev);pump();
    };
    auto runOf=[](const QVariantMap& dock, const QString& item) {
        for (const QVariant& run : dock.value("lines").toList().value(0).toList())
            if (run.toMap().value("item")==item) return run.toMap();
        return QVariantMap{};
    };
    auto wait=[](int ms, const std::function<bool()>& done) {
        QElapsedTimer clock;clock.start();
        while (clock.elapsed()<ms && !done()) { QCoreApplication::processEvents(); QThread::msleep(5); }
    };
    // A tab under the pointer shows in the hover colors, at once.
    event("on_dock_hover",{{"item","tab:11"}});
    ASSERT_EQ(rec.applied.size(),1);
    EXPECT_EQ(runOf(rec.applied.last(),"tab:11").value("bg").toInt(),7);
    EXPECT_EQ(runOf(rec.applied.last(),"tab:10").value("bg").toInt(),4);
    // The current tab keeps its colors under the pointer.
    event("on_dock_hover",{{"item","tab:10"}});
    ASSERT_EQ(rec.applied.size(),1);
    EXPECT_EQ(runOf(rec.applied.last(),"tab:10").value("bg").toInt(),4);
    EXPECT_EQ(runOf(rec.applied.last(),"tab:11").value("bg").toInt(),8);
    event("on_dock_hover",{{"item","new"}});
    ASSERT_EQ(rec.applied.size(),1);
    EXPECT_EQ(runOf(rec.applied.last(),"new").value("bg").toInt(),7);
    // Off the items, the line draws with the draw timer, not at once.
    event("on_dock_hover",{});
    EXPECT_TRUE(rec.applied.isEmpty());
    wait(1000,[&]{return !rec.applied.isEmpty();});
    ASSERT_EQ(rec.applied.size(),1);
    EXPECT_EQ(runOf(rec.applied.last(),"new").value("bg").toInt(),8);
    // Another area of the script is not the line.
    rec.applied.clear();
    host->dispatchTo(owner,"on_dock_hover",7,{{"dock","other"},{"window",1},{"item","tab:11"}});pump();
    EXPECT_TRUE(rec.applied.isEmpty());
    // The buttons of a tab bar.
    event("on_dock_click",{{"item","tab:11"},{"button","middle"}});
    ASSERT_EQ(rec.applied.size(),1);
    EXPECT_EQ(rec.applied.last().value("kind"),"action");
    EXPECT_EQ(rec.applied.last().value("name"),"close_tab");
    EXPECT_EQ(rec.applied.last().value("pane").toUInt(),8u);
    event("on_dock_click",{{"item","tab:11"},{"button","right"}});
    ASSERT_EQ(rec.applied.size(),1);
    EXPECT_EQ(rec.applied.last().value("kind"),"tab_menu");
    EXPECT_EQ(rec.applied.last().value("pane").toUInt(),8u);
    event("on_dock_click",{{"item","new"},{"button","left"}});
    ASSERT_EQ(rec.applied.size(),1);
    EXPECT_EQ(rec.applied.last().value("kind"),"new_tab");
    EXPECT_EQ(rec.applied.last().value("pane").toUInt(),7u);
    // The wheel moves through the tabs and stops at the ends.
    event("on_dock_wheel",{{"direction","down"},{"steps",3}});
    ASSERT_EQ(rec.applied.size(),1);
    EXPECT_EQ(rec.applied.last().value("kind"),"focus");
    EXPECT_EQ(rec.applied.last().value("pane").toUInt(),8u);
    event("on_dock_wheel",{{"direction","up"},{"steps",1}});
    EXPECT_TRUE(rec.applied.isEmpty());
    // The line lays its tabs out for the cells that its dock has.
    setTabs(20);
    event("on_dock_layout",{{"shown",true},{"cols",200},{"rows",1}});
    ASSERT_EQ(rec.applied.size(),1);
    EXPECT_TRUE(dockTexts(rec.applied.last()).contains("20 t19"));
    // The text is ASCII and Powerline separators, one cell for each character.
    auto cellsOf=[](const QVariantMap& dock) {
        qsizetype cells=0;
        for (const QVariant& run : dock.value("lines").toList().value(0).toList())
            cells+=run.toMap().value("text").toString().size();
        return cells;
    };
    auto hidden=[](const QStringList& texts) {
        return std::any_of(texts.begin(),texts.end(),[](const QString& t){return t.size()>1 && t.startsWith('+');});
    };
    event("on_dock_layout",{{"shown",true},{"cols",40},{"rows",1}});
    ASSERT_EQ(rec.applied.size(),1);
    EXPECT_LE(cellsOf(rec.applied.last()),40);
    QStringList texts=dockTexts(rec.applied.last());
    EXPECT_FALSE(texts.contains("20 t19"));
    EXPECT_TRUE(texts.contains("1 t0"));
    EXPECT_TRUE(hidden(texts)) << texts.join('|').toStdString();
    // "+N" takes the hover colors too.
    QString more;
    for (const QVariant& run : rec.applied.last().value("lines").toList().value(0).toList())
        if (run.toMap().value("item").toString().startsWith("more:")) more=run.toMap().value("item").toString();
    ASSERT_FALSE(more.isEmpty());
    event("on_dock_hover",{{"item",more}});
    ASSERT_EQ(rec.applied.size(),1);
    EXPECT_EQ(runOf(rec.applied.last(),more).value("bg").toInt(),7);
    event("on_dock_hover",{{"item","tab:10"}});
    // A line that just fits shows every tab, and one cell less does not.
    int need=4+13; // "+" and "repository", each with the separator before it
    for (int i=0;i<20;++i) need+=QString("%1 t%2").arg(i+1).arg(i).size()+3;
    event("on_dock_layout",{{"shown",true},{"cols",need},{"rows",1}});
    ASSERT_EQ(rec.applied.size(),1);
    EXPECT_EQ(cellsOf(rec.applied.last()),need);
    texts=dockTexts(rec.applied.last());
    EXPECT_TRUE(texts.contains("20 t19"));
    EXPECT_FALSE(hidden(texts)) << texts.join('|').toStdString();
    event("on_dock_layout",{{"shown",true},{"cols",need-1},{"rows",1}});
    ASSERT_EQ(rec.applied.size(),1);
    EXPECT_LE(cellsOf(rec.applied.last()),need-1);
    EXPECT_TRUE(hidden(dockTexts(rec.applied.last())));
    // An area that does not show: the line is for the window's width again.
    event("on_dock_layout",{{"shown",false},{"cols",0},{"rows",0}});
    ASSERT_EQ(rec.applied.size(),1);
    EXPECT_TRUE(dockTexts(rec.applied.last()).contains("20 t19"));
    for(const auto& result:rec.results) EXPECT_TRUE(result.second.isEmpty()) << result.second.toStdString();
}

namespace {
// The tab line over three tabs of window 1, and one tab of window 2. The
// second tab is dragged: its pane, 8, is not the focused pane that the
// events come through, 7.
class TabLineDrag : public ScriptHostTest
{
protected:
    void SetUp() override
    {
        // An ASSERT in a helper returns from the helper only: stop here, and
        // gtest skips the test body.
        ScriptHostTest::SetUp();
        if (HasFatalFailure()) return;
        installPackage("tab-line");
        if (HasFatalFailure()) return;
        WRITE(user,"tab-line/config.json",R"({"git":false,"segments":["cwd"]})");
        if (HasFatalFailure()) return;
        windows={QVariantMap{{"id",1},{"project",""},{"active_pane",7},{"active_tab",10},{"cols",200}},
                 QVariantMap{{"id",2},{"project",""},{"active_pane",9},{"active_tab",20},{"cols",200}}};
        tabs={QVariantMap{{"id",10},{"window",1},{"index",0},{"title","a"},{"active",true},{"active_pane",7}},
              QVariantMap{{"id",11},{"window",1},{"index",1},{"title","b"},{"active",false},{"active_pane",8}},
              QVariantMap{{"id",12},{"window",1},{"index",2},{"title","c"},{"active",false},{"active_pane",100}},
              QVariantMap{{"id",20},{"window",2},{"index",0},{"title","d"},{"active",true},{"active_pane",9}}};
        host->setEnabled(true);pump();
        // A line for each window.
        ASSERT_EQ(rec.applied.size(),2);
        owner=rec.owners.last();
    }
    // Tab @p id is at @p index now, as the line's next state says.
    void place(int id, int index)
    {
        for (QVariant& tab : tabs) {
            QVariantMap map=tab.toMap();
            if (map.value("id").toInt()==id) { map.insert("index",index);tab=map; }
        }
    }
    // A drag event of @p item, as the window sends it: moves count toward
    // the callback limit.
    void drag(const char* item, const char* phase, const char* over)
    {
        QVariantMap ev{{"dock","tabline"},{"window",1},{"scope","window"},{"item",item},{"phase",phase},{"col",3},{"row",0}};
        if (over) ev.insert("over",over);
        rec.applied.clear();
        host->dispatchTo(owner,"on_dock_drag",7,ev,QByteArray(phase)=="move");pump();
    }
    bool moved(quint32 pane, int index) const
    {
        return rec.applied.size()==1 && rec.applied[0].value("kind")=="move_tab"
            && rec.applied[0].value("pane").toUInt()==pane && rec.applied[0].value("index").toInt()==index;
    }
    void TearDown() override
    {
        for(const auto& result:rec.results) EXPECT_TRUE(result.second.isEmpty()) << result.second.toStdString();
    }
    QString owner;
};
} // namespace

TEST_F(TabLineDrag, ATabTakesThePlaceOfTheTabUnderThePointer)
{
    // Over its own cells, the tab stays.
    drag("tab:11","start","tab:11");
    EXPECT_TRUE(rec.applied.isEmpty());
    // Onto the third tab: it takes that tab's place, through its own pane.
    drag("tab:11","move","tab:12");
    EXPECT_TRUE(moved(8,2));
    // The line that shows the move is not there yet: more moves over the
    // same cells do not move the tab back.
    place(11,2);place(12,1);
    drag("tab:11","move","tab:12");
    EXPECT_TRUE(rec.applied.isEmpty());
    // Off the items, then onto that tab again: a new item, at the place
    // that the tab has now.
    drag("tab:11","move",nullptr);
    EXPECT_TRUE(rec.applied.isEmpty());
    drag("tab:11","move","tab:12");
    EXPECT_TRUE(moved(8,1));
    place(11,1);place(12,2);
    // The "+" button is no tab.
    drag("tab:11","move","new");
    EXPECT_TRUE(rec.applied.isEmpty());
    drag("tab:11","end","new");
    EXPECT_TRUE(rec.applied.isEmpty());
    // A drag of the "+" button, or of "+N", moves nothing.
    drag("new","start","new");
    drag("new","move","tab:10");
    EXPECT_TRUE(rec.applied.isEmpty());
    drag("new","end","tab:10");
    EXPECT_TRUE(rec.applied.isEmpty());
    drag("more:12","start","more:12");
    drag("more:12","end","tab:10");
    EXPECT_TRUE(rec.applied.isEmpty());
}

TEST_F(TabLineDrag, TheEndMovesATabThatTheLastMoveDidNotAndACancelMovesNone)
{
    // No move reached the script (moves count toward the callback limit):
    // the end, over another tab, moves the tab.
    drag("tab:11","start","tab:11");
    drag("tab:11","end","tab:12");
    EXPECT_TRUE(moved(8,2));
    place(11,2);place(12,1);
    // An end over the tab that the last move went to does not move it again.
    drag("tab:11","start","tab:11");
    drag("tab:11","move","tab:10");
    EXPECT_TRUE(moved(8,0));
    place(11,0);place(10,1);place(12,2);
    drag("tab:11","end","tab:10");
    EXPECT_TRUE(rec.applied.isEmpty());
    // A cancel moves nothing, also over another tab.
    drag("tab:11","start","tab:11");
    drag("tab:11","cancel","tab:12");
    EXPECT_TRUE(rec.applied.isEmpty());
}

TEST_F(TabLineDrag, ATabMovesOntoAHiddenTabButNotIntoAnotherWindow)
{
    // "+N" stands for the nearest hidden tab: the dragged tab takes its place.
    // The second tab is current, and the line narrows until it shows that
    // tab alone, with a "+1" on each side.
    for (QVariant& tab : tabs) {
        QVariantMap map=tab.toMap();
        if (map.value("window").toInt()==1) { map.insert("active",map.value("id").toInt()==11);tab=map; }
    }
    windows[0]=QVariantMap{{"id",1},{"project",""},{"active_pane",8},{"active_tab",11},{"cols",200}};
    QStringList items;
    for (int cols=40;cols>=16 && !items.contains("more:12");--cols) {
        rec.applied.clear();
        host->dispatchTo(owner,"on_dock_layout",7,{{"dock","tabline"},{"window",1},{"shown",true},{"cols",cols}});pump();
        items.clear();
        for (const QVariant& run : rec.applied.value(0).value("lines").toList().value(0).toList())
            if (run.toMap().contains("item")) items << run.toMap().value("item").toString();
    }
    ASSERT_EQ(items,(QStringList{"more:10","tab:11","more:12","new"}));
    drag("tab:11","start","tab:11");
    drag("tab:11","move","more:12");
    EXPECT_TRUE(moved(8,2));
    place(11,2);place(12,1);
    // A tab moves only in its own window, also at the end of a drag.
    drag("tab:11","end","tab:20");
    EXPECT_TRUE(rec.applied.isEmpty());
    drag("tab:11","start","tab:11");
    drag("tab:11","move","tab:20");
    EXPECT_TRUE(rec.applied.isEmpty());
}

TEST_F(ScriptHostTest, TabLinePacesTheLinesThatThePointerDraws)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("tab-line"));
    WRITE(user,"tab-line/config.json",R"({"git":false,"segments":["cwd"]})");
    paneState[7]["cwd"]="/tmp/repository";
    windows={QVariantMap{{"id",1},{"project",""},{"active_pane",7},{"active_tab",10},{"cols",200}}};
    tabs={QVariantMap{{"id",10},{"window",1},{"index",0},{"title","a"},{"active",true},{"active_pane",7}},
          QVariantMap{{"id",11},{"window",1},{"index",1},{"title","b"},{"active",false},{"active_pane",8}},
          QVariantMap{{"id",12},{"window",1},{"index",2},{"title","c"},{"active",false},{"active_pane",9}}};
    // The package's refresh timer starts with on_load: from here, its first
    // call is a second away.
    QElapsedTimer loaded;loaded.start();
    host->setEnabled(true);pump();
    ASSERT_EQ(rec.applied.size(),1);
    const QString owner=rec.owners.last();
    rec.applied.clear();
    auto wait=[](int ms, const std::function<bool()>& done) {
        QElapsedTimer clock;clock.start();
        while (clock.elapsed()<ms && !done()) { QCoreApplication::processEvents(); QThread::msleep(5); }
    };
    // A sweep back and forth over two tabs, faster than a hand moves: 25
    // lines go out at once in a refresh second, and the rest wait for the
    // draw timer, which calls every 250 ms.
    for (int i=0;i<40;++i) {
        host->dispatchTo(owner,"on_dock_hover",7,{{"dock","tabline"},{"window",1},{"item",i%2?"tab:12":"tab:11"}});
        pump();
    }
    const qint64 swept=loaded.elapsed();
    EXPECT_GE(rec.applied.size(),25);
    // A run slower than 700 ms can let the refresh timer start the next
    // second during the sweep (it may call 5% early), which sends 25 more.
    // Before that, the draw timer has called two or three times.
    if (swept<700) {
        EXPECT_LE(rec.applied.size(),28) << swept << " ms";
    }
    EXPECT_FALSE(host->isDisabled("tab-line"));
    // The timer draws the last hover.
    auto hoveredLast=[&]{
        if (rec.applied.isEmpty()) return false;
        for (const QVariant& run : rec.applied.last().value("lines").toList().value(0).toList())
            if (run.toMap().value("item")=="tab:12") return run.toMap().value("bg").toInt()==7;
        return false;
    };
    wait(1000,hoveredLast);
    EXPECT_TRUE(hoveredLast());
    // The next refresh second draws at once again.
    host->dispatchTo(owner,"on_timer",0,{{"name","refresh"}});pump();
    rec.applied.clear();
    host->dispatchTo(owner,"on_dock_hover",7,{{"dock","tabline"},{"window",1},{"item","tab:11"}});pump();
    EXPECT_EQ(rec.applied.size(),1);
    for(const auto& result:rec.results) EXPECT_TRUE(result.second.isEmpty()) << result.second.toStdString();
}

TEST_F(ScriptHostTest, TabLineParsesGitAndBoundsConfigurableSegments)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("tab-line"));
    WRITE(user,"tab-line/init.luau",R"(
local s=require("segments")
local function texts(runs)
 local out={}
 for _,run in ipairs(runs) do
  local text=run.text and string.match(run.text,"^%s*(.-)%s*$") or ""
  if text~="" and text~="\u{e0b0}" and text~="\u{e0b2}" then table.insert(out,text) end
 end
 return table.concat(out,"|")
end
return {run_command=function() end,on_load=function()
 assert(s.git("main\n"," M file.cpp\n")=="main *")
 assert(s.git("feature\n","")=="feature")
 assert(s.git("a1b2c3d\n")=="a1b2c3d")
 assert(s.git("",nil)=="")
 local values=s.values({cwd="/tmp/project/",command="",context="remote"},"main",1)
 assert(values.cwd=="project" and values.exit=="exit 1" and values.context=="remote")
 local window={id=1,project=""}
 local tabs={{id=3,index=0,title="",active=true}}
 local runs=s.line(window,tabs,values,{segments={"context","cwd","unknown","exit"},powerline=false})
 assert(texts(runs)=="1 shell|+|remote|project|exit 1", texts(runs))
 for _,run in ipairs(runs) do assert(run.text~="\u{e0b0}") end
 -- A line that just fits shows every tab, with and without powerline, with
 -- and without a project and components. One cell less hides a tab.
 local function cells(line)
  local n=0
  for _,run in ipairs(line) do n+=utf8.len(run.text or "") end
  return n
 end
 local three={{id=5,index=0,title="a",active=true},{id=6,index=1,title="b",active=false},{id=7,index=2,title="c",active=false}}
 for _,powerline in ipairs({true,false}) do
  for _,case in ipairs({{project="",values={cwd=""}},{project="dev",values={cwd="here",exit="ok"}}}) do
   local config={powerline=powerline,segments={"cwd","exit"}}
   local full=s.line({id=1,project=case.project},three,case.values,config)
   local width=cells(full)
   local fit=s.line({id=1,project=case.project},three,case.values,config,width)
   assert(string.find(texts(fit),"3 c",1,true) and not string.find(texts(fit),"%+%d"),texts(fit))
   assert(cells(fit)==width,cells(fit).." "..width)
   local short=s.line({id=1,project=case.project},three,case.values,config,width-1)
   assert(string.find(texts(short),"%+%d") and cells(short)<=width-1,texts(short))
  end
 end
 -- Long titles are cut, and only eight component names are read.
 local long={{id=4,index=0,title=string.rep("y",1000),active=false}}
 local names={}
 for i=1,8 do names[i]="cwd" end
 names[9]="exit"
 local bounded=s.line(window,long,s.values({cwd="/"..string.rep("x",1000),command=""},"",2),{segments=names,tab_max_length=10})
 for _,run in ipairs(bounded) do
  assert(#(run.text or "")<=70)
  assert(not run.text or not string.find(run.text,"exit"))
 end
 -- Past 48 tabs, the line shows the current tab and its neighbors, in at most 128 runs.
 local many={}
 for i=1,100 do many[i]={id=i,index=i-1,title="t",active=i==100} end
 local wide=s.line(window,many,values,{})
 assert(#wide<=128,#wide)
 local clickable=0
 for _,run in ipairs(wide) do if run.item then clickable+=1 end end
 assert(clickable==50,clickable) -- 48 tabs, the "+52" before them and the new tab button
 assert(string.find(texts(wide),"^%+52|53 t|"),texts(wide))
 -- A block that sets one color keeps the default of the other.
 local colored=s.line(window,tabs,values,{colors={active={bg=2}}})
 local found=false
 for _,run in ipairs(colored) do
  if run.item=="tab:3" then found=true;assert(run.fg==0 and run.bg==2) end
 end
 assert(found)
 return afterglow.copy("segments passed")
end}
)");
    host->setEnabled(true);
    ASSERT_EQ(rec.applied.size(),1);EXPECT_EQ(rec.applied.first().value("text"),"segments passed");
}

TEST_F(ScriptHostTest, TabLineSendsADroppedLineAgain)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("tab-line"));
    WRITE(user,"tab-line/config.json",R"({"git":false,"segments":["cwd","exit"]})");
    paneState[7]["cwd"]="/tmp/repository";
    windows={QVariantMap{{"id",1},{"project",""},{"active_pane",7}}};
    tabs={QVariantMap{{"id",10},{"window",1},{"index",0},{"title","fish"},{"active",true},{"active_pane",7}}};
    QString refusal="rate_limited";
    applyHook=[&](const QVariantMap& e) {
        ScriptHost::EffectResult result;
        if (e.value("kind")=="dock" && !refusal.isEmpty()) result={"too many docks: a script has at most 8 in a window, and a window at most 32",{},refusal};
        refusal.clear();
        return result;
    };
    // As the timer calls it: its instance only, which the dock names.
    auto tick=[&]{host->dispatchTo(rec.owners.value(0),"on_timer",0,{{"name","refresh"}});pump();};
    host->setEnabled(true);pump();
    ASSERT_EQ(rec.applied.size(),1);
    // Afterglow dropped the line, so the next refresh sends it again, once.
    tick();
    ASSERT_EQ(rec.applied.size(),2);
    EXPECT_EQ(dockTexts(rec.applied.last()),(QStringList{"1 fish","+","repository"}));
    tick();EXPECT_EQ(rec.applied.size(),2);
    // A line refused as invalid goes out again only when it changes.
    refusal="invalid_effect";
    host->dispatch("on_command_finished",7,{{"exit_code",2}});pump();
    ASSERT_EQ(rec.applied.size(),3);
    tick();tick();EXPECT_EQ(rec.applied.size(),3);
    host->dispatch("on_command_finished",7,{{"exit_code",0}});tick();
    ASSERT_EQ(rec.applied.size(),4);
    EXPECT_EQ(dockTexts(rec.applied.last()),(QStringList{"1 fish","+","repository","ok"}));
}

TEST_F(ScriptHostTest, TabLineSendsALineAgainWhenItsResultIsLost)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("tab-line"));
    WRITE(user,"tab-line/config.json",R"({"git":false,"segments":["cwd"]})");
    paneState[7]["cwd"]="/tmp/repository";
    windows={QVariantMap{{"id",1},{"project",""},{"active_pane",7}}};
    tabs={QVariantMap{{"id",10},{"window",1},{"index",0},{"title","fish"},{"active",true},{"active_pane",7}}};
    bool lose=true;
    applyHook=[&](const QVariantMap& e) {
        ScriptHost::EffectResult result;
        result.deferred=e.value("kind")=="dock" && lose; // and never completed
        lose=false;
        return result;
    };
    // As the timer calls it: its instance only, which the dock names.
    auto tick=[&]{host->dispatchTo(rec.owners.value(0),"on_timer",0,{{"name","refresh"}});pump();};
    host->setEnabled(true);pump();
    ASSERT_EQ(rec.applied.size(),1);
    // One line at a time: the first still waits for its result.
    tick();EXPECT_EQ(rec.applied.size(),1);
    tick();ASSERT_EQ(rec.applied.size(),2);
    tick();EXPECT_EQ(rec.applied.size(),2);
}

TEST_F(ScriptHostTest, TabLineCoalescesEventRedraws)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("tab-line"));
    WRITE(user,"tab-line/config.json",R"({"git":false,"segments":["cwd"]})");
    paneState[7]["cwd"]="/tmp/repository";
    windows={QVariantMap{{"id",1},{"project",""},{"active_pane",7}}};
    auto title=[&](const QString& text) {
        tabs={QVariantMap{{"id",10},{"window",1},{"index",0},{"title",text},{"active",true},{"active_pane",7}}};
    };
    title("fish");
    auto wait=[](int ms, const std::function<bool()>& done) {
        QElapsedTimer clock;clock.start();
        while (clock.elapsed()<ms && !done()) { QCoreApplication::processEvents(); QThread::msleep(5); }
    };
    host->setEnabled(true);pump();
    ASSERT_EQ(rec.applied.size(),1);
    for (int i=0;i<10;++i) { title(QString("t%1").arg(i)); host->dispatch("on_title",7,{}); }
    pump();
    // The first change draws at once. The draw timer sends the last one.
    ASSERT_EQ(rec.applied.size(),2);
    EXPECT_EQ(dockTexts(rec.applied.last()).value(0),"1 t0");
    wait(2000,[&]{return rec.applied.size()>2;});
    ASSERT_EQ(rec.applied.size(),3);
    EXPECT_EQ(dockTexts(rec.applied.last()).value(0),"1 t9");
    // With nothing left to draw, the timer stops itself.
    wait(700,[]{return false;});
    EXPECT_EQ(rec.applied.size(),3);
    ASSERT_FALSE(rec.results.isEmpty());
    EXPECT_EQ(rec.results.last().first.value("kind"),"cancel");
    EXPECT_EQ(rec.results.last().first.value("name"),"draw");
}

TEST_F(ScriptHostTest, TabLineShowsTheTabsAroundTheCurrentTab)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("tab-line"));
    WRITE(user,"tab-line/config.json",R"({"git":false,"segments":["cwd"]})");
    paneState[7]["cwd"]="/tmp/repository";
    windows={QVariantMap{{"id",1},{"project",""},{"active_pane",7},{"active_tab",10},{"cols",80}}};
    tabs.clear();
    for (int i=0;i<100;++i) {
        tabs.append(QVariantMap{{"id",i==50?10:1000+i},{"window",1},{"index",i},{"title",QString("t%1").arg(i)},
                                {"active",i==50},{"active_pane",i==50?7:0}});
    }
    host->setEnabled(true);
    ASSERT_EQ(rec.applied.size(),1);
    const QVariantList runs=rec.applied.last().value("lines").toList().value(0).toList();
    EXPECT_LE(runs.size(),128);
    // The text is ASCII and Powerline separators, one cell for each character.
    qsizetype cells=0;
    for (const QVariant& run : runs) cells+=run.toMap().value("text").toString().size();
    EXPECT_LE(cells,80);
    const QStringList texts=dockTexts(rec.applied.last());
    ASSERT_GE(texts.size(),5);
    EXPECT_TRUE(texts.contains("51 t50"));
    EXPECT_EQ(texts.last(),"repository");
    EXPECT_EQ(texts.at(texts.size()-2),"+");
    const QString before=texts.first(), after=texts.at(texts.size()-3);
    ASSERT_TRUE(before.startsWith('+') && after.startsWith('+')) << texts.join('|').toStdString();
    EXPECT_EQ(before.mid(1).toInt()+after.mid(1).toInt()+texts.size()-4,100);
    // "+N" goes to the nearest hidden tab, before and after the shown ones.
    QStringList items;
    for (const QVariant& run : runs) {
        if (run.toMap().contains("item")) items << run.toMap().value("item").toString();
    }
    ASSERT_GE(items.size(),3);
    EXPECT_EQ(items.first(),QString("more:%1").arg(1000+before.mid(1).toInt()-1));
    EXPECT_EQ(items.at(items.size()-2),QString("more:%1").arg(1000+100-after.mid(1).toInt()));
    EXPECT_EQ(items.last(),"new");
}

TEST_F(ScriptHostTest, TabLineLeavesAOneTabWindowWithSingleTabOff)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("tab-line"));
    WRITE(user,"tab-line/config.json",R"({"git":false,"single_tab":false,"segments":["cwd"]})");
    paneState[7]["cwd"]="/tmp/repository";
    windows={QVariantMap{{"id",1},{"project",""},{"active_pane",7}}};
    tabs={QVariantMap{{"id",10},{"window",1},{"index",0},{"title","fish"},{"active",true},{"active_pane",7}}};
    // The instance applies no owned effect before the first tick, so the test
    // cannot learn its owner: dispatch calls on_timer in every script that has
    // it, here this one.
    auto tick=[&]{host->dispatch("on_timer",0,{{"name","refresh"}});pump();};
    host->setEnabled(true);pump();
    EXPECT_TRUE(rec.applied.isEmpty());
    tabs.append(QVariantMap{{"id",11},{"window",1},{"index",1},{"title","vim"},{"active",false},{"active_pane",8}});
    tick();
    ASSERT_EQ(rec.applied.size(),1);
    EXPECT_EQ(rec.applied.last().value("kind"),"dock");
    tabs.removeLast();
    tick();
    ASSERT_EQ(rec.applied.size(),2);
    EXPECT_EQ(rec.applied.last().value("kind"),"undock");
    EXPECT_EQ(rec.applied.last().value("id"),"tabline");
    for(const auto& result:rec.results) EXPECT_TRUE(result.second.isEmpty()) << result.second.toStdString();
}

TEST_F(ScriptHostTest, AutosaveRecoversFromSaveFailureAndCancelledRestore)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("project-autosave"));
    windows={QVariantMap{{"id",1},{"project","dev"},{"active_pane",7}}};
    bool fail=true;
    applyHook=[&](const QVariantMap& e)->ScriptHost::EffectResult {
        if(e.value("kind")=="save_project" && fail) return {"Disk unavailable",{},"io_error"};
        if(e.value("kind")=="list_projects") return {{},{{"names",QStringList{"dev"}}},{}};
        if(e.value("kind")=="select") return {"Cancelled",{},"cancelled"};
        return {};
    };
    host->setEnabled(true);ASSERT_TRUE(run("project-autosave.enable",7));pump(12);
    ASSERT_FALSE(rec.applied.isEmpty());
    EXPECT_EQ(rec.applied.last().value("kind"),"save_project");
    ASSERT_FALSE(notices.isEmpty());
    EXPECT_EQ(notices.last().body,"Autosave is on for dev, but the first save failed: Disk unavailable");
    fail=false;rec.applied.clear();
    host->dispatch("on_timer",0,{{"name","autosave"}});pump(12);
    ASSERT_EQ(rec.applied.size(),1);EXPECT_EQ(rec.applied.last().value("kind"),"save_project");
    rec.applied.clear();ASSERT_TRUE(run("project-autosave.restore",7));pump(12);
    ASSERT_EQ(rec.applied.size(),2);EXPECT_EQ(rec.applied.last().value("kind"),"select");
    ASSERT_TRUE(run("project-autosave.disable",7));pump(12);
    ScriptState::Snapshot stored;
    ASSERT_TRUE(ScriptState(stateDir.path()).read("project-autosave",stored).isEmpty());
    EXPECT_FALSE(stored.value.value("enabled").toMap().contains("dev"));
}

TEST_F(ScriptHostTest, ShaderEffectsPreserveTypedValuesAndValidateShape)
{
    WRITE(user, QStringLiteral("shader.luau"), R"(
return {
    on_bell = function(ev)
        return {
            afterglow.shaders(ev.pane),
            afterglow.shader_parameters(ev.pane, "instance", {strength=0.5, tint="#ff0077", enabled=true}, "tab"),
            afterglow.shader_parameters(ev.pane, "instance", {}),
            afterglow.shader_parameters(ev.pane, "", {strength=1}),
            afterglow.shader_parameters(ev.pane, "instance", {strength=1}, "all"),
            afterglow.shader_parameters(ev.pane, "instance", 3),
        }
    end,
}
)");
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_bell"), 7);
    ASSERT_EQ(rec.results.size(), 6);
    ASSERT_EQ(rec.applied.size(), 3);
    EXPECT_EQ(rec.applied[0].value(QStringLiteral("kind")), QStringLiteral("shaders"));
    const auto values = rec.applied[1].value(QStringLiteral("values")).toMap();
    EXPECT_EQ(values.value(QStringLiteral("enabled")).typeId(), QMetaType::Bool);
    EXPECT_DOUBLE_EQ(values.value(QStringLiteral("strength")).toDouble(), 0.5);
    EXPECT_EQ(rec.applied[1].value(QStringLiteral("scope")), QStringLiteral("tab"));
    EXPECT_TRUE(rec.applied[2].value(QStringLiteral("values")).toMap().isEmpty());
}

TEST_F(ScriptHostTest, ShaderParameterExampleDiscoversThenTargetsReturnedInstance)
{
    ASSERT_TRUE(QFile::copy(QStringLiteral(AFTERGLOW_EXAMPLE_SCRIPTS) + "/shader-parameters.luau",
                           user.filePath("shader-parameters.luau")));
    applyHook = [](const QVariantMap& e) {
        if (e.value(QStringLiteral("kind")) == QLatin1String("shaders"))
            return ScriptHost::EffectResult{{}, {{QStringLiteral("shaders"), QVariantList{
                QVariantMap{{QStringLiteral("instance"), QStringLiteral("window-instance")},
                            {QStringLiteral("shader"), QStringLiteral("bloom")},
                            {QStringLiteral("layer"), QStringLiteral("window")}}}}}, {}};
        return ScriptHost::EffectResult{};
    };
    host->setEnabled(true);
    ASSERT_TRUE(run(QStringLiteral("shader-parameters.boost"), 7));
    QCoreApplication::processEvents();
    ASSERT_EQ(rec.applied.size(), 2);
    EXPECT_EQ(rec.applied[1].value(QStringLiteral("instance")), QStringLiteral("window-instance"));
    EXPECT_EQ(rec.applied[1].value(QStringLiteral("scope")), QStringLiteral("window"));
    EXPECT_DOUBLE_EQ(rec.applied[1].value(QStringLiteral("values")).toMap().value(QStringLiteral("strength")).toDouble(), 1.2);
    ASSERT_TRUE(run(QStringLiteral("shader-parameters.reset"), 7));
    QCoreApplication::processEvents();
    ASSERT_EQ(rec.applied.size(), 4);
    EXPECT_TRUE(rec.applied.last().value(QStringLiteral("values")).toMap().isEmpty());
}

TEST_F(ScriptHostTest, SearchDirectoryPriorityWinsOverMetadataIdFilenameOrder)
{
    // The mirror of UserScriptReplacesSystemScript: here the user's file
    // sorts last, so neither order of names decides.
    WRITE(system, "a-system.luau", "return {metadata={id='same',commands={system={summary='system',palette='system'}}},run_command=function() end}");
    WRITE(user, "z-user.luau", "return {metadata={id='same',commands={user={summary='user',palette='user'}}},run_command=function() end}");
    host->setEnabled(true);
    ASSERT_EQ(host->commands().size(), 1);
    EXPECT_EQ(host->commands()[0].token(), QStringLiteral("same.user"));
}

TEST_F(ScriptHostTest, LaunchOptionsPreserveEnvironmentSizeAndDirection)
{
    WRITE(user, "launch.luau", R"(
return {on_bell=function(ev)
 return {afterglow.split(ev.pane,"left",{"/bin/sh"},{cwd="/tmp",size=0.3,env={VALUE="hello",REMOVE=false}}),
         afterglow.split(ev.pane,"up"), afterglow.new_tab(ev.pane,"",nil,{env={VALUE="tab"}})}
end}
)");
    WRITE(user,"window.luau","return {on_bell=function() return afterglow.new_window('',nil,{cwd='/tmp'}) end}");
    host->setEnabled(true);
    host->dispatch("on_bell",7);
    ASSERT_EQ(rec.applied.size(),4);
    const auto options=rec.applied[0]["options"].toMap();
    EXPECT_EQ(options["cwd"].toString(),"/tmp");
    EXPECT_DOUBLE_EQ(options["size"].toDouble(),0.3);
    EXPECT_EQ(options["env"].toMap()["REMOVE"].typeId(),QMetaType::Bool);
    EXPECT_FALSE(options["env"].toMap()["REMOVE"].toBool());
    EXPECT_EQ(rec.applied[0]["direction"].toString(),"left");
    EXPECT_EQ(rec.applied[1]["direction"].toString(),"up");
    EXPECT_EQ(rec.applied[2]["options"].toMap()["env"].toMap()["VALUE"].toString(),"tab");
    EXPECT_EQ(rec.applied[3]["kind"].toString(),"new_window");
    EXPECT_EQ(rec.applied[3]["options"].toMap()["cwd"].toString(),"/tmp");
}

TEST_F(ScriptHostTest, InvalidLaunchAndEnvironmentOptionsNeverReachHost)
{
    WRITE(user,"bad-options.luau",R"(
return {on_bell=function(ev) return {
 afterglow.split(ev.pane,"up",nil,{size=0/0}),
 afterglow.new_tab(ev.pane,"",nil,{env={FOO=true}}),
 afterglow.new_window("",nil,{env={["BAD=NAME"]="value"}}),
 afterglow.exec("bad",{"/bin/true"},{env={BAD=17}})
} end}
)");
    host->setEnabled(true); host->dispatch("on_bell",7);
    EXPECT_TRUE(rec.applied.empty());
    ASSERT_EQ(rec.results.size(),4);
    for (const auto& result:rec.results) EXPECT_FALSE(result.second.isEmpty());
}

TEST(ScriptTasksTest, ProcessEnvironmentOverridesAndRemovals)
{
    ScriptTasks tasks;
    auto effect=job("env",{"/bin/sh","-c","printf '%s:%s' \"$AFTERGLOW_ENV_TEST\" \"${HOME+set}\""});
    effect["env"]=QVariantMap{{"AFTERGLOW_ENV_TEST","value"},{"HOME",false}};
    bool done=false; ScriptHost::EffectResult result;
    ASSERT_TRUE(tasks.apply("owner",effect,[&](auto value){result=value;done=true;},{}).deferred);
    ASSERT_TRUE(await([&]{return done;}));
    EXPECT_TRUE(result.error.isEmpty());
    EXPECT_EQ(result.data["stdout"].toString(),"value:");
}

TEST_F(ScriptHostTest, DiagnosticsIncludeCallsErrorsTimingAndDroppedEffects)
{
    WRITE(user,"stats.luau",R"(
return {on_bell=function() error("handler failed") end,
 on_focus=function() return {kind="unknown"} end,
 on_title=function() local out={} for i=1,100 do table.insert(out,afterglow.log("x")) end return out end}
)");
    host->setEnabled(true);
    host->dispatch("on_bell",7); host->dispatch("on_focus",7); host->dispatch("on_title",7);
    ASSERT_FALSE(host->diagnostics().isEmpty());
    const auto stats=firstDiagnostic();
    EXPECT_EQ(stats["calls"].toULongLong(),3);
    EXPECT_EQ(stats["handler_errors"].toULongLong(),1);
    EXPECT_EQ(stats["effect_errors"].toULongLong(),2);
    EXPECT_EQ(stats["dropped_effects"].toULongLong(),100);
    EXPECT_GT(stats["total_call_ms"].toDouble(),0);
    EXPECT_GE(stats["max_call_ms"].toDouble(),stats["last_call_ms"].toDouble());
    EXPECT_FALSE(stats["last_error"].toString().isEmpty());
}

TEST_F(ScriptHostTest, ClosedBackgroundPaneUpdatesOnlyItsSurvivingTab)
{
    host->setSearchPaths({QStringLiteral(AFTERGLOW_PHOSPHOR_SCRIPTS)});
    paneState[8]["tab_id"]=20;
    paneState[8]["window"]=2;
    tabs={QVariantMap{{"id",10},{"active_pane",7}},QVariantMap{{"id",20},{"active_pane",9}}};
    host->setEnabled(true);
    paneState.remove(8);
    host->dispatch("on_pane_closed",8,{{"pane",QVariantMap{{"id",8},{"tab_id",20},{"window",2}}}});
    ASSERT_EQ(rec.applied.size(),2);
    for (const auto& effect:rec.applied) EXPECT_EQ(effect["pane"].toUInt(),9u);
    EXPECT_EQ(rec.applied[0]["kind"].toString(),"signals");
    EXPECT_EQ(rec.applied[0]["values"].toList()[0].toMap()["value"].toDouble(),0);
}

TEST(ScriptTasksTest, ExecutableSearchUsesChildPathAndWorkingDirectory)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkpath("bin"));
    QFile file(dir.filePath("bin/afterglow-env-path-probe"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("#!/bin/sh\nprintf child-path"); file.close();
    ASSERT_TRUE(file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner));
    ScriptTasks tasks;
    auto effect=job("path",{"afterglow-env-path-probe"});
    effect["cwd"]=dir.path(); effect["env"]=QVariantMap{{"PATH","bin"}};
    bool done=false; ScriptHost::EffectResult result;
    ASSERT_TRUE(tasks.apply("owner",effect,[&](auto value){result=value;done=true;},{}).deferred);
    ASSERT_TRUE(await([&]{return done;}));
    EXPECT_TRUE(result.error.isEmpty()) << result.error.toStdString();
    EXPECT_EQ(result.data["stdout"].toString(),"child-path");
}

TEST_F(ScriptHostTest, MetadataIdsRejectTrailingNewlines)
{
    WRITE(user, "bad-id.luau", "return {metadata={id='bad\\n'}}");
    WRITE(user, "good-id.luau", "return {metadata={id='bad'}}");
    host->setEnabled(true);
    // The id without its newline is another script's, which loads.
    EXPECT_EQ(host->scriptIds(), QStringList{"bad"});
    QString error;
    for (const QVariant& d : host->diagnostics())
        if (d.toMap()["path"].toString().endsWith("bad-id.luau")) error = d.toMap()["error"].toString();
    EXPECT_TRUE(error.contains("must be 1 to 64")) << error.toStdString();
}

TEST_F(ScriptHostTest, FailedAndSupersededLooseScriptsRecoverFromInPlaceEdits)
{
    WRITE(user, "broken.luau", "return {");
    WRITE(system, "old.luau", "return {metadata={id='shared'}}");
    WRITE(user, "winner.luau", "return {metadata={id='shared'}}");
    host->setEnabled(true);
    EXPECT_EQ(host->scriptIds(), QStringList{"shared"});
    WRITE(user, "broken.luau", "return {metadata={id='recovered'}}");
    ASSERT_TRUE(await([&] { return host->scriptIds().contains("recovered"); }));
    WRITE(system, "old.luau", "return {metadata={id='independent'}}");
    ASSERT_TRUE(await([&] { return host->scriptIds().contains("independent"); }));
    EXPECT_TRUE(host->scriptIds().contains("shared"));
}

TEST_F(ScriptHostTest, ABrokenEditOfASupersededScriptStaysSupersededOnLaterScans)
{
    WRITE(system, "old.luau", "return {metadata={id='shared'}}");
    host->setEnabled(true);
    ASSERT_EQ(host->scriptIds(), QStringList{"shared"});
    // In one scan, the script breaks and a script of higher priority takes its id.
    WRITE(system, "old.luau", "return {");
    WRITE(user, "winner.luau", "return {metadata={id='shared'}}");
    host->reload();
    const auto statuses = [&] {
        QStringList out;
        for (const QVariant& entry : host->diagnostics()) {
            const auto map = entry.toMap();
            if (map["path"].toString().endsWith("old.luau")) out << map["status"].toString();
        }
        out.sort();
        return out;
    };
    EXPECT_EQ(statuses(), QStringList({"failed", "superseded"}));
    // A scan that finds the file unchanged keeps both.
    host->reload();
    EXPECT_EQ(statuses(), QStringList({"failed", "superseded"}));
}

TEST_F(ScriptHostTest, AwaitChainsEffectsInOneHandler)
{
    WRITE(user, "chain.luau", R"(
return {
    metadata = {commands={copy={summary="Copy",palette="Copy"}}},
    run_command = function(_, ev)
        local pane = ev.pane.id
        local regions = afterglow.await(afterglow.semantic_regions(pane))
        local choice = afterglow.await(afterglow.select(pane, "Region", {{id = regions.regions[1].id, title = "first"}}))
        local text, err, result = afterglow.await(afterglow.read_region(pane, choice.id))
        assert(err == nil and result.ok and result.terminal.version == 1)
        return afterglow.copy(pane .. ":" .. choice.id .. ":" .. text.text)
    end,
}
)");
    applyHook = [](const QVariantMap& effect) -> ScriptHost::EffectResult {
        if (effect["kind"] == QLatin1String("semantic_regions"))
            return {{}, {{"regions", QVariantList{QVariantMap{{"id", "42"}}}}}, {}};
        if (effect["kind"] == QLatin1String("select")) return {{}, {{"id", "42"}}, {}};
        if (effect["kind"] == QLatin1String("read_region")) return {{}, {{"text", "output"}}, {}};
        return {};
    };
    host->setEnabled(true);
    ASSERT_TRUE(run("chain.copy", 7));
    ASSERT_TRUE(await([&] { return rec.applied.size() == 4; }));
    EXPECT_TRUE(rec.applied[0]["request_id"].toString().startsWith("afterglow.await:"));
    EXPECT_EQ(rec.applied[1]["items"].toList().front().toMap()["id"], "42");
    EXPECT_EQ(rec.applied[3]["kind"], "copy");
    EXPECT_EQ(rec.applied[3]["text"], "7:42:output");
    ASSERT_FALSE(host->diagnostics().isEmpty());
    EXPECT_EQ(firstDiagnostic()["waiting"].toInt(), 0);
    EXPECT_EQ(firstDiagnostic()["handler_errors"].toULongLong(), 0u);
}

TEST_F(ScriptHostTest, AwaitReturnsErrorsAndCancellationAsValues)
{
    WRITE(user, "errors.luau", R"(
return {
    on_bell = function(ev)
        local data, err = afterglow.await(afterglow.focus(999))
        assert(data == nil)
        local text, cancelled = afterglow.await(afterglow.prompt(ev.pane, "Name"))
        if not text then return afterglow.copy(err.code .. "," .. cancelled.code) end
        return afterglow.copy(text.text)
    end,
}
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_TRUE(await([&] { return bool(pendingReply); }));
    pendingReply({QStringLiteral("Dialog closed"), {}, QStringLiteral("cancelled")});
    ASSERT_TRUE(await([&] { return !rec.applied.isEmpty() && rec.applied.last()["kind"] == "copy"; }));
    EXPECT_EQ(rec.applied.last()["text"], "no_pane,cancelled");
}

TEST_F(ScriptHostTest, AwaitAllReturnsResultsInOrder)
{
    WRITE(user, "batch.luau", R"(
return {
    on_bell = function(ev)
        local results = afterglow.await_all({afterglow.read_text(ev.pane, "screen"), afterglow.focus(999),
            afterglow.read_text(ev.pane, "selection")})
        local out = {}
        for _, r in results do table.insert(out, if r.ok then r.data.text else r.error.code) end
        assert(#afterglow.await_all({}) == 0)
        return afterglow.copy(table.concat(out, ","))
    end,
}
)");
    applyHook = [](const QVariantMap& effect) -> ScriptHost::EffectResult {
        return {{}, {{"text", effect["scope"]}}, {}};
    };
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_TRUE(await([&] { return !rec.applied.isEmpty() && rec.applied.last()["kind"] == "copy"; }));
    EXPECT_EQ(rec.applied.last()["text"], "screen,no_pane,selection");
}

TEST_F(ScriptHostTest, AwaitAllRefusesAListWithAHoleAfterAFullArrayPart)
{
    WRITE(user, "gap.luau", R"(
return { on_bell = function(ev)
    local effects = {afterglow.copy("a"), afterglow.copy("b")}
    -- Too far for the array part, so it goes to the hash part: # is still 2, and ipairs stops at 2.
    effects[16] = afterglow.copy("p")
    afterglow.await_all(effects)
    return afterglow.copy("waited")
end }
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    EXPECT_TRUE(rec.applied.isEmpty());
    EXPECT_TRUE(firstDiagnostic()["last_error"].toString().contains("with no holes"));
}

TEST_F(ScriptHostTest, SleepResumesAfterItsDelayAndIsBounded)
{
    WRITE(user, "sleep.luau", R"(
return {
    on_bell = function(ev)
        afterglow.sleep(60)
        return afterglow.copy("woke")
    end,
    on_focus = function() return {{kind = "sleep", delay_ms = 10}, afterglow.request("long", {kind = "sleep", delay_ms = 10})} end,
    on_title = function() afterglow.sleep(600001) end,
}
)");
    host->setEnabled(true);
    QElapsedTimer clock;
    clock.start();
    host->dispatch("on_bell", 7);
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    EXPECT_TRUE(rec.applied.isEmpty());
    ASSERT_TRUE(await([&] { return !rec.applied.isEmpty(); }));
    EXPECT_GE(clock.elapsed(), 55);
    EXPECT_EQ(rec.applied.last()["text"], "woke");
    // Sleep exists only to be awaited, and only up to ten minutes.
    rec.results.clear();
    host->dispatch("on_focus", 7);
    ASSERT_EQ(rec.results.size(), 2);
    EXPECT_TRUE(rec.results[0].second.contains("awaited"));
    EXPECT_TRUE(rec.results[1].second.contains("awaited")); // a request ID of its own is not a wait
    host->dispatch("on_title", 7);
    ASSERT_EQ(rec.results.size(), 3);
    EXPECT_TRUE(rec.results[2].second.contains("600000"));
}

TEST_F(ScriptHostTest, WaitingHandlersAreBoundedAndReported)
{
    WRITE(user, "many.luau", R"(
return {
    on_bell = function()
        local _, err = afterglow.sleep(10000)
        if err then return afterglow.copy(err.code) end
        return nil
    end,
}
)");
    host->setEnabled(true);
    // The bound is the prelude's, which the host does not know.
    QFile prelude(QStringLiteral(":/afterglow/scripting/afterglow.luau"));
    ASSERT_TRUE(prelude.open(QIODevice::ReadOnly));
    const auto bound = QRegularExpression(QStringLiteral("local MaxWaiting = (\\d+)")).match(QString::fromUtf8(prelude.readAll()));
    ASSERT_TRUE(bound.hasMatch());
    const int maxWaiting = bound.captured(1).toInt();
    // Each handler waits for a sleep, which the host bounds too.
    ASSERT_LE(maxWaiting, ScriptHost::MaxSleeping);
    for (int i = 0; i < maxWaiting; ++i) host->dispatch("on_bell", 7);
    ASSERT_FALSE(host->diagnostics().isEmpty());
    EXPECT_EQ(firstDiagnostic()["waiting"].toInt(), maxWaiting);
    EXPECT_TRUE(rec.applied.isEmpty());
    // One more is refused at once.
    host->dispatch("on_bell", 7);
    EXPECT_EQ(firstDiagnostic()["waiting"].toInt(), maxWaiting);
    ASSERT_EQ(rec.applied.size(), 1);
    EXPECT_EQ(rec.applied.last()["text"], "busy");
}

TEST_F(ScriptHostTest, AwaitMisuseFailsClearly)
{
    WRITE(user, "toplevel.luau", "afterglow.await(afterglow.log('x'))\nreturn {}");
    WRITE(user, "misuse.luau", R"(
return {
    on_bell = function() return coroutine.wrap(function() return afterglow.await(afterglow.log("x")) end)() end,
    on_focus = function() coroutine.yield(1) end,
    on_title = function() return afterglow.await("not an effect") end,
}
)");
    host->setEnabled(true);
    EXPECT_FALSE(host->scriptIds().contains("toplevel"));
    ASSERT_TRUE(host->scriptIds().contains("misuse"));
    for (const char* handler : {"on_bell", "on_focus", "on_title"}) host->dispatch(handler, 7);
    QStringList errors;
    bool misuse = false;
    for (const auto& entry : host->diagnostics()) {
        const auto map = entry.toMap();
        if (map["path"].toString().endsWith("toplevel.luau")) errors << map["error"].toString();
        if (map["id"] == "misuse") {
            misuse = true;
            EXPECT_EQ(map["handler_errors"].toULongLong(), 3u);
        }
    }
    EXPECT_TRUE(misuse);
    ASSERT_EQ(errors.size(), 1);
    EXPECT_TRUE(errors[0].contains("only in a handler")) << errors[0].toStdString();
    EXPECT_TRUE(rec.applied.isEmpty());
}

TEST_F(ScriptHostTest, ResumedHandlersKeepTheirTimeLimitAndErrorAccounting)
{
    WRITE(user, "resumed.luau", R"(
return {
    metadata = {commands={spin={summary="Spin",palette="Spin"}, forever={summary="Spin without end after a wait"}}},
    run_command = function(command)
        afterglow.sleep(0)
        if command == "forever" then while true do end end
        local t = os.clock()
        while os.clock() - t < 0.035 do end -- 35 ms: over an event's 20 ms, well within a command's 100 ms
        afterglow.sleep(0)
        error("after the second await")
    end,
}
)");
    host->setEnabled(true);
    ASSERT_TRUE(run("resumed.spin", 7));
    ASSERT_TRUE(await([&] {
        return !host->diagnostics().isEmpty() && host->diagnostics().first().toMap()["handler_errors"].toULongLong() == 1;
    }));
    const auto stats = firstDiagnostic();
    EXPECT_TRUE(stats["last_error"].toString().startsWith("a handler after await:")) << stats["last_error"].toString().toStdString();
    EXPECT_TRUE(stats["last_error"].toString().contains("after the second await"));
    EXPECT_EQ(stats["waiting"].toInt(), 0);
    // A resumed handler still has a limit: the command's.
    Client& client = newClient();
    start("resumed", "forever", client);
    ASSERT_TRUE(await([&] { return client.status.has_value(); }));
    EXPECT_EQ(client.status, 1);
    EXPECT_TRUE(client.err.startsWith("The command failed after a wait: ")) << client.err.toStdString();
    EXPECT_TRUE(client.err.contains(QStringLiteral("took longer than %1 ms").arg(ScriptHost::CommandCallTimeoutMs)))
        << client.err.toStdString();
}

// The await runtime registers a flow before the engine converts the effects it
// waits for. Effects too large to convert must not keep that flow, or every
// such call would hold one of the script's 16 waiting slots for good.
TEST_F(ScriptHostTest, AwaitWithEffectsTooLargeToConvertFreesItsWaitingSlot)
{
    WRITE(user, "large.luau", R"(
return {
    metadata = {commands={large={summary="Large",palette="Large"},small={summary="Small",palette="Small"}}},
    run_command = function(command, ev)
        if command == "large" then
            -- Each level holds the one below twice: 2^20 conversions, little memory.
            local wide = {}
            for _ = 1, 20 do
                wide = {wide, wide}
            end
            local effect = afterglow.log("large")
            effect.extra = wide
            afterglow.await(effect)
            return afterglow.copy("unreachable")
        end
        local _, err = afterglow.await(afterglow.log("small"))
        return afterglow.copy(err and err.code or "ok")
    end,
}
)");
    host->setEnabled(true);
    // Three failures, then a success that resets the run of failures: 18
    // abandoned waits in all, more than the 16 that can wait at once.
    for (int round = 0; round < 6; ++round) {
        for (int i = 0; i < 3; ++i) ASSERT_TRUE(run("large.large", 7));
        ASSERT_TRUE(run("large.small", 7));
        ASSERT_TRUE(await([&] {
            return std::count_if(rec.applied.cbegin(), rec.applied.cend(),
                                 [](const QVariantMap& e) { return e["kind"] == QLatin1String("copy"); }) == round + 1;
        }));
        EXPECT_EQ(rec.applied.last()["text"], "ok") << "round " << round;
    }
    for (const QVariantMap& e : std::as_const(rec.applied)) EXPECT_NE(e["text"], "unreachable");
    const auto stats = firstDiagnostic();
    EXPECT_EQ(stats["waiting"].toInt(), 0);
    EXPECT_EQ(stats["handler_errors"].toULongLong(), 18u);
    EXPECT_TRUE(stats["last_error"].toString().contains("result too large")) << stats["last_error"].toString().toStdString();
    EXPECT_TRUE(host->scriptIds().contains("large"));
}

TEST_F(ScriptHostTest, AScriptCannotWrapItsOwnHandlers)
{
    // A flow that a script's own wrapped table registered would never reach
    // afterglow: dropped, it would hold a waiting slot for good, and the
    // table's __resume would resume a handler with a made-up result.
    WRITE(user, "nested.luau", R"(
return {
    metadata = {commands={wrap={summary="Wrap",palette="Wrap"}}},
    run_command = function(command, ev)
        local proxy = afterglow.__wrap({h = function() afterglow.await(afterglow.log("inner")) end})
        proxy.h()
        return afterglow.copy("unreachable")
    end,
}
)");
    // Its own code runs before afterglow wraps it: it takes the one wrap.
    WRITE(user, "early.luau", "local proxy = afterglow.__wrap({}) return {}");
    host->setEnabled(true);
    EXPECT_EQ(host->scriptIds(), QStringList{QStringLiteral("nested")});
    for (int i = 0; i < 3; ++i) ASSERT_TRUE(run("nested.wrap", 7));
    pump();
    for (const QVariantMap& e : std::as_const(rec.applied)) EXPECT_NE(e["text"], "unreachable");
    QHash<QString, QVariantMap> stats;
    for (const QVariant& d : host->diagnostics()) stats.insert(QFileInfo(d.toMap()["path"].toString()).fileName(), d.toMap());
    EXPECT_EQ(stats["nested.luau"]["handler_errors"].toULongLong(), 3u);
    EXPECT_EQ(stats["nested.luau"]["waiting"].toInt(), 0);
    EXPECT_TRUE(stats["nested.luau"]["last_error"].toString().contains("afterglow.__wrap belongs to afterglow"))
        << stats["nested.luau"]["last_error"].toString().toStdString();
    EXPECT_EQ(stats["early.luau"]["status"], "failed");
    EXPECT_TRUE(stats["early.luau"]["error"].toString().contains("afterglow.__wrap belongs to afterglow"))
        << stats["early.luau"]["error"].toString().toStdString();
}

TEST_F(ScriptHostTest, ReplacementDropsWaitingHandlersAndOnResultStillWorks)
{
    WRITE(user, "replace.luau", R"(
return {
    on_bell = function() afterglow.sleep(50) return afterglow.copy("old") end,
    on_focus = function(ev) return afterglow.request("plain", afterglow.read_text(ev.pane)) end,
    on_result = function(ev) return afterglow.copy(ev.request_id) end,
}
)");
    host->setEnabled(true);
    host->dispatch("on_focus", 7);
    ASSERT_TRUE(await([&] { return !rec.applied.isEmpty() && rec.applied.last()["kind"] == "copy"; }));
    EXPECT_EQ(rec.applied.last()["text"], "plain");
    host->dispatch("on_bell", 7);
    WRITE(user, "replace.luau", "return {on_bell = function() return afterglow.copy('new') end}");
    host->reload();
    const int before = rec.applied.size();
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 150) { QCoreApplication::processEvents(); QThread::msleep(1); }
    EXPECT_EQ(rec.applied.size(), before);
    // The replacement runs.
    host->dispatch("on_bell", 7);
    ASSERT_EQ(rec.applied.size(), before + 1);
    EXPECT_EQ(rec.applied.last()["text"], "new");
}

TEST_F(ScriptHostTest, PackageLoadCanAwaitItsStateWrite)
{
    QDir().mkpath(user.filePath("counter"));
    WRITE(user, "counter/package.json", R"({"id":"counter","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"main.luau"}})");
    WRITE(user, "counter/main.luau", R"(
return {
    on_load = function(ev)
        local state = ev.storage.value
        state.launches = (state.launches or 0) + 1
        local saved, err = afterglow.await(afterglow.state_write(state, ev.storage.revision))
        assert(saved, err and err.message)
        return afterglow.copy("revision " .. saved.revision)
    end,
}
)");
    host->setEnabled(true);
    ASSERT_TRUE(await([&] { return !rec.applied.isEmpty(); }));
    EXPECT_EQ(rec.applied.last()["text"], "revision 1");
    ScriptState::Snapshot stored;
    ASSERT_TRUE(ScriptState(stateDir.path()).read("counter", stored).isEmpty());
    EXPECT_EQ(stored.value["launches"].toInt(), 1);
}

TEST_F(ScriptHostTest, KeyModesNeedOnKeyAndKeysForNobodyAreDropped)
{
    WRITE(user, "keys.luau", R"(
return {
    on_bell = function(ev) return afterglow.key_mode(ev.pane, "Keys") end,
    on_key = function(ev) return afterglow.copy(ev.key) end,
}
)");
    WRITE(user, "other.luau", R"(
return {
    on_focus = function(ev) return afterglow.key_mode(ev.pane, "No handler") end,
}
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_FALSE(rec.applied.isEmpty());
    EXPECT_EQ(rec.applied.last()["kind"], "key_mode");
    EXPECT_EQ(rec.applied.last()["label"], "Keys");
    // A script without on_key cannot take the keyboard.
    rec.results.clear();
    host->dispatch("on_focus", 7);
    ASSERT_FALSE(rec.results.isEmpty());
    EXPECT_TRUE(rec.results.last().second.contains("on_key"));
    // A key for nobody is dropped.
    const int before = rec.applied.size();
    host->dispatchTo("nobody@1", "on_key", 7, {{"key", "a"}});
    EXPECT_EQ(rec.applied.size(), before);
}

TEST_F(ScriptHostTest, OverviewAndPulseEffectsAreValidatedBeforeTheHost)
{
    WRITE(user, "fx.luau", R"(
return {
    on_bell = function(ev)
        return {
            afterglow.overview(ev.pane, "open"),
            afterglow.overview(ev.pane, "explode"),
            afterglow.overview_move(ev.pane, 1),
            afterglow.overview_move(ev.pane, 99),
            afterglow.pulse(ev.pane, "flash", {color = 1}),
            afterglow.pulse(ev.pane, "melt"),
            afterglow.pulse(ev.pane, "7", {duration_ms = 20000}),
            afterglow.pulse(ev.pane, "300"),
            -- The bounds: 50 to 10000 ms, and colors 0 to 255.
            afterglow.pulse(ev.pane, "255", {duration_ms = 50, color = 255}),
            afterglow.pulse(ev.pane, "1", {duration_ms = 10000, color = 0}),
            afterglow.pulse(ev.pane, "flash", {duration_ms = 49}),
            afterglow.pulse(ev.pane, "flash", {duration_ms = 10001}),
            afterglow.pulse(ev.pane, "flash", {color = 256}),
            afterglow.pulse(ev.pane, "flash", {color = -1}),
            afterglow.pulse(ev.pane, "0"),
        }
    end,
}
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    QStringList kinds;
    for (const auto& e : rec.applied) kinds << e["kind"].toString();
    ASSERT_EQ(kinds, QStringList({"overview", "overview_move", "pulse", "pulse", "pulse"}));
    EXPECT_EQ(rec.applied[2]["effect"], "2"); // flash, by number
    EXPECT_EQ(rec.applied[2]["color"].toInt(), 1);
    EXPECT_EQ(rec.applied[2]["duration_ms"].toInt(), 600);
    EXPECT_EQ(rec.applied[3]["effect"], "255");
    EXPECT_EQ(rec.applied[3]["duration_ms"].toInt(), 50);
    EXPECT_EQ(rec.applied[3]["color"].toInt(), 255);
    EXPECT_EQ(rec.applied[4]["effect"], "1");
    EXPECT_EQ(rec.applied[4]["duration_ms"].toInt(), 10000);
    EXPECT_EQ(rec.applied[4]["color"].toInt(), 0);
    QStringList refused;
    for (const auto& result : rec.results)
        if (!result.second.isEmpty()) refused << result.second;
    ASSERT_EQ(refused.size(), 10) << refused.join(" | ").toStdString();
    for (qsizetype i = 5; i < refused.size(); ++i) {
        const char* reason = i == 9 ? "pulse effect must be" : "pulse needs 50 to 10000 ms and a palette color from 0 to 255";
        EXPECT_TRUE(refused[i].contains(QLatin1String(reason))) << i << ": " << refused[i].toStdString();
    }
}

TEST_F(ScriptHostTest, HandlerFailuresReleaseWhatTheScriptHolds)
{
    WRITE(user, "fails.luau", R"(
return {
    on_bell = function() error("broken") end,
    on_key = function(ev) return afterglow.copy(ev.key .. ":" .. ev.pane.id) end,
}
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_EQ(failedOwners.size(), 1);
    EXPECT_TRUE(failedOwners[0].startsWith("fails@"));
    // on_key reaches the named owner, and nobody else.
    host->dispatchTo(failedOwners[0], "on_key", 7, {{"key", "a"}});
    host->dispatchTo("someone@0", "on_key", 7, {{"key", "b"}});
    ASSERT_EQ(rec.applied.size(), 1);
    EXPECT_EQ(rec.applied[0]["text"], "a:7");
}

TEST_F(ScriptHostTest, PackagesShareTheirShadersSuggestionsAndPulseNames)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("scripted-overview"));
    QDir().mkpath(user.filePath("ripples/shaders/rings"));
    WRITE(user, "ripples/package.json", R"({"id":"ripples","version":"1.0.0","api_version":1,"entry":"main",
        "modules":{"main":"init.luau"},"shaders":{"path":"shaders","suggests":{"paneEffects":"rings"}},"pulses":{"ripple":7}})");
    WRITE(user, "ripples/shaders/rings/metadata.json", R"({"id":"ripples.rings","name":"Rings","fragmentShader":"effect.frag"})");
    WRITE(user, "ripples/shaders/rings/effect.frag", "vec4 pTerminal(vec2 uv) { return terminalTexel(uv); }");
    WRITE(user, "ripples/init.luau", R"(
return { on_bell = function(ev) return {afterglow.pulse(ev.pane, "ripple"), afterglow.pulse(ev.pane, "flash")} end }
)");
    QList<QMap<QString, QString>> announced;
    QObject::connect(host.get(), &ScriptHost::packageShadersChanged, host.get(),
                     [&](const QMap<QString, QString>& dirs) { announced << dirs; });
    host->setEnabled(true);
    ASSERT_EQ(announced.size(), 1);
    EXPECT_EQ(announced[0].keys(), QStringList({"ripples", "scripted-overview"}));
    EXPECT_TRUE(announced[0]["scripted-overview"].endsWith("scripted-overview/shaders"));
    QStringList suggestions;
    for (const auto& s : host->packageSuggestions())
        suggestions << s.toMap()["key"].toString() + "=" + s.toMap()["ref"].toString();
    suggestions.sort();
    EXPECT_EQ(suggestions, QStringList({"overview=scripted-overview.wall", "paneEffects=ripples.rings"}));
    host->dispatch("on_bell", 7);
    ASSERT_EQ(rec.applied.size(), 2);
    EXPECT_EQ(rec.applied[0]["effect"], "7");
    EXPECT_EQ(rec.applied[1]["effect"], "2"); // flash
    // Removing a package withdraws its shaders.
    QDir(user.filePath("ripples")).removeRecursively();
    host->reload();
    ASSERT_EQ(announced.size(), 2);
    EXPECT_EQ(announced[1].keys(), QStringList({"scripted-overview"}));
}

TEST_F(ScriptHostTest, HostLookupsNeverRunTheModulesIndex)
{
    // A "strict module" metatable would loop or raise outside any time limit.
    WRITE(user, "strict.luau", R"(
local s = {on_bell = function() return afterglow.copy("bell") end}
return setmetatable(s, {__index = function() while true do end end})
)");
    host->setEnabled(true);
    ASSERT_TRUE(host->scriptIds().contains("strict"));
    QElapsedTimer clock;
    clock.start();
    host->dispatch("on_focus", 7); // not defined: the lookup must not reach __index
    host->dispatch("on_bell", 7);
    EXPECT_LT(clock.elapsed(), 1000);
    ASSERT_EQ(rec.applied.size(), 1);
    EXPECT_EQ(rec.applied[0]["text"], "bell");
}

TEST_F(ScriptHostTest, SelfReferencingResultsAreCutNotExpanded)
{
    WRITE(user, "cycle.luau", R"(
return {
    on_bell = function()
        local e = afterglow.copy("cycle")
        e.a = e; e.b = e
        return e
    end,
    on_focus = function()
        local t = {}
        for i = 1, 40 do t = {a = t, b = t} end -- 2^40 paths through shared tables
        return {afterglow.copy("shared"), t}
    end,
}
)");
    host->setEnabled(true);
    QElapsedTimer clock;
    clock.start();
    host->dispatch("on_bell", 7);
    host->dispatch("on_focus", 7);
    EXPECT_LT(clock.elapsed(), 2000);
    // The shared tables stop the walk at its limit: the whole result is
    // refused, the valid copy beside them too.
    ASSERT_EQ(rec.applied.size(), 1);
    EXPECT_EQ(rec.applied[0]["text"], "cycle");
    const auto stats = firstDiagnostic();
    EXPECT_EQ(stats["handler_errors"].toULongLong(), 1u);
    EXPECT_TRUE(stats["last_error"].toString().contains("result too large")) << stats["last_error"].toString().toStdString();
}

TEST_F(ScriptHostTest, TurningScriptsOffFromAnEffectIsSafe)
{
    WRITE(user, "first.luau", "return {on_bell = function() return {afterglow.copy('off'), afterglow.copy('after')} end}");
    WRITE(user, "second.luau", "return {on_bell = function() return afterglow.copy('second') end}");
    // A reload_config action that turns scripting off runs inside the call.
    applyHook = [this](const QVariantMap& e) {
        if (e["text"] == QLatin1String("off")) host->setEnabled(false);
        return ScriptHost::EffectResult{};
    };
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    EXPECT_TRUE(host->scriptIds().isEmpty());
    EXPECT_FALSE(host->isEnabled());
    QStringList texts;
    for (const auto& e : rec.applied) texts << e["text"].toString();
    // The effect that turned scripting off applied, and nothing after it.
    EXPECT_EQ(texts, QStringList{"off"});
    applyHook = {};
    host->setEnabled(true);
    EXPECT_EQ(host->scriptIds().size(), 2);
}

TEST_F(ScriptHostTest, TurningScriptsOffDuringAScanWaitsForTheScan)
{
    WRITE(user, "loader.luau", "return {on_load = function() return afterglow.copy('off') end}");
    applyHook = [this](const QVariantMap& e) {
        if (e["text"] == QLatin1String("off")) host->setEnabled(false);
        return ScriptHost::EffectResult{};
    };
    host->setEnabled(true); // on_load runs inside the directory set's own scan
    EXPECT_FALSE(host->isEnabled());
    ASSERT_TRUE(await([&] { return host->scriptIds().isEmpty(); }));
    applyHook = {};
    host->setEnabled(true);
    EXPECT_EQ(host->scriptIds(), QStringList{"loader"});
}

TEST_F(ScriptHostTest, PackageManifestChangesReplaceTheInstance)
{
    QDir().mkpath(user.filePath("ripples/shaders/rings"));
    auto manifest = [&](int number) {
        WRITE(user, "ripples/package.json", QStringLiteral(R"({"id":"ripples","version":"1.0.0","api_version":1,"entry":"main",
            "modules":{"main":"init.luau"},"shaders":{"path":"shaders"},"pulses":{"ripple":%1}})").arg(number).toUtf8());
    };
    WRITE(user, "ripples/shaders/rings/metadata.json", R"({"id":"ripples.rings","name":"Rings","fragmentShader":"effect.frag"})");
    WRITE(user, "ripples/init.luau", "return { on_bell = function(ev) return afterglow.pulse(ev.pane, 'ripple') end }");
    ASSERT_NO_FATAL_FAILURE(manifest(7));
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_NO_FATAL_FAILURE(manifest(9));
    host->reload();
    host->dispatch("on_bell", 7);
    ASSERT_EQ(rec.applied.size(), 2);
    EXPECT_EQ(rec.applied[0]["effect"], "7");
    EXPECT_EQ(rec.applied[1]["effect"], "9");
}

TEST_F(ScriptHostTest, EventsHaveACallbackBudgetAndNoSnapshotWithoutAHandler)
{
    WRITE(user, "titles.luau", "return {on_title = function() return nil end}");
    host->setEnabled(true);
    const int before = snapshots;
    for (int i = 0; i < 10; ++i) host->dispatch("on_bell", 7);
    EXPECT_EQ(snapshots, before); // nobody handles on_bell
    for (int i = 0; i < ScriptHost::MaxCallbacksPerSecond + 6; ++i) host->dispatch("on_title", 7);
    ASSERT_FALSE(host->diagnostics().isEmpty());
    const auto stats = firstDiagnostic();
    EXPECT_EQ(stats["calls"].toULongLong(), quint64(ScriptHost::MaxCallbacksPerSecond));
    EXPECT_EQ(stats["dropped_callbacks"].toULongLong(), 6u);
}

TEST_F(ScriptHostTest, EventsOverTheCallbackBudgetBuildNoSnapshot)
{
    WRITE(user, "titles.luau", "return {on_title = function() return nil end}");
    host->setEnabled(true);
    for (int i = 0; i < ScriptHost::MaxCallbacksPerSecond; ++i) host->dispatch("on_title", 7);
    // A dropped event costs no snapshot: each one reads every pane's directory and process.
    const int before = snapshots;
    for (int i = 0; i < 10; ++i) host->dispatch("on_title", 7);
    EXPECT_EQ(snapshots, before);
    EXPECT_EQ(firstDiagnostic()["dropped_callbacks"].toULongLong(), 10u);
}

TEST_F(ScriptHostTest, RuntimeDisableWithdrawsPackageShadersAndCommands)
{
    QDir().mkpath(user.filePath("broken/shaders/wall"));
    WRITE(user, "broken/package.json", R"({"id":"broken","version":"1.0.0","api_version":1,"entry":"main",
        "modules":{"main":"init.luau"},"shaders":{"path":"shaders"},"commands":{"go":{"summary":"go"}}})");
    WRITE(user, "broken/shaders/wall/metadata.json", R"({"id":"broken.wall","name":"Wall","fragmentShader":"effect.frag"})");
    WRITE(user, "broken/init.luau", R"(
return {run_command = function() end, on_bell = function() error("broken") end}
)");
    QList<QMap<QString, QString>> announced;
    QObject::connect(host.get(), &ScriptHost::packageShadersChanged, host.get(),
                     [&](const QMap<QString, QString>& dirs) { announced << dirs; });
    int commandChanges = 0;
    QObject::connect(host.get(), &ScriptHost::commandsChanged, host.get(), [&] { ++commandChanges; });
    host->setEnabled(true);
    ASSERT_EQ(announced.size(), 1);
    EXPECT_TRUE(announced[0].contains("broken"));
    for (int i = 0; i < ScriptHost::MaxFailures; ++i) host->dispatch("on_bell", 7);
    EXPECT_TRUE(host->isDisabled("broken"));
    ASSERT_EQ(announced.size(), 2);
    EXPECT_TRUE(announced[1].isEmpty());
    EXPECT_EQ(commandChanges, 2); // loaded, then withdrawn
}

TEST_F(ScriptHostTest, PackageShadersExistBeforeOnLoadEffects)
{
    QDir().mkpath(user.filePath("early/shaders/wall"));
    WRITE(user, "early/package.json", R"({"id":"early","version":"1.0.0","api_version":1,"entry":"main",
        "modules":{"main":"init.luau"},"shaders":{"path":"shaders"}})");
    WRITE(user, "early/shaders/wall/metadata.json", R"({"id":"early.wall","name":"Wall","fragmentShader":"effect.frag"})");
    WRITE(user, "early/init.luau", "return {on_load = function() return afterglow.copy('loaded') end}");
    QStringList published;
    QObject::connect(host.get(), &ScriptHost::packageShadersChanged, host.get(),
                     [&](const QMap<QString, QString>& dirs) { published = dirs.keys(); });
    bool seen = false;
    applyHook = [&](const QVariantMap& e) {
        if (e["text"] == QLatin1String("loaded")) seen = published.contains("early");
        return ScriptHost::EffectResult{};
    };
    host->setEnabled(true);
    EXPECT_TRUE(seen);
}

TEST_F(ScriptHostTest, PackageErrorsNameTheModuleFileAndLine)
{
    QDir().mkpath(user.filePath("lines"));
    WRITE(user, "lines/package.json", R"({"id":"lines","version":"1.0.0","api_version":1,"entry":"main",
        "modules":{"main":"init.luau","util":"util.luau"}})");
    WRITE(user, "lines/init.luau", "local util = require('util')\nreturn {on_bell = function() util.fail() end}");
    WRITE(user, "lines/util.luau", "local u = {}\nfunction u.fail()\n    error('deliberate')\nend\nreturn u");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_FALSE(host->diagnostics().isEmpty());
    const auto error = firstDiagnostic()["last_error"].toString();
    EXPECT_TRUE(error.contains("util.luau:3:")) << error.toStdString();
    EXPECT_FALSE(error.contains("package.json:"));
}

TEST_F(ScriptHostTest, PackageModulesTakeVarargsAndCannotReplaceTheLoader)
{
    QDir().mkpath(user.filePath("loader"));
    WRITE(user, "loader/package.json", R"({"id":"loader","version":"1.0.0","api_version":1,"entry":"main",
        "modules":{"main":"init.luau","util":"util.luau"}})");
    // A forgotten "local" must not reach the loader's own variables.
    WRITE(user, "loader/util.luau", "local name = ...\ncache = {}\nfunction load() return {} end\nreturn {value = 1}");
    WRITE(user, "loader/init.luau", R"(
local a, b = require("util"), require("util")
assert(a == b and a.value == 1)
return {on_bell = function() return afterglow.copy(tostring(require("util") == a)) end}
)");
    host->setEnabled(true);
    ASSERT_EQ(host->scriptIds(), QStringList{"loader"});
    host->dispatch("on_bell", 7);
    ASSERT_EQ(rec.applied.size(), 1);
    EXPECT_EQ(rec.applied[0]["text"], "true");
}

TEST_F(ScriptHostTest, ForgedAwaitIdsCannotResumeAFlowEarly)
{
    WRITE(user, "forged.luau", R"(
return {
    on_bell = function(ev)
        local reply = afterglow.await(afterglow.prompt(ev.pane, "Name"))
        return afterglow.copy(reply and reply.text or "nothing")
    end,
    on_focus = function(ev)
        return {afterglow.request("afterglow.await:1:99", afterglow.focus(ev.pane)),
                afterglow.request("afterglow.await:1:0", afterglow.focus(ev.pane)),
                afterglow.request("afterglow.await:1:1", afterglow.focus(ev.pane))}
    end,
}
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_TRUE(bool(pendingReply));
    rec.results.clear();
    host->dispatch("on_focus", 7);
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    for (const auto& e : rec.applied) EXPECT_NE(e["kind"], "copy");
    // Not given in the call that registered the flow: refused, and with no result.
    ASSERT_EQ(rec.results.size(), 3);
    for (const auto& [effect, error] : std::as_const(rec.results)) EXPECT_TRUE(error.contains("belong to afterglow.await"));
    pendingReply({{}, {{"text", "real"}}, {}});
    ASSERT_TRUE(await([&] { return !rec.applied.isEmpty() && rec.applied.last()["kind"] == "copy"; }));
    EXPECT_EQ(rec.applied.last()["text"], "real");
}

TEST_F(ScriptHostTest, EventsWithoutAPaneHaveNoPaneAndClosedPanesKeepTheirId)
{
    WRITE(user, "panes.luau", R"(
return {
    on_config_changed = function(ev) return afterglow.copy(ev.pane == nil and "none" or "fake") end,
    on_bell = function() return afterglow.timer("later", 5) end,
    on_timer = function(ev) return afterglow.log("pane " .. tostring(ev.pane and ev.pane.id)) end,
}
)");
    host->setEnabled(true);
    host->dispatch("on_config_changed", 0);
    ASSERT_EQ(rec.applied.size(), 1);
    EXPECT_EQ(rec.applied[0]["text"], "none");
    host->dispatch("on_bell", 8);
    paneState.remove(8); // closes before the timer fires
    ASSERT_TRUE(await([&] {
        return std::any_of(rec.results.begin(), rec.results.end(), [](const auto& r) { return r.first["message"] == "pane 8"; });
    }));
}

TEST_F(ScriptHostTest, UnchangedCommandListsAnnounceNothing)
{
    WRITE(user, "dynamic.luau", R"(
return {
    run_command = function() end,
    on_bell = function() return afterglow.set_commands({one = {summary = "One"}}) end,
}
)");
    host->setEnabled(true);
    int changes = 0;
    QObject::connect(host.get(), &ScriptHost::commandsChanged, host.get(), [&] { ++changes; });
    host->dispatch("on_bell", 7);
    host->dispatch("on_bell", 7);
    EXPECT_EQ(changes, 1);
}

TEST_F(ScriptHostTest, SetCommandsFollowsTheManifestRules)
{
    // The same shape as the manifest's commands: names to their fields.
    WRITE(user, "dynamic.luau", QStringLiteral(R"(
local lists = {
    -- Each field its own value, so that two fields swapped show.
    {extra = {summary = "Extra summary", usage = "X", palette = "Extra", stdin = true, remote = false},
     far = {summary = "Far", stdin = false, remote = true}},
    {fixed = {summary = "Clash"}},
    {["-dash"] = {summary = "x"}},
    {["bad name"] = {summary = "x"}},
    {nosummary = {usage = "X"}},
    {x = {summary = "x", title = "y"}},
    {blank = {summary = "  "}},
    "not a table",
    {{name = "listed", summary = "A list, not a table of names"}},
    {y = {summary = "x", remote = 1}},
}
local function many(count)
    local out = {}
    for i = 1, count do out["m" .. i] = {summary = "x"} end
    return out
end
return {
    metadata = {commands = {fixed = {summary = "Fixed"}}},
    run_command = function() end,
    on_bell = function(ev) return afterglow.set_commands(lists[ev.pane.id - 6]) end,
    -- With the manifest's one, %1 more is over the limit, and one fewer is at it.
    on_focus = function() return afterglow.set_commands(many(%1)) end,
    on_command_started = function() return afterglow.set_commands(many(%1 - 1)) end,
    on_title = function() return afterglow.set_commands({}) end,
}
)").arg(MaxScriptCommands).toUtf8());
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    auto tokens = [this] {
        QStringList out;
        for (const ScriptCommand& c : host->commands()) out << c.token();
        return out;
    };
    ASSERT_EQ(tokens(), (QStringList{"dynamic.fixed", "dynamic.extra", "dynamic.far"}));
    EXPECT_FALSE(host->commands()[0].runtime);
    const ScriptCommand extra = host->commands()[1];
    EXPECT_TRUE(extra.runtime);
    EXPECT_TRUE(extra.input);
    EXPECT_FALSE(extra.remote);
    EXPECT_EQ(extra.summary, "Extra summary");
    EXPECT_EQ(extra.usage, "X");
    EXPECT_EQ(extra.palette, "Extra");
    const ScriptCommand far = host->commands()[2];
    EXPECT_TRUE(far.runtime);
    EXPECT_FALSE(far.input);
    EXPECT_TRUE(far.remote);
    EXPECT_EQ(far.summary, "Far");
    EXPECT_TRUE(far.usage.isEmpty());
    EXPECT_TRUE(far.palette.isEmpty());
    const std::pair<int, const char*> refused[] = {{2, "Command fixed is in the manifest already"}, {3, "must not start with -"},
        {4, "must be 1 to 64"}, {5, "needs a summary"}, {6, "unknown field title"}, {7, "needs a summary"},
        {8, "must map command names"}, {9, "must map command names"}, {10, "remote must be a boolean"}};
    for (const auto& [index, error] : refused) {
        const quint32 pane = quint32(6 + index);
        paneState.insert(pane, paneState.value(7));
        paneState[pane]["id"] = pane;
        rec.results.clear();
        host->dispatch("on_bell", pane);
        EXPECT_EQ(rec.results.size(), 1) << index;
        if (rec.results.size() != 1) continue;
        EXPECT_TRUE(rec.results[0].second.contains(QLatin1String(error))) << index << " " << rec.results[0].second.toStdString();
    }
    // The refused lists changed nothing.
    EXPECT_EQ(tokens(), (QStringList{"dynamic.fixed", "dynamic.extra", "dynamic.far"}));
    rec.results.clear();
    host->dispatch("on_focus", 7);
    ASSERT_EQ(rec.results.size(), 1);
    EXPECT_EQ(rec.results[0].second, QStringLiteral("A script has at most %1 commands").arg(MaxScriptCommands));
    rec.results.clear();
    host->dispatch("on_command_started", 7);
    ASSERT_EQ(rec.results.size(), 1);
    EXPECT_TRUE(rec.results[0].second.isEmpty()) << rec.results[0].second.toStdString();
    EXPECT_EQ(host->commands().size(), MaxScriptCommands);
    // An empty table keeps the manifest's.
    host->dispatch("on_title", 7);
    EXPECT_EQ(tokens(), QStringList{"dynamic.fixed"});
}

namespace {

/// Commands for the run tests. "wait" writes "a" and then sleeps, so it is
/// still running when the call that started it returns.
const QByteArray RunScript = R"(
return {
    metadata = { commands = {
        greet = { summary = "Greet", usage = "[NAME]", palette = "Greet someone" },
        read = { summary = "Read stdin", stdin = true },
        noinput = { summary = "No stdin" },
        wait = { summary = "Wait, then print" },
        fail = { summary = "Fail" },
        later = { summary = "Fail after a wait" },
        spin = { summary = "Never return" },
        loud = { summary = "Print a lot" },
        status = { summary = "Exit without output" },
        forge = { summary = "Forge the end of another run" },
        twolines = { summary = "Two lines on stderr" },
        noisy = { summary = "Print a lot on stderr" },
        lines = { summary = "Print each line, and wait until it has gone" },
    } },
    run_command = function(command, ev)
        if command == "greet" then
            return {
                afterglow.output(ev, "hello " .. (ev.args[1] or "world") .. " in " .. ev.cwd .. " from " .. ev.source
                    .. " on " .. tostring(ev.pane and ev.pane.id) .. "\n"),
                afterglow.output_error(ev.run, "careful\n"),
                afterglow.exit(ev, 3),
            }
        elseif command == "read" or command == "noinput" then
            return afterglow.output(ev, "got " .. tostring(ev.stdin))
        elseif command == "wait" then
            assert(afterglow.await(afterglow.output(ev, "a")))
            afterglow.sleep(tonumber(ev.args[1]) or 10)
            local results = afterglow.await_all({ afterglow.output(ev, "b"), afterglow.output_error(ev, "c") })
            assert(results[1].ok and results[2].ok)
            return afterglow.exit(ev, 5)
        elseif command == "fail" then
            error("broken on purpose")
        elseif command == "later" then
            afterglow.sleep(5)
            error("broken after a wait")
        elseif command == "spin" then
            while true do end
        elseif command == "loud" then
            return afterglow.output(ev, string.rep("x", 100))
        elseif command == "status" then
            return afterglow.exit(ev, 4)
        elseif command == "forge" then
            return { __run_done = ev.args[1], result = afterglow.output(ev, "forged") }
        elseif command == "twolines" then
            return afterglow.output_error(ev, "first\nsecond\n")
        elseif command == "noisy" then
            return afterglow.output_error(ev, string.rep("e", 100))
        elseif command == "lines" then
            for i = 1, tonumber(ev.args[1]) do
                assert(afterglow.await(afterglow.output(ev, "x")))
            end
            return afterglow.exit(ev, 0)
        end
    end,
}
)";

} // namespace

TEST_F(ScriptHostTest, ARemoteHostRunsOnlyTheCommandsThatDeclareRemote)
{
    WRITE(user, "far.luau", R"(
return {
    metadata = { commands = {
        here = { summary = "Local only" },
        there = { summary = "From remote hosts too", remote = true },
    } },
    -- The commands of set_commands follow the same rule.
    on_bell = function()
        return afterglow.set_commands({
            late = { summary = "Added, from remote hosts too", remote = true },
            near = { summary = "Added, local only" },
        })
    end,
    run_command = function(command, ev)
        -- "none" with no ev.remote, so that a table without its host shows.
        local remote = ev.remote == nil and "none" or "host=" .. tostring(ev.remote.host)
        return afterglow.output(ev, command .. " " .. ev.source .. " " .. remote .. " " .. ev.cwd .. "\n")
    end,
}
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    const auto commands = host->commands();
    ASSERT_EQ(commands.size(), 4);
    EXPECT_FALSE(commands[0].remote); // here
    EXPECT_TRUE(commands[1].remote); // there
    EXPECT_TRUE(commands[2].runtime && commands[2].remote); // late
    EXPECT_TRUE(commands[3].runtime && !commands[3].remote); // near

    // A run that starts: its handle, its output, no error and status 0.
    const auto run = [&](const QString& command, const QString& source, const QString& remoteHost) {
        Client& client = newClient();
        QString error;
        EXPECT_NE(startRequest({"far", command, {}, "/home/far/src", 7, std::nullopt, source, remoteHost}, client, &error), 0u)
            << error.toStdString();
        EXPECT_EQ(client.status, 0) << command.toStdString() << ": " << client.err.toStdString();
        EXPECT_TRUE(client.err.isEmpty()) << client.err.toStdString();
        return client.out;
    };
    // The cwd is the remote host's, as text.
    EXPECT_EQ(run("there", "remote", "box"), "there remote host=box /home/far/src\n");
    EXPECT_EQ(run("there", "shell", ""), "there shell none /home/far/src\n");
    // A host goes with the remote source only.
    EXPECT_EQ(run("there", "shell", "box"), "there shell none /home/far/src\n");
    EXPECT_EQ(run("here", "shell", ""), "here shell none /home/far/src\n");
    EXPECT_EQ(run("late", "remote", "box"), "late remote host=box /home/far/src\n");
    EXPECT_EQ(run("near", "shell", ""), "near shell none /home/far/src\n");
    // For a remote host, a command without remote does not exist.
    for (const char* command : {"here", "near"}) {
        SCOPED_TRACE(command);
        Client& client = newClient();
        QString error;
        bool unknown = false;
        EXPECT_EQ(startRequest({"far", command, {}, "/home/far/src", 7, std::nullopt, "remote", "box"}, client, &error, &unknown), 0u);
        EXPECT_TRUE(unknown);
        EXPECT_EQ(error, QStringLiteral("No command %1 in far").arg(QLatin1String(command)));
        EXPECT_TRUE(client.out.isEmpty());
        EXPECT_FALSE(client.status);
    }
}

TEST_F(ScriptHostTest, CommandRunGetsItsEventAndWritesItsOutput)
{
    WRITE(user, "cmds.luau", RunScript);
    host->setEnabled(true);
    Client& greet = newClient();
    ASSERT_NE(start("cmds", "greet", greet, {"you"}), 0u);
    // A handler that does not wait ends its run before startCommand returns.
    EXPECT_EQ(greet.out, "hello you in /work from shell on 7\n");
    EXPECT_EQ(greet.err, "careful\n");
    EXPECT_EQ(greet.status, 3);

    // A run with a client tells it, not the window.
    EXPECT_TRUE(notices.isEmpty());

    Client& read = newClient();
    Client& noStdin = newClient();
    Client& emptyInput = newClient();
    Client& noinput = newClient();
    Client& nopane = newClient();
    start("cmds", "read", read, {}, 7, QStringLiteral("line one\n"));
    EXPECT_EQ(read.out, "got line one\n");
    start("cmds", "read", noStdin);
    EXPECT_EQ(noStdin.out, "got nil");
    // An empty stdin is a stdin: the client sent its end.
    start("cmds", "read", emptyInput, {}, 7, QString());
    EXPECT_EQ(emptyInput.out, "got ");
    // A command without stdin never gets the client's.
    start("cmds", "noinput", noinput, {}, 7, QStringLiteral("secret"));
    EXPECT_EQ(noinput.out, "got nil");
    // Without a pane, the event has none, and the directory is the client's.
    start("cmds", "greet", nopane, {}, 0);
    EXPECT_EQ(nopane.out, "hello world in /work from shell on nil\n");
    // A client with no directory gets the pane's, and with no pane either, none.
    paneState[7]["cwd"] = "/pane";
    Client& paneDir = newClient();
    Client& noDir = newClient();
    ScriptHost::CommandSink paneSink, noSink;
    paneSink.output = [c = &paneDir](const QString& text, std::function<void(bool)> sent) { c->out += text; sent(true); return true; };
    paneSink.finished = [c = &paneDir](int status) { c->status = status; };
    noSink.output = [c = &noDir](const QString& text, std::function<void(bool)> sent) { c->out += text; sent(true); return true; };
    noSink.finished = [c = &noDir](int status) { c->status = status; };
    host->startCommand({"cmds", "greet", {}, {}, 7, std::nullopt, "shell", {}}, std::move(paneSink), nullptr);
    host->startCommand({"cmds", "greet", {}, {}, 0, std::nullopt, "shell", {}}, std::move(noSink), nullptr);
    EXPECT_EQ(paneDir.out, "hello world in /pane from shell on 7\n");
    EXPECT_EQ(noDir.out, "hello world in  from shell on nil\n");

    Client& none = newClient();
    QString error;
    bool unknown = false;
    EXPECT_EQ(start("cmds", "nope", none, {}, 7, std::nullopt, &error, &unknown), 0u);
    EXPECT_TRUE(unknown);
    EXPECT_EQ(error, "No command nope in cmds");
    unknown = false;
    EXPECT_EQ(start("other", "greet", none, {}, 7, std::nullopt, &error, &unknown), 0u);
    EXPECT_TRUE(unknown);
    // At the argument limits a run starts; past them it does not.
    QStringList most;
    for (int i = 0; i < ScriptHost::MaxCommandArgs; ++i) most << "x";
    Client& full = newClient();
    Client& longest = newClient();
    EXPECT_NE(start("cmds", "greet", full, most), 0u);
    EXPECT_EQ(full.status, 3);
    EXPECT_NE(start("cmds", "greet", longest, {QString(ScriptHost::MaxCommandArgLength, 'x')}), 0u);
    EXPECT_EQ(longest.status, 3);
    QStringList tooMany = most;
    tooMany << "x";
    EXPECT_EQ(start("cmds", "greet", none, tooMany, 7, std::nullopt, &error, &unknown), 0u);
    EXPECT_FALSE(unknown);
    EXPECT_EQ(error, QStringLiteral("A command takes at most %1 arguments").arg(ScriptHost::MaxCommandArgs));
    EXPECT_EQ(start("cmds", "greet", none, {QString(ScriptHost::MaxCommandArgLength + 1, 'x')}, 7, std::nullopt, &error), 0u);
    EXPECT_EQ(error, QStringLiteral("A command argument has at most %1 UTF-16 units").arg(ScriptHost::MaxCommandArgLength));
    // A token without a dot names no command.
    EXPECT_EQ(host->runCommand("cmds", 7, "palette"), "A script command is <script id>.<command>, not cmds");
    EXPECT_FALSE(none.status);
}

TEST_F(ScriptHostTest, CommandRunWithNoClientShowsItsErrorsAsANotification)
{
    WRITE(user, "cmds.luau", RunScript);
    host->setEnabled(true);
    // The palette's run has no client: stdout is dropped, stderr shows.
    ASSERT_TRUE(run("cmds.greet", 7));
    ASSERT_EQ(notices.size(), 1);
    EXPECT_EQ(notices[0].pane, 7u);
    EXPECT_EQ(notices[0].title, "Greet someone");
    EXPECT_EQ(notices[0].body, "careful");
    // A non-zero status with no stderr names the status, under "<script> <command>".
    ASSERT_TRUE(run("cmds.status", 7));
    ASSERT_EQ(notices.size(), 2);
    EXPECT_EQ(notices[1].title, "cmds status");
    EXPECT_EQ(notices[1].body, "Exited with status 4");
    // With no pane (a palette with no focused pane), the notice names pane 0:
    // a desktop notification that belongs to no pane.
    ASSERT_TRUE(run("cmds.status", 0));
    ASSERT_EQ(notices.size(), 3);
    EXPECT_EQ(notices[2].pane, 0u);
    EXPECT_EQ(notices[2].body, "Exited with status 4");
    // Success with no stderr is silent.
    ASSERT_TRUE(run("cmds.loud", 7));
    EXPECT_EQ(notices.size(), 3);
    EXPECT_TRUE(rec.applied.isEmpty());
    EXPECT_FALSE(run("cmds.nope", 7));
    // The notice keeps the lines of stderr.
    ASSERT_TRUE(run("cmds.twolines", 7));
    ASSERT_EQ(notices.size(), 4);
    EXPECT_EQ(notices[3].body, "first\nsecond");

    // The dropped stdout costs nothing of the output limit; stderr does.
    ScriptHost::CommandLimits limits;
    limits.outputBytes = 50;
    limits.timeoutMs = 50;
    host->setCommandLimits(limits);
    notices.clear();
    ASSERT_TRUE(run("cmds.loud", 7));
    EXPECT_TRUE(notices.isEmpty()) << notices.value(0).body.toStdString();
    ASSERT_TRUE(run("cmds.noisy", 7));
    ASSERT_EQ(notices.size(), 1);
    EXPECT_TRUE(notices[0].body.contains("Scripts.commandOutputLimit")) << notices[0].body.toStdString();
    // A run that ends at its time limit says so. A sink with no finished function is a run with no client.
    ASSERT_NE(host->startCommand({"cmds", "wait", {"400"}, {}, 7, std::nullopt, "palette", {}}, {}, nullptr), 0u);
    ASSERT_TRUE(await([&] { return notices.size() == 2; }));
    EXPECT_TRUE(notices[1].body.contains("Scripts.commandTimeout")) << notices[1].body.toStdString();
}

TEST_F(ScriptHostTest, CommandRunEndsWhenItsWaitingHandlerDoes)
{
    WRITE(user, "cmds.luau", RunScript);
    host->setEnabled(true);
    Client& client = newClient();
    ASSERT_NE(start("cmds", "wait", client), 0u);
    EXPECT_EQ(client.out, "a");
    EXPECT_FALSE(client.status);
    ASSERT_TRUE(await([&] { return client.status.has_value(); }));
    EXPECT_EQ(client.out, "ab");
    EXPECT_EQ(client.err, "c");
    EXPECT_EQ(client.status, 5);
    EXPECT_EQ(firstDiagnostic()["waiting"].toInt(), 0);
}

TEST_F(ScriptHostTest, CommandRunsTimeOutAndCancel)
{
    WRITE(user, "cmds.luau", RunScript);
    host->setEnabled(true);
    ScriptHost::CommandLimits limits;
    limits.timeoutMs = 50;
    host->setCommandLimits(limits);
    Client& slow = newClient();
    start("cmds", "wait", slow, {"400"});
    ASSERT_TRUE(await([&] { return slow.status.has_value(); }));
    EXPECT_EQ(slow.status, ScriptHost::CommandTimedOut);
    EXPECT_TRUE(slow.err.contains("Scripts.commandTimeout")) << slow.err.toStdString();
    // The handler resumed with "cancelled" and returns: nothing waits.
    EXPECT_TRUE(await([&] { return firstDiagnostic()["waiting"].toInt() == 0; }));

    host->setCommandLimits({});
    Client& cancelled = newClient();
    const quint64 handle = start("cmds", "wait", cancelled, {"200"});
    host->cancelCommand(handle, 130);
    EXPECT_EQ(cancelled.status, 130);
    host->cancelCommand(handle, 1); // over already
    EXPECT_EQ(cancelled.status, 130);
    // The handler resumes once, with "cancelled", and its sleep stops: past
    // the sleep's time, no result calls the script again.
    pump();
    const auto calls = firstDiagnostic()["calls"].toULongLong();
    await([] { return false; }, 300);
    EXPECT_EQ(firstDiagnostic()["calls"].toULongLong(), calls);
    EXPECT_EQ(cancelled.out, "a");
    EXPECT_EQ(slow.out, "a");
    EXPECT_EQ(firstDiagnostic()["handler_errors"].toInt(), 0);
}

TEST_F(ScriptHostTest, AnEndedRunStopsTheSleepsAndProcessesItWaitsFor)
{
    // Every sleep that a script can have, so that the bell's sleep proves a slot came back.
    WRITE(user, "stops.luau", QStringLiteral(R"(
local log = {}
return {
    metadata = { commands = {
        sleeps = { summary = "Wait for every sleep a script can have" },
        job = { summary = "Wait for a long process" },
        log = { summary = "Print what happened" },
    } },
    run_command = function(command, ev)
        if command == "sleeps" then
            local all = {}
            for i = 1, %1 do all[i] = { kind = "sleep", delay_ms = 60000 } end
            local results = afterglow.await_all(all)
            table.insert(log, "sleeps:" .. results[1].error.code)
        elseif command == "job" then
            local _, err = afterglow.await(afterglow.exec("job", { "/bin/sh", "-c", "sleep 30" }))
            table.insert(log, "job:" .. err.code)
        elseif command == "log" then
            return afterglow.output(ev, table.concat(log, ","))
        end
    end,
    on_bell = function()
        -- Free slots: a sleep, and the task name of the process.
        local _, err = afterglow.sleep(0)
        local started, failed = afterglow.await(afterglow.exec("job", { "/bin/sh", "-c", "true" }))
        table.insert(log, "bell:" .. (err and err.code or "slept") .. ":" .. (started and "started" or failed.code))
        return afterglow.log("bell done")
    end,
}
)").arg(ScriptHost::MaxSleeping).toUtf8());
    host->setEnabled(true);
    if (!childrenListed()) GTEST_SKIP() << "The kernel has no /proc/PID/task/TID/children";
    Client& sleeps = newClient();
    Client& job = newClient();
    const auto before = children();
    const quint64 first = start("stops", "sleeps", sleeps);
    const quint64 second = start("stops", "job", job);
    pump();
    EXPECT_FALSE(sleeps.status || job.status);
    ASSERT_TRUE(await([&] { return !children().subtract(before).isEmpty(); }));
    host->cancelCommand(first, 130);
    host->cancelCommand(second, 130);
    // The process was killed, not only forgotten.
    ASSERT_TRUE(await([&] { return children().subtract(before).isEmpty(); }));
    pump();
    host->dispatch("on_bell", 7);
    // One run reads the log, once the bell's handler has returned.
    ASSERT_TRUE(await([&] {
        return std::any_of(rec.results.cbegin(), rec.results.cend(), [](const auto& r) { return r.first["message"] == "bell done"; });
    }));
    Client& client = newClient();
    start("stops", "log", client);
    ASSERT_TRUE(await([&] { return client.status.has_value(); }));
    EXPECT_EQ(client.out, "sleeps:cancelled,job:cancelled,bell:slept:started");
}

TEST_F(ScriptHostTest, ACommandCanAwaitEachLineOfItsOutput)
{
    // Each awaited output is a result, more than the results a second that
    // disable a script: the output limit bounds them instead.
    WRITE(user, "cmds.luau", RunScript);
    host->setEnabled(true);
    const int lines = ScriptHost::MaxResultsPerSecond * 3;
    Client& client = newClient();
    start("cmds", "lines", client, {QString::number(lines)});
    ASSERT_TRUE(await([&] { return client.status.has_value(); }));
    EXPECT_EQ(client.status, 0) << client.err.toStdString();
    EXPECT_EQ(client.out, QString(lines, 'x'));
    EXPECT_FALSE(host->isDisabled("cmds"));
}

TEST_F(ScriptHostTest, AnAwaitedOutputWithNoClientCountsTowardTheResults)
{
    // A run with no client drops its stdout at once, so nothing else bounds
    // a handler that awaits it in a loop: it would resume on each pass of
    // the event loop until the run's time limit.
    WRITE(user, "cmds.luau", RunScript);
    host->setEnabled(true);
    const QString lines = QString::number(ScriptHost::MaxResultsPerSecond * 3);
    ASSERT_NE(host->startCommand({"cmds", "lines", {lines}, {}, 7, std::nullopt, "palette", {}}, {}, nullptr), 0u);
    ASSERT_TRUE(await([&] { return host->isDisabled("cmds"); }));
    ASSERT_FALSE(notices.isEmpty());
    EXPECT_TRUE(notices.last().body.contains("operation results")) << notices.last().body.toStdString();
}

TEST_F(ScriptHostTest, AnAwaitedOutputToARunThatEndsResolvesAsCancelled)
{
    // A handler that is not the run's waits for output that the client drops.
    WRITE(user, "outside.luau", R"(
local run = nil
local got = "none"
return {
    metadata = { commands = { hold = { summary = "Wait" }, got = { summary = "Print the result" } } },
    run_command = function(command, ev)
        if command == "hold" then
            run = ev.run
            afterglow.sleep(60000)
        else
            return afterglow.output(ev, got)
        end
    end,
    on_bell = function()
        local _, err = afterglow.await(afterglow.output(run, "x"))
        got = err and (err.code .. ":" .. err.message) or "sent"
    end,
}
)");
    host->setEnabled(true);
    Client& held = newClient();
    held.hold = true;
    const quint64 handle = start("outside", "hold", held);
    host->dispatch("on_bell", 7);
    ASSERT_EQ(held.held.size(), 1);
    host->cancelCommand(handle, 130);
    // The client drops what it had not read.
    held.held.takeFirst()(false);
    pump();
    Client& got = newClient();
    start("outside", "got", got);
    EXPECT_EQ(got.out, "cancelled:The run has ended");
}

TEST_F(ScriptHostTest, AFailureAfterTheEndOfARunReleasesTheKeyboard)
{
    WRITE(user, "keys.luau", R"(
return {
    metadata = { commands = { mode = { summary = "Hold a key mode, then fail after the run ends" } } },
    on_key = function() end,
    run_command = function(command, ev)
        local _, err = afterglow.await(afterglow.key_mode(ev.pane, "Keys"))
        afterglow.sleep(60000)
        error("stopped after the end")
    end,
}
)");
    host->setEnabled(true);
    Client& client = newClient();
    const quint64 handle = start("keys", "mode", client);
    pump();
    ASSERT_TRUE(failedOwners.isEmpty());
    host->cancelCommand(handle, 130);
    pump();
    // Not a failure of the script, but whatever it holds must not trap the user.
    EXPECT_EQ(failedOwners.size(), 1);
    EXPECT_EQ(firstDiagnostic()["handler_errors"].toInt(), 0);
}

TEST_F(ScriptHostTest, CommandRunOfAClientThatLeftIsCancelled)
{
    WRITE(user, "cmds.luau", RunScript);
    host->setEnabled(true);
    Client& client = newClient();
    start("cmds", "wait", client, {"20"});
    EXPECT_EQ(client.out, "a");
    client.gone = true;
    ASSERT_TRUE(await([&] { return client.status.has_value(); }));
    EXPECT_EQ(client.status, 1);
    // The write that found the client gone is refused.
    QStringList errors;
    for (const auto& [effect, error] : std::as_const(rec.results))
        if (effect["kind"] == "output") errors << error;
    EXPECT_TRUE(errors.contains("The run's output could not go to its client: the client left, or too much output waits for "
                                "slow clients"));
}

TEST_F(ScriptHostTest, FailedCommandHandlersEndTheirRunOnStderr)
{
    WRITE(user, "cmds.luau", RunScript);
    host->setEnabled(true);
    Client& fail = newClient();
    Client& later = newClient();
    Client& spin = newClient();
    start("cmds", "fail", fail);
    EXPECT_EQ(fail.status, 1);
    EXPECT_TRUE(fail.err.startsWith("The command failed: ")) << fail.err.toStdString();
    EXPECT_TRUE(fail.err.contains("broken on purpose")) << fail.err.toStdString();
    start("cmds", "later", later);
    EXPECT_FALSE(later.status);
    ASSERT_TRUE(await([&] { return later.status.has_value(); }));
    EXPECT_EQ(later.status, 1);
    EXPECT_TRUE(later.err.startsWith("The command failed after a wait: ")) << later.err.toStdString();
    EXPECT_TRUE(later.err.contains("broken after a wait")) << later.err.toStdString();
    start("cmds", "spin", spin);
    EXPECT_EQ(spin.status, 1);
    EXPECT_TRUE(spin.err.contains(QStringLiteral("took longer than %1 ms").arg(ScriptHost::CommandCallTimeoutMs))) << spin.err.toStdString();
}

TEST_F(ScriptHostTest, CommandRunsStayWithinTheirLimits)
{
    WRITE(user, "cmds.luau", RunScript);
    host->setEnabled(true);
    ScriptHost::CommandLimits limits;
    limits.runs = 2;
    limits.outputBytes = 50;
    limits.inputBytes = 4;
    host->setCommandLimits(limits);
    Client& one = newClient();
    Client& two = newClient();
    Client& three = newClient();
    const quint64 first = start("cmds", "wait", one, {"200"});
    const quint64 second = start("cmds", "wait", two, {"200"});
    QString error;
    bool unknown = true;
    EXPECT_EQ(start("cmds", "greet", three, {}, 7, std::nullopt, &error, &unknown), 0u);
    EXPECT_FALSE(unknown);
    EXPECT_TRUE(error.contains("Scripts.commandRunLimit")) << error.toStdString();
    host->cancelCommand(first, 130);
    EXPECT_NE(start("cmds", "greet", three), 0u);
    EXPECT_EQ(three.status, 3);

    Client& loud = newClient();
    start("cmds", "loud", loud);
    EXPECT_EQ(loud.status, 1);
    EXPECT_TRUE(loud.out.isEmpty());
    EXPECT_TRUE(loud.err.contains("Scripts.commandOutputLimit")) << loud.err.toStdString();

    // Exactly at the output limit, stdout and stderr together, a run ends well.
    limits.outputBytes = 100;
    host->setCommandLimits(limits);
    Client& exact = newClient();
    Client& both = newClient();
    start("cmds", "loud", exact);
    EXPECT_EQ(exact.status, 0);
    EXPECT_EQ(exact.out, QString(100, 'x'));
    limits.outputBytes = 37 + 8; // "hello world in /work from shell on 7\n" and "careful\n"
    host->setCommandLimits(limits);
    start("cmds", "greet", both);
    EXPECT_EQ(both.status, 3) << both.err.toStdString();
    limits.outputBytes = 44;
    host->setCommandLimits(limits);
    Client& over = newClient();
    start("cmds", "greet", over);
    EXPECT_EQ(over.status, 1);
    EXPECT_TRUE(over.err.endsWith("its limit (Scripts.commandOutputLimit)\n")) << over.err.toStdString();

    // CommandSocket refuses stdin over Scripts.commandStdinLimit: what arrives is whole.
    Client& read = newClient();
    start("cmds", "read", read, {}, 7, QStringLiteral("abcé"));
    EXPECT_EQ(read.out, "got abcé");
    // The second run was live all along: the limit refused only the third.
    host->cancelCommand(second, 130);
    EXPECT_EQ(two.status, 130);
}

TEST_F(ScriptHostTest, ARunKeepsTheOutputLimitThatItStartedWith)
{
    WRITE(user, "cmds.luau", RunScript);
    host->setEnabled(true);
    // "wait" prints "a", sleeps, then prints "b", and "c" on stderr.
    ScriptHost::CommandLimits limits;
    limits.outputBytes = 1;
    host->setCommandLimits(limits);
    Client& small = newClient();
    start("cmds", "wait", small, {"20"});
    limits.outputBytes = 100;
    host->setCommandLimits(limits);
    ASSERT_TRUE(await([&] { return small.status.has_value(); }));
    EXPECT_EQ(small.status, 1);
    EXPECT_EQ(small.out, "a");
    EXPECT_TRUE(small.err.contains("is over 1 bytes")) << small.err.toStdString();

    Client& large = newClient();
    start("cmds", "wait", large, {"20"});
    limits.outputBytes = 1;
    host->setCommandLimits(limits);
    ASSERT_TRUE(await([&] { return large.status.has_value(); }));
    EXPECT_EQ(large.status, 5) << large.err.toStdString();
    EXPECT_EQ(large.out, "ab");
    EXPECT_EQ(large.err, "c");
}

TEST_F(ScriptHostTest, ARetiredScriptEndsItsRunsAndWithdrawsItsCommands)
{
    WRITE(user, "cmds.luau", RunScript);
    host->setEnabled(true);
    Client& client = newClient();
    start("cmds", "wait", client, {"300"});
    WRITE(user, "cmds.luau", "return {metadata = {commands = {other = {summary = 'Other'}}}, run_command = function() end}");
    host->reload();
    EXPECT_EQ(client.status, 1);
    EXPECT_TRUE(client.err.contains("The script stopped")) << client.err.toStdString();
    ASSERT_EQ(host->commands().size(), 1);
    EXPECT_EQ(host->commands()[0].token(), "cmds.other");
    QFile::remove(user.filePath("cmds.luau"));
    host->reload();
    EXPECT_TRUE(host->commands().isEmpty());
}

TEST_F(ScriptHostTest, ARunThatIsLiveWhenTheHostGoesEndsForItsClient)
{
    WRITE(user, "cmds.luau", RunScript);
    host->setEnabled(true);
    // What every test that ends with a live run does at its teardown: the
    // fixture's clients outlive the host for it.
    Client& client = newClient();
    ASSERT_NE(start("cmds", "wait", client, {"60000"}), 0u);
    ASSERT_FALSE(client.status);
    host.reset();
    EXPECT_EQ(client.status, 1);
    EXPECT_TRUE(client.err.contains("The script stopped")) << client.err.toStdString();
}

TEST_F(ScriptHostTest, ARunEndsOnlyItself)
{
    WRITE(user, "cmds.luau", RunScript);
    WRITE(user, "other.luau", R"(
return {
    on_bell = function(ev) return { __run_done = ev.target, result = afterglow.copy("x") } end,
    on_focus = function(ev) return afterglow.output(ev.target, "not mine") end,
}
)");
    host->setEnabled(true);
    Client& waiting = newClient();
    Client& forger = newClient();
    const quint64 handle = start("cmds", "wait", waiting, {"100"});
    const QString target = QString::number(handle);
    // Another run's handler cannot end it, and another script cannot write to it.
    start("cmds", "forge", forger, {target});
    EXPECT_EQ(forger.status, 0);
    EXPECT_TRUE(forger.out.isEmpty()); // the made-up table is no effect, so its output never applied
    EXPECT_FALSE(waiting.status);
    rec.results.clear();
    host->dispatch("on_bell", 7, {{"target", target}});
    host->dispatch("on_focus", 7, {{"target", target}});
    EXPECT_FALSE(waiting.status);
    EXPECT_EQ(waiting.out, "a");
    ASSERT_EQ(rec.results.size(), 2);
    EXPECT_EQ(rec.results[0].second, "unknown effect \"\"");
    EXPECT_EQ(rec.results[1].second, QStringLiteral("Run %1 has ended, or is not a run of this script").arg(handle));
    for (const auto& e : std::as_const(rec.applied)) EXPECT_NE(e["kind"], "copy");
    ASSERT_TRUE(await([&] { return waiting.status.has_value(); }));
    EXPECT_EQ(waiting.status, 5);
}

TEST_F(ScriptHostTest, PackageCommandsComeFromItsManifest)
{
    QDir().mkpath(user.filePath("pkg"));
    WRITE(user, "pkg/package.json", R"({"id":"pkg","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"init.luau"},
        "commands":{"go":{"summary":"Go","usage":"WHERE","stdin":true,"remote":true}}})");
    WRITE(user, "pkg/init.luau", "return {run_command = function(_, ev) return afterglow.output(ev, ev.args[1]) end}");
    QDir().mkpath(user.filePath("typed"));
    WRITE(user, "typed/package.json", R"({"id":"typed","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"init.luau"},
        "commands":{"go":{"summary":"Go","remote":"yes"}}})");
    WRITE(user, "typed/init.luau", "return {run_command = function() end}");
    QDir().mkpath(user.filePath("meta"));
    WRITE(user, "meta/package.json", R"({"id":"meta","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"init.luau"}})");
    WRITE(user, "meta/init.luau", "return {metadata = {commands = {go = {summary = 'Go'}}}, run_command = function() end}");
    host->setEnabled(true);
    EXPECT_EQ(host->scriptIds(), QStringList{"pkg"});
    ASSERT_EQ(host->commands().size(), 1);
    EXPECT_EQ(host->commands()[0].token(), "pkg.go");
    EXPECT_EQ(host->commands()[0].usage, "WHERE");
    EXPECT_TRUE(host->commands()[0].input);
    EXPECT_TRUE(host->commands()[0].remote);
    Client& client = newClient();
    start("pkg", "go", client, {"there"});
    EXPECT_EQ(client.out, "there");
    QStringList errors;
    for (const QVariant& d : host->diagnostics()) errors << d.toMap()["error"].toString();
    EXPECT_TRUE(errors.contains("A package declares its commands in package.json"));
    EXPECT_TRUE(errors.contains("package.json: Command go: remote must be a boolean")) << errors.join(", ").toStdString();
    // A change to its commands loads the package again.
    WRITE(user, "pkg/package.json", R"({"id":"pkg","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"init.luau"},
        "commands":{"come":{"summary":"Come"}}})");
    host->reload();
    ASSERT_EQ(host->commands().size(), 1);
    EXPECT_EQ(host->commands()[0].token(), "pkg.come");
    EXPECT_FALSE(host->commands()[0].remote);
    // remote alone is a change too.
    WRITE(user, "pkg/package.json", R"({"id":"pkg","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"init.luau"},
        "commands":{"come":{"summary":"Come","remote":true}}})");
    host->reload();
    ASSERT_EQ(host->commands().size(), 1);
    EXPECT_TRUE(host->commands()[0].remote);
}

namespace {

/// Handlers that wait when their run ends, and clean up after it.
const QByteArray CancelScript = R"(
local busy = false
local cleaned = {}
return {
    metadata = { commands = {
        typo = { summary = "Await a misspelled effect" },
        typofail = { summary = "Fail after a misspelled effect" },
        guarded = { summary = "Ask in a dialog, one at a time" },
        sleeper = { summary = "Sleep inside a pcall" },
        leaver = { summary = "Write after a sleep, inside a pcall" },
        overflow = { summary = "Write past the limit, then open a dialog" },
        sent = { summary = "Wait until the output has gone" },
        cleaned = { summary = "Print what cleaned up" },
    } },
    run_command = function(command, ev)
        if command == "typo" then
            local _, err = afterglow.await({ kind = "list_project" })
            return afterglow.output(ev, err.code .. "\n")
        elseif command == "typofail" then
            afterglow.await({ kind = "list_project" })
            error("after the typo")
        elseif command == "guarded" or command == "sleeper" or command == "leaver" then
            if busy then return afterglow.output(ev, "busy\n") end
            busy = true
            local ok, result, err = pcall(function()
                if command == "sleeper" then return afterglow.sleep(10000) end
                if command == "leaver" then
                    afterglow.sleep(20)
                    return afterglow.await(afterglow.output(ev, "x"))
                end
                return afterglow.await(afterglow.prompt(ev.pane, "Name"))
            end)
            busy = false
            table.insert(cleaned, command .. ":" .. tostring(ok) .. ":" .. tostring(err and err.code))
            -- The run has ended: this goes nowhere, and is no error.
            return { afterglow.output(ev, "too late\n"), afterglow.exit(ev, 9), afterglow.log("cleaned " .. command) }
        elseif command == "overflow" then
            return { afterglow.output(ev, string.rep("x", 100)), afterglow.prompt(ev.pane, "After the limit") }
        elseif command == "sent" then
            assert(afterglow.await(afterglow.output(ev, "a")))
            return afterglow.output(ev, "b")
        elseif command == "cleaned" then
            return afterglow.output(ev, table.concat(cleaned, ","))
        end
    end,
}
)";

} // namespace

TEST_F(ScriptHostTest, AnAwaitedEffectThatIsRefusedResumesItsRun)
{
    WRITE(user, "cancel.luau", CancelScript);
    host->setEnabled(true);
    // A misspelled kind is refused at once: its refusal resumes the handler,
    // as that run, so the run ends with it and not at the time limit.
    Client& typo = newClient();
    Client& fail = newClient();
    start("cancel", "typo", typo);
    ASSERT_TRUE(await([&] { return typo.status.has_value(); }, 2000));
    EXPECT_EQ(typo.status, 0);
    EXPECT_EQ(typo.out, "invalid_effect\n");
    start("cancel", "typofail", fail);
    ASSERT_TRUE(await([&] { return fail.status.has_value(); }, 2000));
    EXPECT_EQ(fail.status, 1);
    EXPECT_TRUE(fail.err.contains("after the typo")) << fail.err.toStdString();
}

TEST_F(ScriptHostTest, AnEndedRunResumesItsHandlerOnceWithCancelled)
{
    WRITE(user, "cancel.luau", CancelScript);
    host->setEnabled(true);
    Client& first = newClient();
    const quint64 handle = start("cancel", "guarded", first);
    ASSERT_TRUE(bool(pendingReply));
    const QString prompt = pendingRequest;
    host->cancelCommand(handle, 130);
    EXPECT_EQ(first.status, 130);
    // The run's dialog closed, and the handler's cleanup ran.
    ASSERT_EQ(closedDialogs.size(), 1);
    EXPECT_EQ(closedDialogs[0], prompt.left(prompt.lastIndexOf(':') + 1));
    EXPECT_FALSE(bool(pendingReply));
    pump();
    EXPECT_TRUE(std::any_of(rec.results.cbegin(), rec.results.cend(), [](const auto& r) {
        return r.first["kind"] == "log" && r.first["message"] == "cleaned guarded" && r.second.isEmpty();
    }));
    Client& report = newClient();
    start("cancel", "cleaned", report);
    EXPECT_EQ(report.out, "guarded:true:cancelled");
    // Its output and exit after the end went nowhere, with no error.
    EXPECT_EQ(first.out, "");
    for (const auto& [effect, error] : std::as_const(rec.results)) {
        if (effect["kind"] == "output" || effect["kind"] == "exit") {
            EXPECT_TRUE(error.isEmpty()) << error.toStdString();
        }
    }
    EXPECT_EQ(firstDiagnostic()["waiting"].toInt(), 0);
    // Its busy flag came back: the next run asks again.
    Client& second = newClient();
    start("cancel", "guarded", second);
    EXPECT_EQ(second.out, "");
    EXPECT_TRUE(bool(pendingReply));
}

TEST_F(ScriptHostTest, OutputToAnEndedRunFromAnotherHandlerFailsWithNoRun)
{
    WRITE(user, "late.luau", R"(
local saved
return {
    metadata = { commands = { hold = { summary = "Wait for a long sleep" } } },
    run_command = function(command, ev)
        saved = ev.run
        afterglow.sleep(10000)
    end,
    on_bell = function(ev)
        return afterglow.output(saved, "late\n")
    end,
}
)");
    host->setEnabled(true);
    Client& client = newClient();
    const quint64 handle = start("late", "hold", client);
    pump();
    ASSERT_FALSE(client.status);
    host->cancelCommand(handle, 130);
    EXPECT_EQ(client.status, 130);
    // The run's handler has not resumed yet. Only that handler writes to the
    // ended run with no error: another one is told that the run has ended.
    rec.results.clear();
    host->dispatch("on_bell", 7);
    ASSERT_EQ(rec.results.size(), 1);
    EXPECT_TRUE(rec.results[0].second.contains("has ended")) << rec.results[0].second.toStdString();
    EXPECT_TRUE(client.out.isEmpty());
    pump();
    EXPECT_FALSE(host->isDisabled("late"));
}

TEST_F(ScriptHostTest, ATimedOutRunCleansUpAndAClientThatLeftToo)
{
    WRITE(user, "cancel.luau", CancelScript);
    host->setEnabled(true);
    ScriptHost::CommandLimits limits;
    limits.timeoutMs = 50;
    host->setCommandLimits(limits);
    Client& sleeper = newClient();
    start("cancel", "sleeper", sleeper);
    ASSERT_TRUE(await([&] { return sleeper.status.has_value(); }));
    EXPECT_EQ(sleeper.status, ScriptHost::CommandTimedOut);
    // Its handler resumes with "cancelled" after the call that ended it, and
    // only then clears its busy flag.
    ASSERT_TRUE(await([&] { return firstDiagnostic()["waiting"].toInt() == 0; }));
    host->setCommandLimits({});
    // A client that leaves is found at the next write, which ends the run the same way.
    Client& leaver = newClient();
    start("cancel", "leaver", leaver);
    leaver.gone = true;
    ASSERT_TRUE(await([&] { return leaver.status.has_value(); }));
    EXPECT_EQ(leaver.status, 1);
    pump();
    Client& report = newClient();
    start("cancel", "cleaned", report);
    EXPECT_EQ(report.out, "sleeper:true:cancelled,leaver:true:cancelled");
    EXPECT_EQ(firstDiagnostic()["waiting"].toInt(), 0);
}

TEST_F(ScriptHostTest, NoEffectOfARunAppliesAfterItEndsInTheSameList)
{
    WRITE(user, "cancel.luau", CancelScript);
    host->setEnabled(true);
    ScriptHost::CommandLimits limits;
    limits.outputBytes = 50;
    host->setCommandLimits(limits);
    Client& overflow = newClient();
    start("cancel", "overflow", overflow);
    EXPECT_EQ(overflow.status, 1);
    // The dialog after the output that ended the run did not open.
    EXPECT_FALSE(bool(pendingReply));
    EXPECT_TRUE(rec.owners.isEmpty()); // a prompt would be an owned effect
    for (const auto& [effect, error] : std::as_const(rec.results)) EXPECT_NE(effect["kind"], "prompt");
}

TEST_F(ScriptHostTest, AnAwaitedOutputWaitsUntilItsTextHasGone)
{
    WRITE(user, "cancel.luau", CancelScript);
    host->setEnabled(true);
    Client& client = newClient();
    client.hold = true;
    start("cancel", "sent", client);
    EXPECT_EQ(client.out, "a");
    pump(12);
    // The client has not taken it: the handler still waits.
    EXPECT_FALSE(client.status);
    EXPECT_EQ(client.out, "a");
    ASSERT_EQ(client.held.size(), 1);
    client.hold = false;
    client.held.takeFirst()(true);
    ASSERT_TRUE(await([&] { return client.status.has_value(); }));
    EXPECT_EQ(client.out, "ab");
    EXPECT_EQ(client.status, 0);
}

TEST_F(ScriptHostTest, SceneControlsConfirmsASetWhoseWaitWasCancelled)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("scene-controls"));
    quint64 handle = 0;
    bool cancelNext = false; // cancel the run while it waits for its change
    bool refuseConfirm = false;
    applyHook = [&](const QVariantMap& e) -> ScriptHost::EffectResult {
        const QString kind = e["kind"].toString();
        if (kind == "shaders") {
            QVariantList parameters;
            for (const char* id : {"strength", "glow"})
                parameters << QVariantMap{{"id", id}, {"name", id}, {"type", "float"}, {"supported", true}};
            const QVariantMap shader{{"instance", "bloom-1"}, {"shader", "bloom"}, {"layer", "pane"},
                                     {"parameters", parameters}, {"values", QVariantMap{{"strength", 1.0}, {"glow", 0.0}}}};
            return {{}, {{"shaders", QVariantList{shader}}}, {}};
        }
        if (kind == "shader_parameters" && e["request_id"] == "confirm" && refuseConfirm)
            return {QStringLiteral("refused"), {}, QStringLiteral("operation_failed")};
        if (kind == "shader_parameters" && std::exchange(cancelNext, false)) host->cancelCommand(handle, 130);
        return {};
    };
    host->setEnabled(true);
    auto set = [&](const QString& name, const QString& value, bool cancel) {
        cancelNext = cancel;
        Client& client = newClient();
        handle = start("scene-controls", "set", client, {name, value});
        EXPECT_TRUE(await([&] { return client.status.has_value(); }));
        // The confirm's result comes after the run's end.
        pump();
        return client.status.value_or(-1);
    };
    auto lastValues = [&] {
        for (auto it = rec.applied.crbegin(); it != rec.applied.crend(); ++it)
            if ((*it)["kind"] == "shader_parameters") return (*it)["values"].toMap();
        return QVariantMap{};
    };
    // The change went out, but its result was lost: the confirm sends it again.
    EXPECT_EQ(set("strength", "1.5", true), 130);
    EXPECT_EQ(rec.applied.last()["request_id"], "confirm");
    // Confirmed, the layer keeps it.
    EXPECT_EQ(set("glow", "0.5", false), 0);
    EXPECT_EQ(lastValues(), (QVariantMap{{"strength", 1.5}, {"glow", 0.5}}));
    // A refused confirm does not keep the value.
    refuseConfirm = true;
    EXPECT_EQ(set("strength", "3", true), 130);
    EXPECT_EQ(set("glow", "0.7", false), 0);
    EXPECT_EQ(lastValues(), (QVariantMap{{"strength", 1.5}, {"glow", 0.7}}));
}

// Afterglow took the change, so the layer keeps it, also when the edited pane
// closed before the result came: the tab and its overrides are still there.
TEST_F(ScriptHostTest, SceneControlsKeepsAnAppliedEditWhosePaneClosed)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("scene-controls"));
    const QVariantMap strength{{"id", "strength"}, {"name", "Strength"}, {"type", "float"}, {"supported", true}};
    const QVariantMap radius{{"id", "radius"}, {"name", "Radius"}, {"type", "float"}, {"supported", true}};
    QList<QVariantMap> sent;
    applyHook = [&](const QVariantMap& e) -> ScriptHost::EffectResult {
        const auto kind = e.value("kind").toString();
        if (kind == "shaders")
            return {{}, {{"shaders", QVariantList{QVariantMap{{"instance", "i"}, {"shader", "bloom"}, {"layer", "pane"},
                {"parameters", QVariantList{strength, radius}}, {"values", QVariantMap{{"strength", 0.1}, {"radius", 1}}}}}}}, {}};
        if (kind == "shader_parameters") {
            sent.append(e.value("values").toMap());
            // The pane closes after the change applies: the snapshot of its result has no pane 7.
            if (e.value("request_id") == "applied") paneState.remove(7);
            return {};
        }
        if (kind != "select") return {};
        const auto title = e.value("title").toString();
        if (title == "Select effect") return {{}, {{"id", "i"}}, {}};
        if (title == "Scene controls") return {{}, {{"id", "edit"}}, {}};
        return {{}, {{"id", "strength"}}, {}};
    };
    host->setEnabled(true);
    ASSERT_TRUE(run("scene-controls.open", 7));
    pump(10);
    ASSERT_TRUE(bool(pendingReply));
    pendingReply({{}, {{"text", "0.5"}}, {}});
    pump(10);
    ASSERT_EQ(sent.size(), 1);
    EXPECT_EQ(sent.last(), QVariantMap({{"strength", 0.5}}));
    ASSERT_FALSE(paneState.contains(7));
    // Pane 8 is in the same tab: its set sends the layer with the earlier value.
    Client& client = newClient();
    start("scene-controls", "set", client, {"radius", "2"}, 8);
    ASSERT_TRUE(await([&] { return client.status.has_value(); }));
    EXPECT_EQ(client.status, 0) << client.err.toStdString();
    ASSERT_EQ(sent.size(), 2);
    EXPECT_EQ(sent.last(), QVariantMap({{"strength", 0.5}, {"radius", 2}}));
}

TEST_F(ScriptHostTest, ExamplePackageCommandsWorkFromTheShell)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("project-navigator"));
    WRITE(user, "project-navigator/config.json", R"({"roots":[],"fd":false,"zoxide":false})");
    ASSERT_NO_FATAL_FAILURE(installPackage("scene-controls"));
    ASSERT_NO_FATAL_FAILURE(installPackage("project-autosave"));
    applyHook = [](const QVariantMap& e) -> ScriptHost::EffectResult {
        const QString kind = e["kind"].toString();
        if (kind == "open_directory") return {{}, {{"window_id", 3}, {"cwd", e["cwd"]}, {"reused", false}}, {}};
        if (kind == "list_projects") return {{}, {{"names", QVariantList{"dev"}}}, {}};
        if (kind == "shaders") {
            const QVariantMap parameter{{"id", "strength"}, {"name", "Strength"}, {"type", "float"}, {"supported", true}};
            const QVariantMap shader{{"instance", "bloom-1"}, {"shader", "bloom"}, {"layer", "pane"},
                                     {"parameters", QVariantList{parameter}}, {"values", QVariantMap{{"strength", 1.0}}}};
            return {{}, {{"shaders", QVariantList{shader}}}, {}};
        }
        return {};
    };
    host->setEnabled(true);
    ASSERT_EQ(host->scriptIds().size(), 3);

    Client& open = newClient();
    start("project-navigator", "open", open, {"repo"});
    ASSERT_TRUE(await([&] { return open.status.has_value(); }));
    EXPECT_EQ(open.status, 0) << open.err.toStdString();
    const auto opened = std::find_if(rec.applied.cbegin(), rec.applied.cend(), [](const QVariantMap& e) { return e["kind"] == "open_directory"; });
    ASSERT_NE(opened, rec.applied.cend());
    EXPECT_EQ((*opened)["cwd"], "/work/repo");
    Client& list = newClient();
    start("project-navigator", "list", list);
    ASSERT_TRUE(await([&] { return list.status.has_value(); }));
    EXPECT_EQ(list.out, "recent\t/work/repo\nlayout\tdev\n");
    Client& usage = newClient();
    start("project-navigator", "open", usage);
    ASSERT_TRUE(await([&] { return usage.status.has_value(); }));
    EXPECT_EQ(usage.status, 2);
    EXPECT_TRUE(usage.err.startsWith("usage:"));

    Client& get = newClient();
    Client& set = newClient();
    Client& wrong = newClient();
    start("scene-controls", "get", get);
    ASSERT_TRUE(await([&] { return get.status.has_value(); }));
    EXPECT_EQ(get.out, "bloom-1\tstrength\t1\n");
    rec.applied.clear();
    start("scene-controls", "set", set, {"strength", "1.5"});
    ASSERT_TRUE(await([&] { return set.status.has_value(); }));
    EXPECT_EQ(set.status, 0) << set.err.toStdString();
    EXPECT_EQ(set.out, "bloom-1\tstrength\t1.5\n");
    ASSERT_EQ(rec.applied.size(), 2);
    EXPECT_EQ(rec.applied[1]["kind"], "shader_parameters");
    EXPECT_EQ(rec.applied[1]["scope"], "tab");
    EXPECT_EQ(rec.applied[1]["values"].toMap()["strength"].toDouble(), 1.5);
    start("scene-controls", "set", wrong, {"strength", "lots"});
    ASSERT_TRUE(await([&] { return wrong.status.has_value(); }));
    EXPECT_EQ(wrong.status, 2);

    Client& save = newClient();
    rec.applied.clear();
    start("project-autosave", "save", save, {"demo"});
    ASSERT_TRUE(await([&] { return save.status.has_value(); }));
    EXPECT_EQ(save.out, "Saved demo\n");
    ASSERT_EQ(rec.applied.size(), 1);
    EXPECT_EQ(rec.applied[0]["kind"], "save_project");
    EXPECT_EQ(rec.applied[0]["name"], "demo");
    // With no NAME, the window's project: this window has none.
    Client& unnamed = newClient();
    start("project-autosave", "save", unnamed);
    ASSERT_TRUE(await([&] { return unnamed.status.has_value(); }));
    EXPECT_EQ(unnamed.status, 1);
    EXPECT_TRUE(unnamed.err.contains("Name the project")) << unnamed.err.toStdString();

    // open DIR LAYOUT opens the directory with that layout.
    Client& layout = newClient();
    rec.applied.clear();
    start("project-navigator", "open", layout, {"other", "dev"});
    ASSERT_TRUE(await([&] { return layout.status.has_value(); }));
    EXPECT_EQ(layout.status, 0) << layout.err.toStdString();
    const auto withLayout = std::find_if(rec.applied.cbegin(), rec.applied.cend(), [](const QVariantMap& e) { return e["kind"] == "open_directory"; });
    ASSERT_NE(withLayout, rec.applied.cend());
    EXPECT_EQ((*withLayout)["cwd"], "/work/other");
    EXPECT_EQ((*withLayout)["layout"], "dev");

    // get PARAM prints that parameter, and names a missing one.
    Client& one = newClient();
    Client& missing = newClient();
    start("scene-controls", "get", one, {"strength"});
    ASSERT_TRUE(await([&] { return one.status.has_value(); }));
    EXPECT_EQ(one.out, "bloom-1\tstrength\t1\n");
    start("scene-controls", "get", missing, {"nope"});
    ASSERT_TRUE(await([&] { return missing.status.has_value(); }));
    EXPECT_NE(missing.status, 0);
    EXPECT_TRUE(missing.err.contains("No parameter nope")) << missing.err.toStdString();
}

TEST_F(ScriptHostTest, EffectChecksRefuseBadShapesBeforeTheHost)
{
    WRITE(user, "shapes.luau", R"(
return { on_bell = function(ev)
    return {
        afterglow.select(ev.pane, "Nothing", {}),
        afterglow.read_text(ev.pane, "screen", 0),
        afterglow.read_region(ev.pane, "1", 2 * 1024 * 1024),
        afterglow.overview_move(ev.pane, 1.5),
        afterglow.overview_move(ev.pane, 0, 7.5),
        afterglow.pulse(ev.pane, "flash", {color = 4.5}),
        afterglow.move_pane(ev.pane),
        afterglow.move_pane(ev.pane, 8, "rigth"),
        afterglow.move_pane(ev.pane, 8, "new-tab"),
        afterglow.new_tab(ev.pane, "/tmp\0evil"),
        {nil, afterglow.copy("after a hole")},
        afterglow.split(ev.pane), afterglow.split(ev.pane), afterglow.split(ev.pane),
    }
end }
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    QStringList kinds;
    for (const auto& e : rec.applied) kinds << e["kind"].toString();
    // The refused moves used no structural slot, so all three splits apply.
    EXPECT_EQ(kinds, QStringList({"split", "split", "split"}));
    ASSERT_EQ(rec.results.size(), 14);
    // Each for its own rule: a refusal for another reason would pass a check
    // of a non-empty error.
    const char* const reasons[] = {"A selector needs at least one item", "max_bytes must be a whole number from 1 to",
        "max_bytes must be a whole number from 1 to", "overview_move needs a pane or a whole step",
        "overview_move needs a pane or a whole step", "pulse needs 50 to 10000 ms and a palette color",
        // The helper's default placement is right, which needs a target.
        "left, right, up and down need a target pane",
        "placement must be left, right, up, down, new-tab or new-window",
        "new-tab and new-window take none", "cwd must be a string without NUL"};
    for (int i = 0; i < 10; ++i)
        EXPECT_TRUE(rec.results[i].second.contains(QLatin1String(reasons[i]))) << i << ": " << rec.results[i].second.toStdString();
    EXPECT_EQ(rec.results[10].second, "unknown effect \"\"");
}

TEST_F(ScriptHostTest, NilEntriesInReturnedListsAreSkipped)
{
    WRITE(user, "holes.luau", R"(
return { on_bell = function(ev)
    local failed = false
    return {failed and afterglow.pulse(ev.pane, "flash") or nil, afterglow.copy("kept")}
end }
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_EQ(rec.applied.size(), 1);
    EXPECT_EQ(rec.applied[0]["text"], "kept");
    ASSERT_FALSE(host->diagnostics().isEmpty());
    EXPECT_EQ(firstDiagnostic()["effect_errors"].toULongLong(), 0u);
}

TEST_F(ScriptHostTest, ReleasingEffectsWorkWhenTheBudgetIsSpent)
{
    WRITE(user, "spent.luau", QStringLiteral(R"(
return {
    metadata = {commands = {spend = {summary = "Spend the budget, then write and exit"}}},
    run_command = function(command, ev)
        local out = {}
        for i = 1, %1 do out[i] = afterglow.set_title(ev.pane, "t") end
        table.insert(out, afterglow.output(ev, "out"))
        table.insert(out, afterglow.output_error(ev, "err"))
        table.insert(out, afterglow.exit(ev, 3))
        return out
    end,
    on_bell = function(ev)
        local out = {}
        for i = 1, %1 do out[i] = afterglow.set_title(ev.pane, "t") end
        table.insert(out, afterglow.cancel("nothing"))
        table.insert(out, afterglow.undock(ev.pane, "bar"))
        table.insert(out, afterglow.end_key_mode())
        return out
    end,
}
)").arg(ScriptHost::MaxEffectsPerSecond).toUtf8());
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_EQ(rec.results.size(), ScriptHost::MaxEffectsPerSecond + 3);
    EXPECT_TRUE(rec.results[ScriptHost::MaxEffectsPerSecond].second.isEmpty()); // cancel
    EXPECT_TRUE(rec.results[ScriptHost::MaxEffectsPerSecond + 1].second.isEmpty()); // undock
    EXPECT_TRUE(rec.results.last().second.isEmpty()); // end_key_mode
    EXPECT_EQ(rec.applied.last()["kind"], "end_key_mode");
    // The output of a run and its exit are not counted either.
    Client& client = newClient();
    start("spent", "spend", client);
    ASSERT_TRUE(await([&] { return client.status.has_value(); }));
    EXPECT_EQ(client.out, "out");
    EXPECT_EQ(client.err, "err");
    EXPECT_EQ(client.status, 3);
}

TEST_F(ScriptHostTest, SleepsDoNotSpendTheEffectBudget)
{
    WRITE(user, "poll.luau", QStringLiteral(R"(
return { on_bell = function(ev)
    local first, second = {}, {}
    for i = 1, 16 do first[i] = afterglow.set_title(ev.pane, "t") end
    for i = 1, %1 do second[i] = afterglow.set_title(ev.pane, "t") end
    afterglow.await_all(first)
    afterglow.await_all(second) -- the effects of this second are spent
    local _, err = afterglow.sleep(0)
    return afterglow.log(err and err.code or "slept")
end }
)").arg(ScriptHost::MaxEffectsPerSecond - 16).toUtf8());
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_TRUE(await([&] {
        return std::any_of(rec.results.begin(), rec.results.end(), [](const auto& r) { return r.first["kind"] == "log"; });
    }));
    for (const auto& r : rec.results) {
        if (r.first["kind"] == "sleep") {
            EXPECT_TRUE(r.second.isEmpty()) << r.second.toStdString();
        }
        if (r.first["kind"] == "log") {
            EXPECT_EQ(r.first["message"], "slept");
        }
    }
}

TEST_F(ScriptHostTest, BrokenSourcesDoNotRunAgainOnUnrelatedRescans)
{
    WRITE(user, "spin.luau", "while true do end\nreturn {}");
    QElapsedTimer clock;
    clock.start();
    host->setEnabled(true); // the module load times out once
    const qint64 timedOut = clock.elapsed();
    WRITE(user, "other.luau", "return {}");
    clock.restart();
    host->reload();
    // Loading spin.luau again would take its time limit again.
    EXPECT_LT(clock.elapsed(), timedOut / 2) << timedOut;
    EXPECT_TRUE(host->scriptIds().contains("other"));
    bool failed = false;
    for (const auto& entry : host->diagnostics()) {
        const auto map = entry.toMap();
        if (map["path"].toString().endsWith("spin.luau")) failed = map["status"] == "failed" && !map["error"].toString().isEmpty();
    }
    EXPECT_TRUE(failed);
}

TEST_F(ScriptHostTest, ScriptsWithDifferentIdsLoadDespiteTheSameFileName)
{
    WRITE(system, "notify.luau", "return {metadata = {id = 'notify'}}");
    WRITE(user, "notify.luau", "return {metadata = {id = 'my-notify'}}");
    WRITE(user, "same.luau", "return {metadata = {id = 'pkg'}}");
    QDir().mkpath(user.filePath("pkg"));
    WRITE(user, "pkg/package.json", R"({"id":"pkg","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"init.luau"}})");
    WRITE(user, "pkg/init.luau", "return {}");
    host->setEnabled(true);
    auto ids = host->scriptIds();
    ids.sort();
    EXPECT_EQ(ids, QStringList({"my-notify", "notify", "pkg"}));
    // The file that lost its id to the package is reported, not silently skipped.
    bool superseded = false;
    for (const auto& entry : host->diagnostics()) {
        const auto map = entry.toMap();
        if (map["path"].toString().endsWith("same.luau")) superseded = map["status"] == "superseded";
    }
    EXPECT_TRUE(superseded);
}

TEST_F(ScriptHostTest, SymlinkedPackageDirectoriesLoad)
{
    QTemporaryDir checkout;
    ASSERT_TRUE(checkout.isValid());
    WRITE(checkout, "package.json", R"({"id":"linked","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"init.luau"}})");
    WRITE(checkout, "init.luau", "return {on_bell = function() return afterglow.copy('linked') end}");
    ASSERT_TRUE(QFile::link(checkout.path(), user.filePath("linked")));
    host->setEnabled(true);
    EXPECT_EQ(host->scriptIds(), QStringList{"linked"});
}

TEST_F(ScriptHostTest, PackageIdentityRulesAreReported)
{
    QDir().mkpath(user.filePath("wrong-dir"));
    WRITE(user, "wrong-dir/package.json", R"({"id":"other","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"init.luau"}})");
    WRITE(user, "wrong-dir/init.luau", "return {}");
    QDir().mkpath(user.filePath("entry"));
    WRITE(user, "entry/package.json", R"({"id":"entry","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"init.luau"}})");
    WRITE(user, "entry/init.luau", "return {metadata = {id = 'different'}}");
    host->setEnabled(true);
    EXPECT_TRUE(host->scriptIds().isEmpty());
    QStringList errors;
    for (const auto& entry : host->diagnostics()) errors << entry.toMap()["error"].toString();
    EXPECT_TRUE(errors.contains("Package id must match its directory name")) << errors.join(", ").toStdString();
    EXPECT_TRUE(errors.contains("Entry metadata id differs from manifest")) << errors.join(", ").toStdString();
}

TEST_F(ScriptHostTest, PackageStateRefusalsAndMigrationAreReported)
{
    QDir().mkpath(user.filePath("stored"));
    WRITE(user, "stored/package.json", R"({"id":"stored","version":"1.0.0","api_version":1,"state_version":2,"entry":"main","modules":{"main":"init.luau"}})");
    WRITE(user, "stored/init.luau", "return {}");
    ScriptState::Snapshot saved;
    // Newer state than the package: refused.
    ASSERT_TRUE(ScriptState(stateDir.path()).write("stored", 3, 0, {{"x", 1}}, saved).isEmpty());
    host->setEnabled(true);
    EXPECT_TRUE(host->scriptIds().isEmpty());
    QStringList errors;
    for (const auto& entry : host->diagnostics()) errors << entry.toMap()["error"].toString();
    EXPECT_TRUE(errors.contains("Stored state is newer than this package"));
    // Older state and no migrate function: refused.
    host->setEnabled(false);
    QFile::remove(stateDir.filePath("stored.json"));
    ASSERT_TRUE(ScriptState(stateDir.path()).write("stored", 1, 0, {{"x", 1}}, saved).isEmpty());
    host->setEnabled(true);
    errors.clear();
    for (const auto& entry : host->diagnostics()) errors << entry.toMap()["error"].toString();
    EXPECT_TRUE(errors.contains("State migration function is missing"));
    // A migration that returns no object says so.
    WRITE(user, "stored/init.luau", "return {migrate = function() return 5 end}");
    host->reload();
    errors.clear();
    for (const auto& entry : host->diagnostics()) errors << entry.toMap()["error"].toString();
    EXPECT_TRUE(errors.contains("Migration must return an object table"));
}

TEST_F(ScriptHostTest, PackageWithRefusedStateRetriesWhenTheStateFileChangesAfterARescan)
{
    QDir().mkpath(user.filePath("stored"));
    WRITE(user, "stored/package.json", R"({"id":"stored","version":"1.0.0","api_version":1,"state_version":2,"entry":"main","modules":{"main":"init.luau"}})");
    WRITE(user, "stored/init.luau", "return {}");
    ScriptState::Snapshot saved;
    ASSERT_TRUE(ScriptState(stateDir.path()).write("stored", 3, 0, {{"x", 1}}, saved).isEmpty());
    host->setEnabled(true);
    ASSERT_TRUE(host->scriptIds().isEmpty());
    // An unrelated rescan finds the package unchanged and does not load it again.
    WRITE(user, "other.luau", "return {}");
    host->reload();
    ASSERT_EQ(host->scriptIds(), QStringList{"other"});
    // The rescans that the watches start for other.luau, past their 50 ms
    // debounce, do not load it either.
    await([] { return false; }, 300);
    ASSERT_EQ(host->scriptIds(), QStringList{"other"});
    // The state file is still watched: removing it retries the package.
    ASSERT_TRUE(QFile::remove(stateDir.filePath("stored.json")));
    EXPECT_TRUE(await([&] { return host->scriptIds().contains("stored"); }));
}

TEST_F(ScriptHostTest, SupersededPackageNeverLoadsAgainstTheWinnersState)
{
    // Both copies share dup.json. The winner writes it at a newer version
    // than the loser can read, so loading the loser would fail.
    auto install = [&](const QTemporaryDir& dir, int stateVersion, const QByteArray& source) {
        QDir().mkpath(dir.filePath("dup"));
        WRITE(dir, "dup/package.json", QString(R"({"id":"dup","version":"1.0.0","api_version":1,"state_version":%1,"entry":"main","modules":{"main":"init.luau"}})")
            .arg(stateVersion).toUtf8());
        WRITE(dir, "dup/init.luau", source);
    };
    ASSERT_NO_FATAL_FAILURE(install(system, 1, "return {on_load=function() return afterglow.copy('loser loaded') end}"));
    ASSERT_NO_FATAL_FAILURE(install(user, 2, "return {on_load=function(ev) return afterglow.state_write({n=1},ev.storage.revision) end}"));
    host->setEnabled(true);
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    ScriptState::Snapshot stored;
    ASSERT_TRUE(ScriptState(stateDir.path()).read("dup", stored).isEmpty());
    ASSERT_EQ(stored.version, 2);
    host->reload();
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    EXPECT_EQ(host->scriptIds(), QStringList{"dup"});
    QStringList statuses;
    for (const auto& entry : host->diagnostics()) statuses << entry.toMap()["status"].toString();
    EXPECT_EQ(statuses.count("superseded"), 1);
    EXPECT_FALSE(statuses.contains("failed")) << statuses.join(",").toStdString();
    for (const auto& e : rec.applied) EXPECT_NE(e.value("text"), "loser loaded");
}

TEST_F(ScriptHostTest, NavigatorMigratesSchemaOneHistory)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("project-navigator"));
    WRITE(user, "project-navigator/config.json", R"({"roots":[],"fd":false,"zoxide":false})");
    QFile state(stateDir.filePath("project-navigator.json"));
    ASSERT_TRUE(state.open(QIODevice::WriteOnly));
    const QByteArray schemaOne = R"({"format":1,"version":1,"revision":1,"value":{"favorites":["/a"],"recent":["/b"]}})";
    ASSERT_EQ(state.write(schemaOne), schemaOne.size());
    ASSERT_TRUE(state.flush());
    state.close();
    host->setEnabled(true);
    ASSERT_EQ(host->scriptIds(), QStringList{"project-navigator"});
    ScriptState::Snapshot stored;
    ASSERT_TRUE(ScriptState(stateDir.path()).read("project-navigator", stored).isEmpty());
    EXPECT_EQ(stored.version, 2);
    EXPECT_TRUE(stored.value["favorites"].toMap()["/a"].toBool());
    const auto recent = stored.value["recent"].toList();
    ASSERT_FALSE(recent.isEmpty());
    EXPECT_EQ(recent[0].toMap()["kind"], "directory");
    EXPECT_EQ(recent[0].toMap()["value"], "/b");
}

TEST_F(ScriptHostTest, UnloadEffectsOtherThanStateAndLogAreRefused)
{
    WRITE(user, "unload.luau", "return {on_unload = function() return afterglow.copy('ignored') end}");
    host->setEnabled(true);
    WRITE(user, "unload.luau", "return {}");
    host->reload();
    host->setEnabled(false);
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    EXPECT_TRUE(rec.applied.isEmpty());
    EXPECT_EQ(cleaned.size(), 2);
}

TEST_F(ScriptHostTest, UnloadRunsOnRetireButNotForADisabledScript)
{
    // An on_unload that raises an error reaches the failed callback: that shows it ran.
    WRITE(user, "observed.luau", "return {on_unload = function() error('unloaded') end}");
    WRITE(user, "broken.luau", R"(return {metadata={commands={go={summary="go",palette="go"}}},
        run_command=function() error("boom") end, on_unload=function() error("unloaded") end})");
    host->setEnabled(true);
    for (int i = 0; i < ScriptHost::MaxFailures && !host->isDisabled("broken"); ++i) ASSERT_TRUE(run("broken.go", 7));
    ASSERT_TRUE(host->isDisabled("broken"));
    const auto failures = failedOwners.size();
    WRITE(user, "observed.luau", "return {}");
    host->reload();
    EXPECT_EQ(failedOwners.size(), failures + 1); // the replaced instance ran on_unload
    host->setEnabled(false);
    EXPECT_EQ(failedOwners.size(), failures + 1); // the disabled script did not
}

TEST_F(ScriptHostTest, ReadFileHelperResultsReachOnResultByTaskName)
{
    WRITE(files, "note.txt", "hello");
    WRITE(user, "reader.luau", QStringLiteral(R"(
return {
    on_bell = function() return afterglow.read_file("note", "%1") end,
    on_result = function(ev) return afterglow.copy(ev.request_id .. ":" .. ev.data.text) end,
}
)").arg(files.filePath("note.txt")).toUtf8());
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_TRUE(await([&] { return !rec.applied.isEmpty(); }));
    EXPECT_EQ(rec.applied[0]["text"], "note:hello");
}

TEST_F(ScriptHostTest, ExamplePackagesLoad)
{
    const QDir examples(QStringLiteral(AFTERGLOW_EXAMPLE_PACKAGES));
    const auto names = examples.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const auto& name : names) ASSERT_NO_FATAL_FAILURE(installPackage(name));
    host->setEnabled(true);
    EXPECT_EQ(host->scriptIds().size(), names.size());
    for (const auto& entry : host->diagnostics()) {
        const auto status = entry.toMap()["status"].toString();
        EXPECT_TRUE(status == "active") << entry.toMap()["path"].toString().toStdString() << ": "
                                        << entry.toMap()["error"].toString().toStdString();
    }
}

TEST_F(ScriptHostTest, ExampleScriptsCreatePulseAndNotify)
{
    for (const char* name : {"create-and-title.luau", "pane-pulses.luau", "notify-slow-commands.luau"})
        ASSERT_TRUE(QFile::copy(QStringLiteral(AFTERGLOW_EXAMPLE_SCRIPTS) + "/" + name, user.filePath(name)));
    applyHook = [](const QVariantMap& e) -> ScriptHost::EffectResult {
        if (e["kind"] == QLatin1String("split")) return {{}, {{"pane", QVariantMap{{"id", 8}}}, {"pane_id", 8}}, {}};
        return {};
    };
    host->setEnabled(true);
    ASSERT_TRUE(run("create-and-title.split", 7));
    ASSERT_TRUE(await([&] { return rec.applied.size() == 2; }));
    EXPECT_EQ(rec.applied[0]["kind"], "split");
    EXPECT_DOUBLE_EQ(rec.applied[0]["options"].toMap()["size"].toDouble(), 0.35);
    EXPECT_EQ(rec.applied[1]["kind"], "set_title");
    EXPECT_EQ(rec.applied[1]["pane"].toUInt(), 8u);
    rec.applied.clear();
    host->dispatch("on_command_finished", 9, {{"command", "make"}, {"exit_code", 2}, {"duration_ms", 12000}});
    QStringList kinds;
    for (const auto& e : rec.applied) kinds << e["kind"].toString();
    kinds.sort();
    EXPECT_EQ(kinds, QStringList({"notify", "pulse"}));
    for (const auto& e : rec.applied) {
        if (e["kind"] == "pulse") { EXPECT_EQ(e["effect"], "2"); EXPECT_EQ(e["color"].toInt(), 1); }
        if (e["kind"] == "notify") { EXPECT_EQ(e["pane"].toUInt(), 9u); EXPECT_EQ(e["body"], "failed (2) after 12 s"); }
    }
    rec.applied.clear();
    host->dispatch("on_command_finished", 9, {{"command", "sleep"}, {"exit_code", 130}, {"duration_ms", 100}});
    EXPECT_TRUE(rec.applied.isEmpty()); // Ctrl+C is not a failure
}

TEST_F(ScriptHostTest, TabLineKeepsGitFromRunningRepositoryCommands)
{
    const QString gitProgram = QStandardPaths::findExecutable("git");
    if (gitProgram.isEmpty()) GTEST_SKIP() << "git is not installed";
    // Only the fixture's config: the developer's global core.fsmonitor=true, for
    // one, would start a daemon on the repository that outlives the test. The
    // package's git inherits these too. A test run from a git hook, or under
    // `git -c`, has variables that point git at another repository or add
    // configuration: they are unset.
    const std::pair<const char*, std::optional<QByteArray>> isolated[] = {
        {"GIT_CONFIG_NOSYSTEM", "1"}, {"GIT_CONFIG_GLOBAL", "/dev/null"}, {"GIT_DIR", std::nullopt},
        {"GIT_WORK_TREE", std::nullopt}, {"GIT_INDEX_FILE", std::nullopt}, {"GIT_COMMON_DIR", std::nullopt},
        {"GIT_OBJECT_DIRECTORY", std::nullopt}, {"GIT_ALTERNATE_OBJECT_DIRECTORIES", std::nullopt},
        {"GIT_CONFIG_PARAMETERS", std::nullopt}, {"GIT_CONFIG_COUNT", std::nullopt}};
    std::vector<std::optional<QByteArray>> saved;
    for (const auto& [name, value] : isolated) {
        saved.push_back(qEnvironmentVariableIsSet(name) ? std::optional(qgetenv(name)) : std::nullopt);
        if (value) qputenv(name, *value);
        else qunsetenv(name);
    }
    const auto restore = qScopeGuard([&] {
        for (std::size_t i = 0; i < saved.size(); ++i) {
            if (saved[i]) qputenv(isolated[i].first, *saved[i]);
            else qunsetenv(isolated[i].first);
        }
    });
    // A repository whose own config names a clean filter for every file, with an
    // index entry whose timestamp no longer matches: comparing the work tree runs it.
    const QString repository = files.filePath("repository");
    const QString marker = files.filePath("filter-ran");
    auto git = [&](const QStringList& args) {
        QProcess p;
        p.start(gitProgram, QStringList{"-C", repository} + args);
        return p.waitForFinished(5000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    };
    ASSERT_TRUE(QDir().mkpath(repository));
    ASSERT_TRUE(git({"init", "-q", "-b", "trunk"}));
    WRITE(files, "repository/.gitattributes", "* filter=evil\n");
    WRITE(files, "repository/file", "text\n");
    ASSERT_TRUE(git({"add", ".gitattributes", "file"}));
    ASSERT_TRUE(git({"config", "filter.evil.clean", "touch '" + marker + "'; cat"}));
    {
        QFile f(files.filePath("repository/file"));
        ASSERT_TRUE(f.open(QIODevice::ReadWrite));
        ASSERT_TRUE(f.setFileTime(QDateTime::fromSecsSinceEpoch(1577836800), QFileDevice::FileModificationTime));
    }

    ASSERT_NO_FATAL_FAILURE(installPackage("tab-line"));
    WRITE(user, "tab-line/config.json", R"({"git":true,"segments":["git"]})");
    paneState[7]["cwd"] = repository;
    paneState[7]["context"] = "";
    windows = {QVariantMap{{"id", 1}, {"project", ""}, {"active_pane", 7}}};
    tabs = {QVariantMap{{"id", 10}, {"window", 1}, {"index", 0}, {"title", "fish"}, {"active", true}, {"active_pane", 7}}};
    auto shows = [&](const QString& text) {
        QElapsedTimer clock;
        clock.start();
        // A refresh tick each second, beside the package's own 1 s timer, so
        // about two a second: the package can drop a Git job after about 2 s,
        // near the query's 2 s timeout. These local queries finish long before.
        qint64 tick = -1000;
        while (clock.elapsed() < 8000) {
            if (clock.elapsed() - tick >= 1000) {
                tick = clock.elapsed();
                host->dispatch("on_timer", 0, {{"name", "refresh"}});
            }
            QCoreApplication::processEvents();
            QThread::msleep(5);
            if (!rec.applied.isEmpty() && dockTexts(rec.applied.last()).contains(text)) return true;
        }
        return false;
    };
    host->setEnabled(true);
    ASSERT_TRUE(shows("trunk"));
    EXPECT_FALSE(QFile::exists(marker));
    for (const auto& r : rec.results) {
        if (r.first["kind"] != "exec") continue;
        const auto command = r.first["command"].toStringList();
        EXPECT_EQ(command.mid(0, 3), QStringList({"git", "-c", "core.fsmonitor=false"}));
        EXPECT_FALSE(command.contains("status"));
    }

    // The change mark is opt-in, because git status compares the work tree.
    // That it runs the filter shows that the repository above would have run it.
    WRITE(user, "tab-line/config.json", R"({"git":true,"git_changes":true,"segments":["git"]})");
    host->reload();
    ASSERT_TRUE(shows("trunk *"));
    EXPECT_TRUE(QFile::exists(marker));
}

TEST_F(ScriptHostTest, NavigatorOpensAProjectWithoutAskingForALayout)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("project-navigator"));
    WRITE(user, "project-navigator/config.json", R"({"roots":[],"fd":false,"zoxide":false})");
    applyHook = [](const QVariantMap& e) -> ScriptHost::EffectResult {
        const auto kind = e.value("kind").toString();
        if (kind == "list_projects") return {{}, {{"names", QStringList{"dev"}}}, {}};
        if (kind == "select") {
            if (e.value("title").toString().startsWith("Layout for")) return {{}, {{"id", "shell"}}, {}};
            for (const auto& item : e.value("items").toList())
                if (item.toMap().value("title").toString() == "Layout: dev") return {{}, {{"id", item.toMap().value("id")}}, {}};
        }
        return {};
    };
    paneState[7]["cwd"] = "/tmp";
    host->setEnabled(true);
    ASSERT_TRUE(run("project-navigator.pick", 7));
    ASSERT_TRUE(await([&] {
        return std::any_of(rec.applied.begin(), rec.applied.end(), [](const auto& e) { return e.value("kind") == "open_project"; });
    }));
    for (const auto& e : rec.applied) {
        if (e.value("kind") == "open_project") {
            EXPECT_EQ(e.value("name"), "dev");
        }
        if (e.value("kind") == "select") {
            EXPECT_FALSE(e.value("title").toString().startsWith("Layout for"));
        }
    }
}

TEST_F(ScriptHostTest, NavigatorReusesARunningDirectoryWithoutAskingForALayout)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("project-navigator"));
    WRITE(user, "project-navigator/config.json", R"({"roots":[],"fd":false,"zoxide":false})");
    windows = {QVariantMap{{"id", 1}, {"project_directory", "/tmp/nav-running"}, {"active_pane", 7}}};
    applyHook = [](const QVariantMap& e) -> ScriptHost::EffectResult {
        const auto kind = e.value("kind").toString();
        if (kind == "list_projects") return {{}, {{"names", QStringList{"dev"}}}, {}};
        if (kind == "select") {
            if (e.value("title").toString().startsWith("Layout for")) return {{}, {{"id", "shell"}}, {}};
            for (const auto& item : e.value("items").toList())
                if (item.toMap().value("title").toString() == "[running] /tmp/nav-running") return {{}, {{"id", item.toMap().value("id")}}, {}};
        }
        if (kind == "open_directory") return {{}, {{"window_id", 1}, {"cwd", "/tmp/nav-running"}, {"reused", true}}, {}};
        return {};
    };
    paneState[7]["cwd"] = "/tmp";
    host->setEnabled(true);
    ASSERT_TRUE(run("project-navigator.pick", 7));
    ASSERT_TRUE(await([&] {
        return std::any_of(rec.applied.begin(), rec.applied.end(), [](const auto& e) { return e.value("kind") == "open_directory"; });
    }));
    for (const auto& e : rec.applied) {
        if (e.value("kind") == "open_directory") {
            EXPECT_EQ(e.value("cwd"), "/tmp/nav-running");
            EXPECT_EQ(e.value("layout"), "");
        }
        if (e.value("kind") == "select") {
            EXPECT_FALSE(e.value("title").toString().startsWith("Layout for"));
        }
    }
}

TEST_F(ScriptHostTest, SceneControlsEscapeShowsNoNotice)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("scene-controls"));
    applyHook = [](const QVariantMap& e) -> ScriptHost::EffectResult {
        if (e["kind"] == QLatin1String("shaders"))
            return {{}, {{"shaders", QVariantList{QVariantMap{{"instance", "i"}, {"shader", "bloom"}, {"layer", "pane"},
                {"parameters", QVariantList{QVariantMap{{"id", "strength"}, {"name", "Strength"}, {"type", "float"}, {"supported", true}}}},
                {"values", QVariantMap{{"strength", 0.5}}}}}}}, {}};
        if (e["kind"] == QLatin1String("select")) return {"Dialog canceled", {}, "cancelled"};
        return {};
    };
    host->setEnabled(true);
    ASSERT_TRUE(run("scene-controls.open", 7));
    for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();
    // The dialog opened, and its Escape showed nothing: no notify, and no
    // notice of a failed run.
    EXPECT_TRUE(std::any_of(rec.applied.cbegin(), rec.applied.cend(), [](const QVariantMap& e) { return e["kind"] == "select"; }));
    for (const auto& e : rec.applied) EXPECT_NE(e["kind"], "notify");
    EXPECT_TRUE(notices.isEmpty()) << (notices.isEmpty() ? std::string() : notices.first().body.toStdString());
}

TEST_F(ScriptHostTest, SceneControlsKeepsOnlyAcceptedValuesInItsLayer)
{
    ASSERT_NO_FATAL_FAILURE(installPackage("scene-controls"));
    // The effective values hold another script's override (strength 0.1). The
    // host refuses a strength above 1, as validateParameters does for a range.
    const QVariantMap strength{{"id", "strength"}, {"name", "Strength"}, {"type", "float"}, {"supported", true}};
    const QVariantMap radius{{"id", "radius"}, {"name", "Radius"}, {"type", "float"}, {"supported", true}};
    QString choice; ///< the parameter to edit next, or "@preset"
    QList<QVariantMap> sent;
    applyHook = [&](const QVariantMap& e) -> ScriptHost::EffectResult {
        const auto kind = e.value("kind").toString();
        if (kind == "shaders")
            return {{}, {{"shaders", QVariantList{QVariantMap{{"instance", "i"}, {"shader", "bloom"}, {"layer", "pane"},
                {"parameters", QVariantList{strength, radius}}, {"values", QVariantMap{{"strength", 0.1}, {"radius", 1}}}}}}}, {}};
        if (kind == "shader_presets") return {{}, {{"presets", QVariantList{QVariantMap{{"id", "soft"}, {"name", "Soft"}}}}}, {}};
        if (kind == "shader_preset") return {{}, {{"values", QVariantMap{{"strength", 0.8}, {"radius", 3}}}}, {}};
        if (kind == "shader_parameters") {
            sent.append(e.value("values").toMap());
            if (e.value("values").toMap().value("strength", 0).toDouble() > 1)
                return {"Shader parameter outside its declared range", {}, "operation_failed"};
            return {};
        }
        if (kind != "select") return {};
        const auto title = e.value("title").toString();
        if (title == "Select effect") return {{}, {{"id", "i"}}, {}};
        if (title == "Scene controls") return {{}, {{"id", choice == "@preset" ? "presets" : "edit"}}, {}};
        if (title == "Choose preset") return {{}, {{"id", "soft"}}, {}};
        return {{}, {{"id", choice}}, {}};
    };
    host->setEnabled(true);
    auto edit = [&](const QString& id, const QString& value) {
        choice = id;
        pendingReply = {};
        ASSERT_TRUE(run("scene-controls.open", 7));
        pump(10);
        ASSERT_TRUE(bool(pendingReply));
        pendingReply({{}, {{"text", value}}, {}});
        pump(10);
    };
    ASSERT_NO_FATAL_FAILURE(edit("strength", "9"));
    ASSERT_EQ(sent.size(), 1);
    EXPECT_EQ(sent.last(), QVariantMap({{"strength", 9}}));
    // The refused strength is not sent again with the next edit.
    ASSERT_NO_FATAL_FAILURE(edit("radius", "2"));
    ASSERT_EQ(sent.size(), 2);
    EXPECT_EQ(sent.last(), QVariantMap({{"radius", 2}}));
    ASSERT_NO_FATAL_FAILURE(edit("strength", "0.5"));
    EXPECT_EQ(sent.last(), QVariantMap({{"radius", 2}, {"strength", 0.5}}));

    // Each edit applies six effects: wait for the second of 20 applied
    // effects to pass, so that the next twelve are not rate_limited.
    await([] { return false; }, 1100);
    // After a preset, an edit keeps the preset's values, not the effective ones.
    choice = "@preset";
    ASSERT_TRUE(run("scene-controls.open", 7));
    pump(10);
    ASSERT_NO_FATAL_FAILURE(edit("radius", "4"));
    EXPECT_EQ(sent.last(), QVariantMap({{"strength", 0.8}, {"radius", 4}}));
}

TEST_F(ScriptHostTest, AScanAnnouncesItsCommandsOnce)
{
    WRITE(user, "dynamic.luau", R"(
return {
    metadata = { commands = { fixed = { summary = "Fixed" } } },
    run_command = function() end,
    on_load = function() return afterglow.set_commands({ extra = { summary = "Extra" } }) end,
}
)");
    int changes = 0;
    QObject::connect(host.get(), &ScriptHost::commandsChanged, host.get(), [&] { ++changes; });
    host->setEnabled(true);
    EXPECT_EQ(host->commands().size(), 2);
    EXPECT_EQ(changes, 1);
}

TEST_F(ScriptHostTest, TextReadsOfARunWithAClientAreMarked)
{
    WRITE(user, "reader.luau", R"(
return {
    metadata = { commands = { read = { summary = "Read the pane", palette = "Read" } } },
    run_command = function(command, ev)
        return {
            afterglow.read_text(ev.pane, "screen"),
            { kind = "read_region", pane = ev.pane.id, region = "1" }, -- built by hand: no max_bytes
            afterglow.semantic_regions(ev.pane),
        }
    end,
    on_bell = function(ev) return afterglow.read_text(ev.pane, "screen") end,
}
)");
    host->setEnabled(true);
    Client& client = newClient();
    start("reader", "read", client);
    ASSERT_EQ(rec.applied.size(), 3);
    // Control.allowReadText decides about the text that can go to a client.
    for (const auto& e : std::as_const(rec.applied)) EXPECT_TRUE(e["client_run"].toBool()) << e["kind"].toString().toStdString();
    // The default limit, also for an effect that has none.
    EXPECT_EQ(rec.applied[1]["max_bytes"].toDouble(), 65536.0);
    // A run with no client, and an event, keep the text in the window.
    rec.applied.clear();
    ASSERT_TRUE(run("reader.read", 7));
    host->dispatch("on_bell", 7);
    ASSERT_EQ(rec.applied.size(), 4);
    for (const auto& e : std::as_const(rec.applied)) EXPECT_FALSE(e.contains("client_run")) << e["kind"].toString().toStdString();
}

TEST_F(ScriptHostTest, ADisabledScriptSaysWhyItsRunsEndAndRefusesNewOnes)
{
    // Results that wait for the script disable it: three lists of requests in one pass.
    WRITE(user, "flood.luau", R"(
return {
    metadata = { commands = { hold = { summary = "Wait" } } },
    run_command = function() afterglow.sleep(60000) end,
    on_bell = function()
        local out = {}
        for i = 1, 64 do out[i] = afterglow.request("r" .. i, afterglow.log("x")) end
        return out
    end,
    -- Results reach a script only through a handler for them.
    on_result = function() end,
}
)");
    host->setEnabled(true);
    Client& held = newClient();
    start("flood", "hold", held);
    for (int i = 0; i < 3; ++i) host->dispatch("on_bell", 7);
    ASSERT_TRUE(host->isDisabled("flood"));
    EXPECT_EQ(held.status, 1);
    EXPECT_TRUE(held.err.contains("operation results")) << held.err.toStdString();
    EXPECT_FALSE(held.err.contains("failures")) << held.err.toStdString();
    // It is there, but runs nothing: not "no such command".
    Client& late = newClient();
    QString error;
    bool unknown = true;
    EXPECT_EQ(start("flood", "hold", late, {}, 7, std::nullopt, &error, &unknown), 0u);
    EXPECT_FALSE(unknown);
    EXPECT_EQ(error, "Script flood is disabled. afterglow ctl scripts shows why");
}

TEST_F(ScriptHostTest, ProjectsExampleAddsACommandForEachSavedProject)
{
    QFile example(QStringLiteral(AFTERGLOW_EXAMPLE_SCRIPTS) + QStringLiteral("/projects.luau"));
    ASSERT_TRUE(example.open(QIODevice::ReadOnly));
    WRITE(user, "projects.luau", example.readAll());
    applyHook = [](const QVariantMap& e) -> ScriptHost::EffectResult {
        const QString kind = e["kind"].toString();
        if (kind == "list_projects") return {{}, {{"names", QVariantList{"dev"}}}, {}};
        if (kind == "select") return {{}, {{"id", "dev"}, {"title", "dev"}}, {}};
        if (kind == "open_project") return {{}, {{"window_id", 2}, {"reused", false}}, {}};
        return {};
    };
    host->setEnabled(true);
    Client& open = newClient();
    start("projects", "open", open);
    ASSERT_TRUE(await([&] { return open.status.has_value(); }));
    EXPECT_EQ(open.status, 0) << open.err.toStdString();
    // set_commands in the manifest's shape: names to their fields.
    const auto commands = host->commands();
    ASSERT_EQ(commands.size(), 2);
    EXPECT_EQ(commands[1].token(), "projects.open-dev");
    EXPECT_EQ(commands[1].palette, "Open project dev");
    EXPECT_TRUE(commands[1].runtime);
}

TEST_F(ScriptHostTest, OverlayEffectsAreCheckedBeforeTheyReachTheWindow)
{
    WRITE(user, QStringLiteral("overlays.luau"), R"(
return { on_bell = function(ev)
    local many = {}
    for i = 1, 65 do many[i] = "x" end
    return {
        afterglow.overlay(ev.pane, {}),
        afterglow.overlay(ev.pane, many),
        afterglow.overlay(ev.pane, {"fzf", string.rep("a", 4097)}),
        afterglow.overlay(ev.pane, "fzf", {capture = "stderr"}),
        { kind = "overlay", pane = ev.pane.id, command = {"fzf"}, options = {stdin = 5} },
        afterglow.overlay(ev.pane, "fzf", {env = {["A=B"] = "x"}}),
        afterglow.overlay(ev.pane, "fzf", {env = {HOME = true}}),
        afterglow.overlay(ev.pane, "fzf", {title = string.rep("t", 513)}),
        afterglow.overlay(ev.pane, "fzf", {captur = "file"}),
        afterglow.overlay(ev.pane, "fzf", {cwd = 5}),
        afterglow.overlay(ev.pane, "fzf", {cwd = "src"}),
        afterglow.overlay(ev.pane, "fzf", "--multi"),
        afterglow.request("pick", afterglow.overlay(ev.pane, {"fzf", "--multi"},
            {stdin = "a\nb\n", capture = "file", cwd = "/srv/remote", env = {LANG = "C", EDITOR = false}, title = "Pick"})),
    }
end }
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    // Each row is refused for its own rule: an option that a helper dropped
    // or coerced would reach the window, or be refused for another reason.
    const char* const reasons[] = {"argv list of 1 to 64", "argv list of 1 to 64", "A command argument is over 4096 characters or has a NUL",
        "capture must be stdout, file or none", "option stdin must be a string", "valid variable names",
        "valid variable names", "title is over 512", "Unknown overlay option captur", "option cwd must be a string",
        "cwd must be an absolute path", "Overlay options must be an object"};
    const qsizetype Refused = qsizetype(std::size(reasons));
    ASSERT_EQ(rec.results.size(), Refused + 1);
    for (qsizetype i = 0; i < Refused; ++i)
        EXPECT_TRUE(rec.results[i].second.contains(QLatin1String(reasons[i]))) << i << ": " << rec.results[i].second.toStdString();
    EXPECT_TRUE(rec.results[Refused].second.isEmpty()) << rec.results[Refused].second.toStdString();
    // Only the valid one reaches the window, with the spec that was read.
    ASSERT_EQ(overlays.size(), 1);
    const QVariantMap& e = overlays[0];
    EXPECT_EQ(e.value("pane").toUInt(), 7u);
    EXPECT_EQ(e.value("request_id").toString(), QStringLiteral("pick"));
    ASSERT_TRUE(e.value("spec").canConvert<OverlaySpec>());
    const OverlaySpec spec = e.value("spec").value<OverlaySpec>();
    EXPECT_EQ(spec.argv, (QStringList{"fzf", "--multi"}));
    ASSERT_TRUE(spec.input.has_value());
    EXPECT_EQ(*spec.input, QByteArray("a\nb\n"));
    EXPECT_EQ(spec.capture, OverlaySpec::Capture::File);
    EXPECT_EQ(spec.cwd, QStringLiteral("/srv/remote"));
    EXPECT_EQ(spec.title, QStringLiteral("Pick"));
    EXPECT_EQ(spec.env.value("LANG").toString(), QStringLiteral("C"));
    EXPECT_EQ(spec.env.value("EDITOR").typeId(), QMetaType::Bool);
}

TEST(OverlaySpecTest, ArgumentsAndOptionsKeepTheirLimits)
{
    QString error;
    const QStringList argv{QStringLiteral("fzf")};
    auto refused = [&error](const QStringList& args, const QVariantMap& options, const char* reason) {
        error.clear();
        return !OverlaySpec::read(args, options, &error) && error.contains(QLatin1String(reason));
    };
    auto accepted = [&error](const QStringList& args, const QVariantMap& options) {
        error.clear();
        return OverlaySpec::read(args, options, &error).has_value();
    };

    // stdin: UTF-8 bytes count, not characters.
    const QString most(OverlaySpec::MaxInput, QLatin1Char('x'));
    const auto spec = OverlaySpec::read(argv, {{QStringLiteral("stdin"), most}}, &error);
    ASSERT_TRUE(spec) << error.toStdString();
    ASSERT_TRUE(spec->input.has_value());
    EXPECT_EQ(spec->input->size(), OverlaySpec::MaxInput);
    EXPECT_TRUE(refused(argv, {{QStringLiteral("stdin"), most + QLatin1Char('x')}}, "stdin is over 4 MiB")) << error.toStdString();
    EXPECT_TRUE(accepted(argv, {{QStringLiteral("stdin"), QString(OverlaySpec::MaxInput / 2, QChar(0xe9))}})) << error.toStdString();
    EXPECT_TRUE(refused(argv, {{QStringLiteral("stdin"), QString(OverlaySpec::MaxInput / 2 + 1, QChar(0xe9))}}, "stdin is over"));

    // The arguments, as for exec.
    QStringList many(OverlaySpec::MaxArguments, QStringLiteral("a"));
    EXPECT_TRUE(accepted(many, {})) << error.toStdString();
    many.append(QStringLiteral("a"));
    EXPECT_TRUE(refused(many, {}, "argv list of 1 to 64")) << error.toStdString();
    EXPECT_TRUE(accepted({QStringLiteral("fzf"), QString(OverlaySpec::MaxArgument, QLatin1Char('a'))}, {})) << error.toStdString();
    EXPECT_TRUE(refused({QStringLiteral("fzf"), QString(OverlaySpec::MaxArgument + 1, QLatin1Char('a'))}, {}, "A command argument is over 4096 characters or has a NUL"));
    EXPECT_TRUE(refused({QString()}, {}, "program to run")) << error.toStdString();
    QString withNul = QStringLiteral("ab");
    withNul.insert(1, QChar::Null);
    EXPECT_TRUE(refused({withNul}, {}, "A command argument is over 4096 characters or has a NUL")) << error.toStdString();

    EXPECT_TRUE(refused(argv, {{QStringLiteral("shell"), QStringLiteral("sh")}}, "Unknown overlay option shell")) << error.toStdString();
    EXPECT_TRUE(refused(argv, {{QStringLiteral("title"), 5}}, "option title must be a string")) << error.toStdString();
    const auto titled = OverlaySpec::read(argv, {{QStringLiteral("title"), QString(OverlaySpec::MaxTitle, QLatin1Char('t'))}}, &error);
    ASSERT_TRUE(titled) << error.toStdString();
    EXPECT_EQ(titled->title.size(), OverlaySpec::MaxTitle);
    EXPECT_TRUE(refused(argv, {{QStringLiteral("title"), QString(OverlaySpec::MaxTitle + 1, QLatin1Char('t'))}}, "title is over 512"))
        << error.toStdString();
    // Left out is the default. An empty capture is no choice.
    EXPECT_TRUE(refused(argv, {{QStringLiteral("capture"), QStringLiteral("")}}, "capture must be")) << error.toStdString();
    const auto none = OverlaySpec::read(argv, {{QStringLiteral("capture"), QStringLiteral("none")}}, &error);
    ASSERT_TRUE(none);
    EXPECT_EQ(none->capture, OverlaySpec::Capture::None);
    EXPECT_FALSE(none->input.has_value());

    // The directory: one that the window expands, never one relative to it.
    for (const char* cwd : {"", "/srv/remote", "~", "~/src"})
        EXPECT_TRUE(accepted(argv, {{QStringLiteral("cwd"), QString::fromLatin1(cwd)}})) << cwd << ": " << error.toStdString();
    // ":/" is absolute to QDir: a Qt resource, where no program starts.
    for (const char* cwd : {"src", "./src", "~user", "~user/src", ":/", ":/x"})
        EXPECT_TRUE(refused(argv, {{QStringLiteral("cwd"), QString::fromLatin1(cwd)}}, "cwd must be an absolute path")) << cwd;

    // The environment, by the rules of exec and launch options.
    QVariantMap env;
    for (qsizetype i = 0; i < EnvironmentOverrides::MaxVariables; ++i) env.insert(QStringLiteral("V%1").arg(i), QString());
    EXPECT_TRUE(accepted(argv, {{QStringLiteral("env"), env}})) << error.toStdString();
    env.insert(QStringLiteral("ONE_MORE"), QString());
    EXPECT_TRUE(refused(argv, {{QStringLiteral("env"), env}}, "at most 256 variables")) << error.toStdString();
    // Names and values together.
    const QVariantMap full{{QStringLiteral("V"), QString(EnvironmentOverrides::MaxBytes - 1, QLatin1Char('x'))}};
    EXPECT_TRUE(accepted(argv, {{QStringLiteral("env"), full}})) << error.toStdString();
    const QVariantMap over{{QStringLiteral("V"), QString(EnvironmentOverrides::MaxBytes, QLatin1Char('x'))}};
    EXPECT_TRUE(refused(argv, {{QStringLiteral("env"), over}}, "over 64 KiB")) << error.toStdString();
    EXPECT_TRUE(refused(argv, {{QStringLiteral("env"), QStringLiteral("LANG=C")}}, "env must be an object")) << error.toStdString();
    // An empty JSON list, from the control socket, and an empty object, as
    // an empty Luau table arrives.
    EXPECT_TRUE(accepted(argv, {{QStringLiteral("env"), QVariantList{}}})) << error.toStdString();
    EXPECT_TRUE(accepted(argv, {{QStringLiteral("env"), QVariantMap{}}})) << error.toStdString();
}

TEST_F(ScriptHostTest, PopupOptionsAreCheckedAndReachTheWindow)
{
    WRITE(user, QStringLiteral("popups.luau"), R"(
return { on_bell = function(ev)
    return {
        afterglow.overlay(ev.pane, "fzf", {width = 0}),
        afterglow.overlay(ev.pane, "fzf", {width = 10.5}),
        afterglow.overlay(ev.pane, "fzf", {height = "0%"}),
        afterglow.overlay(ev.pane, "fzf", {height = "101%"}),
        afterglow.overlay(ev.pane, "fzf", {width = "80"}),
        afterglow.overlay(ev.pane, "fzf", {width = true}),
        afterglow.overlay(ev.pane, "fzf", {position = "middle"}),
        afterglow.overlay(ev.pane, "fzf", {position = 1}),
        afterglow.overlay(ev.pane, "fzf", {border = "yes"}),
        afterglow.overlay(ev.pane, "fzf", {width = math.huge}),
        afterglow.overlay(ev.pane, "fzf", {width = 1e300}),
        afterglow.overlay(ev.pane, "fzf", {width = {}}),
        afterglow.overlay(ev.pane, "fzf", {position = {}}),
        afterglow.overlay(ev.pane, "fzf", {title = "a\nb"}),
        afterglow.overlay(ev.pane, "fzf", {title = "a\u{202E}b"}),
        afterglow.overlay(ev.pane, "fzf", {width = "80%", height = 20, position = "bottom-right", border = false}),
        afterglow.overlay(ev.pane, "fzf", {width = 60, border = nil}),
    }
end }
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    // Each row is refused for its own rule.
    const char* const reasons[] = {"width", "width", "height", "height", "width", "width", "position", "position", "border",
                                   "width", "width", "width", "position", "title", "title"};
    const int refusedRows = int(std::size(reasons));
    ASSERT_EQ(rec.results.size(), refusedRows + 2);
    for (int i = 0; i < refusedRows; ++i)
        EXPECT_TRUE(rec.results[i].second.contains(QLatin1String(reasons[i]))) << i << ": " << rec.results[i].second.toStdString();
    for (int i = refusedRows; i < refusedRows + 2; ++i) EXPECT_TRUE(rec.results[i].second.isEmpty()) << rec.results[i].second.toStdString();
    ASSERT_EQ(overlays.size(), 2);
    // The spec that ScriptHost read goes to the window.
    const OverlaySpec spec = overlays[0].value("spec").value<OverlaySpec>();
    EXPECT_EQ(spec.width, (OverlaySpec::Extent{80, true}));
    EXPECT_EQ(spec.height, (OverlaySpec::Extent{20, false}));
    EXPECT_EQ(spec.position, OverlaySpec::Position::BottomRight);
    EXPECT_EQ(spec.border, false);
    EXPECT_TRUE(spec.popup());
    EXPECT_FALSE(spec.framed());
    // Unset options stay at the defaults.
    const OverlaySpec plain = overlays[1].value("spec").value<OverlaySpec>();
    EXPECT_EQ(plain.width, (OverlaySpec::Extent{60, false}));
    EXPECT_FALSE(plain.height);
    EXPECT_EQ(plain.position, OverlaySpec::Position::Center);
    EXPECT_FALSE(plain.border);
}

TEST(OverlaySpecTest, PopupOptionsTakeCellsPercentagesPositionsAndABorder)
{
    QString error;
    const QStringList argv{QStringLiteral("fzf")};
    auto read = [&](const QVariantMap& options) { return OverlaySpec::read(argv, options, &error); };
    // One that must be read: a failed test, not a crash, when it is not.
    auto taken = [&](const QVariantMap& options) {
        const auto spec = read(options);
        EXPECT_TRUE(spec) << error.toStdString();
        return spec.value_or(OverlaySpec{});
    };
    // JSON and Luau give whole numbers as qlonglong; a double such as 60.0
    // is taken too.
    EXPECT_EQ(taken({{"width", qlonglong(60)}}).width, (OverlaySpec::Extent{60, false}));
    EXPECT_EQ(taken({{"width", 60.0}}).width, (OverlaySpec::Extent{60, false}));
    EXPECT_EQ(taken({{"height", "1%"}}).height, (OverlaySpec::Extent{1, true}));
    EXPECT_EQ(taken({{"height", "100%"}}).height, (OverlaySpec::Extent{100, true}));
    EXPECT_EQ(taken({{"width", OverlaySpec::MaxCells}}).width, (OverlaySpec::Extent{OverlaySpec::MaxCells, false}));
    for (const QVariant& bad : {QVariant(0), QVariant(-3), QVariant(2.5), QVariant(OverlaySpec::MaxCells + 1), QVariant("0%"),
                                QVariant("101%"), QVariant("80"), QVariant(" 80%"), QVariant("80 %"), QVariant("%"), QVariant(true),
                                QVariant(std::nan("")), QVariant(-0.0), QVariant("080%"), QVariant("+80%"), QVariant("80%\n")}) {
        EXPECT_FALSE(read({{"width", bad}})) << bad.toString().toStdString();
        EXPECT_TRUE(error.startsWith(QLatin1String("width"))) << error.toStdString();
    }
    EXPECT_FALSE(read({{"height", "abc"}}));
    EXPECT_TRUE(error.startsWith(QLatin1String("height"))) << error.toStdString();
    const std::pair<const char*, OverlaySpec::Position> positions[] = {
        {"center", OverlaySpec::Position::Center},       {"top", OverlaySpec::Position::Top},
        {"bottom", OverlaySpec::Position::Bottom},       {"left", OverlaySpec::Position::Left},
        {"right", OverlaySpec::Position::Right},         {"top-left", OverlaySpec::Position::TopLeft},
        {"top-right", OverlaySpec::Position::TopRight},  {"bottom-left", OverlaySpec::Position::BottomLeft},
        {"bottom-right", OverlaySpec::Position::BottomRight}};
    for (const auto& [name, position] : positions) EXPECT_EQ(taken({{"position", QLatin1String(name)}}).position, position) << name;
    for (const QVariant& bad : {QVariant("Center"), QVariant("middle"), QVariant(""), QVariant(1)}) EXPECT_FALSE(read({{"position", bad}}));
    EXPECT_FALSE(read({{"border", "true"}}));
    EXPECT_FALSE(read({{"border", 1}}));
    // Null is unset, as Luau's nil.
    const auto plain = read({{"width", QVariant()}, {"border", QVariant::fromValue(nullptr)}});
    ASSERT_TRUE(plain) << error.toStdString();
    EXPECT_FALSE(plain->width);
    EXPECT_FALSE(plain->border);
    // A border by default for a popup, none for a full-pane overlay; either way when given.
    EXPECT_FALSE(plain->popup());
    EXPECT_FALSE(plain->framed());
    EXPECT_TRUE(taken({{"height", 10}}).framed());
    EXPECT_FALSE(taken({{"height", 10}, {"border", false}}).framed());
    const OverlaySpec boxed = taken({{"border", true}});
    EXPECT_TRUE(boxed.framed());
    EXPECT_TRUE(boxed.popup());
    EXPECT_FALSE(boxed.sized());
    // A popup's border shows the title: no control, line break or
    // bidirectional control in it.
    EXPECT_EQ(taken({{"title", QStringLiteral("Pick a file")}}).title, QStringLiteral("Pick a file"));
    for (const char16_t c : {u'\n', u'\x1b', u'\x85', u'\u2028', u'\u202e', u'\u2066'}) {
        EXPECT_FALSE(read({{"title", QStringLiteral("a") + QChar(c) + QStringLiteral("b")}})) << int(c);
        EXPECT_TRUE(error.contains(QLatin1String("title"))) << error.toStdString();
    }
}

TEST(OverlaySpecTest, APopupIsPlacedInItsHostGrid)
{
    QString error;
    auto spec = [&](const QVariantMap& options) {
        auto read = OverlaySpec::read({QStringLiteral("fzf")}, options, &error);
        EXPECT_TRUE(read) << error.toStdString();
        return read.value_or(OverlaySpec{});
    };
    using Placement = OverlaySpec::Placement;
    // A full-pane overlay: the whole grid, no border.
    EXPECT_EQ(spec({}).place(100, 30), (Placement{QRect(0, 0, 100, 30), false}));
    // With a border: the grid inside it.
    EXPECT_EQ(spec({{"border", true}}).place(100, 30), (Placement{QRect(1, 1, 98, 28), true}));
    // Percentages of the host grid, the border inside them, in the center.
    EXPECT_EQ(spec({{"width", "80%"}, {"height", "60%"}}).place(100, 30), (Placement{QRect(11, 7, 78, 16), true}));
    // Cells, with the border counted.
    EXPECT_EQ(spec({{"width", 40}, {"height", 10}}).place(100, 30), (Placement{QRect(31, 11, 38, 8), true}));
    EXPECT_EQ(spec({{"width", 40}, {"height", 10}, {"border", false}}).place(100, 30), (Placement{QRect(30, 10, 40, 10), false}));
    // One side given: the other is the whole grid.
    EXPECT_EQ(spec({{"height", 10}}).place(100, 30), (Placement{QRect(1, 11, 98, 8), true}));
    // Each position.
    const std::pair<const char*, QPoint> corners[] = {
        {"center", {30, 10}},  {"top", {30, 0}},      {"bottom", {30, 20}},       {"left", {0, 10}},          {"right", {60, 10}},
        {"top-left", {0, 0}},  {"top-right", {60, 0}}, {"bottom-left", {0, 20}}, {"bottom-right", {60, 20}},
    };
    for (const auto& [name, at] : corners) {
        const Placement placed = spec({{"width", 40}, {"height", 10}, {"position", QLatin1String(name)}}).place(100, 30);
        EXPECT_EQ(placed.grid, QRect(at + QPoint(1, 1), QSize(38, 8))) << name;
    }
    // Clamped to the host: never larger, and at least 2 by 1 inside the border.
    EXPECT_EQ(spec({{"width", 500}, {"height", 500}}).place(100, 30), (Placement{QRect(1, 1, 98, 28), true}));
    EXPECT_EQ(spec({{"width", 1}, {"height", 1}}).place(100, 30), (Placement{QRect(49, 14, 2, 1), true}));
    EXPECT_EQ(spec({{"width", "1%"}, {"height", "1%"}}).place(100, 30), (Placement{QRect(49, 14, 2, 1), true}));
    EXPECT_EQ(spec({{"width", 1}, {"height", 1}, {"border", false}}).place(100, 30), (Placement{QRect(49, 14, 2, 1), false}));
    // A host with no room for the border: the popup goes without it.
    EXPECT_EQ(spec({{"width", "50%"}, {"height", "50%"}}).place(3, 30), (Placement{QRect(0, 7, 2, 15), false}));
    EXPECT_EQ(spec({{"width", "50%"}, {"height", "50%"}}).place(10, 2), (Placement{QRect(2, 0, 5, 1), false}));
    EXPECT_EQ(spec({{"width", "50%"}, {"height", "50%"}}).place(4, 3), (Placement{QRect(1, 1, 2, 1), true}));
    EXPECT_EQ(spec({{"width", "50%"}}).place(2, 1), (Placement{QRect(0, 0, 2, 1), false}));
    // A layout places it anew: a resize moves and sizes it with the host.
    const OverlaySpec resized = spec({{"width", "80%"}, {"height", "60%"}, {"position", "bottom"}});
    EXPECT_EQ(resized.place(100, 30), (Placement{QRect(11, 13, 78, 16), true}));
    EXPECT_EQ(resized.place(50, 20), (Placement{QRect(6, 9, 38, 10), true}));
}

TEST_F(ScriptHostTest, OverlayDefaultsReadTheTerminalAndCaptureStdout)
{
    WRITE(user, QStringLiteral("plain.luau"), R"(
return { on_bell = function(ev) return afterglow.overlay(ev.pane, "htop") end }
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_EQ(overlays.size(), 1);
    const OverlaySpec spec = overlays[0].value("spec").value<OverlaySpec>();
    EXPECT_FALSE(spec.input.has_value()) << "no stdin: the program reads the terminal";
    EXPECT_EQ(spec.capture, OverlaySpec::Capture::Stdout);
    EXPECT_EQ(spec.argv, QStringList{"htop"});
}

TEST_F(ScriptHostTest, AnAwaitedOverlayResumesWithItsResultOrItsError)
{
    WRITE(user, QStringLiteral("pick.luau"), R"(
local log = {}
return {
    metadata = { commands = { log = { summary = "Print what happened" } } },
    run_command = function(_, ev) return afterglow.output(ev, table.concat(log, ",")) end,
    on_bell = function(ev)
        local r, err = afterglow.await(afterglow.overlay(ev.pane, {"fzf"}, {stdin = "a\nb\n"}))
        if r then
            table.insert(log, r.exit_code .. ":" .. r.stdout .. ":" .. tostring(r.truncated))
        else
            table.insert(log, err.code)
        end
    end,
}
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_TRUE(bool(pendingReply));
    std::exchange(pendingReply, {})({{}, {{"exit_code", 0}, {"stdout", "b\n"}, {"truncated", false}}, {}});
    pump();
    host->dispatch("on_bell", 7);
    ASSERT_TRUE(bool(pendingReply));
    std::exchange(pendingReply, {})({QStringLiteral("The pane has an overlay already"), {}, QStringLiteral("busy")});
    pump();
    Client& client = newClient();
    start("pick", "log", client);
    ASSERT_TRUE(await([&] { return client.status.has_value(); }));
    EXPECT_EQ(client.out, QStringLiteral("0:b\n:false,busy"));
}

TEST_F(ScriptHostTest, TheEndOfARunClosesTheOverlayItWaitsFor)
{
    // As for a dialog: the window closes the overlay that the run's flow
    // waits for, and the handler resumes with "cancelled".
    WRITE(user, QStringLiteral("picker.luau"), R"(
local got = "none"
return {
    metadata = { commands = { pick = { summary = "Pick" }, got = { summary = "Print the result" } } },
    run_command = function(command, ev)
        if command == "pick" then
            local _, err = afterglow.await(afterglow.overlay(ev.pane, {"fzf"}))
            got = err and err.code or "picked"
        else
            return afterglow.output(ev, got)
        end
    end,
}
)");
    host->setEnabled(true);
    Client& picker = newClient();
    const quint64 handle = start("picker", "pick", picker);
    pump();
    ASSERT_TRUE(bool(pendingReply));
    ASSERT_EQ(overlays.size(), 1);
    const QString request = overlays[0].value("request_id").toString();
    host->cancelCommand(handle, 130);
    pump();
    // Closed by the prefix of its flow, which the window matches.
    ASSERT_FALSE(closedDialogs.isEmpty());
    EXPECT_TRUE(request.startsWith(closedDialogs.last())) << request.toStdString() << " " << closedDialogs.last().toStdString();
    EXPECT_FALSE(bool(pendingReply));
    EXPECT_EQ(picker.status, 130);
    Client& got = newClient();
    start("picker", "got", got);
    ASSERT_TRUE(await([&] { return got.status.has_value(); }));
    EXPECT_EQ(got.out, QStringLiteral("cancelled"));
}

namespace {
class FzfPicker : public ScriptHostTest
{
protected:
    /// Installs the package with @p fzf as its program, in a directory with
    /// two files, and a third whose name has Ctrl+D.
    bool install(const QString& fzf)
    {
        program = fzf;
        installPackage(QStringLiteral("fzf-picker"));
        if (HasFatalFailure()) return false;
        QFile manifest(user.filePath(QStringLiteral("fzf-picker/package.json")));
        if (!manifest.open(QIODevice::ReadWrite)) {
            ADD_FAILURE() << "no manifest";
            return false;
        }
        QByteArray json = manifest.readAll();
        const QByteArray configured = "\"fzf\": \"" + fzf.toUtf8() + "\"";
        json.replace("\"fzf\": \"fzf\"", configured);
        // Not patched, the package would run the fzf of the PATH.
        if (!json.contains(configured)) {
            ADD_FAILURE() << "the manifest has no fzf entry to set";
            return false;
        }
        manifest.resize(0);
        manifest.seek(0);
        if (manifest.write(json) != json.size()) {
            ADD_FAILURE() << "cannot write the manifest";
            return false;
        }
        manifest.close();
        QDir().mkpath(files.filePath(QStringLiteral("src")));
        for (const char* name : {"notes.txt", "src/it's.c", "evil\x04.txt"}) {
            QFile file(files.filePath(QString::fromUtf8(name)));
            if (!file.open(QIODevice::WriteOnly)) {
                ADD_FAILURE() << "cannot create " << name;
                return false;
            }
        }
        paneState[7].insert(QStringLiteral("cwd"), files.path());
        host->setEnabled(true);
        return true;
    }

    /// Answers the select dialog with src/it's.c after @p during runs, or
    /// cancels it as Escape does when @p cancel is set.
    void answerSelect(const std::function<void()>& during, bool cancel = false)
    {
        applyHook = [this, during, cancel](const QVariantMap& e) -> ScriptHost::EffectResult {
            if (e.value("kind") != QLatin1String("select")) return {};
            selected = true;
            listed.clear();
            QVariantMap pick;
            for (const QVariant& item : e.value("items").toList()) {
                listed << item.toMap().value("title").toString();
                if (listed.last() == QLatin1String("file: src/it's.c")) pick = item.toMap();
            }
            if (during) during();
            if (cancel) return {QStringLiteral("Dialog canceled"), {}, QStringLiteral("cancelled")};
            if (pick.isEmpty()) return {QStringLiteral("not listed"), {}, QStringLiteral("cancelled")};
            return {{}, pick, {}};
        };
    }

    /// Runs pick on pane 7 from the palette. The select dialog and the
    /// overlay both pick src/it's.c, after @p during runs. Returns the effect
    /// that acted on the choice: send_text or notify.
    std::optional<QVariantMap> pick(bool expectOverlay, const std::function<void()>& during = {})
    {
        answerSelect(during);
        if (!run(QStringLiteral("fzf-picker.pick"), 7)) {
            ADD_FAILURE() << "did not run";
            return std::nullopt;
        }
        auto acted = [this] {
            return std::find_if(rec.applied.cbegin(), rec.applied.cend(), [](const QVariantMap& e) {
                return e.value("kind") == QLatin1String("send_text") || e.value("kind") == QLatin1String("notify");
            });
        };
        const bool done = await([&] {
            if (!overlays.isEmpty() && pendingReply) {
                // fzf's list on stdin, in the pane's directory.
                const OverlaySpec spec = overlays.takeFirst().value("spec").value<OverlaySpec>();
                listed = QString::fromUtf8(spec.input.value_or(QByteArray())).split(QLatin1Char('\n'));
                EXPECT_EQ(spec.cwd, files.path());
                EXPECT_EQ(spec.argv.value(0), program);
                if (during) during();
                // A popup over the pane, which shows around it.
                EXPECT_EQ(spec.width, (OverlaySpec::Extent{80, true}));
                EXPECT_EQ(spec.height, (OverlaySpec::Extent{60, true}));
                std::exchange(pendingReply, {})({{}, {{"exit_code", 0}, {"stdout", "file: src/it's.c\n"}, {"truncated", false}}, {}});
            }
            return acted() != rec.applied.cend();
        }, 10000);
        applyHook = {};
        EXPECT_EQ(selected, !expectOverlay);
        if (!done) {
            ADD_FAILURE() << "the pick did nothing";
            return std::nullopt;
        }
        // A name with a control character would type it as a key.
        EXPECT_TRUE(listed.contains(QStringLiteral("file: src/it's.c"))) << listed.join(QLatin1Char('|')).toStdString();
        EXPECT_TRUE(listed.contains(QStringLiteral("file: notes.txt"))) << listed.join(QLatin1Char('|')).toStdString();
        EXPECT_FALSE(listed.join(QLatin1Char('\n')).contains(QLatin1String("evil"))) << listed.join(QLatin1Char('|')).toStdString();
        return *acted();
    }

    /// Installs the package with an fzf in @p bin that starts (its --version
    /// probe succeeds), so the pick takes the overlay, whose result the test
    /// then gives.
    void installStartingFzf(QTemporaryDir& bin)
    {
        ASSERT_TRUE(bin.isValid());
        const QString fake = bin.filePath(QStringLiteral("fzf"));
        WRITE(bin, QStringLiteral("fzf"), "#!/bin/sh\nexit 0\n");
        ASSERT_TRUE(QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        ASSERT_TRUE(install(fake));
    }

    /// The text that the pick types for @p path: quoted, then a space.
    static QString typed(const QString& path) { return QLatin1Char('\'') + QString(path).replace(QLatin1String("'"), QLatin1String("'\\''")) + QStringLiteral("' "); }

    QString program;
    bool selected = false;
    QStringList listed; ///< the entries that fzf or the select dialog got
};
} // namespace

TEST_F(FzfPicker, WithFzfThePickComesFromTheOverlay)
{
    const QString fzf = QStandardPaths::findExecutable(QStringLiteral("fzf"));
    if (fzf.isEmpty()) GTEST_SKIP() << "fzf is not installed";
    ASSERT_TRUE(install(fzf));
    const auto acted = pick(true);
    ASSERT_TRUE(acted);
    EXPECT_EQ(acted->value("kind").toString(), QStringLiteral("send_text"));
    EXPECT_EQ(acted->value("pane").toUInt(), 7u);
    // Absolute: the pane's shell can be in another directory than the run.
    EXPECT_EQ(acted->value("text").toString(), typed(files.path() + QStringLiteral("/src/it's.c")));
}

TEST_F(FzfPicker, WithoutFzfThePickComesFromTheSelectDialog)
{
    ASSERT_TRUE(install(QStringLiteral("/nonexistent/afterglow-test/fzf")));
    const auto acted = pick(false);
    ASSERT_TRUE(acted);
    EXPECT_EQ(acted->value("kind").toString(), QStringLiteral("send_text"));
    EXPECT_EQ(acted->value("pane").toUInt(), 7u);
    EXPECT_EQ(acted->value("text").toString(), typed(files.path() + QStringLiteral("/src/it's.c")));
}

TEST_F(FzfPicker, AProgramThatStartedDuringThePickGetsNoKeys)
{
    ASSERT_TRUE(install(QStringLiteral("/nonexistent/afterglow-test/fzf")));
    // At the prompt when the pick starts, in an editor when it ends: the
    // package reads the pane again after its wait.
    const auto acted = pick(false, [this] { paneState[7].insert(QStringLiteral("command"), QStringLiteral("vim notes.txt")); });
    ASSERT_TRUE(acted);
    EXPECT_EQ(acted->value("kind").toString(), QStringLiteral("notify"));
    EXPECT_TRUE(acted->value("body").toString().contains(QLatin1String("vim notes.txt"))) << acted->value("body").toString().toStdString();
    EXPECT_TRUE(std::none_of(rec.applied.cbegin(), rec.applied.cend(),
                             [](const QVariantMap& e) { return e.value("kind") == QLatin1String("send_text"); }));
}

TEST_F(FzfPicker, FromAShellThePathGoesToStandardOutput)
{
    ASSERT_TRUE(install(QStringLiteral("/nonexistent/afterglow-test/fzf")));
    // The source shell sends the path to stdout, before the package looks
    // at the pane's program. In a pane that runs aglow, as a real run does,
    // and at the prompt, where the check of the program would type it.
    for (const char* command : {"aglow run fzf-picker pick", ""}) {
        SCOPED_TRACE(command);
        answerSelect({});
        selected = false;
        Client& client = newClient();
        paneState[7].insert(QStringLiteral("command"), QString::fromLatin1(command));
        start(QStringLiteral("fzf-picker"), QStringLiteral("pick"), client, {}, 7, std::nullopt, nullptr, nullptr, files.path());
        ASSERT_TRUE(await([&] { return client.status.has_value(); }, 10000));
        applyHook = {};
        EXPECT_TRUE(selected);
        EXPECT_EQ(client.status, 0) << client.err.toStdString();
        EXPECT_EQ(client.out, files.path() + QStringLiteral("/src/it's.c\n"));
        EXPECT_TRUE(std::none_of(rec.applied.cbegin(), rec.applied.cend(),
                                 [](const QVariantMap& e) { return e.value("kind") == QLatin1String("send_text"); }));
    }
}

TEST_F(FzfPicker, ACancelledPickFromAShellExitsWithOne)
{
    ASSERT_TRUE(install(QStringLiteral("/nonexistent/afterglow-test/fzf")));
    answerSelect({}, true);
    Client& client = newClient();
    start(QStringLiteral("fzf-picker"), QStringLiteral("pick"), client, {}, 7, std::nullopt, nullptr, nullptr, files.path());
    ASSERT_TRUE(await([&] { return client.status.has_value(); }, 10000));
    applyHook = {};
    EXPECT_TRUE(selected);
    // No path: `f=$(aglow run fzf-picker pick) && vim "$f"` stops. A
    // cancel is no failure, so it writes no message, and nothing shows on
    // the desktop.
    EXPECT_EQ(client.status, 1);
    EXPECT_TRUE(client.out.isEmpty()) << client.out.toStdString();
    EXPECT_TRUE(client.err.isEmpty()) << client.err.toStdString();
    EXPECT_TRUE(std::none_of(rec.applied.cbegin(), rec.applied.cend(), [](const QVariantMap& e) {
        return e.value("kind") == QLatin1String("send_text") || e.value("kind") == QLatin1String("notify");
    }));
}

TEST_F(FzfPicker, AFailedPickFromAShellWritesItsErrorOnStderr)
{
    ASSERT_TRUE(install(QStringLiteral("/nonexistent/afterglow-test/fzf")));
    applyHook = [](const QVariantMap& e) -> ScriptHost::EffectResult {
        if (e.value("kind") != QLatin1String("select")) return {};
        return {QStringLiteral("A script dialog is open"), {}, QStringLiteral("busy")};
    };
    Client& client = newClient();
    start(QStringLiteral("fzf-picker"), QStringLiteral("pick"), client, {}, 7, std::nullopt, nullptr, nullptr, files.path());
    ASSERT_TRUE(await([&] { return client.status.has_value(); }, 10000));
    applyHook = {};
    EXPECT_EQ(client.status, 1);
    EXPECT_TRUE(client.out.isEmpty()) << client.out.toStdString();
    EXPECT_TRUE(client.err.contains(QLatin1String("A script dialog is open"))) << client.err.toStdString();
    EXPECT_TRUE(std::none_of(rec.applied.cbegin(), rec.applied.cend(),
                             [](const QVariantMap& e) { return e.value("kind") == QLatin1String("notify"); }));
}

TEST_F(FzfPicker, AnFzfErrorFromAShellWritesItsStatusOnStderr)
{
    // An fzf that starts, and then exits with 2 in the overlay, as an old
    // fzf does for an option that it does not know.
    QTemporaryDir bin;
    ASSERT_TRUE(bin.isValid());
    installStartingFzf(bin);
    if (HasFatalFailure()) return;
    Client& client = newClient();
    start(QStringLiteral("fzf-picker"), QStringLiteral("pick"), client, {}, 7, std::nullopt, nullptr, nullptr, files.path());
    ASSERT_TRUE(await([&] { return !overlays.isEmpty() && bool(pendingReply); }, 10000));
    std::exchange(pendingReply, {})({{}, {{"exit_code", 2}, {"stdout", ""}, {"truncated", false}}, {}});
    ASSERT_TRUE(await([&] { return client.status.has_value(); }, 10000));
    EXPECT_EQ(client.status, 1);
    EXPECT_TRUE(client.out.isEmpty()) << client.out.toStdString();
    EXPECT_TRUE(client.err.contains(QLatin1String("exited with status 2"))) << client.err.toStdString();
    EXPECT_TRUE(std::none_of(rec.applied.cbegin(), rec.applied.cend(),
                             [](const QVariantMap& e) { return e.value("kind") == QLatin1String("notify"); }));
}

TEST_F(FzfPicker, AnFzfErrorFromThePaletteShowsItsStatus)
{
    // From the palette the status shows in a notice, and -1, a status that
    // Afterglow could not read, is named as unknown.
    QTemporaryDir bin;
    ASSERT_TRUE(bin.isValid());
    installStartingFzf(bin);
    if (HasFatalFailure()) return;
    for (const auto& [code, said] : {std::pair{2, "exited with status 2"}, std::pair{-1, "status that is not known"}}) {
        rec.applied.clear();
        if (!run(QStringLiteral("fzf-picker.pick"), 7) || !await([&] { return !overlays.isEmpty() && bool(pendingReply); }, 10000)) {
            ADD_FAILURE() << code << ": no overlay";
            continue;
        }
        overlays.clear();
        std::exchange(pendingReply, {})({{}, {{"exit_code", code}, {"stdout", ""}, {"truncated", false}}, {}});
        const auto notice = [this] {
            return std::find_if(rec.applied.cbegin(), rec.applied.cend(),
                                [](const QVariantMap& e) { return e.value("kind") == QLatin1String("notify"); });
        };
        if (!await([&] { return notice() != rec.applied.cend(); }, 10000)) {
            ADD_FAILURE() << code << ": no notice";
            continue;
        }
        const QString body = notice()->value("body").toString();
        EXPECT_TRUE(body.contains(QLatin1String(said))) << code << ": " << body.toStdString();
        EXPECT_FALSE(body.contains(QLatin1String("status -1"))) << body.toStdString();
        EXPECT_TRUE(std::none_of(rec.applied.cbegin(), rec.applied.cend(),
                                 [](const QVariantMap& e) { return e.value("kind") == QLatin1String("send_text"); }));
    }
}

TEST_F(FzfPicker, AProjectPickedFromAShellOpensItAndPrintsNoPath)
{
    ASSERT_TRUE(install(QStringLiteral("/nonexistent/afterglow-test/fzf")));
    applyHook = [](const QVariantMap& e) -> ScriptHost::EffectResult {
        if (e.value("kind") == QLatin1String("list_projects")) return {{}, {{"names", QVariantList{QStringLiteral("dev")}}}, {}};
        if (e.value("kind") != QLatin1String("select")) return {};
        for (const QVariant& item : e.value("items").toList())
            if (item.toMap().value("title") == QLatin1String("project: dev")) return {{}, item.toMap(), {}};
        return {QStringLiteral("not listed"), {}, QStringLiteral("cancelled")};
    };
    Client& client = newClient();
    start(QStringLiteral("fzf-picker"), QStringLiteral("pick"), client, {}, 7, std::nullopt, nullptr, nullptr, files.path());
    ASSERT_TRUE(await([&] { return client.status.has_value(); }, 10000));
    applyHook = {};
    // Nothing for $(...) to take, so `&& vim "$f"` does not run.
    EXPECT_EQ(client.status, 1);
    EXPECT_TRUE(client.out.isEmpty()) << client.out.toStdString();
    EXPECT_TRUE(client.err.isEmpty()) << client.err.toStdString();
    EXPECT_TRUE(std::any_of(rec.applied.cbegin(), rec.applied.cend(), [](const QVariantMap& e) {
        return e.value("kind") == QLatin1String("open_project") && e.value("name") == QLatin1String("dev");
    }));
}

TEST_F(ScriptHostTest, CpuBudgetDropsEventsDelaysResumesAndDisablesAScriptThatKeepsIt)
{
    // A run that never ends its loop: each part is well within a command's
    // 100 ms, and each sleep(0) is far below the results limit. Only the
    // CPU budget stops it.
    WRITE(user, "hog.luau", R"(
local function spin(ms) local t = os.clock() while os.clock() - t < ms / 1000 do end end
return {
    metadata = { commands = { spin = { summary = "Spin" } } },
    on_bell = function() spin(15) return nil end,
    run_command = function(command, ev)
        while true do
            spin(30)
            afterglow.sleep(0)
        end
    end,
})");
    host->setCpuBudget(40);
    host->setEnabled(true);
    // Events past the budget are dropped.
    for (int i = 0; i < 6; ++i) host->dispatch("on_bell", 7);
    auto stats = firstDiagnostic();
    EXPECT_GE(stats["dropped_callbacks"].toULongLong(), 1u);
    EXPECT_GE(stats["cpu_ms"].toDouble(), 40.0);
    EXPECT_LT(stats["calls"].toULongLong(), 6u);

    Client& client = newClient();
    ASSERT_NE(start(QStringLiteral("hog"), QStringLiteral("spin"), client), 0u);
    // The bells spent the budget, so the start itself can wait: count from here.
    const quint64 delayedAtStart = firstDiagnostic()["delayed_callbacks"].toULongLong();
    ASSERT_TRUE(await([&] { return host->isDisabled(QStringLiteral("hog")); }, 15000));
    stats = firstDiagnostic();
    // Its resumes waited for CPU time, rather than run back to back.
    EXPECT_GT(stats["delayed_callbacks"].toULongLong(), delayedAtStart);
    EXPECT_TRUE(stats["error"].toString().contains(QLatin1String("Scripts.cpuBudget"))) << stats["error"].toString().toStdString();
    // Its run ended with the disable, and said why.
    EXPECT_EQ(client.status, 1);
    EXPECT_TRUE(client.err.contains(QLatin1String("Scripts.cpuBudget"))) << client.err.toStdString();
}

TEST_F(ScriptHostTest, ARemoteRunReadsTextAndTakesOtherOutputOnlyWithAllowRemoteReadText)
{
    WRITE(user, "far.luau", R"(
local runs = {}
return {
    metadata = { commands = { look = { summary = "Look", remote = true } } },
    run_command = function(command, ev)
        table.insert(runs, ev.run)
        local _, err = afterglow.await(afterglow.read_text(ev.pane, "screen"))
        if err then afterglow.await(afterglow.output_error(ev, err.message .. "\n")) end
        afterglow.sleep(100)
        return nil
    end,
    -- Another handler, which could read the text that the run may not.
    on_bell = function() return afterglow.request("leak", afterglow.output(runs[#runs], "leak\n")) end,
})");
    host->setEnabled(true);
    const auto look = [&](bool allowed) {
        ScriptHost::CommandLimits limits;
        limits.remoteReadText = allowed;
        host->setCommandLimits(limits);
        rec.applied.clear();
        rec.results.clear();
        Client& client = newClient();
        EXPECT_NE(startRequest({"far", "look", {}, "/home/far", 7, std::nullopt, "remote", "box"}, client), 0u);
        pump();
        host->dispatch("on_bell", 7);
        EXPECT_TRUE(await([&] { return client.status.has_value(); }));
        return &client;
    };
    const Client* refused = look(false);
    EXPECT_TRUE(refused->err.contains(QLatin1String("Control.allowRemoteReadText"))) << refused->err.toStdString();
    EXPECT_FALSE(refused->out.contains(QLatin1String("leak")));
    EXPECT_FALSE(std::any_of(rec.applied.cbegin(), rec.applied.cend(), [](const QVariantMap& e) { return e["kind"] == "read_text"; }));
    EXPECT_TRUE(std::any_of(rec.results.cbegin(), rec.results.cend(), [](const auto& r) {
        return r.first["kind"] == "output" && r.second.contains(QLatin1String("Control.allowRemoteReadText"));
    }));
    const Client* allowed = look(true);
    EXPECT_TRUE(allowed->err.isEmpty()) << allowed->err.toStdString();
    EXPECT_EQ(allowed->out, QStringLiteral("leak\n"));
    EXPECT_TRUE(std::any_of(rec.applied.cbegin(), rec.applied.cend(), [](const QVariantMap& e) {
        return e["kind"] == "read_text" && e["client_run"].toBool();
    }));
}

TEST_F(ScriptHostTest, ARemoteRunWithNoDirectoryGetsNoneAndARemoteHostDoesNotSeeLocalScripts)
{
    WRITE(user, "where.luau", R"(return {
    metadata = { commands = { here = { summary = "Here" }, there = { summary = "There", remote = true } } },
    run_command = function(command, ev) return afterglow.output(ev, "[" .. ev.cwd .. "]\n") end,
})");
    WRITE(user, "local.luau", R"(return {
    metadata = { commands = { go = { summary = "Go" } } },
    run_command = function() error("boom") end,
})");
    paneState[7]["cwd"] = QStringLiteral("/local/dir");
    host->setEnabled(true);
    Client& remote = newClient();
    ASSERT_NE(startRequest({"where", "there", {}, {}, 7, std::nullopt, "remote", "box"}, remote), 0u);
    EXPECT_EQ(remote.out, QStringLiteral("[]\n")); // not the pane's local directory
    Client& shell = newClient();
    ASSERT_NE(startRequest({"where", "here", {}, {}, 7, std::nullopt, "shell", {}}, shell), 0u);
    EXPECT_EQ(shell.out, QStringLiteral("[/local/dir]\n"));
    // A directory that no path has.
    Client& longer = newClient();
    QString error;
    EXPECT_EQ(startRequest({"where", "there", {}, QString(ScriptHost::MaxCommandCwdLength + 1, QLatin1Char('a')), 7, std::nullopt,
                            "remote", "box"}, longer, &error), 0u);
    EXPECT_TRUE(error.contains(QLatin1String("directory"))) << error.toStdString();
    // A disabled script with no remote command does not exist for a remote host.
    for (int i = 0; i < ScriptHost::MaxFailures && !host->isDisabled("local"); ++i) ASSERT_TRUE(run("local.go", 7));
    ASSERT_TRUE(host->isDisabled("local"));
    Client& probe = newClient();
    bool unknown = false;
    EXPECT_EQ(startRequest({"local", "go", {}, {}, 7, std::nullopt, "remote", "box"}, probe, &error, &unknown), 0u);
    EXPECT_TRUE(unknown);
    EXPECT_FALSE(error.contains(QLatin1String("disabled"))) << error.toStdString();
}

TEST_F(ScriptHostTest, OnUnloadWritesStateAndLogsButNothingElse)
{
    WRITE(user, "saver.luau", R"(return {
    on_unload = function()
        return { afterglow.state_write({ saved = true }, 0), afterglow.log("bye"), afterglow.copy("refused") }
    end,
})");
    host->setEnabled(true);
    WRITE(user, "saver.luau", "return {}");
    host->reload();
    pump();
    ScriptState::Snapshot stored;
    ASSERT_TRUE(ScriptState(stateDir.path()).read(QStringLiteral("saver"), stored).isEmpty());
    EXPECT_EQ(stored.value.value(QStringLiteral("saved")), true);
    EXPECT_TRUE(rec.applied.isEmpty()); // the copy never reached the window
    EXPECT_TRUE(std::any_of(rec.results.cbegin(), rec.results.cend(), [](const auto& r) {
        return r.first["kind"] == "copy" && r.second.contains(QLatin1String("on_unload"));
    }));
    // The log applied: no refusal.
    EXPECT_TRUE(std::any_of(rec.results.cbegin(), rec.results.cend(), [](const auto& r) {
        return r.first["kind"] == "log" && r.first["message"] == "bye" && r.second.isEmpty();
    }));
}

TEST_F(ScriptHostTest, ALooseScriptCannotCloseTheAwaitWrapper)
{
    // The source ends the wrapper's function and returns a table of its own,
    // whose __index the host's lookups would run. That __index gives a
    // handler, not an error: an error there, outside a protected call,
    // would abort the test binary instead of failing this test.
    WRITE(user, "escape.luau",
          R"(end)(...)) and nil or setmetatable({ on_bell = function() return afterglow.copy("raw") end }, { __index = function() return function() return afterglow.copy("raw lookup") end end }), ((function(...))");
    host->setEnabled(true);
    ASSERT_FALSE(host->scriptIds().contains(QStringLiteral("escape")));
    bool failed = false;
    for (const QVariant& d : host->diagnostics())
        failed = failed || (d.toMap()["path"].toString().endsWith(QLatin1String("escape.luau")) && d.toMap()["status"] == "failed");
    EXPECT_TRUE(failed);
    host->dispatch("on_bell", 7);
    EXPECT_TRUE(rec.applied.isEmpty());
}

TEST_F(ScriptHostTest, AScriptCannotResumeItsWaitingHandlerWithAResultOfItsOwn)
{
    WRITE(user, "forge.luau", R"(
local waiting
return {
    metadata = { commands = { ask = { summary = "Ask" } } },
    run_command = function(command, ev)
        waiting = coroutine.running()
        local data = afterglow.await(afterglow.prompt(ev.pane, "Name"))
        return afterglow.output(ev, (data and data.text or "none") .. "\n")
    end,
    on_bell = function()
        local ok, err = coroutine.resume(waiting, { text = "forged" })
        local closed = coroutine.close(waiting)
        return afterglow.log(tostring(ok) .. " " .. tostring(closed) .. " " .. tostring(err))
    end,
})");
    host->setEnabled(true);
    Client& client = newClient();
    ASSERT_NE(start(QStringLiteral("forge"), QStringLiteral("ask"), client), 0u);
    ASSERT_TRUE(pendingReply);
    host->dispatch("on_bell", 7);
    const auto log = std::find_if(rec.results.cbegin(), rec.results.cend(), [](const auto& r) { return r.first["kind"] == "log"; });
    ASSERT_NE(log, rec.results.cend());
    EXPECT_TRUE(log->first["message"].toString().startsWith(QLatin1String("false false"))) << log->first["message"].toString().toStdString();
    EXPECT_FALSE(client.status.has_value());
    std::exchange(pendingReply, {})({{}, {{QStringLiteral("text"), QStringLiteral("real")}}, {}});
    ASSERT_TRUE(await([&] { return client.status.has_value(); }));
    EXPECT_EQ(client.out, QStringLiteral("real\n"));
}

TEST_F(ScriptHostTest, AModuleThatWaitsWhileItLoadsFailsWithoutLeavingACycle)
{
    QDir().mkpath(user.filePath("lazy"));
    WRITE(user, "lazy/package.json", R"({"id":"lazy","version":"1.0.0","api_version":1,"entry":"main",
        "modules":{"main":"init.luau","slow":"slow.luau"},"commands":{"go":{"summary":"Go"}}})");
    WRITE(user, "lazy/init.luau", R"(return { run_command = function(command, ev)
    local slow = require("slow")
    return afterglow.output(ev, slow.name .. "\n")
end })");
    WRITE(user, "lazy/slow.luau", R"(afterglow.await(afterglow.sleep(50)) return { name = "slow" })");
    host->setEnabled(true);
    Client& first = newClient();
    Client& second = newClient();
    ASSERT_NE(start(QStringLiteral("lazy"), QStringLiteral("go"), first), 0u);
    ASSERT_NE(start(QStringLiteral("lazy"), QStringLiteral("go"), second), 0u);
    ASSERT_TRUE(await([&] { return first.status.has_value() && second.status.has_value(); }));
    for (const Client* client : {&first, &second}) {
        EXPECT_EQ(client->status, 1);
        EXPECT_FALSE(client->err.contains(QLatin1String("cycle"))) << client->err.toStdString();
        EXPECT_TRUE(client->err.contains(QLatin1String("slow"))) << client->err.toStdString();
    }
}

TEST_F(ScriptHostTest, ADirectoryListedTwiceDoesNotSupersedeItsOwnScripts)
{
    WRITE(user, "twice.luau", "return {}");
    host->setSearchPaths({user.path(), system.path(), user.path() + QStringLiteral("/")});
    host->setEnabled(true);
    EXPECT_TRUE(host->scriptIds().contains(QStringLiteral("twice")));
    for (const QVariant& d : host->diagnostics()) EXPECT_NE(d.toMap()["status"], "superseded") << d.toMap()["path"].toString().toStdString();
}

TEST_F(ScriptHostTest, ADanglingScriptLinkIsDiagnosedAndLoadsWhenItsTargetAppears)
{
    const QString target = files.filePath(QStringLiteral("later.luau"));
    ASSERT_TRUE(QFile::link(target, user.filePath(QStringLiteral("later.luau"))));
    host->setEnabled(true);
    bool failed = false;
    for (const QVariant& d : host->diagnostics())
        failed = failed || (d.toMap()["path"].toString().endsWith(QLatin1String("later.luau")) && d.toMap()["status"] == "failed"
                            && d.toMap()["error"].toString().contains(QLatin1String("does not exist")));
    EXPECT_TRUE(failed);
    WRITE(files, "later.luau", "return {}");
    // The watch of the target's directory sees it appear.
    EXPECT_TRUE(await([&] { return host->scriptIds().contains(QStringLiteral("later")); }, 5000));
}

TEST_F(ScriptHostTest, OverridesAndTaskNamesRefuseValuesOfAnotherType)
{
    WRITE(user, "types.luau", R"(return { on_bell = function(ev)
    return { afterglow.override(ev.pane, 5), afterglow.timer(7, 1000) }
end })");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_EQ(rec.results.size(), 2);
    EXPECT_EQ(rec.results[0].second, QStringLiteral("Overrides must be a table of names to values"));
    EXPECT_EQ(rec.results[1].second, QStringLiteral("task name must contain 1 to 64 characters"));
    EXPECT_TRUE(rec.applied.isEmpty());
}

TEST_F(ScriptHostTest, ACancelledDialogIsNoEffectErrorAndADisableKeepsItsReason)
{
    WRITE(user, "ask.luau", R"(return {
    metadata = { commands = { ask = { summary = "Ask" }, fail = { summary = "Fail" } } },
    run_command = function(command, ev)
        if command == "fail" then error("boom") end
        afterglow.await(afterglow.prompt(ev.pane, "Name"))
        return nil
    end,
    -- A dialog of no run: the end of a run does not close it.
    on_bell = function(ev) return afterglow.request("later", afterglow.prompt(ev.pane, "Later")) end,
})");
    host->setEnabled(true);
    ASSERT_TRUE(run("ask.ask", 7));
    ASSERT_TRUE(pendingReply);
    std::exchange(pendingReply, {})({QStringLiteral("Dialog canceled"), {}, QStringLiteral("cancelled")});
    pump();
    EXPECT_EQ(firstDiagnostic()["effect_errors"].toULongLong(), 0u);
    // A dialog that fails after the disable: the disable and the last error
    // stay those of the failures.
    host->dispatch("on_bell", 7);
    ASSERT_TRUE(pendingReply);
    for (int i = 0; i < ScriptHost::MaxFailures && !host->isDisabled("ask"); ++i) ASSERT_TRUE(run("ask.fail", 7));
    ASSERT_TRUE(host->isDisabled("ask"));
    // This harness's cleanup only records the owner, so the dialog's reply
    // can still come, as it can from a window that answers late. The
    // window's cleanup would close it.
    ASSERT_TRUE(pendingReply);
    std::exchange(pendingReply, {})({QStringLiteral("Dialog failed"), {}, QStringLiteral("operation_failed")});
    pump();
    const auto stats = firstDiagnostic();
    EXPECT_TRUE(stats["error"].toString().contains(QLatin1String("failures in a row"))) << stats["error"].toString().toStdString();
    EXPECT_TRUE(stats["last_error"].toString().contains(QLatin1String("boom"))) << stats["last_error"].toString().toStdString();
    EXPECT_EQ(stats["effect_errors"].toULongLong(), 1u); // the failed dialog still counts
}

TEST_F(ScriptHostTest, AnEventStormThatTheBudgetDropsDoesNotDisableTheScript)
{
    // Each on_title uses the budget quickly, as a program that sets its
    // title in a loop makes it. Dropping the extra calls holds the script to
    // its budget: that is no reason to disable it.
    WRITE(user, "titles.luau", R"(return { on_title = function()
    local t = os.clock() while os.clock() - t < 0.015 do end
    return nil
end })");
    host->setCpuBudget(40);
    host->setEnabled(true);
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 6500) {
        host->dispatch("on_title", 7, {{QStringLiteral("title"), QStringLiteral("t")}});
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    EXPECT_FALSE(host->isDisabled(QStringLiteral("titles")));
    EXPECT_GT(firstDiagnostic()["dropped_callbacks"].toULongLong(), 0u);
}

TEST_F(ScriptHostTest, AOneShotTimerPastTheBudgetWaitsInsteadOfBeingDropped)
{
    WRITE(user, "late.luau", R"(return {
    on_bell = function()
        local t = os.clock() while os.clock() - t < 0.015 do end
        return afterglow.timer("once", 1)
    end,
    on_timer = function(ev) return afterglow.copy(ev.name) end,
})");
    host->setCpuBudget(10);
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_TRUE(await([&] { return !rec.applied.isEmpty(); }, 4000));
    EXPECT_EQ(rec.applied.first().value("text"), "once");
    EXPECT_GE(firstDiagnostic()["delayed_callbacks"].toULongLong(), 1u);
}

TEST_F(ScriptHostTest, ARepeatingTimerPastTheBudgetWaitsWithOneCallNotOneForEachTick)
{
    // The timer ticks each 250 ms while the burn of on_bell keeps the script
    // past its budget for a second. The ticks that come while its call waits
    // add no call: once time returns, on_timer runs once. Its first call
    // cancels the timer, so a second call could come only from a tick that
    // waited.
    WRITE(user, "late.luau", R"(return {
    on_bell = function()
        local t = os.clock() while os.clock() - t < 0.015 do end
        return afterglow.timer("tick", 1, 250)
    end,
    on_timer = function(ev) return { afterglow.cancel(ev.name), afterglow.copy(ev.name) } end,
})");
    host->setCpuBudget(10);
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    const auto copies = [&] {
        return std::count_if(rec.applied.cbegin(), rec.applied.cend(), [](const QVariantMap& e) { return e["kind"] == "copy"; });
    };
    ASSERT_TRUE(await([&] { return copies() > 0; }, 4000));
    await([] { return false; }, 500);
    EXPECT_EQ(copies(), 1);
    const auto stats = firstDiagnostic();
    EXPECT_GE(stats["delayed_callbacks"].toULongLong(), 1u);
    // Each tick that came while the call waited.
    EXPECT_GE(stats["dropped_callbacks"].toULongLong(), 2u);
}

TEST_F(ScriptHostTest, ARemoteReadWithABadLimitStaysAnInvalidEffect)
{
    // The remote gate does not hide the first refusal, a bad max_bytes.
    WRITE(user, "far.luau", R"(return {
    metadata = { commands = { look = { summary = "Look", remote = true } } },
    run_command = function(command, ev)
        local _, err = afterglow.await(afterglow.read_text(ev.pane, "screen", 0))
        return afterglow.output(ev, err.code .. "\n")
    end,
})");
    host->setEnabled(true);
    Client& client = newClient();
    ASSERT_NE(startRequest({"far", "look", {}, "/home/far", 7, std::nullopt, "remote", "box"}, client), 0u);
    ASSERT_TRUE(await([&] { return client.status.has_value(); }));
    EXPECT_EQ(client.out, QStringLiteral("invalid_effect\n"));
}

TEST_F(ScriptHostTest, ACommandRunThatWaitedForCpuTimeGetsTheStateOfItsStart)
{
    WRITE(user, "late.luau", R"(return {
    metadata = { commands = { title = { summary = "Title" } } },
    on_bell = function() local t = os.clock() while os.clock() - t < 0.015 do end return nil end,
    run_command = function(command, ev) return afterglow.output(ev, ev.pane.title .. "\n") end,
})");
    host->setCpuBudget(10);
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    Client& client = newClient();
    ASSERT_NE(start(QStringLiteral("late"), QStringLiteral("title"), client), 0u);
    EXPECT_FALSE(client.status.has_value()); // it waits for CPU time
    paneState[7]["title"] = QStringLiteral("later");
    ASSERT_TRUE(await([&] { return client.status.has_value(); }, 4000));
    EXPECT_EQ(client.out, QStringLiteral("later\n"));
}

TEST_F(ScriptHostTest, OutputToAClientThatLeftIsNoEffectError)
{
    WRITE(user, "gone.luau", R"(return {
    metadata = { commands = { say = { summary = "Say" } } },
    run_command = function(command, ev) afterglow.await(afterglow.output(ev, "x")) return nil end,
})");
    host->setEnabled(true);
    Client& client = newClient();
    client.gone = true;
    start(QStringLiteral("gone"), QStringLiteral("say"), client);
    ASSERT_TRUE(await([&] { return client.status.has_value(); }));
    pump();
    EXPECT_EQ(firstDiagnostic()["effect_errors"].toULongLong(), 0u);
}

TEST_F(ScriptHostTest, ADisabledScriptShowsTheLoadErrorOfANewerEdit)
{
    WRITE(user, "flaky.luau", R"(return { metadata = { commands = { go = { summary = "Go" } } }, run_command = function() error("boom") end })");
    host->setEnabled(true);
    for (int i = 0; i < ScriptHost::MaxFailures && !host->isDisabled("flaky"); ++i) ASSERT_TRUE(run("flaky.go", 7));
    ASSERT_TRUE(host->isDisabled("flaky"));
    WRITE(user, "flaky.luau", "return {");
    host->reload();
    const auto d = firstDiagnostic();
    EXPECT_EQ(d["status"], "disabled");
    EXPECT_TRUE(d["error"].toString().contains(QLatin1String("flaky.luau"))) << d["error"].toString().toStdString();
    EXPECT_TRUE(d["disabled_reason"].toString().contains(QLatin1String("failures in a row")));
}

int main(int argc, char** argv)
{
    // ScriptHost reads the standard locations as it starts: never the user's own.
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
