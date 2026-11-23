// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Afterglow {

/// afterglow mux ...: the sessions of the mux daemon, no GUI. @p argv[0]
/// is "mux".
int runMux(int argc, char** argv);

} // namespace Afterglow
