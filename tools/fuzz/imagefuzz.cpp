// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
//
// libFuzzer target for the image decoders of aglow icat and aglow dnd: the
// patched stb_image and aglow's own GIF decoder, called in this process as
// the decoder child of aglow calls them, so that the sanitizers see each
// read and write. aglow itself decodes in a child process with limits on its
// memory and CPU time. This target bounds the work the same way: images of
// at most 4 MiB, and 64 frames.

#include "StbImage.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace {

constexpr std::uint64_t MaxPixels = std::uint64_t(1) << 20;
constexpr int MaxFrames = 64;

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    const std::string_view bytes(reinterpret_cast<const char*>(data), size);
    int width = 0;
    int height = 0;
    if (Aglow::Stb::probe(bytes, width, height) != Aglow::Stb::Probe::Image
        || std::uint64_t(width) * std::uint64_t(height) > MaxPixels) {
        return 0;
    }
    int frames = 0;
    std::uint32_t sum = 0;
    std::string error;
    std::string warning;
    Aglow::Stb::decode(bytes, [&](const unsigned char* rgba, int w, int h, int) {
        // Every byte of every frame is read, so that the sanitizers see a
        // pixel that the decoder left uninitialized.
        const std::size_t n = std::size_t(w) * std::size_t(h) * 4;
        for (std::size_t i = 0; i < n; ++i) {
            sum += rgba[i];
        }
        return ++frames < MaxFrames;
    }, error, warning);
    // The sum is used, so that the loop above is not optimized away.
    volatile std::uint32_t keep = sum;
    (void)keep;
    return 0;
}
