// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// afterglow-mux: the daemon that keeps terminal sessions running while no
// window shows them. It has no Qt, no display and no GPU. `afterglow mux`
// starts it, in a systemd user unit or in a session of its own.

#include "Daemon.h"
#include "MuxProtocol.h"

#include <AfterglowProto/Socket.h>

#include <cstdio>
#include <cstring>

namespace {

const char* const Usage = R"(Usage: afterglow-mux [--version | --help]

Serves the terminal sessions of `afterglow mux` on
$XDG_RUNTIME_DIR/afterglow-mux.sock, in the foreground. It ends when no
session is left, or on SIGTERM, SIGINT or SIGHUP, which also end the sessions.
)";

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("afterglow-mux %s (protocol %d)\n", AFTERGLOW_VERSION, AfterglowMux::ProtocolVersion);
        return 0;
    }
    if (argc == 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        std::fputs(Usage, stdout);
        return 0;
    }
    if (argc != 1) {
        std::fputs(Usage, stderr);
        return 2;
    }
    const AfterglowProto::MuxSocket socket = AfterglowProto::muxSocket();
    if (socket.path.empty()) {
        std::fprintf(stderr, "afterglow-mux: %s\n", socket.error.c_str());
        return 1;
    }
    AfterglowMux::Daemon daemon;
    std::string error;
    if (!daemon.listen(socket.path, socket.directory, &error)) {
        std::fprintf(stderr, "afterglow-mux: %s\n", error.c_str());
        return 1;
    }
    return daemon.run();
}
