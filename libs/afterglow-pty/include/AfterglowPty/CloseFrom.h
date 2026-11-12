// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <climits>

#include <dirent.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

// For a child between fork() and exec(), in a process with threads: only
// system calls, no allocation and no lock.
namespace AfterglowPty {

/// Closes the descriptors from @p first to @p last.
inline void closeRange(int first, int last)
{
    if (first > last) return;
    if (::syscall(SYS_close_range, unsigned(first), unsigned(last), 0U) == 0) return;
    for (int fd = first; fd <= last; ++fd) ::close(fd);
}

/// Closes the descriptors from @p first up that /proc/self/fd lists. False
/// when it cannot read that list.
inline bool closeListed(int first)
{
    const int directory = ::open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return false;
    alignas(dirent64) char buffer[4096];
    for (;;) {
        const long read = ::syscall(SYS_getdents64, directory, buffer, sizeof buffer);
        if (read < 0) {
            // A filter of system calls, say: the loop below closes them.
            ::close(directory);
            return false;
        }
        if (read == 0) break;
        bool closed = false;
        for (long at = 0; at < read;) {
            const auto* entry = reinterpret_cast<const dirent64*>(buffer + at);
            at += entry->d_reclen;
            int fd = 0;
            bool number = entry->d_name[0] != '\0';
            for (const char* c = entry->d_name; *c && number; ++c) {
                number = *c >= '0' && *c <= '9' && fd <= (INT_MAX - 9) / 10;
                if (number) fd = fd * 10 + (*c - '0');
            }
            if (number && fd >= first && fd != directory) {
                ::close(fd);
                closed = true;
            }
        }
        // The kernel places each entry by its number, so a close moves no
        // other: reading again from the start is only a safeguard, and
        // finds the closed ones gone.
        if (closed) ::lseek(directory, 0, SEEK_SET);
    }
    ::close(directory);
    return true;
}

/// Closes every descriptor from @p first up. close_range() came with Linux
/// 5.9. Before it, the descriptors that /proc/self/fd lists, and without
/// /proc each one up to the soft limit of open files, or without one up to
/// the kernel's default fs.nr_open: a limit can be a million, too many to
/// try one by one where /proc lists them.
inline void closeFrom(int first)
{
    if (::syscall(SYS_close_range, unsigned(first), ~0U, 0U) == 0) return;
    if (closeListed(first)) return;
    rlimit files{};
    const rlim_t last = ::getrlimit(RLIMIT_NOFILE, &files) == 0 && files.rlim_cur != RLIM_INFINITY ? files.rlim_cur : rlim_t(1) << 20;
    for (rlim_t fd = rlim_t(first); fd < last; ++fd) ::close(int(fd));
}

} // namespace AfterglowPty
