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

namespace {

/// The device of the file at @p path as /proc/locks names it, "MAJ:MIN:" in
/// hexadecimal: the device of the file system's superblock. stat() gives
/// another one on a btrfs subvolume (an anonymous device per subvolume),
/// so the device comes from the mount of the file in /proc/self/mountinfo,
/// whose third field is the superblock's, in decimal.
std::string lockDevice(const std::string& path, const struct stat& file)
{
    unsigned int major = ::major(file.st_dev);
    unsigned int minor = ::minor(file.st_dev);
#ifdef STATX_MNT_ID
    struct statx mount{};
    if (::statx(AT_FDCWD, path.c_str(), AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &mount) == 0
        && (mount.stx_mask & STATX_MNT_ID) != 0) {
        std::ifstream mounts("/proc/self/mountinfo");
        for (std::string line; std::getline(mounts, line);) {
            // "26 1 0:32 /@home /home rw,relatime ...": the mount ID, its
            // parent's, then the device.
            std::istringstream fields(line);
            unsigned long long id = 0, parent = 0;
            unsigned int devMajor = 0, devMinor = 0;
            char colon = 0;
            if (fields >> id >> parent >> devMajor >> colon >> devMinor && colon == ':' && id == mount.stx_mnt_id) {
                major = devMajor;
                minor = devMinor;
                break;
            }
        }
    }
#endif
    char device[32];
    std::snprintf(device, sizeof device, "%02x:%02x:", major, minor);
    return device;
}

} // namespace

bool lockFileHeld(const std::string& path)
{
    struct stat file{};
    if (::lstat(path.c_str(), &file) != 0) return false;
    std::ifstream locks("/proc/locks");
    if (!locks) return true;
    const std::string where = lockDevice(path, file) + std::to_string(file.st_ino);
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
