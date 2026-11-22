// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxStream.h"

#include <QObject>
#include <QString>

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class QLocalSocket;

namespace PhosphorChannel {
class StreamWriter;
} // namespace PhosphorChannel

namespace Afterglow {

/// The command clients of a session that this window serves: `aglow run` in
/// a session pane, whose AFTERGLOW_SOCKET names the daemon, and on the
/// remote host of an `afterglow ssh` in a session pane, through the socket
/// that the daemon keeps for that ssh session. The daemon passes each
/// connection in Commands events (Attach::RelayFrame), and the window serves
/// it on a connection to its own Afterglow:
///
/// - a client in a pane of a session of this computer: the commands target
///   of the Afterglow, as for a pane outside a session;
/// - a client in a pane of a session of another host, or of an ssh session:
///   a socket of RemoteSockets::endpoint(), with only the commands that a
///   script marks remote, as for an ssh session of that host.
///
/// The Afterglow answers with its replies, the output of runs and their
/// acknowledgements, so these bytes go back as they come. A run names the
/// daemon's ID of its pane, which becomes the window's. On the GUI thread.
class MuxCommands final : public QObject
{
public:
    /// What the window gives the connections.
    struct Host
    {
        /// The socket of the Afterglow whose commands target serves the
        /// clients of a session of this computer, or empty with the reason
        /// in its argument.
        std::function<QString(QString* error)> socket;
        /// RemoteSockets::endpoint() of the Afterglow, for window pane
        /// @p pane: the socket for the other clients.
        std::function<QString(quint32 pane, const QString& host, bool commandsTarget, QString* error)> remote;
        /// The window's ID of the daemon's pane @p id, or nothing when no
        /// pane of the window shows it.
        std::function<std::optional<quint32>(std::uint32_t id)> pane;
        /// The window makes the daemon's pane @p id next: the connections of
        /// its clients wait until it did (bound()).
        std::function<bool(std::uint32_t id)> binding;
        /// The window's ID of its focused pane, when that pane is one of the
        /// session's: pane 0 of a run of a client of a session of this
        /// computer, which for the Afterglow would be its own focused pane,
        /// maybe of another window. For a session of another host, the pane
        /// of a client in no pane (a relay of pane 0), whose remote endpoint
        /// takes it as the client's own.
        std::function<std::optional<quint32>()> focused;
    };

    /// The bytes that wait for the commands channel, past which the
    /// connections are not read: what each run sends is bounded by the
    /// acknowledgements of its client, so this only paces them. Also the
    /// most bytes of the lines of a client that wait for its pane (binding).
    static constexpr std::size_t MaxWaitingBytes = std::size_t(1) << 20;
    /// The most bytes of the window's own answers to a client that wait for
    /// the Afterglow to end its line: past it, the relay ends.
    static constexpr std::size_t MaxHeldAnswers = std::size_t(64) << 10;

    /// @p writer is the window's Attach::CommandsChannel. @p written runs
    /// after a write, so that the stream sends it.
    MuxCommands(QString session, PhosphorChannel::StreamWriter* writer, std::function<void()> written,
                QObject* parent = nullptr);
    ~MuxCommands() override;

    /// The connections that opened before it serve once it comes, but those
    /// of a pane that the window makes next (Host::binding).
    void setHost(Host host);
    /// The window made the daemon's pane @p pane: the connections of its
    /// clients that waited for it serve now.
    void bound(std::uint32_t pane);
    /// The session runs on @p host: its programs get only the commands that
    /// a script marks remote, with @p host as ev.remote.host.
    void setRemoteHost(QString host)
    {
        m_remoteHost = std::move(host);
    }

    /// A frame of the daemon. False for one that does not apply: a relay
    /// that opens twice. The bytes of a relay that the window refused or
    /// ended go nowhere, and so does every frame after closeAll(): the
    /// daemon ends those relays with the stream.
    bool frame(AfterglowMux::Attach::RelayFrame frame);
    /// The commands channel has room again: what waits goes, and the
    /// connections are read.
    void writable();
    /// The stream ended: every connection closes, and their runs end.
    void closeAll();

    /// The line @p line of a client, without its '\n', for the window's
    /// Afterglow: a run names the window's ID of its pane, by @p pane. Pane
    /// 0 and the other lines of @p target stay. Nothing for a line that is
    /// not one JSON object that the window reads (the checks here would not
    /// see what the Afterglow reads in it), for a line of another target,
    /// whose calls act on every window by the Afterglow's own pane IDs, for
    /// a run whose pane is not a number, for a run of a pane that the window
    /// does not show, and for a run that the window's ID makes longer than
    /// the Afterglow takes: @p error gets the reply. With @p focused (a
    /// client of a session of this computer), pane 0 is the pane that it
    /// gives, and a run of pane 0 when it gives none is refused. Without
    /// it, pane 0 stays: an endpoint of RemoteSockets reads it as the
    /// client's own pane.
    static std::optional<std::string> forWindow(const std::string& line, std::string_view target,
                                                const std::function<std::optional<quint32>(std::uint32_t)>& pane,
                                                const QString& session, std::string* error,
                                                const std::function<std::optional<quint32>()>* focused = nullptr);

    /// The window's pane of a relay to an endpoint of RemoteSockets that
    /// names the daemon's pane @p pane (a client of a session of another
    /// host, or of an ssh session in a session pane): the endpoint takes it
    /// as the client's own. Pane 0, a client in no pane of the session, is
    /// the focused pane of the window. Nothing, with @p error, for a pane
    /// that the window does not show, or no focused pane of the session.
    static std::optional<quint32> relayPane(const Host& host, std::uint32_t pane, const QString& session,
                                            QString* error);

private:
    struct Relay
    {
        QLocalSocket* socket = nullptr;
        std::string target; ///< of the client's lines: AfterglowProto::Run::Target or Remote::Target
        std::string unsent; ///< lines that wait for the socket to connect
        /// The commands target of this process (a client of a session of
        /// this computer), not an endpoint of RemoteSockets.
        bool local = false;
        /// The bytes of the Afterglow that went to the daemon end inside a
        /// line: the window's own answers wait (held).
        bool midLine = false;
        std::vector<std::string> held;
        std::size_t heldBytes = 0;
    };
    /// A connection that waits for the window's host or for its pane.
    struct Early
    {
        AfterglowMux::Attach::RelayFrame open;
        std::vector<std::string> lines;
        std::size_t bytes = 0;
    };
    /// Connects the relay that @p open opens. A relay that cannot be served
    /// ends with the reason.
    void open(const AfterglowMux::Attach::RelayFrame& open);
    void bytes(std::uint64_t id, std::string bytes);
    /// Opens the connections of m_early that @p which takes, and passes
    /// their lines.
    void openEarly(const std::function<bool(const Early& early)>& which);
    /// Sends @p line, an answer of the window's own, to the client of
    /// relay @p id: between the lines of the Afterglow, never inside one.
    void answer(std::uint64_t id, std::string line);
    /// Sends @p bytes of the Afterglow to the client of @p relay, and the
    /// answers of the window that waited for the end of its line.
    void forward(std::uint64_t id, Relay& relay, std::string bytes);
    /// The client ended: the connection closes once what waits went.
    void endFromDaemon(std::uint64_t id);
    /// The window's side ended or failed: the daemon hears it, after
    /// @p reason as an error that answers no call when not empty.
    void endFromWindow(std::uint64_t id, const std::string& reason = {});
    /// Reads the connections while the channel has room.
    void readSockets();
    void send(AfterglowMux::Attach::RelayFrame frame);
    void flush();

    QString m_session;
    PhosphorChannel::StreamWriter* m_writer = nullptr;
    std::function<void()> m_written;
    Host m_host;
    QString m_remoteHost;
    std::map<std::uint64_t, Relay> m_relays;
    /// The connections that opened before setHost(), or before the window
    /// made their pane: their Open, and their lines.
    std::map<std::uint64_t, Early> m_early;
    std::deque<std::string> m_waiting; ///< encoded frames for the channel
    std::size_t m_waitingBytes = 0;
};

} // namespace Afterglow
