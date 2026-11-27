// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

// aglow: a small afterglow, as one static binary. From the command line it
// speaks kitty's file transfer, drag and drop, graphics and clipboard
// protocols, which need neither kitty nor Afterglow on the computer where it
// runs. It also runs the commands of the scripts of an Afterglow (aglow
// run): on the same computer, or on the computer that afterglow ssh came
// from.

#include "Cli.h"
#include "Commands.h"
#include "Tty.h"

#include <AfterglowProto/Version.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

extern char** environ;

#ifndef AGLOW_VERSION
#define AGLOW_VERSION "0.0.0"
#endif

namespace Aglow {

std::string homeDirectory()
{
    const char* home = std::getenv("HOME");
    return home && *home == '/' ? std::string(home) : std::string("/");
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
    std::fprintf(stderr, "%s %s: %s\n", Cli::program().c_str(), printable(command).c_str(), lines.c_str());
}

std::string humanSize(std::uint64_t bytes)
{
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = double(bytes);
    int unit = 0;
    // 1023.96 KiB prints as 1.0 MiB, not as 1024.0 KiB.
    while (unit < 4 && (unit ? value >= 1023.95 : value >= 1024)) {
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

TmuxRun runTmux(std::initializer_list<const char*> args, long long deadline)
{
    TmuxRun run;
    int out[2];
    if (::pipe2(out, O_CLOEXEC | O_NONBLOCK) != 0) {
        return run;
    }
    std::vector<char*> argv{const_cast<char*>("tmux")};
    for (const char* arg : args) {
        argv.push_back(const_cast<char*>(arg));
    }
    argv.push_back(nullptr);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid = 0;
    run.started = ::posix_spawnp(&pid, "tmux", &actions, nullptr, argv.data(), environ) == 0;
    posix_spawn_file_actions_destroy(&actions);
    ::close(out[1]);
    // The end of tmux's output: tmux exits there.
    bool ended = false;
    while (run.started && !ended) {
        pollfd ready{out[0], POLLIN, 0};
        const int polled = ::poll(&ready, 1, int(std::max(0LL, deadline - nowMs())));
        if (polled < 0 && errno == EINTR) {
            continue;
        }
        if (polled <= 0) {
            break;
        }
        char bytes[256];
        const ssize_t n = ::read(out[0], bytes, sizeof bytes);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }
        if (n <= 0) {
            ended = true;
        } else {
            run.output.append(bytes, std::size_t(n));
        }
    }
    ::close(out[0]);
    if (!run.started) {
        return run;
    }
    // A tmux that did not answer in time ends now, so that the wait for it
    // is short.
    run.late = !ended;
    if (run.late) {
        ::kill(pid, SIGKILL);
    }
    while (::waitpid(pid, &run.status, 0) < 0 && errno == EINTR) { }
    return run;
}

} // namespace Aglow

namespace {

// %1 stands for the program name. afterglow --help shows the summaries of
// the commands too (Afterglow::Aglow::Commands), and a test checks that
// they agree.
const char* usageText =
    "Usage: %1 COMMAND [options] [arguments]\n"
    "\n"
    "Kitty's terminal protocols from the command line. They work in Afterglow, kitty and the\n"
    "other terminals that support them, also over SSH.\n"
    "The run command talks to an Afterglow on the same computer, or on the computer that\n"
    "afterglow ssh came from.\n"
    "\n"
    "Commands:\n"
    "  transfer   Copy files to and from the computer of the terminal\n"
    "  dnd        Drag files and data out of the terminal, and drop them in\n"
    "  icat       Show images in the terminal\n"
    "  clipboard  Read and write the clipboard, with any MIME type\n"
    "  run        Run a command of a script in a running Afterglow\n"
    "\n"
    "Run %1 COMMAND --help or %1 help COMMAND for the options of a command.\n"
    "afterglow ssh runs %1 remote-prune DIR on remote hosts: it removes the sockets of ssh\n"
    "sessions that ended.\n"
    "\n"
    "Options:\n"
    "  -h, --help     Show this help\n"
    "  -v, --version  Show the version of aglow and of its protocol\n";

std::string usage()
{
    const std::string_view text = usageText;
    std::string out;
    for (std::size_t at = 0; at < text.size();) {
        const auto mark = std::min(text.find("%1", at), text.size());
        out += text.substr(at, mark - at);
        if (mark < text.size()) {
            out += Aglow::Cli::program();
        }
        at = mark + 2;
    }
    return out;
}

} // namespace

namespace {

int run(int argc, char** argv);

} // namespace

// Every command restores the terminal when it ends: the Tty that it opened
// does so as the stack unwinds. An exception that no handler catches would
// end the program without unwinding, so main catches them all.
int main(int argc, char** argv)
{
    // A parent that ignores SIGCHLD passes that on through exec, and then the
    // kernel reaps the children of aglow (its decoder, tmux) before waitpid()
    // can read how they ended.
    std::signal(SIGCHLD, SIG_DFL);
    Aglow::Cli::setProgram(argc > 0 && argv[0] ? argv[0] : "");
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
    // No command and an unknown command are usage errors: status 2, as in
    // run and remote-prune. The other commands give 1, as kitten does.
    if (argc < 2) {
        std::fputs(usage().c_str(), stderr);
        return 2;
    }
    const std::string_view command = argv[1];
    if (command == "help" && argc > 2) {
        // aglow help COMMAND is aglow COMMAND --help.
        static char helpOption[] = "--help";
        char* helpArgs[] = {argv[0], argv[2], helpOption, nullptr};
        return run(3, helpArgs);
    }
    if (command == "--help" || command == "-h" || command == "help") {
        std::fputs(usage().c_str(), stdout);
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
    if (command == "run") {
        return Aglow::runRun(argc - 2, argv + 2);
    }
    if (command == "remote-prune") {
        return Aglow::runRemotePrune(argc - 2, argv + 2);
    }
    std::fprintf(stderr, "%s: unknown command: %s\n\n%s", Aglow::Cli::program().c_str(),
                 Aglow::printable(argv[1]).c_str(), usage().c_str());
    return 2;
}

} // namespace
