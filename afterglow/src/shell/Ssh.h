// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QByteArrayList>
#include <QString>
#include <QStringList>

// `afterglow ssh`: runs ssh with the remote side prepared for afterglow. The
// pane shows the remote host while ssh runs, COLORTERM and TERM_PROGRAM are
// offered to the server, and TERM=afterglow either reaches the host with its
// terminfo entry or falls back to xterm-256color.
namespace Afterglow::Ssh {

/// Where the destination is in ssh's arguments, and what kind of run it is.
/// ssh reads options before and after the destination, up to the remote
/// command or a "--".
struct Arguments {
    int destination = -1;     ///< index of the destination, -1 when there is none
    int command = -1;         ///< index of the remote command's first word, -1 when there is none
    QList<int> options;       ///< indexes of the option words and their values, in order
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
    QString proxyJump;
    QString proxyCommand;
    bool setEnv = false;         ///< the user's config already sets environment variables
    bool controlPath = false;    ///< the user's config already multiplexes connections
    bool controlMaster = false;  ///< ControlMaster auto or autoask: a run may become the master
    bool controlPersist = false; ///< the master outlives its first session
    bool remoteCommand = false;  ///< the config runs a command in place of one on the command line
};
Config parseConfig(const QByteArray& resolved);

/// OSC 3008 sequences that mark the ssh session as a remote context.
QByteArray contextStart(const QString& id, const Config& config);
QByteArray contextEnd(const QString& id);

/// Runs ssh with ARGS, byte for byte, and returns its exit status. When ssh
/// dies from an interrupt, the helper dies from it too. The environment
/// controls it: AFTERGLOW_SSH_TERMINFO is "install" (the default) or "fallback".
int run(const QByteArrayList& args);

} // namespace Afterglow::Ssh
