// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

/// Where the afterglow socket is. The terminal listens there (InstanceSocket),
/// and its clients (`afterglow ctl`, `afterglow run`, aglow) look it up here,
/// so that all of them agree on the path.
namespace AfterglowProto {

/// Reads an environment variable: nullptr when it is not set. Tests give
/// their own.
using EnvironmentLookup = std::function<const char*(const char* name)>;
/// The process's environment.
const char* systemEnvironment(const char* name);

/// The Wayland display's name, safe in a file name: a leading "/" goes, and
/// each byte that is not a letter, a digit, "-", "_" or "." becomes "_".
/// "wayland-0" when it is empty.
std::string displayName(std::string_view waylandDisplay);

/// $XDG_RUNTIME_DIR/afterglow-<display>.sock: the socket of the main
/// afterglow on the display. Empty without XDG_RUNTIME_DIR.
std::string sharedSocketPath(const EnvironmentLookup& env = systemEnvironment);
/// $XDG_RUNTIME_DIR/afterglow-<display>-<pid>.sock: the socket of an
/// afterglow that runs on its own (--config, -o, --class). Empty without
/// XDG_RUNTIME_DIR.
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
/// too, and would act on a pane with the same number.
SocketLookup findSocket(const EnvironmentLookup& env = systemEnvironment);

} // namespace AfterglowProto
