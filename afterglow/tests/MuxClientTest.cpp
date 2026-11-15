// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// A window's link to a session (MuxClient) against a daemon of the test's
// own (TestDaemon.h), with panes and no window: the channels of the panes
// that come and go.

#include "Pane.h"
#include "mux/MuxClient.h"

#include "TestDaemon.h"

#include <AfterglowProto/Ipc.h>

#include <QEventLoop>
#include <QGuiApplication>
#include <QTimer>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <functional>
#include <memory>
#include <thread>
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
    // A round trip: the daemon lists the session as detached once it let the
    // window go, and sends it nothing more. The window reads what came, and
    // a close that it queued runs before a call queued after it.
    ASSERT_TRUE(waitFor([&] {
        for (const AfterglowMux::SessionInfo& s : daemon.sessions()) {
            if (s.name == "left") return !s.attached;
        }
        return false;
    }));
    QCoreApplication::processEvents();
    bool drained = false;
    QMetaObject::invokeMethod(&context, [&drained] { drained = true; }, Qt::QueuedConnection);
    ASSERT_TRUE(waitFor([&] { return drained; }));
    EXPECT_EQ(closed, 0);
}

// Panes that exit, and panes that the window closes, free their channels:
// more of each kind than the window takes at once leave room for the next.
// Where a window lets a pane go between its PaneClosed and the end of its
// channel depends on where the stream splits what it reads, which the test
// cannot choose: the two kinds here let go after both and before both.
TEST(MuxClient, PanesThatGoFreeTheirChannels)
{
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    Shown shown = show(daemon, "cycle");
    ASSERT_TRUE(shown.client && shown.first);
    for (std::size_t i = 0; i < AfterglowMux::MaxSessionPanes + 8; ++i) {
        {
            // The window closes it at once, maybe before its PaneOpened came:
            // its program ends, and its frames are read and dropped.
            std::unique_ptr<Pane> pane = newPane(*shown.client, shown.client, {"/bin/cat"});
            ASSERT_TRUE(pane) << i;
        }
        // Its exit comes whole (its last frame and PaneClosed) before the
        // window lets it go. The wait runs the stream, which reads what came
        // of the panes before it too.
        std::unique_ptr<Pane> pane = newPane(*shown.client, shown.client, {"/bin/sh", "-c", "exit 3"});
        ASSERT_TRUE(pane) << i;
        ASSERT_TRUE(waitFor([&] { return !pane->showsSession(QStringLiteral("cycle")); })) << "pane " << i << " " << daemon.errors();
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

// The daemon holds the screens of a pane that the user cannot see. A read of
// its text asks for them (Pane::refreshSession): the read gives the text as
// it is now, and the output after it is held again.
TEST(MuxClient, AReadOfAHiddenPaneGivesItsCurrentText)
{
    using AfterglowVt::Terminal;
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    Shown shown = show(daemon, "hidden");
    ASSERT_TRUE(shown.client && shown.first);
    Pane& pane = *shown.first;
    const auto replica = [&pane] { return pane.withTerminal([](Terminal& t) { return t.text(Terminal::TextScope::Screen); }); };
    pane.setSessionVisible(false);
    EXPECT_TRUE(pane.sessionScreensHeld());
    // cat prints the title sequence that it reads: the title comes, and the
    // text before it is held.
    ASSERT_TRUE(pane.sendInput("hidden text\n\x1b]2;one\x07\n"));
    ASSERT_TRUE(waitFor([&] { return pane.title() == QStringLiteral("one"); })) << daemon.errors();
    EXPECT_EQ(replica().find("hidden text"), std::string::npos) << replica();
    EXPECT_TRUE(pane.text(Terminal::TextScope::Screen).contains(QStringLiteral("hidden text")));
    EXPECT_TRUE(pane.readText(Terminal::TextScope::All, 4096).value(QStringLiteral("text")).toString().contains(QStringLiteral("hidden text")));
    // Still hidden: the next read asks again.
    EXPECT_TRUE(pane.sessionScreensHeld());
    ASSERT_TRUE(pane.sendInput("more text\n\x1b]2;two\x07\n"));
    ASSERT_TRUE(waitFor([&] { return pane.title() == QStringLiteral("two"); })) << daemon.errors();
    EXPECT_EQ(replica().find("more text"), std::string::npos) << replica();
    EXPECT_TRUE(pane.text(Terminal::TextScope::Screen).contains(QStringLiteral("more text")));
    // Seen again, the screens come, and a read after one refresh needs none.
    pane.setSessionVisible(true);
    pane.refreshSession();
    EXPECT_FALSE(pane.sessionScreensHeld());
    ASSERT_TRUE(pane.sendInput("seen text\n"));
    EXPECT_TRUE(waitFor([&] { return replica().find("seen text") != std::string::npos; })) << replica();
}

// A window binds its panes before it shows: the daemon hears that the user
// cannot see them, maybe before their first frame went, then that the user
// can. The size that the window measured meanwhile reaches the replica with
// the screens that the Visible brings, without a read of the text. A pane
// that the user saw from the start holds nothing, so its reads ask the daemon
// for nothing.
TEST(MuxClient, APaneThatShowsAfterItsFirstFramesHasTheWindowsSize)
{
    using AfterglowVt::Terminal;
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options({"/bin/cat"}, "late"))) << daemon.lastError();
    QString error;
    const auto client = MuxClient::attach(QStringLiteral("late"), false, &error, daemon.sockets(), std::size_t(1) << 20, 1000);
    ASSERT_TRUE(client) << error.toStdString();
    ASSERT_EQ(client->layout().panes().size(), 1u);
    Pane pane(AfterglowVt::TerminalOptions{});
    pane.showSession(client, client->layout().panes().front());
    pane.setSessionVisible(false);
    pane.resize(40, 10, 8, 16);
    ASSERT_TRUE(waitFor([&] { return pane.sessionSynced(); })) << daemon.errors();
    pane.setSessionVisible(true);
    EXPECT_TRUE(waitFor([&] { return pane.cols() == 40 && pane.rows() == 10; }, 5000)) << pane.cols() << "x" << pane.rows();
    const auto replica = [&pane] { return pane.withTerminal([](Terminal& t) { return t.text(Terminal::TextScope::Screen); }); };
    ASSERT_TRUE(pane.sendInput("first\n"));
    EXPECT_TRUE(waitFor([&] { return replica().find("first") != std::string::npos; }, 5000)) << replica();

    // A split: a new pane that the user sees from the start.
    auto split = newPane(*client, client, {"/bin/cat"});
    ASSERT_TRUE(split);
    split->setSessionVisible(true);
    ASSERT_TRUE(waitFor([&] { return split->sessionSynced(); })) << daemon.errors();
    EXPECT_FALSE(split->sessionScreensHeld());
    ASSERT_TRUE(split->sendInput("second\n"));
    EXPECT_TRUE(waitFor([&] { return split->text(Terminal::TextScope::Screen).contains(QStringLiteral("second")); }, 5000));
}

// A pane that shows again asks for the screens that the daemon held, as the
// window does on a tab switch, and waits for nothing: the answer comes with
// the stream, and the pane holds nothing from then on. An answer to a
// Refresh asked before the pane hid again leaves it held.
TEST(MuxClient, APaneThatShowsAgainAsksForItsScreensWithoutAWait)
{
    using AfterglowVt::Terminal;
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    Shown shown = show(daemon, "again");
    ASSERT_TRUE(shown.client && shown.first);
    Pane& pane = *shown.first;
    const auto replica = [&pane] { return pane.withTerminal([](Terminal& t) { return t.text(Terminal::TextScope::Screen); }); };
    pane.setSessionVisible(false);
    ASSERT_TRUE(pane.sendInput("hidden text\n\x1b]2;one\x07\n"));
    ASSERT_TRUE(waitFor([&] { return pane.title() == QStringLiteral("one"); })) << daemon.errors();
    EXPECT_EQ(replica().find("hidden text"), std::string::npos) << replica();

    pane.setSessionVisible(true);
    const std::uint64_t ticket = pane.askSessionRefresh();
    EXPECT_NE(ticket, 0u);
    // Nothing read the stream yet: the pane still holds the older screens.
    EXPECT_TRUE(pane.sessionScreensHeld());
    // A read meanwhile waits for the same Refresh.
    EXPECT_EQ(pane.askSessionRefresh(), ticket);
    ASSERT_TRUE(waitFor([&] { return !pane.sessionScreensHeld(); })) << daemon.errors();
    EXPECT_NE(replica().find("hidden text"), std::string::npos) << replica();
    EXPECT_EQ(pane.askSessionRefresh(), 0u);

    // Asked while hidden, then shown before the answer came: that answer
    // lacks the screens that the daemon holds until it heard of the sight,
    // so the pane stays held for the answer to a Refresh asked since.
    pane.setSessionVisible(false);
    const std::uint64_t before = pane.askSessionRefresh();
    ASSERT_NE(before, 0u);
    pane.setSessionVisible(true);
    ASSERT_TRUE(shown.client->waitRefreshed({{pane.sessionPaneId(), before}}));
    EXPECT_TRUE(pane.sessionScreensHeld());
}

// Each pane hears of the answer to its own Refresh: one whose answer does
// not come in time leaves the others refreshed.
TEST(MuxClient, EachPaneHearsOfItsOwnRefresh)
{
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    Shown shown = show(daemon, "each");
    ASSERT_TRUE(shown.client && shown.first);
    std::unique_ptr<Pane> other = newPane(*shown.client, shown.client, {"/bin/cat"});
    ASSERT_TRUE(other);
    ASSERT_TRUE(waitFor([&] { return other->sessionSynced(); })) << daemon.errors();
    for (Pane* p : {shown.first.get(), other.get()}) {
        p->setSessionVisible(false);
        p->setSessionVisible(true);
        EXPECT_TRUE(p->sessionScreensHeld());
    }
    const std::uint64_t ticket = shown.first->askSessionRefresh();
    ASSERT_NE(ticket, 0u);
    // A ticket of the other pane that the daemon never answers: nobody asked.
    EXPECT_FALSE(shown.client->waitRefreshed({{shown.first->sessionPaneId(), ticket}, {other->sessionPaneId(), 1000}}, 500));
    // The first pane hears of its answer when the window reads it, whether
    // or not it came inside that wait.
    EXPECT_TRUE(waitFor([&] { return !shown.first->sessionScreensHeld(); })) << daemon.errors();
    EXPECT_TRUE(other->sessionScreensHeld());
}

// The answer to a call of `afterglow ctl` that waits behind the input of a
// pane reaches the client, also when a newer layout comes after it: only an
// older layout makes room for a newer one. The handler stops the daemon, so
// that the answer and the layout wait in the window's queue.
TEST(MuxClient, AnAnswerThatWaitsIsNoOldLayout)
{
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    Shown shown = show(daemon, "busy");
    ASSERT_TRUE(shown.client && shown.first);
    const std::uint32_t paneId = shown.first->sessionPaneId();
    bool asked = false;
    shown.client->setControlHandler([&](const QString& fn, const QJsonArray&) {
        asked = fn == QLatin1String("setTitle");
        ::kill(daemon.pid(), SIGSTOP);
        // More input than the stream lets go to a daemon that reads nothing:
        // the answer waits behind it.
        const std::string chunk(std::size_t(64) << 10, 'a');
        for (int i = 0; i < 96; ++i) shown.first->sendInput(chunk);
        // After the answer queued: a newer layout.
        QTimer::singleShot(0, [&] {
            AfterglowMux::Layout layout = shown.client->layout();
            layout.tabs.front().title = "newer";
            shown.client->sendLayout(layout);
            ::kill(daemon.pid(), SIGCONT);
        });
        return MuxClient::ControlAnswer{};
    });
    std::atomic<int> answered{-1}; ///< -1 until the call ends, then 0 or 1
    std::thread ctl([&] {
        AfterglowProto::IpcConnection connection;
        AfterglowProto::IpcConnection::Error error;
        bool ok = connection.connect(daemon.socket(), &error, 2000)
            && connection.call(AfterglowMux::Target, AfterglowMux::Fn::SelectSession,
                               MuxTest::Json::Array{AfterglowMux::ProtocolVersion, "busy"}, 5000, &error)
            && connection.call("control", "setTitle", MuxTest::Json::Array{std::int64_t(paneId), "t"}, 30000, &error);
        if (!ok) ADD_FAILURE() << error.code << " " << error.message;
        answered = ok ? 1 : 0;
    });
    const bool done = waitFor([&] { return answered >= 0; }, 40000);
    ::kill(daemon.pid(), SIGCONT);
    ctl.join();
    EXPECT_TRUE(done);
    EXPECT_TRUE(asked);
    EXPECT_EQ(answered, 1);
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
