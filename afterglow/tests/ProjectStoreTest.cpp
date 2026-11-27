// SPDX-FileCopyrightText: 2026 Phosphor Works Team
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
    return {{QStringLiteral("pane"),
             QJsonObject{{QStringLiteral("id"), id},
                         {QStringLiteral("cwd"), QStringLiteral("~")},
                         {QStringLiteral("title"), QStringLiteral("")},
                         {QStringLiteral("command"), QJsonArray{}}}}};
}
QJsonObject project(QJsonObject tree = pane())
{
    return {{QStringLiteral("version"), 1},
            {QStringLiteral("active_tab"), QStringLiteral("main")},
            {QStringLiteral("tabs"),
             QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("main")},
                                    {QStringLiteral("title"), QStringLiteral("Development")},
                                    {QStringLiteral("active_pane"), QStringLiteral("shell")},
                                    {QStringLiteral("tree"), tree}}}}};
}
}
TEST(ProjectStore, RoundTripAndExplicitReplacement)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ProjectStore store(dir.path());
    const auto original = project();
    ASSERT_TRUE(store.save(QStringLiteral("dev"), original, false).isEmpty());
    EXPECT_EQ(store.names(), QStringList{QStringLiteral("dev")});
    QJsonObject read;
    ASSERT_TRUE(store.read(QStringLiteral("dev"), read).isEmpty());
    EXPECT_EQ(read, original);
    EXPECT_FALSE(store.save(QStringLiteral("dev"), original, false).isEmpty());
    EXPECT_TRUE(store.save(QStringLiteral("dev"), original, true).isEmpty());
}
TEST(ProjectStore, RejectsUnsafeNamesCorruptFilesAndSymlinks)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ProjectStore store(dir.path());
    for (const QString& name : {QStringLiteral("../oops"), QStringLiteral("a/b"), QStringLiteral(""),
                                QStringLiteral("."), QStringLiteral("two words")})
        EXPECT_FALSE(store.save(name, project(), false).isEmpty());
    QFile broken(dir.filePath(QStringLiteral("broken.json")));
    ASSERT_TRUE(broken.open(QIODevice::WriteOnly));
    broken.write("{bad");
    broken.close();
    QJsonObject output;
    EXPECT_FALSE(store.read(QStringLiteral("broken"), output).isEmpty());
    // The link points at a valid project: only the symlink rule refuses it.
    ASSERT_TRUE(store.save(QStringLiteral("dev"), project(), false).isEmpty());
    ASSERT_TRUE(QFile::link(dir.filePath(QStringLiteral("dev.json")), dir.filePath(QStringLiteral("link.json"))));
    ASSERT_TRUE(store.read(QStringLiteral("dev"), output).isEmpty());
    EXPECT_FALSE(store.read(QStringLiteral("link"), output).isEmpty());
    EXPECT_FALSE(store.save(QStringLiteral("link"), project(), true).isEmpty());
    EXPECT_FALSE(store.names().contains(QStringLiteral("link")));
    // A valid project padded past 1 MiB with whitespace, which JSON allows:
    // only the size rule refuses it.
    QFile padded(dir.filePath(QStringLiteral("padded.json")));
    ASSERT_TRUE(padded.open(QIODevice::WriteOnly));
    padded.write(QJsonDocument(project()).toJson(QJsonDocument::Compact) + QByteArray(1024 * 1024, ' '));
    padded.close();
    EXPECT_FALSE(store.read(QStringLiteral("padded"), output).isEmpty());
}
TEST(ProjectStore, ReadNamesAJsonRootThatIsNotAnObject)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ProjectStore store(dir.path());
    QFile file(dir.filePath(QStringLiteral("list.json")));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("[1, 2]");
    file.close();
    QJsonObject output;
    const QString error = store.read(QStringLiteral("list"), output);
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(error.contains(QStringLiteral("no error occurred"))) << error.toStdString();
}
TEST(ProjectStore, RejectsInvalidVersionsIdentitiesRatiosAndCommands)
{
    auto doc = project();
    doc[QStringLiteral("version")] = 2;
    EXPECT_FALSE(ProjectStore::validate(doc).isEmpty());
    doc = project();
    doc[QStringLiteral("active_tab")] = QStringLiteral("missing");
    EXPECT_FALSE(ProjectStore::validate(doc).isEmpty());
    QJsonObject split{{QStringLiteral("direction"), QStringLiteral("right")},
                      {QStringLiteral("ratio"), 0.3},
                      {QStringLiteral("first"), pane()},
                      {QStringLiteral("second"), pane()}};
    EXPECT_FALSE(ProjectStore::validate(project(split)).isEmpty());
    split[QStringLiteral("second")] = pane(QStringLiteral("second"));
    EXPECT_TRUE(ProjectStore::validate(project(split)).isEmpty());
    for (double ratio : {0., 1., -0.5, 2.}) {
        split[QStringLiteral("ratio")] = ratio;
        EXPECT_FALSE(ProjectStore::validate(project(split)).isEmpty());
    }
    // ":/" and ":dir" are Qt resource paths, not absolute directories.
    for (const auto cwd : {"relative", "", "$HOME", ":/", ":dir"}) {
        auto p = pane()[QStringLiteral("pane")].toObject();
        p[QStringLiteral("cwd")] = QString::fromLatin1(cwd);
        EXPECT_TRUE(ProjectStore::validate(project({{QStringLiteral("pane"), p}}))
                        .startsWith(QStringLiteral("A pane directory must be an absolute path")))
            << cwd << ": " << ProjectStore::validate(project({{QStringLiteral("pane"), p}})).toStdString();
    }
    auto p = pane()[QStringLiteral("pane")].toObject();
    p[QStringLiteral("command")] = QJsonArray{42};
    EXPECT_FALSE(ProjectStore::validate(project({{QStringLiteral("pane"), p}})).isEmpty());
    p[QStringLiteral("command")] = QJsonArray{QStringLiteral("sh"), QStringLiteral("-c"), QStringLiteral("echo test")};
    EXPECT_TRUE(ProjectStore::validate(project({{QStringLiteral("pane"), p}})).isEmpty());
}
// "session" says whether the project opens into a mux session: true or
// false, and false when it is not there.
TEST(ProjectStore, TheSessionOfAProjectIsTrueOrFalse)
{
    auto doc = project();
    EXPECT_FALSE(ProjectStore::opensSession(doc));
    doc[QStringLiteral("session")] = true;
    EXPECT_TRUE(ProjectStore::validate(doc).isEmpty());
    EXPECT_TRUE(ProjectStore::opensSession(doc));
    doc[QStringLiteral("session")] = false;
    EXPECT_TRUE(ProjectStore::validate(doc).isEmpty());
    EXPECT_FALSE(ProjectStore::opensSession(doc));
    for (const QJsonValue& value : {QJsonValue(1), QJsonValue(QStringLiteral("yes")), QJsonValue(QJsonValue::Null)}) {
        doc[QStringLiteral("session")] = value;
        EXPECT_EQ(ProjectStore::validate(doc), QStringLiteral("The session of a project is true or false"));
    }
}

// The first pane that a project starts, which starts its session, and its
// launch as a window starts it.
TEST(ProjectStore, TheFirstPaneIsTheFirstLeafOfTheFirstTab)
{
    QJsonObject first{
        {QStringLiteral("id"), QStringLiteral("first")},
        {QStringLiteral("cwd"), QStringLiteral("/")},
        {QStringLiteral("title"), QStringLiteral("")},
        {QStringLiteral("command"), QJsonArray{QStringLiteral("/no/such/program"), QStringLiteral("-x")}}};
    QJsonObject split{{QStringLiteral("direction"), QStringLiteral("down")},
                      {QStringLiteral("ratio"), 0.5},
                      {QStringLiteral("first"), QJsonObject{{QStringLiteral("pane"), first}}},
                      {QStringLiteral("second"), pane(QStringLiteral("second"))}};
    auto doc = project(split);
    // The active pane is not the first: the first is the first leaf.
    QJsonArray tabs = doc[QStringLiteral("tabs")].toArray();
    QJsonObject tab = tabs[0].toObject();
    tab[QStringLiteral("active_pane")] = QStringLiteral("second");
    tabs[0] = tab;
    doc[QStringLiteral("tabs")] = tabs;
    EXPECT_EQ(ProjectStore::panes(doc).value(0), first);
    QStringList warnings;
    const ProjectStore::Launch launch = ProjectStore::launchOf(first, warnings);
    EXPECT_EQ(launch.cwd, QStringLiteral("/"));
    // A command that is not there starts a shell, and the project keeps it.
    EXPECT_TRUE(launch.command.isEmpty());
    EXPECT_EQ(launch.configured, (QStringList{QStringLiteral("/no/such/program"), QStringLiteral("-x")}));
    ASSERT_EQ(warnings.size(), 1);
    EXPECT_TRUE(warnings.first().contains(QLatin1String("command unavailable"))) << warnings.first().toStdString();
    // A directory that is not there is the home directory.
    first[QStringLiteral("cwd")] = QStringLiteral("/no/such/directory");
    first[QStringLiteral("command")] = QJsonArray{};
    warnings.clear();
    EXPECT_EQ(ProjectStore::launchOf(first, warnings).cwd, QDir::homePath());
    EXPECT_EQ(warnings.size(), 1);
}

TEST(ProjectStore, BoundsTreesBeforeAnyPaneIsCreated)
{
    auto root = pane();
    for (int i = 0; i < 17; ++i)
        root = {{QStringLiteral("direction"), QStringLiteral("down")},
                {QStringLiteral("ratio"), 0.5},
                {QStringLiteral("first"), root},
                {QStringLiteral("second"), pane(QString::number(i))}};
    EXPECT_FALSE(ProjectStore::validate(project(root)).isEmpty());
    auto doc = project();
    auto tabs = doc[QStringLiteral("tabs")].toArray();
    tabs.append(tabs.first());
    doc[QStringLiteral("tabs")] = tabs;
    EXPECT_FALSE(ProjectStore::validate(doc).isEmpty());
}
TEST(ProjectStore, BoundsAcceptTheLimitsAndRejectOnePast)
{
    // Depth 16 is accepted, 17 is not.
    const auto chain = [](int depth) {
        auto root = pane();
        for (int i = 0; i < depth; ++i)
            root = {{QStringLiteral("direction"), QStringLiteral("down")},
                    {QStringLiteral("ratio"), 0.5},
                    {QStringLiteral("first"), root},
                    {QStringLiteral("second"), pane(QString::number(i))}};
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
                    next.append(QJsonObject{{QStringLiteral("direction"), QStringLiteral("right")},
                                            {QStringLiteral("ratio"), 0.5},
                                            {QStringLiteral("first"), level[i]},
                                            {QStringLiteral("second"), level[i + 1]}});
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
            list.append(QJsonObject{{QStringLiteral("id"), id},
                                    {QStringLiteral("title"), id},
                                    {QStringLiteral("active_pane"), id},
                                    {QStringLiteral("tree"), pane(id)}});
        }
        return QJsonObject{{QStringLiteral("version"), 1},
                           {QStringLiteral("active_tab"), QStringLiteral("0")},
                           {QStringLiteral("tabs"), list}};
    };
    EXPECT_TRUE(ProjectStore::validate(tabs(16)).isEmpty());
    EXPECT_FALSE(ProjectStore::validate(tabs(17)).isEmpty());
    // A repeated tab id alone, then a repeated pane id alone.
    auto doc = tabs(2);
    auto list = doc[QStringLiteral("tabs")].toArray();
    auto second = list[1].toObject();
    second[QStringLiteral("id")] = QStringLiteral("0");
    list[1] = second;
    doc[QStringLiteral("tabs")] = list;
    EXPECT_FALSE(ProjectStore::validate(doc).isEmpty());
    doc = tabs(2);
    list = doc[QStringLiteral("tabs")].toArray();
    second = list[1].toObject();
    second[QStringLiteral("tree")] = pane(QStringLiteral("0"));
    second[QStringLiteral("active_pane")] = QStringLiteral("0");
    list[1] = second;
    doc[QStringLiteral("tabs")] = list;
    EXPECT_FALSE(ProjectStore::validate(doc).isEmpty());
}
TEST(ProjectStore, EachLimitNamesItselfInTheError)
{
    const auto chain = [](int depth) {
        auto root = pane();
        for (int i = 0; i < depth; ++i)
            root = {{QStringLiteral("direction"), QStringLiteral("down")},
                    {QStringLiteral("ratio"), 0.5},
                    {QStringLiteral("first"), root},
                    {QStringLiteral("second"), pane(QString::number(i))}};
        return root;
    };
    EXPECT_TRUE(ProjectStore::validate(project(chain(17))).contains(QStringLiteral("16 levels")));
    // The count of the window, with the limit.
    EXPECT_TRUE(ProjectStore::validate(project(chain(17))).contains(QStringLiteral("17 levels deep")));
    // 33 panes in a tree of depth 6, so only the pane count is over.
    QJsonArray level;
    for (int i = 0; i < 33; ++i)
        level.append(i == 0 ? pane() : pane(QString::number(i)));
    while (level.size() > 1) {
        QJsonArray next;
        for (qsizetype i = 0; i < level.size(); i += 2) {
            if (i + 1 < level.size())
                next.append(QJsonObject{{QStringLiteral("direction"), QStringLiteral("right")},
                                        {QStringLiteral("ratio"), 0.5},
                                        {QStringLiteral("first"), level[i]},
                                        {QStringLiteral("second"), level[i + 1]}});
            else
                next.append(level[i]);
        }
        level = next;
    }
    EXPECT_TRUE(ProjectStore::validate(project(level.first().toObject())).contains(QStringLiteral("32 panes")));
    EXPECT_TRUE(ProjectStore::validate(project(level.first().toObject())).contains(QStringLiteral("this one has 33")));
    QJsonArray tabList;
    for (int i = 0; i < 17; ++i) {
        const QString id = QString::number(i);
        tabList.append(QJsonObject{{QStringLiteral("id"), id},
                                   {QStringLiteral("title"), id},
                                   {QStringLiteral("active_pane"), id},
                                   {QStringLiteral("tree"), pane(id)}});
    }
    EXPECT_TRUE(ProjectStore::validate(QJsonObject{{QStringLiteral("version"), 1},
                                                   {QStringLiteral("active_tab"), QStringLiteral("0")},
                                                   {QStringLiteral("tabs"), tabList}})
                    .contains(QStringLiteral("this one has 17")));
    QJsonObject split{{QStringLiteral("direction"), QStringLiteral("right")},
                      {QStringLiteral("ratio"), 0.3},
                      {QStringLiteral("first"), pane()},
                      {QStringLiteral("second"), pane()}};
    EXPECT_TRUE(ProjectStore::validate(project(split)).contains(QStringLiteral("\"shell\" is used more than once")));
    auto p = pane()[QStringLiteral("pane")].toObject();
    QJsonArray many;
    for (int i = 0; i < 65; ++i)
        many.append(QStringLiteral("x"));
    p[QStringLiteral("command")] = many;
    EXPECT_TRUE(
        ProjectStore::validate(project({{QStringLiteral("pane"), p}})).contains(QStringLiteral("64 arguments")));
    p[QStringLiteral("command")] = QJsonArray{QStringLiteral("sh"), QString(4097, QLatin1Char('a'))};
    EXPECT_TRUE(ProjectStore::validate(project({{QStringLiteral("pane"), p}})).contains(QStringLiteral("argument")));
    p = pane()[QStringLiteral("pane")].toObject();
    p[QStringLiteral("title")] = QString(257, QLatin1Char('t'));
    EXPECT_TRUE(ProjectStore::validate(project({{QStringLiteral("pane"), p}})).contains(QStringLiteral("pane title")));
    auto doc = project();
    auto tabs = doc[QStringLiteral("tabs")].toArray();
    auto tab = tabs[0].toObject();
    tab[QStringLiteral("active_pane")] = QStringLiteral("elsewhere");
    tabs[0] = tab;
    doc[QStringLiteral("tabs")] = tabs;
    EXPECT_TRUE(ProjectStore::validate(doc).contains(QStringLiteral("active pane")));
}

// The panes of a project in the order that a window lays them out: tab by
// tab, each tree's first side before its second. A session starts them in
// this order before its window shows them.
TEST(ProjectStore, ThePanesComeInTheOrderOfTheLayout)
{
    const QJsonObject inner{{QStringLiteral("direction"), QStringLiteral("right")},
                            {QStringLiteral("ratio"), 0.5},
                            {QStringLiteral("first"), pane(QStringLiteral("b"))},
                            {QStringLiteral("second"), pane(QStringLiteral("c"))}};
    const QJsonObject outer{{QStringLiteral("direction"), QStringLiteral("down")},
                            {QStringLiteral("ratio"), 0.5},
                            {QStringLiteral("first"), pane(QStringLiteral("a"))},
                            {QStringLiteral("second"), inner}};
    auto doc = project(outer);
    QJsonArray tabs = doc[QStringLiteral("tabs")].toArray();
    QJsonObject main = tabs[0].toObject();
    // Neither the active tab nor the active pane comes first.
    main[QStringLiteral("active_pane")] = QStringLiteral("c");
    tabs[0] = main;
    tabs.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("logs")},
                            {QStringLiteral("title"), QStringLiteral("")},
                            {QStringLiteral("active_pane"), QStringLiteral("d")},
                            {QStringLiteral("tree"), pane(QStringLiteral("d"))}});
    doc[QStringLiteral("tabs")] = tabs;
    doc[QStringLiteral("active_tab")] = QStringLiteral("logs");
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
    auto p = pane()[QStringLiteral("pane")].toObject();
    p[QStringLiteral("command")] = QJsonArray{QStringLiteral("sh"), accented};
    EXPECT_TRUE(ProjectStore::validate(project({{QStringLiteral("pane"), p}})).contains(QStringLiteral("bytes")))
        << ProjectStore::validate(project({{QStringLiteral("pane"), p}})).toStdString();
}

// The directory of a pane with ~, and the program of its command: one with a
// slash against the pane's directory, a bare name on the search path.
TEST(ProjectStore, LaunchOfExpandsTheHomeAndFindsTheProgram)
{
    QTemporaryDir home;
    ASSERT_TRUE(home.isValid());
    ASSERT_TRUE(QDir(home.path()).mkpath(QStringLiteral("sub")));
    const Afterglow::Test::Saved savedHome("HOME", QFile::encodeName(home.path()));
    QJsonObject p{{QStringLiteral("id"), QStringLiteral("p")},
                  {QStringLiteral("cwd"), QStringLiteral("~")},
                  {QStringLiteral("title"), QStringLiteral("")},
                  {QStringLiteral("command"), QJsonArray{}}};
    QStringList warnings;
    EXPECT_EQ(ProjectStore::launchOf(p, warnings).cwd, QDir::homePath());
    EXPECT_EQ(QDir::homePath(), home.path());
    p[QStringLiteral("cwd")] = QStringLiteral("~/sub");
    EXPECT_EQ(ProjectStore::launchOf(p, warnings).cwd, home.path() + QStringLiteral("/sub"));
    EXPECT_TRUE(warnings.isEmpty()) << warnings.join(QLatin1Char('\n')).toStdString();
    // A program on PATH stays.
    p[QStringLiteral("command")] = QJsonArray{QStringLiteral("sh"), QStringLiteral("-c"), QStringLiteral("true")};
    EXPECT_EQ(ProjectStore::launchOf(p, warnings).command,
              (QStringList{QStringLiteral("sh"), QStringLiteral("-c"), QStringLiteral("true")}));
    // ./x is the pane's own.
    QFile tool(home.filePath(QStringLiteral("sub/x")));
    ASSERT_TRUE(tool.open(QIODevice::WriteOnly));
    tool.write("#!/bin/sh\n");
    tool.close();
    ASSERT_TRUE(tool.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    p[QStringLiteral("command")] = QJsonArray{QStringLiteral("./x")};
    EXPECT_EQ(ProjectStore::launchOf(p, warnings).command, QStringList{QStringLiteral("./x")});
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
    const QJsonObject p{{QStringLiteral("id"), QStringLiteral("p")},
                        {QStringLiteral("cwd"), dir.path()},
                        {QStringLiteral("title"), QStringLiteral("")},
                        {QStringLiteral("command"), QJsonArray{QStringLiteral("agtool-x")}}};
    // Not on the PATH of this process.
    QStringList warnings;
    EXPECT_TRUE(ProjectStore::launchOf(p, warnings).command.isEmpty());
    ASSERT_EQ(warnings.size(), 1);
    EXPECT_TRUE(warnings.first().contains(QLatin1String("command unavailable"))) << warnings.first().toStdString();
    warnings.clear();
    EXPECT_EQ(ProjectStore::launchOf(p, warnings, {dir.path()}).command, QStringList{QStringLiteral("agtool-x")});
    EXPECT_TRUE(warnings.isEmpty());
    // A relative entry, in the pane's directory.
    EXPECT_EQ(ProjectStore::launchOf(p, warnings, {QStringLiteral("bin")}).command,
              QStringList{QStringLiteral("agtool-x")});
    EXPECT_TRUE(warnings.isEmpty());
}
