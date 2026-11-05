// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "shell/Aglow.h"

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace Afterglow::Aglow {

bool handles(const char* command)
{
    for (const char* name : { "transfer", "dnd", "icat", "clipboard" }) {
        if (std::strcmp(command, name) == 0) {
            return true;
        }
    }
    return false;
}

int run(int argc, char** argv)
{
    // The aglow of the same build or package: never another one on PATH,
    // which could be another program of that name.
    char self[PATH_MAX];
    const ssize_t length = ::readlink("/proc/self/exe", self, sizeof self - 1);
    if (length <= 0) {
        std::fprintf(stderr, "afterglow: cannot find its own executable: %s\n", std::strerror(errno));
        return 1;
    }
    std::string path(self, std::size_t(length));
    path = path.substr(0, path.rfind('/') + 1) + "aglow";
    std::vector<char*> args { const_cast<char*>("aglow") };
    for (int i = 1; i < argc; ++i) {
        args.push_back(argv[i]);
    }
    args.push_back(nullptr);
    ::execv(path.c_str(), args.data());
    std::fprintf(stderr, "afterglow %s: cannot run %s: %s\n", argv[1], path.c_str(), std::strerror(errno));
    return 127;
}

} // namespace Afterglow::Aglow
