// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxConnect.h"

#include "MuxProtocol.h"

namespace AfterglowMux {

using AfterglowProto::IpcConnection;

Reached connectAny(const AfterglowProto::MuxSockets& sockets, std::string* error)
{
    error->clear();
    for (const AfterglowProto::MuxSocket& socket : sockets.candidates) {
        auto connection = std::make_unique<IpcConnection>();
        IpcConnection::Error connectError;
        if (!connection->connect(socket.path, &connectError, ConnectTimeoutMs)) {
            if (!connectError.noServer && error->empty()) *error = connectError.message;
            continue;
        }
        IpcConnection::Error callError;
        const auto version = connection->call(AfterglowMux::Target, AfterglowMux::Fn::Version, {}, CallTimeoutMs, &callError);
        if (!version) {
            // A daemon that closed the connection without an answer was
            // ending: as no daemon there.
            if (callError.code.empty() && !callError.timedOut) continue;
            *error = callError.message;
            return {};
        }
        const auto protocol = (*version)["protocol"].toInteger();
        if (protocol != AfterglowMux::ProtocolVersion) {
            *error = "afterglow-mux speaks protocol " + (protocol ? std::to_string(*protocol) : std::string("?"))
                + " and this afterglow speaks " + std::to_string(AfterglowMux::ProtocolVersion)
                + ": end its sessions with the afterglow that started it, or wait until they end";
            return {};
        }
        return {std::move(connection), socket.path};
    }
    return {};
}

} // namespace AfterglowMux
