// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// afterglow-mux: the daemon that keeps terminal sessions running while no
// window shows them. It has no Qt, no display and no GPU. `afterglow mux`
// starts it, in a systemd user unit or in a session of its own.

#include "Daemon.h"
#include "MuxProtocol.h"

#include <AfterglowProto/Socket.h>

#include <charconv>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <string_view>

#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {

const char* const Usage = R"(Usage: afterglow-mux [--version | --help]

Serves the terminal sessions of `afterglow mux`, in the foreground, on
$XDG_RUNTIME_DIR/afterglow-mux/socket and on
$XDG_STATE_HOME/afterglow-mux/ID.sock (by default under ~/.local/state).
ID is a hash of /etc/machine-id, or else the host name.
It ends when no session is left, and after %d seconds when no session
starts. SIGTERM, SIGINT and SIGHUP end it and its sessions.
)";

int g_endSignal = 0;

/// Ends the process with the signal that ended the daemon, so that its
/// parent (systemd, a shell) sees the signal. Registered before anything of
/// static lifetime is made, it runs after their destructors: the PTY's
/// reaper first kills the programs that outlived their hangup.
void endWithSignal()
{
    if (g_endSignal == 0) return;
    std::signal(g_endSignal, SIG_DFL);
    sigset_t set;
    ::sigemptyset(&set);
    ::sigaddset(&set, g_endSignal);
    // The daemon blocked it for its signalfd.
    ::pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
    std::raise(g_endSignal);
}

/// Each session and connection takes descriptors, and the soft limit is
/// often 1024: the daemon takes the hard limit, which needs no privilege.
/// Returns the soft limit from before, for the programs of the sessions
/// (a program that uses select() breaks past 1024), or -1 when the daemon
/// did not change it.
long raiseFileLimit()
{
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0 || limit.rlim_cur >= limit.rlim_max) return -1;
    const rlim_t soft = limit.rlim_cur;
    limit.rlim_cur = limit.rlim_max;
    if (::setrlimit(RLIMIT_NOFILE, &limit) != 0) {
        std::perror("afterglow-mux: cannot raise the descriptor limit");
        return -1;
    }
    return long(soft);
}

/// The descriptor of AfterglowMux::ReportFdVariable: `afterglow mux new`
/// reads there whether a daemon that it started in a session of its own
/// listens, or why it cannot start, as that daemon's standard error is
/// /dev/null. The variable goes, and the descriptor does not reach the
/// programs of the sessions.
int takeReportFd()
{
    const char* text = std::getenv(AfterglowMux::ReportFdVariable);
    if (!text) return -1;
    int fd = -1;
    const std::string_view value(text);
    const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), fd);
    ::unsetenv(AfterglowMux::ReportFdVariable);
    if (ec != std::errc() || end != value.data() + value.size() || fd < 3 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) != 0) return -1;
    return fd;
}

/// Writes why the daemon cannot start on stderr and to the client that
/// waits on @p reportFd, after AfterglowMux::ReportHeld when another daemon
/// holds a socket (@p held).
int startFailed(int reportFd, const std::string& message, bool held = false)
{
    const std::string line = "afterglow-mux: " + message + "\n";
    std::fputs(line.c_str(), stderr);
    if (reportFd >= 0) {
        const std::string report = held ? AfterglowMux::ReportHeld + line : line;
        (void)!::write(reportFd, report.data(), report.size());
        ::close(reportFd);
    }
    return 1;
}

} // namespace

int main(int argc, char** argv)
{
    std::atexit(endWithSignal);
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::printf("afterglow-mux %s (protocol %d)\n", AFTERGLOW_VERSION, AfterglowMux::ProtocolVersion);
        return 0;
    }
    const int startWait = AfterglowMux::Daemon::StartWaitMs / 1000;
    if (argc == 2 && (std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0)) {
        std::printf(Usage, startWait);
        return 0;
    }
    if (argc != 1) {
        std::fprintf(stderr, Usage, startWait);
        return 2;
    }
    int reportFd = takeReportFd();
    const AfterglowProto::MuxSockets sockets = AfterglowProto::muxSockets();
    if (sockets.candidates.empty()) return startFailed(reportFd, sockets.error);
    const long fileLimit = raiseFileLimit();
    try {
        AfterglowMux::Daemon daemon(fileLimit);
        std::string error;
        if (const auto listened = daemon.listen(sockets.candidates, &error); listened != AfterglowMux::Daemon::Listen::Listening) {
            return startFailed(reportFd, error, listened == AfterglowMux::Daemon::Listen::Held);
        }
        // It listens. Not the end of the pipe alone: a daemon that a signal
        // ends before it listens ends the pipe too.
        if (reportFd >= 0) {
            (void)!::write(reportFd, &AfterglowMux::ReportListening, 1);
            ::close(reportFd);
        }
        reportFd = -1;
        const AfterglowMux::Daemon::Exit exit = daemon.run();
        g_endSignal = exit.signal;
        return exit.status;
    } catch (const std::exception& e) {
        // The daemon's destructor ran: its programs got their hangup.
        if (reportFd >= 0) return startFailed(reportFd, e.what());
        std::fprintf(stderr, "afterglow-mux: %s\n", e.what());
        return 1;
    }
}
