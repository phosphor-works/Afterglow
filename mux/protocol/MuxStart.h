// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxConnect.h"

#include <string>
#include <vector>

// How a client starts the mux daemon when none answers: `afterglow mux new`,
// and `aglow-mux proxy --start` on another host. No Qt: the proxy runs
// where only the Qt-free build is.

namespace AfterglowMux {

/// A connection to the daemon of @p sockets, which this starts from
/// @p daemon (the path of aglow-mux) when none answers: in its own
/// systemd user unit when the user manager outlives the logins, else in a
/// session of its own. Waits up to 5 seconds for it to answer. Nothing, with
/// @p error, when it does not. SIGCHLD must have its default action: the
/// start waits for its children.
Reached connectOrStart(const AfterglowProto::MuxSockets& sockets, const std::string& daemon, std::string* error);

/// An environment, as NAME=VALUE entries.
using Environment = std::vector<std::string>;
/// This process's environment.
Environment currentEnvironment();
/// connectOrStart() with @p environment in place of this process's, which
/// the start reads and passes on: for a thread other than the one that
/// changes the environment.
Reached connectOrStart(const AfterglowProto::MuxSockets& sockets, const std::string& daemon, const Environment& environment,
                       std::string* error);

} // namespace AfterglowMux
