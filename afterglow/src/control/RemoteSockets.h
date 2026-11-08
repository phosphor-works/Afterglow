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

/// The sockets that `afterglow ssh` forwards to remote hosts, one for each
/// ssh session, while Terminal.sshRemoteCommands is on (and
/// Terminal.sshIntegration, Control.enabled and Scripts.enabled).
///
/// A session's socket is $XDG_RUNTIME_DIR/afterglow-remote-<token>.sock,
/// with the "remote" target only (CommandSocket for a remote session). The
/// session also keeps the answer of its pane to `aglow run`'s query (OSC
/// 24569): the socket's path on the remote host, or the reason why there is
/// none. It ends when the process that opened it exits (watched with a
/// pidfd), when its pane closes, or when the settings turn it off. Then a
/// forward that was added is cancelled with `ssh -O cancel`.
class RemoteSockets : public QObject
{
    Q_OBJECT

public:
    /// Sets what @p pane answers to the query. False when there is no such pane.
    using PaneAnswer = std::function<bool(quint32 pane, const AfterglowProto::Remote::Answer& answer)>;

    RemoteSockets(Control& control, ScriptHost& scripts, SettingsStore& settings, PaneAnswer answer, QObject* parent = nullptr);
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
        /// ssh's options that reach the shared connection (-F, -p, ControlPath
        /// and the others), for `ssh -O cancel`. Never the program: it is
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
    /// with no socket (Opened::error) still sets the pane's answer.
    QString open(const Request& request, Opened& out);
    /// The forward of @p token's session failed: the socket closes, and the
    /// pane answers with @p reason. Only "forward" comes from the client.
    /// Returns an error, or empty.
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
    /// Ends @p token's session: its socket closes and its forward is cancelled.
    void close(const QString& token);
    /// Sets @p pane's answer from its newest session, or "off" without one.
    void refresh(quint32 pane);
    /// Runs `ssh OPTIONS -O cancel -R REMOTE:LOCAL DESTINATION`, without waiting.
    void cancel(const Session& session) const;

    Control& m_control;
    ScriptHost& m_scripts;
    SettingsStore& m_settings;
    PaneAnswer m_answer;
    QHash<QString, std::shared_ptr<Session>> m_sessions;
    quint64 m_opened = 0; ///< orders the sessions of a pane
    QString m_ssh = QStringLiteral("ssh");
};

} // namespace Afterglow
