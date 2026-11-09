// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Socket.h>

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

using namespace AfterglowProto;

namespace {

/// An environment of the test's own: never the process's.
EnvironmentLookup environment(std::map<std::string, std::string> values)
{
    return [values = std::move(values)](const char* name) -> const char* {
        const auto it = values.find(name);
        return it == values.end() ? nullptr : it->second.c_str();
    };
}

std::vector<std::string> paths(const MuxSockets& sockets)
{
    std::vector<std::string> list;
    for (const MuxSocket& socket : sockets.candidates) list.push_back(socket.path);
    return list;
}

} // namespace

TEST(Socket, DisplayNameIsSafeInAFileName)
{
    EXPECT_EQ(displayName("wayland-1"), "wayland-1");
    EXPECT_EQ(displayName(""), "wayland-0");
    EXPECT_EQ(displayName("/"), "wayland-0");
    // An absolute display path loses its first "/", and the rest become "_".
    EXPECT_EQ(displayName("/run/user/1000/wayland-2"), "run_user_1000_wayland-2");
    EXPECT_EQ(displayName("a b..c_d"), "a_b..c_d");
    EXPECT_EQ(displayName("é"), "__"); // each byte of a character that is not ASCII
}

TEST(Socket, SharedAndOwnPathsNeedTheRuntimeDirectory)
{
    const auto env = environment({{"XDG_RUNTIME_DIR", "/run/user/1000"}, {"WAYLAND_DISPLAY", "wayland-1"}});
    EXPECT_EQ(sharedSocketPath(env), "/run/user/1000/afterglow-wayland-1.sock");
    EXPECT_EQ(ownSocketPath(4242, env), "/run/user/1000/afterglow-wayland-1-4242.sock");
    EXPECT_EQ(sharedSocketPath(environment({{"XDG_RUNTIME_DIR", "/r"}})), "/r/afterglow-wayland-0.sock");
    EXPECT_EQ(sharedSocketPath(environment({{"WAYLAND_DISPLAY", "wayland-1"}})), "");
    EXPECT_EQ(ownSocketPath(1, environment({})), "");
}

TEST(Socket, AClientUsesThePanesSocketFirst)
{
    const auto own = findSocket(environment({{"AFTERGLOW_SOCKET", "/r/afterglow-wayland-1-77.sock"},
                                             {"AFTERGLOW_PANE_ID", "12"},
                                             {"XDG_RUNTIME_DIR", "/r"}}));
    EXPECT_EQ(own.path, "/r/afterglow-wayland-1-77.sock");
    EXPECT_TRUE(own.own);
    EXPECT_EQ(own.pane, 12);
    EXPECT_TRUE(own.error.empty());

    // A pane ID that is not one names no pane.
    for (const char* bad : {"0", "-3", "12x", "", "4294967296"}) {
        const auto lookup = findSocket(environment({{"AFTERGLOW_SOCKET", "/s"}, {"AFTERGLOW_PANE_ID", bad}}));
        EXPECT_EQ(lookup.pane, 0) << bad;
        EXPECT_EQ(lookup.path, "/s") << bad;
    }
}

TEST(Socket, AClientOutsideAPaneUsesTheDisplaySocket)
{
    const auto shared = findSocket(environment({{"XDG_RUNTIME_DIR", "/r"}, {"WAYLAND_DISPLAY", "wayland-3"}}));
    EXPECT_EQ(shared.path, "/r/afterglow-wayland-3.sock");
    EXPECT_FALSE(shared.own);
    EXPECT_EQ(shared.pane, 0);

    const auto none = findSocket(environment({{"WAYLAND_DISPLAY", "wayland-3"}}));
    EXPECT_TRUE(none.path.empty());
    EXPECT_EQ(none.error, "XDG_RUNTIME_DIR is not set");
}

TEST(Socket, APaneWithoutASocketIsAnError)
{
    // Another afterglow on the display numbers its panes from 1 too.
    const auto lookup = findSocket(environment({{"AFTERGLOW_PANE_ID", "3"}, {"XDG_RUNTIME_DIR", "/r"}}));
    EXPECT_TRUE(lookup.path.empty());
    EXPECT_EQ(lookup.error, "the Afterglow that runs this pane has no socket");
}

TEST(Socket, ARelativeRuntimeDirectoryIsNotUsed)
{
    // It would follow the working directory of each process.
    const auto env = environment({{"XDG_RUNTIME_DIR", "run/user/1000"}, {"WAYLAND_DISPLAY", "wayland-1"}});
    EXPECT_EQ(sharedSocketPath(env), "");
    EXPECT_EQ(ownSocketPath(7, env), "");
    const auto lookup = findSocket(env);
    EXPECT_TRUE(lookup.path.empty());
    EXPECT_EQ(lookup.error, "XDG_RUNTIME_DIR is not an absolute path");
}

TEST(Socket, BothSpellingsOfADisplayNameOneSocket)
{
    // Wayland reads a relative display in the runtime directory.
    for (const char* runtime : {"/run/user/1000", "/run/user/1000/"}) {
        const auto env = environment({{"XDG_RUNTIME_DIR", runtime}, {"WAYLAND_DISPLAY", "/run/user/1000/wayland-1"}});
        EXPECT_TRUE(sharedSocketPath(env).ends_with("/afterglow-wayland-1.sock")) << sharedSocketPath(env);
    }
    // A display elsewhere keeps its whole path in the name.
    const auto elsewhere = environment({{"XDG_RUNTIME_DIR", "/run/user/1000"}, {"WAYLAND_DISPLAY", "/tmp/wl"}});
    EXPECT_EQ(sharedSocketPath(elsewhere), "/run/user/1000/afterglow-tmp_wl.sock");
}

TEST(Socket, APathTooLongForASocketIsAnError)
{
    const std::string runtime = "/" + std::string(MaxSocketPathBytes, 'r');
    const auto shared = findSocket(environment({{"XDG_RUNTIME_DIR", runtime}}));
    EXPECT_TRUE(shared.path.empty());
    EXPECT_NE(shared.error.find("over 107 bytes"), std::string::npos) << shared.error;
    const std::string longest = "/" + std::string(MaxSocketPathBytes - 1, 's');
    EXPECT_EQ(findSocket(environment({{"AFTERGLOW_SOCKET", longest}})).path, longest);
    const auto own = findSocket(environment({{"AFTERGLOW_SOCKET", longest + "s"}}));
    EXPECT_TRUE(own.path.empty());
    EXPECT_FALSE(own.error.empty());
}

TEST(Socket, ARuntimeDirectoryWithASlashAtItsEndGivesOneSlash)
{
    const auto env = environment({{"XDG_RUNTIME_DIR", "/run/user/1000//"}, {"WAYLAND_DISPLAY", "wayland-1"}});
    EXPECT_EQ(sharedSocketPath(env), "/run/user/1000/afterglow-wayland-1.sock");
    EXPECT_EQ(ownSocketPath(5, env), "/run/user/1000/afterglow-wayland-1-5.sock");
    EXPECT_EQ(sharedSocketPath(environment({{"XDG_RUNTIME_DIR", "/"}})), "/afterglow-wayland-0.sock");
}

TEST(Socket, ThePathCheckNamesWhatIsWrong)
{
    EXPECT_EQ(socketPathError("/r/s"), "");
    EXPECT_EQ(socketPathError(""), "the socket path is empty");
    const std::string longest = "/" + std::string(MaxSocketPathBytes - 1, 's');
    EXPECT_EQ(socketPathError(longest), "");
    EXPECT_NE(socketPathError(longest + "s").find("over 107 bytes: " + longest + "s"), std::string::npos);
}

TEST(Socket, MuxSocketsAreTheRuntimeAndTheStateDirectory)
{
    const auto env = environment({{"XDG_RUNTIME_DIR", "/run/user/1000/"},
                                  {"XDG_STATE_HOME", "/home/u/state/"},
                                  {"HOME", "/home/u"},
                                  {"WAYLAND_DISPLAY", "mux"}});
    const MuxSockets sockets = muxSockets(env);
    ASSERT_EQ(sockets.candidates.size(), 2u) << sockets.error;
    EXPECT_TRUE(sockets.error.empty()) << sockets.error;
    // A directory of its own: never afterglow-*.sock next to the terminal's
    // sockets, which a WAYLAND_DISPLAY of "mux" would name.
    EXPECT_EQ(sockets.candidates[0].path, "/run/user/1000/afterglow-mux/socket");
    EXPECT_EQ(sockets.candidates[0].directory, "/run/user/1000/afterglow-mux");
    EXPECT_NE(sockets.candidates[0].path, sharedSocketPath(env));
    // A file for each host: a home on NFS has one state directory for all.
    EXPECT_EQ(sockets.candidates[1].path, "/home/u/state/afterglow-mux/" + muxStateSocketName());
    EXPECT_EQ(sockets.candidates[1].directory, "/home/u/state/afterglow-mux");
}

TEST(Socket, MuxSocketsDoNotDependOnTheDisplay)
{
    const auto one = environment({{"XDG_RUNTIME_DIR", "/r"}, {"HOME", "/h"}, {"WAYLAND_DISPLAY", "wayland-1"}});
    const auto other = environment({{"XDG_RUNTIME_DIR", "/r"}, {"HOME", "/h"}, {"WAYLAND_DISPLAY", "wayland-7"}});
    EXPECT_EQ(paths(muxSockets(one)), paths(muxSockets(other)));
}

TEST(Socket, MuxSocketsWithoutARuntimeDirectoryHaveTheStateOne)
{
    // A relative value is not a base directory: the default under HOME.
    for (const auto& env : {environment({{"HOME", "/home/u"}}),
                            environment({{"XDG_RUNTIME_DIR", "run"}, {"XDG_STATE_HOME", "s"}, {"HOME", "/home/u/"}})}) {
        const MuxSockets sockets = muxSockets(env);
        EXPECT_EQ(paths(sockets), std::vector<std::string>{"/home/u/.local/state/afterglow-mux/" + muxStateSocketName()});
        EXPECT_NE(sockets.error.find("XDG_RUNTIME_DIR"), std::string::npos) << sockets.error;
    }
    const MuxSockets none = muxSockets(environment({{"HOME", "relative"}}));
    EXPECT_TRUE(none.candidates.empty());
    EXPECT_NE(none.error.find("XDG_RUNTIME_DIR is not set"), std::string::npos) << none.error;
    EXPECT_NE(none.error.find("neither XDG_STATE_HOME nor HOME is an absolute path"), std::string::npos) << none.error;
    // Without a state directory, the runtime one alone.
    EXPECT_EQ(paths(muxSockets(environment({{"XDG_RUNTIME_DIR", "/r"}}))), std::vector<std::string>{"/r/afterglow-mux/socket"});
}

TEST(Socket, AMuxSocketOverTheLimitIsLeftOutAlone)
{
    const std::string longDirectory = "/" + std::string(MaxSocketPathBytes, 'd');
    // The runtime one is too long: the state one is left.
    const MuxSockets state = muxSockets(environment({{"XDG_RUNTIME_DIR", longDirectory}, {"XDG_STATE_HOME", "/s"}}));
    EXPECT_EQ(paths(state), std::vector<std::string>{"/s/afterglow-mux/" + muxStateSocketName()});
    EXPECT_NE(state.error.find(longDirectory), std::string::npos) << state.error;
    // The state one is too long: the runtime one is left.
    const MuxSockets runtime = muxSockets(environment({{"XDG_RUNTIME_DIR", "/r"}, {"XDG_STATE_HOME", longDirectory}}));
    EXPECT_EQ(paths(runtime), std::vector<std::string>{"/r/afterglow-mux/socket"});
    EXPECT_NE(runtime.error.find(longDirectory), std::string::npos) << runtime.error;
    // Both: the lookup fails, and says why for each.
    const MuxSockets none = muxSockets(environment({{"XDG_RUNTIME_DIR", longDirectory}, {"HOME", longDirectory}}));
    EXPECT_TRUE(none.candidates.empty());
    EXPECT_NE(none.error.find(longDirectory + "/afterglow-mux/socket"), std::string::npos) << none.error;
    EXPECT_NE(none.error.find(longDirectory + "/.local/state/afterglow-mux/"), std::string::npos) << none.error;
}

TEST(Socket, TheStateSocketIsNamedByTheMachineId)
{
    const std::string id = "0123456789abcdef0123456789ABCDEF";
    const std::string name = muxStateSocketName(id + "\n", "box");
    // Not the ID itself (machine-id(5) keeps it confidential), and the same
    // name for the same ID whatever the host is called.
    EXPECT_EQ(name.size(), 32u + 5u) << name;
    EXPECT_TRUE(name.ends_with(".sock")) << name;
    EXPECT_EQ(name.find(id), std::string::npos) << name;
    EXPECT_EQ(name.find_first_not_of("0123456789abcdef"), 32u) << name;
    // The value itself, HMAC-SHA256 keyed with "afterglow-mux-state-socket"
    // over the ID as the file writes it (trimmed, not lowercased): the name
    // that every build gives this computer.
    EXPECT_EQ(name, "a5ac02801df2fd01f375060eb4ba2d4a.sock");
    EXPECT_EQ(muxStateSocketName(id, "other-name"), name);
    // Another computer, another name.
    EXPECT_NE(muxStateSocketName("1123456789abcdef0123456789abcdef", "box"), name);
    // No usable ID: the host name.
    EXPECT_EQ(muxStateSocketName("", "box"), "box.sock");
    EXPECT_EQ(muxStateSocketName(id.substr(1), "box"), "box.sock");
    EXPECT_EQ(muxStateSocketName(id + "0", "box"), "box.sock");
    EXPECT_EQ(muxStateSocketName("0123456789abcdef0123456789abcdeg", "a/b"), "a_b.sock");
    EXPECT_EQ(muxStateSocketName("", ""), "localhost.sock");
}

TEST(Socket, AHostNameIsSafeInAFileName)
{
    EXPECT_EQ(hostFileName("box"), "box");
    EXPECT_EQ(hostFileName("a/b/"), "a_b_");
    EXPECT_EQ(hostFileName(""), "localhost");
}
