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
    /// else the error code of the call.
    std::string attach(TestDaemon& daemon, const std::string& name, bool takeover = true, std::size_t eventQueue = 0)
    {
        if (std::string error = call(daemon, name, takeover, eventQueue); !error.empty()) return error;
        return connect();
    }

    /// The attach call alone. Empty on success, else the error code.
    std::string call(TestDaemon& daemon, const std::string& name, bool takeover = true, std::size_t eventQueue = 0)
    {
        IpcConnection::Error error;
        Json::Object options{{"takeover", takeover}};
        if (eventQueue) options.emplace_back("eventQueue", eventQueue);
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
        if (auto* events = stream->reader(Attach::ControlChannel)) {
            while (events->read(frame) == PhosphorChannel::ReadResult::Frame) {
                const auto event = Attach::decodeEvent(frame);
                if (!event) {
                    ADD_FAILURE() << "an event does not decode";
                    continue;
                }
                this->events.push_back(*event);
                if (event->type == Attach::EventType::Host) {
                    const auto call = Host::decode(event->payload);
                    if (!call) ADD_FAILURE() << "a host call does not decode";
                    else calls.push_back(*call);
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
    const auto result = daemon.call(Fn::Attach, Json::Array{ProtocolVersion, "sh", Json::Object{{"takeover", false}}}, &error);
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
    EXPECT_FALSE(daemon.call(Fn::Attach, Json::Array{ProtocolVersion, "keep", Json::Object{{"takeover", 1}}}, &error));
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

} // namespace

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
    // A bell, and an OSC 52 write of "hello".
    ASSERT_TRUE(w.type("printf '\\a\\033]52;c;aGVsbG8=\\a'\n"));
    ASSERT_TRUE(w.until([&] { return hasCall(w, Host::Kind::Bell) && hasCall(w, Host::Kind::ClipboardWrite); }));
    const auto write = std::find_if(w.calls.begin(), w.calls.end(), [](const Host::Call& c) { return c.kind == Host::Kind::ClipboardWrite; });
    EXPECT_EQ(write->title, "c");
    // As the terminal hands it to a host: in base64, which the window decodes.
    EXPECT_EQ(write->text, "aGVsbG8=");
    // The bell came first, as the program sent it.
    EXPECT_EQ(w.calls.front().kind, Host::Kind::Bell);
}

// A clipboard read waits for the window: the daemon does not answer it, and
// the window's answer reaches the program as its input.
TEST(MuxAttach, AClipboardReadWaitsForTheWindowsAnswer)
{
    if (!pythonFound()) GTEST_SKIP() << "python3 is not installed";
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    const std::string answer = daemon.dir() + "/answer";
    ASSERT_TRUE(daemon.newSession(daemon.options(ask("\\x1b]52;c;?\\x07", "\\x07", answer), "read"))) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "read"), "");
    ASSERT_TRUE(w.until([&] { return hasCall(w, Host::Kind::ClipboardRequest); }));
    const auto request = std::find_if(w.calls.begin(), w.calls.end(), [](const Host::Call& c) { return c.kind == Host::Kind::ClipboardRequest; });
    EXPECT_TRUE(request->clipboard.osc52);
    EXPECT_EQ(request->clipboard.kind, AfterglowVt::ClipboardRequest::Kind::Read);
    // Nothing answered it yet.
    EXPECT_FALSE(exists(answer));
    ASSERT_TRUE(w.type(request->clipboard.osc52Reply("from the window")));
    ASSERT_TRUE(w.until([&] { return exists(answer); }));
    EXPECT_EQ(readFile(answer), request->clipboard.osc52Reply("from the window"));
}

// A call larger than the window's event queue gets the answer of a pane that
// no window shows: an OSC 52 write is dropped, and what follows still comes.
TEST(MuxAttach, ACallOverTheEventQueueGetsTheAnswerWithoutAWindow)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh", true, std::size_t(1) << 20), "");
    ASSERT_TRUE(w.until([&] { return w.pane != 0 && contains(w.screen(), "$ "); })) << w.screen();
    // 1.5 MiB of clipboard text, then a bell.
    ASSERT_TRUE(w.type("head -c 1572864 /dev/zero | tr '\\0' a > big; printf '\\033]52;c;'; base64 -w0 big; printf '\\a\\a'\n"));
    ASSERT_TRUE(w.until([&] { return hasCall(w, Host::Kind::Bell); }));
    EXPECT_FALSE(hasCall(w, Host::Kind::ClipboardWrite));
}

TEST(MuxAttach, RefusesAnEventQueueOutOfRange)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    for (const std::int64_t bytes : {MinEventQueue - 1, MaxEventQueue + 1}) {
        IpcConnection::Error error;
        EXPECT_FALSE(daemon.call(Fn::Attach, Json::Array{ProtocolVersion, "keep", Json::Object{{"eventQueue", bytes}}}, &error));
        EXPECT_EQ(error.code, "INVALID_ARG") << bytes;
    }
}
