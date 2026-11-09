// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Lock.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace AfterglowProto {

namespace {

/// Whether @p path names the file of @p fd. lstat: the path itself, never
/// the target of a link that someone put there after the open.
bool names(const std::string& path, int fd)
{
    struct stat held{};
    struct stat current{};
    return ::fstat(fd, &held) == 0 && ::lstat(path.c_str(), &current) == 0 && held.st_dev == current.st_dev
        && held.st_ino == current.st_ino;
}

} // namespace

int lockFile(const std::string& path, std::string* error)
{
    for (int attempt = 0; attempt < 8; ++attempt) {
        const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) {
            if (error) *error = "cannot open " + path + ": " + std::strerror(errno);
            return LockFailed;
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            const int failure = errno;
            ::close(fd);
            // Only EWOULDBLOCK says that another holder has it. Another
            // error (no lock left, a file system without locks) is no
            // holder: a caller that took it for one would hand its work to
            // a server that is not there.
            if (failure == EWOULDBLOCK) return LockHeld;
            if (error) *error = "cannot lock " + path + ": " + std::strerror(failure);
            return LockFailed;
        }
        if (names(path, fd)) return fd;
        ::close(fd);
    }
    // Each lock went to a file that its holder removed at once: holders
    // come and go, so one holds it now.
    return LockHeld;
}

void unlockFile(const std::string& path, int fd)
{
    if (names(path, fd)) ::unlink(path.c_str());
    ::close(fd);
}

bool lockFileHeld(const std::string& path)
{
    struct stat file{};
    if (::lstat(path.c_str(), &file) != 0) return false;
    std::ifstream locks("/proc/locks");
    if (!locks) return true;
    char device[32];
    std::snprintf(device, sizeof device, "%02x:%02x:", ::major(file.st_dev), ::minor(file.st_dev));
    const std::string where = device + std::to_string(file.st_ino);
    for (std::string line; std::getline(locks, line);) {
        // "1: FLOCK  ADVISORY  WRITE 1234 00:49:9071 0 EOF". A waiter has
        // "->" after the number.
        std::istringstream fields(line);
        std::string number, type, mode, access, pid, place;
        fields >> number >> type;
        if (type == "->") fields >> type;
        if (type == "FLOCK" && fields >> mode >> access >> pid >> place && place == where) return true;
    }
    return false;
}

} // namespace AfterglowProto
