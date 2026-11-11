// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// WebP through libwebp, and its animations through libwebpdemux, which
// composes each frame on the canvas.

#include "Formats.h"

#include <webp/decode.h>
#include <webp/demux.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace AfterglowImage::detail {

namespace {

std::uint64_t little(std::string_view bytes, std::size_t index, int count)
{
    std::uint64_t value = 0;
    for (int i = count; i-- > 0;) value = value << 8 | at(bytes, index + std::size_t(i));
    return value;
}

struct AnimDecoder
{
    void operator()(WebPAnimDecoder* decoder) const { WebPAnimDecoderDelete(decoder); }
};

} // namespace

Probe probeWebP(std::string_view bytes, int& width, int& height)
{
    width = height = 0;
    // RIFF, then the first chunk: one frame (VP8, VP8L), or the extended
    // header (VP8X) with the size of the canvas.
    if (bytes.size() < 30) return Probe::Unknown;
    const std::string_view chunk = bytes.substr(12, 4);
    if (chunk == "VP8 ") {
        // The frame tag, then the start code and two sides of 14 bits.
        if (at(bytes, 23) != 0x9D || at(bytes, 24) != 0x01 || at(bytes, 25) != 0x2A) return Probe::Unknown;
        return sized(little(bytes, 26, 2) & 0x3FFF, little(bytes, 28, 2) & 0x3FFF, width, height);
    }
    if (chunk == "VP8L") {
        if (at(bytes, 20) != 0x2F) return Probe::Unknown;
        const std::uint64_t bits = little(bytes, 21, 4);
        return sized((bits & 0x3FFF) + 1, (bits >> 14 & 0x3FFF) + 1, width, height);
    }
    if (chunk == "VP8X") return sized(little(bytes, 24, 3) + 1, little(bytes, 27, 3) + 1, width, height);
    return Probe::Unknown;
}

bool decodeWebP(std::string_view bytes, const FrameSink& sink, std::string& error, std::string& warning)
{
    int width = 0;
    int height = 0;
    if (probeWebP(bytes, width, height) != Probe::Image) {
        error = "the WebP has no valid header";
        return false;
    }
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.data());
    WebPBitstreamFeatures features{};
    if (WebPGetFeatures(data, bytes.size(), &features) != VP8_STATUS_OK) {
        error = "the WebP is corrupt";
        return false;
    }
    if (!features.has_animation) {
        if (features.width != width || features.height != height) {
            error = "the WebP is not what its header says";
            return false;
        }
        std::vector<std::uint8_t> rgba(std::size_t(width) * std::size_t(height) * 4);
        if (!WebPDecodeRGBAInto(data, bytes.size(), rgba.data(), rgba.size(), width * 4)) {
            error = "the WebP is corrupt";
            return false;
        }
        sink(rgba.data(), width, height, 0);
        return true;
    }
    WebPAnimDecoderOptions options;
    if (!WebPAnimDecoderOptionsInit(&options)) {
        error = "the WebP decoder does not start";
        return false;
    }
    options.color_mode = MODE_RGBA;
    options.use_threads = 0;
    const WebPData file{data, bytes.size()};
    const std::unique_ptr<WebPAnimDecoder, AnimDecoder> decoder(WebPAnimDecoderNew(&file, &options));
    WebPAnimInfo info{};
    if (!decoder || !WebPAnimDecoderGetInfo(decoder.get(), &info)) {
        error = "the animation of the WebP is corrupt";
        return false;
    }
    if (info.canvas_width != std::uint32_t(width) || info.canvas_height != std::uint32_t(height)) {
        error = "the WebP is not what its header says";
        return false;
    }
    int frames = 0;
    int shown = 0; ///< the end of the frame before, in milliseconds
    while (WebPAnimDecoderHasMoreFrames(decoder.get())) {
        std::uint8_t* canvas = nullptr;
        int end = 0;
        if (!WebPAnimDecoderGetNext(decoder.get(), &canvas, &end)) {
            if (frames == 0) {
                error = "the animation of the WebP is corrupt";
                return false;
            }
            warning = "the animation of the WebP is corrupt, so it stops after frame " + std::to_string(frames);
            return true;
        }
        ++frames;
        const int delay = std::max(0, end - shown);
        shown = end;
        if (!sink(canvas, width, height, delay)) return true;
    }
    if (frames == 0) {
        error = "the WebP has no frame";
        return false;
    }
    return true;
}

} // namespace AfterglowImage::detail
