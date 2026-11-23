// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// The command clients of session panes: `aglow run` and `afterglow run`,
// whose AFTERGLOW_SOCKET names the daemon, and `afterglow mcp`, which selected
// its agent session. The daemon runs no scripts, so it passes each such
// connection to the window that shows the session of the client's pane, or
// of the agent session. The window serves it on a connection to its own Afterglow,
// whose commands target answers as it answers a pane outside a session. The
// daemon reads whole request lines and passes them on, and passes the lines
// of the window back: replies, the output of runs and the end.

#include "Daemon.h"

#include <AfterglowProto/Ipc.h>

#include <algorithm>
#include <exception>
#include <utility>

namespace AfterglowMux {

using AfterglowProto::ipcError;
using AfterglowProto::IpcRequest;
using AfterglowProto::IpcServerConnection;
namespace IpcErrorCode = AfterglowProto::IpcErrorCode;

std::string Daemon::startRelay(const IpcRequest& request, std::string line, pid_t caller,
                               IpcServerConnection& connection)
{
    std::uint32_t pane = 0;
    // An agent's MCP server selected its agent session: its runs name their
    // panes, and it reaches no other session. Its process can run in a pane
    // of another session (an agent in the user's session pane), so the
    // selection comes first, and a connection that selected a session never
    // falls back to the session of its pane: that would be the user's.
    Session* session = nullptr;
    if (const auto selected = m_selected.find(&connection); selected != m_selected.end()) {
        session = sessionBySerial(selected->second.serial);
        if (!session)
            return ipcError(request.id, Error::NoSuchSession, "session '" + selected->second.name + "' ended");
        if (!session->options().agent) {
            return failed(request, "session '" + session->name()
                                       + "' is no agent session: a connection that selected a session reaches the "
                                         "commands of scripts only in an agent session");
        }
        if (!session->options().control.enabled)
            return failed(request, "session '" + session->name() + "' takes no control calls (Control.enabled)");
    } else {
        session = sessionOfProcess(caller, &pane);
    }
    if (!session)
        return failed(request, "only a client in a session pane reaches the mux daemon for the commands of scripts");
    Attachment* const window = showing(session->name());
    if (!window)
        return ipcError(request.id, Error::NotAttached,
                        "no window shows session '" + session->name()
                            + "', and only a window runs the commands of scripts");
    // Each takes a connection in the window, and its first line waits in the
    // window's queue whatever its room. Only this window's: those of a
    // window that it took the session from end with that window. The
    // clients of ssh sessions have their own limit.
    const auto clients = std::count_if(m_relays.begin(), m_relays.end(), [&](const auto& r) {
        return r.second.window == window->serial() && r.second.sshToken.empty();
    });
    if (std::size_t(clients) >= MaxRelaysPerSession) {
        return failed(request,
                      "session '" + session->name() + "' has " + std::to_string(MaxRelaysPerSession)
                          + " command clients already");
    }
    const std::uint64_t id = m_nextRelay++;
    line += '\n';
    // The relay before the window hears of it: a failure (out of memory)
    // leaves no relay that the end of its connection would not find
    // (forget()), and no client that the window serves and the daemon does
    // not know.
    m_relays[id] = Relay{&connection, window->serial(), session->name(), {}, {}};
    try {
        m_relayOf[&connection] = id;
    } catch (const std::exception&) {
        m_relays.erase(id);
        throw;
    }
    const bool sent = guarded(*window, [&] {
        window->sendRelay(Attach::RelayFrame{id, Attach::RelayPart::Open, {}, pane, Attach::RelayKind::Commands, {}});
        window->sendRelay(Attach::RelayFrame{id, Attach::RelayPart::Bytes, std::move(line)});
        return true;
    });
    if (!sent) {
        m_relayOf.erase(&connection);
        m_relays.erase(id);
        return failed(request, "the window of session '" + session->name() + "' failed");
    }
    return {};
}

void Daemon::readRelay(IpcServerConnection& connection)
{
    const auto of = m_relayOf.find(&connection);
    if (of == m_relayOf.end())
        return;
    const std::uint64_t id = of->second;
    Attachment* const window = attachment(m_relays.at(id).window);
    // answerRelays() ends the relay of a window that went.
    if (!window || !window->showing())
        return;
    std::string line;
    for (int lines = 0; lines < MaxLinesPerRead && window->relayRoom(); ++lines) {
        if (!connection.nextLine(line))
            break;
        if (line.empty())
            continue;
        line += '\n';
        const bool sent = guarded(*window, [&] {
            window->sendRelay(Attach::RelayFrame{id, Attach::RelayPart::Bytes, std::move(line)});
            return true;
        });
        if (!sent)
            return;
    }
    // The client ended its side: its runs end with the window's connection.
    if (!connection.wantsRead() && !connection.pending())
        endRelay(id, true);
}

void Daemon::endRelay(std::uint64_t id, bool tellWindow)
{
    const auto relay = m_relays.find(id);
    if (relay == m_relays.end())
        return;
    IpcServerConnection* const connection = relay->second.connection;
    Attachment* const window = attachment(relay->second.window);
    // An ssh session's socket that refused clients takes them again.
    if (const auto ssh = m_sshSessions.find(relay->second.sshToken); ssh != m_sshSessions.end())
        ssh->second->full = false;
    // The relay goes first, which needs no allocation: a failure below (out
    // of memory) leaves no relay half ended.
    m_relayOf.erase(connection);
    m_relays.erase(relay);
    if (tellWindow && window && window->showing()) {
        guarded(*window, [&] {
            window->sendRelay(Attach::RelayFrame{id, Attach::RelayPart::End, {}});
            return true;
        });
    }
    // What the window sent still goes to the client, and then the
    // connection closes. Out of memory, it closes now: the end of a relay
    // is how a failed one is dropped, and must not fail.
    try {
        m_closing.insert(connection);
    } catch (const std::exception&) {
        connection->close();
    }
}

void Daemon::deliverRelayFrames(Attachment& window)
{
    for (Attach::RelayFrame& frame : window.takeRelayFrames()) {
        const auto relay = m_relays.find(frame.relay);
        // The client left before the window heard it: what came after is for nobody.
        if (relay == m_relays.end() || relay->second.window != window.serial())
            continue;
        const bool delivered = contained("a command client", [&] {
            Relay& r = relay->second;
            if (frame.part == Attach::RelayPart::End) {
                // The Afterglow ended the connection: all that it sent goes.
                if (!r.partial.empty())
                    sendTo(*r.connection, std::exchange(r.partial, {}));
                endRelay(frame.relay, false);
                return;
            }
            // Whole lines only: the window reads its connection in any parts,
            // and an error of the daemon must not join the half of a line.
            r.partial += frame.bytes;
            const std::size_t end = r.partial.rfind('\n');
            if (end == std::string::npos) {
                // No client reads a longer line: the relay ends, rather than
                // hold a line that grows without a bound.
                if (r.partial.size() > AfterglowProto::IpcConnection::MaxFrameBytes) {
                    sendTo(
                        *r.connection,
                        ipcError(std::nullopt, IpcErrorCode::CallFailed,
                                 "the window of session '" + r.session + "' sent a line longer than a client reads"));
                    endRelay(frame.relay, true);
                }
                return;
            }
            std::string rest = r.partial.substr(end + 1);
            r.partial.resize(end + 1);
            sendTo(*r.connection, std::exchange(r.partial, std::move(rest)));
        });
        // A frame that failed (out of memory) ends its relay, whose lines it
        // broke, and not the other relays of the window. The window ended
        // its side already when the frame was its end.
        if (!delivered)
            endRelay(frame.relay, frame.part != Attach::RelayPart::End);
    }
}

void Daemon::answerRelays()
{
    for (const auto& window : m_attachments)
        deliverRelayFrames(*window);
    // A window that went serves its connections no more: their clients hear
    // why, as an error that answers no call. One that finishes still sends
    // what it sent them before it heard, the end of a run, until it goes
    // (FinishWaitMs at most): the daemon passes it no more of theirs.
    for (auto it = m_relays.begin(); it != m_relays.end();) {
        const std::uint64_t id = it->first;
        const Relay& relay = it->second;
        // endRelay() erases only this one.
        ++it;
        const Attachment* const window = attachment(relay.window);
        if (window && !window->failed())
            continue;
        // The half of a line that did not go is dropped. Out of memory, the
        // client gets no error, and its relay ends all the same.
        contained("a command client", [&] {
            sendTo(
                *relay.connection,
                ipcError(std::nullopt, IpcErrorCode::CallFailed,
                         "the window of session '" + relay.session + "' closed, and with it the runs of this client"));
        });
        endRelay(id, false);
    }
}

bool Daemon::reads(const IpcServerConnection& connection) const
{
    if (m_closing.contains(&connection))
        return false;
    const auto of = m_relayOf.find(&connection);
    if (of == m_relayOf.end())
        return true;
    const Attachment* const window = attachment(m_relays.at(of->second).window);
    return window && window->showing() && window->relayRoom();
}

Attachment* Daemon::attachment(std::uint64_t serial) const
{
    for (const auto& window : m_attachments) {
        if (window->serial() == serial)
            return window.get();
    }
    return nullptr;
}

} // namespace AfterglowMux
