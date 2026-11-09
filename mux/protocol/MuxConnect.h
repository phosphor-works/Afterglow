// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/Socket.h>

#include <memory>
#include <string>

// How a client reaches the mux daemon: `afterglow mux` and a window that
// attaches to a session use the same lookup.

namespace AfterglowMux {

/// How long a client waits to connect to a socket, and for the answer to a call.
inline constexpr int ConnectTimeoutMs = 1000;
inline constexpr int CallTimeoutMs = 5000;

struct Reached
{
    std::unique_ptr<AfterglowProto::IpcConnection> connection;
    std::string socket;
};

/// The first candidate whose daemon answers the version call, in the order
/// of muxSockets(). Without one, no connection: @p error is empty when no
/// daemon listens on any, and says why when one cannot be used (another
/// protocol, no answer, a socket of another user).
Reached connectAny(const AfterglowProto::MuxSockets& sockets, std::string* error);

} // namespace AfterglowMux
