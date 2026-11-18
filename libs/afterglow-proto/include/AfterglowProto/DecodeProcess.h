// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

// The inline AfterglowPty/CloseFrom.h: a program that includes this header
// adds the include directory of libs/afterglow-pty, not a link.
#include <AfterglowProto/Limits.h>
#include <AfterglowPty/CloseFrom.h>

#include <csignal>

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <unistd.h>

/// The start of a process that decodes untrusted input, right after its
/// fork(): aglow's decoder child (tools/aglow/Image.cpp) and the decoding
/// processes of aglow-mux (mux/src/ImageServer.cpp). Each then sets its
/// limits of memory and CPU time, and AfterglowProto::allowOnlyDecoding().
namespace AfterglowProto {

/// Makes this process a decoding process of @p parent:
/// - it ends with @p parent, also when a signal kills the parent before it
///   can kill the process;
/// - no signal is blocked, the signals that a terminal or a parent sends to
///   stop a program (SIGINT, SIGTERM, SIGHUP, SIGQUIT) end it, SIGWINCH and
///   SIGCONT run no handler of the parent, and SIGPIPE ends it when the
///   parent stops reading. A signal that came ignored stays ignored;
/// - @p input is standard input (-1: /dev/null) and @p output standard
///   output. Standard error is /dev/null, except with @p keepStandardError
///   (a sanitizer build, for its reports) when neither @p input nor
///   @p output is descriptor 2: then it stays the parent's. Every other
///   descriptor closes: it holds none of the parent's (a terminal, a
///   socket);
/// - the kernel's OOM killer takes it before the parent;
/// - it writes no core file, and no other process of the user can trace it
///   or read its memory.
/// False when it cannot: the caller exits at once.
[[nodiscard]] inline bool startDecodingProcess(pid_t parent, int input, int output, bool keepStandardError)
{
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() != parent) return false;

    // aglow-mux's server blocks SIGCHLD for its signalfd: the signals of the
    // limits must arrive here.
    sigset_t none;
    ::sigemptyset(&none);
    ::sigprocmask(SIG_SETMASK, &none, nullptr);
    for (const int number : {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGWINCH, SIGCONT}) {
        struct sigaction inherited{};
        if (::sigaction(number, nullptr, &inherited) == 0 && inherited.sa_handler != SIG_IGN) std::signal(number, SIG_DFL);
    }
    std::signal(SIGPIPE, SIG_DFL);

    // Standard output first. A parent that started without its standard
    // descriptors can hold @p output or @p input in 0 to 2: an input there
    // moves above them before the output takes its place. Without exec(),
    // O_CLOEXEC closes nothing here. One in 2 is no standard error for a
    // sanitizer to write to: it becomes /dev/null too.
    const bool nullError = !keepStandardError || input == STDERR_FILENO || output == STDERR_FILENO;
    if (input > STDIN_FILENO && input <= STDERR_FILENO) {
        input = ::fcntl(input, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (input < 0) return false;
    }
    if (::dup2(output, STDOUT_FILENO) < 0) return false;
    if (input >= 0 && ::dup2(input, STDIN_FILENO) < 0) return false;
    if (input < 0 || nullError) {
        const int null = ::open("/dev/null", O_RDWR | O_CLOEXEC);
        if (null < 0) return false;
        if (input < 0 && ::dup2(null, STDIN_FILENO) < 0) return false;
        if (nullError && ::dup2(null, STDERR_FILENO) < 0) return false;
    }
    AfterglowPty::closeFrom(STDERR_FILENO + 1);

    // Before PR_SET_DUMPABLE, which gives /proc/self to root.
    if (const int adjust = ::open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC); adjust >= 0) {
        (void)!::write(adjust, "1000", 4);
        ::close(adjust);
    }
    // No core file, also where systemd-coredump takes cores through a pipe,
    // which RLIMIT_CORE does not stop.
    limit(RLIMIT_CORE, 0, 0);
    ::prctl(PR_SET_DUMPABLE, 0);
    return true;
}

} // namespace AfterglowProto
