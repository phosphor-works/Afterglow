// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowProto/Clipboard.h>

namespace AfterglowVt {

// The message format is shared with clients. The terminal keeps the
// dispatch, and the host keeps the clipboard and its permission checks.
using AfterglowProto::ClipboardAssembler;
using AfterglowProto::ClipboardRequest;

} // namespace AfterglowVt
