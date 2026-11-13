// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Attach.h"
#include "ImageDecoder.h"
#include "Session.h"

#include <AfterglowProto/IpcServer.h>
#include <AfterglowProto/Socket.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace AfterglowMux {

/// The headless process that owns the sessions. One thread runs the sockets
/// of the daemon, the windows that show sessions and the lifetimes of
/// sessions and panes. Each pane's PTY has its reader thread, which wakes
/// this one through an eventfd after output, when a call of its terminal
/// waits for the window, when the PTY's input queue has room again, and when
/// its program exits (MuxPane).
class Daemon
{
public:
    /// The lines of one connection that one pass of the loop answers or
    /// passes on, empty ones included: a client that pipelines calls cannot
    /// keep the others, the signals and the reaping of programs waiting.
    static constexpr int MaxLinesPerRead = 64;
    /// The command clients that the window of one session serves at a time.
    static constexpr std::size_t MaxRelaysPerSession = 64;
    /// The processes above a command client that the daemon looks at for
    /// the pane that it runs in.
    static constexpr int MaxAncestors = 64;
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
    /// Serves until it is idle (no session, no connection and no window) after its
    /// first session, or after StartWaitMs without one, or until SIGTERM,
    /// SIGINT or SIGHUP. A signal that the daemon got as ignored (nohup)
    /// stays ignored.
    Exit run();

private:
    void accept();
    void read(AfterglowProto::IpcServerConnection& connection);
    /// @p caller is the process of the client, 0 when unknown. Empty when
    /// the answer comes later: a window gives it (answerForwarded()).
    std::string answer(const AfterglowProto::IpcRequest& request, pid_t caller, AfterglowProto::IpcServerConnection& connection);

    // The command clients of session panes (`aglow run`), which the window
    // of the session serves, in DaemonCommands.cpp.
    /// Passes @p connection, whose first request is @p request, read from
    /// @p line, to the window that shows the session of the caller's pane.
    /// Empty: the window answers. Else the error.
    std::string startRelay(const AfterglowProto::IpcRequest& request, std::string line, pid_t caller,
                           AfterglowProto::IpcServerConnection& connection);
    /// Passes the lines of a relayed @p connection to its window, while the
    /// window has room for them.
    void readRelay(AfterglowProto::IpcServerConnection& connection);
    /// The relay @p id ends: its connection closes once its output went.
    /// @p tellWindow: the client ended it, and the window closes its side.
    void endRelay(std::uint64_t id, bool tellWindow);
    /// Sends the clients the lines that @p window sent them, and ends the
    /// relays that it ended.
    void deliverRelayFrames(Attachment& window);
    /// deliverRelayFrames() for each window, and ends the relays whose
    /// window went.
    void answerRelays();
    /// Whether the daemon reads the lines of @p connection now: not those of
    /// a relayed one whose window has no room, nor of one that closes after
    /// its output.
    bool reads(const AfterglowProto::IpcServerConnection& connection) const;
    /// The attachment by its serial(), or nullptr.
    Attachment* attachment(std::uint64_t serial) const;
    /// @p caller gives the environment of a session with callerEnvironment.
    std::string newSession(const AfterglowProto::IpcRequest& request, pid_t caller);
    std::string killSession(const AfterglowProto::IpcRequest& request);
    /// The connection of the call becomes the window's stream once the
    /// answer went (m_attaching).
    std::string attach(const AfterglowProto::IpcRequest& request);
    std::string detach(const AfterglowProto::IpcRequest& request);
    std::string paneDirectory(const AfterglowProto::IpcRequest& request);
    std::string newPane(const AfterglowProto::IpcRequest& request);
    std::string paneJob(const AfterglowProto::IpcRequest& request, pid_t caller);
    std::string breakOut(const AfterglowProto::IpcRequest& request);
    std::string paneSession(const AfterglowProto::IpcRequest& request);
    /// Moves the panes of @p layout out of @p from to a new session that
    /// keeps that layout: its name, or nothing with @p error.
    std::optional<std::string> breakOutPanes(Session& from, Layout layout, std::string* error);

    // The control target (`afterglow ctl` for a session), in DaemonControl.cpp.
    /// selectSession and selectPane: the session that the control calls of
    /// @p connection act on.
    std::string select(const AfterglowProto::IpcRequest& request, AfterglowProto::IpcServerConnection& connection);
    /// A call of the control target, for the session that @p connection
    /// selected. Empty when a window answers it later.
    std::string control(const AfterglowProto::IpcRequest& request, AfterglowProto::IpcServerConnection& connection, pid_t caller);
    /// Passes @p request to the window that shows @p session, with @p args in
    /// place of its own. Empty: its answer comes later. Without a window, the
    /// error that says so.
    std::string forward(const AfterglowProto::IpcRequest& request, AfterglowProto::IpcServerConnection& connection, Session& session,
                        const AfterglowProto::Json::Array& args);
    /// Tells the window that shows @p session of a change that the daemon
    /// made (a pane to place, a focus): no answer comes.
    void tellWindow(const Session& session, const std::string& fn, AfterglowProto::Json::Array args);
    /// Sends the answers that windows gave to the calls passed to them, and
    /// fails those whose window went.
    void answerForwarded();
    /// The connection closed: its selection and the calls that wait for a
    /// window go.
    void forget(const AfterglowProto::IpcServerConnection* connection);
    /// What a pane runs when it has news for the daemon's thread: it wakes
    /// the poll. Safe to call from any thread.
    std::function<void()> waker() const;
    /// The error of a call whose first argument is not this daemon's
    /// protocol version.
    static std::optional<std::string> versionError(const AfterglowProto::IpcRequest& request);
    /// Ends the programs of @p session and the session: its window hears it.
    void endSession(std::vector<std::unique_ptr<Session>>::iterator session);
    /// Sends what changed in each attached session.
    void pumpAttachments();
    /// Tells each pane which window shows it, for its host calls.
    void syncAttached();
    /// The attachment that shows the session @p name, connected or not;
    /// not one that finishes.
    Attachment* shown(const std::string& name) const;
    /// Runs @p work for @p attachment. An exception finishes the attachment,
    /// whose host calls go back to the panes of its session, and returns
    /// false.
    bool guarded(Attachment& attachment, const std::function<bool()>& work);
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
    /// Before the sessions: their panes decode with it until they end.
    ImageDecoder m_images{"/proc/self/exe"};
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
    /// The attachment of the attach call that read() answers: it takes the
    /// call's connection after the answer.
    std::unique_ptr<Attachment> m_attaching;
    std::uint32_t m_nextPane = 1;
    /// The session that the control calls of each connection act on.
    std::map<const AfterglowProto::IpcServerConnection*, std::string> m_selected;
    /// The control calls that wait for the answer of a window, by the ID of
    /// their Control event.
    struct Forward
    {
        AfterglowProto::IpcServerConnection* connection = nullptr;
        std::optional<std::int64_t> request;
        std::uint64_t window = 0; ///< Attachment::serial()
        std::string session;
    };
    std::map<std::uint64_t, Forward> m_forwards;
    std::uint64_t m_nextForward = 1;
    /// The command connections that windows serve, by the ID of their relay.
    struct Relay
    {
        AfterglowProto::IpcServerConnection* connection = nullptr;
        std::uint64_t window = 0; ///< Attachment::serial()
        std::string session;
        std::string partial; ///< what the window sent after the last whole line
    };
    std::map<std::uint64_t, Relay> m_relays;
    std::map<const AfterglowProto::IpcServerConnection*, std::uint64_t> m_relayOf;
    /// Connections that close once their output went: relays that ended.
    std::set<const AfterglowProto::IpcServerConnection*> m_closing;
    std::uint64_t m_nextRelay = 1;
    /// The number of the next session without a name of its client.
    std::uint64_t m_nextName = 1;
    /// A session was created. Before that, the daemon waits for one for
    /// StartWaitMs; a connection alone does not end the wait (a client may
    /// probe the socket before it calls).
    bool m_hadSession = false;
};

} // namespace AfterglowMux
