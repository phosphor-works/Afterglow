// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/FdDelta.h>

#include <algorithm>
#include <cerrno>
#include <unistd.h>

namespace AfterglowProto {

FdSource::FdSource(int fd, std::uint64_t limit)
    : m_fd(fd)
    , m_limit(limit)
{
}

bool FdSource::read(std::string& out, std::size_t max)
{
    const auto want = std::size_t(std::min<std::uint64_t>(max, m_limit - m_consumed));
    if (want == 0) {
        m_end = true;
        return true;
    }
    const std::size_t old = out.size();
    out.resize(old + want);
    ssize_t n;
    do {
        n = ::read(m_fd, out.data() + old, want);
    } while (n < 0 && errno == EINTR);
    out.resize(old + std::size_t(std::max<ssize_t>(n, 0)));
    if (n < 0) {
        return false;
    }
    // A regular file ends at an empty read. A short read is not the end:
    // the caller reads again.
    m_consumed += std::uint64_t(n);
    if (n == 0 || m_consumed == m_limit) {
        m_end = true;
    }
    return true;
}

FdDeltaTarget::FdDeltaTarget(int basis, int output, std::uint32_t blockSize, std::uint64_t limit)
    : m_basis(basis)
    , m_output(output)
    , m_blockSize(blockSize)
    , m_limit(limit)
{
}

bool FdDeltaTarget::readBlock(std::uint64_t index, std::string& out)
{
    if (m_blockSize == 0 || index > std::uint64_t(std::numeric_limits<off_t>::max()) / m_blockSize) {
        return false;
    }
    const std::size_t old = out.size();
    out.resize(old + m_blockSize);
    std::size_t have = 0;
    // pread can return short before the end of the file: read until the
    // block is whole or the file ends.
    while (have < m_blockSize) {
        const ssize_t n = ::pread(m_basis, out.data() + old + have, m_blockSize - have, off_t(index * m_blockSize + have));
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            out.resize(old);
            return false;
        }
        if (n == 0) {
            break;
        }
        have += std::size_t(n);
    }
    out.resize(old + have);
    return true;
}

bool FdDeltaTarget::write(std::string_view bytes)
{
    if (bytes.size() > m_limit - m_written) {
        return false;
    }
    if (!writeAll(m_output, bytes)) {
        return false;
    }
    m_written += bytes.size();
    return true;
}

bool writeAll(int fd, std::string_view bytes)
{
    while (!bytes.empty()) {
        const ssize_t n = ::write(fd, bytes.data(), bytes.size());
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        bytes.remove_prefix(std::size_t(n));
    }
    return true;
}

} // namespace AfterglowProto
