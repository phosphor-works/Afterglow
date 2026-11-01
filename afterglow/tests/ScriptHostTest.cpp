// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "scripting/ScriptHost.h"
#include "scripting/ScriptTasks.h"
#include "scripting/ScriptState.h"
#include <QDateTime>
#include <QElapsedTimer>
#include <QProcess>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QThread>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <signal.h>
#include <algorithm>
#include <cerrno>
#include <sys/stat.h>

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

using namespace Afterglow;

namespace {

struct Recorder
{
    QList<QVariantMap> applied;
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
        c.failed = [this](const QString& owner) { failedOwners << owner; };
        c.applyOwned = [this, apply = c.apply](const QString&, const QVariantMap& effect, std::function<void(ScriptHost::EffectResult)> complete) {
            if (effect.value(QStringLiteral("kind")) == QLatin1String("prompt")) {
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
        ASSERT_TRUE(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write(source);
    }

    void installPackage(const QString& name)
    {
        const QDir package(QStringLiteral(AFTERGLOW_EXAMPLE_PACKAGES) + "/" + name);
        QDirIterator files(package.path(), QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString source = files.next();
            const QString target = user.filePath(name + "/" + package.relativeFilePath(source));
            ASSERT_TRUE(QDir().mkpath(QFileInfo(target).absolutePath()));
            ASSERT_TRUE(QFile::copy(source, target));
        }
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
    std::unique_ptr<ScriptHost> host;
};

} // namespace

TEST_F(ScriptHostTest, LoadsMetadataAndActions)
{
    write(user, QStringLiteral("hello.luau"), R"(
local s = {}
s.metadata = { id = "greeter", name = "Greeter", actions = { { id = "hi", title = "Say hi" }, { id = "bad id!" } } }
function s.run_action(action, ev) return nil end
return s
)");
    write(user, QStringLiteral("afterglow.d.luau"), "this is not code");
    host->setEnabled(true);
    EXPECT_EQ(host->scriptIds(), QStringList{QStringLiteral("greeter")});
    // The declarations file is not loaded, so it is not even a failure.
    EXPECT_EQ(host->diagnostics().size(), 1);
    const auto actions = host->actions();
    ASSERT_EQ(actions.size(), 1);
    EXPECT_EQ(actions[0].token, QStringLiteral("greeter.hi"));
    EXPECT_EQ(actions[0].title, QStringLiteral("Say hi"));
}

TEST_F(ScriptHostTest, UserScriptReplacesSystemScript)
{
    write(system, QStringLiteral("a.luau"), "return { metadata = { id = 'same', actions = { { id = 'sys' } } }, run_action = function() end }");
    write(user, QStringLiteral("b.luau"), "return { metadata = { id = 'same', actions = { { id = 'user' } } }, run_action = function() end }");
    host->setEnabled(true);
    ASSERT_EQ(host->actions().size(), 1);
    EXPECT_EQ(host->actions()[0].token, QStringLiteral("same.user"));
}

TEST_F(ScriptHostTest, BrokenScriptsAreSkipped)
{
    write(user, QStringLiteral("syntax.luau"), "return {");
    write(user, QStringLiteral("nothing.luau"), "return 42");
    write(user, QStringLiteral("fine.luau"), "return {}");
    host->setEnabled(true);
    EXPECT_EQ(host->scriptIds(), QStringList{QStringLiteral("fine")});
}

TEST_F(ScriptHostTest, EventEffectsDefaultToTheEventPane)
{
    write(user, QStringLiteral("n.luau"), R"(
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

TEST_F(ScriptHostTest, ActionsReturnEffectLists)
{
    write(user, QStringLiteral("a.luau"), R"(
return {
    metadata = { actions = { { id = "go" } } },
    run_action = function(action, ev)
        return {
            afterglow.split(ev.pane, "down", "htop"),
            afterglow.send_text(ev.pane, "echo " .. action .. "\n"),
            afterglow.log("ran " .. action),
            { kind = "format_disk" },
            afterglow.action(ev.pane, "script:a.go"),
            afterglow.split(ev.pane, "sideways"),
        }
    end,
}
)");
    host->setEnabled(true);
    EXPECT_FALSE(host->runAction(QStringLiteral("a.nope"), 7));
    ASSERT_TRUE(host->runAction(QStringLiteral("a.go"), 7));
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

TEST_F(ScriptHostTest, PaneEffectsNeedAPane)
{
    write(user, QStringLiteral("a.luau"), R"(
return {
    metadata = { actions = { { id = "go" } } },
    run_action = function(action, ev) return afterglow.close(ev.pane) end,
}
)");
    host->setEnabled(true);
    ASSERT_TRUE(host->runAction(QStringLiteral("a.go"), 0));
    EXPECT_TRUE(rec.applied.isEmpty());
    ASSERT_EQ(rec.results.size(), 1);
    EXPECT_FALSE(rec.results[0].second.isEmpty());
}

TEST_F(ScriptHostTest, StructuralEffectsAreRateLimited)
{
    write(user, QStringLiteral("a.luau"), R"(
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
    write(user, QStringLiteral("loop.luau"), R"(
return {
    metadata = { actions = { { id = "spin" } } },
    on_bell = function(ev) while true do end end,
    run_action = function() error("nope") end,
}
)");
    host->setEnabled(true);
    ASSERT_EQ(host->actions().size(), 1);
    for (int i = 0; i < ScriptHost::MaxFailures - 1; ++i) {
        host->dispatch(QStringLiteral("on_bell"), 7); // times out
    }
    EXPECT_FALSE(host->isDisabled(QStringLiteral("loop")));
    host->runAction(QStringLiteral("loop.spin"), 7);
    EXPECT_TRUE(host->isDisabled(QStringLiteral("loop")));
    EXPECT_TRUE(host->actions().isEmpty());

    // Still disabled while the file is unchanged; an edit brings it back.
    host->reload();
    EXPECT_TRUE(host->isDisabled(QStringLiteral("loop")));
    write(user, QStringLiteral("loop.luau"), "return { metadata = { actions = { { id = 'spin' } } }, run_action = function() end }");
    host->reload();
    EXPECT_FALSE(host->isDisabled(QStringLiteral("loop")));
    EXPECT_EQ(host->actions().size(), 1);
}

TEST_F(ScriptHostTest, ScriptsAreSandboxed)
{
    write(user, QStringLiteral("s.luau"), R"(
return {
    on_bell = function(ev)
        local ok = pcall(function() afterglow.notify = nil end)
        local found = (io ~= nil) or (os ~= nil and os.execute ~= nil) or (require ~= nil) or (loadstring ~= nil)
        return afterglow.log((ok and "writable" or "frozen") .. " " .. (found and "escaped" or "contained"))
    end,
}
)");
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_bell"), 7);
    ASSERT_EQ(rec.results.size(), 1);
    EXPECT_EQ(rec.results[0].first.value(QStringLiteral("message")), QStringLiteral("frozen contained"));
}

TEST_F(ScriptHostTest, DisablingUnloads)
{
    write(user, QStringLiteral("a.luau"), "return { metadata = { actions = { { id = 'x' } } }, run_action = function() end }");
    host->setEnabled(true);
    EXPECT_EQ(host->actions().size(), 1);
    host->setEnabled(false);
    EXPECT_TRUE(host->scriptIds().isEmpty());
    EXPECT_FALSE(host->runAction(QStringLiteral("a.x"), 7));
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
    write(user, "semantic-regions.luau", example.readAll());
    applyHook = [](const QVariantMap& effect) -> ScriptHost::EffectResult {
        if (effect["kind"] == QLatin1String("semantic_regions"))
            return {{}, {{"regions", QVariantList{QVariantMap{{"id", "42"}, {"kind", "output"}, {"available", true}}}}}, {}};
        if (effect["kind"] == QLatin1String("select")) return {{}, {{"id", "42"}}, {}};
        if (effect["kind"] == QLatin1String("read_region"))
            return {{}, {{"text", "command output\n"}, {"available", true}, {"truncated", false}}, {}};
        return {};
    };
    host->setEnabled(true);
    ASSERT_TRUE(host->runAction("semantic-regions.copy", 7));
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    ASSERT_EQ(rec.applied.size(), 4);
    EXPECT_EQ(rec.applied[1]["items"].toList().front().toMap()["id"].toString(), "42");
    EXPECT_EQ(rec.applied[2]["region"].toString(), "42");
    EXPECT_EQ(rec.applied[2]["max_bytes"].toInt(), 262144);
    EXPECT_EQ(rec.applied[3]["kind"].toString(), "copy");
    EXPECT_EQ(rec.applied[3]["text"].toString(), "command output\n");
    ASSERT_TRUE(host->runAction("semantic-regions.copy", 7));
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    EXPECT_EQ(rec.applied.size(), 8);
}

TEST_F(ScriptHostTest, SemanticRegionExampleReportsARefusedSelector)
{
    QFile example(QStringLiteral(AFTERGLOW_EXAMPLE_SCRIPTS) + QStringLiteral("/semantic-regions.luau"));
    ASSERT_TRUE(example.open(QIODevice::ReadOnly));
    write(user, "semantic-regions.luau", example.readAll());
    QString selectCode = "busy";
    applyHook = [&](const QVariantMap& effect) -> ScriptHost::EffectResult {
        if (effect["kind"] == QLatin1String("semantic_regions"))
            return {{}, {{"regions", QVariantList{QVariantMap{{"id", "42"}, {"kind", "output"}, {"available", true}}}}}, {}};
        if (effect["kind"] == QLatin1String("select")) return {"Another dialog is open", {}, selectCode};
        return {};
    };
    host->setEnabled(true);
    ASSERT_TRUE(host->runAction("semantic-regions.copy", 7));
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    ASSERT_EQ(rec.applied.size(), 3);
    EXPECT_EQ(rec.applied[2]["kind"].toString(), "notify");
    EXPECT_EQ(rec.applied[2]["body"].toString(), "Another dialog is open");
    // Escape is not an error.
    selectCode = "cancelled";
    rec.applied.clear();
    ASSERT_TRUE(host->runAction("semantic-regions.copy", 7));
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    ASSERT_EQ(rec.applied.size(), 2);
    EXPECT_EQ(rec.applied[1]["kind"].toString(), "select");
}

TEST_F(ScriptHostTest, SignalEffects)
{
    write(user, QStringLiteral("s.luau"), R"(
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
    EXPECT_EQ(host->actions().size(), 4);
    ASSERT_TRUE(host->runAction(QStringLiteral("glasshouse.strike"), 7));
    EXPECT_EQ(signalsSent(), (Sent{{0, 1.0}}));
}

TEST_F(ScriptHostTest, ContextualSceneSamplesTransitionsAndCleansUp)
{
    installPackage("contextual-scene");
    applyHook=[](const QVariantMap& e) -> ScriptHost::EffectResult {
        if(e.value("kind")=="scene_state") return {{}, {{"scene",QVariantMap{{"tab_id",10},{"history_normalized",.5},{"audio_available",true},{"overall",.75}}}}, {}};
        if(e.value("kind")=="shaders") return {{}, {{"shaders",QVariantList{QVariantMap{{"shader","phosphor-field"},{"instance","field"},{"layer","background"}}}}}, {}};
        return {};
    };
    host->setEnabled(true);
    ASSERT_TRUE(host->runAction("contextual-scene.toggle",7));
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
    ASSERT_TRUE(host->runAction("contextual-scene.toggle",7));
    bool reset=false;
    for(const auto& e:std::as_const(rec.applied)) if(e.value("kind")=="shader_parameters") {
        reset=true; EXPECT_TRUE(e.value("values").toMap().isEmpty()); EXPECT_EQ(e.value("scope"),"window");
    }
    EXPECT_TRUE(reset);
    host->setEnabled(false);
    for (const auto& result : rec.results) EXPECT_TRUE(result.second.isEmpty()) << result.second.toStdString();
}

TEST_F(ScriptHostTest, PhosphorReactsToCommandsAndDemoActions)
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
    for(int i=0;i<64;++i) host->dispatch("on_focus", i%2 ? 7 : 8);
    EXPECT_TRUE(rec.applied.isEmpty());
    host->dispatch("on_command_finished",7,{{"exit_code",0},{"duration_ms",2000}});
    EXPECT_EQ(rec.applied.size(),2);
    for(const auto& result:rec.results) EXPECT_TRUE(result.second.isEmpty()) << result.second.toStdString();
}

TEST_F(ScriptHostTest, PhosphorDemoActionsAndStatus)
{
    host->setSearchPaths({QStringLiteral(AFTERGLOW_PHOSPHOR_SCRIPTS)});
    host->setEnabled(true);
    for (const auto& [action, channel] : std::initializer_list<std::pair<const char*, int>>{
             {"strike", 0}, {"complete", 1}, {"ring", 3}, {"heat", 2}}) {
        ASSERT_TRUE(host->runAction(QStringLiteral("phosphor.") + QString::fromLatin1(action), 7));
        ASSERT_EQ(rec.applied.size(), channel == 2 ? 2 : 1);
        const auto value = rec.applied[0].value(QStringLiteral("values")).toList().first().toMap();
        EXPECT_EQ(value.value(QStringLiteral("channel")).toInt(), channel);
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
    ASSERT_TRUE(host->runAction(QStringLiteral("phosphor.complete"), 7));
    ASSERT_EQ(rec.applied.size(), 1);
    EXPECT_EQ(rec.applied[0].value(QStringLiteral("pane")).toUInt(), 7u);
    EXPECT_EQ(rec.applied[0].value(QStringLiteral("values")).toList().first().toMap().value(QStringLiteral("channel")).toInt(), 1);
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
    write(user, QStringLiteral("state.luau"), R"(
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
    write(user, QStringLiteral("creator.luau"), R"(
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
    write(user, QStringLiteral("other.luau"), R"(
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
    write(user, QStringLiteral("errors.luau"), R"(
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
    write(user, QStringLiteral("stale.luau"), R"(
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
    write(user, QStringLiteral("pending.luau"), source);
    host->setEnabled(true);
    host->dispatch(QStringLiteral("on_bell"), 7);
    write(user, QStringLiteral("pending.luau"), source + "\n-- new instance\n");
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
    write(user, QStringLiteral("budget.luau"), R"(
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
    write(user, QStringLiteral("batch.luau"), R"(
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
    write(user, QStringLiteral("loop.luau"), R"(
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
    write(user, QStringLiteral("large.luau"), R"(
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
QVariantMap job(const QString& name, const QStringList& argv)
{
    return {{QStringLiteral("kind"), QStringLiteral("exec")}, {QStringLiteral("name"), name},
            {QStringLiteral("command"), argv}, {QStringLiteral("timeout_ms"), 1000}, {QStringLiteral("max_bytes"), 128}};
}
}

TEST(ScriptTasksTest, ProcessesReportSuccessFailureLimitsAndCancellationOnce)
{
    ScriptTasks tasks;
    QMap<QString, QList<ScriptHost::EffectResult>> results;
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
    EXPECT_TRUE(start(job("limit", {"/bin/sh", "-c", "while :; do printf xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx; done"})).deferred);
    EXPECT_TRUE(start(job("cancel", {"/bin/sleep", "10"})).deferred);
    EXPECT_TRUE(tasks.apply("owner", {{"kind", "cancel"}, {"name", "cancel"}}, {}, {}).error.isEmpty());
    ASSERT_TRUE(await([&] { return results.size() == 6; }));
    EXPECT_EQ(results["ok"][0].data["stdout"], QStringLiteral("hello"));
    EXPECT_EQ(results["ok"][0].data["stderr"], QStringLiteral("warning"));
    EXPECT_TRUE(results["ok"][0].error.isEmpty());
    EXPECT_EQ(results["bad"][0].code, QStringLiteral("process_failed"));
    EXPECT_EQ(results["bad"][0].data["exit_code"], 7);
    EXPECT_EQ(results["spawn"][0].code, QStringLiteral("spawn_failed"));
    EXPECT_EQ(results["timeout"][0].code, QStringLiteral("timeout"));
    EXPECT_EQ(results["limit"][0].code, QStringLiteral("output_limit"));
    EXPECT_LE(results["limit"][0].data["stdout"].toString().size(), 128);
    EXPECT_EQ(results["cancel"][0].code, QStringLiteral("cancelled"));
    // The killed processes report finished later: that must not complete them again.
    await([] { return false; }, 200);
    for (const auto& values : results) EXPECT_EQ(values.size(), 1);
}

TEST(ScriptTasksTest, FileReadsAreBoundedAndRejectSpecialFiles)
{
    QTemporaryDir dir;
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
    ScriptTasks tasks;
    int callbacks = 0;
    EXPECT_TRUE(tasks.apply("owner", {{"kind", "timer"}, {"name", "tick"}, {"delay_ms", 5}, {"repeat_ms", 250}},
        {}, [&](auto, auto) { ++callbacks; }).error.isEmpty());
    ASSERT_TRUE(await([&] { return callbacks == 1; }));
    EXPECT_TRUE(tasks.apply("owner", job("job", {"/bin/sleep", "5"}), [&](auto) { ++callbacks; }, {}).deferred);
    tasks.clear("owner");
    await([] { return false; }, 300);
    EXPECT_EQ(callbacks, 1);
}

TEST(ScriptTasksTest, WatchFileRefusesThePathsThatReadFileRefuses)
{
    QTemporaryDir dir;
    ScriptTasks tasks;
    const QString tooLong = dir.path() + "/" + QString(4096, 'x');
    const QString withNul = dir.path() + QChar(0) + "watch";
    for (const QString& path : {QStringLiteral("relative"), tooLong, withNul}) {
        for (const char* kind : {"watch_file", "read_file"}) {
            const auto result = tasks.apply("owner", {{"kind", QString::fromLatin1(kind)}, {"name", "watch"}, {"path", path}}, {}, {});
            EXPECT_EQ(result.code, "invalid_effect") << kind;
            EXPECT_TRUE(result.error.contains("at most 4096 characters")) << kind << ' ' << result.error.toStdString();
        }
    }
}

TEST(ScriptTasksTest, WatchSurvivesAtomicFileReplacement)
{
    QTemporaryDir dir;
    const auto path = dir.filePath("watch");
    { QFile f(path); ASSERT_TRUE(f.open(QIODevice::WriteOnly)); f.write("old"); }
    ScriptTasks tasks;
    int changes = 0;
    EXPECT_TRUE(tasks.apply("owner", {{"kind", "watch_file"}, {"name", "watch"}, {"path", path}}, {},
        [&](const QString& handler, const QVariantMap& event) {
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

TEST_F(ScriptHostTest, LifecycleTimersJobsAndDynamicActions)
{
    write(user, "lifecycle.luau", R"(
return {
    on_load = function()
        return {afterglow.copy("loaded"), afterglow.timer("tick", 1),
                afterglow.exec("job", {"/bin/sh", "-c", "printf hello"}),
                afterglow.set_actions({{id="dynamic",title="Dynamic"}})}
    end,
    on_timer = function(ev) return afterglow.copy(ev.name) end,
    on_result = function(ev)
        assert(ev.ok)
        return afterglow.copy(ev.data.stdout)
    end,
    run_action = function() return afterglow.copy("action") end,
}
)");
    host->setEnabled(true);
    ASSERT_TRUE(await([&] { return rec.applied.size() == 3; }));
    QStringList text;
    for (const auto& e : rec.applied) text << e["text"].toString();
    EXPECT_TRUE(text.contains("loaded")); EXPECT_TRUE(text.contains("tick")); EXPECT_TRUE(text.contains("hello"));
    ASSERT_EQ(host->actions().size(), 1);
    EXPECT_EQ(host->actions()[0].token, QStringLiteral("lifecycle.dynamic"));
    host->reload(); // unchanged source keeps its instance
    EXPECT_EQ(rec.applied.size(), 3);
    write(user, "lifecycle.luau", "return {"); host->reload(); // broken edit retains last good
    EXPECT_EQ(host->actions().size(), 1);
    EXPECT_TRUE(host->runAction("lifecycle.dynamic", 7));
    EXPECT_EQ(rec.applied.last()["text"], QStringLiteral("action"));
    host->setEnabled(false);
    EXPECT_TRUE(host->actions().isEmpty());
}

TEST_F(ScriptHostTest, InvalidOwnedUiEffectsNeverReachTheWindow)
{
    write(user, "invalidui.luau", R"(
return { on_bell = function(ev)
    return {
        afterglow.override(ev.pane, {backgroundOpacity=2}),
        afterglow.override(ev.pane, {cursorBlink=1}),
        afterglow.override(ev.pane, {shell="evil"}),
        afterglow.select(ev.pane, "Duplicate", {{id="x"},{id="x"}}),
        afterglow.prompt(ev.pane, "Too long", string.rep("x",4097)),
        afterglow.set_actions({{id="no-handler"}}),
        afterglow.override(ev.pane, {backgroundOpacity=0.5,cursorBlink=false}),
    }
end }
)");
    host->setEnabled(true); host->dispatch("on_bell", 7);
    ASSERT_EQ(rec.applied.size(), 1);
    EXPECT_EQ(rec.applied[0]["kind"], QStringLiteral("override"));
    // The prompt and set_actions never reach apply even when accepted, so check them here.
    EXPECT_FALSE(bool(pendingReply));
    EXPECT_TRUE(host->actions().isEmpty());
    ASSERT_EQ(rec.results.size(), 7);
    for (int i = 0; i < 6; ++i) EXPECT_FALSE(rec.results[i].second.isEmpty()) << i;
    EXPECT_TRUE(rec.results[6].second.isEmpty());
}


TEST_F(ScriptHostTest, RetiredInstanceDropsPendingUiResultsAndCleansOwnedResources)
{
    write(user, "retire.luau", R"(
return {
    on_bell = function(ev) return afterglow.request("ui",afterglow.prompt(ev.pane,"Prompt")) end,
    on_result = function() return afterglow.copy("obsolete") end,
}
)");
    host->setEnabled(true); host->dispatch("on_bell", 7);
    ASSERT_TRUE(bool(pendingReply));
    EXPECT_TRUE(rec.applied.isEmpty());
    write(user, "retire.luau", "return {}"); host->reload();
    ASSERT_EQ(cleaned.size(), 1);
    pendingReply({{}, {{"text","late"}}, {}});
    QCoreApplication::processEvents();
    EXPECT_TRUE(rec.applied.isEmpty());
    host->setEnabled(false);
    EXPECT_EQ(cleaned.size(), 2);
}

TEST_F(ScriptHostTest, PromptResultsAreDeferredAndDeliveredToTheirOwner)
{
    write(user, "prompt.luau", R"(
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
    write(user, "inspector.luau", example.readAll());
    applyHook = [](const QVariantMap& effect) -> ScriptHost::EffectResult {
        if (effect["kind"] == QLatin1String("read_text"))
            return {{}, {{"available",true},{"truncated",false},
                {"text","src/main.cpp:42:3: error: broken\nlib/other.cpp:7: warning\n"}} , {}};
        return {};
    };
    host->setEnabled(true);
    ASSERT_TRUE(host->runAction("build-inspector.inspect", 7));
    QCoreApplication::processEvents();
    ASSERT_EQ(rec.applied.size(), 2);
    EXPECT_EQ(rec.applied[1]["kind"], QStringLiteral("select"));
    const auto items = rec.applied[1]["items"].toList();
    ASSERT_EQ(items.size(), 2);
    EXPECT_EQ(items[0].toMap()["id"], QStringLiteral("1"));
    EXPECT_TRUE(items[1].toMap()["title"].toString().startsWith("lib/other.cpp:7:"));
}

TEST_F(ScriptHostTest, ProjectHelpersPreserveExplicitIntentAndResults)
{
    write(user, QStringLiteral("projects.luau"), R"(
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
    write(user, QStringLiteral("projects.luau"), R"(
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
    ASSERT_TRUE(host->runAction("projects.open", 7));
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
    write(user,"bundle/package.json",R"({"id":"bundle","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"init.luau","counter":"counter.luau"}})");
    write(user,"bundle/init.luau",R"(
local count=require("counter")
assert(count==require("counter"))
return {on_bell=function() return afterglow.copy(count.next()) end}
)");
    write(user,"bundle/counter.luau","local i=0; return {next=function() i+=1; return tostring(i) end}");
    host->setEnabled(true);
    host->dispatch("on_bell",7); ASSERT_EQ(rec.applied.size(),1); EXPECT_EQ(rec.applied.last().value("text"),"1");
    write(user,"bundle/counter.luau","broken syntax !"); host->reload();
    host->dispatch("on_bell",7); ASSERT_FALSE(rec.applied.isEmpty()); EXPECT_EQ(rec.applied.last().value("text"),"2");
    write(user,"bundle/counter.luau","return {next=function() return 'fixed' end}"); host->reload();
    host->dispatch("on_bell",7); EXPECT_EQ(rec.applied.last().value("text"),"fixed");
}
TEST_F(ScriptHostTest, PackageMigrationAndInitializationAreTransactional)
{
    QDir().mkpath(user.filePath("bundle"));
    auto manifest=[&](int schema) { write(user,"bundle/package.json",QString(R"({"id":"bundle","version":"1.0.0","api_version":1,"state_version":%1,"entry":"main","modules":{"main":"init.luau"},"config":{"label":"test"}})").arg(schema).toUtf8()); };
    manifest(1);
    write(user,"bundle/init.luau",R"(
return {on_load=function(ev)
 assert(ev.package.config.label=="test")
 return afterglow.state_write({count=41},ev.storage.revision)
end,on_bell=function() return afterglow.copy("original") end}
)");
    host->setEnabled(true);
    ScriptState::Snapshot stored;
    ScriptState store(stateDir.path());
    ASSERT_TRUE(store.read("bundle",stored).isEmpty()); ASSERT_EQ(stored.version,1);
    manifest(2);
    write(user,"bundle/init.luau",R"(
return {migrate=function(value,from,to) assert(from==1 and to==2); return {count=value.count+1} end,
on_load=function(ev) error("broken initialization") end}
)");
    host->reload();
    host->dispatch("on_bell",7); ASSERT_FALSE(rec.applied.isEmpty()); EXPECT_EQ(rec.applied.last().value("text"),"original");
    ASSERT_TRUE(store.read("bundle",stored).isEmpty()); EXPECT_EQ(stored.version,1);
    write(user,"bundle/init.luau",R"(
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
    write(user,"bundle/package.json",R"({"id":"bundle","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"init.luau"},"dependencies":{"lib":{"path":"vendor/lib","version":"1.0.0"}}})");
    write(user,"bundle/init.luau","local lib=require('@lib'); return {on_bell=function() return afterglow.copy(lib.label) end}");
    host->setEnabled(true); EXPECT_TRUE(host->scriptIds().isEmpty());
    write(user,"bundle/vendor/lib/package.json",R"({"id":"lib","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"lib.luau"}})");
    write(user,"bundle/vendor/lib/lib.luau","return {label='dependency'}");
    host->reload(); host->dispatch("on_bell",7); ASSERT_FALSE(rec.applied.isEmpty()); EXPECT_EQ(rec.applied.last().value("text"),"dependency");
    write(user,"bundle/vendor/lib/lib.luau","return require('main')");
    host->reload(); host->dispatch("on_bell",7); EXPECT_EQ(rec.applied.last().value("text"),"dependency");
    // The cycle is found, not a stack overflow that happens to fail the load too.
    bool reported = false;
    for (const auto& entry : host->diagnostics()) reported |= entry.toMap()["error"].toString().contains("cycle");
    EXPECT_TRUE(reported);
}

TEST_F(ScriptHostTest, NavigatorLaunchesChosenLayoutAndPersistsFavoritesAcrossReload)
{
    QDir package(QStringLiteral(AFTERGLOW_EXAMPLE_PACKAGES) + "/project-navigator");
    QDir().mkpath(user.filePath("project-navigator"));
    for (const auto& name : package.entryList(QDir::Files)) ASSERT_TRUE(QFile::copy(package.filePath(name),user.filePath("project-navigator/")+name));
    write(user,"project-navigator/config.json",R"({"roots":["/tmp/navigator-project"],"fd":false,"zoxide":false})");
    auto pane = paneState[7]; pane["cwd"]="/tmp/navigator-project"; paneState[7]=pane;
    applyHook = [](const QVariantMap& e) -> ScriptHost::EffectResult {
        const auto kind=e.value("kind").toString();
        if (kind=="list_projects") return {{},{{"names",QStringList{"dev"}}},{}};
        if (kind=="select") {
            const auto title=e.value("title").toString();
            if (title.startsWith("Layout for")) return {{},{{"id","layout-1"}},{}};
            for (const auto& item : e.value("items").toList()) if (item.toMap().value("title").toString().contains("/tmp/navigator-project")) return {{},{{"id",item.toMap().value("id")}},{}};
        }
        if (kind=="open_directory") return {{},{{"window_id",3},{"cwd","/tmp/navigator-project"},{"reused",false}},{}};
        return {};
    };
    host->setEnabled(true);
    ASSERT_EQ(host->scriptIds(),QStringList{"project-navigator"});
    ASSERT_TRUE(host->runAction("project-navigator.open",7));
    for (int i=0;i<10;++i) QCoreApplication::processEvents();
    bool launched=false;
    for (const auto& e : rec.applied) if (e.value("kind")=="open_directory") { launched=true; EXPECT_EQ(e.value("layout"),"dev"); }
    EXPECT_TRUE(launched);
    ASSERT_TRUE(host->runAction("project-navigator.favorite",7));
    for (int i=0;i<5;++i) QCoreApplication::processEvents();
    ScriptState::Snapshot stored; ScriptState store(stateDir.path());
    ASSERT_TRUE(store.read("project-navigator",stored).isEmpty());
    EXPECT_TRUE(stored.value.value("favorites").toMap().value("/tmp/navigator-project").toBool());
    EXPECT_EQ(stored.value.value("templates").toMap().value("/tmp/navigator-project"),"dev");
    host->setEnabled(false); host->setEnabled(true);
    EXPECT_EQ(host->scriptIds(),QStringList{"project-navigator"});
    ASSERT_FALSE(host->diagnostics().isEmpty());
    EXPECT_EQ(host->diagnostics().first().toMap().value("status"),"active");
    for (const auto& result : rec.results) EXPECT_TRUE(result.second.isEmpty()) << result.second.toStdString();
}

TEST_F(ScriptHostTest, NavigatorParsesBoundedProviderRecords)
{
    QDir().mkpath(user.filePath("providers"));
    ASSERT_TRUE(QFile::copy(QStringLiteral(AFTERGLOW_EXAMPLE_PACKAGES)+"/project-navigator/providers.luau",user.filePath("providers/providers.luau")));
    write(user,"providers/package.json",R"({"id":"providers","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"main.luau","providers":"providers.luau"}})");
    write(user,"providers/main.luau",R"(
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
    QDir package(QStringLiteral(AFTERGLOW_EXAMPLE_PACKAGES)+"/project-navigator");
    QDir().mkpath(user.filePath("project-navigator"));
    for (const auto& name : package.entryList(QDir::Files)) ASSERT_TRUE(QFile::copy(package.filePath(name),user.filePath("project-navigator/")+name));
    write(user,"project-navigator/config.json",R"({"roots":["/tmp"],"fd":true,"zoxide":false})");
    QFile provider(user.filePath("project-navigator/providers.luau")); ASSERT_TRUE(provider.open(QIODevice::ReadOnly));
    auto source=provider.readAll(); provider.close();
    ASSERT_TRUE(source.contains("command={\"fd\""));
    source.replace("command={\"fd\"", "command={\"afterglow-nonexistent-fd-test\"");
    write(user,"project-navigator/providers.luau",source);
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
    host->setEnabled(true); ASSERT_TRUE(host->runAction("project-navigator.open",7));
    QElapsedTimer clock;clock.start();
    while (!selected && clock.elapsed()<3000) { QCoreApplication::processEvents(); QThread::msleep(5); }
    ASSERT_TRUE(selected);
    for (int i=0;i<5;++i) QCoreApplication::processEvents();
    ASSERT_TRUE(host->runAction("project-navigator.favorite",7));
    for (int i=0;i<5;++i) QCoreApplication::processEvents();
    ScriptState::Snapshot stored;
    ASSERT_TRUE(ScriptState(stateDir.path()).read("project-navigator",stored).isEmpty());
    EXPECT_TRUE(stored.value.value("favorites").toMap().value("/tmp").toBool());
}

TEST_F(ScriptHostTest, NavigatorNamesAProviderThatTimedOut)
{
    installPackage("project-navigator");
    write(user, "project-navigator/config.json", R"({"roots":["/tmp"],"fd":true,"zoxide":false})");
    // A provider that runs past its timeout returns no output at all.
    auto patch = [&](const QString& name, const QByteArray& from, const QByteArray& to) {
        QFile f(user.filePath("project-navigator/" + name));
        ASSERT_TRUE(f.open(QIODevice::ReadOnly));
        auto source = f.readAll();
        f.close();
        ASSERT_TRUE(source.contains(from)) << from.toStdString();
        write(user, "project-navigator/" + name, source.replace(from, to));
    };
    patch("providers.luau", R"(command={"fd", )", R"(command={"/bin/sh", "-c", "exec sleep 5", )");
    patch("init.luau", "timeout_ms=5000", "timeout_ms=100");
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
    ASSERT_TRUE(host->runAction("project-navigator.open", 7));
    ASSERT_TRUE(await([&] { return !title.isEmpty(); }));
    EXPECT_TRUE(title.contains("(unavailable: fd-1 timed out)")) << title.toStdString();
}

TEST_F(ScriptHostTest, PackageReloadAndRemovalRetireJobsTimersWatchesAndUi)
{
    QDir().mkpath(user.filePath("lifecycle"));
    write(user, "lifecycle/package.json", R"({"id":"lifecycle","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"init.luau"}})");
    write(files, "watched", "original");
    write(user, "lifecycle/init.luau", R"(
local generation
return {
 metadata={actions={{id="dialog"}}},
 on_load=function(ev)
  generation=ev.package.config.generation
  return {afterglow.timer("tick",250,250),
   afterglow.watch_file("watch",ev.package.config.watch),
   afterglow.request("job",afterglow.exec("job",{"/bin/sh","-c",'echo $$ > "$1"; exec sleep 60',"probe",ev.package.config.pid})),
   afterglow.status(7,"generation "..generation)}
 end,
 run_action=function(_,ev) return afterglow.request("dialog",afterglow.prompt(ev.pane,"Pending")) end,
 on_timer=function() return afterglow.copy("tick-"..generation) end,
 on_file_changed=function() return afterglow.copy("watch-"..generation) end,
 on_result=function(ev) return afterglow.copy("late-"..generation) end,
}
)");
    auto config = [&](int generation) {
        QJsonObject value{{"generation",generation},{"pid",files.filePath("pid")},{"watch",files.filePath("watched")}};
        write(user,"lifecycle/config.json",QJsonDocument(value).toJson());
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
    config(0); host->setEnabled(true);
    ASSERT_TRUE(wait([&]{return pid()>0;}));
    qint64 previousPid=pid();
    for (int generation=1;generation<=5;++generation) {
        ASSERT_TRUE(host->runAction("lifecycle.dialog",7));
        auto obsoleteReply=std::move(pendingReply);
        ASSERT_TRUE(bool(obsoleteReply));
        config(generation); host->reload();
        ASSERT_TRUE(wait([&]{return pid()>0 && pid()!=previousPid;}));
        ASSERT_TRUE(wait([&]{return ::kill(pid_t(previousPid),0)==-1 && errno==ESRCH;}));
        previousPid=pid();
        EXPECT_EQ(cleaned.size(),generation);
        obsoleteReply({{},{{"text","obsolete"}}, {}});
        rec.applied.clear();
        write(files,"watched",QByteArray::number(generation));
        const auto marker=QStringLiteral("watch-%1").arg(generation);
        ASSERT_TRUE(wait([&]{return std::any_of(rec.applied.begin(),rec.applied.end(),[&](const auto& e){return e.value("text")==marker;});}));
        for (const auto& effect:rec.applied) {
            const auto text=effect.value("text").toString();
            EXPECT_TRUE(text==marker || text==QStringLiteral("tick-%1").arg(generation)) << text.toStdString();
        }
        EXPECT_EQ(host->actions().size(),1);
    }
    ASSERT_TRUE(QDir(user.filePath("lifecycle")).removeRecursively());
    host->reload();
    ASSERT_TRUE(wait([&]{return ::kill(pid_t(previousPid),0)==-1 && errno==ESRCH;}));
    EXPECT_EQ(cleaned.size(),6);
    EXPECT_TRUE(host->actions().isEmpty());
    EXPECT_TRUE(host->diagnostics().isEmpty());
    rec.applied.clear();
    write(files,"watched","after removal");
    QElapsedTimer quiet;quiet.start();
    while(quiet.elapsed()<600) {QCoreApplication::processEvents();QThread::msleep(5);}
    EXPECT_TRUE(rec.applied.isEmpty());
}

TEST_F(ScriptHostTest, AutosaveIsOptInPersistsAndRestoresOnlySelectedProjects)
{
    installPackage("project-autosave");
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
    auto pump=[] {for(int i=0;i<12;++i) QCoreApplication::processEvents();};
    host->setEnabled(true);
    host->dispatch("on_timer",0,{{"name","autosave"}});pump();
    EXPECT_TRUE(rec.applied.isEmpty());
    ASSERT_TRUE(host->runAction("project-autosave.enable",7));pump();
    ScriptState::Snapshot value;
    ASSERT_TRUE(ScriptState(stateDir.path()).read("project-autosave",value).isEmpty());
    EXPECT_TRUE(value.value.value("enabled").toMap().value("dev").toBool());
    ASSERT_GE(rec.applied.size(),1);
    EXPECT_EQ(rec.applied.first().value("kind"),"save_project");
    EXPECT_TRUE(rec.applied.first().value("overwrite").toBool());
    host->setEnabled(false);host->setEnabled(true);rec.applied.clear();
    host->dispatch("on_timer",0,{{"name","autosave"}});pump();
    ASSERT_EQ(rec.applied.size(),1);EXPECT_EQ(rec.applied.first().value("name"),"dev");
    // Duplicate live instances of a named project cannot overwrite each other.
    windows.append(QVariantMap{{"id",2},{"project","dev"},{"active_pane",9}});
    rec.applied.clear();host->dispatch("on_timer",0,{{"name","autosave"}});pump();
    EXPECT_TRUE(rec.applied.isEmpty());windows.removeLast();
    ASSERT_TRUE(host->runAction("project-autosave.restore",7));pump();
    ASSERT_FALSE(rec.applied.isEmpty());
    ASSERT_EQ(rec.applied.last().value("kind"),"open_project");
    EXPECT_EQ(rec.applied.last().value("name"),"dev");
    EXPECT_FALSE(rec.applied.last().value("new_instance").toBool());
    ASSERT_TRUE(host->runAction("project-autosave.disable",7));pump();rec.applied.clear();
    host->dispatch("on_timer",0,{{"name","autosave"}});pump();EXPECT_TRUE(rec.applied.isEmpty());
    for(const auto& result:rec.results) EXPECT_TRUE(result.second.isEmpty()) << result.second.toStdString();
}

TEST_F(ScriptHostTest, TabStatusTracksActivePaneAndPersistsToggle)
{
    installPackage("tab-status");
    write(user,"tab-status/config.json",R"({"git":false,"segments":["cwd","command","exit"],"separator":" / "})");
    paneState[7]["cwd"]="/tmp/repository";paneState[7]["context"]="";
    paneState[8]["cwd"]="/tmp/other";paneState[8]["context"]="";
    tabs={QVariantMap{{"active_pane",7}}};
    auto tick=[&]{host->dispatch("on_timer",0,{{"name","refresh"}});for(int i=0;i<6;++i)QCoreApplication::processEvents();};
    host->setEnabled(true);tick();
    ASSERT_EQ(rec.applied.size(),1);
    EXPECT_EQ(rec.applied.last().value("text"),"repository / idle");
    EXPECT_EQ(rec.applied.last().value("tab_title"),"repository");
    host->dispatch("on_command_finished",7,{{"exit_code",2}});tick();
    EXPECT_EQ(rec.applied.last().value("text"),"repository / idle / exit 2");
    paneState[7]["command"]="make";tick();
    EXPECT_EQ(rec.applied.last().value("text"),"repository / make");
    tabs={QVariantMap{{"active_pane",8}}};rec.applied.clear();tick();
    ASSERT_EQ(rec.applied.size(),2);
    EXPECT_EQ(rec.applied.first().value("pane").toUInt(),7u);
    EXPECT_EQ(rec.applied.first().value("text"),"");
    EXPECT_EQ(rec.applied.last().value("text"),"other / idle");
    ASSERT_TRUE(host->runAction("tab-status.toggle",8));
    for(int i=0;i<6;++i)QCoreApplication::processEvents();
    tick();EXPECT_EQ(rec.applied.last().value("text"),"");
    host->setEnabled(false);host->setEnabled(true);rec.applied.clear();tick();
    EXPECT_TRUE(rec.applied.isEmpty());
    for(const auto& result:rec.results) EXPECT_TRUE(result.second.isEmpty()) << result.second.toStdString();
}

TEST_F(ScriptHostTest, TabStatusParsesGitAndBoundsConfigurableSegments)
{
    installPackage("tab-status");
    write(user,"tab-status/init.luau",R"(
local s=require("segments")
return {on_load=function()
 assert(s.git("main\n"," M file.cpp\n")=="main *")
 assert(s.git("feature\n","")=="feature")
 assert(s.git("a1b2c3d\n")=="a1b2c3d")
 assert(s.git("",nil)=="")
 local text,title=s.render({cwd="/tmp/project",command="",context="remote"},"main",1,{segments={"context","cwd","unknown","exit"},separator=" | ",tab_title=false})
 assert(text=="remote | project | exit 1" and title=="")
 local long={cwd=string.rep("x",1000),command=string.rep("y",1000)}
 local commands={}
 for i=1,8 do commands[i]="command" end
 local bounded=s.render(long,"",nil,{segments=commands})
 assert(#bounded<=500)
 local _,boundedTitle=s.render(long,"",nil,{})
 assert(#boundedTitle<=120)
 return afterglow.copy("segments passed")
end}
)");
    host->setEnabled(true);
    ASSERT_EQ(rec.applied.size(),1);EXPECT_EQ(rec.applied.first().value("text"),"segments passed");
}

TEST_F(ScriptHostTest, AutosaveRecoversFromSaveFailureAndCancelledRestore)
{
    installPackage("project-autosave");
    windows={QVariantMap{{"id",1},{"project","dev"},{"active_pane",7}}};
    bool fail=true;
    applyHook=[&](const QVariantMap& e)->ScriptHost::EffectResult {
        if(e.value("kind")=="save_project" && fail) return {"Disk unavailable",{},"io_error"};
        if(e.value("kind")=="list_projects") return {{},{{"names",QStringList{"dev"}}},{}};
        if(e.value("kind")=="select") return {"Cancelled",{},"cancelled"};
        return {};
    };
    auto pump=[] {for(int i=0;i<12;++i)QCoreApplication::processEvents();};
    host->setEnabled(true);ASSERT_TRUE(host->runAction("project-autosave.enable",7));pump();
    ASSERT_FALSE(rec.applied.isEmpty());
    EXPECT_EQ(rec.applied.last().value("kind"),"notify");
    EXPECT_EQ(rec.applied.last().value("body"),"Disk unavailable");
    fail=false;rec.applied.clear();
    host->dispatch("on_timer",0,{{"name","autosave"}});pump();
    ASSERT_EQ(rec.applied.size(),1);EXPECT_EQ(rec.applied.last().value("kind"),"save_project");
    rec.applied.clear();ASSERT_TRUE(host->runAction("project-autosave.restore",7));pump();
    ASSERT_EQ(rec.applied.size(),2);EXPECT_EQ(rec.applied.last().value("kind"),"select");
    ASSERT_TRUE(host->runAction("project-autosave.disable",7));pump();
    ScriptState::Snapshot stored;
    ASSERT_TRUE(ScriptState(stateDir.path()).read("project-autosave",stored).isEmpty());
    EXPECT_FALSE(stored.value.value("enabled").toMap().contains("dev"));
}

TEST_F(ScriptHostTest, ShaderEffectsPreserveTypedValuesAndValidateShape)
{
    write(user, QStringLiteral("shader.luau"), R"(
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
    ASSERT_TRUE(host->runAction(QStringLiteral("shader-parameters.boost"), 7));
    QCoreApplication::processEvents();
    ASSERT_EQ(rec.applied.size(), 2);
    EXPECT_EQ(rec.applied[1].value(QStringLiteral("instance")), QStringLiteral("window-instance"));
    EXPECT_EQ(rec.applied[1].value(QStringLiteral("scope")), QStringLiteral("window"));
    EXPECT_DOUBLE_EQ(rec.applied[1].value(QStringLiteral("values")).toMap().value(QStringLiteral("strength")).toDouble(), 1.2);
    ASSERT_TRUE(host->runAction(QStringLiteral("shader-parameters.reset"), 7));
    QCoreApplication::processEvents();
    ASSERT_EQ(rec.applied.size(), 4);
    EXPECT_TRUE(rec.applied.last().value(QStringLiteral("values")).toMap().isEmpty());
}

TEST_F(ScriptHostTest, SearchDirectoryPriorityWinsOverMetadataIdFilenameOrder)
{
    write(system, "z-system.luau", "return {metadata={id='same',actions={{id='system'}}},run_action=function() end}");
    write(user, "a-user.luau", "return {metadata={id='same',actions={{id='user'}}},run_action=function() end}");
    host->setEnabled(true);
    ASSERT_EQ(host->actions().size(), 1);
    EXPECT_EQ(host->actions()[0].token, QStringLiteral("same.user"));
}

TEST_F(ScriptHostTest, LaunchOptionsPreserveEnvironmentSizeAndDirection)
{
    write(user, "launch.luau", R"(
return {on_bell=function(ev)
 return {afterglow.split(ev.pane,"left",{"/bin/sh"},{cwd="/tmp",size=0.3,env={VALUE="hello",REMOVE=false}}),
         afterglow.split(ev.pane,"up"), afterglow.new_tab(ev.pane,"",nil,{env={VALUE="tab"}})}
end}
)");
    write(user,"window.luau","return {on_bell=function() return afterglow.new_window('',nil,{cwd='/tmp'}) end}");
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
    write(user,"bad-options.luau",R"(
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
    write(user,"stats.luau",R"(
return {on_bell=function() error("handler failed") end,
 on_focus=function() return {kind="unknown"} end,
 on_title=function() local out={} for i=1,100 do table.insert(out,afterglow.log("x")) end return out end}
)");
    host->setEnabled(true);
    host->dispatch("on_bell",7); host->dispatch("on_focus",7); host->dispatch("on_title",7);
    ASSERT_FALSE(host->diagnostics().isEmpty());
    const auto stats=host->diagnostics().first().toMap();
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
    write(user, "bad-id.luau", "return {metadata={id='bad\\n'}}");
    host->setEnabled(true);
    EXPECT_TRUE(host->scriptIds().isEmpty());
}

TEST_F(ScriptHostTest, FailedAndSupersededLooseScriptsRecoverFromInPlaceEdits)
{
    write(user, "broken.luau", "return {");
    write(system, "old.luau", "return {metadata={id='shared'}}");
    write(user, "winner.luau", "return {metadata={id='shared'}}");
    host->setEnabled(true);
    EXPECT_EQ(host->scriptIds(), QStringList{"shared"});
    write(user, "broken.luau", "return {metadata={id='recovered'}}");
    ASSERT_TRUE(await([&] { return host->scriptIds().contains("recovered"); }));
    write(system, "old.luau", "return {metadata={id='independent'}}");
    ASSERT_TRUE(await([&] { return host->scriptIds().contains("independent"); }));
    EXPECT_TRUE(host->scriptIds().contains("shared"));
}

TEST_F(ScriptHostTest, AwaitChainsEffectsInOneHandler)
{
    write(user, "chain.luau", R"(
return {
    metadata = {actions = {{id = "copy", title = "Copy"}}},
    run_action = function(_, ev)
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
    ASSERT_TRUE(host->runAction("chain.copy", 7));
    ASSERT_TRUE(await([&] { return rec.applied.size() == 4; }));
    EXPECT_TRUE(rec.applied[0]["request_id"].toString().startsWith("afterglow.await:"));
    EXPECT_EQ(rec.applied[1]["items"].toList().front().toMap()["id"], "42");
    EXPECT_EQ(rec.applied[3]["kind"], "copy");
    EXPECT_EQ(rec.applied[3]["text"], "7:42:output");
    ASSERT_FALSE(host->diagnostics().isEmpty());
    EXPECT_EQ(host->diagnostics().first().toMap()["waiting"].toInt(), 0);
    EXPECT_EQ(host->diagnostics().first().toMap()["handler_errors"].toULongLong(), 0u);
}

TEST_F(ScriptHostTest, AwaitReturnsErrorsAndCancellationAsValues)
{
    write(user, "errors.luau", R"(
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
    write(user, "batch.luau", R"(
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

TEST_F(ScriptHostTest, SleepResumesAfterItsDelayAndIsBounded)
{
    write(user, "sleep.luau", R"(
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
    write(user, "many.luau", R"(
return {
    on_bell = function()
        local _, err = afterglow.sleep(10000)
        if err then return afterglow.copy(err.code) end
        return nil
    end,
}
)");
    host->setEnabled(true);
    for (int i = 0; i < 17; ++i) host->dispatch("on_bell", 7);
    ASSERT_FALSE(host->diagnostics().isEmpty());
    EXPECT_EQ(host->diagnostics().first().toMap()["waiting"].toInt(), 16);
    ASSERT_FALSE(rec.applied.isEmpty());
    EXPECT_EQ(rec.applied.last()["text"], "busy");
}

TEST_F(ScriptHostTest, AwaitMisuseFailsClearly)
{
    write(user, "toplevel.luau", "afterglow.await(afterglow.log('x'))\nreturn {}");
    write(user, "misuse.luau", R"(
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
    for (const auto& entry : host->diagnostics()) {
        const auto map = entry.toMap();
        if (map["path"].toString().endsWith("toplevel.luau")) errors << map["error"].toString();
        if (map["id"] == "misuse") { EXPECT_EQ(map["handler_errors"].toULongLong(), 3u); }
    }
    ASSERT_EQ(errors.size(), 1);
    EXPECT_TRUE(errors[0].contains("only in a handler")) << errors[0].toStdString();
    EXPECT_TRUE(rec.applied.isEmpty());
}

TEST_F(ScriptHostTest, ResumedHandlersKeepTheirTimeLimitAndErrorAccounting)
{
    write(user, "resumed.luau", R"(
return {
    metadata = {actions = {{id = "spin", title = "Spin"}}},
    run_action = function()
        afterglow.sleep(0)
        local t = os.clock()
        while os.clock() - t < 0.035 do end -- 35 ms: over an event's 20 ms, well within an action's 100 ms
        afterglow.sleep(0)
        error("after the second await")
    end,
}
)");
    host->setEnabled(true);
    ASSERT_TRUE(host->runAction("resumed.spin", 7));
    ASSERT_TRUE(await([&] {
        return !host->diagnostics().isEmpty() && host->diagnostics().first().toMap()["handler_errors"].toULongLong() == 1;
    }));
    const auto stats = host->diagnostics().first().toMap();
    EXPECT_TRUE(stats["last_error"].toString().startsWith("a handler after await:")) << stats["last_error"].toString().toStdString();
    EXPECT_TRUE(stats["last_error"].toString().contains("after the second await"));
    EXPECT_EQ(stats["waiting"].toInt(), 0);
}

// The await runtime registers a flow before the engine converts the effects it
// waits for. Effects too large to convert must not keep that flow, or every
// such call would hold one of the script's 16 waiting slots for good.
TEST_F(ScriptHostTest, AwaitWithEffectsTooLargeToConvertFreesItsWaitingSlot)
{
    write(user, "large.luau", R"(
return {
    metadata = {actions = {{id = "large", title = "Large"}, {id = "small", title = "Small"}}},
    run_action = function(action, ev)
        if action == "large" then
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
        for (int i = 0; i < 3; ++i) ASSERT_TRUE(host->runAction("large.large", 7));
        ASSERT_TRUE(host->runAction("large.small", 7));
        ASSERT_TRUE(await([&] {
            return std::count_if(rec.applied.cbegin(), rec.applied.cend(),
                                 [](const QVariantMap& e) { return e["kind"] == QLatin1String("copy"); }) == round + 1;
        }));
        EXPECT_EQ(rec.applied.last()["text"], "ok") << "round " << round;
    }
    for (const QVariantMap& e : std::as_const(rec.applied)) EXPECT_NE(e["text"], "unreachable");
    const auto stats = host->diagnostics().first().toMap();
    EXPECT_EQ(stats["waiting"].toInt(), 0);
    EXPECT_EQ(stats["handler_errors"].toULongLong(), 18u);
    EXPECT_TRUE(stats["last_error"].toString().contains("result too large")) << stats["last_error"].toString().toStdString();
    EXPECT_TRUE(host->scriptIds().contains("large"));
}

TEST_F(ScriptHostTest, AwaitTooLargeToConvertFreesEveryFlowTheCallRegistered)
{
    // afterglow.__wrap is reachable from a handler: a call through it waits
    // too, so one failed call can leave more than one flow behind.
    write(user, "nested.luau", R"(
return {
    metadata = {actions = {{id = "large", title = "Large"}}},
    run_action = function(action, ev)
        afterglow.__wrap({h = function() afterglow.await(afterglow.log("inner")) end}).h()
        local wide = {}
        for _ = 1, 20 do
            wide = {wide, wide}
        end
        local effect = afterglow.log("large")
        effect.extra = wide
        afterglow.await(effect)
    end,
}
)");
    host->setEnabled(true);
    ASSERT_TRUE(host->runAction("nested.large", 7));
    const auto stats = host->diagnostics().first().toMap();
    EXPECT_EQ(stats["handler_errors"].toULongLong(), 1u);
    EXPECT_EQ(stats["waiting"].toInt(), 0);
}

TEST_F(ScriptHostTest, ReplacementDropsWaitingHandlersAndOnResultStillWorks)
{
    write(user, "replace.luau", R"(
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
    write(user, "replace.luau", "return {on_bell = function() return afterglow.copy('new') end}");
    host->reload();
    const int before = rec.applied.size();
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < 150) { QCoreApplication::processEvents(); QThread::msleep(1); }
    EXPECT_EQ(rec.applied.size(), before);
}

TEST_F(ScriptHostTest, PackageLoadCanAwaitItsStateWrite)
{
    QDir().mkpath(user.filePath("counter"));
    write(user, "counter/package.json", R"({"id":"counter","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"main.luau"}})");
    write(user, "counter/main.luau", R"(
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
    write(user, "keys.luau", R"(
return {
    on_bell = function(ev) return afterglow.key_mode(ev.pane, "Keys") end,
    on_key = function(ev) return afterglow.copy(ev.key) end,
}
)");
    write(user, "other.luau", R"(
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
    write(user, "fx.luau", R"(
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
        }
    end,
}
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    QStringList kinds;
    for (const auto& e : rec.applied) kinds << e["kind"].toString();
    EXPECT_EQ(kinds, QStringList({"overview", "overview_move", "pulse"}));
    EXPECT_EQ(rec.applied[2]["effect"], "2"); // flash, by number
    EXPECT_EQ(rec.applied[2]["color"].toInt(), 1);
    EXPECT_EQ(rec.applied[2]["duration_ms"].toInt(), 600);
    int refused = 0;
    for (const auto& result : rec.results) refused += result.second.isEmpty() ? 0 : 1;
    EXPECT_EQ(refused, 5);
}

TEST_F(ScriptHostTest, HandlerFailuresReleaseWhatTheScriptHolds)
{
    write(user, "fails.luau", R"(
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
    installPackage("scripted-overview");
    QDir().mkpath(user.filePath("ripples/shaders/rings"));
    write(user, "ripples/package.json", R"({"id":"ripples","version":"1.0.0","api_version":1,"entry":"main",
        "modules":{"main":"init.luau"},"shaders":{"path":"shaders","suggests":{"paneEffects":"rings"}},"pulses":{"ripple":7}})");
    write(user, "ripples/shaders/rings/metadata.json", R"({"id":"ripples.rings","name":"Rings","fragmentShader":"effect.frag"})");
    write(user, "ripples/shaders/rings/effect.frag", "vec4 pTerminal(vec2 uv) { return terminalTexel(uv); }");
    write(user, "ripples/init.luau", R"(
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
    write(user, "strict.luau", R"(
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
    write(user, "cycle.luau", R"(
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
    ASSERT_FALSE(rec.applied.isEmpty());
    EXPECT_EQ(rec.applied[0]["text"], "cycle");
}

TEST_F(ScriptHostTest, TurningScriptsOffFromAnEffectIsSafe)
{
    write(user, "first.luau", "return {on_bell = function() return {afterglow.copy('off'), afterglow.copy('after')} end}");
    write(user, "second.luau", "return {on_bell = function() return afterglow.copy('second') end}");
    // A reload_config action that turns scripting off runs inside the call.
    applyHook = [this](const QVariantMap& e) {
        if (e["text"] == QLatin1String("off")) host->setEnabled(false);
        return ScriptHost::EffectResult{};
    };
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    EXPECT_TRUE(host->scriptIds().isEmpty());
    QStringList texts;
    for (const auto& e : rec.applied) texts << e["text"].toString();
    EXPECT_FALSE(texts.contains("after"));
    EXPECT_FALSE(texts.contains("second"));
}

TEST_F(ScriptHostTest, TurningScriptsOffDuringAScanWaitsForTheScan)
{
    write(user, "loader.luau", "return {on_load = function() return afterglow.copy('off') end}");
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
        write(user, "ripples/package.json", QStringLiteral(R"({"id":"ripples","version":"1.0.0","api_version":1,"entry":"main",
            "modules":{"main":"init.luau"},"shaders":{"path":"shaders"},"pulses":{"ripple":%1}})").arg(number).toUtf8());
    };
    write(user, "ripples/shaders/rings/metadata.json", R"({"id":"ripples.rings","name":"Rings","fragmentShader":"effect.frag"})");
    write(user, "ripples/init.luau", "return { on_bell = function(ev) return afterglow.pulse(ev.pane, 'ripple') end }");
    manifest(7);
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    manifest(9);
    host->reload();
    host->dispatch("on_bell", 7);
    ASSERT_EQ(rec.applied.size(), 2);
    EXPECT_EQ(rec.applied[0]["effect"], "7");
    EXPECT_EQ(rec.applied[1]["effect"], "9");
}

TEST_F(ScriptHostTest, EventsHaveACallbackBudgetAndNoSnapshotWithoutAHandler)
{
    write(user, "titles.luau", "return {on_title = function() return nil end}");
    host->setEnabled(true);
    const int before = snapshots;
    for (int i = 0; i < 10; ++i) host->dispatch("on_bell", 7);
    EXPECT_EQ(snapshots, before); // nobody handles on_bell
    for (int i = 0; i < ScriptHost::MaxCallbacksPerSecond + 6; ++i) host->dispatch("on_title", 7);
    ASSERT_FALSE(host->diagnostics().isEmpty());
    const auto stats = host->diagnostics().first().toMap();
    EXPECT_EQ(stats["calls"].toULongLong(), quint64(ScriptHost::MaxCallbacksPerSecond));
    EXPECT_EQ(stats["dropped_callbacks"].toULongLong(), 6u);
}

TEST_F(ScriptHostTest, RuntimeDisableWithdrawsPackageShadersAndActions)
{
    QDir().mkpath(user.filePath("broken/shaders/wall"));
    write(user, "broken/package.json", R"({"id":"broken","version":"1.0.0","api_version":1,"entry":"main",
        "modules":{"main":"init.luau"},"shaders":{"path":"shaders"}})");
    write(user, "broken/shaders/wall/metadata.json", R"({"id":"broken.wall","name":"Wall","fragmentShader":"effect.frag"})");
    write(user, "broken/init.luau", R"(
return {metadata = {actions = {{id = "go"}}}, run_action = function() end, on_bell = function() error("broken") end}
)");
    QList<QMap<QString, QString>> announced;
    QObject::connect(host.get(), &ScriptHost::packageShadersChanged, host.get(),
                     [&](const QMap<QString, QString>& dirs) { announced << dirs; });
    int actionChanges = 0;
    QObject::connect(host.get(), &ScriptHost::actionsChanged, host.get(), [&] { ++actionChanges; });
    host->setEnabled(true);
    ASSERT_EQ(announced.size(), 1);
    EXPECT_TRUE(announced[0].contains("broken"));
    for (int i = 0; i < ScriptHost::MaxFailures; ++i) host->dispatch("on_bell", 7);
    EXPECT_TRUE(host->isDisabled("broken"));
    ASSERT_EQ(announced.size(), 2);
    EXPECT_TRUE(announced[1].isEmpty());
    EXPECT_EQ(actionChanges, 2); // loaded, then withdrawn
}

TEST_F(ScriptHostTest, PackageShadersExistBeforeOnLoadEffects)
{
    QDir().mkpath(user.filePath("early/shaders/wall"));
    write(user, "early/package.json", R"({"id":"early","version":"1.0.0","api_version":1,"entry":"main",
        "modules":{"main":"init.luau"},"shaders":{"path":"shaders"}})");
    write(user, "early/shaders/wall/metadata.json", R"({"id":"early.wall","name":"Wall","fragmentShader":"effect.frag"})");
    write(user, "early/init.luau", "return {on_load = function() return afterglow.copy('loaded') end}");
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
    write(user, "lines/package.json", R"({"id":"lines","version":"1.0.0","api_version":1,"entry":"main",
        "modules":{"main":"init.luau","util":"util.luau"}})");
    write(user, "lines/init.luau", "local util = require('util')\nreturn {on_bell = function() util.fail() end}");
    write(user, "lines/util.luau", "local u = {}\nfunction u.fail()\n    error('deliberate')\nend\nreturn u");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_FALSE(host->diagnostics().isEmpty());
    const auto error = host->diagnostics().first().toMap()["last_error"].toString();
    EXPECT_TRUE(error.contains("util.luau:3:")) << error.toStdString();
    EXPECT_FALSE(error.contains("package.json:"));
}

TEST_F(ScriptHostTest, PackageModulesTakeVarargsAndCannotReplaceTheLoader)
{
    QDir().mkpath(user.filePath("loader"));
    write(user, "loader/package.json", R"({"id":"loader","version":"1.0.0","api_version":1,"entry":"main",
        "modules":{"main":"init.luau","util":"util.luau"}})");
    // A forgotten "local" must not reach the loader's own variables.
    write(user, "loader/util.luau", "local name = ...\ncache = {}\nfunction load() return {} end\nreturn {value = 1}");
    write(user, "loader/init.luau", R"(
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
    write(user, "forged.luau", R"(
return {
    on_bell = function(ev)
        local reply = afterglow.await(afterglow.prompt(ev.pane, "Name"))
        return afterglow.copy(reply and reply.text or "nothing")
    end,
    on_focus = function(ev)
        return {afterglow.request("afterglow.await:1:99", afterglow.focus(ev.pane)),
                afterglow.request("afterglow.await:1:0", afterglow.focus(ev.pane))}
    end,
}
)");
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_TRUE(bool(pendingReply));
    host->dispatch("on_focus", 7);
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    for (const auto& e : rec.applied) EXPECT_NE(e["kind"], "copy");
    pendingReply({{}, {{"text", "real"}}, {}});
    ASSERT_TRUE(await([&] { return !rec.applied.isEmpty() && rec.applied.last()["kind"] == "copy"; }));
    EXPECT_EQ(rec.applied.last()["text"], "real");
}

TEST_F(ScriptHostTest, EventsWithoutAPaneHaveNoPaneAndClosedPanesKeepTheirId)
{
    write(user, "panes.luau", R"(
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

TEST_F(ScriptHostTest, UnchangedActionListsAnnounceNothing)
{
    write(user, "actions.luau", R"(
return {
    run_action = function() end,
    on_bell = function() return afterglow.set_actions({{id = "one", title = "One"}}) end,
}
)");
    host->setEnabled(true);
    int changes = 0;
    QObject::connect(host.get(), &ScriptHost::actionsChanged, host.get(), [&] { ++changes; });
    host->dispatch("on_bell", 7);
    host->dispatch("on_bell", 7);
    EXPECT_EQ(changes, 1);
}

TEST_F(ScriptHostTest, MetadataActionsFollowTheSetActionsRules)
{
    QByteArray actions;
    for (int i = 0; i < 40; ++i) actions += "{id='a" + QByteArray::number(i) + "'},";
    write(user, "many.luau", "return {metadata = {actions = {{id='a0'}, " + actions + "{id='long', title=string.rep('x', 513)}}},"
                             " run_action = function() end}");
    host->setEnabled(true);
    const auto list = host->actions();
    ASSERT_EQ(list.size(), ScriptHost::MaxActions);
    QSet<QString> tokens;
    for (const auto& a : list) tokens.insert(a.token);
    EXPECT_EQ(tokens.size(), ScriptHost::MaxActions);
}

TEST_F(ScriptHostTest, EffectChecksRefuseBadShapesBeforeTheHost)
{
    write(user, "shapes.luau", R"(
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
    for (int i = 0; i < 10; ++i) EXPECT_FALSE(rec.results[i].second.isEmpty()) << i;
    EXPECT_EQ(rec.results[10].second, "unknown effect \"\"");
}

TEST_F(ScriptHostTest, NilEntriesInReturnedListsAreSkipped)
{
    write(user, "holes.luau", R"(
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
    EXPECT_EQ(host->diagnostics().first().toMap()["effect_errors"].toULongLong(), 0u);
}

TEST_F(ScriptHostTest, ReleasingEffectsWorkWhenTheBudgetIsSpent)
{
    write(user, "spent.luau", QStringLiteral(R"(
return {
    on_bell = function(ev)
        local out = {}
        for i = 1, %1 do out[i] = afterglow.set_title(ev.pane, "t") end
        table.insert(out, afterglow.cancel("nothing"))
        table.insert(out, afterglow.end_key_mode())
        return out
    end,
}
)").arg(ScriptHost::MaxEffectsPerSecond).toUtf8());
    host->setEnabled(true);
    host->dispatch("on_bell", 7);
    ASSERT_EQ(rec.results.size(), ScriptHost::MaxEffectsPerSecond + 2);
    EXPECT_TRUE(rec.results[ScriptHost::MaxEffectsPerSecond].second.isEmpty()); // cancel
    EXPECT_TRUE(rec.results.last().second.isEmpty()); // end_key_mode
    EXPECT_EQ(rec.applied.last()["kind"], "end_key_mode");
}

TEST_F(ScriptHostTest, SleepsDoNotSpendTheEffectBudget)
{
    write(user, "poll.luau", R"(
return { on_bell = function(ev)
    local first, second = {}, {}
    for i = 1, 16 do first[i] = afterglow.set_title(ev.pane, "t") end
    for i = 1, 4 do second[i] = afterglow.set_title(ev.pane, "t") end
    afterglow.await_all(first)
    afterglow.await_all(second) -- the 20 effects of this second are spent
    local _, err = afterglow.sleep(0)
    return afterglow.log(err and err.code or "slept")
end }
)");
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
    write(user, "spin.luau", "while true do end\nreturn {}");
    host->setEnabled(true); // the module load times out once
    write(user, "other.luau", "return {}");
    QElapsedTimer clock;
    clock.start();
    host->reload();
    EXPECT_LT(clock.elapsed(), 500);
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
    write(system, "notify.luau", "return {metadata = {id = 'notify'}}");
    write(user, "notify.luau", "return {metadata = {id = 'my-notify'}}");
    write(user, "same.luau", "return {metadata = {id = 'pkg'}}");
    QDir().mkpath(user.filePath("pkg"));
    write(user, "pkg/package.json", R"({"id":"pkg","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"init.luau"}})");
    write(user, "pkg/init.luau", "return {}");
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
    write(checkout, "package.json", R"({"id":"linked","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"init.luau"}})");
    write(checkout, "init.luau", "return {on_bell = function() return afterglow.copy('linked') end}");
    ASSERT_TRUE(QFile::link(checkout.path(), user.filePath("linked")));
    host->setEnabled(true);
    EXPECT_EQ(host->scriptIds(), QStringList{"linked"});
}

TEST_F(ScriptHostTest, PackageIdentityRulesAreReported)
{
    QDir().mkpath(user.filePath("wrong-dir"));
    write(user, "wrong-dir/package.json", R"({"id":"other","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"init.luau"}})");
    write(user, "wrong-dir/init.luau", "return {}");
    QDir().mkpath(user.filePath("entry"));
    write(user, "entry/package.json", R"({"id":"entry","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"init.luau"}})");
    write(user, "entry/init.luau", "return {metadata = {id = 'different'}}");
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
    write(user, "stored/package.json", R"({"id":"stored","version":"1.0.0","api_version":1,"state_version":2,"entry":"main","modules":{"main":"init.luau"}})");
    write(user, "stored/init.luau", "return {}");
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
    write(user, "stored/init.luau", "return {migrate = function() return 5 end}");
    host->reload();
    errors.clear();
    for (const auto& entry : host->diagnostics()) errors << entry.toMap()["error"].toString();
    EXPECT_TRUE(errors.contains("Migration must return an object table"));
}

TEST_F(ScriptHostTest, PackageWithRefusedStateRetriesWhenTheStateFileChangesAfterARescan)
{
    QDir().mkpath(user.filePath("stored"));
    write(user, "stored/package.json", R"({"id":"stored","version":"1.0.0","api_version":1,"state_version":2,"entry":"main","modules":{"main":"init.luau"}})");
    write(user, "stored/init.luau", "return {}");
    ScriptState::Snapshot saved;
    ASSERT_TRUE(ScriptState(stateDir.path()).write("stored", 3, 0, {{"x", 1}}, saved).isEmpty());
    host->setEnabled(true);
    ASSERT_TRUE(host->scriptIds().isEmpty());
    // An unrelated rescan finds the package unchanged and does not load it again.
    write(user, "other.luau", "return {}");
    host->reload();
    ASSERT_EQ(host->scriptIds(), QStringList{"other"});
    await([] { return false; }, 300);
    // The state file is still watched: removing it retries the package.
    ASSERT_TRUE(QFile::remove(stateDir.filePath("stored.json")));
    EXPECT_TRUE(await([&] { return host->scriptIds().contains("stored"); }));
}

TEST_F(ScriptHostTest, SupersededPackageNeverLoadsAgainstTheWinnersState)
{
    // Both copies share stored.json. The winner writes it at a newer version
    // than the loser can read, so loading the loser would fail.
    auto install = [&](const QTemporaryDir& dir, int stateVersion, const QByteArray& source) {
        QDir().mkpath(dir.filePath("dup"));
        write(dir, "dup/package.json", QString(R"({"id":"dup","version":"1.0.0","api_version":1,"state_version":%1,"entry":"main","modules":{"main":"init.luau"}})")
            .arg(stateVersion).toUtf8());
        write(dir, "dup/init.luau", source);
    };
    install(system, 1, "return {on_load=function() return afterglow.copy('loser loaded') end}");
    install(user, 2, "return {on_load=function(ev) return afterglow.state_write({n=1},ev.storage.revision) end}");
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
    installPackage("project-navigator");
    write(user, "project-navigator/config.json", R"({"roots":[],"fd":false,"zoxide":false})");
    QFile state(stateDir.filePath("project-navigator.json"));
    QDir().mkpath(stateDir.path());
    ASSERT_TRUE(state.open(QIODevice::WriteOnly));
    state.write(R"({"format":1,"version":1,"revision":1,"value":{"favorites":["/a"],"recent":["/b"]}})");
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

TEST_F(ScriptHostTest, UnloadEffectsAreIgnored)
{
    write(user, "unload.luau", "return {on_unload = function() return afterglow.copy('ignored') end}");
    host->setEnabled(true);
    write(user, "unload.luau", "return {}");
    host->reload();
    host->setEnabled(false);
    for (int i = 0; i < 5; ++i) QCoreApplication::processEvents();
    EXPECT_TRUE(rec.applied.isEmpty());
    EXPECT_EQ(cleaned.size(), 2);
}

TEST_F(ScriptHostTest, UnloadRunsOnRetireButNotForADisabledScript)
{
    // An on_unload that raises an error reaches the failed callback: that shows it ran.
    write(user, "observed.luau", "return {on_unload = function() error('unloaded') end}");
    write(user, "broken.luau", R"(return {metadata={actions={{id="go"}}},
        run_action=function() error("boom") end, on_unload=function() error("unloaded") end})");
    host->setEnabled(true);
    while (!host->isDisabled("broken")) {
        ASSERT_TRUE(host->runAction("broken.go", 7));
        ASSERT_LE(failedOwners.size(), 5);
    }
    const auto failures = failedOwners.size();
    write(user, "observed.luau", "return {}");
    host->reload();
    EXPECT_EQ(failedOwners.size(), failures + 1); // the replaced instance ran on_unload
    host->setEnabled(false);
    EXPECT_EQ(failedOwners.size(), failures + 1); // the disabled script did not
}

TEST_F(ScriptHostTest, ReadFileHelperResultsReachOnResultByTaskName)
{
    write(files, "note.txt", "hello");
    write(user, "reader.luau", QStringLiteral(R"(
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
    for (const auto& name : names) installPackage(name);
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
    ASSERT_TRUE(host->runAction("create-and-title.split", 7));
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

TEST_F(ScriptHostTest, TabStatusKeepsGitFromRunningRepositoryCommands)
{
    const QString gitProgram = QStandardPaths::findExecutable("git");
    if (gitProgram.isEmpty()) GTEST_SKIP() << "git is not installed";
    // Only the fixture's config: the developer's global core.fsmonitor=true, for
    // one, would start a daemon on the repository that outlives the test. The
    // package's git inherits these too.
    const std::pair<const char*, QByteArray> isolated[] = {{"GIT_CONFIG_NOSYSTEM", "1"}, {"GIT_CONFIG_GLOBAL", "/dev/null"}};
    std::vector<std::optional<QByteArray>> saved;
    for (const auto& [name, value] : isolated) {
        saved.push_back(qEnvironmentVariableIsSet(name) ? std::optional(qgetenv(name)) : std::nullopt);
        qputenv(name, value);
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
    write(files, "repository/.gitattributes", "* filter=evil\n");
    write(files, "repository/file", "text\n");
    ASSERT_TRUE(git({"add", ".gitattributes", "file"}));
    ASSERT_TRUE(git({"config", "filter.evil.clean", "touch '" + marker + "'; cat"}));
    {
        QFile f(files.filePath("repository/file"));
        ASSERT_TRUE(f.open(QIODevice::ReadWrite));
        ASSERT_TRUE(f.setFileTime(QDateTime::fromSecsSinceEpoch(1577836800), QFileDevice::FileModificationTime));
    }

    installPackage("tab-status");
    write(user, "tab-status/config.json", R"({"git":true,"segments":["git"],"tab_title":false})");
    paneState[7]["cwd"] = repository;
    paneState[7]["context"] = "";
    tabs = {QVariantMap{{"active_pane", 7}}};
    auto shows = [&](const QString& text) {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 5000) {
            host->dispatch("on_timer", 0, {{"name", "refresh"}});
            for (int i = 0; i < 20; ++i) { QCoreApplication::processEvents(); QThread::msleep(5); }
            if (!rec.applied.isEmpty() && rec.applied.last().value("text") == text) return true;
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
    write(user, "tab-status/config.json", R"({"git":true,"git_changes":true,"segments":["git"],"tab_title":false})");
    host->reload();
    ASSERT_TRUE(shows("trunk *"));
    EXPECT_TRUE(QFile::exists(marker));
}

TEST_F(ScriptHostTest, NavigatorOpensAProjectWithoutAskingForALayout)
{
    installPackage("project-navigator");
    write(user, "project-navigator/config.json", R"({"roots":[],"fd":false,"zoxide":false})");
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
    ASSERT_TRUE(host->runAction("project-navigator.open", 7));
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
    installPackage("project-navigator");
    write(user, "project-navigator/config.json", R"({"roots":[],"fd":false,"zoxide":false})");
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
    ASSERT_TRUE(host->runAction("project-navigator.open", 7));
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
    installPackage("scene-controls");
    applyHook = [](const QVariantMap& e) -> ScriptHost::EffectResult {
        if (e["kind"] == QLatin1String("shaders"))
            return {{}, {{"shaders", QVariantList{QVariantMap{{"instance", "i"}, {"shader", "bloom"}, {"layer", "pane"},
                {"parameters", QVariantList{QVariantMap{{"id", "strength"}, {"name", "Strength"}, {"type", "float"}, {"supported", true}}}},
                {"values", QVariantMap{{"strength", 0.5}}}}}}}, {}};
        if (e["kind"] == QLatin1String("select")) return {"Dialog canceled", {}, "cancelled"};
        return {};
    };
    host->setEnabled(true);
    ASSERT_TRUE(host->runAction("scene-controls.open", 7));
    for (int i = 0; i < 10; ++i) QCoreApplication::processEvents();
    for (const auto& e : rec.applied) EXPECT_NE(e["kind"], "notify");
}

TEST_F(ScriptHostTest, SceneControlsKeepsOnlyAcceptedValuesInItsLayer)
{
    installPackage("scene-controls");
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
    auto pump = [] { for (int i = 0; i < 10; ++i) QCoreApplication::processEvents(); };
    auto edit = [&](const QString& id, const QString& value) {
        choice = id;
        pendingReply = {};
        ASSERT_TRUE(host->runAction("scene-controls.open", 7));
        pump();
        ASSERT_TRUE(bool(pendingReply));
        pendingReply({{}, {{"text", value}}, {}});
        pump();
    };
    edit("strength", "9");
    ASSERT_EQ(sent.size(), 1);
    EXPECT_EQ(sent.last(), QVariantMap({{"strength", 9}}));
    // The refused strength is not sent again with the next edit.
    edit("radius", "2");
    ASSERT_EQ(sent.size(), 2);
    EXPECT_EQ(sent.last(), QVariantMap({{"radius", 2}}));
    edit("strength", "0.5");
    EXPECT_EQ(sent.last(), QVariantMap({{"radius", 2}, {"strength", 0.5}}));

    // Each edit applies six effects: wait for the second of 20 applied
    // effects to pass, so that the next twelve are not rate_limited.
    await([] { return false; }, 1100);
    // After a preset, an edit keeps the preset's values, not the effective ones.
    choice = "@preset";
    ASSERT_TRUE(host->runAction("scene-controls.open", 7));
    pump();
    edit("radius", "4");
    EXPECT_EQ(sent.last(), QVariantMap({{"strength", 0.8}, {"radius", 4}}));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
