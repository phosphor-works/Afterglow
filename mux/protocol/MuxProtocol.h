// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowProto/Json.h>
#include <AfterglowVt/Terminal.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The calls of the mux daemon's control target, and the values that they
// carry. The client (afterglow mux) and the daemon (afterglow-mux) share this
// code, so the two ends read the same format.

namespace AfterglowMux {

/// Changes when a call, its arguments or its result change incompatibly. The
/// daemon refuses a client with another version, and names both.
inline constexpr int ProtocolVersion = 1;

/// The phosphor-ipc target of the daemon's calls.
inline constexpr const char* Target = "mux";

namespace Fn {
inline constexpr const char* Version = "version"; ///< () -> {version, protocol}
inline constexpr const char* NewSession = "newSession"; ///< (protocol, options) -> {name}
inline constexpr const char* ListSessions = "listSessions"; ///< () -> [session]
inline constexpr const char* KillSession = "killSession"; ///< (name) -> null
} // namespace Fn

/// The error codes of the daemon's calls, besides those of phosphor-ipc.
namespace Error {
inline constexpr const char* NoSuchSession = "NO_SUCH_SESSION";
inline constexpr const char* SessionExists = "SESSION_EXISTS";
inline constexpr const char* VersionMismatch = "VERSION_MISMATCH";
inline constexpr const char* SpawnFailed = "SPAWN_FAILED";
} // namespace Error

/// The longest session name.
inline constexpr std::size_t MaxSessionName = 64;

/// 1 to 64 letters, digits, underscores or hyphens.
bool validSessionName(std::string_view name);

/// What runs in a new pane. The client prepares it from its configuration
/// (the shell, shell integration, TERM and the environment of the launch).
/// The daemon adds only the variables that name the session and the pane.
struct Launch
{
    std::vector<std::string> argv; ///< empty: the user's login shell
    std::string workingDirectory;
    std::vector<std::pair<std::string, std::string>> environment;
    std::vector<std::string> unsetEnvironment;
    /// The program of a command launch, which counts as running while it is
    /// in the foreground. Empty for a shell.
    std::string launchedProgram;
};

/// The size of a pane, as the client measures it with its font.
struct PaneSize
{
    int cols = 80;
    int rows = 24;
    int cellWidth = 0; ///< pixels; 0 when unknown
    int cellHeight = 0;
};

struct SessionOptions
{
    std::string name; ///< empty: the smallest free number, from 1
    Launch launch;
    PaneSize size;
    /// The terminal values of the client: scrollback, theme colors and the
    /// rest. The daemon reads no configuration file.
    AfterglowVt::TerminalOptions terminal;
};

AfterglowProto::Json::Value toJson(const SessionOptions& options);
/// nullopt for a value that is not a valid options object: @p error says why.
std::optional<SessionOptions> sessionOptions(const AfterglowProto::Json::Value& value, std::string* error);

/// A session in the result of listSessions.
struct SessionInfo
{
    std::string name;
    bool attached = false;
    int windows = 0;
    int panes = 0;
    /// The programs that run in the foreground of its panes, for the
    /// question of `afterglow mux kill`. Their names are the programs' own
    /// choice: show them as plain text.
    std::vector<std::string> jobs;
};

AfterglowProto::Json::Value toJson(const SessionInfo& info);
std::optional<SessionInfo> sessionInfo(const AfterglowProto::Json::Value& value);

} // namespace AfterglowMux
