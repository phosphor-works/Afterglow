// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop/DragDrop.h"
#include <QDir>
#include <QDropEvent>
#include <QFile>
#include <QGuiApplication>
#include <QMimeData>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <gtest/gtest.h>
using namespace Afterglow;
namespace {
struct Dnd : testing::Test {
    DragDrop service;
    QList<QByteArray> replies;
    void SetUp() override
    {
        QObject::connect(&service, &DragDrop::reply, &service, [this](const QByteArray& b) { replies.append(b); });
    }
    bool contains(QByteArray text)
    {
        return std::any_of(replies.begin(), replies.end(), [&](auto b) { return b.contains(text); });
    }
    void enter(QMimeData& mime)
    {
        service.handle("t=a:i=42;");
        QTimer::singleShot(0, &service, [this] { service.handle("t=m:o=1;text/plain"); });
        QDropEvent event(QPointF(4, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        service.drop(&event, false, { 1, 1 }, { 4, 4 }, nullptr);
        ASSERT_TRUE(event.isAccepted());
    }
};
TEST_F(Dnd, QueryEchoAndUnsolicitedDragDenied)
{
    service.handle("t=q:i=123;");
    EXPECT_TRUE(contains("i=123"));
    EXPECT_TRUE(contains("t=q"));
    service.handle("t=o:o=1;text/plain");
    EXPECT_TRUE(contains("EPERM"));
    service.handle("t=e:y=0;YQ==");
    EXPECT_TRUE(contains("EINVAL"));
}
TEST_F(Dnd, ReadBeforeDropAndAfterLeaveDenied)
{
    service.handle("t=a;");
    service.handle("t=r:x=1;");
    EXPECT_TRUE(contains("EPERM"));
    replies.clear();
    QMimeData mime;
    mime.setText(QStringLiteral("private"));
    enter(mime);
    service.leave();
    EXPECT_TRUE(contains("x=-1"));
    service.handle("t=r:x=1;");
    EXPECT_TRUE(contains("EPERM"));
    EXPECT_FALSE(contains("cHJpdmF0ZQ=="));
}
TEST_F(Dnd, DropStreamsMimeAndFinishesWithClientAction)
{
    QMimeData mime;
    mime.setText(QStringLiteral("hello desktop"));
    enter(mime);
    QObject::connect(&service, &DragDrop::reply, &service, [this](const QByteArray& b) {
        if (b.contains("t=M"))
            QTimer::singleShot(0, &service, [this] { service.handle("t=r:x=1;"); });
        if (b.contains("t=r") && b.contains(";\x1b\\"))
            QTimer::singleShot(0, &service, [this] { service.handle("t=r:o=1;"); });
    });
    QDropEvent event(QPointF(4, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&event, true, { 1, 1 }, { 4, 4 }, nullptr);
    EXPECT_TRUE(event.isAccepted());
    EXPECT_TRUE(contains("aGVsbG8gZGVza3RvcA=="));
    EXPECT_TRUE(contains("i=42"));
}
TEST_F(Dnd, RemoteReceiverMustReadUrisBeforeFile)
{
    QMimeData mime;
    mime.setText(QStringLiteral("hello"));
    mime.setUrls({ QUrl::fromLocalFile(QStringLiteral("/etc/passwd")) });
    enter(mime);
    QObject::connect(&service, &DragDrop::reply, &service, [this](const QByteArray& b) {
        if (b.contains("t=M"))
            QTimer::singleShot(0, &service, [this] { service.handle("t=r:x=2:y=1;"); });
    });
    QDropEvent event(QPointF(4, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&event, true, { 1, 1 }, { 4, 4 }, nullptr);
    EXPECT_FALSE(event.isAccepted());
    EXPECT_TRUE(contains("EINVAL"));
}

TEST_F(Dnd, RemoteReceiverReadsDirectoryAndChildWithHandles)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    QFile file(dir.filePath(QStringLiteral("child")));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("remote child");
    file.close();
    QMimeData mime;
    mime.setText(QStringLiteral("hello"));
    mime.setUrls({ QUrl::fromLocalFile(dir.path()) });
    enter(mime);
    service.handle("t=a:x=1;1:remote-machine");
    int stage = 0;
    QObject::connect(&service, &DragDrop::reply, &service, [&](const QByteArray& b) {
        if (b.contains("t=M"))
            QTimer::singleShot(0, &service, [&] { service.handle("t=r:x=2;"); });
        if (b.contains("t=r") && b.contains(";\x1b\\")) {
            ++stage;
            QTimer::singleShot(0, &service, [&, step = stage] {
                if (step == 1)
                    service.handle("t=r:x=2:y=1;");
                if (step == 2)
                    service.handle("t=r:Y=2:x=1;");
                if (step == 3) {
                    service.handle("t=r:Y=2;");
                    service.handle("t=r:o=1;");
                }
            });
        }
    });
    QDropEvent event(QPointF(4, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&event, true, { 1, 1 }, { 4, 4 }, nullptr);
    EXPECT_TRUE(event.isAccepted());
    EXPECT_EQ(stage, 3);
    EXPECT_TRUE(contains("X=1"));
    EXPECT_TRUE(contains("X=2"));
    EXPECT_TRUE(contains("cmVtb3RlIGNoaWxk"));
}
TEST_F(Dnd, QueryReplyCanPrecedeDeviceAttributesWithoutGuiDispatch)
{
    EXPECT_EQ(DragDrop::queryReply("t=q:i=4294967295;"), QByteArray("\x1b]72;t=q:i=4294967295;\x1b\\"));
    EXPECT_TRUE(DragDrop::queryReply("t=q:i=4294967296;").isEmpty());
    EXPECT_TRUE(DragDrop::queryReply("t=o:x=1;").isEmpty());
}
TEST_F(Dnd, UnsignedMimeIndexCannotBecomeFinish)
{
    QMimeData mime;
    mime.setText(QStringLiteral("private"));
    enter(mime);
    QObject::connect(&service, &DragDrop::reply, &service, [this](const QByteArray& b) {
        if (b.contains("t=M"))
            QTimer::singleShot(0, &service, [this] { service.handle("t=r:x=4294967295;"); });
    });
    QDropEvent event(QPointF(4, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&event, true, { 1, 1 }, { 4, 4 }, nullptr);
    EXPECT_FALSE(event.isAccepted());
    EXPECT_TRUE(contains("EINVAL"));
}
TEST_F(Dnd, ChunkedEnableAndReset)
{
    service.handle("t=a:i=2:m=1;text/");
    service.handle("t=q:i=99;");
    EXPECT_TRUE(contains("i=99"));
    service.handle("m=0;plain");
    EXPECT_TRUE(service.acceptsDrops());
    service.reset();
    EXPECT_FALSE(service.acceptsDrops());
    EXPECT_FALSE(service.offersDrags());
}
} // namespace
int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
