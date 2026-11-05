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
    for (const char* name : Commands) {
        if (std::strcmp(command, name) == 0) {
            return true;
        }
    }
    return false;
}

int run(int argc, char** argv)
{
    // 127, as a shell answers for a command that it cannot find.
    if (!built()) {
        std::fprintf(stderr, "afterglow %s: this build of Afterglow has no aglow (AFTERGLOW_BUILD_AGLOW=OFF)\n", argv[1]);
        return 127;
    }
    // The aglow of the same build or package: never another one on PATH,
    // which could be another program of that name. read_symlink grows its
    // buffer for a long path, where a fixed one would cut it short.
    std::error_code error;
    const auto self = std::filesystem::read_symlink("/proc/self/exe", error);
    if (error || self.empty()) {
        std::fprintf(stderr, "afterglow %s: cannot find its own executable: %s\n", argv[1],
                     error ? error.message().c_str() : "the link is empty");
        return 127;
    }
    const std::string path = (self.parent_path() / "aglow").string();
    // argv[0] is the program that the user typed, for aglow's usage and
    // messages: "afterglow icat", not "aglow icat".
    std::vector<char*> args { const_cast<char*>("afterglow") };
    for (int i = 1; i < argc; ++i) {
        args.push_back(argv[i]);
    }
    args.push_back(nullptr);
    ::execv(path.c_str(), args.data());
    std::fprintf(stderr, "afterglow %s: cannot run %s: %s\n", argv[1], path.c_str(), std::strerror(errno));
    return 127;
}

} // namespace Afterglow::Aglow
