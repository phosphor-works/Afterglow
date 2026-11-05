// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// aglow: a small afterglow. Kitty's file transfer, drag and drop, graphics
// and clipboard protocols from the command line, in one static binary that
// needs neither kitty nor Afterglow on the computer where it runs.

#include "Commands.h"
#include "Tty.h"

#include <AfterglowProto/Version.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <unistd.h>

#ifndef AGLOW_VERSION
#define AGLOW_VERSION "0.0.0"
#endif

namespace Aglow {

std::string homeDirectory()
{
    const char* home = std::getenv("HOME");
    return home && *home == '/' ? std::string(home) : std::string("/");
}

std::string currentDirectory()
{
    char buffer[PATH_MAX];
    if (::getcwd(buffer, sizeof buffer)) {
        return buffer;
    }
    const char* pwd = std::getenv("PWD");
    return pwd && *pwd == '/' ? std::string(pwd) : homeDirectory();
}

void report(std::string_view command, std::string_view message)
{
    // Messages hold file names and what the terminal sent: never escape codes.
    // printable() keeps no line break: a message of several lines keeps them.
    std::string lines;
    for (std::size_t start = 0; start <= message.size();) {
        const auto end = std::min(message.find('\n', start), message.size());
        lines += (start ? "\n  " : "") + printable(message.substr(start, end - start));
        start = end + 1;
    }
    std::fprintf(stderr, "aglow %s: %s\n", printable(command).c_str(), lines.c_str());
}

std::string humanSize(std::uint64_t bytes)
{
    static const char* units[] = { "B", "KiB", "MiB", "GiB", "TiB" };
    double value = double(bytes);
    int unit = 0;
    while (value >= 1024 && unit < 4) {
        value /= 1024;
        ++unit;
    }
    char text[32];
    std::snprintf(text, sizeof text, unit ? "%.1f %s" : "%.0f %s", value, units[unit]);
    return text;
}

bool readAll(int fd, std::string& out)
{
    char buffer[65536];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            return false;
        }
        if (n == 0) {
            return true;
        }
        out.append(buffer, std::size_t(n));
    }
}

} // namespace Aglow

namespace {

const char* usage = "Usage: aglow COMMAND [options] [arguments]\n"
                    "\n"
                    "Kitty's terminal protocols from the command line. They work in Afterglow, kitty and the other\n"
                    "terminals that support them, also over SSH.\n"
                    "\n"
                    "Commands:\n"
                    "  transfer   Copy files to and from the computer of the terminal\n"
                    "  dnd        Drag files and data out of the terminal, and drop them in\n"
                    "  icat       Show images in the terminal\n"
                    "  clipboard  Read and write the clipboard, with any MIME type\n"
                    "\n"
                    "Run aglow COMMAND --help for the options of a command.\n"
                    "  -h, --help     Show this help\n"
                    "  -v, --version  Show the version of aglow and of its protocols\n";

} // namespace

namespace {

int run(int argc, char** argv);

} // namespace

// Every command restores the terminal when it ends: the Tty that it opened
// does so as the stack unwinds. An exception that no handler catches would
// end the program without unwinding, so main catches them all.
int main(int argc, char** argv)
{
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        Aglow::report(argc > 1 ? argv[1] : "", std::string("internal error: ") + error.what());
    } catch (...) {
        Aglow::report(argc > 1 ? argv[1] : "", "internal error");
    }
    return 1;
}

namespace {

int run(int argc, char** argv)
{
    if (argc < 2) {
        std::fputs(usage, stderr);
        return 1;
    }
    const std::string_view command = argv[1];
    if (command == "--help" || command == "-h" || command == "help") {
        std::fputs(usage, stdout);
        return 0;
    }
    if (command == "--version" || command == "-v" || command == "version") {
        std::printf("aglow %s (protocol %d)\n", AGLOW_VERSION, AfterglowProto::ProtocolVersion);
        return 0;
    }
    if (command == "transfer") {
        return Aglow::runTransfer(argc - 2, argv + 2);
    }
    if (command == "dnd") {
        return Aglow::runDnd(argc - 2, argv + 2);
    }
    if (command == "icat") {
        return Aglow::runIcat(argc - 2, argv + 2);
    }
    if (command == "clipboard") {
        return Aglow::runClipboard(argc - 2, argv + 2);
    }
    std::fprintf(stderr, "aglow: unknown command: %s\n\n%s", Aglow::printable(argv[1]).c_str(), usage);
    return 1;
}

} // namespace
