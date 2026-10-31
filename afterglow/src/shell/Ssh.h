// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QString>
#include <QStringList>

// `afterglow ssh`: runs ssh with the remote side prepared for afterglow. The
// pane shows the remote host while ssh runs, COLORTERM and TERM_PROGRAM are
// offered to the server, and TERM=afterglow either reaches the host with its
// terminfo entry or falls back to xterm-256color.
namespace Afterglow::Ssh {

/// Where the destination is in ssh's arguments, and what kind of run it is.
struct Arguments {
    int destination = -1;     ///< index of the destination, -1 when there is none
    bool passThrough = false; ///< -G, -V, -Q or -O: ssh does not open a session
    bool noSession = false;   ///< -N, -W or -f: no remote command runs in the foreground
};
Arguments parseArguments(const QStringList& args);

/// The parts of `ssh -G` the helper uses.
struct Config {
    QString host;     ///< the name as typed, or its Host alias
    QString hostname; ///< what it resolves to
    QString user;
    QString port;
    bool setEnv = false;      ///< the user's config already sets environment variables
    bool controlPath = false; ///< the user's config already multiplexes connections
};
Config parseConfig(const QByteArray& resolved);

/// OSC 3008 sequences that mark the ssh session as a remote context.
QByteArray contextStart(const QString& id, const Config& config);
QByteArray contextEnd(const QString& id);

/// Runs ssh with ARGS and returns its exit status. The environment controls it:
/// AFTERGLOW_SSH_TERMINFO is "install" (the default) or "fallback".
int run(const QStringList& args);

} // namespace Afterglow::Ssh
