// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Aglow {

/// A decoded image: one RGBA canvas for each frame (a GIF can have many),
/// with straight (not premultiplied) alpha.
class Image
{
public:
    int width = 0;
    int height = 0;
    int frameCount() const { return int(m_frames.size()); }
    /// Milliseconds that frame @p index shows, for an animation.
    int delay(int index) const { return m_delays[std::size_t(index)]; }
    /// Frame @p index, as RGBA bytes.
    const std::string& frame(int index) const { return m_frames[std::size_t(index)]; }
    /// Why an animation has fewer frames than its file: empty when it has all of them.
    const std::string& warning() const { return m_warning; }

private:
    friend std::optional<Image> decodeImage(std::string_view, std::size_t, std::string&);
    std::vector<std::string> m_frames;
    std::vector<int> m_delays;
    std::string m_warning;
};

/// The size of an encoded image without decoding it, turned as its EXIF
/// orientation says. False when aglow cannot read it.
bool imageSize(std::string_view bytes, int& width, int& height);
/// The same, with @p error set to the reason when it is false.
bool imageSize(std::string_view bytes, int& width, int& height, std::string& error);
/// Whether @p bytes are a PNG file that a terminal can decode itself: a valid
/// IHDR first, and not Apple's CgBI variant.
bool isPng(std::string_view bytes);
/// Decodes PNG, JPEG, GIF and WebP (with every frame) and BMP
/// (AfterglowImage), when the decoded pixels fit in @p budget bytes and
/// each side in AfterglowProto::GraphicsMaxSide. A JPEG is turned as its EXIF
/// orientation says. An animation whose frames do not all fit in @p budget, or that
/// has more than AfterglowProto::GraphicsMaxFrames, gives its first frame.
/// The decoder runs in a child process with limits on its memory and CPU
/// time, so that a corrupt file cannot harm aglow. nullopt with @p error set
/// otherwise.
std::optional<Image> decodeImage(std::string_view bytes, std::size_t budget, std::string& error);

/// Scales RGBA frames of one size to another. The filter weights are worked
/// out once, for all the frames of an animation.
class Resizer
{
public:
    Resizer(int width, int height, int newWidth, int newHeight);
    /// Whether the sizes are the same: then a frame needs no scaling.
    bool identity() const { return m_width == m_newWidth && m_height == m_newHeight; }
    std::string operator()(const std::string& rgba) const;

private:
    struct Taps
    {
        int first = 0;
        std::vector<float> weights;
    };
    static std::vector<Taps> taps(int from, int to);

    int m_width;
    int m_height;
    int m_newWidth;
    int m_newHeight;
    std::vector<Taps> m_across;
    std::vector<Taps> m_down;
    std::size_t m_window = 1;
};

/// One frame, as RGBA bytes, scaled to a new size (a copy).
std::string resizeFrame(const std::string& rgba, int width, int height, int newWidth, int newHeight);
/// These change an RGBA frame in place.
void mirrorFrame(std::string& rgba, int width, int height, bool horizontal, bool vertical);
/// Draws the frame over an opaque colour, so that it has no transparency left.
void flattenFrame(std::string& rgba, std::uint8_t red, std::uint8_t green, std::uint8_t blue);
bool frameOpaque(const std::string& rgba);
/// The RGB bytes of an RGBA frame (a copy).
std::string dropAlpha(const std::string& rgba);

} // namespace Aglow
