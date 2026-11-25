// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#include "desktop/Download.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <sys/resource.h>
#include <unistd.h>

using Afterglow::DownloadFile;

namespace {

/// What a download reported at its end, from its writer thread.
struct Ending
{
    std::mutex mutex;
    std::condition_variable done;
    bool ended = false;
    QString path;
    bool ok = false;
    QString error;

    DownloadFile::Ended callback()
    {
        return [this](const QString& p, bool o, const QString& e) {
            std::lock_guard lock(mutex);
            ended = true;
            path = p;
            ok = o;
            error = e;
            done.notify_all();
        };
    }
    bool wait()
    {
        std::unique_lock lock(mutex);
        return done.wait_for(lock, std::chrono::seconds(10), [this] {
            return ended;
        });
    }
};

QByteArray contentOf(const QString& path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

void writeFile(const QString& path, const QByteArray& content)
{
    QFile f(path);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write(content);
}

/// Every name in @p dir.
QStringList namesIn(const QTemporaryDir& dir)
{
    return QDir(dir.path()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
}

TEST(DownloadFile, AWholeDownloadBecomesItsFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString target = dir.filePath(QStringLiteral("got.txt"));
    Ending ending;
    QString error;
    auto file = DownloadFile::create(target, false, {}, ending.callback(), error);
    ASSERT_NE(file, nullptr) << error.toStdString();
    // It streams to a part file beside the target.
    EXPECT_EQ(namesIn(dir), QStringList{QStringLiteral("got.txt.part")});
    EXPECT_TRUE(file->write("one "));
    EXPECT_TRUE(file->write("two"));
    file->finish(true, QStringLiteral("unused"));
    ASSERT_TRUE(ending.wait());
    EXPECT_TRUE(ending.ok) << ending.error.toStdString();
    EXPECT_EQ(ending.path, target);
    EXPECT_EQ(contentOf(target), QByteArray("one two"));
    EXPECT_EQ(namesIn(dir), QStringList{QStringLiteral("got.txt")});
}

TEST(DownloadFile, ADownloadThatDidNotEndWholeLosesItsPartFile)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Ending ending;
    QString error;
    auto file = DownloadFile::create(dir.filePath(QStringLiteral("got.txt")), false, {}, ending.callback(), error);
    ASSERT_NE(file, nullptr) << error.toStdString();
    EXPECT_TRUE(file->write("half"));
    file->finish(false, QStringLiteral("the program did not send it all"));
    ASSERT_TRUE(ending.wait());
    EXPECT_FALSE(ending.ok);
    EXPECT_EQ(ending.error, QStringLiteral("the program did not send it all"));
    EXPECT_TRUE(namesIn(dir).isEmpty()) << namesIn(dir).join(QLatin1Char(',')).toStdString();
}

TEST(DownloadFile, ACancelRemovesThePartFileAndReportsNothing)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Ending ending;
    QString error;
    auto file = DownloadFile::create(dir.filePath(QStringLiteral("got.txt")), false, {}, ending.callback(), error);
    ASSERT_NE(file, nullptr) << error.toStdString();
    EXPECT_TRUE(file->write("half"));
    file.reset();
    std::lock_guard lock(ending.mutex);
    EXPECT_FALSE(ending.ended);
    EXPECT_TRUE(namesIn(dir).isEmpty()) << namesIn(dir).join(QLatin1Char(',')).toStdString();
}

TEST(DownloadFile, AReleasedDownloadEndsWithoutItsOwnerAndReportsNothingAfter)
{
    // A pane that closes releases its downloads: the one that streams loses
    // its part file, the one that finished gets its name, and neither calls
    // the pane that went.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString target = dir.filePath(QStringLiteral("whole.txt"));
    std::atomic<int> calls{0};
    const auto count = [&calls](const QString&, bool, const QString&) {
        ++calls;
    };
    QString error;
    auto streaming = DownloadFile::create(
        dir.filePath(QStringLiteral("half.txt")), false,
        [&calls] {
            ++calls;
        },
        count, error);
    ASSERT_NE(streaming, nullptr) << error.toStdString();
    auto finished = DownloadFile::create(
        target, false,
        [&calls] {
            ++calls;
        },
        count, error);
    ASSERT_NE(finished, nullptr) << error.toStdString();
    EXPECT_TRUE(streaming->write("half"));
    EXPECT_TRUE(finished->write("whole"));
    finished->finish(true, {});
    DownloadFile::release(std::move(streaming));
    DownloadFile::release(std::move(finished));
    // The finished one may have reported before its release, never after.
    const int reported = calls.load();
    EXPECT_LE(reported, 1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (namesIn(dir) != QStringList{QStringLiteral("whole.txt")} && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_EQ(namesIn(dir), QStringList{QStringLiteral("whole.txt")})
        << namesIn(dir).join(QLatin1Char(',')).toStdString();
    EXPECT_EQ(contentOf(target), QByteArray("whole"));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(calls.load(), reported);
}

TEST(DownloadFile, ANameThatIsTakenGivesTheNextNumberedName)
{
    // A file that came to the name while the download streamed stays.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString target = dir.filePath(QStringLiteral("got.txt"));
    Ending ending;
    QString error;
    auto file = DownloadFile::create(target, false, {}, ending.callback(), error);
    ASSERT_NE(file, nullptr) << error.toStdString();
    writeFile(target, "mine");
    EXPECT_TRUE(file->write("theirs"));
    file->finish(true, {});
    ASSERT_TRUE(ending.wait());
    EXPECT_TRUE(ending.ok) << ending.error.toStdString();
    EXPECT_EQ(ending.path, dir.filePath(QStringLiteral("got (1).txt")));
    EXPECT_EQ(contentOf(target), QByteArray("mine"));
    EXPECT_EQ(contentOf(ending.path), QByteArray("theirs"));
}

TEST(DownloadFile, WithoutNoReplaceTheRenameTakesAFreeNameAlike)
{
    // A file system without RENAME_NOREPLACE: link(2), then one without
    // hard links either, which looks at the name before the rename. Both
    // leave a file at the name, and take the next free one.
    for (const bool link : {true, false}) {
        SCOPED_TRACE(link ? "link" : "no link");
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        const QString target = dir.filePath(QStringLiteral("got.txt"));
        const QString part = dir.filePath(QStringLiteral("got.txt.part"));
        writeFile(target, "mine");
        writeFile(dir.filePath(QStringLiteral("got (1).txt")), "mine too");
        writeFile(part, "theirs");
        const QString renamed = Afterglow::renameToFreeName(QFile::encodeName(part), target, {false, link});
        EXPECT_EQ(renamed, dir.filePath(QStringLiteral("got (2).txt")));
        EXPECT_EQ(contentOf(target), QByteArray("mine"));
        EXPECT_EQ(contentOf(dir.filePath(QStringLiteral("got (1).txt"))), QByteArray("mine too"));
        EXPECT_EQ(contentOf(renamed), QByteArray("theirs"));
        EXPECT_FALSE(QFileInfo::exists(part));
        // A free name at once.
        writeFile(part, "again");
        const QString other = dir.filePath(QStringLiteral("other.txt"));
        EXPECT_EQ(Afterglow::renameToFreeName(QFile::encodeName(part), other, {false, link}), other);
        EXPECT_EQ(contentOf(other), QByteArray("again"));
        EXPECT_FALSE(QFileInfo::exists(part));
    }
}

TEST(DownloadFile, ARenameThatFailsKeepsTheWholeDownload)
{
    // A replace over a directory fails: the download is whole, and stays in
    // its part file, which the error names.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString target = dir.filePath(QStringLiteral("got"));
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("got/inside")));
    Ending ending;
    QString error;
    auto file = DownloadFile::create(target, true, {}, ending.callback(), error);
    ASSERT_NE(file, nullptr) << error.toStdString();
    EXPECT_TRUE(file->write("whole"));
    file->finish(true, {});
    ASSERT_TRUE(ending.wait());
    EXPECT_FALSE(ending.ok);
    const QString part = dir.filePath(QStringLiteral("got.part"));
    EXPECT_TRUE(ending.error.contains(part)) << ending.error.toStdString();
    EXPECT_EQ(contentOf(part), QByteArray("whole"));
}

TEST(DownloadFile, ARenameToAFreeNameThatFailsKeepsTheWholeDownload)
{
    if (::geteuid() == 0)
        GTEST_SKIP() << "root writes in a directory without write permission";
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Ending ending;
    QString error;
    auto file = DownloadFile::create(dir.filePath(QStringLiteral("got.txt")), false, {}, ending.callback(), error);
    ASSERT_NE(file, nullptr) << error.toStdString();
    EXPECT_TRUE(file->write("whole"));
    // No rename in the directory any more.
    ASSERT_TRUE(QFile::setPermissions(dir.path(), QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    file->finish(true, {});
    const bool ended = ending.wait();
    QFile::setPermissions(dir.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    ASSERT_TRUE(ended);
    EXPECT_FALSE(ending.ok);
    const QString part = dir.filePath(QStringLiteral("got.txt.part"));
    EXPECT_TRUE(ending.error.contains(part)) << ending.error.toStdString();
    EXPECT_EQ(contentOf(part), QByteArray("whole"));
}

TEST(DownloadFile, AFullQueuePausesTheProducerUntilHalfOfItIsWritten)
{
    // Chunks of more than the queue holds: the queue counts a chunk until
    // all of it is written, so the queue is full when the producer looks,
    // unless the writer wrote the whole chunk first. Then the producer
    // writes another.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString target = dir.filePath(QStringLiteral("big.bin"));
    std::atomic<int> rooms{0};
    Ending ending;
    QString error;
    auto file = DownloadFile::create(
        target, false,
        [&rooms] {
            ++rooms;
        },
        ending.callback(), error);
    ASSERT_NE(file, nullptr) << error.toStdString();
    const std::string chunk(DownloadFile::MaxQueued + 1, 'x');
    bool paused = false;
    int chunks = 0;
    while (!paused && chunks < 10) {
        EXPECT_TRUE(file->write(chunk));
        ++chunks;
        EXPECT_EQ(file->pauseIfFull([&paused] {
            paused = true;
        }),
                  paused);
    }
    ASSERT_TRUE(paused);
    // room() comes once, when half the queue is written.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (rooms.load() == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_EQ(rooms.load(), 1);
    file->finish(true, {});
    ASSERT_TRUE(ending.wait());
    EXPECT_TRUE(ending.ok) << ending.error.toStdString();
    EXPECT_FALSE(file->pauseIfFull([] { }));
    EXPECT_EQ(QFileInfo(target).size(), qint64(chunks) * qint64(chunk.size()));
    EXPECT_EQ(rooms.load(), 1);
}

TEST(DownloadFile, AWriteThatFailsLosesTheDownload)
{
    // The process may write files of 4 KiB at most: the write fails with
    // EFBIG (SIGXFSZ ignored), the next writes are refused, and the end says
    // why, whatever the producer's reason.
    struct rlimit before{};
    ASSERT_EQ(::getrlimit(RLIMIT_FSIZE, &before), 0);
    const auto oldHandler = std::signal(SIGXFSZ, SIG_IGN);
    struct rlimit small = before;
    small.rlim_cur = 4096;
    ASSERT_EQ(::setrlimit(RLIMIT_FSIZE, &small), 0);
    struct Restore
    {
        rlimit limit;
        void (*handler)(int);
        ~Restore()
        {
            ::setrlimit(RLIMIT_FSIZE, &limit);
            std::signal(SIGXFSZ, handler);
        }
    } restore{before, oldHandler};
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Ending ending;
    QString error;
    auto file = DownloadFile::create(dir.filePath(QStringLiteral("got.bin")), false, {}, ending.callback(), error);
    ASSERT_NE(file, nullptr) << error.toStdString();
    EXPECT_TRUE(file->write(std::string(64 << 10, 'x')));
    // The writer fails once it reaches the chunk.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (file->write("more") && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_FALSE(file->write("more"));
    file->finish(true, QStringLiteral("unused"));
    ASSERT_TRUE(ending.wait());
    EXPECT_FALSE(ending.ok);
    EXPECT_EQ(ending.error, QString::fromLocal8Bit(std::strerror(EFBIG)));
    EXPECT_TRUE(namesIn(dir).isEmpty()) << namesIn(dir).join(QLatin1Char(',')).toStdString();
}

} // namespace
