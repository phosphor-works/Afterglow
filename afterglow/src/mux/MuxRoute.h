// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "MuxConnect.h"

#include <QString>

#include <optional>

namespace Afterglow {

/// The way to the mux daemon of @p host: `afterglow-mux proxy` there, which
/// ssh runs over the connection that `afterglow ssh` shares with the host.
/// Nothing, with @p error, when ssh cannot read the destination.
std::optional<AfterglowMux::Route> muxRoute(const QString& host, QString* error);

} // namespace Afterglow
