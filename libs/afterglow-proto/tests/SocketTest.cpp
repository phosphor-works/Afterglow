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
    EXPECT_EQ(lookup.error, "the afterglow that runs this pane has no socket");
}
