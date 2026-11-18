// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// What the client of the image decoder (ImageDecoder.cpp, in the daemon and
// in the window) and its server (ImageServer.cpp, aglow-mux --decode-images)
// share: the bounds and their checks. The frames on the pipe are the records
// of AfterglowProto/DecodeRecord.h, as aglow's. A change of either is a new
// DecodeWireVersion (ImageDecoder.h).

#include "ImageDecoder.h"

#include <AfterglowImage/Decode.h>
#include <AfterglowProto/Fd.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <signal.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace AfterglowMux::decoding {

static_assert(AfterglowImage::MaxSide == int(AfterglowProto::GraphicsMaxSide), "the decoders and the store bound a side alike");

inline constexpr std::size_t MaxBytes = ImageDecoder::MaxBytes;
/// The exit status of a server whose client is of another version.
inline constexpr int OtherVersionStatus = 64;
/// The exit status of aglow-mux for arguments that it does not take
/// (main.cpp): an aglow-mux of a version from before the version argument
/// of the server exits with it. The server itself never does.
inline constexpr int UsageStatus = 2;

inline bool fits(std::uint64_t width, std::uint64_t height)
{
    return width > 0 && height > 0 && width <= AfterglowProto::GraphicsMaxSide && height <= AfterglowProto::GraphicsMaxSide
        && width * height * 4 <= MaxBytes;
}

// The formats of @p formats, by their signatures: the client checks them
// before it sends an image, and the decoding process again.
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

/// Whether a report of its kind, last made at @p last, may go now: at most
/// one a minute, so that a program that sends broken images in a loop fills
/// no log.
inline bool reportDue(std::chrono::steady_clock::time_point& last)
{
    const auto now = std::chrono::steady_clock::now();
    if (last != std::chrono::steady_clock::time_point{} && now - last < std::chrono::minutes(1)) return false;
    last = now;
    return true;
}

} // namespace AfterglowMux::decoding
