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
    bool noSession = false;   ///< -N, -W, -f or -s: no remote command runs in the foreground
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
    bool noSession = false;      ///< SessionType none or subsystem, or ForkAfterAuthentication: like -N, -s or -f
    bool stdinNull = false;      ///< StdinNull: like -n
    QString requestTty;          ///< ssh -G's word for RequestTTY and -t, -T: auto, true, false or force
};
Config parseConfig(const QByteArray& resolved);

/// OSC 3008 sequences that mark the ssh session as a remote context.
QByteArray contextStart(const QString& id, const Config& config);
QByteArray contextEnd(const QString& id);

/// What the probe for remote commands found on the host.
struct Probe {
    bool ran = false;  ///< the script ran there: its first line came, whatever its exit status
    bool aglow = false; ///< `aglow --version` ran
    QString version;   ///< its line
    int protocol = -1; ///< the protocol in that line, -1 when it names none
    /// The remote directory, made with mode 0700: $XDG_RUNTIME_DIR/afterglow
    /// when that is an absolute path, else ~/.local/state/afterglow/run/HOST
    /// for an absolute $HOME (one for each host, as a home on NFS is shared).
    /// Empty when there is none, or it is not UTF-8 text.
    QString directory;
};
/// The probe: one sh script that reports that it ran and
/// `aglow --version`, and with aglow there makes the remote directory and
/// runs `aglow remote-prune` in it. It exits with 0 once it ran.
QByteArray probeScript();

/// The ssh command that runs a command on @p destination over the connection
/// that `afterglow ssh` shares with it, so that the user authenticates once:
/// ssh, -T, the options that share the connection, "--" and the
/// destination. The command's words follow. Empty, with @p error, when ssh
/// cannot read the destination (ssh -G).
QByteArrayList sharedCommand(const QString& destination, QString* error);

/// Puts afterglow's terminfo entry on @p destination, as `afterglow ssh`
/// does, once per entry version: over the shared connection. False when the
/// host cannot take it (no tic) or ssh fails: then TERM is xterm-256color
/// there.
bool installTerminfo(const QString& destination);
Probe parseProbe(const QByteArray& output);

/// Runs ssh with ARGS, byte for byte, and returns its exit status. When ssh
/// dies from an interrupt, the helper dies from it too. The environment
/// controls it: AFTERGLOW_SSH_TERMINFO is "install" (the default) or
/// "fallback". In a pane (AFTERGLOW_SOCKET and AFTERGLOW_PANE_ID), when the
/// terminal allows remote commands, it probes the host and forwards a socket
/// of the terminal there for `aglow run` (RemoteSockets).
int run(const QByteArrayList& args);

} // namespace Afterglow::Ssh
