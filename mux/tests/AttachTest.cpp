// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// A window attached to a session, played without Qt: the stream of the attach
// call, and a replica terminal that only the daemon's frames change. Each test
// runs its own daemon (TestDaemon.h).

#include "TestDaemon.h"

#include "MuxStream.h"

#include <AfterglowVt/StateSync.h>
#include <AfterglowVt/Terminal.h>
#include <PhosphorChannel/Stream.h>

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
    std::string attach(TestDaemon& daemon, const std::string& name, bool takeover = true)
    {
        IpcConnection::Error error;
        const auto result = daemon.call(Fn::Attach, Json::Array{ProtocolVersion, name, Json::Object{{"takeover", takeover}}}, &error);
        if (!result) return error.code.empty() ? error.message : error.code;
        socket = (*result)["socket"].toString();
        const int fd = connectTo(socket);
        if (fd < 0) return "no connection";
        stream = std::make_unique<PhosphorChannel::Stream>(fd);
        std::string why;
        input = stream->openWriter(Attach::ControlChannel, Attach::InputCodec, &why);
        return input ? std::string() : why;
    }

    /// Runs the stream until @p done, up to five seconds.
    bool until(const std::function<bool()>& done)
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            if (done()) return true;
            stream->pump(20);
            read();
        }
        return done();
    }

    void read()
    {
        std::string frame;
        if (auto* events = stream->reader(Attach::ControlChannel)) {
            while (events->read(frame) == PhosphorChannel::ReadResult::Frame) {
                if (auto event = Attach::decodeEvent(frame)) {
                    this->events.push_back(*event);
                    if (event->type == Attach::EventType::PaneOpened) pane = event->pane;
                }
            }
        }
        if (auto* state = pane ? stream->reader(pane) : nullptr) {
            while (state->read(frame) == PhosphorChannel::ReadResult::Frame) {
                std::string error;
                if (!reader.apply(replica, frame, &error)) ADD_FAILURE() << "a frame does not apply: " << error;
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

    void type(std::string_view bytes)
    {
        Attach::Input message;
        message.pane = pane;
        message.bytes = bytes;
        ASSERT_EQ(input->tryWrite(Attach::encode(message)), PhosphorChannel::WriteResult::Written);
    }

    void resize(int cols, int rows)
    {
        Attach::Input message;
        message.type = Attach::InputType::Resize;
        message.pane = pane;
        message.size = {cols, rows, 8, 16};
        ASSERT_EQ(input->tryWrite(Attach::encode(message)), PhosphorChannel::WriteResult::Written);
    }

    std::string socket;
    std::unique_ptr<PhosphorChannel::Stream> stream;
    PhosphorChannel::StreamWriter* input = nullptr;
    std::uint32_t pane = 0;
    std::vector<Attach::Event> events;
    NullHost host;
    AfterglowVt::Terminal replica{20, 5, host};
    AfterglowVt::StateReader reader;
};

/// A session that runs an interactive sh, with a plain prompt.
bool shellSession(TestDaemon& daemon, const std::string& name)
{
    SessionOptions o = daemon.options({"/bin/sh"}, name);
    o.launch.environment.emplace_back("PS1", "$ ");
    o.launch.environment.emplace_back("ENV", "/dev/null");
    return bool(daemon.newSession(o));
}

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
    w.type("echo hel''lo\n");
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "\nhello"); })) << w.screen();
    // The socket of the attach is gone once the window connected.
    EXPECT_NE(::access(w.socket.c_str(), F_OK), 0);
    const auto sessions = daemon.sessions();
    const auto it = std::find_if(sessions.begin(), sessions.end(), [](const SessionInfo& s) { return s.name == "sh"; });
    ASSERT_NE(it, sessions.end());
    EXPECT_TRUE(it->attached);
}

TEST(MuxAttach, TheWindowsSizeReachesTheProgram)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh"), "");
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "$ "); })) << w.screen();
    w.resize(97, 31);
    ASSERT_TRUE(w.until([&] { return w.replica.cols() == 97 && w.replica.rows() == 31; }));
    w.type("stty size\n");
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "31 97"); })) << w.screen();
}

TEST(MuxAttach, ASecondWindowTakesTheSessionOver)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window first;
    ASSERT_EQ(first.attach(daemon, "sh"), "");
    ASSERT_TRUE(first.until([&] { return contains(first.screen(), "$ "); }));
    first.type("echo before\n");
    ASSERT_TRUE(first.until([&] { return contains(first.screen(), "\nbefore"); }));
    Window refused;
    EXPECT_EQ(refused.attach(daemon, "sh", false), Error::Attached);
    Window second;
    ASSERT_EQ(second.attach(daemon, "sh"), "");
    ASSERT_TRUE(first.until([&] { return first.got(Attach::EventType::TakenOver); }));
    // The second window gets the whole screen, the first one's output too.
    ASSERT_TRUE(second.until([&] { return contains(second.screen(), "\nbefore"); })) << second.screen();
}

TEST(MuxAttach, DetachKeepsTheSessionForTheNextWindow)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    {
        Window w;
        ASSERT_EQ(w.attach(daemon, "sh"), "");
        ASSERT_TRUE(w.until([&] { return contains(w.screen(), "$ "); }));
        w.type("echo kept\n");
        ASSERT_TRUE(w.until([&] { return contains(w.screen(), "\nkept"); }));
        ASSERT_TRUE(daemon.call(Fn::Detach, Json::Array{"sh"})) << daemon.lastError();
        ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::Detached); }));
    }
    IpcConnection::Error error;
    EXPECT_FALSE(daemon.call(Fn::Detach, Json::Array{"sh"}, &error));
    EXPECT_EQ(error.code, Error::NotAttached);
    // Output while no window shows the session is there for the next one.
    Window again;
    ASSERT_EQ(again.attach(daemon, "sh"), "");
    ASSERT_TRUE(again.until([&] { return contains(again.screen(), "\nkept"); })) << again.screen();
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

TEST(MuxAttach, TheEndOfTheProgramEndsTheSessionForTheWindow)
{
    TestDaemon daemon;
    ASSERT_TRUE(daemon.start()) << daemon.errors();
    ASSERT_TRUE(shellSession(daemon, "sh")) << daemon.lastError();
    Window w;
    ASSERT_EQ(w.attach(daemon, "sh"), "");
    ASSERT_TRUE(w.until([&] { return contains(w.screen(), "$ "); }));
    w.type("exit 3\n");
    ASSERT_TRUE(w.until([&] { return w.got(Attach::EventType::SessionEnded); }));
    const auto closed = std::find_if(w.events.begin(), w.events.end(),
                                     [](const Attach::Event& e) { return e.type == Attach::EventType::PaneClosed; });
    ASSERT_NE(closed, w.events.end());
    EXPECT_EQ(closed->pane, w.pane);
    EXPECT_TRUE(WIFEXITED(closed->status) && WEXITSTATUS(closed->status) == 3);
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
    EXPECT_EQ(::access(socket.c_str(), F_OK), 0);
    // The connect wait of the daemon, and a margin.
    std::this_thread::sleep_for(std::chrono::milliseconds(Attach::ConnectWaitMs + 500));
    EXPECT_NE(::access(socket.c_str(), F_OK), 0);
    // The session is free for a window that asks for no takeover.
    Window w;
    EXPECT_EQ(w.attach(daemon, "sh", false), "");
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
    EXPECT_EQ(error.code, "INVALID_ARG");
    EXPECT_FALSE(daemon.call(Fn::Detach, Json::Array{"none"}, &error));
    EXPECT_EQ(error.code, Error::NoSuchSession);
}
