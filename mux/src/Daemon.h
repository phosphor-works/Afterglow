// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Attach.h"
#include "Session.h"

#include <AfterglowProto/IpcServer.h>
#include <AfterglowProto/Socket.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <memory>
#include <string>
#include <vector>

namespace AfterglowMux {

/// The headless process that owns the sessions. One thread runs the sockets
/// of the daemon, the windows that show sessions and the lifetimes of
/// sessions and panes. Each pane's PTY has its reader thread, which wakes
/// this one through an eventfd after output, a resize, room in the PTY's
/// input queue, and when its program exits.
class Daemon
{
public:
    /// How run() ended.
    struct Exit
    {
        int status = 0;
        /// The signal that ended the daemon, else 0. The process ends with
        /// it once the daemon is gone, so that its parent sees the signal.
        int signal = 0;
    };

    /// A daemon that no client reaches in this time ends, so a client that
    /// died before it connected leaves no process behind.
    static constexpr int StartWaitMs = 10000;

    /// @p fileLimit is the soft RLIMIT_NOFILE that the programs of sessions
    /// get (see Session), -1 for the daemon's own.
    explicit Daemon(long fileLimit = -1);
    ~Daemon();
    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;

    /// What listen() did.
    enum class Listen : std::uint8_t {
        Listening,
        Held, ///< another daemon holds one of the sockets: its sessions are there
        Failed, ///< no socket is left, or the event descriptors could not be made
    };
    /// Listens on each of @p sockets, after it creates the socket's
    /// directory (mode 0700). A socket that fails for another reason than a
    /// second daemon (its directory, bind()) is left out, with a message on
    /// stderr. Unless it listens, @p error says why: with no socket left, it
    /// names the reason of each.
    Listen listen(const std::vector<AfterglowProto::MuxSocket>& sockets, std::string* error);
    /// Serves until it is idle (no session and no connection) after its
    /// first session, or after StartWaitMs without one, or until SIGTERM,
    /// SIGINT or SIGHUP. A signal that the daemon got as ignored (nohup)
    /// stays ignored.
    Exit run();

private:
    void accept();
    void read(AfterglowProto::IpcServerConnection& connection);
    /// @p caller is the process of the client, 0 when unknown.
    std::string answer(const AfterglowProto::IpcRequest& request, pid_t caller);
    std::string newSession(const AfterglowProto::IpcRequest& request);
    std::string killSession(const AfterglowProto::IpcRequest& request);
    std::string attach(const AfterglowProto::IpcRequest& request, pid_t caller);
    std::string detach(const AfterglowProto::IpcRequest& request);
    /// The error of a call whose first argument is not this daemon's
    /// protocol version.
    static std::optional<std::string> versionError(const AfterglowProto::IpcRequest& request);
    /// Ends the programs of @p session and the session: its window hears it.
    void endSession(std::vector<std::unique_ptr<Session>>::iterator session);
    /// Sends what changed in each attached session.
    void pumpAttachments();
    /// The attachment that shows the session @p name, connected or not;
    /// not one that finishes.
    Attachment* shown(const std::string& name) const;
    /// Runs @p work for @p attachment. An exception finishes the attachment
    /// and returns false.
    static bool guarded(Attachment& attachment, const std::function<bool()>& work);
    std::string listSessions(const AfterglowProto::IpcRequest& request, pid_t caller);
    /// The socket that session panes name in AFTERGLOW_SOCKET.
    const std::string& paneSocket() const;
    std::vector<std::unique_ptr<Session>>::iterator find(const std::string& name);
    std::string freeName();
    void reap();
    /// No session, no connection and no window: nothing can need the daemon
    /// now.
    bool idle() const;

    const long m_fileLimit;
    std::vector<std::unique_ptr<AfterglowProto::IpcListener>> m_listeners;
    int m_wakeFd = -1;
    int m_signalFd = -1;
    std::string m_setupError; ///< why the event descriptors are missing
    std::vector<std::unique_ptr<AfterglowProto::IpcServerConnection>> m_connections;
    std::vector<std::unique_ptr<Session>> m_sessions;
    /// The windows of sessions: at most one that is not finishing for each
    /// session, and those that finish while they send what waits. An
    /// attachment holds no session: it may outlive its session.
    std::vector<std::unique_ptr<Attachment>> m_attachments;
    std::uint32_t m_nextPane = 1;
    /// The number of the next session without a name of its client.
    std::uint64_t m_nextName = 1;
    /// A session was created. Before that, the daemon waits for one for
    /// StartWaitMs; a connection alone does not end the wait (a client may
    /// probe the socket before it calls).
    bool m_hadSession = false;
};

} // namespace AfterglowMux
