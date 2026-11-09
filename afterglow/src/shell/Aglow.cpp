// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shell/Aglow.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <unistd.h>
#include <vector>

namespace Afterglow::Aglow {

bool built()
{
#ifdef AFTERGLOW_HAS_AGLOW
    return true;
#else
    return false;
#endif
}

bool handles(const char* command)
{
    for (const Command& known : Commands) {
        if (std::strcmp(command, known.name) == 0) {
            return true;
        }
    }
    return false;
}

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

int run(int argc, char** argv)
{
    // 127, as a shell answers for a command that it cannot find.
    if (!built()) {
        std::fprintf(stderr, "afterglow %s: this build of Afterglow has no aglow (AFTERGLOW_BUILD_AGLOW=OFF)\n", argv[1]);
        return 127;
    }
    std::string error;
    const std::string path = siblingExecutable("aglow", &error);
    if (path.empty()) {
        std::fprintf(stderr, "afterglow %s: %s\n", argv[1], error.c_str());
        return 127;
    }
    // argv[0] names the program in aglow's usage and messages: "afterglow
    // icat", not "aglow icat". It is "afterglow" whatever the name that
    // started this executable, as the commands in afterglow --help say.
    std::vector<char*> args { const_cast<char*>("afterglow") };
    for (int i = 1; i < argc; ++i) {
        args.push_back(argv[i]);
    }
    args.push_back(nullptr);
    ::execv(path.c_str(), args.data());
    const int failure = errno;
    std::fprintf(stderr, "afterglow %s: cannot run %s: %s\n", argv[1], path.c_str(), std::strerror(failure));
    // As a shell answers: 127 when there is no aglow, 126 when there is one
    // that cannot run (no permission, not an executable).
    return failure == ENOENT || failure == ENOTDIR ? 127 : 126;
}

} // namespace Afterglow::Aglow
