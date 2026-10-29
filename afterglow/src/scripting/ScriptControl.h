// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "ScriptHost.h"

namespace Afterglow {

class Control;

/// Pane lookups and effects, carried out through @p control.
ScriptHost::Callbacks scriptCallbacks(Control& control);
/// Passes @p control's events on to the scripts' handlers. Queued, so an
/// effect that causes an event never runs a handler inside another.
void connectScriptEvents(ScriptHost& host, Control& control);

} // namespace Afterglow
