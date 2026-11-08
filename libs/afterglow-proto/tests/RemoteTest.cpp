// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Remote.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

using namespace AfterglowProto;
using Remote::Answer;
using Remote::Error;

namespace {

/// The payload of the OSC in @p sequence, after "24569;", as a reader gets it.
std::string payload(const std::string& sequence)
{
    const std::string head = "\x1b]24569;";
    EXPECT_TRUE(sequence.starts_with(head)) << sequence;
    const std::size_t end = sequence.ends_with("\x1b\\") ? sequence.size() - 2 : sequence.size() - 1;
    return sequence.substr(head.size(), end - head.size());
}

/// A socket bound at @p path: listening when @p listen, else closed at once,
/// which leaves the file as a socket that refuses connections.
int bindSocket(const std::string& path, bool listen)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof address.sun_path - 1);
    EXPECT_EQ(::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address), 0) << path;
    if (listen) {
        EXPECT_EQ(::listen(fd, 4), 0);
        return fd;
    }
    ::close(fd);
    return -1;
}

bool exists(const std::string& path)
{
    struct stat info{};
    return ::lstat(path.c_str(), &info) == 0;
}

} // namespace

TEST(Remote, TheQueryAndEachReplyMakeTheRoundTrip)
{
    EXPECT_EQ(Remote::query(), "\x1b]24569;q=remote\x1b\\");
    EXPECT_TRUE(Remote::isQuery(payload(Remote::query())));
    EXPECT_FALSE(Remote::isQuery("q=remote;x"));
    EXPECT_FALSE(Remote::isQuery("path=/run/a.sock"));

    const Answer path{"/run/user/1000/afterglow/0123456789abcdef.sock", Error::Off};
    EXPECT_EQ(Remote::reply(path), "\x1b]24569;path=/run/user/1000/afterglow/0123456789abcdef.sock\x1b\\");
    EXPECT_EQ(Remote::parseReply(payload(Remote::reply(path))), path);
    // A query that ended with BEL gets a reply that does.
    EXPECT_TRUE(Remote::reply(path, true).ends_with('\x07'));
    EXPECT_EQ(Remote::parseReply(payload(Remote::reply(path, true))), path);

    for (const Error error : {Error::Off, Error::Forward, Error::Version, Error::Length}) {
        const Answer answer{{}, error};
        const std::string sequence = Remote::reply(answer);
        EXPECT_EQ(sequence, "\x1b]24569;error=" + std::string(Remote::errorName(error)) + "\x1b\\");
        EXPECT_EQ(Remote::parseReply(payload(sequence)), answer);
        EXPECT_EQ(Remote::errorFromName(Remote::errorName(error)), error);
    }
}

TEST(Remote, ARepliedPathIsAbsolutePlainTextOfAtMost104Bytes)
{
    EXPECT_FALSE(Remote::parseReply("path=relative/a.sock"));
    EXPECT_FALSE(Remote::parseReply("path="));
    EXPECT_FALSE(Remote::parseReply("path=/a\x1b" "b"));
    EXPECT_FALSE(Remote::parseReply("path=/a\nb"));
    EXPECT_FALSE(Remote::parseReply("error=busy"));
    EXPECT_FALSE(Remote::parseReply("q=remote"));
    EXPECT_TRUE(Remote::parseReply("path=/" + std::string(103, 'a')));
    EXPECT_FALSE(Remote::parseReply("path=/" + std::string(104, 'a')));
    // The pane never writes what a reader would refuse.
    EXPECT_EQ(Remote::reply({"/a\x07" "b", Error::Forward}), "\x1b]24569;error=off\x1b\\");
}

TEST(Remote, TmuxPassthroughDoublesEachEscape)
{
    EXPECT_EQ(Remote::tmuxPassthrough(Remote::query() + "\x1b[5n"),
              "\x1bPtmux;\x1b\x1b]24569;q=remote\x1b\x1b\\\x1b\x1b[5n\x1b\\");
}

TEST(Remote, TheDirectoryIsInTheRuntimeDirectoryElseInTheHome)
{
    EXPECT_EQ(Remote::directory("/run/user/1000", "/home/u"), "/run/user/1000/afterglow");
    EXPECT_EQ(Remote::directory("/run/user/1000/", "/home/u"), "/run/user/1000/afterglow");
    EXPECT_EQ(Remote::directory("", "/home/u"), "/home/u/.local/state/afterglow/run");
    // A relative runtime directory counts as none, as the XDG specification says.
    EXPECT_EQ(Remote::directory("run", "/home/u"), "/home/u/.local/state/afterglow/run");
    EXPECT_EQ(Remote::directory("", "home"), "");
    EXPECT_EQ(Remote::socketPath("/r/afterglow", "0123456789abcdef"), "/r/afterglow/0123456789abcdef.sock");
}

TEST(Remote, PruneRemovesOnlyTheUsersSocketsThatRefuseConnections)
{
    char pattern[] = "/tmp/aglow-prune-XXXXXX";
    ASSERT_NE(::mkdtemp(pattern), nullptr);
    const std::string dir = pattern;
    const int live = bindSocket(dir + "/live.sock", true);
    bindSocket(dir + "/dead.sock", false);
    bindSocket(dir + "/dead.other", false);
    const int file = ::open((dir + "/file.sock").c_str(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    ASSERT_GE(file, 0);
    ::close(file);

    // Another user's socket: the same file, for an owner that is not its own.
    const Remote::Pruned other = Remote::prune(dir, ::getuid() + 1);
    EXPECT_TRUE(other.removed.empty());
    EXPECT_TRUE(exists(dir + "/dead.sock"));

    const Remote::Pruned pruned = Remote::prune(dir, ::getuid());
    EXPECT_TRUE(pruned.error.empty());
    EXPECT_EQ(pruned.removed, std::vector<std::string>{"dead.sock"});
    EXPECT_FALSE(exists(dir + "/dead.sock"));
    EXPECT_TRUE(exists(dir + "/live.sock"));
    EXPECT_TRUE(exists(dir + "/file.sock"));
    // Not named as a socket of a session.
    EXPECT_TRUE(exists(dir + "/dead.other"));

    EXPECT_FALSE(Remote::prune(dir + "/missing", ::getuid()).error.empty());
    ::close(live);
    for (const char* name : {"/live.sock", "/file.sock", "/dead.other"}) ::unlink((dir + name).c_str());
    ::rmdir(dir.c_str());
}
