// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop/FileTransfer.h"
#include <AfterglowProto/Transfer.h>
#include <AfterglowProto/TransferClient.h>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QScopeGuard>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QThread>
#include <QtEndian>
#include <algorithm>
#include <fcntl.h>
#include <functional>
#include <gtest/gtest.h>
#include <optional>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include <xxhash.h>
#include <zlib.h>
using namespace Afterglow;
namespace {
QByteArray b64(const QByteArray& s) { return s.toBase64(QByteArray::OmitTrailingEquals); }
// The messages of the afterglow.desktop category while it lives. The worker thread writes them.
struct DesktopLog {
    static inline QMutex mutex;
    static inline QStringList lines;
    static inline QtMessageHandler previous = nullptr;
    DesktopLog()
    {
        QMutexLocker lock(&mutex);
        lines.clear();
        previous = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext& context, const QString& message) {
            if (context.category && QLatin1String(context.category) == QLatin1String("afterglow.desktop")) {
                QMutexLocker lock(&mutex);
                lines.append(message);
            } else if (previous)
                previous(type, context, message);
        });
    }
    ~DesktopLog() { qInstallMessageHandler(previous); }
    bool contains(const QString& text) const
    {
        QMutexLocker lock(&mutex);
        return std::any_of(lines.begin(), lines.end(), [&](const QString& line) { return line.contains(text); });
    }
    qsizetype count(const QString& text) const
    {
        QMutexLocker lock(&mutex);
        return std::count_if(lines.begin(), lines.end(), [&](const QString& line) { return line.contains(text); });
    }
};
bool waitFor(const std::function<bool()>& predicate)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < 3000) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return predicate();
}
struct Transfer : testing::Test {
    QTemporaryDir dir;
    FileTransfer service;
    QList<QByteArray> replies;
    QString token;
    QStringList prompted;
    // Every list that a prompt showed, in order.
    QList<QStringList> promptedLists;
    int promptedTotal = 0;
    int prompts = 0;
    int closed = 0;
    // A prompt is approved at once.
    bool autoApprove = false;
    void SetUp() override
    {
        QObject::connect(&service, &FileTransfer::reply, &service, [this](const QByteArray& b) { replies.append(b); });
        QObject::connect(&service, &FileTransfer::permissionRequested, &service,
            [this](const QString& t, bool, const QStringList& paths, int total) {
                token = t;
                prompted = paths;
                promptedLists.append(paths);
                promptedTotal = total;
                ++prompts;
                if (autoApprove)
                    service.approve(t, true);
            });
        QObject::connect(&service, &FileTransfer::sessionClosed, &service, [this] { ++closed; });
    }
    void send(QByteArray command) { service.handle("id=test;" + command); }
    // Waits until the worker has handled every command sent before: it
    // handles them in order, and answers a command of an unknown session.
    bool barrier()
    {
        static int count = 0;
        const QByteArray id = "barrier" + QByteArray::number(++count);
        service.handle("id=" + id + ";ac=finish");
        return waitFor([&] { return std::any_of(replies.begin(), replies.end(), [&](const auto& b) { return b.contains("id=" + id + ";"); }); });
    }
    bool repliedFor(const QByteArray& id)
    {
        return std::any_of(replies.begin(), replies.end(), [&](const auto& b) { return b.contains("id=" + id + ";"); });
    }
    bool status(const QByteArray& value)
    {
        return waitFor([&] {
            return std::any_of(replies.begin(), replies.end(), [&](auto b) { return b.contains("st=" + b64(value)); });
        });
    }
    // A send session: it starts at once, and the prompt for each file's
    // destination is approved.
    bool start()
    {
        autoApprove = true;
        send("ac=send");
        return status("OK");
    }
    bool fileStatus(const QByteArray& fid, const QByteArray& value)
    {
        return waitFor([&] {
            return std::any_of(replies.begin(), replies.end(), [&](const auto& b) {
                return b.contains(";fid=" + fid + ";") && b.contains("st=" + b64(value));
            });
        });
    }
    // The data of every data and end_data reply for @p fid, decoded.
    QByteArray received(const QByteArray& fid)
    {
        QByteArray data;
        for (const QByteArray& reply : std::as_const(replies)) {
            if (!reply.contains(";fid=" + fid + ";") || !(reply.contains("ac=data;") || reply.contains("ac=end_data;")))
                continue;
            const qsizetype at = reply.indexOf(";d=");
            if (at < 0)
                continue;
            const qsizetype end = reply.indexOf(';', at + 3);
            data += QByteArray::fromBase64(reply.mid(at + 3, end - at - 3));
        }
        return data;
    }
    bool ended(const QByteArray& fid)
    {
        return waitFor([&] {
            return std::any_of(replies.begin(), replies.end(),
                [&](const auto& b) { return b.contains("ac=end_data;") && b.contains(";fid=" + fid + ";"); });
        });
    }
    // A path in the test's directory, through no link: a prompt lists it as it is.
    QString file(const char* name) const
    {
        return QDir(QFileInfo(dir.path()).canonicalFilePath()).filePath(QString::fromUtf8(name));
    }
    QByteArray path(const char* name) { return b64(file(name).toUtf8()); }
    // Answers each prompt with @p allowed until @p done, or none comes for a while.
    bool answerPrompts(bool allowed, const std::function<bool()>& done)
    {
        int answered = 0;
        return waitFor([&] {
            if (prompts > answered) {
                answered = prompts;
                service.approve(token, allowed);
            }
            return done();
        });
    }
    // The temporary files of uploads in the test's directory.
    QStringList temporaries() const
    {
        return QDir(dir.path()).entryList({ QStringLiteral(".afterglow-*") }, QDir::Files | QDir::Hidden | QDir::System);
    }
    QByteArray contents(const char* name)
    {
        QFile f(dir.filePath(QString::fromLatin1(name)));
        if (!f.open(QIODevice::ReadOnly))
            return { };
        return f.readAll();
    }
};
TEST_F(Transfer, PermissionDenialCannotCreateFiles)
{
    send("ac=send");
    send("ac=file;fid=f;n=" + path("denied"));
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    service.approve(token, false);
    ASSERT_TRUE(status("EPERM"));
    send("ac=file;fid=g;n=" + path("denied"));
    ASSERT_TRUE(status("ENOENT"));
    EXPECT_FALSE(QFile::exists(dir.filePath("denied")));
}
TEST_F(Transfer, ASendPromptListsTheDestinationsBeforeAnyIsWritten)
{
    // The session starts at once: the sender names its files only after that.
    send("ac=send");
    ASSERT_TRUE(status("OK"));
    EXPECT_EQ(prompts, 0);
    send("ac=file;fid=a;n=" + path("a"));
    send("ac=file;fid=b;ft=directory;n=" + path("sub/b"));
    send("ac=data;fid=a;d=" + b64("early"));
    ASSERT_TRUE(waitFor([&] { return prompts == 1; }));
    ASSERT_TRUE(barrier());
    // Nothing reaches the disk, not even a temporary file or a directory,
    // and no file starts, before the answer.
    EXPECT_TRUE(QDir(dir.path()).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System));
    EXPECT_FALSE(std::any_of(replies.begin(), replies.end(), [](const auto& b) { return b.contains(";fid=a;"); }));
    EXPECT_EQ(prompted.front(), file("a"));
    // A destination that came after the worker asked has a prompt of its own.
    ASSERT_TRUE(answerPrompts(true, [&] { return QFileInfo(file("sub/b")).isDir(); }));
    ASSERT_TRUE(fileStatus("a", "STARTED"));
    QStringList listed;
    for (const auto& list : std::as_const(promptedLists))
        listed += list;
    EXPECT_EQ(listed, (QStringList { file("a"), file("sub/b") }));
    // The data that came early follows its file.
    send("ac=end_data;fid=a;d=" + b64("!"));
    ASSERT_TRUE(fileStatus("a", "OK"));
    EXPECT_EQ(contents("a"), QByteArray("early!"));
}
TEST_F(Transfer, ADestinationThatThePromptDidNotShowGetsAPromptOfItsOwn)
{
    send("ac=send");
    send("ac=file;fid=a;n=" + path("a"));
    ASSERT_TRUE(waitFor([&] { return prompts == 1; }));
    const QString first = token;
    // b comes while the prompt shows. The prompt stays as it was.
    send("ac=file;fid=b;n=" + path("b"));
    send("ac=data;fid=b;d=" + b64("b"));
    ASSERT_TRUE(barrier());
    EXPECT_EQ(prompts, 1);
    service.approve(first, true);
    ASSERT_TRUE(fileStatus("a", "STARTED"));
    ASSERT_TRUE(waitFor([&] { return prompts == 2; }));
    EXPECT_EQ(prompted, QStringList { file("b") });
    EXPECT_EQ(promptedTotal, 1);
    // A later No refuses the files of its prompt. The session goes on, and
    // the file that the user approved before finishes.
    service.approve(token, false);
    ASSERT_TRUE(fileStatus("b", "EPERM"));
    send("ac=end_data;fid=a;d=" + b64("a"));
    ASSERT_TRUE(fileStatus("a", "OK"));
    EXPECT_EQ(contents("a"), "a");
    EXPECT_FALSE(QFileInfo::exists(file("b")));
    EXPECT_EQ(closed, 0);
    send("ac=finish");
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
    EXPECT_TRUE(temporaries().isEmpty());
}
TEST_F(Transfer, ADestinationThatComesAfterAYesGetsAPromptOfItsOwn)
{
    send("ac=send");
    send("ac=file;fid=a;n=" + path("a"));
    ASSERT_TRUE(waitFor([&] { return prompts == 1; }));
    service.approve(token, true);
    ASSERT_TRUE(fileStatus("a", "STARTED"));
    send("ac=file;fid=b;n=" + path("b"));
    ASSERT_TRUE(waitFor([&] { return prompts == 2; }));
    EXPECT_EQ(prompted, QStringList { file("b") });
    ASSERT_TRUE(barrier());
    EXPECT_FALSE(std::any_of(replies.begin(), replies.end(), [](const auto& b) { return b.contains(";fid=b;"); }));
}
// The data of an approved file goes on while a later file waits for its
// prompt: aglow sends data as soon as a file starts.
TEST_F(Transfer, AFileThatWaitsHoldsBackOnlyItsOwnCommands)
{
    send("ac=send");
    send("ac=file;fid=a;n=" + path("a"));
    ASSERT_TRUE(waitFor([&] { return prompts == 1; }));
    service.approve(token, true);
    ASSERT_TRUE(fileStatus("a", "STARTED"));
    send("ac=file;fid=b;n=" + path("b"));
    ASSERT_TRUE(waitFor([&] { return prompts == 2; }));
    send("ac=data;fid=b;d=" + b64("b"));
    send("ac=end_data;fid=a;d=" + b64("a"));
    ASSERT_TRUE(fileStatus("a", "OK"));
    EXPECT_EQ(contents("a"), "a");
    // A finish waits for the file that waits.
    send("ac=finish");
    ASSERT_TRUE(barrier());
    EXPECT_EQ(closed, 0);
    service.approve(token, true);
    ASSERT_TRUE(fileStatus("b", "STARTED"));
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
    // The finish ran after b's data: b had no end, so it failed at the finish.
    EXPECT_FALSE(QFileInfo::exists(file("b")));
    EXPECT_TRUE(temporaries().isEmpty());
}
// A program that ends its session while the prompt shows cannot ask again at once.
TEST_F(Transfer, ACancelWhileThePromptShowsCoolsDown)
{
    send("ac=send");
    send("ac=file;fid=a;n=" + path("a"));
    ASSERT_TRUE(waitFor([&] { return prompts == 1; }));
    send("ac=cancel");
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
    replies.clear();
    send("ac=send");
    ASSERT_TRUE(status("EPERM"));
    EXPECT_EQ(prompts, 1);
}
// A send session that started before a denial gets its first prompt after
// it: the cooldown refuses the session.
TEST_F(Transfer, ASessionThatStartedBeforeADenialIsRefusedAtItsFirstFile)
{
    service.handle("id=early;ac=send");
    ASSERT_TRUE(waitFor([&] { return repliedFor("early"); }));
    send("ac=receive;sz=1");
    send("ac=file;fid=r;n=" + path("r"));
    ASSERT_TRUE(waitFor([&] { return prompts == 1; }));
    service.approve(token, false);
    ASSERT_TRUE(status("EPERM"));
    service.handle("id=early;ac=file;fid=f;n=" + path("f"));
    ASSERT_TRUE(waitFor([&] {
        return std::any_of(replies.begin(), replies.end(),
            [](const auto& b) { return b.contains("id=early;") && !b.contains("fid=") && b.contains("st=" + b64("EPERM")); });
    }));
    EXPECT_EQ(prompts, 1);
    EXPECT_FALSE(QFileInfo::exists(file("f")));
}
// The commands of files that wait are bounded, and so are their files.
TEST_F(Transfer, WaitingFilesAreBounded)
{
    send("ac=send");
    ASSERT_TRUE(status("OK"));
    for (int i = 0; i < 16384; ++i)
        send("ac=file;fid=f" + QByteArray::number(i) + ";n=" + b64(file("x").toUtf8() + QByteArray::number(i)));
    send("ac=file;fid=over;n=" + path("over"));
    ASSERT_TRUE(fileStatus("over", "EINVAL"));
    // The first prompt came when the worker had the first files. The next
    // lists 100 of the others.
    ASSERT_TRUE(waitFor([&] { return prompts == 1; }));
    const int first = int(prompted.size());
    EXPECT_GE(first, 1);
    EXPECT_LE(first, 100);
    service.approve(token, true);
    ASSERT_TRUE(waitFor([&] { return prompts == 2; }));
    EXPECT_EQ(prompted.size(), 100);
    EXPECT_EQ(promptedTotal, 16384 - first);
    // Data for a file that waits fills the budget: it ends the session.
    const QByteArray data = "ac=data;fid=f16000;d=" + b64(QByteArray(3000, 'd'));
    // In parts that the worker handles before the next: a full input queue
    // would abort every session, which is another limit.
    for (int i = 0; i < 6000 && closed == 0; ++i) {
        send(data);
        if (i % 500 == 499) {
            ASSERT_TRUE(barrier());
        }
    }
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
    EXPECT_TRUE(std::any_of(replies.begin(), replies.end(),
        [](const auto& b) { return !b.contains("fid=") && b.contains("st=" + b64("ENOMEM")); }));
}
TEST(TransferQuestion, ShowsTheFirstPathsAndCountsTheRest)
{
    QStringList paths;
    for (int i = 0; i < 101; ++i)
        paths << QStringLiteral("/home/u/f%1").arg(i);
    const QString write = QStringLiteral("A terminal program wants to write these files on this computer. Existing files can be replaced. Allow it?");
    const QString read = QStringLiteral("A terminal program wants to read these files on this computer. Allow it?");
    // No path: the question alone.
    EXPECT_EQ(FileTransfer::question(true, { }, 0).text, write);
    // 10 of 10 fit in the question.
    const auto ten = FileTransfer::question(true, paths.mid(0, 10), 10);
    EXPECT_TRUE(ten.text.endsWith(QStringLiteral("/home/u/f9")));
    EXPECT_FALSE(ten.text.contains(QStringLiteral("more")));
    EXPECT_EQ(ten.detail, QStringList(paths.mid(0, 10)).join(QLatin1Char('\n')));
    // 11 of 11: one more.
    const auto eleven = FileTransfer::question(true, paths.mid(0, 11), 11);
    EXPECT_FALSE(eleven.text.contains(QStringLiteral("/home/u/f10")));
    EXPECT_TRUE(eleven.text.endsWith(QStringLiteral("/home/u/f9\nand 1 more")));
    // 100 of 100: the detail has every path.
    const auto hundred = FileTransfer::question(false, paths.mid(0, 100), 100);
    EXPECT_TRUE(hundred.text.startsWith(read + QLatin1Char('\n')));
    EXPECT_TRUE(hundred.text.endsWith(QStringLiteral("and 90 more")));
    EXPECT_TRUE(hundred.detail.endsWith(QStringLiteral("/home/u/f99")));
    // 100 of 101: the answer does not cover the last one.
    const auto writeMore = FileTransfer::question(true, paths.mid(0, 100), 101);
    EXPECT_TRUE(writeMore.text.endsWith(QStringLiteral("and 90 more\nFiles not in this list: 1. They get a question of their own.")));
    const auto readMore = FileTransfer::question(false, paths.mid(0, 100), 101);
    EXPECT_TRUE(readMore.text.endsWith(QStringLiteral("and 90 more\nRequested files not in this list: 1. They are refused.")));
    EXPECT_FALSE(readMore.detail.contains(QStringLiteral("/home/u/f100")));
    // 0 of 5: only the count.
    EXPECT_EQ(FileTransfer::question(false, { }, 5).text, read + QStringLiteral("\nRequested files not in this list: 5. They are refused."));
    // Who asks, and a long path cut in the middle of the question but whole in the detail.
    const QString longPath = QStringLiteral("/") + QString(300, QLatin1Char('a')) + QStringLiteral("/end");
    const auto named = FileTransfer::question(true, { longPath }, 1, QStringLiteral("vim in tab 2"));
    EXPECT_TRUE(named.text.startsWith(QStringLiteral("vim in tab 2 wants to write")));
    const QString line = named.text.section(QLatin1Char('\n'), 1, 1);
    EXPECT_EQ(line.size(), FileTransfer::QuestionLength);
    EXPECT_TRUE(line.endsWith(QStringLiteral("/end")));
    EXPECT_EQ(named.detail, longPath);
    // Where a long path resolves stays a line of its own, cut on its own.
    const QString resolved = longPath + QStringLiteral("\n  resolves to ") + longPath;
    const auto both = FileTransfer::question(true, { resolved }, 1);
    EXPECT_EQ(both.text.section(QLatin1Char('\n'), 1, 1).size(), FileTransfer::QuestionLength);
    EXPECT_TRUE(both.text.section(QLatin1Char('\n'), 2, 2).startsWith(QStringLiteral("  resolves to ")));
    EXPECT_EQ(both.detail, resolved);
    // A directory that a program reads is read whole.
    const auto directory = FileTransfer::question(false, { QStringLiteral("/home/u/dir/") }, 1);
    EXPECT_TRUE(directory.text.contains(QStringLiteral("ends with a slash")));
    EXPECT_FALSE(FileTransfer::question(false, { QStringLiteral("/home/u/file") }, 1).text.contains(QStringLiteral("slash")));
}
TEST_F(Transfer, CompressedFileAndLinksCommit)
{
    ASSERT_TRUE(start());
    send("ac=file;fid=f;sz=11;prm=384;n=" + path("file") + ";zip=zlib");
    ASSERT_TRUE(status("STARTED"));
    QByteArray zipped(128, '\0');
    uLongf size = zipped.size();
    ASSERT_EQ(
        compress(reinterpret_cast<Bytef*>(zipped.data()), &size, reinterpret_cast<const Bytef*>("hello world"), 11),
        Z_OK);
    zipped.resize(qsizetype(size));
    send("ac=end_data;fid=f;d=" + b64(zipped));
    send("ac=file;fid=l;ft=symlink;n=" + path("link"));
    send("ac=end_data;fid=l;d=" + b64("fid:f"));
    send("ac=file;fid=h;ft=link;n=" + path("hard"));
    send("ac=end_data;fid=h;d=" + b64("f"));
    send("ac=finish");
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
    EXPECT_EQ(contents("file"), "hello world");
    EXPECT_EQ(contents("link"), "hello world");
    EXPECT_EQ(contents("hard"), "hello world");
}
TEST_F(Transfer, ModesKeepOnlyThePermissionBits)
{
    ASSERT_TRUE(start());
    send("ac=file;fid=f;sz=2;prm=" + QByteArray::number(06777) + ";n=" + path("setid"));
    ASSERT_TRUE(fileStatus("f", "STARTED"));
    send("ac=end_data;fid=f;d=" + b64("ok"));
    ASSERT_TRUE(fileStatus("f", "OK"));
    send("ac=file;fid=d;ft=directory;prm=" + QByteArray::number(01777) + ";n=" + path("sticky"));
    ASSERT_TRUE(fileStatus("d", "OK"));
    send("ac=finish");
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
    struct stat st { };
    ASSERT_EQ(::stat(QFile::encodeName(dir.filePath(QStringLiteral("setid"))).constData(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0777u);
    ASSERT_EQ(::stat(QFile::encodeName(dir.filePath(QStringLiteral("sticky"))).constData(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0777u);
}
TEST_F(Transfer, CancellationPreservesExistingFileAndRemovesTemporaryFiles)
{
    QFile original(dir.filePath("file"));
    ASSERT_TRUE(original.open(QIODevice::WriteOnly));
    original.write("original");
    original.close();
    ASSERT_TRUE(start());
    send("ac=file;fid=f;n=" + path("file"));
    ASSERT_TRUE(status("STARTED"));
    send("ac=data;fid=f;d=" + b64("replacement"));
    send("ac=cancel");
    ASSERT_TRUE(status("CANCELED"));
    EXPECT_EQ(contents("file"), "original");
    EXPECT_EQ(QDir(dir.path()).entryList(QDir::Files | QDir::Hidden).size(), 1);
}
TEST_F(Transfer, ReadRequiresApprovalAndStreamsContent)
{
    QFile source(dir.filePath("file"));
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    source.write("source data");
    source.close();
    send("ac=receive;sz=1");
    send("ac=file;fid=req;n=" + path("file"));
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    // Every reply that the requests made is queued before the barrier's.
    ASSERT_TRUE(barrier());
    EXPECT_FALSE(repliedFor("test"));
    service.approve(token, true);
    ASSERT_TRUE(status("OK"));
    ASSERT_TRUE(waitFor(
        [&] { return std::any_of(replies.begin(), replies.end(), [](auto b) { return b.contains("ac=file"); }); }));
    send("ac=file;fid=data;n=" + path("file"));
    ASSERT_TRUE(ended("data"));
    EXPECT_EQ(received("data"), "source data");
}

// A name that does not start with '/' is in the home directory, also when it
// starts with ':', which Qt would take for a resource.
TEST_F(Transfer, ANameWithAColonIsAFileInTheHomeDirectory)
{
    const QByteArray home = qgetenv("HOME");
    const auto restore = qScopeGuard([&] { qputenv("HOME", home); });
    qputenv("HOME", QFile::encodeName(dir.path()));
    // Not dir.filePath(): it too takes ":notes" for a resource.
    QFile source(dir.path() + QStringLiteral("/:notes"));
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    source.write("home data");
    source.close();
    send("ac=receive;sz=1");
    send("ac=file;fid=req;n=" + b64(":notes"));
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    service.approve(token, true);
    ASSERT_TRUE(status("OK"));
    ASSERT_TRUE(waitFor(
        [&] { return std::any_of(replies.begin(), replies.end(), [](auto b) { return b.contains("ac=file"); }); }));
    send("ac=file;fid=data;n=" + b64(":notes"));
    ASSERT_TRUE(ended("data"));
    EXPECT_EQ(received("data"), "home data");
}

// A file in /proc states st_size 0 and holds data. QFile::atEnd() is true
// each time its 16 KiB read buffer is empty, so the stream must end at an
// empty read, not at the first buffer.
TEST_F(Transfer, ReadOfFileWithSizeZeroSendsMoreThanOneBuffer)
{
    const QByteArray name = "/proc/self/smaps";
    QFile probe(QString::fromLatin1(name));
    struct stat st { };
    if (::stat(name.constData(), &st) || st.st_size != 0 || !probe.open(QIODevice::ReadOnly))
        GTEST_SKIP() << "needs a /proc file that states size 0";
    // The test reads the file itself. The mappings change while the worker
    // runs, so the test compares the shape of the contents, not each byte.
    const QByteArray own = probe.readAll();
    if (own.size() <= 16384)
        GTEST_SKIP() << "needs a /proc file larger than one read buffer";
    send("ac=receive;sz=1");
    send("ac=file;fid=req;n=" + b64(name));
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    service.approve(token, true);
    ASSERT_TRUE(status("OK"));
    ASSERT_TRUE(waitFor(
        [&] { return std::any_of(replies.begin(), replies.end(), [](auto b) { return b.contains("ac=file"); }); }));
    send("ac=file;fid=plain;n=" + b64(name));
    ASSERT_TRUE(ended("plain"));
    const QByteArray data = received("plain");
    EXPECT_GT(data.size(), 16384);
    EXPECT_EQ(data.left(data.indexOf('\n')), own.left(own.indexOf('\n')));
    // Each mapping ends with its VmFlags line: a cut stream ends inside one.
    ASSERT_TRUE(data.endsWith('\n'));
    const QByteArray last = data.left(data.size() - 1);
    EXPECT_TRUE(last.mid(last.lastIndexOf('\n') + 1).startsWith("VmFlags:"));
}

// A sysfs attribute states st_size 4096 and holds a few bytes: QFile::atEnd()
// stays false after the last read, so the end must come from the empty read.
TEST_F(Transfer, ReadOfFileShorterThanItsStatedSizeEnds)
{
    const QString name = QStringLiteral("/sys/devices/system/cpu/online");
    QFile probe(name);
    struct stat st { };
    if (::stat(QFile::encodeName(name).constData(), &st) || !probe.open(QIODevice::ReadOnly)
        || probe.readAll().size() >= st.st_size)
        GTEST_SKIP() << "needs a sysfs file whose stated size exceeds its contents";
    send("ac=receive;sz=1");
    send("ac=file;fid=req;n=" + b64(name.toUtf8()));
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    service.approve(token, true);
    ASSERT_TRUE(status("OK"));
    ASSERT_TRUE(waitFor(
        [&] { return std::any_of(replies.begin(), replies.end(), [](auto b) { return b.contains("ac=file"); }); }));
    send("ac=file;fid=plain;n=" + b64(name.toUtf8()));
    send("ac=file;fid=zipped;zip=zlib;n=" + b64(name.toUtf8()));
    for (const char* fid : { "plain", "zipped" })
        EXPECT_TRUE(waitFor([&] {
            return std::any_of(replies.begin(), replies.end(),
                [&](auto b) { return b.contains("ac=end_data") && b.contains(";fid=" + QByteArray(fid) + ";"); });
        })) << fid;
    // The stream holds what the file holds, not its stated size.
    probe.seek(0);
    const QByteArray expected = probe.readAll();
    EXPECT_EQ(received("plain"), expected);
    QByteArray inflated(4096, '\0');
    const QByteArray zipped = received("zipped");
    uLongf size = uLongf(inflated.size());
    ASSERT_EQ(uncompress(reinterpret_cast<Bytef*>(inflated.data()), &size, reinterpret_cast<const Bytef*>(zipped.constData()), uLong(zipped.size())), Z_OK);
    inflated.resize(qsizetype(size));
    EXPECT_EQ(inflated, expected);
}

TEST_F(Transfer, EmptyFileIsCommittedBeforeSuccessWithoutFinish)
{
    ASSERT_TRUE(start());
    replies.clear();
    send("ac=file;fid=empty;sz=0;n=" + path("empty"));
    ASSERT_TRUE(status("STARTED"));
    send("ac=end_data;fid=empty");
    ASSERT_TRUE(status("OK"));
    ASSERT_TRUE(QFile::exists(dir.filePath("empty")));
    EXPECT_TRUE(contents("empty").isEmpty());
}
TEST_F(Transfer, InvalidCompressedDataNeverReplacesDestination)
{
    QFile file(dir.filePath("file"));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("original");
    file.close();
    ASSERT_TRUE(start());
    send("ac=file;fid=f;zip=zlib;n=" + path("file"));
    ASSERT_TRUE(status("STARTED"));
    send("ac=end_data;fid=f;d=" + b64("not compressed"));
    ASSERT_TRUE(status("EIO"));
    send("ac=finish");
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
    EXPECT_EQ(contents("file"), "original");
}

TEST_F(Transfer, CancelRemovesNewParentDirectories)
{
    ASSERT_TRUE(start());
    send("ac=file;fid=f;n=" + path("new/parent/file"));
    ASSERT_TRUE(status("STARTED"));
    send("ac=data;fid=f;d=" + b64("partial"));
    send("ac=cancel");
    ASSERT_TRUE(status("CANCELED"));
    EXPECT_FALSE(QFile::exists(dir.filePath("new")));
}

TEST_F(Transfer, ResourceAbortReportsFailureAndClosesSession)
{
    ASSERT_TRUE(start());
    service.abort();
    ASSERT_TRUE(status("ENOMEM"));
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
}
// The terminal forwards a command over the limit cut to one byte past it.
// A command at the limit is read. One byte more aborts every session.
TEST_F(Transfer, CommandOverTheLimitAbortsTheSessions)
{
    ASSERT_TRUE(start());
    send("ac=file;fid=f;n=" + path("file"));
    ASSERT_TRUE(fileStatus("f", "STARTED"));
    QByteArray command = "id=test;ac=data;fid=f;d=" + b64("at the limit") + ";x=";
    command += QByteArray(qsizetype(AfterglowProto::TransferMaxCommand) - command.size(), 'a');
    ASSERT_EQ(std::size_t(command.size()), AfterglowProto::TransferMaxCommand);
    service.handle(command);
    send("ac=end_data;fid=f");
    ASSERT_TRUE(fileStatus("f", "OK"));
    EXPECT_EQ(contents("file"), "at the limit");
    EXPECT_EQ(closed, 0);
    service.handle(command + 'a');
    ASSERT_TRUE(status("ENOMEM"));
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
}
TEST_F(Transfer, PendingUploadsAreBoundedAndCompletionReleasesCapacity)
{
    ASSERT_TRUE(start());
    // A file holds nothing until its data comes: every one starts.
    for (int i = 0; i < 160; ++i) {
        const auto fid = QByteArray::number(i);
        send("ac=file;fid=" + fid + ";n=" + b64(dir.filePath(QString::number(i)).toUtf8()));
    }
    ASSERT_TRUE(fileStatus("159", "STARTED"));
    // Its data opens its temporary file and its directory: 64 fit.
    for (int i = 0; i < 160; ++i)
        send("ac=data;fid=" + QByteArray::number(i) + ";d=" + b64("x"));
    ASSERT_TRUE(fileStatus("159", "EBUSY"));
    ASSERT_TRUE(fileStatus("63", "PROGRESS"));
    EXPECT_FALSE(std::any_of(replies.begin(), replies.end(),
        [](const auto& b) { return b.contains(";fid=63;") && b.contains("st=" + b64("EBUSY")); }));
    send("ac=end_data;fid=0");
    ASSERT_TRUE(fileStatus("0", "OK"));
    send("ac=file;fid=next;n=" + path("next"));
    send("ac=data;fid=next;d=" + b64("x"));
    ASSERT_TRUE(fileStatus("next", "PROGRESS"));
    send("ac=cancel");
    ASSERT_TRUE(status("CANCELED"));
    token.clear();
    replies.clear();
    ASSERT_TRUE(start());
    send("ac=file;fid=fresh;n=" + path("fresh"));
    EXPECT_TRUE(fileStatus("fresh", "STARTED"));
}
TEST_F(Transfer, CompletedUploadsCloseDescriptorsBeforeSessionFinish)
{
    ASSERT_TRUE(start());
    const auto descriptors = [] { return QDir("/proc/self/fd").entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size(); };
    const auto before = descriptors();
    for (int i = 0; i < 160; ++i) {
        const auto fid = QByteArray::number(i);
        send("ac=file;fid=" + fid + ";n=" + b64(dir.filePath(QString::number(i)).toUtf8()));
        send("ac=end_data;fid=" + fid);
    }
    ASSERT_TRUE(fileStatus("159", "OK"));
    EXPECT_LE(descriptors(), before + 4);
    EXPECT_EQ(QDir(dir.path()).entryList(QDir::Files).size(), 160);
    EXPECT_EQ(closed, 0);
}
TEST_F(Transfer, PendingRsyncReadsAreBoundedAndReleaseCapacity)
{
    QFile source(dir.filePath("empty"));
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    source.close();
    send("ac=receive;sz=1");
    send("ac=file;fid=request;n=" + path("empty"));
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    service.approve(token, true);
    ASSERT_TRUE(status("OK"));
    for (int i = 0; i < 160; ++i)
        send("ac=file;tt=rsync;fid=r" + QByteArray::number(i) + ";n=" + path("empty"));
    ASSERT_TRUE(fileStatus("r159", "EBUSY"));
    QByteArray signature(12, '\0');
    signature[8] = 64;
    const auto finish = [&](const QByteArray& fid) {
        send("ac=end_data;fid=" + fid + ";d=" + b64(signature));
        return waitFor([&] {
            return std::any_of(replies.begin(), replies.end(), [&](const auto& b) {
                return b.contains("ac=end_data;") && b.contains(";fid=" + fid + ";");
            });
        });
    };
    ASSERT_TRUE(finish("r0"));
    send("ac=file;tt=rsync;fid=next;n=" + path("empty"));
    EXPECT_TRUE(finish("next"));
}
TEST_F(Transfer, ResetClosesPendingPrompt)
{
    send("ac=send");
    send("ac=file;fid=w;n=" + path("waiting"));
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    service.reset();
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
    service.approve(token, true);
    send("ac=file;fid=f;n=" + path("file"));
    ASSERT_TRUE(status("ENOENT"));
    EXPECT_FALSE(QFile::exists(dir.filePath("file")));
    EXPECT_FALSE(QFile::exists(dir.filePath("waiting")));
}
TEST_F(Transfer, CancelRemovesNewParentsOfEveryFile)
{
    ASSERT_TRUE(start());
    send("ac=file;fid=a;n=" + path("new/a/f"));
    ASSERT_TRUE(fileStatus("a", "STARTED"));
    send("ac=file;fid=b;n=" + path("new/a/b/f"));
    ASSERT_TRUE(fileStatus("b", "STARTED"));
    send("ac=cancel");
    ASSERT_TRUE(status("CANCELED"));
    EXPECT_FALSE(QFile::exists(dir.filePath("new")));
}
TEST_F(Transfer, RejectedFileCreatesNoDirectories)
{
    ASSERT_TRUE(start());
    send("ac=file;fid=f;zip=bogus;n=" + path("new/parent/file"));
    ASSERT_TRUE(fileStatus("f", "EINVAL"));
    send("ac=finish");
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
    EXPECT_FALSE(QFile::exists(dir.filePath("new")));
}
TEST_F(Transfer, FileWithoutModeFollowsTheUmask)
{
    const mode_t mask = ::umask(022);
    ::umask(mask);
    ASSERT_TRUE(start());
    send("ac=file;fid=f;n=" + path("plain"));
    ASSERT_TRUE(fileStatus("f", "STARTED"));
    send("ac=end_data;fid=f;d=" + b64("x"));
    ASSERT_TRUE(fileStatus("f", "OK"));
    struct stat st { };
    ASSERT_EQ(::stat(QFile::encodeName(dir.filePath("plain")).constData(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0666 & ~mask);
    // prm=-1 means "not given" too.
    send("ac=file;fid=g;prm=-1;n=" + path("unset"));
    ASSERT_TRUE(fileStatus("g", "STARTED"));
    send("ac=end_data;fid=g;d=" + b64("x"));
    ASSERT_TRUE(fileStatus("g", "OK"));
    ASSERT_EQ(::stat(QFile::encodeName(dir.filePath("unset")).constData(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0666 & ~mask);
}
TEST_F(Transfer, ModificationTimeBefore1970IsKept)
{
    ASSERT_TRUE(start());
    send("ac=file;fid=f;mod=-1500000000;n=" + path("old"));
    ASSERT_TRUE(fileStatus("f", "STARTED"));
    send("ac=end_data;fid=f");
    ASSERT_TRUE(fileStatus("f", "OK"));
    struct stat st { };
    ASSERT_EQ(::stat(QFile::encodeName(dir.filePath("old")).constData(), &st), 0);
    EXPECT_EQ(st.st_mtim.tv_sec, -2);
    EXPECT_EQ(st.st_mtim.tv_nsec, 500000000);
}
TEST_F(Transfer, LinkChainSentBeforeItsTargetCommits)
{
    ASSERT_TRUE(start());
    send("ac=file;fid=l3;ft=symlink;n=" + path("l3"));
    send("ac=end_data;fid=l3;d=" + b64("fid:l2"));
    send("ac=file;fid=l2;ft=symlink;n=" + path("l2"));
    send("ac=end_data;fid=l2;d=" + b64("fid:l1"));
    send("ac=file;fid=l1;ft=link;n=" + path("l1"));
    send("ac=end_data;fid=l1;d=" + b64("f"));
    send("ac=file;fid=f;n=" + path("f"));
    send("ac=end_data;fid=f;d=" + b64("chain"));
    ASSERT_TRUE(fileStatus("l3", "OK"));
    EXPECT_EQ(contents("l3"), "chain");
    EXPECT_EQ(contents("l1"), "chain");
}
TEST_F(Transfer, LinksThatNameEachOtherReportALoop)
{
    ASSERT_TRUE(start());
    send("ac=file;fid=a;ft=symlink;n=" + path("a"));
    send("ac=end_data;fid=a;d=" + b64("fid:b"));
    send("ac=file;fid=b;ft=symlink;n=" + path("b"));
    send("ac=end_data;fid=b;d=" + b64("fid:a"));
    send("ac=file;fid=c;ft=symlink;n=" + path("c"));
    send("ac=end_data;fid=c;d=" + b64("fid:missing"));
    DesktopLog log;
    // The approval of the destinations reaches the worker before the barrier.
    ASSERT_TRUE(waitFor([&] { return prompts > 0; }));
    ASSERT_TRUE(barrier());
    const auto beforeFinish = replies.size();
    send("ac=finish");
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
    // The client reads no more after its finish: the shell would get a
    // status as typed input. The failures go to the log.
    ASSERT_TRUE(barrier());
    EXPECT_FALSE(std::any_of(replies.begin() + qsizetype(beforeFinish), replies.end(),
        [](const auto& b) { return b.contains("fid=a;") || b.contains("fid=c;"); }));
    EXPECT_TRUE(log.contains(QStringLiteral("ELOOP")));
    EXPECT_TRUE(log.contains(QStringLiteral("ENOENT")));
    EXPECT_FALSE(QFileInfo(dir.filePath(QStringLiteral("a"))).isSymLink());
}
TEST_F(Transfer, FailuresAtTheFinishAreLoggedInOneLine)
{
    ASSERT_TRUE(start());
    for (int i = 0; i < 5; ++i) {
        const QByteArray fid = "l" + QByteArray::number(i);
        send("ac=file;fid=" + fid + ";ft=symlink;n=" + b64(dir.filePath(QString::fromLatin1(fid)).toUtf8()));
        send("ac=end_data;fid=" + fid + ";d=" + b64("fid:missing"));
    }
    DesktopLog log;
    ASSERT_TRUE(barrier());
    send("ac=finish");
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
    ASSERT_TRUE(barrier());
    EXPECT_EQ(log.count(QStringLiteral("failed at the finish")), 1);
    EXPECT_TRUE(log.contains(QStringLiteral("ENOENT for 5 files")));
}
TEST_F(Transfer, AFailureAtTheFinishNamesThePathOfTheFile)
{
    // The fid is the client's name for the file. The log names the file.
    ASSERT_TRUE(start());
    send("ac=file;fid=clientfid;ft=symlink;n=" + path("dangling-link"));
    send("ac=end_data;fid=clientfid;d=" + b64("fid:missing"));
    DesktopLog log;
    ASSERT_TRUE(barrier());
    send("ac=finish");
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
    ASSERT_TRUE(barrier());
    ASSERT_EQ(log.count(QStringLiteral("failed at the finish")), 1);
    EXPECT_TRUE(log.contains(QStringLiteral("ENOENT for 1 file (") + dir.filePath(QStringLiteral("dangling-link")) + QLatin1Char(')')));
    EXPECT_FALSE(log.contains(QStringLiteral("clientfid")));
}
TEST_F(Transfer, AnUploadIntoAnExistingDirectoryKeepsItsMode)
{
    // ~/.config/nvim as a link to a directory: the upload goes in it, and
    // neither the link nor the directory takes the listed mode.
    const QString real = dir.filePath(QStringLiteral("real"));
    ASSERT_TRUE(QDir().mkpath(real));
    ASSERT_EQ(::chmod(QFile::encodeName(real).constData(), 0700), 0);
    ASSERT_EQ(::symlink("real", QFile::encodeName(dir.filePath(QStringLiteral("linked"))).constData()), 0);
    DesktopLog log;
    ASSERT_TRUE(start());
    send("ac=file;fid=d;ft=directory;prm=511;n=" + path("linked"));
    ASSERT_TRUE(fileStatus("d", "OK"));
    send("ac=file;fid=e;ft=directory;prm=511;n=" + path("real"));
    ASSERT_TRUE(fileStatus("e", "OK"));
    send("ac=file;fid=f;sz=1;n=" + path("linked/f"));
    ASSERT_TRUE(fileStatus("f", "STARTED"));
    send("ac=end_data;fid=f;d=" + b64("x"));
    ASSERT_TRUE(fileStatus("f", "OK"));
    send("ac=finish");
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
    ASSERT_TRUE(barrier());
    EXPECT_FALSE(log.contains(QStringLiteral("failed")));
    EXPECT_EQ(contents("real/f"), "x");
    struct stat st { };
    ASSERT_EQ(::stat(QFile::encodeName(real).constData(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0700u);
}
// A directory that did not exist when the prompt showed must be one that the
// transfer makes. Another session that made it a link to elsewhere since
// leads nothing there.
TEST_F(Transfer, ALinkMadeAfterThePromptLeadsNoFileElsewhere)
{
    QTemporaryDir outside;
    ASSERT_TRUE(outside.isValid());
    service.setPassword(QStringLiteral("secret"));
    send("ac=send");
    send("ac=file;fid=x;ft=directory;n=" + path("d/x"));
    ASSERT_TRUE(waitFor([&] { return prompts == 1; }));
    EXPECT_EQ(prompted, QStringList { file("d/x") });
    // While the prompt shows, a session with the password makes d a link.
    const QByteArray pw = "pw=" + b64(QByteArray::fromStdString(AfterglowProto::transferPasswordHash("other", "secret"))) + ";";
    service.handle("id=other;ac=send;" + pw);
    service.handle("id=other;ac=file;fid=l;ft=symlink;n=" + path("d"));
    service.handle("id=other;ac=end_data;fid=l;d=" + b64("path:" + outside.path().toUtf8()));
    ASSERT_TRUE(waitFor([&] { return QFileInfo(file("d")).isSymLink(); }));
    service.approve(token, true);
    ASSERT_TRUE(waitFor([&] {
        return std::any_of(replies.begin(), replies.end(), [](const auto& b) {
            return b.contains("id=test;") && b.contains(";fid=x;") && !b.contains("st=" + b64("OK"));
        });
    }));
    EXPECT_FALSE(QFileInfo::exists(outside.filePath(QStringLiteral("x"))));
    EXPECT_FALSE(std::any_of(replies.begin(), replies.end(),
        [](const auto& b) { return b.contains("id=test;") && b.contains(";fid=x;") && b.contains("st=" + b64("OK")); }));
}
// A link that was there when the prompt showed is the user's: the prompt
// names where it leads, and the file goes there.
TEST_F(Transfer, APromptNamesWhereALinkLeads)
{
    QTemporaryDir outside;
    ASSERT_TRUE(outside.isValid());
    const QString real = QFileInfo(outside.path()).canonicalFilePath();
    ASSERT_EQ(::symlink(QFile::encodeName(real).constData(), QFile::encodeName(file("downloads")).constData()), 0);
    send("ac=send");
    send("ac=file;fid=f;n=" + path("downloads/f"));
    ASSERT_TRUE(waitFor([&] { return prompts == 1; }));
    EXPECT_EQ(prompted, QStringList { file("downloads/f") + QStringLiteral("\n  resolves to ") + real + QStringLiteral("/f") });
    service.approve(token, true);
    send("ac=end_data;fid=f;d=" + b64("x"));
    ASSERT_TRUE(fileStatus("f", "OK"));
    QFile copy(real + QStringLiteral("/f"));
    ASSERT_TRUE(copy.open(QIODevice::ReadOnly));
    EXPECT_EQ(copy.readAll(), "x");
    // A read names where it leads too, and a directory with a slash.
    service.handle("id=read;ac=receive;sz=1");
    ASSERT_TRUE(QDir().mkpath(real + QStringLiteral("/sub")));
    service.handle("id=read;ac=file;fid=r;n=" + path("downloads/sub"));
    ASSERT_TRUE(waitFor([&] { return prompts == 2; }));
    EXPECT_EQ(prompted, QStringList { file("downloads/sub") + QStringLiteral("/\n  resolves to ") + real + QStringLiteral("/sub/") });
}
// A home directory under a link (/home on another disk) is the same place
// for itself and every path under it: the home gets no second line either.
TEST_F(Transfer, AHomeUnderALinkGetsNoLineOfItsOwn)
{
    QTemporaryDir outside;
    ASSERT_TRUE(outside.isValid());
    const QString real = QFileInfo(outside.path()).canonicalFilePath();
    ASSERT_TRUE(QDir().mkpath(real + QStringLiteral("/user/sub")));
    ASSERT_EQ(::symlink(QFile::encodeName(real).constData(), QFile::encodeName(file("home")).constData()), 0);
    const QByteArray home = qgetenv("HOME");
    const auto restore = qScopeGuard([&] { qputenv("HOME", home); });
    qputenv("HOME", QFile::encodeName(file("home/user")));
    service.handle("id=read;ac=receive;sz=2");
    service.handle("id=read;ac=file;fid=h;n=" + b64("~"));
    service.handle("id=read;ac=file;fid=s;n=" + b64("~/sub"));
    ASSERT_TRUE(waitFor([&] { return prompts == 1; }));
    EXPECT_EQ(prompted, (QStringList { file("home/user") + QStringLiteral("/"), file("home/user/sub") + QStringLiteral("/") }));
}
// A read goes where the prompt showed. A directory that became a link since
// leads the read nowhere else.
TEST_F(Transfer, AReadGoesWhereThePromptShowed)
{
    QTemporaryDir outside;
    ASSERT_TRUE(outside.isValid());
    {
        QFile secret(outside.filePath(QStringLiteral("secret")));
        ASSERT_TRUE(secret.open(QIODevice::WriteOnly));
        secret.write("secret");
    }
    ASSERT_TRUE(QDir().mkpath(file("x")));
    {
        QFile harmless(file("x/secret"));
        ASSERT_TRUE(harmless.open(QIODevice::WriteOnly));
        harmless.write("harmless");
    }
    send("ac=receive;sz=1");
    send("ac=file;fid=r;n=" + path("x/secret"));
    ASSERT_TRUE(waitFor([&] { return prompts == 1; }));
    EXPECT_EQ(prompted, QStringList { file("x/secret") });
    // Before the answer, x becomes a link to elsewhere.
    ASSERT_TRUE(QDir().rename(file("x"), file("y")));
    ASSERT_EQ(::symlink(QFile::encodeName(outside.path()).constData(), QFile::encodeName(file("x")).constData()), 0);
    service.approve(token, true);
    // An error: the link where a directory was (ELOOP), or a path that leads elsewhere (EPERM).
    ASSERT_TRUE(waitFor([&] {
        return std::any_of(replies.begin(), replies.end(), [](const auto& b) {
            return b.contains(";fid=r;") && (b.contains("st=" + b64("ELOOP")) || b.contains("st=" + b64("EPERM")) || b.contains("st=" + b64("ENOTDIR")));
        });
    }));
    ASSERT_TRUE(barrier());
    EXPECT_FALSE(std::any_of(replies.begin(), replies.end(), [](const auto& b) { return b.contains("ac=file;"); }));
}
// The mode of a new file and of a new directory comes from the umask when
// the service was made, on the GUI thread. Code that sets another umask for a
// moment (a socket that binds with mode 0600) does not change it.
TEST(TransferUmask, ComesFromTheGuiThread)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const mode_t previous = ::umask(022);
    const auto restore = qScopeGuard([&] { ::umask(previous); });
    FileTransfer service;
    QList<QByteArray> replies;
    QObject::connect(&service, &FileTransfer::reply, &service, [&](const QByteArray& b) { replies.append(b); });
    QObject::connect(&service, &FileTransfer::permissionRequested, &service,
        [&](const QString& token, bool, const QStringList&, int) { service.approve(token, true); });
    ::umask(0177);
    const QByteArray target = dir.filePath(QStringLiteral("new/f")).toUtf8().toBase64(QByteArray::OmitTrailingEquals);
    service.handle("id=u;ac=send");
    service.handle("id=u;ac=file;fid=f;n=" + target);
    service.handle("id=u;ac=end_data;fid=f;d=eA");
    ASSERT_TRUE(waitFor([&] {
        return std::any_of(replies.begin(), replies.end(),
            [](const auto& b) { return b.contains(";fid=f;") && b.contains("st=" + b64("OK")); });
    }));
    struct stat st { };
    ASSERT_EQ(::stat(QFile::encodeName(dir.filePath(QStringLiteral("new/f"))).constData(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0644u);
    ASSERT_EQ(::stat(QFile::encodeName(dir.filePath(QStringLiteral("new"))).constData(), &st), 0);
    EXPECT_EQ(st.st_mode & 0777, 0755u);
}
TEST_F(Transfer, MetadataNeverReachesTheTargetOfALink)
{
    // A file outside the destination, named by a symbolic link of the upload.
    QTemporaryDir outsideDir;
    const QString outside = outsideDir.filePath(QStringLiteral("outside"));
    {
        QFile file(outside);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("keep"), 4);
    }
    ASSERT_EQ(::chmod(QFile::encodeName(outside).constData(), 0600), 0);
    const timespec old[2] { { 1000000000, 0 }, { 1000000000, 0 } };
    ASSERT_EQ(::utimensat(AT_FDCWD, QFile::encodeName(outside).constData(), old, 0), 0);
    ASSERT_TRUE(start());
    send("ac=file;fid=l;ft=symlink;prm=511;mod=1700000000000000000;n=" + path("link"));
    send("ac=end_data;fid=l;d=" + b64("path:" + outside.toUtf8()));
    ASSERT_TRUE(fileStatus("l", "OK"));
    // A hard link to the link would be a second name for it, with the metadata of a file.
    send("ac=file;fid=h;ft=link;prm=511;mod=1700000000000000000;n=" + path("hard"));
    send("ac=end_data;fid=h;d=" + b64("l"));
    ASSERT_TRUE(fileStatus("h", "EINVAL"));
    send("ac=finish");
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
    struct stat st { };
    ASSERT_EQ(::stat(QFile::encodeName(outside).constData(), &st), 0);
    EXPECT_EQ(st.st_mode & 07777, 0600u);
    EXPECT_EQ(st.st_mtim.tv_sec, 1000000000);
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(QStringLiteral("hard"))));
}
TEST_F(Transfer, DataPastTheDeclaredSizeFailsWithEFBIG)
{
    ASSERT_TRUE(start());
    send("ac=file;fid=f;sz=2;n=" + path("f"));
    ASSERT_TRUE(fileStatus("f", "STARTED"));
    send("ac=end_data;fid=f;d=" + b64("too long"));
    ASSERT_TRUE(fileStatus("f", "EFBIG"));
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(QStringLiteral("f"))));
}
TEST_F(Transfer, TildeAloneIsTheHomeDirectory)
{
    const QByteArray home = qgetenv("HOME");
    const auto restore = qScopeGuard([&] { qputenv("HOME", home); });
    qputenv("HOME", QFile::encodeName(dir.path()));
    ASSERT_TRUE(start());
    send("ac=file;fid=d;ft=directory;n=" + b64("~"));
    ASSERT_TRUE(fileStatus("d", "OK"));
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(QStringLiteral("~"))));
}
TEST_F(Transfer, AQuietSessionGetsNoAnswerForAnInvalidField)
{
    send("ac=send;q=2");
    send("ac=file;fid=f;sz=many;n=" + path("f"));
    ASSERT_TRUE(barrier());
    EXPECT_FALSE(repliedFor("test"));
    // Refused before it could wait for a prompt.
    EXPECT_EQ(prompts, 0);
    // A session that is not quiet gets the answer.
    service.handle("id=loud;ac=send");
    service.handle("id=loud;ac=file;fid=f;sz=many;n=" + path("f"));
    ASSERT_TRUE(waitFor([&] {
        return std::any_of(replies.begin(), replies.end(),
            [](const auto& b) { return b.contains("id=loud;") && b.contains("st=" + b64("EINVAL")); });
    }));
    EXPECT_EQ(prompts, 0);
}
TEST_F(Transfer, ADenialKeepsAWaitingReceiveFromPrompting)
{
    // The receive session exists before the denial; its prompt comes after.
    send("ac=receive;sz=2");
    send("ac=file;fid=a;n=" + path("a"));
    service.handle("id=second;ac=send");
    service.handle("id=second;ac=file;fid=s;n=" + path("s"));
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    service.approve(token, false);
    ASSERT_TRUE(waitFor([&] {
        return std::any_of(replies.begin(), replies.end(),
            [](const auto& b) { return b.contains("id=second;") && b.contains("st=" + b64("EPERM")); });
    }));
    send("ac=file;fid=b;n=" + path("b"));
    ASSERT_TRUE(waitFor([&] {
        return std::any_of(replies.begin(), replies.end(),
            [](const auto& b) { return b.contains("id=test;") && b.contains("st=" + b64("EPERM")); });
    }));
    EXPECT_EQ(prompts, 1);
}
TEST_F(Transfer, PromptPathsShowHiddenCharactersAsEscapes)
{
    send("ac=receive;sz=1");
    // A line feed, a bidirectional override, a zero-width space and a tag character.
    send("ac=file;fid=a;n=" + b64(file("a\nb\u202Ec\u200Bd").toUtf8() + "\xf3\xa0\x80\x81"));
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    ASSERT_EQ(prompted.size(), 1);
    EXPECT_FALSE(prompted[0].contains(QLatin1Char('\n')));
    EXPECT_FALSE(prompted[0].contains(QChar(0x202e)));
    EXPECT_FALSE(prompted[0].contains(QChar(0x200b)));
    EXPECT_TRUE(prompted[0].endsWith(QStringLiteral("a\\u000ab\\u202ec\\u200bd\\U000e0001")));
}
TEST_F(Transfer, AListingLeavesOutTheSpecialFilesOfADirectory)
{
    // A pipe in the requested directory: a status for it would name the
    // request, and the client would fail the whole directory.
    QDir().mkpath(dir.filePath(QStringLiteral("tree")));
    {
        QFile file(dir.filePath(QStringLiteral("tree/a")));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    }
    ASSERT_EQ(::mkfifo(QFile::encodeName(dir.filePath(QStringLiteral("tree/pipe"))).constData(), 0600), 0);
    send("ac=receive;sz=1");
    send("ac=file;fid=req;n=" + path("tree"));
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    service.approve(token, true);
    const auto oks = [&] {
        return std::count_if(replies.begin(), replies.end(), [](const auto& b) { return b.contains("st=" + b64("OK")); });
    };
    ASSERT_TRUE(waitFor([&] { return oks() == 2; }));
    EXPECT_FALSE(std::any_of(replies.begin(), replies.end(), [](const auto& b) { return b.contains("ac=status") && b.contains("fid=req;"); }));
    const auto listed = [&](const char* name) {
        return std::any_of(replies.begin(), replies.end(), [&](const auto& b) { return b.contains("ac=file") && b.contains(";n=" + path(name) + ";"); });
    };
    EXPECT_TRUE(listed("tree/a"));
    EXPECT_FALSE(listed("tree/pipe"));
}
TEST_F(Transfer, ChildrenLeftOutOfAListingAreLoggedInOneLine)
{
    // A directory that can be read but not searched: the listing sees the
    // names of its children, and lstat() of each one fails, as for children
    // that went away since their directory was read.
    if (::geteuid() == 0)
        GTEST_SKIP() << "root searches any directory";
    const QString locked = dir.filePath(QStringLiteral("tree/locked"));
    ASSERT_TRUE(QDir().mkpath(locked));
    for (const char* name : { "a", "b", "c" }) {
        QFile file(locked + QLatin1Char('/') + QLatin1String(name));
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    }
    ASSERT_EQ(::chmod(QFile::encodeName(locked).constData(), 0400), 0);
    // QTemporaryDir cannot remove the children of a directory that it cannot search.
    struct Unlock {
        QByteArray path;
        ~Unlock() { ::chmod(path.constData(), 0700); }
    } unlock { QFile::encodeName(locked) };
    DesktopLog log;
    send("ac=receive;sz=1");
    send("ac=file;fid=req;n=" + path("tree"));
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    service.approve(token, true);
    ASSERT_TRUE(waitFor([&] {
        return std::count_if(replies.begin(), replies.end(), [](const auto& b) { return b.contains("st=" + b64("OK")); }) == 2;
    }));
    ASSERT_TRUE(barrier());
    EXPECT_EQ(log.count(QStringLiteral("left out")), 1);
    EXPECT_TRUE(log.contains(QStringLiteral("left out 3 entries")));
}
TEST_F(Transfer, RsyncCopyCannotWritePastTheDeclaredSize)
{
    QFile basis(dir.filePath("basis"));
    ASSERT_TRUE(basis.open(QIODevice::WriteOnly));
    basis.write(QByteArray(5000, 'a'));
    basis.close();
    ASSERT_TRUE(start());
    send("ac=file;fid=f;tt=rsync;sz=10;n=" + path("basis"));
    ASSERT_TRUE(fileStatus("f", "STARTED"));
    // One copy operation of 16 blocks of 71 bytes for a file declared as 10.
    QByteArray copy(13, '\0');
    copy[0] = 3;
    copy[9] = 15;
    send("ac=data;fid=f;d=" + b64(copy));
    EXPECT_TRUE(fileStatus("f", "EFBIG"));
}
TEST_F(Transfer, SmallFileWithLargeBlockSizeDeltaFinishes)
{
    QFile source(dir.filePath("source"));
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    source.write(QByteArray(200000, 'x'));
    source.close();
    send("ac=receive;sz=1");
    send("ac=file;fid=request;n=" + path("source"));
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    service.approve(token, true);
    ASSERT_TRUE(status("OK"));
    send("ac=file;tt=rsync;fid=r;n=" + path("source"));
    // A block size over kitty's 1 MiB is refused.
    QByteArray signature(12, '\0');
    signature[11] = 1;
    send("ac=end_data;fid=r;d=" + b64(signature));
    EXPECT_TRUE(fileStatus("r", "EINVAL"));
    send("ac=file;tt=rsync;fid=r2;n=" + path("source"));
    // The largest block size with no blocks: the whole file is one shrinking window.
    signature[11] = 0;
    signature[10] = 0x10;
    send("ac=end_data;fid=r2;d=" + b64(signature));
    EXPECT_TRUE(waitFor([&] {
        return std::any_of(replies.begin(), replies.end(),
            [](const auto& b) { return b.contains("ac=end_data;") && b.contains(";fid=r2;"); });
    }));
}
TEST_F(Transfer, LargeDeltaRebuildsTheFileFromBlocksAndLiterals)
{
    // The receiver's copy, and the file that differs from it: an insertion,
    // a changed run and more data than one delta step reads.
    QByteArray basis(512 * 1024, '\0');
    quint32 seed = 1;
    for (char& c : basis) {
        seed = seed * 1664525u + 1013904223u;
        c = char(seed >> 24);
    }
    QByteArray tail(1536 * 1024, '\0');
    for (char& c : tail) {
        seed = seed * 1664525u + 1013904223u;
        c = char(seed >> 24);
    }
    QByteArray changed = basis.left(200000) + "inserted" + basis.mid(200000);
    changed.replace(300000, 5000, QByteArray(5000, 'z'));
    changed += tail;
    QFile source(dir.filePath("source"));
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    source.write(changed);
    source.close();
    constexpr quint32 blockSize = 4096;
    QByteArray signature(12, '\0');
    qToLittleEndian(blockSize, signature.data() + 8);
    for (qsizetype at = 0, index = 0; at < basis.size(); at += blockSize, ++index) {
        const QByteArray block = basis.mid(at, blockSize);
        quint32 a = 0, b = 0;
        for (unsigned char c : block) {
            a += c;
            b += a;
        }
        QByteArray record(20, '\0');
        qToLittleEndian(quint64(index), record.data());
        qToLittleEndian((a & 0xffffu) | (b << 16), record.data() + 8);
        qToLittleEndian(quint64(XXH3_64bits(block.constData(), std::size_t(block.size()))), record.data() + 12);
        signature += record;
    }
    send("ac=receive;sz=1");
    send("ac=file;fid=request;n=" + path("source"));
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    service.approve(token, true);
    ASSERT_TRUE(status("OK"));
    send("ac=file;tt=rsync;fid=r;n=" + path("source"));
    for (qsizetype at = 0; at < signature.size(); at += 4096) {
        const bool last = at + 4096 >= signature.size();
        send(QByteArray(last ? "ac=end_data" : "ac=data") + ";fid=r;d=" + b64(signature.mid(at, 4096)));
    }
    ASSERT_TRUE(ended("r"));
    const QByteArray delta = received("r");
    // Apply the operations to the receiver's copy.
    QByteArray rebuilt;
    int blocks = 0;
    bool hashed = false;
    for (qsizetype at = 0; at < delta.size() && !hashed;) {
        const char op = delta[at];
        if (op == 0) {
            ASSERT_LE(at + 9, delta.size());
            const auto index = qFromLittleEndian<quint64>(delta.constData() + at + 1);
            rebuilt += basis.mid(qsizetype(index * blockSize), blockSize);
            ++blocks;
            at += 9;
        } else if (op == 1) {
            ASSERT_LE(at + 5, delta.size());
            const auto length = qFromLittleEndian<quint32>(delta.constData() + at + 1);
            rebuilt += delta.mid(at + 5, length);
            at += 5 + length;
        } else {
            ASSERT_EQ(op, 2);
            ASSERT_EQ(qFromLittleEndian<quint16>(delta.constData() + at + 1), 16);
            ASSERT_EQ(at + 19, delta.size());
            XXH128_canonical_t digest { };
            XXH128_canonicalFromHash(&digest, XXH3_128bits(changed.constData(), std::size_t(changed.size())));
            EXPECT_EQ(delta.mid(at + 3, 16), QByteArray(reinterpret_cast<const char*>(digest.digest), 16));
            hashed = true;
        }
    }
    EXPECT_TRUE(hashed);
    EXPECT_TRUE(rebuilt == changed);
    // Most of the basis travels as block references.
    EXPECT_GT(blocks, 100);
}
// A reset that the GUI thread queued behind a packet ends the packet's
// session: the packet must not create a file first.
TEST_F(Transfer, PacketQueuedBeforeAResetIsDropped)
{
    // A listing that keeps the worker busy while the packets and the reset queue up.
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("many")));
    for (int i = 0; i < 5000; ++i) {
        QFile entry(dir.filePath(QStringLiteral("many/%1").arg(i)));
        ASSERT_TRUE(entry.open(QIODevice::WriteOnly));
    }
    // The file is approved before: only the reset can keep its data from the disk.
    ASSERT_TRUE(start());
    send("ac=file;fid=f;n=" + path("late"));
    ASSERT_TRUE(fileStatus("f", "STARTED"));
    autoApprove = false;
    token.clear();
    service.handle("id=busy;ac=receive;sz=1");
    service.handle("id=busy;ac=file;fid=d;n=" + path("many"));
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    service.approve(token, true);
    send("ac=end_data;fid=f;d=" + b64("late"));
    service.reset();
    ASSERT_TRUE(waitFor([&] { return closed == 2; }));
    EXPECT_FALSE(QFile::exists(dir.filePath("late")));
    EXPECT_TRUE(QDir(dir.path()).entryList({ QStringLiteral(".afterglow-*") }, QDir::Files | QDir::Hidden).isEmpty());
}
// handle() stops the session's work as soon as it reads a cancel's id. A
// cancel that does not decode must still end the session with an answer.
TEST_F(Transfer, CancelThatDoesNotDecodeStillCancels)
{
    ASSERT_TRUE(start());
    send("ac=cancel;d=!!!");
    ASSERT_TRUE(status("CANCELED"));
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
    token.clear();
    ASSERT_TRUE(start());
    send("ac=cancel;ac=cancel");
    ASSERT_TRUE(waitFor([&] { return closed == 2; }));
}
// A file in /proc states st_size 0 and holds data. QFile::atEnd() is true
// before the first read, so a delta of it must end at an empty read.
TEST_F(Transfer, DeltaReadOfFileWithSizeZeroSendsItsContents)
{
    const QByteArray name = "/proc/self/status";
    struct stat st { };
    if (::stat(name.constData(), &st) || st.st_size != 0)
        GTEST_SKIP() << "needs a /proc file that states size 0";
    send("ac=receive;sz=1");
    send("ac=file;fid=req;n=" + b64(name));
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    service.approve(token, true);
    ASSERT_TRUE(status("OK"));
    ASSERT_TRUE(waitFor(
        [&] { return std::any_of(replies.begin(), replies.end(), [](auto b) { return b.contains("ac=file"); }); }));
    send("ac=file;tt=rsync;fid=r;n=" + b64(name));
    QByteArray signature(12, '\0');
    signature[8] = 64; // no records: the whole file goes as literals
    send("ac=end_data;fid=r;d=" + b64(signature));
    ASSERT_TRUE(ended("r"));
    const QByteArray delta = received("r");
    ASSERT_GE(delta.size(), 5);
    ASSERT_EQ(delta[0], 1); // a literal, not only the hash of an empty file
    const auto length = qFromLittleEndian<quint32>(delta.constData() + 1);
    EXPECT_GT(length, 0u);
    EXPECT_TRUE(delta.mid(5, length).startsWith("Name:"));
}
// kitten transfer sends no sz. Its delta uploads are accepted, and copy
// operations stop at the configured limit.
TEST_F(Transfer, DeltaUploadWithoutSizeStopsAtTheLimit)
{
    QFile basis(dir.filePath("basis"));
    ASSERT_TRUE(basis.open(QIODevice::WriteOnly));
    basis.write(QByteArray(5000, 'a'));
    basis.close();
    service.setDeltaLimit(1500);
    ASSERT_TRUE(start());
    send("ac=file;fid=f;tt=rsync;n=" + path("basis"));
    ASSERT_TRUE(fileStatus("f", "STARTED"));
    EXPECT_TRUE(std::any_of(replies.begin(), replies.end(), [](const auto& b) {
        return b.contains(";fid=f;") && b.contains("st=" + b64("STARTED")) && b.contains(";tt=rsync");
    }));
    // One copy operation of 16 blocks of 71 bytes: 1,136 bytes. The second passes the limit.
    QByteArray copy(13, '\0');
    copy[0] = 3;
    copy[9] = 15;
    send("ac=data;fid=f;d=" + b64(copy));
    ASSERT_TRUE(fileStatus("f", "PROGRESS"));
    send("ac=data;fid=f;d=" + b64(copy));
    ASSERT_TRUE(fileStatus("f", "EFBIG"));
    EXPECT_EQ(contents("basis"), QByteArray(5000, 'a'));
}
// Compressed data without a declared size stops at the same limit: a
// packet can inflate to a thousand times its size.
TEST_F(Transfer, CompressedUploadWithoutSizeStopsAtTheLimit)
{
    service.setDeltaLimit(100000);
    ASSERT_TRUE(start());
    send("ac=file;fid=f;zip=zlib;n=" + path("bomb"));
    ASSERT_TRUE(fileStatus("f", "STARTED"));
    const QByteArray plain(1000000, '\0');
    QByteArray zipped(int(compressBound(uLong(plain.size()))), '\0');
    uLongf size = uLongf(zipped.size());
    ASSERT_EQ(compress(reinterpret_cast<Bytef*>(zipped.data()), &size, reinterpret_cast<const Bytef*>(plain.constData()), uLong(plain.size())), Z_OK);
    zipped.resize(qsizetype(size));
    ASSERT_LE(zipped.size(), 4096);
    send("ac=end_data;fid=f;d=" + b64(zipped));
    ASSERT_TRUE(fileStatus("f", "EFBIG"));
    EXPECT_FALSE(QFileInfo::exists(file("bomb")));
    EXPECT_TRUE(temporaries().isEmpty());
}
// The signature of a small old file would be a hash of what may be a short
// secret, and a delta of it gains nothing: it is replaced whole.
TEST_F(Transfer, ASmallOldFileGetsNoDelta)
{
    QFile basis(dir.filePath("small"));
    ASSERT_TRUE(basis.open(QIODevice::WriteOnly));
    basis.write("1234");
    basis.close();
    ASSERT_TRUE(start());
    send("ac=file;fid=f;tt=rsync;n=" + path("small"));
    ASSERT_TRUE(fileStatus("f", "STARTED"));
    ASSERT_TRUE(barrier());
    EXPECT_TRUE(std::any_of(replies.begin(), replies.end(),
        [](const auto& b) { return b.contains(";fid=f;") && b.contains(";tt=simple"); }));
    EXPECT_FALSE(std::any_of(replies.begin(), replies.end(),
        [](const auto& b) { return b.contains(";fid=f;") && (b.contains("ac=data;") || b.contains("ac=end_data;")); }));
    send("ac=end_data;fid=f;d=" + b64("5678"));
    ASSERT_TRUE(fileStatus("f", "OK"));
    EXPECT_EQ(contents("small"), "5678");
}
// kitten marks the links of a compressed transfer with zip=zlib, and sends
// their targets plain: kitty reads them plain.
TEST_F(Transfer, ALinkOfACompressedTransferHasAPlainTarget)
{
    ASSERT_TRUE(start());
    send("ac=file;fid=l;ft=symlink;zip=zlib;n=" + path("link"));
    send("ac=end_data;fid=l;d=" + b64("path:" + file("target").toUtf8()));
    ASSERT_TRUE(fileStatus("l", "OK"));
    EXPECT_EQ(QFileInfo(file("link")).symLinkTarget(), file("target"));
}
TEST_F(Transfer, OnePromptAtATimeAndDenialCoolsDown)
{
    send("ac=send");
    send("ac=file;fid=f;n=" + path("f"));
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    service.handle("id=second;ac=send");
    ASSERT_TRUE(waitFor([&] {
        return std::any_of(replies.begin(), replies.end(),
            [](const auto& b) { return b.contains("id=second;") && b.contains("st=" + b64("EBUSY")); });
    }));
    service.approve(token, false);
    ASSERT_TRUE(status("EPERM"));
    service.handle("id=third;ac=send");
    ASSERT_TRUE(waitFor([&] {
        return std::any_of(replies.begin(), replies.end(),
            [](const auto& b) { return b.contains("id=third;") && b.contains("st=" + b64("EPERM")); });
    }));
    EXPECT_EQ(prompts, 1);
}
// A wrong password is refused, and no pane checks a password for a while:
// a program cannot guess one, nor tell a wrong one from a dialog.
TEST_F(Transfer, AWrongPasswordIsRefusedAndStopsPasswordChecks)
{
    // The pause is the process's: the tests after this one in the same process check passwords.
    const auto endPause = qScopeGuard([] { FileTransfer::endPasswordPause(); });
    service.setPassword(QStringLiteral("secret"));
    DesktopLog log;
    const auto pw = [](const QByteArray& id, const char* password) {
        return "pw=" + b64(QByteArray::fromStdString(AfterglowProto::transferPasswordHash(id.toStdString(), password))) + ";";
    };
    const auto refused = [&](QList<QByteArray>& from, const QByteArray& id) {
        return waitFor([&] {
            return std::any_of(from.begin(), from.end(),
                [&](const auto& b) { return b.contains("id=" + id + ";") && b.contains("st=" + b64("EPERM")); });
        });
    };
    service.handle("id=wrong;ac=send;" + pw("wrong", "guess"));
    ASSERT_TRUE(refused(replies, "wrong"));
    EXPECT_TRUE(log.contains(QStringLiteral("wrong password")));
    // The right password is not checked meanwhile, in this pane or another.
    service.handle("id=right;ac=send;" + pw("right", "secret"));
    ASSERT_TRUE(refused(replies, "right"));
    FileTransfer other;
    QList<QByteArray> otherReplies;
    QObject::connect(&other, &FileTransfer::reply, &other, [&](const QByteArray& b) { otherReplies.append(b); });
    other.setPassword(QStringLiteral("secret"));
    other.handle("id=elsewhere;ac=send;" + pw("elsewhere", "secret"));
    ASSERT_TRUE(refused(otherReplies, "elsewhere"));
    EXPECT_EQ(prompts, 0);
}
TEST_F(Transfer, PromptListsAtMostOneHundredPaths)
{
    send("ac=receive;sz=150");
    for (int i = 0; i < 150; ++i)
        send("ac=file;fid=r" + QByteArray::number(i) + ";n=" + b64(dir.filePath(QString::number(i)).toUtf8()));
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    // The list holds real paths only. The total counts the paths not shown.
    ASSERT_EQ(prompted.size(), 100);
    EXPECT_EQ(prompted.last(), dir.filePath(QStringLiteral("99")));
    EXPECT_EQ(promptedTotal, 150);
    // A Yes covers the listed requests. The others are refused, after the
    // permission: a client takes nothing before it.
    service.approve(token, true);
    ASSERT_TRUE(fileStatus("r120", "EPERM"));
    ASSERT_TRUE(fileStatus("r5", "ENOENT"));
    EXPECT_FALSE(std::any_of(replies.begin(), replies.end(),
        [](const auto& b) { return b.contains(";fid=r5;") && b.contains("st=" + b64("EPERM")); }));
    const auto first = std::find_if(replies.begin(), replies.end(), [](const auto& b) { return b.contains("id=test;"); });
    ASSERT_NE(first, replies.end());
    EXPECT_FALSE(first->contains("fid="));
    EXPECT_TRUE(first->contains("st=" + b64("OK")));
}
// The pane's reader posts each command to the GUI thread on its own. The
// prompt waits for those that it had posted before the first file waited.
TEST_F(Transfer, APromptWaitsForTheCommandsThatThePaneHadPosted)
{
    send("ac=send");
    ASSERT_TRUE(status("OK"));
    for (int i = 0; i < 50; ++i) {
        const QByteArray command = "id=test;ac=file;fid=f" + QByteArray::number(i) + ";n=" + b64(file("x").toUtf8() + QByteArray::number(i));
        QMetaObject::invokeMethod(&service, [this, command] { service.handle(command); }, Qt::QueuedConnection);
    }
    ASSERT_TRUE(waitFor([&] { return prompts == 1; }));
    EXPECT_EQ(prompted.size(), 50);
    EXPECT_EQ(promptedTotal, 50);
}
// A later No refuses the files of its own prompt. Those that came while it
// showed did not ask again: they get a prompt of their own, in the cooldown too.
TEST_F(Transfer, FilesThatCameDuringADeniedPromptGetAPromptOfTheirOwn)
{
    send("ac=send");
    send("ac=file;fid=a;n=" + path("a"));
    ASSERT_TRUE(waitFor([&] { return prompts == 1; }));
    service.approve(token, true);
    ASSERT_TRUE(fileStatus("a", "STARTED"));
    send("ac=file;fid=b;n=" + path("b"));
    ASSERT_TRUE(waitFor([&] { return prompts == 2; }));
    send("ac=file;fid=c;n=" + path("c"));
    ASSERT_TRUE(barrier());
    service.approve(token, false);
    ASSERT_TRUE(fileStatus("b", "EPERM"));
    ASSERT_TRUE(waitFor([&] { return prompts == 3; }));
    EXPECT_EQ(prompted, QStringList { file("c") });
    EXPECT_FALSE(std::any_of(replies.begin(), replies.end(), [](const auto& b) { return b.contains(";fid=c;"); }));
    // A file that comes after the No, in the cooldown, is refused.
    service.approve(token, true);
    ASSERT_TRUE(fileStatus("c", "STARTED"));
    send("ac=file;fid=d;n=" + path("d"));
    ASSERT_TRUE(fileStatus("d", "EPERM"));
    EXPECT_EQ(prompts, 3);
}
// The prompt after a later No opens in the cooldown once. A No of that
// prompt too leaves the files that came while it showed to the cooldown: a
// program that names a file during each prompt cannot keep one open.
TEST_F(Transfer, ANoOfThePromptAfterANoOpensNoOther)
{
    send("ac=send");
    send("ac=file;fid=a;n=" + path("a"));
    ASSERT_TRUE(waitFor([&] { return prompts == 1; }));
    service.approve(token, true);
    ASSERT_TRUE(fileStatus("a", "STARTED"));
    send("ac=file;fid=b;n=" + path("b"));
    ASSERT_TRUE(waitFor([&] { return prompts == 2; }));
    send("ac=file;fid=c;n=" + path("c"));
    ASSERT_TRUE(barrier());
    service.approve(token, false);
    ASSERT_TRUE(fileStatus("b", "EPERM"));
    ASSERT_TRUE(waitFor([&] { return prompts == 3; }));
    EXPECT_EQ(prompted, QStringList { file("c") });
    send("ac=file;fid=d;n=" + path("d"));
    ASSERT_TRUE(barrier());
    service.approve(token, false);
    ASSERT_TRUE(fileStatus("c", "EPERM"));
    ASSERT_TRUE(fileStatus("d", "EPERM"));
    ASSERT_TRUE(barrier());
    EXPECT_EQ(prompts, 3);
    // The session goes on: the file that the user approved finishes.
    send("ac=end_data;fid=a;d=" + b64("x"));
    ASSERT_TRUE(fileStatus("a", "OK"));
}
// A level that did not exist when the prompt showed, and that exists now as
// a directory, is part of the path that is read: not the file of the same
// name above it.
TEST_F(Transfer, AReadKeepsTheLevelsThatDidNotExistWhenThePromptShowed)
{
    {
        QFile other(file("f"));
        ASSERT_TRUE(other.open(QIODevice::WriteOnly));
        other.write("other");
    }
    send("ac=receive;sz=1");
    send("ac=file;fid=r;n=" + path("a/b/f"));
    ASSERT_TRUE(waitFor([&] { return prompts == 1; }));
    ASSERT_TRUE(QDir().mkpath(file("a/b")));
    {
        QFile shown(file("a/b/f"));
        ASSERT_TRUE(shown.open(QIODevice::WriteOnly));
        shown.write("shown");
    }
    service.approve(token, true);
    ASSERT_TRUE(waitFor([&] {
        return std::any_of(replies.begin(), replies.end(), [](const auto& b) { return b.contains("ac=file;"); });
    }));
    send("ac=file;fid=data;n=" + path("a/b/f"));
    ASSERT_TRUE(ended("data"));
    EXPECT_EQ(received("data"), "shown");
}
// answered() follows every handled command once, also when the packets
// that it waits for are dropped. The pane holds the terminal's reports until
// then, so one that is missing stops them for the life of the pane.
struct Answers : Transfer {
    int answering = 0, answered = 0;
    // Called with the number of each answer as it comes.
    std::function<void(int)> onAnswer;
    void SetUp() override
    {
        Transfer::SetUp();
        QObject::connect(&service, &FileTransfer::answering, &service, [this] { ++answering; });
        QObject::connect(&service, &FileTransfer::answered, &service, [this] {
            ++answered;
            if (onAnswer)
                onAnswer(answered);
        });
    }
    // A command whose status waits behind the delivery window: a cancel of
    // an unknown session. Returns its number.
    int probe()
    {
        service.handle("id=probe;ac=cancel");
        return answering;
    }
    // The GUI thread runs no events meanwhile: the worker fills its delivery
    // window, and packets wait in its queue.
    static void busy() { QThread::msleep(300); }
    // A download of a large file whose packets wait in the queue.
    bool download(const QByteArray& options = { })
    {
        QFile source(dir.filePath(QStringLiteral("large")));
        if (!source.open(QIODevice::WriteOnly))
            return false;
        QByteArray data(8 * 1024 * 1024, '\0');
        quint32 seed = 1;
        for (auto& c : data)
            c = char((seed = seed * 1664525u + 1013904223u) >> 24);
        source.write(data);
        source.close();
        send("ac=receive;sz=1" + options);
        send("ac=file;fid=request;n=" + path("large"));
        if (!waitFor([&] { return !token.isEmpty(); }))
            return false;
        service.approve(token, true);
        if (!waitFor([&] {
                return std::any_of(replies.begin(), replies.end(), [](const auto& b) { return b.contains("ac=file;"); });
            }))
            return false;
        send("ac=file;fid=data;n=" + path("large"));
        busy();
        // The premise: the download is under way, and its packets wait.
        QCoreApplication::processEvents();
        return !std::any_of(replies.begin(), replies.end(),
            [](const auto& b) { return b.contains("ac=end_data;") && b.contains(";fid=data;"); });
    }
    bool balanced()
    {
        waitFor([&] { return answered == answering; });
        return answered == answering;
    }
};
TEST_F(Answers, ACancelDuringADownloadAnswersEveryCommand)
{
    ASSERT_TRUE(download());
    send("ac=cancel");
    busy();
    ASSERT_TRUE(status("CANCELED"));
    EXPECT_TRUE(balanced()) << answering << " taken, " << answered << " answered";
}
TEST_F(Answers, AQuietCancelDuringADownloadAnswersEveryCommand)
{
    // The cancel of a quiet session queues no status of its own.
    ASSERT_TRUE(download(";q=2"));
    send("ac=cancel");
    busy();
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
    EXPECT_TRUE(balanced()) << answering << " taken, " << answered << " answered";
}
TEST_F(Answers, AnAbortDuringADownloadAnswersEveryCommand)
{
    ASSERT_TRUE(download());
    service.abort();
    busy();
    ASSERT_TRUE(status("ENOMEM"));
    EXPECT_TRUE(balanced()) << answering << " taken, " << answered << " answered";
}
TEST_F(Answers, ACommandOverTheLimitDuringADownloadAnswersEveryCommand)
{
    ASSERT_TRUE(download());
    service.handle("id=test;ac=data;x=" + QByteArray(qsizetype(AfterglowProto::TransferMaxCommand) + 1, 'a'));
    busy();
    ASSERT_TRUE(status("ENOMEM"));
    EXPECT_TRUE(balanced()) << answering << " taken, " << answered << " answered";
}
TEST_F(Answers, AResetDuringADownloadAnswersEveryCommand)
{
    ASSERT_TRUE(download());
    probe();
    service.reset();
    busy();
    // The reset answers the commands that waited for the packets it dropped.
    // Without that, the answer waits for a later delivery, which one pass
    // over the events posted so far does not reach.
    QCoreApplication::sendPostedEvents(&service, QEvent::MetaCall);
    EXPECT_EQ(answered, answering);
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
    EXPECT_TRUE(balanced()) << answering << " taken, " << answered << " answered";
}
TEST_F(Answers, AnAnswerFollowsThePacketsQueuedBeforeIt)
{
    ASSERT_TRUE(download());
    const int command = probe();
    std::optional<bool> replied;
    onAnswer = [&](int n) {
        if (n == command)
            replied = repliedFor("probe");
    };
    busy();
    ASSERT_TRUE(waitFor([&] { return replied.has_value(); }));
    EXPECT_TRUE(*replied);
    send("ac=cancel");
    EXPECT_TRUE(balanced()) << answering << " taken, " << answered << " answered";
}
TEST_F(Answers, AnAnswerDoesNotWaitForTheProgressThatReplacedItsOwn)
{
    ASSERT_TRUE(start());
    send("ac=file;fid=f;n=" + path("upload"));
    ASSERT_TRUE(fileStatus("f", "STARTED"));
    // Fill the delivery window, so that the progress statuses wait.
    for (int i = 0; i < 4000; ++i)
        send("ac=data;fid=f;d=" + b64("data"));
    send("ac=data;fid=f;d=" + b64("x"));
    const int first = answering;
    // Its progress status replaces that of the command before.
    send("ac=data;fid=f;d=" + b64("y"));
    const QByteArray last = "st=" + b64("PROGRESS") + ";sz=" + QByteArray::number(4000 * 4 + 2);
    std::optional<bool> passed;
    onAnswer = [&](int n) {
        if (n == first)
            passed = std::any_of(replies.begin(), replies.end(), [&](const auto& b) { return b.contains(last); });
    };
    busy();
    ASSERT_TRUE(waitFor([&] { return passed.has_value(); }));
    EXPECT_FALSE(*passed);
    send("ac=end_data;fid=f");
    ASSERT_TRUE(fileStatus("f", "OK"));
    EXPECT_TRUE(balanced()) << answering << " taken, " << answered << " answered";
}
TEST_F(Answers, ProgressThatReplacesAWaitingProgressAnswersEveryCommand)
{
    ASSERT_TRUE(start());
    send("ac=file;fid=f;n=" + path("upload"));
    ASSERT_TRUE(fileStatus("f", "STARTED"));
    // More progress statuses than the delivery window holds: the newest
    // replaces the one that waits.
    for (int i = 0; i < 4000; ++i)
        send("ac=data;fid=f;d=" + b64("data"));
    busy();
    send("ac=end_data;fid=f");
    ASSERT_TRUE(fileStatus("f", "OK"));
    EXPECT_TRUE(balanced()) << answering << " taken, " << answered << " answered";
}
TEST_F(Answers, CommandsRefusedWhileTheInputQueueIsFullAreAnswered)
{
    // A listing keeps the worker busy while more than its input queue of
    // 16 MiB waits for it.
    ASSERT_TRUE(QDir(dir.path()).mkpath(QStringLiteral("many")));
    for (int i = 0; i < 5000; ++i) {
        QFile entry(dir.filePath(QStringLiteral("many/%1").arg(i)));
        ASSERT_TRUE(entry.open(QIODevice::WriteOnly));
    }
    send("ac=receive;sz=1");
    send("ac=file;fid=d;n=" + path("many"));
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    service.approve(token, true);
    QByteArray command = "id=other;ac=data;fid=f;x=";
    command += QByteArray(qsizetype(AfterglowProto::TransferMaxCommand) - command.size(), 'a');
    // 1200 commands are more than the queue holds. A worker that keeps up
    // with them gets more, until the queue refuses one.
    const auto refused = [&] {
        return std::any_of(replies.begin(), replies.end(), [](const auto& b) { return b.contains("st=" + b64("ENOMEM")); });
    };
    for (int round = 0; round < 50 && !refused(); ++round) {
        for (int i = 0; i < 1200; ++i)
            service.handle(command);
        QElapsedTimer settle;
        settle.start();
        while (!refused() && settle.elapsed() < 100)
            QCoreApplication::processEvents();
    }
    ASSERT_TRUE(refused());
    EXPECT_TRUE(balanced()) << answering << " taken, " << answered << " answered";
}
// The descriptor limit of every pane's transfers is half of RLIMIT_NOFILE,
// fixed at its first use. These tests reach it, so they run in a process with
// a soft limit of 64 (afterglow_transfer_descriptor_limits), and skip in
// another one.
struct TransferLimits : Transfer {
    std::size_t limit = 0;
    void SetUp() override
    {
        rlimit nofile { };
        if (::getrlimit(RLIMIT_NOFILE, &nofile) != 0 || nofile.rlim_cur > 64)
            GTEST_SKIP() << "Needs a soft RLIMIT_NOFILE of 64 or less (ulimit -n 64)";
        limit = std::max<std::size_t>(nofile.rlim_cur / 2, 16);
        Transfer::SetUp();
    }
};
TEST_F(TransferLimits, ADeltaUploadWithNoRoomForItsDescriptorsIsRefusedBeforeItStarts)
{
    // A delta upload holds its output (two descriptors), its basis and the
    // signature stream. The file command checks for all four before it
    // opens any.
    QFile basis(dir.filePath(QStringLiteral("basis")));
    ASSERT_TRUE(basis.open(QIODevice::WriteOnly));
    ASSERT_EQ(basis.write(QByteArray(8192, 'b')), 8192);
    basis.close();
    ASSERT_TRUE(start());
    // Each upload with data holds two: its temporary file and its directory.
    // Two or three are left: room for the basis and the signature, not for
    // the output.
    const qsizetype open = (qsizetype(limit) - 2) / 2;
    for (qsizetype i = 0; i < open; ++i) {
        send("ac=file;fid=" + QByteArray::number(i) + ";n=" + b64(dir.filePath(QString::number(i)).toUtf8()));
        send("ac=data;fid=" + QByteArray::number(i) + ";d=" + b64("x"));
    }
    ASSERT_TRUE(fileStatus(QByteArray::number(open - 1), "PROGRESS"));
    send("ac=file;tt=rsync;fid=delta;n=" + path("basis"));
    ASSERT_TRUE(fileStatus("delta", "EBUSY"));
    ASSERT_TRUE(barrier());
    // A refusal is the file's only status.
    EXPECT_FALSE(std::any_of(replies.begin(), replies.end(),
        [](const auto& b) { return b.contains(";fid=delta;") && b.contains("st=" + b64("STARTED")); }));
}
TEST_F(TransferLimits, ADeltaReadAtTheLimitStreams)
{
    // The basis of a delta read becomes the descriptor of its stream.
    QFile source(dir.filePath(QStringLiteral("source")));
    ASSERT_TRUE(source.open(QIODevice::WriteOnly));
    ASSERT_EQ(source.write(QByteArray(8192, 's')), 8192);
    source.close();
    send("ac=receive;sz=1");
    send("ac=file;fid=request;n=" + path("source"));
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    service.approve(token, true);
    ASSERT_TRUE(status("OK"));
    for (qsizetype i = 0; i < qsizetype(limit) - 1; ++i)
        send("ac=file;tt=rsync;fid=r" + QByteArray::number(i) + ";n=" + path("source"));
    send("ac=file;tt=rsync;fid=last;n=" + path("source"));
    ASSERT_TRUE(barrier());
    ASSERT_FALSE(std::any_of(replies.begin(), replies.end(), [](const auto& b) { return b.contains("st=" + b64("EBUSY")); }));
    // A signature with no blocks: the delta is the whole file as literals.
    QByteArray signature(12, '\0');
    signature[8] = 64;
    send("ac=end_data;fid=last;d=" + b64(signature));
    ASSERT_TRUE(ended("last"));
    EXPECT_FALSE(std::any_of(replies.begin(), replies.end(),
        [](const auto& b) { return b.contains(";fid=last;") && b.contains("st=" + b64("EBUSY")); }));
}
// The client of afterglow-proto (aglow transfer) against this service, as
// they talk through a terminal: each OSC 5113 sequence of one side reaches
// the other side whole.
struct Client : Transfer {
    QByteArray noise(int size, quint32 seed)
    {
        QByteArray out(size, '\0');
        for (auto& c : out) {
            seed = seed * 1664525u + 1013904223u;
            c = char(seed >> 24);
        }
        return out;
    }
    void write(const QString& path, const QByteArray& bytes)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(bytes), bytes.size());
    }
    static ino_t inode(const QString& path)
    {
        struct stat st { };
        return ::stat(QFile::encodeName(path).constData(), &st) == 0 ? st.st_ino : 0;
    }
    static QByteArray read(const QString& path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }
    AfterglowProto::TransferClientOptions options()
    {
        AfterglowProto::TransferClientOptions o;
        o.home = QDir::homePath().toStdString();
        o.cwd = dir.path().toStdString();
        o.deltas = true;
        return o;
    }
    // Runs @p client until its session is over. A prompt is approved.
    bool run(AfterglowProto::TransferClient& client)
    {
        const int closedBefore = closed;
        qsizetype seen = 0;
        QElapsedTimer elapsed;
        elapsed.start();
        while (!client.over() && elapsed.elapsed() < 20000) {
            if (!token.isEmpty()) {
                service.approve(token, true);
                token.clear();
            }
            std::string out;
            client.pull(out, 256 * 1024);
            for (std::size_t at = 0; (at = out.find("\x1b]5113;", at)) != std::string::npos;) {
                const auto end = out.find("\x1b\\", at);
                if (end == std::string::npos)
                    break;
                service.handle(QByteArray(out.data() + at + 7, qsizetype(end - at - 7)));
                at = end + 2;
            }
            QCoreApplication::processEvents();
            if (out.empty())
                QThread::msleep(1);
            for (; seen < replies.size(); ++seen) {
                const QByteArray& bytes = replies[seen];
                for (qsizetype at = 0; (at = bytes.indexOf("\x1b]5113;", at)) >= 0;) {
                    const auto end = bytes.indexOf("\x1b\\", at);
                    if (end < 0)
                        break;
                    client.receive(std::string_view(bytes.constData() + at + 7, std::size_t(end - at - 7)));
                    at = end + 2;
                }
            }
        }
        // Nothing answers ac=finish, so a client is done when it sends it. The
        // worker then sets the directory metadata and closes the session.
        if (client.over() && client.state() == AfterglowProto::TransferClient::State::Done)
            return waitFor([&] { return closed > closedBefore; });
        return client.over();
    }
};
TEST_F(Client, SendsATreeWithDeltasToTheTerminal)
{
    service.setPassword(QStringLiteral("secret"));
    const QString source = dir.filePath(QStringLiteral("source"));
    const QByteArray big = noise(300000, 1) + QByteArray(200000, 'x');
    QByteArray changed = big;
    changed.insert(1000, "inserted");
    changed.append("tail");
    write(source + QStringLiteral("/big"), changed);
    write(source + QStringLiteral("/nested/small"), "small file");
    write(source + QStringLiteral("/nested/empty"), { });
    ASSERT_EQ(::symlink("big", QFile::encodeName(source + QStringLiteral("/link")).constData()), 0);
    ASSERT_EQ(::link(QFile::encodeName(source + QStringLiteral("/big")).constData(),
                  QFile::encodeName(source + QStringLiteral("/hard")).constData()),
        0);
    ASSERT_EQ(::chmod(QFile::encodeName(source + QStringLiteral("/nested")).constData(), 0750), 0);
    // The old copy on the terminal's side: the big file goes as a delta.
    const QString destination = dir.filePath(QStringLiteral("destination"));
    write(destination + QStringLiteral("/source/big"), big);
    auto o = options();
    o.password = "secret";
    std::string error;
    auto client = AfterglowProto::makeTransferSender(o, { source.toStdString(), destination.toStdString() + "/" }, error);
    ASSERT_TRUE(client) << error;
    ASSERT_TRUE(run(*client));
    // The password replaced the dialog.
    EXPECT_EQ(prompts, 0);
    EXPECT_EQ(client->state(), AfterglowProto::TransferClient::State::Done) << client->error();
    for (const auto& item : client->items())
        EXPECT_TRUE(item.error.empty()) << item.local << ": " << item.error;
    const QString copy = destination + QStringLiteral("/source");
    EXPECT_TRUE(read(copy + QStringLiteral("/big")) == changed);
    EXPECT_EQ(read(copy + QStringLiteral("/nested/small")), "small file");
    EXPECT_TRUE(QFileInfo::exists(copy + QStringLiteral("/nested/empty")));
    EXPECT_EQ(QFileInfo(copy + QStringLiteral("/link")).symLinkTarget(), copy + QStringLiteral("/big"));
    EXPECT_EQ(inode(copy + QStringLiteral("/hard")), inode(copy + QStringLiteral("/big")));
    struct stat nested { };
    ASSERT_EQ(::stat(QFile::encodeName(copy + QStringLiteral("/nested")).constData(), &nested), 0);
    EXPECT_EQ(nested.st_mode & 0777, 0750u);
    EXPECT_EQ(client->deltaFileBytes(), quint64(changed.size()));
    EXPECT_LT(client->deltaBytes(), quint64(changed.size() / 10));
}
TEST_F(Client, ReceivesATreeWithDeltasFromTheTerminal)
{
    const QString remote = dir.filePath(QStringLiteral("remote"));
    const QByteArray big = noise(400000, 2);
    QByteArray changed = big;
    changed.replace(200000, 5000, noise(5000, 3));
    write(remote + QStringLiteral("/big"), changed);
    write(remote + QStringLiteral("/nested/small"), "small file");
    write(remote + QStringLiteral("/nested/empty"), { });
    ASSERT_EQ(::symlink("nested/small", QFile::encodeName(remote + QStringLiteral("/link")).constData()), 0);
    // The terminal lists the first name of an inode that it finds as the
    // file and the other as a link: nested/small or hard, in either order.
    ASSERT_EQ(::link(QFile::encodeName(remote + QStringLiteral("/nested/small")).constData(),
                  QFile::encodeName(remote + QStringLiteral("/hard")).constData()),
        0);
    const qint64 modified = 1600000000;
    const timespec times[2] { { modified, 0 }, { modified, 0 } };
    ASSERT_EQ(::utimensat(AT_FDCWD, QFile::encodeName(remote + QStringLiteral("/nested")).constData(), times, 0), 0);
    const QString local = dir.filePath(QStringLiteral("local"));
    write(local + QStringLiteral("/remote/big"), big);
    std::string error;
    auto client = AfterglowProto::makeTransferReceiver(options(), { remote.toStdString(), local.toStdString() + "/" }, error);
    ASSERT_TRUE(client) << error;
    ASSERT_TRUE(run(*client));
    EXPECT_EQ(prompts, 1);
    EXPECT_EQ(client->state(), AfterglowProto::TransferClient::State::Done) << client->error();
    for (const auto& item : client->items())
        EXPECT_TRUE(item.error.empty()) << item.local << ": " << item.error;
    const QString copy = local + QStringLiteral("/remote");
    EXPECT_TRUE(read(copy + QStringLiteral("/big")) == changed);
    EXPECT_EQ(read(copy + QStringLiteral("/nested/small")), "small file");
    // An empty file gets no data packet, only the end.
    EXPECT_TRUE(QFileInfo::exists(copy + QStringLiteral("/nested/empty")));
    EXPECT_EQ(QFileInfo(copy + QStringLiteral("/link")).symLinkTarget(), copy + QStringLiteral("/nested/small"));
    EXPECT_EQ(inode(copy + QStringLiteral("/hard")), inode(copy + QStringLiteral("/nested/small")));
    EXPECT_EQ(QFileInfo(copy + QStringLiteral("/nested")).lastModified().toSecsSinceEpoch(), modified);
    EXPECT_EQ(client->deltaFileBytes(), quint64(changed.size()));
    EXPECT_LT(client->deltaBytes(), quint64(changed.size() / 10));
}
TEST_F(Client, CancelEndsTheSessionOnBothSides)
{
    write(dir.filePath(QStringLiteral("source/file")), noise(2000000, 4));
    std::string error;
    auto o = options();
    o.deltas = false;
    auto client = AfterglowProto::makeTransferSender(o,
        { dir.filePath(QStringLiteral("source/file")).toStdString(), dir.filePath(QStringLiteral("out")).toStdString() }, error);
    ASSERT_TRUE(client) << error;
    client->cancel("stopped");
    ASSERT_TRUE(run(*client));
    EXPECT_EQ(client->state(), AfterglowProto::TransferClient::State::Canceled);
    EXPECT_FALSE(QFileInfo::exists(dir.filePath(QStringLiteral("out"))));
    // The service ended its side too, and takes the next session.
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
    auto again = AfterglowProto::makeTransferSender(o,
        { dir.filePath(QStringLiteral("source/file")).toStdString(), dir.filePath(QStringLiteral("out")).toStdString() }, error);
    ASSERT_TRUE(again) << error;
    ASSERT_TRUE(run(*again));
    EXPECT_EQ(again->state(), AfterglowProto::TransferClient::State::Done) << again->error();
    EXPECT_TRUE(read(dir.filePath(QStringLiteral("out"))) == noise(2000000, 4));
}
// A receive of more files than the prompt lists: the client learns which
// sources were refused, after the permission, and stops before any file moves.
TEST_F(Client, AReceiveOfMoreSourcesThanThePromptListsNamesTheRefusedOnes)
{
    std::vector<std::string> arguments;
    for (int i = 0; i < 101; ++i) {
        const QString source = dir.filePath(QStringLiteral("remote/f%1").arg(i));
        write(source, "x");
        arguments.push_back(source.toStdString());
    }
    const QString local = dir.filePath(QStringLiteral("local"));
    ASSERT_TRUE(QDir().mkpath(local));
    arguments.push_back(local.toStdString() + "/");
    std::string error;
    auto client = AfterglowProto::makeTransferReceiver(options(), arguments, error);
    ASSERT_TRUE(client) << error;
    ASSERT_TRUE(run(*client));
    EXPECT_EQ(prompts, 1);
    EXPECT_NE(client->state(), AfterglowProto::TransferClient::State::Done);
    EXPECT_EQ(client->error().find("Unexpected"), std::string::npos) << client->error();
    EXPECT_NE(client->error().find("f100"), std::string::npos) << client->error();
    EXPECT_TRUE(QDir(local).isEmpty());
}
// aglow names its plain files at once: one prompt, or two when the worker
// asked before the last names came, covers them all.
TEST_F(Client, SendsManyFilesAfterOneOrTwoPrompts)
{
    const QString source = dir.filePath(QStringLiteral("many"));
    for (int i = 0; i < 40; ++i)
        write(source + QStringLiteral("/f%1").arg(i), noise(100, quint32(i)));
    const QString destination = dir.filePath(QStringLiteral("out"));
    auto o = options();
    o.deltas = false;
    std::string error;
    auto client = AfterglowProto::makeTransferSender(o, { source.toStdString(), destination.toStdString() + "/" }, error);
    ASSERT_TRUE(client) << error;
    ASSERT_TRUE(run(*client));
    EXPECT_EQ(client->state(), AfterglowProto::TransferClient::State::Done) << client->error();
    EXPECT_GE(prompts, 1);
    EXPECT_LE(prompts, 2);
    for (int i = 0; i < 40; ++i)
        EXPECT_TRUE(read(destination + QStringLiteral("/many/f%1").arg(i)) == noise(100, quint32(i))) << i;
}
// Delta uploads go in windows of 32: the files of a later window get
// prompts of their own, and the files that the user approved go on meanwhile.
TEST_F(Client, SendsMoreDeltasThanOneWindowWithPrompts)
{
    const QString source = dir.filePath(QStringLiteral("many"));
    const QString destination = dir.filePath(QStringLiteral("out"));
    for (int i = 0; i < 40; ++i) {
        write(source + QStringLiteral("/f%1").arg(i), noise(8000, quint32(i)));
        write(destination + QStringLiteral("/many/f%1").arg(i), noise(8000, quint32(i + 100)));
    }
    std::string error;
    auto client = AfterglowProto::makeTransferSender(options(), { source.toStdString(), destination.toStdString() + "/" }, error);
    ASSERT_TRUE(client) << error;
    ASSERT_TRUE(run(*client));
    EXPECT_EQ(client->state(), AfterglowProto::TransferClient::State::Done) << client->error();
    for (const auto& item : client->items())
        EXPECT_TRUE(item.error.empty()) << item.local << ": " << item.error;
    EXPECT_GE(prompts, 2);
    EXPECT_LT(prompts, 40);
    for (int i = 0; i < 40; ++i)
        EXPECT_TRUE(read(destination + QStringLiteral("/many/f%1").arg(i)) == noise(8000, quint32(i))) << i;
}
} // namespace
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--protocol-relay"))) {
        FileTransfer transfer;
        QObject::connect(&transfer, &FileTransfer::permissionRequested, &transfer,
            [&](const QString& token, bool, const QStringList&, int) { transfer.approve(token, true); });
        QObject::connect(&transfer, &FileTransfer::reply, &transfer, [](const QByteArray& bytes) {
            qsizetype offset = 0;
            while (offset < bytes.size()) {
                const auto n = ::write(STDOUT_FILENO, bytes.constData() + offset, std::size_t(bytes.size() - offset));
                if (n <= 0)
                    break;
                offset += n;
            }
        });
        // At the end of the input, the relay quits once the worker has
        // answered every command: the last ones can still be writing files.
        int unanswered = 0;
        bool ended = false;
        QObject::connect(&transfer, &FileTransfer::answering, &transfer, [&] { ++unanswered; });
        QObject::connect(&transfer, &FileTransfer::answered, &transfer, [&] {
            if (--unanswered == 0 && ended)
                app.quit();
        });
        QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
        QByteArray pending;
        QObject::connect(&input, &QSocketNotifier::activated, &transfer, [&] {
            char bytes[8192];
            const auto n = ::read(STDIN_FILENO, bytes, sizeof bytes);
            if (n <= 0) {
                input.setEnabled(false);
                ended = true;
                if (unanswered == 0)
                    app.quit();
                return;
            }
            pending.append(bytes, n);
            for (auto end = pending.indexOf('\n'); end >= 0; end = pending.indexOf('\n')) {
                transfer.handle(QByteArray::fromBase64(pending.left(end)));
                pending.remove(0, end + 1);
            }
        });
        return app.exec();
    }
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
