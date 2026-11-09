// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <string>

// The lock file that decides which server owns a socket path: IpcListener
// (afterglow-mux) and the terminal's InstanceSocket take the same lock, in
// the same way.

namespace AfterglowProto {

/// lockFile() while another holder has the lock: another process, or an
/// earlier lockFile() of this one. A flock belongs to an open file, not to
/// a process, so a second open in the same process does not get it.
inline constexpr int LockHeld = -1;
/// lockFile() when the file cannot be opened or locked.
inline constexpr int LockFailed = -2;

/// Takes an exclusive lock on the file at @p path, which it creates with
/// mode 0600 and never opens through a symbolic link. Returns the
/// descriptor that holds the lock (close-on-exec), LockHeld while another
/// holder has it, or LockFailed with @p error. A holder that lets go
/// removes the file (unlockFile()), so the lock is taken again on the file
/// that is there after the lock: one taken on a removed file guards nothing.
int lockFile(const std::string& path, std::string* error);

/// Whether a holder has the lock of the file at @p path, without taking it:
/// a probe that took it would make a holder that starts meanwhile fail with
/// LockHeld. A file that a holder left when it was killed, or when the
/// computer stopped, exists with no lock on it. Reads /proc/locks, which
/// names each flock by device and inode. Without /proc/locks, a file that
/// exists counts as held. /proc/locks shows only the locks of processes in
/// the pid namespace of the reader's /proc: from a sandbox with a pid
/// namespace of its own, a lock that a process outside holds does not show.
bool lockFileHeld(const std::string& path);

/// Lets go of a lock of lockFile(): removes the file at @p path when it is
/// still the one of @p fd, and closes @p fd. While the lock is held, no
/// other process takes the file that this removes.
void unlockFile(const std::string& path, int fd);

} // namespace AfterglowProto
