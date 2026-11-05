// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop/DragDrop.h"
#include "desktop/FileDrop.h"
#include <QDir>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFile>
#include <QGuiApplication>
#include <QMimeData>
#include <QSocketNotifier>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <csignal>
#include <gtest/gtest.h>
#include <pty.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
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
    });
    QDropEvent event(QPointF(4, 4), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&event, true, { 1, 1 }, { 4, 4 });
    EXPECT_FALSE(event.isAccepted());
    EXPECT_TRUE(contains("EUNKNOWN"));
}
TEST(DndTree, LinksCannotLeaveTheTreeThroughOtherLinks)
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
// aglow dnd as the drop target of a drag from another computer: it reads the
// dropped tree through the terminal, one directory handle at a time (t=r with
// X). The program runs in a pseudo-terminal that the test makes, and each
// OSC 72 sequence goes between it and the service whole.
struct AglowDrop : Dnd {
    pid_t child = -1;
    int master = -1;
    std::unique_ptr<QSocketNotifier> notifier;
    QByteArray pending;
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
    bool start(const QString& aglow, const QString& dropDir)
    {
        winsize size { 30, 100, 900, 600 };
        child = ::forkpty(&master, nullptr, nullptr, &size);
        if (child == 0) {
            ::chdir(QFile::encodeName(dropDir).constData());
            ::setenv("TERM", "xterm-256color", 1);
            const QByteArray program = QFile::encodeName(aglow);
            ::execl(program.constData(), "aglow", "dnd", "--drop-anywhere=copy", static_cast<char*>(nullptr));
            ::_exit(127);
        }
        if (child < 0)
            return false;
        QObject::connect(&service, &DragDrop::reply, &service, [this](const QByteArray& bytes) {
            for (qsizetype at = 0; at < bytes.size();) {
                const auto n = ::write(master, bytes.constData() + at, std::size_t(bytes.size() - at));
                if (n <= 0)
                    return;
                at += n;
            }
        });
        notifier = std::make_unique<QSocketNotifier>(master, QSocketNotifier::Read);
        QObject::connect(notifier.get(), &QSocketNotifier::activated, &service, [this] {
            char bytes[65536];
            const auto n = ::read(master, bytes, sizeof bytes);
            if (n <= 0) {
                notifier->setEnabled(false);
                return;
            }
            pending.append(bytes, n);
            for (qsizetype at = pending.indexOf("\x1b]72;"); at >= 0; at = pending.indexOf("\x1b]72;")) {
                const auto end = pending.indexOf("\x1b\\", at);
                if (end < 0) {
                    pending.remove(0, at);
                    return;
                }
                service.handle(pending.mid(at + 5, end - at - 5));
                pending.remove(0, end + 2);
            }
            // Text and other escape codes of the program: only a partial OSC 72 can matter.
            const auto partial = pending.lastIndexOf('\x1b');
            pending = partial >= 0 ? pending.mid(partial) : QByteArray();
        });
        return true;
    }
    bool waitFor(const std::function<bool()>& done)
    {
        QElapsedTimer elapsed;
        elapsed.start();
        while (!done() && elapsed.elapsed() < 10000)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        return done();
    }
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
    auto write = [](const QString& path, const QByteArray& bytes) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    QByteArray large;
    for (int i = 0; i < 20000; ++i)
        large += QByteArray::number(i) + ' ';
    ASSERT_TRUE(write(tree + QStringLiteral("/a.txt"), "first file"));
    ASSERT_TRUE(write(tree + QStringLiteral("/sub/deeper/large"), large));
    ASSERT_TRUE(write(tree + QStringLiteral("/sub/zero"), { }));
    ASSERT_EQ(::symlink("a.txt", QFile::encodeName(tree + QStringLiteral("/link")).constData()), 0);
    ASSERT_TRUE(start(QStringLiteral(AFTERGLOW_AGLOW), destination));
    ASSERT_TRUE(waitFor([&] { return service.acceptsDrops(); }));
    // The program sends its machine id right after t=a. Replaced after it,
    // the id puts the program on another computer: the service reports X=1
    // and the program reads the files through it.
    QElapsedTimer settle;
    settle.start();
    while (settle.elapsed() < 300)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    service.handle("t=a:x=1;1:remote-test-machine");
    QMimeData mime;
    mime.setUrls({ QUrl::fromLocalFile(tree + QStringLiteral("/")) });
    // The program answers each move with the action it takes. The drop goes once a move was accepted.
    bool accepted = false;
    QElapsedTimer elapsed;
    elapsed.start();
    for (int x = 4; !accepted && elapsed.elapsed() < 10000; ++x) {
        QDropEvent move(QPointF(x, 40), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        service.drop(&move, false, { 10, 2 }, { x, 40 });
        accepted = move.isAccepted();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    ASSERT_TRUE(accepted);
    QDropEvent drop(QPointF(4, 40), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    service.drop(&drop, true, { 10, 2 }, { 4, 40 });
    EXPECT_TRUE(drop.isAccepted());
    EXPECT_EQ(drop.dropAction(), Qt::CopyAction);
    // Directory reads went through handles (the first is 2).
    EXPECT_TRUE(contains("X=2:"));
    const QString copy = destination + QStringLiteral("/tree");
    auto read = [](const QString& path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray("missing");
    };
    EXPECT_EQ(read(copy + QStringLiteral("/a.txt")), "first file");
    EXPECT_EQ(read(copy + QStringLiteral("/sub/deeper/large")), large);
    EXPECT_EQ(read(copy + QStringLiteral("/sub/zero")), QByteArray());
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
} // namespace
int main(int argc, char** argv)
{
    // Test discovery runs this binary too: it must never reach the user's session.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
