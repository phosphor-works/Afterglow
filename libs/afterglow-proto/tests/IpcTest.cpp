// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "FakeServer.h"

#include <AfterglowProto/Ipc.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include <fcntl.h>
#include <unistd.h>

using namespace AfterglowProto;
using AfterglowProto::Test::FakeServer;

namespace {

/// A client connected to @p server, and the server's end accepted.
bool connect(IpcConnection& client, FakeServer& server)
{
    IpcConnection::Error error;
    return server.valid() && client.connect(server.path(), &error, 5000) && server.accept();
}

std::string eventLine(int n)
{
    return Json::write(Json::Object{{"type", "event"}, {"target", "t"}, {"signal", "s"}, {"args", Json::Array{n}}}) + "\n";
}

long elapsedMs(std::chrono::steady_clock::time_point since)
{
    return long(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count());
}

} // namespace

TEST(Ipc, ReadsFramesInPiecesAndSkipsEmptyLines)
{
    FakeServer server;
    IpcConnection client;
    ASSERT_TRUE(connect(client, server));
    const std::string line = eventLine(1);
    ASSERT_TRUE(server.write("\n" + line.substr(0, 10)));
    Json::Value frame;
    EXPECT_EQ(client.next(frame, 50), IpcConnection::Read::Timeout);
    ASSERT_TRUE(server.write(line.substr(10) + eventLine(2)));
    std::size_t bytes = 0;
    ASSERT_EQ(client.next(frame, 5000, -1, &bytes), IpcConnection::Read::Frame);
    EXPECT_EQ(frame["args"].toArray().at(0).toInteger(), 1);
    EXPECT_EQ(bytes, line.size());
    ASSERT_EQ(client.next(frame, 5000), IpcConnection::Read::Frame);
    EXPECT_EQ(frame["args"].toArray().at(0).toInteger(), 2);
}

TEST(Ipc, ACallLeavesOtherFramesForNext)
{
    FakeServer server;
    IpcConnection client;
    ASSERT_TRUE(connect(client, server));
    // The answer to the first call has id 1: it comes after two events.
    ASSERT_TRUE(server.write(eventLine(1) + eventLine(2)));
    ASSERT_TRUE(server.frame(Json::Object{{"type", "reply"}, {"id", 1}, {"result", Json::Object{{"x", "y"}}}}));
    IpcConnection::Error error;
    const auto result = client.call("t", "f", {}, 5000, &error);
    ASSERT_TRUE(result) << error.message;
    EXPECT_EQ((*result)["x"].toString(), "y");
    const auto request = server.call();
    ASSERT_TRUE(request);
    EXPECT_EQ((*request)["fn"].toString(), "f");
    Json::Value frame;
    for (int n : {1, 2}) {
        ASSERT_EQ(client.next(frame, 0), IpcConnection::Read::Frame);
        EXPECT_EQ(frame["args"].toArray().at(0).toInteger(), n);
    }
    EXPECT_EQ(client.next(frame, 0), IpcConnection::Read::Timeout);
}

TEST(Ipc, AnErrorWithoutAnIdAnswersTheCall)
{
    FakeServer server;
    IpcConnection client;
    ASSERT_TRUE(connect(client, server));
    ASSERT_TRUE(server.frame(Json::Object{{"type", "error"}, {"code", "MALFORMED_REQUEST"}, {"message", "bad"}}));
    IpcConnection::Error error;
    EXPECT_FALSE(client.call("t", "f", {}, 5000, &error));
    EXPECT_EQ(error.code, "MALFORMED_REQUEST");
    EXPECT_EQ(error.message, "bad");
}

TEST(Ipc, ALineThatIsNotJsonClosesWithItsReason)
{
    FakeServer server;
    IpcConnection client;
    ASSERT_TRUE(connect(client, server));
    ASSERT_TRUE(server.write("{\"type\": nope}\n"));
    IpcConnection::Error error;
    EXPECT_FALSE(client.call("t", "f", {}, 5000, &error));
    EXPECT_FALSE(client.connected());
    EXPECT_NE(error.message.find("not a JSON object"), std::string::npos) << error.message;
    EXPECT_NE(error.message.find("at byte 9"), std::string::npos) << error.message;
    EXPECT_EQ(client.closeReason(), error.message);
}

TEST(Ipc, ALineOverTheFrameLimitCloses)
{
    FakeServer server;
    IpcConnection client;
    ASSERT_TRUE(connect(client, server));
    std::thread writer([&] { server.write(std::string(IpcConnection::MaxFrameBytes + 70000, 'x')); });
    Json::Value frame;
    EXPECT_EQ(client.next(frame, 20000), IpcConnection::Read::Closed);
    EXPECT_NE(client.closeReason().find("over 32 MiB"), std::string::npos) << client.closeReason();
    writer.join(); // its send fails once the client has closed
}

TEST(Ipc, ACallWithoutATimeoutWaits)
{
    FakeServer server;
    IpcConnection client;
    ASSERT_TRUE(connect(client, server));
    std::thread answer([&] {
        const auto request = server.call();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (request) server.reply(*request, "late");
    });
    IpcConnection::Error error;
    const auto result = client.call("t", "f", {}, -1, &error);
    answer.join();
    ASSERT_TRUE(result) << error.message;
    EXPECT_EQ(result->toString(), "late");
}

TEST(Ipc, FramesThatKeepComingDoNotHoldACallPastItsDeadline)
{
    FakeServer server;
    IpcConnection client;
    ASSERT_TRUE(connect(client, server));
    std::atomic<bool> stop = false;
    std::thread flood([&] {
        for (int n = 0; !stop && server.write(eventLine(n)); ++n) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    const auto start = std::chrono::steady_clock::now();
    IpcConnection::Error error;
    EXPECT_FALSE(client.call("t", "f", {}, 200, &error));
    EXPECT_TRUE(error.timedOut);
    EXPECT_LT(elapsedMs(start), 2000);
    stop = true;
    flood.join();
}

TEST(Ipc, AWakeDescriptorEndsTheWaitEvenWhenItHangsUp)
{
    FakeServer server;
    IpcConnection client;
    ASSERT_TRUE(connect(client, server));
    int wake[2];
    ASSERT_EQ(::pipe(wake), 0);
    Json::Value frame;
    ASSERT_EQ(::write(wake[1], "x", 1), 1);
    EXPECT_EQ(client.next(frame, 5000, wake[0]), IpcConnection::Read::Woken);
    char byte;
    ASSERT_EQ(::read(wake[0], &byte, 1), 1);
    // A pipe whose writer went away polls as hung up, not readable.
    ::close(wake[1]);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(client.next(frame, 5000, wake[0]), IpcConnection::Read::Woken);
    EXPECT_LT(elapsedMs(start), 2000);
    ::close(wake[0]);
}

TEST(Ipc, AWakeDescriptorEndsASendThatBlocks)
{
    FakeServer server;
    IpcConnection client;
    ASSERT_TRUE(connect(client, server));
    int wake[2];
    ASSERT_EQ(::pipe(wake), 0);
    ASSERT_EQ(::write(wake[1], "x", 1), 1);
    // The server reads nothing: the request fills the socket's buffer and
    // the rest of it waits, until the wake descriptor ends the wait. A send
    // buffer (net.core.wmem_default) that takes the whole request has no
    // wait to end, and a request cannot be larger than 1 MiB.
    IpcConnection::Error error;
    const bool sent = client.send("t", "f", {std::string(900 * 1024, 'x')}, &error, wake[0]).has_value();
    if (sent) {
        ::close(wake[0]);
        ::close(wake[1]);
        GTEST_SKIP() << "the socket's send buffer took the whole request";
    }
    EXPECT_TRUE(error.interrupted);
    // Part of the line went out: the next request would join it.
    EXPECT_FALSE(client.connected());
    EXPECT_NE(client.closeReason().find("cut off"), std::string::npos) << client.closeReason();
    ::close(wake[0]);
    ::close(wake[1]);
}

TEST(Ipc, ARequestOverTheLimitIsNotSent)
{
    FakeServer server;
    IpcConnection client;
    ASSERT_TRUE(connect(client, server));
    IpcConnection::Error error;
    EXPECT_FALSE(client.send("t", "f", {std::string(IpcConnection::MaxRequestBytes, 'x')}, &error));
    EXPECT_EQ(error.code, IpcConnection::RequestTooLong);
    EXPECT_EQ(error.message, "the request is over 1 MiB");
    EXPECT_TRUE(client.connected());
    // The id is not used up.
    EXPECT_EQ(client.send("t", "f", {}, &error), 1);
}

TEST(Ipc, CloseDropsTheFramesThatWait)
{
    FakeServer server;
    IpcConnection client;
    ASSERT_TRUE(connect(client, server));
    ASSERT_TRUE(server.write(eventLine(1)));
    ASSERT_TRUE(server.frame(Json::Object{{"type", "reply"}, {"id", 1}, {"result", nullptr}}));
    IpcConnection::Error error;
    ASSERT_TRUE(client.call("t", "f", {}, 5000, &error));
    // The event of the old connection waits for next(): a new one must not get it.
    ASSERT_TRUE(connect(client, server));
    Json::Value frame;
    EXPECT_EQ(client.next(frame, 50), IpcConnection::Read::Timeout);
}

TEST(Ipc, AConnectionThatTheServerDoesNotTakeTimesOut)
{
    // A backlog of 0 holds one connection: a second one waits.
    FakeServer server(0);
    ASSERT_TRUE(server.valid());
    IpcConnection first;
    IpcConnection::Error error;
    ASSERT_TRUE(first.connect(server.path(), &error, 1000)) << error.message;
    IpcConnection second;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(second.connect(server.path(), &error, 200));
    EXPECT_TRUE(error.timedOut) << error.message;
    EXPECT_LT(elapsedMs(start), 3000);
}

TEST(Ipc, AnAnswerGivesTheResult)
{
    Json::Value frame = Json::Object{{"type", "reply"}, {"id", 3}, {"result", Json::Array{"a", 1}}};
    IpcConnection::Error error;
    EXPECT_TRUE(IpcConnection::answers(frame, 3));
    EXPECT_FALSE(IpcConnection::answers(frame, 4));
    EXPECT_EQ(IpcConnection::answer(frame, &error), (Json::Value(Json::Array{"a", 1})));
    EXPECT_EQ(IpcConnection::answer(std::move(frame), &error), (Json::Value(Json::Array{"a", 1})));
    const Json::Value failed = Json::Object{{"type", "error"}, {"id", 3}, {"code", "C"}, {"message", "M"}};
    EXPECT_FALSE(IpcConnection::answer(failed, &error));
    EXPECT_EQ(error.code, "C");
    EXPECT_EQ(error.message, "M");
}
