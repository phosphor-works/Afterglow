// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Pane.h"
#include "desktop/FileTransfer.h"

#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QSysInfo>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>

using Afterglow::DragDrop;
using Afterglow::Pane;

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
class PaneProtocol : public ::testing::Test {
protected:
    void SetUp() override
    {
        QObject::connect(&pane, &Pane::terminalProtocolLost,
                         [this](DragDrop::Lost lost, bool reset) { gaps.append({ lost, reset }); });
        QObject::connect(&pane, &Pane::terminalProtocolRequested, [this](int code, const QByteArray&) { codes.append(code); });
    }
    void feed(const std::string& bytes)
    {
        pane.withTerminal([&](AfterglowVt::Terminal& terminal) { terminal.feed(bytes); });
    }
    /// @p count commands of protocol @p code, each near its size limit.
    static std::string commands(int code, int count)
    {
        const std::string command = "\x1b]" + std::to_string(code) + ";t=r:x=1;"
            + std::string(code == 72 ? 8000 : 16000, 'A') + "\x1b\\";
        std::string bytes;
        for (int i = 0; i < count; ++i)
            bytes += command;
        return bytes;
    }
    /// More than the 16 MiB that the pane holds for the GUI thread.
    static std::string overflow(int code) { return commands(code, code == 72 ? 2100 : 1100); }
    static constexpr const char* Reset = "\x1b" "c";

    Pane pane { AfterglowVt::TerminalOptions {} };
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
    EXPECT_EQ(codes, QList<int> { 72 });
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
    QTimer::singleShot(0, &pane, [&] { other = true; });
    QMetaObject::invokeMethod(&pane, [&] { other = true; }, Qt::QueuedConnection);
    feed(commands(72, 1));
    pane.deliverProtocolCommands();
    EXPECT_EQ(codes, QList<int> { 72 });
    EXPECT_FALSE(other);
}

TEST_F(PaneProtocol, DeliveryWaitsForTheNextCommand)
{
    // The program writes until the GUI thread has its command, so the wait
    // ends early however the two threads are scheduled.
    std::atomic_bool stop { false };
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
    options.argv = { "/bin/sh", "-c", R"(stty raw -echo && printf '\033]72;t=e:x=1\033\\\033[cx' && exec cat > "$0")",
                     QFile::encodeName(out).toStdString() };
    QString error;
    ASSERT_TRUE(pane.start(options, QStringLiteral("xterm-256color"), 20, 5, 10, 20, &error)) << error.toStdString();
    // The PTY thread has read the whole write before the GUI thread runs the
    // command: an answer that does not wait for the command comes first.
    ASSERT_TRUE(waitFor([&] { return pane.text(AfterglowVt::Terminal::TextScope::Screen).contains(QLatin1Char('x')); },
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
class QueuedCalls : public QObject {
public:
    QueuedCalls() { QCoreApplication::instance()->installEventFilter(this); }
    ~QueuedCalls() override { QCoreApplication::instance()->removeEventFilter(this); }
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
    options.argv = { "/bin/sh", "-c", std::string("stty raw -echo && ") + script + R"( && exec cat > "$0")",
                     QFile::encodeName(out).toStdString() };
    QString error;
    EXPECT_TRUE(pane.start(options, QStringLiteral("xterm-256color"), 20, 5, 10, 20, &error)) << error.toStdString();
    EXPECT_TRUE(waitFor([&] { return pane.text(AfterglowVt::Terminal::TextScope::Screen).contains(QLatin1Char('x')); },
                        false));
    QByteArray input;
    EXPECT_TRUE(waitFor([&] {
        QFile file(out);
        input = file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        return std::all_of(expected.begin(), expected.end(), [&](const QByteArray& e) { return input.contains(e); });
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
    EXPECT_EQ(codes, QList<int> { 5113 });
    EXPECT_LE(calls.count, 3);
}

TEST_F(PaneProtocol, ReportsThatWaitPastTheBudgetAreRefused)
{
    int rejected = 0;
    QObject::connect(&pane, &Pane::inputRejected, [&] { ++rejected; });
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
    EXPECT_EQ(codes, (QList<int> { 5113, 72 }));
    EXPECT_TRUE(gaps.isEmpty());
}

TEST_F(PaneProtocol, EachReportFollowsOnlyTheCommandsBeforeIt)
{
    route(pane);
    const QByteArray reply = "\x1b]72;m=0:t=E;EINVAL\x1b\\";
    const auto input = answers(pane, R"(printf '\033]72;t=e:x=1\033\\\033[c\033]72;t=e:x=1\033\\\033[cx')",
                               { reply + DeviceAttributes + reply + DeviceAttributes });
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
    const auto input = answers(pane,
        R"(printf '\033]5113;ac=receive;id=x;sz=0\033\\\033[c\033]5113;ac=receive;id=y;sz=0\033\\\033[cx')",
        { first + DeviceAttributes + second + DeviceAttributes });
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
    EXPECT_EQ(codes, (QList<int> { 5113, 72 }));
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
                     [this](const QString& token, bool, const QStringList&, int) { pane.fileTransfers()->approve(token, true); });
    const QString out = dir.filePath(QStringLiteral("input"));
    AfterglowPty::SpawnOptions options;
    options.argv = { "/bin/sh", "-c", R"(stty raw -echo && printf x && exec cat > "$0")", QFile::encodeName(out).toStdString() };
    QString error;
    ASSERT_TRUE(pane.start(options, QStringLiteral("xterm-256color"), 20, 5, 10, 20, &error)) << error.toStdString();
    ASSERT_TRUE(waitFor([&] { return pane.text(AfterglowVt::Terminal::TextScope::Screen).contains(QLatin1Char('x')); }));
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
    QThread::msleep(300);
    feed("\x1b]5113;id=t;ac=cancel\x1b\\");
    pane.deliverProtocolCommands();
    QThread::msleep(300);
    feed("\x1b[c");
    EXPECT_TRUE(read(DeviceAttributes)) << input.right(200).toStdString();
}

TEST(PaneAnimation, AnImageScrolledBackIntoViewShowsItsCurrentFrameAtOnce)
{
    Pane pane { AfterglowVt::TerminalOptions {} };
    pane.resize(20, 10, 10, 20);
    const auto feed = [&](const std::string& bytes) {
        pane.withTerminal([&](AfterglowVt::Terminal& terminal) { terminal.feed(bytes); });
    };
    const auto shown = [&] {
        const auto& images = pane.snapshot().images;
        return images.size() == 1 && images[0].frame ? images[0].frame->rgba : std::vector<std::uint8_t> {};
    };
    const std::vector<std::uint8_t> red { 255, 0, 0, 255 }, green { 0, 255, 0, 255 };
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
    ASSERT_TRUE(waitFor([&] { return clock.elapsed() > 700; }));
    int changes = 0;
    QObject::connect(&pane, &Pane::contentChanged, &pane, [&] { ++changes; });
    pane.withTerminal([](AfterglowVt::Terminal& terminal) { terminal.scrollViewport(30); });
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
    EXPECT_EQ(pane.localPathDirectory(), std::optional<QString>(QStringLiteral("/srv/project")));
    feed("\x1b]7;file://localhost/srv/other\x1b\\");
    EXPECT_EQ(pane.localPathDirectory(), std::optional<QString>(QStringLiteral("/srv/other")));
    // A directory on another host: its paths are that host's files.
    feed("\x1b]7;file://far-away-host/home/someone\x1b\\");
    EXPECT_EQ(pane.localPathDirectory(), std::nullopt);
    feed("\x1b]7;file://" + here + "/srv/project\x1b\\");
    ASSERT_EQ(pane.localPathDirectory(), std::optional<QString>(QStringLiteral("/srv/project")));
    // A shell on another host (OSC 3008), whatever directory it reported.
    feed("\x1b]3008;start=r1;type=remote;user=someone;hostname=far-away-host\x1b\\");
    EXPECT_EQ(pane.localPathDirectory(), std::nullopt);
    feed("\x1b]3008;end=r1;exit=success\x1b\\");
    EXPECT_EQ(pane.localPathDirectory(), std::optional<QString>(QStringLiteral("/srv/project")));
    // A container is another machine's files too.
    feed("\x1b]3008;start=c1;type=container;container=box-of-tools\x1b\\");
    EXPECT_EQ(pane.localPathDirectory(), std::nullopt);
}

} // namespace

int main(int argc, char** argv)
{
    // Test discovery runs this binary too: it must never reach the user's session.
    if (!qEnvironmentVariableIsSet("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
