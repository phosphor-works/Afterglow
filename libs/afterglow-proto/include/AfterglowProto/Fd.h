// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

/// File descriptor and process helpers for the clients of the library.
namespace AfterglowProto {

/// Writes all of @p bytes to @p fd: again after EINTR and short writes, and
/// after EAGAIN once poll(2) reports room, so that a descriptor that another
/// process made non-blocking still takes everything. False on any other
/// error, with errno set: EIO when write(2) wrote nothing and gave no error.
///
/// With @p wake at 0 or more, a readable @p wake ends the write: the call
/// returns false with errno EINTR, and writes nothing when @p wake was
/// readable before it. A signal ends it only through @p wake: its handler
/// writes to that self-pipe, and is installed without SA_RESTART so that a
/// blocking write(2) returns. A signal that leaves @p wake empty, or any
/// signal without @p wake, does not end the write. Nor does a signal that
/// comes between the check of @p wake and a write(2) that then blocks: that
/// write returns once the reader took all of its bytes, and the caller sees
/// the signal after it.
inline bool writeAll(int fd, std::string_view bytes, int wake = -1)
{
    const auto woken = [wake] {
        pollfd ready { wake, POLLIN, 0 };
        return wake >= 0 && ::poll(&ready, 1, 0) > 0;
    };
    while (!bytes.empty()) {
        // Before each write: a signal that came before a write that blocks
        // interrupts nothing, and a blocking write that a signal interrupts
        // after some bytes returns their count, not EINTR. Only a signal
        // between this check and the write's start goes unseen here.
        if (woken()) {
            errno = EINTR;
            return false;
        }
        const ssize_t n = ::write(fd, bytes.data(), bytes.size());
        if (n > 0) {
            bytes.remove_prefix(std::size_t(n));
            continue;
        }
        if (n == 0) {
            errno = EIO;
            return false;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            return false;
        }
        // The poll ends on room, an error or the wake: the next write or the
        // wake check tells which.
        pollfd fds[2] = { { fd, POLLOUT, 0 }, { wake, POLLIN, 0 } };
        if (::poll(fds, wake >= 0 ? 2 : 1, -1) < 0 && errno != EINTR) {
            return false;
        }
    }
    return true;
}

/// The working directory. When getcwd(3) gives no path, $PWD stands in if
/// it names the working directory: it may be stale, or set by anyone. That
/// is so for a directory above it that cannot be read (EACCES), and for a
/// path that takes more than 1 MiB with its NUL. Empty otherwise, as for a
/// deleted directory.
inline std::string currentDirectory()
{
    std::string path(4096, '\0');
    while (!::getcwd(path.data(), path.size())) {
        if (errno == ERANGE && path.size() < (1u << 20)) {
            path.resize(path.size() * 2);
            continue;
        }
        const char* pwd = std::getenv("PWD");
        struct stat named { };
        struct stat here { };
        if (pwd && *pwd == '/' && ::stat(pwd, &named) == 0 && ::stat(".", &here) == 0 && named.st_dev == here.st_dev
            && named.st_ino == here.st_ino) {
            return pwd;
        }
        return {};
    }
    path.resize(std::strlen(path.c_str()));
    return path;
}

/// Owns one file descriptor and closes it.
class UniqueFd
{
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd)
        : m_fd(fd)
    {
    }
    UniqueFd(UniqueFd&& other) noexcept
        : m_fd(std::exchange(other.m_fd, -1))
    {
    }
    UniqueFd& operator=(UniqueFd&& other) noexcept
    {
        if (this != &other) {
            reset(std::exchange(other.m_fd, -1));
        }
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;
    ~UniqueFd() { reset(); }

    int get() const { return m_fd; }
    bool valid() const { return m_fd >= 0; }
    /// Closes the descriptor that it held and takes @p fd. errno is kept.
    void reset(int fd = -1)
    {
        if (m_fd >= 0) {
            const int saved = errno;
            ::close(m_fd);
            errno = saved;
        }
        m_fd = fd;
    }
    int release() { return std::exchange(m_fd, -1); }

private:
    int m_fd = -1;
};

} // namespace AfterglowProto
