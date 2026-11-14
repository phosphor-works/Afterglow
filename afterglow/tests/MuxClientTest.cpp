// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// A window's link to a session (MuxClient) against a daemon of the test's
// own (TestDaemon.h), with panes and no window: the channels of the panes
// that come and go.

#include "Pane.h"
#include "mux/MuxClient.h"

#include "TestDaemon.h"

#include <QEventLoop>
#include <QGuiApplication>
#include <QTimer>
#include <gtest/gtest.h>

#include <chrono>
#include <functional>
#include <memory>
#include <vector>

using Afterglow::MuxClient;
using Afterglow::Pane;

namespace {

/// Runs the event loop, which reads the stream, until @p done.
bool waitFor(const std::function<bool()>& done, int ms = 10000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        QEventLoop loop;
        QTimer::singleShot(5, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return true;
}

AfterglowMux::Launch launchOf(std::vector<std::string> argv, std::string launchedProgram = {})
{
    AfterglowMux::Launch launch;
    launch.argv = std::move(argv);
    launch.launchedProgram = std::move(launchedProgram);
    return launch;
}

const AfterglowMux::PaneSize Size{80, 24, 8, 16};

struct Shown
{
    std::shared_ptr<MuxClient> client;
    std::unique_ptr<Pane> first; ///< keeps the session while the others come and go
};

/// Shows session @p name, whose one pane runs `cat`.
Shown show(MuxTest::TestDaemon& daemon, const std::string& name, AfterglowMux::SessionOptions* made = nullptr)
{
    Shown shown;
    const AfterglowMux::SessionOptions options = daemon.options({"/bin/cat"}, name);
    if (made) *made = options;
    EXPECT_TRUE(daemon.newSession(options)) << daemon.lastError();
    QString error;
    shown.client = MuxClient::attach(QString::fromStdString(name), false, &error, daemon.sockets(), std::size_t(1) << 20, 1000);
    EXPECT_TRUE(shown.client) << error.toStdString();
    if (!shown.client) return shown;
    const std::vector<std::uint32_t> panes = shown.client->layout().panes();
    EXPECT_EQ(panes.size(), 1u);
    if (panes.size() != 1) return shown;
    shown.first = std::make_unique<Pane>(AfterglowVt::TerminalOptions{});
    shown.first->showSession(shown.client, panes.front());
    EXPECT_TRUE(waitFor([&] { return shown.first->sessionSynced(); }));
    return shown;
}

/// A new pane of the session, bound, that showed its first frame.
std::unique_ptr<Pane> newPane(MuxClient& client, const std::shared_ptr<MuxClient>& shared, std::vector<std::string> argv,
                              std::string launchedProgram = {})
{
    QString error;
    const auto id = client.newPane(launchOf(std::move(argv), std::move(launchedProgram)), Size, &error);
    EXPECT_TRUE(id) << error.toStdString();
    if (!id) return nullptr;
    auto pane = std::make_unique<Pane>(AfterglowVt::TerminalOptions{});
    pane->showSession(shared, *id);
    return pane;
}

// A window shows a session with more panes than the 64 channels that a
// stream takes by default: each pane has a channel of its own.
TEST(MuxClient, AWindowShowsMorePanesThanTheDefaultChannels)
{
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    AfterglowMux::SessionOptions made;
    Shown shown = show(daemon, "many", &made);
    ASSERT_TRUE(shown.client && shown.first);
    // The session's environment, as `afterglow mux new` gave it: a new pane
    // starts from it, not from this process's.
    EXPECT_TRUE(shown.client->environment() == made.environment);
    // The history that the attach asked for, which every replica keeps.
    EXPECT_EQ(shown.client->history(), 1000u);
    std::vector<std::unique_ptr<Pane>> panes;
    for (int i = 0; i < 99; ++i) {
        panes.push_back(newPane(*shown.client, shown.client, {"/bin/cat"}));
        ASSERT_TRUE(panes.back()) << i;
    }
    for (std::size_t i = 0; i < panes.size(); ++i) {
        ASSERT_TRUE(waitFor([&] { return panes[i]->sessionSynced(); })) << "pane " << i << " " << daemon.errors();
    }
    EXPECT_TRUE(shown.client->showing());
}

// A window that loses its session hears it of each pane as a pane that left
// the session, not as the exit of its program: the window closes them
// together (TerminalWindow::closeLostSessionPanes).
TEST(MuxClient, ADetachLeavesThePanesWithoutAnExit)
{
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    Shown shown = show(daemon, "left");
    ASSERT_TRUE(shown.client && shown.first);
    std::unique_ptr<Pane> second = newPane(*shown.client, shown.client, {"/bin/cat"});
    ASSERT_TRUE(second);
    ASSERT_TRUE(waitFor([&] { return second->sessionSynced(); })) << daemon.errors();
    int closed = 0;
    int left = 0;
    QObject context; // ends before the counters
    for (Pane* pane : {shown.first.get(), second.get()}) {
        QObject::connect(pane, &Pane::closed, &context, [&closed] { ++closed; });
        QObject::connect(pane, &Pane::sessionLeft, &context, [&left] { ++left; });
    }
    ASSERT_TRUE(daemon.call(AfterglowMux::Fn::Detach, MuxTest::Json::Array{"left"})) << daemon.lastError();
    EXPECT_TRUE(waitFor([&] { return left == 2; }));
    EXPECT_FALSE(shown.client->showing());
    EXPECT_TRUE(shown.first->leftSession());
    EXPECT_TRUE(second->leftSession());
    EXPECT_EQ(closed, 0);
}

// Panes that exit, and panes that the window closes, free their channels:
// many more of them than the window takes at once leave room for the next.
TEST(MuxClient, PanesThatGoFreeTheirChannels)
{
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    Shown shown = show(daemon, "cycle");
    ASSERT_TRUE(shown.client && shown.first);
    for (std::size_t i = 0; i < AfterglowMux::MaxSessionPanes + 8; ++i) {
        if (i % 2 == 0) {
            // Its exit comes whole (its last frame and PaneClosed) before
            // the window lets it go.
            std::unique_ptr<Pane> pane = newPane(*shown.client, shown.client, {"/bin/sh", "-c", "exit 3"});
            ASSERT_TRUE(pane) << i;
            ASSERT_TRUE(waitFor([&] { return !pane->showsSession(QStringLiteral("cycle")); })) << "pane " << i << " " << daemon.errors();
        } else {
            // The window closes it at once, maybe before its PaneOpened came:
            // its program ends, and its frames are read and dropped.
            std::unique_ptr<Pane> pane = newPane(*shown.client, shown.client, {"/bin/cat"});
            ASSERT_TRUE(pane) << i;
        }
        ASSERT_TRUE(shown.client->showing()) << i;
    }
    std::unique_ptr<Pane> last = newPane(*shown.client, shown.client, {"/bin/cat"});
    ASSERT_TRUE(last);
    EXPECT_TRUE(waitFor([&] { return last->sessionSynced(); })) << daemon.errors();
    EXPECT_TRUE(shown.client->showing());
    // Only the two panes that the window shows run on.
    EXPECT_TRUE(waitFor([&] {
        for (const AfterglowMux::SessionInfo& s : daemon.sessions()) {
            if (s.name == "cycle") return s.panes == 2;
        }
        return false;
    }));
}

// The jobs of several panes, for the question before a close, come on one
// connection: a pane that the daemon does not know asks nothing of the
// others.
TEST(MuxClient, TheJobsOfPanesComeTogether)
{
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    Shown shown = show(daemon, "jobs");
    ASSERT_TRUE(shown.client && shown.first);
    std::unique_ptr<Pane> first = newPane(*shown.client, shown.client, {"/bin/cat"}, "reader");
    std::unique_ptr<Pane> second = newPane(*shown.client, shown.client, {"/bin/cat"}, "writer");
    ASSERT_TRUE(first && second);
    ASSERT_TRUE(waitFor([&] { return first->sessionSynced() && second->sessionSynced(); })) << daemon.errors();
    const std::vector<QString> jobs = shown.client->paneJobs({first->sessionPaneId(), 0xfffffff0u, second->sessionPaneId()});
    ASSERT_EQ(jobs.size(), 3u);
    EXPECT_EQ(jobs[0], QStringLiteral("reader"));
    EXPECT_EQ(jobs[1], QString());
    EXPECT_EQ(jobs[2], QStringLiteral("writer"));
    EXPECT_EQ(shown.client->paneJob(second->sessionPaneId()), QStringLiteral("writer"));
}

} // namespace

int main(int argc, char** argv)
{
    // Test discovery runs this binary too: it must never reach the user's
    // display or the platform theme of the session.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QPA_PLATFORMTHEME", "generic");
    QGuiApplication app(argc, argv);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
