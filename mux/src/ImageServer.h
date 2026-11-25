// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <string_view>

namespace AfterglowMux {

/// The server of ImageDecoder (aglow-mux --decode-images): forks a decoding
/// process for each request on standard input, until the client @p parent
/// closes it. A @p version other than DecodeWireVersion ends it at once,
/// with ImageWire.h's OtherVersionStatus. Returns the exit status.
int runImageServer(std::string_view parent, std::string_view version);

} // namespace AfterglowMux
