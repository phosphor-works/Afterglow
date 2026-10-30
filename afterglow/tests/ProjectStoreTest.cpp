// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ProjectStore.h"
#include "SplitTree.h"
#include <QFile>
#include <QJsonArray>
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
    ASSERT_TRUE(QFile::link(broken.fileName(), dir.filePath("link.json")));
    EXPECT_FALSE(store.read("link", output).isEmpty());
    EXPECT_FALSE(store.save("link", project(), true).isEmpty());
    EXPECT_FALSE(store.names().contains("link"));
    ASSERT_TRUE(broken.open(QIODevice::WriteOnly));
    broken.write(QByteArray(1024 * 1024 + 1, ' ')); broken.close();
    EXPECT_FALSE(store.read("broken", output).isEmpty());
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
TEST(SplitTree, LayoutRoundTripRetainsNestedRatios)
{
    auto* a = reinterpret_cast<Pane*>(1); auto* b = reinterpret_cast<Pane*>(2); auto* c = reinterpret_cast<Pane*>(3);
    SplitTree original(a);
    original.split(a, b, SplitTree::Direction::Right);
    original.split(b, c, SplitTree::Direction::Down);
    original.layout(QRect(0, 0, 1200, 800), 2);
    ASSERT_TRUE(original.resizePane(a, SplitTree::Neighbour::Right, 100, 2, QSize(40, 20)));
    original.layout(QRect(0, 0, 1200, 800), 2);
    auto encode = [](Pane* p) { return QJsonObject{{"id", QString::number(reinterpret_cast<quintptr>(p))}}; };
    const auto saved = original.snapshot(encode);
    auto restored = SplitTree::restore(saved, [](const QJsonObject& p) { return reinterpret_cast<Pane*>(p["id"].toString().toULongLong()); });
    restored->layout(QRect(0, 0, 1200, 800), 2);
    EXPECT_EQ(restored->snapshot(encode), saved);
    for (auto* p : {a,b,c}) EXPECT_EQ(restored->rectOf(p), original.rectOf(p));
    EXPECT_EQ(restored->paneCount(), 3);
}
