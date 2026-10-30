// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <AfterglowPty/Pty.h>
#include <QString>

namespace Afterglow::ShellIntegration {
/// Modify only recognized interactive shell invocations. Does not edit user files.
bool prepare(AfterglowPty::SpawnOptions& options, bool enabled, const QString& root = {});
}
