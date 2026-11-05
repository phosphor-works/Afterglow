// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Pane.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QTimer>
#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <utility>

using Afterglow::DragDrop;
using Afterglow::Pane;

namespace {

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
