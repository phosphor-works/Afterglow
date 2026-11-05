// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop/DragDrop.h"
#include "desktop/FileDrop.h"
#include <AfterglowProto/DndTree.h>
#include <AfterglowProto/DragDrop.h>
#include <QDir>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFile>
#include <QGuiApplication>
#include <QMimeData>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QWindow>
#include <QtGui/private/qguiapplication_p.h>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <gtest/gtest.h>
#include <pty.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
using namespace Afterglow;
namespace {
bool writeFile(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
// Runs the event loop until @p done or @p ms pass.
bool waitUntil(const std::function<bool()>& done, int ms = 10000)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!done() && elapsed.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}
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
    // With no delivery hook, a drag event takes no answer while it runs: the
    // enter goes out, the program answers, and the next move carries its answer.
    void enter(QMimeData& mime)
    {
        service.handle("t=a:i=42;");
        QDropEvent event(QPointF(4, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        service.drop(&event, false, { 1, 1 }, { 4, 4 });
        ASSERT_FALSE(event.isAccepted());
        service.handle("t=m:o=1;text/plain");
        QDropEvent move(QPointF(5, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        service.drop(&move, false, { 1, 1 }, { 5, 4 });
        ASSERT_TRUE(move.isAccepted());
    }
};
TEST_F(Dnd, KittenDropTargetIsAcceptedWithExplicitLastChunks)
{
    // What kitten dnd --drop writes: every message carries m=0, the machine id
    // comes in its own t=a:x=1 message, and the reply to the drop enter names
    // its type.
    service.handle("t=a:m=0;text/uri-list text/plain");
    service.handle("t=a:x=1:m=0;1:8a154d99010298a194728c7014eeaf5999124e99c90f607de9e1dc0cf4e26047");
    ASSERT_TRUE(service.acceptsDrops());
    QMimeData mime;
    mime.setText(QStringLiteral("dropped"));
    QDropEvent event(QPointF(4, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&event, false, { 1, 1 }, { 4, 4 });
    service.handle("t=m:o=1:m=0;text/plain");
    QDropEvent move(QPointF(5, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&move, false, { 1, 1 }, { 5, 4 });
    EXPECT_TRUE(move.isAccepted());
}
TEST_F(Dnd, DragEventsRunNoEventLoop)
{
    // A drag event that ran the event loop would handle the next drag event
    // inside it, and Qt would take the older answer last.
    service.handle("t=a;");
    bool ran = false;
    QTimer::singleShot(0, &service, [&] { ran = true; });
    QMimeData mime;
    mime.setText(QStringLiteral("text"));
    QDropEvent event(QPointF(4, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&event, false, { 1, 1 }, { 4, 4 });
    EXPECT_FALSE(ran);
    EXPECT_FALSE(event.isAccepted());
    EXPECT_TRUE(contains("t=m"));
}
TEST_F(Dnd, ADragEventTakesTheAnswersThatWaitFirst)
{
    // The pane delivers the program's commands that wait for the GUI thread
    // before a drag event answers.
    QByteArrayList waiting;
    service.setCommandDelivery([&](int) {
        for (const auto& command : std::exchange(waiting, {}))
            service.handle(command);
    });
    service.handle("t=a;");
    QMimeData mime;
    mime.setText(QStringLiteral("text"));
    QDropEvent event(QPointF(4, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&event, false, { 1, 1 }, { 4, 4 });
    EXPECT_FALSE(event.isAccepted());
    waiting.append("t=m:o=1;text/plain");
    QDropEvent move(QPointF(5, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&move, false, { 1, 1 }, { 5, 4 });
    EXPECT_TRUE(waiting.isEmpty());
    EXPECT_TRUE(move.isAccepted());
}
TEST_F(Dnd, ADragEventWaitsForTheAnswerToNewTypes)
{
    // A still pointer sends no more drag events: the event that gives the
    // program the types waits a short time for its answer, without an event loop.
    int waits = 0;
    bool answered = false;
    service.setCommandDelivery([&](int waitMs) {
        if (waitMs <= 0)
            return;
        ++waits;
        EXPECT_LE(waitMs, 100);
        if (!std::exchange(answered, true))
            service.handle("t=m:o=1;text/plain");
    });
    service.handle("t=a;");
    bool ran = false;
    QTimer::singleShot(0, &service, [&] { ran = true; });
    QMimeData mime;
    mime.setText(QStringLiteral("text"));
    QDropEvent event(QPointF(4, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&event, false, { 1, 1 }, { 4, 4 });
    EXPECT_EQ(waits, 1);
    EXPECT_FALSE(ran);
    EXPECT_TRUE(event.isAccepted());
    EXPECT_TRUE(service.acceptsDrag());
    // The same types again: the program has answered them, so no wait.
    QDropEvent move(QPointF(5, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&move, false, { 1, 1 }, { 5, 4 });
    EXPECT_EQ(waits, 1);
    EXPECT_TRUE(move.isAccepted());
}
TEST_F(Dnd, AcceptsDragTellsWhatTheLastEventAnswered)
{
    // The desktop learns an answer only from a drag event. An answer that
    // arrives between events is not yet the desktop's.
    service.handle("t=a;");
    QMimeData mime;
    mime.setText(QStringLiteral("text"));
    QDropEvent event(QPointF(4, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&event, false, { 1, 1 }, { 4, 4 });
    EXPECT_FALSE(event.isAccepted());
    service.handle("t=m:o=1;text/plain");
    EXPECT_FALSE(service.acceptsDrag());
    QDropEvent move(QPointF(5, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&move, false, { 1, 1 }, { 5, 4 });
    EXPECT_TRUE(service.acceptsDrag());
    service.handle("t=m:o=0;");
    EXPECT_TRUE(service.acceptsDrag());
    QDropEvent refused(QPointF(6, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&refused, false, { 1, 1 }, { 6, 4 });
    EXPECT_FALSE(refused.isAccepted());
    EXPECT_FALSE(service.acceptsDrag());
}
TEST_F(Dnd, TheAnswerIsCheckedAgainstEachEventsActions)
{
    // A drag can offer no action at first (a source that sets its actions
    // late). The program's answer must count against later events too.
    service.handle("t=a;");
    QMimeData mime;
    mime.setText(QStringLiteral("text"));
    QDropEvent none(QPointF(4, 4), Qt::IgnoreAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&none, false, { 1, 1 }, { 4, 4 });
    service.handle("t=m:o=1;text/plain");
    EXPECT_FALSE(service.acceptsDrag());
    QDropEvent copy(QPointF(5, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&copy, false, { 1, 1 }, { 5, 4 });
    EXPECT_TRUE(copy.isAccepted());
    EXPECT_TRUE(service.acceptsDrag());
    // A type the drag stops offering is not accepted.
    QMimeData html;
    html.setHtml(QStringLiteral("<b>x</b>"));
    QDropEvent other(QPointF(6, 4), Qt::CopyAction, &html, Qt::LeftButton, Qt::NoModifier);
    service.drop(&other, false, { 1, 1 }, { 6, 4 });
    EXPECT_FALSE(other.isAccepted());
}
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
    service.drop(&event, true, { 1, 1 }, { 4, 4 });
    EXPECT_TRUE(event.isAccepted());
    EXPECT_TRUE(contains("aGVsbG8gZGVza3RvcA=="));
    EXPECT_TRUE(contains("i=42"));
}
TEST_F(Dnd, ResetFromEndReplyReleasesReadBeforeCallback)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        QMimeData mime;
        mime.setText(QStringLiteral("reset at end"));
        enter(mime);
        bool reset = false;
        const auto connection = QObject::connect(&service, &DragDrop::reply, &service, [&](const QByteArray& b) {
            if (b.contains("t=M"))
                QTimer::singleShot(0, &service, [this] { service.handle("t=r:x=1;"); });
            if (b.contains("t=r") && b.contains(";\x1b\\")) {
                reset = true;
                service.reset();
            }
        });
        QDropEvent event(QPointF(4, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        service.drop(&event, true, { 1, 1 }, { 4, 4 });
        QObject::disconnect(connection);
        EXPECT_TRUE(reset);
        EXPECT_FALSE(event.isAccepted());
        EXPECT_FALSE(service.acceptsDrops());
    }
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
        // The error fails the one read. The program ends the drop.
        if (b.contains("t=R"))
            QTimer::singleShot(0, &service, [this] { service.handle("t=r;"); });
    });
    QDropEvent event(QPointF(4, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&event, true, { 1, 1 }, { 4, 4 });
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
    service.drop(&event, true, { 1, 1 }, { 4, 4 });
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
        if (b.contains("t=R"))
            QTimer::singleShot(0, &service, [this] { service.handle("t=r;"); });
    });
    QDropEvent event(QPointF(4, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&event, true, { 1, 1 }, { 4, 4 });
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
TEST_F(Dnd, RejectedFinalDropSendsLeave)
{
    service.handle("t=a;");
    QMimeData mime;
    mime.setText(QStringLiteral("unwanted"));
    QDropEvent event(QPointF(4, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&event, true, { 1, 1 }, { 4, 4 });
    EXPECT_FALSE(event.isAccepted());
    // Qt sends no leave event after a drop, so the client hears it here.
    EXPECT_TRUE(contains("t=m:x=-1:y=-1"));
}
TEST_F(Dnd, DesktopMimeNamesAreFilteredBeforeTheyReachTheProgram)
{
    QMimeData mime;
    mime.setText(QStringLiteral("hello"));
    mime.setData(QStringLiteral("evil/\x1b\\inject"), "x");
    mime.setData(QStringLiteral("spaced type/x"), "y");
    enter(mime);
    EXPECT_TRUE(contains("text/plain"));
    EXPECT_FALSE(contains("inject"));
    EXPECT_FALSE(contains("spaced"));
}
TEST_F(Dnd, ControlPayloadsAreBoundedAndReportedToTheirRole)
{
    service.handle("t=a;");
    service.handle("t=a:x=1;" + QByteArray(200, 'a'));
    EXPECT_TRUE(contains("t=R"));
    EXPECT_TRUE(contains("EINVAL"));
    replies.clear();
    service.handle("t=r:x=1;" + QByteArray(5000, 'a'));
    EXPECT_TRUE(contains("EFBIG"));
    EXPECT_FALSE(contains("t=E"));
    replies.clear();
    service.handle("t=m:m=1;" + QByteArray(4096, 'a'));
    for (int n = 0; n < 40 && !contains("EFBIG"); ++n)
        service.handle("m=1;" + QByteArray(4096, 'a'));
    EXPECT_TRUE(contains("EFBIG"));
    EXPECT_FALSE(contains("t=E"));
}
TEST_F(Dnd, OverlongCommandGetsEfbigAndEndsTheProtocol)
{
    service.handle("t=a;");
    ASSERT_TRUE(service.acceptsDrops());
    // A payload over one packet (4096 bytes) is an error, but the protocol goes on.
    service.handle("t=r:x=1;" + QByteArray(4097, 'a'));
    EXPECT_TRUE(contains("t=R"));
    EXPECT_TRUE(contains("EFBIG"));
    EXPECT_TRUE(service.acceptsDrops());
    replies.clear();
    // The terminal hands on the first 8193 bytes of a command over 8192.
    const QByteArray command = "t=r:x=1:" + QByteArray(8193 - 8, 'z');
    ASSERT_EQ(command.size(), 8193);
    service.handle(command);
    EXPECT_TRUE(contains("t=R"));
    EXPECT_TRUE(contains("EFBIG"));
    EXPECT_FALSE(service.acceptsDrops());
}
TEST_F(Dnd, OversizedCommandsGetNoReplyWithoutTheProtocol)
{
    // A program that uses no role, such as one that prints a binary file.
    service.handle("t=r:x=1;" + QByteArray(4097, 'a'));
    service.handle("t=e:y=0:" + QByteArray(8193 - 8, 'z'));
    service.handle(QByteArray(8193, 'z'));
    EXPECT_TRUE(replies.isEmpty());
    // A message that is being put together is the protocol in use.
    service.handle("t=o:x=0:m=1;text/");
    service.handle("m=0;" + QByteArray(4097, 'a'));
    EXPECT_TRUE(contains("t=E"));
    EXPECT_TRUE(contains("EFBIG"));
    replies.clear();
    // So is a role: a drag source hears of an overlong command, and the protocol ends.
    service.handle("t=o:x=1;");
    ASSERT_TRUE(service.offersDrags());
    service.handle("t=e:y=0:" + QByteArray(8193 - 8, 'z'));
    EXPECT_TRUE(contains("t=E"));
    EXPECT_TRUE(contains("EFBIG"));
    EXPECT_FALSE(service.offersDrags());
}
TEST_F(Dnd, ChunksOfAFailedMessageStartNoNewMessage)
{
    // A MIME list over the 128 KiB limit of control messages.
    service.handle("t=a:m=1;" + QByteArray(4096, 'a'));
    for (int n = 0; n < 40 && !contains("EFBIG"); ++n)
        service.handle("m=1;" + QByteArray(4096, 'a'));
    ASSERT_TRUE(contains("EFBIG"));
    replies.clear();
    // Its tail is dropped: no second error, and its last chunk enables nothing.
    for (int n = 0; n < 40; ++n)
        service.handle("m=1;" + QByteArray(4096, 'a'));
    service.handle("m=0;text/plain");
    EXPECT_TRUE(replies.isEmpty());
    EXPECT_FALSE(service.acceptsDrops());
    // A chunk that names another id ends the protocol, and the rest of its message goes too.
    service.handle("t=a:i=1:m=1;text/");
    service.handle("i=2:m=1;x");
    service.handle("m=0;plain");
    EXPECT_FALSE(service.acceptsDrops());
    // The next message is read again.
    service.handle("t=a;");
    EXPECT_TRUE(service.acceptsDrops());
}
TEST_F(Dnd, MalformedMetadataDecidesFromItsOwnMore)
{
    // A first packet that does not parse, with chunks after it: they start no new message.
    service.handle("t=a:i=bad:m=1;text/");
    service.handle("m=0;plain");
    EXPECT_FALSE(service.acceptsDrops());
    // The last chunk of a message does not parse: the next message is read.
    service.handle("t=a:m=1;text/");
    service.handle("i=bad:m=0;plain");
    service.handle("t=a;");
    EXPECT_TRUE(service.acceptsDrops());
}
TEST_F(Dnd, ARefusedReplyDropsTheRestOfTheMessage)
{
    // The pane ends the protocol when it cannot write a reply.
    const auto connection = QObject::connect(&service, &DragDrop::reply, &service, [this] { service.end(); });
    service.handle("t=a:m=1;text/");
    service.handle("t=q;");
    ASSERT_TRUE(contains("t=q"));
    service.handle("m=0;plain");
    EXPECT_FALSE(service.acceptsDrops());
    service.handle("t=a;");
    EXPECT_TRUE(service.acceptsDrops());
    QObject::disconnect(connection);
    // A terminal reset ends the message too: what follows is a new one, here an untyped t=a.
    service.handle("t=A;");
    service.handle("t=a:m=1;text/");
    service.reset();
    service.handle("m=0;plain");
    EXPECT_TRUE(service.acceptsDrops());
}
TEST_F(Dnd, AnAbortDropsPacketsToTheNextLastChunk)
{
    service.handle("t=a;");
    // Packets were lost in a message: its rest enables nothing.
    service.handle("t=A:m=1;");
    service.abort(DragDrop::Lost::Packets);
    service.handle("m=1;text/");
    service.handle("m=0;plain");
    EXPECT_FALSE(service.acceptsDrops());
    service.handle("t=a;");
    EXPECT_TRUE(service.acceptsDrops());
    // The lost packets held the first one of a message.
    service.abort(DragDrop::Lost::Packets);
    service.handle("m=0;text/plain");
    EXPECT_FALSE(service.acceptsDrops());
    service.handle("t=a;");
    EXPECT_TRUE(service.acceptsDrops());
}
TEST_F(Dnd, AnAbortWithoutLostPacketsKeepsTheNextMessage)
{
    // The pane dropped only file transfer commands: the protocol is whole.
    service.handle("t=a;");
    service.abort(DragDrop::Lost::Nothing);
    EXPECT_TRUE(service.acceptsDrops());
    // A message that was being put together keeps the rest of its chunks: its last one ends it.
    service.handle("t=A:m=1;");
    service.abort(DragDrop::Lost::Nothing);
    service.handle("m=0;");
    EXPECT_FALSE(service.acceptsDrops());
    service.handle("t=a;");
    EXPECT_TRUE(service.acceptsDrops());
}
TEST_F(Dnd, AnAbortAfterALostResetSendsNoError)
{
    // The lost reset ended the drop; the lost packets came after it.
    QMimeData mime;
    mime.setText(QStringLiteral("text"));
    enter(mime);
    QDropEvent drop(QPointF(5, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QTimer::singleShot(0, &service, [&] { service.abort(DragDrop::Lost::Packets, true); });
    service.drop(&drop, true, { 1, 1 }, { 5, 4 });
    EXPECT_FALSE(contains("ENOMEM"));
    EXPECT_FALSE(drop.isAccepted());
}
TEST_F(Dnd, AnAbortThatLostAResetStartsANewMessage)
{
    // The reset ended the message that was being put together, and every
    // dropped OSC 72 packet came before it.
    service.handle("t=a;");
    service.handle("t=A:m=1;");
    service.abort(DragDrop::Lost::Reset);
    EXPECT_FALSE(service.acceptsDrops());
    service.handle("t=a;");
    EXPECT_TRUE(service.acceptsDrops());
}
TEST_F(Dnd, ChunkedStatusIsReadAsOneList)
{
    for (const auto* last : { "plain", "html" }) {
        service.reset();
        service.handle("t=a:i=42;");
        QMimeData mime;
        mime.setText(QStringLiteral("hello"));
        QDropEvent event(QPointF(4, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        service.drop(&event, false, { 1, 1 }, { 4, 4 });
        service.handle("t=m:o=1:m=1;text/");
        service.handle(QByteArray("m=0;") + last);
        QDropEvent move(QPointF(5, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        service.drop(&move, false, { 1, 1 }, { 5, 4 });
        // The source offers text/plain only.
        EXPECT_EQ(move.isAccepted(), QByteArray(last) == "plain") << last;
        service.leave();
    }
}
TEST_F(Dnd, AResetFromOnePacketStopsTheRestOfItsMessage)
{
    service.handle("t=a;");
    QMimeData mime;
    // About 9 KB of MIME names: three packets.
    for (int n = 0; n < 40; ++n)
        mime.setData(QStringLiteral("application/x-long-%1-").arg(n) + QString(200, QLatin1Char('a')), "x");
    int packets = 0;
    QObject::connect(&service, &DragDrop::reply, &service, [&](const QByteArray& b) {
        if (!b.contains("t=m"))
            return;
        ++packets;
        // The pane ends the protocol when it cannot write a reply.
        service.end();
    });
    QDropEvent event(QPointF(4, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&event, false, { 1, 1 }, { 4, 4 });
    EXPECT_EQ(packets, 1);
    EXPECT_FALSE(event.isAccepted());
}
TEST_F(Dnd, FileUrlWithRemoteHostIsNotRead)
{
    QMimeData mime;
    mime.setText(QStringLiteral("hello"));
    mime.setUrls({ QUrl(QStringLiteral("file://server/etc/passwd")) });
    enter(mime);
    int stage = 0;
    QObject::connect(&service, &DragDrop::reply, &service, [&](const QByteArray& b) {
        if (b.contains("t=M"))
            QTimer::singleShot(0, &service, [&] { service.handle("t=r:x=2;"); });
        if (b.contains("t=r") && b.contains(";\x1b\\") && ++stage == 1)
            QTimer::singleShot(0, &service, [&] { service.handle("t=r:x=2:y=1;"); });
        if (b.contains("t=R"))
            QTimer::singleShot(0, &service, [&] { service.handle("t=r;"); });
    });
    QDropEvent event(QPointF(4, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&event, true, { 1, 1 }, { 4, 4 });
    EXPECT_FALSE(event.isAccepted());
    EXPECT_TRUE(contains("EUNKNOWN"));
}
TEST_F(Dnd, AnUnreadableEntryFailsOnlyItsRead)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(writeFile(dir.filePath(QStringLiteral("present")), "still here"));
    QMimeData mime;
    mime.setText(QStringLiteral("hello"));
    mime.setUrls({ QUrl::fromLocalFile(dir.filePath(QStringLiteral("missing"))),
        QUrl::fromLocalFile(dir.filePath(QStringLiteral("present"))) });
    enter(mime);
    int ends = 0;
    QObject::connect(&service, &DragDrop::reply, &service, [&](const QByteArray& b) {
        if (b.contains("t=M"))
            QTimer::singleShot(0, &service, [&] { service.handle("t=r:x=2;"); });
        // The missing file fails its read, and the drop goes on to the next one.
        if (b.contains("t=R"))
            QTimer::singleShot(0, &service, [&] { service.handle("t=r:x=2:y=2;"); });
        if (b.contains("t=r") && b.contains(";\x1b\\")) {
            QTimer::singleShot(0, &service, [&, step = ++ends] {
                service.handle(step == 1 ? "t=r:x=2:y=1;" : "t=r:o=1;");
            });
        }
    });
    QDropEvent event(QPointF(4, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&event, true, { 1, 1 }, { 4, 4 });
    EXPECT_TRUE(contains("ENOENT"));
    EXPECT_TRUE(contains(QByteArray("still here").toBase64()));
    EXPECT_EQ(ends, 2);
    EXPECT_TRUE(event.isAccepted());
}
TEST_F(Dnd, AListingNamesTheBytesOnDiskAndLeavesOutNamesThatAreNotUtf8)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString tree = dir.filePath(QStringLiteral("tree"));
    ASSERT_TRUE(QDir().mkpath(tree));
    ASSERT_TRUE(writeFile(tree + QStringLiteral("/ok"), "fine"));
    // Two names that are not UTF-8. As QStrings both would read as U+FFFD,
    // the same name twice, and the receiver would refuse the whole listing.
    const QByteArray raw = QFile::encodeName(tree);
    for (const char* name : { "/\xff", "/\xfe" }) {
        const int fd = ::open((raw + name).constData(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0)
            GTEST_SKIP() << "the file system refuses names that are not UTF-8";
        ::close(fd);
    }
    QMimeData mime;
    mime.setText(QStringLiteral("hello"));
    mime.setUrls({ QUrl::fromLocalFile(tree) });
    enter(mime);
    service.handle("t=a:x=1;1:remote-machine");
    QByteArray listing;
    int ends = 0;
    QObject::connect(&service, &DragDrop::reply, &service, [&](const QByteArray& b) {
        if (b.contains("t=M"))
            QTimer::singleShot(0, &service, [&] { service.handle("t=r:x=2;"); });
        if (b.contains("t=r") && b.contains("X=2") && !b.contains(";\x1b\\"))
            listing += b.mid(b.lastIndexOf(';') + 1).chopped(2);
        if (b.contains("t=r") && b.contains(";\x1b\\")) {
            QTimer::singleShot(0, &service, [&, step = ++ends] {
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
    service.drop(&event, true, { 1, 1 }, { 4, 4 });
    EXPECT_EQ(QByteArray::fromBase64(listing), QByteArray("ok"));
    // The child is read by its name on disk.
    EXPECT_TRUE(contains(QByteArray("fine").toBase64()));
    EXPECT_FALSE(contains("ENOENT"));
    EXPECT_TRUE(event.isAccepted());
}
TEST(DragTrees, EachFinishedTreeRemainsForItsOwnLifetime)
{
    auto& trees = DragTrees::instance();
    const auto before = trees.size();
    trees.setLifetime(std::chrono::milliseconds(100));
    auto first = trees.create();
    ASSERT_TRUE(first);
    const QString firstPath = first->path();
    trees.finish(first);
    // Only the process holds the tree now, as after the pane that made it closed.
    first.reset();
    // The next drag starts while a drop target may still copy from the first tree.
    trees.setLifetime(std::chrono::seconds(30));
    auto second = trees.create();
    ASSERT_TRUE(second);
    EXPECT_TRUE(QFileInfo::exists(firstPath));
    trees.finish(second);
    EXPECT_TRUE(waitUntil([&] { return !QFileInfo::exists(firstPath); }));
    EXPECT_TRUE(QFileInfo::exists(second->path()));
    trees.remove(second);
    const QString secondPath = second->path();
    second.reset();
    EXPECT_FALSE(QFileInfo::exists(secondPath));
    EXPECT_EQ(trees.size(), before);
    trees.setLifetime(DragTrees::DefaultLifetime);
}
TEST(DragTrees, HoldsOnlyFilesInsideATree)
{
    auto& trees = DragTrees::instance();
    auto tree = trees.create();
    ASSERT_TRUE(tree);
    const QByteArray root = QFile::encodeName(tree->path());
    ASSERT_TRUE(QDir().mkpath(tree->filePath(QStringLiteral("1/dir"))));
    ASSERT_TRUE(writeFile(tree->filePath(QStringLiteral("1/dir/file")), "x"));
    ASSERT_EQ(::symlink("/etc", (root + "/1/out").constData()), 0);
    EXPECT_TRUE(trees.holds(root + "/1/dir"));
    EXPECT_TRUE(trees.holds(root + "/1/dir/file"));
    // The link is inside, and a read sends its target. What it points to is not.
    EXPECT_TRUE(trees.holds(root + "/1/out"));
    EXPECT_FALSE(trees.holds(root + "/1/out/passwd"));
    EXPECT_FALSE(trees.holds(root + "/1/.."));
    EXPECT_FALSE(trees.holds(root));
    EXPECT_FALSE(trees.holds("/etc/passwd"));
    trees.remove(tree);
    EXPECT_FALSE(trees.holds(root + "/1/dir/file"));
}
// A drag that the program in the pane starts. The offscreen platform ends
// each drag at once without a drop, as a drag that the user canceled.
struct DndSource : Dnd {
    QWindow window;
    QTemporaryDir temp;
    QByteArray oldTemp;
    void SetUp() override
    {
        Dnd::SetUp();
        if (QGuiApplication::platformName() != QLatin1String("offscreen"))
            GTEST_SKIP() << "a drag ends at once only on the offscreen platform";
        ASSERT_TRUE(temp.isValid());
        // The trees of remote drags go into the test's own directory.
        oldTemp = qgetenv("TMPDIR");
        qputenv("TMPDIR", QFile::encodeName(temp.path()));
        QGuiApplicationPrivate::mouse_buttons = Qt::LeftButton;
    }
    void TearDown() override
    {
        QGuiApplicationPrivate::mouse_buttons = Qt::NoButton;
        if (oldTemp.isEmpty())
            qunsetenv("TMPDIR");
        else
            qputenv("TMPDIR", oldTemp);
    }
    bool tempEmpty() { return QDir(temp.path()).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden); }
    // The program offers @p types after a press and a move, from a computer with @p machine.
    void offer(const QByteArray& machine, const QByteArray& types)
    {
        service.handle("t=o:x=1;" + machine);
        service.press({ 0, 0 });
        ASSERT_TRUE(service.move(&window, { 40, 0 }, { 1, 1 }, { 40, 0 }));
        service.handle("t=o:o=3;" + types);
        ASSERT_FALSE(contains("t=E"));
    }
    void data(int index, const QByteArray& bytes)
    {
        service.handle("t=p:x=" + QByteArray::number(index) + ";" + bytes.toBase64());
        service.handle("t=p:x=" + QByteArray::number(index) + ";");
    }
    void startDrag() { service.handle("t=P:x=-1;"); }
};
TEST_F(DndSource, ACanceledDragNamesNoAction)
{
    offer(QByteArray::fromStdString(AfterglowProto::dndMachineId()), "text/plain");
    data(0, "dragged text");
    startDrag();
    ASSERT_TRUE(waitUntil([&] { return contains("x=4"); }));
    // y=1: no drop. o=0: no action, so a source deletes nothing.
    EXPECT_TRUE(contains("o=0:t=e:x=4:y=1"));
}
TEST_F(DndSource, ACanceledRemoteDragRemovesItsTreeAtOnce)
{
    offer("1:remote-machine", "text/uri-list");
    data(0, "file:///home/someone/notes.txt\r\n");
    bool made = false;
    QObject::connect(&service, &DragDrop::reply, &service, [&](const QByteArray& b) {
        if (!b.contains("t=k"))
            return;
        made = !tempEmpty();
        QTimer::singleShot(0, &service, [&] {
            service.handle("t=k:x=1:X=0;" + QByteArray("remote notes").toBase64());
            service.handle("t=k:x=1:X=0;");
        });
    });
    startDrag();
    ASSERT_TRUE(waitUntil([&] { return contains("x=4"); }));
    EXPECT_TRUE(made);
    EXPECT_TRUE(contains("o=0:t=e:x=4:y=1"));
    EXPECT_TRUE(tempEmpty());
}
TEST_F(DndSource, ATreeWithMoreDirectoriesThanOpenHandlesIsCopied)
{
    offer("1:remote-machine", "text/uri-list");
    data(0, "file:///home/someone/top\r\n");
    // 300 nested directories, over the 256 open handles. Each one closes
    // when its one child is done, so no more than two are open at once.
    constexpr int Depth = 300;
    QByteArrayList packets { "t=k:x=1:X=2;" + QByteArray("d").toBase64(), "t=k:x=1:X=2;" };
    for (int level = 0; level < Depth; ++level) {
        const QByteArray meta = "t=k:x=1:Y=" + QByteArray::number(2 + level) + ":y=1:X=" + QByteArray::number(3 + level) + ";";
        if (level + 1 < Depth)
            packets << meta + QByteArray("d").toBase64();
        packets << meta;
    }
    QObject::connect(&service, &DragDrop::reply, &service, [&](const QByteArray& b) {
        if (b.contains("t=k"))
            QTimer::singleShot(0, &service, [&] {
                for (const auto& packet : packets)
                    service.handle(packet);
            });
    });
    startDrag();
    // The drag started: the whole tree arrived.
    ASSERT_TRUE(waitUntil([&] { return contains("x=4") || contains("EINVAL") || contains("EMFILE"); }));
    EXPECT_FALSE(contains("EINVAL"));
    EXPECT_FALSE(contains("EMFILE"));
    EXPECT_TRUE(contains("x=4"));
}
TEST_F(DndSource, ANegativeKindIsNoEntry)
{
    EXPECT_FALSE(AfterglowProto::dndIsDirectoryKind(-1));
    EXPECT_FALSE(AfterglowProto::dndIsDirectoryKind(AfterglowProto::DndLinkKind));
    EXPECT_TRUE(AfterglowProto::dndIsDirectoryKind(AfterglowProto::DndFirstHandle));
    offer("1:remote-machine", "text/uri-list");
    data(0, "file:///home/someone/top\r\n");
    QObject::connect(&service, &DragDrop::reply, &service, [&](const QByteArray& b) {
        if (b.contains("t=k"))
            QTimer::singleShot(0, &service, [&] { service.handle("t=k:x=1:X=-1;"); });
    });
    startDrag();
    ASSERT_TRUE(waitUntil([&] { return contains("x=4") || contains("EINVAL"); }));
    EXPECT_TRUE(contains("EINVAL"));
    EXPECT_FALSE(contains("x=4"));
    EXPECT_TRUE(tempEmpty());
}
TEST_F(DndSource, AProgramThatCancelsTheCopyCanOfferAgainInTheSamePress)
{
    offer("1:remote-machine", "text/uri-list");
    data(0, "file:///home/someone/notes.txt\r\n");
    QObject::connect(&service, &DragDrop::reply, &service, [&](const QByteArray& b) {
        if (b.contains("t=k"))
            QTimer::singleShot(0, &service, [&] {
                service.handle("t=k:x=1:X=0;" + QByteArray("part").toBase64());
                service.handle("t=E;");
            });
    });
    startDrag();
    ASSERT_TRUE(waitUntil([&] { return contains("t=k") && tempEmpty(); }));
    // The press goes on: the next move is a new gesture, and its offer is taken.
    replies.clear();
    ASSERT_TRUE(service.move(&window, { 60, 0 }, { 2, 1 }, { 60, 0 }));
    service.handle("t=o:o=3;text/plain");
    EXPECT_FALSE(contains("EPERM"));
}
TEST_F(DndSource, AStartAfterAResetSendsNothing)
{
    offer(QByteArray::fromStdString(AfterglowProto::dndMachineId()), "text/plain");
    data(0, "dragged text");
    startDrag();
    // The program ended before the queued start ran. The shell reads what comes now.
    service.reset();
    replies.clear();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    EXPECT_TRUE(replies.isEmpty());
}
TEST_F(DndSource, ARefusedRequestForATreeSendsNoOtherRequest)
{
    offer("1:remote-machine", "text/uri-list");
    data(0, "file:///home/someone/a\r\nfile:///home/someone/b\r\n");
    int requests = 0;
    QObject::connect(&service, &DragDrop::reply, &service, [&](const QByteArray& b) {
        if (!b.contains("t=k"))
            return;
        ++requests;
        // The pane ends the protocol when it cannot write a reply.
        service.end();
    });
    startDrag();
    ASSERT_TRUE(waitUntil([&] { return requests > 0 && tempEmpty(); }));
    QCoreApplication::processEvents();
    EXPECT_EQ(requests, 1);
}
TEST(RemoteTreeLinks, LinksCannotLeaveTheTreeThroughOtherLinks)
{
    QTemporaryDir tree;
    ASSERT_TRUE(tree.isValid());
    const QString root = tree.path();
    ASSERT_TRUE(QDir().mkpath(root + QStringLiteral("/1/a/b")));
    EXPECT_TRUE(linkStaysInside(root + QStringLiteral("/1/a/b/s1"), "../..", root));
    ASSERT_EQ(::symlink("../..", QFile::encodeName(root + QStringLiteral("/1/a/b/s1")).constData()), 0);
    // Lexically "s1/../.." stays inside, but on disk it resolves above the tree.
    EXPECT_FALSE(linkStaysInside(root + QStringLiteral("/1/a/b/s2"), "s1/../..", root));
    EXPECT_FALSE(linkStaysInside(root + QStringLiteral("/1/a/b/s3"), "later/../..", root));
    EXPECT_FALSE(linkStaysInside(root + QStringLiteral("/1/s4"), "../..", root));
    EXPECT_TRUE(linkStaysInside(root + QStringLiteral("/1/a/b/s5"), "s1/a", root));
}
// aglow dnd in a pseudo-terminal that the test makes. Each OSC 72 sequence
// that it writes is kept, and goes to the service when relay is set.
struct AglowDrop : Dnd {
    pid_t child = -1;
    int master = -1;
    bool relay = true;
    std::unique_ptr<QSocketNotifier> notifier;
    QByteArray pending;
    QByteArrayList sent;
    // Whether this computer has a machine id: a drop from a program on it is then local.
    static bool hasMachineId() { return !AfterglowProto::dndMachineId().empty(); }
    static QByteArray readFile(const QString& path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("missing");
    }
    void TearDown() override
    {
        notifier.reset();
        if (child > 0) {
            ::kill(child, SIGKILL);
            ::waitpid(child, nullptr, 0);
        }
        if (master >= 0)
            ::close(master);
    }
    // Runs @p argv in @p cwd with TERM=xterm-256color, in an environment of
    // its own. The program's path, its arguments and its environment are
    // made before fork: the child only changes directory and executes the
    // path, with no search that could allocate.
    bool start(const QString& cwd, const QStringList& argv)
    {
        const QString program = argv.front().contains(QLatin1Char('/')) ? argv.front() : QStandardPaths::findExecutable(argv.front());
        if (program.isEmpty())
            return false;
        const QByteArray path = QFile::encodeName(program);
        std::vector<QByteArray> bytes;
        for (const auto& arg : argv)
            bytes.push_back(QFile::encodeName(arg));
        std::vector<char*> args;
        for (auto& arg : bytes)
            args.push_back(arg.data());
        args.push_back(nullptr);
        std::vector<QByteArray> variables;
        for (char** variable = environ; *variable; ++variable)
            if (std::strncmp(*variable, "TERM=", 5) != 0)
                variables.emplace_back(*variable);
        variables.emplace_back("TERM=xterm-256color");
        std::vector<char*> environment;
        for (auto& variable : variables)
            environment.push_back(variable.data());
        environment.push_back(nullptr);
        const QByteArray directory = QFile::encodeName(cwd);
        winsize size { 30, 100, 900, 600 };
        child = ::forkpty(&master, nullptr, nullptr, &size);
        if (child == 0) {
            if (::chdir(directory.constData()) == 0)
                ::execve(path.constData(), args.data(), environment.data());
            ::_exit(127);
        }
        if (child < 0)
            return false;
        QObject::connect(&service, &DragDrop::reply, &service, [this](const QByteArray& reply) {
            if (relay)
                write(reply);
        });
        notifier = std::make_unique<QSocketNotifier>(master, QSocketNotifier::Read);
        QObject::connect(notifier.get(), &QSocketNotifier::activated, &service, [this] {
            char buffer[65536];
            const auto n = ::read(master, buffer, sizeof buffer);
            if (n <= 0) {
                notifier->setEnabled(false);
                return;
            }
            pending.append(buffer, n);
            for (qsizetype at = pending.indexOf("\x1b]72;"); at >= 0; at = pending.indexOf("\x1b]72;")) {
                const auto end = pending.indexOf("\x1b\\", at);
                if (end < 0) {
                    pending.remove(0, at);
                    return;
                }
                const auto command = pending.mid(at + 5, end - at - 5);
                sent.append(command);
                if (relay)
                    service.handle(command);
                pending.remove(0, end + 2);
            }
            // Text and other escape codes of the program: only a partial OSC 72 can matter.
            const auto partial = pending.lastIndexOf('\x1b');
            pending = partial >= 0 ? pending.mid(partial) : QByteArray();
        });
        return true;
    }
    void write(const QByteArray& bytes)
    {
        for (qsizetype at = 0; at < bytes.size();) {
            const auto n = ::write(master, bytes.constData() + at, std::size_t(bytes.size() - at));
            if (n <= 0)
                return;
            at += n;
        }
    }
    // What a terminal says to the program.
    void say(const QByteArray& command) { write("\x1b]72;" + command + "\x1b\\"); }
    bool waitFor(const std::function<bool()>& done, int ms = 10000) { return waitUntil(done, ms); }
    void settle(int ms = 300)
    {
        QElapsedTimer elapsed;
        elapsed.start();
        while (elapsed.elapsed() < ms)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    bool saw(const QByteArray& text)
    {
        return std::any_of(sent.begin(), sent.end(), [&](const QByteArray& c) { return c.contains(text); });
    }
    // Moves a drag of @p mime over the pane until the program accepts it.
    // The program answers each move with the action it takes.
    bool hover(QMimeData& mime, Qt::DropActions actions)
    {
        bool accepted = false;
        QElapsedTimer elapsed;
        elapsed.start();
        for (int x = 4; !accepted && elapsed.elapsed() < 10000; ++x) {
            QDropEvent move(QPointF(x, 40), actions, &mime, Qt::LeftButton, Qt::NoModifier);
            service.drop(&move, false, { 10, 2 }, { x, 40 });
            accepted = move.isAccepted();
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        }
        return accepted;
    }
    // Drops @p urls on the program, as a desktop drag that is not from this
    // window, with @p actions, once a move was accepted. Returns the action
    // of the drop, or IgnoreAction when the program refused it.
    Qt::DropAction drop(const QList<QUrl>& urls, Qt::DropActions actions = Qt::CopyAction)
    {
        QMimeData mime;
        mime.setUrls(urls);
        if (!hover(mime, actions)) {
            ADD_FAILURE() << "the program accepted no move of the drag";
            return Qt::IgnoreAction;
        }
        QDropEvent event(QPointF(4, 40), actions, &mime, Qt::LeftButton, Qt::NoModifier);
        service.drop(&event, true, { 10, 2 }, { 4, 40 });
        return event.isAccepted() ? event.dropAction() : Qt::IgnoreAction;
    }
    Qt::DropAction dropTree(const QString& tree) { return drop({ QUrl::fromLocalFile(tree + QStringLiteral("/")) }); }
};
TEST_F(AglowDrop, ReadsARemoteTreeThroughTheTerminal)
{
#ifndef AFTERGLOW_AGLOW
    GTEST_SKIP() << "aglow is not built (AFTERGLOW_BUILD_AGLOW=OFF)";
#else
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString tree = directory.filePath(QStringLiteral("tree"));
    const QString destination = directory.filePath(QStringLiteral("destination"));
    ASSERT_TRUE(QDir().mkpath(tree + QStringLiteral("/sub/deeper")));
    ASSERT_TRUE(QDir().mkpath(tree + QStringLiteral("/empty")));
    ASSERT_TRUE(QDir().mkpath(destination));
    QByteArray large;
    for (int i = 0; i < 20000; ++i)
        large += QByteArray::number(i) + ' ';
    ASSERT_TRUE(writeFile(tree + QStringLiteral("/a.txt"), "first file"));
    ASSERT_TRUE(writeFile(tree + QStringLiteral("/sub/deeper/large"), large));
    ASSERT_TRUE(writeFile(tree + QStringLiteral("/sub/zero"), { }));
    ASSERT_EQ(::symlink("a.txt", QFile::encodeName(tree + QStringLiteral("/link")).constData()), 0);
    ASSERT_TRUE(start(destination, { QStringLiteral(AFTERGLOW_AGLOW), QStringLiteral("dnd"), QStringLiteral("--drop-anywhere=copy") }));
    ASSERT_TRUE(waitFor([&] { return service.acceptsDrops(); }));
    // The program sends its machine id right after t=a. Replaced after it,
    // the id puts the program on another computer: the service reports X=1
    // and the program reads the files through it.
    if (hasMachineId()) {
        ASSERT_TRUE(waitFor([&] { return saw("t=a:x=1"); }));
    }
    service.handle("t=a:x=1;1:remote-test-machine");
    EXPECT_EQ(dropTree(tree), Qt::CopyAction);
    // Directory reads went through handles (the first is 2).
    EXPECT_TRUE(contains("X=2:"));
    const QString copy = destination + QStringLiteral("/tree");
    EXPECT_EQ(readFile(copy + QStringLiteral("/a.txt")), "first file");
    EXPECT_EQ(readFile(copy + QStringLiteral("/sub/deeper/large")), large);
    EXPECT_EQ(readFile(copy + QStringLiteral("/sub/zero")), QByteArray());
    EXPECT_TRUE(QFileInfo(copy + QStringLiteral("/empty")).isDir());
    EXPECT_EQ(QFileInfo(copy + QStringLiteral("/link")).symLinkTarget(), copy + QStringLiteral("/a.txt"));
    // Ctrl+C ends the program, which removes its staging directory.
    ASSERT_EQ(::write(master, "\x03", 1), 1);
    int status = 0;
    bool exited = false;
    ASSERT_TRUE(waitFor([&] { return exited || (exited = ::waitpid(child, &status, WNOHANG) == child); }));
    child = -1;
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << status;
    EXPECT_EQ(QDir(destination).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden),
        QStringList { QStringLiteral("tree") });
#endif
}

TEST_F(AglowDrop, WithoutAMachineIdTheFilesAreReadThroughTheTerminal)
{
#ifndef AFTERGLOW_AGLOW
    GTEST_SKIP() << "aglow is not built (AFTERGLOW_BUILD_AGLOW=OFF)";
#else
    // /etc/machine-id hidden in a mount namespace of the program's own.
    if (::system("unshare -rm sh -c 'mount --bind /dev/null /etc/machine-id' >/dev/null 2>&1") != 0)
        GTEST_SKIP() << "unshare cannot make a mount namespace here";
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString tree = directory.filePath(QStringLiteral("tree"));
    const QString destination = directory.filePath(QStringLiteral("destination"));
    ASSERT_TRUE(QDir().mkpath(tree + QStringLiteral("/sub")));
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(writeFile(tree + QStringLiteral("/sub/f"), "data"));
    ASSERT_TRUE(start(destination, { QStringLiteral("unshare"), QStringLiteral("-rm"), QStringLiteral("sh"), QStringLiteral("-c"),
        QStringLiteral("mount --bind /dev/null /etc/machine-id && exec \"$0\" dnd --drop-anywhere=copy"),
        QStringLiteral(AFTERGLOW_AGLOW) }));
    ASSERT_TRUE(waitFor([&] { return service.acceptsDrops(); }));
    settle();
    // No id came, so the service takes the program for one on this computer
    // and sends no X=1. The program cannot know that: it reads the files through the terminal.
    EXPECT_FALSE(saw("t=a:x=1"));
    EXPECT_EQ(dropTree(tree), Qt::CopyAction);
    EXPECT_TRUE(contains("X=2:"));
    EXPECT_EQ(readFile(destination + QStringLiteral("/tree/sub/f")), "data");
#endif
}

TEST_F(AglowDrop, ADirectoryThatHoldsTheDropDirectoryIsRefused)
{
#ifndef AFTERGLOW_AGLOW
    GTEST_SKIP() << "aglow is not built (AFTERGLOW_BUILD_AGLOW=OFF)";
#else
    // A local drop: the program's machine id is this computer's, so it copies the files itself.
    if (!hasMachineId())
        GTEST_SKIP() << "no /etc/machine-id: no drop is local";
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString tree = directory.filePath(QStringLiteral("tree"));
    const QString destination = tree + QStringLiteral("/inside");
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(start(destination, { QStringLiteral(AFTERGLOW_AGLOW), QStringLiteral("dnd"), QStringLiteral("--drop-anywhere=copy") }));
    ASSERT_TRUE(waitFor([&] { return service.acceptsDrops(); }));
    ASSERT_TRUE(waitFor([&] { return saw("t=a:x=1"); }));
    EXPECT_EQ(dropTree(tree), Qt::IgnoreAction);
    // Nothing was copied, and the program leaves no staging directory behind.
    const auto entries = QDir(destination).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    EXPECT_TRUE(entries.isEmpty()) << entries.join(QLatin1Char(' ')).toStdString();
#endif
}

TEST_F(AglowDrop, ALocalMoveIntoTheDirectoryOfTheFileKeepsIt)
{
#ifndef AFTERGLOW_AGLOW
    GTEST_SKIP() << "aglow is not built (AFTERGLOW_BUILD_AGLOW=OFF)";
#else
    if (!hasMachineId())
        GTEST_SKIP() << "no /etc/machine-id: no drop is local";
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString file = directory.filePath(QStringLiteral("notes"));
    ASSERT_TRUE(writeFile(file, "the only copy"));
    ASSERT_TRUE(start(directory.path(), { QStringLiteral(AFTERGLOW_AGLOW), QStringLiteral("dnd"), QStringLiteral("--drop-anywhere=move") }));
    ASSERT_TRUE(waitFor([&] { return service.acceptsDrops(); }));
    ASSERT_TRUE(waitFor([&] { return saw("t=a:x=1"); }));
    // The file moves onto itself: whatever the program answers, the data stays.
    drop({ QUrl::fromLocalFile(file) }, Qt::CopyAction | Qt::MoveAction);
    // The program handles its input in order: once it answers the next
    // drag, it has finished the drop.
    QMimeData next;
    next.setUrls({ QUrl::fromLocalFile(file) });
    EXPECT_TRUE(hover(next, Qt::CopyAction | Qt::MoveAction));
    service.leave();
    EXPECT_EQ(readFile(file), "the only copy");
#endif
}

TEST_F(AglowDrop, ALocalCopyIsIndependentOfItsSource)
{
#ifndef AFTERGLOW_AGLOW
    GTEST_SKIP() << "aglow is not built (AFTERGLOW_BUILD_AGLOW=OFF)";
#else
    if (!hasMachineId())
        GTEST_SKIP() << "no /etc/machine-id: no drop is local";
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString source = directory.filePath(QStringLiteral("source"));
    const QString destination = directory.filePath(QStringLiteral("destination"));
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(QDir().mkpath(destination));
    ASSERT_TRUE(writeFile(source + QStringLiteral("/file"), "original"));
    // The default copy links where it can, as kitten's does: an independent copy is asked for.
    ASSERT_TRUE(start(destination, { QStringLiteral(AFTERGLOW_AGLOW), QStringLiteral("dnd"), QStringLiteral("--drop-anywhere=copy"),
        QStringLiteral("--copy-mode=independent") }));
    ASSERT_TRUE(waitFor([&] { return service.acceptsDrops(); }));
    ASSERT_TRUE(waitFor([&] { return saw("t=a:x=1"); }));
    EXPECT_EQ(drop({ QUrl::fromLocalFile(source + QStringLiteral("/file")) }), Qt::CopyAction);
    const QString copy = destination + QStringLiteral("/file");
    ASSERT_TRUE(waitFor([&] { return readFile(copy) == "original"; }));
    // A copy that shared the source's inode (a hard link) would change with it.
    struct stat original { }, copied { };
    ASSERT_EQ(::stat(QFile::encodeName(source + QStringLiteral("/file")).constData(), &original), 0);
    ASSERT_EQ(::stat(QFile::encodeName(copy).constData(), &copied), 0);
    EXPECT_NE(original.st_ino, copied.st_ino);
    EXPECT_EQ(original.st_nlink, 1u);
    ASSERT_TRUE(writeFile(copy, "changed"));
    EXPECT_EQ(readFile(source + QStringLiteral("/file")), "original");
#endif
}

// aglow dnd as a drag source of a remote drag (t=k), against a terminal that
// the test plays: its files go only after a drag that ended as a move.
struct AglowDrag : AglowDrop {
    QTemporaryDir directory;
    QString file;
    void SetUp() override
    {
        AglowDrop::SetUp();
        relay = false;
        file = directory.filePath(QStringLiteral("dragged"));
        ASSERT_TRUE(writeFile(file, "payload"));
    }
    // A drag through one remote read, up to the drop, in the order of the
    // service: the tree is copied before the start is confirmed.
    void dragAndDrop()
    {
        sent.clear();
        say("t=o:x=2:y=2:X=20:Y=40");
        ASSERT_TRUE(waitFor([&] { return saw("t=P:x=-1"); }));
        say("t=k:x=1");
        // The entry ends with an empty message.
        ASSERT_TRUE(waitFor([&] {
            return std::any_of(sent.begin(), sent.end(), [](const QByteArray& c) { return c.contains("t=k") && c.endsWith(";"); });
        }));
        say("t=E;OK");
        say("t=e:x=2:o=2");
        say("t=e:x=3");
    }
};
TEST_F(AglowDrag, ACanceledMoveKeepsTheFiles)
{
#ifndef AFTERGLOW_AGLOW
    GTEST_SKIP() << "aglow is not built (AFTERGLOW_BUILD_AGLOW=OFF)";
#else
    ASSERT_TRUE(start(directory.path(), { QStringLiteral(AFTERGLOW_AGLOW), QStringLiteral("dnd"), file }));
    ASSERT_TRUE(waitFor([&] { return saw("t=o:x=1"); }));
    dragAndDrop();
    say("t=e:x=4:y=1");
    // The program handles its input in order: once it starts the next drag,
    // it has handled the end of this one.
    dragAndDrop();
    EXPECT_TRUE(QFileInfo::exists(file));
    // A drop that ended as a copy, although the target chose a move before.
    say("t=e:o=1:x=4:y=0");
    dragAndDrop();
    EXPECT_TRUE(QFileInfo::exists(file));
    // A move: the files go.
    say("t=e:o=2:x=4:y=0");
    EXPECT_TRUE(waitFor([&] { return !QFileInfo::exists(file); }));
#endif
}
TEST_F(AglowDrag, WithoutTheActionOfTheDropTheLastChosenOneCounts)
{
#ifndef AFTERGLOW_AGLOW
    GTEST_SKIP() << "aglow is not built (AFTERGLOW_BUILD_AGLOW=OFF)";
#else
    ASSERT_TRUE(start(directory.path(), { QStringLiteral(AFTERGLOW_AGLOW), QStringLiteral("dnd"), file }));
    ASSERT_TRUE(waitFor([&] { return saw("t=o:x=1"); }));
    // kitty names no action when the drag ends: the target chose a move (x=2), so the files go.
    dragAndDrop();
    say("t=e:x=4:y=0");
    EXPECT_TRUE(waitFor([&] { return !QFileInfo::exists(file); }));
#endif
}
} // namespace
int main(int argc, char** argv)
{
    // Test discovery runs this binary too: it must never reach the user's session.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
