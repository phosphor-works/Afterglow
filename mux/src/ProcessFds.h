// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cerrno>
#include <climits>
#include <csignal>
#include <string>

#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/signalfd.h>
#include <sys/syscall.h>
#elif defined(__FreeBSD__)
#include <sys/event.h>
#include <sys/procdesc.h>
#include <sys/sysctl.h>
#include <time.h>
#else
#error "aglow-mux needs Linux or FreeBSD"
#endif

/// Descriptors that poll() shows ready for the end of a process or for a
/// signal: pidfds and a signalfd on Linux, process descriptors and kqueues on
/// FreeBSD. Each is close-on-exec.
namespace AfterglowMux {

#if defined(__FreeBSD__)
namespace detail {
inline int closeKeepingErrno(int fd)
{
    const int saved = errno;
    ::close(fd);
    errno = saved;
    return -1;
}
} // namespace detail
#endif

/// The path that runs this program again: on Linux /proc/self/exe, which
/// stays this binary when its file is replaced; on FreeBSD the kernel's path
/// of it (no /proc there). Empty when the kernel does not tell.
inline std::string selfExecutable()
{
#if defined(__linux__)
    return "/proc/self/exe";
#else
    char path[PATH_MAX];
    int name[] = {CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, -1};
    std::size_t size = sizeof path;
    if (::sysctl(name, 4, path, &size, nullptr, 0) != 0 || size <= 1)
        return {};
    return std::string(path, size - 1);
#endif
}

/// A descriptor that poll() shows readable once process @p pid ended (any
/// process of the user, not only a child): a pidfd on Linux, a kqueue that
/// waits for its exit on FreeBSD. -1 and errno when there is no such process,
/// or the kernel has no pidfd_open() (Linux before 5.3).
inline int watchProcess(pid_t pid)
{
#if defined(__linux__)
    // Through syscall(2): <sys/pidfd.h> of some glibc versions declares it
    // without C linkage.
    return int(::syscall(SYS_pidfd_open, pid, 0));
#else
    const int queue = ::kqueuex(KQUEUE_CLOEXEC);
    if (queue < 0)
        return -1;
    struct kevent change{};
    EV_SET(&change, uintptr_t(pid), EVFILT_PROC, EV_ADD, NOTE_EXIT, 0, nullptr);
    if (::kevent(queue, &change, 1, nullptr, 0, nullptr) != 0)
        return detail::closeKeepingErrno(queue);
    return queue;
#endif
}

/// fork(), with a descriptor of the child in @p descriptor (-1 in the child,
/// and when there is none): poll() shows it ready once the child ended,
/// killProcess() kills the child through it, and SCM_RIGHTS can pass it to
/// another process. A pidfd on Linux, a process descriptor of pdfork() on
/// FreeBSD. There the child sends no SIGCHLD, and its end SIGKILLs it when
/// the last copy of the descriptor closes. waitpid() of its pid reaps it.
inline pid_t forkWithDescriptor(int& descriptor)
{
    descriptor = -1;
#if defined(__linux__)
    const pid_t pid = ::fork();
    if (pid > 0)
        descriptor = watchProcess(pid);
    return pid;
#else
    int fd = -1;
    const pid_t pid = ::pdfork(&fd, PD_CLOEXEC);
    if (pid > 0)
        descriptor = fd;
    return pid;
#endif
}

/// Kills the process of @p descriptor, of forkWithDescriptor(). Its pid may
/// be another process's already: the descriptor names only that child.
inline void killProcess(int descriptor)
{
#if defined(__linux__)
    ::syscall(SYS_pidfd_send_signal, descriptor, SIGKILL, nullptr, 0);
#else
    ::pdkill(descriptor, SIGKILL);
#endif
}

/// A descriptor that poll() shows readable when one of @p signals arrives.
/// The caller blocks them first, so that they arrive only here: a signalfd
/// on Linux, a kqueue on FreeBSD (which sees a signal that is blocked too).
/// -1 and errno when it cannot.
inline int openSignalFd(const sigset_t& signals)
{
#if defined(__linux__)
    return ::signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC);
#else
    const int queue = ::kqueuex(KQUEUE_CLOEXEC);
    if (queue < 0)
        return -1;
    for (int number = 1; number <= SIGRTMAX; ++number) {
        if (::sigismember(&signals, number) != 1)
            continue;
        struct kevent change{};
        EV_SET(&change, uintptr_t(number), EVFILT_SIGNAL, EV_ADD, 0, 0, nullptr);
        if (::kevent(queue, &change, 1, nullptr, 0, nullptr) != 0)
            return detail::closeKeepingErrno(queue);
    }
    return queue;
#endif
}

/// Reads every signal that arrived at @p fd, of openSignalFd(), without
/// waiting. The number of one of them, or 0 when none came.
inline int takeSignals(int fd)
{
    int signal = 0;
#if defined(__linux__)
    signalfd_siginfo info{};
    while (::read(fd, &info, sizeof info) == ssize_t(sizeof info)) {
        if (signal == 0)
            signal = int(info.ssi_signo);
    }
#else
    struct kevent events[8];
    const timespec now{};
    for (;;) {
        const int count = ::kevent(fd, nullptr, 0, events, 8, &now);
        if (count <= 0)
            break;
        if (signal == 0)
            signal = int(events[0].ident);
        if (count < 8)
            break;
    }
#endif
    return signal;
}

} // namespace AfterglowMux
