// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

/// The limits of a process that decodes untrusted input: aglow's decoder and
/// the image decoders of aglow-mux.
namespace AfterglowProto {

/// The address space of this process, from /proc; 0 when it cannot be read.
inline std::uint64_t addressSpace()
{
    const int fd = ::open("/proc/self/statm", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    char text[128];
    const ssize_t n = ::read(fd, text, sizeof text - 1);
    ::close(fd);
    if (n <= 0) return 0;
    text[n] = '\0';
    const long page = ::sysconf(_SC_PAGESIZE);
    return std::strtoull(text, nullptr, 10) * std::uint64_t(page > 0 ? page : 4096);
}

/// The address space of this process, or, without /proc, a base that is far
/// more than a decoding process needs, with the @p fileBytes that it maps.
inline std::uint64_t addressSpaceOr(std::uint64_t fileBytes)
{
    const std::uint64_t space = addressSpace();
    return space != 0 ? space : (std::uint64_t(256) << 20) + fileBytes;
}

/// The memory that a decoder may take above its address space: a few times
/// the decoded image while it works (16-bit PNG, the canvas of an
/// animation). Past this, its allocations fail, whatever a corrupt file asks
/// for.
inline std::uint64_t decodeMemory(std::uint64_t frameBytes) { return 8 * frameBytes + (std::uint64_t(64) << 20); }

/// Seconds of CPU time to decode @p pixelBytes: decoding runs at well over
/// 16 MiB a second.
inline rlim_t decodeSeconds(std::uint64_t pixelBytes) { return rlim_t(5 + (pixelBytes >> 24)); }

/// Lowers the limit of @p resource to @p soft and @p hard, within the hard
/// limit that the process has: an unprivileged process cannot raise it. Set
/// a limit once, with both values, as a later call cannot go past the hard
/// limit of this one.
inline void limit(int resource, rlim_t soft, rlim_t hard)
{
    rlimit current{};
    if (::getrlimit(resource, &current) != 0) return;
    const rlimit next{std::min(soft, current.rlim_max), std::min(hard, current.rlim_max)};
    ::setrlimit(resource, &next);
}

} // namespace AfterglowProto
