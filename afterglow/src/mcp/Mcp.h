// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Afterglow {

/// afterglow mcp ...: an MCP server on standard input and output that gives
/// an agent a mux session of its own (docs/MCP.md), no GUI. @p argv[0] is
/// "mcp".
int runMcp(int argc, char** argv);

} // namespace Afterglow
