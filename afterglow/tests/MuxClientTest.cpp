// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// A window's link to a session (MuxClient) against a daemon of the test's
// own (TestDaemon.h), with panes and no window: the channels of the panes
// that come and go, a detach, the jobs of panes, the screens that the
// daemon holds and their refresh, a search while the history still comes,
// the answers to control calls that wait,
// panes that start and move out without a wait of the GUI thread, and a
// daemon that refuses the attach.

#include "Pane.h"
#include "SavedEnvironment.h"
#include "config/Settings.h"
#include "mux/MuxClient.h"
#include "mux/MuxSessions.h"

#include "TestDaemon.h"

#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/IpcServer.h>

#include <QDir>
#include <QEventLoop>
#include <QGuiApplication>
#include <QScopeGuard>
#include <QTimer>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <thread>
#include <vector>

#include <poll.h>
#include <unistd.h>

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

/// kill(@p pid, @p sig) for a daemon that started: TestDaemon::pid() is -1
/// before a start and after a wait, and kill(-1, SIGSTOP) would stop every
/// process of the user.
bool signalDaemon(pid_t pid, int sig)
{
    return pid > 0 && ::kill(pid, sig) == 0;
}

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
    shown.client = MuxClient::attach(QString::fromStdString(name), false, &error, AfterglowMux::Route{daemon.sockets(), {}, {}}, std::size_t(1) << 20, 1000);
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
    const std::vector<std::uint32_t> ids{first->sessionPaneId(), 0xfffffff0u, second->sessionPaneId()};
    const std::vector<std::optional<QString>> jobs = shown.client->paneJobs(ids);
    ASSERT_EQ(jobs.size(), 3u);
    EXPECT_EQ(jobs[0], std::optional<QString>(QStringLiteral("reader")));
    // The daemon refused the pane that it does not know: an answer, which
    // runs nothing.
    EXPECT_EQ(jobs[1], std::optional<QString>(QString()));
    EXPECT_EQ(jobs[2], std::optional<QString>(QStringLiteral("writer")));
    EXPECT_EQ(shown.client->paneJob(second->sessionPaneId()), QStringLiteral("writer"));

    // A daemon that does not answer gives no job for any pane, not an idle
    // one: the first call waits once, and the others are not asked.
    ASSERT_TRUE(signalDaemon(daemon.pid(), SIGSTOP));
    const auto start = std::chrono::steady_clock::now();
    const std::vector<std::optional<QString>> unanswered = shown.client->paneJobs(ids);
    const auto waited = std::chrono::steady_clock::now() - start;
    ASSERT_TRUE(signalDaemon(daemon.pid(), SIGCONT));
    ASSERT_EQ(unanswered.size(), 3u);
    for (const std::optional<QString>& job : unanswered) EXPECT_FALSE(job.has_value()) << job->toStdString();
    EXPECT_LT(waited, std::chrono::milliseconds(2 * AfterglowMux::CallTimeoutMs + 1000));
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
    const auto client = MuxClient::attach(QStringLiteral("late"), false, &error, AfterglowMux::Route{daemon.sockets(), {}, {}}, std::size_t(1) << 20, 1000);
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
    ASSERT_GT(daemon.pid(), 0);
    Shown shown = show(daemon, "busy");
    ASSERT_TRUE(shown.client && shown.first);
    const std::uint32_t paneId = shown.first->sessionPaneId();
    bool asked = false;
    shown.client->setControlHandler([&](const QString& fn, const QJsonArray&, qint64, MuxClient::ControlReply reply) {
        asked = fn == QLatin1String("setTitle");
        signalDaemon(daemon.pid(), SIGSTOP);
        // More input than the stream lets go to a daemon that reads nothing:
        // the answer waits behind it.
        const std::string chunk(std::size_t(64) << 10, 'a');
        for (int i = 0; i < 96; ++i) shown.first->sendInput(chunk);
        // After the answer queued: a newer layout.
        QTimer::singleShot(0, shown.client.get(), [&] {
            AfterglowMux::Layout layout = shown.client->layout();
            layout.tabs.front().title = "newer";
            shown.client->sendLayout(layout);
            signalDaemon(daemon.pid(), SIGCONT);
        });
        reply(MuxClient::ControlAnswer{});
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
    signalDaemon(daemon.pid(), SIGCONT);
    ctl.join();
    EXPECT_TRUE(done);
    EXPECT_TRUE(asked);
    EXPECT_EQ(answered, 1);
}

// A window that detaches while an answer to a control call and a newer
// layout wait behind a pane's input sends the layout first (release()): the
// next window shows the tabs of this one. The program reads nothing, in raw
// mode, so that its PTY takes no more than its bound, and the daemon holds
// no more than MaxHeldInput: the input before the layout in the queue is
// more than both, and the layout would wait behind it until the window lets
// the session go. The answer, which is no layout, is not what goes first.
TEST(MuxClient, ADetachSendsTheLayoutThatWaitsBeforeTheInputAndTheAnswers)
{
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_GT(daemon.pid(), 0);
    const AfterglowMux::SessionOptions options =
        daemon.options({"/bin/sh", "-c", "stty raw -echo; printf ready; exec /bin/sleep 600"}, "detaching");
    ASSERT_TRUE(daemon.newSession(options)) << daemon.lastError();
    QString error;
    std::shared_ptr<MuxClient> client =
        MuxClient::attach(QStringLiteral("detaching"), false, &error, AfterglowMux::Route{daemon.sockets(), {}, {}}, std::size_t(1) << 20, 1000);
    ASSERT_TRUE(client) << error.toStdString();
    ASSERT_EQ(client->layout().panes().size(), 1u);
    auto pane = std::make_unique<Pane>(AfterglowVt::TerminalOptions{});
    pane->showSession(client, client->layout().panes().front());
    ASSERT_TRUE(waitFor([&] { return pane->text(AfterglowVt::Terminal::TextScope::Screen).contains(QStringLiteral("ready")); }))
        << daemon.errors();
    const std::uint32_t paneId = pane->sessionPaneId();
    bool asked = false;
    bool released = false;
    client->setControlHandler([&](const QString& fn, const QJsonArray&, qint64, MuxClient::ControlReply reply) {
        asked = fn == QLatin1String("setTitle");
        signalDaemon(daemon.pid(), SIGSTOP);
        // As much input as the window keeps for a pane, to a daemon that
        // reads nothing: the answer waits behind it.
        const std::string chunk(std::size_t(64) << 10, 'a');
        std::size_t sent = 0;
        while (sent < 2 * MuxClient::MaxWaitingInput && pane->sendInput(chunk, false)) sent += chunk.size();
        // The premise: the window's queue filled, so the layout waits behind
        // input, and only the rotate of release() sends it first.
        EXPECT_GE(sent + chunk.size(), MuxClient::MaxWaitingInput) << sent;
        reply(MuxClient::ControlAnswer{});
        // After the answer queued: a newer layout, then the detach, which
        // the daemon reads once it runs again.
        QTimer::singleShot(0, client.get(), [&] {
            AfterglowMux::Layout layout = client->layout();
            layout.tabs.front().title = "newer";
            client->sendLayout(layout);
            signalDaemon(daemon.pid(), SIGCONT);
            pane->setSessionRelease(Afterglow::MuxRelease::Nothing);
            pane.reset();
            released = true;
        });
    });
    std::thread ctl([&] {
        AfterglowProto::IpcConnection connection;
        AfterglowProto::IpcConnection::Error error;
        // The window that went answers with an error, or the answer came:
        // either is the end of the call.
        if (connection.connect(daemon.socket(), &error, 2000)
            && connection.call(AfterglowMux::Target, AfterglowMux::Fn::SelectSession,
                               MuxTest::Json::Array{AfterglowMux::ProtocolVersion, "detaching"}, 5000, &error)) {
            connection.call("control", "setTitle", MuxTest::Json::Array{std::int64_t(paneId), "t"}, 30000, &error);
        }
    });
    const bool done = waitFor([&] { return released; }, 40000);
    signalDaemon(daemon.pid(), SIGCONT);
    ctl.join();
    ASSERT_TRUE(done);
    EXPECT_TRUE(asked);
    // The next window gets the layout of the window that went.
    std::shared_ptr<MuxClient> next =
        MuxClient::attach(QStringLiteral("detaching"), true, &error, AfterglowMux::Route{daemon.sockets(), {}, {}}, std::size_t(1) << 20, 1000);
    ASSERT_TRUE(next) << error.toStdString();
    ASSERT_FALSE(next->layout().tabs.empty());
    EXPECT_EQ(next->layout().tabs.front().title, "newer") << daemon.errors();
}

// attachSession() sends Terminal.scrollback as AttachOption::History, at
// most MaxHistory, past which the daemon refuses the attach: the daemon
// sends a window no history row older than that. A window with more
// scrollback than MaxHistory still attaches, and gets the whole history.
TEST(MuxClient, AnAttachAsksForTheWindowsScrollbackUpToMaxHistory)
{
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const AfterglowMux::SessionOptions options = daemon.options({"/bin/sh", "-c", "seq 1 200; exec cat"}, "history");
    ASSERT_TRUE(daemon.newSession(options)) << daemon.lastError();
    struct Attached
    {
        std::shared_ptr<MuxClient> client;
        std::unique_ptr<Pane> pane;
    };
    // Takes the session from the window before.
    const auto attach = [&daemon](std::size_t history) {
        Attached attached;
        QString error;
        attached.client =
            MuxClient::attach(QStringLiteral("history"), true, &error, AfterglowMux::Route{daemon.sockets(), {}, {}}, std::size_t(1) << 20, history);
        EXPECT_TRUE(attached.client) << error.toStdString() << " " << daemon.errors();
        if (!attached.client || attached.client->layout().panes().size() != 1) return attached;
        attached.pane = std::make_unique<Pane>(AfterglowVt::TerminalOptions{});
        attached.pane->showSession(attached.client, attached.client->layout().panes().front());
        Pane& pane = *attached.pane;
        EXPECT_TRUE(waitFor([&pane] {
            return pane.sessionSynced() && pane.text(AfterglowVt::Terminal::TextScope::Screen).contains(QStringLiteral("200"));
        })) << daemon.errors();
        return attached;
    };
    const auto historyRows = [](Pane& pane) {
        return pane.withTerminal([](AfterglowVt::Terminal& t) { return t.primaryScreen().historySize(); });
    };
    const auto replicaText = [](Pane& pane) {
        return pane.withTerminal([](AfterglowVt::Terminal& t) { return t.text(AfterglowVt::Terminal::TextScope::All); });
    };
    // Past MaxHistory: the daemon gets MaxHistory, and sends every row.
    Attached large = attach(std::size_t(AfterglowMux::MaxHistory) + 1000);
    ASSERT_TRUE(large.client && large.pane);
    EXPECT_TRUE(waitFor([&] { return replicaText(*large.pane).starts_with("1\n2\n3\n"); })) << replicaText(*large.pane);
    // All 200 rows are in the daemon's terminal now: a window that keeps 3
    // gets the newest 3, and no older one.
    Attached small = attach(3);
    ASSERT_TRUE(small.client && small.pane);
    EXPECT_EQ(historyRows(*small.pane), 3u) << replicaText(*small.pane);
}

// A read of a pane whose Refresh got no answer in time does not wait again:
// a daemon that stopped would hold the GUI thread for each read. The answer
// that comes later ends the wait, and the next Refresh waits as before.
TEST(MuxClient, AReadAfterALateRefreshDoesNotWaitAgain)
{
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_GT(daemon.pid(), 0);
    Shown shown = show(daemon, "late");
    ASSERT_TRUE(shown.client && shown.first);
    Pane& pane = *shown.first;
    pane.setSessionVisible(false);
    ASSERT_TRUE(signalDaemon(daemon.pid(), SIGSTOP));
    const auto resume = qScopeGuard([&daemon] { signalDaemon(daemon.pid(), SIGCONT); });
    const std::uint64_t ticket = pane.askSessionRefresh();
    ASSERT_NE(ticket, 0u);
    EXPECT_FALSE(shown.client->waitRefreshed({{pane.sessionPaneId(), ticket}}, 300));
    // Late: the next read asks for no other Refresh, and waits for none.
    EXPECT_EQ(pane.askSessionRefresh(), ticket);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(shown.client->waitRefreshed({{pane.sessionPaneId(), ticket}}, 3000));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(1000));
    ASSERT_TRUE(signalDaemon(daemon.pid(), SIGCONT));
    // The late answer comes: the next Refresh is a new one, and its wait
    // ends with its answer.
    std::uint64_t next = 0;
    ASSERT_TRUE(waitFor([&] {
        next = pane.askSessionRefresh();
        return next > ticket;
    })) << daemon.errors();
    EXPECT_TRUE(shown.client->waitRefreshed({{pane.sessionPaneId(), next}}, 5000));
}

// A search in a session pane while its history still comes from the daemon
// says that older lines are still arriving, and the same search once they
// came finds what they hold. A window that attaches gets the screens in the
// first frame, then the history from the newest row back. The daemon sends
// no more of a pane than the channel's window (4 MiB,
// PhosphorChannel::Stream::Options) until the window returns what it read,
// which it does as it reads. The test reads the stream only until the
// screens came, at most one window, and stops the daemon before it reads
// more: no more than two windows can come, whatever the timing, and the
// history holds more. The marker is the oldest row.
TEST(MuxClient, ASearchWaitsForTheHistoryOfASession)
{
    using AfterglowVt::Terminal;
    // About 12.8 MB of rows, in wide rows: fewer rows for the same bytes.
    // Both terminals keep all of them.
    constexpr std::size_t Scrollback = 100000;
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_GT(daemon.pid(), 0);
    AfterglowMux::SessionOptions options = daemon.options(
        {"/bin/sh", "-c",
         "echo late-marker; base64 -w 319 /dev/urandom | head -n 40000; printf 'newest-line\\n\\033]2;done\\007'; exec cat"},
        "history");
    options.size.cols = 320;
    options.terminal.maxScrollback = Scrollback;
    ASSERT_TRUE(daemon.newSession(options)) << daemon.lastError();
    const AfterglowMux::Route route{daemon.sockets(), {}, {}};
    QString error;
    // A window that keeps no history waits for the title, which the program
    // prints last: then the daemon's terminal holds all of the output.
    const auto first = MuxClient::attach(QStringLiteral("history"), false, &error, route, std::size_t(1) << 20, 0);
    ASSERT_TRUE(first) << error.toStdString();
    ASSERT_EQ(first->layout().panes().size(), 1u);
    Pane early(AfterglowVt::TerminalOptions{});
    early.showSession(first, first->layout().panes().front());
    ASSERT_TRUE(waitFor([&] { return early.title() == QStringLiteral("done"); }, 30000)) << daemon.errors();

    // From here on, the event loop does not run until the daemon stopped:
    // only waitForHostCalls() reads the stream.
    const auto shown = MuxClient::attach(QStringLiteral("history"), true, &error, route, std::size_t(1) << 20, Scrollback);
    ASSERT_TRUE(shown) << error.toStdString();
    ASSERT_EQ(shown->layout().panes().size(), 1u);
    AfterglowVt::TerminalOptions keepAll;
    keepAll.maxScrollback = Scrollback;
    Pane pane(keepAll);
    pane.showSession(shown, shown->layout().panes().front());
    const auto replica = [&pane](Terminal::TextScope scope) {
        return pane.withTerminal([scope](Terminal& t) { return t.text(scope, std::size_t(64) << 20); });
    };
    ASSERT_TRUE(shown->waitForHostCalls(10000, [&] { return replica(Terminal::TextScope::Screen).find("newest-line") != std::string::npos; }))
        << daemon.errors();
    bool stopped = signalDaemon(daemon.pid(), SIGSTOP);
    const auto resume = qScopeGuard([&] {
        if (stopped) signalDaemon(daemon.pid(), SIGCONT);
    });
    ASSERT_TRUE(stopped);
    // Stopped for certain before the window returns what it read: the state
    // in /proc is T then.
    ASSERT_TRUE(MuxTest::waitFor([&] {
        const std::string stat = MuxTest::readFile("/proc/" + std::to_string(daemon.pid()) + "/stat");
        const auto state = stat.rfind(") ");
        return state != std::string::npos && state + 2 < stat.size() && stat[state + 2] == 'T';
    }));
    ASSERT_EQ(replica(Terminal::TextScope::All).find("late-marker"), std::string::npos);

    QObject listener;
    QStringList statuses;
    QObject::connect(&pane, &Pane::searchStatusChanged, &listener, [&](const QString& status) { statuses << status; });
    const QString searching = QStringLiteral("Searching…");
    const QString arriving = QStringLiteral("Older lines are still arriving");
    // The window reads what the socket holds meanwhile, which is less than
    // the history. The search covers the rows that the pane had when it
    // started. Its text has a limit (searchText), which the history passes:
    // the notice can follow "Limited results".
    pane.search(QStringLiteral("late-marker"), 0, false);
    ASSERT_TRUE(waitFor([&] { return !statuses.isEmpty() && statuses.last() != searching; }, 15000));
    EXPECT_TRUE(statuses.last().startsWith(QStringLiteral("No matches"))) << statuses.join(QLatin1Char('|')).toStdString();
    EXPECT_TRUE(statuses.last().contains(arriving, Qt::CaseInsensitive)) << statuses.join(QLatin1Char('|')).toStdString();
    // Once all of it came, the same search starts again, and finds it.
    ASSERT_TRUE(signalDaemon(daemon.pid(), SIGCONT));
    stopped = false;
    EXPECT_TRUE(waitFor([&] {
        if (statuses.last() != searching) pane.search(QStringLiteral("late-marker"), 1, false);
        return statuses.last().startsWith(QStringLiteral("1 of 1"));
    }, 30000)) << statuses.join(QLatin1Char('|')).toStdString();
    EXPECT_FALSE(statuses.last().contains(arriving, Qt::CaseInsensitive)) << statuses.last().toStdString();
}

// The PaneOpened of a pane that newPaneLater() asked for can come before
// the answer of the call: the pane waits for the answer, and is the window's
// then, not a pane that the window did not ask for.
TEST(MuxClient, NewPaneLaterKeepsAPaneWhosePaneOpenedCameFirst)
{
    using AfterglowVt::Terminal;
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    Shown shown = show(daemon, "later");
    ASSERT_TRUE(shown.client && shown.first);
    struct Outcome
    {
        bool called = false;
        std::optional<std::uint32_t> pane;
        QString error;
    };
    const auto outcome = std::make_shared<Outcome>();
    const QString refused = shown.client->newPaneLater(launchOf({"/bin/cat"}), Size,
                                                       [outcome](std::optional<std::uint32_t> pane, const QString& error) {
                                                           outcome->called = true;
                                                           outcome->pane = pane;
                                                           outcome->error = error;
                                                       });
    EXPECT_TRUE(refused.isEmpty()) << refused.toStdString();
    EXPECT_FALSE(outcome->called);
    // The daemon starts the pane, answers and sends PaneOpened meanwhile.
    // Neither wait runs the posted answer: the read of the stream takes the
    // PaneOpened first.
    std::uint32_t started = 0;
    ASSERT_TRUE(MuxTest::waitFor([&] {
        for (std::uint32_t id = 1; id <= 64 && !started; ++id) {
            AfterglowProto::IpcConnection::Error error;
            const auto session = daemon.call(AfterglowMux::Fn::PaneSession, MuxTest::Json::Array{std::int64_t(id)}, &error);
            if (session && session->toString() == "later" && id != shown.first->sessionPaneId()) started = id;
        }
        return started != 0;
    })) << daemon.errors();
    ASSERT_TRUE(shown.client->waitForHostCalls(5000, [&] { return shown.client->heard(started).opened; })) << daemon.errors();
    EXPECT_FALSE(outcome->called);
    ASSERT_TRUE(waitFor([&] { return outcome->called; })) << daemon.errors();
    ASSERT_TRUE(outcome->pane) << outcome->error.toStdString();
    EXPECT_EQ(*outcome->pane, started);
    EXPECT_TRUE(outcome->error.isEmpty()) << outcome->error.toStdString();
    // The daemon kept its program: it echoes.
    Pane pane(AfterglowVt::TerminalOptions{});
    pane.showSession(shown.client, *outcome->pane);
    ASSERT_TRUE(waitFor([&] { return pane.sessionSynced(); })) << daemon.errors();
    ASSERT_TRUE(pane.sendInput("echoed\n"));
    EXPECT_TRUE(waitFor([&] { return pane.text(Terminal::TextScope::Screen).contains(QStringLiteral("echoed")); }, 5000));
}

// A pane that newPaneLater() gave and that the window does not place (it
// closed while the daemon started the program): its program ends, and the
// session keeps only the panes that the window shows.
TEST(MuxClient, APaneThatTheWindowDoesNotPlaceEnds)
{
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    Shown shown = show(daemon, "unplaced");
    ASSERT_TRUE(shown.client && shown.first);
    struct Outcome
    {
        bool called = false;
        std::optional<std::uint32_t> pane;
    };
    const auto outcome = std::make_shared<Outcome>();
    const QString refused = shown.client->newPaneLater(launchOf({"/bin/cat"}), Size,
                                                       [outcome, client = std::weak_ptr<MuxClient>(shown.client)](std::optional<std::uint32_t> pane,
                                                                                                                const QString&) {
                                                           outcome->called = true;
                                                           outcome->pane = pane;
                                                           if (const auto shared = client.lock(); shared && pane) shared->endUnbound(*pane);
                                                       });
    EXPECT_TRUE(refused.isEmpty()) << refused.toStdString();
    ASSERT_TRUE(waitFor([&] { return outcome->called; })) << daemon.errors();
    ASSERT_TRUE(outcome->pane) << daemon.errors();
    EXPECT_TRUE(waitFor([&] {
        const auto session = MuxTest::sessionNamed(daemon, "unplaced");
        return session && session->panes == 1;
    })) << daemon.errors();
    EXPECT_TRUE(shown.client->showing());
    // A second end of it, or of the bound first pane, does nothing.
    shown.client->endUnbound(*outcome->pane);
    shown.client->endUnbound(shown.first->sessionPaneId());
    QCoreApplication::processEvents();
    EXPECT_TRUE(shown.client->showing());
    EXPECT_TRUE(shown.first->sendInput("still\n"));
    EXPECT_TRUE(waitFor([&] { return shown.first->text(AfterglowVt::Terminal::TextScope::Screen).contains(QStringLiteral("still")); }, 5000));
}

// A pane that breakOutLater() moves to a new session: the PaneClosed of -1
// that the daemon sends as it moves is no exit of its program, also when
// the window reads it before the answer of the call.
TEST(MuxClient, BreakOutLaterHoldsTheExitOfAPaneThatMoved)
{
    MuxTest::TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    Shown shown = show(daemon, "whole");
    ASSERT_TRUE(shown.client && shown.first);
    std::unique_ptr<Pane> second = newPane(*shown.client, shown.client, {"/bin/cat"});
    ASSERT_TRUE(second);
    ASSERT_TRUE(waitFor([&] { return second->sessionSynced(); })) << daemon.errors();
    int closed = 0;
    int left = 0;
    QObject context; // ends before the counters
    QObject::connect(second.get(), &Pane::closed, &context, [&closed] { ++closed; });
    QObject::connect(second.get(), &Pane::sessionLeft, &context, [&left] { ++left; });
    struct Outcome
    {
        bool called = false;
        std::optional<QString> session;
        QString error;
    };
    const auto outcome = std::make_shared<Outcome>();
    AfterglowMux::Layout layout;
    layout.addTab(second->sessionPaneId());
    const QString refused = shown.client->breakOutLater(layout, [outcome](std::optional<QString> session, const QString& error) {
        outcome->called = true;
        outcome->session = session;
        outcome->error = error;
    });
    EXPECT_TRUE(refused.isEmpty()) << refused.toStdString();
    // The PaneClosed and the end of the pane's channel come before the
    // window runs the posted answer: a read of the stream runs no posted
    // call.
    const std::uint32_t moving = second->sessionPaneId();
    ASSERT_TRUE(shown.client->waitForHostCalls(5000, [&] {
        const MuxClient::Heard heard = shown.client->heard(moving);
        return heard.exited && heard.channelEnded;
    })) << daemon.errors();
    EXPECT_FALSE(outcome->called);
    ASSERT_TRUE(waitFor([&] { return outcome->called; })) << daemon.errors();
    ASSERT_TRUE(outcome->session) << outcome->error.toStdString();
    QCoreApplication::processEvents();
    // The program runs on in the new session: the pane heard of no exit,
    // and of no loss of its session.
    EXPECT_EQ(closed, 0);
    EXPECT_EQ(left, 0);
    const std::string name = outcome->session->toStdString();
    EXPECT_TRUE(waitFor([&] {
        for (const AfterglowMux::SessionInfo& s : daemon.sessions()) {
            if (s.name == name) return s.panes == 1;
        }
        return false;
    })) << name;
}

// A daemon that refuses the attach with no words of its own: the error
// names the call and the code, not nothing.
TEST(MuxClient, ARefusalWithoutWordsNamesTheCall)
{
    std::string dir = "/tmp/afterglow-muxclient-" + std::to_string(::getpid()) + "-XXXXXX";
    ASSERT_TRUE(::mkdtemp(dir.data()));
    const auto removeDir = qScopeGuard([&dir] {
        std::error_code ignored;
        std::filesystem::remove_all(dir, ignored);
    });
    const std::string path = dir + "/socket";
    AfterglowProto::IpcListener listener;
    std::string error;
    ASSERT_EQ(listener.listen(path, &error), AfterglowProto::IpcListener::Listen::Listening) << error;
    // Its end asks the thread to stop, and joins it, before the listener goes.
    std::jthread server([&listener](std::stop_token stop) {
        std::vector<std::unique_ptr<AfterglowProto::IpcServerConnection>> connections;
        while (!stop.stop_requested()) {
            pollfd p{listener.fd(), POLLIN, 0};
            ::poll(&p, 1, 20);
            for (int fd = listener.accept(); fd >= 0; fd = listener.accept()) {
                connections.push_back(std::make_unique<AfterglowProto::IpcServerConnection>(fd));
            }
            for (auto& connection : connections) {
                AfterglowProto::IpcRequest request;
                for (AfterglowProto::IpcServerConnection::Read read;
                     (read = connection->nextRequest(request)) != AfterglowProto::IpcServerConnection::Read::None;) {
                    if (read != AfterglowProto::IpcServerConnection::Read::Request) continue;
                    if (request.fn == AfterglowMux::Fn::Version) {
                        connection->send(AfterglowProto::ipcReply(
                            request.id.value_or(0), MuxTest::Json::Object{{"version", "0"}, {"protocol", AfterglowMux::ProtocolVersion}}));
                    } else {
                        connection->send(AfterglowProto::ipcError(request.id, AfterglowProto::IpcErrorCode::CallFailed, ""));
                    }
                }
                connection->flush();
            }
        }
    });
    AfterglowProto::MuxSockets sockets;
    sockets.candidates.push_back({path, dir});
    QString why;
    EXPECT_FALSE(MuxClient::attach(QStringLiteral("s"), false, &why, AfterglowMux::Route{sockets, {}, {}}, std::size_t(1) << 20, 1000));
    EXPECT_EQ(why, QStringLiteral("the mux daemon refused attach (CALL_FAILED)"));
}

// The settings that a window sends its session: the variables of
// Mux.updateEnvironment as this process has them, set or unset, only for a
// session of this computer, and the Control values.
TEST(MuxSessionSettings, TakesTheUpdatedVariablesOfThisProcess)
{
    const Afterglow::Test::Saved set("AG_TEST_SET", "value");
    const Afterglow::Test::Saved unset("AG_TEST_UNSET");
    qunsetenv("AG_TEST_UNSET");
    Afterglow::Settings settings;
    settings.mux.updateEnvironment = {QStringLiteral("AG_TEST_SET"), QStringLiteral("AG_TEST_UNSET")};
    settings.control.eventInterval = 250;
    settings.control.allowReadText = true;
    const AfterglowMux::SessionSettings local = Afterglow::sessionSettings(settings, true);
    EXPECT_TRUE(local.environment
                == (std::vector<std::pair<std::string, std::optional<std::string>>>{{"AG_TEST_SET", "value"}, {"AG_TEST_UNSET", std::nullopt}}));
    EXPECT_EQ(local.control.eventInterval, 250);
    EXPECT_EQ(local.control.enabled, settings.control.enabled);
    EXPECT_TRUE(local.control.allowReadText);
    // Another host has variables of its own.
    EXPECT_TRUE(Afterglow::sessionSettings(settings, false).environment.empty());
}

// A pane that a session of this computer starts reads the variables that
// it inherits from the session's environment, not from this process, and
// gets the launch's own variables.
TEST(MuxSessions, LocalLaunchReadsTheInheritedEnvironment)
{
    const Afterglow::Test::Saved shell("SHELL", "/no/such/shell-of-this-process");
    Afterglow::Settings settings;
    settings.shell.clear();
    const QString dir = QDir::tempPath();
    // bash, which the shell integration names in argv: a shell that it
    // leaves alone stays out of argv, and the daemon starts the SHELL of the
    // launch's environment.
    if (::access("/bin/bash", X_OK) != 0) GTEST_SKIP() << "no /bin/bash";
    const Afterglow::InheritedEnvironment inherited{{"SHELL", "/bin/bash"}, {"HOME", dir.toStdString()}};
    const AfterglowMux::Launch launch = Afterglow::localLaunch(settings, dir, {}, &inherited);
    for (const std::string& word : launch.argv) EXPECT_NE(word, "/no/such/shell-of-this-process");
    ASSERT_FALSE(launch.argv.empty());
    EXPECT_EQ(launch.argv.front(), "/bin/bash");
    for (const auto& [name, value] : launch.environment) {
        if (name == "SHELL") {
            EXPECT_NE(value, "/no/such/shell-of-this-process");
        }
    }
    const AfterglowMux::Launch withOwn = Afterglow::localLaunch(settings, dir, {}, &inherited, {{QStringLiteral("FOO"), QStringLiteral("bar")}});
    const auto foo = std::find_if(withOwn.environment.begin(), withOwn.environment.end(), [](const auto& kv) { return kv.first == "FOO"; });
    ASSERT_NE(foo, withOwn.environment.end());
    EXPECT_EQ(foo->second, "bar");
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
