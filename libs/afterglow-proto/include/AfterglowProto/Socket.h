// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

/// Where the afterglow socket is. The terminal listens there (InstanceSocket),
/// and its clients (`afterglow ctl`, `aglow run`) look it up here, so
/// that all of them agree on the path.
namespace AfterglowProto {

/// Reads an environment variable: nullptr when it is not set. Tests give
/// their own.
using EnvironmentLookup = std::function<const char*(const char* name)>;
/// The process's environment.
const char* systemEnvironment(const char* name);

/// The longest socket path: sockaddr_un holds 108 bytes, the NUL included.
inline constexpr std::size_t MaxSocketPathBytes = 107;

/// The Wayland display's name, safe in a file name: a leading "/" goes, and
/// each byte that is not a letter, a digit, "-", "_" or "." becomes "_".
/// "wayland-0" when it is empty.
std::string displayName(std::string_view waylandDisplay);

/// $XDG_RUNTIME_DIR/afterglow-<display>.sock: the socket of the main
/// afterglow on the display. <display> is displayName() of WAYLAND_DISPLAY,
/// after a leading "$XDG_RUNTIME_DIR/": Wayland reads a relative display in
/// that directory, so both spellings name one compositor. Empty when
/// XDG_RUNTIME_DIR is not set or not an absolute path.
std::string sharedSocketPath(const EnvironmentLookup& env = systemEnvironment);
/// $XDG_RUNTIME_DIR/afterglow-<display>-<pid>.sock: the socket of an
/// afterglow that runs on its own. That is one started with --config, -o or
/// --class, and one that could not hand its window to the main afterglow
/// (another executable, or no answer). Empty as sharedSocketPath() is.
std::string ownSocketPath(std::int64_t pid, const EnvironmentLookup& env = systemEnvironment);

struct SocketLookup
{
    std::string path; ///< empty when there is none: error says why
    /// The socket of the afterglow that runs this pane ($AFTERGLOW_SOCKET).
    /// Its pane IDs are the ones that $AFTERGLOW_PANE_ID names.
    bool own = false;
    /// $AFTERGLOW_PANE_ID when the socket is the pane's own afterglow, else 0.
    std::int64_t pane = 0;
    std::string error;
};

/// The socket of a client: $AFTERGLOW_SOCKET, the afterglow that runs this
/// pane, else the shared socket of the display. A pane ID without a socket
/// is an error: another afterglow on the display numbers its panes from 1
/// too, and would act on a pane with the same number. So is a path over
/// MaxSocketPathBytes.
SocketLookup findSocket(const EnvironmentLookup& env = systemEnvironment);

} // namespace AfterglowProto
