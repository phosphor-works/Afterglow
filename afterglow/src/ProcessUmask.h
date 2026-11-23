// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <sys/types.h>

#include <exception>
#include <fstream>
#include <optional>
#include <string>

namespace Afterglow {

/// The file mode creation mask of this process, read without changing it
/// (umask() would set one to read it), or nothing when /proc does not say:
/// the caller takes its own default.
inline std::optional<mode_t> currentUmask()
{
    std::ifstream status("/proc/self/status");
    for (std::string line; std::getline(status, line);) {
        if (!line.starts_with("Umask:"))
            continue;
        try {
            return mode_t(std::stoul(line.substr(6), nullptr, 8) & 0777);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

} // namespace Afterglow
