// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Socket.h>

#include <gtest/gtest.h>

#include <map>
#include <string>

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

TEST(Socket, MuxSocketDoesNotDependOnTheDisplay)
{
    const auto runtime = environment({{"XDG_RUNTIME_DIR", "/run/user/1000/"}, {"WAYLAND_DISPLAY", "wayland-1"}});
    EXPECT_EQ(muxSocket(runtime).path, "/run/user/1000/afterglow-mux.sock");
    // The runtime directory exists: the daemon creates nothing.
    EXPECT_EQ(muxSocket(runtime).directory, "");
}

TEST(Socket, MuxSocketFallsBackToTheStateDirectory)
{
    const auto state = environment({{"XDG_STATE_HOME", "/home/u/state/"}, {"HOME", "/home/u"}});
    EXPECT_EQ(muxSocket(state).path, "/home/u/state/afterglow/mux.sock");
    EXPECT_EQ(muxSocket(state).directory, "/home/u/state/afterglow");
    // A relative value is not a base directory: the default under HOME.
    for (const auto& env : {environment({{"HOME", "/home/u"}}),
                            environment({{"XDG_RUNTIME_DIR", "run"}, {"XDG_STATE_HOME", "s"}, {"HOME", "/home/u"}})}) {
        EXPECT_EQ(muxSocket(env).path, "/home/u/.local/state/afterglow/mux.sock");
    }
    const MuxSocket none = muxSocket(environment({{"HOME", "relative"}}));
    EXPECT_EQ(none.path, "");
    EXPECT_FALSE(none.error.empty());
}

TEST(Socket, MuxSocketRefusesAPathOverTheLimit)
{
    const std::string runtime = "/" + std::string(MaxSocketPathBytes, 'r');
    const MuxSocket socket = muxSocket(environment({{"XDG_RUNTIME_DIR", runtime}}));
    EXPECT_EQ(socket.path, "");
    EXPECT_NE(socket.error.find(runtime), std::string::npos);
}
