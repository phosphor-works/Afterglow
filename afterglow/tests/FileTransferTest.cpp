// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop/FileTransfer.h"
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QThread>
#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>
using namespace Afterglow;
namespace {
QByteArray b64(const QByteArray& s) { return s.toBase64(QByteArray::OmitTrailingEquals); }
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
    int prompts = 0;
    int closed = 0;
    void SetUp() override
    {
        QObject::connect(&service, &FileTransfer::reply, &service, [this](const QByteArray& b) { replies.append(b); });
        QObject::connect(&service, &FileTransfer::permissionRequested, &service,
            [this](const QString& t, bool, const QStringList& paths) {
                token = t;
                prompted = paths;
                ++prompts;
            });
        QObject::connect(&service, &FileTransfer::sessionClosed, &service, [this] { ++closed; });
    }
    void send(QByteArray command) { service.handle("id=test;" + command); }
    bool status(const QByteArray& value)
    {
        return waitFor([&] {
            return std::any_of(replies.begin(), replies.end(), [&](auto b) { return b.contains("st=" + b64(value)); });
        });
    }
    bool start()
    {
        send("ac=send");
        if (!waitFor([&] { return !token.isEmpty(); }))
            return false;
        service.approve(token, true);
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
    QByteArray path(const char* name) { return b64(dir.filePath(QString::fromLatin1(name)).toUtf8()); }
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
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    service.approve(token, false);
    ASSERT_TRUE(status("EPERM"));
    send("ac=file;fid=f;n=" + path("denied"));
    ASSERT_TRUE(status("ENOENT"));
    EXPECT_FALSE(QFile::exists(dir.filePath("denied")));
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
    // A reply sent too early takes two queued hops and a worker tick to arrive.
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 50) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    EXPECT_TRUE(replies.isEmpty());
    service.approve(token, true);
    ASSERT_TRUE(status("OK"));
    ASSERT_TRUE(waitFor(
        [&] { return std::any_of(replies.begin(), replies.end(), [](auto b) { return b.contains("ac=file"); }); }));
    send("ac=file;fid=data;n=" + path("file"));
    ASSERT_TRUE(waitFor([&] {
        return std::any_of(replies.begin(), replies.end(),
            [](auto b) { return b.contains("ac=end_data") && b.contains(b64("source data")); });
    }));
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
TEST_F(Transfer, PendingUploadsAreBoundedAndCompletionReleasesCapacity)
{
    ASSERT_TRUE(start());
    for (int i = 0; i < 160; ++i) {
        const auto fid = QByteArray::number(i);
        send("ac=file;fid=" + fid + ";n=" + b64(dir.filePath(QString::number(i)).toUtf8()));
    }
    ASSERT_TRUE(fileStatus("159", "EBUSY"));
    send("ac=end_data;fid=0");
    ASSERT_TRUE(fileStatus("0", "OK"));
    send("ac=file;fid=next;n=" + path("next"));
    ASSERT_TRUE(fileStatus("next", "STARTED"));
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
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    service.reset();
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
    service.approve(token, true);
    send("ac=file;fid=f;n=" + path("file"));
    ASSERT_TRUE(status("ENOENT"));
    EXPECT_FALSE(QFile::exists(dir.filePath("file")));
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
    send("ac=finish");
    ASSERT_TRUE(waitFor([&] { return closed == 1; }));
    EXPECT_TRUE(fileStatus("a", "ELOOP"));
    EXPECT_TRUE(fileStatus("b", "ELOOP"));
    EXPECT_TRUE(fileStatus("c", "ENOENT"));
}
TEST_F(Transfer, RsyncCopyCannotWritePastTheDeclaredSize)
{
    QFile basis(dir.filePath("basis"));
    ASSERT_TRUE(basis.open(QIODevice::WriteOnly));
    basis.write(QByteArray(1000, 'a'));
    basis.close();
    ASSERT_TRUE(start());
    send("ac=file;fid=f;tt=rsync;sz=10;n=" + path("basis"));
    ASSERT_TRUE(fileStatus("f", "STARTED"));
    // One copy operation of 16 blocks: 1,000 bytes for a file declared as 10.
    QByteArray copy(13, '\0');
    copy[0] = 3;
    copy[9] = 15;
    send("ac=data;fid=f;d=" + b64(copy));
    EXPECT_TRUE(fileStatus("f", "EIO"));
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
    // The largest block size with no blocks: the whole file is one shrinking window.
    QByteArray signature(12, '\0');
    signature[10] = 0;
    signature[11] = 1;
    send("ac=end_data;fid=r;d=" + b64(signature));
    EXPECT_TRUE(waitFor([&] {
        return std::any_of(replies.begin(), replies.end(),
            [](const auto& b) { return b.contains("ac=end_data;") && b.contains(";fid=r;"); });
    }));
}
TEST_F(Transfer, OnePromptAtATimeAndDenialCoolsDown)
{
    send("ac=send");
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
TEST_F(Transfer, PromptListsAtMostOneHundredPaths)
{
    send("ac=receive;sz=150");
    for (int i = 0; i < 150; ++i)
        send("ac=file;fid=r" + QByteArray::number(i) + ";n=" + b64(dir.filePath(QString::number(i)).toUtf8()));
    ASSERT_TRUE(waitFor([&] { return !token.isEmpty(); }));
    ASSERT_EQ(prompted.size(), 101);
    EXPECT_TRUE(prompted.last().contains(QStringLiteral("50")));
}
} // namespace
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (app.arguments().contains(QStringLiteral("--protocol-relay"))) {
        FileTransfer transfer;
        QObject::connect(&transfer, &FileTransfer::permissionRequested, &transfer,
            [&](const QString& token, bool, const QStringList&) { transfer.approve(token, true); });
        QObject::connect(&transfer, &FileTransfer::reply, &transfer, [](const QByteArray& bytes) {
            qsizetype offset = 0;
            while (offset < bytes.size()) {
                const auto n = ::write(STDOUT_FILENO, bytes.constData() + offset, std::size_t(bytes.size() - offset));
                if (n <= 0)
                    break;
                offset += n;
            }
        });
        QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read);
        QByteArray pending;
        QObject::connect(&input, &QSocketNotifier::activated, &transfer, [&] {
            char bytes[8192];
            const auto n = ::read(STDIN_FILENO, bytes, sizeof bytes);
            if (n <= 0) {
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
