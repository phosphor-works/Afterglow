// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>

#include <memory>

namespace PhosphorIpc {
class IpcRouter;
}

namespace Afterglow {

/// The process's Unix socket: one phosphor-ipc router for launches (target
/// "launch"), external control (target "control") and the commands of
/// scripts (target "commands", CommandSocket). Only the user can open it.
///
/// The main process of a Wayland display listens on sharedPath(). A process
/// that runs on its own (-o, --config, --class, another build) listens on
/// ownPath(). A process holds an exclusive lock on PATH.lock while it listens
/// on PATH, and the kernel releases it when the process ends, also in a crash.
/// So of two launches at the same moment one gets the path and the other
/// hands its window over, and only a socket whose lock is free is dead.
class InstanceSocket
{
public:
    InstanceSocket();
    ~InstanceSocket();
    InstanceSocket(const InstanceSocket&) = delete;
    InstanceSocket& operator=(const InstanceSocket&) = delete;

    enum class Listen {
        Listening,
        Held, ///< another process holds the path
        Failed, ///< this process cannot use the path (a bind error, a file that is not a socket)
    };
    /// Listens on @p path. On Held or Failed the router has no socket.
    Listen listen(const QString& path);
    bool listening() const { return !m_path.isEmpty(); }
    PhosphorIpc::IpcRouter& router() { return *m_router; }

    /// Removes the sockets in $XDG_RUNTIME_DIR that a process left when it
    /// ended without its destructors (a crash, SIGKILL). Nothing else removes
    /// them until a later process binds the same name.
    static void removeStaleSockets();
    /// The socket that this process listens on, for the panes'
    /// AFTERGLOW_SOCKET. Empty when it listens on none.
    static QString processPath();
    /// The socket of the main process on this Wayland display. Empty without
    /// XDG_RUNTIME_DIR.
    static QString sharedPath();
    /// The socket of a process that runs on its own. Empty without
    /// XDG_RUNTIME_DIR.
    static QString ownPath();

private:
    std::unique_ptr<PhosphorIpc::IpcRouter> m_router;
    QString m_path;
    int m_lock = -1;
};

} // namespace Afterglow
