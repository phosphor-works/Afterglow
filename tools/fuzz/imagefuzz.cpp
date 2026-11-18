// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// libFuzzer target for the image decoders of aglow and aglow-mux
// (libs/afterglow-image), called in this process as their decoder children
// call them, so that the sanitizers see each read and write of the GIF, BMP,
// TIFF and netPBM readers, of the APNG frames, of the code around libpng,
// libjpeg-turbo, libwebp and Little CMS, and of those four libraries, which
// a fuzz build instruments too. It also checks what Decode.h promises about
// the result: frames of the probed size, one frame of no time and no warning
// for JPEG, BMP, TIFF, netPBM and a PNG read for its default image (each PNG
// is read that way too), and no frame after the sink said stop. aglow and
// aglow-mux decode in a child process with limits on its memory and CPU
// time. This target bounds the work by the size instead: a canvas of at
// most 2^20 pixels (4 MiB of RGBA), and 64 frames.
// The work of a GIF frame is bounded by its canvas and its data, whatever
// size the frame claims, so the canvas bounds it here too. The 64 frames
// keep one path out of reach: a WebP animation stops after 128 frames of the
// longest time (the tests of the library have it).
// afterglow_imagefuzz_seeds writes the files of those tests as a first
// corpus.

#include <AfterglowImage/Decode.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

constexpr std::uint64_t MaxPixels = std::uint64_t(1) << 20;
constexpr int MaxFrames = 64;

/// Decodes @p bytes, whose probe gave @p width by @p height, and traps on a
/// result that Decode.h does not allow. A still image gives one frame of no
/// time and no warning.
void check(std::string_view bytes, int width, int height, bool still, AfterglowImage::PngFrames pngFrames)
{
    std::string error;
    std::string warning;
    int frames = 0;
    std::uint32_t sum = 0;
    const bool decoded = AfterglowImage::decode(
        bytes,
        [&](const unsigned char* rgba, int w, int h, int delay) {
            // A sink that said stop is not called again: the 64 frames bound
            // the work only if the decoders stop.
            if (frames >= MaxFrames)
                __builtin_trap();
            // Each frame has the size that the probe gave, and a time that is
            // not negative.
            if (w != width || h != height || delay < 0 || (still && (frames > 0 || delay != 0)))
                __builtin_trap();
            // Every byte of every frame is read, so that AddressSanitizer sees a
            // frame that is shorter than its size says.
            const std::size_t n = std::size_t(w) * std::size_t(h) * 4;
            for (std::size_t i = 0; i < n; ++i) {
                sum += rgba[i];
            }
            return ++frames < MaxFrames;
        },
        error, warning, pngFrames);
    // Decoded means a frame, and not decoded means an error text and no
    // frame.
    if (decoded ? frames == 0 : (frames != 0 || error.empty()))
        __builtin_trap();
    if (still && !warning.empty())
        __builtin_trap();
    // The sum is used, so that the loop above is not optimized away.
    volatile std::uint32_t keep = sum;
    (void)keep;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    int width = 0;
    int height = 0;
    const AfterglowImage::Probe probed = AfterglowImage::probe(bytes, width, height);
    if (probed != AfterglowImage::Probe::Image) {
        // What the probe does not take, the decoder of the format refuses
        // before it decodes: no frame, and an error that says why.
        if (AfterglowImage::format(bytes) == AfterglowImage::Format::Unknown)
            return 0;
        std::string error;
        std::string warning;
        const bool decoded = AfterglowImage::decode(
            bytes,
            [](const unsigned char*, int, int, int) -> bool {
                __builtin_trap();
            },
            error, warning);
        if (decoded || error.empty())
            __builtin_trap();
        return 0;
    }
    if (std::uint64_t(width) * std::uint64_t(height) > MaxPixels)
        return 0;
    // GIF, WebP and PNG can be animations. A PNG read for its default image
    // is still, also an APNG.
    const AfterglowImage::Format kind = AfterglowImage::format(bytes);
    const bool animation = kind == AfterglowImage::Format::Gif || kind == AfterglowImage::Format::WebP
        || kind == AfterglowImage::Format::Png;
    check(bytes, width, height, !animation, AfterglowImage::PngFrames::Animation);
    if (kind == AfterglowImage::Format::Png)
        check(bytes, width, height, true, AfterglowImage::PngFrames::DefaultImage);
    return 0;
}
