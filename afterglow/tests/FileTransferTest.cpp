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
    int closed = 0;
    void SetUp() override
    {
        QObject::connect(&service, &FileTransfer::reply, &service, [this](const QByteArray& b) { replies.append(b); });
        QObject::connect(&service, &FileTransfer::permissionRequested, &service,
            [this](const QString& t, bool, const QStringList&) { token = t; });
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
