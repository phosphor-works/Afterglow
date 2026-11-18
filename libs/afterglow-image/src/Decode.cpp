// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "Color.h"
#include "Formats.h"

#include <algorithm>
#include <climits>
#include <vector>

namespace AfterglowImage {

namespace detail {

Probe sized(std::uint64_t width, std::uint64_t height, int& outWidth, int& outHeight)
{
    outWidth = outHeight = 0;
    if (width == 0 || height == 0) return Probe::Unknown;
    outWidth = int(std::min<std::uint64_t>(width, INT_MAX));
    outHeight = int(std::min<std::uint64_t>(height, INT_MAX));
    return width > std::uint64_t(MaxSide) || height > std::uint64_t(MaxSide) ? Probe::TooLarge : Probe::Image;
}

bool refused(Probe probe, const char* name, std::string& error)
{
    if (probe == Probe::Image) return false;
    error = std::string("the ") + name;
    error += probe == Probe::TooLarge ? " has a side longer than " + std::to_string(MaxSide) + " pixels" : " has no valid header";
    return true;
}

std::string libraryMessage(std::string_view text)
{
    const std::size_t colon = text.find(": ");
    if (colon != std::string_view::npos) {
        const std::string_view name = text.substr(0, colon);
        if (name.ends_with("()") || name.starts_with("png_")) text.remove_prefix(colon + 2);
    }
    return std::string(text);
}

namespace {

/// A format's probe, colour space (nullptr: its pixels are sRGB) and
/// decoder: the one place where a format is picked.
struct Codec
{
    Format format;
    Probe (*probe)(std::string_view, int&, int&);
    ColorSpace (*colorSpace)(std::string_view);
    bool (*decode)(std::string_view, const FrameSink&, const Options&, std::string&, std::string&);
};

constexpr Codec Codecs[] = {
    {Format::Png, probePng, pngColorSpace, decodePng},
    {Format::Jpeg, probeJpeg, jpegColorSpace, decodeJpeg},
    {Format::Gif, probeGif, nullptr, decodeGif},
    {Format::WebP, probeWebP, webpColorSpace, decodeWebP},
    {Format::Bmp, probeBmp, nullptr, decodeBmp},
    {Format::Tiff, probeTiff, tiffColorSpace, decodeTiff},
    {Format::Pnm, probePnm, nullptr, decodePnm},
};

const Codec* codecOf(Format format)
{
    for (const Codec& codec : Codecs) {
        if (codec.format == format) return &codec;
    }
    return nullptr;
}

} // namespace

} // namespace detail

Probe probe(std::string_view bytes, int& width, int& height)
{
    width = height = 0;
    const detail::Codec* codec = detail::codecOf(format(bytes));
    return codec ? codec->probe(bytes, width, height) : Probe::Unknown;
}

bool decode(std::string_view bytes, const FrameSink& sink, std::string& error, std::string& warning, PngFrames pngFrames)
{
    const detail::Codec* codec = detail::codecOf(format(bytes));
    if (!codec) {
        error = "not an image that this decoder reads";
        return false;
    }
    detail::Options options;
    options.pngFrames = pngFrames;
    // The probe first: a file that it refuses goes to its decoder, which
    // refuses it with the reason, and nothing of such a file (a profile)
    // goes to Little CMS.
    int width = 0;
    int height = 0;
    if (!codec->colorSpace || codec->probe(bytes, width, height) != Probe::Image) return codec->decode(bytes, sink, options, error, warning);
    // A file with an ICC profile or a cICP chunk converts to sRGB, frame by
    // frame, as kitty converts the images that it decodes.
    const detail::SrgbConverter converter(codec->colorSpace(bytes));
    options.colorKnown = converter.known();
    if (!converter.active()) return codec->decode(bytes, sink, options, error, warning);
    // A copy: the decoder of an animation draws the next frame over the
    // last one, in the colours of the file.
    std::vector<unsigned char> converted;
    const FrameSink colored = [&](const unsigned char* rgba, int frameWidth, int frameHeight, int delayMs) {
        const std::size_t pixels = std::size_t(frameWidth) * std::size_t(frameHeight);
        converted.resize(pixels * 4);
        converter.convert(rgba, converted.data(), pixels);
        return sink(converted.data(), frameWidth, frameHeight, delayMs);
    };
    return codec->decode(bytes, colored, options, error, warning);
}

} // namespace AfterglowImage
