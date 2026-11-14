// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "mux/MuxClient.h"

#include <QJsonArray>
#include <QString>

namespace Afterglow {

class Control;
class TerminalWindow;
struct Settings;

/// Answers a call of `afterglow ctl` for the session that @p window shows,
/// which the mux daemon passes to the window (MuxClient::ControlHandler): the
/// calls that only a window can do, and the panes that the daemon started to
/// place. Its arguments and its result name panes by the daemon's IDs. The
/// answer goes to @p reply, at once, or later for a project that opens into
/// a session, an overlay and a screenshot. The overlays and the overlay
/// inputs of @p client, the daemon's number of the client, are its own.
void answerSessionCall(Control& control, const Settings& settings, TerminalWindow& window, const QString& fn, const QJsonArray& args,
                       qint64 client, MuxClient::ControlReply reply);

} // namespace Afterglow
