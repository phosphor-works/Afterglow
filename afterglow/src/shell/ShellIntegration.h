// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <AfterglowPty/Pty.h>
#include <QString>
#include <string>
#include <utility>
#include <vector>

namespace Afterglow {
/// The environment that a pane's program starts from when it is not this
/// process's: a mux session's (MuxClient::environment), to which the daemon
/// applies what the launch sets and unsets. Null means this process's.
using InheritedEnvironment = std::vector<std::pair<std::string, std::string>>;
}

namespace Afterglow::ShellIntegration {
/// What prepare() integrates, and how.
struct Options
{
    /// Terminal.shellIntegration as Settings::read leaves it: "enabled",
    /// "disabled", or Kitty's switches, each once. "disabled" turns it off.
    /// no-rc sets the environment but changes no command line and no
    /// startup file of the shell: the user's startup files load the
    /// script. no-complete changes nothing, as the scripts load no
    /// completions. The switches reach the scripts in
    /// AFTERGLOW_SHELL_FEATURES.
    QString switches = QStringLiteral("enabled");
    /// The directory of the scripts; empty: the source tree's in a build
    /// tree, else the installed one.
    QString root;
    /// The environment that the child starts from when it is not this
    /// process's (a mux session's): AFTERGLOW_SHELL_INTEGRATION, SHELL and
    /// the variables that the integration saves come from it.
    const InheritedEnvironment* inherited = nullptr;
    /// The blinking or the steady bar that the scripts show at the prompt
    /// (AFTERGLOW_PROMPT_CURSOR, DECSCUSR 5 or 6), so that it follows
    /// Terminal.cursorBlink. The shell reads it once, when it starts: a
    /// later change of Terminal.cursorBlink reaches new shells only. The
    /// scripts leave the cursor alone in vi key bindings, which set their own.
    bool promptCursorBlinks = true;
};

/// Modify only recognized interactive shell invocations. Does not edit user files.
/// Nothing changes, and false comes back, for a bash entry path that holds
/// a dollar sign, a backquote or a backslash: bash expands its ENV.
/// True when the shell gets the integration's environment.
bool prepare(AfterglowPty::SpawnOptions& options, const Options& integration = {});
}
