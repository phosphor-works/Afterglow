// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowProto/Remote.h>

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace Afterglow {

class Control;
class ScriptHost;
class SettingsStore;
struct CommandBudget;
struct Settings;

/// Why @p settings allow no remote commands, as the refusal of a call says
/// it: "remote commands are off (SETTING)", for the first of
/// Terminal.sshRemoteCommands, Control.enabled and Scripts.enabled that is
/// off. Empty when they are all on. Terminal.sshIntegration does not count:
/// `afterglow ssh` runs without the shell's ssh function too.
QString remoteCommandsOff(const Settings& settings);

/// The sockets that `afterglow ssh` forwards to remote hosts, one for each
/// ssh session, while remoteCommandsOff() is empty.
///
/// A session's socket is $XDG_RUNTIME_DIR/afterglow-remote-<token>.sock,
/// with the "remote" target only (CommandSocket for a remote session), and
/// the lock file of InstanceSocket next to it. The session also keeps the
/// answer of its pane to `aglow run`'s query (OSC 24569): the socket's path
/// on the remote host once `ssh -O forward` added it (forwarded()), or the
/// reason why there is none. It ends when the process that opened it exits
/// (watched with a pidfd), when its pane closes, or when the settings turn
/// it off. Then a forward that was added is cancelled with `ssh -O cancel`.
class RemoteSockets : public QObject
{
    Q_OBJECT

public:
    /// Sets what @p pane answers to the query. False when there is no such pane.
    using PaneAnswer = std::function<bool(quint32 pane, const AfterglowProto::Remote::Answer& answer)>;

    /// Sessions of one pane at most: its ssh sessions in the background and
    /// the one in front. A client in a loop opens no more.
    static constexpr int MaxSessionsPerPane = 16;
    /// Sessions of the process at most, each a socket and a pidfd.
    static constexpr int MaxSessions = 256;
    /// UTF-16 units of the host name of a session (ev.remote.host) at most.
    static constexpr qsizetype MaxHostLength = 255;

    /// The runs of every session share @p budget, the one of the process's
    /// own CommandSocket. Without one, the sessions share one of their own.
    RemoteSockets(Control& control, ScriptHost& scripts, SettingsStore& settings, PaneAnswer answer,
                  std::shared_ptr<CommandBudget> budget = {}, QObject* parent = nullptr);
    /// Closes every session, and cancels their forwards.
    ~RemoteSockets() override;

    /// What a session needs from the ssh session that opens it.
    struct Request
    {
        quint32 pane = 0;
        qint64 pid = 0; ///< the process of `afterglow ssh`: the session ends with it
        QString host; ///< ev.remote.host
        QString directory; ///< the remote directory of the sockets
        int protocol = 0; ///< of the remote aglow (`aglow --version`)
        /// ssh's options for `ssh -O cancel` on the shared connection: from
        /// `afterglow ssh`, "-F none -S PATH". Never the program: it is
        /// always "ssh".
        QStringList ssh;
        QString destination;
    };
    /// What open() made.
    struct Opened
    {
        QString token;
        QString socket; ///< the local socket, the target of `ssh -O forward -R`; empty without one
        QString path; ///< the socket's path on the remote host
        /// The reason why there is no socket (version or length), else empty.
        QString error;
    };

    /// Whether open() can make a socket: the settings allow it.
    bool enabled() const;
    /// Opens a session for @p request. Returns an error, or empty. A session
    /// with no socket (Opened::error) sets the pane's answer now. One with a
    /// socket waits for forwarded(): until then the remote path may not
    /// exist.
    QString open(const Request& request, Opened& out);
    /// `ssh -O forward` added the forward of @p token's socket: the pane
    /// answers with its path. An error, or empty. The client cancels the
    /// forward on an error: the session ended before the forward came, and
    /// its cancel found nothing to cancel.
    QString forwarded(const QString& token);
    /// The forward of @p token's session failed, or the client cancelled it
    /// when forwarded() did not answer in time: the socket closes, and the
    /// pane answers with @p reason. Only "forward" comes from the client.
    /// Returns an error (also for a session with no socket), or empty.
    QString failed(const QString& token, const QString& reason);
    /// Ends the sessions of @p pane, which closed.
    void paneClosed(quint32 pane);

    /// The tokens of the open sessions. A test hook.
    QStringList tokens() const { return m_sessions.keys(); }
    /// The local socket of @p token, empty without one. A test hook.
    QString socket(const QString& token) const;
    /// Replaces the program that cancels the forwards ("ssh"). A test hook.
    void setSsh(const QString& program) { m_ssh = program; }

private:
    struct Session;

    void applySettings();
    /// Ends @p token's session: its socket closes and its forward is
    /// cancelled. Its runs fail with @p reason.
    void close(const QString& token, const QString& reason);
    /// Sets @p pane's answer from its newest session that has one, or "off"
    /// without one. False when there is no such pane.
    bool refresh(quint32 pane);
    /// Runs `ssh OPTIONS -O cancel -R REMOTE:LOCAL -- DESTINATION`, without waiting.
    void cancel(const Session& session) const;

    Control& m_control;
    ScriptHost& m_scripts;
    SettingsStore& m_settings;
    PaneAnswer m_answer;
    std::shared_ptr<CommandBudget> m_budget;
    QHash<QString, std::shared_ptr<Session>> m_sessions;
    quint64 m_opened = 0; ///< orders the sessions of a pane
    QString m_ssh = QStringLiteral("ssh");
};

} // namespace Afterglow
