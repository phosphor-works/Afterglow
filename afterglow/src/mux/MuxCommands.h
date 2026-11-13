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
#include <vector>

class QLocalSocket;

namespace PhosphorChannel {
class StreamWriter;
} // namespace PhosphorChannel

namespace Afterglow {

/// The command clients of a session's panes that this window serves (`aglow
/// run`, whose AFTERGLOW_SOCKET names the daemon): the daemon passes each
/// connection in Commands events (Attach::RelayFrame), and the window serves
/// it on a connection to its own Afterglow. The commands target there
/// answers as it answers a pane outside a session, with its replies, the
/// output of runs and their acknowledgements, so these bytes go back as they
/// come. A run names the daemon's ID of its pane, which becomes the
/// window's. On the GUI thread.
class MuxCommands final : public QObject
{
public:
    /// What the window gives the connections.
    struct Host
    {
        /// The socket of the Afterglow whose commands target serves them,
        /// or empty with the reason in its argument.
        std::function<QString(QString* error)> socket;
        /// The window's ID of the daemon's pane @p id, or nothing when no
        /// pane of the window shows it.
        std::function<std::optional<quint32>(std::uint32_t id)> pane;
    };

    /// The bytes that wait for the commands channel, past which the
    /// connections are not read: what each run sends is bounded by the
    /// acknowledgements of its client, so this only paces them.
    static constexpr std::size_t MaxWaitingBytes = std::size_t(1) << 20;

    /// @p writer is the window's Attach::CommandsChannel. @p written runs
    /// after a write, so that the stream sends it.
    MuxCommands(QString session, PhosphorChannel::StreamWriter* writer, std::function<void()> written, QObject* parent = nullptr);
    ~MuxCommands() override;

    /// The connections that opened before it serve once it comes.
    void setHost(Host host);
    /// Every connection gets @p reason as its error, and none is served: for
    /// a session of another host, whose programs reach no command of this
    /// computer. Empty: they are served.
    void setRefusal(QString reason) { m_refusal = std::move(reason); }

    /// A frame of the daemon. False for one that does not apply: a relay
    /// that opens twice, or bytes for one that never opened.
    bool frame(AfterglowMux::Attach::RelayFrame frame);
    /// The commands channel has room again: what waits goes, and the
    /// connections are read.
    void writable();
    /// The stream ended: every connection closes, and their runs end.
    void closeAll();
    std::size_t connections() const { return m_relays.size(); }

    /// The line @p line of a client, without its '\n', for the window's
    /// Afterglow: a run names the window's ID of its pane, by @p pane. Pane
    /// 0, the focused pane, and the other lines of the commands target stay.
    /// Nothing for a line of another target, whose calls act on every
    /// window by the Afterglow's own pane IDs, for a run of a pane that the
    /// window does not show, and for a run that the window's ID makes
    /// longer than the Afterglow takes: @p error gets the reply.
    static std::optional<std::string> forWindow(const std::string& line, const std::function<std::optional<quint32>(std::uint32_t)>& pane,
                                                const QString& session, std::string* error);

private:
    struct Relay
    {
        QLocalSocket* socket = nullptr;
        std::string unsent; ///< lines that wait for the socket to connect
    };
    void open(std::uint64_t id);
    void bytes(std::uint64_t id, std::string bytes);
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
    QString m_refusal;
    std::map<std::uint64_t, Relay> m_relays;
    /// The connections that opened before setHost(), with their lines.
    std::map<std::uint64_t, std::vector<std::string>> m_early;
    std::deque<std::string> m_waiting; ///< encoded frames for the channel
    std::size_t m_waitingBytes = 0;
};

} // namespace Afterglow
