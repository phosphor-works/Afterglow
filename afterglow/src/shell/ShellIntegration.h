// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <AfterglowPty/Pty.h>
#include <QString>
#include <string>
#include <utility>
#include <vector>

namespace Afterglow::ShellIntegration {
/// Modify only recognized interactive shell invocations. Does not edit user files.
/// @p inherited is the environment that the child starts from when it is
/// not this process's (a mux session's): AFTERGLOW_SHELL_INTEGRATION, SHELL
/// and the variables that the integration saves come from it.
bool prepare(AfterglowPty::SpawnOptions& options, bool enabled, const QString& root = {},
             const std::vector<std::pair<std::string, std::string>>* inherited = nullptr);
}
