// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProjectStore.h"
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
    return {{"version", 1}, {"active_tab", "main"}, {"tabs", QJsonArray{QJsonObject{
        {"id", "main"}, {"title", "Development"}, {"active_pane", "shell"}, {"tree", tree}}}}};
}
}
TEST(ProjectStore, RoundTripAndExplicitReplacement)
{
    QTemporaryDir dir;
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
    ProjectStore store(dir.path());
    for (const auto name : {"../oops", "a/b", "", ".", "two words"}) EXPECT_FALSE(store.save(name, project(), false).isEmpty());
    QFile broken(dir.filePath("broken.json"));
    ASSERT_TRUE(broken.open(QIODevice::WriteOnly));
    broken.write("{bad"); broken.close();
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
    padded.write(QJsonDocument(project()).toJson(QJsonDocument::Compact) + QByteArray(1024 * 1024, ' ')); padded.close();
    EXPECT_FALSE(store.read("padded", output).isEmpty());
}
TEST(ProjectStore, ReadNamesAJsonRootThatIsNotAnObject)
{
    QTemporaryDir dir;
    ProjectStore store(dir.path());
    QFile file(dir.filePath("list.json"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("[1, 2]"); file.close();
    QJsonObject output;
    const QString error = store.read("list", output);
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(error.contains("no error occurred")) << error.toStdString();
}
TEST(ProjectStore, RejectsInvalidVersionsIdentitiesRatiosAndCommands)
{
    auto doc = project(); doc["version"] = 2;
    EXPECT_FALSE(ProjectStore::validate(doc).isEmpty());
    doc = project(); doc["active_tab"] = "missing";
    EXPECT_FALSE(ProjectStore::validate(doc).isEmpty());
    QJsonObject split{{"direction", "right"}, {"ratio", 0.3}, {"first", pane()}, {"second", pane()}};
    EXPECT_FALSE(ProjectStore::validate(project(split)).isEmpty());
    split["second"] = pane("second");
    EXPECT_TRUE(ProjectStore::validate(project(split)).isEmpty());
    for (double ratio : {0., 1., -0.5, 2.}) {
        split["ratio"] = ratio;
        EXPECT_FALSE(ProjectStore::validate(project(split)).isEmpty());
    }
    for (const auto cwd : {"relative", "", "$HOME"}) {
        auto p = pane()["pane"].toObject(); p["cwd"] = cwd;
        EXPECT_FALSE(ProjectStore::validate(project({{"pane", p}})).isEmpty());
    }
    auto p = pane()["pane"].toObject(); p["command"] = QJsonArray{42};
    EXPECT_FALSE(ProjectStore::validate(project({{"pane", p}})).isEmpty());
    p["command"] = QJsonArray{"sh", "-c", "echo test"};
    EXPECT_TRUE(ProjectStore::validate(project({{"pane", p}})).isEmpty());
}
TEST(ProjectStore, BoundsTreesBeforeAnyPaneIsCreated)
{
    auto root = pane();
    for (int i = 0; i < 17; ++i) root = {{"direction", "down"}, {"ratio", 0.5}, {"first", root}, {"second", pane(QString::number(i))}};
    EXPECT_FALSE(ProjectStore::validate(project(root)).isEmpty());
    auto doc = project();
    auto tabs = doc["tabs"].toArray(); tabs.append(tabs.first()); doc["tabs"] = tabs;
    EXPECT_FALSE(ProjectStore::validate(doc).isEmpty());
}
TEST(ProjectStore, BoundsAcceptTheLimitsAndRejectOnePast)
{
    // Depth 16 is accepted, 17 is not.
    const auto chain = [](int depth) {
        auto root = pane();
        for (int i = 0; i < depth; ++i) root = {{"direction", "down"}, {"ratio", 0.5}, {"first", root}, {"second", pane(QString::number(i))}};
        return root;
    };
    EXPECT_TRUE(ProjectStore::validate(project(chain(16))).isEmpty());
    EXPECT_FALSE(ProjectStore::validate(project(chain(17))).isEmpty());
    // 32 panes are accepted, 33 are not: a balanced tree stays within the depth.
    const auto balanced = [](int count) {
        QJsonArray level;
        for (int i = 0; i < count; ++i) level.append(i == 0 ? pane() : pane(QString::number(i)));
        while (level.size() > 1) {
            QJsonArray next;
            for (qsizetype i = 0; i < level.size(); i += 2) {
                if (i + 1 < level.size()) next.append(QJsonObject{{"direction", "right"}, {"ratio", 0.5}, {"first", level[i]}, {"second", level[i + 1]}});
                else next.append(level[i]);
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
    auto second = list[1].toObject(); second["id"] = "0"; list[1] = second; doc["tabs"] = list;
    EXPECT_FALSE(ProjectStore::validate(doc).isEmpty());
    doc = tabs(2);
    list = doc["tabs"].toArray();
    second = list[1].toObject(); second["tree"] = pane("0"); second["active_pane"] = "0"; list[1] = second; doc["tabs"] = list;
    EXPECT_FALSE(ProjectStore::validate(doc).isEmpty());
}
TEST(ProjectStore, EachLimitNamesItselfInTheError)
{
    const auto chain = [](int depth) {
        auto root = pane();
        for (int i = 0; i < depth; ++i) root = {{"direction", "down"}, {"ratio", 0.5}, {"first", root}, {"second", pane(QString::number(i))}};
        return root;
    };
    EXPECT_TRUE(ProjectStore::validate(project(chain(17))).contains("16 levels"));
    // 33 panes in a tree of depth 6, so only the pane count is over.
    QJsonArray level;
    for (int i = 0; i < 33; ++i) level.append(i == 0 ? pane() : pane(QString::number(i)));
    while (level.size() > 1) {
        QJsonArray next;
        for (qsizetype i = 0; i < level.size(); i += 2) {
            if (i + 1 < level.size()) next.append(QJsonObject{{"direction", "right"}, {"ratio", 0.5}, {"first", level[i]}, {"second", level[i + 1]}});
            else next.append(level[i]);
        }
        level = next;
    }
    EXPECT_TRUE(ProjectStore::validate(project(level.first().toObject())).contains("32 panes"));
    QJsonObject split{{"direction", "right"}, {"ratio", 0.3}, {"first", pane()}, {"second", pane()}};
    EXPECT_TRUE(ProjectStore::validate(project(split)).contains("\"shell\" is used more than once"));
    auto p = pane()["pane"].toObject();
    QJsonArray many;
    for (int i = 0; i < 65; ++i) many.append("x");
    p["command"] = many;
    EXPECT_TRUE(ProjectStore::validate(project({{"pane", p}})).contains("64 arguments"));
    p["command"] = QJsonArray{"sh", QString(4097, QLatin1Char('a'))};
    EXPECT_TRUE(ProjectStore::validate(project({{"pane", p}})).contains("argument"));
    p = pane()["pane"].toObject(); p["title"] = QString(257, QLatin1Char('t'));
    EXPECT_TRUE(ProjectStore::validate(project({{"pane", p}})).contains("pane title"));
    auto doc = project();
    auto tabs = doc["tabs"].toArray();
    auto tab = tabs[0].toObject(); tab["active_pane"] = "elsewhere"; tabs[0] = tab; doc["tabs"] = tabs;
    EXPECT_TRUE(ProjectStore::validate(doc).contains("active pane"));
}
