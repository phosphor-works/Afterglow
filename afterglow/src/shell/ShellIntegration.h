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
/// Modify only recognized interactive shell invocations. Does not edit user files.
/// @p switches is Terminal.shellIntegration: "disabled" turns it off, and the
/// no-* switches reach the scripts in AFTERGLOW_SHELL_FEATURES.
/// @p promptCursorBlinks picks the blinking or the steady bar that the
/// scripts show at the prompt (AFTERGLOW_PROMPT_CURSOR, DECSCUSR 5 or 6), so
/// that it follows Terminal.cursorBlink.
/// @p inherited is the environment that the child starts from when it is
/// not this process's (a mux session's): AFTERGLOW_SHELL_INTEGRATION, SHELL
/// and the variables that the integration saves come from it.
bool prepare(AfterglowPty::SpawnOptions& options, const QString& switches, const QString& root = {},
             const InheritedEnvironment* inherited = nullptr, bool promptCursorBlinks = true);
}
