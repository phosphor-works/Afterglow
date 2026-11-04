// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <PhosphorIpc/IpcRouter.h>

#include <QString>

namespace Afterglow {

/// The process's Unix socket: one phosphor-ipc router for launches (target
/// "launch") and external control (target "control"). Only the user can open
/// it.
///
/// The main process of a Wayland display listens on sharedPath(). Binding a
/// path is atomic, so of two launches at the same moment one wins and the
/// other hands its window over. A process that runs on its own (-o, --config,
/// --class, another build) listens on ownPath().
class InstanceSocket
{
public:
    /// Removes the dead sockets of earlier processes in $XDG_RUNTIME_DIR.
    InstanceSocket();
    ~InstanceSocket();
    InstanceSocket(const InstanceSocket&) = delete;
    InstanceSocket& operator=(const InstanceSocket&) = delete;

    /// Listens on @p path. False when another process listens there, or the
    /// path cannot be bound; the router then has no socket.
    bool listen(const QString& path);
    bool listening() const { return !m_path.isEmpty(); }
    QString path() const { return m_path; }
    PhosphorIpc::IpcRouter& router() { return m_router; }

    /// The socket that this process listens on, for the panes'
    /// AFTERGLOW_SOCKET. Empty when it listens on none.
    static QString processPath();
    /// The socket of the main process on this Wayland display. Empty without
    /// XDG_RUNTIME_DIR.
    static QString sharedPath();
    /// The socket of a process that runs on its own.
    static QString ownPath();

private:
    PhosphorIpc::IpcRouter m_router;
    QString m_path;
};

} // namespace Afterglow
