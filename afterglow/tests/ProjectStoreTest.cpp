// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProjectStore.h"
#include "SavedEnvironment.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <gtest/gtest.h>

using namespace Afterglow;
namespace {
QJsonObject pane(QString id = QStringLiteral("shell"))
{
    return {{"pane", QJsonObject{{"id", id}, {"cwd", "~"}, {"title", ""}, {"command", QJsonArray{}}}}};
}
QJsonObject project(QJsonObject tree = pane())
{
    return {
        {"version", 1},
        {"active_tab", "main"},
        {"tabs",
         QJsonArray{QJsonObject{{"id", "main"}, {"title", "Development"}, {"active_pane", "shell"}, {"tree", tree}}}}};
}
}
TEST(ProjectStore, RoundTripAndExplicitReplacement)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ProjectStore store(dir.path());
    const auto original = project();
    ASSERT_TRUE(store.save("dev", original, false).isEmpty());
    EXPECT_EQ(store.names(), QStringList{"dev"});
    QJsonObject read;
    ASSERT_TRUE(store.read("dev", read).isEmpty());
    EXPECT_EQ(read, original);
    EXPECT_FALSE(store.save("dev", original, false).isEmpty());
    EXPECT_TRUE(store.save("dev", original, true).isEmpty());
}
TEST(ProjectStore, RejectsUnsafeNamesCorruptFilesAndSymlinks)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ProjectStore store(dir.path());
    for (const auto name : {"../oops", "a/b", "", ".", "two words"})
        EXPECT_FALSE(store.save(name, project(), false).isEmpty());
    QFile broken(dir.filePath("broken.json"));
    ASSERT_TRUE(broken.open(QIODevice::WriteOnly));
    broken.write("{bad");
    broken.close();
    QJsonObject output;
    EXPECT_FALSE(store.read("broken", output).isEmpty());
    // The link points at a valid project: only the symlink rule refuses it.
    ASSERT_TRUE(store.save("dev", project(), false).isEmpty());
    ASSERT_TRUE(QFile::link(dir.filePath("dev.json"), dir.filePath("link.json")));
    ASSERT_TRUE(store.read("dev", output).isEmpty());
    EXPECT_FALSE(store.read("link", output).isEmpty());
    EXPECT_FALSE(store.save("link", project(), true).isEmpty());
    EXPECT_FALSE(store.names().contains("link"));
    // A valid project padded past 1 MiB with whitespace, which JSON allows:
    // only the size rule refuses it.
    QFile padded(dir.filePath("padded.json"));
    ASSERT_TRUE(padded.open(QIODevice::WriteOnly));
    padded.write(QJsonDocument(project()).toJson(QJsonDocument::Compact) + QByteArray(1024 * 1024, ' '));
    padded.close();
    EXPECT_FALSE(store.read("padded", output).isEmpty());
}
TEST(ProjectStore, ReadNamesAJsonRootThatIsNotAnObject)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ProjectStore store(dir.path());
    QFile file(dir.filePath("list.json"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("[1, 2]");
    file.close();
    QJsonObject output;
    const QString error = store.read("list", output);
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(error.contains("no error occurred")) << error.toStdString();
}
TEST(ProjectStore, RejectsInvalidVersionsIdentitiesRatiosAndCommands)
{
    auto doc = project();
    doc["version"] = 2;
    EXPECT_FALSE(ProjectStore::validate(doc).isEmpty());
    doc = project();
    doc["active_tab"] = "missing";
    EXPECT_FALSE(ProjectStore::validate(doc).isEmpty());
    QJsonObject split{{"direction", "right"}, {"ratio", 0.3}, {"first", pane()}, {"second", pane()}};
    EXPECT_FALSE(ProjectStore::validate(project(split)).isEmpty());
    split["second"] = pane("second");
    EXPECT_TRUE(ProjectStore::validate(project(split)).isEmpty());
    for (double ratio : {0., 1., -0.5, 2.}) {
        split["ratio"] = ratio;
        EXPECT_FALSE(ProjectStore::validate(project(split)).isEmpty());
    }
    // ":/" and ":dir" are Qt resource paths, not absolute directories.
    for (const auto cwd : {"relative", "", "$HOME", ":/", ":dir"}) {
        auto p = pane()["pane"].toObject();
        p["cwd"] = cwd;
        EXPECT_TRUE(
            ProjectStore::validate(project({{"pane", p}})).startsWith("A pane directory must be an absolute path"))
            << cwd << ": " << ProjectStore::validate(project({{"pane", p}})).toStdString();
    }
    auto p = pane()["pane"].toObject();
    p["command"] = QJsonArray{42};
    EXPECT_FALSE(ProjectStore::validate(project({{"pane", p}})).isEmpty());
    p["command"] = QJsonArray{"sh", "-c", "echo test"};
    EXPECT_TRUE(ProjectStore::validate(project({{"pane", p}})).isEmpty());
}
// "session" says whether the project opens into a mux session: true or
// false, and false when it is not there.
TEST(ProjectStore, TheSessionOfAProjectIsTrueOrFalse)
{
    auto doc = project();
    EXPECT_FALSE(ProjectStore::opensSession(doc));
    doc["session"] = true;
    EXPECT_TRUE(ProjectStore::validate(doc).isEmpty());
    EXPECT_TRUE(ProjectStore::opensSession(doc));
    doc["session"] = false;
    EXPECT_TRUE(ProjectStore::validate(doc).isEmpty());
    EXPECT_FALSE(ProjectStore::opensSession(doc));
    for (const QJsonValue& value : {QJsonValue(1), QJsonValue("yes"), QJsonValue(QJsonValue::Null)}) {
        doc["session"] = value;
        EXPECT_EQ(ProjectStore::validate(doc), QStringLiteral("The session of a project is true or false"));
    }
}

// The first pane that a project starts, which starts its session, and its
// launch as a window starts it.
TEST(ProjectStore, TheFirstPaneIsTheFirstLeafOfTheFirstTab)
{
    QJsonObject first{{"id", "first"}, {"cwd", "/"}, {"title", ""}, {"command", QJsonArray{"/no/such/program", "-x"}}};
    QJsonObject split{{"direction", "down"},
                      {"ratio", 0.5},
                      {"first", QJsonObject{{"pane", first}}},
                      {"second", pane("second")}};
    auto doc = project(split);
    // The active pane is not the first: the first is the first leaf.
    QJsonArray tabs = doc["tabs"].toArray();
    QJsonObject tab = tabs[0].toObject();
    tab["active_pane"] = "second";
    tabs[0] = tab;
    doc["tabs"] = tabs;
    EXPECT_EQ(ProjectStore::panes(doc).value(0), first);
    QStringList warnings;
    const ProjectStore::Launch launch = ProjectStore::launchOf(first, warnings);
    EXPECT_EQ(launch.cwd, QStringLiteral("/"));
    // A command that is not there starts a shell, and the project keeps it.
    EXPECT_TRUE(launch.command.isEmpty());
    EXPECT_EQ(launch.configured, (QStringList{"/no/such/program", "-x"}));
    ASSERT_EQ(warnings.size(), 1);
    EXPECT_TRUE(warnings.first().contains(QLatin1String("command unavailable"))) << warnings.first().toStdString();
    // A directory that is not there is the home directory.
    first["cwd"] = "/no/such/directory";
    first["command"] = QJsonArray{};
    warnings.clear();
    EXPECT_EQ(ProjectStore::launchOf(first, warnings).cwd, QDir::homePath());
    EXPECT_EQ(warnings.size(), 1);
}

TEST(ProjectStore, BoundsTreesBeforeAnyPaneIsCreated)
{
    auto root = pane();
    for (int i = 0; i < 17; ++i)
        root = {{"direction", "down"}, {"ratio", 0.5}, {"first", root}, {"second", pane(QString::number(i))}};
    EXPECT_FALSE(ProjectStore::validate(project(root)).isEmpty());
    auto doc = project();
    auto tabs = doc["tabs"].toArray();
    tabs.append(tabs.first());
    doc["tabs"] = tabs;
    EXPECT_FALSE(ProjectStore::validate(doc).isEmpty());
}
TEST(ProjectStore, BoundsAcceptTheLimitsAndRejectOnePast)
{
    // Depth 16 is accepted, 17 is not.
    const auto chain = [](int depth) {
        auto root = pane();
        for (int i = 0; i < depth; ++i)
            root = {{"direction", "down"}, {"ratio", 0.5}, {"first", root}, {"second", pane(QString::number(i))}};
        return root;
    };
    EXPECT_TRUE(ProjectStore::validate(project(chain(16))).isEmpty());
    EXPECT_FALSE(ProjectStore::validate(project(chain(17))).isEmpty());
    // 32 panes are accepted, 33 are not: a balanced tree stays within the depth.
    const auto balanced = [](int count) {
        QJsonArray level;
        for (int i = 0; i < count; ++i)
            level.append(i == 0 ? pane() : pane(QString::number(i)));
        while (level.size() > 1) {
            QJsonArray next;
            for (qsizetype i = 0; i < level.size(); i += 2) {
                if (i + 1 < level.size())
                    next.append(QJsonObject{{"direction", "right"},
                                            {"ratio", 0.5},
                                            {"first", level[i]},
                                            {"second", level[i + 1]}});
                else
                    next.append(level[i]);
            }
            level = next;
        }
        return level.first().toObject();
    };
    EXPECT_TRUE(ProjectStore::validate(project(balanced(32))).isEmpty());
    EXPECT_FALSE(ProjectStore::validate(project(balanced(33))).isEmpty());
    // 16 tabs are accepted, 17 are not; each tab has its own ids.
    const auto tabs = [](int count) {
        QJsonArray list;
        for (int i = 0; i < count; ++i) {
            const QString id = QString::number(i);
            list.append(QJsonObject{{"id", id}, {"title", id}, {"active_pane", id}, {"tree", pane(id)}});
        }
        return QJsonObject{{"version", 1}, {"active_tab", "0"}, {"tabs", list}};
    };
    EXPECT_TRUE(ProjectStore::validate(tabs(16)).isEmpty());
    EXPECT_FALSE(ProjectStore::validate(tabs(17)).isEmpty());
    // A repeated tab id alone, then a repeated pane id alone.
    auto doc = tabs(2);
    auto list = doc["tabs"].toArray();
    auto second = list[1].toObject();
    second["id"] = "0";
    list[1] = second;
    doc["tabs"] = list;
    EXPECT_FALSE(ProjectStore::validate(doc).isEmpty());
    doc = tabs(2);
    list = doc["tabs"].toArray();
    second = list[1].toObject();
    second["tree"] = pane("0");
    second["active_pane"] = "0";
    list[1] = second;
    doc["tabs"] = list;
    EXPECT_FALSE(ProjectStore::validate(doc).isEmpty());
}
TEST(ProjectStore, EachLimitNamesItselfInTheError)
{
    const auto chain = [](int depth) {
        auto root = pane();
        for (int i = 0; i < depth; ++i)
            root = {{"direction", "down"}, {"ratio", 0.5}, {"first", root}, {"second", pane(QString::number(i))}};
        return root;
    };
    EXPECT_TRUE(ProjectStore::validate(project(chain(17))).contains("16 levels"));
    // The count of the window, with the limit.
    EXPECT_TRUE(ProjectStore::validate(project(chain(17))).contains("17 levels deep"));
    // 33 panes in a tree of depth 6, so only the pane count is over.
    QJsonArray level;
    for (int i = 0; i < 33; ++i)
        level.append(i == 0 ? pane() : pane(QString::number(i)));
    while (level.size() > 1) {
        QJsonArray next;
        for (qsizetype i = 0; i < level.size(); i += 2) {
            if (i + 1 < level.size())
                next.append(
                    QJsonObject{{"direction", "right"}, {"ratio", 0.5}, {"first", level[i]}, {"second", level[i + 1]}});
            else
                next.append(level[i]);
        }
        level = next;
    }
    EXPECT_TRUE(ProjectStore::validate(project(level.first().toObject())).contains("32 panes"));
    EXPECT_TRUE(ProjectStore::validate(project(level.first().toObject())).contains("this one has 33"));
    QJsonArray tabList;
    for (int i = 0; i < 17; ++i) {
        const QString id = QString::number(i);
        tabList.append(QJsonObject{{"id", id}, {"title", id}, {"active_pane", id}, {"tree", pane(id)}});
    }
    EXPECT_TRUE(ProjectStore::validate(QJsonObject{{"version", 1}, {"active_tab", "0"}, {"tabs", tabList}})
                    .contains("this one has 17"));
    QJsonObject split{{"direction", "right"}, {"ratio", 0.3}, {"first", pane()}, {"second", pane()}};
    EXPECT_TRUE(ProjectStore::validate(project(split)).contains("\"shell\" is used more than once"));
    auto p = pane()["pane"].toObject();
    QJsonArray many;
    for (int i = 0; i < 65; ++i)
        many.append("x");
    p["command"] = many;
    EXPECT_TRUE(ProjectStore::validate(project({{"pane", p}})).contains("64 arguments"));
    p["command"] = QJsonArray{"sh", QString(4097, QLatin1Char('a'))};
    EXPECT_TRUE(ProjectStore::validate(project({{"pane", p}})).contains("argument"));
    p = pane()["pane"].toObject();
    p["title"] = QString(257, QLatin1Char('t'));
    EXPECT_TRUE(ProjectStore::validate(project({{"pane", p}})).contains("pane title"));
    auto doc = project();
    auto tabs = doc["tabs"].toArray();
    auto tab = tabs[0].toObject();
    tab["active_pane"] = "elsewhere";
    tabs[0] = tab;
    doc["tabs"] = tabs;
    EXPECT_TRUE(ProjectStore::validate(doc).contains("active pane"));
}

// The panes of a project in the order that a window lays them out: tab by
// tab, each tree's first side before its second. A session starts them in
// this order before its window shows them.
TEST(ProjectStore, ThePanesComeInTheOrderOfTheLayout)
{
    const QJsonObject inner{{"direction", "right"}, {"ratio", 0.5}, {"first", pane("b")}, {"second", pane("c")}};
    const QJsonObject outer{{"direction", "down"}, {"ratio", 0.5}, {"first", pane("a")}, {"second", inner}};
    auto doc = project(outer);
    QJsonArray tabs = doc["tabs"].toArray();
    QJsonObject main = tabs[0].toObject();
    // Neither the active tab nor the active pane comes first.
    main["active_pane"] = "c";
    tabs[0] = main;
    tabs.append(QJsonObject{{"id", "logs"}, {"title", ""}, {"active_pane", "d"}, {"tree", pane("d")}});
    doc["tabs"] = tabs;
    doc["active_tab"] = "logs";
    ASSERT_TRUE(ProjectStore::validate(doc).isEmpty()) << qPrintable(ProjectStore::validate(doc));
    QStringList ids;
    for (const QJsonObject& leaf : ProjectStore::panes(doc))
        ids << leaf.value(QStringLiteral("id")).toString();
    EXPECT_EQ(ids, (QStringList{QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("d")}));
    EXPECT_EQ(ProjectStore::panes(doc).value(0).value(QStringLiteral("id")).toString(), QStringLiteral("a"));
}

// A command argument counts its UTF-8 bytes, as a session's layout does, not
// its UTF-16 units.
TEST(ProjectStore, KeepsCommandCountsUtf8Bytes)
{
    EXPECT_TRUE(ProjectStore::keepsCommand({QStringLiteral("sh"), QString(4096, QLatin1Char('a'))}));
    // 2,049 UTF-16 units, 4,098 bytes.
    const QString accented(2049, QChar(0xE9));
    EXPECT_FALSE(ProjectStore::keepsCommand({QStringLiteral("sh"), accented}));
    EXPECT_FALSE(ProjectStore::keepsCommand(QStringList(65, QStringLiteral("x"))));
    EXPECT_FALSE(ProjectStore::keepsCommand({QString()}));
    EXPECT_TRUE(ProjectStore::keepsCommand({}));
    auto p = pane()["pane"].toObject();
    p["command"] = QJsonArray{"sh", accented};
    EXPECT_TRUE(ProjectStore::validate(project({{"pane", p}})).contains("bytes"))
        << ProjectStore::validate(project({{"pane", p}})).toStdString();
}

// The directory of a pane with ~, and the program of its command: one with a
// slash against the pane's directory, a bare name on the search path.
TEST(ProjectStore, LaunchOfExpandsTheHomeAndFindsTheProgram)
{
    QTemporaryDir home;
    ASSERT_TRUE(home.isValid());
    ASSERT_TRUE(QDir(home.path()).mkpath(QStringLiteral("sub")));
    const Afterglow::Test::Saved savedHome("HOME", QFile::encodeName(home.path()));
    QJsonObject p{{"id", "p"}, {"cwd", "~"}, {"title", ""}, {"command", QJsonArray{}}};
    QStringList warnings;
    EXPECT_EQ(ProjectStore::launchOf(p, warnings).cwd, QDir::homePath());
    EXPECT_EQ(QDir::homePath(), home.path());
    p["cwd"] = "~/sub";
    EXPECT_EQ(ProjectStore::launchOf(p, warnings).cwd, home.path() + QStringLiteral("/sub"));
    EXPECT_TRUE(warnings.isEmpty()) << warnings.join(QLatin1Char('\n')).toStdString();
    // A program on PATH stays.
    p["command"] = QJsonArray{"sh", "-c", "true"};
    EXPECT_EQ(ProjectStore::launchOf(p, warnings).command, (QStringList{"sh", "-c", "true"}));
    // ./x is the pane's own.
    QFile tool(home.filePath(QStringLiteral("sub/x")));
    ASSERT_TRUE(tool.open(QIODevice::WriteOnly));
    tool.write("#!/bin/sh\n");
    tool.close();
    ASSERT_TRUE(tool.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    p["command"] = QJsonArray{"./x"};
    EXPECT_EQ(ProjectStore::launchOf(p, warnings).command, QStringList{"./x"});
    EXPECT_TRUE(warnings.isEmpty()) << warnings.join(QLatin1Char('\n')).toStdString();
}

// A bare program name is looked for in the PATH that the pane's program
// gets, and a relative entry of it in the pane's directory.
TEST(ProjectStore, LaunchOfSearchesTheGivenPath)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("bin")));
    for (const QString& path :
         {dir.filePath(QStringLiteral("agtool-x")), dir.filePath(QStringLiteral("bin/agtool-x"))}) {
        QFile tool(path);
        ASSERT_TRUE(tool.open(QIODevice::WriteOnly));
        tool.write("#!/bin/sh\n");
        tool.close();
        ASSERT_TRUE(tool.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    }
    const QJsonObject p{{"id", "p"}, {"cwd", dir.path()}, {"title", ""}, {"command", QJsonArray{"agtool-x"}}};
    // Not on the PATH of this process.
    QStringList warnings;
    EXPECT_TRUE(ProjectStore::launchOf(p, warnings).command.isEmpty());
    ASSERT_EQ(warnings.size(), 1);
    EXPECT_TRUE(warnings.first().contains(QLatin1String("command unavailable"))) << warnings.first().toStdString();
    warnings.clear();
    EXPECT_EQ(ProjectStore::launchOf(p, warnings, {dir.path()}).command, QStringList{"agtool-x"});
    EXPECT_TRUE(warnings.isEmpty());
    // A relative entry, in the pane's directory.
    EXPECT_EQ(ProjectStore::launchOf(p, warnings, {QStringLiteral("bin")}).command, QStringList{"agtool-x"});
    EXPECT_TRUE(warnings.isEmpty());
}
