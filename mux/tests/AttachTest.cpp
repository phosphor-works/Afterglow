// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// A window attached to a session, played without Qt: the stream of the attach
// call, and a replica terminal that only the daemon's frames change. Each test
// runs its own daemon (TestDaemon.h).

#include "TestDaemon.h"

#include "ImageFiles.h"
#include "MuxHost.h"
#include "MuxLayout.h"
#include "MuxStream.h"

#include <AfterglowProto/Base64.h>
#include <AfterglowProto/Version.h>
#include <AfterglowVt/Graphics.h>
#include <AfterglowVt/StateSync.h>
#include <AfterglowVt/Terminal.h>
#include <PhosphorChannel/Stream.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace MuxTest;

namespace {

struct NullHost : AfterglowVt::ITerminalHost
{
    void writeToPty(std::string_view) override {}
    void regionScrolled(const AfterglowVt::RegionScroll& scroll) override { scrolls.push_back(scroll); }
    std::vector<AfterglowVt::RegionScroll> scrolls; ///< what the replica's frames scrolled, oldest first
};

/// What a window of a session does, without the window.
class Window
{
public:
    /// The attach call and the connection to its socket. Empty on success,
    /// else the error code or message of the call, or why the connection
    /// failed. Without @p eventQueue or @p history, the call passes none.
    std::string attach(TestDaemon& daemon, const std::string& name, bool takeover = true, std::optional<std::size_t> eventQueue = {},
                       std::optional<std::size_t> history = {})
    {
        if (std::string error = call(daemon, name, takeover, eventQueue, history); !error.empty()) return error;
        return connect();
    }

    /// The attach call alone, on a connection of its own, which the stream
    /// takes (connect()). Empty on success, else the error code, or the
    /// message of an error without one.
    std::string call(TestDaemon& daemon, const std::string& name, bool takeover = true, std::optional<std::size_t> eventQueue = {},
                     std::optional<std::size_t> history = {})
    {
        IpcConnection::Error error;
        Json::Object options{{AttachOption::Takeover, takeover}};
        if (eventQueue) options.emplace_back(AttachOption::EventQueue, *eventQueue);
        if (history) options.emplace_back(AttachOption::History, *history);
        connection = std::make_unique<IpcConnection>();
        if (!connection->connect(daemon.socket(), &error, 2000)) return "no connection: " + error.message;
        const auto result = connection->call(Target, Fn::Attach, Json::Array{ProtocolVersion, name, std::move(options)}, 5000, &error);
        if (!result) return error.code.empty() ? error.message : error.code;
        return {};
    }

    /// The window's stream, on the connection of the call.
    std::string connect()
    {
        IpcConnection::Error error;
        const int fd = connection->release(&error);
        if (fd < 0) return "no stream: " + error.message;
        stream = std::make_unique<PhosphorChannel::Stream>(fd);
        std::string why;
        input = stream->openWriter(Attach::ControlChannel, Attach::InputCodec, &why);
        if (input) actions = stream->openWriter(Attach::ActionChannel, Attach::ActionCodec, &why);
        return input && actions ? std::string() : why;
    }

    /// Runs the stream until @p done, up to @p wait.
    bool until(const std::function<bool()>& done, std::chrono::milliseconds wait = 5s)
    {
        const auto deadline = std::chrono::steady_clock::now() + wait;
        while (std::chrono::steady_clock::now() < deadline) {
            if (done()) return true;
            if (!stream->pump(20)) {
                // A stream that ended returns at once: wait as pump would.
                ended = true;
                std::this_thread::sleep_for(20ms);
            }
            read();
        }
        return done();
    }

    /// As a window reads: the frames of a pane before its calls, which the
    /// daemon sends after the frame that they follow. The two channels keep
    /// no order between them, so a call may arrive with a frame before it.
    void read()
    {
        readFrames();
        std::string frame;
        if (auto* events = readEvents ? stream->reader(Attach::ControlChannel) : nullptr) {
            while (events->read(frame) == PhosphorChannel::ReadResult::Frame) {
                const auto event = Attach::decodeEvent(frame);
                if (!event) {
                    ADD_FAILURE() << "an event does not decode";
                    continue;
                }
                this->events.push_back(*event);
                if (event->type == Attach::EventType::Host) {
                    // A call larger than one frame comes in parts.
                    parts += event->payload;
                    if (event->status == Attach::HostPart) continue;
                    const auto call = Host::decode(parts);
                    parts.clear();
                    if (!call) {
                        ADD_FAILURE() << "a host call does not decode";
                        continue;
                    }
                    calls.push_back(*call);
                    // As a window does: the call waits for the frames of its
                    // pane that the daemon wrote before it.
                    callFrames.push_back(std::max(frames, int(event->frames)));
                    callSentAfter.push_back(event->frames);
                    // As a window does: it writes the terminal's reports.
                    if (call->kind == Host::Kind::Report && answerReports && !type(call->text)) ADD_FAILURE() << "a report does not go";
                }
                if (event->type == Attach::EventType::PaneOpened) {
                    // The first event is the layout, which names the pane.
                    if (layouts.empty()) ADD_FAILURE() << "a pane opened before the layout came";
                    opened.push_back(event->pane);
                    // The test reads the frames of the session's first pane.
                    if (!pane) {
                        pane = event->pane;
                        channel = event->channel;
                    }
                }
                if (event->type == Attach::EventType::PaneClosed) closed.push_back(event->pane);
                if (event->type == Attach::EventType::Layout) {
                    const auto json = Json::parse(event->payload);
                    std::string error;
                    auto layout = json ? layoutFrom(*json, &error) : std::nullopt;
                    if (!layout) ADD_FAILURE() << "the layout does not decode: " << error;
                    else layouts.push_back(std::move(*layout));
                }
                if (event->type == Attach::EventType::PaneClosed && channelClosed && !screenAtExit) screenAtExit = screen();
            }
        }
        // The frames of a pane that opened in these events.
        readFrames();
    }

    void readFrames()
    {
        std::string frame;
        if (auto* state = channel && !channelClosed ? stream->reader(channel) : nullptr) {
            while (true) {
                const PhosphorChannel::ReadResult result = state->read(frame);
                if (result == PhosphorChannel::ReadResult::Frame) {
                    ++frames;
                    std::string error;
                    if (!reader.apply(replica, frame, &error)) ADD_FAILURE() << "a frame does not apply: " << error;
                    if (!marker.empty() && !markerFrame && screen().find(marker) != std::string::npos) markerFrame = frames;
                    continue;
                }
                if (result == PhosphorChannel::ReadResult::Closed) {
                    channelClosed = true;
                    if (got(Attach::EventType::PaneClosed) && !screenAtExit) screenAtExit = screen();
                }
                break;
            }
        }
    }

    bool got(Attach::EventType type) const
    {
        return std::any_of(events.begin(), events.end(), [type](const Attach::Event& e) { return e.type == type; });
    }

    /// The visible text of the replica, one line for each row.
    std::string screen() const
    {
        std::string text;
        const AfterglowVt::Screen& s = replica.screen();
        for (int y = 0; y < s.rows(); ++y) {
            for (const AfterglowVt::Cell& c : s.row(y).cells) {
                if (c.width() == 0) continue;
                const char32_t cp = c.codepoint() ? c.codepoint() : U' ';
                if (cp < 0x80) text.push_back(char(cp));
                else text.push_back('?');
            }
            text.push_back('\n');
        }
        return text;
    }

    [[nodiscard]] bool send(const Attach::Input& message)
    {
        return input->tryWrite(Attach::encode(message)) == PhosphorChannel::WriteResult::Written;
    }

    [[nodiscard]] bool type(std::string_view bytes)
    {
        Attach::Input message;
        message.pane = pane;
        message.bytes = bytes;
        return send(message);
    }

    [[nodiscard]] bool resize(int cols, int rows)
    {
        Attach::Input message;
        message.type = Attach::InputType::Resize;
        message.pane = pane;
        message.size = {cols, rows, 8, 16};
        return send(message);
    }

    [[nodiscard]] bool sendSettings(const SessionSettings& settings)
    {
        Attach::Input message;
        message.type = Attach::InputType::Settings;
        message.bytes = Json::write(toJson(settings));
        return send(message);
    }

    [[nodiscard]] bool sendLayout(const Layout& layout)
    {
        Attach::Input message;
        message.type = Attach::InputType::Layout;
        message.bytes = Json::write(toJson(layout));
        return send(message);
    }

    /// The user closed pane @p id, and the window stays.
    [[nodiscard]] bool closePane(std::uint32_t id)
    {
        Attach::Input message;
        message.type = Attach::InputType::ClosePane;
        message.pane = id;
        return actions->tryWrite(Attach::encode(message)) == PhosphorChannel::WriteResult::Written;
    }

    bool closedPane(std::uint32_t id) const { return std::find(closed.begin(), closed.end(), id) != closed.end(); }

    [[nodiscard]] bool action(Attach::InputType type)
    {
        Attach::Input message;
        message.type = type;
        message.pane = pane;
        // A Kill goes on the action channel, the others with the input.
        if (type == Attach::InputType::Kill) return actions->tryWrite(Attach::encode(message)) == PhosphorChannel::WriteResult::Written;
        return send(message);
    }

    std::unique_ptr<IpcConnection> connection; ///< the attach call's, until the stream takes it
    std::unique_ptr<PhosphorChannel::Stream> stream;
    PhosphorChannel::StreamWriter* input = nullptr;
    PhosphorChannel::StreamWriter* actions = nullptr;
    std::uint32_t pane = 0;
    std::uint32_t channel = 0;
    bool channelClosed = false;
    bool ended = false;
    /// What the window showed once it had both halves of the pane's exit.
    std::optional<std::string> screenAtExit;
    int frames = 0;
    std::vector<Attach::Event> events;
    std::vector<Layout> layouts; ///< the Layout events
    std::vector<std::uint32_t> opened; ///< the panes of PaneOpened, in order
    std::vector<std::uint32_t> closed; ///< the panes of PaneClosed, in order
    std::vector<Host::Call> calls; ///< the host calls of the pane's terminal, oldest first
    std::vector<int> callFrames; ///< for each call, the frames that came before it
    std::vector<std::uint64_t> callSentAfter; ///< for each call, the frames that the daemon wrote before it
    std::string marker; ///< text whose first frame markerFrame counts
    int markerFrame = 0;
    std::string parts; ///< the parts of a call that came
    /// Off, the window reads no events: their channel gets no more credit,
    /// and the calls wait in the daemon.
    bool readEvents = true;
    bool answerReports = true;
    NullHost host;
    AfterglowVt::Terminal replica{20, 5, host};
    AfterglowVt::StateReader reader;
};

bool contains(const std::string& text, std::string_view part)
{
    return text.find(part) != std::string::npos;
}

/// A pane that a window asks for in session @p name, with @p argv, at a size
/// of 40 by 10 cells. Its ID, or nothing with @p error.
std::optional<std::uint32_t> newPane(TestDaemon& daemon, const std::string& name, Launch launch, IpcConnection::Error* error = nullptr)
{
    const auto result = daemon.call(Fn::NewPane, Json::Array{ProtocolVersion, name, toJson(launch), toJson(PaneSize{40, 10, 8, 16})}, error);
    if (!result) return std::nullopt;
    const auto id = (*result)["pane"].toInteger();
    if (!id) return std::nullopt;
    return std::uint32_t(*id);
}

Launch launchOf(std::vector<std::string> argv, const TestDaemon& daemon)
{
    Launch launch;
    launch.argv = std::move(argv);
    launch.workingDirectory = daemon.dir();
    return launch;
}

} // namespace

TEST(MuxAttach, AWindowSeesTheScreenAndTypes)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "$ "); })) << w.screen();
    ASSERT_TRUE(w.type("echo hel''lo\n"));
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "\nhello"); })) << w.screen();
    const auto info = sessionNamed(daemon, "sh");
    ASSERT_TRUE(info);
    EXPECT_TRUE(info->attached);
}

TEST(MuxAttach, APaneTakesTheLowestFreeChannel)
{
    // The daemon's "keep" session has pane 1, so this session's pane is 2 or
    // later: its channel is the first one all the same, and stays below the
    // limit of the window's stream however many panes the daemon ran.
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    EXPECT_GT(w.pane, 1u);
    EXPECT_EQ(w.channel, Attach::ControlChannel + 1);
}

TEST(MuxAttach, TheWindowsSizeReachesTheProgram)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh"), "");
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "$ "); })) << w.screen();
    ASSERT_TRUE(w.resize(97, 31));
    ASSERT_TRUE(w.until([&] { return w.replica.cols() == 97 && w.replica.rows() == 31; }));
    ASSERT_TRUE(w.type("stty size\n"));
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "31 97"); })) << w.screen();
    // A size past what a terminal takes is clamped, not refused.
    ASSERT_TRUE(w.resize(100000, 3));
    ASSERT_TRUE(w.until([&] { return w.replica.cols() == AfterglowVt::Screen::MaxColumns && w.replica.rows() == 3; })) << w.replica.cols();
    EXPECT_FALSE(w.ended);
}

TEST(MuxAttach, ResetAndClearActOnTheSessionsTerminal)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh"), "");
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "$ "); }));
    ASSERT_TRUE(w.type("printf '\\033[?2004h'; echo mark\n"));
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "\nmark") && w.replica.modes().bracketedPaste; })) << w.screen();
    // Clear first: a reset clears the screen too. Clear keeps the modes.
    ASSERT_TRUE(w.action(Attach::InputType::Clear));
    ASSERT_TRUE(w.until([&] { return !contains(w.screen(), "mark"); })) << w.screen();
    EXPECT_TRUE(w.replica.modes().bracketedPaste);
    ASSERT_TRUE(w.action(Attach::InputType::Reset));
    ASSERT_TRUE(w.until([&] { return !w.replica.modes().bracketedPaste; }));
}

TEST(MuxAttach, ASecondWindowTakesTheSessionOver)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window first;
    ASSERT_EQ(first.attach(daemon, "sh"), "");
    ASSERT_TRUE(first.until([&] { return contains(first.screen(), "$ "); }));
    ASSERT_TRUE(first.type("echo before\n"));
    ASSERT_TRUE(first.until([&] { return contains(first.screen(), "\nbefore"); }));
    Window refused;
    EXPECT_EQ(refused.attach(daemon, "sh", false), Error::Attached);
    Window second;
    ASSERT_EQ(second.attach(daemon, "sh"), "");
    ASSERT_TRUE(first.until([&] { return first.got(Attach::EventType::TakenOver) && first.ended; }));
    // The second window gets the whole screen, the first one's output too.
    ASSERT_TRUE(second.until([&] { return contains(second.screen(), "\nbefore"); })) << second.screen();
    // The first window gets nothing more. A finished attachment reads no
    // more input, so its kill ends nothing: only the window that shows the
    // session can end it.
    // The daemon ended its output to the first window (first.ended above):
    // the second window's output cannot reach it.
    ASSERT_TRUE(second.type("echo after\n"));
    ASSERT_TRUE(second.until([&] { return contains(second.screen(), "\nafter"); }));
    if (first.action(Attach::InputType::Kill)) (void)first.stream->flush(500);
    ASSERT_TRUE(second.type("echo still\n"));
    ASSERT_TRUE(second.until([&] { return contains(second.screen(), "\nstill"); }));
    EXPECT_TRUE(sessionNamed(daemon, "sh"));
}

TEST(MuxAttach, DetachKeepsTheSessionForTheNextWindow)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    // Made when the job's output is out: while no window shows the session.
    const std::string go = daemon.dir() + "/job-go";
    const std::string done = daemon.dir() + "/job-done";
    {
        Window w;
        ASSERT_EQ(w.attach(daemon, "sh"), "");
        ASSERT_TRUE(w.until([&] { return contains(w.screen(), "$ "); }));
        // Output that comes while no window shows the session: once the
        // window went, the test says when. Bounded by the test's directory,
        // which goes with the daemon.
        ASSERT_TRUE(w.type("echo kept; (while [ -d '" + daemon.dir() + "' ] && [ ! -e '" + go + "' ]; do sleep 0.05; done; echo la''ter; : > '"
                           + done + "') &\n"));
        ASSERT_TRUE(w.until([&] { return contains(w.screen(), "\nkept"); }));
        ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"sh"})) << daemon.lastError();
        ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached); }));
        // The command line has la''ter: only the job's output has "later".
        EXPECT_FALSE(contains(w.screen(), "later")) << w.screen();
    }
    std::ofstream(go).flush();
    IpcConnection::Error error;
    EXPECT_FALSE(daemon.call(Fn::Detach, Json::Array{"sh"}, &error));
    EXPECT_EQ(error.code, Error::NotAttached);
    // The output while no window showed the session is there for the next one.
    ASSERT_TRUE(waitFor([&] {
        const auto info = sessionNamed(daemon, "sh");
        return info && !info->attached;
    }));
    ASSERT_TRUE(waitFor([&] { return exists(done); }));
    Window again;
    ASSERT_EQ(again.attach(daemon, "sh"), "");
    ASSERT_TRUE(again.until([&] { return contains(again.screen(), "\nkept") && contains(again.screen(), "later"); }))
        << again.screen();
}

TEST(MuxAttach, ADetachBeforeTheWindowConnectsReachesIt)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.call(daemon, "sh"), "");
    // The window did not connect yet: no window shows the session.
    const auto info = sessionNamed(daemon, "sh");
    ASSERT_TRUE(info);
    EXPECT_FALSE(info->attached);
    ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"sh"})) << daemon.lastError();
    ASSERT_EQ(w.connect(), "");
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached); }));
}

TEST(MuxAttach, AWindowThatLeavesDetaches)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    {
        Window w;
        ASSERT_EQ(w.attach(daemon, "sh"), "");
        ASSERT_TRUE(w.until([&] { return contains(w.screen(), "$ "); }));
    }
    // Its connection closed without a word, as at a crash: the session stays.
    ASSERT_TRUE(waitFor([&] {
        const auto list = daemon.sessions();
        return list.size() == 2 && !list[0].attached && !list[1].attached;
    }));
}

TEST(MuxAttach, TheWindowSeesTheLastOutputBeforeTheExit)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh"), "");
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "$ "); }));
    // The output and the exit come together: the last frame still goes,
    // before the window learns of the exit.
    ASSERT_TRUE(w.type("echo good''bye; exit 3\n"));
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::SessionEnded) && w.screenAtExit; }));
    EXPECT_TRUE(contains(*w.screenAtExit, "\ngoodbye")) << *w.screenAtExit;
    const auto closed = std::find_if(w.events.begin(), w.events.end(),
                                     [](const Attach::Event& e) { return e.type == Attach::EventType::PaneClosed; });
    ASSERT_NE(closed, w.events.end());
    EXPECT_EQ(closed->pane, w.pane);
    EXPECT_TRUE(WIFEXITED(closed->status) && WEXITSTATUS(closed->status) == 3);
    // SessionEnded is the last event, after the pane's.
    EXPECT_EQ(w.events.back().type, Attach::EventType::SessionEnded);
}

TEST(MuxAttach, AKillEndsTheSessionForTheWindow)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh"), "");
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "$ "); }));
    ASSERT_TRUE(daemon.call(Fn::KillSession, Json::Array{"sh"})) << daemon.lastError();
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::SessionEnded) && w.ended; }));
    EXPECT_FALSE(sessionNamed(daemon, "sh"));
}

TEST(MuxAttach, TheWindowsKillEndsTheSession)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh"), "");
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "$ "); }));
    ASSERT_TRUE(w.action(Attach::InputType::Kill));
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::SessionEnded); }));
    EXPECT_TRUE(waitFor([&] { return !sessionNamed(daemon, "sh"); }));
}

TEST(MuxAttach, ASlowWindowGetsFewerFramesAndMissesNothing)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh"), "");
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "$ "); }));
    // The window reads nothing while the program writes much more than the
    // window of a channel: the daemon's frame waits, and newer output goes
    // in the next one.
    // Wide rows of many colours: far more state than the 4 MiB window of the
    // channel, so a frame has to wait for credit.
    ASSERT_TRUE(w.resize(400, 50));
    // The program says when it wrote it all: the window reads nothing
    // before. The daemon reads the PTY as the program writes.
    const std::string written = daemon.dir() + "/written";
    ASSERT_TRUE(w.type("i=0; while [ $i -lt 30000 ]; do printf '\\033[3%dmline %d %0390d\\033[0m\\n' $((i%8)) $i 0; i=$((i+1)); done; echo E''ND; : > '"
                       + written + "'\n"));
    ASSERT_TRUE(w.stream->flush(2000));
    // Within the test's timeout of 60 s, so that a failure says why.
    ASSERT_TRUE(waitFor([&] { return exists(written); }, 35s));
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "\nEND"); }, 15s)) << w.screen();
    // Far fewer frames than lines, and every one applied (Window::read),
    // up to the last line.
    EXPECT_LT(w.frames, 3000);
    EXPECT_TRUE(contains(w.screen(), "line 29999 ")) << w.screen();
}

TEST(MuxAttach, TheAttachCallAnswersWithItsKeys)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    IpcConnection::Error error;
    const auto result = daemon.call(Fn::Attach, Json::Array{ProtocolVersion, "keep", Json::Object{}}, &error);
    ASSERT_TRUE(result) << error.message;
    ASSERT_TRUE(result->isObject());
    std::vector<std::string> keys;
    for (const auto& [key, value] : result->toObject()) keys.push_back(key);
    std::sort(keys.begin(), keys.end());
    EXPECT_EQ(keys, (std::vector<std::string>{"name"}));
    EXPECT_EQ((*result)["name"].toString(), "keep");
}

TEST(MuxAttach, InputThatAFullPtyRefusesWaitsAndArrivesWhole)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    // A program that reads nothing, until it reads everything.
    const std::string file = daemon.dir() + "/read";
    const std::string go = daemon.dir() + "/go";
    const std::string ready = daemon.dir() + "/ready";
    SessionOptions o = daemon.options(
        {"/bin/sh", "-c", "stty raw -echo; : > '" + ready + "'; while [ ! -e '" + go + "' ]; do sleep 0.1; done; exec cat > '" + file + "'"});
    o.name = "slow";
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    // Raw before the first byte: a line of the terminal holds 4 KiB.
    ASSERT_TRUE(waitFor([&] { return exists(ready); }));
    Window w;
    ASSERT_EQ(w.attach(daemon, "slow"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    // Past the 32 MiB that the PTY queues: the daemon holds the rest, and
    // the window's channel fills behind it.
    const std::string part(60000, 'x');
    std::size_t sent = 0;
    for (int stuck = 0; sent < 48u << 20 && stuck < 200;) {
        if (w.type(part)) {
            sent += part.size();
            stuck = 0;
        } else {
            ++stuck;
            w.stream->pump(20);
            w.read();
        }
    }
    EXPECT_GT(sent, std::size_t(32) << 20) << "the PTY never filled";
    // The window's input waits; the program now reads, and every byte goes.
    std::ofstream(go).put('x');
    std::size_t queued = sent;
    // Below the test's timeout in CTest (60 s), so that a failure says why.
    const auto deadline = std::chrono::steady_clock::now() + 40s;
    while (std::filesystem::exists(file) ? std::filesystem::file_size(file) < queued : true) {
        if (std::chrono::steady_clock::now() > deadline) break;
        if (queued < sent + (8u << 20) && w.type(part)) queued += part.size();
        w.stream->pump(20);
        w.read();
    }
    ASSERT_TRUE(std::filesystem::exists(file));
    EXPECT_EQ(std::filesystem::file_size(file), queued);
}

TEST(MuxAttach, AKillGoesPastInputThatWaitsForAFullPty)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string ready = daemon.dir() + "/ready";
    SessionOptions o = daemon.options({"/bin/sh", "-c", "stty raw -echo; : > '" + ready + "'; exec sleep 60"});
    o.name = "stuck";
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return exists(ready); }));
    Window w;
    ASSERT_EQ(w.attach(daemon, "stuck"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    // Until the window's channel takes no more: the daemon holds input, and
    // reads its control channel no more.
    const std::string part(60000, 'x');
    std::size_t sent = 0;
    // Bounded: a daemon that read on past held input would take it all.
    for (int stuck = 0; stuck < 100 && sent < std::size_t(64) << 20;) {
        if (w.type(part)) {
            sent += part.size();
            stuck = 0;
        } else {
            ++stuck;
            w.stream->pump(20);
            w.read();
        }
    }
    EXPECT_GT(sent, std::size_t(32) << 20);
    // The PTY's 32 MiB, the channel's 4 MiB and the socket's buffers: far
    // below the bound unless the daemon read on.
    EXPECT_LT(sent, std::size_t(64) << 20) << "the daemon read on past held input";
    ASSERT_TRUE(w.action(Attach::InputType::Kill));
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::SessionEnded); }));
    EXPECT_FALSE(sessionNamed(daemon, "stuck"));
}

// The stream starts after the answer: bytes that came with the call would be
// lost between the two readers, and the daemon refuses them.
TEST(MuxAttach, AStreamThatComesWithTheAttachCallIsRefused)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    RawClient raw(daemon.socket());
    ASSERT_TRUE(raw.connected());
    const std::string call = Json::write(Json::Object{{"type", "call"}, {"id", 1}, {"target", Target}, {"fn", Fn::Attach},
                                                      {"args", Json::Array{ProtocolVersion, "sh", Json::Object{}}}});
    ASSERT_TRUE(raw.send(call + "\nstream bytes"));
    const auto reply = Json::parse(raw.line());
    ASSERT_TRUE(reply);
    EXPECT_EQ((*reply)["type"].toString(), "reply") << Json::write(*reply);
    EXPECT_TRUE(waitFor([&] { return daemon.errors().find("sent its stream before the answer") != std::string::npos; })) << daemon.errors();
    // The session is free for the next window.
    Window w;
    EXPECT_EQ(w.attach(daemon, "sh", false), "");
}

TEST(MuxAttach, AWindowThatDoesNotStartItsStreamLetsTheSessionGo)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    // Before the call: the daemon's wait starts before it answers.
    const auto made = std::chrono::steady_clock::now();
    Window silent;
    ASSERT_EQ(silent.call(daemon, "sh", false), "");
    // Until the wait ends, the session is the silent window's.
    Window early;
    EXPECT_EQ(early.call(daemon, "sh", false), Error::Attached);
    const auto freed = [&] {
        const auto deadline = made + std::chrono::milliseconds(Attach::ConnectWaitMs + 5000);
        while (std::chrono::steady_clock::now() < deadline) {
            Window w;
            if (w.call(daemon, "sh", false).empty()) return true;
            std::this_thread::sleep_for(100ms);
        }
        return false;
    };
    ASSERT_TRUE(freed());
    EXPECT_GE(std::chrono::steady_clock::now() - made, std::chrono::milliseconds(Attach::ConnectWaitMs));
}

TEST(MuxAttach, RefusesBadAttachCalls)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    IpcConnection::Error error;
    EXPECT_FALSE(daemon.call(Fn::Attach, Json::Array{ProtocolVersion, "none", Json::Object{}}, &error));
    EXPECT_EQ(error.code, Error::NoSuchSession);
    EXPECT_FALSE(daemon.call(Fn::Attach, Json::Array{ProtocolVersion + 1, "keep", Json::Object{}}, &error));
    EXPECT_EQ(error.code, Error::VersionMismatch);
    EXPECT_FALSE(daemon.call(Fn::Attach, Json::Array{ProtocolVersion, "keep", Json::Object{{AttachOption::Takeover, 1}}}, &error));
    EXPECT_EQ(error.code, IpcErrorCode::InvalidArg);
    EXPECT_FALSE(daemon.call(Fn::Attach, Json::Array{ProtocolVersion, "keep"}, &error));
    EXPECT_EQ(error.code, IpcErrorCode::InvalidArg);
    EXPECT_FALSE(daemon.call(Fn::Detach, Json::Array{"none"}, &error));
    EXPECT_EQ(error.code, Error::NoSuchSession);
    for (const Json::Array& args : {Json::Array{}, Json::Array{1}, Json::Array{"a", "b"}}) {
        SCOPED_TRACE(Json::write(Json::Value(args)));
        EXPECT_FALSE(daemon.call(Fn::Detach, args, &error));
        EXPECT_EQ(error.code, IpcErrorCode::InvalidArg);
    }
}

namespace {

bool hasCall(const Window& w, Host::Kind kind)
{
    return std::any_of(w.calls.begin(), w.calls.end(), [kind](const Host::Call& c) { return c.kind == kind; });
}

/// The texts of the reports that the window got, oldest first.
std::vector<std::string> reports(const Window& w)
{
    std::vector<std::string> out;
    for (const Host::Call& c : w.calls) {
        if (c.kind == Host::Kind::Report) out.push_back(c.text);
    }
    return out;
}

} // namespace

TEST(MuxAttach, TheWindowSaysWhetherThePaneCanBeSeen)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "$ "); })) << w.screen();
    w.answerReports = false;
    // Mode 2033 reports at once: the window said nothing yet, so the pane
    // cannot be seen.
    ASSERT_TRUE(w.type("printf '\\033[?2033h'\n"));
    ASSERT_TRUE(w.until([&] { return reports(w).size() == 1; }));
    EXPECT_EQ(reports(w)[0], "\x1b[?999;2n");
    Attach::Input visible;
    visible.type = Attach::InputType::Visible;
    visible.pane = w.pane;
    visible.visible = true;
    ASSERT_TRUE(w.send(visible));
    ASSERT_TRUE(w.until([&] { return reports(w).size() == 2; }));
    EXPECT_EQ(reports(w)[1], "\x1b[?999;1n");
    // No change, no report.
    ASSERT_TRUE(w.send(visible));
    ASSERT_TRUE(w.type("echo mark\n"));
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "\nmark "); })) << w.screen();
    EXPECT_EQ(reports(w).size(), 2u);
    // A pane that no window shows cannot be seen, and the program hears so
    // from the daemon: a reader in the foreground keeps what comes.
    const std::string kept = daemon.dir() + "/report";
    ASSERT_TRUE(w.type("stty raw -echo; echo armed; head -c 9 > '" + kept + "'; stty sane\n"));
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "\narmed"); })) << w.screen();
    ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"sh"}));
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached); }));
    EXPECT_TRUE(waitFor([&] { return readFile(kept).size() == 9; }));
    EXPECT_TRUE(readFile(kept) == "\x1b[?999;2n") << readFile(kept);
    // The next window that can see it changes that again, and its report is
    // the only one that goes through it.
    Window next;
    next.answerReports = false;
    ASSERT_EQ(next.attach(daemon, "sh"), "");
    ASSERT_TRUE(next.until([&] { return next.pane != 0; }));
    visible.pane = next.pane;
    ASSERT_TRUE(next.send(visible));
    ASSERT_TRUE(next.until([&] { return !reports(next).empty(); }));
    EXPECT_TRUE(reports(next) == std::vector<std::string>{"\x1b[?999;1n"});
    // A window that takes the session over has not said yet whether the user
    // can see the pane: the pane is unseen until it does. The daemon goes
    // through no window between the two, or straight from one to the other,
    // as the takeover meets its loop: either way, the new window's report is
    // a change.
    Window over;
    over.answerReports = false;
    ASSERT_EQ(over.attach(daemon, "sh"), "");
    ASSERT_TRUE(over.until([&] { return over.pane != 0; }));
    visible.pane = over.pane;
    ASSERT_TRUE(over.send(visible));
    ASSERT_TRUE(over.until([&] { return !reports(over).empty() && reports(over).back() == "\x1b[?999;1n"; }));
}

TEST(MuxAttach, ThePaneDirectoryIsThatOfTheForegroundProgram)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "$ "); })) << w.screen();
    const auto directory = [&] {
        const auto result = daemon.call(Fn::PaneDirectory, Json::Array{"sh", std::int64_t(w.pane)});
        return result && result->isString() ? result->toString() : std::string("(no answer)");
    };
    // The shell reports no directory (OSC 7): the system has it.
    ASSERT_TRUE(w.type("cd / && echo in-root\n"));
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "\nin-root "); })) << w.screen();
    EXPECT_EQ(directory(), "/");
    // A directory that was removed is none.
    ASSERT_TRUE(w.type("cd '" + daemon.dir() + "' && mkdir gone && cd gone && rmdir ../gone && echo removed\n"));
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "\nremoved "); })) << w.screen();
    EXPECT_EQ(directory(), "");
    ASSERT_TRUE(w.type("cd / && echo back\n"));
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "\nback "); })) << w.screen();
    // A program in the foreground in another directory.
    ASSERT_TRUE(w.type("(cd /usr && echo in-usr && exec sleep 30)\n"));
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "\nin-usr "); })) << w.screen();
    // The shell puts the program in the foreground after it forked it.
    EXPECT_TRUE(w.until([&] { return directory() == "/usr"; })) << directory();
    IpcConnection::Error error;
    EXPECT_FALSE(daemon.call(Fn::PaneDirectory, Json::Array{"none", 1}, &error));
    EXPECT_EQ(error.code, Error::NoSuchSession);
    EXPECT_FALSE(daemon.call(Fn::PaneDirectory, Json::Array{"sh", std::int64_t(w.pane) + 100}, &error));
    EXPECT_EQ(error.code, Error::NoSuchPane);
    // Pane ids are the daemon's, but a session answers for its own panes only.
    ASSERT_TRUE(shellSession(daemon, "other")) << daemon.lastError();
    EXPECT_FALSE(daemon.call(Fn::PaneDirectory, Json::Array{"other", std::int64_t(w.pane)}, &error));
    EXPECT_EQ(error.code, Error::NoSuchPane);
    for (const Json::Array& args : {Json::Array{}, Json::Array{"sh"}, Json::Array{"sh", "1"}, Json::Array{"sh", -1}, Json::Array{1, 1},
                                    Json::Array{"sh", 1.5}, Json::Array{"sh", std::int64_t(1) << 32}, Json::Array{"sh", 1, 2}}) {
        SCOPED_TRACE(Json::write(Json::Value(args)));
        EXPECT_FALSE(daemon.call(Fn::PaneDirectory, args, &error));
        EXPECT_EQ(error.code, IpcErrorCode::InvalidArg);
    }
}

// The scrolls of a region since the last frame go with the next frame, and
// the replica's host hears of them. Those of a time when no window showed the
// pane stay out of the first frame of the next window, whose replica keeps
// nothing that they move.
TEST(MuxAttach, TheFramesCarryTheRegionScrollsSinceTheLastFrame)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "$ "); })) << w.screen();
    // Rows 2 to 4 scroll up two rows, and the cursor goes below them (a
    // region's end puts it at the top). A scroll of the whole screen is none
    // of a region's, so the prompt and the echo add none.
    ASSERT_TRUE(w.type("printf '\\033[2;4r\\033[4;1H\\n\\n\\033[r\\033[10;1H'; echo scrolled\n"));
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "\nscrolled"); })) << w.screen();
    ASSERT_FALSE(w.host.scrolls.empty());
    int rows = 0;
    for (const AfterglowVt::RegionScroll& scroll : w.host.scrolls) {
        EXPECT_EQ(scroll.bottom - scroll.top, 2u);
        EXPECT_TRUE(scroll.fullWidth);
        rows += std::abs(scroll.delta);
    }
    EXPECT_EQ(rows, 2);

    // The same while no window shows the pane: the program waits on a FIFO
    // until the window went.
    const std::string go = daemon.dir() + "/go";
    const std::string done = daemon.dir() + "/done";
    ASSERT_EQ(::mkfifo(go.c_str(), 0600), 0);
    ASSERT_TRUE(w.type("echo armed; cat '" + go + "' > /dev/null; printf '\\033[2;4r\\033[4;1H\\n\\033[r\\033[12;1H'; "
                       // The daemon's answer to DSR comes once it parsed all before it.
                       "stty raw -echo; printf '\\033[5n'; dd bs=1 count=4 > /dev/null 2>&1; stty sane; touch '" + done + "'; echo fed\n"));
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "\narmed"); })) << w.screen();
    ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"sh"}));
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached); }));
    // The shell opens the FIFO to read once it runs cat: a writer that opens
    // it first gets ENXIO, without a wait.
    int fifo = -1;
    ASSERT_TRUE(waitFor([&] { return (fifo = ::open(go.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC)) >= 0; }));
    ASSERT_EQ(::write(fifo, "go", 2), 2);
    ::close(fifo);
    ASSERT_TRUE(waitFor([&] { return std::filesystem::exists(done); }));
    // The daemon's terminal parsed the scroll before it answered the DSR,
    // which the program read before it touched the file.
    Window next;
    ASSERT_EQ(next.attach(daemon, "sh"), "");
    ASSERT_TRUE(next.until([&] { return next.pane != 0 && contains(next.screen(), "\nfed"); })) << next.screen();
    EXPECT_TRUE(next.host.scrolls.empty());
}

// The calls of a pane's terminal to its host reach the window that shows it,
// in place of the daemon's answers for a pane that no window shows.
TEST(MuxAttach, TheWindowGetsTheCallsOfThePanesTerminal)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "$ "); })) << w.screen();
    // A bell, an OSC 52 write of "hello", and the start of a file transfer.
    ASSERT_TRUE(w.type("printf '\\a\\033]52;c;aGVsbG8=\\a\\033]5113;ac=send;id=t1\\033\\\\'\n"));
    ASSERT_TRUE(w.until([&] { return hasCall(w, Host::Kind::TerminalProtocol); }));
    const auto bell = std::find_if(w.calls.begin(), w.calls.end(), [](const Host::Call& c) { return c.kind == Host::Kind::Bell; });
    const auto write = std::find_if(w.calls.begin(), w.calls.end(), [](const Host::Call& c) { return c.kind == Host::Kind::ClipboardWrite; });
    const auto transfer = std::find_if(w.calls.begin(), w.calls.end(), [](const Host::Call& c) { return c.kind == Host::Kind::TerminalProtocol; });
    ASSERT_NE(bell, w.calls.end());
    ASSERT_NE(write, w.calls.end());
    EXPECT_EQ(write->title, "c");
    // As the terminal hands it to a host: in base64, which the window decodes.
    EXPECT_EQ(write->text, "aGVsbG8=");
    // The window gets the transfer: the test after this one checks that the
    // daemon does not refuse it too.
    EXPECT_EQ(transfer->number, 5113);
    EXPECT_EQ(transfer->text, "ac=send;id=t1");
    // In the order that the program sent them.
    EXPECT_LT(bell, write);
    EXPECT_LT(write, transfer);
}

// A clipboard read waits for the window: the daemon does not answer it, and
// the window's answer reaches the program as its input.
TEST(MuxAttach, AClipboardReadWaitsForTheWindowsAnswer)
{
    if (!pythonFound()) GTEST_SKIP() << "python3 is not installed";
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string answer = daemon.dir() + "/answer";
    ASSERT_TRUE(daemon.newSession(daemon.options(ask("\\x1b]52;c;?\\x07", "\\x07", answer, true), "read"))) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "read"), "");
    // The program asks once the window shows it.
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "go?"); })) << w.screen();
    ASSERT_TRUE(w.type("g"));
    ASSERT_TRUE(w.until([&] { return hasCall(w, Host::Kind::ClipboardRequest); }));
    const auto request = std::find_if(w.calls.begin(), w.calls.end(), [](const Host::Call& c) { return c.kind == Host::Kind::ClipboardRequest; });
    const AfterglowVt::ClipboardRequest asked = request->clipboard;
    EXPECT_TRUE(asked.osc52);
    EXPECT_EQ(asked.kind, AfterglowVt::ClipboardRequest::Kind::Read);
    // Nothing answered it yet.
    EXPECT_FALSE(exists(answer));
    ASSERT_TRUE(w.type(asked.osc52Reply("from the window")));
    ASSERT_TRUE(w.until([&] { return exists(answer); }));
    EXPECT_EQ(readFile(answer), asked.osc52Reply("from the window"));
}

// The daemon's terminal answers a query (DA1) after the commands before it:
// the window writes the answer behind its replies to them.
TEST(MuxAttach, AReportFollowsTheCommandsBeforeIt)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "$ "); })) << w.screen();
    w.answerReports = false;
    // aglow dnd's query, then DA1: the window answers both, in this order.
    ASSERT_TRUE(w.type("printf '\\033]72;t=q:i=3;\\033\\\\\\033[c'\n"));
    ASSERT_TRUE(w.until([&] { return hasCall(w, Host::Kind::Report); }));
    const auto query = std::find_if(w.calls.begin(), w.calls.end(), [](const Host::Call& c) { return c.kind == Host::Kind::TerminalProtocol; });
    const auto report = std::find_if(w.calls.begin(), w.calls.end(), [](const Host::Call& c) { return c.kind == Host::Kind::Report; });
    ASSERT_NE(query, w.calls.end());
    EXPECT_EQ(query->number, 72);
    EXPECT_LT(query, report);
    EXPECT_TRUE(report->text.starts_with("\x1b[?")) << testing::PrintToString(report->text);
}

// With a window, the window answers a file transfer and a drag and drop
// query: the daemon writes no refusal of its own to the program, which gets
// only the answer of its DSR after them.
TEST(MuxAttach, TheDaemonDoesNotAnswerWhatTheWindowAnswers)
{
    if (!pythonFound()) GTEST_SKIP() << "python3 is not installed";
    for (const char* query : {"\\x1b]5113;ac=send;id=t1\\x1b\\\\", "\\x1b]72;t=q:i=3;\\x1b\\\\"}) {
        SCOPED_TRACE(query);
        TestDaemon daemon;
        ASSERT_TRUE(daemon.start()) << daemon.errors();
        const std::string answer = daemon.dir() + "/answer";
        ASSERT_TRUE(daemon.newSession(daemon.options(ask(std::string(query) + "\\x1b[5n", "n", answer, true), "ask"))) << daemon.lastError();
        Window w;
        ASSERT_EQ(w.attach(daemon, "ask"), "");
        ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "go?"); })) << w.screen();
        ASSERT_TRUE(w.type("g"));
        ASSERT_TRUE(w.until([&] { return exists(answer); }));
        EXPECT_EQ(readFile(answer), "\x1b[0n");
    }
}

// A call larger than Mux.eventQueue goes when nothing else waits, in parts
// that the window puts back together. What follows it comes after it.
TEST(MuxAttach, ACallLargerThanTheEventQueueGoesWhenNothingElseWaits)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh", true, std::size_t(MinEventQueue)), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "$ "); })) << w.screen();
    // 1.5 MiB of clipboard text, which is 2 MiB in base64, then a bell.
    ASSERT_TRUE(w.type("head -c 1572864 /dev/zero | tr '\\0' a > big; printf '\\033]52;c;'; base64 -w0 big; printf '\\a\\a'\n"));
    ASSERT_TRUE(w.until([&] { return hasCall(w, Host::Kind::Bell); }));
    const auto write = std::find_if(w.calls.begin(), w.calls.end(), [](const Host::Call& c) { return c.kind == Host::Kind::ClipboardWrite; });
    ASSERT_NE(write, w.calls.end());
    EXPECT_EQ(write->text.size(), std::size_t(2) << 20);
    EXPECT_EQ(write->text.substr(0, 8), "YWFhYWFh");
    EXPECT_GT(std::find_if(w.calls.begin(), w.calls.end(), [](const Host::Call& c) { return c.kind == Host::Kind::Bell; }), write);
}

namespace {

/// A program that writes "go?" once in raw mode and waits for a byte. Then
/// for each of @p sizes, it writes an OSC 52 write of that many bytes and
/// "w" with its index, and waits for a byte: the daemon took the write by
/// the time that the window sees the text after it. Then it writes an OSC
/// 52 read and "ready", and " early" or " waited": whether an answer came
/// within @p earlyMs. A test that expects an answer waits long, which costs
/// nothing when it comes. A test that expects none waits briefly. It saves
/// all that it reads until a second passes without more, in @p file.
std::vector<std::string> fillThenRead(const std::vector<std::size_t>& sizes, const std::string& file, int earlyMs)
{
    std::string list;
    for (const std::size_t size : sizes) list += std::to_string(size) + ",";
    const std::string script = R"(import os, select, sys, tty
fd = sys.stdin.fileno()
tty.setraw(fd)
os.write(1, b'go?')
os.read(fd, 1)
for i, size in enumerate(sys.argv[1].split(',')[:-1]):
    os.write(1, b'\x1b]52;c;' + b'A' * int(size) + b'\x07w%d' % i)
    os.read(fd, 1)
os.write(1, b'\x1b]52;c;?\x07ready')
ready, _, _ = select.select([fd], [], [], int(sys.argv[3]) / 1000)
os.write(1, b' early\r\n' if ready else b' waited\r\n')
data = b''
while True:
    ready, _, _ = select.select([fd], [], [], 1 if data else 30)
    if not ready:
        break
    data += os.read(fd, 4096)
with open(sys.argv[2] + '.part', 'wb') as f:
    f.write(data)
os.rename(sys.argv[2] + '.part', sys.argv[2])
# The session runs on until the test ends: its end would drop what waits.
os.read(fd, 1)
)";
    return {"python3", "-I", "-c", script, list, file, std::to_string(earlyMs)};
}

/// Lets the program of fillThenRead go through its @p writes writes.
::testing::AssertionResult step(Window& w, std::size_t writes)
{
    for (std::size_t i = 0; i <= writes; ++i) {
        const std::string mark = i == 0 ? "go?" : "w" + std::to_string(i - 1);
        if (!w.until([&] { return contains(w.screen(), mark); })) return ::testing::AssertionFailure() << "no " << mark << ": " << w.screen();
        if (!w.type("g")) return ::testing::AssertionFailure() << "no input";
    }
    return ::testing::AssertionSuccess();
}

/// The answer of a session that no window shows to the read of fillThenRead.
std::string readRefused()
{
    AfterglowVt::ClipboardRequest read;
    read.osc52 = true;
    read.osc52Bel = true;
    read.osc52Selection = "c";
    return read.refusal("EPERM");
}

} // namespace

// A request that does not fit the queue, while other calls wait, gets the
// answer of a session that no window shows, at once and once.
TEST(MuxAttach, ARequestThatDoesNotFitGetsTheAnswerWithoutAWindow)
{
    if (!pythonFound()) GTEST_SKIP() << "python3 is not installed";
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string answer = daemon.dir() + "/answer";
    // Two writes take the credit of the channel and fill the daemon's queue,
    // and the third waits in the pane: the read finds no room there.
    const std::size_t mib = std::size_t(1) << 20;
    ASSERT_TRUE(daemon.newSession(daemon.options(fillThenRead({2 * mib, 5 * mib / 2, 2 * mib}, answer, 5000), "full"))) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "full", true, mib), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    w.readEvents = false;
    ASSERT_TRUE(step(w, 3));
    // The window has not read a call yet: the daemon answered at once.
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "ready early"); })) << w.screen();
    ASSERT_TRUE(w.until([&] { return exists(answer); }));
    EXPECT_EQ(readFile(answer), readRefused());
    // The writes still come, and the read does not.
    w.readEvents = true;
    ASSERT_TRUE(w.until([&] {
        return std::count_if(w.calls.begin(), w.calls.end(), [](const Host::Call& c) { return c.kind == Host::Kind::ClipboardWrite; }) == 3;
    }));
    EXPECT_FALSE(hasCall(w, Host::Kind::ClipboardRequest));
}

// A request that waits for a window that detaches, in the daemon's queue or
// in the pane's, gets the answer of a session that no window shows, once.
TEST(MuxAttach, RequestsThatWaitWhenTheWindowDetachesGetTheAnswerWithoutAWindow)
{
    if (!pythonFound()) GTEST_SKIP() << "python3 is not installed";
    const std::size_t mib = std::size_t(1) << 20;
    // Each size alone: a failure of the first does not skip the second.
    const auto detach = [mib](std::size_t queue) {
        TestDaemon daemon;
        ASSERT_TRUE(daemon.start()) << daemon.errors();
        const std::string answer = daemon.dir() + "/answer";
        ASSERT_TRUE(daemon.newSession(daemon.options(fillThenRead({2 * mib, 5 * mib / 2}, answer, 500), "detach"))) << daemon.lastError();
        Window w;
        ASSERT_EQ(w.attach(daemon, "detach", true, queue), "");
        ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
        w.readEvents = false;
        ASSERT_TRUE(step(w, 2));
        // The read waits: the window got no answer to give, and the daemon
        // gave none. The program says "ready waited" when no answer came
        // in its time (fillThenRead).
        ASSERT_TRUE(w.until([&] { return contains(w.screen(), "ready waited"); })) << w.screen();
        IpcConnection::Error error;
        ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"detach"}, &error)) << error.message;
        ASSERT_TRUE(w.until([&] { return exists(answer); }));
        EXPECT_EQ(readFile(answer), readRefused());
    };
    // With 1 MiB, the read waits in the pane; with 4 MiB, behind the second
    // write in the daemon's queue.
    for (const std::size_t queue : {mib, 4 * mib}) {
        SCOPED_TRACE(queue);
        detach(queue);
    }
}

// A call whose first part went goes whole, also when the session ends
// before its other parts went: the window takes no half call.
TEST(MuxAttach, ACallWhoseFirstPartWentGoesWholeWhenTheSessionEnds)
{
    if (!pythonFound()) GTEST_SKIP() << "python3 is not installed";
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::size_t mib = std::size_t(1) << 20;
    // Two writes, which take more than the credit of the channel, and the
    // program ends. The second comes once the daemon took the first.
    const std::string script = R"(import os, sys, tty
fd = sys.stdin.fileno()
tty.setraw(fd)
os.write(1, b'go?')
os.read(fd, 1)
os.write(1, b'\x1b]52;c;' + b'A' * (2 << 20) + b'\x07w0')
os.read(fd, 1)
os.write(1, b'\x1b]52;c;' + b'A' * (5 << 19) + b'\x07w1')
os.read(fd, 1)
)";
    ASSERT_TRUE(daemon.newSession(daemon.options({"python3", "-I", "-c", script}, "ends"))) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "ends", true, mib), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    w.readEvents = false;
    // The daemon took both writes off the pane once w1 shows: the window's
    // credit holds the second one's parts back when the program ends.
    ASSERT_TRUE(step(w, 2));
    // The pane's last frame: the program ended.
    ASSERT_TRUE(w.until([&] { return w.channelClosed; }));
    w.readEvents = true;
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::SessionEnded); }));
    std::vector<std::size_t> sizes;
    for (const Host::Call& c : w.calls) {
        if (c.kind == Host::Kind::ClipboardWrite) sizes.push_back(c.text.size());
    }
    EXPECT_EQ(sizes, (std::vector<std::size_t>{2 * mib, 5 * mib / 2}));
}

// The calls of a pane whose program exited all go before its PaneClosed,
// also those that wait in the daemon's queue for the window's credit when
// the session ends with it.
TEST(MuxAttach, TheCallsThatWaitWhenTheLastProgramExitsStillGo)
{
    if (!pythonFound()) GTEST_SKIP() << "python3 is not installed";
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::size_t mib = std::size_t(1) << 20;
    // Three writes, more than the credit of the channel, which a queue of
    // 8 MiB takes whole, and the program ends.
    const std::string script = R"(import os, sys, tty
fd = sys.stdin.fileno()
tty.setraw(fd)
os.write(1, b'go?')
os.read(fd, 1)
os.write(1, b'\x1b]52;c;' + b'A' * (2 << 20) + b'\x07w0')
os.read(fd, 1)
os.write(1, b'\x1b]52;c;' + b'A' * (5 << 19) + b'\x07w1')
os.read(fd, 1)
os.write(1, b'\x1b]52;c;' + b'A' * (1 << 20) + b'\x07w2')
os.read(fd, 1)
)";
    ASSERT_TRUE(daemon.newSession(daemon.options({"python3", "-I", "-c", script}, "ends"))) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "ends", true, 8 * mib), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    w.readEvents = false;
    ASSERT_TRUE(step(w, 3));
    ASSERT_TRUE(w.until([&] { return w.channelClosed; }));
    w.readEvents = true;
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::SessionEnded); }));
    std::vector<std::size_t> sizes;
    for (const Host::Call& c : w.calls) {
        if (c.kind == Host::Kind::ClipboardWrite) sizes.push_back(c.text.size());
    }
    EXPECT_EQ(sizes, (std::vector<std::size_t>{2 * mib, 5 * mib / 2, mib}));
    // Before the pane's exit.
    std::size_t lastCall = 0;
    std::size_t closed = w.events.size();
    for (std::size_t i = 0; i < w.events.size(); ++i) {
        if (w.events[i].type == Attach::EventType::Host) lastCall = i;
        if (w.events[i].type == Attach::EventType::PaneClosed && closed == w.events.size()) closed = i;
    }
    EXPECT_TRUE(w.got(Attach::EventType::PaneClosed));
    EXPECT_LT(lastCall, closed);
}

TEST(MuxAttach, RefusesAnEventQueueOrHistoryOutOfRangeAndAnUnknownOption)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::vector<Json::Value> refused = {Json::Value(MinEventQueue - 1), Json::Value(MaxEventQueue + 1), Json::Value("4194304"),
                                              Json::Value(1048576.5), Json::Value(true)};
    for (std::size_t i = 0; i < refused.size(); ++i) {
        IpcConnection::Error error;
        EXPECT_FALSE(daemon.call(Fn::Attach, Json::Array{ProtocolVersion, "keep", Json::Object{{AttachOption::EventQueue, refused[i]}}}, &error));
        EXPECT_EQ(error.code, IpcErrorCode::InvalidArg) << i;
    }
    for (const Json::Value& rows : {Json::Value(-1), Json::Value(MaxHistory + 1), Json::Value("10"), Json::Value(1.5)}) {
        SCOPED_TRACE(Json::write(rows));
        IpcConnection::Error error;
        EXPECT_FALSE(daemon.call(Fn::Attach, Json::Array{ProtocolVersion, "keep", Json::Object{{AttachOption::History, rows}}}, &error));
        EXPECT_EQ(error.code, IpcErrorCode::InvalidArg);
    }
    IpcConnection::Error error;
    EXPECT_FALSE(daemon.call(Fn::Attach, Json::Array{ProtocolVersion, "keep", Json::Object{{"eventQueues", MinEventQueue}}}, &error));
    EXPECT_EQ(error.code, IpcErrorCode::InvalidArg);
    // The bounds themselves are values of the settings.
    for (const std::int64_t bytes : {MinEventQueue, MaxEventQueue}) {
        Window w;
        EXPECT_EQ(w.attach(daemon, "keep", true, std::size_t(bytes)), "") << bytes;
    }
    for (const std::int64_t rows : {std::int64_t(0), MaxHistory}) {
        Window w;
        EXPECT_EQ(w.attach(daemon, "keep", true, {}, std::size_t(rows)), "") << rows;
    }
}

// A window that keeps no history gets none: the frames carry the screen.
TEST(MuxAttach, AWindowGetsNoMoreHistoryThanItKeeps)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh", true, {}, 0), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "$ "); })) << w.screen();
    ASSERT_TRUE(w.type("seq 200; echo E''ND\n"));
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "\nEND"); })) << w.screen();
    EXPECT_EQ(w.replica.primaryScreen().historySize(), 0u);
    // Another window, with the default: the history comes.
    Window all;
    ASSERT_EQ(all.attach(daemon, "sh"), "");
    ASSERT_TRUE(all.until([&] { return all.pane != 0 && contains(all.screen(), "\nEND") && all.replica.primaryScreen().historySize() > 100; }))
        << all.replica.primaryScreen().historySize();
}

// The daemon decodes a PNG (kitty f=100) and an iTerm2 image in a process of
// its own, and the window's replica gets their pixels.
TEST(MuxAttach, ThePngAndItermImagesOfASessionReachTheWindow)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "$ "); })) << w.screen();
    // The size of a cell, which a window measures: an iTerm2 image is sized
    // in cells, and a session that no window measured refuses it.
    ASSERT_TRUE(w.resize(40, 10));
    ASSERT_TRUE(w.until([&] { return w.replica.cols() == 40 && w.replica.cellWidth() == 8; }));
    const std::string images = daemon.dir() + "/images";
    {
        std::ofstream out(images, std::ios::binary);
        out << "\x1b_Ga=T,f=100,i=7,q=2;" << AfterglowProto::base64Encode(AfterglowImage::TestFiles::Png) << "\x1b\\";
        // kitty's f=100 is PNG only: a JPEG with it stores nothing.
        out << "\x1b_Ga=T,f=100,i=8,q=2;" << AfterglowProto::base64Encode(AfterglowImage::TestFiles::Jpeg) << "\x1b\\";
        out << "\x1b]1337;File=inline=1;size=" << AfterglowImage::TestFiles::Jpeg.size() << ':'
            << AfterglowProto::base64Encode(AfterglowImage::TestFiles::Jpeg) << '\a';
        out << "\x1b]1337;File=inline=1;size=" << AfterglowImage::TestFiles::Webp.size() << ':'
            << AfterglowProto::base64Encode(AfterglowImage::TestFiles::Webp) << '\a';
    }
    ASSERT_TRUE(w.type("cat '" + images + "'; echo im''ages-shown\n"));
    const AfterglowVt::GraphicsStore& store = w.replica.graphics();
    // The iTerm2 images, in their order: the JPEG (8 by 8), then the WebP.
    const auto iterm = [&] {
        std::vector<const AfterglowVt::Image*> found;
        for (const auto& [id, image] : store.images())
            if (image.isSixel) found.push_back(&image);
        std::sort(found.begin(), found.end(), [](const AfterglowVt::Image* a, const AfterglowVt::Image* b) {
            return a->frames.at(0)->width > b->frames.at(0)->width;
        });
        return found;
    };
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "images-shown") && store.images().contains(7) && iterm().size() == 2; }))
        << w.screen() << store.images().size();
    EXPECT_FALSE(store.images().contains(8));
    const AfterglowVt::Image& png = store.images().at(7);
    ASSERT_EQ(png.frames.size(), 1u);
    EXPECT_EQ(png.frames[0]->width, 3u);
    EXPECT_EQ(png.frames[0]->height, 2u);
    EXPECT_TRUE(png.frames[0]->rgba == AfterglowImage::TestFiles::PngPixels);
    const AfterglowVt::Image& jpeg = *iterm()[0];
    ASSERT_EQ(jpeg.frames.size(), 1u);
    EXPECT_EQ(jpeg.frames[0]->width, 8u);
    EXPECT_EQ(jpeg.frames[0]->height, 8u);
    // A lossy format: each pixel near red.
    int notRed = 0;
    const std::vector<std::uint8_t>& rgba = jpeg.frames[0]->rgba;
    ASSERT_EQ(rgba.size(), 8u * 8 * 4);
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        if (rgba[i] <= 240 || rgba[i + 1] >= 16 || rgba[i + 2] >= 16 || rgba[i + 3] != 255) ++notRed;
    }
    EXPECT_EQ(notRed, 0);
    const AfterglowVt::Image& webp = *iterm()[1];
    ASSERT_EQ(webp.frames.size(), 1u);
    EXPECT_EQ(webp.frames[0]->width, 2u);
    EXPECT_EQ(webp.frames[0]->height, 2u);
    EXPECT_TRUE(webp.frames[0]->rgba == AfterglowImage::TestFiles::repeat({255, 0, 0, 255}, 4));
}

// A frame that waits for a decode holds the host calls behind it: the bell
// that the program rang after its text comes after the frame with the text.
TEST(MuxAttach, AFrameThatWaitsForADecodeHoldsTheCallsBehindIt)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "$ "); })) << w.screen();
    ASSERT_TRUE(w.resize(40, 10));
    ASSERT_TRUE(w.until([&] { return w.replica.cellWidth() == 8; }));
    // Text, a bell and an image that takes a while to decode, in one write
    // that the pane reads at once: the image comes as a file (t=f), so the
    // decode starts in the feed that has the bell. The image leaves the
    // cursor where it is (C=1): its rows would scroll the text off the screen.
    const std::string png = daemon.dir() + "/slow.png";
    std::ofstream(png, std::ios::binary) << greyPng(8000, 8000);
    const std::string file = daemon.dir() + "/slow";
    {
        std::ofstream out(file, std::ios::binary);
        out << "\x1b[2J\x1b[Hrang\a\x1b_Ga=T,f=100,t=f,C=1,i=5,q=2;" << AfterglowProto::base64Encode(png) << "\x1b\\";
    }
    w.marker = "rang";
    ASSERT_TRUE(w.type("cat '" + file + "'\n"));
    ASSERT_TRUE(w.until([&] { return hasCall(w, Host::Kind::Bell) && w.markerFrame; }, 30s))
        << "bell " << hasCall(w, Host::Kind::Bell) << ", frame of the text " << w.markerFrame << ", screen " << w.screen();
    const std::size_t bell = std::size_t(std::find_if(w.calls.begin(), w.calls.end(), [](const Host::Call& c) { return c.kind == Host::Kind::Bell; })
                                         - w.calls.begin());
    EXPECT_GE(w.callFrames[bell], w.markerFrame);
}

// A call names the frames of its pane that the daemon wrote before it: a
// frame larger than a fragment of the stream (16 KiB) can still be on its
// way when the call comes, and the window waits for it.
TEST(MuxAttach, ACallNamesTheFramesBeforeIt)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "$ "); })) << w.screen();
    ASSERT_TRUE(w.resize(300, 100));
    ASSERT_TRUE(w.until([&] { return w.replica.cols() == 300 && w.replica.rows() == 100; }));
    // A screen of colored text, its last word and a bell in one write: the
    // frame with them holds far more than a fragment.
    const std::string file = daemon.dir() + "/screen";
    {
        std::ofstream out(file, std::ios::binary);
        out << "\x1b[2J\x1b[H";
        for (int row = 0; row < 98; ++row) {
            for (int col = 0; col < 299; ++col) out << "\x1b[3" << (col % 8) << ";4" << (row % 8) << "m" << char('a' + col % 26);
            out << "\r\n";
        }
        out << "\x1b[0mfinal\a";
    }
    w.marker = "final";
    ASSERT_TRUE(w.type("cat '" + file + "'\n"));
    ASSERT_TRUE(w.until([&] { return hasCall(w, Host::Kind::Bell) && w.markerFrame; }, 30s)) << w.screen();
    const std::size_t bell = std::size_t(std::find_if(w.calls.begin(), w.calls.end(), [](const Host::Call& c) { return c.kind == Host::Kind::Bell; })
                                         - w.calls.begin());
    EXPECT_GE(w.callSentAfter[bell], std::uint64_t(w.markerFrame));
}

// An image that the program prints while no window shows the session, after
// a window measured its cells: the daemon keeps it, and the next window gets
// it with its first frames.
TEST(MuxAttach, AnImagePrintedWhileDetachedReachesTheNextWindow)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    const std::string image = daemon.dir() + "/image";
    {
        std::ofstream out(image, std::ios::binary);
        out << "\x1b_Ga=T,f=100,i=9,q=2;" << AfterglowProto::base64Encode(AfterglowImage::TestFiles::Png) << "\x1b\\";
    }
    const std::string go = daemon.dir() + "/image-go";
    const std::string done = daemon.dir() + "/image-done";
    {
        Window w;
        ASSERT_EQ(w.attach(daemon, "sh"), "");
        ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "$ "); })) << w.screen();
        ASSERT_TRUE(w.resize(40, 10));
        ASSERT_TRUE(w.until([&] { return w.replica.cellWidth() == 8; }));
        // Printed once the window went: the test says when.
        // Bounded by the test's directory, which goes with the daemon.
        ASSERT_TRUE(w.type("(while [ -d '" + daemon.dir() + "' ] && [ ! -e '" + go + "' ]; do sleep 0.05; done; cat '" + image
                           + "'; : > '" + done + "') &\n"));
        ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"sh"})) << daemon.lastError();
        ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached); }));
        EXPECT_FALSE(w.replica.graphics().images().contains(9));
    }
    std::ofstream(go).flush();
    ASSERT_TRUE(waitFor([&] { return exists(done); }));
    Window again;
    ASSERT_EQ(again.attach(daemon, "sh"), "");
    const AfterglowVt::GraphicsStore& store = again.replica.graphics();
    ASSERT_TRUE(again.until([&] { return store.images().contains(9) && !store.placements().empty(); })) << store.images().size();
    const AfterglowVt::Image& png = store.images().at(9);
    ASSERT_EQ(png.frames.size(), 1u);
    EXPECT_TRUE(png.frames[0]->rgba == AfterglowImage::TestFiles::PngPixels);
}

// The session's tabs and splits come first, so that the window lays them out
// before it makes their panes.
TEST(MuxAttach, TheFirstEventIsTheSessionsLayout)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    ASSERT_FALSE(w.events.empty());
    EXPECT_EQ(w.events.front().type, Attach::EventType::Layout);
    ASSERT_EQ(w.layouts.size(), 1u);
    EXPECT_EQ(w.layouts[0].panes(), std::vector<std::uint32_t>{w.pane});
    EXPECT_EQ(w.layouts[0].tabs[0].focused, w.pane);
}

// A new tab or split of the window: the pane runs under the daemon, with the
// session's environment and the changes of its own launch.
TEST(MuxAttach, ANewPaneHasTheSessionsEnvironmentAndItsOwnChanges)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    SessionOptions o = daemon.options(Waiting, "s");
    o.environment.emplace_back("FROM_SESSION", "1");
    o.environment.emplace_back("TMUX", "/tmp/tmux-1/default,1,0");
    o.launch.environment.emplace_back("FIRST_PANE_ONLY", "1");
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));

    const std::string out = daemon.dir() + "/env";
    Launch launch = launchOf(writesEnvironment(out), daemon);
    launch.environment.emplace_back("FROM_PANE", "2");
    launch.unset.push_back("TMUX");
    launch.launchedProgram = "sleeper";
    const auto id = newPane(daemon, "s", launch);
    ASSERT_TRUE(id) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return !readFile(out).empty(); }));
    const std::string env = "\n" + readFile(out);
    EXPECT_NE(env.find("\nFROM_SESSION=1\n"), std::string::npos) << env;
    EXPECT_NE(env.find("\nFROM_PANE=2\n"), std::string::npos) << env;
    EXPECT_EQ(env.find("\nTMUX="), std::string::npos) << env;
    EXPECT_EQ(env.find("\nFIRST_PANE_ONLY="), std::string::npos) << env;
    EXPECT_NE(env.find("\nAFTERGLOW_SESSION=s\n"), std::string::npos) << env;
    EXPECT_NE(env.find("\nAFTERGLOW_PANE_ID=" + std::to_string(*id) + "\n"), std::string::npos) << env;
    // The window that shows the session gets its state.
    ASSERT_TRUE(w.until([&] { return std::count(w.opened.begin(), w.opened.end(), *id) == 1; }));
    const auto info = sessionNamed(daemon, "s");
    ASSERT_TRUE(info);
    EXPECT_EQ(info->panes, 2);
    // Its job, for the question before the window closes it: the program of
    // its launch, which runs in the foreground.
    const auto job = daemon.call(Fn::PaneJob, Json::Array{"s", std::int64_t(*id)});
    ASSERT_TRUE(job) << daemon.lastError();
    EXPECT_EQ(job->toString(), "sleeper");
}

TEST(MuxAttach, NewPaneAndPaneJobRefuseBadCalls)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    const Json::Value launch = toJson(launchOf(Waiting, daemon));
    const Json::Value size = toJson(PaneSize{});
    const std::pair<Json::Array, std::string> bad[] = {
        {Json::Array{ProtocolVersion, "none", launch, size}, Error::NoSuchSession},
        {Json::Array{ProtocolVersion - 1, "s", launch, size}, Error::VersionMismatch},
        {Json::Array{ProtocolVersion, "s", launch}, IpcErrorCode::InvalidArg},
        {Json::Array{ProtocolVersion, "s", Json::Object{}, size}, IpcErrorCode::InvalidArg},
        {Json::Array{ProtocolVersion, "s", launch, Json::Object{{"cols", 1}}}, IpcErrorCode::InvalidArg},
        {Json::Array{ProtocolVersion, "s", toJson(launchOf({"/nonexistent/program"}, daemon)), size}, Error::SpawnFailed},
    };
    for (const auto& [args, code] : bad) {
        IpcConnection::Error error;
        EXPECT_FALSE(daemon.call(Fn::NewPane, args, &error)) << Json::write(args);
        EXPECT_EQ(error.code, code) << Json::write(args) << ": " << error.message;
    }
    // The session keeps its one pane.
    EXPECT_EQ(sessionNamed(daemon, "s")->panes, 1);
    IpcConnection::Error error;
    EXPECT_FALSE(daemon.call(Fn::PaneJob, Json::Array{"s", 999}, &error));
    EXPECT_EQ(error.code, Error::NoSuchPane);
    EXPECT_FALSE(daemon.call(Fn::PaneJob, Json::Array{"none", 1}, &error));
    EXPECT_EQ(error.code, Error::NoSuchSession);
}

// The tabs and splits that a window sends are those of the next window.
TEST(MuxAttach, TheNextWindowGetsTheLayoutThatTheWindowSent)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    Layout sent;
    {
        Window w;
        ASSERT_EQ(w.attach(daemon, "s"), "");
        ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
        const auto second = newPane(daemon, "s", launchOf(Waiting, daemon));
        const auto third = newPane(daemon, "s", launchOf(Waiting, daemon));
        ASSERT_TRUE(second && third) << daemon.lastError();
        // A split of the first pane, and a tab with a title, which shows.
        sent.tabs.push_back(LayoutTab{{LayoutNode{0, true, 0.25}, LayoutNode{w.pane}, LayoutNode{*second}}, *second, *second, ""});
        sent.tabs.push_back(LayoutTab{{LayoutNode{*third}}, *third, 0, "logs"});
        sent.current = 1;
        ASSERT_TRUE(w.sendLayout(sent));
        ASSERT_TRUE(w.until([&] { return w.opened.size() == 3; }));
        ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"s"})) << daemon.lastError();
        ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached); }));
    }
    Window again;
    ASSERT_EQ(again.attach(daemon, "s"), "");
    ASSERT_TRUE(again.until([&] { return again.opened.size() == 3; }));
    ASSERT_EQ(again.layouts.size(), 1u);
    EXPECT_TRUE(again.layouts[0] == sent) << Json::write(toJson(again.layouts[0]));
}

// A layout that a pane's exit passed, or that came before a new pane: the
// daemon keeps the panes of the session, each once.
TEST(MuxAttach, TheDaemonKeepsTheSessionsPanesInTheLayout)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    const auto second = newPane(daemon, "s", launchOf(Waiting, daemon));
    ASSERT_TRUE(second) << daemon.lastError();
    // It names a pane that the session does not have, and not the new one.
    Layout stale;
    stale.tabs.push_back(LayoutTab{{LayoutNode{0, false, 0.5}, LayoutNode{w.pane}, LayoutNode{9999}}, 9999, 0, "old"});
    ASSERT_TRUE(w.sendLayout(stale));
    ASSERT_TRUE(w.until([&] { return w.opened.size() == 2; }));
    ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"s"})) << daemon.lastError();
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached); }));
    Window again;
    ASSERT_EQ(again.attach(daemon, "s"), "");
    ASSERT_TRUE(again.until([&] { return again.opened.size() == 2; }));
    ASSERT_EQ(again.layouts.size(), 1u);
    const Layout& kept = again.layouts[0];
    ASSERT_EQ(kept.tabs.size(), 2u);
    EXPECT_EQ(kept.tabs[0].tree, std::vector<LayoutNode>{LayoutNode{w.pane}});
    EXPECT_EQ(kept.tabs[0].focused, w.pane);
    EXPECT_EQ(kept.tabs[0].title, "old");
    EXPECT_EQ(kept.tabs[1].tree, std::vector<LayoutNode>{LayoutNode{*second}});
}

TEST(MuxAttach, AWindowThatSendsALayoutThatIsNotValidLosesTheSession)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    Attach::Input message;
    message.type = Attach::InputType::Layout;
    message.bytes = R"({"tabs":[{"tree":[1,1],"focused":1,"zoomed":0,"title":""}],"current":0})";
    ASSERT_TRUE(w.send(message));
    // The stream ends; the session keeps running for the next window.
    ASSERT_TRUE(w.until([&] { return w.ended; }));
    ASSERT_TRUE(waitFor([&] { return !sessionNamed(daemon, "s")->attached; }));
    EXPECT_NE(daemon.errors().find("a layout that is not valid"), std::string::npos) << daemon.errors();
}

// The user closes a pane, and the window stays: that program ends, the others
// run on.
TEST(MuxAttach, ClosingAPaneEndsOnlyItsProgram)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    const std::string pidFile = daemon.dir() + "/pid";
    const auto second = newPane(daemon, "s", launchOf({"/bin/sh", "-c", "echo $$ > '" + pidFile + "'; exec sleep 60"}, daemon));
    ASSERT_TRUE(second) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return readFile(pidFile).find('\n') != std::string::npos; }));
    const pid_t program = pid_t(std::strtol(readFile(pidFile).c_str(), nullptr, 10));
    ASSERT_GT(program, 0);
    ASSERT_TRUE(w.until([&] { return w.opened.size() == 2; }));
    ASSERT_TRUE(w.closePane(*second));
    ASSERT_TRUE(w.until([&] { return w.closedPane(*second); }));
    // The hangup ended the program: it is gone, or a zombie that waits for its reaper.
    EXPECT_TRUE(waitFor([&] {
        const std::string stat = readFile("/proc/" + std::to_string(program) + "/stat");
        return stat.empty() || stat.find(") Z ") != std::string::npos;
    }));
    // Its PaneClosed has no status: the window closed it.
    for (const Attach::Event& e : w.events) {
        if (e.type == Attach::EventType::PaneClosed) {
            EXPECT_EQ(e.status, -1);
        }
    }
    EXPECT_EQ(sessionNamed(daemon, "s")->panes, 1);
    EXPECT_FALSE(w.got(Attach::EventType::SessionEnded));
    // A pane that is gone already: nothing happens.
    ASSERT_TRUE(w.closePane(*second));
    // The last pane: the session ends, as when its last program exits.
    ASSERT_TRUE(w.closePane(w.pane));
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::SessionEnded); }));
    EXPECT_FALSE(sessionNamed(daemon, "s"));
}

// While no window shows the session, a pane whose program exits leaves the
// layout: the next window does not get it.
TEST(MuxAttach, APaneThatExitsWhileDetachedLeavesTheLayout)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    const std::string go = daemon.dir() + "/go";
    std::uint32_t first = 0;
    {
        Window w;
        ASSERT_EQ(w.attach(daemon, "s"), "");
        ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
        first = w.pane;
        const auto second = newPane(daemon, "s", launchOf({"/bin/sh", "-c", "while [ ! -e '" + go + "' ]; do sleep 0.05; done"}, daemon));
        ASSERT_TRUE(second) << daemon.lastError();
        Layout split;
        split.tabs.push_back(LayoutTab{{LayoutNode{0, false, 0.5}, LayoutNode{first}, LayoutNode{*second}}, *second, 0, ""});
        ASSERT_TRUE(w.sendLayout(split));
        ASSERT_TRUE(w.until([&] { return w.opened.size() == 2; }));
        ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"s"})) << daemon.lastError();
        ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached); }));
    }
    std::ofstream(go).flush();
    ASSERT_TRUE(waitFor([&] { return sessionNamed(daemon, "s")->panes == 1; }));
    Window again;
    ASSERT_EQ(again.attach(daemon, "s"), "");
    ASSERT_TRUE(again.until([&] { return again.pane != 0; }));
    ASSERT_EQ(again.layouts.size(), 1u);
    EXPECT_EQ(again.layouts[0].tabs.size(), 1u);
    EXPECT_EQ(again.layouts[0].tabs[0].tree, std::vector<LayoutNode>{LayoutNode{first}});
    EXPECT_EQ(again.layouts[0].tabs[0].focused, first);
}

// A tab that the user drags out of the session's window: its panes go to a
// new session with their layout, and their programs run on.
TEST(MuxAttach, PanesThatBreakOutRunOnInANewSession)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    SessionOptions o = daemon.options(Waiting, "s");
    o.environment.emplace_back("FROM_SESSION", "1");
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    const std::string pidFile = daemon.dir() + "/pid";
    const auto second = newPane(daemon, "s", launchOf({"/bin/sh", "-c", "echo $$ > '" + pidFile + "'; exec sleep 60"}, daemon));
    const auto third = newPane(daemon, "s", launchOf(Waiting, daemon));
    ASSERT_TRUE(second && third) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return readFile(pidFile).find('\n') != std::string::npos; }));
    const pid_t program = pid_t(std::strtol(readFile(pidFile).c_str(), nullptr, 10));
    ASSERT_TRUE(w.until([&] { return w.opened.size() == 3; }));

    Layout moved;
    moved.tabs.push_back(LayoutTab{{LayoutNode{0, true, 0.4}, LayoutNode{*second}, LayoutNode{*third}}, *third, 0, "build"});
    const auto result = daemon.call(Fn::BreakOut, Json::Array{ProtocolVersion, "s", toJson(moved)});
    ASSERT_TRUE(result) << daemon.lastError();
    const std::string fresh = (*result)["name"].toString();
    ASSERT_TRUE(validSessionName(fresh)) << fresh;
    // The window of the first session no longer shows them, and the session
    // keeps its first pane.
    ASSERT_TRUE(w.until([&] { return w.closedPane(*second) && w.closedPane(*third); }));
    EXPECT_FALSE(w.got(Attach::EventType::SessionEnded));
    EXPECT_EQ(sessionNamed(daemon, "s")->panes, 1);
    ASSERT_TRUE(sessionNamed(daemon, fresh));
    EXPECT_EQ(sessionNamed(daemon, fresh)->panes, 2);
    EXPECT_EQ(::kill(program, 0), 0);
    // A window of the new session gets their layout.
    Window other;
    ASSERT_EQ(other.attach(daemon, fresh), "");
    ASSERT_TRUE(other.until([&] { return other.opened.size() == 2; }));
    ASSERT_EQ(other.layouts.size(), 1u);
    EXPECT_TRUE(other.layouts[0] == moved) << Json::write(toJson(other.layouts[0]));
    // Each pane names the session that holds it now: its AFTERGLOW_SESSION
    // still names the first one.
    const auto holder = daemon.call(Fn::PaneSession, Json::Array{std::int64_t(*second)});
    ASSERT_TRUE(holder) << daemon.lastError();
    EXPECT_EQ(holder->toString(), fresh);
    EXPECT_EQ(daemon.call(Fn::PaneSession, Json::Array{std::int64_t(w.pane)})->toString(), "s");
    IpcConnection::Error noPane;
    EXPECT_FALSE(daemon.call(Fn::PaneSession, Json::Array{9999}, &noPane));
    EXPECT_EQ(noPane.code, Error::NoSuchPane);
    // A new pane of the new session has the environment of the first one.
    const std::string out = daemon.dir() + "/env";
    ASSERT_TRUE(newPane(daemon, fresh, launchOf(writesEnvironment(out), daemon))) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return !readFile(out).empty(); }));
    EXPECT_NE(("\n" + readFile(out)).find("\nFROM_SESSION=1\n"), std::string::npos);
}

TEST(MuxAttach, BreakOutRefusesBadCalls)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    const auto second = newPane(daemon, "s", launchOf(Waiting, daemon));
    ASSERT_TRUE(second) << daemon.lastError();
    const std::vector<std::string> before = names(daemon.sessions());
    Layout alone;
    alone.addTab(*second);
    Layout unknown;
    unknown.addTab(9999);
    Layout all;
    all.addTab(*second);
    all.addTab(*second - 1);
    const std::pair<Json::Array, std::string> bad[] = {
        {Json::Array{ProtocolVersion, "none", toJson(alone)}, Error::NoSuchSession},
        {Json::Array{ProtocolVersion - 1, "s", toJson(alone)}, Error::VersionMismatch},
        {Json::Array{ProtocolVersion, "s", Json::Object{}}, IpcErrorCode::InvalidArg},
        {Json::Array{ProtocolVersion, "s"}, IpcErrorCode::InvalidArg},
        {Json::Array{ProtocolVersion, "s", toJson(unknown)}, Error::NoSuchPane},
        // Every pane of the session: it would only get another name.
        {Json::Array{ProtocolVersion, "s", toJson(all)}, IpcErrorCode::InvalidArg},
    };
    for (const auto& [args, code] : bad) {
        IpcConnection::Error error;
        EXPECT_FALSE(daemon.call(Fn::BreakOut, args, &error)) << Json::write(args);
        EXPECT_EQ(error.code, code) << Json::write(args) << ": " << error.message;
    }
    EXPECT_EQ(sessionNamed(daemon, "s")->panes, 2);
    EXPECT_EQ(names(daemon.sessions()), before);
}

namespace {

/// A client of the daemon's control target, as `afterglow ctl --session`
/// speaks it: one connection, which selects its session first.
struct ControlClient
{
    explicit ControlClient(TestDaemon& daemon)
    {
        IpcConnection::Error error;
        if (!connection.connect(daemon.socket(), &error, 2000)) ADD_FAILURE() << "no connection: " << error.message;
    }
    /// Empty on success, else the error code.
    std::string select(const std::string& name)
    {
        return call(Fn::SelectSession, Json::Array{ProtocolVersion, name}, Target).has_value() ? std::string() : error.code;
    }
    std::optional<Json::Value> call(const std::string& fn, Json::Array args, const std::string& target = "control")
    {
        error = {};
        return connection.call(target, fn, std::move(args), 5000, &error);
    }
    IpcConnection connection;
    IpcConnection::Error error;
};

/// A session with a plain sh whose text `afterglow ctl get-text` may read.
bool readableShell(TestDaemon& daemon, const std::string& name, bool readText = true)
{
    SessionOptions o = daemon.options({"/bin/sh"}, name);
    o.launch.environment.emplace_back("PS1", "$ ");
    o.launch.environment.emplace_back("ENV", "/dev/null");
    o.control.allowReadText = readText;
    return bool(daemon.newSession(o));
}

std::vector<std::uint32_t> paneIds(const Json::Value& panes)
{
    std::vector<std::uint32_t> ids;
    for (const Json::Value& pane : panes.toArray()) ids.push_back(std::uint32_t(pane["id"].toInteger().value_or(0)));
    return ids;
}

} // namespace

TEST(MuxControl, ACallNeedsASelectedSession)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "s")) << daemon.lastError();
    ControlClient ctl(daemon);
    EXPECT_FALSE(ctl.call("listPanes", {}));
    EXPECT_EQ(ctl.error.code, IpcErrorCode::CallFailed);
    EXPECT_NE(ctl.error.message.find("no session is selected"), std::string::npos) << ctl.error.message;
    EXPECT_EQ(ctl.select("none"), Error::NoSuchSession);
    EXPECT_FALSE(ctl.call(Fn::SelectSession, Json::Array{ProtocolVersion - 1, "s"}, Target));
    EXPECT_EQ(ctl.error.code, Error::VersionMismatch);
    EXPECT_FALSE(ctl.call(Fn::SelectPane, Json::Array{ProtocolVersion, 9999}, Target));
    EXPECT_EQ(ctl.error.code, Error::NoSuchPane);
    // By a pane: the session that holds it.
    const auto selected = ctl.call(Fn::SelectPane, Json::Array{ProtocolVersion, 2}, Target);
    ASSERT_TRUE(selected) << ctl.error.message;
    EXPECT_EQ((*selected)["name"].toString(), "s");
    // `afterglow ssh` asks for remote commands: a caller that runs in no
    // pane of the daemon has none.
    const auto enabled = ctl.call("remoteCommandsEnabled", {});
    ASSERT_TRUE(enabled) << ctl.error.message;
    EXPECT_FALSE(enabled->toBool());
}

namespace {

/// A process that `afterglow ssh` plays: its socket ends with it.
pid_t sleeper()
{
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::execl("/bin/sleep", "sleep", "60", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return pid;
}

void endProcess(pid_t pid)
{
    ::kill(pid, SIGKILL);
    ::waitpid(pid, nullptr, 0);
}

/// The program of a session pane, which makes the calls of `afterglow ssh`
/// from there: the daemon opens a remote socket only for a caller in its
/// pane. Each line that it reads goes to the daemon, and its answer comes
/// back; "spawn" starts a sleep in the pane and answers with its process ID.
struct PaneAgent
{
    PaneAgent(TestDaemon& daemon, const std::string& name)
        : input(daemon.dir() + "/agent-" + name + ".in")
        , output(daemon.dir() + "/agent-" + name + ".out")
    {
        if (::mkfifo(input.c_str(), 0600) != 0) ADD_FAILURE() << "no fifo " << input;
        static const std::string script = R"(import os, socket, sys
s = socket.socket(socket.AF_UNIX)
s.connect(os.environ['AFTERGLOW_SOCKET'])
buf = b''
def answer(line):
    with open(sys.argv[2], 'ab') as f:
        f.write(line)
while True:
    with open(sys.argv[1], 'rb') as f:
        for line in f:
            if line.strip() == b'spawn':
                pid = os.fork()
                if pid == 0:
                    os.execvp('sleep', ['sleep', '60'])
                answer(b'%d\n' % pid)
                continue
            s.sendall(line)
            while b'\n' not in buf:
                more = s.recv(65536)
                if not more:
                    sys.exit(1)
                buf += more
            reply, _, buf = buf.partition(b'\n')
            answer(reply + b'\n')
)";
        started = bool(daemon.newSession(daemon.options({"python3", "-I", "-c", script, input, output}, name)));
    }
    /// The next answer, after @p line went to the agent.
    std::string ask(const std::string& line)
    {
        // Without blocking: an agent that ended fails the test, not hangs it.
        int fd = -1;
        if (!waitFor([&] { return (fd = ::open(input.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC)) >= 0; })) return {};
        const std::string text = line + '\n';
        std::size_t written = 0;
        while (written < text.size()) {
            const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
            if (n > 0) written += std::size_t(n);
            else if (n < 0 && errno != EAGAIN && errno != EINTR) break;
        }
        ::close(fd);
        std::string answer;
        waitFor([&] {
            const std::string all = readFile(output);
            std::size_t at = 0;
            for (std::size_t i = 0; i < answered && at != std::string::npos; ++i) at = all.find('\n', at) + 1;
            const std::size_t end = all.find('\n', at);
            if (end == std::string::npos) return false;
            answer = all.substr(at, end - at);
            return true;
        });
        ++answered;
        return answer;
    }
    /// A process in the pane, which plays `afterglow ssh`.
    pid_t spawn() { return pid_t(std::strtol(ask("spawn").c_str(), nullptr, 10)); }
    /// A call of the control target from the pane, with its error message.
    std::optional<Json::Value> call(const std::string& fn, Json::Array args)
    {
        error.clear();
        const auto reply = Json::parse(ask(Json::write(Json::Object{
            {"type", "call"}, {"id", std::int64_t(++calls)}, {"target", "control"}, {"fn", fn}, {"args", std::move(args)}})));
        if (!reply) {
            error = "no answer";
            return std::nullopt;
        }
        if (const Json::Value* result = reply->find("result")) return *result;
        error = (*reply)["message"].toString();
        if (const Json::Value* inner = reply->find("error")) error = (*inner)["message"].toString();
        return std::nullopt;
    }
    std::string input;
    std::string output;
    bool started = false;
    std::size_t answered = 0;
    std::int64_t calls = 0;
    std::string error;
};

} // namespace

// `afterglow ssh` in a session pane: the daemon keeps the socket that ssh
// forwards for remote commands, with the checks and the lifetime of the
// sockets that a window keeps for its own panes (RemoteCommandsTest).
TEST(MuxControl, AnSshSessionOfASessionPaneGetsASocketOfTheDaemon)
{
    if (!pythonFound()) GTEST_SKIP() << "python3 is not installed";
    TestDaemon daemon;
    // An ssh that logs its arguments: the daemon runs it to cancel a forward.
    const std::string bin = daemon.dir() + "/bin";
    const std::string log = daemon.dir() + "/ssh.log";
    ASSERT_EQ(::mkdir(bin.c_str(), 0700), 0);
    {
        std::ofstream ssh(bin + "/ssh");
        ssh << "#!/bin/sh\nprintf '%s\\n' \"$*\" >> '" << log << "'\n";
    }
    ASSERT_EQ(::chmod((bin + "/ssh").c_str(), 0700), 0);
    const char* path = std::getenv("PATH");
    daemon.daemonOnly("PATH", bin + ":" + (path ? path : "/usr/bin:/bin"));
    // The socket of an ssh session that a killed daemon left goes.
    const std::string directory = daemon.runtime() + "/aglow-mux";
    const std::string stale = directory + "/remote-0123456789abcdef.sock";
    ASSERT_EQ(::mkdir(directory.c_str(), 0700), 0);
    std::ofstream(stale).put('x');
    ASSERT_TRUE(exists(stale));
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    EXPECT_FALSE(exists(stale));
    EXPECT_FALSE(exists(stale + ".lock"));
    PaneAgent agent(daemon, "s");
    ASSERT_TRUE(agent.started) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("s"), "");
    const auto listed = ctl.call("listPanes", {});
    ASSERT_TRUE(listed) << ctl.error.message;
    const std::uint32_t pane = paneIds(*listed).front();
    pid_t process = agent.spawn();
    ASSERT_GT(process, 0);
    const auto args = [&](std::int64_t protocol, const std::string& directory, bool overlay, const std::string& host) {
        return Json::Array{std::int64_t(pane), std::int64_t(process), host, directory, protocol, Json::Array{"-F", "none", "-S", "/cm"}, "prod",
                           overlay};
    };
    const auto open = [&](std::int64_t protocol, std::string directory = "/run/user/1000/afterglow", bool overlay = false,
                          std::string host = "box") { return agent.call("openRemoteSocket", args(protocol, directory, overlay, host)); };

    // Only from the pane, for a process of the pane: a process of another
    // terminal that names the pane gets no socket.
    const std::string onlyThePane = "only `afterglow ssh` in pane " + std::to_string(pane) + " opens a remote socket for it";
    EXPECT_FALSE(ctl.call("openRemoteSocket", args(AfterglowProto::ProtocolVersion, "/run/user/1000/afterglow", false, "box")));
    EXPECT_EQ(ctl.error.message, onlyThePane);
    pid_t outside = sleeper();
    std::swap(process, outside);
    EXPECT_FALSE(open(AfterglowProto::ProtocolVersion));
    EXPECT_EQ(agent.error, onlyThePane);
    std::swap(process, outside);
    endProcess(outside);

    const auto opened = open(AfterglowProto::ProtocolVersion);
    ASSERT_TRUE(opened) << agent.error;
    const std::string token = (*opened)["token"].toString();
    const std::string socket = (*opened)["socket"].toString();
    const std::string remote = (*opened)["path"].toString();
    EXPECT_EQ(token.size(), 16u);
    EXPECT_EQ(remote, "/run/user/1000/afterglow/" + token + ".sock");
    EXPECT_EQ(socket, daemon.runtime() + "/aglow-mux/remote-" + token + ".sock");
    struct stat info{};
    ASSERT_EQ(::stat(socket.c_str(), &info), 0);
    EXPECT_TRUE(S_ISSOCK(info.st_mode));
    EXPECT_EQ(info.st_mode & 077, 0u);
    EXPECT_TRUE(ctl.call("remoteSocketForwarded", Json::Array{token})) << ctl.error.message;
    {
        // While no window shows the session, its client hears why.
        RawClient client(socket);
        ASSERT_TRUE(client.connected());
        const auto reply = Json::parse(client.line());
        ASSERT_TRUE(reply);
        EXPECT_EQ((*reply)["type"].toString(), "error");
        EXPECT_FALSE(reply->find("id"));
        EXPECT_NE((*reply)["message"].toString().find("no window shows session 's'"), std::string::npos) << Json::write(*reply);
    }

    // Another protocol, and a path too long for the remote host: an answer
    // and no socket.
    auto other = open(AfterglowProto::ProtocolVersion + 1);
    ASSERT_TRUE(other) << agent.error;
    EXPECT_EQ((*other)["error"].toString(), "version");
    EXPECT_FALSE(other->find("socket"));
    other = open(AfterglowProto::ProtocolVersion, "/" + std::string(100, 'd'));
    ASSERT_TRUE(other) << agent.error;
    EXPECT_EQ((*other)["error"].toString(), "length");
    // What a window refuses too.
    const auto refuses = [&](std::optional<Json::Value> refused, const std::string& why) {
        EXPECT_FALSE(refused) << why;
        EXPECT_NE(agent.error.find(why), std::string::npos) << agent.error;
    };
    refuses(open(AfterglowProto::ProtocolVersion, "/run/user/1000/afterglow", true), "overlay");
    refuses(open(AfterglowProto::ProtocolVersion, "run/user"), "remote directory");
    refuses(open(AfterglowProto::ProtocolVersion, "/a:b"), "remote directory");
    refuses(open(AfterglowProto::ProtocolVersion, "/run/user/1000/afterglow", false, ""), "host");
    refuses(open(AfterglowProto::ProtocolVersion, "/run/user/1000/afterglow", false, "a\x1b"), "host");

    // The socket ends with the process of `afterglow ssh`, and its forward
    // is cancelled with the options that reach the shared connection.
    endProcess(process);
    EXPECT_TRUE(waitFor([&] { return !exists(socket); }));
    const std::string cancel = "-F none -S /cm -O cancel -R " + remote + ":" + socket + " -- prod\n";
    EXPECT_TRUE(waitFor([&] { return readFile(log).find(cancel) != std::string::npos; })) << readFile(log);
    EXPECT_FALSE(ctl.call("remoteSocketForwarded", Json::Array{token}));

    // A forward that failed: the socket closes, and nothing is cancelled.
    process = agent.spawn();
    const auto failing = open(AfterglowProto::ProtocolVersion);
    ASSERT_TRUE(failing) << agent.error;
    const std::string failed = (*failing)["socket"].toString();
    EXPECT_FALSE(ctl.call("remoteSocketFailed", Json::Array{(*failing)["token"].toString(), "other"}));
    EXPECT_TRUE(ctl.call("remoteSocketFailed", Json::Array{(*failing)["token"].toString(), "forward"})) << ctl.error.message;
    EXPECT_FALSE(exists(failed));
    // A window turns remote commands off: the sessions end, and a new one
    // gets the reason. On again, a new one opens.
    {
        const auto live = open(AfterglowProto::ProtocolVersion);
        ASSERT_TRUE(live) << agent.error;
        const std::string liveSocket = (*live)["socket"].toString();
        Window w;
        ASSERT_EQ(w.attach(daemon, "s"), "");
        ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
        SessionSettings settings{daemon.options({"/bin/sh"}, "x").terminal, {true, true, "remote commands are off (Terminal.sshRemoteCommands)"}};
        ASSERT_TRUE(w.sendSettings(settings));
        EXPECT_TRUE(w.until([&] { return !exists(liveSocket); }));
        EXPECT_FALSE(open(AfterglowProto::ProtocolVersion));
        EXPECT_EQ(agent.error, "remote commands are off (Terminal.sshRemoteCommands)");
        settings.control.remoteCommandsOff.clear();
        ASSERT_TRUE(w.sendSettings(settings));
        EXPECT_TRUE(w.until([&] { return bool(open(AfterglowProto::ProtocolVersion)); })) << agent.error;
    }
    // The pane closes: its sessions end.
    const auto closing = open(AfterglowProto::ProtocolVersion);
    ASSERT_TRUE(closing) << agent.error;
    const std::string closed = (*closing)["socket"].toString();
    ASSERT_TRUE(ctl.call("close", Json::Array{std::int64_t(pane)})) << ctl.error.message;
    EXPECT_TRUE(waitFor([&] { return !exists(closed); }));
    EXPECT_TRUE(waitFor([&] { return readFile(log).find(closed) != std::string::npos; })) << readFile(log);
    EXPECT_EQ(readFile(log).find(failed), std::string::npos) << readFile(log);
    endProcess(process);
}

// The session follows the settings of the window that shows it: the daemon
// gives its panes the new terminal values, whose colors reach the window
// with the next frame, and answers `afterglow ctl` by the new Control
// values, also once no window shows the session.
TEST(MuxControl, TheSessionFollowsTheSettingsOfItsWindow)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    // A shell whose session has GONE, which a window's settings remove.
    SessionOptions options = daemon.options({"/bin/sh"}, "s");
    options.launch.environment.emplace_back("PS1", "$ ");
    options.launch.environment.emplace_back("ENV", "/dev/null");
    options.control.allowReadText = false;
    options.environment.emplace_back("GONE", "old");
    ASSERT_TRUE(daemon.newSession(options)) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("s"), "");
    const auto listed = ctl.call("listPanes", {});
    ASSERT_TRUE(listed) << ctl.error.message;
    const std::uint32_t pane = paneIds(*listed).front();
    EXPECT_FALSE(ctl.call("getText", Json::Array{std::int64_t(pane), "screen"}));
    SessionSettings settings{daemon.options({"/bin/sh"}, "x").terminal, {true, true, ""}};
    settings.terminal.defaultBackground = {1, 2, 3};
    // The panes that start later get the window's variables: a value, or none.
    settings.environment = {{"UPDATED", "new"}, {"GONE", std::nullopt}};
    {
        Window w;
        ASSERT_EQ(w.attach(daemon, "s"), "");
        ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "$ "); })) << w.screen();
        EXPECT_NE(w.replica.defaultBackground(), (AfterglowVt::Rgb{1, 2, 3}));
        ASSERT_TRUE(w.sendSettings(settings));
        EXPECT_TRUE(w.until([&] { return w.replica.defaultBackground() == AfterglowVt::Rgb{1, 2, 3}; }));
        EXPECT_TRUE(w.until([&] { return bool(ctl.call("getText", Json::Array{std::int64_t(pane), "screen"})); })) << ctl.error.message;
    }
    // No window shows it: the session keeps them, and its next window shows them.
    EXPECT_TRUE(ctl.call("getText", Json::Array{std::int64_t(pane), "screen"})) << ctl.error.message;
    const auto split = ctl.call("split", Json::Array{std::int64_t(pane), "right", Json::Array{}});
    ASSERT_TRUE(split) << ctl.error.message;
    const std::int64_t later = split->toInteger().value_or(0);
    ASSERT_TRUE(ctl.call("sendText", Json::Array{later, "echo \"[$UPDATED][${GONE-unset}]\"\n"})) << ctl.error.message;
    EXPECT_TRUE(waitFor([&] {
        const auto text = ctl.call("getText", Json::Array{later, "screen"});
        return text && contains(text->toString(), "\n[new][unset]");
    })) << ctl.error.message;
    Window next;
    ASSERT_EQ(next.attach(daemon, "s"), "");
    ASSERT_TRUE(next.until([&] { return next.pane != 0; }));
    EXPECT_TRUE(next.until([&] { return next.replica.defaultBackground() == AfterglowVt::Rgb{1, 2, 3}; }));
    // A window that turns Control.enabled off: the session has no control
    // target, as such a window has none.
    settings.control.enabled = false;
    ASSERT_TRUE(next.sendSettings(settings));
    EXPECT_TRUE(next.until([&] {
        return !ctl.call("listPanes", {}) && ctl.error.code == AfterglowProto::IpcErrorCode::NoSuchTarget;
    })) << ctl.error.code << ": " << ctl.error.message;
    // Settings that are not valid end the window's link, as a layout does.
    Attach::Input bad;
    bad.type = Attach::InputType::Settings;
    bad.bytes = R"({"terminal":{},"control":{}})";
    ASSERT_TRUE(next.send(bad));
    EXPECT_TRUE(next.until([&] { return next.ended; }));
    EXPECT_NE(daemon.errors().find("settings that are not valid"), std::string::npos) << daemon.errors();
}

// A session whose client had remote commands off gives `afterglow ssh` no
// socket, and says why.
TEST(MuxControl, RemoteCommandsOffInTheSessionGiveNoSocket)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    SessionOptions o = daemon.options({"/bin/sh"}, "off");
    o.control.remoteCommandsOff = "remote commands are off (Terminal.sshRemoteCommands)";
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("off"), "");
    const auto listed = ctl.call("listPanes", {});
    ASSERT_TRUE(listed) << ctl.error.message;
    const pid_t process = sleeper();
    EXPECT_FALSE(ctl.call("openRemoteSocket", Json::Array{std::int64_t(paneIds(*listed).front()), std::int64_t(process), "box",
                                                          "/run/user/1000/afterglow", AfterglowProto::ProtocolVersion, Json::Array{},
                                                          "prod", false}));
    EXPECT_EQ(ctl.error.message, "remote commands are off (Terminal.sshRemoteCommands)");
    endProcess(process);
}

// While no window shows the session, the daemon answers the calls on its
// panes, their text and their layout, with its own pane IDs.
TEST(MuxControl, TheDaemonAnswersWithoutAWindow)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "s")) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("s"), "");
    const auto listed = ctl.call("listPanes", {});
    ASSERT_TRUE(listed) << ctl.error.message;
    ASSERT_EQ(listed->toArray().size(), 1u);
    const std::uint32_t first = paneIds(*listed).front();
    EXPECT_TRUE(listed->toArray()[0]["focused"].toBool());
    EXPECT_EQ(listed->toArray()[0]["tab"].toInteger(), 0);
    EXPECT_EQ(ctl.call("focusedPane", {})->toInteger(), first);
    // Text in, text out. Pane 0 is the focused pane.
    ASSERT_TRUE(ctl.call("sendText", Json::Array{0, "echo hel''lo\n"})) << ctl.error.message;
    EXPECT_TRUE(waitFor([&] {
        const auto text = ctl.call("getText", Json::Array{std::int64_t(first), "screen"});
        return text && text->toString().find("\nhello") != std::string::npos;
    }));
    // A split takes the focus, and a tab shows.
    const auto split = ctl.call("split", Json::Array{std::int64_t(first), "down", Json::Array{}});
    ASSERT_TRUE(split) << ctl.error.message;
    const auto tab = ctl.call("newTab", Json::Array{std::int64_t(first), "", Json::Array{}});
    ASSERT_TRUE(tab) << ctl.error.message;
    const auto three = ctl.call("listPanes", {});
    EXPECT_EQ(paneIds(*three), (std::vector<std::uint32_t>{first, std::uint32_t(*split->toInteger()), std::uint32_t(*tab->toInteger())}));
    EXPECT_EQ(three->toArray()[2]["tab"].toInteger(), 1);
    EXPECT_EQ(ctl.call("focusedPane", {})->toInteger(), tab->toInteger());
    ASSERT_TRUE(ctl.call("focus", Json::Array{std::int64_t(first)}));
    EXPECT_EQ(ctl.call("focusedPane", {})->toInteger(), first);
    // The split moves to a tab of its own, beside its tab.
    const auto moved = ctl.call("movePane", Json::Array{*split, 0, "new-tab"});
    ASSERT_TRUE(moved) << ctl.error.message;
    EXPECT_EQ((*moved)["tab"].toInteger(), 1);
    ASSERT_TRUE(ctl.call("close", Json::Array{*tab})) << ctl.error.message;
    EXPECT_EQ(paneIds(*ctl.call("listPanes", {})), (std::vector<std::uint32_t>{first, std::uint32_t(*split->toInteger())}));
    // The next window gets that layout.
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.opened.size() == 2; }));
    ASSERT_EQ(w.layouts.size(), 1u);
    EXPECT_EQ(w.layouts[0].tabs.size(), 2u);
    // The tab that the move made shows, as in a window.
    EXPECT_EQ(w.layouts[0].current, 1u);
}

TEST(MuxControl, TheSessionsControlSettingsApply)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "closed", false)) << daemon.lastError();
    SessionOptions off = daemon.options(Waiting, "off");
    off.control.enabled = false;
    ASSERT_TRUE(daemon.newSession(off)) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("closed"), "");
    EXPECT_FALSE(ctl.call("getText", Json::Array{0, "screen"}));
    EXPECT_NE(ctl.error.message.find("Control.allowReadText"), std::string::npos) << ctl.error.message;
    ASSERT_EQ(ctl.select("off"), "");
    EXPECT_FALSE(ctl.call("listPanes", {}));
    EXPECT_EQ(ctl.error.code, IpcErrorCode::NoSuchTarget);
}

TEST(MuxControl, WhatOnlyAWindowDoesNeedsOne)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "s")) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("s"), "");
    EXPECT_FALSE(ctl.call("setTitle", Json::Array{0, "t"}));
    EXPECT_NE(ctl.error.message.find("no window shows session 's'"), std::string::npos) << ctl.error.message;
    EXPECT_FALSE(ctl.call("screenshot", Json::Array{0, "pane"}));
    EXPECT_NE(ctl.error.message.find("only a window can do screenshot"), std::string::npos) << ctl.error.message;
    EXPECT_FALSE(ctl.call("split", Json::Array{0, "right", Json::Array{"htop"}}));
    EXPECT_NE(ctl.error.message.find("launch"), std::string::npos) << ctl.error.message;
    EXPECT_FALSE(ctl.call("noSuchCall", {}));
    EXPECT_EQ(ctl.error.code, IpcErrorCode::NoSuchFn);
}

// While a window shows the session, the daemon places the panes that it
// starts in that window, and passes it what only a window can answer.
TEST(MuxControl, AWindowPlacesNewPanesAndAnswersWindowCalls)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "s")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("s"), "");
    const auto split = ctl.call("split", Json::Array{std::int64_t(w.pane), "left", Json::Array{}});
    ASSERT_TRUE(split) << ctl.error.message;
    const auto controls = [&w] {
        std::vector<Json::Value> calls;
        for (const Attach::Event& e : w.events) {
            if (e.type == Attach::EventType::Control) calls.push_back(*Json::parse(e.payload));
        }
        return calls;
    };
    ASSERT_TRUE(w.until([&] { return !controls().empty() && w.opened.size() == 2; }));
    const Json::Value place = controls().front();
    EXPECT_EQ(place["id"].toInteger(), 0);
    EXPECT_EQ(place["fn"].toString(), "placePane");
    EXPECT_EQ(Json::write(place["args"]), "[" + std::to_string(*split->toInteger()) + "," + std::to_string(w.pane) + ",\"left\",[]]");

    // The window answers a title; the client waits for that answer.
    std::optional<Json::Value> titled;
    std::atomic<bool> answered{false};
    std::thread caller([&] {
        titled = ctl.call("setTitle", Json::Array{std::int64_t(w.pane), "work"});
        answered = true;
    });
    ASSERT_TRUE(w.until([&] { return controls().size() == 2; }));
    const Json::Value call = controls().back();
    EXPECT_EQ(call["fn"].toString(), "setTitle");
    EXPECT_EQ(Json::write(call["args"]), "[" + std::to_string(w.pane) + ",\"work\"]");
    Attach::Input reply;
    reply.type = Attach::InputType::ControlReply;
    reply.bytes = Json::write(Json::Object{{"id", call["id"]}, {"result", "done"}});
    EXPECT_TRUE(w.send(reply));
    EXPECT_TRUE(w.until([&] { return answered.load(); }));
    caller.join();
    ASSERT_TRUE(titled) << ctl.error.message;
    EXPECT_EQ(titled->toString(), "done");

    // A window that goes before it answers: the client hears why.
    std::thread waiting([&] { titled = ctl.call("notify", Json::Array{std::int64_t(w.pane), "t", "b"}); });
    ASSERT_TRUE(w.until([&] { return controls().size() == 3; }));
    ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"s"})) << daemon.lastError();
    waiting.join();
    EXPECT_FALSE(titled);
    EXPECT_NE(ctl.error.message.find("closed before it answered"), std::string::npos) << ctl.error.message;
}

// The calls of `afterglow ctl overlay` and `screenshot` go to the window that
// shows the session, with the daemon's number of the client: the window
// keeps that client's overlays and inputs, and hears when it goes. A large
// answer, such as a PNG, comes in parts, and an error keeps its code.
TEST(MuxControl, OverlaysAndScreenshotsGoToTheWindowWithTheirClient)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "s")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    const auto controls = [&w] {
        std::vector<Json::Value> calls;
        for (const Attach::Event& e : w.events) {
            if (e.type == Attach::EventType::Control) calls.push_back(*Json::parse(e.payload));
        }
        return calls;
    };
    auto ctl = std::make_unique<ControlClient>(daemon);
    ASSERT_EQ(ctl->select("s"), "");

    // Each call waits on a thread of its own while the window's stream runs.
    // A failed assertion joins it first: a call ends within its timeout.
    std::thread caller;
    struct JoinAtEnd
    {
        std::thread& thread;
        ~JoinAtEnd()
        {
            if (thread.joinable()) thread.join();
        }
    } joinAtEnd{caller};
    std::atomic<bool> answered{false};
    const auto join = [&](std::thread& caller) {
        EXPECT_TRUE(w.until([&] { return answered.load(); }));
        caller.join();
        answered = false;
    };
    // A screenshot whose answer is larger than a part: 3 parts and the rest.
    std::optional<Json::Value> shot;
    caller = std::thread([&] {
        shot = ctl->call("screenshot", Json::Array{0, "pane"});
        answered = true;
    });
    ASSERT_TRUE(w.until([&] { return controls().size() == 1; }));
    const Json::Value call = controls().back();
    EXPECT_EQ(call["fn"].toString(), "screenshot");
    // The focused pane, by its ID.
    EXPECT_EQ(Json::write(call["args"]), "[" + std::to_string(w.pane) + ",\"pane\"]");
    const std::int64_t client = call["client"].toInteger().value_or(0);
    EXPECT_GT(client, 0);
    const std::string png(3 * Attach::ControlReplyPartBytes + 17, 'A');
    const std::string answer = Json::write(Json::Object{{"id", call["id"]}, {"result", Json::Object{{"png", png}, {"width", 4}, {"height", 2}}}});
    for (std::size_t at = 0; at < answer.size(); at += Attach::ControlReplyPartBytes) {
        Attach::Input part;
        part.type = at + Attach::ControlReplyPartBytes < answer.size() ? Attach::InputType::ControlReplyPart : Attach::InputType::ControlReply;
        part.bytes = answer.substr(at, Attach::ControlReplyPartBytes);
        ASSERT_TRUE(w.send(part));
    }
    join(caller);
    ASSERT_TRUE(shot) << ctl->error.message;
    EXPECT_EQ((*shot)["png"].toString().size(), png.size());
    EXPECT_EQ((*shot)["width"].toInteger(), 4);

    // An overlay's input and the overlay come with the same client, and an
    // error of the window keeps its code.
    std::optional<Json::Value> input;
    caller = std::thread([&] {
        input = ctl->call("overlayInput", Json::Array{"in", "text", true});
        answered = true;
    });
    ASSERT_TRUE(w.until([&] { return controls().size() == 2; }));
    EXPECT_EQ(controls().back()["fn"].toString(), "overlayInput");
    EXPECT_EQ(controls().back()["client"].toInteger(), client);
    Attach::Input reply;
    reply.type = Attach::InputType::ControlReply;
    reply.bytes = Json::write(Json::Object{{"id", controls().back()["id"]}, {"result", true}});
    ASSERT_TRUE(w.send(reply));
    join(caller);
    EXPECT_TRUE(input) << ctl->error.message;
    std::optional<Json::Value> overlay;
    caller = std::thread([&] {
        overlay = ctl->call("overlay", Json::Array{0, Json::Array{"true"}, Json::Object{{"input", "in"}}});
        answered = true;
    });
    ASSERT_TRUE(w.until([&] { return controls().size() == 3; }));
    EXPECT_EQ(controls().back()["fn"].toString(), "overlay");
    EXPECT_EQ(controls().back()["client"].toInteger(), client);
    reply.bytes = Json::write(Json::Object{{"id", controls().back()["id"]},
                                           {"error", Json::Object{{"code", "spawn_failed"}, {"message", "no program true"}}}});
    ASSERT_TRUE(w.send(reply));
    join(caller);
    EXPECT_FALSE(overlay);
    EXPECT_EQ(ctl->error.code, "spawn_failed");

    // The client goes: its window hears it, by its number.
    ctl.reset();
    ASSERT_TRUE(w.until([&] { return controls().size() == 4; }));
    EXPECT_EQ(controls().back()["fn"].toString(), "clientGone");
    EXPECT_EQ(controls().back()["id"].toInteger(), 0);
    EXPECT_EQ(Json::write(controls().back()["args"]), "[" + std::to_string(client) + "]");
}

// A screenshot needs Control.allowReadText: refused with the window's code
// first, also for a pane that does not exist, and with no window.
TEST(MuxControl, AScreenshotWithoutReadAccessIsDenied)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "s", false)) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("s"), "");
    EXPECT_FALSE(ctl.call("screenshot", Json::Array{99, "pane"}));
    EXPECT_EQ(ctl.error.code, "denied");
    EXPECT_EQ(ctl.error.message, "screenshots are off (Control.allowReadText)");
}
