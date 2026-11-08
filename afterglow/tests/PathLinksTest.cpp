// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "config/Settings.h"
#include "desktop/PathLinks.h"

#include <AfterglowVt/TextSearch.h>

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <gtest/gtest.h>

#include <atomic>
#include <optional>

using namespace Afterglow;
using PathLinks::Kind;
using PathLinks::Location;

namespace {

/// The candidates of @p text as the text and the location of each.
std::vector<std::pair<QString, Location>> found(const QString& text)
{
    std::vector<std::pair<QString, Location>> out;
    for (const PathLinks::Candidate& c : PathLinks::find(text)) out.emplace_back(text.mid(c.start, c.length), c.location);
    return out;
}

/// The one candidate of @p text, if it has exactly one.
std::optional<QString> only(const QString& text)
{
    const auto all = found(text);
    if (all.size() != 1) return std::nullopt;
    return all.front().first;
}

TEST(PathLinks, FindsAbsoluteHomeAndDotPaths)
{
    EXPECT_EQ(only(QStringLiteral("open /usr/lib/libc.so.6 now")), QStringLiteral("/usr/lib/libc.so.6"));
    EXPECT_EQ(only(QStringLiteral("cat /etc/hosts")), QStringLiteral("/etc/hosts"));
    EXPECT_EQ(only(QStringLiteral("ls /usr/share/")), QStringLiteral("/usr/share/"));
    EXPECT_EQ(only(QStringLiteral("edit ~/notes/todo")), QStringLiteral("~/notes/todo"));
    EXPECT_EQ(only(QStringLiteral("run ./build.sh")), QStringLiteral("./build.sh"));
    EXPECT_EQ(only(QStringLiteral("see ../lib/x")), QStringLiteral("../lib/x"));
    EXPECT_EQ(only(QStringLiteral("dir ~/")), std::nullopt) << "the home directory alone is no path link";
    EXPECT_EQ(only(QStringLiteral("a / b")), std::nullopt);
}

TEST(PathLinks, FindsRelativePathsWithASlashOrAnExtension)
{
    EXPECT_EQ(only(QStringLiteral("M src/main.cpp")), QStringLiteral("src/main.cpp"));
    EXPECT_EQ(only(QStringLiteral("in docs/ now")), QStringLiteral("docs/"));
    EXPECT_EQ(only(QStringLiteral("read README.md first")), QStringLiteral("README.md"));
    EXPECT_EQ(only(QStringLiteral("unpack data.tar.gz")), QStringLiteral("data.tar.gz"));
    EXPECT_EQ(only(QStringLiteral("cat .bashrc")), QStringLiteral(".bashrc"));
    EXPECT_EQ(only(QStringLiteral("die Datei grüße.txt")), QStringLiteral("grüße.txt"));
    const auto two = found(QStringLiteral("a.c b/c"));
    ASSERT_EQ(two.size(), 2u);
    EXPECT_EQ(two[0].first, QStringLiteral("a.c"));
    EXPECT_EQ(two[1].first, QStringLiteral("b/c"));
}

TEST(PathLinks, FindsLinesAndColumns)
{
    const auto all = found(QStringLiteral("src/main.cpp:12:5: error, Makefile:3: and a.c:7"));
    ASSERT_EQ(all.size(), 3u);
    EXPECT_EQ(all[0].first, QStringLiteral("src/main.cpp:12:5"));
    EXPECT_EQ(all[0].second, (Location{QStringLiteral("src/main.cpp"), 12, 5}));
    EXPECT_EQ(all[1].first, QStringLiteral("Makefile:3"));
    EXPECT_EQ(all[1].second, (Location{QStringLiteral("Makefile"), 3, 0}));
    EXPECT_EQ(all[2].second, (Location{QStringLiteral("a.c"), 7, 0}));
    EXPECT_EQ(PathLinks::parseLocation(QStringLiteral("/x/y")), (Location{QStringLiteral("/x/y"), 0, 0}));
    EXPECT_EQ(PathLinks::parseLocation(QStringLiteral("a.c:99999999999")), (Location{QStringLiteral("a.c"), 0, 0}))
        << "a line past int is no line";
}

TEST(PathLinks, LeavesTrailingPunctuationOut)
{
    EXPECT_EQ(only(QStringLiteral("See /etc/hosts.")), QStringLiteral("/etc/hosts"));
    EXPECT_EQ(only(QStringLiteral("(in src/main.cpp)")), QStringLiteral("src/main.cpp"));
    EXPECT_EQ(only(QStringLiteral("files: a.txt,")), QStringLiteral("a.txt"));
    EXPECT_EQ(only(QStringLiteral("\"~/x/y\";")), QStringLiteral("~/x/y"));
    EXPECT_EQ(only(QStringLiteral("at main.c:4:")), QStringLiteral("main.c:4"));
    EXPECT_EQ(only(QStringLiteral("is it ./run.sh?")), QStringLiteral("./run.sh"));
    EXPECT_EQ(only(QStringLiteral("the end of lib/.")), QStringLiteral("lib/"));
}

TEST(PathLinks, WordsAreNoPaths)
{
    for (const char* text : {"hello world", "version 1.5 and 2.0.1", "it is 12:30", "std::vector", "...", "--", "~"}) {
        EXPECT_TRUE(found(QString::fromUtf8(text)).empty()) << text;
    }
}

TEST(PathLinks, ResolvesFromTheDirectoryAndHome)
{
    const QString home = QStringLiteral("/home/someone");
    EXPECT_EQ(PathLinks::resolve(QStringLiteral("src/a.c"), QStringLiteral("/work"), home), QStringLiteral("/work/src/a.c"));
    EXPECT_EQ(PathLinks::resolve(QStringLiteral("./a.c"), QStringLiteral("/work"), home), QStringLiteral("/work/a.c"));
    EXPECT_EQ(PathLinks::resolve(QStringLiteral("../a.c"), QStringLiteral("/work/sub"), home), QStringLiteral("/work/a.c"));
    EXPECT_EQ(PathLinks::resolve(QStringLiteral("~/a.c"), QStringLiteral("/work"), home), QStringLiteral("/home/someone/a.c"));
    EXPECT_EQ(PathLinks::resolve(QStringLiteral("/etc//hosts"), QStringLiteral("/work"), home), QStringLiteral("/etc/hosts"));
    EXPECT_EQ(PathLinks::resolve(QStringLiteral("/etc/hosts"), QString(), home), QStringLiteral("/etc/hosts"));
    EXPECT_TRUE(PathLinks::resolve(QStringLiteral("a.c"), QString(), home).isEmpty()) << "no directory to start from";
    EXPECT_TRUE(PathLinks::resolve(QStringLiteral("a.c"), QStringLiteral("work"), home).isEmpty());
}

TEST(PathLinks, OpensWithTheCommandOrTheFileUrl)
{
    const QStringList command{QStringLiteral("editor"), QStringLiteral("+{line}"), QStringLiteral("--column={column}"),
                              QStringLiteral("{file}"), QStringLiteral("{other}")};
    const auto withLine = PathLinks::openAction(command, {QStringLiteral("/w/a.c"), 12, 5}, false);
    EXPECT_EQ(withLine.command, (QStringList{QStringLiteral("editor"), QStringLiteral("+12"), QStringLiteral("--column=5"),
                                             QStringLiteral("/w/a.c"), QStringLiteral("{other}")}));
    EXPECT_TRUE(withLine.url.isEmpty());
    // No line or column in the text: each is 1.
    const auto withoutLine = PathLinks::openAction(command, {QStringLiteral("/w/a.c"), 0, 0}, false);
    EXPECT_EQ(withoutLine.command, (QStringList{QStringLiteral("editor"), QStringLiteral("+1"), QStringLiteral("--column=1"),
                                                QStringLiteral("/w/a.c"), QStringLiteral("{other}")}));
    const auto lineOnly = PathLinks::openAction({QStringLiteral("code"), QStringLiteral("--goto"),
                                                 QStringLiteral("{file}:{line}:{column}")},
                                                {QStringLiteral("/w/a.c"), 7, 0}, false);
    EXPECT_EQ(lineOnly.command.last(), QStringLiteral("/w/a.c:7:1"));
    // A placeholder that a path brings in stays text.
    const auto braces = PathLinks::openAction({QStringLiteral("e"), QStringLiteral("{file}")},
                                              {QStringLiteral("/w/{line}.c"), 3, 0}, false);
    EXPECT_EQ(braces.command.last(), QStringLiteral("/w/{line}.c"));
    // No command: the file:// URL, as other links open.
    const auto empty = PathLinks::openAction({}, {QStringLiteral("/w/a b.c"), 3, 0}, false);
    EXPECT_TRUE(empty.command.isEmpty());
    EXPECT_EQ(empty.url, QUrl(QStringLiteral("file:///w/a%20b.c")));
    // A directory opens in the file manager, also with a command.
    const auto directory = PathLinks::openAction(command, {QStringLiteral("/w"), 0, 0}, true);
    EXPECT_TRUE(directory.command.isEmpty());
    EXPECT_EQ(directory.url, QUrl::fromLocalFile(QStringLiteral("/w")));
}

TEST(PathLinks, TheDefaultHintPatternIsThePathPattern)
{
    const QVariantList hints = defaultHintPatterns();
    ASSERT_EQ(hints.size(), 1);
    EXPECT_EQ(hints.first().toMap().value(QStringLiteral("pattern")).toString(), PathLinks::pattern());
    EXPECT_EQ(hints.first().toMap().value(QStringLiteral("action")).toString(), QStringLiteral("copy"));
    // The hint search compiles it too, and matches as the mouse does.
    AfterglowVt::TextSnapshot text;
    AfterglowVt::TextLine line;
    line.text = U"at src/main.cpp:12:5.";
    for (std::size_t i = 0; i < line.text.size(); ++i) line.cells.push_back({0, std::int32_t(i), std::int32_t(i)});
    text.characters = line.text.size();
    text.lines.push_back(line);
    const auto result = AfterglowVt::searchText(text, PathLinks::pattern().toStdU32String(), true, std::atomic<bool>{false});
    ASSERT_TRUE(result.error.empty()) << result.error;
    ASSERT_EQ(result.matches.size(), 1u);
    EXPECT_EQ(result.matches[0].start.col, 3);
    EXPECT_EQ(result.matches[0].end.col, 19);
}

class PathChecker : public ::testing::Test {
protected:
    void SetUp() override
    {
        // Kept to the end of the process: a check that a test leaves running
        // posts its answer to the application.
        static int argc = 1;
        static char name[] = "path-links-test";
        static char* argv[] = {name, nullptr};
        if (!QCoreApplication::instance()) new QCoreApplication(argc, argv);
        ASSERT_TRUE(directory.isValid());
        QFile file(directory.filePath(QStringLiteral("a.txt")));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_TRUE(QDir(directory.path()).mkdir(QStringLiteral("sub")));
    }
    /// The kind of @p path, after its check.
    Kind kindOf(PathLinks::Checker& checker, const QString& path)
    {
        std::optional<Kind> kind;
        QObject context;
        checker.check(path, &context, [&](Kind k) { kind = k; });
        QElapsedTimer timer;
        timer.start();
        while (!kind && timer.elapsed() < 5000) {
            QCoreApplication::processEvents();
            QThread::msleep(1);
        }
        return kind.value_or(Kind::Unknown);
    }
    QTemporaryDir directory;
};

TEST_F(PathChecker, FindsFilesAndDirectoriesAndNotMissingPaths)
{
    PathLinks::Checker checker;
    EXPECT_EQ(kindOf(checker, directory.filePath(QStringLiteral("a.txt"))), Kind::File);
    EXPECT_EQ(kindOf(checker, directory.filePath(QStringLiteral("sub"))), Kind::Directory);
    EXPECT_EQ(kindOf(checker, directory.filePath(QStringLiteral("missing.txt"))), Kind::Missing);
}

TEST_F(PathChecker, LookupStartsACheckAndKeepsItsAnswer)
{
    PathLinks::Checker checker;
    const QString path = directory.filePath(QStringLiteral("a.txt"));
    QStringList checked;
    QObject::connect(&checker, &PathLinks::Checker::checked, [&](const QString& p) { checked << p; });
    // Never an answer on the calling thread: a stat could hang there.
    EXPECT_EQ(checker.lookup(path), Kind::Unknown);
    EXPECT_EQ(checker.lookup(path), Kind::Unknown);
    EXPECT_EQ(checker.pending(), 1) << "one check for each path";
    QElapsedTimer timer;
    timer.start();
    while (checked.isEmpty() && timer.elapsed() < 5000) QCoreApplication::processEvents();
    EXPECT_EQ(checked, QStringList{path});
    EXPECT_EQ(checker.lookup(path), Kind::File);
    EXPECT_EQ(checker.pending(), 0);
    // The answer is kept: a file removed now still counts until it expires.
    ASSERT_TRUE(QFile::remove(path));
    EXPECT_EQ(checker.lookup(path), Kind::File);
}

TEST_F(PathChecker, AnswersExpireAndTheCacheIsBounded)
{
    PathLinks::Checker brief(nullptr, std::chrono::milliseconds(0));
    const QString path = directory.filePath(QStringLiteral("a.txt"));
    EXPECT_EQ(kindOf(brief, path), Kind::File);
    EXPECT_EQ(brief.lookup(path), Kind::Unknown) << "an expired answer is checked again";
    EXPECT_EQ(brief.pending(), 1);
    QElapsedTimer timer;
    timer.start();
    while (brief.pending() > 0 && timer.elapsed() < 5000) QCoreApplication::processEvents();

    PathLinks::Checker small(nullptr, std::chrono::seconds(60), 2);
    const QString a = directory.filePath(QStringLiteral("a.txt"));
    const QString sub = directory.filePath(QStringLiteral("sub"));
    const QString missing = directory.filePath(QStringLiteral("missing"));
    EXPECT_EQ(kindOf(small, a), Kind::File);
    EXPECT_EQ(kindOf(small, sub), Kind::Directory);
    EXPECT_EQ(kindOf(small, missing), Kind::Missing);
    EXPECT_EQ(small.lookup(a), Kind::Unknown) << "the oldest answer made room";
    EXPECT_EQ(small.lookup(missing), Kind::Missing);
}

TEST_F(PathChecker, ACallWhoseContextIsGoneIsNotMade)
{
    PathLinks::Checker checker;
    bool called = false;
    {
        QObject context;
        checker.check(directory.filePath(QStringLiteral("a.txt")), &context, [&](Kind) { called = true; });
    }
    QElapsedTimer timer;
    timer.start();
    while (checker.pending() > 0 && timer.elapsed() < 5000) QCoreApplication::processEvents();
    EXPECT_EQ(checker.pending(), 0);
    EXPECT_FALSE(called);
}

} // namespace
