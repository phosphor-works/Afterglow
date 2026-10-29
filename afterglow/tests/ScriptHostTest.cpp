// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "scripting/ScriptHost.h"

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
        c.paneInfo = [](quint32 pane) {
            if (pane != 7) {
                return QVariantMap{};
            }
            return QVariantMap{{QStringLiteral("id"), 7u}, {QStringLiteral("focused"), false},
                               {QStringLiteral("title"), QStringLiteral("fish")}};
        };
        c.apply = [this](const QVariantMap& e) {
            rec.applied.append(e);
            return QString();
        };
        host = std::make_unique<ScriptHost>(c);
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

    QTemporaryDir system;
    QTemporaryDir user;
    Recorder rec;
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

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
