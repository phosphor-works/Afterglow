// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "scripting/ScriptPackage.h"
#include "scripting/ScriptState.h"
#include "scripting/ScriptFiles.h"
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <algorithm>
#include <limits>
using namespace Afterglow;
namespace {
void put(const QString& path, const QByteArray& bytes) { QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path); ASSERT_TRUE(f.open(QIODevice::WriteOnly)); ASSERT_EQ(f.write(bytes), bytes.size()); }
QByteArray manifest(const QJsonObject& extra = {}) {
    QJsonObject doc{{"id","test"},{"version","1.0.0"},{"api_version",1},{"entry","main"},{"modules",QJsonObject{{"main","init.luau"}}}};
    for (auto it = extra.begin(); it != extra.end(); ++it) doc[it.key()] = it.value();
    return QJsonDocument(doc).toJson();
}
}
TEST(ScriptPackage, BoundsSourcesAndRejectsEscapesAndIncompatibleVersions)
{
    QTemporaryDir parent;
    const QString root = parent.filePath("package");
    QDir dir(root);
    const auto file = dir.filePath("package.json");
    put(dir.filePath("init.luau"), "return {}");
    put(file, manifest());
    ScriptPackage p;
    EXPECT_TRUE(ScriptPackage::read(file, p).isEmpty());
    // The targets exist, so only the containment rules can refuse them.
    put(parent.filePath("outside.luau"), "return {}");
    for (const auto& path : {QStringLiteral("../outside.luau"), parent.filePath("outside.luau")}) {
        put(file, manifest({{"modules", QJsonObject{{"main", path}}}}));
        EXPECT_TRUE(ScriptPackage::read(file, p).contains("must remain inside")) << path.toStdString();
    }
    put(file, manifest({{"api_version",2}}));
    EXPECT_FALSE(ScriptPackage::read(file, p).isEmpty());
    put(file, manifest()); put(dir.filePath("init.luau"), QByteArray(256 * 1024 + 1, ' '));
    EXPECT_FALSE(ScriptPackage::read(file, p).isEmpty());
    QTemporaryDir outside; put(outside.filePath("module.luau"), "return {}");
    ASSERT_TRUE(QFile::link(outside.filePath("module.luau"), dir.filePath("link.luau")));
    put(file, manifest({{"modules",QJsonObject{{"main","link.luau"}}}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("escapes its root"));
}
TEST(ScriptState, AtomicRevisionChecksAndRoundTrip)
{
    QTemporaryDir dir; ScriptState store(dir.path()); ScriptState::Snapshot s;
    ASSERT_TRUE(store.read("navigator", s).isEmpty()); EXPECT_EQ(s.revision,0);
    const QVariantMap value{{"favorites",QVariantList{QStringLiteral("/tmp/project")}}, {"count",2}};
    ASSERT_TRUE(store.write("navigator",1,0,value,s).isEmpty()); EXPECT_EQ(s.revision,1);
    ScriptState::Snapshot read; ASSERT_TRUE(store.read("navigator",read).isEmpty()); EXPECT_EQ(read.value,value);
    EXPECT_FALSE(store.write("navigator",1,0,{},s).isEmpty());
    ASSERT_TRUE(store.write("navigator",2,1,{{"migrated",true}},s).isEmpty());
    EXPECT_FALSE(store.write("navigator",1,2,{},s).isEmpty());
    EXPECT_FALSE(store.write("../escape",1,0,{},s).isEmpty());
}
TEST(ScriptState, RejectsCorruptionSymlinksAndUnserializableValues)
{
    QTemporaryDir dir; ScriptState store(dir.path()); ScriptState::Snapshot s;
    put(dir.filePath("bad.json"), "{oops");
    EXPECT_FALSE(store.read("bad",s).isEmpty()); EXPECT_FALSE(store.write("bad",1,0,{},s).isEmpty());
    // A link to a valid state: only the symlink rule can refuse it.
    ASSERT_TRUE(store.write("good",1,0,{{"x",1}},s).isEmpty());
    ASSERT_TRUE(QFile::link(dir.filePath("good.json"),dir.filePath("link.json")));
    EXPECT_TRUE(store.read("link",s).contains("symlinks"));
    EXPECT_FALSE(store.write("link",1,1,{{"x",2}},s).isEmpty());
    ASSERT_TRUE(store.read("good",s).isEmpty()); EXPECT_EQ(s.value.value("x").toInt(), 1);
    EXPECT_FALSE(ScriptState::validate({{"nan",std::numeric_limits<double>::quiet_NaN()}}).isEmpty());
    EXPECT_FALSE(ScriptState::validate({{"large",QString(300000,'x')}}).isEmpty());
    EXPECT_FALSE(ScriptState::validate({{"huge",1e300}}).isEmpty());
    EXPECT_FALSE(ScriptState::validate({{QString(257,'k'),1}}).isEmpty());
    QVariantMap nested;
    for (int i=0;i<20;++i) nested={{"child",nested}};
    EXPECT_FALSE(ScriptState::validate(nested).isEmpty());
}

TEST(ScriptState, LimitsSizeCountAndDepth)
{
    // Every string is under its own limit, so only the encoded size refuses this.
    QVariantMap large;
    for (int i = 0; i < 5; ++i) large.insert(QStringLiteral("k%1").arg(i), QString(60000, 'x'));
    EXPECT_TRUE(ScriptState::validate(large).contains("255 KiB"));
    // 8192 values, the top-level object and the list included, and no more.
    QVariantList many;
    for (int i = 0; i < 8191; ++i) many.append(i);
    EXPECT_FALSE(ScriptState::validate({{"many", many}}).isEmpty());
    many.removeLast();
    EXPECT_TRUE(ScriptState::validate({{"many", many}}).isEmpty());
    // Sixteen tables, the top-level object included, and no more.
    QVariantMap deep{{"leaf", 1}};
    for (int i = 0; i < 15; ++i) deep = {{"child", deep}};
    EXPECT_TRUE(ScriptState::validate(deep).isEmpty());
    EXPECT_FALSE(ScriptState::validate({{"child", deep}}).isEmpty());
}

TEST(ScriptPackage, ConfigOverridesAndDeclaredAssetsStayLocal)
{
    QTemporaryDir dir;
    put(dir.filePath("init.luau"),"return {}"); put(dir.filePath("theme.json"),"{}");
    put(dir.filePath("package.json"),manifest({{"config",QJsonObject{{"label","default"}}},{"assets",QJsonObject{{"theme","theme.json"}}}}));
    put(dir.filePath("config.json"),"{\"label\":\"user\"}");
    ScriptPackage p;
    ASSERT_TRUE(ScriptPackage::read(dir.filePath("package.json"),p).isEmpty());
    EXPECT_EQ(p.config.value("label"),"user"); EXPECT_EQ(p.assets.value("theme").toString(),dir.filePath("theme.json"));
    // A real file outside the package, so only the containment rule refuses it.
    QTemporaryDir outer; const QString nested = outer.filePath("package");
    put(outer.filePath("theme.json"),"{}"); put(nested + "/init.luau","return {}");
    put(nested + "/package.json",manifest({{"assets",QJsonObject{{"escape","../theme.json"}}}}));
    EXPECT_TRUE(ScriptPackage::read(nested + "/package.json",p).contains("must remain inside"));
}

TEST(ScriptFiles, RejectsSpecialFilesAndChecksOpenedContainment)
{
    QTemporaryDir inside, outside;
    const auto fifo = inside.filePath("fifo");
    ASSERT_EQ(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
    QByteArray bytes("unchanged");
    EXPECT_FALSE(readScriptFile(fifo, 1024, bytes).isEmpty());
    EXPECT_EQ(bytes, "unchanged");
    put(outside.filePath("source"), "return {}");
    ASSERT_TRUE(QFile::link(outside.filePath("source"), inside.filePath("link")));
    EXPECT_FALSE(readScriptFile(inside.filePath("link"), 1024, bytes, inside.path()).isEmpty());
    EXPECT_FALSE(readScriptFile(inside.filePath("link"), 1024, bytes, {}, true).isEmpty());
    EXPECT_TRUE(readScriptFile(inside.filePath("link"), 1024, bytes).isEmpty());
    EXPECT_EQ(bytes, "return {}");
    EXPECT_FALSE(readScriptFile(outside.filePath("source"), 4, bytes).isEmpty());
}

TEST(ScriptPackage, ShaderPacksCarryThePackageNameAndSuggestionsNameThem)
{
    QTemporaryDir dir;
    const auto file = dir.filePath("package.json");
    put(dir.filePath("init.luau"), "return {}");
    put(dir.filePath("shaders/wall/metadata.json"), R"({"id":"test.wall","name":"Wall","fragmentShader":"effect.frag"})");
    put(dir.filePath("shaders/wall/effect.frag"), "vec4 pTerminal(vec2 uv) { return terminalTexel(uv); }");
    const QJsonObject shaders{{"path", "shaders"}, {"suggests", QJsonObject{{"overview", "wall"}}}};
    put(file, manifest({{"shaders", shaders}, {"pulses", QJsonObject{{"ripple", 4}}}}));
    ScriptPackage p;
    ASSERT_TRUE(ScriptPackage::read(file, p).isEmpty());
    EXPECT_EQ(QFileInfo(p.shaderDirectory).fileName(), "shaders");
    EXPECT_EQ(p.shaderPacks, QStringList{"wall"});
    EXPECT_EQ(p.suggests.value("overview"), "wall");
    EXPECT_EQ(p.pulses.value("ripple"), 4);
    // Suggestions name the package's own packs and real Shaders keys.
    for (const auto& bad : {QJsonObject{{"overview", "missing"}}, QJsonObject{{"palette", "wall"}}}) {
        put(file, manifest({{"shaders", QJsonObject{{"path", "shaders"}, {"suggests", bad}}}}));
        EXPECT_FALSE(ScriptPackage::read(file, p).isEmpty());
    }
    // Pulse names map to the numbers packs own: 4 to 255.
    for (const auto number : {3, 256}) {
        put(file, manifest({{"pulses", QJsonObject{{"ripple", number}}}}));
        EXPECT_FALSE(ScriptPackage::read(file, p).isEmpty());
    }
    // A pack must declare "<package>.<pack>", so it cannot replace a bundled one.
    put(dir.filePath("shaders/wall/metadata.json"), R"({"id":"pane-deck","name":"Wall","fragmentShader":"effect.frag"})");
    put(file, manifest({{"shaders", shaders}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("test.wall"));
    put(dir.filePath("shaders/wall/metadata.json"), R"({"id":"test.wall","name":"Wall","fragmentShader":"effect.frag"})");
    // A real pack directory outside the package, so only the containment rule refuses it.
    QTemporaryDir parent; const QString root = parent.filePath("package");
    put(root + "/init.luau", "return {}");
    put(parent.filePath("elsewhere/wall/metadata.json"), R"({"id":"test.wall","name":"Wall","fragmentShader":"effect.frag"})");
    put(root + "/package.json", manifest({{"shaders", QJsonObject{{"path", "../elsewhere"}}}}));
    EXPECT_TRUE(ScriptPackage::read(root + "/package.json", p).contains("must remain inside"));
    // A symlinked pack that points out of the package is checked like a directory.
    QTemporaryDir outside;
    put(outside.filePath("crt/metadata.json"), R"({"id":"crt","name":"CRT","fragmentShader":"effect.frag"})");
    ASSERT_TRUE(QFile::link(outside.filePath("crt"), dir.filePath("shaders/crt")));
    put(file, manifest({{"shaders", shaders}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("escapes its root"));
    QFile::remove(dir.filePath("shaders/crt"));
    // So is the packs' include directory, although it is not a pack.
    put(outside.filePath("include/common.glsl"), "");
    ASSERT_TRUE(QFile::link(outside.filePath("include"), dir.filePath("shaders/shared")));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("escapes its root"));
    QFile::remove(dir.filePath("shaders/shared"));
    // A non-string path is an error, not the default directory.
    put(file, manifest({{"shaders", QJsonObject{{"path", 5}}}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("shaders.path"));
    // Built-in pulse names and plain numbers already select effects.
    for (const auto& name : {QStringLiteral("flash"), QStringLiteral("portal"), QStringLiteral("shake"), QStringLiteral("7")}) {
        put(file, manifest({{"pulses", QJsonObject{{name, 9}}}}));
        EXPECT_TRUE(ScriptPackage::read(file, p).contains("reserved")) << name.toStdString();
    }
    // "shared" holds includes and is skipped; "none" is reserved.
    put(dir.filePath("shaders/shared/common.glsl"), "");
    put(file, manifest({{"shaders", shaders}}));
    ASSERT_TRUE(ScriptPackage::read(file, p).isEmpty());
    EXPECT_EQ(p.shaderPacks, QStringList{"wall"});
    put(dir.filePath("shaders/none/metadata.json"), R"({"id":"test.none"})");
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("reserved"));
}

TEST(ScriptPackage, RejectsBadManifestsWithTheRealError)
{
    QTemporaryDir dir;
    const auto file = dir.filePath("package.json");
    put(dir.filePath("init.luau"), "return {}");
    ScriptPackage p;
    put(file, "{");
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("Invalid package JSON"));
    put(file, QByteArray(64 * 1024 + 1, ' '));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("byte limit"));
    for (const auto& version : {QJsonValue(0), QJsonValue(10001), QJsonValue(1.5), QJsonValue("2")}) {
        put(file, manifest({{"state_version", version}}));
        EXPECT_TRUE(ScriptPackage::read(file, p).contains("state_version"));
    }
    put(file, manifest({{"config", 3}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("config must be an object"));
    put(file, manifest());
    put(dir.filePath("config.json"), "[1]");
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("Invalid package JSON"));
    QFile::remove(dir.filePath("config.json"));
    // A dependency with another version than declared.
    put(dir.filePath("vendor/lib/package.json"), R"({"id":"lib","version":"2.0.0","api_version":1,"entry":"main","modules":{"main":"lib.luau"}})");
    put(dir.filePath("vendor/lib/lib.luau"), "return {}");
    put(file, manifest({{"dependencies", QJsonObject{{"lib", QJsonObject{{"path", "vendor/lib"}, {"version", "1.0.0"}}}}}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("dependency version"));
    // More than 16 packages in one tree: each depends on the next.
    for (int i = 0; i < 17; ++i) {
        const QString next = i < 16 ? QStringLiteral(R"("dependencies":{"d":{"path":"d","version":"1.0.0"}},)") : QString();
        put(dir.filePath(QStringLiteral("chain") + QString("/d").repeated(i) + "/package.json"),
            QStringLiteral(R"({"id":"d","version":"1.0.0","api_version":1,"entry":"main",%1"modules":{"main":"m.luau"}})").arg(next).toUtf8());
        put(dir.filePath(QStringLiteral("chain") + QString("/d").repeated(i) + "/m.luau"), "return {}");
    }
    put(file, manifest({{"dependencies", QJsonObject{{"d", QJsonObject{{"path", "chain"}, {"version", "1.0.0"}}}}}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("Too many dependencies"));
    // Assets: a count and a size limit.
    QJsonObject assets;
    for (int i = 0; i < 33; ++i) assets.insert(QStringLiteral("a%1").arg(i), "init.luau");
    put(file, manifest({{"assets", assets}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("at most 32"));
    put(dir.filePath("big.bin"), QByteArray(4 * 1024 * 1024 + 1, 'x'));
    put(file, manifest({{"assets", QJsonObject{{"big", "big.bin"}}}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("asset"));
    // Shader packs: at least one, at most 16.
    QDir().mkpath(dir.filePath("empty"));
    put(file, manifest({{"shaders", QJsonObject{{"path", "empty"}}}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("1 to 16 packs"));
    for (int i = 0; i < 17; ++i)
        put(dir.filePath(QStringLiteral("many/p%1/metadata.json").arg(i)), QStringLiteral(R"({"id":"test.p%1"})").arg(i).toUtf8());
    put(file, manifest({{"shaders", QJsonObject{{"path", "many"}}}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("1 to 16 packs"));
}

TEST(ScriptPackage, ModuleErrorsNameTheirFileAndLine)
{
    QTemporaryDir dir;
    const auto file = dir.filePath("package.json");
    put(dir.filePath("init.luau"), "local util = require('util')\nreturn {}");
    put(dir.filePath("util.luau"), "local x = 1\nlocal y = 2\nerror('broken')\nreturn {}");
    put(file, manifest({{"modules", QJsonObject{{"main", "init.luau"}, {"util", "util.luau"}}}}));
    ScriptPackage p;
    ASSERT_TRUE(ScriptPackage::read(file, p).isEmpty());
    ASSERT_EQ(p.modules.size(), 2);
    // Find the generated line of util.luau's third line, and map it back.
    const auto util = std::find_if(p.modules.cbegin(), p.modules.cend(), [](const auto& m) { return m.file == "util.luau"; });
    ASSERT_NE(util, p.modules.cend());
    const QByteArrayList lines = p.source.split('\n');
    ASSERT_GE(lines.size(), util->line + 2);
    EXPECT_EQ(lines[util->line + 1], "error('broken')");
    const QString message = QStringLiteral("package.json:%1: broken").arg(util->line + 2);
    EXPECT_EQ(ScriptPackage::locate(p.modules, message), QStringLiteral("util.luau:3: broken"));
    EXPECT_EQ(ScriptPackage::locate(p.modules, "package.json:1: loader"), QStringLiteral("package.json:1: loader"));
}
