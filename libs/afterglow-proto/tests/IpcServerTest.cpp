// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/IpcServer.h>
#include <AfterglowProto/Lock.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

using namespace AfterglowProto;
using Listen = IpcListener::Listen;

namespace {

/// A directory of its own under /tmp: TMPDIR can be too long for a socket path.
class TempDir
{
public:
    TempDir()
    {
        char dir[] = "/tmp/afterglow-ipcserver-XXXXXX";
        if (::mkdtemp(dir)) m_path = dir;
    }
    ~TempDir()
    {
        if (m_path.empty()) return;
        for (const char* name : {"/s.sock", "/s.sock.lock", "/file", "/file.lock"}) ::unlink((m_path + name).c_str());
        ::rmdir(m_path.c_str());
    }
    std::string socket() const { return m_path + "/s.sock"; }
    std::string lock() const { return socket() + ".lock"; }
    const std::string& path() const { return m_path; }

private:
    std::string m_path;
};

bool exists(const std::string& path)
{
    return ::access(path.c_str(), F_OK) == 0;
}

/// A client socket connected to @p path, with a send timeout: a test whose
/// server stops reading cannot hang in a send.
int connectTo(const std::string& path)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::memcpy(a.sun_path, path.data(), path.size());
    timeval timeout{0, 200 * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

/// Accepts one connection, waiting up to a second.
int acceptOne(IpcListener& listener)
{
    pollfd p{listener.fd(), POLLIN, 0};
    if (::poll(&p, 1, 1000) != 1) return -1;
    return listener.accept();
}

/// The next line of @p connection that is not empty, waiting up to a second.
std::optional<std::string> lineOf(IpcServerConnection& connection)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::string line;
    while (std::chrono::steady_clock::now() < deadline) {
        if (connection.nextLine(line)) {
            if (!line.empty()) return line;
            continue;
        }
        if (!connection.open()) return std::nullopt;
        pollfd p{connection.fd(), POLLIN, 0};
        ::poll(&p, 1, 50);
    }
    return std::nullopt;
}

/// What the client has received, waiting up to @p ms for the first byte.
std::string received(int client, int ms = 1000)
{
    std::string all;
    pollfd p{client, POLLIN, 0};
    if (::poll(&p, 1, ms) != 1) return all;
    char buffer[65536];
    while (true) {
        const ssize_t n = ::recv(client, buffer, sizeof buffer, MSG_DONTWAIT);
        if (n <= 0) return all;
        all.append(buffer, std::size_t(n));
    }
}

/// A listener and one connection to it, from a client socket of the test.
struct Pair
{
    TempDir dir;
    IpcListener listener;
    int client = -1;
    std::unique_ptr<IpcServerConnection> server;

    Pair()
    {
        std::string error;
        if (listener.listen(dir.socket(), &error) != Listen::Listening) return;
        client = connectTo(dir.socket());
        if (client < 0) return;
        const int fd = acceptOne(listener);
        if (fd >= 0) server = std::make_unique<IpcServerConnection>(fd);
    }
    ~Pair()
    {
        if (client >= 0) ::close(client);
    }
    bool ready() const { return server != nullptr; }
};

std::size_t count(const std::string& text, std::string_view what)
{
    std::size_t n = 0;
    for (std::size_t at = text.find(what); at != std::string::npos; at = text.find(what, at + what.size())) ++n;
    return n;
}

} // namespace

TEST(IpcServer, ParsesACall)
{
    std::string error;
    const auto request = parseIpcRequest(R"({"type":"call","id":7,"target":"mux","fn":"version","args":[1,"a"]})", &error);
    ASSERT_TRUE(request) << error;
    EXPECT_EQ(request->type, "call");
    EXPECT_EQ(request->id, 7);
    EXPECT_EQ(request->target, "mux");
    EXPECT_EQ(request->fn, "version");
    ASSERT_EQ(request->args.size(), 2u);
    EXPECT_EQ(request->args[1].toString(), "a");
}

TEST(IpcServer, RefusesWhatPhosphorIpcRefuses)
{
    const char* const bad[] = {
        "not json",
        "[1]",
        R"({"id":1})",
        R"({"type":"call","id":"1","target":"t","fn":"f"})",
        R"({"type":"call","id":1.5,"target":"t","fn":"f"})",
        R"({"type":"call","id":9.3e18,"target":"t","fn":"f"})",
        R"({"type":"call","id":1,"fn":"f"})",
        R"({"type":"call","id":1,"target":"t"})",
        R"({"type":"call","id":1,"target":"t","fn":"f","args":{}})",
        R"({"type":"subscribe","id":1,"target":"t"})",
        R"({"type":"subscribe","id":1,"signal":"s"})",
        R"({"type":"unsubscribe","id":2})",
        R"({"type":"unsubscribe","id":2,"subscriptionId":0})",
        R"({"type":"unsubscribe","id":2,"subscriptionId":"1"})",
        R"({"type":"unsubscribe","id":2,"subscriptionId":1.5})",
    };
    for (const char* line : bad) {
        std::string error;
        EXPECT_FALSE(parseIpcRequest(line, &error)) << line;
        EXPECT_FALSE(error.empty()) << line;
    }
    std::string many = R"({"type":"call","id":3,"target":"t","fn":"f","args":[)";
    for (int i = 0; i < 4097; ++i) many += i ? ",0" : "0";
    many += "]}";
    std::optional<std::int64_t> id;
    EXPECT_FALSE(parseIpcRequest(many, nullptr, &id));
    // The error can still name the request.
    EXPECT_EQ(id, 3);
}

TEST(IpcServer, ReadsFieldsInTheOrderAndRangeOfPhosphorIpc)
{
    // The type comes first: a bad id of a request without one is not the error.
    std::string error;
    EXPECT_FALSE(parseIpcRequest(R"({"id":"x"})", &error));
    EXPECT_EQ(error, "missing or non-string 'type' field");
    // Any whole number that an int64 holds, as PhosphorIpc reads an id.
    const auto large = parseIpcRequest(R"({"type":"call","id":1152921504606846976,"target":"t","fn":"f"})", &error);
    ASSERT_TRUE(large) << error;
    EXPECT_EQ(large->id, std::int64_t(1) << 60);
    const auto negative = parseIpcRequest(R"({"type":"call","id":-5,"target":"t","fn":"f"})", &error);
    ASSERT_TRUE(negative) << error;
    EXPECT_EQ(negative->id, -5);
}

// A subscription and its end, as PhosphorIpc reads them, and an event of it
// as PhosphorIpc writes one.
TEST(IpcServer, ParsesSubscriptionsAndWritesTheirEvents)
{
    std::string error;
    const auto subscribe = parseIpcRequest(R"({"type":"subscribe","id":9,"target":"control","signal":"titleChanged"})", &error);
    ASSERT_TRUE(subscribe) << error;
    EXPECT_EQ(subscribe->target, "control");
    EXPECT_EQ(subscribe->signal, "titleChanged");
    const auto unsubscribe = parseIpcRequest(R"({"type":"unsubscribe","id":10,"subscriptionId":9})", &error);
    ASSERT_TRUE(unsubscribe) << error;
    EXPECT_EQ(unsubscribe->subscriptionId, 9);
    EXPECT_EQ(ipcEvent(9, Json::Array{3, "vim"}), "{\"type\":\"event\",\"subscriptionId\":9,\"args\":[3,\"vim\"]}\n");
}

TEST(IpcServer, AnErrorWithoutAnIdHasNone)
{
    EXPECT_EQ(ipcError(std::nullopt, "C", "m"), "{\"type\":\"error\",\"code\":\"C\",\"message\":\"m\"}\n");
    EXPECT_EQ(ipcError(0, "C", "m"), "{\"type\":\"error\",\"code\":\"C\",\"message\":\"m\"}\n");
    EXPECT_EQ(ipcError(4, "C", "m"), "{\"type\":\"error\",\"id\":4,\"code\":\"C\",\"message\":\"m\"}\n");
}

TEST(IpcServer, AnswersTheClientOfThisLibrary)
{
    TempDir dir;
    IpcListener listener;
    std::string error;
    ASSERT_EQ(listener.listen(dir.socket(), &error), Listen::Listening) << error;
    struct stat info{};
    ASSERT_EQ(::stat(dir.socket().c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600u);

    std::optional<Json::Value> result;
    IpcConnection::Error callError;
    // Joined on every return, an ASSERT's too: the client's waits end it.
    std::jthread client([&] {
        IpcConnection connection;
        if (connection.connect(dir.socket(), &callError, 1000)) {
            result = connection.call("mux", "version", Json::Array{1}, 2000, &callError);
        }
    });
    const int fd = acceptOne(listener);
    ASSERT_GE(fd, 0);
    IpcServerConnection connection(fd);
    IpcRequest request;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    IpcServerConnection::Read read = IpcServerConnection::Read::None;
    while (std::chrono::steady_clock::now() < deadline && (read = connection.nextRequest(request)) != IpcServerConnection::Read::Request) {
        pollfd p{fd, POLLIN, 0};
        ::poll(&p, 1, 50);
    }
    ASSERT_EQ(read, IpcServerConnection::Read::Request);
    connection.send(ipcReply(*request.id, Json::Object{{"ok", true}}));
    client.join();
    ASSERT_TRUE(result) << callError.message;
    EXPECT_TRUE((*result)["ok"].toBool());
}

TEST(IpcServer, ReplacesAStaleSocketAndRefusesAHeldOne)
{
    TempDir dir;
    {
        // A socket file whose server is gone.
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        std::memcpy(a.sun_path, dir.socket().data(), dir.socket().size());
        ASSERT_EQ(::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
        ::close(fd);
    }
    IpcListener first;
    std::string error;
    ASSERT_EQ(first.listen(dir.socket(), &error), Listen::Listening) << error;
    EXPECT_TRUE(exists(dir.lock()));
    IpcListener second;
    EXPECT_EQ(second.listen(dir.socket(), &error), Listen::Held);
    EXPECT_NE(error.find("another server listens on " + dir.socket()), std::string::npos) << error;
    // The refused one removed nothing of the first.
    second.close();
    EXPECT_TRUE(exists(dir.socket()));
    EXPECT_TRUE(exists(dir.lock()));
    first.close();
    EXPECT_FALSE(exists(dir.socket()));
    EXPECT_FALSE(exists(dir.lock()));
    // The path is free again.
    EXPECT_EQ(second.listen(dir.socket(), &error), Listen::Listening) << error;
}

TEST(IpcServer, TheLockAloneDecidesWhoListens)
{
    // A server that holds the lock serves the path, whether its socket file
    // is there yet or not: the second never touches it.
    TempDir dir;
    const int holder = ::open(dir.lock().c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    ASSERT_GE(holder, 0);
    ASSERT_EQ(::flock(holder, LOCK_EX | LOCK_NB), 0);
    IpcListener listener;
    std::string error;
    EXPECT_EQ(listener.listen(dir.socket(), &error), Listen::Held);
    EXPECT_FALSE(exists(dir.socket()));
    // A holder that lets go removes its lock file.
    ::unlink(dir.lock().c_str());
    ::close(holder);
    EXPECT_EQ(listener.listen(dir.socket(), &error), Listen::Listening) << error;
}

TEST(IpcServer, ALockFileIsHeldOnlyWhileAHolderHasItsLock)
{
    // A file that a killed holder left has no lock: a client that waits for
    // the holder of such a file would wait for nothing.
    TempDir dir;
    EXPECT_FALSE(AfterglowProto::lockFileHeld(dir.lock()));
    const int left = ::open(dir.lock().c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    ASSERT_GE(left, 0);
    ::close(left);
    EXPECT_FALSE(AfterglowProto::lockFileHeld(dir.lock()));
    std::string error;
    const int held = AfterglowProto::lockFile(dir.lock(), &error);
    ASSERT_GE(held, 0) << error;
    EXPECT_TRUE(AfterglowProto::lockFileHeld(dir.lock()));
    // Asking takes no lock: the holder keeps it, and another taker is refused.
    EXPECT_EQ(AfterglowProto::lockFile(dir.lock(), &error), AfterglowProto::LockHeld);
    AfterglowProto::unlockFile(dir.lock(), held);
    EXPECT_FALSE(AfterglowProto::lockFileHeld(dir.lock()));
}

TEST(IpcServer, ALockFileIsHeldOnTheFileSystemOfTheTests)
{
    // /proc/locks names the device of the file system, which stat() does not
    // give on a btrfs subvolume. /tmp is often tmpfs, where the two agree:
    // this test makes its file where ctest runs it, in the build directory.
    std::string dir = (std::filesystem::current_path() / "lockheld-XXXXXX").string();
    ASSERT_NE(::mkdtemp(dir.data()), nullptr);
    const std::string lock = dir + "/s.sock.lock";
    std::string error;
    const int held = AfterglowProto::lockFile(lock, &error);
    EXPECT_GE(held, 0) << error;
    if (held >= 0) {
        EXPECT_TRUE(AfterglowProto::lockFileHeld(lock));
        AfterglowProto::unlockFile(lock, held);
    }
    EXPECT_FALSE(AfterglowProto::lockFileHeld(lock));
    std::error_code ignored;
    std::filesystem::remove_all(dir, ignored);
}

TEST(IpcServer, ACloseRemovesOnlyTheFilesOfItsOwnListener)
{
    TempDir dir;
    IpcListener first;
    std::string error;
    ASSERT_EQ(first.listen(dir.socket(), &error), Listen::Listening) << error;
    // Something removed the files of the first (a runtime directory that
    // went), and a second server bound the path again.
    ::unlink(dir.socket().c_str());
    ::unlink(dir.lock().c_str());
    IpcListener second;
    ASSERT_EQ(second.listen(dir.socket(), &error), Listen::Listening) << error;
    first.close();
    EXPECT_TRUE(exists(dir.socket()));
    EXPECT_TRUE(exists(dir.lock()));
    // The second still serves, and still holds its path.
    const int client = connectTo(dir.socket());
    ASSERT_GE(client, 0);
    EXPECT_GE(acceptOne(second), 0);
    ::close(client);
    IpcListener third;
    EXPECT_EQ(third.listen(dir.socket(), &error), Listen::Held);
}

TEST(IpcServer, StillOursSaysWhetherThePathIsTheBoundSocket)
{
    TempDir dir;
    IpcListener listener;
    EXPECT_FALSE(listener.stillOurs());
    std::string error;
    ASSERT_EQ(listener.listen(dir.socket(), &error), Listen::Listening) << error;
    EXPECT_TRUE(listener.stillOurs());
    // Removed, as logind removes the runtime directory, then a file of the
    // same name: the listener still listens, on a socket that no path names.
    ::unlink(dir.socket().c_str());
    EXPECT_FALSE(listener.stillOurs());
    IpcListener other;
    ::unlink(dir.lock().c_str());
    ASSERT_EQ(other.listen(dir.socket(), &error), Listen::Listening) << error;
    EXPECT_FALSE(listener.stillOurs());
    EXPECT_TRUE(other.stillOurs());
    other.close();
    EXPECT_FALSE(other.stillOurs());
}

TEST(IpcServer, RefusesAPathThatIsNotASocket)
{
    TempDir dir;
    const std::string file = dir.path() + "/file";
    const int fd = ::open(file.c_str(), O_CREAT | O_WRONLY, 0600);
    ASSERT_GE(fd, 0);
    ::close(fd);
    IpcListener listener;
    std::string error;
    EXPECT_EQ(listener.listen(file, &error), Listen::Failed);
    EXPECT_NE(error.find("not a socket"), std::string::npos) << error;
    EXPECT_TRUE(exists(file));
    // Its lock went with the failure.
    EXPECT_FALSE(exists(file + ".lock"));
    EXPECT_EQ(listener.listen("", &error), Listen::Failed);
    EXPECT_EQ(error, "the socket path is empty");
}

TEST(IpcServer, ClosesAConnectionWhoseLineIsTooLong)
{
    Pair pair;
    ASSERT_TRUE(pair.ready());
    const int client = pair.client;
    std::atomic<bool> stop = false;
    // A send timeout and a stop: the writer ends even when the server never reads.
    std::jthread writer([client, &stop] {
        const std::string chunk(65536, 'x');
        for (std::size_t sent = 0; !stop && sent <= IpcConnection::MaxRequestBytes + chunk.size();) {
            const ssize_t n = ::send(client, chunk.data(), chunk.size(), MSG_NOSIGNAL);
            if (n < 0 && errno == EAGAIN) continue;
            if (n <= 0) break;
            sent += std::size_t(n);
        }
    });
    EXPECT_FALSE(lineOf(*pair.server));
    EXPECT_FALSE(pair.server->open());
    stop = true;
    writer.join();
    // The client got the reason before the close.
    EXPECT_NE(received(client).find("MALFORMED_REQUEST"), std::string::npos);
}

TEST(IpcServer, TheLineLimitCountsTheNewline)
{
    // As PhosphorIpc::IpcRouter: MaxRequestBytes with its '\n' is a line,
    // one byte more is not.
    for (const std::size_t size : {IpcConnection::MaxRequestBytes - 1, IpcConnection::MaxRequestBytes}) {
        Pair pair;
        ASSERT_TRUE(pair.ready());
        const std::string text = std::string(size, 'x') + "\n";
        const int client = pair.client;
        std::atomic<bool> stop = false;
        std::jthread writer([client, &text, &stop] {
            for (std::size_t sent = 0; !stop && sent < text.size();) {
                const ssize_t n = ::send(client, text.data() + sent, text.size() - sent, MSG_NOSIGNAL);
                if (n < 0 && errno == EAGAIN) continue;
                if (n <= 0) break;
                sent += std::size_t(n);
            }
        });
        const auto line = lineOf(*pair.server);
        stop = true;
        if (size < IpcConnection::MaxRequestBytes) {
            ASSERT_TRUE(line);
            EXPECT_EQ(line->size(), size);
            EXPECT_TRUE(pair.server->open());
        } else {
            EXPECT_FALSE(line);
            EXPECT_FALSE(pair.server->open());
        }
    }
}

TEST(IpcServer, ReadsLinesThatArriveTogetherAndApart)
{
    Pair pair;
    ASSERT_TRUE(pair.ready());
    IpcServerConnection& connection = *pair.server;
    const std::string text = "one\r\n\ntwo\nthr";
    ASSERT_EQ(::send(pair.client, text.data(), text.size(), 0), ssize_t(text.size()));
    EXPECT_EQ(lineOf(connection), "one");
    EXPECT_EQ(lineOf(connection), "two");
    ASSERT_EQ(::send(pair.client, "ee\n", 3, 0), 3);
    EXPECT_EQ(lineOf(connection), "three");
    // A line without its end is not a request: the close ends the connection.
    ASSERT_EQ(::send(pair.client, "four", 4, 0), 4);
    ::close(pair.client);
    pair.client = -1;
    EXPECT_FALSE(lineOf(connection));
    EXPECT_FALSE(connection.open());
}

TEST(IpcServer, OneLineAtATimeAndPendingSaysMoreWait)
{
    Pair pair;
    ASSERT_TRUE(pair.ready());
    IpcServerConnection& connection = *pair.server;
    EXPECT_FALSE(connection.pending());
    const std::string text = "a\n\nb\nc";
    ASSERT_EQ(::send(pair.client, text.data(), text.size(), 0), ssize_t(text.size()));
    pollfd p{connection.fd(), POLLIN, 0};
    ASSERT_EQ(::poll(&p, 1, 1000), 1);
    std::string line;
    ASSERT_TRUE(connection.nextLine(line));
    EXPECT_EQ(line, "a");
    // The rest is in the buffer: the socket has nothing more to say.
    EXPECT_TRUE(connection.pending());
    int unread = -1;
    ASSERT_EQ(::ioctl(connection.fd(), FIONREAD, &unread), 0);
    EXPECT_EQ(unread, 0);
    // An empty line is a line: the caller counts it.
    ASSERT_TRUE(connection.nextLine(line));
    EXPECT_EQ(line, "");
    ASSERT_TRUE(connection.nextLine(line));
    EXPECT_EQ(line, "b");
    EXPECT_FALSE(connection.pending());
    EXPECT_FALSE(connection.nextLine(line));
    EXPECT_TRUE(connection.open());
}

TEST(IpcServer, AStreamOfEmptyLinesDoesNotKeepTheReader)
{
    // A client that sends "\n" without a pause: each call returns at once,
    // after one read at most, however long the stream goes on.
    Pair pair;
    ASSERT_TRUE(pair.ready());
    const int client = pair.client;
    std::atomic<bool> stop = false;
    std::jthread writer([client, &stop] {
        const std::string chunk(65536, '\n');
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!stop && std::chrono::steady_clock::now() < end) {
            const ssize_t n = ::send(client, chunk.data(), chunk.size(), MSG_NOSIGNAL);
            if (n < 0 && errno != EAGAIN) break;
        }
    });
    IpcServerConnection& connection = *pair.server;
    std::string line;
    std::chrono::steady_clock::duration longest{};
    std::size_t lines = 0;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
    while (std::chrono::steady_clock::now() < end) {
        const auto start = std::chrono::steady_clock::now();
        const bool got = connection.nextLine(line);
        longest = std::max(longest, std::chrono::steady_clock::now() - start);
        if (got) {
            ++lines;
            EXPECT_TRUE(line.empty());
        }
    }
    stop = true;
    writer.join();
    EXPECT_GT(lines, 0u);
    EXPECT_LT(longest, std::chrono::milliseconds(200));
    EXPECT_TRUE(connection.open());
}

TEST(IpcServer, ClosesAfterTooManyMalformedLinesInARow)
{
    Pair pair;
    ASSERT_TRUE(pair.ready());
    IpcServerConnection& connection = *pair.server;
    const auto drive = [&] {
        IpcRequest request;
        int requests = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (connection.open() && std::chrono::steady_clock::now() < deadline) {
            const auto read = connection.nextRequest(request);
            if (read == IpcServerConnection::Read::Request) ++requests;
            if (read != IpcServerConnection::Read::None) continue;
            pollfd p{connection.fd(), POLLIN, 0};
            if (::poll(&p, 1, 50) == 0) break;
        }
        return requests;
    };
    const std::string good = R"({"type":"call","id":1,"target":"t","fn":"f"})" "\n";
    std::string text;
    for (int i = 0; i < IpcServerConnection::MaxMalformedLines; ++i) text += "bad\n";
    // A good line ends the run, and an empty line does not count.
    text += good + "\n";
    for (int i = 0; i < IpcServerConnection::MaxMalformedLines; ++i) text += "bad\n";
    ASSERT_EQ(::send(pair.client, text.data(), text.size(), 0), ssize_t(text.size()));
    EXPECT_EQ(drive(), 1);
    EXPECT_TRUE(connection.open());
    EXPECT_EQ(count(received(pair.client), "MALFORMED_REQUEST"), std::size_t(2 * IpcServerConnection::MaxMalformedLines));
    // One more in the run closes it, after its error.
    ASSERT_EQ(::send(pair.client, "bad\n", 4, 0), 4);
    EXPECT_EQ(drive(), 0);
    EXPECT_FALSE(connection.open());
    EXPECT_EQ(count(received(pair.client), "MALFORMED_REQUEST"), 1u);
}

TEST(IpcServer, ClosesAConnectionWhoseOutputWaitsTooLong)
{
    // A client that reads nothing: the waiting output passes the limit.
    Pair pair;
    ASSERT_TRUE(pair.ready());
    IpcServerConnection& connection = *pair.server;
    const std::string line(1024 * 1024, 'r');
    std::size_t sends = 0;
    while (connection.open() && sends < 64) {
        connection.send(line);
        ++sends;
    }
    EXPECT_FALSE(connection.open());
    EXPECT_LE(sends, IpcServerConnection::MaxWaitingBytes / line.size() + 1);
}

TEST(IpcServer, WaitingOutputGoesOutWholeAndInOrder)
{
    // Lines that the socket takes in parts, while more are queued between
    // the flushes: the client reads every byte once, in order.
    Pair pair;
    ASSERT_TRUE(pair.ready());
    IpcServerConnection& connection = *pair.server;
    std::string sent;
    for (int i = 0; i < 40; ++i) {
        std::string line = std::to_string(i) + ":" + std::string(std::size_t(50000 + i * 1000), char('a' + i % 26)) + "\n";
        sent += line;
        connection.send(std::move(line));
    }
    ASSERT_TRUE(connection.wantsWrite());
    std::string got;
    char buffer[16384];
    int queued = 40;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (got.size() < sent.size() && std::chrono::steady_clock::now() < deadline) {
        // Small reads, so that the server's output waits in parts.
        const ssize_t n = ::recv(pair.client, buffer, sizeof buffer, MSG_DONTWAIT);
        if (n > 0) got.append(buffer, std::size_t(n));
        ASSERT_TRUE(connection.flush());
        if (queued < 60 && got.size() % 3 == 0) {
            std::string line = std::to_string(queued++) + ":more\n";
            sent += line;
            connection.send(std::move(line));
        }
        if (n <= 0) {
            pollfd p{pair.client, POLLIN, 0};
            ::poll(&p, 1, 10);
        }
    }
    EXPECT_TRUE(got == sent) << got.size() << " of " << sent.size() << " bytes";
    EXPECT_FALSE(connection.wantsWrite());
}

TEST(IpcServer, AClientThatEndsItsSideStillGetsItsAnswers)
{
    Pair pair;
    ASSERT_TRUE(pair.ready());
    IpcServerConnection& connection = *pair.server;
    // More than the socket takes at once: part of it waits.
    const std::string answer(4 * 1024 * 1024, 'a');
    connection.send(answer + "\n");
    ASSERT_TRUE(connection.wantsWrite());
    ASSERT_EQ(::shutdown(pair.client, SHUT_WR), 0);
    pollfd p{connection.fd(), POLLIN, 0};
    ASSERT_EQ(::poll(&p, 1, 1000), 1);
    std::string line;
    EXPECT_FALSE(connection.nextLine(line));
    // The end of the input does not drop the output.
    EXPECT_TRUE(connection.open());
    EXPECT_FALSE(connection.wantsRead());
    std::string got;
    char buffer[65536];
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (connection.open() && std::chrono::steady_clock::now() < deadline) {
        const ssize_t n = ::recv(pair.client, buffer, sizeof buffer, MSG_DONTWAIT);
        if (n > 0) got.append(buffer, std::size_t(n));
        connection.flush();
    }
    got += received(pair.client, 100);
    EXPECT_TRUE(got == answer + "\n") << got.size() << " bytes";
    // Once it all went out, the connection closed.
    EXPECT_FALSE(connection.open());
}

TEST(IpcServer, WithoutDescriptorsTheWaitingConnectionsAreClosed)
{
    TempDir dir;
    IpcListener listener;
    std::string error;
    ASSERT_EQ(listener.listen(dir.socket(), &error), Listen::Listening) << error;
    int clients[3];
    for (int& client : clients) {
        client = connectTo(dir.socket());
        ASSERT_GE(client, 0);
    }
    rlimit saved{};
    ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &saved), 0);
    // The lowest free descriptor: with this limit, no new one opens.
    const int lowest = ::dup(0);
    ASSERT_GE(lowest, 0);
    ::close(lowest);
    rlimit low = saved;
    low.rlim_cur = rlim_t(lowest);
    ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &low), 0);
    const int accepted = listener.accept();
    pollfd p{listener.fd(), POLLIN, 0};
    const int readable = ::poll(&p, 1, 0);
    ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &saved), 0);
    EXPECT_EQ(accepted, -1);
    // The queue is empty: a poll does not wake for it again.
    EXPECT_EQ(readable, 0);
    for (const int client : clients) {
        char byte = 0;
        pollfd c{client, POLLIN, 0};
        EXPECT_EQ(::poll(&c, 1, 1000), 1);
        EXPECT_EQ(::recv(client, &byte, 1, MSG_DONTWAIT), 0);
        ::close(client);
    }
    // With descriptors again, the listener accepts as before.
    const int client = connectTo(dir.socket());
    ASSERT_GE(client, 0);
    const int fd = acceptOne(listener);
    EXPECT_GE(fd, 0);
    if (fd >= 0) ::close(fd);
    ::close(client);
}

TEST(IpcServer, AListenerWithoutItsKeptDescriptorSaysItIsStarved)
{
    TempDir dir;
    // Made first: connect() takes no new descriptor.
    int clients[2];
    for (int& client : clients) {
        client = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        ASSERT_GE(client, 0);
    }
    rlimit saved{};
    ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &saved), 0);
    const auto lowestFree = [] {
        const int fd = ::dup(0);
        if (fd >= 0) ::close(fd);
        return fd;
    };
    // Room for the lock file and the socket, and not for the kept
    // descriptor: the listener starts without it.
    const int lowest = lowestFree();
    ASSERT_GE(lowest, 0);
    rlimit low = saved;
    low.rlim_cur = rlim_t(lowest + 2);
    ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &low), 0);
    IpcListener listener;
    std::string error;
    const Listen listened = listener.listen(dir.socket(), &error);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::memcpy(a.sun_path, dir.socket().data(), dir.socket().size());
    bool connected = true;
    for (const int client : clients) connected = connected && ::connect(client, reinterpret_cast<sockaddr*>(&a), sizeof a) == 0;
    const int starvedAccept = listener.accept();
    const bool starved = listener.starved();
    pollfd p{listener.fd(), POLLIN, 0};
    const int readable = ::poll(&p, 1, 0);
    ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &saved), 0);
    ASSERT_EQ(listened, Listen::Listening) << error;
    ASSERT_TRUE(connected) << std::strerror(errno);
    EXPECT_EQ(starvedAccept, -1);
    EXPECT_TRUE(starved);
    // The connection still waits, so the listener stays readable: a caller
    // leaves it out of its poll until a descriptor was freed.
    EXPECT_EQ(readable, 1);

    // With descriptors again, the kept one comes back first, and the
    // waiting connection is accepted.
    const int fd = listener.accept();
    EXPECT_GE(fd, 0);
    EXPECT_FALSE(listener.starved());
    if (fd >= 0) ::close(fd);
    // At the limit once more, the kept descriptor drains the other waiting
    // connection: the listener is not starved, and not readable.
    const int next = lowestFree();
    ASSERT_GE(next, 0);
    low.rlim_cur = rlim_t(next);
    ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &low), 0);
    const int drained = listener.accept();
    const bool starvedAgain = listener.starved();
    const int readableAgain = ::poll(&p, 1, 0);
    ASSERT_EQ(::setrlimit(RLIMIT_NOFILE, &saved), 0);
    EXPECT_EQ(drained, -1);
    EXPECT_FALSE(starvedAgain);
    EXPECT_EQ(readableAgain, 0);
    for (const int client : clients) ::close(client);
}
