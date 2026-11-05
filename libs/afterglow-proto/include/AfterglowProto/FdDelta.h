// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowProto/Delta.h>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

/// The delta classes of Delta.h over file descriptors, for a client that
/// sends or rebuilds files on disk. The caller owns the descriptors.
namespace AfterglowProto {

/// A ByteSource that reads @p fd from its position with read(2), up to
/// @p limit bytes in all. A client gives the size that it announced for
/// the file, so a file that grows while it is read still matches.
class FdSource : public ByteSource
{
public:
    explicit FdSource(int fd, std::uint64_t limit = std::numeric_limits<std::uint64_t>::max());
    bool read(std::string& out, std::size_t max) override;
    bool atEnd() const override { return m_end; }
    /// The bytes read so far.
    std::uint64_t consumed() const { return m_consumed; }

private:
    int m_fd;
    std::uint64_t m_limit;
    std::uint64_t m_consumed = 0;
    bool m_end = false;
};

/// A DeltaTarget that reads the blocks of the basis from @p basis with
/// pread(2) and appends the new file to @p output with write(2). It stops
/// the delta when the new file would pass @p limit bytes, so a small delta
/// cannot fill the disk with copies of one block.
class FdDeltaTarget : public DeltaTarget
{
public:
    FdDeltaTarget(int basis, int output, std::uint32_t blockSize,
                  std::uint64_t limit = std::numeric_limits<std::uint64_t>::max());
    bool readBlock(std::uint64_t index, std::string& out) override;
    bool write(std::string_view bytes) override;
    /// The bytes of the new file written so far.
    std::uint64_t written() const { return m_written; }

private:
    int m_basis;
    int m_output;
    std::uint32_t m_blockSize;
    std::uint64_t m_limit;
    std::uint64_t m_written = 0;
};

/// Writes all of @p bytes to @p fd, again after EINTR and short writes.
/// False on any other error, with errno set.
bool writeAll(int fd, std::string_view bytes);

} // namespace AfterglowProto
