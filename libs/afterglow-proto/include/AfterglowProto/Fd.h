// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cerrno>
#include <cstddef>
#include <string_view>
#include <utility>

#include <poll.h>
#include <unistd.h>

/// File descriptor helpers for the clients of the library.
namespace AfterglowProto {

/// Writes all of @p bytes to @p fd: again after EINTR and short writes, and
/// after EAGAIN once poll(2) reports room, so that a descriptor that another
/// process made non-blocking still takes everything. False on any other
/// error, with errno set: EIO when write(2) wrote nothing and gave no error.
///
/// With @p wake at 0 or more, a signal or a readable @p wake ends the wait:
/// the call returns false with errno EINTR, so that a caller whose signal
/// handler writes to a self-pipe can act on the signal. Without @p wake, a
/// signal does not end the write.
inline bool writeAll(int fd, std::string_view bytes, int wake = -1)
{
    const auto woken = [wake] {
        pollfd ready { wake, POLLIN, 0 };
        return wake >= 0 && ::poll(&ready, 1, 0) > 0;
    };
    while (!bytes.empty()) {
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
            if (woken()) {
                errno = EINTR;
                return false;
            }
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
        if (woken()) {
            errno = EINTR;
            return false;
        }
    }
    return true;
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
