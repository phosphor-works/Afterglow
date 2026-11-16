// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace AfterglowMux {

/// `aglow-mux proxy [--start]` (MuxConnect.h: ProxyArgument,
/// ProxyStartOption): carries one connection to the daemon of
/// this host on standard input and output, for a client on another host
/// that runs it over ssh (MuxConnect.h, Route). With @p start, it starts
/// the daemon when none answers. Its exit status is ProxyNoDaemon when no
/// daemon runs and none starts, 1 with a message on standard error when it
/// cannot connect or cannot wait for either end, and 0 once the connection
/// ended: both ends closed, or the client went.
int runProxy(bool start);

} // namespace AfterglowMux
