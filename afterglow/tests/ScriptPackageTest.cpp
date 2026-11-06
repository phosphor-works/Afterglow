// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "scripting/ScriptPackage.h"
#include "scripting/ScriptState.h"
#include "scripting/ScriptFiles.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
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
// A failed write stops the test, not only the helper.
#define PUT(...) ASSERT_NO_FATAL_FAILURE(put(__VA_ARGS__))
}
TEST(ScriptPackage, BoundsSourcesAndRejectsEscapesAndIncompatibleVersions)
{
    QTemporaryDir parent;
    const QString root = parent.filePath("package");
    QDir dir(root);
    const auto file = dir.filePath("package.json");
    PUT(dir.filePath("init.luau"), "return {}");
    PUT(file, manifest());
    ScriptPackage p;
    EXPECT_TRUE(ScriptPackage::read(file, p).isEmpty());
    // The targets exist, so only the containment rules can refuse them.
    PUT(parent.filePath("outside.luau"), "return {}");
    for (const auto& path : {QStringLiteral("../outside.luau"), parent.filePath("outside.luau")}) {
        PUT(file, manifest({{"modules", QJsonObject{{"main", path}}}}));
        EXPECT_TRUE(ScriptPackage::read(file, p).contains("must remain inside")) << path.toStdString();
    }
    PUT(file, manifest({{"api_version",2}}));
    EXPECT_EQ(ScriptPackage::read(file, p), "package.json: api_version must be 1");
    PUT(file, manifest());
    PUT(dir.filePath("init.luau"), QByteArray(256 * 1024 + 1, ' '));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("byte limit"));
    PUT(dir.filePath("init.luau"), "return {}");
    // An id that `aglow run` would read as an option.
    PUT(file, manifest({{"id", "-test"}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).startsWith("package.json: id must be 1 to 64"));
    QTemporaryDir outside; PUT(outside.filePath("module.luau"), "return {}");
    ASSERT_TRUE(QFile::link(outside.filePath("module.luau"), dir.filePath("link.luau")));
    PUT(file, manifest({{"modules",QJsonObject{{"main","link.luau"}}}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("escapes its root"));
}
TEST(ScriptPackage, ReadsAndChecksItsCommands)
{
    QTemporaryDir dir;
    const auto file = dir.filePath("package.json");
    PUT(dir.filePath("init.luau"), "return {}");
    PUT(file, manifest({{"commands", QJsonObject{
        {"open", QJsonObject{{"summary", "Open a project"}, {"usage", "DIR [LAYOUT]"}, {"palette", "Open project…"}}},
        {"import", QJsonObject{{"summary", "Add projects from stdin"}, {"stdin", true}}}}}}));
    ScriptPackage p;
    ASSERT_TRUE(ScriptPackage::read(file, p).isEmpty());
    ASSERT_EQ(p.commands.size(), 2);
    EXPECT_EQ(p.commands[0].name, "import"); // by name
    EXPECT_TRUE(p.commands[0].input);
    EXPECT_TRUE(p.commands[0].palette.isEmpty());
    EXPECT_EQ(p.commands[1].usage, "DIR [LAYOUT]");
    EXPECT_EQ(p.commands[1].palette, QStringLiteral("Open project…"));
    EXPECT_FALSE(p.commands[1].input);
    const std::pair<QJsonValue, const char*> bad[] = {
        {QJsonArray{"open"}, "commands must map command names"},
        {QJsonObject{{"open", "Open"}}, "must be a table of fields"},
        {QJsonObject{{"open", QJsonObject{}}}, "needs a summary"},
        {QJsonObject{{"open", QJsonObject{{"summary", 3}}}}, "must be text"},
        {QJsonObject{{"open", QJsonObject{{"summary", "x"}, {"stdin", "yes"}}}}, "stdin must be a boolean"},
        {QJsonObject{{"open", QJsonObject{{"summary", "x"}, {"title", "x"}}}}, "unknown field title"},
        {QJsonObject{{"op en", QJsonObject{{"summary", "x"}}}}, "must be 1 to 64"},
        {QJsonObject{{"--help", QJsonObject{{"summary", "x"}}}}, "must not start with -"},
        {QJsonObject{{"open", QJsonObject{{"summary", " "}}}}, "needs a summary"},
        {QJsonObject{{"open", QJsonObject{{"summary", "x"}, {"palette", "  "}}}}, "palette must not be blank"},
        {QJsonObject{{"open", QJsonObject{{"summary", "a\tb"}}}}, "no control characters"},
        {QJsonObject{{"open", QJsonObject{{"summary", "x"}, {"usage", 5}}}}, "usage must be text"},
        {QJsonObject{{"open", QJsonObject{{"summary", QString(513, 'x')}}}}, "at most 512"},
    };
    for (const auto& [commands, error] : bad) {
        PUT(file, manifest({{"commands", commands}}));
        const QString message = ScriptPackage::read(file, p);
        EXPECT_TRUE(message.contains(QLatin1String(error))) << message.toStdString();
    }
    QJsonObject many;
    for (int i = 0; i <= MaxScriptCommands; ++i) many.insert(QStringLiteral("c%1").arg(i), QJsonObject{{"summary", "x"}});
    PUT(file, manifest({{"commands", many}}));
    EXPECT_EQ(ScriptPackage::read(file, p), QStringLiteral("package.json: A script has at most %1 commands").arg(MaxScriptCommands));
}
TEST(ScriptFiles, Utf8PrefixKeepsACharacterThatEndsAtTheLimit)
{
    for (const QByteArray& character : {QByteArray("\xC3\xA9"), QByteArray("\xE6\xBC\xA2"), QByteArray("\xF0\x9F\x98\x80")}) {
        const QByteArray text = "ab" + character + "cd";
        const qsizetype end = 2 + character.size();
        // Ends exactly at the limit: it stays.
        EXPECT_EQ(utf8Prefix(text, end), "ab" + character) << character.size();
        // Cut through: it goes whole.
        for (qsizetype limit = 3; limit < end; ++limit) EXPECT_EQ(utf8Prefix(text, limit), "ab") << character.size() << limit;
        EXPECT_EQ(utf8Prefix(text, end + 1), "ab" + character + "c");
    }
    EXPECT_EQ(utf8Prefix("abc", 10), "abc");
    EXPECT_EQ(utf8Prefix("abc", 0), "");
    EXPECT_EQ(utf8Prefix("abc", -1), "");
    // A stray continuation byte after a whole character ends the cut: no character runs through it, so it stays.
    EXPECT_EQ(utf8Prefix("a\xF0\x9F\x98\x80\x80" "b", 6), QByteArray("a\xF0\x9F\x98\x80\x80"));
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
    PUT(dir.filePath("bad.json"), "{oops");
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
    PUT(dir.filePath("init.luau"),"return {}");
    PUT(dir.filePath("theme.json"),"{}");
    PUT(dir.filePath("package.json"),manifest({{"config",QJsonObject{{"label","default"}}},{"assets",QJsonObject{{"theme","theme.json"}}}}));
    PUT(dir.filePath("config.json"),"{\"label\":\"user\"}");
    ScriptPackage p;
    ASSERT_TRUE(ScriptPackage::read(dir.filePath("package.json"),p).isEmpty());
    EXPECT_EQ(p.config.value("label"),"user"); EXPECT_EQ(p.assets.value("theme").toString(),dir.filePath("theme.json"));
    // A real file outside the package, so only the containment rule refuses it.
    QTemporaryDir outer; const QString nested = outer.filePath("package");
    PUT(outer.filePath("theme.json"),"{}");
    PUT(nested + "/init.luau","return {}");
    PUT(nested + "/package.json",manifest({{"assets",QJsonObject{{"escape","../theme.json"}}}}));
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
    PUT(outside.filePath("source"), "return {}");
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
    PUT(dir.filePath("init.luau"), "return {}");
    PUT(dir.filePath("shaders/wall/metadata.json"), R"({"id":"test.wall","name":"Wall","fragmentShader":"effect.frag"})");
    PUT(dir.filePath("shaders/wall/effect.frag"), "vec4 pTerminal(vec2 uv) { return terminalTexel(uv); }");
    const QJsonObject shaders{{"path", "shaders"}, {"suggests", QJsonObject{{"overview", "wall"}}}};
    PUT(file, manifest({{"shaders", shaders}, {"pulses", QJsonObject{{"ripple", 4}}}}));
    ScriptPackage p;
    ASSERT_TRUE(ScriptPackage::read(file, p).isEmpty());
    EXPECT_EQ(QFileInfo(p.shaderDirectory).fileName(), "shaders");
    EXPECT_EQ(p.shaderPacks, QStringList{"wall"});
    EXPECT_EQ(p.suggests.value("overview"), "wall");
    EXPECT_EQ(p.pulses.value("ripple"), 4);
    // Suggestions name the package's own packs and real Shaders keys.
    for (const auto& bad : {QJsonObject{{"overview", "missing"}}, QJsonObject{{"palette", "wall"}}}) {
        PUT(file, manifest({{"shaders", QJsonObject{{"path", "shaders"}, {"suggests", bad}}}}));
        EXPECT_TRUE(ScriptPackage::read(file, p).contains("suggests maps a Shaders key"));
    }
    // Pulse names map to the numbers packs own: 4 to 255.
    for (const auto number : {3, 256}) {
        PUT(file, manifest({{"pulses", QJsonObject{{"ripple", number}}}}));
        EXPECT_TRUE(ScriptPackage::read(file, p).contains("from 4 to 255")) << number;
    }
    // A pack must declare "<package>.<pack>", so it cannot replace a bundled one.
    PUT(dir.filePath("shaders/wall/metadata.json"), R"({"id":"pane-deck","name":"Wall","fragmentShader":"effect.frag"})");
    PUT(file, manifest({{"shaders", shaders}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("test.wall"));
    PUT(dir.filePath("shaders/wall/metadata.json"), R"({"id":"test.wall","name":"Wall","fragmentShader":"effect.frag"})");
    // A real pack directory outside the package, so only the containment rule refuses it.
    QTemporaryDir parent; const QString root = parent.filePath("package");
    PUT(root + "/init.luau", "return {}");
    PUT(parent.filePath("elsewhere/wall/metadata.json"), R"({"id":"test.wall","name":"Wall","fragmentShader":"effect.frag"})");
    PUT(root + "/package.json", manifest({{"shaders", QJsonObject{{"path", "../elsewhere"}}}}));
    EXPECT_TRUE(ScriptPackage::read(root + "/package.json", p).contains("must remain inside"));
    // A symlinked pack that points out of the package is checked like a directory.
    QTemporaryDir outside;
    PUT(outside.filePath("crt/metadata.json"), R"({"id":"crt","name":"CRT","fragmentShader":"effect.frag"})");
    ASSERT_TRUE(QFile::link(outside.filePath("crt"), dir.filePath("shaders/crt")));
    PUT(file, manifest({{"shaders", shaders}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("escapes its root"));
    QFile::remove(dir.filePath("shaders/crt"));
    // So is the packs' include directory, although it is not a pack.
    PUT(outside.filePath("include/common.glsl"), "");
    ASSERT_TRUE(QFile::link(outside.filePath("include"), dir.filePath("shaders/shared")));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("escapes its root"));
    QFile::remove(dir.filePath("shaders/shared"));
    // A non-string path is an error, not the default directory.
    PUT(file, manifest({{"shaders", QJsonObject{{"path", 5}}}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("shaders.path"));
    // Built-in pulse names and plain numbers already select effects.
    for (const auto& name : {QStringLiteral("flash"), QStringLiteral("portal"), QStringLiteral("shake"), QStringLiteral("7")}) {
        PUT(file, manifest({{"pulses", QJsonObject{{name, 9}}}}));
        EXPECT_TRUE(ScriptPackage::read(file, p).contains("reserved")) << name.toStdString();
    }
    // "shared" holds includes and is skipped; "none" is reserved.
    PUT(dir.filePath("shaders/shared/common.glsl"), "");
    PUT(file, manifest({{"shaders", shaders}}));
    ASSERT_TRUE(ScriptPackage::read(file, p).isEmpty());
    EXPECT_EQ(p.shaderPacks, QStringList{"wall"});
    PUT(dir.filePath("shaders/none/metadata.json"), R"({"id":"test.none"})");
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("reserved"));
}

TEST(ScriptPackage, RejectsBadManifestsWithTheRealError)
{
    QTemporaryDir dir;
    const auto file = dir.filePath("package.json");
    PUT(dir.filePath("init.luau"), "return {}");
    ScriptPackage p;
    PUT(file, "{");
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("Invalid package JSON"));
    PUT(file, QByteArray(64 * 1024 + 1, ' '));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("byte limit"));
    for (const auto& version : {QJsonValue(0), QJsonValue(10001), QJsonValue(1.5), QJsonValue("2")}) {
        PUT(file, manifest({{"state_version", version}}));
        EXPECT_TRUE(ScriptPackage::read(file, p).contains("state_version"));
    }
    PUT(file, manifest({{"config", 3}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("config must be an object"));
    PUT(file, manifest());
    PUT(dir.filePath("config.json"), "[1]");
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("Invalid package JSON"));
    QFile::remove(dir.filePath("config.json"));
    // A dependency with another version than declared.
    PUT(dir.filePath("vendor/lib/package.json"), R"({"id":"lib","version":"2.0.0","api_version":1,"entry":"main","modules":{"main":"lib.luau"}})");
    PUT(dir.filePath("vendor/lib/lib.luau"), "return {}");
    PUT(file, manifest({{"dependencies", QJsonObject{{"lib", QJsonObject{{"path", "vendor/lib"}, {"version", "1.0.0"}}}}}}));
    EXPECT_EQ(ScriptPackage::read(file, p), "vendor/lib/package.json: version is 2.0.0, but the package that depends on it declares 1.0.0");
    // require("@lib") names the package with the id lib.
    PUT(dir.filePath("vendor/lib/package.json"), R"({"id":"other","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"lib.luau"}})");
    EXPECT_EQ(ScriptPackage::read(file, p), "vendor/lib/package.json: id is other, but the package that depends on it names it lib");
    PUT(dir.filePath("vendor/lib/package.json"), R"({"id":"lib","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"lib.luau"}})");
    EXPECT_TRUE(ScriptPackage::read(file, p).isEmpty());
    // A dependency with no path, or a path that is not text.
    for (const QJsonValue& path : {QJsonValue(), QJsonValue(5)}) {
        QJsonObject dependency{{"version", "1.0.0"}};
        if (!path.isUndefined() && !path.isNull()) dependency.insert("path", path);
        PUT(file, manifest({{"dependencies", QJsonObject{{"lib", dependency}}}}));
        EXPECT_TRUE(ScriptPackage::read(file, p).contains("dependency lib needs a package id as its name, a local path"));
    }
    // An asset that is not a path.
    PUT(file, manifest({{"assets", QJsonObject{{"theme", 5}}}}));
    EXPECT_EQ(ScriptPackage::read(file, p), "package.json: asset theme must be a path in the package");
    // 16 packages in one tree, and no more: the package and a chain of
    // dependencies, each depending on the next.
    auto chain = [&](int dependencies) {
        const QString top = QStringLiteral("chain%1").arg(dependencies);
        for (int i = 0; i < dependencies; ++i) {
            const QString next = i + 1 < dependencies ? QStringLiteral(R"("dependencies":{"d":{"path":"d","version":"1.0.0"}},)") : QString();
            PUT(dir.filePath(top + QString("/d").repeated(i) + "/package.json"),
                QStringLiteral(R"({"id":"d","version":"1.0.0","api_version":1,"entry":"main",%1"modules":{"main":"m.luau"}})").arg(next).toUtf8());
            PUT(dir.filePath(top + QString("/d").repeated(i) + "/m.luau"), "return {}");
        }
        PUT(file, manifest({{"dependencies", QJsonObject{{"d", QJsonObject{{"path", top}, {"version", "1.0.0"}}}}}}));
    };
    ASSERT_NO_FATAL_FAILURE(chain(15));
    EXPECT_TRUE(ScriptPackage::read(file, p).isEmpty());
    ASSERT_NO_FATAL_FAILURE(chain(16));
    EXPECT_EQ(ScriptPackage::read(file, p), "A package tree has at most 16 packages");
    // Assets: a count and a size limit.
    QJsonObject assets;
    for (int i = 0; i < 33; ++i) assets.insert(QStringLiteral("a%1").arg(i), "init.luau");
    PUT(file, manifest({{"assets", assets}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("at most 32"));
    PUT(dir.filePath("big.bin"), QByteArray(4 * 1024 * 1024 + 1, 'x'));
    PUT(file, manifest({{"assets", QJsonObject{{"big", "big.bin"}}}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("asset"));
    // Shader packs: at least one, at most 16.
    QDir().mkpath(dir.filePath("empty"));
    PUT(file, manifest({{"shaders", QJsonObject{{"path", "empty"}}}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("1 to 16 packs"));
    for (int i = 0; i < 17; ++i)
        PUT(dir.filePath(QStringLiteral("many/p%1/metadata.json").arg(i)), QStringLiteral(R"({"id":"test.p%1"})").arg(i).toUtf8());
    PUT(file, manifest({{"shaders", QJsonObject{{"path", "many"}}}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("1 to 16 packs"));
}

TEST(ScriptPackage, ModuleErrorsNameTheirFileAndLine)
{
    QTemporaryDir dir;
    const auto file = dir.filePath("package.json");
    PUT(dir.filePath("init.luau"), "local util = require('util')\nreturn {}");
    PUT(dir.filePath("util.luau"), "local x = 1\nlocal y = 2\nerror('broken')\nreturn {}");
    PUT(file, manifest({{"modules", QJsonObject{{"main", "init.luau"}, {"util", "util.luau"}}}}));
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

TEST(ScriptPackage, EachManifestRuleHasItsOwnError)
{
    QTemporaryDir dir;
    const auto file = dir.filePath("package.json");
    PUT(dir.filePath("init.luau"), "return {}");
    ScriptPackage p;
    const std::pair<QJsonObject, const char*> bad[] = {
        {{{"version", "1.0"}}, "package.json: version must be X.Y.Z"},
        {{{"version", "01.0.0"}}, "package.json: version must be X.Y.Z"},
        {{{"modules", QJsonArray{"init.luau"}}}, "package.json: modules must map module names to files"},
        {{{"entry", "no entry"}}, "package.json: entry must name a module"},
        {{{"entry", "other"}}, "package.json: the entry module other is not in modules"},
        {{{"dependencies", QJsonArray{}}}, "package.json: dependencies must map package ids"},
        {{{"modules", QJsonObject{{"main", "init.luau"}, {"bad name", "init.luau"}}}}, "package.json: module bad name needs a name"},
        {{{"modules", QJsonObject{{"main", "init.luau"}, {"other", 5}}}}, "package.json: module other needs a name"},
    };
    for (const auto& [fields, error] : bad) {
        PUT(file, manifest(fields));
        const QString message = ScriptPackage::read(file, p);
        EXPECT_TRUE(message.startsWith(QLatin1String(error))) << message.toStdString();
    }
    // A name that was never checked is cut in the message.
    PUT(file, manifest({{"assets", QJsonObject{{QString(300, 'a'), 5}}}}));
    EXPECT_EQ(ScriptPackage::read(file, p), QStringLiteral("package.json: asset %1… must be a path in the package").arg(QString(64, 'a')));
    // 64 modules in one tree, and no more.
    QJsonObject modules{{"main", "init.luau"}};
    for (int i = 1; i < 64; ++i) modules.insert(QStringLiteral("m%1").arg(i), "init.luau");
    PUT(file, manifest({{"modules", modules}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).isEmpty());
    modules.insert("extra", "init.luau");
    PUT(file, manifest({{"modules", modules}}));
    EXPECT_EQ(ScriptPackage::read(file, p), "A package tree has at most 64 modules");
    // A package directory that does not resolve has no root to hold paths in.
    EXPECT_TRUE(ScriptPackage::read(dir.filePath("missing/package.json"), p).startsWith("Cannot resolve the package directory"));
}

TEST(ScriptPackage, ASymlinkBackIntoTheTreeIsACycle)
{
    QTemporaryDir dir;
    const auto file = dir.filePath("package.json");
    PUT(dir.filePath("init.luau"), "return {}");
    PUT(dir.filePath("a/package.json"),
        R"({"id":"a","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"m.luau"},"dependencies":{"b":{"path":"b","version":"1.0.0"}}})");
    PUT(dir.filePath("a/m.luau"), "return {}");
    // a/b is a itself: by its path a new package, by where it leads the one being read.
    ASSERT_TRUE(QFile::link(dir.filePath("a"), dir.filePath("a/b")));
    PUT(file, manifest({{"dependencies", QJsonObject{{"a", QJsonObject{{"path", "a"}, {"version", "1.0.0"}}}}}}));
    ScriptPackage p;
    EXPECT_EQ(ScriptPackage::read(file, p), "Dependency cycle at a/b/package.json");
}

TEST(ScriptPackage, ADependencyErrorIsNotReplacedByALaterOne)
{
    QTemporaryDir dir;
    const auto file = dir.filePath("package.json");
    PUT(dir.filePath("init.luau"), "return {}");
    PUT(dir.filePath("vendor/lib/package.json"), R"({"id":"lib","version":"1.0.0","api_version":2,"entry":"main","modules":{"main":"lib.luau"}})");
    // The root's own modules break a rule too: its error would come later.
    PUT(file, manifest({{"modules", QJsonObject{{"main", "init.luau"}, {"bad name", "init.luau"}}},
                        {"dependencies", QJsonObject{{"lib", QJsonObject{{"path", "vendor/lib"}, {"version", "1.0.0"}}}}}}));
    ScriptPackage p;
    EXPECT_EQ(ScriptPackage::read(file, p), "vendor/lib/package.json: api_version must be 1");
    // So does an invalid config.json: its error stands.
    PUT(file, manifest({{"modules", QJsonObject{{"main", "init.luau"}, {"bad name", "init.luau"}}}}));
    PUT(dir.filePath("config.json"), "[1]");
    EXPECT_TRUE(ScriptPackage::read(file, p).startsWith("Invalid package JSON")) << ScriptPackage::read(file, p).toStdString();
}

TEST(ScriptPackage, AnEscapingPathIsWatchedSoItsFixLoads)
{
    QTemporaryDir dir, outside;
    const auto file = dir.filePath("package.json");
    PUT(outside.filePath("module.luau"), "return {}");
    ASSERT_TRUE(QFile::link(outside.filePath("module.luau"), dir.filePath("init.luau")));
    PUT(file, manifest());
    ScriptPackage p;
    EXPECT_TRUE(ScriptPackage::read(file, p).contains("escapes its root"));
    EXPECT_TRUE(p.watched.contains(dir.filePath("init.luau")));
}

TEST(ScriptPackage, CommandTextRefusesSeparatorsAndBidirectionalControls)
{
    QTemporaryDir dir;
    const auto file = dir.filePath("package.json");
    PUT(dir.filePath("init.luau"), "return {}");
    ScriptPackage p;
    for (const char16_t c : {u'\u2028', u'\u2029', u'\u061c', u'\u200e', u'\u200f', u'\u202a', u'\u202e', u'\u2066', u'\u2069', u'\u0085'}) {
        const QString summary = QStringLiteral("a") + QChar(c) + QStringLiteral("b");
        PUT(file, manifest({{"commands", QJsonObject{{"go", QJsonObject{{"summary", summary}}}}}}));
        EXPECT_TRUE(ScriptPackage::read(file, p).contains("bidirectional controls")) << int(c);
    }
    // Other formatting characters stay: a zero-width joiner is part of an emoji.
    PUT(file, manifest({{"commands", QJsonObject{{"go", QJsonObject{{"summary", QStringLiteral("a‍b")}}}}}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).isEmpty());
}

TEST(ScriptPackage, APackDirectoryNameIsCheckedFirst)
{
    QTemporaryDir dir;
    const auto file = dir.filePath("package.json");
    PUT(dir.filePath("init.luau"), "return {}");
    // No metadata.json: only the name can refuse it.
    QDir().mkpath(dir.filePath("shaders/bad name"));
    PUT(file, manifest({{"shaders", QJsonObject{{"path", "shaders"}}}}));
    ScriptPackage p;
    EXPECT_EQ(ScriptPackage::read(file, p), "package.json: shader pack directory bad name must be 1 to 64 letters, digits, _ and -");
}

TEST(ScriptPackage, ErrorsAtTheEndOfAModuleNameTheModule)
{
    QTemporaryDir dir;
    const auto file = dir.filePath("package.json");
    PUT(dir.filePath("init.luau"), "return {}");
    PUT(dir.filePath("util.luau"), "local x = 1\nif x then end\nreturn {}");
    PUT(file, manifest({{"modules", QJsonObject{{"main", "init.luau"}, {"util", "util.luau"}}}}));
    ScriptPackage p;
    ASSERT_TRUE(ScriptPackage::read(file, p).isEmpty());
    const auto util = std::find_if(p.modules.cbegin(), p.modules.cend(), [](const auto& m) { return m.file == "util.luau"; });
    ASSERT_NE(util, p.modules.cend());
    // Its byte range in the chunk is its source, which ScriptHost parses alone.
    EXPECT_EQ(p.source.mid(util->offset, util->size), "local x = 1\nif x then end\nreturn {}");
    // The line that closes its factory counts as its last line.
    EXPECT_EQ(ScriptPackage::locate(p.modules, QStringLiteral("package.json:%1: closed").arg(util->line + util->lines)),
              "util.luau:3: closed");
    // Only the chunk's positions change. Lines named in the text, as a
    // script's own error can have them, stay as they are.
    const QString message = QStringLiteral("package.json:%1: could not close 'door' at line 9").arg(util->line);
    EXPECT_EQ(ScriptPackage::locate(p.modules, message), "util.luau:1: could not close 'door' at line 9");
    // The loader's own lines keep their positions.
    EXPECT_EQ(ScriptPackage::locate(p.modules, "package.json:1: (to close 'function' at line 1)"),
              QStringLiteral("package.json:1: (to close 'function' at line 1)"));
}
