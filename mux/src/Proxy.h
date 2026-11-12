// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace AfterglowMux {

/// The argument of `afterglow-mux proxy`.
inline constexpr const char* ProxyArgument = "proxy";

/// `afterglow-mux proxy [--start]`: carries one connection to the daemon of
/// this host on standard input and output, for a client on another host
/// that runs it over ssh (MuxConnect.h, Route). With @p start, it starts
/// the daemon when none answers. Its exit status is ProxyNoDaemon when no
/// daemon runs and none starts, 1 with a message on standard error when it
/// cannot connect, and 0 once either end closed.
int runProxy(bool start);

} // namespace AfterglowMux
