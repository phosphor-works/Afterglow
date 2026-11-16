// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What the client of the image decoder (ImageDecoder.cpp, in the daemon and
// in the window) and its server (ImageServer.cpp, aglow-mux --decode-images)
// share: the frame header on the pipe, the bounds, and their checks.

#include "ImageDecoder.h"

#include <AfterglowImage/Decode.h>
#include <AfterglowProto/Fd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include <poll.h>
#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace AfterglowMux::decoding {

static_assert(AfterglowImage::MaxSide == int(AfterglowProto::GraphicsMaxSide), "the decoders and the store bound a side alike");

inline constexpr std::size_t MaxBytes = ImageDecoder::MaxBytes;

/// Before the pixels of each frame that a decoding process writes.
struct Header
{
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t delayMs = 0;
};

inline bool fits(std::uint64_t width, std::uint64_t height)
{
    return width > 0 && height > 0 && width <= AfterglowProto::GraphicsMaxSide && height <= AfterglowProto::GraphicsMaxSide
        && width * height * 4 <= MaxBytes;
}

// The formats that the window takes too (imageFormatAllowed in
// afterglow/src/ImageDecode.cpp), by their signatures.
inline bool allowed(std::string_view data, ImageFormats formats)
{
    const AfterglowImage::Format format = AfterglowImage::format(data);
    if (formats == ImageFormats::Png) return format == AfterglowImage::Format::Png;
    return format != AfterglowImage::Format::Unknown;
}

// Through syscall(2): <sys/pidfd.h> of some glibc versions declares them
// without C linkage.
inline int pidfdOpen(pid_t pid)
{
    return int(::syscall(SYS_pidfd_open, pid, 0));
}

inline void pidfdKill(int pidfd)
{
    ::syscall(SYS_pidfd_send_signal, pidfd, SIGKILL, nullptr, 0);
}

inline bool writeAll(int fd, const void* data, std::size_t size)
{
    const auto* p = static_cast<const char*>(data);
    while (size > 0) {
        const ssize_t n = ::write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        size -= std::size_t(n);
    }
    return true;
}

/// One line on standard error, the journal under systemd, at most once a
/// minute for each @p last: a program that sends broken images in a loop
/// fills no log.
inline void report(std::chrono::steady_clock::time_point& last, const std::string& text, const std::string& who = "aglow-mux")
{
    const auto now = std::chrono::steady_clock::now();
    if (last != std::chrono::steady_clock::time_point{} && now - last < std::chrono::minutes(1)) return;
    last = now;
    std::fprintf(stderr, "%s: %s\n", who.c_str(), text.c_str());
}

} // namespace AfterglowMux::decoding
