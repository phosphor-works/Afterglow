// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// A window attached to a session, played without Qt: the stream of the attach
// call, and a replica terminal that only the daemon's frames change. Each test
// runs its own daemon (TestDaemon.h).

#include "TestDaemon.h"

#include "Attach.h"
#include "ImageFiles.h"
#include "MuxHost.h"
#include "MuxLayout.h"
#include "MuxStream.h"

#include <AfterglowProto/Base64.h>
#include <AfterglowProto/Remote.h>
#include <AfterglowProto/Version.h>
#include <AfterglowVt/Graphics.h>
#include <AfterglowVt/KeyNames.h>
#include <AfterglowVt/StateSync.h>
#include <AfterglowVt/Terminal.h>
#include <PhosphorChannel/Stream.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <map>
#include <memory>
#include <set>

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
struct Window
{
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
        serial = (*result)["window"].toInteger().value_or(0);
        return {};
    }

    /// The window's stream, on the connection of the call.
    std::string connect()
    {
        IpcConnection::Error error;
        const int fd = connection->release(&error);
        if (fd < 0) return "no stream: " + error.message;
        PhosphorChannel::Stream::Options options;
        if (streamWindow) options.window = streamWindow;
        stream = std::make_unique<PhosphorChannel::Stream>(fd, fd, options);
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
                    callSentAfter.push_back(event->frames);
                    // As a window does: it writes the terminal's reports, to
                    // the pane whose terminal made them.
                    if (call->kind == Host::Kind::Report && answerReports) {
                        Attach::Input report;
                        report.pane = event->pane;
                        report.bytes = call->text;
                        if (!send(report)) ADD_FAILURE() << "a report does not go";
                    }
                }
                if (event->type == Attach::EventType::PaneOpened) {
                    // The first event is the layout, which names the pane.
                    if (layouts.empty()) ADD_FAILURE() << "a pane opened before the layout came";
                    // A pane opens once in a window while it is there: one
                    // that closed comes back only when it went to another
                    // session and the window let it go (forget()).
                    if (std::find(opened.begin(), opened.end(), event->pane) != opened.end() && !forgotten.contains(event->pane)) {
                        ADD_FAILURE() << "pane " << event->pane << " opened twice";
                    }
                    forgotten.erase(event->pane);
                    opened.push_back(event->pane);
                    // The test reads the frames of the session's first pane
                    // into replica, and those of the others into their own.
                    if (!pane) {
                        pane = event->pane;
                        channel = event->channel;
                    } else {
                        others[event->pane] = std::make_unique<OtherPane>(event->channel);
                    }
                }
                if (event->type == Attach::EventType::PaneClosed) closed.push_back(event->pane);
                if (event->type == Attach::EventType::Commands) {
                    auto frame = Attach::decodeRelay(event->payload);
                    if (frame) relays.push_back(std::move(*frame));
                    else ADD_FAILURE() << "a Commands event does not decode";
                }
                if (event->type == Attach::EventType::Refreshed) refreshes.push_back({event->pane, event->frames});
                if (event->type == Attach::EventType::Layout) {
                    const auto json = Json::parse(event->payload);
                    std::string error;
                    auto layout = json ? layoutFrom(*json, &error) : std::nullopt;
                    if (!layout) ADD_FAILURE() << "the layout does not decode: " << error;
                    else layouts.push_back(std::move(*layout));
                }
                if (event->type == Attach::EventType::PaneClosed && event->pane == pane && channelClosed && !screenAtExit) screenAtExit = screen();
            }
        }
        // The frames of a pane that opened in these events.
        readFrames();
    }

    void readFrames()
    {
        std::string frame;
        for (auto& [id, other] : others) {
            auto* state = other->closed ? nullptr : stream->reader(other->channel);
            while (state) {
                const PhosphorChannel::ReadResult result = state->read(frame);
                if (result == PhosphorChannel::ReadResult::Frame) {
                    ++other->frames;
                    std::string error;
                    if (!other->reader.apply(other->replica, frame, &error)) ADD_FAILURE() << "a frame of pane " << id << " does not apply: " << error;
                    continue;
                }
                // Read to its end: the daemon may give its channel to a pane
                // that opens later.
                if (result == PhosphorChannel::ReadResult::Closed) other->closed = true;
                break;
            }
        }
        if (auto* state = channel && !channelClosed ? stream->reader(channel) : nullptr) {
            while (true) {
                const PhosphorChannel::ReadResult result = state->read(frame);
                if (result == PhosphorChannel::ReadResult::Frame) {
                    ++frames;
                    std::string error;
                    if (!reader.apply(replica, frame, &error)) ADD_FAILURE() << "a frame does not apply: " << error;
                    if (reader.screenChanged()) ++screenFrames;
                    if (!marker.empty() && !markerFrame && screen().find(marker) != std::string::npos) markerFrame = frames;
                    continue;
                }
                if (result == PhosphorChannel::ReadResult::Closed) {
                    channelClosed = true;
                    if (closedPane(pane) && !screenAtExit) screenAtExit = screen();
                }
                break;
            }
        }
    }

    /// The frames that came of pane @p id.
    int framesOf(std::uint32_t id) const
    {
        if (id == pane) return frames;
        const auto it = others.find(id);
        return it == others.end() ? 0 : it->second->frames;
    }

    bool got(Attach::EventType type) const
    {
        return std::any_of(events.begin(), events.end(), [type](const Attach::Event& e) { return e.type == type; });
    }

    /// The Control events, as their JSON. One that is not JSON is a failure.
    std::vector<Json::Value> controls() const
    {
        std::vector<Json::Value> calls;
        for (const Attach::Event& e : events) {
            if (e.type != Attach::EventType::Control) continue;
            auto json = Json::parse(e.payload);
            if (json) calls.push_back(std::move(*json));
            else ADD_FAILURE() << "a Control event is not JSON: " << e.payload;
        }
        return calls;
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

    /// As a window sends: what is not for a program (Attach::isAction) on
    /// the action channel, the rest with the input.
    [[nodiscard]] bool send(const Attach::Input& message)
    {
        return (Attach::isAction(message.type) ? actions : input)->tryWrite(Attach::encode(message)) == PhosphorChannel::WriteResult::Written;
    }

    /// Says whether the user can see pane @p id.
    [[nodiscard]] bool show(std::uint32_t id, bool visible)
    {
        Attach::Input message;
        message.type = Attach::InputType::Visible;
        message.pane = id;
        message.visible = visible;
        return send(message);
    }

    /// Asks for the screens of pane @p id, as a read of its text does.
    [[nodiscard]] bool refresh(std::uint32_t id)
    {
        Attach::Input message;
        message.type = Attach::InputType::Refresh;
        message.pane = id;
        return send(message);
    }

    /// Waits until the daemon finished the pass that read what the window
    /// sent before: two reads of the first pane's text, each with its
    /// Refreshed. The daemon reads the actions first in a pass, and a frame
    /// that came before them in the same pass, so the second read comes in
    /// a later pass. The window reads its events meanwhile.
    [[nodiscard]] bool roundTrip()
    {
        for (int i = 0; i < 2; ++i) {
            const std::size_t before = refreshes.size();
            if (!refresh(pane) || !until([&] { return refreshes.size() > before; })) return false;
        }
        return true;
    }

    /// Types @p part into the first pane until @p most bytes went, or until
    /// it found no room @p tries times in a row, a pass of the stream each.
    /// The bytes that went.
    std::size_t fill(const std::string& part, std::size_t most, int tries)
    {
        std::size_t sent = 0;
        for (int stuck = 0; sent < most && stuck < tries;) {
            if (type(part)) {
                sent += part.size();
                stuck = 0;
            } else {
                ++stuck;
                stream->pump(20);
                read();
            }
        }
        return sent;
    }

    /// A frame of a command connection that the window serves, on
    /// Attach::CommandsChannel, which it opens on its first use.
    [[nodiscard]] bool relay(const Attach::RelayFrame& frame)
    {
        std::string why;
        if (!commands) commands = stream->openWriter(Attach::CommandsChannel, Attach::CommandsCodec, &why);
        if (!commands) {
            ADD_FAILURE() << "no commands channel: " << why;
            return false;
        }
        // A channel that the daemon has not taken yet has no credit: its
        // first frame waits for the daemon's answer to the open.
        const std::string encoded = Attach::encode(frame);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            const PhosphorChannel::WriteResult result = commands->tryWrite(encoded);
            if (result == PhosphorChannel::WriteResult::Written) return true;
            if (result != PhosphorChannel::WriteResult::WouldBlock || std::chrono::steady_clock::now() >= deadline) {
                ADD_FAILURE() << "the commands channel took no frame: " << int(result);
                return false;
            }
            stream->pump(20);
            read();
        }
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
        return send(message);
    }

    bool closedPane(std::uint32_t id) const { return std::find(closed.begin(), closed.end(), id) != closed.end(); }

    /// The window let go of pane @p id, which went to another session.
    [[nodiscard]] bool forget(std::uint32_t id)
    {
        forgotten.insert(id);
        Attach::Input message;
        message.type = Attach::InputType::PaneForgotten;
        message.pane = id;
        return send(message);
    }

    [[nodiscard]] bool action(Attach::InputType type)
    {
        Attach::Input message;
        message.type = type;
        message.pane = pane;
        return send(message);
    }

    /// A pane of the window after the first, with a replica of its own.
    struct OtherPane
    {
        explicit OtherPane(std::uint32_t channel)
            : channel(channel)
        {
        }
        std::uint32_t channel = 0;
        bool closed = false; ///< its channel ended
        int frames = 0;
        NullHost host;
        AfterglowVt::Terminal replica{20, 5, host};
        AfterglowVt::StateReader reader;
    };

    std::unique_ptr<IpcConnection> connection; ///< the attach call's, until the stream takes it
    std::int64_t serial = 0; ///< "window" of the attach answer
    std::unique_ptr<PhosphorChannel::Stream> stream;
    PhosphorChannel::StreamWriter* input = nullptr;
    PhosphorChannel::StreamWriter* actions = nullptr;
    PhosphorChannel::StreamWriter* commands = nullptr; ///< see relay()
    std::vector<Attach::RelayFrame> relays; ///< the Commands events, oldest first
    /// The session's first pane, which pane, channel, frames, screen() and
    /// screenAtExit are of, and which type() and resize() go to.
    std::uint32_t pane = 0;
    std::uint32_t channel = 0;
    bool channelClosed = false;
    bool ended = false;
    /// What the window showed once it had both halves of the first pane's exit.
    std::optional<std::string> screenAtExit;
    int frames = 0;
    int screenFrames = 0; ///< those of frames that changed the visible rows (StateReader::screenChanged)
    std::map<std::uint32_t, std::unique_ptr<OtherPane>> others; ///< the other panes, by ID
    std::vector<Attach::Event> events;
    std::vector<Layout> layouts; ///< the Layout events
    std::vector<std::uint32_t> opened; ///< the panes of PaneOpened, in order
    std::set<std::uint32_t> forgotten; ///< the panes that forget() let go, until they open again
    std::vector<std::uint32_t> closed; ///< the panes of PaneClosed, in order
    /// A Refreshed event: its pane, and the frames that it names.
    struct Refreshed
    {
        std::uint32_t pane = 0;
        std::uint64_t frames = 0;
    };
    std::vector<Refreshed> refreshes;
    std::vector<Host::Call> calls; ///< the host calls of the panes' terminals, oldest first
    std::vector<std::uint64_t> callSentAfter; ///< for each call, the frames that the daemon wrote before it
    std::string marker; ///< text whose first frame markerFrame counts
    int markerFrame = 0;
    std::string parts; ///< the parts of a call that came
    /// Off, the window reads no events: their channel gets no more credit,
    /// and the calls wait in the daemon.
    bool readEvents = true;
    /// The window that the stream grants each channel that it reads, 0 for
    /// the default: a small one makes the daemon's events wait sooner.
    std::size_t streamWindow = 0;
    bool answerReports = true;
    NullHost host;
    AfterglowVt::Terminal replica{20, 5, host};
    AfterglowVt::StateReader reader;
};

bool contains(const std::string& text, std::string_view part)
{
    return text.find(part) != std::string::npos;
}

/// The size of the panes of newPane().
constexpr PaneSize NewPaneSize{40, 10, 8, 16};

/// A pane that a window asks for in session @p name, with @p launch, at
/// NewPaneSize. Its ID, or nothing: why is in @p error, or in the daemon's
/// lastError() without one. An answer without the ID is a failure.
std::optional<std::uint32_t> newPane(TestDaemon& daemon, const std::string& name, Launch launch, IpcConnection::Error* error = nullptr)
{
    const auto result = daemon.call(Fn::NewPane, Json::Array{ProtocolVersion, name, toJson(launch), toJson(NewPaneSize)}, error);
    if (!result) return std::nullopt;
    const auto id = paneIdFrom((*result)["pane"]);
    if (!id) {
        ADD_FAILURE() << "the answer of newPane names no pane: " << Json::write(*result);
        if (error) *error = {"", "the answer names no pane"};
        return std::nullopt;
    }
    return *id;
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

// The answer to OSC 99's query follows the window's settings: none while
// the window shows no notifications (Notifications.enabled), as from a
// terminal without the protocol. DA1 after it shows that the query was read.
TEST(MuxAttach, TheNotificationQueryFollowsTheWindowsSettings)
{
    if (!pythonFound()) GTEST_SKIP() << "python3 is not installed";
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string answers = daemon.dir() + "/answers";
    SessionOptions o = daemon.options(ask("\\x1b]99;i=n:p=?;\\x1b\\\\\\x1b[c", "c", answers, true), "s");
    o.notificationCapabilities = "p=title";
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "go?"); })) << w.screen();
    SessionSettings settings{o.terminal, o.control};
    settings.notificationCapabilities.clear();
    ASSERT_TRUE(w.sendSettings(settings));
    // After the settings, on the same stream: the program asks now. The
    // window passes DA1's answer on (AReportFollowsTheCommandsBeforeIt).
    ASSERT_TRUE(w.type("x"));
    ASSERT_TRUE(w.until([&] { return exists(answers); })) << daemon.errors();
    EXPECT_TRUE(readFile(answers).starts_with("\x1b[?")) << readFile(answers);
    EXPECT_EQ(readFile(answers).find("99;"), std::string::npos) << readFile(answers);

    // Other capabilities of the window: the query gets those.
    const std::string changed = daemon.dir() + "/changed";
    SessionOptions other = daemon.options(ask("\\x1b]99;i=n:p=?;\\x1b\\\\\\x1b[c", "c", changed, true), "t");
    other.notificationCapabilities = "p=title";
    ASSERT_TRUE(daemon.newSession(other)) << daemon.lastError();
    Window next;
    ASSERT_EQ(next.attach(daemon, "t"), "");
    ASSERT_TRUE(next.until([&] { return next.pane != 0 && contains(next.screen(), "go?"); })) << next.screen();
    settings.notificationCapabilities = "p=body";
    ASSERT_TRUE(next.sendSettings(settings));
    ASSERT_TRUE(next.type("x"));
    ASSERT_TRUE(next.until([&] { return exists(changed); })) << daemon.errors();
    EXPECT_NE(readFile(changed).find("p=body"), std::string::npos) << readFile(changed);
    EXPECT_EQ(readFile(changed).find("p=title"), std::string::npos) << readFile(changed);
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
    // The first window's attachment finished: it still reads what the
    // window sent before it heard (Attachment::readLate), for up to
    // FinishWaitMs, but a Kill of a window that no longer shows the session
    // ends nothing. Only the window that shows the session can end it.
    ASSERT_TRUE(first.action(Attach::InputType::Kill));
    ASSERT_TRUE(first.stream->flush(2000));
    // The second window gets the whole screen, the first one's output too.
    ASSERT_TRUE(second.until([&] { return contains(second.screen(), "\nbefore"); })) << second.screen();
    // The daemon ended its output to the first window (first.ended above):
    // the second window's output cannot reach it.
    ASSERT_TRUE(second.type("echo after\n"));
    ASSERT_TRUE(second.until([&] { return contains(second.screen(), "\nafter"); }));
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
        // which goes with the daemon, and by a minute: a test that CTest
        // killed leaves its directory.
        ASSERT_TRUE(w.type("echo kept; (n=0; while [ -d '" + daemon.dir() + "' ] && [ ! -e '" + go
                           + "' ] && [ $n -lt 1200 ]; do sleep 0.05; n=$((n+1)); done; echo la''ter; : > '" + done + "') &\n"));
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
    // The window did not connect yet: the session counts as attached until
    // its connect wait ends.
    const auto info = sessionNamed(daemon, "sh");
    ASSERT_TRUE(info);
    EXPECT_TRUE(info->attached);
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
    EXPECT_EQ(keys, (std::vector<std::string>{"environment", "name", "window"}));
    EXPECT_EQ((*result)["name"].toString(), "keep");
    EXPECT_GT((*result)["window"].toInteger().value_or(0), 0);
    // The session's environment, as `afterglow mux new` sent it: the window
    // prepares the launches of new panes over it.
    SessionOptions o = daemon.options(Waiting, "env");
    o.environment = {{"FROM_SESSION", "1"}, {"EMPTY", ""}};
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    const auto answer = daemon.call(Fn::Attach, Json::Array{ProtocolVersion, "env", Json::Object{}}, &error);
    ASSERT_TRUE(answer) << error.message;
    std::string why;
    const auto environment = environmentFrom((*answer)["environment"], &why);
    ASSERT_TRUE(environment) << why;
    EXPECT_EQ(*environment, o.environment);
    // Each window gets a serial of its own.
    EXPECT_NE((*answer)["window"].toInteger(), (*result)["window"].toInteger());
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
    const std::size_t sent = w.fill(part, std::size_t(48) << 20, 200);
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
    // Bounded: a daemon that read on past held input would take it all.
    const std::size_t sent = w.fill(part, std::size_t(64) << 20, 100);
    EXPECT_GT(sent, std::size_t(32) << 20);
    // The PTY's 32 MiB, the input that the daemon holds (Attachment::
    // MaxHeldInput, 4 MiB), the channel's 4 MiB and the socket's buffers:
    // far below the bound unless the daemon read on.
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
        const auto deadline = made + std::chrono::milliseconds(Attach::StreamStartWaitMs + 5000);
        while (std::chrono::steady_clock::now() < deadline) {
            Window w;
            if (w.call(daemon, "sh", false).empty()) return true;
            std::this_thread::sleep_for(100ms);
        }
        return false;
    };
    ASSERT_TRUE(freed());
    EXPECT_GE(std::chrono::steady_clock::now() - made, std::chrono::milliseconds(Attach::StreamStartWaitMs));
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
    ASSERT_TRUE(w.show(w.pane, true));
    ASSERT_TRUE(w.until([&] { return reports(w).size() == 2; }));
    EXPECT_EQ(reports(w)[1], "\x1b[?999;1n");
    // No change, no report.
    ASSERT_TRUE(w.show(w.pane, true));
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
    ASSERT_TRUE(next.show(next.pane, true));
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
    ASSERT_TRUE(over.show(over.pane, true));
    ASSERT_TRUE(over.until([&] { return !reports(over).empty() && reports(over).back() == "\x1b[?999;1n"; }));
}

// A pane that the window does not show (a tab in the background) gets no
// screens: a program that floods it costs the window nothing to decode. What
// the window shows of the tab still comes (the title). Its host calls come
// after a frame with the text before them, its exit after the screen that the
// program left, and the screen catches up once the pane shows again.
TEST(MuxAttach, APaneThatTheWindowDoesNotShowGetsNoScreens)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "$ "); })) << w.screen();
    const auto show = [&](bool visible) { return w.show(w.pane, visible); };
    ASSERT_TRUE(show(false));
    ASSERT_TRUE(w.type("printf '\\033]2;hid''den\\007'\n"));
    ASSERT_TRUE(w.until([&] { return w.replica.title() == "hidden"; }));
    const std::string shown = w.screen();
    const std::size_t history = w.replica.primaryScreen().historySize();
    const int screenFrames = w.screenFrames;
    ASSERT_TRUE(w.type("i=0; while [ $i -lt 2000 ]; do echo flo''od $i; i=$((i+1)); done; printf '\\033]2;flo''oded\\007'\n"));
    ASSERT_TRUE(w.until([&] { return w.replica.title() == "flooded"; }, 30s));
    // The frames of the title and of the modes of the shell's prompt, which
    // change no rows.
    EXPECT_EQ(w.screenFrames, screenFrames);
    EXPECT_EQ(w.screen(), shown);
    EXPECT_EQ(w.replica.primaryScreen().historySize(), history);

    // Shown again: the screen as it is now, and the history that it missed.
    ASSERT_TRUE(show(true));
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "\nflood 1999 "); }, 30s)) << w.screen();
    EXPECT_GT(w.replica.primaryScreen().historySize(), history);

    // Hidden: a bell and a report come in the order that the program made
    // them, after a frame with the text before them.
    ASSERT_TRUE(show(false));
    w.answerReports = false;
    w.marker = "rang";
    ASSERT_TRUE(w.type("printf 'ra''ng\\a\\033[6n'\n"));
    ASSERT_TRUE(w.until([&] { return hasCall(w, Host::Kind::Report); }));
    const auto bell = std::find_if(w.calls.begin(), w.calls.end(), [](const Host::Call& c) { return c.kind == Host::Kind::Bell; });
    const auto report = std::find_if(w.calls.begin(), w.calls.end(), [](const Host::Call& c) { return c.kind == Host::Kind::Report; });
    ASSERT_NE(bell, w.calls.end());
    EXPECT_LT(bell, report);
    ASSERT_GT(w.markerFrame, 0);
    EXPECT_GE(w.callSentAfter[std::size_t(bell - w.calls.begin())], std::uint64_t(w.markerFrame));

    // A read of a hidden pane's text: the screen as it is now, then the
    // answer, which names the frames that hold it. The pane stays hidden.
    ASSERT_TRUE(w.type("i=0; while [ $i -lt 500 ]; do echo ag''ain $i; i=$((i+1)); done; printf '\\033]2;ag''ain\\007'\n"));
    ASSERT_TRUE(w.until([&] { return w.replica.title() == "again"; }, 30s));
    EXPECT_FALSE(contains(w.screen(), "again 499"));
    const int before = w.frames;
    ASSERT_TRUE(w.refresh(w.pane));
    ASSERT_TRUE(w.until([&] { return !w.refreshes.empty() && w.frames >= int(w.refreshes.back().frames); }));
    ASSERT_EQ(w.refreshes.size(), 1u);
    EXPECT_EQ(w.refreshes[0].pane, w.pane);
    EXPECT_GT(w.refreshes[0].frames, std::uint64_t(before));
    EXPECT_TRUE(contains(w.screen(), "\nagain 499 ")) << w.screen();
    // With the history that scrolled meanwhile. The read can come before
    // the shell's prompt, and then the last line ends the text.
    const std::string all = w.replica.text(AfterglowVt::Terminal::TextScope::All) + "\n";
    for (int i = 0; i < 500; ++i) EXPECT_TRUE(contains(all, "again " + std::to_string(i) + "\n")) << i;
    const std::string refreshed = w.screen();
    const int screenFrames2 = w.screenFrames;
    ASSERT_TRUE(w.type("echo af''ter; printf '\\033]2;af''ter\\007'\n"));
    ASSERT_TRUE(w.until([&] { return w.replica.title() == "after"; }));
    EXPECT_EQ(w.screenFrames, screenFrames2);
    EXPECT_EQ(w.screen(), refreshed);

    // The exit of a hidden pane: the window has the screen that the program
    // left before it learns of it.
    ASSERT_TRUE(w.type("echo good''bye; exit 3\n"));
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::SessionEnded) && w.screenAtExit; }));
    EXPECT_TRUE(contains(*w.screenAtExit, "\ngoodbye")) << *w.screenAtExit;
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
    // The window gets the transfer: TheDaemonDoesNotAnswerWhatTheWindowAnswers
    // checks that the daemon does not refuse it too.
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

namespace {

/// A program that waits for a line, then sends each of @p files as an iTerm2
/// download named got.bin, with its size, then prints @p tail and waits.
std::vector<std::string> downloads(const std::vector<std::string>& files, const std::string& tail)
{
    std::vector<std::string> argv{"/bin/sh", "-c",
                                  "read -r _; for f in \"$@\"; do printf '\\033]1337;File=name=Z290LmJpbg==;size=%s:' \"$(stat -c %s \"$f\")\"; "
                                  "base64 -w 0 \"$f\"; printf '\\007'; done; printf '" + tail + "'; exec cat",
                                  "sh"};
    argv.insert(argv.end(), files.begin(), files.end());
    return argv;
}

/// @p bytes of data that no two downloads share, in @p file.
std::string payloadIn(const std::string& file, std::size_t bytes, int seed)
{
    std::string payload(bytes, '\0');
    for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = char(i * 13 + std::size_t(seed));
    std::ofstream(file, std::ios::binary) << payload;
    return payload;
}

Attach::Input downloadAnswer(std::uint32_t pane, Attach::DownloadReply reply)
{
    Attach::Input answer;
    answer.type = Attach::InputType::DownloadAnswer;
    answer.pane = pane;
    answer.download = reply;
    return answer;
}

std::size_t countOf(const Window& w, Host::Kind kind)
{
    return std::size_t(std::count_if(w.calls.begin(), w.calls.end(), [kind](const Host::Call& c) { return c.kind == kind; }));
}

/// The place of the first call of @p kind in the window's calls, or their
/// count.
std::size_t firstOf(const Window& w, Host::Kind kind)
{
    return std::size_t(std::find_if(w.calls.begin(), w.calls.end(), [kind](const Host::Call& c) { return c.kind == kind; }) - w.calls.begin());
}

} // namespace

// An iTerm2 download in a session pane: the window asks, as for a pane of its
// own, while the program waits; then the data streams to the window as host
// calls, none dropped also past Mux.eventQueue, and ends whole. A window
// that reads slowly holds the program: the data waits for room, not in the
// daemon without a bound. The data never shows as text.
TEST(MuxAttach, ADownloadAsksTheWindowAndStreamsItsData)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string file = daemon.dir() + "/payload";
    const std::string payload = payloadIn(file, 3 << 20, 0);
    // The program sends once the window shows it (a line typed then): with
    // no window, the daemon refuses a download.
    ASSERT_TRUE(daemon.newSession(daemon.options(downloads({file}, "after"), "take"))) << daemon.lastError();
    Window w;
    // A small window of the stream, and the smallest queue, a third of the
    // file: the data waits for room.
    w.streamWindow = std::size_t(64) << 10;
    ASSERT_EQ(w.attach(daemon, "take", true, std::size_t(MinEventQueue)), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    ASSERT_TRUE(w.type("\r"));
    ASSERT_TRUE(w.until([&] { return hasCall(w, Host::Kind::DownloadBegin); })) << daemon.errors();
    const Host::Call& begin = w.calls[firstOf(w, Host::Kind::DownloadBegin)];
    EXPECT_EQ(begin.title, "got.bin");
    EXPECT_EQ(begin.value, payload.size() + 1);
    // The program waits for the answer: the daemon took nothing after the
    // download's head.
    ASSERT_TRUE(w.roundTrip());
    EXPECT_FALSE(hasCall(w, Host::Kind::DownloadData));
    EXPECT_FALSE(contains(w.screen(), "after"));
    // A window that reads no events: the data fills the channel and the
    // queue, and the program waits for room.
    w.readEvents = false;
    ASSERT_TRUE(w.send(downloadAnswer(w.pane, Attach::DownloadReply::Accept)));
    // A program that waits in its write shows only by what it does not
    // print: no state of the daemon tells it. A slow run can only pass here
    // wrongly, never fail.
    EXPECT_FALSE(w.until([&] { return contains(w.screen(), "after"); }, 1s)) << w.screen();
    w.readEvents = true;
    ASSERT_TRUE(w.until([&] { return hasCall(w, Host::Kind::DownloadEnd); }, 30s)) << daemon.errors();
    std::string received;
    for (const Host::Call& c : w.calls) {
        if (c.kind == Host::Kind::DownloadData) received += c.text;
    }
    EXPECT_EQ(w.calls[firstOf(w, Host::Kind::DownloadEnd)].number, 1);
    EXPECT_TRUE(received == payload) << received.size();
    // In their order: the question, the data, the end.
    EXPECT_LT(firstOf(w, Host::Kind::DownloadBegin), firstOf(w, Host::Kind::DownloadData));
    std::size_t lastData = 0;
    for (std::size_t i = 0; i < w.calls.size(); ++i) {
        if (w.calls[i].kind == Host::Kind::DownloadData) lastData = i;
    }
    EXPECT_LT(lastData, firstOf(w, Host::Kind::DownloadEnd));
    EXPECT_EQ(countOf(w, Host::Kind::DownloadEnd), 1u);
    EXPECT_TRUE(w.until([&] { return contains(w.screen(), "after"); })) << w.screen();
    EXPECT_FALSE(contains(w.screen(), AfterglowProto::base64Encode(payload.substr(0, 12)))) << w.screen();
}

// A refusal lets the program go on, and no data or end of the download
// comes, also a pass later.
TEST(MuxAttach, ARefusedDownloadSendsNothing)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string file = daemon.dir() + "/payload";
    const std::string payload = payloadIn(file, 64 << 10, 1);
    ASSERT_TRUE(daemon.newSession(daemon.options(downloads({file}, "refused"), "refuse"))) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "refuse"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    ASSERT_TRUE(w.type("\r"));
    ASSERT_TRUE(w.until([&] { return hasCall(w, Host::Kind::DownloadBegin); }));
    ASSERT_TRUE(w.send(downloadAnswer(w.pane, Attach::DownloadReply::Refuse)));
    EXPECT_TRUE(w.until([&] { return contains(w.screen(), "refused"); })) << w.screen();
    ASSERT_TRUE(w.roundTrip());
    EXPECT_FALSE(hasCall(w, Host::Kind::DownloadData));
    EXPECT_FALSE(hasCall(w, Host::Kind::DownloadEnd));
    EXPECT_FALSE(contains(w.screen(), AfterglowProto::base64Encode(payload.substr(0, 12)))) << w.screen();
}

// A stop ends only the download whose data streams. The window stops one
// when a write of its data fails: its rest goes nowhere, no end comes, and
// the program goes on. A stop that comes late, after the next download
// asked, stops nothing: that one still gets its answer.
TEST(MuxAttach, AStopEndsOnlyTheDownloadThatStreams)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string large = daemon.dir() + "/large";
    const std::string small = daemon.dir() + "/small";
    const std::string first = payloadIn(large, 3 << 20, 2);
    const std::string second = payloadIn(small, 4 << 10, 3);
    ASSERT_TRUE(daemon.newSession(daemon.options(downloads({large, small}, "after"), "stop"))) << daemon.lastError();
    Window w;
    w.streamWindow = std::size_t(64) << 10;
    ASSERT_EQ(w.attach(daemon, "stop", true, std::size_t(MinEventQueue)), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    ASSERT_TRUE(w.type("\r"));
    ASSERT_TRUE(w.until([&] { return hasCall(w, Host::Kind::DownloadBegin); }));
    ASSERT_TRUE(w.send(downloadAnswer(w.pane, Attach::DownloadReply::Accept)));
    ASSERT_TRUE(w.until([&] { return hasCall(w, Host::Kind::DownloadData); }, 30s));
    ASSERT_TRUE(w.send(downloadAnswer(w.pane, Attach::DownloadReply::Stop)));
    // The second download asks once the program sent the rest of the first.
    ASSERT_TRUE(w.until([&] { return countOf(w, Host::Kind::DownloadBegin) == 2; }, 30s)) << daemon.errors();
    std::string received;
    for (const Host::Call& c : w.calls) {
        if (c.kind == Host::Kind::DownloadData) received += c.text;
    }
    EXPECT_LT(received.size(), first.size());
    EXPECT_FALSE(hasCall(w, Host::Kind::DownloadEnd));
    // A stop of the first download that comes after the second asked.
    const std::size_t asked = w.calls.size();
    ASSERT_TRUE(w.send(downloadAnswer(w.pane, Attach::DownloadReply::Stop)));
    ASSERT_TRUE(w.send(downloadAnswer(w.pane, Attach::DownloadReply::Accept)));
    ASSERT_TRUE(w.until([&] { return hasCall(w, Host::Kind::DownloadEnd); }, 30s)) << daemon.errors();
    received.clear();
    for (std::size_t i = asked; i < w.calls.size(); ++i) {
        if (w.calls[i].kind == Host::Kind::DownloadData) received += w.calls[i].text;
    }
    EXPECT_EQ(w.calls[firstOf(w, Host::Kind::DownloadEnd)].number, 1);
    EXPECT_TRUE(received == second) << received.size();
    EXPECT_TRUE(w.until([&] { return contains(w.screen(), "after"); })) << w.screen();
}

// A download needs the window that it asked: a window that detaches while
// the download asks it refuses it, and the data of one that it took goes to
// no other window.
TEST(MuxAttach, ADownloadNeedsTheWindowThatItAsked)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string file = daemon.dir() + "/payload";
    payloadIn(file, 3 << 20, 4);

    // A window that detaches while the download asks it: refused.
    ASSERT_TRUE(daemon.newSession(daemon.options(downloads({file}, "after"), "asked"))) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "asked"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    ASSERT_TRUE(w.type("\r"));
    ASSERT_TRUE(w.until([&] { return hasCall(w, Host::Kind::DownloadBegin); }));
    ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"asked"})) << daemon.lastError();
    Window next;
    ASSERT_EQ(next.attach(daemon, "asked"), "");
    ASSERT_TRUE(next.until([&] { return next.pane != 0 && contains(next.screen(), "after"); }, 30s)) << next.screen();
    ASSERT_TRUE(next.roundTrip());
    EXPECT_FALSE(hasCall(next, Host::Kind::DownloadBegin));
    EXPECT_FALSE(hasCall(next, Host::Kind::DownloadData));

    // A window that another one takes over while the data streams to it:
    // the rest goes nowhere, and the program goes on.
    ASSERT_TRUE(daemon.newSession(daemon.options(downloads({file}, "after"), "taken"))) << daemon.lastError();
    Window first;
    first.streamWindow = std::size_t(64) << 10;
    ASSERT_EQ(first.attach(daemon, "taken", true, std::size_t(MinEventQueue)), "");
    ASSERT_TRUE(first.until([&] { return first.pane != 0; }));
    ASSERT_TRUE(first.type("\r"));
    ASSERT_TRUE(first.until([&] { return hasCall(first, Host::Kind::DownloadBegin); }));
    first.readEvents = false;
    ASSERT_TRUE(first.send(downloadAnswer(first.pane, Attach::DownloadReply::Accept)));
    // The data fills what the first window does not read: the program waits.
    // Only the absence of its next output shows it, as above.
    EXPECT_FALSE(first.until([&] { return contains(first.screen(), "after"); }, 1s)) << first.screen();
    Window second;
    ASSERT_EQ(second.attach(daemon, "taken"), "");
    ASSERT_TRUE(second.until([&] { return second.pane != 0 && contains(second.screen(), "after"); }, 30s)) << second.screen();
    ASSERT_TRUE(second.roundTrip());
    EXPECT_FALSE(hasCall(second, Host::Kind::DownloadData));
    EXPECT_FALSE(hasCall(second, Host::Kind::DownloadEnd));
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
    // Each query alone: a failure of the first does not skip the second.
    const auto answers = [](const std::string& query) {
        SCOPED_TRACE(query);
        TestDaemon daemon;
        ASSERT_TRUE(daemon.start()) << daemon.errors();
        const std::string answer = daemon.dir() + "/answer";
        ASSERT_TRUE(daemon.newSession(daemon.options(ask(query + "\\x1b[5n", "n", answer, true), "ask"))) << daemon.lastError();
        Window w;
        ASSERT_EQ(w.attach(daemon, "ask"), "");
        ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "go?"); })) << w.screen();
        ASSERT_TRUE(w.type("g"));
        ASSERT_TRUE(w.until([&] { return exists(answer); }));
        EXPECT_EQ(readFile(answer), "\x1b[0n");
    };
    for (const char* query : {"\\x1b]5113;ac=send;id=t1\\x1b\\\\", "\\x1b]72;t=q:i=3;\\x1b\\\\"}) answers(query);
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
    // A request that the daemon queued wrongly would come a pass later.
    ASSERT_TRUE(w.roundTrip());
    EXPECT_FALSE(hasCall(w, Host::Kind::ClipboardRequest));
}

// A request that waits for a window that detaches, in the daemon's queue or
// in the pane's, gets the answer of a session that no window shows, once.
TEST(MuxAttach, RequestsThatWaitWhenTheWindowDetachesGetTheAnswerWithoutAWindow)
{
    if (!pythonFound()) GTEST_SKIP() << "python3 is not installed";
    constexpr std::size_t mib = std::size_t(1) << 20;
    // Each size alone: a failure of the first does not skip the second.
    const auto detach = [](std::size_t queue) {
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

// The daemon decodes a PNG (kitty f=100) and iTerm2 images in a process of
// its own, and the window's replica gets their pixels, and the frames and
// delays of an animation.
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
        out << "\x1b]1337;File=inline=1;size=" << AfterglowImage::TestFiles::Gif.size() << ':'
            << AfterglowProto::base64Encode(AfterglowImage::TestFiles::Gif) << '\a';
    }
    ASSERT_TRUE(w.type("cat '" + images + "'; echo im''ages-shown\n"));
    const AfterglowVt::GraphicsStore& store = w.replica.graphics();
    // The iTerm2 images, sorted by size, the larger first: the JPEG (8 by
    // 8), the GIF (4 by 4), then the WebP (2 by 2).
    const auto iterm = [&] {
        std::vector<const AfterglowVt::Image*> found;
        for (const auto& [id, image] : store.images())
            if (image.isSixel) found.push_back(&image);
        std::sort(found.begin(), found.end(), [](const AfterglowVt::Image* a, const AfterglowVt::Image* b) {
            return a->frames.at(0)->width > b->frames.at(0)->width;
        });
        return found;
    };
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "images-shown") && store.images().contains(7) && iterm().size() == 3; }))
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
    // Two frames of 100 ms: red, then blue.
    const AfterglowVt::Image& gif = *iterm()[1];
    ASSERT_EQ(gif.frames.size(), 2u);
    EXPECT_EQ(gif.gaps, (std::vector<std::uint32_t>{100, 100}));
    EXPECT_EQ(gif.frames[0]->width, 4u);
    EXPECT_EQ(gif.frames[1]->height, 4u);
    EXPECT_TRUE(gif.frames[0]->rgba == AfterglowImage::TestFiles::repeat({255, 0, 0, 255}, 16));
    EXPECT_TRUE(gif.frames[1]->rgba == AfterglowImage::TestFiles::repeat({0, 0, 255, 255}, 16));
    const AfterglowVt::Image& webp = *iterm()[2];
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
    EXPECT_GE(w.callSentAfter[bell], std::uint64_t(w.markerFrame));
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
        // Printed once the window went: the test says when. Bounded by the
        // test's directory, which goes with the daemon, and by a minute: a
        // test that CTest killed leaves its directory.
        ASSERT_TRUE(w.type("(n=0; while [ -d '" + daemon.dir() + "' ] && [ ! -e '" + go + "' ] && [ $n -lt 1200 ]; do sleep 0.05; n=$((n+1)); done; cat '"
                           + image + "'; : > '" + done + "') &\n"));
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
// session's environment and the changes of its own launch, at the size that
// the window asked for. Its program prints nothing, and the window gets the
// pane and its first frame all the same: the daemon sends what a call
// changed after it answered the call, not at its next wake. The call's
// connection stays open until the frame came, so that its close is no wake.
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
    const std::string size = daemon.dir() + "/size";
    // The size first: the environment's file says that both are there.
    Launch launch = launchOf({"/bin/sh", "-c", "stty size > '" + size + "'; env > '" + out + ".part' && mv '" + out + ".part' '" + out + "'; exec sleep 60"},
                             daemon);
    launch.environment.emplace_back("FROM_PANE", "2");
    launch.unset.push_back("TMUX");
    launch.launchedProgram = "sleeper";
    // The answer has the ID only.
    IpcConnection connection;
    IpcConnection::Error error;
    ASSERT_TRUE(connection.connect(daemon.socket(), &error, 2000)) << error.message;
    const auto answer = connection.call(Target, Fn::NewPane, Json::Array{ProtocolVersion, "s", toJson(launch), toJson(NewPaneSize)}, 5000, &error);
    ASSERT_TRUE(answer) << error.code << ": " << error.message << " " << daemon.errors();
    std::vector<std::string> keys;
    for (const auto& [key, value] : answer->toObject()) keys.push_back(key);
    EXPECT_EQ(keys, std::vector<std::string>{"pane"});
    const auto id = paneIdFrom((*answer)["pane"]);
    ASSERT_TRUE(id) << Json::write(*answer);
    ASSERT_TRUE(waitFor([&] { return !readFile(out).empty(); }));
    EXPECT_EQ(readFile(size), std::to_string(NewPaneSize.rows) + " " + std::to_string(NewPaneSize.cols) + "\n");
    const std::string env = "\n" + readFile(out);
    EXPECT_NE(env.find("\nFROM_SESSION=1\n"), std::string::npos) << env;
    EXPECT_NE(env.find("\nFROM_PANE=2\n"), std::string::npos) << env;
    EXPECT_EQ(env.find("\nTMUX="), std::string::npos) << env;
    EXPECT_EQ(env.find("\nFIRST_PANE_ONLY="), std::string::npos) << env;
    EXPECT_NE(env.find("\nAFTERGLOW_SESSION=s\n"), std::string::npos) << env;
    EXPECT_NE(env.find("\nAFTERGLOW_PANE_ID=" + std::to_string(*id) + "\n"), std::string::npos) << env;
    // The window that shows the session gets the pane and its first frame,
    // at its size, though the program prints nothing.
    ASSERT_TRUE(w.until([&] { return w.framesOf(*id) > 0; })) << daemon.errors();
    EXPECT_EQ(std::count(w.opened.begin(), w.opened.end(), *id), 1);
    EXPECT_EQ(w.others.at(*id)->replica.cols(), NewPaneSize.cols);
    EXPECT_EQ(w.others.at(*id)->replica.rows(), NewPaneSize.rows);
    const auto info = sessionNamed(daemon, "s");
    ASSERT_TRUE(info);
    EXPECT_EQ(info->panes, 2);
    // Its job, for the question before the window closes it: the program of
    // its launch, which runs in the foreground.
    const auto job = daemon.call(Fn::PaneJob, Json::Array{"s", std::int64_t(*id)});
    ASSERT_TRUE(job) << daemon.lastError();
    EXPECT_EQ(job->toString(), "sleeper");
}

TEST(MuxAttach, NewPanePaneJobAndPaneSessionRefuseBadCalls)
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
    EXPECT_EQ(sessionNamed(daemon, "s").value_or(SessionInfo{}).panes, 1);
    // The pane of another session is none of this one.
    const std::uint32_t kept = firstPaneOf(daemon, "keep");
    ASSERT_NE(kept, 0u);
    const std::pair<Json::Array, std::string> badJob[] = {
        {Json::Array{"s", 999}, Error::NoSuchPane},
        {Json::Array{"s", std::int64_t(kept)}, Error::NoSuchPane},
        {Json::Array{"none", 1}, Error::NoSuchSession},
        {Json::Array{"s"}, IpcErrorCode::InvalidArg},
        {Json::Array{"s", 1, 1}, IpcErrorCode::InvalidArg},
        {Json::Array{1, 1}, IpcErrorCode::InvalidArg},
        {Json::Array{"s", "1"}, IpcErrorCode::InvalidArg},
        {Json::Array{"s", 0}, IpcErrorCode::InvalidArg},
        {Json::Array{"s", std::int64_t(1) << 32}, IpcErrorCode::InvalidArg},
    };
    for (const auto& [args, code] : badJob) {
        IpcConnection::Error error;
        EXPECT_FALSE(daemon.call(Fn::PaneJob, args, &error)) << Json::write(args);
        EXPECT_EQ(error.code, code) << Json::write(args) << ": " << error.message;
    }
    const std::pair<Json::Array, std::string> badSession[] = {
        {Json::Array{999}, Error::NoSuchPane},
        {Json::Array{}, IpcErrorCode::InvalidArg},
        {Json::Array{1, 1}, IpcErrorCode::InvalidArg},
        {Json::Array{"1"}, IpcErrorCode::InvalidArg},
        {Json::Array{0}, IpcErrorCode::InvalidArg},
        {Json::Array{-1}, IpcErrorCode::InvalidArg},
        {Json::Array{1.5}, IpcErrorCode::InvalidArg},
    };
    for (const auto& [args, code] : badSession) {
        IpcConnection::Error error;
        EXPECT_FALSE(daemon.call(Fn::PaneSession, args, &error)) << Json::write(args);
        EXPECT_EQ(error.code, code) << Json::write(args) << ": " << error.message;
    }
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
        // No wait for the daemon to read it: the layout is in the socket
        // once sendLayout() returns (a small frame goes at once), and the
        // detach reads what the window sent before the call
        // (Attachment::catchUp) before it ends the window's stream.
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

// A layout that names a pane that the session does not have (as one does
// that a pane's exit passed: 9999 stands for that pane), and that came
// before a new pane: the daemon keeps the panes of the session, each once.
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
    ASSERT_TRUE(waitFor([&] {
        const auto info = sessionNamed(daemon, "s");
        return info && !info->attached;
    }));
    EXPECT_NE(daemon.errors().find("a layout that is not valid"), std::string::npos) << daemon.errors();
}

// The user closes a pane, and the window stays: that program ends, the others
// run on. Closing the last pane ends the session.
TEST(MuxAttach, ClosingAPaneEndsItsProgramAndTheLastOneTheSession)
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
    EXPECT_TRUE(waitFor([&] { return !running(program); }));
    // Its PaneClosed has no status: the window closed it.
    for (const Attach::Event& e : w.events) {
        if (e.type == Attach::EventType::PaneClosed) {
            EXPECT_EQ(e.status, -1);
        }
    }
    EXPECT_EQ(sessionNamed(daemon, "s").value_or(SessionInfo{}).panes, 1);
    EXPECT_FALSE(w.got(Attach::EventType::SessionEnded));
    // A pane that is gone already: the window keeps the session, and the
    // daemon ends nothing. A close that acted on another pane would show
    // only in that pane's session (AWindowChangesNoPaneOfAnotherSession).
    ASSERT_TRUE(w.closePane(*second));
    ASSERT_TRUE(w.roundTrip());
    EXPECT_FALSE(w.got(Attach::EventType::SessionEnded));
    EXPECT_FALSE(w.ended);
    EXPECT_TRUE(sessionNamed(daemon, "s"));
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
    ASSERT_TRUE(waitFor([&] { return sessionNamed(daemon, "s").value_or(SessionInfo{}).panes == 1; }));
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
    // A session without a name takes the next number: the new one takes the
    // number after it.
    const auto numbered = daemon.newSession(daemon.options(Waiting));
    ASSERT_TRUE(numbered) << daemon.lastError();
    const std::string number = (*numbered)["name"].toString();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    const std::string pidFile = daemon.dir() + "/pid";
    const std::string movedEnvironment = daemon.dir() + "/moved-env";
    const auto second =
        newPane(daemon, "s", launchOf({"/bin/sh", "-c", "env > '" + movedEnvironment + "'; echo $$ > '" + pidFile + "'; exec sleep 60"}, daemon));
    const auto third = newPane(daemon, "s", launchOf(Waiting, daemon));
    ASSERT_TRUE(second && third) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return readFile(pidFile).find('\n') != std::string::npos; }));
    const pid_t program = pid_t(std::strtol(readFile(pidFile).c_str(), nullptr, 10));
    ASSERT_GT(program, 0);
    ASSERT_TRUE(w.until([&] { return w.opened.size() == 3; }));

    Layout moved;
    moved.tabs.push_back(LayoutTab{{LayoutNode{0, true, 0.4}, LayoutNode{*second}, LayoutNode{*third}}, *third, 0, "build"});
    const auto result = daemon.call(Fn::BreakOut, Json::Array{ProtocolVersion, "s", toJson(moved)});
    ASSERT_TRUE(result) << daemon.lastError();
    // The answer has the new session's name only.
    std::vector<std::string> keys;
    for (const auto& [key, value] : result->toObject()) keys.push_back(key);
    EXPECT_EQ(keys, std::vector<std::string>{"name"});
    const std::string fresh = (*result)["name"].toString();
    ASSERT_TRUE(validSessionName(fresh)) << fresh;
    EXPECT_EQ(fresh, std::to_string(std::stoi(number) + 1));
    // The window of the first session no longer shows them, and the session
    // keeps its first pane.
    ASSERT_TRUE(w.until([&] { return w.closedPane(*second) && w.closedPane(*third); }));
    EXPECT_FALSE(w.got(Attach::EventType::SessionEnded));
    EXPECT_EQ(sessionNamed(daemon, "s").value_or(SessionInfo{}).panes, 1);
    ASSERT_TRUE(sessionNamed(daemon, fresh));
    EXPECT_EQ(sessionNamed(daemon, fresh).value_or(SessionInfo{}).panes, 2);
    EXPECT_TRUE(running(program));
    // A window of the new session gets their layout.
    Window other;
    ASSERT_EQ(other.attach(daemon, fresh), "");
    ASSERT_TRUE(other.until([&] { return other.opened.size() == 2; }));
    ASSERT_EQ(other.layouts.size(), 1u);
    EXPECT_TRUE(other.layouts[0] == moved) << Json::write(toJson(other.layouts[0]));
    // Each pane names the session that holds it now: its AFTERGLOW_SESSION
    // still names the first one, which its program got when it started.
    const auto holder = daemon.call(Fn::PaneSession, Json::Array{std::int64_t(*second)});
    ASSERT_TRUE(holder) << daemon.lastError();
    EXPECT_EQ(holder->toString(), fresh);
    const auto own = daemon.call(Fn::PaneSession, Json::Array{std::int64_t(w.pane)});
    ASSERT_TRUE(own) << daemon.lastError();
    EXPECT_EQ(own->toString(), "s");
    EXPECT_NE(("\n" + readFile(movedEnvironment)).find("\nAFTERGLOW_SESSION=s\n"), std::string::npos) << readFile(movedEnvironment);
    // A new pane of the new session has the environment of the first one,
    // and the name of the new session.
    const std::string out = daemon.dir() + "/env";
    ASSERT_TRUE(newPane(daemon, fresh, launchOf(writesEnvironment(out), daemon))) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return !readFile(out).empty(); }));
    const std::string env = "\n" + readFile(out);
    EXPECT_NE(env.find("\nFROM_SESSION=1\n"), std::string::npos) << env;
    EXPECT_NE(env.find("\nAFTERGLOW_SESSION=" + fresh + "\n"), std::string::npos) << env;
}

TEST(MuxAttach, BreakOutRefusesBadCalls)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    const std::uint32_t first = firstPaneOf(daemon, "s");
    ASSERT_NE(first, 0u);
    const auto second = newPane(daemon, "s", launchOf(Waiting, daemon));
    ASSERT_TRUE(second) << daemon.lastError();
    const std::uint32_t kept = firstPaneOf(daemon, "keep");
    ASSERT_NE(kept, 0u);
    const std::vector<std::string> before = names(daemon.sessions());
    Layout alone;
    alone.addTab(*second);
    Layout unknown;
    unknown.addTab(9999);
    Layout other;
    other.addTab(kept);
    Layout all;
    all.addTab(*second);
    all.addTab(first);
    const std::pair<Json::Array, std::string> bad[] = {
        {Json::Array{ProtocolVersion, "none", toJson(alone)}, Error::NoSuchSession},
        {Json::Array{ProtocolVersion - 1, "s", toJson(alone)}, Error::VersionMismatch},
        {Json::Array{ProtocolVersion, "s", Json::Object{}}, IpcErrorCode::InvalidArg},
        {Json::Array{ProtocolVersion, "s"}, IpcErrorCode::InvalidArg},
        {Json::Array{ProtocolVersion, 1, toJson(alone)}, IpcErrorCode::InvalidArg},
        {Json::Array{ProtocolVersion, "s", toJson(unknown)}, Error::NoSuchPane},
        // The pane of another session.
        {Json::Array{ProtocolVersion, "s", toJson(other)}, Error::NoSuchPane},
        // Every pane of the session: it would only get another name.
        {Json::Array{ProtocolVersion, "s", toJson(all)}, IpcErrorCode::InvalidArg},
    };
    for (const auto& [args, code] : bad) {
        IpcConnection::Error error;
        EXPECT_FALSE(daemon.call(Fn::BreakOut, args, &error)) << Json::write(args);
        EXPECT_EQ(error.code, code) << Json::write(args) << ": " << error.message;
    }
    EXPECT_EQ(sessionNamed(daemon, "s").value_or(SessionInfo{}).panes, 2);
    EXPECT_EQ(sessionNamed(daemon, "keep").value_or(SessionInfo{}).panes, 1);
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
    SessionOptions o = shellOptions(daemon, name);
    o.control.allowReadText = readText;
    return bool(daemon.newSession(o));
}

std::vector<std::uint32_t> paneIds(const Json::Value& panes)
{
    std::vector<std::uint32_t> ids;
    for (const Json::Value& pane : panes.toArray()) ids.push_back(std::uint32_t(pane["id"].toInteger().value_or(0)));
    return ids;
}

/// The ID of the first pane that listPanes names, 0 with a failure when it
/// names none.
std::uint32_t firstPane(ControlClient& ctl)
{
    const auto listed = ctl.call("listPanes", {});
    if (!listed || !listed->isArray() || listed->toArray().empty()) {
        ADD_FAILURE() << "listPanes names no pane: " << (listed ? Json::write(*listed) : ctl.error.message);
        return 0;
    }
    return paneIds(*listed).front();
}

/// Waits until the screen of pane @p pane, as getText reads it, holds
/// @p part, up to @p wait.
bool screenShows(ControlClient& ctl, std::int64_t pane, const std::string& part, std::chrono::milliseconds wait = 5s)
{
    return waitFor(
        [&] {
            const auto text = ctl.call("getText", Json::Array{pane, "screen"});
            return text && contains(text->toString(), part);
        },
        wait);
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
    // The arguments of a selection.
    for (const auto& [fn, args] : std::vector<std::pair<std::string, Json::Array>>{
             {Fn::SelectSession, Json::Array{ProtocolVersion}},
             {Fn::SelectSession, Json::Array{ProtocolVersion, 1}},
             {Fn::SelectSession, Json::Array{ProtocolVersion, "s", 1}},
             {Fn::SelectPane, Json::Array{ProtocolVersion}},
             {Fn::SelectPane, Json::Array{ProtocolVersion, "1"}},
             {Fn::SelectPane, Json::Array{ProtocolVersion, -1}}}) {
        SCOPED_TRACE(fn + " " + Json::write(Json::Value(args)));
        EXPECT_FALSE(ctl.call(fn, args, Target));
        EXPECT_EQ(ctl.error.code, IpcErrorCode::InvalidArg) << ctl.error.message;
    }
    // By a pane: the session that holds it.
    const std::uint32_t pane = firstPaneOf(daemon, "s");
    ASSERT_NE(pane, 0u);
    const auto selected = ctl.call(Fn::SelectPane, Json::Array{ProtocolVersion, std::int64_t(pane)}, Target);
    ASSERT_TRUE(selected) << ctl.error.message;
    EXPECT_EQ((*selected)["name"].toString(), "s");
    // `afterglow ssh` asks for remote commands: a caller that runs in no
    // pane of the daemon has none.
    const auto enabled = ctl.call("remoteCommandsEnabled", {});
    ASSERT_TRUE(enabled) << ctl.error.message;
    EXPECT_FALSE(enabled->toBool());
    // The selection is of the session, not of its name: once it ended, a
    // new session of that name is another one.
    ASSERT_TRUE(daemon.call(Fn::KillSession, Json::Array{"s"})) << daemon.lastError();
    EXPECT_FALSE(ctl.call("listPanes", {}));
    EXPECT_EQ(ctl.error.code, Error::NoSuchSession);
    EXPECT_EQ(ctl.error.message, "session 's' ended");
    ASSERT_TRUE(readableShell(daemon, "s")) << daemon.lastError();
    EXPECT_FALSE(ctl.call("listPanes", {}));
    EXPECT_EQ(ctl.error.code, Error::NoSuchSession);
    EXPECT_EQ(ctl.error.message, "session 's' ended");
}

// The answer of a selection has the session's environment, over which
// `afterglow ctl` prepares the launch of a split or a new tab.
TEST(MuxControl, ASelectionAnswersWithTheSessionsEnvironment)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    SessionOptions o = daemon.options(Waiting, "e");
    o.environment.emplace_back("A", "1");
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    ControlClient ctl(daemon);
    const auto answers = [&](const std::optional<Json::Value>& answer) {
        ASSERT_TRUE(answer) << ctl.error.message;
        EXPECT_EQ((*answer)["name"].toString(), "e");
        std::string why;
        const auto environment = environmentFrom((*answer)["environment"], &why);
        ASSERT_TRUE(environment) << why;
        EXPECT_EQ(*environment, o.environment);
    };
    answers(ctl.call(Fn::SelectSession, Json::Array{ProtocolVersion, "e"}, Target));
    const std::uint32_t pane = firstPaneOf(daemon, "e");
    ASSERT_NE(pane, 0u);
    answers(ctl.call(Fn::SelectPane, Json::Array{ProtocolVersion, std::int64_t(pane)}, Target));
}

namespace {

/// A process that `afterglow ssh` plays: its socket ends with it. -1 when
/// fork() failed.
pid_t sleeper()
{
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::execl("/bin/sleep", "sleep", "60", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    return pid;
}

/// Kills process @p pid, and reaps it when it is a child of the test.
/// Nothing for a pid of no single process: kill(-1) would end every process
/// of the user, and kill(0) the test's process group.
void endProcess(pid_t pid)
{
    if (pid <= 0) {
        ADD_FAILURE() << "no process " << pid << " to end";
        return;
    }
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
    /// Sends @p line to the agent. False when the agent is gone, or took
    /// no line in five seconds: it is lost then.
    bool send(const std::string& line)
    {
        if (lost) return false;
        // Without blocking: an agent that ended fails the test, not hangs it.
        int fd = -1;
        if (!waitFor([&] { return (fd = ::open(input.c_str(), O_WRONLY | O_NONBLOCK | O_CLOEXEC)) >= 0; })) return lose();
        // An agent that ended between the open and the write raises
        // SIGPIPE, which would end the test: it waits blocked, and is taken
        // below.
        sigset_t pipe;
        ::sigemptyset(&pipe);
        ::sigaddset(&pipe, SIGPIPE);
        sigset_t old;
        ::pthread_sigmask(SIG_BLOCK, &pipe, &old);
        const std::string text = line + '\n';
        std::size_t written = 0;
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (written < text.size() && std::chrono::steady_clock::now() < deadline) {
            const ssize_t n = ::write(fd, text.data() + written, text.size() - written);
            if (n > 0) {
                written += std::size_t(n);
            } else if (n < 0 && errno == EAGAIN) {
                pollfd room{fd, POLLOUT, 0};
                ::poll(&room, 1, 100);
            } else if (n >= 0 || errno != EINTR) {
                break;
            }
        }
        ::close(fd);
        const timespec now{0, 0};
        while (::sigtimedwait(&pipe, nullptr, &now) == SIGPIPE) {
        }
        ::pthread_sigmask(SIG_SETMASK, &old, nullptr);
        return written == text.size() || lose();
    }
    /// The next answer of the agent, within five seconds. One that does not
    /// come loses the agent: a late answer would pass for the next one's.
    std::string next()
    {
        if (lost) return {};
        std::string answer;
        const bool came = waitFor([&] {
            const std::string all = readFile(output);
            std::size_t at = 0;
            for (std::size_t i = 0; i < answered; ++i) {
                const std::size_t end = all.find('\n', at);
                if (end == std::string::npos) return false;
                at = end + 1;
            }
            const std::size_t end = all.find('\n', at);
            if (end == std::string::npos) return false;
            answer = all.substr(at, end - at);
            return true;
        });
        if (!came) {
            lose();
            return {};
        }
        ++answered;
        return answer;
    }
    /// The next answer, after @p line went to the agent.
    std::string ask(const std::string& line) { return send(line) ? next() : std::string(); }
    /// A process in the pane, which plays `afterglow ssh`. 0 without one.
    pid_t spawn() { return pid_t(std::strtol(ask("spawn").c_str(), nullptr, 10)); }
    /// The line of a call of the control target from the pane.
    std::string callLine(const std::string& fn, Json::Array args, const std::string& target = "control")
    {
        return Json::write(Json::Object{{"type", "call"}, {"id", std::int64_t(++calls)}, {"target", target}, {"fn", fn}, {"args", std::move(args)}});
    }
    /// A call of the control target from the pane, with its error message.
    std::optional<Json::Value> call(const std::string& fn, Json::Array args)
    {
        error.clear();
        const auto reply = Json::parse(ask(callLine(fn, std::move(args))));
        if (!reply) {
            error = "no answer";
            return std::nullopt;
        }
        if (const Json::Value* result = reply->find("result")) return *result;
        error = (*reply)["message"].toString();
        if (const Json::Value* inner = reply->find("error")) error = (*inner)["message"].toString();
        return std::nullopt;
    }
    bool lose()
    {
        lost = true;
        return false;
    }
    std::string input;
    std::string output;
    bool started = false;
    bool lost = false; ///< an answer did not come: no later one is trusted
    std::size_t answered = 0;
    std::int64_t calls = 0;
    std::string error;
};

/// An executable script @p name in @p directory that runs @p body. Empty
/// where the directory runs no program (a file system mounted noexec):
/// PATH would then find the user's own program of that name.
std::string script(const std::string& directory, const std::string& name, const std::string& body)
{
    const std::string file = directory + "/" + name;
    std::ofstream(file) << "#!/bin/sh\n" << body << "\n";
    return ::chmod(file.c_str(), 0700) == 0 && ::access(file.c_str(), X_OK) == 0 ? file : std::string();
}

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
    if (script(bin, "ssh", "printf '%s\\n' \"$*\" >> '" + log + "'").empty()) GTEST_SKIP() << bin << " runs no program";
    const char* path = std::getenv("PATH");
    daemon.daemonOnly("PATH", bin + ":" + (path ? path : "/usr/bin:/bin"));
    // The socket of an ssh session that a killed daemon of this host left
    // goes. Its name has the ID of the host, as its state socket has: that
    // of another host whose home is the same stays.
    std::string host = AfterglowProto::muxStateSocketName();
    if (host.ends_with(".sock")) host.resize(host.size() - 5);
    const std::string directory = daemon.runtime() + "/aglow-mux";
    const std::string stale = directory + "/remote-" + host + "-0123456789abcdef.sock";
    const std::string elsewhere = directory + "/remote-" + std::string(32, '0') + "-0123456789abcdef.sock";
    ASSERT_NE(host, std::string(32, '0'));
    ASSERT_EQ(::mkdir(directory.c_str(), 0700), 0);
    std::ofstream(stale).put('x');
    std::ofstream(elsewhere).put('x');
    ASSERT_TRUE(exists(stale));
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    EXPECT_FALSE(exists(stale));
    EXPECT_FALSE(exists(stale + ".lock"));
    EXPECT_TRUE(exists(elsewhere));
    PaneAgent agent(daemon, "s");
    ASSERT_TRUE(agent.started) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("s"), "");
    const std::uint32_t pane = firstPane(ctl);
    ASSERT_NE(pane, 0u);
    // `afterglow ssh` in the pane asks first: the client of the session
    // allowed remote commands.
    const auto enabled = agent.call("remoteCommandsEnabled", {});
    ASSERT_TRUE(enabled) << agent.error;
    EXPECT_TRUE(enabled->toBool());
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
    ASSERT_GT(outside, 0) << "fork failed";
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
    EXPECT_EQ(socket, directory + "/remote-" + host + "-" + token + ".sock");
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
    // A token of no session.
    EXPECT_FALSE(ctl.call("remoteSocketForwarded", Json::Array{"0000000000000000"}));
    EXPECT_EQ(ctl.error.message, "no remote socket 0000000000000000: its session has ended");
    EXPECT_FALSE(ctl.call("remoteSocketFailed", Json::Array{"0000000000000000", "forward"}));
    EXPECT_EQ(ctl.error.message, "no remote socket 0000000000000000");

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
    const auto openWith = [&](Json::Array ssh, const std::string& destination) {
        return agent.call("openRemoteSocket", Json::Array{std::int64_t(pane), std::int64_t(process), "box", "/run/user/1000/afterglow",
                                                          AfterglowProto::ProtocolVersion, std::move(ssh), destination, false});
    };
    // A destination that ssh would take for an option, and too many options.
    refuses(openWith(Json::Array{"-F", "none"}, "-oProxyCommand=x"), "the ssh destination must not be empty or start with '-'");
    refuses(openWith(Json::Array{"-F", "none"}, ""), "the ssh destination must not be empty or start with '-'");
    refuses(openWith(Json::Array(AfterglowProto::Remote::MaxOptions + 1, Json::Value("-v")), "prod"),
            "ssh takes at most " + std::to_string(AfterglowProto::Remote::MaxOptions) + " options here");
    refuses(openWith(Json::Array{"-F", 1}, "prod"), "the ssh options are text");
    // At most Remote::MaxSessionsPerPane sessions of one pane: the three
    // above (one socket, and two answers without one), and more up to it.
    std::size_t more = 0;
    while (more < AfterglowProto::Remote::MaxSessionsPerPane && open(AfterglowProto::ProtocolVersion)) ++more;
    EXPECT_EQ(more, AfterglowProto::Remote::MaxSessionsPerPane - 3);
    EXPECT_EQ(agent.error,
              "pane " + std::to_string(pane) + " has " + std::to_string(AfterglowProto::Remote::MaxSessionsPerPane) + " remote sessions already");

    // The sockets end with the process of `afterglow ssh`, and their
    // forwards are cancelled with the options that reach the shared
    // connection. The process is the agent's, and still runs.
    endProcess(process);
    EXPECT_TRUE(waitFor([&] { return !exists(socket); }));
    const std::string cancel = "-F none -S /cm -O cancel -R " + remote + ":" + socket + " -- prod\n";
    EXPECT_TRUE(waitFor([&] { return readFile(log).find(cancel) != std::string::npos; })) << readFile(log);
    EXPECT_FALSE(ctl.call("remoteSocketForwarded", Json::Array{token}));
    EXPECT_EQ(ctl.error.message, "no remote socket " + token + ": its session has ended");

    // A forward that failed: the socket closes, and nothing is cancelled.
    process = agent.spawn();
    ASSERT_GT(process, 0);
    const auto failing = open(AfterglowProto::ProtocolVersion);
    ASSERT_TRUE(failing) << agent.error;
    const std::string failed = (*failing)["socket"].toString();
    EXPECT_FALSE(ctl.call("remoteSocketFailed", Json::Array{(*failing)["token"].toString(), "other"}));
    EXPECT_EQ(ctl.error.message, "the reason of a failed forward is forward");
    EXPECT_TRUE(ctl.call("remoteSocketFailed", Json::Array{(*failing)["token"].toString(), "forward"})) << ctl.error.message;
    EXPECT_FALSE(exists(failed));
    // A window turns remote commands off: the sessions end, and a new one
    // gets the reason, as `afterglow ssh` does when it asks. On again, a
    // new one opens.
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
        const auto off = agent.call("remoteCommandsEnabled", {});
        ASSERT_TRUE(off) << agent.error;
        EXPECT_FALSE(off->toBool());
        EXPECT_FALSE(open(AfterglowProto::ProtocolVersion));
        EXPECT_EQ(agent.error, "remote commands are off (Terminal.sshRemoteCommands)");
        settings.control.remoteCommandsOff.clear();
        ASSERT_TRUE(w.sendSettings(settings));
        EXPECT_TRUE(w.until([&] { return bool(open(AfterglowProto::ProtocolVersion)); })) << agent.error;
        const auto on = agent.call("remoteCommandsEnabled", {});
        ASSERT_TRUE(on) << agent.error;
        EXPECT_TRUE(on->toBool());
    }
    // The pane closes: its sessions end, and so does the sleep of the agent,
    // which the hangup of the pane ends.
    const auto closing = open(AfterglowProto::ProtocolVersion);
    ASSERT_TRUE(closing) << agent.error;
    const std::string closed = (*closing)["socket"].toString();
    ASSERT_TRUE(ctl.call("close", Json::Array{std::int64_t(pane)})) << ctl.error.message;
    EXPECT_TRUE(waitFor([&] { return !exists(closed); }));
    EXPECT_TRUE(waitFor([&] { return readFile(log).find(closed) != std::string::npos; })) << readFile(log);
    EXPECT_EQ(readFile(log).find(failed), std::string::npos) << readFile(log);

    // The end of the daemon cancels the forwards of the sessions that remain.
    PaneAgent last(daemon, "t");
    ASSERT_TRUE(last.started) << daemon.lastError();
    const std::uint32_t lastPane = firstPaneOf(daemon, "t");
    ASSERT_NE(lastPane, 0u);
    const pid_t ssh = last.spawn();
    ASSERT_GT(ssh, 0);
    const auto kept = last.call("openRemoteSocket", Json::Array{std::int64_t(lastPane), std::int64_t(ssh), "box", "/run/user/1000/afterglow",
                                                                AfterglowProto::ProtocolVersion, Json::Array{"-S", "/end"}, "prod", false});
    ASSERT_TRUE(kept) << last.error;
    ASSERT_EQ(::kill(daemon.pid(), SIGTERM), 0);
    EXPECT_EQ(daemon.waitExit(), 128 + SIGTERM) << daemon.errors();
    const std::string ended = "-S /end -O cancel -R " + (*kept)["path"].toString() + ":" + (*kept)["socket"].toString() + " -- prod\n";
    EXPECT_TRUE(waitFor([&] { return readFile(log).find(ended) != std::string::npos; })) << readFile(log);
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
    SessionOptions options = shellOptions(daemon, "s");
    options.control.allowReadText = false;
    options.environment.emplace_back("GONE", "old");
    ASSERT_TRUE(daemon.newSession(options)) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("s"), "");
    const std::uint32_t pane = firstPane(ctl);
    ASSERT_NE(pane, 0u);
    EXPECT_FALSE(ctl.call("getText", Json::Array{std::int64_t(pane), "screen"}));
    EXPECT_EQ(ctl.error.message, "reading pane text is off (Control.allowReadText)");
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
    const auto later = split->toInteger();
    ASSERT_TRUE(later) << Json::write(*split);
    ASSERT_TRUE(ctl.call("sendText", Json::Array{*later, "echo \"[$UPDATED][${GONE-unset}]\"\n"})) << ctl.error.message;
    EXPECT_TRUE(screenShows(ctl, *later, "\n[new][unset]")) << ctl.error.message;
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
    SessionOptions o = daemon.options(Waiting, "off");
    o.control.remoteCommandsOff = "remote commands are off (Terminal.sshRemoteCommands)";
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("off"), "");
    const std::uint32_t pane = firstPane(ctl);
    ASSERT_NE(pane, 0u);
    const pid_t process = sleeper();
    ASSERT_GT(process, 0) << "fork failed";
    EXPECT_FALSE(ctl.call("openRemoteSocket", Json::Array{std::int64_t(pane), std::int64_t(process), "box",
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
    // The focused pane, or nothing when the call failed.
    const auto focused = [&]() -> std::optional<std::int64_t> {
        const auto answer = ctl.call("focusedPane", {});
        return answer ? answer->toInteger() : std::nullopt;
    };
    EXPECT_EQ(focused(), std::int64_t(first));
    // Text in, text out. Pane 0 is the focused pane.
    ASSERT_TRUE(ctl.call("sendText", Json::Array{0, "echo hel''lo\n"})) << ctl.error.message;
    EXPECT_TRUE(screenShows(ctl, std::int64_t(first), "\nhello"));
    // A split takes the focus, and a tab shows.
    const auto split = ctl.call("split", Json::Array{std::int64_t(first), "down", Json::Array{}});
    ASSERT_TRUE(split) << ctl.error.message;
    const auto splitId = split->toInteger();
    ASSERT_TRUE(splitId) << Json::write(*split);
    const auto tab = ctl.call("newTab", Json::Array{std::int64_t(first), "", Json::Array{}});
    ASSERT_TRUE(tab) << ctl.error.message;
    const auto tabId = tab->toInteger();
    ASSERT_TRUE(tabId) << Json::write(*tab);
    const auto three = ctl.call("listPanes", {});
    ASSERT_TRUE(three) << ctl.error.message;
    ASSERT_EQ(three->toArray().size(), 3u) << Json::write(*three);
    EXPECT_EQ(paneIds(*three), (std::vector<std::uint32_t>{first, std::uint32_t(*splitId), std::uint32_t(*tabId)}));
    EXPECT_EQ(three->toArray()[2]["tab"].toInteger(), 1);
    EXPECT_EQ(focused(), tabId);
    ASSERT_TRUE(ctl.call("focus", Json::Array{std::int64_t(first)}));
    EXPECT_EQ(focused(), std::int64_t(first));
    // The split moves to a tab of its own, beside its tab.
    const auto moved = ctl.call("movePane", Json::Array{*splitId, 0, "new-tab"});
    ASSERT_TRUE(moved) << ctl.error.message;
    EXPECT_EQ((*moved)["tab"].toInteger(), 1);
    ASSERT_TRUE(ctl.call("close", Json::Array{*tabId})) << ctl.error.message;
    const auto two = ctl.call("listPanes", {});
    ASSERT_TRUE(two) << ctl.error.message;
    EXPECT_EQ(paneIds(*two), (std::vector<std::uint32_t>{first, std::uint32_t(*splitId)}));
    // The next window gets that layout.
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.opened.size() == 2; }));
    ASSERT_EQ(w.layouts.size(), 1u);
    EXPECT_EQ(w.layouts[0].tabs.size(), 2u);
    // The tab that the move made shows, as in a window.
    EXPECT_EQ(w.layouts[0].current, 1u);
}

// While no window shows a session, each pane keeps whether a bell rang and
// its newest notifications, up to Mux.keptNotifications, for the next window:
// it gets them marked missed, in their order. An OSC 99 notification with the
// id of a kept one replaces it, and a close of that id takes it away. A
// clipboard write is not kept.
TEST(MuxAttach, TheNextWindowGetsTheBellAndTheNewestNotificationsOfADetachedSession)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    SessionOptions options = shellOptions(daemon, "kept");
    options.control.allowReadText = true;
    options.keptNotifications = 3;
    ASSERT_TRUE(daemon.newSession(options)) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("kept"), "");
    const std::uint32_t pane = firstPane(ctl);
    ASSERT_NE(pane, 0u);
    // The program's text after the sequences: the daemon parsed them all
    // once it shows. The quotes keep the word off the command line.
    const auto run = [&](const std::string& command, const std::string& word) {
        if (!ctl.call("sendText", Json::Array{std::int64_t(pane), command + "; echo " + word.substr(0, 1) + "''" + word.substr(1) + "\n"})) {
            return false;
        }
        return screenShows(ctl, std::int64_t(pane), "\n" + word);
    };
    // A bell, five OSC 9 notifications, an OSC 99 one with id x, an OSC 777
    // one, a clipboard write, an OSC 99 one that the program then closes,
    // and a new x. Three are kept at most: the OSC 777 one and the new x
    // remain.
    ASSERT_TRUE(run(R"(printf '\a'; for n in 1 2 3 4 5; do printf '\033]9;note%s\a' "$n"; done; )"
                    R"(printf '\033]99;i=x;first\033\\'; printf '\033]777;notify;title;last\a'; )"
                    R"(printf '\033]52;c;aGVsbG8=\a'; printf '\033]99;i=gone;closed\033\\'; )"
                    R"(printf '\033]99;i=gone:p=close;\033\\'; printf '\033]99;i=x;keep\033\\')",
                    "ready"))
        << ctl.error.message;
    // The calls that tell the user something, oldest first.
    const auto alerts = [](const Window& w) {
        std::vector<Host::Call> out;
        std::copy_if(w.calls.begin(), w.calls.end(), std::back_inserter(out), [](const Host::Call& c) {
            return c.kind == Host::Kind::Bell || c.kind == Host::Kind::Flash || c.kind == Host::Kind::Notify
                || c.kind == Host::Kind::DesktopNotification || c.kind == Host::Kind::ClipboardWrite;
        });
        return out;
    };
    SessionSettings settings{options.terminal, options.control};
    {
        Window w;
        ASSERT_EQ(w.attach(daemon, "kept"), "");
        ASSERT_TRUE(w.until([&] { return w.pane != 0 && alerts(w).size() >= 3; })) << alerts(w).size();
        // Nothing more comes.
        ASSERT_TRUE(w.roundTrip());
        const std::vector<Host::Call> got = alerts(w);
        ASSERT_EQ(got.size(), 3u);
        EXPECT_EQ(got[0].kind, Host::Kind::Bell);
        EXPECT_EQ(got[1].kind, Host::Kind::Notify);
        EXPECT_EQ(got[1].title, "title");
        EXPECT_EQ(got[1].text, "last");
        EXPECT_TRUE(got[1].missed);
        EXPECT_EQ(got[2].kind, Host::Kind::DesktopNotification);
        EXPECT_EQ(got[2].notification.kind, AfterglowVt::NotificationRequest::Kind::Show);
        EXPECT_EQ(got[2].notification.id, "x");
        EXPECT_EQ(got[2].notification.title, "keep");
        EXPECT_TRUE(got[2].missed);
        // While the window shows the session, a notification goes at once,
        // and is no missed one.
        ASSERT_TRUE(w.type("printf '\\033]9;live\\a'\n"));
        ASSERT_TRUE(w.until([&] { return alerts(w).size() == 4; }));
        EXPECT_EQ(alerts(w).back().text, "live");
        EXPECT_FALSE(alerts(w).back().missed);
        // A window that keeps none: the session follows it once it detaches.
        settings.keptNotifications = 0;
        ASSERT_TRUE(w.sendSettings(settings));
        ASSERT_TRUE(w.roundTrip());
    }
    ASSERT_TRUE(waitFor([&] {
        const auto info = sessionNamed(daemon, "kept");
        return info && !info->attached;
    }));
    ASSERT_TRUE(run(R"(printf '\033]9;dropped\a\a')", "again")) << ctl.error.message;
    Window next;
    ASSERT_EQ(next.attach(daemon, "kept"), "");
    ASSERT_TRUE(next.until([&] { return next.pane != 0 && !alerts(next).empty(); }));
    ASSERT_TRUE(next.roundTrip());
    // The bell, and no notification: the window got the kept ones before.
    const std::vector<Host::Call> got = alerts(next);
    ASSERT_EQ(got.size(), 1u);
    EXPECT_EQ(got[0].kind, Host::Kind::Bell);
}

// OSC 7 reports file://host/path, percent encoded. The daemon gives the path
// to `afterglow ctl ls` and starts a new tab there, as a window does: the URL
// itself is no directory to start in. A directory of another host is none
// of this machine.
TEST(MuxControl, TheDirectoryOfAShellIsAPathOfThisMachine)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "d")) << daemon.lastError();
    const TempDir temp;
    ASSERT_FALSE(temp.path.empty());
    const std::string directory = (temp.path / "a b").string();
    ASSERT_TRUE(std::filesystem::create_directory(directory));
    // Every byte but the unreserved ones of RFC 3986 and the slashes.
    std::string encoded;
    for (const char c : directory) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (std::isalnum(byte) || c == '-' || c == '.' || c == '_' || c == '~' || c == '/') {
            encoded += c;
            continue;
        }
        static constexpr char Hex[] = "0123456789ABCDEF";
        encoded += {'%', Hex[byte >> 4], Hex[byte & 15]};
    }
    std::array<char, 256> host{};
    ASSERT_EQ(::gethostname(host.data(), host.size() - 1), 0);
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("d"), "");
    const auto cwd = [&](std::size_t index) {
        const auto listed = ctl.call("listPanes", {});
        return listed && listed->toArray().size() > index ? listed->toArray()[index]["cwd"].toString() : std::string();
    };
    const auto report = [&](const std::string& url) {
        return ctl.call("sendText", Json::Array{0, "printf '\\033]7;%s\\007' '" + url + "'\n"}).has_value();
    };
    ASSERT_TRUE(report("file://" + std::string(host.data()) + encoded)) << ctl.error.message;
    ASSERT_TRUE(waitFor([&] { return cwd(0) == directory; })) << cwd(0);
    const auto first = ctl.call("focusedPane", {});
    ASSERT_TRUE(first) << ctl.error.message;
    const auto tab = ctl.call("newTab", Json::Array{*first, "", Json::Array{}});
    ASSERT_TRUE(tab) << ctl.error.message;
    EXPECT_TRUE(waitFor([&] { return cwd(1) == directory; })) << cwd(1);
    ASSERT_TRUE(ctl.call("focus", Json::Array{*first})) << ctl.error.message;
    ASSERT_TRUE(report("file://elsewhere.invalid" + encoded)) << ctl.error.message;
    // The shell never went there: /proc answers.
    EXPECT_TRUE(waitFor([&] {
        const std::string now = cwd(0);
        return !now.empty() && now != directory && !now.starts_with("file:");
    })) << cwd(0);
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

// Without a window, the daemon types keys by name in the program's keyboard
// mode, and `ls` says what a window says of a pane: the user variables, the
// foreground process and the panes beside it, from the splits of the layout.
// The daemon also moves the focus to a neighbor. A tab's title and a scroll
// of the view need the window.
TEST(MuxControl, TheDaemonTypesKeysAndTellsThePanesFacts)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "k")) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("k"), "");
    const std::uint32_t first = firstPane(ctl);
    ASSERT_NE(first, 0u);
    const auto screen = [&](const std::string& word) { return screenShows(ctl, std::int64_t(first), word); };
    // The quotes keep the word off the command line.
    ASSERT_TRUE(ctl.call("sendKeys", Json::Array{std::int64_t(first), Json::Array{"e", "c", "h", "o", "space", "k", "'", "'", "e",
                                                                                    "y", "Return"}}))
        << ctl.error.message;
    EXPECT_TRUE(screen("\nkey"));
    // A name that names no key sends none of the keys: no x before the
    // command that follows.
    EXPECT_FALSE(ctl.call("sendKeys", Json::Array{std::int64_t(first), Json::Array{"x", "nope"}}));
    EXPECT_EQ(ctl.error.message, "no key is named 'nope'");
    ASSERT_TRUE(ctl.call("sendText", Json::Array{std::int64_t(first), "echo no''ne\n"}));
    EXPECT_TRUE(screen("\nnone"));
    EXPECT_FALSE(ctl.call("sendKeys", Json::Array{std::int64_t(first), Json::Array{}}));
    EXPECT_EQ(ctl.error.message, "sendKeys needs a key");
    Json::Array many;
    for (std::size_t i = 0; i <= AfterglowVt::MaxKeysAtOnce; ++i) many.push_back("a");
    EXPECT_FALSE(ctl.call("sendKeys", Json::Array{std::int64_t(first), many}));
    EXPECT_EQ(ctl.error.message, "sendKeys sends at most " + std::to_string(AfterglowVt::MaxKeysAtOnce) + " keys at once");
    // In the program's keyboard mode: Up is ESC O A with application cursor
    // keys (DECCKM), not ESC [ A.
    ASSERT_TRUE(ctl.call("sendText", Json::Array{std::int64_t(first), "stty raw -echo; printf '\\033[?1hre''ady'; head -c3 | od -An -tx1; "
                                                                       "printf '\\033[?1l'; stty sane\n"}));
    ASSERT_TRUE(screen("ready"));
    ASSERT_TRUE(ctl.call("sendKeys", Json::Array{std::int64_t(first), Json::Array{"Up"}})) << ctl.error.message;
    EXPECT_TRUE(screen(" 1b 4f 41"));
    // A user variable, in base64 as iTerm2 sends it.
    ASSERT_TRUE(ctl.call("sendText", Json::Array{std::int64_t(first), "printf '\\033]1337;SetUserVar=mode=%s\\a' djE=; echo s''et\n"}));
    ASSERT_TRUE(screen("\nset"));
    // The shell at its prompt (OSC 133 A), as shell integration marks it.
    ASSERT_TRUE(ctl.call("sendText", Json::Array{std::int64_t(first), "printf '\\033]133;A\\a'\n"}));
    EXPECT_TRUE(waitFor([&] {
        const auto listed = ctl.call("listPanes", {});
        return listed && !listed->toArray().empty() && listed->toArray()[0]["at_prompt"].isBool()
            && listed->toArray()[0]["at_prompt"].toBool();
    }));
    const auto split = ctl.call("split", Json::Array{std::int64_t(first), "down", Json::Array{}});
    ASSERT_TRUE(split) << ctl.error.message;
    const auto splitId = split->toInteger();
    ASSERT_TRUE(splitId) << Json::write(*split);
    const auto listed = ctl.call("listPanes", {});
    ASSERT_TRUE(listed) << ctl.error.message;
    ASSERT_EQ(listed->toArray().size(), 2u);
    const Json::Value& pane = listed->toArray()[0];
    EXPECT_EQ(Json::write(pane["user_vars"]), R"({"mode":"v1"})");
    EXPECT_EQ(pane["neighbors"]["down"].toInteger(), *splitId);
    EXPECT_EQ(pane["neighbors"]["up"].toInteger(), 0);
    EXPECT_EQ(listed->toArray()[1]["neighbors"]["up"].toInteger(), std::int64_t(first));
    EXPECT_GT(pane["foreground"]["pid"].toInteger().value_or(0), 0) << Json::write(pane);
    EXPECT_FALSE(pane["foreground"]["cmdline"].toArray().empty()) << Json::write(pane);
    // The split has the focus: up goes back to the first pane, and the
    // first pane has nothing to its left.
    EXPECT_EQ(ctl.call("focusedPane", {}).value_or(Json::Value()).toInteger(), *splitId);
    const auto up = ctl.call("focusNeighbor", Json::Array{*splitId, "up"});
    ASSERT_TRUE(up) << ctl.error.message;
    EXPECT_EQ(up->toInteger(), std::int64_t(first));
    EXPECT_EQ(ctl.call("focusedPane", {}).value_or(Json::Value()).toInteger(), std::int64_t(first));
    EXPECT_FALSE(ctl.call("focusNeighbor", Json::Array{std::int64_t(first), "left"}));
    EXPECT_EQ(ctl.error.message, "no pane to the left of the pane");
    EXPECT_FALSE(ctl.call("focusNeighbor", Json::Array{std::int64_t(first), "back"}));
    EXPECT_EQ(ctl.error.message, "direction must be left, right, up or down");
    for (const char* fn : {"scroll", "setTabTitle"}) {
        EXPECT_FALSE(ctl.call(fn, Json::Array{std::int64_t(first), "down"})) << fn;
        EXPECT_NE(ctl.error.message.find(std::string("only a window can do ") + fn), std::string::npos) << ctl.error.message;
    }
}

// it2ul (OSC 1337 RequestUpload) in a session: the window that shows it
// asks the user. Without a window nobody can choose the files or cancel:
// the program gets nothing, not the Ctrl+C of a cancel, which only a person
// sends. Nor does it when the window goes before it answers.
TEST(MuxAttach, AnUploadRequestGoesToTheWindowOrGetsNothing)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "u")) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("u"), "");
    const std::int64_t pane = firstPane(ctl);
    ASSERT_NE(pane, 0);
    // What the program reads after its request, in hexadecimal, within
    // @p seconds; then "end" (the quotes keep it off the command line). In
    // raw mode a line feed returns no carriage: "end" can follow spaces.
    const auto request = [](const std::string& seconds) {
        return "stty raw -echo; printf '\\033]1337;RequestUpload=format=tgz\\a'; timeout " + seconds
            + " head -c1 | od -An -tx1; stty sane; echo en''d\n";
    };
    // The screen of the last request goes: its text does not count again.
    const std::string clear = "printf '\\033[H\\033[2J'\n";
    const auto readNothing = [&] {
        const auto text = ctl.call("getText", Json::Array{pane, "screen"});
        return text && !contains(text->toString(), " 03") && !contains(text->toString(), " 78");
    };
    ASSERT_TRUE(ctl.call("sendText", Json::Array{pane, request("1")})) << ctl.error.message;
    ASSERT_TRUE(screenShows(ctl, pane, "end"));
    EXPECT_TRUE(readNothing());

    Window w;
    ASSERT_EQ(w.attach(daemon, "u"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    ASSERT_TRUE(w.type(clear + request("30")));
    ASSERT_TRUE(w.until([&] { return hasCall(w, Host::Kind::UploadRequest); }));
    // The window answers as typed input; here a byte.
    ASSERT_TRUE(w.type("x"));
    EXPECT_TRUE(screenShows(ctl, pane, " 78"));
    ASSERT_TRUE(screenShows(ctl, pane, "end"));

    // The window goes with the question: the program reads nothing.
    const auto asked = std::count_if(w.calls.begin(), w.calls.end(), [](const Host::Call& c) { return c.kind == Host::Kind::UploadRequest; });
    ASSERT_TRUE(w.type(clear + request("2")));
    ASSERT_TRUE(w.until([&] {
        return std::count_if(w.calls.begin(), w.calls.end(), [](const Host::Call& c) { return c.kind == Host::Kind::UploadRequest; }) > asked;
    }));
    ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"u"})) << daemon.lastError();
    ASSERT_TRUE(screenShows(ctl, pane, "end"));
    EXPECT_TRUE(readNothing());
}

// A window that goes before it answers an upload leaves the question to
// the next window, while the job that asked still holds the terminal: that
// window asks again, and its answer reaches the program. Nobody answers
// meanwhile. An answer leaves nothing to ask.
TEST(MuxAttach, TheNextWindowAsksForAnUploadThatTheLastOneDidNotAnswer)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "q")) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("q"), "");
    const std::int64_t pane = firstPane(ctl);
    ASSERT_NE(pane, 0);
    const auto asks = [](const Window& w) {
        return std::count_if(w.calls.begin(), w.calls.end(), [](const Host::Call& c) { return c.kind == Host::Kind::UploadRequest; });
    };
    auto first = std::make_unique<Window>();
    ASSERT_EQ(first->attach(daemon, "q"), "");
    ASSERT_TRUE(first->until([&] { return first->pane != 0; }));
    // One job asks and reads: the subshell leads its process group. What it
    // reads shows in hexadecimal, then "end" (the quotes keep it off the
    // command line). It still holds the terminal after: an answer, not the
    // end of the job, is why the last window does not ask.
    ASSERT_TRUE(first->type("stty raw -echo; (printf '\\033]1337;RequestUpload=format=tgz\\a'; head -c1 | od -An -tx1; echo en''d; sleep 60)\n"));
    ASSERT_TRUE(first->until([&] { return asks(*first) == 1; }));
    ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"q"})) << daemon.lastError();
    first.reset();

    Window second;
    ASSERT_EQ(second.attach(daemon, "q"), "");
    ASSERT_TRUE(second.until([&] { return asks(second) == 1; }));
    const auto screen = ctl.call("getText", Json::Array{pane, "screen"});
    ASSERT_TRUE(screen) << ctl.error.message;
    EXPECT_FALSE(contains(screen->toString(), "end")) << screen->toString();
    // The answer as a window sends one.
    Attach::Input answer;
    answer.type = Attach::InputType::UploadAnswer;
    answer.pane = second.pane;
    answer.bytes = "x";
    ASSERT_TRUE(second.send(answer));
    EXPECT_TRUE(screenShows(ctl, pane, " 78"));
    ASSERT_TRUE(screenShows(ctl, pane, "end"));
    ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"q"})) << daemon.lastError();

    Window third;
    ASSERT_EQ(third.attach(daemon, "q"), "");
    ASSERT_TRUE(third.until([&] { return third.pane != 0; }));
    // A question would come with the first frames, before the answers of
    // the reads.
    ASSERT_TRUE(third.roundTrip());
    EXPECT_EQ(asks(third), 0);
}

namespace {

/// The answer to an upload, as a window sends it.
Attach::Input uploadAnswer(std::uint32_t pane, std::string bytes)
{
    Attach::Input answer;
    answer.type = Attach::InputType::UploadAnswer;
    answer.pane = pane;
    answer.bytes = std::move(bytes);
    return answer;
}

std::size_t uploadAsks(const Window& w)
{
    return std::size_t(std::count_if(w.calls.begin(), w.calls.end(), [](const Host::Call& c) { return c.kind == Host::Kind::UploadRequest; }));
}

} // namespace

// A reset (RIS) does not end the command that asked for files: the
// question stays, and the next window asks again. Only the end of the
// command or of the program ends it.
TEST(MuxAttach, AResetKeepsTheQuestionOfAnUpload)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "r")) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("r"), "");
    const std::int64_t pane = firstPane(ctl);
    ASSERT_NE(pane, 0);
    auto first = std::make_unique<Window>();
    ASSERT_EQ(first->attach(daemon, "r"), "");
    ASSERT_TRUE(first->until([&] { return first->pane != 0; }));
    // The job asks, resets the terminal, then reads a byte and shows it in
    // hexadecimal, then "end".
    ASSERT_TRUE(first->type("stty raw -echo; (printf '\\033]1337;RequestUpload=format=tgz\\a\\033c'; head -c1 | od -An -tx1; echo en''d; sleep 60)\n"));
    ASSERT_TRUE(first->until([&] { return uploadAsks(*first) == 1; }));
    // The reset reached the terminal: the screen is clear of the command.
    ASSERT_TRUE(waitFor([&] {
        const auto text = ctl.call("getText", Json::Array{pane, "screen"});
        return text && !contains(text->toString(), "RequestUpload");
    })) << ctl.error.message;
    ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"r"})) << daemon.lastError();
    first.reset();
    Window second;
    ASSERT_EQ(second.attach(daemon, "r"), "");
    ASSERT_TRUE(second.until([&] { return uploadAsks(second) == 1; }));
    ASSERT_TRUE(second.send(uploadAnswer(second.pane, "x")));
    EXPECT_TRUE(screenShows(ctl, pane, " 78"));
    ASSERT_TRUE(screenShows(ctl, pane, "end"));
}

// A window that a takeover replaced may answer the upload that it was asked
// for before it hears: the program gets only the answer of the window that
// the daemon asked last, which may show the question already. A second
// answer would reach the shell.
TEST(MuxAttach, OnlyTheWindowAskedLastAnswersAnUpload)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "q")) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("q"), "");
    const std::int64_t pane = firstPane(ctl);
    ASSERT_NE(pane, 0);
    Window first;
    ASSERT_EQ(first.attach(daemon, "q"), "");
    ASSERT_TRUE(first.until([&] { return first.pane != 0; }));
    // The job reads two bytes and shows them in hexadecimal, then "end".
    ASSERT_TRUE(first.type("stty raw -echo; (printf '\\033]1337;RequestUpload=format=tgz\\a'; head -c2 | od -An -tx1; echo en''d; sleep 60)\n"));
    ASSERT_TRUE(first.until([&] { return uploadAsks(first) == 1; }));
    Window second;
    ASSERT_EQ(second.attach(daemon, "q"), "");
    ASSERT_TRUE(second.until([&] { return uploadAsks(second) == 1; }));
    // The answer of the window that went: two calls of the control target
    // later, the daemon read it.
    ASSERT_TRUE(first.send(uploadAnswer(first.pane, "a")));
    for (int i = 0; i < 2; ++i) ASSERT_TRUE(ctl.call("getText", Json::Array{pane, "screen"})) << ctl.error.message;
    // The answer of the window that shows the question, then a byte typed.
    ASSERT_TRUE(second.send(uploadAnswer(second.pane, "b")));
    ASSERT_TRUE(second.type("c"));
    EXPECT_TRUE(screenShows(ctl, pane, " 62 63"));
    ASSERT_TRUE(screenShows(ctl, pane, "end"));
}

// An answer to an upload that waits for room in a full PTY when its window
// goes is lost with the rest of that window's input: the program did not
// get it, and the next window asks again.
TEST(MuxAttach, TheNextWindowAsksAgainForAnUploadAnswerThatWasLost)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string go = daemon.dir() + "/go";
    // Asks once the test says, and reads nothing.
    SessionOptions o = daemon.options({"/bin/sh", "-c",
                                       "stty raw -echo; while [ ! -e '" + go + "' ]; do sleep 0.05; done; "
                                       "printf '\\033]1337;RequestUpload=format=tgz\\a'; exec sleep 60"});
    o.name = "s";
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    std::ofstream(go).put('x');
    ASSERT_TRUE(w.until([&] { return uploadAsks(w) == 1; }));
    // A second pane, whose echo says that the daemon read what came before it.
    const auto echo = newPane(daemon, "s", launchOf({"/bin/cat"}, daemon));
    ASSERT_TRUE(echo) << daemon.lastError();
    ASSERT_TRUE(w.until([&] { return w.others.contains(*echo); }));
    // Past the 32 MiB that the PTY queues, and below the bound of held
    // input (Attachment::MaxHeldInput): the daemon holds the rest, and the
    // answer behind it.
    const std::string part(60000, 'x');
    ASSERT_GE(w.fill(part, std::size_t(34) << 20, 200), std::size_t(34) << 20);
    ASSERT_TRUE(w.send(uploadAnswer(w.pane, "y")));
    Attach::Input ping;
    ping.pane = *echo;
    ping.bytes = "ping\n";
    ASSERT_TRUE(w.send(ping));
    const auto echoed = [&] { return w.others.at(*echo)->replica.text(AfterglowVt::Terminal::TextScope::Screen); };
    ASSERT_TRUE(w.until([&] { return contains(echoed(), "ping"); }, 10s)) << echoed();
    ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"s"})) << daemon.lastError();
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached); }));
    Window again;
    ASSERT_EQ(again.attach(daemon, "s"), "");
    EXPECT_TRUE(again.until([&] { return uploadAsks(again) == 1; })) << daemon.errors();
}

// Without a window no user can answer: the daemon refuses a download, the
// program goes on, and the next window hears nothing of it.
TEST(MuxAttach, ADownloadWithoutAWindowIsRefused)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    SessionOptions o = daemon.options({"/bin/sh", "-c", "printf '\\033]1337;File=name=Z290LmJpbg==;size=4:eHl6dw==\\007'; printf al''one; exec cat"},
                                      "alone");
    o.control.allowReadText = true;
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("alone"), "");
    const std::int64_t pane = firstPane(ctl);
    ASSERT_NE(pane, 0);
    // The daemon parsed the download before the text after it.
    ASSERT_TRUE(screenShows(ctl, pane, "alone"));
    Window w;
    ASSERT_EQ(w.attach(daemon, "alone"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "alone"); })) << w.screen();
    ASSERT_TRUE(w.roundTrip());
    EXPECT_FALSE(hasCall(w, Host::Kind::DownloadBegin));
    EXPECT_FALSE(contains(w.screen(), "eHl6dw")) << w.screen();
}

// The colours, user variables and signals of a session's panes are the
// daemon's: it has their terminals and programs, also without a window. The
// window's font, opacity and state need a window.
TEST(MuxControl, TheDaemonSetsColoursVariablesAndSignals)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "c")) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("c"), "");
    const std::int64_t pane = firstPane(ctl);
    ASSERT_NE(pane, 0);
    const auto color = [&](const std::string& name) {
        const auto colors = ctl.call("getColors", Json::Array{pane});
        return colors ? Json::write((*colors)[name]) : std::string("no answer: ") + ctl.error.message;
    };
    const std::string configured = color("color1");
    ASSERT_TRUE(ctl.call("setColors", Json::Array{pane, Json::Object{{"color1", "#ff0010"}, {"background", "rgb:01/02/03"}}, false}))
        << ctl.error.message;
    EXPECT_EQ(color("color1"), "\"#ff0010\"");
    EXPECT_EQ(color("background"), "\"#010203\"");
    EXPECT_EQ(color("cursor"), "null");
    // As a window reads a colour: "#fff" is white as in CSS and kitty, not
    // f0f0f0 as XParseColor reads it, and blanks around it go.
    ASSERT_TRUE(ctl.call("setColors", Json::Array{pane, Json::Object{{"color5", " #fff "}, {"color6", "#123456789"}}, false}))
        << ctl.error.message;
    EXPECT_EQ(color("color5"), "\"#ffffff\"");
    EXPECT_EQ(color("color6"), "\"#124578\"");
    EXPECT_FALSE(ctl.call("setColors", Json::Array{pane, Json::Object{{"color5", "#1e1e2eff"}}, false}));
    EXPECT_EQ(ctl.error.message, "#1e1e2eff is no color");
    // A bad colour or name sets none.
    const std::string color2 = color("color2");
    EXPECT_FALSE(ctl.call("setColors", Json::Array{pane, Json::Object{{"color2", "#00ff00"}, {"color3", "nope"}}, false}));
    EXPECT_EQ(ctl.error.message, "nope is no color");
    EXPECT_FALSE(ctl.call("setColors", Json::Array{pane, Json::Object{{"color2", "#00ff00"}, {"colour4", "#00ff00"}}, false}));
    EXPECT_EQ(ctl.error.message, "no color is named colour4");
    EXPECT_FALSE(ctl.call("setColors", Json::Array{pane, Json::Object{{"color2", std::int64_t(255)}}, false}));
    EXPECT_EQ(ctl.error.message, "the color of color2 is a string");
    EXPECT_EQ(color("color2"), color2);
    ASSERT_TRUE(ctl.call("setColors", Json::Array{pane, Json::Object{}, true})) << ctl.error.message;
    EXPECT_EQ(color("color1"), configured);

    ASSERT_TRUE(ctl.call("setUserVar", Json::Array{pane, "role", "build"})) << ctl.error.message;
    const auto listed = ctl.call("listPanes", {});
    ASSERT_TRUE(listed) << ctl.error.message;
    ASSERT_EQ(listed->toArray().size(), 1u);
    EXPECT_EQ(Json::write(listed->toArray()[0]["user_vars"]), R"({"role":"build"})");
    // A name as OSC 1337 SetUserVar takes one: 1 to 128 bytes, no =.
    for (const std::string& name : {std::string("a=b"), std::string(), std::string(129, 'n')}) {
        EXPECT_FALSE(ctl.call("setUserVar", Json::Array{pane, name, "c"})) << name;
        EXPECT_EQ(ctl.error.message, "a user variable's name has 1 to 128 bytes and no =");
    }
    EXPECT_TRUE(ctl.call("setUserVar", Json::Array{pane, std::string(128, 'n'), "c"})) << ctl.error.message;
    // A pane keeps MaxUserVars: then a new name is refused, not dropped
    // without a word, and a name that it has takes a new value.
    for (std::size_t i = 2; i < AfterglowVt::ITerminalHost::MaxUserVars; ++i) {
        ASSERT_TRUE(ctl.call("setUserVar", Json::Array{pane, "v" + std::to_string(i), "x"})) << i << ": " << ctl.error.message;
    }
    EXPECT_FALSE(ctl.call("setUserVar", Json::Array{pane, "one-more", "x"}));
    EXPECT_EQ(ctl.error.message, "pane " + std::to_string(pane) + " has 256 user variables: it takes no new name");
    EXPECT_TRUE(ctl.call("setUserVar", Json::Array{pane, "role", "test"})) << ctl.error.message;

    // INT ends the sleep in the foreground: the next command runs at once.
    ASSERT_TRUE(ctl.call("sendText", Json::Array{pane, "sleep 60\n"}));
    ASSERT_TRUE(waitFor([&] {
        const auto listed = ctl.call("listPanes", {});
        return listed && !listed->toArray().empty() && contains(Json::write(listed->toArray()[0]["foreground"]["cmdline"]), "sleep");
    }));
    ASSERT_TRUE(ctl.call("signalProgram", Json::Array{pane, "SIGINT"})) << ctl.error.message;
    ASSERT_TRUE(ctl.call("sendText", Json::Array{pane, "echo af''ter\n"}));
    EXPECT_TRUE(screenShows(ctl, pane, "\nafter"));
    EXPECT_FALSE(ctl.call("signalProgram", Json::Array{pane, "SIGNOPE"}));
    EXPECT_EQ(ctl.error.message, "no signal is named SIGNOPE");
    // The name first, as a window checks it, also for a pane that is not.
    EXPECT_FALSE(ctl.call("signalProgram", Json::Array{9999, "SIGNOPE"}));
    EXPECT_EQ(ctl.error.message, "no signal is named SIGNOPE");

    for (const auto& [fn, call] : std::vector<std::pair<std::string, Json::Array>>{
             {"setFontSize", Json::Array{pane, "+2"}}, {"setOpacity", Json::Array{pane, 0.5}}, {"setWindowState", Json::Array{pane, "maximized", 0, 0}}}) {
        EXPECT_FALSE(ctl.call(fn, call)) << fn;
        EXPECT_NE(ctl.error.message.find("only a window can do " + fn), std::string::npos) << ctl.error.message;
    }
}

// A window that shows the pane sees what the daemon sets: the colours in the
// next frame, and a user variable as the call of the program's terminal.
TEST(MuxControl, TheWindowSeesTheColoursAndVariablesThatTheDaemonSets)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "w")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "w"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "$ "); })) << w.screen();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("w"), "");
    ASSERT_TRUE(ctl.call("setColors", Json::Array{std::int64_t(w.pane), Json::Object{{"color1", "#ff0010"}}, false})) << ctl.error.message;
    const auto color1 = [&w] {
        for (const auto& color : w.replica.namedColors()) {
            if (color.name == "color1") return color.rgb;
        }
        return std::optional<AfterglowVt::Rgb>();
    };
    EXPECT_TRUE(w.until([&] {
        const auto rgb = color1();
        return rgb && rgb->r == 0xff && rgb->g == 0x00 && rgb->b == 0x10;
    }));
    ASSERT_TRUE(ctl.call("setUserVar", Json::Array{std::int64_t(w.pane), "role", "build"})) << ctl.error.message;
    EXPECT_TRUE(w.until([&] {
        return std::any_of(w.calls.begin(), w.calls.end(), [](const Host::Call& c) {
            return c.kind == Host::Kind::UserVar && c.title == "role" && c.text == "build";
        });
    }));
}

// get-text with the options of kitten @ get-text: the daemon writes its own
// terminal's text with styles, and finds the first output on its screen.
// What a window's view knows (the last visited output) is refused without one.
TEST(MuxControl, TheDaemonWritesTextWithItsStyles)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "t")) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("t"), "");
    const std::int64_t pane = firstPane(ctl);
    ASSERT_NE(pane, 0);
    // A command with OSC 133 marks and red output; the quotes keep the word
    // off the command line.
    ASSERT_TRUE(ctl.call("sendText", Json::Array{pane, "printf '\\033]133;A\\a$ \\033]133;B\\acmd\\033]133;C\\a\\n\\033[31mre''d\\033[m\\n\\033]133;D;0\\a'\n"}))
        << ctl.error.message;
    ASSERT_TRUE(screenShows(ctl, pane, "\nred"));
    const auto styled = ctl.call("getText", Json::Array{pane, "screen", Json::Object{{"ansi", true}}});
    ASSERT_TRUE(styled) << ctl.error.message;
    EXPECT_TRUE(contains(styled->toString(), "\x1b[0;31mred\x1b[m")) << styled->toString();
    const auto first = ctl.call("getText", Json::Array{pane, "first-output-on-screen"});
    ASSERT_TRUE(first) << ctl.error.message;
    EXPECT_EQ(first->toString(), "\nred\n");
    // The text of one output ends before the cursor's place: refused as a
    // window refuses it.
    for (const char* scope : {"first-output-on-screen", "last-visited-output"}) {
        EXPECT_FALSE(ctl.call("getText", Json::Array{pane, scope, Json::Object{{"cursor", true}}})) << scope;
        EXPECT_EQ(ctl.error.message, "cursor is for the screen, scrollback and last-output scopes") << scope;
    }
    EXPECT_FALSE(ctl.call("getText", Json::Array{pane, "last-visited-output"}));
    EXPECT_EQ(ctl.error.message, "only a window that shows session 't' has its last-visited-output");
    const auto cursor = ctl.call("getText", Json::Array{pane, "screen", Json::Object{{"cursor", true}}});
    ASSERT_TRUE(cursor) << ctl.error.message;
    EXPECT_TRUE(contains(cursor->toString(), "\x1b[?25h")) << cursor->toString();
    // A row that wraps ends with a carriage return with wrap_markers, and
    // runs on into the next row without.
    ASSERT_TRUE(ctl.call("sendText", Json::Array{pane, "printf '%0300d\\n' 0\n"})) << ctl.error.message;
    ASSERT_TRUE(screenShows(ctl, pane, std::string(300, '0')));
    const auto wrapped = ctl.call("getText", Json::Array{pane, "screen", Json::Object{{"wrap_markers", true}}});
    ASSERT_TRUE(wrapped) << ctl.error.message;
    EXPECT_TRUE(contains(wrapped->toString(), "0\r0")) << wrapped->toString();
    EXPECT_FALSE(contains(wrapped->toString(), std::string(300, '0'))) << wrapped->toString();
    EXPECT_FALSE(ctl.call("getText", Json::Array{pane, "screen", Json::Object{{"bold", true}}}));
    EXPECT_EQ(ctl.error.message, "unknown getText option bold: the options are ansi, cursor and wrap_markers");
    EXPECT_FALSE(ctl.call("getText", Json::Array{pane, "screen", Json::Object{{"ansi", 1}}}));
    EXPECT_EQ(ctl.error.message, "the getText option ansi is true or false");
}

// While a window shows the session, the daemon places the panes that it
// starts in that window, and passes it what only a window can answer. Only
// the answer of the window that got a call, with the call's ID, reaches its
// client.
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
    const auto splitId = split->toInteger();
    ASSERT_TRUE(splitId) << Json::write(*split);
    ASSERT_TRUE(w.until([&] { return !w.controls().empty() && w.opened.size() == 2; }));
    const Json::Value place = w.controls().front();
    EXPECT_EQ(place["id"].toInteger(), 0);
    EXPECT_EQ(place["fn"].toString(), "placePane");
    EXPECT_EQ(Json::write(place["args"]), "[" + std::to_string(*splitId) + "," + std::to_string(w.pane) + ",\"left\",[]]");
    const auto logged = [&daemon] {
        const std::string errors = daemon.errors();
        std::size_t count = 0;
        for (std::size_t at = 0; (at = errors.find("answered a call that it did not get", at)) != std::string::npos; ++at) ++count;
        return count;
    };

    // The window answers a title; the client waits for that answer. The
    // calls wait on threads, which a failed assertion joins: a call ends
    // within its timeout.
    std::optional<Json::Value> titled;
    std::atomic<bool> answered{false};
    std::jthread caller([&] {
        titled = ctl.call("setTitle", Json::Array{std::int64_t(w.pane), "work"});
        answered = true;
    });
    ASSERT_TRUE(w.until([&] { return w.controls().size() == 2; }));
    const Json::Value call = w.controls().back();
    EXPECT_EQ(call["fn"].toString(), "setTitle");
    EXPECT_EQ(Json::write(call["args"]), "[" + std::to_string(w.pane) + ",\"work\"]");
    // An answer to a call that the window did not get, and one that is not
    // JSON, reach no client.
    Attach::Input reply;
    reply.type = Attach::InputType::ControlReply;
    reply.bytes = Json::write(Json::Object{{"id", 999999}, {"result", "wrong"}});
    ASSERT_TRUE(w.send(reply));
    reply.bytes = "not JSON";
    ASSERT_TRUE(w.send(reply));
    ASSERT_TRUE(w.roundTrip());
    EXPECT_FALSE(answered.load());
    EXPECT_TRUE(waitFor([&] { return logged() == 2; })) << daemon.errors();
    reply.bytes = Json::write(Json::Object{{"id", call["id"]}, {"result", "done"}});
    EXPECT_TRUE(w.send(reply));
    EXPECT_TRUE(w.until([&] { return answered.load(); }));
    caller.join();
    ASSERT_TRUE(titled) << ctl.error.message;
    EXPECT_EQ(titled->toString(), "done");

    // A client that goes before the answer: the window's answer goes
    // nowhere, and is no answer to a call that the window did not get. A
    // second answer to that call is one.
    {
        RawClient gone(daemon.socket());
        ASSERT_TRUE(gone.connected());
        // The answer of the selection stays unread: the close resets the
        // connection, and the daemon drops it with its call.
        ASSERT_TRUE(gone.send(Json::write(Json::Object{{"type", "call"}, {"id", 1}, {"target", Target}, {"fn", Fn::SelectSession},
                                                       {"args", Json::Array{ProtocolVersion, "s"}}})
                              + "\n"
                              + Json::write(Json::Object{{"type", "call"}, {"id", 2}, {"target", "control"}, {"fn", "setTitle"},
                                                         {"args", Json::Array{std::int64_t(w.pane), "gone"}}})
                              + "\n"));
        ASSERT_TRUE(w.until([&] { return w.controls().size() == 3; }));
    }
    // A call on another connection: the daemon handled the close before it.
    ASSERT_TRUE(daemon.call(Fn::ListSessions, {})) << daemon.lastError();
    const std::size_t before = logged();
    reply.bytes = Json::write(Json::Object{{"id", w.controls().back()["id"]}, {"result", "late"}});
    ASSERT_TRUE(w.send(reply));
    ASSERT_TRUE(w.roundTrip());
    EXPECT_EQ(logged(), before) << daemon.errors();
    ASSERT_TRUE(w.send(reply));
    ASSERT_TRUE(w.roundTrip());
    EXPECT_TRUE(waitFor([&] { return logged() == before + 1; })) << daemon.errors();

    // A window that goes before it answers: the client hears why.
    std::jthread waiting([&] { titled = ctl.call("notify", Json::Array{std::int64_t(w.pane), "t", "b"}); });
    ASSERT_TRUE(w.until([&] { return w.controls().size() == 4; }));
    ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"s"})) << daemon.lastError();
    waiting.join();
    EXPECT_FALSE(titled);
    EXPECT_NE(ctl.error.message.find("closed before it answered"), std::string::npos) << ctl.error.message;
}

// A window whose answer, with its parts, is larger than Attach::
// MaxControlReply loses the session: the daemon would hold it all. The
// client of the call hears that the window went.
TEST(MuxControl, AWindowThatAnswersMoreThanAReplyHoldsLosesTheSession)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "s")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("s"), "");
    std::optional<Json::Value> titled;
    std::atomic<bool> answered{false};
    std::jthread caller([&] {
        titled = ctl.call("setTitle", Json::Array{std::int64_t(w.pane), "t"});
        answered = true;
    });
    ASSERT_TRUE(w.until([&] { return w.controls().size() == 1; }));
    Attach::Input part;
    part.type = Attach::InputType::ControlReplyPart;
    part.bytes = std::string(Attach::ControlReplyPartBytes, 'A');
    for (std::size_t sent = 0; sent <= Attach::MaxControlReply && !w.ended; sent += part.bytes.size()) {
        if (!w.until([&] { return w.send(part) || w.ended; }, 10s)) break;
    }
    ASSERT_TRUE(w.until([&] { return w.ended; }));
    EXPECT_TRUE(waitFor([&] { return contains(daemon.errors(), "sent a reply of more than"); })) << daemon.errors();
    EXPECT_TRUE(waitFor([&] { return answered.load(); }));
    caller.join();
    EXPECT_FALSE(titled);
    EXPECT_NE(ctl.error.message.find("closed before it answered"), std::string::npos) << ctl.error.message;
    EXPECT_TRUE(waitFor([&] {
        const auto info = sessionNamed(daemon, "s");
        return info && !info->attached;
    }));
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
    const auto controls = [&w] { return w.controls(); };
    auto ctl = std::make_unique<ControlClient>(daemon);
    ASSERT_EQ(ctl->select("s"), "");

    // What the calls write: before the thread, which a failed assertion
    // joins first, so that a call that still waits writes into them. A
    // call ends within its timeout.
    std::atomic<bool> answered{false};
    std::optional<Json::Value> shot;
    std::optional<Json::Value> input;
    std::optional<Json::Value> overlay;
    // Each call waits on a thread of its own while the window's stream runs.
    std::thread caller;
    struct JoinAtEnd
    {
        std::thread& thread;
        ~JoinAtEnd()
        {
            if (thread.joinable()) thread.join();
        }
    } joinAtEnd{caller};
    const auto join = [&](std::thread& caller) {
        EXPECT_TRUE(w.until([&] { return answered.load(); }));
        caller.join();
        answered = false;
    };
    // A screenshot whose answer is larger than a part: 3 parts and the rest.
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

// Two answers of a window that each fit a connection, but not together,
// for a client that reads nothing meanwhile: the first goes, the second
// becomes an error. The connection stays open: it would close with neither.
TEST(MuxControl, AWindowsAnswerThatTheConnectionCannotTakeIsAnError)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "s")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("s"), "");
    // Both calls before any answer, which the client does not read yet.
    const auto first = ctl.connection.send("control", "screenshot", Json::Array{0, "pane"}, &ctl.error);
    ASSERT_TRUE(first) << ctl.error.message;
    const auto second = ctl.connection.send("control", "screenshot", Json::Array{0, "pane"}, &ctl.error);
    ASSERT_TRUE(second) << ctl.error.message;
    ASSERT_TRUE(w.until([&] { return w.controls().size() == 2; }));
    // Each just under Attach::MaxControlReply: two pass the daemon's output
    // limit by far more than the socket takes.
    const std::string png(Attach::MaxControlReply - 1024, 'A');
    for (const Json::Value& call : w.controls()) {
        const std::string answer = Json::write(Json::Object{{"id", call["id"]}, {"result", Json::Object{{"png", png}}}});
        for (std::size_t at = 0; at < answer.size(); at += Attach::ControlReplyPartBytes) {
            Attach::Input part;
            part.type = at + Attach::ControlReplyPartBytes < answer.size() ? Attach::InputType::ControlReplyPart : Attach::InputType::ControlReply;
            part.bytes = answer.substr(at, Attach::ControlReplyPartBytes);
            ASSERT_TRUE(w.until([&] { return w.send(part) || w.ended; }, 10s));
            ASSERT_FALSE(w.ended);
        }
    }
    // Each channel queues its own frames, and the stream takes turns between
    // them: a refresh would pass the parts that still wait in the window, and
    // nothing pumps the window while the client reads. So first all of them
    // go out. The daemon then reads them before the refreshes behind them,
    // and passes the answers on in that pass.
    ASSERT_TRUE(w.until([&] { return !w.stream->wantsWrite(); }, 10s));
    ASSERT_TRUE(w.roundTrip());
    Json::Value frame;
    ASSERT_EQ(ctl.connection.next(frame, 10000), IpcConnection::Read::Frame) << ctl.connection.closeReason();
    ASSERT_TRUE(IpcConnection::answers(frame, *first));
    const auto shot = IpcConnection::answer(std::move(frame), &ctl.error);
    ASSERT_TRUE(shot) << ctl.error.message;
    EXPECT_EQ((*shot)["png"].toString().size(), png.size());
    ASSERT_EQ(ctl.connection.next(frame, 10000), IpcConnection::Read::Frame) << ctl.connection.closeReason();
    ASSERT_TRUE(IpcConnection::answers(frame, *second));
    EXPECT_FALSE(IpcConnection::answer(frame, &ctl.error));
    EXPECT_EQ(ctl.error.code, IpcErrorCode::CallFailed);
    EXPECT_NE(ctl.error.message.find("larger than the connection can take"), std::string::npos) << ctl.error.message;
    // The connection goes on.
    EXPECT_TRUE(ctl.call("focusedPane", {})) << ctl.error.message;
    EXPECT_FALSE(w.ended);
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

// `afterglow ctl watch` for a session, also while no window shows it: the
// daemon sends the events of its panes to the subscriptions of the session,
// with the values that read access allows.
TEST(MuxControl, WatchSendsTheEventsOfASessionWithoutAWindow)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    SessionOptions options = shellOptions(daemon, "w");
    options.control.allowReadText = false;
    options.control.eventInterval = 0;
    ASSERT_TRUE(daemon.newSession(options)) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("w"), "");
    const std::int64_t pane = firstPane(ctl);
    ASSERT_NE(pane, 0);

    RawClient watcher(daemon.socket());
    ASSERT_TRUE(watcher.connected());
    const auto request = [&](Json::Object fields) {
        EXPECT_TRUE(watcher.send(Json::write(Json::Value(std::move(fields))) + "\n"));
        return Json::parse(watcher.line()).value_or(Json::Value());
    };
    EXPECT_EQ(request({{"type", "call"}, {"id", 1}, {"target", Target}, {"fn", Fn::SelectSession}, {"args", Json::Array{ProtocolVersion, "w"}}})["type"]
                  .toString(),
              "reply");
    const std::vector<std::string> signals{"titleChanged", "paneCreated", "paneClosed", "bell", "userVarChanged", "focusChanged"};
    for (std::size_t i = 0; i < signals.size(); ++i) {
        const Json::Value reply = request({{"type", "subscribe"}, {"id", std::int64_t(10 + i)}, {"target", "control"}, {"signal", signals[i]}});
        EXPECT_EQ(reply["type"].toString(), "reply") << Json::write(reply);
    }
    EXPECT_EQ(request({{"type", "subscribe"}, {"id", 30}, {"target", "control"}, {"signal", "noSuchEvent"}})["code"].toString(), "NO_SUCH_SIGNAL");

    /// The events that come until @p signal with @p first argument, by
    /// their signal: the subscription of each names it.
    std::vector<std::pair<std::string, Json::Array>> events;
    const auto until = [&](const std::string& signal, std::int64_t first) {
        for (int i = 0; i < 64; ++i) {
            const auto line = Json::parse(watcher.line());
            if (!line) return false;
            const auto id = (*line)["subscriptionId"].toInteger().value_or(0);
            if ((*line)["type"].toString() != "event" || id < 10 || id >= std::int64_t(10 + signals.size())) continue;
            const std::string& got = signals[std::size_t(id - 10)];
            events.emplace_back(got, (*line)["args"].toArray());
            if (got == signal && (*line)["args"].toArray().at(0).toInteger() == first) return true;
        }
        return false;
    };
    const auto seen = [&](const std::string& signal, const std::string& args) {
        return std::any_of(events.begin(), events.end(), [&](const auto& e) { return e.first == signal && Json::write(e.second) == args; });
    };
    // A title, a bell and a user variable, whose value read access keeps.
    ASSERT_TRUE(ctl.call("sendText", Json::Array{pane, "printf '\\033]2;watched\\007\\007\\033]1337;SetUserVar=k=dmFs\\007'\n"}))
        << ctl.error.message;
    ASSERT_TRUE(until("userVarChanged", pane));
    EXPECT_TRUE(seen("titleChanged", "[" + std::to_string(pane) + ",\"watched\"]"));
    EXPECT_TRUE(seen("bell", "[" + std::to_string(pane) + "]"));
    EXPECT_TRUE(seen("userVarChanged", "[" + std::to_string(pane) + ",\"k\",\"\"]"));
    // A pane that starts, with the focus, and that closes.
    const auto split = ctl.call("split", Json::Array{pane, "right", Json::Array{}});
    ASSERT_TRUE(split) << ctl.error.message;
    ASSERT_TRUE(split->toInteger()) << Json::write(*split);
    const std::int64_t added = *split->toInteger();
    ASSERT_TRUE(until("focusChanged", added));
    EXPECT_TRUE(seen("paneCreated", "[" + std::to_string(added) + "]"));
    // No longer the titles: a title, and the bell after it, which the
    // daemon hears after the title, give only the bell.
    EXPECT_EQ(request({{"type", "unsubscribe"}, {"id", 31}, {"subscriptionId", 10}})["type"].toString(), "reply");
    EXPECT_EQ(request({{"type", "unsubscribe"}, {"id", 32}, {"subscriptionId", 10}})["code"].toString(), "NO_SUCH_SUBSCRIPTION");
    events.clear();
    ASSERT_TRUE(ctl.call("sendText", Json::Array{pane, "printf '\\033]2;unwatched\\007\\007'\n"})) << ctl.error.message;
    ASSERT_TRUE(until("bell", pane));
    ASSERT_TRUE(ctl.call("close", Json::Array{added})) << ctl.error.message;
    ASSERT_TRUE(until("paneClosed", added));
    EXPECT_FALSE(std::any_of(events.begin(), events.end(), [](const auto& e) { return e.first == "titleChanged"; }));
}

namespace {

/// A connection that subscribes to events of session @p name, and reads them.
struct Watcher
{
    Watcher(TestDaemon& daemon, const std::string& name)
        : client(daemon.socket())
    {
        const Json::Value selected =
            request({{"type", "call"}, {"id", 1}, {"target", Target}, {"fn", Fn::SelectSession}, {"args", Json::Array{ProtocolVersion, name}}});
        EXPECT_EQ(selected["type"].toString(), "reply") << Json::write(selected);
    }
    Json::Value request(Json::Object fields)
    {
        EXPECT_TRUE(client.send(Json::write(Json::Value(std::move(fields))) + "\n"));
        return Json::parse(client.line()).value_or(Json::Value());
    }
    /// Subscribes to @p signal with request @p id: the reply, or the error.
    Json::Value subscribe(std::int64_t id, const std::string& signal)
    {
        signals[id] = signal;
        return request({{"type", "subscribe"}, {"id", id}, {"target", "control"}, {"signal", signal}});
    }
    /// The next line: an event as "signal args", or an error as "error ID CODE MESSAGE".
    std::string next()
    {
        const auto line = Json::parse(client.line());
        if (!line) return {};
        if ((*line)["type"].toString() == "error") {
            return "error " + std::to_string((*line)["id"].toInteger().value_or(0)) + " " + (*line)["code"].toString() + " "
                + (*line)["message"].toString();
        }
        return signals[(*line)["subscriptionId"].toInteger().value_or(0)] + " " + Json::write((*line)["args"]);
    }
    RawClient client;
    std::map<std::int64_t, std::string> signals;
};

} // namespace

// A session's watch: a title alone, with no window, is an event; merged
// events keep the newest; a move of `afterglow ctl` is one; a second
// subscription to an event is refused as PhosphorIpc refuses it; and the end
// of the session closes its panes and ends the subscriptions with an error.
TEST(MuxControl, WatchHearsATitleAloneMergesAndEndsWithItsSession)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    SessionOptions options = shellOptions(daemon, "t");
    options.control.allowReadText = true;
    options.control.eventInterval = 400;
    ASSERT_TRUE(daemon.newSession(options)) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("t"), "");
    const std::int64_t pane = firstPane(ctl);
    ASSERT_NE(pane, 0);
    const std::string id = std::to_string(pane);

    Watcher watcher(daemon, "t");
    for (const auto& [n, signal] : std::vector<std::pair<std::int64_t, std::string>>{{10, "titleChanged"}, {11, "paneMoved"}, {12, "paneClosed"}}) {
        EXPECT_EQ(watcher.subscribe(n, signal)["type"].toString(), "reply");
    }
    const Json::Value twice = watcher.subscribe(20, "titleChanged");
    EXPECT_EQ(twice["code"].toString(), "MALFORMED_REQUEST");
    EXPECT_EQ(twice["message"].toString(), "already subscribed to 'control.titleChanged' on this connection (subscriptionId 10)");
    EXPECT_EQ(watcher.subscribe(21, "noSuchEvent")["message"].toString(), "target 'control' has no signal 'noSuchEvent'");

    // Three titles at once: the first goes, and the last waits for the
    // interval; the one between is merged away.
    ASSERT_TRUE(ctl.call("sendText", Json::Array{pane, "printf '\\033]2;a\\007\\033]2;b\\007\\033]2;c\\007'\n"})) << ctl.error.message;
    EXPECT_EQ(watcher.next(), "titleChanged [" + id + ",\"a\"]");
    EXPECT_EQ(watcher.next(), "titleChanged [" + id + ",\"c\"]");

    // A move of `afterglow ctl`, with no window.
    const auto split = ctl.call("split", Json::Array{pane, "right", Json::Array{}});
    ASSERT_TRUE(split) << ctl.error.message;
    ASSERT_TRUE(split->toInteger()) << Json::write(*split);
    const std::int64_t added = *split->toInteger();
    ASSERT_TRUE(ctl.call("movePane", Json::Array{added, 0, "new-tab"})) << ctl.error.message;
    EXPECT_EQ(watcher.next(), "paneMoved [" + std::to_string(added) + "]");

    // The session ends: its panes close, then the subscriptions end.
    ASSERT_TRUE(daemon.call(Fn::KillSession, Json::Array{"t"})) << daemon.lastError();
    std::set<std::string> closed{watcher.next(), watcher.next()};
    EXPECT_EQ(closed, (std::set<std::string>{"paneClosed [" + id + "]", "paneClosed [" + std::to_string(added) + "]"}));
    std::set<std::string> ended{watcher.next(), watcher.next(), watcher.next()};
    EXPECT_EQ(ended, (std::set<std::string>{"error 10 NO_SUCH_SESSION session 't' ended", "error 11 NO_SUCH_SESSION session 't' ended",
                                            "error 12 NO_SUCH_SESSION session 't' ended"}));
}

// A window that turns Control.enabled off ends the subscriptions of its
// session, as a window's control target ends its own.
TEST(MuxControl, WatchEndsWhenAWindowTurnsControlOff)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "c")) << daemon.lastError();
    Watcher watcher(daemon, "c");
    EXPECT_EQ(watcher.subscribe(10, "bell")["type"].toString(), "reply");
    Window w;
    ASSERT_EQ(w.attach(daemon, "c"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    SessionSettings settings{daemon.options({"/bin/sh"}, "x").terminal, {false, true, ""}};
    ASSERT_TRUE(w.sendSettings(settings));
    // In the daemon's socket: the watcher's read waits for the daemon.
    ASSERT_TRUE(w.stream->flush(2000));
    EXPECT_EQ(watcher.next(), "error 10 NO_SUCH_TARGET no target 'control'");
}

// One pane whose program reads nothing holds only its own input: the input
// of the window's other panes goes on.
TEST(MuxAttach, APaneThatReadsNothingDoesNotStopTheInputOfTheOthers)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string ready = daemon.dir() + "/ready";
    SessionOptions o = daemon.options({"/bin/sh", "-c", "stty raw -echo; : > '" + ready + "'; exec sleep 60"});
    o.name = "two";
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return exists(ready); }));
    Window w;
    ASSERT_EQ(w.attach(daemon, "two"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    const std::string out = daemon.dir() + "/out";
    const std::string readyB = daemon.dir() + "/readyB";
    const auto other = newPane(daemon, "two", launchOf({"/bin/sh", "-c", "stty raw -echo; : > '" + readyB + "'; exec cat > '" + out + "'"}, daemon));
    ASSERT_TRUE(other) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return exists(readyB); }));
    ASSERT_TRUE(w.until([&] { return w.opened.size() == 2; }));
    // Past the 32 MiB that the first pane's PTY queues, and below what the
    // daemon holds for it before it stops reading the window's input.
    const std::size_t total = (std::size_t(32) << 20) + (Attachment::MaxHeldInput / 2);
    const std::string part(60000, 'x');
    std::size_t sent = 0;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (sent < total && std::chrono::steady_clock::now() < deadline) {
        if (w.type(part)) {
            sent += part.size();
        } else {
            w.stream->pump(20);
            w.read();
        }
    }
    ASSERT_GE(sent, total) << "the first pane's input stopped the channel early";
    // The other pane's program gets its input all the same.
    Attach::Input input;
    input.pane = *other;
    input.bytes = "hello";
    ASSERT_TRUE(w.until([&] { return w.send(input); }, 10s));
    ASSERT_TRUE(w.until([&] { return readFile(out) == "hello"; }, 10s)) << readFile(out).size() << " bytes: " << readFile(out).substr(0, 64);
}

// The tabs and splits that the next window gets after the window closed a
// pane and dragged another out: neither is in them.
TEST(MuxAttach, TheNextWindowGetsNoPaneThatClosedOrBrokeOut)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    std::uint32_t first = 0;
    {
        Window w;
        ASSERT_EQ(w.attach(daemon, "s"), "");
        ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
        first = w.pane;
        const auto second = newPane(daemon, "s", launchOf(Waiting, daemon));
        const auto third = newPane(daemon, "s", launchOf(Waiting, daemon));
        ASSERT_TRUE(second && third) << daemon.lastError();
        Layout sent;
        sent.tabs.push_back(LayoutTab{{LayoutNode{0, true, 0.25}, LayoutNode{first}, LayoutNode{*second}}, *second, *second, "work"});
        sent.tabs.push_back(LayoutTab{{LayoutNode{*third}}, *third, 0, "logs"});
        ASSERT_TRUE(w.sendLayout(sent));
        ASSERT_TRUE(w.until([&] { return w.opened.size() == 3; }));
        ASSERT_TRUE(w.closePane(*second));
        ASSERT_TRUE(w.until([&] { return w.closedPane(*second); }));
        Layout moved;
        moved.addTab(*third);
        ASSERT_TRUE(daemon.call(Fn::BreakOut, Json::Array{ProtocolVersion, "s", toJson(moved)})) << daemon.lastError();
        ASSERT_TRUE(w.until([&] { return w.closedPane(*third); }));
        ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"s"})) << daemon.lastError();
        ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached); }));
    }
    Window again;
    ASSERT_EQ(again.attach(daemon, "s"), "");
    ASSERT_TRUE(again.until([&] { return again.pane != 0; }));
    ASSERT_EQ(again.layouts.size(), 1u);
    const Layout& kept = again.layouts[0];
    ASSERT_EQ(kept.tabs.size(), 1u);
    EXPECT_EQ(kept.tabs[0].tree, std::vector<LayoutNode>{LayoutNode{first}});
    EXPECT_EQ(kept.tabs[0].focused, first);
    EXPECT_EQ(kept.tabs[0].zoomed, 0u);
    EXPECT_EQ(kept.tabs[0].title, "work");
    EXPECT_EQ(again.opened, std::vector<std::uint32_t>{first});
}

// A session has at most MaxSessionPanes panes. A window whose stream takes
// fewer channels at once gets as many panes as it takes, and keeps the
// session: the others wait for a channel.
TEST(MuxAttach, ASessionStartsNoMorePanesThanAWindowReads)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    for (std::size_t panes = 1; panes < MaxSessionPanes; ++panes) ASSERT_TRUE(newPane(daemon, "s", launchOf(Waiting, daemon))) << panes << " " << daemon.lastError();
    IpcConnection::Error error;
    EXPECT_FALSE(newPane(daemon, "s", launchOf(Waiting, daemon), &error));
    EXPECT_EQ(error.code, Error::TooManyPanes) << error.message;
    // Nor a split or a tab of `afterglow ctl`.
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("s"), "");
    for (const std::string fn : {"split", "newTab"}) {
        EXPECT_FALSE(ctl.call(fn, Json::Array{0, fn == "split" ? "right" : "", Json::Array{}}));
        EXPECT_EQ(ctl.error.code, Error::TooManyPanes) << fn << ": " << ctl.error.message;
    }
    ASSERT_EQ(sessionNamed(daemon, "s").value_or(SessionInfo{}).panes, int(MaxSessionPanes));
    // The test's stream takes the default of 64 channels, the events among them.
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    const std::size_t channels = PhosphorChannel::Stream::Options{}.maxChannels - 1;
    ASSERT_TRUE(w.until([&] { return w.opened.size() == channels; }, 15s)) << w.opened.size();
    // Each pass of the daemon tries to link every pane. The frame of a resize
    // comes from a pass after the last pane opened, and the frame of the next
    // one only from a stream that this pass kept.
    ASSERT_TRUE(w.resize(30, 8));
    ASSERT_TRUE(w.until([&] { return w.replica.cols() == 30 || w.ended; })) << w.replica.cols();
    ASSERT_FALSE(w.ended);
    ASSERT_TRUE(w.resize(32, 9));
    ASSERT_TRUE(w.until([&] { return w.replica.cols() == 32 || w.ended; })) << w.replica.cols();
    EXPECT_FALSE(w.ended);
    EXPECT_EQ(w.opened.size(), channels);
    ASSERT_EQ(w.layouts.size(), 1u);
    EXPECT_EQ(w.layouts[0].panes().size(), MaxSessionPanes);
    EXPECT_TRUE(sessionNamed(daemon, "s").value_or(SessionInfo{}).attached);
}

// A window that another one took the session from can call before it hears
// of it, and would change a session that it no longer shows: the calls that
// add panes or move them out (newPane, breakOut) name their window, and the
// daemon refuses those of a window that does not show the session. A call
// that names no window is no window's. The process does not count: through
// a proxy, each call of a window comes from another one.
TEST(MuxAttach, OnlyTheShownWindowChangesItsPanes)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    Window old;
    ASSERT_EQ(old.attach(daemon, "s"), "");
    ASSERT_TRUE(old.until([&] { return old.pane != 0; }));
    ASSERT_GT(old.serial, 0);
    const auto newPaneOf = [&](std::int64_t window, IpcConnection::Error* error) {
        return daemon.call(Fn::NewPane, Json::Array{ProtocolVersion, "s", toJson(launchOf(Waiting, daemon)), toJson(NewPaneSize), window}, error);
    };
    IpcConnection::Error error;
    // The window that shows the session may add a pane.
    const auto second = newPaneOf(old.serial, &error);
    ASSERT_TRUE(second) << error.message;
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Layout); }));
    ASSERT_NE(w.serial, old.serial);
    // The window that the session went from may not.
    EXPECT_FALSE(newPaneOf(old.serial, &error));
    EXPECT_EQ(error.code, Error::Attached);
    Layout moved;
    moved.addTab(std::uint32_t((*second)["pane"].toInteger().value_or(0)));
    EXPECT_FALSE(daemon.call(Fn::BreakOut, Json::Array{ProtocolVersion, "s", toJson(moved), old.serial}, &error));
    EXPECT_EQ(error.code, Error::Attached);
    // A window is a whole number from 1.
    EXPECT_FALSE(newPaneOf(0, &error));
    EXPECT_EQ(error.code, IpcErrorCode::InvalidArg);
    EXPECT_EQ(sessionNamed(daemon, "s").value_or(SessionInfo{}).panes, 2);
    // The window that shows it now may, and so may a call of no window.
    ASSERT_TRUE(newPaneOf(w.serial, &error)) << error.message;
    ASSERT_TRUE(newPane(daemon, "s", launchOf(Waiting, daemon), &error)) << error.message;
    EXPECT_EQ(sessionNamed(daemon, "s").value_or(SessionInfo{}).panes, 4);
    const auto broken = daemon.call(Fn::BreakOut, Json::Array{ProtocolVersion, "s", toJson(moved), w.serial}, &error);
    ASSERT_TRUE(broken) << error.message;
    EXPECT_EQ(sessionNamed(daemon, "s").value_or(SessionInfo{}).panes, 3);
    EXPECT_EQ(sessionNamed(daemon, (*broken)["name"].toString()).value_or(SessionInfo{}).panes, 1);
}

// A window acts on the panes of the session that it shows only: a close and
// a layout that name a pane of another session, and the calls that do, leave
// that pane where it is.
TEST(MuxAttach, AWindowChangesNoPaneOfAnotherSession)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::uint32_t kept = firstPaneOf(daemon, "keep");
    ASSERT_NE(kept, 0u);
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    ASSERT_TRUE(w.closePane(kept));
    Layout stolen;
    stolen.tabs.push_back(LayoutTab{{LayoutNode{0, false, 0.5}, LayoutNode{w.pane}, LayoutNode{kept}}, kept, 0, "both"});
    ASSERT_TRUE(w.sendLayout(stolen));
    Layout moved;
    moved.addTab(kept);
    IpcConnection::Error error;
    EXPECT_FALSE(daemon.call(Fn::BreakOut, Json::Array{ProtocolVersion, "s", toJson(moved)}, &error));
    EXPECT_EQ(error.code, Error::NoSuchPane) << error.message;
    EXPECT_FALSE(daemon.call(Fn::PaneJob, Json::Array{"s", std::int64_t(kept)}, &error));
    EXPECT_EQ(error.code, Error::NoSuchPane) << error.message;
    // The detach reads what the window sent before it (Attachment::catchUp).
    ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"s"})) << daemon.lastError();
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached); }));
    EXPECT_FALSE(w.closedPane(kept));
    EXPECT_EQ(sessionNamed(daemon, "keep").value_or(SessionInfo{}).panes, 1);
    const auto holder = daemon.call(Fn::PaneSession, Json::Array{std::int64_t(kept)});
    ASSERT_TRUE(holder) << daemon.lastError();
    EXPECT_EQ(holder->toString(), "keep");
    // The next window of the session gets its own pane only, with the title
    // of the tab.
    Window again;
    ASSERT_EQ(again.attach(daemon, "s"), "");
    ASSERT_TRUE(again.until([&] { return again.pane != 0; }));
    ASSERT_EQ(again.layouts.size(), 1u);
    ASSERT_EQ(again.layouts[0].tabs.size(), 1u);
    EXPECT_EQ(again.layouts[0].panes(), std::vector<std::uint32_t>{w.pane});
    EXPECT_EQ(again.layouts[0].tabs[0].title, "both");
    EXPECT_EQ(again.opened, std::vector<std::uint32_t>{w.pane});
}

// One of two panes exits while a window shows the session: the window hears
// its status, the session goes on with the other pane, and its layout loses
// the pane. The exit of the last pane ends the session.
TEST(MuxAttach, APaneThatExitsWhileShownLeavesTheOthersAndTheLastEndsTheSession)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const auto exitsOn = [](const std::string& flag, int status) {
        return std::vector<std::string>{"/bin/sh", "-c", "while [ ! -e '" + flag + "' ]; do sleep 0.05; done; exit " + std::to_string(status)};
    };
    const std::string firstGoes = daemon.dir() + "/first";
    const std::string secondGoes = daemon.dir() + "/second";
    ASSERT_TRUE(daemon.newSession(daemon.options(exitsOn(firstGoes, 4), "s"))) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    const auto second = newPane(daemon, "s", launchOf(exitsOn(secondGoes, 3), daemon));
    ASSERT_TRUE(second) << daemon.lastError();
    Layout split;
    split.tabs.push_back(LayoutTab{{LayoutNode{0, false, 0.5}, LayoutNode{w.pane}, LayoutNode{*second}}, *second, 0, "work"});
    ASSERT_TRUE(w.sendLayout(split));
    ASSERT_TRUE(w.until([&] { return w.opened.size() == 2; }));

    std::ofstream(secondGoes).flush();
    ASSERT_TRUE(w.until([&] { return w.closedPane(*second); })) << daemon.errors();
    const auto statusOf = [](const Window& window, std::uint32_t id) -> std::optional<int> {
        for (const Attach::Event& e : window.events) {
            if (e.type == Attach::EventType::PaneClosed && e.pane == id) return e.status;
        }
        return std::nullopt;
    };
    const auto status = statusOf(w, *second);
    ASSERT_TRUE(status);
    EXPECT_TRUE(WIFEXITED(*status) && WEXITSTATUS(*status) == 3) << *status;
    EXPECT_FALSE(w.got(Attach::EventType::SessionEnded));
    EXPECT_FALSE(w.ended);
    EXPECT_EQ(sessionNamed(daemon, "s").value_or(SessionInfo{}).panes, 1);
    // A window that takes the session over gets the layout without the pane.
    Window next;
    ASSERT_EQ(next.attach(daemon, "s"), "");
    ASSERT_TRUE(next.until([&] { return next.pane != 0; }));
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::TakenOver); }));
    ASSERT_EQ(next.layouts.size(), 1u);
    ASSERT_EQ(next.layouts[0].tabs.size(), 1u);
    EXPECT_EQ(next.layouts[0].tabs[0].tree, std::vector<LayoutNode>{LayoutNode{w.pane}});
    EXPECT_EQ(next.layouts[0].tabs[0].focused, w.pane);
    EXPECT_EQ(next.layouts[0].tabs[0].title, "work");
    EXPECT_EQ(next.opened, std::vector<std::uint32_t>{w.pane});

    std::ofstream(firstGoes).flush();
    ASSERT_TRUE(next.until([&] { return next.got(Attach::EventType::SessionEnded); })) << daemon.errors();
    const auto last = statusOf(next, w.pane);
    ASSERT_TRUE(last);
    EXPECT_TRUE(WIFEXITED(*last) && WEXITSTATUS(*last) == 4) << *last;
    EXPECT_EQ(next.events.back().type, Attach::EventType::SessionEnded);
    EXPECT_FALSE(sessionNamed(daemon, "s"));
}

// A pane that starts while no window shows the session is a tab of its own
// in the next window, after the tabs that the last window sent.
TEST(MuxAttach, APaneThatStartsWhileDetachedIsANewTabOfTheNextWindow)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    std::uint32_t first = 0;
    {
        Window w;
        ASSERT_EQ(w.attach(daemon, "s"), "");
        ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
        first = w.pane;
        Layout titled;
        titled.tabs.push_back(LayoutTab{{LayoutNode{first, false, 0.5}}, first, 0, "work"});
        titled.values[first] = {"shell", {}};
        ASSERT_TRUE(w.sendLayout(titled));
        ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"s"})) << daemon.lastError();
        ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached); }));
    }
    const auto later = newPane(daemon, "s", launchOf(Waiting, daemon));
    ASSERT_TRUE(later) << daemon.lastError();
    Window again;
    ASSERT_EQ(again.attach(daemon, "s"), "");
    ASSERT_TRUE(again.until([&] { return again.opened.size() == 2; }));
    ASSERT_EQ(again.layouts.size(), 1u);
    const Layout& layout = again.layouts[0];
    ASSERT_EQ(layout.tabs.size(), 2u);
    EXPECT_EQ(layout.tabs[0].tree, std::vector<LayoutNode>{(LayoutNode{first, false, 0.5})});
    EXPECT_EQ(layout.values.at(first).title, "shell");
    EXPECT_EQ(layout.tabs[0].title, "work");
    EXPECT_EQ(layout.tabs[1].tree, std::vector<LayoutNode>{LayoutNode{*later}});
    EXPECT_EQ(layout.tabs[1].focused, *later);
    EXPECT_EQ(layout.current, 0u);
}

// The actions (a Kill, a ClosePane, a Visible, a Refresh) come on the action
// channel, a Layout with the input: one on the other channel ends the
// window's stream, and the session runs on with its panes for the next window.
TEST(MuxAttach, AnInputOnTheWrongChannelLosesTheSession)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    const std::uint32_t first = firstPaneOf(daemon, "s");
    ASSERT_NE(first, 0u);
    const auto second = newPane(daemon, "s", launchOf(Waiting, daemon));
    ASSERT_TRUE(second) << daemon.lastError();
    const auto detached = [&] {
        const auto info = sessionNamed(daemon, "s");
        return info && !info->attached;
    };
    // Each action alone: a failure of one does not skip the others.
    const auto onTheInput = [&](Attach::InputType type) {
        SCOPED_TRACE(int(type));
        Window w;
        ASSERT_EQ(w.attach(daemon, "s"), "");
        ASSERT_TRUE(w.until([&] { return w.opened.size() == 2; }));
        Attach::Input action;
        action.type = type;
        action.pane = *second;
        ASSERT_EQ(w.input->tryWrite(Attach::encode(action)), PhosphorChannel::WriteResult::Written);
        ASSERT_TRUE(w.until([&] { return w.ended; }));
        EXPECT_FALSE(w.closedPane(*second));
        EXPECT_TRUE(w.refreshes.empty());
        ASSERT_TRUE(waitFor(detached));
        EXPECT_EQ(sessionNamed(daemon, "s").value_or(SessionInfo{}).panes, 2);
    };
    for (const auto type : {Attach::InputType::Kill, Attach::InputType::ClosePane, Attach::InputType::Visible, Attach::InputType::Refresh,
                            Attach::InputType::PaneForgotten}) {
        onTheInput(type);
    }
    {
        SCOPED_TRACE("a Layout on the action channel");
        Window w;
        ASSERT_EQ(w.attach(daemon, "s"), "");
        ASSERT_TRUE(w.until([&] { return w.opened.size() == 2; }));
        Layout layout;
        layout.addTab(*second);
        layout.addTab(first);
        Attach::Input message;
        message.type = Attach::InputType::Layout;
        message.bytes = Json::write(toJson(layout));
        ASSERT_EQ(w.actions->tryWrite(Attach::encode(message)), PhosphorChannel::WriteResult::Written);
        ASSERT_TRUE(w.until([&] { return w.ended; }));
        ASSERT_TRUE(waitFor(detached));
        EXPECT_EQ(sessionNamed(daemon, "s").value_or(SessionInfo{}).panes, 2);
    }
    // Neither changed the layout.
    Window again;
    ASSERT_EQ(again.attach(daemon, "s"), "");
    ASSERT_TRUE(again.until([&] { return again.opened.size() == 2; }));
    ASSERT_EQ(again.layouts.size(), 1u);
    ASSERT_EQ(again.layouts[0].tabs.size(), 2u);
    EXPECT_EQ(again.layouts[0].tabs[0].focused, first);
    EXPECT_EQ(again.layouts[0].tabs[1].focused, *second);
}

// The read of a pane that waits for the window, in the pane or in the
// daemon's queue for the window, when the pane breaks out to a session that
// no window shows: it gets the answer of such a session, as at a detach.
TEST(MuxAttach, RequestsThatWaitWhenTheirPaneBreaksOutGetTheAnswerWithoutAWindow)
{
    if (!pythonFound()) GTEST_SKIP() << "python3 is not installed";
    constexpr std::size_t mib = std::size_t(1) << 20;
    // Each size alone: a failure of the first does not skip the second.
    const auto breakOut = [](std::size_t queue) {
        TestDaemon daemon;
        ASSERT_TRUE(daemon.start()) << daemon.errors();
        const std::string answer = daemon.dir() + "/answer";
        ASSERT_TRUE(daemon.newSession(daemon.options(fillThenRead({2 * mib, 5 * mib / 2}, answer, 500), "s"))) << daemon.lastError();
        Window w;
        ASSERT_EQ(w.attach(daemon, "s", true, queue), "");
        ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
        // The pane that stays in the session.
        ASSERT_TRUE(newPane(daemon, "s", launchOf(Waiting, daemon))) << daemon.lastError();
        ASSERT_TRUE(w.until([&] { return w.opened.size() == 2; }));
        w.readEvents = false;
        ASSERT_TRUE(step(w, 2));
        // The read waits: the window got no answer to give, and the daemon
        // gave none (fillThenRead says "waited").
        ASSERT_TRUE(w.until([&] { return contains(w.screen(), "ready waited"); })) << w.screen();
        Layout moved;
        moved.addTab(w.pane);
        IpcConnection::Error error;
        ASSERT_TRUE(daemon.call(Fn::BreakOut, Json::Array{ProtocolVersion, "s", toJson(moved)}, &error)) << error.message;
        ASSERT_TRUE(w.until([&] { return exists(answer); }));
        EXPECT_EQ(readFile(answer), readRefused());
    };
    // With 1 MiB, the read waits in the pane; with 4 MiB, behind the second
    // write in the daemon's queue.
    for (const std::size_t queue : {mib, 4 * mib}) {
        SCOPED_TRACE(queue);
        breakOut(queue);
    }
}

// What the user sees of a pane, and a read of its text, are not input for
// its program: they go at once on the action channel, also while the pane's
// input waits for a program that reads nothing and holds the control
// channel.
TEST(MuxAttach, ASightAndARefreshGoPastInputThatWaitsForAFullPty)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string ready = daemon.dir() + "/ready";
    const std::string mark = daemon.dir() + "/mark";
    // Reads nothing, and prints a word when the test asks, with a title
    // that counts the words: held frames carry the title, not the screen.
    SessionOptions o = daemon.options({"/bin/sh", "-c",
                                       "stty raw -echo; : > '" + ready + "'; n=0; while :; do if [ -e '" + mark
                                           + "' ]; then n=$((n+1)); printf 'mar''ked\\033]2;t%d\\007' $n; rm '" + mark
                                           + "'; fi; sleep 0.1; done"});
    o.name = "stuck";
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return exists(ready); }));
    Window w;
    ASSERT_EQ(w.attach(daemon, "stuck"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && w.frames > 0; }));
    // Until the daemon reads the control channel no more. Parts of 2 MiB:
    // the PTY queues 16 of them (its 32 MiB, as the kernel holds less than
    // one part), the daemon holds the next two (Attachment::MaxHeldInput)
    // and then stops, and the channel's window of 4 MiB takes one more.
    // So once 19 went, the daemon read 18 and stopped: the 20th never goes.
    constexpr std::size_t PartBytes = std::size_t(2) << 20;
    static_assert(PartBytes < Attachment::MaxHeldInput && 2 * PartBytes >= Attachment::MaxHeldInput);
    const std::string part(PartBytes, 'x');
    int sent = 0;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (sent < 19 && std::chrono::steady_clock::now() < deadline) {
        if (w.type(part)) {
            ++sent;
        } else {
            w.stream->pump(20);
            w.read();
        }
    }
    ASSERT_EQ(sent, 19);
    // The 20th finds no credit at once. Only a daemon that holds the input
    // past Attachment::MaxHeldInput and reads no more gives none while the
    // stream runs.
    (void)w.until([] { return false; }, 1s);
    ASSERT_FALSE(w.type(part));
    ASSERT_TRUE(w.show(w.pane, false));
    std::ofstream(mark).put('x');
    ASSERT_TRUE(waitFor([&] { return !exists(mark); }));
    // Reads of the hidden pane: each gets its answer, with the screen as it
    // is then, until the daemon parsed the word.
    bool marked = false;
    for (std::size_t reads = 1; reads <= 20 && !marked; ++reads) {
        ASSERT_TRUE(w.refresh(w.pane));
        ASSERT_TRUE(w.until([&] { return w.refreshes.size() == reads && w.frames >= int(w.refreshes.back().frames); }, 10s)) << reads;
        marked = contains(w.screen(), "marked");
    }
    EXPECT_TRUE(marked) << w.screen();
    // Hidden: a word now changes no screen of the window. The frame that
    // brings its title came after the daemon parsed the word.
    const int screenFrames = w.screenFrames;
    std::ofstream(mark).put('x');
    ASSERT_TRUE(w.until([&] { return w.replica.title() == "t2"; }, 10s)) << w.replica.title();
    EXPECT_EQ(w.screenFrames, screenFrames);
    EXPECT_FALSE(contains(w.screen(), "markedmarked")) << w.screen();
    // Seen again: the screen comes.
    ASSERT_TRUE(w.show(w.pane, true));
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "markedmarked"); }, 10s)) << w.screen();
}

// The window says what the user sees of a pane once: a pane that links only
// later (no channel was free) starts with it.
TEST(MuxAttach, APaneThatLinksLaterStartsAsTheWindowSaid)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    // One pane more than the test's stream reads: the default of 64
    // channels, the events among them.
    const std::size_t channels = PhosphorChannel::Stream::Options{}.maxChannels - 1;
    for (std::size_t panes = 1; panes <= channels; ++panes) ASSERT_TRUE(newPane(daemon, "s", launchOf(Waiting, daemon))) << panes << " " << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.opened.size() == channels; }, 15s)) << w.opened.size();
    ASSERT_EQ(w.layouts.size(), 1u);
    std::uint32_t late = 0;
    for (const std::uint32_t id : w.layouts[0].panes()) {
        if (std::find(w.opened.begin(), w.opened.end(), id) == w.opened.end()) late = id;
    }
    ASSERT_NE(late, 0u);
    // On the action channel, which the close below takes too: the daemon
    // reads the Visible first.
    ASSERT_TRUE(w.show(late, false));
    // A pane that goes frees a channel for it.
    const std::uint32_t other = w.opened.back();
    ASSERT_NE(other, w.pane);
    ASSERT_TRUE(w.closePane(other));
    ASSERT_TRUE(w.until([&] { return w.framesOf(late) > 0; }, 10s));
    // Its first frame carried no screens, which a link that started shown
    // sends at once: the replica keeps the size it started with, not the
    // pane's.
    EXPECT_EQ(w.others.at(late)->replica.cols(), 20);
    ASSERT_TRUE(w.show(late, true));
    EXPECT_TRUE(w.until([&] { return w.others.at(late)->replica.cols() != 20; }));
}

// A read of a pane whose program floods it gets its answer once the frames
// carry the text as it was at the read: the rows that the flood adds
// meanwhile keep no read waiting, also when they fill the pane's channel in
// each pass.
TEST(MuxAttach, AReadOfAPaneThatFloodsGetsItsAnswer)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string go = daemon.dir() + "/go";
    const std::string count = daemon.dir() + "/count";
    // Once the test says: numbered lines without an end, of 1024 cells that
    // a repeat (REP) makes of a few bytes. The daemon parses rows faster
    // than the frames carry them, so its frames never catch up. Each 100
    // lines, the number of the last one goes to a file.
    SessionOptions o = daemon.options({"/bin/sh", "-c",
                                       "while [ ! -e '" + go + "' ]; do sleep 0.05; done; exec awk -v count='" + count
                                           + "' 'BEGIN { for (i = 1; ; i++) { printf \"n %d x\\033[1023b\\r\\n\", i; "
                                             "if (i % 100 == 0) { print i > count; close(count) } } }'"});
    o.name = "flood";
    // History of 40000 rows, some 8 MiB of frames: at each read, what the
    // hidden pane's frames left out takes several frames, and the flood adds
    // more meanwhile. A read waits for the rows up to it (StateWriter::
    // textMarked), never for the frames to catch up with the flood.
    o.terminal.maxScrollback = 40000;
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    // The window keeps all the history that the daemon sends.
    Window w;
    ASSERT_EQ(w.attach(daemon, "flood"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && w.frames > 0; }));
    ASSERT_TRUE(w.show(w.pane, false));
    std::ofstream(go).put('x');
    // awk rewrites the file in place: a read can find it empty, or with a
    // part of the number. The highest number read so far.
    std::uint64_t last = 0;
    const auto written = [&] {
        const std::string n = readFile(count);
        if (!n.empty() && std::isdigit(static_cast<unsigned char>(n[0]))) last = std::max<std::uint64_t>(last, std::stoull(n));
        return last;
    };
    // The highest line number on the replica's screen.
    const auto shown = [&] {
        const std::string screen = w.screen();
        std::uint64_t highest = 0;
        for (std::size_t at = 0; (at = screen.find("n ", at)) != std::string::npos; at += 2) {
            if (at + 2 < screen.size() && std::isdigit(static_cast<unsigned char>(screen[at + 2]))) {
                highest = std::max<std::uint64_t>(highest, std::stoull(screen.substr(at + 2)));
            }
        }
        return highest;
    };
    for (std::size_t reads = 1; reads <= 3; ++reads) {
        ASSERT_TRUE(w.until([&] { return written() > 0; }, 10s));
        const std::uint64_t line = written();
        // Two megabytes after it (lines of some 20 bytes), more than the PTY
        // holds: the daemon parsed that line.
        ASSERT_TRUE(w.until([&] { return written() >= line + 120000; }, 30s)) << written();
        ASSERT_TRUE(w.refresh(w.pane));
        ASSERT_TRUE(w.until([&] { return w.refreshes.size() == reads && w.frames >= int(w.refreshes.back().frames); }, 10s))
            << "read " << reads << " got no answer while the program floods";
        EXPECT_GE(shown(), line) << w.screen();
    }
}

// Each read gets its answer, also when more of them wait than the events
// channel and the queue behind it (Mux.eventQueue) take: the answers that
// do not fit wait in the daemon as a count.
TEST(MuxAttach, EveryReadGetsItsAnswerAlsoPastTheEventQueue)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s", true, std::size_t(MinEventQueue)), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && w.frames > 0; }));
    // The window reads no events: their channel fills, then the queue.
    w.readEvents = false;
    const std::size_t reads =
        (PhosphorChannel::Stream::Options{}.window + std::size_t(MinEventQueue)) / PhosphorChannel::Stream::FrameCost + 1000;
    std::size_t sent = 0;
    for (int stuck = 0; sent < reads && stuck < 500;) {
        if (w.refresh(w.pane)) {
            ++sent;
            stuck = 0;
        } else {
            ++stuck;
            w.stream->pump(20);
            w.read();
        }
    }
    ASSERT_EQ(sent, reads);
    w.readEvents = true;
    ASSERT_TRUE(w.until([&] { return w.refreshes.size() >= reads; }, 40s)) << w.refreshes.size() << " of " << reads;
    EXPECT_EQ(w.refreshes.size(), reads);
    // Each of the pane, in order: after the frames of the reads before it.
    std::uint64_t frames = 0;
    std::size_t wrong = 0;
    for (const Window::Refreshed& r : w.refreshes) {
        if (r.pane != w.pane || r.frames < frames) ++wrong;
        frames = r.frames;
    }
    EXPECT_EQ(wrong, 0u);
    EXPECT_FALSE(w.ended);
}

// What the window sent before it heard of a takeover counts for the window
// that took the session, until that one got its first layout: the late
// layout of the old window is the new one's.
TEST(MuxAttach, ALateLayoutReachesTheWindowThatTookTheSessionBeforeItsFirst)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    Window old;
    ASSERT_EQ(old.attach(daemon, "s"), "");
    ASSERT_TRUE(old.until([&] { return old.pane != 0; }));
    const auto second = newPane(daemon, "s", launchOf(Waiting, daemon));
    ASSERT_TRUE(second) << daemon.lastError();
    ASSERT_TRUE(old.until([&] { return old.opened.size() == 2; }));
    // The new window's call takes the session: the old window's attachment
    // finishes, and reads what it sends from now on as late input.
    Window taker;
    ASSERT_EQ(taker.call(daemon, "s"), "");
    Layout sent;
    sent.tabs.push_back(LayoutTab{{LayoutNode{0, true, 0.5}, LayoutNode{old.pane}, LayoutNode{*second}}, *second, 0, "late"});
    ASSERT_TRUE(old.sendLayout(sent));
    ASSERT_TRUE(old.stream->flush(2000));
    // The layout is in the socket before the new window connects: the
    // daemon reads it in the pass that sees the connection, or before, and
    // handles the old attachment first.
    ASSERT_EQ(taker.connect(), "");
    ASSERT_TRUE(taker.until([&] { return taker.opened.size() == 2; }));
    ASSERT_EQ(taker.layouts.size(), 1u);
    EXPECT_TRUE(taker.layouts[0] == sent) << Json::write(toJson(taker.layouts[0]));
    EXPECT_TRUE(old.until([&] { return old.got(Attach::EventType::TakenOver); }));
}

// A detach right after the window closed the last pane: the session ends,
// as when its last program exits, whether the daemon read the close before
// the call or with it (Attachment::catchUp).
TEST(MuxAttach, ADetachRightAfterTheLastPaneClosedEndsTheSession)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    ASSERT_TRUE(w.closePane(w.pane));
    ASSERT_TRUE(w.stream->flush(2000));
    IpcConnection::Error error;
    const bool detached = daemon.call(Fn::Detach, Json::Array{"s"}, &error).has_value();
    EXPECT_TRUE(detached || error.code == Error::NoSuchSession) << error.code << ": " << error.message;
    EXPECT_TRUE(waitFor([&] { return !sessionNamed(daemon, "s"); }));
    EXPECT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached) || w.got(Attach::EventType::SessionEnded); }));
}

// Input that waits for a full PTY goes once when the window detaches, as for
// a window that left: the bytes that the PTY has no room for are lost, and a
// clear behind them reaches the terminal (Attachment::finish).
TEST(MuxAttach, AClearThatWaitsForAFullPtyGoesWhenTheWindowDetaches)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string ready = daemon.dir() + "/ready";
    // Reads nothing.
    SessionOptions o = daemon.options({"/bin/sh", "-c", "stty raw -echo; printf 'bef''ore'; : > '" + ready + "'; exec sleep 60"});
    o.name = "s";
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return exists(ready); }));
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "before"); })) << w.screen();
    // A second pane, whose echo says that the daemon read what came before it.
    const auto echo = newPane(daemon, "s", launchOf({"/bin/cat"}, daemon));
    ASSERT_TRUE(echo) << daemon.lastError();
    ASSERT_TRUE(w.until([&] { return w.others.contains(*echo); }));
    // Past the 32 MiB that the PTY queues, and below the bound of held
    // input (Attachment::MaxHeldInput): the daemon holds the rest, and the
    // clear behind it.
    const std::string part(60000, 'x');
    ASSERT_GE(w.fill(part, std::size_t(34) << 20, 200), std::size_t(34) << 20);
    ASSERT_TRUE(w.action(Attach::InputType::Clear));
    Attach::Input ping;
    ping.pane = *echo;
    ping.bytes = "ping\n";
    ASSERT_TRUE(w.send(ping));
    const auto echoed = [&] { return w.others.at(*echo)->replica.text(AfterglowVt::Terminal::TextScope::Screen); };
    ASSERT_TRUE(w.until([&] { return contains(echoed(), "ping"); }, 10s)) << echoed();
    // Still held: the screen of the stuck pane did not change.
    ASSERT_TRUE(contains(w.screen(), "before")) << w.screen();
    ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"s"})) << daemon.lastError();
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached); }));
    // The next window shows the cleared screen.
    Window again;
    ASSERT_EQ(again.attach(daemon, "s"), "");
    ASSERT_TRUE(again.until([&] { return again.pane != 0 && again.replica.cols() != 20; })) << again.replica.cols();
    EXPECT_FALSE(contains(again.screen(), "before")) << again.screen();
}

// The input that the daemon holds for a pane that went to another session
// goes to its program when the old session ends (Attachment::finish), and so
// does what the window sends before it hears of the end (Attachment::
// readLate): as far as the program's PTY has room, as for a window that left.
// Once the PTY refused a part, the rest of that pane's input goes nowhere: a
// cut paste must not run with the Enter after it.
TEST(MuxAttach, TheInputOfAPaneThatBrokeOutGoesWhenItsOldSessionEnds)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string ready = daemon.dir() + "/ready";
    const std::string go = daemon.dir() + "/go";
    const std::string out = daemon.dir() + "/out";
    // Reads nothing until the test says, then writes what it reads to a file.
    SessionOptions o = daemon.options({"/bin/sh", "-c",
                                       "stty raw -echo; : > '" + ready + "'; while [ ! -e '" + go + "' ]; do sleep 0.05; done; exec cat > '" + out
                                           + "'"});
    o.name = "s";
    ASSERT_TRUE(daemon.newSession(o)) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return exists(ready); }));
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    // A second pane keeps the session, and its echo says that the daemon read
    // what came before it.
    const auto echo = newPane(daemon, "s", launchOf({"/bin/cat"}, daemon));
    ASSERT_TRUE(echo) << daemon.lastError();
    ASSERT_TRUE(w.until([&] { return w.others.contains(*echo); }));
    // Parts of 2 MiB: the PTY queues 16 of them (its 32 MiB, as the kernel
    // holds less than one part), and the daemon holds the 17th and the end
    // behind it, below Attachment::MaxHeldInput. The PTY's queue has room
    // for what the kernel took of it: less than a part, more than the end.
    constexpr std::size_t PartBytes = std::size_t(2) << 20;
    static_assert(PartBytes + 2 * PhosphorChannel::Stream::FrameCost + 4 < Attachment::MaxHeldInput);
    const std::string part(PartBytes, 'x');
    int sent = 0;
    const auto deadline = std::chrono::steady_clock::now() + 30s;
    while (sent < 17 && std::chrono::steady_clock::now() < deadline) {
        if (w.type(part)) {
            ++sent;
        } else {
            w.stream->pump(20);
            w.read();
        }
    }
    ASSERT_EQ(sent, 17);
    ASSERT_TRUE(w.until([&] { return w.type("END\n"); }, 10s));
    Attach::Input ping;
    ping.pane = *echo;
    ping.bytes = "ping\n";
    ASSERT_TRUE(w.until([&] { return w.send(ping); }, 10s));
    const auto echoed = [&] { return w.others.at(*echo)->replica.text(AfterglowVt::Terminal::TextScope::Screen); };
    ASSERT_TRUE(w.until([&] { return contains(echoed(), "ping"); }, 10s)) << echoed();
    Layout moved;
    moved.addTab(w.pane);
    const auto fresh = daemon.call(Fn::BreakOut, Json::Array{ProtocolVersion, "s", toJson(moved)});
    ASSERT_TRUE(fresh) << daemon.lastError();
    // The old session ends: the held part does not fit, so it and the end
    // after it are lost.
    ASSERT_TRUE(daemon.call(Fn::KillSession, Json::Array{"s"})) << daemon.lastError();
    // The window has not heard of the end yet.
    ASSERT_TRUE(w.until([&] { return w.type("LATE\n"); }, 10s));
    ASSERT_TRUE(w.stream->flush(2000));
    std::ofstream(go).put('x');
    // What the program read: the 16 parts that its PTY queued, and nothing
    // after the part that it refused.
    const auto size = [&] {
        std::error_code error;
        const auto bytes = std::filesystem::file_size(out, error);
        return error ? std::uintmax_t(0) : bytes;
    };
    ASSERT_TRUE(waitFor([&] { return size() >= 16 * PartBytes; }, 30s)) << size();
    std::uintmax_t read = 0;
    ASSERT_TRUE(waitFor(
        [&] {
            const std::uintmax_t now = size();
            const bool still = now == read;
            read = now;
            std::this_thread::sleep_for(200ms);
            return still && size() == now;
        },
        10s));
    EXPECT_LT(read, 17 * PartBytes);
    std::ifstream in(out, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(text.find_first_not_of('x'), std::string::npos) << "the program read the input after a part that its PTY refused";
    EXPECT_TRUE(sessionNamed(daemon, (*fresh)["name"].toString()));
}

namespace {

/// The values of pane @p pane in @p layout, if it has any.
std::optional<PaneValues> valuesOf(const Layout& layout, std::uint32_t pane)
{
    const auto it = layout.values.find(pane);
    return it == layout.values.end() ? std::nullopt : std::optional<PaneValues>(it->second);
}

/// A call of the control target as a line, for a RawClient.
std::string controlLine(std::int64_t id, const std::string& fn, Json::Array args, const std::string& target = "control")
{
    return Json::write(Json::Object{{"type", "call"}, {"id", id}, {"target", target}, {"fn", fn}, {"args", std::move(args)}}) + "\n";
}

/// The checks of TheCommandLinesOfAPaneNeedReadAccess, with Control.
/// allowReadText @p readText.
void commandLines(bool readText)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    SessionOptions options = shellOptions(daemon, "c");
    options.control.allowReadText = readText;
    options.control.eventInterval = 0;
    ASSERT_TRUE(daemon.newSession(options)) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("c"), "");
    const std::int64_t pane = firstPane(ctl);
    ASSERT_NE(pane, 0);
    Watcher watcher(daemon, "c");
    EXPECT_EQ(watcher.subscribe(10, "commandStarted")["type"].toString(), "reply");
    EXPECT_EQ(watcher.subscribe(11, "commandFinished")["type"].toString(), "reply");
    // The marks of shell integration (OSC 133) around a command line: the
    // command is the text between B and C.
    ASSERT_TRUE(ctl.call("sendText", Json::Array{pane, "printf '\\033]133;A\\007$ \\033]133;B\\007sec''ret\\033]133;C\\007'\n"}))
        << ctl.error.message;
    const std::string shown = readText ? "secret" : "";
    const std::string id = std::to_string(pane);
    EXPECT_EQ(watcher.next(), "commandStarted [" + id + ",\"" + shown + "\"]");
    // While it runs, `afterglow ctl ls` names it.
    const auto listed = ctl.call("listPanes", {});
    ASSERT_TRUE(listed) << ctl.error.message;
    ASSERT_EQ(listed->toArray().size(), 1u) << Json::write(*listed);
    EXPECT_EQ(listed->toArray()[0]["command"].toString(), shown);
    // Its output follows: no prompt. The command line of the process in the
    // foreground needs read access too.
    EXPECT_TRUE(listed->toArray()[0]["at_prompt"].isBool()) << Json::write(*listed);
    EXPECT_FALSE(listed->toArray()[0]["at_prompt"].toBool());
    EXPECT_GT(listed->toArray()[0]["foreground"]["pid"].toInteger().value_or(0), 0) << Json::write(*listed);
    EXPECT_EQ(listed->toArray()[0]["foreground"]["cmdline"].toArray().empty(), !readText) << Json::write(*listed);
    ASSERT_TRUE(ctl.call("sendText", Json::Array{pane, "printf '\\033]133;D;3\\007'\n"})) << ctl.error.message;
    const std::string finished = watcher.next();
    EXPECT_TRUE(finished.starts_with("commandFinished [" + id + ",\"" + shown + "\",3,")) << finished;
}

} // namespace

// The command lines of a pane leave the daemon only with read access
// (Control.allowReadText), as a window's: in `afterglow ctl ls`, and in the
// events of `afterglow ctl watch`.
TEST(MuxControl, TheCommandLinesOfAPaneNeedReadAccess)
{
    for (const bool readText : {false, true}) {
        SCOPED_TRACE(readText ? "read access" : "no read access");
        commandLines(readText);
    }
}

// A program that prints a title and exits: the watch hears the title before
// the close of its pane, and the end of a session after both.
TEST(MuxControl, WatchHearsTheLastTitleOfAProgramThatExits)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const auto titleThenExit = [](const std::string& flag) {
        return std::vector<std::string>{"/bin/sh", "-c", "while [ ! -e '" + flag + "' ]; do sleep 0.05; done; printf '\\033]2;last\\007'; exit 0"};
    };
    // Alone in its session: the session ends with it.
    const std::string alone = daemon.dir() + "/alone";
    ASSERT_TRUE(daemon.newSession(daemon.options(titleThenExit(alone), "alone"))) << daemon.lastError();
    const std::uint32_t pane = firstPaneOf(daemon, "alone");
    ASSERT_NE(pane, 0u);
    {
        Watcher watcher(daemon, "alone");
        EXPECT_EQ(watcher.subscribe(10, "titleChanged")["type"].toString(), "reply");
        EXPECT_EQ(watcher.subscribe(11, "paneClosed")["type"].toString(), "reply");
        std::ofstream(alone).put('x');
        EXPECT_EQ(watcher.next(), "titleChanged [" + std::to_string(pane) + ",\"last\"]");
        EXPECT_EQ(watcher.next(), "paneClosed [" + std::to_string(pane) + "]");
        const std::set<std::string> ended{watcher.next(), watcher.next()};
        EXPECT_EQ(ended, (std::set<std::string>{"error 10 NO_SUCH_SESSION session 'alone' ended", "error 11 NO_SUCH_SESSION session 'alone' ended"}));
    }
    // One of two panes: the session goes on.
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "two"))) << daemon.lastError();
    Watcher watcher(daemon, "two");
    EXPECT_EQ(watcher.subscribe(10, "titleChanged")["type"].toString(), "reply");
    EXPECT_EQ(watcher.subscribe(11, "paneClosed")["type"].toString(), "reply");
    const std::string second = daemon.dir() + "/second";
    const auto added = newPane(daemon, "two", launchOf(titleThenExit(second), daemon));
    ASSERT_TRUE(added) << daemon.lastError();
    std::ofstream(second).put('x');
    EXPECT_EQ(watcher.next(), "titleChanged [" + std::to_string(*added) + ",\"last\"]");
    EXPECT_EQ(watcher.next(), "paneClosed [" + std::to_string(*added) + "]");
    EXPECT_TRUE(sessionNamed(daemon, "two"));
}

// A title that waits for its interval when `afterglow ctl close` ends its
// pane still goes, before the close.
TEST(MuxControl, WatchHearsTheTitleOfAPaneBeforeItsClose)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    SessionOptions options = daemon.options(Waiting, "c");
    options.control.eventInterval = 2000;
    ASSERT_TRUE(daemon.newSession(options)) << daemon.lastError();
    Watcher watcher(daemon, "c");
    EXPECT_EQ(watcher.subscribe(10, "titleChanged")["type"].toString(), "reply");
    EXPECT_EQ(watcher.subscribe(11, "paneClosed")["type"].toString(), "reply");
    // The second title waits for the interval of the first. The daemon
    // answers the status report (DSR) once it parsed the title before it.
    const std::string ready = daemon.dir() + "/ready";
    const auto added = newPane(daemon, "c",
                               launchOf({"/bin/sh", "-c",
                                         "printf '\\033]2;a\\007'; sleep 0.2; stty raw -echo; printf '\\033]2;b\\007\\033[5n'; dd bs=1 count=4 > /dev/null 2>&1; : > '"
                                             + ready + "'; exec sleep 3600"},
                                        daemon));
    ASSERT_TRUE(added) << daemon.lastError();
    const std::string id = std::to_string(*added);
    EXPECT_EQ(watcher.next(), "titleChanged [" + id + ",\"a\"]");
    ASSERT_TRUE(waitFor([&] { return exists(ready); }));
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("c"), "");
    ASSERT_TRUE(ctl.call("close", Json::Array{std::int64_t(*added)})) << ctl.error.message;
    EXPECT_EQ(watcher.next(), "titleChanged [" + id + ",\"b\"]");
    EXPECT_EQ(watcher.next(), "paneClosed [" + id + "]");
}

// The first title of a pane that `afterglow ctl split` starts is news.
TEST(MuxControl, WatchHearsTheFirstTitleOfANewPane)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "n"))) << daemon.lastError();
    Watcher watcher(daemon, "n");
    EXPECT_EQ(watcher.subscribe(10, "titleChanged")["type"].toString(), "reply");
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("n"), "");
    const std::uint32_t first = firstPane(ctl);
    ASSERT_NE(first, 0u);
    const Launch launch = launchOf({"/bin/sh", "-c", "printf '\\033]2;first\\007'; exec sleep 3600"}, daemon);
    const auto split = ctl.call("split", Json::Array{std::int64_t(first), "right", Json::Array{}, toJson(launch)});
    ASSERT_TRUE(split) << ctl.error.message;
    ASSERT_TRUE(split->toInteger()) << Json::write(*split);
    EXPECT_EQ(watcher.next(), "titleChanged [" + std::to_string(*split->toInteger()) + ",\"first\"]");
}

// A session's watch takes subscriptions as PhosphorIpc does.
TEST(MuxControl, ASessionsWatchRefusesAsPhosphorIpcDoes)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "r")) << daemon.lastError();
    Watcher watcher(daemon, "r");
    const Json::Value none = watcher.request({{"type", "unsubscribe"}, {"id", 5}, {"subscriptionId", 1}});
    EXPECT_EQ(none["code"].toString(), "NO_SUCH_SUBSCRIPTION");
    EXPECT_EQ(none["message"].toString(), "no subscriptions on this connection");
    EXPECT_EQ(watcher.subscribe(10, "bell")["type"].toString(), "reply");
    const Json::Value taken = watcher.subscribe(10, "titleChanged");
    EXPECT_EQ(taken["code"].toString(), "MALFORMED_REQUEST");
    EXPECT_EQ(taken["message"].toString(), "subscriptionId 10 is already in use on this connection (by 'control.bell')");
    const Json::Value unknown = watcher.request({{"type", "unsubscribe"}, {"id", 6}, {"subscriptionId", 11}});
    EXPECT_EQ(unknown["code"].toString(), "NO_SUCH_SUBSCRIPTION");
    EXPECT_EQ(unknown["message"].toString(), "unknown subscriptionId 11");
}

// Without a window, the daemon refuses what a window refuses: a split with
// no room, and a call with another number of arguments. A split that names
// no program runs the login shell, not the program of the session's first
// pane.
TEST(MuxControl, TheDaemonRefusesWhatAWindowRefuses)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    SessionOptions narrow = daemon.options(Waiting, "narrow");
    narrow.size = PaneSize{3, 24, 8, 16};
    ASSERT_TRUE(daemon.newSession(narrow)) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("narrow"), "");
    EXPECT_FALSE(ctl.call("split", Json::Array{0, "right", Json::Array{}}));
    EXPECT_EQ(ctl.error.message, "not enough room for another pane");
    EXPECT_FALSE(ctl.call("focus", Json::Array{1, 2}));
    EXPECT_EQ(ctl.error.code, IpcErrorCode::InvalidArg);
    EXPECT_EQ(ctl.error.message, "argument count mismatch: 'control.focus' expects 1, got 2");
    EXPECT_FALSE(ctl.call("split", Json::Array{0, "sideways", Json::Array{}}));
    EXPECT_EQ(ctl.error.message, "direction must be left, right, up or down");
    EXPECT_EQ(sessionNamed(daemon, "narrow").value_or(SessionInfo{}).panes, 1);

    // A command launch: a split without a launch runs the login shell.
    SessionOptions plain = daemon.options(Waiting, "plain");
    plain.launch.launchedProgram = "sleep";
    plain.control.allowReadText = true;
    ASSERT_TRUE(daemon.newSession(plain)) << daemon.lastError();
    ASSERT_EQ(ctl.select("plain"), "");
    const auto split = ctl.call("split", Json::Array{0, "right", Json::Array{}});
    ASSERT_TRUE(split) << ctl.error.message;
    ASSERT_TRUE(split->toInteger()) << Json::write(*split);
    // The prompt of the shell (SHELL of the test's sessions is /bin/sh): the
    // first pane's sleep prints nothing.
    EXPECT_TRUE(screenShows(ctl, *split->toInteger(), "$")) << ctl.error.message;
}

// A split or a tab without a launch runs the shell of a shell launch
// whole, with its arguments: the shell integration of the window gives bash
// --posix and ENV together, and ENV alone would reach every sh of the pane.
TEST(MuxControl, ASplitOfAShellSessionRunsTheShellOfItsFirstLaunch)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const TempDir temp;
    // A shell with an argument that the login shell (/bin/sh) does not get.
    const std::string shell = temp.script("shell", "printf 'argument %s\\n' \"$1\"; exec /bin/sh");
    if (shell.empty()) GTEST_SKIP() << "the temporary directory runs no program";
    SessionOptions options = shellOptions(daemon, "s");
    options.launch.argv = {shell, "--posix"};
    options.control.allowReadText = true;
    ASSERT_TRUE(daemon.newSession(options)) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("s"), "");
    const std::uint32_t first = firstPane(ctl);
    ASSERT_NE(first, 0u);
    for (const char* fn : {"split", "newTab"}) {
        const auto added = ctl.call(fn, Json::Array{std::int64_t(first), fn == std::string("split") ? "right" : "", Json::Array{}});
        ASSERT_TRUE(added) << fn << ": " << ctl.error.message;
        ASSERT_TRUE(added->toInteger()) << Json::write(*added);
        std::string screen;
        EXPECT_TRUE(waitFor([&] {
            const auto text = ctl.call("getText", Json::Array{*added->toInteger(), "screen"});
            if (text) screen = text->toString();
            // getText drops the space at the end of the prompt's line.
            return contains(screen, "argument --posix") && contains(screen, "$");
        })) << fn << ": " << ctl.error.message << "\n" << screen;
    }

    // A command that is a shell (`afterglow mux new -- bash`) got the
    // integration's options and variables: a split runs the login shell
    // without them, with the values that they replaced, or without the
    // variable when there was none.
    const auto commandSplit = [&](const std::string& name, const char* hadEnv, const std::string& expected) {
        SCOPED_TRACE(name);
        SessionOptions command = shellOptions(daemon, name);
        command.launch.argv = {shell, "--posix"};
        command.launch.launchedProgram = "bash";
        command.launch.command = {"bash"};
        command.control.allowReadText = true;
        std::erase_if(command.launch.environment, [](const auto& e) { return e.first == "ENV"; });
        command.launch.environment.emplace_back("ENV", "/integration/bash");
        command.launch.environment.emplace_back("AFTERGLOW_BASH_INJECT", "1");
        command.launch.environment.emplace_back("AFTERGLOW_ORIGINAL_ENV_SET", hadEnv);
        command.launch.environment.emplace_back("AFTERGLOW_ORIGINAL_ENV", hadEnv == std::string("1") ? "/dev/null" : "");
        ASSERT_TRUE(daemon.newSession(command)) << daemon.lastError();
        ASSERT_EQ(ctl.select(name), "");
        const std::uint32_t commandPane = firstPane(ctl);
        ASSERT_NE(commandPane, 0u);
        const auto split = ctl.call("split", Json::Array{std::int64_t(commandPane), "right", Json::Array{}});
        ASSERT_TRUE(split) << ctl.error.message;
        ASSERT_TRUE(split->toInteger()) << Json::write(*split);
        std::string screen;
        const auto shows = [&](const std::string& part) {
            return waitFor([&] {
                const auto text = ctl.call("getText", Json::Array{*split->toInteger(), "screen"});
                if (text) screen = text->toString();
                return contains(screen, part);
            });
        };
        ASSERT_TRUE(shows("$")) << screen;
        ASSERT_TRUE(ctl.call("sendText", Json::Array{*split->toInteger(),
                                                     "echo \"[${ENV-none}][${AFTERGLOW_BASH_INJECT-unset}][${AFTERGLOW_ORIGINAL_ENV-unset}]\"\n"}))
            << ctl.error.message;
        // The echo of the line holds the text unexpanded; the output holds
        // it expanded.
        EXPECT_TRUE(shows(expected)) << screen;
        EXPECT_FALSE(contains(screen, "argument --posix")) << screen;
    };
    commandSplit("c", "1", "[/dev/null][unset][unset]");
    commandSplit("d", "0", "[none][unset][unset]");
}

// Without a window, `afterglow ctl move-pane` and `detach-tab` keep the
// command that a pane was started with, for the next window's project.
TEST(MuxControl, MovesWithoutAWindowKeepThePaneCommands)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("s"), "");
    const std::uint32_t first = firstPane(ctl);
    ASSERT_NE(first, 0u);
    const std::vector<std::string> command{"sleep", "60"};
    Launch launch = launchOf(Waiting, daemon);
    launch.command = command;
    const auto split = ctl.call("split", Json::Array{std::int64_t(first), "right", Json::Array{}, toJson(launch)});
    ASSERT_TRUE(split) << ctl.error.message;
    ASSERT_TRUE(split->toInteger()) << Json::write(*split);
    const std::uint32_t id = std::uint32_t(*split->toInteger());
    ASSERT_TRUE(ctl.call("movePane", Json::Array{std::int64_t(id), 0, "new-tab"})) << ctl.error.message;
    {
        Window w;
        ASSERT_EQ(w.attach(daemon, "s"), "");
        ASSERT_TRUE(w.until([&] { return w.opened.size() == 2; }));
        ASSERT_EQ(w.layouts.size(), 1u);
        EXPECT_EQ(valuesOf(w.layouts[0], id).value_or(PaneValues{}).command, command);
        ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"s"})) << daemon.lastError();
        ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached); }));
    }
    ASSERT_TRUE(ctl.call("detachTab", Json::Array{std::int64_t(id)})) << ctl.error.message;
    const auto holder = daemon.call(Fn::PaneSession, Json::Array{std::int64_t(id)});
    ASSERT_TRUE(holder) << daemon.lastError();
    ASSERT_NE(holder->toString(), "s");
    Window other;
    ASSERT_EQ(other.attach(daemon, holder->toString()), "");
    ASSERT_TRUE(other.until([&] { return other.pane != 0; }));
    ASSERT_EQ(other.layouts.size(), 1u);
    EXPECT_EQ(valuesOf(other.layouts[0], id).value_or(PaneValues{}).command, command);
}

// A window that knows no command of its panes (a window of a session of
// another host) sends their values without one: the daemon keeps its own.
// A command of the window, an empty one too, and a pane with no values
// clear it.
TEST(MuxAttach, AWindowLayoutWithoutACommandKeepsTheDaemons)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    const std::uint32_t first = firstPaneOf(daemon, "s");
    ASSERT_NE(first, 0u);
    const std::vector<std::string> command{"sleep", "60"};
    Launch launch = launchOf(Waiting, daemon);
    launch.command = command;
    const auto a = newPane(daemon, "s", launch);
    const auto b = newPane(daemon, "s", launch);
    ASSERT_TRUE(a && b) << daemon.lastError();
    Layout sent;
    sent.addTab(first);
    sent.addTab(*a);
    sent.addTab(*b);
    // A window sends @p layout, and the next window gets what the daemon kept.
    const auto through = [&](const Layout& layout) -> std::optional<Layout> {
        {
            Window w;
            if (w.attach(daemon, "s") != "" || !w.until([&] { return w.opened.size() == 3; }) || !w.sendLayout(layout)
                || !daemon.call(Fn::Detach, Json::Array{"s"}) || !w.until([&] { return w.got(Attach::EventType::Detached); })) {
                ADD_FAILURE() << "the layout did not go: " << daemon.lastError();
                return std::nullopt;
            }
        }
        Window again;
        if (again.attach(daemon, "s") != "" || !again.until([&] { return again.opened.size() == 3; }) || again.layouts.size() != 1
            || !daemon.call(Fn::Detach, Json::Array{"s"}) || !again.until([&] { return again.got(Attach::EventType::Detached); })) {
            ADD_FAILURE() << "no window got the layout: " << daemon.lastError();
            return std::nullopt;
        }
        return again.layouts[0];
    };
    Layout keeping = sent;
    keeping.values[*a] = PaneValues{"", {}, true};
    keeping.values[*b] = PaneValues{"b", {}, true};
    const auto kept = through(keeping);
    ASSERT_TRUE(kept);
    EXPECT_TRUE(valuesOf(*kept, *a) == (PaneValues{"", command, false})) << Json::write(toJson(*kept));
    EXPECT_TRUE(valuesOf(*kept, *b) == (PaneValues{"b", command, false})) << Json::write(toJson(*kept));
    Layout clearing = sent;
    clearing.values[*a] = PaneValues{"t", {}, false};
    const auto cleared = through(clearing);
    ASSERT_TRUE(cleared);
    EXPECT_TRUE(valuesOf(*cleared, *a) == (PaneValues{"t", {}, false})) << Json::write(toJson(*cleared));
    EXPECT_FALSE(valuesOf(*cleared, *b)) << Json::write(toJson(*cleared));
}

// The same for the panes that a window drags out of a session without
// their commands (Fn::BreakOut).
TEST(MuxAttach, PanesThatBreakOutWithoutACommandKeepTheDaemons)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    const std::vector<std::string> command{"sleep", "60"};
    Launch launch = launchOf(Waiting, daemon);
    launch.command = command;
    const auto moving = newPane(daemon, "s", launch);
    ASSERT_TRUE(moving) << daemon.lastError();
    Layout moved;
    moved.addTab(*moving);
    moved.values[*moving] = PaneValues{"", {}, true};
    const auto fresh = daemon.call(Fn::BreakOut, Json::Array{ProtocolVersion, "s", toJson(moved)});
    ASSERT_TRUE(fresh) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, (*fresh)["name"].toString()), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    ASSERT_EQ(w.layouts.size(), 1u);
    EXPECT_TRUE(valuesOf(w.layouts[0], *moving) == (PaneValues{"", command, false})) << Json::write(toJson(w.layouts[0]));
}

// The calls that the daemon passes a window stop at its Mux.eventQueue, as
// the lines of command clients do: past it, a call is refused at once. At a
// detach, the calls whose events still wait in the daemon fail, and never
// reach the window.
TEST(MuxControl, ForwardedCallsStopAtTheEventQueueAndThoseThatWaitDoNotGo)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "s")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s", true, std::size_t(MinEventQueue)), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    // The window reads no more events: the channel's window fills, then the queue.
    w.readEvents = false;
    RawClient client(daemon.socket());
    ASSERT_TRUE(client.connected());
    constexpr int Calls = 200;
    const std::string title(std::size_t(64) << 10, 't');
    std::string lines = controlLine(1, Fn::SelectSession, Json::Array{ProtocolVersion, "s"}, Target);
    for (int i = 0; i < Calls; ++i) lines += controlLine(100 + i, "notify", Json::Array{std::int64_t(w.pane), title, ""});
    // A call that the daemon answers itself marks the end of the others.
    lines += controlLine(999, "listPanes", {});
    ASSERT_TRUE(client.send(lines));
    std::size_t refused = 0;
    bool selected = false;
    bool marked = false;
    for (int i = 0; i < Calls + 2 && !marked; ++i) {
        const auto line = Json::parse(client.line());
        ASSERT_TRUE(line) << "the answers ended after " << i;
        const std::int64_t id = (*line)["id"].toInteger().value_or(0);
        if (id == 1) selected = (*line)["type"].toString() == "reply";
        if (id == 999) marked = true;
        if (id >= 100 && id < 100 + Calls) {
            EXPECT_NE((*line)["message"].toString().find("has not taken the calls before this one"), std::string::npos) << Json::write(*line);
            ++refused;
        }
    }
    ASSERT_TRUE(selected);
    ASSERT_TRUE(marked);
    const std::size_t accepted = std::size_t(Calls) - refused;
    EXPECT_GT(refused, 0u);
    EXPECT_GT(accepted, 0u);
    // No more than the channel's window and the queue hold, and one past.
    EXPECT_LE(accepted, (PhosphorChannel::Stream::Options{}.window + std::size_t(MinEventQueue)) / title.size() + 4);
    // The detach fails the calls that the window did not answer. The window
    // reads again at once: the daemon gives it FinishWaitMs for the events
    // in the channel and the last one.
    ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"s"})) << daemon.lastError();
    w.readEvents = true;
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached); }, 10s));
    for (std::size_t i = 0; i < accepted; ++i) {
        const auto line = Json::parse(client.line());
        ASSERT_TRUE(line) << "no answer to " << accepted - i << " calls";
        EXPECT_NE((*line)["message"].toString().find("closed before it answered"), std::string::npos) << Json::write(*line);
    }
    std::size_t received = 0;
    for (const Json::Value& call : w.controls()) received += call["fn"].toString() == "notify" ? 1 : 0;
    EXPECT_LT(received, accepted);
}

// A window that answers and ends at once: the answer still reaches the client.
TEST(MuxControl, AnAnswerThatComesWithTheWindowsEndReachesItsClient)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "s")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("s"), "");
    std::optional<Json::Value> ran;
    std::atomic<bool> answered{false};
    std::jthread caller([&] {
        ran = ctl.call("runAction", Json::Array{std::int64_t(w.pane), "x"});
        answered = true;
    });
    ASSERT_TRUE(w.until([&] { return w.controls().size() == 1; }));
    Attach::Input reply;
    reply.type = Attach::InputType::ControlReply;
    reply.bytes = Json::write(Json::Object{{"id", w.controls().back()["id"]}, {"result", nullptr}});
    ASSERT_TRUE(w.send(reply));
    ASSERT_TRUE(w.stream->flush(2000));
    // The end of the window follows its answer in the socket.
    w.stream.reset();
    EXPECT_TRUE(waitFor([&] { return answered.load(); }));
    caller.join();
    EXPECT_TRUE(ran) << ctl.error.message;
}

// A client that ends its side after its calls still gets the answer that a
// window gives, and the events of its subscriptions until they end.
TEST(MuxControl, AHalfClosedClientGetsItsForwardedAnswerAndItsEvents)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "s")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "$ "); })) << w.screen();
    {
        RawClient client(daemon.socket());
        ASSERT_TRUE(client.connected());
        ASSERT_TRUE(client.send(controlLine(1, Fn::SelectSession, Json::Array{ProtocolVersion, "s"}, Target)
                                + controlLine(2, "setTitle", Json::Array{std::int64_t(w.pane), "x"})));
        ASSERT_TRUE(client.endSending());
        EXPECT_EQ(Json::parse(client.line()).value_or(Json::Value())["type"].toString(), "reply");
        ASSERT_TRUE(w.until([&] { return w.controls().size() == 1; }));
        Attach::Input reply;
        reply.type = Attach::InputType::ControlReply;
        reply.bytes = Json::write(Json::Object{{"id", w.controls().back()["id"]}, {"result", "done"}});
        ASSERT_TRUE(w.send(reply));
        ASSERT_TRUE(w.stream->flush(2000));
        const auto answer = Json::parse(client.line());
        ASSERT_TRUE(answer);
        EXPECT_EQ((*answer)["id"].toInteger(), 2) << Json::write(*answer);
        EXPECT_EQ((*answer)["result"].toString(), "done") << Json::write(*answer);
        EXPECT_EQ(client.line(), "");
        EXPECT_TRUE(client.ended());
    }
    RawClient client(daemon.socket());
    ASSERT_TRUE(client.connected());
    ASSERT_TRUE(client.send(controlLine(1, Fn::SelectSession, Json::Array{ProtocolVersion, "s"}, Target)
                            + Json::write(Json::Object{{"type", "subscribe"}, {"id", 10}, {"target", "control"}, {"signal", "titleChanged"}})
                            + "\n"));
    ASSERT_TRUE(client.endSending());
    EXPECT_EQ(Json::parse(client.line()).value_or(Json::Value())["type"].toString(), "reply");
    EXPECT_EQ(Json::parse(client.line()).value_or(Json::Value())["type"].toString(), "reply");
    ASSERT_TRUE(w.type("printf '\\033]2;half\\007'\n"));
    ASSERT_TRUE(w.stream->flush(2000));
    const auto event = Json::parse(client.line());
    ASSERT_TRUE(event);
    EXPECT_EQ(Json::write((*event)["args"]), "[" + std::to_string(w.pane) + ",\"half\"]") << Json::write(*event);
    ASSERT_TRUE(daemon.call(Fn::KillSession, Json::Array{"s"})) << daemon.lastError();
    const auto ended = Json::parse(client.line());
    ASSERT_TRUE(ended);
    EXPECT_EQ((*ended)["code"].toString(), Error::NoSuchSession) << Json::write(*ended);
    EXPECT_EQ(client.line(), "");
    EXPECT_TRUE(client.ended());
}

// A window's attach call whose stream came with it is refused: the window
// that shows the session keeps it.
TEST(MuxAttach, ATakeoverWhoseStreamComesWithTheCallLeavesTheSession)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh"), "");
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "$ "); })) << w.screen();
    RawClient raw(daemon.socket());
    ASSERT_TRUE(raw.connected());
    const std::string call = Json::write(Json::Object{{"type", "call"}, {"id", 1}, {"target", Target}, {"fn", Fn::Attach},
                                                      {"args", Json::Array{ProtocolVersion, "sh", Json::Object{{AttachOption::Takeover, true}}}}});
    ASSERT_TRUE(raw.send(call + "\nstream bytes"));
    EXPECT_EQ(Json::parse(raw.line()).value_or(Json::Value())["type"].toString(), "reply");
    EXPECT_TRUE(waitFor([&] { return contains(daemon.errors(), "sent its stream before the answer"); })) << daemon.errors();
    ASSERT_TRUE(w.type("echo x''y\n"));
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "\nxy"); })) << w.screen();
    EXPECT_FALSE(w.got(Attach::EventType::TakenOver));
    EXPECT_FALSE(w.ended);
}

// A Kill of the window that the daemon has not read when a detach comes:
// the detach reads it first (Attachment::catchUp), and the session ends.
TEST(MuxAttach, AKillRightBeforeADetachEndsTheSession)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    ASSERT_TRUE(w.action(Attach::InputType::Kill));
    ASSERT_TRUE(w.stream->flush(2000));
    IpcConnection::Error error;
    const bool detached = daemon.call(Fn::Detach, Json::Array{"s"}, &error).has_value();
    EXPECT_TRUE(detached || error.code == Error::NoSuchSession) << error.code << ": " << error.message;
    EXPECT_TRUE(waitFor([&] { return !sessionNamed(daemon, "s"); }));
}

// A command client in a session pane (`aglow run`): the daemon passes its
// lines to the window, and the window's bytes back as whole lines. What the
// window sends a client after it heard of a detach, the end of a run, still
// reaches it, and then its connection closes.
TEST(MuxAttach, ACommandClientOfASessionPaneReachesTheWindow)
{
    if (!pythonFound()) GTEST_SKIP() << "python3 is not installed";
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    PaneAgent agent(daemon, "s");
    ASSERT_TRUE(agent.started) << daemon.lastError();
    const std::uint32_t pane = firstPaneOf(daemon, "s");
    ASSERT_NE(pane, 0u);
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    const std::string request = agent.callLine("list", {}, AfterglowProto::Run::Target);
    ASSERT_TRUE(agent.send(request));
    ASSERT_TRUE(w.until([&] { return w.relays.size() >= 2; }));
    const Attach::RelayFrame open = w.relays[0];
    EXPECT_EQ(open.part, Attach::RelayPart::Open);
    EXPECT_EQ(open.pane, pane);
    EXPECT_EQ(open.kind, Attach::RelayKind::Commands);
    EXPECT_EQ(w.relays[1].part, Attach::RelayPart::Bytes);
    EXPECT_EQ(w.relays[1].relay, open.relay);
    EXPECT_EQ(w.relays[1].bytes, request + "\n");
    // A line in two parts reaches the client whole.
    ASSERT_TRUE(w.relay(Attach::RelayFrame{open.relay, Attach::RelayPart::Bytes, R"({"type":"reply","id":1,)"}));
    ASSERT_TRUE(w.relay(Attach::RelayFrame{open.relay, Attach::RelayPart::Bytes, "\"result\":[]}\n"}));
    ASSERT_TRUE(w.stream->flush(2000));
    EXPECT_EQ(agent.next(), R"({"type":"reply","id":1,"result":[]})");
    // A run that the window serves when the session detaches.
    const std::string run = agent.callLine("run", {}, AfterglowProto::Run::Target);
    ASSERT_TRUE(agent.send(run));
    ASSERT_TRUE(w.until([&] { return w.relays.size() >= 3; }));
    EXPECT_EQ(w.relays[2].relay, open.relay);
    EXPECT_EQ(w.relays[2].bytes, run + "\n");
    ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"s"})) << daemon.lastError();
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached); }));
    ASSERT_TRUE(w.relay(Attach::RelayFrame{open.relay, Attach::RelayPart::Bytes, "last\n"}));
    ASSERT_TRUE(w.relay(Attach::RelayFrame{open.relay, Attach::RelayPart::End}));
    ASSERT_TRUE(w.stream->flush(2000));
    EXPECT_EQ(agent.next(), "last");
}

namespace {

/// The line of a call of the commands target of `aglow run`, which the daemon
/// passes to the window of the caller's session.
std::string commandsLine()
{
    return Json::write(Json::Object{{"type", "call"}, {"id", 1}, {"target", AfterglowProto::Run::Target}, {"fn", "list"}, {"args", Json::Array{}}});
}

/// The program of a session pane that waits until @p go exists, then
/// opens @p clients connections to the daemon of its pane (AFTERGLOW_SOCKET)
/// and sends @p line on each. It writes the first line that each client got
/// within three seconds to @p out, one line for each client that got one,
/// and keeps the connections open until the test ends.
std::vector<std::string> commandClients(const std::string& go, const std::string& out, const std::string& line, int clients)
{
    static const std::string script = R"(import os, select, socket, sys, time
while not os.path.exists(sys.argv[1]):
    time.sleep(0.02)
clients = []
for i in range(int(sys.argv[4])):
    s = socket.socket(socket.AF_UNIX)
    s.connect(os.environ['AFTERGLOW_SOCKET'])
    s.sendall(sys.argv[3].encode() + b'\n')
    clients.append(s)
lines = []
waiting = list(clients)
deadline = time.time() + 3
while waiting and time.time() < deadline:
    ready, _, _ = select.select(waiting, [], [], 0.1)
    for s in ready:
        waiting.remove(s)
        lines.append(s.recv(65536).split(b'\n')[0].decode(errors='replace'))
with open(sys.argv[2] + '.part', 'w') as f:
    f.write(''.join(l + '\n' for l in lines))
os.rename(sys.argv[2] + '.part', sys.argv[2])
time.sleep(3600)
)";
    return {"python3", "-I", "-c", script, go, out, line, std::to_string(clients)};
}

std::ptrdiff_t relayParts(const Window& w, Attach::RelayPart part)
{
    return std::count_if(w.relays.begin(), w.relays.end(), [part](const Attach::RelayFrame& f) { return f.part == part; });
}

} // namespace

// The window of a session serves at most 64 command clients at a time
// (Daemon::MaxRelaysPerSession): the next one hears why, and its window
// hears nothing of it.
TEST(MuxAttach, TheWindowOfASessionServesAtMost64CommandClients)
{
    if (!pythonFound()) GTEST_SKIP() << "python3 is not installed";
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string go = daemon.dir() + "/go";
    const std::string out = daemon.dir() + "/answers";
    ASSERT_TRUE(daemon.newSession(daemon.options(commandClients(go, out, commandsLine(), 65), "s"))) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    std::ofstream(go).put('x');
    ASSERT_TRUE(w.until([&] { return exists(out) && relayParts(w, Attach::RelayPart::Open) >= 64; }, 15s))
        << relayParts(w, Attach::RelayPart::Open);
    // Two passes later, nothing more came for the client that was refused.
    ASSERT_TRUE(w.roundTrip());
    EXPECT_EQ(relayParts(w, Attach::RelayPart::Open), 64);
    const std::string answers = readFile(out);
    EXPECT_EQ(std::count(answers.begin(), answers.end(), '\n'), 1) << answers;
    EXPECT_TRUE(contains(answers, "session 's' has 64 command clients already")) << answers;
}

// A command client that closes its connection ends the window's side of it:
// the window hears the end of the relay.
TEST(MuxAttach, TheWindowHearsTheEndOfACommandClient)
{
    if (!pythonFound()) GTEST_SKIP() << "python3 is not installed";
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string go = daemon.dir() + "/go";
    const std::string end = daemon.dir() + "/end";
    const std::string script = R"(import os, socket, sys, time
def wait(path):
    while not os.path.exists(path):
        time.sleep(0.02)
wait(sys.argv[1])
s = socket.socket(socket.AF_UNIX)
s.connect(os.environ['AFTERGLOW_SOCKET'])
s.sendall(sys.argv[3].encode() + b'\n')
wait(sys.argv[2])
s.close()
time.sleep(3600)
)";
    ASSERT_TRUE(daemon.newSession(daemon.options({"python3", "-I", "-c", script, go, end, commandsLine()}, "s"))) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    std::ofstream(go).put('x');
    ASSERT_TRUE(w.until([&] { return w.relays.size() >= 2; }));
    const std::uint64_t relay = w.relays[0].relay;
    EXPECT_EQ(w.relays[0].part, Attach::RelayPart::Open);
    EXPECT_EQ(w.relays[1].part, Attach::RelayPart::Bytes);
    EXPECT_EQ(relayParts(w, Attach::RelayPart::End), 0);
    std::ofstream(end).put('x');
    ASSERT_TRUE(w.until([&] { return relayParts(w, Attach::RelayPart::End) > 0; }));
    EXPECT_EQ(w.relays.back().part, Attach::RelayPart::End);
    EXPECT_EQ(w.relays.back().relay, relay);
}

// While the events of the window fill Mux.eventQueue, the daemon reads no
// more lines of its command clients: a client that sends without a pause
// waits, and every line reaches the window whole and in order once it reads
// again.
TEST(MuxAttach, ACommandClientWaitsWhileTheWindowHasNoRoom)
{
    if (!pythonFound()) GTEST_SKIP() << "python3 is not installed";
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string go = daemon.dir() + "/go";
    const std::string stuck = daemon.dir() + "/stuck";
    const std::string out = daemon.dir() + "/lines";
    // Numbered lines of 1 KiB until the daemon took none for a second, or
    // 32 MiB went: the bytes that went then, in stuck. Then the line that
    // went in part goes whole, and the number of lines goes to out.
    const std::string script = R"(import os, select, socket, sys, time
while not os.path.exists(sys.argv[1]):
    time.sleep(0.02)
s = socket.socket(socket.AF_UNIX)
s.connect(os.environ['AFTERGLOW_SOCKET'])
s.sendall(sys.argv[4].encode() + b'\n')
s.setblocking(False)
pad = b'x' * 1000
lines = 0
pending = b''
total = 0
while total < 32 << 20:
    if not pending:
        pending = b'%08d ' % lines + pad + b'\n'
        lines += 1
    try:
        sent = s.send(pending)
    except BlockingIOError:
        if not select.select([], [s], [], 1)[1]:
            break
        continue
    total += sent
    pending = pending[sent:]
with open(sys.argv[2] + '.part', 'w') as f:
    f.write(str(total))
os.rename(sys.argv[2] + '.part', sys.argv[2])
s.setblocking(True)
s.sendall(pending)
with open(sys.argv[3] + '.part', 'w') as f:
    f.write(str(lines))
os.rename(sys.argv[3] + '.part', sys.argv[3])
time.sleep(3600)
)";
    ASSERT_TRUE(daemon.newSession(daemon.options({"python3", "-I", "-c", script, go, stuck, out, commandsLine()}, "s"))) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s", true, std::size_t(MinEventQueue)), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    w.readEvents = false;
    std::ofstream(go).put('x');
    ASSERT_TRUE(w.until([&] { return exists(stuck); }, 30s));
    // Mux.eventQueue, the lines of one pass and the buffers between: far
    // below what a client that nothing stops sends.
    const std::uint64_t sent = std::stoull(readFile(stuck));
    EXPECT_LT(sent, std::uint64_t(16) << 20);
    w.readEvents = true;
    const auto received = [&] {
        std::string bytes;
        for (const Attach::RelayFrame& f : w.relays) {
            if (f.part == Attach::RelayPart::Bytes) bytes += f.bytes;
        }
        return bytes;
    };
    ASSERT_TRUE(w.until([&] { return exists(out) && received().size() >= commandsLine().size() + 1 + std::stoull(readFile(out)) * 1010; }, 30s));
    const std::uint64_t lines = std::stoull(readFile(out));
    std::string expected = commandsLine() + "\n";
    char number[24];
    for (std::uint64_t i = 0; i < lines; ++i) {
        std::snprintf(number, sizeof number, "%08llu ", static_cast<unsigned long long>(i));
        expected += number + std::string(1000, 'x') + "\n";
    }
    const std::string bytes = received();
    EXPECT_EQ(bytes.size(), expected.size());
    EXPECT_TRUE(bytes == expected);
    EXPECT_EQ(relayParts(w, Attach::RelayPart::End), 0);
}

namespace {

/// The tab of pane @p id that listPanes names, -1 when it names no such pane.
std::int64_t tabOf(ControlClient& ctl, std::uint32_t id)
{
    const auto listed = ctl.call("listPanes", {});
    if (!listed) return -1;
    for (const Json::Value& pane : listed->toArray()) {
        if (pane["id"].toInteger() == std::int64_t(id)) return pane["tab"].toInteger().value_or(-1);
    }
    return -1;
}

} // namespace

// Without a window, the daemon refuses a pane argument that is no pane ID, a
// pane of another session, a request that is no call, a scope and a launch
// that it does not know. It reads each scope of a pane's text, and starts a
// tab in the directory that the call names.
TEST(MuxControl, TheDaemonChecksTheArgumentsOfItsControlCalls)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(readableShell(daemon, "s")) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("s"), "");
    const std::uint32_t first = firstPane(ctl);
    ASSERT_NE(first, 0u);
    const std::uint32_t kept = firstPaneOf(daemon, "keep");
    ASSERT_NE(kept, 0u);
    const auto pane = std::int64_t(first);

    for (const Json::Value& bad : {Json::Value("1"), Json::Value(std::int64_t(-1)), Json::Value(std::int64_t(1) << 32)}) {
        SCOPED_TRACE(Json::write(bad));
        EXPECT_FALSE(ctl.call("sendText", Json::Array{bad, "x"}));
        EXPECT_EQ(ctl.error.code, IpcErrorCode::InvalidArg);
        EXPECT_EQ(ctl.error.message, "a pane ID is a whole number from 0 to 4294967295");
    }
    EXPECT_FALSE(ctl.call("focus", Json::Array{std::int64_t(kept)}));
    EXPECT_EQ(ctl.error.code, IpcErrorCode::CallFailed);
    EXPECT_EQ(ctl.error.message, "no pane " + std::to_string(kept) + " in session 's'");

    // A request of another type than call or subscribe, on a connection that
    // selected the session.
    RawClient raw(daemon.socket());
    ASSERT_TRUE(raw.connected());
    ASSERT_TRUE(raw.send(Json::write(Json::Object{{"type", "call"}, {"id", 1}, {"target", Target}, {"fn", Fn::SelectSession},
                                                  {"args", Json::Array{ProtocolVersion, "s"}}})
                         + "\n"));
    EXPECT_EQ(Json::parse(raw.line()).value_or(Json::Value())["type"].toString(), "reply");
    ASSERT_TRUE(raw.send(Json::write(Json::Object{{"type", "event"}, {"id", 2}, {"target", "control"}, {"fn", "listPanes"}}) + "\n"));
    const Json::Value other = Json::parse(raw.line()).value_or(Json::Value());
    EXPECT_EQ(other["code"].toString(), IpcErrorCode::MalformedRequest) << Json::write(other);
    EXPECT_EQ(other["message"].toString(), "the control target answers calls, not 'event'");

    // The scopes of a pane's text. A selection is a window's: refused without one.
    ASSERT_TRUE(ctl.call("sendText", Json::Array{pane, "echo mar''k\n"})) << ctl.error.message;
    EXPECT_TRUE(waitFor([&] {
        const auto text = ctl.call("getText", Json::Array{pane, "scrollback"});
        return text && contains(text->toString(), "\nmark");
    })) << ctl.error.message;
    const auto last = ctl.call("getText", Json::Array{pane, "last-output"});
    ASSERT_TRUE(last) << ctl.error.message;
    EXPECT_TRUE(last->isString());
    EXPECT_FALSE(ctl.call("getText", Json::Array{pane, "selection"}));
    EXPECT_EQ(ctl.error.code, IpcErrorCode::CallFailed);
    EXPECT_EQ(ctl.error.message, "only a window that shows session 's' has its selection");
    EXPECT_FALSE(ctl.call("getText", Json::Array{pane, "everything"}));
    EXPECT_EQ(ctl.error.code, IpcErrorCode::CallFailed);
    EXPECT_EQ(ctl.error.message, "scope is screen, scrollback, selection, last-output, first-output-on-screen or last-visited-output");

    // A launch that is no launch.
    EXPECT_FALSE(ctl.call("split", Json::Array{pane, "right", Json::Array{}, "sh"}));
    EXPECT_EQ(ctl.error.code, IpcErrorCode::InvalidArg) << ctl.error.message;
    EXPECT_EQ(sessionNamed(daemon, "s").value_or(SessionInfo{}).panes, 1);

    // A tab in a directory: its program starts there.
    const TempDir temp;
    ASSERT_FALSE(temp.path.empty());
    const std::string directory = std::filesystem::canonical(temp.path).string();
    const auto tab = ctl.call("newTab", Json::Array{pane, directory, Json::Array{}});
    ASSERT_TRUE(tab) << ctl.error.message;
    ASSERT_TRUE(tab->toInteger()) << Json::write(*tab);
    EXPECT_TRUE(waitFor([&] {
        const auto listed = ctl.call("listPanes", {});
        if (!listed) return false;
        for (const Json::Value& p : listed->toArray()) {
            if (p["id"].toInteger() == tab->toInteger()) return p["cwd"].toString() == directory;
        }
        return false;
    })) << ctl.error.message;
}

// A program that reads nothing fills its input queue: the text that does
// not fit is refused, with the words of a window.
TEST(MuxControl, TextForAProgramThatReadsNothingStopsAtItsQueue)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "q"))) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("q"), "");
    // Below the limit of a request (IpcConnection::MaxRequestBytes): the
    // queue of the PTY (Pty::MaxPendingInput, 32 MiB) takes 32 of them.
    const std::string part(1000 * 1024, 'a');
    int written = 0;
    while (written < 40 && ctl.call("sendText", Json::Array{0, part})) ++written;
    EXPECT_GE(written, 30);
    EXPECT_LT(written, 40);
    EXPECT_EQ(ctl.error.code, IpcErrorCode::CallFailed);
    EXPECT_EQ(ctl.error.message, "program closed or input queue full");
}

// Without a window, `afterglow ctl move-pane` places a pane beside another
// one or in a new session, and refuses what a window refuses. A window that
// shows the session moves its panes itself, but a target that is no pane of
// the session never reaches it.
TEST(MuxControl, MovesWithoutAWindowPlaceThePaneOrRefuseAsAWindowDoes)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "m"))) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("m"), "");
    const std::uint32_t first = firstPane(ctl);
    ASSERT_NE(first, 0u);
    const Launch launch = launchOf(Waiting, daemon);
    const auto add = [&](const char* fn, const char* where) -> std::uint32_t {
        const auto added = ctl.call(fn, Json::Array{std::int64_t(first), where, Json::Array{}, toJson(launch)});
        if (!added || !added->toInteger()) {
            ADD_FAILURE() << fn << ": " << ctl.error.message;
            return 0;
        }
        return std::uint32_t(*added->toInteger());
    };
    const std::uint32_t right = add("split", "right");
    const std::uint32_t tab = add("newTab", "");
    ASSERT_NE(right, 0u);
    ASSERT_NE(tab, 0u);
    EXPECT_EQ(tabOf(ctl, tab), 1);

    struct Row
    {
        Json::Array args;
        std::string code;
        std::string message;
    };
    const auto id = [](std::uint32_t pane) { return std::int64_t(pane); };
    const std::vector<Row> rows{
        {{id(right), 0}, IpcErrorCode::InvalidArg, "movePane takes (pane, target, placement)"},
        {{id(right), id(first), "sideways"}, IpcErrorCode::CallFailed, "placement must be left, right, up, down, new-tab or new-window"},
        {{id(right), 0, "down"}, IpcErrorCode::CallFailed, "a target pane is required"},
        {{id(right), id(first), "new-window"}, IpcErrorCode::CallFailed, "new-window takes no target pane"},
        {{id(right), id(right), "left"}, IpcErrorCode::CallFailed, "cannot move a pane beside itself"},
        {{id(right), 9999, "left"}, IpcErrorCode::CallFailed, "no pane 9999 in session 'm'"},
        {{id(tab), 0, "new-tab"}, IpcErrorCode::CallFailed, "The pane is alone in its tab already"},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(Json::write(Json::Value(row.args)));
        EXPECT_FALSE(ctl.call("movePane", row.args));
        EXPECT_EQ(ctl.error.code, row.code);
        EXPECT_EQ(ctl.error.message, row.message);
    }
    EXPECT_EQ(sessionNamed(daemon, "m").value_or(SessionInfo{}).panes, 3);

    // Beside a pane of another tab: the tab of the pane that it went to.
    const auto beside = ctl.call("movePane", Json::Array{id(tab), id(right), "down"});
    ASSERT_TRUE(beside) << ctl.error.message;
    EXPECT_EQ((*beside)["tab"].toInteger(), 0);
    EXPECT_EQ(tabOf(ctl, tab), 0);
    // To a new window: a new session that holds it.
    const auto window = ctl.call("movePane", Json::Array{id(tab), 0, "new-window"});
    ASSERT_TRUE(window) << ctl.error.message;
    EXPECT_EQ((*window)["id"].toInteger(), id(tab));
    const auto holder = daemon.call(Fn::PaneSession, Json::Array{id(tab)});
    ASSERT_TRUE(holder) << daemon.lastError();
    EXPECT_NE(holder->toString(), "m");
    EXPECT_EQ(sessionNamed(daemon, "m").value_or(SessionInfo{}).panes, 2);
    // A tab that holds every pane of the session would only get a new name.
    EXPECT_FALSE(ctl.call("detachTab", Json::Array{id(first)}));
    EXPECT_EQ(ctl.error.code, IpcErrorCode::CallFailed);
    EXPECT_EQ(ctl.error.message, "these are all the panes of session 'm', and a new session would only give them another name");

    Window w;
    ASSERT_EQ(w.attach(daemon, "m"), "");
    ASSERT_TRUE(w.until([&] { return w.opened.size() == 2; }));
    EXPECT_FALSE(ctl.call("movePane", Json::Array{id(first), 9999, "left"}));
    EXPECT_EQ(ctl.error.message, "no pane 9999 in session 'm'");
    ASSERT_TRUE(w.roundTrip());
    for (const Json::Value& call : w.controls()) EXPECT_NE(call["fn"].toString(), "movePane") << Json::write(call);
}

// A session's watch hears the statuses of a pane (OSC 9;4); merges the user
// variables of a pane by their names, so that one of each name goes at once;
// and sends what waits for a pane before an event that is never merged.
TEST(MuxControl, WatchHearsStatusesMergesUserVariablesByNameAndKeepsTheOrderOfAPane)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    SessionOptions options = shellOptions(daemon, "v");
    options.control.allowReadText = true;
    options.control.eventInterval = 400;
    ASSERT_TRUE(daemon.newSession(options)) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("v"), "");
    const std::int64_t pane = firstPane(ctl);
    ASSERT_NE(pane, 0);
    const std::string id = std::to_string(pane);
    Watcher watcher(daemon, "v");
    for (const auto& [n, signal] : std::vector<std::pair<std::int64_t, std::string>>{
             {10, "statusChanged"}, {11, "userVarChanged"}, {12, "titleChanged"}, {13, "commandStarted"}}) {
        EXPECT_EQ(watcher.subscribe(n, signal)["type"].toString(), "reply");
    }

    // The second title waits for the interval of the first; the start of a
    // command, which is never merged, sends it first.
    ASSERT_TRUE(ctl.call("sendText", Json::Array{pane, "printf '\\033]2;x\\007\\033]2;y\\007\\033]133;A;cmdline=run\\007\\033]133;C\\007'\n"}))
        << ctl.error.message;
    EXPECT_EQ(watcher.next(), "titleChanged [" + id + ",\"x\"]");
    EXPECT_EQ(watcher.next(), "titleChanged [" + id + ",\"y\"]");
    EXPECT_EQ(watcher.next(), "commandStarted [" + id + ",\"run\"]");

    // Two names in one interval: each goes at once, and the newer value of
    // the first waits for the interval.
    ASSERT_TRUE(ctl.call("sendText", Json::Array{pane, "printf '\\033]1337;SetUserVar=a=MQ==\\007\\033]1337;SetUserVar=b=MQ==\\007"
                                                       "\\033]1337;SetUserVar=a=Mg==\\007'\n"}))
        << ctl.error.message;
    EXPECT_EQ(watcher.next(), "userVarChanged [" + id + ",\"a\",\"1\"]");
    EXPECT_EQ(watcher.next(), "userVarChanged [" + id + ",\"b\",\"1\"]");
    EXPECT_EQ(watcher.next(), "userVarChanged [" + id + ",\"a\",\"2\"]");

    // A progress, then an error: the error waits for the interval.
    ASSERT_TRUE(ctl.call("sendText", Json::Array{pane, "printf '\\033]9;4;3\\007\\033]9;4;2\\007'\n"})) << ctl.error.message;
    EXPECT_EQ(watcher.next(), "statusChanged [" + id + ",\"working\",\"\"]");
    EXPECT_EQ(watcher.next(), "statusChanged [" + id + ",\"error\",\"\"]");
}

// A watcher that goes while an event waits for its interval leaves the
// daemon nothing of it: the other watchers of the session get the event, and
// the daemon goes on.
TEST(MuxControl, AWatcherThatGoesWhileItsEventsWaitLeavesTheOthersTheirs)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    SessionOptions options = shellOptions(daemon, "f");
    options.control.eventInterval = 2000;
    ASSERT_TRUE(daemon.newSession(options)) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("f"), "");
    const std::int64_t pane = firstPane(ctl);
    ASSERT_NE(pane, 0);
    const std::string id = std::to_string(pane);
    Watcher stays(daemon, "f");
    EXPECT_EQ(stays.subscribe(10, "titleChanged")["type"].toString(), "reply");
    {
        Watcher goes(daemon, "f");
        EXPECT_EQ(goes.subscribe(10, "titleChanged")["type"].toString(), "reply");
        ASSERT_TRUE(ctl.call("sendText", Json::Array{pane, "printf '\\033]2;a\\007\\033]2;b\\007'\n"})) << ctl.error.message;
        EXPECT_EQ(goes.next(), "titleChanged [" + id + ",\"a\"]");
    }
    EXPECT_EQ(stays.next(), "titleChanged [" + id + ",\"a\"]");
    EXPECT_EQ(stays.next(), "titleChanged [" + id + ",\"b\"]");
    EXPECT_TRUE(daemon.alive());
    EXPECT_TRUE(daemon.call(Fn::ListSessions, {})) << daemon.lastError();
}

// The panes that a window's layout names as moved are events of the watch.
TEST(MuxControl, WatchHearsTheMovesOfAWindowsLayout)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "l"))) << daemon.lastError();
    const auto second = newPane(daemon, "l", launchOf(Waiting, daemon));
    ASSERT_TRUE(second) << daemon.lastError();
    Watcher watcher(daemon, "l");
    EXPECT_EQ(watcher.subscribe(10, "paneMoved")["type"].toString(), "reply");
    Window w;
    ASSERT_EQ(w.attach(daemon, "l"), "");
    ASSERT_TRUE(w.until([&] { return w.opened.size() == 2 && !w.layouts.empty(); }));
    Layout layout = w.layouts.back();
    layout.moved = {*second};
    ASSERT_TRUE(w.sendLayout(layout));
    ASSERT_TRUE(w.stream->flush(2000));
    EXPECT_EQ(watcher.next(), "paneMoved [" + std::to_string(*second) + "]");
}

// A call of a window that went before its call came (it closed, or detached
// while ssh signed in) changes nothing: a pane that it asks for would be
// nobody's, in no layout, and its panes stay in the session.
TEST(MuxAttach, AWindowThatNoLongerShowsTheSessionChangesNoPane)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
    ASSERT_GT(w.serial, 0);
    ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"s"})) << daemon.lastError();
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached); }));
    ASSERT_TRUE(waitFor([&] { return !sessionNamed(daemon, "s").value_or(SessionInfo{}).attached; }));
    IpcConnection::Error error;
    EXPECT_FALSE(daemon.call(Fn::NewPane, Json::Array{ProtocolVersion, "s", toJson(launchOf(Waiting, daemon)), toJson(NewPaneSize), w.serial}, &error));
    EXPECT_EQ(error.code, Error::NotAttached) << error.message;
    EXPECT_EQ(sessionNamed(daemon, "s").value_or(SessionInfo{}).panes, 1);
    // A pane of its own, which a call of no window adds.
    const auto second = newPane(daemon, "s", launchOf(Waiting, daemon), &error);
    ASSERT_TRUE(second) << error.message;
    Layout moved;
    moved.addTab(*second);
    EXPECT_FALSE(daemon.call(Fn::BreakOut, Json::Array{ProtocolVersion, "s", toJson(moved), w.serial}, &error));
    EXPECT_EQ(error.code, Error::NotAttached) << error.message;
    EXPECT_EQ(sessionNamed(daemon, "s").value_or(SessionInfo{}).panes, 2);
    EXPECT_EQ(names(daemon.sessions()), (std::vector<std::string>{"keep", "s"}));
}

namespace {

/// The visible text of @p terminal, one line for each row, as
/// Window::screen() gives it for the first pane.
std::string textOf(const AfterglowVt::Terminal& terminal)
{
    std::string text;
    const AfterglowVt::Screen& s = terminal.screen();
    for (int y = 0; y < s.rows(); ++y) {
        for (const AfterglowVt::Cell& c : s.row(y).cells) {
            if (c.width() == 0) continue;
            const char32_t cp = c.codepoint() ? c.codepoint() : U' ';
            text.push_back(cp < 0x80 ? char(cp) : '?');
        }
        text.push_back('\n');
    }
    return text;
}

/// The place in the events of @p w of the first Control event that calls
/// @p fn for pane @p id, else the number of the events.
std::size_t controlAt(const Window& w, const std::string& fn, std::uint32_t id)
{
    for (std::size_t i = 0; i < w.events.size(); ++i) {
        if (w.events[i].type != Attach::EventType::Control) continue;
        const auto json = Json::parse(w.events[i].payload);
        if (!json || (*json)["fn"].toString() != fn) continue;
        const Json::Array& args = (*json)["args"].toArray();
        if (!args.empty() && args[0].toInteger() == std::int64_t(id)) return i;
    }
    return w.events.size();
}

/// The place of the first event of @p type for pane @p id, else the number
/// of the events.
std::size_t eventAt(const Window& w, Attach::EventType type, std::uint32_t id)
{
    for (std::size_t i = 0; i < w.events.size(); ++i) {
        if (w.events[i].type == type && w.events[i].pane == id) return i;
    }
    return w.events.size();
}

/// Fn::JoinPane of pane @p id of session @p from to the session of pane
/// @p beside, from window @p window (0: a call of no window).
std::optional<Json::Value> joinPane(TestDaemon& daemon, const std::string& from, std::uint32_t id, std::uint32_t beside,
                                    const std::string& placement, std::int64_t window, IpcConnection::Error* error = nullptr)
{
    Json::Array args{ProtocolVersion, from, std::int64_t(id), std::int64_t(beside), placement};
    if (window != 0) args.push_back(window);
    return daemon.call(Fn::JoinPane, std::move(args), error);
}

bool openedIn(const Window& w, std::uint32_t id)
{
    return std::find(w.opened.begin(), w.opened.end(), id) != w.opened.end();
}

} // namespace

// A pane that joins another session answers OSC 99's query with that
// session's capabilities, those of the window that shows it now.
TEST(MuxAttach, AJoinedPaneAnswersTheNotificationQueryOfItsNewSession)
{
    if (!pythonFound()) GTEST_SKIP() << "python3 is not installed";
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    SessionOptions a = daemon.options(Waiting, "a");
    a.notificationCapabilities = "p=title";
    SessionOptions b = daemon.options(Waiting, "b");
    b.notificationCapabilities = "p=body";
    ASSERT_TRUE(daemon.newSession(a)) << daemon.lastError();
    ASSERT_TRUE(daemon.newSession(b)) << daemon.lastError();
    Window wa;
    Window wb;
    ASSERT_EQ(wa.attach(daemon, "a"), "");
    ASSERT_EQ(wb.attach(daemon, "b"), "");
    ASSERT_TRUE(wa.until([&] { return wa.pane != 0; }));
    ASSERT_TRUE(wb.until([&] { return wb.pane != 0; }));
    const std::string answers = daemon.dir() + "/answers";
    const auto moving = newPane(daemon, "a", launchOf(ask("\\x1b]99;i=n:p=?;\\x1b\\\\\\x1b[c", "c", answers, true), daemon));
    ASSERT_TRUE(moving) << daemon.lastError();
    // The program asks once it has a byte, after "go?".
    const auto asks = [&wa, id = *moving] {
        const auto other = wa.others.find(id);
        if (other == wa.others.end()) return false;
        const AfterglowVt::Screen& screen = other->second->replica.screen();
        std::string text;
        for (int y = 0; y < screen.rows(); ++y) {
            for (const AfterglowVt::Cell& c : screen.row(y).cells) text.push_back(c.codepoint() > 0 && c.codepoint() < 0x80 ? char(c.codepoint()) : ' ');
        }
        return contains(text, "go?");
    };
    ASSERT_TRUE(wa.until(asks));
    IpcConnection::Error error;
    ASSERT_TRUE(joinPane(daemon, "a", *moving, wb.pane, "down", wa.serial, &error)) << error.message;
    ASSERT_TRUE(wb.until([&] { return openedIn(wb, *moving); }));
    Attach::Input key;
    key.pane = *moving;
    key.bytes = "x";
    ASSERT_TRUE(wb.send(key));
    ASSERT_TRUE(wb.until([&] { return exists(answers); })) << daemon.errors();
    EXPECT_NE(readFile(answers).find("p=body"), std::string::npos) << readFile(answers);
    EXPECT_EQ(readFile(answers).find("p=title"), std::string::npos) << readFile(answers);
}

// A pane of the window of one session joins the session that another window
// shows, beside one of its panes, with its program running: the window that
// showed it drags it out, and the other one places it, then gets its frames
// and sends its input. The session that loses its last pane ends.
TEST(MuxAttach, APaneJoinsAnotherSessionWithItsProgram)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "a"))) << daemon.lastError();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "b"))) << daemon.lastError();
    Window wa;
    Window wb;
    ASSERT_EQ(wa.attach(daemon, "a"), "");
    ASSERT_EQ(wb.attach(daemon, "b"), "");
    ASSERT_TRUE(wa.until([&] { return wa.pane != 0; }));
    ASSERT_TRUE(wb.until([&] { return wb.pane != 0; }));
    const std::string pidFile = daemon.dir() + "/pid";
    const auto moving =
        newPane(daemon, "a", launchOf({"/bin/sh", "-c", "echo $$ > '" + pidFile + ".part' && mv '" + pidFile + ".part' '" + pidFile + "'; exec cat"}, daemon));
    ASSERT_TRUE(moving) << daemon.lastError();
    ASSERT_TRUE(waitFor([&] { return readFile(pidFile).find('\n') != std::string::npos; }));
    const pid_t program = pid_t(std::strtol(readFile(pidFile).c_str(), nullptr, 10));
    ASSERT_GT(program, 0);
    ASSERT_TRUE(wa.until([&] { return openedIn(wa, *moving); }));

    IpcConnection::Error error;
    const auto joined = joinPane(daemon, "a", *moving, wb.pane, "down", wa.serial, &error);
    ASSERT_TRUE(joined) << error.message;
    std::vector<std::string> keys;
    for (const auto& [key, value] : joined->toObject()) keys.push_back(key);
    EXPECT_EQ(keys, std::vector<std::string>{"name"});
    EXPECT_EQ((*joined)["name"].toString(), "b");
    // The window of the first session no longer shows it, and the session
    // keeps its first pane. That window asked: no paneLeft tells it.
    ASSERT_TRUE(wa.until([&] { return wa.closedPane(*moving); }));
    EXPECT_FALSE(wa.got(Attach::EventType::SessionEnded));
    EXPECT_EQ(controlAt(wa, "paneLeft", *moving), wa.events.size());
    EXPECT_EQ(sessionNamed(daemon, "a").value_or(SessionInfo{}).panes, 1);
    EXPECT_EQ(sessionNamed(daemon, "b").value_or(SessionInfo{}).panes, 2);
    EXPECT_TRUE(running(program));
    const auto holder = daemon.call(Fn::PaneSession, Json::Array{std::int64_t(*moving)});
    ASSERT_TRUE(holder) << daemon.lastError();
    EXPECT_EQ(holder->toString(), "b");
    // The window of the other session hears where to place it before its
    // PaneOpened, as for a split of `afterglow ctl`.
    ASSERT_TRUE(wb.until([&] { return openedIn(wb, *moving); }));
    const std::size_t placed = controlAt(wb, "placePane", *moving);
    ASSERT_LT(placed, wb.events.size());
    EXPECT_LT(placed, eventAt(wb, Attach::EventType::PaneOpened, *moving));
    const auto place = Json::parse(wb.events[placed].payload);
    ASSERT_TRUE(place);
    EXPECT_EQ((*place)["id"].toInteger(), 0);
    EXPECT_EQ(Json::write((*place)["args"]),
              Json::write(Json::Value(Json::Array{std::int64_t(*moving), std::int64_t(wb.pane), "down", Json::Array{}, ""})));
    // The daemon's layout of the other session holds it below its pane.
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("b"), "");
    EXPECT_EQ(tabOf(ctl, *moving), 0);
    // The same program answers through the other window.
    Attach::Input typed;
    typed.pane = *moving;
    typed.bytes = "joined-b\n";
    ASSERT_TRUE(wb.send(typed));
    ASSERT_TRUE(wb.until([&] {
        const auto it = wb.others.find(*moving);
        return it != wb.others.end() && contains(textOf(it->second->replica), "joined-b");
    }));
    EXPECT_TRUE(running(program));
    EXPECT_EQ(pid_t(std::strtol(readFile(pidFile).c_str(), nullptr, 10)), program);

    // The last pane of the first session goes to a tab of its own: that
    // session ends, as when its last program exits.
    const std::uint32_t last = wa.pane;
    ASSERT_TRUE(joinPane(daemon, "a", last, *moving, "new-tab", wa.serial, &error)) << error.message;
    ASSERT_TRUE(wa.until([&] { return wa.got(Attach::EventType::SessionEnded); }));
    EXPECT_FALSE(sessionNamed(daemon, "a"));
    EXPECT_EQ(sessionNamed(daemon, "b").value_or(SessionInfo{}).panes, 3);
    ASSERT_TRUE(wb.until([&] { return openedIn(wb, last); }));
    const std::size_t tab = controlAt(wb, "placePane", last);
    ASSERT_LT(tab, wb.events.size());
    EXPECT_EQ((*Json::parse(wb.events[tab].payload))["args"].toArray().at(2).toString(), "tab");
    EXPECT_EQ(tabOf(ctl, last), 1);
    EXPECT_TRUE(running(program));
}

TEST(MuxAttach, JoinPaneRefusesBadCalls)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "t"))) << daemon.lastError();
    const std::uint32_t first = firstPaneOf(daemon, "s");
    const std::uint32_t target = firstPaneOf(daemon, "t");
    ASSERT_NE(first, 0u);
    ASSERT_NE(target, 0u);
    const auto second = newPane(daemon, "s", launchOf(Waiting, daemon));
    ASSERT_TRUE(second) << daemon.lastError();
    const auto pane = [](std::uint32_t id) { return std::int64_t(id); };
    const std::pair<Json::Array, std::string> bad[] = {
        {Json::Array{ProtocolVersion, "none", pane(*second), pane(target), "right"}, Error::NoSuchSession},
        {Json::Array{ProtocolVersion - 1, "s", pane(*second), pane(target), "right"}, Error::VersionMismatch},
        {Json::Array{ProtocolVersion, "s", pane(*second), pane(target)}, IpcErrorCode::InvalidArg},
        {Json::Array{ProtocolVersion, "s", pane(*second), pane(target), 1}, IpcErrorCode::InvalidArg},
        {Json::Array{ProtocolVersion, "s", 0, pane(target), "right"}, IpcErrorCode::InvalidArg},
        // A window is a whole number from 1.
        {Json::Array{ProtocolVersion, "s", pane(*second), pane(target), "right", 0}, IpcErrorCode::InvalidArg},
        // The pane of another session.
        {Json::Array{ProtocolVersion, "s", pane(target), pane(first), "right"}, Error::NoSuchPane},
        // A pane that no session has.
        {Json::Array{ProtocolVersion, "s", pane(*second), 9999, "right"}, Error::NoSuchPane},
        // A pane of the same session: its window moves it.
        {Json::Array{ProtocolVersion, "s", pane(*second), pane(first), "right"}, IpcErrorCode::CallFailed},
        {Json::Array{ProtocolVersion, "s", pane(*second), pane(target), "sideways"}, IpcErrorCode::CallFailed},
        // A new window is a new session (Fn::BreakOut).
        {Json::Array{ProtocolVersion, "s", pane(*second), pane(target), "new-window"}, IpcErrorCode::CallFailed},
    };
    for (const auto& [args, code] : bad) {
        IpcConnection::Error error;
        EXPECT_FALSE(daemon.call(Fn::JoinPane, args, &error)) << Json::write(Json::Value(args));
        EXPECT_EQ(error.code, code) << Json::write(Json::Value(args)) << ": " << error.message;
    }
    // A window that another one took the session from moves none of its
    // panes, and neither does one that no longer shows it.
    Window old;
    ASSERT_EQ(old.attach(daemon, "s"), "");
    ASSERT_TRUE(old.until([&] { return old.pane != 0; }));
    Window w;
    ASSERT_EQ(w.attach(daemon, "s"), "");
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Layout); }));
    IpcConnection::Error error;
    EXPECT_FALSE(joinPane(daemon, "s", *second, target, "right", old.serial, &error));
    EXPECT_EQ(error.code, Error::Attached) << error.message;
    ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"s"})) << daemon.lastError();
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached); }));
    ASSERT_TRUE(waitFor([&] { return !sessionNamed(daemon, "s").value_or(SessionInfo{}).attached; }));
    EXPECT_FALSE(joinPane(daemon, "s", *second, target, "right", w.serial, &error));
    EXPECT_EQ(error.code, Error::NotAttached) << error.message;
    EXPECT_EQ(sessionNamed(daemon, "s").value_or(SessionInfo{}).panes, 2);
    EXPECT_EQ(sessionNamed(daemon, "t").value_or(SessionInfo{}).panes, 1);
    // A call of no window may, while no window shows either session.
    ASSERT_TRUE(joinPane(daemon, "s", *second, target, "right", 0, &error)) << error.message;
    EXPECT_EQ(sessionNamed(daemon, "s").value_or(SessionInfo{}).panes, 1);
    EXPECT_EQ(sessionNamed(daemon, "t").value_or(SessionInfo{}).panes, 2);
}

// A session of MaxSessionPanes panes takes no pane of another session, from
// a window or from `afterglow ctl --session`.
TEST(MuxAttach, ASessionThatIsFullTakesNoJoinedPane)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "s"))) << daemon.lastError();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "full"))) << daemon.lastError();
    for (std::size_t panes = 1; panes < MaxSessionPanes; ++panes) {
        ASSERT_TRUE(newPane(daemon, "full", launchOf(Waiting, daemon))) << panes << " " << daemon.lastError();
    }
    const std::uint32_t target = firstPaneOf(daemon, "full");
    ASSERT_NE(target, 0u);
    const auto second = newPane(daemon, "s", launchOf(Waiting, daemon));
    ASSERT_TRUE(second) << daemon.lastError();
    IpcConnection::Error error;
    EXPECT_FALSE(joinPane(daemon, "s", *second, target, "right", 0, &error));
    EXPECT_EQ(error.code, Error::TooManyPanes) << error.message;
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("s"), "");
    EXPECT_FALSE(ctl.call("movePane", Json::Array{std::int64_t(*second), std::int64_t(target), "right"}));
    EXPECT_EQ(ctl.error.code, Error::TooManyPanes) << ctl.error.message;
    EXPECT_EQ(sessionNamed(daemon, "s").value_or(SessionInfo{}).panes, 2);
    EXPECT_EQ(sessionNamed(daemon, "full").value_or(SessionInfo{}).panes, int(MaxSessionPanes));
}

// `afterglow ctl --session` moves a pane beside a pane of another session:
// the daemon joins it there. The window of the first session did not ask,
// and hears that the pane left before its PaneClosed. The other session,
// which no window shows, keeps it in its layout for the next window.
TEST(MuxControl, AMoveBesideAPaneOfAnotherSessionJoinsIt)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "a"))) << daemon.lastError();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "b"))) << daemon.lastError();
    Window wa;
    ASSERT_EQ(wa.attach(daemon, "a"), "");
    ASSERT_TRUE(wa.until([&] { return wa.pane != 0; }));
    const auto moving = newPane(daemon, "a", launchOf(Waiting, daemon));
    ASSERT_TRUE(moving) << daemon.lastError();
    ASSERT_TRUE(wa.until([&] { return openedIn(wa, *moving); }));
    const std::uint32_t target = firstPaneOf(daemon, "b");
    ASSERT_NE(target, 0u);
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("a"), "");
    EXPECT_FALSE(ctl.call("movePane", Json::Array{std::int64_t(*moving), std::int64_t(target), "new-window"}));
    EXPECT_EQ(ctl.error.message, "new-window takes no target pane");

    const auto moved = ctl.call("movePane", Json::Array{std::int64_t(*moving), std::int64_t(target), "up"});
    ASSERT_TRUE(moved) << ctl.error.message;
    EXPECT_EQ((*moved)["id"].toInteger(), std::int64_t(*moving));
    EXPECT_EQ((*moved)["tab"].toInteger(), 0);
    ASSERT_TRUE(wa.until([&] { return wa.closedPane(*moving); }));
    const std::size_t left = controlAt(wa, "paneLeft", *moving);
    ASSERT_LT(left, wa.events.size());
    EXPECT_LT(left, eventAt(wa, Attach::EventType::PaneClosed, *moving));
    EXPECT_EQ(Json::write((*Json::parse(wa.events[left].payload))["args"]), Json::write(Json::Value(Json::Array{std::int64_t(*moving), "b"})));
    EXPECT_EQ(sessionNamed(daemon, "a").value_or(SessionInfo{}).panes, 1);
    EXPECT_EQ(sessionNamed(daemon, "b").value_or(SessionInfo{}).panes, 2);
    const auto holder = daemon.call(Fn::PaneSession, Json::Array{std::int64_t(*moving)});
    ASSERT_TRUE(holder) << daemon.lastError();
    EXPECT_EQ(holder->toString(), "b");

    // The next window of the other session shows it above the target, with
    // the focus.
    Window wb;
    ASSERT_EQ(wb.attach(daemon, "b"), "");
    ASSERT_TRUE(wb.until([&] { return wb.opened.size() == 2; }));
    ASSERT_EQ(wb.layouts.size(), 1u);
    ASSERT_EQ(wb.layouts[0].tabs.size(), 1u);
    EXPECT_EQ(wb.layouts[0].panes(), (std::vector<std::uint32_t>{*moving, target}));
    EXPECT_EQ(wb.layouts[0].tabs[0].focused, *moving);

    // The last pane of the first session: that session ends, and the window
    // of the other one places the pane in a tab after the tab of the target.
    const std::uint32_t last = wa.pane;
    ASSERT_TRUE(ctl.call("movePane", Json::Array{std::int64_t(last), std::int64_t(target), "new-tab"})) << ctl.error.message;
    ASSERT_TRUE(wa.until([&] { return wa.got(Attach::EventType::SessionEnded); }));
    EXPECT_LT(controlAt(wa, "paneLeft", last), wa.events.size());
    EXPECT_FALSE(sessionNamed(daemon, "a"));
    ASSERT_TRUE(wb.until([&] { return openedIn(wb, last); }));
    EXPECT_LT(controlAt(wb, "placePane", last), eventAt(wb, Attach::EventType::PaneOpened, last));
    EXPECT_EQ(sessionNamed(daemon, "b").value_or(SessionInfo{}).panes, 3);
}

// A window that goes leaves the calls that it did not get, in the queue of
// its session and in that of the pane, for the next window: those of the
// session's queue are older, and go first. So the next window gets the
// newest notifications in the program's order, and a close of an OSC 99
// notification that waited behind its show still takes it away.
TEST(MuxAttach, TheCallsThatAWindowDidNotGetGoToTheNextOneInTheirOrder)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    SessionOptions options = shellOptions(daemon, "order");
    options.control.allowReadText = true;
    options.keptNotifications = int(MaxKeptNotifications);
    ASSERT_TRUE(daemon.newSession(options)) << daemon.lastError();
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("order"), "");
    const std::uint32_t pane = firstPane(ctl);
    ASSERT_NE(pane, 0u);
    {
        // A window that reads its events through a small window, then none:
        // the daemon's queue of the session fills, then that of the pane.
        Window w;
        w.streamWindow = std::size_t(16) << 10;
        ASSERT_EQ(w.attach(daemon, "order", true, std::size_t(MinEventQueue)), "");
        ASSERT_TRUE(w.until([&] { return w.pane != 0; }));
        w.readEvents = false;
        // An OSC 99 notification first, its close last, and between them
        // notifications of 1 KiB. A call waits in the pane's queue until the
        // frame before it went, so the session's queue takes the first ones,
        // and the pane's queue (Mux.eventQueue) the newer ones, all of them.
        ASSERT_TRUE(ctl.call("sendText", Json::Array{std::int64_t(pane),
                                                     "printf '\\033]99;i=x;shown\\033\\\\'; pad=$(printf '%01000d' 0); "
                                                     "n=1; while [ $n -le 900 ]; do printf '\\033]9;%04d %s\\a' $n $pad; n=$((n+1)); done; "
                                                     "printf '\\033]99;i=x:p=close;\\033\\\\'; echo do''ne\n"}))
            << ctl.error.message;
        ASSERT_TRUE(screenShows(ctl, std::int64_t(pane), "\ndone", 20s));
        ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"order"})) << daemon.lastError();
    }
    ASSERT_TRUE(waitFor([&] {
        const auto info = sessionNamed(daemon, "order");
        return info && !info->attached;
    }));
    Window next;
    ASSERT_EQ(next.attach(daemon, "order"), "");
    ASSERT_TRUE(next.until([&] { return next.pane != 0; }));
    ASSERT_TRUE(next.roundTrip());
    std::vector<int> numbers;
    for (const Host::Call& call : next.calls) {
        EXPECT_FALSE(call.kind == Host::Kind::DesktopNotification && call.notification.kind == AfterglowVt::NotificationRequest::Kind::Show)
            << "the closed OSC 99 notification came back";
        if (call.kind == Host::Kind::Notify) numbers.push_back(std::atoi(call.text.substr(0, 4).c_str()));
    }
    ASSERT_FALSE(numbers.empty());
    EXPECT_TRUE(std::is_sorted(numbers.begin(), numbers.end()));
    // The newest of all: the last one that the program sent.
    EXPECT_EQ(numbers.back(), 900) << numbers.front() << " to " << numbers.back();
}

// A pane ID comes into a window's stream once: a pane that went to another
// session comes back into the window that it left only after that window let
// it go (PaneForgotten). Until then the move back is refused, and nothing
// moves.
TEST(MuxControl, APaneComesBackIntoTheWindowThatItLeftOnceThatWindowLetItGo)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "a"))) << daemon.lastError();
    ASSERT_TRUE(daemon.newSession(daemon.options(Waiting, "b"))) << daemon.lastError();
    Window wa;
    ASSERT_EQ(wa.attach(daemon, "a"), "");
    ASSERT_TRUE(wa.until([&] { return wa.pane != 0; }));
    const std::uint32_t stays = wa.pane;
    const auto moving = newPane(daemon, "a", launchOf(Waiting, daemon));
    ASSERT_TRUE(moving) << daemon.lastError();
    ASSERT_TRUE(wa.until([&] { return openedIn(wa, *moving); }));
    const std::uint32_t target = firstPaneOf(daemon, "b");
    ASSERT_NE(target, 0u);
    ControlClient ctl(daemon);
    ASSERT_EQ(ctl.select("a"), "");
    ASSERT_TRUE(ctl.call("movePane", Json::Array{std::int64_t(*moving), std::int64_t(target), "right"})) << ctl.error.message;
    ASSERT_TRUE(wa.until([&] { return wa.closedPane(*moving); }));
    // Back at once: the window of session a still holds the pane.
    ControlClient back(daemon);
    ASSERT_EQ(back.select("b"), "");
    EXPECT_FALSE(back.call("movePane", Json::Array{std::int64_t(*moving), std::int64_t(stays), "right"}));
    EXPECT_EQ(back.error.message, "pane " + std::to_string(*moving) + " is still leaving the window of session 'a'");
    EXPECT_EQ(sessionNamed(daemon, "b").value_or(SessionInfo{}).panes, 2);
    const auto opens = [&] { return std::count(wa.opened.begin(), wa.opened.end(), *moving); };
    EXPECT_EQ(opens(), 1);
    // The window let it go: the move back goes, and the window gets the pane
    // again, placed before it opens.
    ASSERT_TRUE(wa.forget(*moving));
    ASSERT_TRUE(waitFor([&] {
        return back.call("movePane", Json::Array{std::int64_t(*moving), std::int64_t(stays), "right"}).has_value()
            || back.error.message.find("still leaving") == std::string::npos;
    })) << back.error.message;
    ASSERT_TRUE(wa.until([&] { return opens() == 2; }));
    EXPECT_EQ(sessionNamed(daemon, "a").value_or(SessionInfo{}).panes, 2);
    EXPECT_EQ(sessionNamed(daemon, "b").value_or(SessionInfo{}).panes, 1);
    std::size_t placed = wa.events.size();
    for (std::size_t i = wa.events.size(); i-- > 0;) {
        const auto& event = wa.events[i];
        if (event.type != Attach::EventType::Control) continue;
        const auto call = Json::parse(event.payload);
        if (call && (*call)["fn"].toString() == "placePane" && (*call)["args"].toArray().at(0).toInteger() == std::int64_t(*moving)) {
            placed = i;
            break;
        }
    }
    ASSERT_LT(placed, wa.events.size());
    std::size_t reopened = wa.events.size();
    for (std::size_t i = wa.events.size(); i-- > 0;) {
        if (wa.events[i].type == Attach::EventType::PaneOpened && wa.events[i].pane == *moving) {
            reopened = i;
            break;
        }
    }
    EXPECT_LT(placed, reopened);
}
