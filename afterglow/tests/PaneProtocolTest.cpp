// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Pane.h"
#include "ProgramImages.h"
#include "desktop/Download.h"
#include "desktop/FileTransfer.h"
#include "ImageFiles.h"

#include <AfterglowProto/Base64.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using Afterglow::DragDrop;
using Afterglow::Pane;

namespace Afterglow {
struct FileTransferTestPeer
{
    static bool deliveryWindowFull(const FileTransfer& transfer)
    {
        return transfer.deliveryWindowFull();
    }
};
} // namespace Afterglow

namespace {

/// Waits until @p condition holds or 5 s pass, and returns the condition.
/// With @p events, the GUI thread runs its events meanwhile.
bool waitFor(const std::function<bool()>& condition, bool events = true)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < 5000) {
        if (events)
            QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return condition();
}

/// Feeds the pane's terminal as the PTY reader does: with the pane's lock held.
class PaneProtocol : public ::testing::Test
{
protected:
    void SetUp() override
    {
        QObject::connect(&pane, &Pane::terminalProtocolLost, [this](DragDrop::Lost lost, bool reset) {
            gaps.append({lost, reset});
        });
        QObject::connect(&pane, &Pane::terminalProtocolRequested, [this](int code, const QByteArray&) {
            codes.append(code);
        });
    }
    void feed(const std::string& bytes)
    {
        pane.withTerminal([&](AfterglowVt::Terminal& terminal) {
            terminal.feed(bytes);
        });
    }
    /// @p count commands of protocol @p code, each near its size limit.
    static std::string commands(int code, int count)
    {
        const std::string command =
            "\x1b]" + std::to_string(code) + ";t=r:x=1;" + std::string(code == 72 ? 8000 : 16000, 'A') + "\x1b\\";
        std::string bytes;
        for (int i = 0; i < count; ++i)
            bytes += command;
        return bytes;
    }
    /// More than the 16 MiB that the pane holds for the GUI thread.
    static std::string overflow(int code)
    {
        return commands(code, code == 72 ? 2100 : 1100);
    }
    static constexpr const char* Reset =
        "\x1b"
        "c";

    Pane pane{AfterglowVt::TerminalOptions{}};
    QList<std::pair<DragDrop::Lost, bool>> gaps;
    QList<int> codes;
};

TEST_F(PaneProtocol, AGapOfFileTransferCommandsHoldsNothingForDragAndDrop)
{
    feed(overflow(5113));
    QCoreApplication::sendPostedEvents();
    ASSERT_EQ(gaps.size(), 1);
    EXPECT_EQ(gaps[0], std::pair(DragDrop::Lost::Nothing, false));
    // Every command queued before the overflow is part of the gap.
    EXPECT_TRUE(codes.isEmpty());
    // The pane takes commands again after the gap.
    feed(commands(72, 1));
    QCoreApplication::sendPostedEvents();
    EXPECT_EQ(codes, QList<int>{72});
}

TEST_F(PaneProtocol, AGapThatEndsWithAResetIsAReset)
{
    feed(overflow(72));
    feed(Reset);
    QCoreApplication::sendPostedEvents();
    ASSERT_EQ(gaps.size(), 1);
    EXPECT_EQ(gaps[0], std::pair(DragDrop::Lost::Reset, true));
}

TEST_F(PaneProtocol, PacketsAfterTheResetAreLostPackets)
{
    feed(overflow(72));
    feed(Reset);
    feed(commands(72, 1));
    QCoreApplication::sendPostedEvents();
    ASSERT_EQ(gaps.size(), 1);
    EXPECT_EQ(gaps[0], std::pair(DragDrop::Lost::Packets, true));
}

TEST_F(PaneProtocol, TheCommandsThatWaitedComeFirstInTheGap)
{
    // Packets that waited for the GUI thread, then file transfer commands
    // that came while the pane was blocked.
    feed(commands(72, 1500));
    feed(commands(5113, 400));
    QCoreApplication::sendPostedEvents();
    ASSERT_EQ(gaps.size(), 1);
    EXPECT_EQ(gaps[0], std::pair(DragDrop::Lost::Packets, false));
}

TEST_F(PaneProtocol, AResetThatWaitedIsInTheGap)
{
    feed(Reset);
    feed(overflow(5113));
    QCoreApplication::sendPostedEvents();
    ASSERT_EQ(gaps.size(), 1);
    EXPECT_EQ(gaps[0], std::pair(DragDrop::Lost::Reset, true));
}

TEST_F(PaneProtocol, DeliveryTakesOnlyTheProtocolCommands)
{
    bool other = false;
    QTimer::singleShot(0, &pane, [&] {
        other = true;
    });
    QMetaObject::invokeMethod(
        &pane,
        [&] {
            other = true;
        },
        Qt::QueuedConnection);
    feed(commands(72, 1));
    pane.deliverProtocolCommands();
    EXPECT_EQ(codes, QList<int>{72});
    EXPECT_FALSE(other);
}

TEST_F(PaneProtocol, DeliveryWaitsForTheNextCommand)
{
    // The program writes until the GUI thread has its command, so the wait
    // ends early however the two threads are scheduled.
    std::atomic_bool stop{false};
    std::thread program([&] {
        while (!stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            feed(commands(72, 1));
        }
    });
    QElapsedTimer waited;
    waited.start();
    while (codes.isEmpty() && waited.elapsed() < 5000)
        pane.deliverProtocolCommands(5000);
    const auto elapsed = waited.elapsed();
    stop = true;
    program.join();
    EXPECT_FALSE(codes.isEmpty());
    EXPECT_LT(elapsed, 2000);
    // A command that came since the last call is delivered without a wait.
    pane.deliverProtocolCommands();
    feed(commands(72, 1));
    const auto before = codes.size();
    waited.restart();
    pane.deliverProtocolCommands(5000);
    EXPECT_LT(waited.elapsed(), 2000);
    EXPECT_EQ(codes.size(), before + 1);
    // Nothing comes: the wait ends at its limit.
    pane.deliverProtocolCommands();
    const auto delivered = codes.size();
    pane.deliverProtocolCommands();
    waited.restart();
    pane.deliverProtocolCommands(30);
    EXPECT_GE(waited.elapsed(), 25);
    EXPECT_EQ(codes.size(), delivered);
}

TEST_F(PaneProtocol, ADeviceReportFollowsTheRepliesToTheCommandsBeforeIt)
{
    // aglow reads the DA1 answer as the fence after the replies to the
    // commands that it wrote before the query.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("input"));
    // As the window routes OSC 72.
    QObject::connect(&pane, &Pane::terminalProtocolRequested, &pane, [this](int code, const QByteArray& payload) {
        if (code == 72)
            pane.dragDrop()->handle(payload);
    });
    AfterglowPty::SpawnOptions options;
    // An OSC 72 command that the pane answers with EINVAL, DA1 and a mark,
    // in one write. The program keeps what the terminal answers.
    options.argv = {"/bin/sh", "-c", R"(stty raw -echo && printf '\033]72;t=e:x=1\033\\\033[cx' && exec cat > "$0")",
                    QFile::encodeName(out).toStdString()};
    QString error;
    ASSERT_TRUE(pane.start(options, QStringLiteral("xterm-256color"), 20, 5, 10, 20, &error)) << error.toStdString();
    // The PTY thread has read the whole write before the GUI thread runs the
    // command: an answer that does not wait for the command comes first.
    ASSERT_TRUE(waitFor(
        [&] {
            return pane.text(AfterglowVt::Terminal::TextScope::Screen).contains(QLatin1Char('x'));
        },
        false));
    const QByteArray reply = "\x1b]72;m=0:t=E;EINVAL\x1b\\";
    QByteArray input;
    const auto answered = [&] {
        QFile file(out);
        input = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        const auto report = input.indexOf("\x1b[?");
        return input.contains(reply) && report >= 0 && input.indexOf('c', report) > report;
    };
    ASSERT_TRUE(waitFor(answered)) << input.toStdString();
    EXPECT_TRUE(input.startsWith(reply)) << input.toStdString();
    EXPECT_GT(input.indexOf("\x1b[?"), input.indexOf(reply));
}

/// Counts the queued calls that the GUI thread runs.
class QueuedCalls : public QObject
{
public:
    QueuedCalls()
    {
        QCoreApplication::instance()->installEventFilter(this);
    }
    ~QueuedCalls() override
    {
        QCoreApplication::instance()->removeEventFilter(this);
    }
    bool eventFilter(QObject*, QEvent* event) override
    {
        if (event->type() == QEvent::MetaCall)
            ++count;
        return false;
    }
    int count = 0;
};

/// The answer of the terminal to DA1.
constexpr const char* DeviceAttributes = "\x1b[?65;4;6;22;28;52c";

/// Starts @p script in the pane, waits until the pane has read its whole
/// write (a mark 'x' at its end) before the GUI thread runs, and returns the
/// input that the program read once it holds each of @p expected.
QByteArray answers(Pane& pane, const char* script, const QList<QByteArray>& expected)
{
    QTemporaryDir dir;
    EXPECT_TRUE(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("input"));
    AfterglowPty::SpawnOptions options;
    options.argv = {"/bin/sh", "-c", std::string("stty raw -echo && ") + script + R"( && exec cat > "$0")",
                    QFile::encodeName(out).toStdString()};
    QString error;
    EXPECT_TRUE(pane.start(options, QStringLiteral("xterm-256color"), 20, 5, 10, 20, &error)) << error.toStdString();
    EXPECT_TRUE(waitFor(
        [&] {
            return pane.text(AfterglowVt::Terminal::TextScope::Screen).contains(QLatin1Char('x'));
        },
        false));
    QByteArray input;
    EXPECT_TRUE(waitFor([&] {
        QFile file(out);
        input = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        return std::all_of(expected.begin(), expected.end(), [&](const QByteArray& e) {
            return input.contains(e);
        });
    })) << input.toStdString();
    return input;
}

/// Routes the protocols as the window does.
void route(Pane& pane)
{
    QObject::connect(&pane, &Pane::terminalProtocolRequested, &pane, [&pane](int code, const QByteArray& payload) {
        if (code == 72)
            pane.dragDrop()->handle(payload);
        else if (code == 5113)
            pane.fileTransfers()->handle(payload);
    });
}

TEST_F(PaneProtocol, ReportsThatWaitForACommandShareOneQueuedCall)
{
    // A program that writes one command and then a flood of queries does
    // not grow the GUI thread's queue with them.
    feed(commands(5113, 1));
    std::string queries;
    for (int i = 0; i < 100000; ++i)
        queries += "\x1b[c";
    feed(queries);
    QueuedCalls calls;
    QCoreApplication::sendPostedEvents();
    EXPECT_EQ(codes, QList<int>{5113});
    EXPECT_LE(calls.count, 3);
}

TEST_F(PaneProtocol, ReportsThatWaitPastTheBudgetAreRefused)
{
    int rejected = 0;
    QObject::connect(&pane, &Pane::inputRejected, [&] {
        ++rejected;
    });
    feed(commands(5113, 1));
    // More answers than the 16 MiB that the pane holds for the GUI thread.
    std::string queries;
    for (int i = 0; i < 17 * 1024 * 1024 / int(std::strlen(DeviceAttributes)); ++i)
        queries += "\x1b[c";
    feed(queries);
    QCoreApplication::sendPostedEvents();
    EXPECT_EQ(rejected, 1);
    // The budget is free again when the reports are gone.
    feed(commands(72, 1));
    QCoreApplication::sendPostedEvents();
    EXPECT_EQ(codes, (QList<int>{5113, 72}));
    EXPECT_TRUE(gaps.isEmpty());
}

TEST_F(PaneProtocol, EachReportFollowsOnlyTheCommandsBeforeIt)
{
    route(pane);
    const QByteArray reply = "\x1b]72;m=0:t=E;EINVAL\x1b\\";
    const auto input = answers(pane, R"(printf '\033]72;t=e:x=1\033\\\033[c\033]72;t=e:x=1\033\\\033[cx')",
                               {reply + DeviceAttributes + reply + DeviceAttributes});
    EXPECT_EQ(input.toStdString(), (reply + DeviceAttributes + reply + DeviceAttributes).toStdString());
}

TEST_F(PaneProtocol, ADeviceReportFollowsTheRepliesOfTheFileTransferWorker)
{
    // The worker answers a file transfer command after the command's closure
    // returned. Each report waits for the commands before it, and only for those.
    route(pane);
    // EINVAL, in base64.
    const QByteArray first = "\x1b]5113;ac=status;id=x;st=RUlOVkFM\x1b\\";
    const QByteArray second = "\x1b]5113;ac=status;id=y;st=RUlOVkFM\x1b\\";
    const auto input = answers(
        pane, R"(printf '\033]5113;ac=receive;id=x;sz=0\033\\\033[c\033]5113;ac=receive;id=y;sz=0\033\\\033[cx')",
        {first + DeviceAttributes + second + DeviceAttributes});
    EXPECT_EQ(input.toStdString(), (first + DeviceAttributes + second + DeviceAttributes).toStdString());
}

TEST_F(PaneProtocol, ADragCommandDoesNotWaitForAFileTransfer)
{
    // A drag event delivers the program's OSC 72 answer while the worker
    // still owes the replies of a file transfer command.
    route(pane);
    feed("\x1b]5113;ac=receive;id=x;sz=0\x1b\\");
    feed(commands(72, 1));
    pane.deliverProtocolCommands();
    EXPECT_EQ(codes, (QList<int>{5113, 72}));
}

TEST_F(PaneProtocol, ADeviceReportAfterACanceledDownloadIsAnswered)
{
    // A cancel drops the packets of the download that wait in the worker's
    // queue. The report after it must not wait for them.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString large = dir.filePath(QStringLiteral("large"));
    {
        QFile source(large);
        ASSERT_TRUE(source.open(QIODevice::WriteOnly));
        QByteArray data(8 * 1024 * 1024, '\0');
        quint32 seed = 1;
        for (auto& c : data)
            c = char((seed = seed * 1664525u + 1013904223u) >> 24);
        source.write(data);
    }
    route(pane);
    QObject::connect(&pane, &Pane::fileTransferPermissionRequested, &pane,
                     [this](const QString& token, bool, const QStringList&, int) {
                         pane.fileTransfers()->approve(token, true);
                     });
    const QString out = dir.filePath(QStringLiteral("input"));
    AfterglowPty::SpawnOptions options;
    options.argv = {"/bin/sh", "-c", R"(stty raw -echo && printf x && exec cat > "$0")",
                    QFile::encodeName(out).toStdString()};
    QString error;
    ASSERT_TRUE(pane.start(options, QStringLiteral("xterm-256color"), 20, 5, 10, 20, &error)) << error.toStdString();
    ASSERT_TRUE(waitFor([&] {
        return pane.text(AfterglowVt::Terminal::TextScope::Screen).contains(QLatin1Char('x'));
    }));
    const std::string name = large.toUtf8().toBase64(QByteArray::OmitTrailingEquals).toStdString();
    QByteArray input;
    const auto read = [&](const QByteArray& wanted) {
        return waitFor([&] {
            QFile file(out);
            input = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
            return input.contains(wanted);
        });
    };
    feed("\x1b]5113;id=t;ac=receive;sz=1\x1b\\\x1b]5113;id=t;ac=file;fid=request;n=" + name + "\x1b\\");
    ASSERT_TRUE(read("ac=file;"));
    // The GUI thread runs the commands and no other events: the download
    // fills the worker's queue.
    feed("\x1b]5113;id=t;ac=file;fid=data;n=" + name + "\x1b\\");
    pane.deliverProtocolCommands();
    // The worker reads the file until its queue is full: the window took
    // none of the replies, and the worker waits for it.
    ASSERT_TRUE(waitFor(
        [&] {
            return Afterglow::FileTransferTestPeer::deliveryWindowFull(*pane.fileTransfers());
        },
        false));
    feed("\x1b]5113;id=t;ac=cancel\x1b\\");
    pane.deliverProtocolCommands();
    feed("\x1b[c");
    EXPECT_TRUE(read(DeviceAttributes)) << input.right(200).toStdString();
    // The cancel came while the file was on its way: its last packet never went.
    EXPECT_FALSE(input.contains("ac=end_data"));
}

TEST(PaneEvents, AClipboardRequestThatFindsTheQueueFullIsBusy)
{
    // More clipboard reads (400) than the queue holds (Pane::MaxQueuedEvents,
    // 256), before the GUI thread runs: those over get EBUSY, which a
    // program may try again after. ENOSYS would tell it that the terminal
    // has no clipboard.
    Pane pane{AfterglowVt::TerminalOptions{}};
    const QByteArray input = answers(
        pane,
        R"(i=0; while [ $i -lt 400 ]; do printf '\033]5522;type=read:id=x;Lg==\033\\'; i=$((i+1)); done; printf x)",
        {"type=read:status=EBUSY:id=x"});
    EXPECT_FALSE(input.contains("ENOSYS")) << input.left(200).toStdString();
}

TEST(PaneEvents, AFloodOfNotificationsDoesNotDropTheCommandEvents)
{
    // More notifications (400) than the queue holds (Pane::MaxQueuedEvents,
    // 256), then a command, in one read: the command's start and end still
    // reach the window, which pairs them.
    Pane pane{AfterglowVt::TerminalOptions{}};
    int notifications = 0, began = 0, ended = 0;
    QObject::connect(&pane, &Pane::notificationRequested, [&](const QString&, const QString&) {
        ++notifications;
    });
    QObject::connect(&pane, &Pane::commandBegan, [&](const QString&) {
        ++began;
    });
    QObject::connect(&pane, &Pane::commandEnded, [&](const QString&, int, qint64) {
        ++ended;
    });
    std::string bytes;
    for (int i = 0; i < 400; ++i)
        bytes += "\x1b]9;flood\x07";
    bytes += "\x1b]133;A\x1b\\$ \x1b]133;B\x1b\\echo test\x1b]133;C\x1b\\result\x1b]133;D;0\x1b\\";
    pane.withTerminal([&](AfterglowVt::Terminal& terminal) {
        terminal.feed(bytes);
    });
    QCoreApplication::sendPostedEvents(&pane, QEvent::MetaCall);
    EXPECT_GT(notifications, 0);
    EXPECT_LT(notifications, 400);
    EXPECT_EQ(began, 1);
    EXPECT_EQ(ended, 1);
}

namespace {
// Records the command events of @p pane in @p events, as "began:COMMAND"
// and "ended:COMMAND".
void recordCommands(Pane& pane, QStringList& events)
{
    QObject::connect(&pane, &Pane::commandBegan, [&events](const QString& c) {
        events.append(QStringLiteral("began:") + c);
    });
    QObject::connect(&pane, &Pane::commandEnded, [&events](const QString& c, int, qint64) {
        events.append(QStringLiteral("ended:") + c);
    });
}
constexpr std::string_view CommandStart = "\x1b]133;C\x1b\\";
constexpr std::string_view CommandEnd = "\x1b]133;D;0\x1b\\";
} // namespace

TEST(PaneEvents, ACommandThatStartsInTheLastPlaceOfTheQueueStillEnds)
{
    // A start with no end (C C), then commands up to one place short of
    // the queue's bound: the next command's start takes the last place, and
    // its end must come too, or the receivers see it run forever.
    QStringList events;
    Pane pane{AfterglowVt::TerminalOptions{}};
    recordCommands(pane, events);
    std::string bytes(CommandStart);
    for (int i = 0; i < 127; ++i)
        (bytes += CommandStart) += CommandEnd;
    (bytes += "\x1b]133;B\x1b\\last") += CommandStart;
    bytes += CommandEnd;
    pane.withTerminal([&](AfterglowVt::Terminal& terminal) {
        terminal.feed(bytes);
    });
    QCoreApplication::sendPostedEvents(&pane, QEvent::MetaCall);
    ASSERT_GE(events.size(), 2);
    EXPECT_EQ(events.at(events.size() - 2).toStdString(), "began:last");
    EXPECT_EQ(events.constLast().toStdString(), "ended:last");
}

TEST(PaneEvents, TheEndOfACommandWhoseStartWasDroppedIsDroppedToo)
{
    // The queue is full when a command starts, and has room again when it
    // ends: an end alone would end a command that never ran for the
    // receivers.
    QStringList events;
    Pane pane{AfterglowVt::TerminalOptions{}};
    recordCommands(pane, events);
    std::string bytes;
    for (int i = 0; i < 128; ++i)
        (bytes += CommandStart) += CommandEnd;
    (bytes += "\x1b]133;B\x1b\\dropped") += CommandStart;
    pane.withTerminal([&](AfterglowVt::Terminal& terminal) {
        terminal.feed(bytes);
    });
    QCoreApplication::sendPostedEvents(&pane, QEvent::MetaCall);
    ASSERT_EQ(events.size(), 256);
    pane.withTerminal([&](AfterglowVt::Terminal& terminal) {
        terminal.feed(CommandEnd);
    });
    QCoreApplication::sendPostedEvents(&pane, QEvent::MetaCall);
    EXPECT_EQ(events.size(), 256) << events.constLast().toStdString();
}

TEST(PaneText, TheOutputsThatAReadNamesByWhereTheyAre)
{
    // kitty's first_cmd_output_on_screen and last_visited_cmd_output, as
    // read_text and afterglow ctl get-text read them: none on the alternate
    // screen for the first, none until a prompt jump visits one for the
    // second, which stays on the alternate screen.
    Pane pane{AfterglowVt::TerminalOptions{}};
    const auto command = [](const std::string& output) {
        return "\x1b]133;A\x1b\\$ \x1b]133;B\x1b\\cmd\r\n\x1b]133;C\x1b\\" + output + "\r\n\x1b]133;D;0\x1b\\";
    };
    // The second output is taller than the screen: its prompt is in the
    // history, where a jump up finds it.
    std::string tall = "second";
    for (int i = 0; i < 60; ++i)
        tall += "\r\nrow" + std::to_string(i);
    pane.withTerminal([&](AfterglowVt::Terminal& terminal) {
        terminal.feed(command("first") + command(tall) + "\x1b]133;A\x1b\\$ ");
    });
    const auto read = [&pane](Pane::OutputRegion which) {
        const QVariantMap result = pane.readOutputRegion(which, Pane::MaxText);
        return result.value(QStringLiteral("available")).toBool() ? result.value(QStringLiteral("text")).toString()
                                                                  : QStringLiteral("<none>");
    };
    const QString onScreen = read(Pane::OutputRegion::FirstOnScreen);
    EXPECT_TRUE(onScreen.startsWith(QStringLiteral("second"))) << onScreen.left(80).toStdString();
    EXPECT_EQ(read(Pane::OutputRegion::LastVisited), QStringLiteral("<none>"));
    ASSERT_TRUE(pane.jumpToPrompt(-1));
    EXPECT_TRUE(read(Pane::OutputRegion::LastVisited).startsWith(QStringLiteral("second")));
    ASSERT_TRUE(pane.jumpToPrompt(-1));
    EXPECT_EQ(read(Pane::OutputRegion::LastVisited).trimmed(), QStringLiteral("first"));
    EXPECT_EQ(read(Pane::OutputRegion::FirstOnScreen).trimmed(), QStringLiteral("first"));
    pane.withTerminal([](AfterglowVt::Terminal& terminal) {
        terminal.feed("\x1b[?1049h");
    });
    EXPECT_EQ(read(Pane::OutputRegion::FirstOnScreen), QStringLiteral("<none>"));
    EXPECT_EQ(read(Pane::OutputRegion::LastVisited).trimmed(), QStringLiteral("first"));
    pane.withTerminal([](AfterglowVt::Terminal& terminal) {
        terminal.feed("\x1b[?1049l");
    });
    EXPECT_NE(read(Pane::OutputRegion::FirstOnScreen), QStringLiteral("<none>"));
}

namespace {

/// Whether the terminal of @p pane holds its input for a download's answer.
bool holdsInput(Pane& pane)
{
    return pane.withTerminal([](AfterglowVt::Terminal& terminal) {
        return terminal.holdsInput();
    });
}

/// The size of the file at @p path, or -1.
qint64 sizeOf(const QString& path)
{
    const QFileInfo info(path);
    return info.exists() ? info.size() : -1;
}

} // namespace

TEST(PaneDownload, TheProgramWaitsForTheAnswerAndTheFileStreamsToItsPlace)
{
    // An iTerm2 download from a real program: 2 MiB, more than the PTY's
    // pipe holds, so the program waits while the question does, and its
    // output after the file shows only after the answer. Half of the file
    // comes, then the program waits for a line: the half is in "name.part",
    // which becomes "name" once whole.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString source = dir.filePath(QStringLiteral("source"));
    {
        QFile f(source);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        QByteArray data(2 << 20, '\0');
        for (int i = 0; i < data.size(); ++i)
            data[i] = char(i * 7);
        f.write(data);
    }
    // A whole number of base64 groups: 3 bytes each.
    constexpr qint64 Half = 349525 * 3;
    const QString target = dir.filePath(QStringLiteral("got.bin"));
    const auto run = [&](const QString& answer, const char* tail) {
        Pane pane{AfterglowVt::TerminalOptions{}};
        QString asked;
        qint64 askedSize = -2;
        QObject::connect(&pane, &Pane::downloadRequested, [&](const QString& name, qint64 size, quint64) {
            asked = name;
            askedSize = size;
        });
        bool finished = false;
        bool savedOk = false;
        QObject::connect(&pane, &Pane::downloadFinished, [&](const QString&, bool ok, const QString&) {
            finished = true;
            savedOk = ok;
        });
        AfterglowPty::SpawnOptions options;
        options.argv = {"/bin/sh", "-c",
                        "printf '\\033]1337;File=name=Z290LmJpbg==;size=2097152:'; head -c " + std::to_string(Half)
                            + " \"$0\" | base64 -w 0; read -r _; tail -c +" + std::to_string(Half + 1)
                            + " \"$0\" | base64 -w 0; printf '\\007" + tail + "'; exec cat",
                        QFile::encodeName(source).toStdString()};
        QString error;
        EXPECT_TRUE(pane.start(options, QStringLiteral("xterm-256color"), 40, 5, 10, 20, &error))
            << error.toStdString();
        EXPECT_TRUE(waitFor([&] {
            return !asked.isEmpty();
        }));
        EXPECT_EQ(asked, QStringLiteral("got.bin"));
        EXPECT_EQ(askedSize, 2 << 20);
        // The program waits: the terminal holds what came after the question.
        EXPECT_TRUE(holdsInput(pane));
        pane.answerDownload(answer);
        if (!answer.isEmpty()) {
            // The first half streams to the part file, and the file is not
            // there before it is whole.
            EXPECT_TRUE(waitFor([&] {
                return sizeOf(target + QStringLiteral(".part")) == Half;
            }));
            EXPECT_FALSE(QFileInfo::exists(target));
        }
        pane.sendInput("\n");
        EXPECT_TRUE(waitFor([&] {
            pane.refreshSnapshot();
            return pane.text(AfterglowVt::Terminal::TextScope::Screen).contains(QLatin1String(tail));
        }));
        // The base64 of a refused download goes nowhere, the screen neither.
        EXPECT_FALSE(pane.text(AfterglowVt::Terminal::TextScope::Screen).contains(QLatin1String("AAcOFRwj")));
        if (!answer.isEmpty()) {
            EXPECT_TRUE(waitFor([&] {
                return finished;
            }));
        }
        return savedOk;
    };
    EXPECT_TRUE(run(target, "saved"));
    QFile got(target);
    ASSERT_TRUE(got.open(QIODevice::ReadOnly));
    QFile want(source);
    ASSERT_TRUE(want.open(QIODevice::ReadOnly));
    EXPECT_TRUE(got.readAll() == want.readAll());
    EXPECT_FALSE(QFileInfo::exists(target + QStringLiteral(".part")));
    // Refused: no file, and the program goes on.
    run({}, "refused");
    EXPECT_EQ(QDir(dir.path()).entryList(QDir::Files),
              (QStringList{QStringLiteral("got.bin"), QStringLiteral("source")}));
}

TEST(PaneDownload, APaneThatClosesWhileItsProgramWaitsForAnAnswerClosesAtOnce)
{
    // The reader thread paused the output for the question: the pane's end
    // stops it, without an answer, and an accepted download that did not
    // finish leaves no part file.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    for (const bool accept : {false, true}) {
        SCOPED_TRACE(accept ? "accepted" : "asked");
        auto pane = std::make_unique<Pane>(AfterglowVt::TerminalOptions{});
        bool asked = false;
        QObject::connect(pane.get(), &Pane::downloadRequested, [&](const QString&, qint64, quint64) {
            asked = true;
        });
        AfterglowPty::SpawnOptions options;
        options.argv = {"/bin/sh", "-c", "printf '\\033]1337;File=name=Z290LmJpbg==:QUJD'; exec sleep 60"};
        QString error;
        ASSERT_TRUE(pane->start(options, QStringLiteral("xterm-256color"), 40, 5, 10, 20, &error))
            << error.toStdString();
        ASSERT_TRUE(waitFor([&] {
            return asked;
        }));
        const QString target = dir.filePath(QStringLiteral("got.bin"));
        if (accept) {
            pane->answerDownload(target);
            ASSERT_TRUE(waitFor([&] {
                return sizeOf(target + QStringLiteral(".part")) == 3;
            }));
        }
        QElapsedTimer closing;
        closing.start();
        pane.reset();
        EXPECT_LT(closing.elapsed(), 2000);
        // The writer's thread removes the part file after the pane went: the
        // pane does not wait for the disk (DownloadFile::release).
        EXPECT_TRUE(waitFor([&] {
            return QDir(dir.path()).entryList(QDir::Files).isEmpty();
        })) << QDir(dir.path()).entryList(QDir::Files).join(QLatin1Char(' ')).toStdString();
    }
}

TEST(PaneDownload, AProgramThatExitsInTheMiddleOfItsDownloadEndsIt)
{
    // A held pane keeps the terminal after the program's exit: the download
    // that its program did not end ends there, not when the pane closes.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    Pane pane{AfterglowVt::TerminalOptions{}};
    pane.setHold(true);
    bool asked = false;
    bool finished = false;
    bool saved = true;
    QObject::connect(&pane, &Pane::downloadRequested, [&](const QString&, qint64, quint64) {
        asked = true;
    });
    QObject::connect(&pane, &Pane::downloadFinished, [&](const QString&, bool ok, const QString&) {
        finished = true;
        saved = ok;
    });
    AfterglowPty::SpawnOptions options;
    options.argv = {"/bin/sh", "-c", "printf '\\033]1337;File=name=Z290LmJpbg==:QUJD'; read -r _"};
    QString error;
    ASSERT_TRUE(pane.start(options, QStringLiteral("xterm-256color"), 40, 5, 10, 20, &error)) << error.toStdString();
    ASSERT_TRUE(waitFor([&] {
        return asked;
    }));
    const QString target = dir.filePath(QStringLiteral("got.bin"));
    pane.answerDownload(target);
    ASSERT_TRUE(waitFor([&] {
        return sizeOf(target + QStringLiteral(".part")) == 3;
    }));
    pane.sendInput("\n");
    ASSERT_TRUE(waitFor([&] {
        return finished;
    }));
    EXPECT_FALSE(saved);
    EXPECT_TRUE(pane.isHeld());
    EXPECT_TRUE(QDir(dir.path()).entryList(QDir::Files).isEmpty());
}

TEST(PaneDownload, AFileThatTakesTheNameWhileTheDownloadStreamsStays)
{
    // The name was free at the answer. A file that came to it before the
    // end stays, and the download takes the next free name, unless the
    // user chose to replace the file (Save As).
    for (const bool replace : {false, true}) {
        SCOPED_TRACE(replace ? "replace" : "keep");
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        Pane pane{AfterglowVt::TerminalOptions{}};
        bool asked = false;
        bool finished = false;
        bool saved = false;
        QString savedPath;
        QObject::connect(&pane, &Pane::downloadRequested, [&](const QString&, qint64, quint64) {
            asked = true;
        });
        QObject::connect(&pane, &Pane::downloadFinished, [&](const QString& path, bool ok, const QString&) {
            finished = true;
            saved = ok;
            savedPath = path;
        });
        AfterglowPty::SpawnOptions options;
        options.argv = {"/bin/sh", "-c",
                        "printf '\\033]1337;File=name=Z290LmJpbg==:QUJD'; read -r _; printf '\\007'; exec sleep 60"};
        QString error;
        ASSERT_TRUE(pane.start(options, QStringLiteral("xterm-256color"), 40, 5, 10, 20, &error))
            << error.toStdString();
        ASSERT_TRUE(waitFor([&] {
            return asked;
        }));
        const QString target = dir.filePath(QStringLiteral("got.bin"));
        pane.answerDownload(target, 0, replace);
        ASSERT_TRUE(waitFor([&] {
            return sizeOf(target + QStringLiteral(".part")) == 3;
        }));
        {
            QFile mine(target);
            ASSERT_TRUE(mine.open(QIODevice::WriteOnly));
            mine.write("mine");
        }
        pane.sendInput("\n");
        ASSERT_TRUE(waitFor([&] {
            return finished;
        }));
        EXPECT_TRUE(saved);
        const QString expected = replace ? target : dir.filePath(QStringLiteral("got (1).bin"));
        EXPECT_EQ(savedPath, expected);
        QFile got(expected);
        ASSERT_TRUE(got.open(QIODevice::ReadOnly));
        EXPECT_EQ(got.readAll(), QByteArray("ABC"));
        if (!replace) {
            QFile kept(target);
            ASSERT_TRUE(kept.open(QIODevice::ReadOnly));
            EXPECT_EQ(kept.readAll(), QByteArray("mine"));
        }
        EXPECT_FALSE(QFileInfo::exists(target + QStringLiteral(".part")));
    }
}

TEST(PaneDownload, TheNumberedNameKeepsTheExtension)
{
    using Afterglow::numberedDownloadName;
    EXPECT_EQ(numberedDownloadName(QStringLiteral("got.bin"), 0), QStringLiteral("got.bin"));
    EXPECT_EQ(numberedDownloadName(QStringLiteral("got.bin"), 2), QStringLiteral("got (2).bin"));
    EXPECT_EQ(numberedDownloadName(QStringLiteral("README"), 1), QStringLiteral("README (1)"));
    // The program chose the stem: a %2 in it stays as it is.
    EXPECT_EQ(numberedDownloadName(QStringLiteral("a%2b.txt"), 3), QStringLiteral("a%2b (3).txt"));
}

TEST(PaneDownload, AResetEndsTheQuestionAndTheOutputGoesOn)
{
    // The question of a download that a reset ends gets no answer: the
    // output that it paused goes on.
    Pane pane{AfterglowVt::TerminalOptions{}};
    bool asked = false;
    bool cancelled = false;
    QObject::connect(&pane, &Pane::downloadRequested, [&](const QString&, qint64, quint64) {
        asked = true;
    });
    QObject::connect(&pane, &Pane::downloadCancelled, [&] {
        cancelled = true;
    });
    AfterglowPty::SpawnOptions options;
    options.argv = {"/bin/sh", "-c",
                    "printf '\\033]1337;File=name=Z290LmJpbg==:QUJD'; read -r _; printf after; exec sleep 60"};
    QString error;
    ASSERT_TRUE(pane.start(options, QStringLiteral("xterm-256color"), 40, 5, 10, 20, &error)) << error.toStdString();
    ASSERT_TRUE(waitFor([&] {
        return asked;
    }));
    pane.resetTerminal();
    EXPECT_TRUE(waitFor([&] {
        return cancelled;
    }));
    EXPECT_FALSE(holdsInput(pane));
    pane.sendInput("\n");
    EXPECT_TRUE(waitFor([&] {
        return pane.text(AfterglowVt::Terminal::TextScope::Screen).contains(QLatin1String("after"));
    }));
    // An answer to the question that ended does nothing.
    pane.answerDownload(QStringLiteral("/nonexistent/got.bin"));
}

TEST(PaneDownload, TheSuggestedNameIsCleanAndNeverHidden)
{
    using Afterglow::cleanDownloadName;
    EXPECT_EQ(cleanDownloadName("report.pdf"), QStringLiteral("report.pdf"));
    // The last part of a path, of either separator.
    EXPECT_EQ(cleanDownloadName("../../etc/passwd"), QStringLiteral("passwd"));
    EXPECT_EQ(cleanDownloadName("C:\\Users\\me\\notes.txt"), QStringLiteral("notes.txt"));
    // No hidden file, and nothing that names a directory.
    EXPECT_EQ(cleanDownloadName(".bashrc"), QStringLiteral("bashrc"));
    EXPECT_EQ(cleanDownloadName("..."), QStringLiteral("download.bin"));
    EXPECT_EQ(cleanDownloadName(".."), QStringLiteral("download.bin"));
    EXPECT_EQ(cleanDownloadName(""), QStringLiteral("download.bin"));
    // Control and format characters go.
    EXPECT_EQ(cleanDownloadName("a\x1b[31mb\xe2\x80\xae"
                                "c"),
              QStringLiteral("a[31mbc"));
    // A long name keeps its short extension, within 200 bytes.
    const QString cut = cleanDownloadName(std::string(300, 'x') + ".tar");
    EXPECT_TRUE(cut.endsWith(QStringLiteral(".tar")));
    EXPECT_EQ(cut.toUtf8().size(), 200);
}

TEST(PaneUpload, TheAnswerGoesOnceToTheCommandThatAsked)
{
    // it2ul's request: the window hears of one at a time. Only a person's
    // cancel sends Ctrl+C; a refusal of the settings sends nothing, and so
    // does an answer after the command that asked ended, which would go to
    // the shell.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("input"));
    Pane pane{AfterglowVt::TerminalOptions{}};
    int asked = 0;
    int cancelled = 0;
    QObject::connect(&pane, &Pane::uploadAsked, [&] {
        ++asked;
    });
    QObject::connect(&pane, &Pane::uploadCancelled, [&] {
        ++cancelled;
    });
    AfterglowPty::SpawnOptions options;
    options.argv = {"/bin/sh", "-c", R"(stty raw -echo && printf x && exec cat > "$0")",
                    QFile::encodeName(out).toStdString()};
    QString error;
    ASSERT_TRUE(pane.start(options, QStringLiteral("xterm-256color"), 20, 5, 10, 20, &error)) << error.toStdString();
    ASSERT_TRUE(waitFor([&] {
        return pane.text(AfterglowVt::Terminal::TextScope::Screen).contains(QLatin1Char('x'));
    }));
    const auto request = [&] {
        pane.withTerminal([](AfterglowVt::Terminal& terminal) {
            terminal.feed("\x1b]1337;RequestUpload=format=tgz\x07");
        });
        QCoreApplication::sendPostedEvents(&pane, QEvent::MetaCall);
    };
    request();
    EXPECT_EQ(asked, 1);
    // A second request while one waits: nothing for it.
    request();
    EXPECT_EQ(asked, 1);
    pane.refuseUpload();
    request();
    EXPECT_EQ(asked, 2);
    EXPECT_TRUE(pane.answerUpload(std::string("ok\nQUJD\n\n")));
    request();
    EXPECT_EQ(asked, 3);
    EXPECT_TRUE(pane.answerUpload(std::nullopt));
    // The command ends before the answer.
    request();
    EXPECT_EQ(asked, 4);
    EXPECT_EQ(cancelled, 0);
    pane.withTerminal([](AfterglowVt::Terminal& terminal) {
        terminal.feed("\x1b]133;C\x1b\\\x1b]133;D;0\x1b\\");
    });
    // The window hears of it, once, and closes its question.
    QCoreApplication::sendPostedEvents(&pane, QEvent::MetaCall);
    EXPECT_EQ(cancelled, 1);
    EXPECT_FALSE(pane.answerUpload(std::string("ok\nREVG\n\n")));
    // A reset ends no command: the question stays, and the answer goes to
    // the program that still waits for it.
    request();
    EXPECT_EQ(asked, 5);
    pane.resetTerminal();
    QCoreApplication::sendPostedEvents(&pane, QEvent::MetaCall);
    EXPECT_EQ(cancelled, 1);
    EXPECT_TRUE(pane.answerUpload(std::string("ok\nREVG\n\n")));
    ASSERT_TRUE(pane.sendReply("END"));
    QByteArray input;
    EXPECT_TRUE(waitFor([&] {
        QFile file(out);
        input = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        return input.endsWith("END");
    }));
    EXPECT_EQ(input,
              QByteArray("ok\nQUJD\n\n\x03"
                         "ok\nREVG\n\nEND"));
}

TEST(PaneUpload, WithoutShellIntegrationTheAnswerGoesOnlyToTheGroupThatAsked)
{
    // No OSC 133 says that the command ended: another process group in the
    // foreground at the answer does. The answer would go to the shell.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString out = dir.filePath(QStringLiteral("input"));
    Pane pane{AfterglowVt::TerminalOptions{}};
    int asked = 0;
    QObject::connect(&pane, &Pane::uploadAsked, [&] {
        ++asked;
    });
    AfterglowPty::SpawnOptions options;
    // With job control (set -m), the subshell is a foreground job of its
    // own group, as a command of an interactive shell is.
    options.argv = {"/bin/sh", "-c", R"(set -m; stty raw -echo; (printf x; read -r _); printf y; exec cat > "$0")",
                    QFile::encodeName(out).toStdString()};
    QString error;
    ASSERT_TRUE(pane.start(options, QStringLiteral("xterm-256color"), 20, 5, 10, 20, &error)) << error.toStdString();
    const auto screen = [&pane] {
        pane.refreshSnapshot();
        return pane.text(AfterglowVt::Terminal::TextScope::Screen);
    };
    const auto foreground = [&pane] {
        const auto stat = AfterglowPty::processStat(pane.programPid());
        return stat ? pid_t(stat->foregroundGroup) : pid_t(-1);
    };
    ASSERT_TRUE(waitFor([&] {
        return screen().contains(QLatin1Char('x')) && foreground() != pane.programPid();
    }));
    const auto request = [&] {
        pane.withTerminal([](AfterglowVt::Terminal& terminal) {
            terminal.feed("\x1b]1337;RequestUpload=format=tgz\x07");
        });
        QCoreApplication::sendPostedEvents(&pane, QEvent::MetaCall);
    };
    request();
    EXPECT_EQ(asked, 1);
    // The job ends: the shell has the terminal again.
    ASSERT_TRUE(pane.sendReply("\n"));
    ASSERT_TRUE(waitFor([&] {
        return screen().contains(QLatin1Char('y')) && foreground() == pane.programPid();
    }));
    EXPECT_FALSE(pane.answerUpload(std::string("ok\nREVG\n\n")));
    // The group that asks now gets its answer.
    request();
    EXPECT_EQ(asked, 2);
    EXPECT_TRUE(pane.answerUpload(std::string("ok\nQUJD\n\n")));
    ASSERT_TRUE(pane.sendReply("END"));
    QByteArray input;
    EXPECT_TRUE(waitFor([&] {
        QFile file(out);
        input = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        return input.endsWith("END");
    }));
    EXPECT_EQ(input, QByteArray("ok\nQUJD\n\nEND"));
}

TEST(PaneImages, AReadOfTheTerminalGoesOnWhileAnImageDecodes)
{
    // A decoder that starts 3 s late: the program's first image waits for
    // it on the reader thread, which lets the pane's lock go meanwhile. What
    // a frame or a key reads (readTerminal) does not wait for the image, and
    // neither do a move of the view, what a relayout or a tab switch does to
    // a pane that does not change (a resize to its size, the same
    // visibility), and a real change of its size or visibility, which the
    // terminal and the program take at the end of the feed.
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    const QString sleep = QStandardPaths::findExecutable(QStringLiteral("sleep"));
    ASSERT_FALSE(sleep.isEmpty());
    // The decoder starts with an empty environment: no PATH.
    const QString server = dir.filePath(QStringLiteral("slow-decoder"));
    {
        QFile f(server);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("#!/bin/sh\n'" + QFile::encodeName(sleep) + "' 3\nexec '" AFTERGLOW_MUX "' \"$@\"\n");
        ASSERT_TRUE(f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
    }
    Afterglow::ProgramImages::setServerExecutable(QFile::encodeName(server).toStdString());
    const QString output = dir.filePath(QStringLiteral("output"));
    {
        QFile f(output);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        // Visibility reports (mode 2033), which the program reads after
        // the image.
        f.write(QByteArray::fromStdString("\x1b[?2033hBEFORE\x1b_Ga=T,f=100,q=2;"
                                          + AfterglowProto::base64Encode(AfterglowImage::TestFiles::Png)
                                          + "\x1b\\AFTER"));
    }
    const QString input = dir.filePath(QStringLiteral("input"));
    Pane pane{AfterglowVt::TerminalOptions{}};
    AfterglowPty::SpawnOptions options;
    options.argv = {"/bin/sh", "-c", "stty raw -echo; cat \"$0\"; exec cat > \"$1\"",
                    QFile::encodeName(output).toStdString(), QFile::encodeName(input).toStdString()};
    QString error;
    ASSERT_TRUE(pane.start(options, QStringLiteral("xterm-256color"), 40, 5, 10, 20, &error)) << error.toStdString();
    QElapsedTimer total;
    total.start();
    qint64 slowest = 0;
    qint64 before = -1;
    qint64 after = -1;
    int cols = 40;
    bool visible = true;
    while (after < 0 && total.elapsed() < 20000) {
        QElapsedTimer one;
        one.start();
        const std::string text = pane.readTerminal([](const AfterglowVt::Terminal& t) {
            return t.text(AfterglowVt::Terminal::TextScope::Screen);
        });
        // The image decodes: the pane gets narrower, and hidden, once.
        if (before < 0 && text.find("BEFORE") != std::string::npos && text.find("AFTER") == std::string::npos) {
            cols = 30;
            visible = false;
        }
        pane.resize(cols, 5, 10, 20);
        pane.setVisible(visible);
        pane.scrollViewport(1);
        pane.setViewportOffset(0);
        slowest = std::max(slowest, one.elapsed());
        if (before < 0 && text.find("BEFORE") != std::string::npos)
            before = total.elapsed();
        if (text.find("AFTER") != std::string::npos)
            after = total.elapsed();
        QCoreApplication::processEvents();
        QThread::msleep(5);
    }
    ASSERT_GE(before, 0);
    ASSERT_GE(after, 0) << "the image never decoded";
    // The whole binary in one process: an earlier test started the decoder.
    if (after - before < 2000)
        GTEST_SKIP() << "the decoder started before this test: run it in a process of its own";
    EXPECT_LT(slowest, 500) << "a read waited for the decode";
    pane.refreshSnapshot();
    EXPECT_EQ(pane.snapshot().images.size(), 1u);
    // The feed that decoded gave the terminal what the window asked meanwhile.
    ASSERT_EQ(cols, 30) << "the pane showed no text before the image";
    EXPECT_EQ(pane.cols(), 30);
    QByteArray reports;
    EXPECT_TRUE(waitFor([&] {
        QFile file(input);
        reports = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        return reports.contains("\x1b[?999;2n");
    })) << reports.toStdString();
    EXPECT_EQ(reports, QByteArray("\x1b[?999;1n\x1b[?999;2n"));
}

TEST(PaneImages, ProgramImagesDecodeInTheDecoderProcessWithTheirColours)
{
    // A pane decodes the images of its program as a session pane does: in
    // aglow-mux's decoding processes, which convert an ICC profile and a
    // cICP colour space to sRGB.
    ASSERT_NE(Afterglow::ProgramImages::decoder(), nullptr) << "aglow-mux " AFTERGLOW_MUX " cannot run";
    Pane pane{AfterglowVt::TerminalOptions{}};
    pane.resize(20, 10, 10, 20);
    const auto near = [](const std::vector<std::uint8_t>& px, std::size_t at, int r, int g, int b) {
        return px.size() >= at + 4 && std::abs(px[at] - r) <= 1 && std::abs(px[at + 1] - g) <= 1
            && std::abs(px[at + 2] - b) <= 1;
    };
    // Whether an image of the snapshot starts with @p first, then @p second.
    // The snapshot keeps no order of arrival.
    const auto shows = [&](std::array<int, 3> first, std::array<int, 3> second) {
        pane.refreshSnapshot();
        for (const auto& image : pane.snapshot().images) {
            if (image.frame && near(image.frame->rgba, 0, first[0], first[1], first[2])
                && near(image.frame->rgba, 4, second[0], second[1], second[2]))
                return true;
        }
        return false;
    };
    // A Kitty PNG (f=100) in Adobe RGB, then an iTerm2 PNG in Display P3 (cICP).
    pane.withTerminal([](AfterglowVt::Terminal& terminal) {
        terminal.feed("\x1b_Ga=T,f=100,q=2;" + AfterglowProto::base64Encode(AfterglowImage::TestFiles::PngAdobe)
                      + "\x1b\\");
        terminal.feed("\x1b[5;1H\x1b]1337;File=inline=1:"
                      + AfterglowProto::base64Encode(AfterglowImage::TestFiles::PngCicpP3) + "\x07");
    });
    EXPECT_TRUE(shows({66, 151, 203}, {0, 201, 0}));
    EXPECT_TRUE(shows({83, 152, 205}, {0, 204, 0}));
    // A WebP, which the decoder reads with libwebp.
    pane.withTerminal([](AfterglowVt::Terminal& terminal) {
        terminal.feed("\x1b[8;1H\x1b]1337;File=inline=1:"
                      + AfterglowProto::base64Encode(AfterglowImage::TestFiles::WebpAdobe) + "\x07");
    });
    EXPECT_TRUE(shows({66, 151, 203}, {66, 151, 203}));
    EXPECT_EQ(pane.snapshot().images.size(), 3u);
}

TEST(PaneAnimation, AnITerm2GifPlaysThroughThePanesDecoder)
{
    // OSC 1337 with an animated GIF, decoded by the pane as the window does:
    // red, then blue after its 100 ms, as iTerm2 plays it.
    ASSERT_NE(Afterglow::ProgramImages::decoder(), nullptr) << "aglow-mux " AFTERGLOW_MUX " cannot run";
    Pane pane{AfterglowVt::TerminalOptions{}};
    pane.resize(20, 10, 10, 20);
    const auto shown = [&] {
        const auto& images = pane.snapshot().images;
        return images.size() == 1 && images[0].frame ? images[0].frame->rgba : std::vector<std::uint8_t>{};
    };
    const auto pixels = [](std::initializer_list<std::uint8_t> px) {
        std::vector<std::uint8_t> v;
        for (int i = 0; i < 16; ++i)
            v.insert(v.end(), px);
        return v;
    };
    // Output that moves the viewport first, so that the first refresh ticks
    // the animation, as the output of a program does (ptyData).
    pane.withTerminal([](AfterglowVt::Terminal& terminal) {
        terminal.feed(std::string(20, '\n'));
        terminal.feed("\x1b]1337;File=inline=1:" + AfterglowProto::base64Encode(AfterglowImage::TestFiles::Gif)
                      + "\x07");
    });
    pane.refreshSnapshot();
    ASSERT_EQ(shown(), pixels({255, 0, 0, 255}));
    ASSERT_TRUE(waitFor([&] {
        QCoreApplication::sendPostedEvents(&pane, QEvent::MetaCall);
        pane.refreshSnapshot();
        return shown() == pixels({0, 0, 255, 255});
    }));
    // And it loops: red again.
    ASSERT_TRUE(waitFor([&] {
        QCoreApplication::sendPostedEvents(&pane, QEvent::MetaCall);
        pane.refreshSnapshot();
        return shown() == pixels({255, 0, 0, 255});
    }));
}

TEST(PaneAnimation, AnITerm2ApngPlaysItsFrames)
{
    // OSC 1337 with an APNG of three frames: red, blue over red in the
    // middle, then green in a corner over red again. The decoder process
    // draws each frame.
    ASSERT_NE(Afterglow::ProgramImages::decoder(), nullptr) << "aglow-mux " AFTERGLOW_MUX " cannot run";
    Pane pane{AfterglowVt::TerminalOptions{}};
    pane.resize(20, 10, 10, 20);
    // The corner (0, 0) and the middle (1, 1) of the 4 by 4 frame shown.
    const auto pixel = [&](std::size_t x, std::size_t y) {
        QCoreApplication::sendPostedEvents(&pane, QEvent::MetaCall);
        pane.refreshSnapshot();
        const auto& images = pane.snapshot().images;
        if (images.size() != 1 || !images[0].frame || images[0].frame->rgba.size() != 64)
            return std::vector<std::uint8_t>{};
        const auto at = images[0].frame->rgba.begin() + std::ptrdiff_t((y * 4 + x) * 4);
        return std::vector<std::uint8_t>(at, at + 4);
    };
    pane.withTerminal([](AfterglowVt::Terminal& terminal) {
        terminal.feed(std::string(20, '\n'));
        terminal.feed("\x1b]1337;File=inline=1:" + AfterglowProto::base64Encode(AfterglowImage::TestFiles::Apng)
                      + "\x07");
    });
    const std::vector<std::uint8_t> red{255, 0, 0, 255};
    EXPECT_EQ(pixel(0, 0), red);
    EXPECT_EQ(pixel(1, 1), red);
    // Frame 2: blue at half alpha over the red middle, for 50 ms.
    EXPECT_TRUE(waitFor([&] {
        const auto middle = pixel(1, 1);
        return middle.size() == 4 && middle[2] > 100 && middle[0] < 160 && middle[3] == 255;
    }));
    // Frame 3: green in the corner, and the middle red again.
    EXPECT_TRUE(waitFor([&] {
        return pixel(0, 0) == std::vector<std::uint8_t>{0, 255, 0, 255};
    }));
    EXPECT_EQ(pixel(1, 1), red);
}

TEST(PaneAnimation, AnImageScrolledBackIntoViewShowsItsCurrentFrameAtOnce)
{
    Pane pane{AfterglowVt::TerminalOptions{}};
    pane.resize(20, 10, 10, 20);
    const auto feed = [&](const std::string& bytes) {
        pane.withTerminal([&](AfterglowVt::Terminal& terminal) {
            terminal.feed(bytes);
        });
    };
    const auto shown = [&] {
        const auto& images = pane.snapshot().images;
        return images.size() == 1 && images[0].frame ? images[0].frame->rgba : std::vector<std::uint8_t>{};
    };
    const std::vector<std::uint8_t> red{255, 0, 0, 255}, green{0, 255, 0, 255};
    // Output that moves the viewport, so that the first refresh ticks the animation.
    feed(std::string(20, '\n'));
    // A red frame and a green one, 600 ms each, played once: green stays.
    feed("\x1b_Ga=t,f=32,s=1,v=1,i=7,q=2;/wAA/w==\x1b\\");
    feed("\x1b_Ga=f,f=32,s=1,v=1,i=7,z=600,q=2;AP8A/w==\x1b\\");
    feed("\x1b_Ga=p,i=7,C=1,q=2\x1b\\");
    feed("\x1b_Ga=a,i=7,r=1,z=600,s=3,v=2,q=2\x1b\\");
    pane.refreshSnapshot();
    ASSERT_EQ(shown(), red);
    // The tick starts the animation's clock.
    QCoreApplication::sendPostedEvents(&pane, QEvent::MetaCall);
    QElapsedTimer clock;
    clock.start();
    // Output takes the image's row into the scrollback, where it asks for no frames.
    feed(std::string(30, '\n'));
    pane.refreshSnapshot();
    QCoreApplication::sendPostedEvents(&pane, QEvent::MetaCall);
    ASSERT_TRUE(pane.snapshot().images.empty());
    // The green frame is due while the image is out of view.
    ASSERT_TRUE(waitFor([&] {
        return clock.elapsed() > 700;
    }));
    int changes = 0;
    QObject::connect(&pane, &Pane::contentChanged, &pane, [&] {
        ++changes;
    });
    pane.withTerminal([](AfterglowVt::Terminal& terminal) {
        terminal.scrollViewport(30);
    });
    pane.refreshSnapshot();
    // One pass: the queued tick that the moved viewport asked for.
    QCoreApplication::sendPostedEvents(&pane, QEvent::MetaCall);
    EXPECT_EQ(changes, 1);
    pane.refreshSnapshot();
    EXPECT_EQ(shown(), green);
}

TEST_F(PaneProtocol, PathsStartFromTheReportedDirectoryOnlyWhileTheShellIsLocal)
{
    const std::string here = QSysInfo::machineHostName().toStdString();
    feed("\x1b]7;file://" + here + "/srv/project\x1b\\");
    EXPECT_EQ(pane.localPathDirectory({}), std::optional<QString>(QStringLiteral("/srv/project")));
    feed("\x1b]7;file://localhost/srv/other\x1b\\");
    EXPECT_EQ(pane.localPathDirectory({}), std::optional<QString>(QStringLiteral("/srv/other")));
    // A directory on another host: its paths are that host's files.
    feed("\x1b]7;file://far-away-host/home/someone\x1b\\");
    EXPECT_EQ(pane.localPathDirectory({}), std::nullopt);
    feed("\x1b]7;file://" + here + "/srv/project\x1b\\");
    ASSERT_EQ(pane.localPathDirectory({}), std::optional<QString>(QStringLiteral("/srv/project")));
    // A shell on another host (OSC 3008), whatever directory it reported.
    feed("\x1b]3008;start=r1;type=remote;user=someone;hostname=far-away-host\x1b\\");
    EXPECT_EQ(pane.localPathDirectory({}), std::nullopt);
    feed("\x1b]3008;end=r1;exit=success\x1b\\");
    EXPECT_EQ(pane.localPathDirectory({}), std::optional<QString>(QStringLiteral("/srv/project")));
    // A container is another machine's files too.
    feed("\x1b]3008;start=c1;type=container;container=box-of-tools\x1b\\");
    EXPECT_EQ(pane.localPathDirectory({}), std::nullopt);
}

TEST_F(PaneProtocol, APaneWhoseForegroundJobIsARemoteClientHasNoLocalPaths)
{
    // Opened to run ssh: its output names the far host's files, though no
    // shell integration there says so.
    pane.setLaunchedProgram(QStringLiteral("ssh"));
    AfterglowPty::SpawnOptions options;
    options.argv = {"/bin/sh", "-c", "stty -echo; exec cat"};
    QString error;
    ASSERT_TRUE(pane.start(options, QStringLiteral("xterm-256color"), 20, 5, 10, 20, &error)) << error.toStdString();
    feed("\x1b]7;file://localhost/srv/project\x1b\\");
    ASSERT_EQ(pane.foregroundJob(), QStringLiteral("ssh"));
    const QStringList clients{QStringLiteral("ssh"), QStringLiteral("mosh-client")};
    EXPECT_EQ(pane.localPathDirectory(clients), std::nullopt);
    // A program that is not in the list is local.
    EXPECT_EQ(pane.localPathDirectory({QStringLiteral("mosh-client"), QStringLiteral("telnet")}),
              std::optional<QString>(QStringLiteral("/srv/project")));
    EXPECT_EQ(pane.localPathDirectory({}), std::optional<QString>(QStringLiteral("/srv/project")));
    // The kernel keeps 15 bytes of a process name: a longer name in the list matches them.
    pane.setLaunchedProgram(QStringLiteral("remote-client-1"));
    EXPECT_EQ(pane.localPathDirectory({QStringLiteral("remote-client-1234")}), std::nullopt);
    EXPECT_EQ(pane.localPathDirectory({QStringLiteral("remote-client")}),
              std::optional<QString>(QStringLiteral("/srv/project")));
}

} // namespace

int main(int argc, char** argv)
{
    // Test discovery runs this binary too: it must never reach the user's
    // display or the platform theme of the session.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    QGuiApplication app(argc, argv);
    Afterglow::ProgramImages::setServerExecutable(AFTERGLOW_MUX);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
