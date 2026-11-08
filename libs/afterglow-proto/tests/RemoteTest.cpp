// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Remote.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
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

/// A directory for sockets, removed with what is in it when the test ends,
/// also after a failed assertion. Under /tmp, not TMPDIR: sun_path holds 108
/// bytes.
struct SocketDir
{
    SocketDir()
    {
        char pattern[] = "/tmp/aglow-prune-XXXXXX";
        if (::mkdtemp(pattern)) path = pattern;
    }
    ~SocketDir()
    {
        if (path.empty()) return;
        if (DIR* dir = ::opendir(path.c_str())) {
            std::vector<std::string> names;
            while (const dirent* entry = ::readdir(dir)) {
                if (std::strcmp(entry->d_name, ".") != 0 && std::strcmp(entry->d_name, "..") != 0) names.emplace_back(entry->d_name);
            }
            ::closedir(dir);
            for (const std::string& name : names) ::unlink((path + "/" + name).c_str());
        }
        ::rmdir(path.c_str());
    }
    std::string path;
};

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

TEST(Remote, ARepliedPathIsAbsolutePlainTextOfAtMost103Bytes)
{
    EXPECT_FALSE(Remote::parseReply("path=relative/a.sock"));
    EXPECT_FALSE(Remote::parseReply("path="));
    EXPECT_FALSE(Remote::parseReply("path=/a\x1b" "b"));
    EXPECT_FALSE(Remote::parseReply("path=/a\nb"));
    EXPECT_FALSE(Remote::parseReply("error=busy"));
    EXPECT_FALSE(Remote::parseReply("q=remote"));
    // sun_path holds 104 bytes on the BSDs and macOS, with its NUL.
    EXPECT_TRUE(Remote::parseReply("path=/" + std::string(102, 'a')));
    EXPECT_FALSE(Remote::parseReply("path=/" + std::string(103, 'a')));
    // UTF-8 text only: no C1 control, no byte that is not UTF-8.
    EXPECT_TRUE(Remote::parseReply("path=/h\xc3\xa9/a.sock"));
    EXPECT_FALSE(Remote::parseReply("path=/a\xc2\x9b" "b"));
    EXPECT_FALSE(Remote::parseReply("path=/a\xff" "b"));
    EXPECT_FALSE(Remote::parseReply("path=/a\x7f" "b"));
    // The pane never writes what a reader would refuse.
    EXPECT_EQ(Remote::reply({"/a\x07" "b", Error::Forward}), "\x1b]24569;error=off\x1b\\");
}

TEST(Remote, TmuxPassthroughDoublesEachEscape)
{
    EXPECT_EQ(Remote::tmuxPassthrough(Remote::query() + "\x1b[5n"),
              "\x1bPtmux;\x1b\x1b]24569;q=remote\x1b\x1b\\\x1b\x1b[5n\x1b\\");
}

TEST(Remote, TheSocketPathIsTheTokenInTheDirectory)
{
    EXPECT_EQ(Remote::socketPath("/r/afterglow", "0123456789abcdef"), "/r/afterglow/0123456789abcdef.sock");
    EXPECT_EQ(Remote::socketPath("/r/afterglow/", "0123456789abcdef"), "/r/afterglow/0123456789abcdef.sock");
}

TEST(Remote, PruneRemovesOnlyTheUsersSocketsThatRefuseConnections)
{
    const SocketDir temp;
    ASSERT_FALSE(temp.path.empty());
    const std::string& dir = temp.path;
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
}

TEST(Remote, PruneKeepsASocketThatIsBoundButNotListeningAndASymlink)
{
    const SocketDir temp;
    ASSERT_FALSE(temp.path.empty());
    const std::string& dir = temp.path;
    // sshd binds the socket of a forward, then listens on it: between the
    // two a connection is refused, but the socket is live. /proc/net/unix
    // lists it.
    const int bound = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT_GE(bound, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string boundPath = dir + "/bound.sock";
    std::strncpy(address.sun_path, boundPath.c_str(), sizeof address.sun_path - 1);
    ASSERT_EQ(::bind(bound, reinterpret_cast<const sockaddr*>(&address), sizeof address), 0);
    // A link named as a socket, to a dead socket: not a socket itself.
    bindSocket(dir + "/target", false);
    ASSERT_EQ(::symlink((dir + "/target").c_str(), (dir + "/link.sock").c_str()), 0);
    bindSocket(dir + "/dead.sock", false);

    const Remote::Pruned pruned = Remote::prune(dir, ::getuid());
    EXPECT_TRUE(pruned.error.empty()) << pruned.error;
    EXPECT_EQ(pruned.removed, std::vector<std::string>{"dead.sock"});
    if (::access("/proc/net/unix", R_OK) == 0) {
        EXPECT_TRUE(exists(boundPath));
    }
    EXPECT_TRUE(exists(dir + "/link.sock"));
    EXPECT_TRUE(exists(dir + "/target"));
    ::close(bound);
}
