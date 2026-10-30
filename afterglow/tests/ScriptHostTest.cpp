// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "scripting/ScriptHost.h"
#include "scripting/ScriptTasks.h"
#include "scripting/ScriptState.h"
#include <QElapsedTimer>
#include <QThread>
#include <QSaveFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <signal.h>
#include <cerrno>
#include <sys/stat.h>

#include <QCoreApplication>
#include <QDir>
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
        ASSERT_TRUE(system.isValid() && user.isValid());
        ScriptHost::Callbacks c;
        for (quint32 pane : {7u, 8u, 9u}) {
            paneState.insert(pane, {{QStringLiteral("id"), pane}, {QStringLiteral("window"), pane == 9 ? 2 : 1},
                                   {QStringLiteral("tab_id"), pane == 9 ? 20 : 10}, {QStringLiteral("tab"), 0},
                                   {QStringLiteral("focused"), false}, {QStringLiteral("command"), QString()},
                                   {QStringLiteral("title"), QStringLiteral("fish")}});
        }
        c.paneInfo = [this](quint32 pane) { return paneState.value(pane); };
        c.state = [this] {
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
        c.applyOwned = [this, apply = c.apply](const QString&, const QVariantMap& effect, std::function<void(ScriptHost::EffectResult)> complete) {
            if (effect.value(QStringLiteral("kind")) == QLatin1String("prompt")) {
                pendingReply = std::move(complete);
                return ScriptHost::EffectResult{{}, {}, {}, true};
            }
            return apply(effect);
        };
        host = std::make_unique<ScriptHost>(c);
        host->setStateDirectory(user.filePath("state"));
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
        ASSERT_TRUE(QDir().mkpath(user.filePath(name)));
        for (const auto& file : package.entryList(QDir::Files))
            ASSERT_TRUE(QFile::copy(package.filePath(file),user.filePath(name+"/"+file)));
    }

    QVariantList windows, tabs;
    QTemporaryDir system;
    QTemporaryDir user;
    Recorder rec;
    QMap<quint32, QVariantMap> paneState;
    std::function<ScriptHost::EffectResult(const QVariantMap&)> applyHook;
    QStringList cleaned;
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
    EXPECT_EQ(rec.applied[1].value(QStringLiteral("text")), QStringLiteral("2"));
    EXPECT_EQ(paneState[7].value(QStringLiteral("title")), QStringLiteral("fish"));
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
    QCoreApplication::processEvents();
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
    QFile example(QStringLiteral(AFTERGLOW_PHOSPHOR_SCRIPTS) + QStringLiteral("/../../scripts/build-inspector.luau"));
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

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
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
    host->dispatch("on_bell",7); EXPECT_EQ(rec.applied.last().value("text"),"2");
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
    ScriptState store(user.filePath("state"));
    ASSERT_TRUE(store.read("bundle",stored).isEmpty()); ASSERT_EQ(stored.version,1);
    manifest(2);
    write(user,"bundle/init.luau",R"(
return {migrate=function(value,from,to) assert(from==1 and to==2); return {count=value.count+1} end,
on_load=function(ev) error("broken initialization") end}
)");
    host->reload();
    host->dispatch("on_bell",7); EXPECT_EQ(rec.applied.last().value("text"),"original");
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
    ScriptState::Snapshot stored; ScriptState store(user.filePath("state"));
    ASSERT_TRUE(store.read("project-navigator",stored).isEmpty());
    EXPECT_TRUE(stored.value.value("favorites").toMap().value("/tmp/navigator-project").toBool());
    EXPECT_EQ(stored.value.value("templates").toMap().value("/tmp/navigator-project"),"dev");
    host->setEnabled(false); host->setEnabled(true);
    EXPECT_EQ(host->scriptIds(),QStringList{"project-navigator"});
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
    source.replace("command={\"fd\"", "command={\"afterglow-nonexistent-fd-test\"");
    write(user,"project-navigator/providers.luau",source);
    auto pane=paneState[7];pane["cwd"]="/tmp";paneState[7]=pane;
    bool selected=false;
    applyHook=[&](const QVariantMap& e) -> ScriptHost::EffectResult {
        if (e.value("kind")=="list_projects") return {{},{{"names",QStringList{}}},{}};
        if (e.value("kind")=="select") {
            selected=true;
            EXPECT_TRUE(e.value("title").toString().contains("fd-1"));
            EXPECT_FALSE(e.value("items").toList().isEmpty());
            return {"Dialog cancelled",{},"cancelled"};
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
    ASSERT_TRUE(ScriptState(user.filePath("state")).read("project-navigator",stored).isEmpty());
    EXPECT_TRUE(stored.value.value("favorites").toMap().value("/tmp").toBool());
}

TEST_F(ScriptHostTest, PackageReloadAndRemovalRetireJobsTimersWatchesAndUi)
{
    QDir().mkpath(user.filePath("lifecycle"));
    write(user, "lifecycle/package.json", R"({"id":"lifecycle","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"init.luau"}})");
    write(user, "watched", "original");
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
        QJsonObject value{{"generation",generation},{"pid",user.filePath("pid")},{"watch",user.filePath("watched")}};
        write(user,"lifecycle/config.json",QJsonDocument(value).toJson());
    };
    auto pid = [&]() { QFile file(user.filePath("pid")); return file.open(QIODevice::ReadOnly) ? file.readAll().trimmed().toLongLong() : 0; };
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
        write(user,"watched",QByteArray::number(generation));
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
    write(user,"watched","after removal");
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
    ASSERT_TRUE(ScriptState(user.filePath("state")).read("project-autosave",value).isEmpty());
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
 assert(s.git("## main...origin/main\n M file.cpp\n")=="main *")
 assert(s.git("## No commits yet on feature\n")=="feature")
 assert(s.git("fatal: not a repository")=="")
 local text,title=s.render({cwd="/tmp/project",command="",context="remote"},"main",1,{segments={"context","cwd","unknown","exit"},separator=" | ",tab_title=false})
 assert(text=="remote | project | exit 1" and title=="")
 local bounded=s.render({cwd=string.rep("x",1000),command=string.rep("y",1000)},"",nil,{})
 assert(#bounded<=500)
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
    EXPECT_EQ(rec.applied.last().value("kind"),"notify");
    EXPECT_EQ(rec.applied.last().value("body"),"Disk unavailable");
    fail=false;rec.applied.clear();
    host->dispatch("on_timer",0,{{"name","autosave"}});pump();
    ASSERT_EQ(rec.applied.size(),1);EXPECT_EQ(rec.applied.last().value("kind"),"save_project");
    rec.applied.clear();ASSERT_TRUE(host->runAction("project-autosave.restore",7));pump();
    ASSERT_EQ(rec.applied.size(),2);EXPECT_EQ(rec.applied.last().value("kind"),"select");
    ASSERT_TRUE(host->runAction("project-autosave.disable",7));pump();
    ScriptState::Snapshot stored;
    ASSERT_TRUE(ScriptState(user.filePath("state")).read("project-autosave",stored).isEmpty());
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
