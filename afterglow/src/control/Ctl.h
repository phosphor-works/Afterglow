// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QtGlobal>

namespace Afterglow {

// The router refuses a request line over 1 MiB (IpcClient::MaxLineBytes).
// JSON can write a UTF-16 unit as six bytes (a control character): send-text
// goes in parts of this many units, which stay under the limit whatever they
// hold.
inline constexpr qsizetype CtlTextPart = 128 * 1024;
// A pane takes at most this much input that its program has not read yet
// (MaxPending in afterglow-pty's Pty.cpp). Longer text would be typed in part
// and then refused.
inline constexpr qsizetype CtlMaxText = 32 * 1024 * 1024;

/// `afterglow ctl ...`: drives a running afterglow through its control socket.
int runCtl(int argc, char** argv);

} // namespace Afterglow
