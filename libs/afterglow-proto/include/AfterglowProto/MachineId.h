// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <fstream>
#include <iterator>
#include <string>

namespace AfterglowProto {

/// The contents of /etc/machine-id, which the callers hash before they use
/// them (machine-id(5) keeps the ID confidential). Empty when the file
/// cannot be read.
inline std::string machineIdContents()
{
    std::ifstream file("/etc/machine-id", std::ios::binary);
    return file ? std::string{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()} : std::string();
}

} // namespace AfterglowProto
