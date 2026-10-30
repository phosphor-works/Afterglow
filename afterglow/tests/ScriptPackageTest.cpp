// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "scripting/ScriptPackage.h"
#include "scripting/ScriptState.h"
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
