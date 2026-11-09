// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxProtocol.h"

#include <AfterglowProto/Socket.h>

#include <QObject>
#include <QPointer>
#include <QString>

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class QSocketNotifier;

namespace PhosphorChannel {
class Stream;
class StreamWriter;
} // namespace PhosphorChannel

namespace Afterglow {

class Pane;

/// A window's link to a session of the mux daemon: the stream that the attach
/// call opens (MuxStream.h). The state of each pane of the session comes in
/// frames, which the Pane applies to its replica terminal; the pane's input
/// goes back. It runs on the GUI thread, on socket notifiers.
class MuxClient : public QObject
{
    Q_OBJECT
public:
    /// The most input that waits while the channel is full, in bytes: as much
    /// as a pane's own PTY queues. Input past it is refused, as a PTY refuses it.
    static constexpr std::size_t MaxWaitingInput = 32u * 1024 * 1024;

    /// Attaches to session @p name: when @p takeover, also when another window
    /// shows it, which then detaches. Waits until the daemon names the
    /// session's panes. Nullptr with @p error on a failure. @p sockets are
    /// the daemon's: the user's, or a test's.
    static std::shared_ptr<MuxClient> attach(const QString& name, bool takeover, QString* error,
                                             const AfterglowProto::MuxSockets& sockets = AfterglowProto::muxSockets());
    ~MuxClient() override;

    const QString& session() const { return m_session; }
    /// The panes of the session, as the daemon named them.
    const std::vector<std::uint32_t>& panes() const { return m_paneIds; }

    /// @p pane shows session pane @p id from now on: its frames go to it.
    void bind(std::uint32_t id, Pane* pane);
    /// The pane that showed @p id goes. The last one ends the link: with
    /// @p kill, it also ends the session (Mux.closeOnLastWindow).
    void release(std::uint32_t id, bool kill);

    /// False when the input that waits is past MaxWaitingInput.
    bool sendInput(std::uint32_t id, std::string_view bytes);
    void sendResize(std::uint32_t id, const AfterglowMux::PaneSize& size);
    /// The program in the foreground of the session, for the question before
    /// a close. Empty when none runs, or when the daemon does not answer.
    QString foregroundJob() const;

private:
    MuxClient() = default;
    void readable();
    void writable();
    void handleFrames();
    void sendWaitingInput();
    void updateNotifiers();
    void lost();
    std::vector<QPointer<Pane>> shownPanes() const;

    QString m_session;
    AfterglowProto::MuxSockets m_sockets; ///< the daemon's, for kill and the job
    std::unique_ptr<PhosphorChannel::Stream> m_stream;
    PhosphorChannel::StreamWriter* m_input = nullptr;
    QSocketNotifier* m_readNotifier = nullptr;
    QSocketNotifier* m_writeNotifier = nullptr;
    std::vector<std::uint32_t> m_paneIds;
    std::map<std::uint32_t, QPointer<Pane>> m_panes;
    std::deque<std::string> m_waitingInput;
    std::size_t m_waitingBytes = 0;
    bool m_ended = false; ///< the session ended, or another window took it
};

} // namespace Afterglow
