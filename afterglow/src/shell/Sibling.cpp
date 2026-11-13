// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// Apart from the rest of Aglow.cpp, which only the executable builds: the
// application starts the aglow-mux next to it too (mux/MuxSessions.cpp).

#include "shell/Aglow.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace Afterglow::Aglow {

std::string siblingExecutable(const char* name, std::string* error)
{
    // read_symlink grows its buffer for a long path, where a fixed one would
    // cut it short.
    std::error_code failure;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", failure);
    if (failure || self.empty()) {
        *error = "cannot find its own executable: " + (failure ? failure.message() : std::string("the link is empty"));
        return {};
    }
    return (self.parent_path() / name).string();
}

} // namespace Afterglow::Aglow
