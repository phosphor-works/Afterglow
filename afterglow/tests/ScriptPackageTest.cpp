// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later
#include "scripting/ScriptPackage.h"
#include "scripting/ScriptState.h"
#include "scripting/ScriptFiles.h"
#include "QStringPrinter.h" // IWYU pragma: keep (gtest finds PrintTo(QString) by ADL)
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <algorithm>
#include <limits>
using namespace Afterglow;
namespace {
void put(const QString& path, const QByteArray& bytes)
{
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath())) << path.toStdString();
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly)) << path.toStdString();
    // A short write would leave a cut manifest or module, refused for another reason than the one under test.
    ASSERT_EQ(f.write(bytes), bytes.size()) << path.toStdString();
    ASSERT_TRUE(f.flush()) << path.toStdString();
}
QByteArray manifest(const QJsonObject& extra = {})
{
    QJsonObject doc{{QStringLiteral("id"), QStringLiteral("test")},
                    {QStringLiteral("version"), QStringLiteral("1.0.0")},
                    {QStringLiteral("api_version"), 1},
                    {QStringLiteral("entry"), QStringLiteral("main")},
                    {QStringLiteral("modules"), QJsonObject{{QStringLiteral("main"), QStringLiteral("init.luau")}}}};
    for (auto it = extra.begin(); it != extra.end(); ++it)
        doc[it.key()] = it.value();
    return QJsonDocument(doc).toJson();
}
// A failed write stops the test, not only the helper.
#define PUT(...) ASSERT_NO_FATAL_FAILURE(put(__VA_ARGS__))
}
TEST(ScriptPackage, BoundsSourcesAndRejectsEscapesAndIncompatibleVersions)
{
    QTemporaryDir parent;
    ASSERT_TRUE(parent.isValid());
    const QString root = parent.filePath(QStringLiteral("package"));
    QDir dir(root);
    const auto file = dir.filePath(QStringLiteral("package.json"));
    PUT(dir.filePath(QStringLiteral("init.luau")), "return {}");
    PUT(file, manifest());
    ScriptPackage p;
    EXPECT_TRUE(ScriptPackage::read(file, p).isEmpty());
    // The targets exist, so only the containment rules can refuse them.
    PUT(parent.filePath(QStringLiteral("outside.luau")), "return {}");
    for (const auto& path : {QStringLiteral("../outside.luau"), parent.filePath(QStringLiteral("outside.luau"))}) {
        PUT(file, manifest({{QStringLiteral("modules"), QJsonObject{{QStringLiteral("main"), path}}}}));
        EXPECT_TRUE(ScriptPackage::read(file, p).contains(QStringLiteral("must remain inside"))) << path.toStdString();
    }
    PUT(file, manifest({{QStringLiteral("api_version"), 2}}));
    EXPECT_EQ(ScriptPackage::read(file, p), QStringLiteral("package.json: api_version must be 1"));
    PUT(file, manifest());
    PUT(dir.filePath(QStringLiteral("init.luau")), QByteArray(256 * 1024 + 1, ' '));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains(QStringLiteral("byte limit")));
    PUT(dir.filePath(QStringLiteral("init.luau")), "return {}");
    // An id that `aglow run` would read as an option.
    PUT(file, manifest({{QStringLiteral("id"), QStringLiteral("-test")}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).startsWith(QStringLiteral("package.json: id must be 1 to 64")));
    QTemporaryDir outside;
    ASSERT_TRUE(outside.isValid());
    PUT(outside.filePath(QStringLiteral("module.luau")), "return {}");
    ASSERT_TRUE(
        QFile::link(outside.filePath(QStringLiteral("module.luau")), dir.filePath(QStringLiteral("link.luau"))));
    PUT(file,
        manifest({{QStringLiteral("modules"), QJsonObject{{QStringLiteral("main"), QStringLiteral("link.luau")}}}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains(QStringLiteral("escapes its root")));
}
TEST(ScriptPackage, ReadsAndChecksItsCommands)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto file = dir.filePath(QStringLiteral("package.json"));
    PUT(dir.filePath(QStringLiteral("init.luau")), "return {}");
    PUT(file,
        manifest({{QStringLiteral("commands"),
                   QJsonObject{{QStringLiteral("open"),
                                QJsonObject{{QStringLiteral("summary"), QStringLiteral("Open a project")},
                                            {QStringLiteral("usage"), QStringLiteral("DIR [LAYOUT]")},
                                            {QStringLiteral("palette"), QStringLiteral("Open project…")}}},
                               {QStringLiteral("import"),
                                QJsonObject{{QStringLiteral("summary"), QStringLiteral("Add projects from stdin")},
                                            {QStringLiteral("stdin"), true}}}}}}));
    ScriptPackage p;
    ASSERT_TRUE(ScriptPackage::read(file, p).isEmpty());
    ASSERT_EQ(p.commands.size(), 2);
    EXPECT_EQ(p.commands[0].name, QStringLiteral("import")); // by name
    EXPECT_TRUE(p.commands[0].input);
    EXPECT_TRUE(p.commands[0].palette.isEmpty());
    EXPECT_EQ(p.commands[1].usage, QStringLiteral("DIR [LAYOUT]"));
    EXPECT_EQ(p.commands[1].palette, QStringLiteral("Open project…"));
    EXPECT_FALSE(p.commands[1].input);
    const std::pair<QJsonValue, const char*> bad[] = {
        {QJsonArray{QStringLiteral("open")}, "commands must map command names"},
        {QJsonObject{{QStringLiteral("open"), QStringLiteral("Open")}}, "must be a table of fields"},
        {QJsonObject{{QStringLiteral("open"), QJsonObject{}}}, "needs a summary"},
        {QJsonObject{{QStringLiteral("open"), QJsonObject{{QStringLiteral("summary"), 3}}}}, "must be text"},
        {QJsonObject{{QStringLiteral("open"),
                      QJsonObject{{QStringLiteral("summary"), QStringLiteral("x")},
                                  {QStringLiteral("stdin"), QStringLiteral("yes")}}}},
         "stdin must be a boolean"},
        {QJsonObject{{QStringLiteral("open"),
                      QJsonObject{{QStringLiteral("summary"), QStringLiteral("x")},
                                  {QStringLiteral("title"), QStringLiteral("x")}}}},
         "unknown field title"},
        {QJsonObject{{QStringLiteral("op en"), QJsonObject{{QStringLiteral("summary"), QStringLiteral("x")}}}},
         "must be 1 to 64"},
        {QJsonObject{{QStringLiteral("--help"), QJsonObject{{QStringLiteral("summary"), QStringLiteral("x")}}}},
         "must not start with -"},
        {QJsonObject{{QStringLiteral("open"), QJsonObject{{QStringLiteral("summary"), QStringLiteral(" ")}}}},
         "needs a summary"},
        {QJsonObject{{QStringLiteral("open"),
                      QJsonObject{{QStringLiteral("summary"), QStringLiteral("x")},
                                  {QStringLiteral("palette"), QStringLiteral("  ")}}}},
         "palette must not be blank"},
        {QJsonObject{{QStringLiteral("open"), QJsonObject{{QStringLiteral("summary"), QStringLiteral("a\tb")}}}},
         "no control characters"},
        {QJsonObject{{QStringLiteral("open"),
                      QJsonObject{{QStringLiteral("summary"), QStringLiteral("x")}, {QStringLiteral("usage"), 5}}}},
         "usage must be text"},
        {QJsonObject{
             {QStringLiteral("open"), QJsonObject{{QStringLiteral("summary"), QString(513, QLatin1Char('x'))}}}},
         "at most 512"},
    };
    for (const auto& [commands, error] : bad) {
        PUT(file, manifest({{QStringLiteral("commands"), commands}}));
        const QString message = ScriptPackage::read(file, p);
        EXPECT_TRUE(message.contains(QLatin1String(error))) << message.toStdString();
    }
    QJsonObject many;
    for (int i = 0; i <= MaxScriptCommands; ++i)
        many.insert(QStringLiteral("c%1").arg(i), QJsonObject{{QStringLiteral("summary"), QStringLiteral("x")}});
    PUT(file, manifest({{QStringLiteral("commands"), many}}));
    EXPECT_EQ(ScriptPackage::read(file, p),
              QStringLiteral("package.json: A script has at most %1 commands").arg(MaxScriptCommands));
}
TEST(ScriptFiles, Utf8PrefixKeepsACharacterThatEndsAtTheLimit)
{
    for (const QByteArray& character :
         {QByteArray("\xC3\xA9"), QByteArray("\xE6\xBC\xA2"), QByteArray("\xF0\x9F\x98\x80")}) {
        const QByteArray text = "ab" + character + "cd";
        const qsizetype end = 2 + character.size();
        // Ends exactly at the limit: it stays.
        EXPECT_EQ(utf8Prefix(text, end), "ab" + character) << character.size();
        // Cut through: it goes whole.
        for (qsizetype limit = 3; limit < end; ++limit)
            EXPECT_EQ(utf8Prefix(text, limit), "ab") << character.size() << limit;
        EXPECT_EQ(utf8Prefix(text, end + 1), "ab" + character + "c");
    }
    EXPECT_EQ(utf8Prefix("abc", 10), "abc");
    EXPECT_EQ(utf8Prefix("abc", 0), "");
    EXPECT_EQ(utf8Prefix("abc", -1), "");
    // A stray continuation byte after a whole character ends the cut: no character runs through it, so it stays.
    EXPECT_EQ(utf8Prefix("a\xF0\x9F\x98\x80\x80"
                         "b",
                         6),
              QByteArray("a\xF0\x9F\x98\x80\x80"));
}
TEST(ScriptState, AtomicRevisionChecksAndRoundTrip)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ScriptState store(dir.path());
    ScriptState::Snapshot s;
    ASSERT_TRUE(store.read(QStringLiteral("navigator"), s).isEmpty());
    EXPECT_EQ(s.revision, 0);
    const QVariantMap value{{QStringLiteral("favorites"), QVariantList{QStringLiteral("/tmp/project")}},
                            {QStringLiteral("count"), 2}};
    ASSERT_TRUE(store.write(QStringLiteral("navigator"), 1, 0, value, s).isEmpty());
    EXPECT_EQ(s.revision, 1);
    ScriptState::Snapshot read;
    ASSERT_TRUE(store.read(QStringLiteral("navigator"), read).isEmpty());
    EXPECT_EQ(read.value, value);
    EXPECT_FALSE(store.write(QStringLiteral("navigator"), 1, 0, {}, s).isEmpty());
    ASSERT_TRUE(store.write(QStringLiteral("navigator"), 2, 1, {{QStringLiteral("migrated"), true}}, s).isEmpty());
    EXPECT_FALSE(store.write(QStringLiteral("navigator"), 1, 2, {}, s).isEmpty());
    EXPECT_FALSE(store.write(QStringLiteral("../escape"), 1, 0, {}, s).isEmpty());
}
TEST(ScriptState, RejectsCorruptionSymlinksAndUnserializableValues)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ScriptState store(dir.path());
    ScriptState::Snapshot s;
    PUT(dir.filePath(QStringLiteral("bad.json")), "{oops");
    EXPECT_FALSE(store.read(QStringLiteral("bad"), s).isEmpty());
    EXPECT_FALSE(store.write(QStringLiteral("bad"), 1, 0, {}, s).isEmpty());
    // A link to a valid state: only the symlink rule can refuse it.
    ASSERT_TRUE(store.write(QStringLiteral("good"), 1, 0, {{QStringLiteral("x"), 1}}, s).isEmpty());
    ASSERT_TRUE(QFile::link(dir.filePath(QStringLiteral("good.json")), dir.filePath(QStringLiteral("link.json"))));
    EXPECT_TRUE(store.read(QStringLiteral("link"), s).contains(QStringLiteral("symlinks")));
    EXPECT_FALSE(store.write(QStringLiteral("link"), 1, 1, {{QStringLiteral("x"), 2}}, s).isEmpty());
    ASSERT_TRUE(store.read(QStringLiteral("good"), s).isEmpty());
    EXPECT_EQ(s.value.value(QStringLiteral("x")).toInt(), 1);
    EXPECT_FALSE(ScriptState::validate({{QStringLiteral("nan"), std::numeric_limits<double>::quiet_NaN()}}).isEmpty());
    EXPECT_FALSE(ScriptState::validate({{QStringLiteral("large"), QString(300000, QLatin1Char('x'))}}).isEmpty());
    EXPECT_FALSE(ScriptState::validate({{QStringLiteral("huge"), 1e300}}).isEmpty());
    EXPECT_FALSE(ScriptState::validate({{QString(257, QLatin1Char('k')), 1}}).isEmpty());
    QVariantMap nested;
    for (int i = 0; i < 20; ++i)
        nested = {{QStringLiteral("child"), nested}};
    EXPECT_FALSE(ScriptState::validate(nested).isEmpty());
}

TEST(ScriptState, LimitsSizeCountAndDepth)
{
    // Every string is under its own limit, so only the encoded size refuses this.
    QVariantMap large;
    for (int i = 0; i < 5; ++i)
        large.insert(QStringLiteral("k%1").arg(i), QString(60000, QLatin1Char('x')));
    EXPECT_TRUE(ScriptState::validate(large).contains(QStringLiteral("255 KiB")));
    // 8192 values, the top-level object and the list included, and no more.
    QVariantList many;
    for (int i = 0; i < 8191; ++i)
        many.append(i);
    EXPECT_FALSE(ScriptState::validate({{QStringLiteral("many"), many}}).isEmpty());
    many.removeLast();
    EXPECT_TRUE(ScriptState::validate({{QStringLiteral("many"), many}}).isEmpty());
    // Sixteen tables, the top-level object included, and no more.
    QVariantMap deep{{QStringLiteral("leaf"), 1}};
    for (int i = 0; i < 15; ++i)
        deep = {{QStringLiteral("child"), deep}};
    EXPECT_TRUE(ScriptState::validate(deep).isEmpty());
    EXPECT_FALSE(ScriptState::validate({{QStringLiteral("child"), deep}}).isEmpty());
}

TEST(ScriptPackage, ConfigOverridesAndDeclaredAssetsStayLocal)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    PUT(dir.filePath(QStringLiteral("init.luau")), "return {}");
    PUT(dir.filePath(QStringLiteral("theme.json")), "{}");
    PUT(dir.filePath(QStringLiteral("package.json")),
        manifest({{QStringLiteral("config"), QJsonObject{{QStringLiteral("label"), QStringLiteral("default")}}},
                  {QStringLiteral("assets"), QJsonObject{{QStringLiteral("theme"), QStringLiteral("theme.json")}}}}));
    PUT(dir.filePath(QStringLiteral("config.json")), "{\"label\":\"user\"}");
    ScriptPackage p;
    ASSERT_TRUE(ScriptPackage::read(dir.filePath(QStringLiteral("package.json")), p).isEmpty());
    EXPECT_EQ(p.config.value(QStringLiteral("label")), QStringLiteral("user"));
    EXPECT_EQ(p.assets.value(QStringLiteral("theme")).toString(), dir.filePath(QStringLiteral("theme.json")));
    // A real file outside the package, so only the containment rule refuses it.
    QTemporaryDir outer;
    ASSERT_TRUE(outer.isValid());
    const QString nested = outer.filePath(QStringLiteral("package"));
    PUT(outer.filePath(QStringLiteral("theme.json")), "{}");
    PUT(nested + QStringLiteral("/init.luau"), "return {}");
    PUT(nested + QStringLiteral("/package.json"),
        manifest(
            {{QStringLiteral("assets"), QJsonObject{{QStringLiteral("escape"), QStringLiteral("../theme.json")}}}}));
    EXPECT_TRUE(ScriptPackage::read(nested + QStringLiteral("/package.json"), p)
                    .contains(QStringLiteral("must remain inside")));
}

TEST(ScriptFiles, RejectsSpecialFilesAndChecksOpenedContainment)
{
    QTemporaryDir inside, outside;
    ASSERT_TRUE(inside.isValid() && outside.isValid());
    const auto fifo = inside.filePath(QStringLiteral("fifo"));
    ASSERT_EQ(::mkfifo(QFile::encodeName(fifo).constData(), 0600), 0);
    QByteArray bytes("unchanged");
    EXPECT_FALSE(readScriptFile(fifo, 1024, bytes).isEmpty());
    EXPECT_EQ(bytes, "unchanged");
    PUT(outside.filePath(QStringLiteral("source")), "return {}");
    ASSERT_TRUE(QFile::link(outside.filePath(QStringLiteral("source")), inside.filePath(QStringLiteral("link"))));
    EXPECT_FALSE(readScriptFile(inside.filePath(QStringLiteral("link")), 1024, bytes, inside.path()).isEmpty());
    EXPECT_FALSE(readScriptFile(inside.filePath(QStringLiteral("link")), 1024, bytes, {}, true).isEmpty());
    EXPECT_TRUE(readScriptFile(inside.filePath(QStringLiteral("link")), 1024, bytes).isEmpty());
    EXPECT_EQ(bytes, "return {}");
    EXPECT_FALSE(readScriptFile(outside.filePath(QStringLiteral("source")), 4, bytes).isEmpty());
}

TEST(ScriptPackage, ShaderPacksCarryThePackageNameAndSuggestionsNameThem)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto file = dir.filePath(QStringLiteral("package.json"));
    PUT(dir.filePath(QStringLiteral("init.luau")), "return {}");
    PUT(dir.filePath(QStringLiteral("shaders/wall/metadata.json")),
        R"({"id":"test.wall","name":"Wall","fragmentShader":"effect.frag"})");
    PUT(dir.filePath(QStringLiteral("shaders/wall/effect.frag")),
        "vec4 pTerminal(vec2 uv) { return terminalTexel(uv); }");
    const QJsonObject shaders{
        {QStringLiteral("path"), QStringLiteral("shaders")},
        {QStringLiteral("suggests"), QJsonObject{{QStringLiteral("overview"), QStringLiteral("wall")}}}};
    PUT(file,
        manifest({{QStringLiteral("shaders"), shaders},
                  {QStringLiteral("pulses"), QJsonObject{{QStringLiteral("ripple"), 4}}}}));
    ScriptPackage p;
    ASSERT_TRUE(ScriptPackage::read(file, p).isEmpty());
    EXPECT_EQ(QFileInfo(p.shaderDirectory).fileName(), QStringLiteral("shaders"));
    EXPECT_EQ(p.shaderPacks, QStringList{QStringLiteral("wall")});
    EXPECT_EQ(p.suggests.value(QStringLiteral("overview")), QStringLiteral("wall"));
    EXPECT_EQ(p.pulses.value(QStringLiteral("ripple")), 4);
    // Suggestions name the package's own packs and real Shaders keys.
    for (const auto& bad : {QJsonObject{{QStringLiteral("overview"), QStringLiteral("missing")}},
                            QJsonObject{{QStringLiteral("palette"), QStringLiteral("wall")}}}) {
        PUT(file,
            manifest({{QStringLiteral("shaders"),
                       QJsonObject{{QStringLiteral("path"), QStringLiteral("shaders")},
                                   {QStringLiteral("suggests"), bad}}}}));
        EXPECT_TRUE(ScriptPackage::read(file, p).contains(QStringLiteral("suggests maps a Shaders key")));
    }
    // Pulse names map to the numbers packs own: 4 to 255.
    for (const auto number : {3, 256}) {
        PUT(file, manifest({{QStringLiteral("pulses"), QJsonObject{{QStringLiteral("ripple"), number}}}}));
        EXPECT_TRUE(ScriptPackage::read(file, p).contains(QStringLiteral("from 4 to 255"))) << number;
    }
    // A pack must declare "<package>.<pack>", so it cannot replace a bundled one.
    PUT(dir.filePath(QStringLiteral("shaders/wall/metadata.json")),
        R"({"id":"phosphor-overview","name":"Wall","fragmentShader":"effect.frag"})");
    PUT(file, manifest({{QStringLiteral("shaders"), shaders}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains(QStringLiteral("test.wall")));
    PUT(dir.filePath(QStringLiteral("shaders/wall/metadata.json")),
        R"({"id":"test.wall","name":"Wall","fragmentShader":"effect.frag"})");
    // A real pack directory outside the package, so only the containment rule refuses it.
    QTemporaryDir parent;
    ASSERT_TRUE(parent.isValid());
    const QString root = parent.filePath(QStringLiteral("package"));
    PUT(root + QStringLiteral("/init.luau"), "return {}");
    PUT(parent.filePath(QStringLiteral("elsewhere/wall/metadata.json")),
        R"({"id":"test.wall","name":"Wall","fragmentShader":"effect.frag"})");
    PUT(root + QStringLiteral("/package.json"),
        manifest({{QStringLiteral("shaders"), QJsonObject{{QStringLiteral("path"), QStringLiteral("../elsewhere")}}}}));
    EXPECT_TRUE(
        ScriptPackage::read(root + QStringLiteral("/package.json"), p).contains(QStringLiteral("must remain inside")));
    // A symlinked pack that points out of the package is checked like a directory.
    QTemporaryDir outside;
    ASSERT_TRUE(outside.isValid());
    PUT(outside.filePath(QStringLiteral("crt/metadata.json")),
        R"({"id":"crt","name":"CRT","fragmentShader":"effect.frag"})");
    ASSERT_TRUE(QFile::link(outside.filePath(QStringLiteral("crt")), dir.filePath(QStringLiteral("shaders/crt"))));
    PUT(file, manifest({{QStringLiteral("shaders"), shaders}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains(QStringLiteral("escapes its root")));
    ASSERT_TRUE(QFile::remove(dir.filePath(QStringLiteral("shaders/crt"))));
    // So is the packs' include directory, although it is not a pack.
    PUT(outside.filePath(QStringLiteral("include/common.glsl")), "");
    ASSERT_TRUE(
        QFile::link(outside.filePath(QStringLiteral("include")), dir.filePath(QStringLiteral("shaders/shared"))));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains(QStringLiteral("escapes its root")));
    ASSERT_TRUE(QFile::remove(dir.filePath(QStringLiteral("shaders/shared"))));
    // A non-string path is an error, not the default directory.
    PUT(file, manifest({{QStringLiteral("shaders"), QJsonObject{{QStringLiteral("path"), 5}}}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains(QStringLiteral("shaders.path")));
    // Built-in pulse names and plain numbers already select effects.
    for (const auto& name :
         {QStringLiteral("flash"), QStringLiteral("portal"), QStringLiteral("shake"), QStringLiteral("7")}) {
        PUT(file, manifest({{QStringLiteral("pulses"), QJsonObject{{name, 9}}}}));
        EXPECT_TRUE(ScriptPackage::read(file, p).contains(QStringLiteral("reserved"))) << name.toStdString();
    }
    // "shared" holds includes and is skipped; "none" is reserved.
    PUT(dir.filePath(QStringLiteral("shaders/shared/common.glsl")), "");
    PUT(file, manifest({{QStringLiteral("shaders"), shaders}}));
    ASSERT_TRUE(ScriptPackage::read(file, p).isEmpty());
    EXPECT_EQ(p.shaderPacks, QStringList{QStringLiteral("wall")});
    PUT(dir.filePath(QStringLiteral("shaders/none/metadata.json")), R"({"id":"test.none"})");
    EXPECT_TRUE(ScriptPackage::read(file, p).contains(QStringLiteral("reserved")));
}

TEST(ScriptPackage, RejectsBadManifestsWithTheRealError)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto file = dir.filePath(QStringLiteral("package.json"));
    PUT(dir.filePath(QStringLiteral("init.luau")), "return {}");
    ScriptPackage p;
    PUT(file, "{");
    EXPECT_TRUE(ScriptPackage::read(file, p).contains(QStringLiteral("Invalid package JSON")));
    PUT(file, QByteArray(64 * 1024 + 1, ' '));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains(QStringLiteral("byte limit")));
    for (const auto& version : {QJsonValue(0), QJsonValue(10001), QJsonValue(1.5), QJsonValue(QStringLiteral("2"))}) {
        PUT(file, manifest({{QStringLiteral("state_version"), version}}));
        EXPECT_TRUE(ScriptPackage::read(file, p).contains(QStringLiteral("state_version")));
    }
    PUT(file, manifest({{QStringLiteral("config"), 3}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains(QStringLiteral("config must be an object")));
    PUT(file, manifest());
    PUT(dir.filePath(QStringLiteral("config.json")), "[1]");
    EXPECT_TRUE(ScriptPackage::read(file, p).contains(QStringLiteral("Invalid package JSON")));
    ASSERT_TRUE(QFile::remove(dir.filePath(QStringLiteral("config.json"))));
    // A dependency with another version than declared.
    PUT(dir.filePath(QStringLiteral("vendor/lib/package.json")),
        R"({"id":"lib","version":"2.0.0","api_version":1,"entry":"main","modules":{"main":"lib.luau"}})");
    PUT(dir.filePath(QStringLiteral("vendor/lib/lib.luau")), "return {}");
    PUT(file,
        manifest({{QStringLiteral("dependencies"),
                   QJsonObject{{QStringLiteral("lib"),
                                QJsonObject{{QStringLiteral("path"), QStringLiteral("vendor/lib")},
                                            {QStringLiteral("version"), QStringLiteral("1.0.0")}}}}}}));
    EXPECT_EQ(
        ScriptPackage::read(file, p),
        QStringLiteral("vendor/lib/package.json: version is 2.0.0, but the package that depends on it declares 1.0.0"));
    // require("@lib") names the package with the id lib.
    PUT(dir.filePath(QStringLiteral("vendor/lib/package.json")),
        R"({"id":"other","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"lib.luau"}})");
    EXPECT_EQ(ScriptPackage::read(file, p),
              QStringLiteral("vendor/lib/package.json: id is other, but the package that depends on it names it lib"));
    PUT(dir.filePath(QStringLiteral("vendor/lib/package.json")),
        R"({"id":"lib","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"lib.luau"}})");
    EXPECT_TRUE(ScriptPackage::read(file, p).isEmpty());
    // A dependency with no path, or a path that is not text.
    for (const QJsonValue& path : {QJsonValue(), QJsonValue(5)}) {
        QJsonObject dependency{{QStringLiteral("version"), QStringLiteral("1.0.0")}};
        if (!path.isUndefined() && !path.isNull())
            dependency.insert(QStringLiteral("path"), path);
        PUT(file, manifest({{QStringLiteral("dependencies"), QJsonObject{{QStringLiteral("lib"), dependency}}}}));
        EXPECT_TRUE(ScriptPackage::read(file, p).contains(
            QStringLiteral("dependency lib needs a package id as its name, a local path")));
    }
    // An asset that is not a path.
    PUT(file, manifest({{QStringLiteral("assets"), QJsonObject{{QStringLiteral("theme"), 5}}}}));
    EXPECT_EQ(ScriptPackage::read(file, p), QStringLiteral("package.json: asset theme must be a path in the package"));
    // 16 packages in one tree, and no more: the package and a chain of
    // dependencies, each depending on the next.
    auto chain = [&](int dependencies) {
        const QString top = QStringLiteral("chain%1").arg(dependencies);
        for (int i = 0; i < dependencies; ++i) {
            const QString next = i + 1 < dependencies
                ? QStringLiteral(R"("dependencies":{"d":{"path":"d","version":"1.0.0"}},)")
                : QString();
            PUT(dir.filePath(top + QStringLiteral("/d").repeated(i) + QStringLiteral("/package.json")),
                QStringLiteral(
                    R"({"id":"d","version":"1.0.0","api_version":1,"entry":"main",%1"modules":{"main":"m.luau"}})")
                    .arg(next)
                    .toUtf8());
            PUT(dir.filePath(top + QStringLiteral("/d").repeated(i) + QStringLiteral("/m.luau")), "return {}");
        }
        PUT(file,
            manifest({{QStringLiteral("dependencies"),
                       QJsonObject{{QStringLiteral("d"),
                                    QJsonObject{{QStringLiteral("path"), top},
                                                {QStringLiteral("version"), QStringLiteral("1.0.0")}}}}}}));
    };
    ASSERT_NO_FATAL_FAILURE(chain(15));
    EXPECT_TRUE(ScriptPackage::read(file, p).isEmpty());
    ASSERT_NO_FATAL_FAILURE(chain(16));
    EXPECT_EQ(ScriptPackage::read(file, p), QStringLiteral("A package tree has at most 16 packages"));
    // Assets: 32 files, 4 MiB for each and 16 MiB in total, each limit included.
    QJsonObject assets;
    for (int i = 0; i < 32; ++i)
        assets.insert(QStringLiteral("a%1").arg(i), QStringLiteral("init.luau"));
    PUT(file, manifest({{QStringLiteral("assets"), assets}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).isEmpty());
    assets.insert(QStringLiteral("a32"), QStringLiteral("init.luau"));
    PUT(file, manifest({{QStringLiteral("assets"), assets}}));
    EXPECT_EQ(ScriptPackage::read(file, p), QStringLiteral("package.json: assets must declare at most 32 files"));
    const auto sizeRule = [](const char* name) {
        return QStringLiteral(
                   "package.json: asset %1 must have a name of 1 to 64 letters, digits, _ and -, and be a file "
                   "of at most 4 MiB. All assets together have at most 16 MiB")
            .arg(QLatin1String(name));
    };
    constexpr qsizetype MiB = 1024 * 1024;
    PUT(dir.filePath(QStringLiteral("big.bin")), QByteArray(4 * MiB, 'x'));
    PUT(file, manifest({{QStringLiteral("assets"), QJsonObject{{QStringLiteral("big"), QStringLiteral("big.bin")}}}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).isEmpty());
    PUT(dir.filePath(QStringLiteral("big.bin")), QByteArray(4 * MiB + 1, 'x'));
    EXPECT_EQ(ScriptPackage::read(file, p), sizeRule("big"));
    // Four files of 4 MiB fill the total. A fifth of one byte is under its own
    // limit, so only the total refuses it. Keys are read in order: a4 is last.
    QJsonObject full;
    for (int i = 0; i < 4; ++i) {
        PUT(dir.filePath(QStringLiteral("full%1.bin").arg(i)), QByteArray(4 * MiB, 'x'));
        full.insert(QStringLiteral("a%1").arg(i), QStringLiteral("full%1.bin").arg(i));
    }
    PUT(file, manifest({{QStringLiteral("assets"), full}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).isEmpty());
    PUT(dir.filePath(QStringLiteral("byte.bin")), "x");
    full.insert(QStringLiteral("a4"), QStringLiteral("byte.bin"));
    PUT(file, manifest({{QStringLiteral("assets"), full}}));
    EXPECT_EQ(ScriptPackage::read(file, p), sizeRule("a4"));
    // Shader packs: at least one, at most 16.
    ASSERT_TRUE(QDir().mkpath(dir.filePath(QStringLiteral("empty"))));
    PUT(file, manifest({{QStringLiteral("shaders"), QJsonObject{{QStringLiteral("path"), QStringLiteral("empty")}}}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains(QStringLiteral("1 to 16 packs")));
    for (int i = 0; i < 17; ++i)
        PUT(dir.filePath(QStringLiteral("many/p%1/metadata.json").arg(i)),
            QStringLiteral(R"({"id":"test.p%1"})").arg(i).toUtf8());
    PUT(file, manifest({{QStringLiteral("shaders"), QJsonObject{{QStringLiteral("path"), QStringLiteral("many")}}}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).contains(QStringLiteral("1 to 16 packs")));
}

TEST(ScriptPackage, ModuleErrorsNameTheirFileAndLine)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto file = dir.filePath(QStringLiteral("package.json"));
    PUT(dir.filePath(QStringLiteral("init.luau")), "local util = require('util')\nreturn {}");
    PUT(dir.filePath(QStringLiteral("util.luau")), "local x = 1\nlocal y = 2\nerror('broken')\nreturn {}");
    PUT(file,
        manifest({{QStringLiteral("modules"),
                   QJsonObject{{QStringLiteral("main"), QStringLiteral("init.luau")},
                               {QStringLiteral("util"), QStringLiteral("util.luau")}}}}));
    ScriptPackage p;
    ASSERT_TRUE(ScriptPackage::read(file, p).isEmpty());
    ASSERT_EQ(p.modules.size(), 2);
    // Find the generated line of util.luau's third line, and map it back.
    const auto util = std::find_if(p.modules.cbegin(), p.modules.cend(), [](const auto& m) {
        return m.file == QLatin1String("util.luau");
    });
    ASSERT_NE(util, p.modules.cend());
    const QByteArrayList lines = p.source.split('\n');
    ASSERT_GE(lines.size(), util->line + 2);
    EXPECT_EQ(lines[util->line + 1], "error('broken')");
    const QString message = QStringLiteral("package.json:%1: broken").arg(util->line + 2);
    EXPECT_EQ(ScriptPackage::locate(p.modules, message), QStringLiteral("util.luau:3: broken"));
    EXPECT_EQ(ScriptPackage::locate(p.modules, QStringLiteral("package.json:1: loader")),
              QStringLiteral("package.json:1: loader"));
}

TEST(ScriptPackage, EachManifestRuleHasItsOwnError)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto file = dir.filePath(QStringLiteral("package.json"));
    PUT(dir.filePath(QStringLiteral("init.luau")), "return {}");
    ScriptPackage p;
    const std::pair<QJsonObject, const char*> bad[] = {
        {{{QStringLiteral("version"), QStringLiteral("1.0")}}, "package.json: version must be X.Y.Z"},
        {{{QStringLiteral("version"), QStringLiteral("01.0.0")}}, "package.json: version must be X.Y.Z"},
        {{{QStringLiteral("modules"), QJsonArray{QStringLiteral("init.luau")}}},
         "package.json: modules must map module names to files"},
        {{{QStringLiteral("entry"), QStringLiteral("no entry")}}, "package.json: entry must name a module"},
        {{{QStringLiteral("entry"), QStringLiteral("other")}},
         "package.json: the entry module other is not in modules"},
        {{{QStringLiteral("dependencies"), QJsonArray{}}}, "package.json: dependencies must map package ids"},
        {{{QStringLiteral("modules"),
           QJsonObject{{QStringLiteral("main"), QStringLiteral("init.luau")},
                       {QStringLiteral("bad name"), QStringLiteral("init.luau")}}}},
         "package.json: module bad name needs a name"},
        {{{QStringLiteral("modules"),
           QJsonObject{{QStringLiteral("main"), QStringLiteral("init.luau")}, {QStringLiteral("other"), 5}}}},
         "package.json: module other needs a name"},
    };
    for (const auto& [fields, error] : bad) {
        PUT(file, manifest(fields));
        const QString message = ScriptPackage::read(file, p);
        EXPECT_TRUE(message.startsWith(QLatin1String(error))) << message.toStdString();
    }
    // A name that was never checked is cut in the message.
    PUT(file, manifest({{QStringLiteral("assets"), QJsonObject{{QString(300, QLatin1Char('a')), 5}}}}));
    EXPECT_EQ(
        ScriptPackage::read(file, p),
        QStringLiteral("package.json: asset %1… must be a path in the package").arg(QString(64, QLatin1Char('a'))));
    // 64 modules in one tree, and no more.
    QJsonObject modules{{QStringLiteral("main"), QStringLiteral("init.luau")}};
    for (int i = 1; i < 64; ++i)
        modules.insert(QStringLiteral("m%1").arg(i), QStringLiteral("init.luau"));
    PUT(file, manifest({{QStringLiteral("modules"), modules}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).isEmpty());
    modules.insert(QStringLiteral("extra"), QStringLiteral("init.luau"));
    PUT(file, manifest({{QStringLiteral("modules"), modules}}));
    EXPECT_EQ(ScriptPackage::read(file, p), QStringLiteral("A package tree has at most 64 modules"));
    // A package directory that does not resolve has no root to hold paths in.
    EXPECT_TRUE(ScriptPackage::read(dir.filePath(QStringLiteral("missing/package.json")), p)
                    .startsWith(QStringLiteral("Cannot resolve the package directory")));
}

TEST(ScriptPackage, ASymlinkBackIntoTheTreeIsACycle)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto file = dir.filePath(QStringLiteral("package.json"));
    PUT(dir.filePath(QStringLiteral("init.luau")), "return {}");
    PUT(dir.filePath(QStringLiteral("a/package.json")),
        R"({"id":"a","version":"1.0.0","api_version":1,"entry":"main","modules":{"main":"m.luau"},"dependencies":{"b":{"path":"b","version":"1.0.0"}}})");
    PUT(dir.filePath(QStringLiteral("a/m.luau")), "return {}");
    // a/b is a itself: by its path a new package, by where it leads the one being read.
    ASSERT_TRUE(QFile::link(dir.filePath(QStringLiteral("a")), dir.filePath(QStringLiteral("a/b"))));
    PUT(file,
        manifest({{QStringLiteral("dependencies"),
                   QJsonObject{{QStringLiteral("a"),
                                QJsonObject{{QStringLiteral("path"), QStringLiteral("a")},
                                            {QStringLiteral("version"), QStringLiteral("1.0.0")}}}}}}));
    ScriptPackage p;
    EXPECT_EQ(ScriptPackage::read(file, p), QStringLiteral("Dependency cycle at a/b/package.json"));
}

TEST(ScriptPackage, ADependencyErrorIsNotReplacedByALaterOne)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto file = dir.filePath(QStringLiteral("package.json"));
    PUT(dir.filePath(QStringLiteral("init.luau")), "return {}");
    PUT(dir.filePath(QStringLiteral("vendor/lib/package.json")),
        R"({"id":"lib","version":"1.0.0","api_version":2,"entry":"main","modules":{"main":"lib.luau"}})");
    // The root's own modules break a rule too: its error would come later.
    PUT(file,
        manifest({{QStringLiteral("modules"),
                   QJsonObject{{QStringLiteral("main"), QStringLiteral("init.luau")},
                               {QStringLiteral("bad name"), QStringLiteral("init.luau")}}},
                  {QStringLiteral("dependencies"),
                   QJsonObject{{QStringLiteral("lib"),
                                QJsonObject{{QStringLiteral("path"), QStringLiteral("vendor/lib")},
                                            {QStringLiteral("version"), QStringLiteral("1.0.0")}}}}}}));
    ScriptPackage p;
    EXPECT_EQ(ScriptPackage::read(file, p), QStringLiteral("vendor/lib/package.json: api_version must be 1"));
    // So does an invalid config.json: its error stands.
    PUT(file,
        manifest({{QStringLiteral("modules"),
                   QJsonObject{{QStringLiteral("main"), QStringLiteral("init.luau")},
                               {QStringLiteral("bad name"), QStringLiteral("init.luau")}}}}));
    PUT(dir.filePath(QStringLiteral("config.json")), "[1]");
    EXPECT_TRUE(ScriptPackage::read(file, p).startsWith(QStringLiteral("Invalid package JSON")))
        << ScriptPackage::read(file, p).toStdString();
}

TEST(ScriptPackage, AnEscapingPathIsWatchedSoItsFixLoads)
{
    QTemporaryDir dir, outside;
    ASSERT_TRUE(dir.isValid() && outside.isValid());
    const auto file = dir.filePath(QStringLiteral("package.json"));
    PUT(outside.filePath(QStringLiteral("module.luau")), "return {}");
    ASSERT_TRUE(
        QFile::link(outside.filePath(QStringLiteral("module.luau")), dir.filePath(QStringLiteral("init.luau"))));
    PUT(file, manifest());
    ScriptPackage p;
    EXPECT_TRUE(ScriptPackage::read(file, p).contains(QStringLiteral("escapes its root")));
    EXPECT_TRUE(p.watched.contains(dir.filePath(QStringLiteral("init.luau"))));
}

TEST(ScriptPackage, CommandTextRefusesSeparatorsAndBidirectionalControls)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto file = dir.filePath(QStringLiteral("package.json"));
    PUT(dir.filePath(QStringLiteral("init.luau")), "return {}");
    ScriptPackage p;
    for (const char16_t c : {u'\u2028', u'\u2029', u'\u061c', u'\u200e', u'\u200f', u'\u202a', u'\u202e', u'\u2066',
                             u'\u2069', u'\u0085'}) {
        const QString summary = QStringLiteral("a") + QChar(c) + QStringLiteral("b");
        PUT(file,
            manifest({{QStringLiteral("commands"),
                       QJsonObject{{QStringLiteral("go"), QJsonObject{{QStringLiteral("summary"), summary}}}}}}));
        EXPECT_TRUE(ScriptPackage::read(file, p).contains(QStringLiteral("bidirectional controls"))) << int(c);
    }
    // Other formatting characters stay: a zero-width joiner is part of an emoji.
    PUT(file,
        manifest({{QStringLiteral("commands"),
                   QJsonObject{{QStringLiteral("go"),
                                QJsonObject{{QStringLiteral("summary"),
                                             QStringLiteral("a") + QChar(u'\u200d') + QStringLiteral("b")}}}}}}));
    EXPECT_TRUE(ScriptPackage::read(file, p).isEmpty());
}

TEST(ScriptPackage, APackDirectoryNameIsCheckedFirst)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto file = dir.filePath(QStringLiteral("package.json"));
    PUT(dir.filePath(QStringLiteral("init.luau")), "return {}");
    // No metadata.json: only the name can refuse it.
    ASSERT_TRUE(QDir().mkpath(dir.filePath(QStringLiteral("shaders/bad name"))));
    PUT(file,
        manifest({{QStringLiteral("shaders"), QJsonObject{{QStringLiteral("path"), QStringLiteral("shaders")}}}}));
    ScriptPackage p;
    EXPECT_EQ(ScriptPackage::read(file, p),
              QStringLiteral("package.json: shader pack directory bad name must be 1 to 64 letters, digits, _ and -"));
}

TEST(ScriptPackage, ErrorsAtTheEndOfAModuleNameTheModule)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const auto file = dir.filePath(QStringLiteral("package.json"));
    PUT(dir.filePath(QStringLiteral("init.luau")), "return {}");
    PUT(dir.filePath(QStringLiteral("util.luau")), "local x = 1\nif x then end\nreturn {}");
    PUT(file,
        manifest({{QStringLiteral("modules"),
                   QJsonObject{{QStringLiteral("main"), QStringLiteral("init.luau")},
                               {QStringLiteral("util"), QStringLiteral("util.luau")}}}}));
    ScriptPackage p;
    ASSERT_TRUE(ScriptPackage::read(file, p).isEmpty());
    const auto util = std::find_if(p.modules.cbegin(), p.modules.cend(), [](const auto& m) {
        return m.file == QLatin1String("util.luau");
    });
    ASSERT_NE(util, p.modules.cend());
    // Its byte range in the chunk is its source, which ScriptHost parses alone.
    EXPECT_EQ(p.source.mid(util->offset, util->size), "local x = 1\nif x then end\nreturn {}");
    // The line that closes its factory counts as its last line.
    EXPECT_EQ(ScriptPackage::locate(p.modules, QStringLiteral("package.json:%1: closed").arg(util->line + util->lines)),
              QStringLiteral("util.luau:3: closed"));
    // Only the chunk's positions change. Lines named in the text, as a
    // script's own error can have them, stay as they are.
    const QString message = QStringLiteral("package.json:%1: could not close 'door' at line 9").arg(util->line);
    EXPECT_EQ(ScriptPackage::locate(p.modules, message),
              QStringLiteral("util.luau:1: could not close 'door' at line 9"));
    // The loader's own lines keep their positions.
    EXPECT_EQ(ScriptPackage::locate(p.modules, QStringLiteral("package.json:1: (to close 'function' at line 1)")),
              QStringLiteral("package.json:1: (to close 'function' at line 1)"));
}
