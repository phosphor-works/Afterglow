// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <QByteArray>
#include <QByteArrayList>
#include <QString>
#include <QStringList>

#include <optional>
#include <string>
#include <utility>
#include <vector>

// ssh for Afterglow. `afterglow ssh` runs ssh with the remote side prepared
// for afterglow: the pane shows the remote host while ssh runs, COLORTERM
// and TERM_PROGRAM are offered to the server, and TERM=afterglow either
// reaches the host with its terminfo entry or falls back to xterm-256color.
// `afterglow mux --ssh` and a window that shows a mux session of another
// host reach the daemon there through sharedCommand(), and `afterglow mux
// new --ssh` installs the entry there with installTerminfo().
namespace Afterglow::Ssh {

/// Where the destination is in ssh's arguments, and what kind of run it is.
/// ssh reads options before and after the destination, up to the remote
/// command or a "--".
struct Arguments
{
    int destination = -1; ///< index of the destination, -1 when there is none
    int command = -1; ///< index of the remote command's first word, -1 when there is none
    QList<int> options; ///< indexes of the option words and their values, in order
    bool passThrough = false; ///< -G, -V, -Q or -O: ssh does not open a session
    bool noSession = false; ///< -N, -W, -f or -s: no remote command runs in the foreground
};
Arguments parseArguments(const QStringList& args);

/// The parts of `ssh -G` the helper uses.
struct Config
{
    QString host; ///< the name as typed, or its Host alias
    QString hostname; ///< what it resolves to
    QString user;
    QString port;
    QString proxyJump;
    QString proxyCommand;
    bool setEnv = false; ///< the user's config already sets environment variables
    bool controlPath = false; ///< the user's config already multiplexes connections
    bool controlMaster = false; ///< ControlMaster auto or autoask: a run may become the master
    bool controlPersist = false; ///< the master outlives its first session
    bool remoteCommand = false; ///< the config runs a command in place of one on the command line
    bool noSession = false; ///< SessionType none or subsystem, or ForkAfterAuthentication: like -N, -s or -f
    bool stdinNull = false; ///< StdinNull: like -n
    QString requestTty; ///< ssh -G's word for RequestTTY and -t, -T: auto, true, false or force
};
Config parseConfig(const QByteArray& resolved);

/// OSC 3008 sequences that mark the ssh session as a remote context.
QByteArray contextStart(const QString& id, const Config& config);
QByteArray contextEnd(const QString& id);

/// What the probe for remote commands found on the host.
struct Probe
{
    bool ran = false; ///< the script ran there: its first line came, whatever its exit status
    bool aglow = false; ///< `aglow --version` ran
    QString version; ///< its line
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
Probe parseProbe(const QByteArray& output);

/// Whether the answer to openRemoteSocket names the pair that the session
/// with @p token asked for, which goes into ssh's -R REMOTE:LOCAL: @p local
/// is the socket of an Afterglow (afterglow-remote-TOKEN.sock) or of the mux
/// daemon for a session pane (remote-ID-TOKEN.sock in its directory), with
/// no ':' or '\', where ssh splits and escapes. @p remote is
/// DIRECTORY/TOKEN.sock in the @p directory of the probe.
bool forwardPair(const std::string& local, const std::string& remote, const std::string& token,
                 const std::string& directory);

/// What `ssh -G -- DESTINATION` says of @p destination, or nothing with
/// @p error. It runs with posix_spawn and a plain wait, as the window's
/// process needs: no fork that allocates beside other threads, no change to
/// the signals of the process, and a stop of ssh stops nothing here. ssh
/// gets @p environment, and comes from its PATH: the environment of the
/// process can change on another thread meanwhile (an activation token).
std::optional<Config> resolve(const QString& destination, const std::vector<std::string>& environment, QString* error);

/// The ssh command that runs a command on @p destination for `afterglow mux
/// --ssh` and for a window that shows a session there: ssh, -T, the options
/// of Afterglow's own connection, the options that let a command run where
/// the user's config runs none, "--" and the destination. The command's
/// words follow. The connection is always Afterglow's own master for these
/// commands (afterglow-ssh-%C in the private XDG_RUNTIME_DIR), never one of
/// the user's or the one of `afterglow ssh`, and it stays @p controlPersist
/// seconds (Terminal.sshControlPersist) after its last command, so that a
/// window joins the connection that a terminal signed in. It forwards
/// nothing and runs no LocalCommand. ssh -G runs with @p environment
/// (resolve()). Empty, with @p error, when ssh cannot read the destination
/// (ssh -G), or there is no private XDG_RUNTIME_DIR for that connection.
/// The form in one step; muxRoute() resolves first, and keeps the Config.
QByteArrayList sharedCommand(const QString& destination, int controlPersist,
                             const std::vector<std::string>& environment, QString* error);
/// sharedCommand() with @p config, which resolve() read for @p destination
/// already: no ssh -G runs.
QByteArrayList sharedCommand(const QString& destination, const Config& config, int controlPersist, QString* error);

/// The variables of the terminal in @p environment (offerTerminalEnvironment)
/// that go to a session of another host: all but TERMINFO_DIRS, which names
/// directories of this computer. The host finds the entry that
/// installTerminfo() put in its own database.
std::vector<std::pair<std::string, std::string>>
remoteVariables(std::vector<std::pair<std::string, std::string>> environment);

/// How a terminfo install ended.
enum class Install {
    Installed,
    Unavailable, ///< no entry to send, or the host cannot take it
    ConnectionFailed, ///< ssh could not connect: the session would fail the same way
    Ended, ///< ssh died from a signal, such as Ctrl+C at a password prompt
};
struct InstallResult
{
    Install result = Install::Unavailable;
    int status = 0; ///< the install's wait status, for Install::Ended
    /// Why, for Install::ConnectionFailed when ssh did not run, and for
    /// Install::Unavailable when this computer has no entry to send.
    QString error;
};

/// Puts afterglow's terminfo entry on @p destination for `afterglow mux new
/// --ssh`, as `afterglow ssh` does, once per entry version, over the
/// connection of sharedCommand(), which the session then shares. @p config
/// is what resolve() read for @p destination, for the route there: ssh -G
/// does not run again. infocmp reads the entry with @p terminfoDirs as its
/// TERMINFO_DIRS (the pane's, for an entry in the build tree), empty for
/// this process's. ssh can ask for a password on the terminal meanwhile: an
/// interrupt there ends ssh and not this process, which then ends with
/// endAs().
InstallResult installTerminfo(const QString& destination, const Config& config, int controlPersist,
                              const QByteArray& terminfoDirs);

/// Ends this process the way a run with wait status @p status ended: by the
/// same signal when an interrupt, a hang-up, a termination, SIGPIPE or
/// SIGKILL ended it, as a shell needs to stop a loop. Else it returns the
/// exit status to end with.
int endAs(int status);

/// Runs ssh with ARGS, byte for byte, and returns its exit status. When ssh
/// dies from an interrupt, the helper dies from it too. The environment
/// controls it: AFTERGLOW_SSH_TERMINFO is "install" (the default) or
/// "fallback". In a pane (AFTERGLOW_SOCKET and AFTERGLOW_PANE_ID), when the
/// terminal allows remote commands, it probes the host and forwards a socket
/// of the terminal there for `aglow run` (RemoteSockets).
int run(const QByteArrayList& args);

} // namespace Afterglow::Ssh
