// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/IpcServer.h>

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace AfterglowProto;

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
        for (const char* name : {"/s.sock", "/file"}) ::unlink((m_path + name).c_str());
        ::rmdir(m_path.c_str());
    }
    std::string socket() const { return m_path + "/s.sock"; }
    const std::string& path() const { return m_path; }

private:
    std::string m_path;
};

/// Accepts one connection, waiting up to a second.
int acceptOne(IpcListener& listener)
{
    pollfd p{listener.fd(), POLLIN, 0};
    if (::poll(&p, 1, 1000) != 1) return -1;
    return listener.accept();
}

/// The next line of @p connection, waiting up to a second.
std::optional<std::string> lineOf(IpcServerConnection& connection)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    std::string line;
    while (std::chrono::steady_clock::now() < deadline) {
        if (connection.nextLine(line)) return line;
        if (!connection.open()) return std::nullopt;
        pollfd p{connection.fd(), POLLIN, 0};
        ::poll(&p, 1, 50);
    }
    return std::nullopt;
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
        R"({"type":"call","id":1,"fn":"f"})",
        R"({"type":"call","id":1,"target":"t"})",
        R"({"type":"call","id":1,"target":"t","fn":"f","args":{}})",
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
    ASSERT_TRUE(listener.listen(dir.socket(), &error)) << error;
    struct stat info{};
    ASSERT_EQ(::stat(dir.socket().c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 0777, 0600u);

    std::optional<Json::Value> result;
    IpcConnection::Error callError;
    std::thread client([&] {
        IpcConnection connection;
        if (connection.connect(dir.socket(), &callError, 1000)) {
            result = connection.call("mux", "version", Json::Array{1}, 2000, &callError);
        }
    });
    const int fd = acceptOne(listener);
    ASSERT_GE(fd, 0);
    IpcServerConnection connection(fd);
    const auto line = lineOf(connection);
    ASSERT_TRUE(line);
    const auto request = parseIpcRequest(*line, &error);
    ASSERT_TRUE(request) << error;
    connection.send(ipcReply(*request->id, Json::Object{{"ok", true}}));
    client.join();
    ASSERT_TRUE(result) << callError.message;
    EXPECT_TRUE((*result)["ok"].toBool());
}

TEST(IpcServer, ReplacesAStaleSocketAndRefusesALiveOne)
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
    ASSERT_TRUE(first.listen(dir.socket(), &error)) << error;
    IpcListener second;
    EXPECT_FALSE(second.listen(dir.socket(), &error));
    EXPECT_NE(error.find("another server"), std::string::npos) << error;
    // The refused one did not remove the file of the first.
    second.close();
    EXPECT_EQ(::access(dir.socket().c_str(), F_OK), 0);
    first.close();
    EXPECT_NE(::access(dir.socket().c_str(), F_OK), 0);
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
    EXPECT_FALSE(listener.listen(file, &error));
    EXPECT_NE(error.find("not a socket"), std::string::npos) << error;
    EXPECT_EQ(::access(file.c_str(), F_OK), 0);
}

TEST(IpcServer, ClosesAConnectionWhoseLineIsTooLong)
{
    TempDir dir;
    IpcListener listener;
    std::string error;
    ASSERT_TRUE(listener.listen(dir.socket(), &error)) << error;
    const int client = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::memcpy(a.sun_path, dir.socket().data(), dir.socket().size());
    ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
    const int fd = acceptOne(listener);
    ASSERT_GE(fd, 0);
    IpcServerConnection connection(fd);
    std::thread writer([client] {
        const std::string chunk(65536, 'x');
        for (std::size_t sent = 0; sent <= IpcConnection::MaxRequestBytes + chunk.size();) {
            const ssize_t n = ::send(client, chunk.data(), chunk.size(), MSG_NOSIGNAL);
            if (n <= 0) break;
            sent += std::size_t(n);
        }
    });
    EXPECT_FALSE(lineOf(connection));
    EXPECT_FALSE(connection.open());
    writer.join();
    // The client got the reason before the close.
    std::string answer(4096, '\0');
    const ssize_t n = ::recv(client, answer.data(), answer.size(), MSG_DONTWAIT);
    ASSERT_GT(n, 0);
    answer.resize(std::size_t(n));
    EXPECT_NE(answer.find("MALFORMED_REQUEST"), std::string::npos) << answer;
    ::close(client);
}

TEST(IpcServer, ReadsLinesThatArriveTogetherAndApart)
{
    TempDir dir;
    IpcListener listener;
    std::string error;
    ASSERT_TRUE(listener.listen(dir.socket(), &error)) << error;
    const int client = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::memcpy(a.sun_path, dir.socket().data(), dir.socket().size());
    ASSERT_EQ(::connect(client, reinterpret_cast<sockaddr*>(&a), sizeof a), 0);
    const int fd = acceptOne(listener);
    ASSERT_GE(fd, 0);
    IpcServerConnection connection(fd);
    const std::string text = "one\r\n\ntwo\nthr";
    ASSERT_EQ(::send(client, text.data(), text.size(), 0), ssize_t(text.size()));
    EXPECT_EQ(lineOf(connection), "one");
    EXPECT_EQ(lineOf(connection), "two");
    ASSERT_EQ(::send(client, "ee\n", 3, 0), 3);
    EXPECT_EQ(lineOf(connection), "three");
    // A line without its end is not a request: the close ends the connection.
    ASSERT_EQ(::send(client, "four", 4, 0), 4);
    ::close(client);
    EXPECT_FALSE(lineOf(connection));
    EXPECT_FALSE(connection.open());
}
