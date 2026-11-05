// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

namespace AfterglowProto {

/// The version of the protocols that afterglow and aglow speak to each
/// other. aglow --version prints it. It is in this library so that afterglow
/// can read the same number when it starts to compare it with the one of an
/// aglow. Raise it when a change in this library breaks an older aglow or terminal.
inline constexpr int ProtocolVersion = 1;

} // namespace AfterglowProto
