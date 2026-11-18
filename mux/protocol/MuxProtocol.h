// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowProto/Json.h>
#include <AfterglowVt/Terminal.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The calls of the mux daemon (Target), and the values that they carry. Its
// clients (`afterglow mux`, the windows and the projects of afterglow, and
// `aglow-mux proxy`) and the daemon (aglow-mux) share this code. They can
// still come from two builds: a daemon keeps running while the user installs
// a new afterglow. ProtocolVersion tells them apart.

namespace AfterglowMux {

/// Changes when a call, its arguments or its result change. Each end checks
/// it: the client calls Fn::Version first on each connection, and the daemon
/// refuses a newSession, an attach, a newPane, a breakOut, a joinPane, a
/// selectSession or a selectPane of another version: the calls that take it
/// as their first argument. The codecs of the attach stream (MuxStream.h), of the
/// host calls on it (MuxHost.h) and of the state frames in it
/// (AfterglowVt::StateCodec, StateSync.h) change with it. MuxProtocolTest
/// pins the keys of the options to this number, so that a new key changes it
/// too.
inline constexpr int ProtocolVersion = 30;

/// The phosphor-ipc target of the daemon's calls.
inline constexpr const char* Target = "mux";

/// The events of the control target, of a window (ControlApi's signals) and
/// of a session in the daemon, to which `afterglow ctl watch` subscribes.
/// The application's ControlSocketTest checks that they are ControlApi's.
inline constexpr std::array<std::string_view, 13> ControlEvents{
    "paneCreated",    "paneMoved",       "paneClosed",    "titleChanged", "bell",
    "commandStarted", "commandFinished", "statusChanged", "focusChanged", "userVarChanged",
    "contextChanged", "shellChanged",    "textChanged"};

/// The text scopes of the getText call of the control target, as the error
/// for another scope names them: a window and the daemon say the same.
inline constexpr const char* TextScopeNames =
    "screen, scrollback, selection, last-output, first-output-on-screen or last-visited-output";
/// The scopes of getText itself: the text scopes, and since, which reads
/// from a mark (the mark call) to the cursor.
inline constexpr const char* GetTextScopeNames =
    "screen, scrollback, selection, last-output, first-output-on-screen, last-visited-output or since";
/// The error of getText for the cursor option with a scope of one output
/// region (first-output-on-screen, last-visited-output), whose text ends
/// before the cursor's place: a window and the daemon say the same.
inline constexpr const char* CursorScopeError = "cursor is for the screen, scrollback and last-output scopes";
/// The options of getText, as the error for another option names them.
inline constexpr const char* TextOptionNames = "the options are ansi, cursor, wrap_markers and mark";
/// The error of getText for a mark that is not one that the mark call gave.
inline constexpr const char* MarkOptionError =
    "the getText option mark is the object {row, col} that the mark call gives";
/// The error of getText for the since scope without a mark, or a mark with
/// another scope.
inline constexpr const char* MarkScopeError = "the scope since takes the option mark, and no other scope takes it";
/// The kinds of the semantic regions that shellRegions gives.
inline constexpr std::array<std::string_view, 3> ShellRegionKinds{"prompt", "input", "output"};

namespace Fn {
inline constexpr const char* Version = "version"; ///< () -> {version, protocol}
inline constexpr const char* NewSession =
    "newSession"; ///< (protocol, options) -> {name, pane}: pane is its first pane's ID
/// (protocol, environment) -> null: the environment of the caller
/// (environmentJson()), which a session that a later newSession call on
/// this connection starts with SessionOptions::callerEnvironment takes.
/// `aglow-mux proxy` sends its own, that of the user's ssh login, before it
/// carries the client's bytes. The daemon reads the environment of no other
/// process: a kernel that refuses that to the user (FreeBSD with
/// security.bsd.unprivileged_proc_debug=0) needs no exception.
inline constexpr const char* CallerEnvironment = "callerEnvironment";
inline constexpr const char* ListSessions = "listSessions"; ///< () -> [session]
inline constexpr const char* KillSession = "killSession"; ///< (name) -> null
/// (protocol, name, {takeover, eventQueue, history}) -> {name, window,
/// environment}: after the answer, the connection of the call carries the
/// session's stream (MuxStream.h). The client sends nothing after the call
/// until the answer came, and then starts the stream, which writes first:
/// the daemon sends nothing after the answer until then. So neither side's
/// line reader takes a byte of the stream, also through a proxy (`aglow-mux
/// proxy`). window is the serial of the window, which its newPane and
/// breakOut give. environment is the session's environment
/// (SessionOptions::environment, environmentJson()), which the window's
/// launches of new panes change, as the daemon does.
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
/// (protocol, name, launch, size[, window]) -> {pane}: starts a pane in the
/// session, for a new tab or split of the window that shows it. The pane is
/// a tab of its own in the session's layout until the window sends its
/// layout. A window gives the window of its attach answer: a window that
/// another one took the session from gets Error::Attached, and one that no
/// longer shows the session, while none does, gets Error::NotAttached. A
/// session of MaxSessionPanes panes gets Error::TooManyPanes.
inline constexpr const char* NewPane = "newPane";
/// (name, pane) -> string: the program in the foreground of the pane, for
/// the question before a close (MuxPane::foregroundJob). Empty when none runs.
inline constexpr const char* PaneJob = "paneJob";
/// (protocol, name, layout[, window]) -> {name}: the panes of the layout leave the
/// session for a new one, with that layout, the session's environment and
/// terminal values, and the daemon's next number as its name. Their programs
/// run on. The window that showed them drags them out, and shows the new
/// session in a new window. Not every pane of the session: that would only
/// give it another name. A window gives the window of its attach answer, as
/// for newPane.
inline constexpr const char* BreakOut = "breakOut";
/// (protocol, name, pane, beside, placement[, window]) -> {name}: pane
/// @p pane leaves session @p name for the session that holds pane @p beside,
/// another session of the daemon, with its program running. placement is
/// "left", "right", "up" or "down": beside that pane. "new-tab": in a tab of
/// its own after its tab. name in the answer is the session that holds the
/// pane now. The window that shows that session places the pane (the
/// placePane call of the control target), and the window that showed it
/// drags it out, as for breakOut. A session that loses its last pane ends.
/// The session of @p beside takes no pane past MaxSessionPanes
/// (Error::TooManyPanes). A window gives the window of its attach answer, as
/// for newPane.
inline constexpr const char* JoinPane = "joinPane";
/// (pane) -> string: the name of the session that holds the pane now. A
/// pane that broke out or joined another session keeps the
/// AFTERGLOW_SESSION of its first session, as no process changes the
/// environment of another: a client in a session pane asks by its
/// AFTERGLOW_PANE_ID.
inline constexpr const char* PaneSession = "paneSession";
/// (protocol, name) -> {name, environment}: the calls of the control target
/// on this connection act on session @p name (`afterglow ctl --session
/// NAME`). environment is the session's environment (environmentJson()),
/// for the programs that `afterglow ctl` prepares for its panes.
inline constexpr const char* SelectSession = "selectSession";
/// (protocol, pane) -> {name, environment}: the calls of the control target
/// on this connection act on the session that holds the pane now
/// (`afterglow ctl` in a session pane, by its AFTERGLOW_PANE_ID), as for
/// selectSession.
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
/// The session has MaxSessionPanes panes (MuxLayout.h): newPane, and the
/// split and newTab of `afterglow ctl`, start none.
inline constexpr const char* TooManyPanes = "TOO_MANY_PANES";
/// A pane cannot answer now, and can a moment later: the daemon's pane
/// decodes an image, or a window's session pane still gets its rows from
/// the daemon. A window and the daemon give the same code.
inline constexpr const char* TryAgain = "TRY_AGAIN";
} // namespace Error

/// The environment variable with the descriptor of a pipe that a client
/// (connectOrStart: `afterglow mux`, the projects of afterglow from a worker
/// thread, `aglow-mux proxy --start`) gives a daemon that it starts in a
/// session of its own, whose stderr is /dev/null. The daemon writes
/// ReportListening there once it listens, else ReportHeld and why, or only
/// why it cannot start, and closes it. An end of the pipe with none of them
/// is a daemon that ended before it listened.
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

/// The longest Control.eventInterval, in milliseconds.
inline constexpr std::int64_t MaxEventInterval = 10000;

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
/// The bounds of Mux.keptNotifications, and its default: the most
/// notifications that a pane keeps while no window shows its session
/// (MuxPane::setKeptNotifications).
inline constexpr std::int64_t MaxKeptNotifications = 100;
inline constexpr std::int64_t DefaultKeptNotifications = 8;

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
    /// The command that the pane was asked to run, as a project saves it
    /// (Layout::values), within validPaneCommand. Empty for a shell.
    std::vector<std::string> command;
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
    /// Control.allowReadText: get-text, screenshots, the command lines in ls,
    /// and the command lines and user variables in the events of watch.
    bool allowReadText = false;
    /// Why the client's settings allow no remote commands, as the refusal
    /// of a call says it ("remote commands are off (SETTING)"), or empty:
    /// whether `afterglow ssh` in a session pane gets a socket.
    std::string remoteCommandsOff{};
    /// Control.eventInterval: the shortest time in milliseconds between two
    /// events of one kind for one pane of `afterglow ctl watch`, 0 to
    /// MaxEventInterval.
    int eventInterval = 100;
    bool operator==(const ControlOptions&) const = default;
};

AfterglowProto::Json::Value toJson(const PaneSize& size);
/// nullopt for a value that is not a valid size: @p error says why. A size
/// past what a terminal can be is clamped to it, as a Resize of the attach
/// stream is (Terminal::resize): the window measures any size.
std::optional<PaneSize> paneSizeFrom(const AfterglowProto::Json::Value& value, std::string* error);

struct SessionOptions
{
    std::string name; ///< empty: the daemon's next number
    /// The environment of the client, which each pane of the session changes
    /// with its launch: the daemon's own is not used. A local session
    /// carries all of the client's environment: with the rest of the call,
    /// it must fit one request line of 1 MiB (IpcConnection::MaxRequestBytes)
    /// as JSON, with its escapes. With
    /// callerEnvironment, the variables that it sets over the environment of
    /// the caller.
    std::vector<std::pair<std::string, std::string>> environment;
    /// The session's environment is the one that the caller sent on the same
    /// connection (Fn::CallerEnvironment), with `environment` over it: for a
    /// session on another host, that of `aglow-mux proxy` there, which runs
    /// in the user's ssh login. Its
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
    /// Control.enabled, Control.allowReadText and the remote commands of the
    /// client: what `afterglow ctl` and `afterglow ssh` may do with the
    /// session in the daemon.
    ControlOptions control;
    /// Mux.keptNotifications of the client: the most notifications that
    /// each pane keeps for the next window, 0 to MaxKeptNotifications.
    int keptNotifications = int(DefaultKeptNotifications);
};

/// The variables of an environment as SessionOptions and Launch send them:
/// [name, value] pairs, in order.
AfterglowProto::Json::Value environmentJson(const std::vector<std::pair<std::string, std::string>>& environment);
/// nullopt for a value that is not such a list, or that has a name that is
/// empty or holds '=' or NUL: @p error says why.
std::optional<std::vector<std::pair<std::string, std::string>>>
environmentFrom(const AfterglowProto::Json::Value& value, std::string* error);

AfterglowProto::Json::Value toJson(const SessionOptions& options);
/// nullopt for a value that is not a valid options object: @p error says why.
std::optional<SessionOptions> sessionOptions(const AfterglowProto::Json::Value& value, std::string* error);

/// The settings of the window that shows a session, which it sends after
/// its attach and after each change of them (Attach::InputType::Settings):
/// the session follows them from then on, also while no window shows it.
struct SessionSettings
{
    AfterglowVt::TerminalOptions terminal;
    ControlOptions control;
    /// The variables of Mux.updateEnvironment as the window's process has
    /// them: a value replaces the session's, and none removes it. The
    /// panes that start later get them.
    std::vector<std::pair<std::string, std::optional<std::string>>> environment{};
    /// Mux.keptNotifications, as in SessionOptions.
    int keptNotifications = int(DefaultKeptNotifications);
    /// The answer to OSC 99's query, as in SessionOptions: empty while the
    /// window shows no notifications (Notifications.enabled).
    std::string notificationCapabilities{};
    bool operator==(const SessionSettings&) const = default;
};
/// The most variables that the settings of a window name.
inline constexpr std::size_t MaxUpdatedVariables = 256;

AfterglowProto::Json::Value toJson(const SessionSettings& settings);
/// nullopt for a value that is not a valid settings object, by the rules of
/// the terminal and control values of SessionOptions: @p error says why.
std::optional<SessionSettings> sessionSettings(const AfterglowProto::Json::Value& value, std::string* error);

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
