// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// A window attached to a session, played without Qt: the stream of the attach
// call, and a replica terminal that only the daemon's frames change. Each test
// runs its own daemon (TestDaemon.h).

#include "TestDaemon.h"

#include "MuxHost.h"
#include "MuxStream.h"

#include <AfterglowVt/StateSync.h>
#include <AfterglowVt/Terminal.h>
#include <PhosphorChannel/Stream.h>

#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace MuxTest;

namespace {

struct NullHost : AfterglowVt::ITerminalHost
{
    void writeToPty(std::string_view) override {}
};

int connectTo(const std::string& path)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), std::min(path.size(), sizeof address.sun_path - 1));
    if (fd < 0 || ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        if (fd >= 0) ::close(fd);
        return -1;
    }
    return fd;
}

/// What a window of a session does, without the window.
class Window
{
public:
    /// The attach call and the connection to its socket. Empty on success,
    /// else the error code or message of the call, or why the connection
    /// failed. Without @p eventQueue, the call passes none.
    std::string attach(TestDaemon& daemon, const std::string& name, bool takeover = true, std::optional<std::size_t> eventQueue = {})
    {
        if (std::string error = call(daemon, name, takeover, eventQueue); !error.empty()) return error;
        return connect();
    }

    /// The attach call alone. Empty on success, else the error code, or the
    /// message of an error without one.
    std::string call(TestDaemon& daemon, const std::string& name, bool takeover = true, std::optional<std::size_t> eventQueue = {})
    {
        IpcConnection::Error error;
        Json::Object options{{AttachOption::Takeover, takeover}};
        if (eventQueue) options.emplace_back(AttachOption::EventQueue, *eventQueue);
        const auto result = daemon.call(Fn::Attach, Json::Array{ProtocolVersion, name, std::move(options)}, &error);
        if (!result) return error.code.empty() ? error.message : error.code;
        socket = (*result)["socket"].toString();
        return {};
    }

    /// The connection to the socket of the call.
    std::string connect()
    {
        const int fd = connectTo(socket);
        if (fd < 0) return "no connection";
        stream = std::make_unique<PhosphorChannel::Stream>(fd);
        std::string why;
        input = stream->openWriter(Attach::ControlChannel, Attach::InputCodec, &why);
        if (input) actions = stream->openWriter(Attach::ActionChannel, Attach::ActionCodec, &why);
        return input && actions ? std::string() : why;
    }

    /// Runs the stream until @p done, up to five seconds.
    bool until(const std::function<bool()>& done)
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (done()) return true;
            if (!stream->pump(20)) ended = true;
            read();
        }
        return done();
    }

    void read()
    {
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
                    // As a window does: it writes the terminal's reports.
                    if (call->kind == Host::Kind::Report && answerReports && !type(call->text)) ADD_FAILURE() << "a report does not go";
                }
                if (event->type == Attach::EventType::PaneOpened) {
                    pane = event->pane;
                    channel = event->channel;
                }
                if (event->type == Attach::EventType::PaneClosed && channelClosed && !screenAtExit) screenAtExit = screen();
            }
        }
        if (auto* state = channel && !channelClosed ? stream->reader(channel) : nullptr) {
            while (true) {
                const PhosphorChannel::ReadResult result = state->read(frame);
                if (result == PhosphorChannel::ReadResult::Frame) {
                    ++frames;
                    std::string error;
                    if (!reader.apply(replica, frame, &error)) ADD_FAILURE() << "a frame does not apply: " << error;
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

    [[nodiscard]] bool action(Attach::InputType type)
    {
        Attach::Input message;
        message.type = type;
        message.pane = pane;
        // A Kill goes on the action channel, the others with the input.
        if (type == Attach::InputType::Kill) return actions->tryWrite(Attach::encode(message)) == PhosphorChannel::WriteResult::Written;
        return send(message);
    }

    std::string socket;
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
    std::vector<Host::Call> calls; ///< the host calls of the pane's terminal, oldest first
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
    // The socket of the attach is gone once the window connected.
    EXPECT_NE(::access(w.socket.c_str(), F_OK), 0);
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
    ASSERT_TRUE(w.until([&] { return w.replica.cols() == 4096 && w.replica.rows() == 3; })) << w.replica.cols();
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
    ASSERT_TRUE(second.type("echo after\n"));
    ASSERT_TRUE(second.until([&] { return contains(second.screen(), "\nafter"); }));
    // A frame that the daemon made before the takeover may be read after its
    // event (two channels keep no order): none shows the second window's
    // output.
    EXPECT_FALSE(contains(first.screen(), "\nafter")) << first.screen();
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
    const std::string done = daemon.dir() + "/job-done";
    {
        Window w;
        ASSERT_EQ(w.attach(daemon, "sh"), "");
        ASSERT_TRUE(w.until([&] { return contains(w.screen(), "$ "); }));
        // Output that comes while no window shows the session.
        ASSERT_TRUE(w.type("echo kept; (sleep 1; echo la''ter; : > " + done + ") &\n"));
        ASSERT_TRUE(w.until([&] { return contains(w.screen(), "\nkept"); }));
        ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"sh"})) << daemon.lastError();
        ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached); }));
        // The command line has la''ter: only the job's output has "later".
        EXPECT_FALSE(contains(w.screen(), "later")) << w.screen();
    }
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
    ASSERT_TRUE(w.type("i=0; while [ $i -lt 30000 ]; do printf '\\033[3%dmline %d %0390d\\033[0m\\n' $((i%8)) $i 0; i=$((i+1)); done; echo E''ND\n"));
    ASSERT_TRUE(w.stream->flush(2000));
    std::this_thread::sleep_for(3s);
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "\nEND"); })) << w.screen();
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
    EXPECT_EQ(keys, (std::vector<std::string>{"name", "socket"}));
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
        {"/bin/sh", "-c", "stty raw -echo; : > " + ready + "; while [ ! -e " + go + " ]; do sleep 0.1; done; exec cat > " + file});
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
    const auto deadline = std::chrono::steady_clock::now() + 60s;
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
    SessionOptions o = daemon.options({"/bin/sh", "-c", "stty raw -echo; : > " + ready + "; exec sleep 60"});
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

TEST(MuxAttach, OnlyTheCallerMayConnect)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    IpcConnection::Error error;
    const auto result = daemon.call(Fn::Attach, Json::Array{ProtocolVersion, "sh", Json::Object{}}, &error);
    ASSERT_TRUE(result) << error.message;
    const std::string socket = (*result)["socket"].toString();
    // Another process of the user: the daemon closes its connection.
    const pid_t child = ::fork();
    ASSERT_GE(child, 0) << std::strerror(errno);
    if (child == 0) {
        const int fd = connectTo(socket);
        if (fd < 0) ::_exit(2);
        pollfd p{fd, POLLIN, 0};
        char byte = 0;
        const bool closed = ::poll(&p, 1, 3000) == 1 && ::read(fd, &byte, 1) == 0;
        ::_exit(closed ? 0 : 1);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << status;
    // The caller still can.
    const int fd = connectTo(socket);
    ASSERT_GE(fd, 0);
    PhosphorChannel::Stream stream(fd);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!stream.reader(Attach::ControlChannel) && std::chrono::steady_clock::now() < deadline) stream.pump(20);
    EXPECT_TRUE(stream.reader(Attach::ControlChannel));
}

TEST(MuxAttach, AWindowThatDoesNotConnectLosesItsSocket)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    IpcConnection::Error error;
    const auto result = daemon.call(Fn::Attach, Json::Array{ProtocolVersion, "sh", Json::Object{{AttachOption::Takeover, false}}}, &error);
    ASSERT_TRUE(result) << error.message;
    const std::string socket = (*result)["socket"].toString();
    const auto made = std::chrono::steady_clock::now();
    EXPECT_EQ(::access(socket.c_str(), F_OK), 0);
    // The socket stays for the connect wait of the daemon, then goes.
    const auto gone = [&] {
        const auto deadline = made + std::chrono::milliseconds(Attach::ConnectWaitMs + 5000);
        while (std::chrono::steady_clock::now() < deadline) {
            if (::access(socket.c_str(), F_OK) != 0) return true;
            std::this_thread::sleep_for(50ms);
        }
        return false;
    };
    ASSERT_TRUE(gone());
    EXPECT_GE(std::chrono::steady_clock::now() - made, std::chrono::milliseconds(Attach::ConnectWaitMs - 200));
    // The session is free for a window that asks for no takeover.
    Window w;
    EXPECT_EQ(w.attach(daemon, "sh", false), "");
}

TEST(MuxAttach, ADaemonRemovesTheAttachSocketsThatItsPredecessorLeft)
{
    TestDaemon daemon;
    // A socket of a daemon that was killed while a window did not connect.
    const std::string stale = daemon.runtime() + "/afterglow-mux/attach-0123456789abcdef.sock";
    ASSERT_EQ(::mkdir((daemon.runtime() + "/afterglow-mux").c_str(), 0700), 0);
    std::ofstream(stale).put('x');
    ASSERT_TRUE(exists(stale));
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    EXPECT_FALSE(exists(stale));
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
    // A pane that no window shows cannot be seen: the next window that can
    // see it changes that again.
    ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"sh"}));
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached); }));
    Window next;
    next.answerReports = false;
    ASSERT_EQ(next.attach(daemon, "sh"), "");
    ASSERT_TRUE(next.until([&] { return next.pane != 0; }));
    visible.pane = next.pane;
    ASSERT_TRUE(next.send(visible));
    ASSERT_TRUE(next.until([&] { return !reports(next).empty(); }));
    EXPECT_EQ(reports(next).back(), "\x1b[?999;1n");
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
    for (const Json::Array& args : {Json::Array{}, Json::Array{"sh"}, Json::Array{"sh", "1"}, Json::Array{"sh", -1}, Json::Array{1, 1}}) {
        EXPECT_FALSE(daemon.call(Fn::PaneDirectory, args, &error));
        EXPECT_EQ(error.code, IpcErrorCode::InvalidArg);
    }
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
    // The window answers the transfer: the daemon does not refuse it.
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
        // gave none.
        ASSERT_TRUE(w.until([&] { return contains(w.screen(), "ready waited"); })) << w.screen();
        EXPECT_FALSE(exists(answer));
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

TEST(MuxAttach, RefusesAnEventQueueOutOfRangeAndAnUnknownOption)
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
    IpcConnection::Error error;
    EXPECT_FALSE(daemon.call(Fn::Attach, Json::Array{ProtocolVersion, "keep", Json::Object{{"eventQueues", MinEventQueue}}}, &error));
    EXPECT_EQ(error.code, IpcErrorCode::InvalidArg);
    // The bounds themselves are values of the setting.
    for (const std::int64_t bytes : {MinEventQueue, MaxEventQueue}) {
        Window w;
        EXPECT_EQ(w.attach(daemon, "keep", true, std::size_t(bytes)), "") << bytes;
    }
}
