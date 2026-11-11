// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace AfterglowMux {

/// The argument that makes afterglow-mux an image decoder.
inline constexpr std::string_view DecodeImageArgument = "--decode-image";

/// The formats that a program may send.
enum class ImageFormats {
    Png, ///< kitty graphics f=100: PNG only
    Common, ///< iTerm2 inline images: PNG, JPEG, GIF (its first frame) and BMP
};

/// Decodes @p data into RGBA8 with straight alpha, as the window's
/// decodeBoundedImage does with Qt. The daemon has no Qt: the patched
/// stb_image of aglow decodes, in a process of its own. @p decoder runs with
/// DecodeImageArgument and limits on its memory, its CPU time and its core
/// files, so that a corrupt file ends that process, not the daemon and its
/// sessions. The daemon passes its own executable, which a rebuild cannot
/// replace while it runs. A process with threads cannot fork without an exec
/// safely. False when the format is not one of @p formats, when the image is
/// larger than the store takes, when the decoder fails or when it takes more
/// than DecodeWaitMs.
bool decodeImage(std::string_view data, ImageFormats formats, std::uint32_t& width, std::uint32_t& height,
                 std::vector<std::uint8_t>& rgba, const char* decoder = "/proc/self/exe");

/// How long decodeImage() waits for the decoder before it kills it. The
/// decoder's CPU limit ends a decode that computes first.
inline constexpr int DecodeWaitMs = 30000;

/// The decoder process: reads the image from standard input and writes its
/// width and height (two 32-bit integers in the order of this host) and its
/// pixels to standard output. Returns the exit status.
int runImageDecoder();

} // namespace AfterglowMux
