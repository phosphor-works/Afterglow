// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowProto/Json.h>
#include <AfterglowVt/Terminal.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The calls of the mux daemon's control target, and the values that they
// carry. The client (afterglow mux) and the daemon (aglow-mux) share this
// code. They can still come from two builds: a daemon keeps running while
// the user installs a new afterglow. ProtocolVersion tells them apart.

namespace AfterglowMux {

/// Changes when a call, its arguments or its result change. Each end checks
/// it: the client calls Fn::Version first on each connection, and the daemon
/// refuses a newSession or an attach of another version. The codecs of the
/// attach stream (MuxStream.h), of the host calls on it (MuxHost.h) and of the
/// state frames in it (AfterglowVt::StateCodec, StateSync.h) change with it.
/// MuxProtocolTest pins the keys of the options to this number, so that a new
/// key changes it too.
inline constexpr int ProtocolVersion = 14;

/// The phosphor-ipc target of the daemon's calls.
inline constexpr const char* Target = "mux";

namespace Fn {
inline constexpr const char* Version = "version"; ///< () -> {version, protocol}
inline constexpr const char* NewSession = "newSession"; ///< (protocol, options) -> {name}
inline constexpr const char* ListSessions = "listSessions"; ///< () -> [session]
inline constexpr const char* KillSession = "killSession"; ///< (name) -> null
/// (protocol, name, {takeover, eventQueue, history}) -> {name}: after the
/// answer, the connection of the call carries the session's stream
/// (MuxStream.h). The client sends nothing after the call until the answer
/// came, and then starts the stream, which writes first: the daemon sends
/// nothing after the answer until then. So neither side's line reader takes
/// a byte of the stream, also through a proxy (`aglow-mux proxy`).
inline constexpr const char* Attach = "attach";
/// (name) -> null: the window of the session closes, and the session keeps running.
inline constexpr const char* Detach = "detach";
/// (name, pane) -> string: the working directory of the program in the
/// foreground of the pane, else of its first program, as the system has it.
/// Empty when the program exited, when neither directory remains, and for a
/// path that is not UTF-8. The window asks, for a new tab or split, when the
/// shell reported no directory that exists on its machine (OSC 7, 9;9, 633,
/// 1337).
inline constexpr const char* PaneDirectory = "paneDirectory";
/// (protocol, name, launch, size) -> {pane}: starts a pane in the session, for
/// a new tab or split of the window that shows it. The pane is a tab of its
/// own in the session's layout until the window sends its layout.
inline constexpr const char* NewPane = "newPane";
/// (name, pane) -> string: the program in the foreground of the pane, for
/// the question before a close (MuxPane::foregroundJob). Empty when none runs.
inline constexpr const char* PaneJob = "paneJob";
/// (protocol, name, layout) -> {name}: the panes of the layout leave the
/// session for a new one, with that layout, the session's environment and
/// terminal values, and the daemon's next number as its name. Their programs
/// run on. The window that showed them drags them out, and shows the new
/// session in a new window. Not every pane of the session: that would only
/// give it another name.
inline constexpr const char* BreakOut = "breakOut";
/// (pane) -> string: the name of the session that holds the pane now. A
/// pane that broke out keeps the AFTERGLOW_SESSION of its first session, as
/// no process changes the environment of another: a client in a session
/// pane asks by its AFTERGLOW_PANE_ID.
inline constexpr const char* PaneSession = "paneSession";
/// (protocol, name) -> {name}: the calls of the control target on this
/// connection act on session @p name (`afterglow ctl --session NAME`).
inline constexpr const char* SelectSession = "selectSession";
/// (protocol, pane) -> {name}: the calls of the control target on this
/// connection act on the session that holds the pane now (`afterglow ctl`
/// in a session pane, by its AFTERGLOW_PANE_ID).
inline constexpr const char* SelectPane = "selectPane";
} // namespace Fn

/// The keys of the options of Fn::Attach. The daemon refuses another key.
namespace AttachOption {
/// A bool: another window that shows the session detaches. Without it, the
/// call fails while one does.
inline constexpr const char* Takeover = "takeover";
/// Mux.eventQueue of the window, in bytes (MinEventQueue to MaxEventQueue).
inline constexpr const char* EventQueue = "eventQueue";
/// Terminal.scrollback of the window: the most history rows that its replica
/// of a pane keeps (0 to MaxHistory). The daemon sends no older rows.
/// Without it, the daemon sends all that it has.
inline constexpr const char* History = "history";
} // namespace AttachOption

/// The error codes of the daemon's calls, besides those of phosphor-ipc.
namespace Error {
inline constexpr const char* NoSuchSession = "NO_SUCH_SESSION";
inline constexpr const char* NoSuchPane = "NO_SUCH_PANE";
inline constexpr const char* SessionExists = "SESSION_EXISTS";
inline constexpr const char* VersionMismatch = "VERSION_MISMATCH";
inline constexpr const char* SpawnFailed = "SPAWN_FAILED";
/// Another window shows the session, and the attach asked for no takeover.
inline constexpr const char* Attached = "ATTACHED";
/// No window shows the session.
inline constexpr const char* NotAttached = "NOT_ATTACHED";
} // namespace Error

/// The environment variable with the descriptor of a pipe that `afterglow
/// mux` gives a daemon that it starts in a session of its own, whose stderr
/// is /dev/null. The daemon writes ReportListening there once it listens,
/// else ReportHeld and why, or only why it cannot start, and closes it. An
/// end of the pipe with none of them is a daemon that ended before it
/// listened.
inline constexpr const char* ReportFdVariable = "AGLOW_MUX_REPORT_FD";
/// A byte that no message has: the daemon listens.
inline constexpr char ReportListening = '\0';
/// A byte that no message has, before the message: another daemon holds
/// one of the sockets. That one took its lock before it binds, so it may
/// answer soon: a client waits for it as for a daemon that listens.
inline constexpr char ReportHeld = '\x01';

/// The longest session name.
inline constexpr std::size_t MaxSessionName = 64;
/// The rule of validSessionName(), in the words of a message.
inline constexpr const char* SessionNameRule =
    "1 to 64 ASCII letters, digits, '_' or '-', with a letter, a digit or '_' first";

/// 1 to 64 ASCII letters, digits, '_' or '-', and the first character is a
/// letter, a digit or '_': a name never reads as an option of `afterglow mux
/// kill`.
bool validSessionName(std::string_view name);

/// The bounds that the daemon takes for the terminal values of a session.
/// They hold every value of the settings that they come from
/// (Terminal.scrollback, Colors.minimumContrast, Graphics.quota): the
/// application's MuxCommandTest checks that.
inline constexpr std::int64_t MaxScrollback = 1000000;
inline constexpr double MinContrast = 1;
inline constexpr double MaxContrast = 21;
inline constexpr std::int64_t MaxGraphicsQuota = std::int64_t(AfterglowVt::GraphicsStore::MaxQuota);
/// The bounds of Mux.eventQueue, in bytes, which an attach passes, and its
/// default, which an attach without it gets.
inline constexpr std::int64_t MinEventQueue = std::int64_t(1) << 20;
inline constexpr std::int64_t MaxEventQueue = std::int64_t(64) << 20;
inline constexpr std::int64_t DefaultEventQueue = std::int64_t(4) << 20;
/// The most history rows that an attach can name: Terminal.scrollback, as
/// for a session's own terminal.
inline constexpr std::int64_t MaxHistory = MaxScrollback;

/// What runs in a new pane. The client prepares it from its configuration
/// (the shell, shell integration, TERM and the environment of the launch),
/// as changes to the session's environment. The daemon then sets
/// AFTERGLOW_SOCKET, AFTERGLOW_PANE_ID and AFTERGLOW_SESSION, and removes
/// AFTERGLOW_OVERLAY.
struct Launch
{
    std::vector<std::string> argv; ///< empty: the user's login shell
    std::string workingDirectory;
    /// The variables that the pane sets over the session's environment.
    std::vector<std::pair<std::string, std::string>> environment;
    /// The variables that the pane removes from the session's environment,
    /// before it sets its own.
    std::vector<std::string> unset;
    /// The program of a command launch, which counts as running while it is
    /// in the foreground. Empty for a shell.
    std::string launchedProgram;
};

AfterglowProto::Json::Value toJson(const Launch& launch);
/// nullopt for a value that is not a valid launch: @p error says why.
std::optional<Launch> launchFrom(const AfterglowProto::Json::Value& value, std::string* error);

/// The size of a pane, as the client measures it with its font.
struct PaneSize
{
    int cols = 80;
    int rows = 24;
    int cellWidth = 0; ///< pixels; 0 when unknown
    int cellHeight = 0;
};

/// The Control settings that the daemon follows for `afterglow ctl` calls to
/// a session (the calls that it answers itself; a window that shows the
/// session follows its own for the calls that it answers).
struct ControlOptions
{
    bool enabled = true; ///< Control.enabled: off, the session takes no control calls
    bool allowReadText = false; ///< Control.allowReadText: get-text, and the commands in ls
};

AfterglowProto::Json::Value toJson(const PaneSize& size);
/// nullopt for a value that is not a valid size: @p error says why.
std::optional<PaneSize> paneSizeFrom(const AfterglowProto::Json::Value& value, std::string* error);

struct SessionOptions
{
    std::string name; ///< empty: the daemon's next number
    /// The environment of the client, which each pane of the session changes
    /// with its launch: the daemon's own is not used. With
    /// callerEnvironment, the variables that it sets over the environment of
    /// the caller.
    std::vector<std::pair<std::string, std::string>> environment;
    /// The session's environment is that of the process that calls
    /// newSession, with `environment` over it: for a session on another host,
    /// `aglow-mux proxy` there, which runs in the user's ssh login. Its
    /// panes without a command run the user's shell as a login shell, in
    /// HOME without a directory.
    bool callerEnvironment = false;
    /// The file mode creation mask of the client, which the programs get in
    /// place of the daemon's. -1 keeps the daemon's.
    int umask = -1;
    /// The first pane.
    Launch launch;
    PaneSize size;
    /// The terminal values of the client: scrollback, theme colors and the
    /// rest. The daemon reads no configuration file.
    AfterglowVt::TerminalOptions terminal;
    /// The answer of a window to an OSC 99 query: what a program may ask of
    /// the desktop notifications of a window that later shows the session.
    std::string notificationCapabilities;
    /// Control.enabled and Control.allowReadText of the client: what
    /// `afterglow ctl` may do with the session in the daemon.
    ControlOptions control;
};

AfterglowProto::Json::Value toJson(const SessionOptions& options);
/// nullopt for a value that is not a valid options object: @p error says why.
std::optional<SessionOptions> sessionOptions(const AfterglowProto::Json::Value& value, std::string* error);

/// A session in the result of listSessions.
struct SessionInfo
{
    std::string name;
    bool attached = false;
    int panes = 0;
    /// The programs that run in the foreground of its panes, for the
    /// question of `afterglow mux kill`. Their names are the programs' own
    /// choice: show them as plain text.
    std::vector<std::string> jobs;
};

AfterglowProto::Json::Value toJson(const SessionInfo& info);
std::optional<SessionInfo> sessionInfo(const AfterglowProto::Json::Value& value);

} // namespace AfterglowMux
