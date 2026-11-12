// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// WebP through libwebp, and its animations through libwebpdemux, which
// composes each frame on the canvas.

#include "Formats.h"

#include <webp/decode.h>
#include <webp/demux.h>

#include <climits>
#include <memory>
#include <new>
#include <vector>

namespace AfterglowImage::detail {

namespace {

struct AnimDecoder
{
    void operator()(WebPAnimDecoder* decoder) const { WebPAnimDecoderDelete(decoder); }
};

} // namespace

Probe probeWebP(std::string_view bytes, int& width, int& height)
{
    width = height = 0;
    // RIFF, then the first chunk: one frame (VP8, VP8L), or the extended
    // header (VP8X) with the size of the canvas. Each kind has its size in
    // its own place, and a lossless image of one colour is shorter than the
    // headers of the other two.
    if (bytes.size() < 16) return Probe::Unknown;
    const std::string_view chunk = bytes.substr(12, 4);
    if (chunk == "VP8L") {
        if (bytes.size() < 25 || at(bytes, 20) != 0x2F) return Probe::Unknown;
        const std::uint32_t bits = little(bytes, 21, 4);
        return sized((bits & 0x3FFF) + 1, (bits >> 14 & 0x3FFF) + 1, width, height);
    }
    if (bytes.size() < 30) return Probe::Unknown;
    if (chunk == "VP8 ") {
        // The frame tag, then the start code and two sides of 14 bits.
        if (at(bytes, 23) != 0x9D || at(bytes, 24) != 0x01 || at(bytes, 25) != 0x2A) return Probe::Unknown;
        return sized(little(bytes, 26, 2) & 0x3FFF, little(bytes, 28, 2) & 0x3FFF, width, height);
    }
    if (chunk == "VP8X") return sized(std::uint64_t(little(bytes, 24, 3)) + 1, std::uint64_t(little(bytes, 27, 3)) + 1, width, height);
    return Probe::Unknown;
}

bool decodeWebP(std::string_view bytes, const FrameSink& sink, std::string& error, std::string& warning)
{
    int width = 0;
    int height = 0;
    if (refused(probeWebP(bytes, width, height), "WebP", error)) return false;
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
        WebPDecoderConfig config;
        if (!WebPInitDecoderConfig(&config)) {
            error = "the WebP decoder does not start";
            return false;
        }
        config.output.colorspace = MODE_RGBA;
        config.output.is_external_memory = 1;
        config.output.u.RGBA.rgba = rgba.data();
        config.output.u.RGBA.stride = width * 4;
        config.output.u.RGBA.size = rgba.size();
        const VP8StatusCode status = WebPDecode(data, bytes.size(), &config);
        WebPFreeDecBuffer(&config.output);
        // libwebp decodes through buffers of its own: their lack is a lack
        // of memory, not a bad file. But it reports a corrupt alpha chunk of
        // a lossy image with that status too, so there the status says
        // nothing and the file counts as bad (Decode.h says so).
        const bool lossyAlpha = features.format == 1 && features.has_alpha;
        if (status == VP8_STATUS_OUT_OF_MEMORY && !lossyAlpha) throw std::bad_alloc();
        if (status != VP8_STATUS_OK) {
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
    if (!decoder) {
        // The options are valid, so the decoder fails either on a container
        // that does not demux, or on the canvases that it allocates itself.
        if (WebPDemuxer* demuxer = WebPDemux(&file)) {
            WebPDemuxDelete(demuxer);
            throw std::bad_alloc();
        }
        error = "the animation of the WebP is corrupt";
        return false;
    }
    WebPAnimInfo info{};
    if (!WebPAnimDecoderGetInfo(decoder.get(), &info)) {
        error = "the animation of the WebP is corrupt";
        return false;
    }
    if (info.canvas_width != std::uint32_t(width) || info.canvas_height != std::uint32_t(height)) {
        error = "the WebP is not what its header says";
        return false;
    }
    const WebPDemuxer* const demuxer = WebPAnimDecoderGetDemuxer(decoder.get());
    int frames = 0;
    std::int64_t shown = 0; ///< the end of the frame before, in milliseconds
    auto damaged = [&](const char* what) {
        if (frames == 0) {
            error = what;
            return false;
        }
        warning = std::string(what) + ", so it stops after frame " + std::to_string(frames);
        return true;
    };
    while (WebPAnimDecoderHasMoreFrames(decoder.get())) {
        // The time that the frame shows, from the file. The decoder gives
        // only the end of each frame, a sum that it keeps in an int: 129
        // frames of the longest time pass it. Such an animation stops at the
        // frame before, so that the decoder never adds past its int.
        WebPIterator next;
        if (!WebPDemuxGetFrame(demuxer, frames + 1, &next)) return damaged("the animation of the WebP is corrupt");
        const int delay = next.duration;
        WebPDemuxReleaseIterator(&next);
        if (delay < 0 || shown + delay > INT_MAX) return damaged("the animation of the WebP is longer than 24 days");
        std::uint8_t* canvas = nullptr;
        int end = 0;
        if (!WebPAnimDecoderGetNext(decoder.get(), &canvas, &end)) return damaged("the animation of the WebP is corrupt");
        ++frames;
        shown += delay;
        if (!sink(canvas, width, height, delay)) return true;
    }
    if (frames == 0) {
        error = "the WebP has no frame";
        return false;
    }
    return true;
}

} // namespace AfterglowImage::detail
