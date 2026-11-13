// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// The command clients of session panes: `aglow run` and `afterglow run`,
// whose AFTERGLOW_SOCKET names the daemon. The daemon runs no scripts, so it
// passes each such connection to the window that shows the session of the
// client's pane. The window serves it on a connection to its own Afterglow,
// whose commands target answers as it answers a pane outside a session. The
// daemon reads whole request lines and passes them on, and passes the lines
// of the window back: replies, the output of runs and the end.

#include "Daemon.h"

#include <AfterglowPty/Pty.h>

#include <algorithm>

namespace AfterglowMux {

using AfterglowProto::ipcError;
using AfterglowProto::IpcRequest;
using AfterglowProto::IpcServerConnection;
namespace IpcErrorCode = AfterglowProto::IpcErrorCode;

std::string Daemon::startRelay(const IpcRequest& request, std::string line, pid_t caller, IpcServerConnection& connection)
{
    const auto failed = [&request](const std::string& message) { return ipcError(request.id, IpcErrorCode::CallFailed, message); };
    // The session of the pane whose terminal the client runs in: a pane's
    // program leads the terminal session of the client, or of the nearest
    // process above it (a shell in an editor's terminal has a session of
    // its own). By the processes, not by an AFTERGLOW_PANE_ID that a
    // process of another terminal can carry.
    Session* session = nullptr;
    std::uint32_t pane = 0;
    pid_t process = caller;
    for (int up = 0; !session && process > 1 && up < MaxAncestors; ++up) {
        const auto stat = AfterglowPty::processStat(process);
        if (!stat) break;
        for (const auto& s : m_sessions) {
            for (const auto& p : s->panes()) {
                if (p->leads(pid_t(stat->session))) {
                    session = s.get();
                    pane = p->id();
                }
            }
        }
        process = pid_t(stat->parent);
    }
    if (!session) return failed("only a client in a session pane reaches the mux daemon for the commands of scripts");
    Attachment* const window = shown(session->name());
    if (!window || !window->showing()) {
        return failed("no window shows session '" + session->name() + "', and only a window runs the commands of scripts");
    }
    // Each takes a connection in the window, and its first line waits in the
    // window's queue whatever its room.
    const auto clients = std::count_if(m_relays.begin(), m_relays.end(), [&](const auto& r) { return r.second.session == session->name(); });
    if (std::size_t(clients) >= MaxRelaysPerSession) {
        return failed("session '" + session->name() + "' has " + std::to_string(MaxRelaysPerSession) + " command clients already");
    }
    const std::uint64_t id = m_nextRelay++;
    line += '\n';
    const bool sent = guarded(*window, [&] {
        window->sendRelay(Attach::RelayFrame{id, Attach::RelayPart::Open, {}, pane, Attach::RelayKind::Commands, {}});
        window->sendRelay(Attach::RelayFrame{id, Attach::RelayPart::Bytes, std::move(line)});
        return true;
    });
    if (!sent) return failed("the window of session '" + session->name() + "' failed");
    m_relays[id] = Relay{&connection, window->serial(), session->name(), {}};
    m_relayOf[&connection] = id;
    return {};
}

void Daemon::readRelay(IpcServerConnection& connection)
{
    const auto of = m_relayOf.find(&connection);
    if (of == m_relayOf.end()) return;
    const std::uint64_t id = of->second;
    Attachment* const window = attachment(m_relays.at(id).window);
    // answerRelays() ends the relay of a window that went.
    if (!window || !window->showing()) return;
    std::string line;
    for (int lines = 0; lines < MaxLinesPerRead && window->relayRoom(); ++lines) {
        if (!connection.nextLine(line)) break;
        if (line.empty()) continue;
        line += '\n';
        const bool sent = guarded(*window, [&] {
            window->sendRelay(Attach::RelayFrame{id, Attach::RelayPart::Bytes, std::move(line)});
            return true;
        });
        if (!sent) return;
    }
    // The client ended its side: its runs end with the window's connection.
    if (!connection.wantsRead() && !connection.pending()) endRelay(id, true);
}

void Daemon::endRelay(std::uint64_t id, bool tellWindow)
{
    const auto relay = m_relays.find(id);
    if (relay == m_relays.end()) return;
    if (tellWindow) {
        if (Attachment* const window = attachment(relay->second.window); window && window->showing()) {
            guarded(*window, [&] {
                window->sendRelay(Attach::RelayFrame{id, Attach::RelayPart::End, {}});
                return true;
            });
        }
    }
    // What the window sent still goes to the client, and then the
    // connection closes.
    m_closing.insert(relay->second.connection);
    m_relayOf.erase(relay->second.connection);
    m_relays.erase(relay);
}

void Daemon::deliverRelayFrames(Attachment& window)
{
    for (Attach::RelayFrame& frame : window.takeRelayFrames()) {
        const auto relay = m_relays.find(frame.relay);
        // The client left before the window heard it: what came after is for nobody.
        if (relay == m_relays.end() || relay->second.window != window.serial()) continue;
        Relay& r = relay->second;
        if (frame.part == Attach::RelayPart::End) {
            // The Afterglow ended the connection: all that it sent goes.
            if (!r.partial.empty()) r.connection->send(std::exchange(r.partial, {}));
            endRelay(frame.relay, false);
            continue;
        }
        // Whole lines only: the window reads its connection in any parts,
        // and an error of the daemon must not join the half of a line.
        r.partial += frame.bytes;
        const std::size_t end = r.partial.rfind('\n');
        if (end == std::string::npos) continue;
        std::string rest = r.partial.substr(end + 1);
        r.partial.resize(end + 1);
        r.connection->send(std::exchange(r.partial, std::move(rest)));
    }
}

void Daemon::answerRelays()
{
    for (const auto& window : m_attachments) deliverRelayFrames(*window);
    // A window that went serves its connections no more: their clients hear
    // why, as an error that answers no call.
    std::vector<std::uint64_t> orphaned;
    for (const auto& [id, relay] : m_relays) {
        const Attachment* const window = attachment(relay.window);
        if (!window || !window->showing()) orphaned.push_back(id);
    }
    for (const std::uint64_t id : orphaned) {
        // The half of a line that did not go is dropped.
        const Relay& relay = m_relays.at(id);
        relay.connection->send(ipcError(std::nullopt, IpcErrorCode::CallFailed,
                                        "the window of session '" + relay.session + "' closed, and with it the runs of this client"));
        endRelay(id, false);
    }
}

bool Daemon::reads(const IpcServerConnection& connection) const
{
    if (m_closing.contains(&connection)) return false;
    const auto of = m_relayOf.find(&connection);
    if (of == m_relayOf.end()) return true;
    const Attachment* const window = attachment(m_relays.at(of->second).window);
    return window && window->showing() && window->relayRoom();
}

Attachment* Daemon::attachment(std::uint64_t serial) const
{
    for (const auto& window : m_attachments) {
        if (window->serial() == serial) return window.get();
    }
    return nullptr;
}

} // namespace AfterglowMux
