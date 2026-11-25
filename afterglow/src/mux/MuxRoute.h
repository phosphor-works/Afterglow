// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxConnect.h"

#include <QString>

#include <optional>

namespace Afterglow {

namespace Ssh {
struct Config;
}

/// The way to the mux daemon of @p host: @p remoteCommand there
/// (Mux.remoteCommand, `aglow-mux proxy`), which ssh runs over Afterglow's
/// own connection to the host (Ssh::sharedCommand), which stays
/// @p controlPersist seconds after its last command
/// (Terminal.sshControlPersist). Plain values: for any thread, with the
/// @p environment that the thread which changes it took (ssh -G runs with
/// it). Nothing, with @p error, when ssh cannot read the destination, or
/// there is no private XDG_RUNTIME_DIR for that connection. With
/// @p resolved, it also gets what ssh -G read, for another command over the
/// same connection (Ssh::installTerminfo), which then runs no ssh -G.
std::optional<AfterglowMux::Route> muxRoute(const QString& host, const QString& remoteCommand, int controlPersist,
                                            const AfterglowMux::Environment& environment, QString* error,
                                            Ssh::Config* resolved = nullptr);

} // namespace Afterglow
