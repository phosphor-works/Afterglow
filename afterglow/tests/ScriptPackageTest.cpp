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
    QTemporaryDir dir;
    const auto file = dir.filePath("package.json");
    put(dir.filePath("init.luau"), "return {}");
    put(file, manifest());
    ScriptPackage p;
    EXPECT_TRUE(ScriptPackage::read(file, p).isEmpty());
    for (const auto path : {"../outside.luau", "/tmp/outside.luau"}) {
        put(file, manifest({{"modules", QJsonObject{{"main", path}}}}));
        EXPECT_FALSE(ScriptPackage::read(file, p).isEmpty());
    }
    put(file, manifest({{"api_version",2}}));
    EXPECT_FALSE(ScriptPackage::read(file, p).isEmpty());
    put(file, manifest()); put(dir.filePath("init.luau"), QByteArray(256 * 1024 + 1, ' '));
    EXPECT_FALSE(ScriptPackage::read(file, p).isEmpty());
    QTemporaryDir outside; put(outside.filePath("module.luau"), "return {}");
    ASSERT_TRUE(QFile::link(outside.filePath("module.luau"), dir.filePath("link.luau")));
    put(file, manifest({{"modules",QJsonObject{{"main","link.luau"}}}}));
    EXPECT_FALSE(ScriptPackage::read(file, p).isEmpty());
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
    ASSERT_TRUE(QFile::link(dir.filePath("bad.json"),dir.filePath("link.json")));
    EXPECT_FALSE(store.read("link",s).isEmpty()); EXPECT_FALSE(store.write("link",1,0,{},s).isEmpty());
    EXPECT_FALSE(ScriptState::validate({{"nan",std::numeric_limits<double>::quiet_NaN()}}).isEmpty());
    EXPECT_FALSE(ScriptState::validate({{"large",QString(300000,'x')}}).isEmpty());
    QVariantMap nested;
    for (int i=0;i<20;++i) nested={{"child",nested}};
    EXPECT_FALSE(ScriptState::validate(nested).isEmpty());
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
    put(dir.filePath("package.json"),manifest({{"assets",QJsonObject{{"escape","../theme.json"}}}}));
    EXPECT_FALSE(ScriptPackage::read(dir.filePath("package.json"),p).isEmpty());
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
    put(file, manifest({{"shaders", QJsonObject{{"path", "../elsewhere"}}}}));
    EXPECT_FALSE(ScriptPackage::read(file, p).isEmpty());
}
