// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Aglow {

/// A decoded image: one RGBA canvas for each frame (a GIF can have many),
/// with straight (not premultiplied) alpha. The frames stay in the decoder's
/// one buffer: frame() copies one at a time, so that a caller never holds a
/// copy of all of them.
class Image
{
public:
    int width = 0;
    int height = 0;
    int frameCount() const { return int(m_delays.size()); }
    /// Milliseconds that frame @p index shows, for an animation.
    int delay(int index) const { return m_delays[std::size_t(index)]; }
    /// A copy of frame @p index, as RGBA bytes.
    std::string frame(int index) const;

private:
    friend std::optional<Image> decodeImage(std::string_view, std::size_t, std::string&);
    std::shared_ptr<unsigned char> m_pixels;
    std::vector<int> m_delays;
};

/// The size of an encoded image without decoding it. False when the format is unknown.
bool imageSize(std::string_view bytes, int& width, int& height);
/// Whether @p bytes are a PNG file.
bool isPng(std::string_view bytes);
/// The frames of a GIF file, counted from its blocks without decoding them; 0 for other data.
int gifFrames(std::string_view bytes);
/// Decodes PNG, JPEG, GIF (with every frame), BMP, PSD, TGA, HDR, PIC and
/// PNM, when the decoded pixels fit in @p budget bytes and each side in
/// AfterglowProto::GraphicsMaxSide. A GIF whose frames do not all fit gives
/// its first frame. nullopt with @p error set otherwise.
std::optional<Image> decodeImage(std::string_view bytes, std::size_t budget, std::string& error);

/// One frame, as RGBA bytes: these work on it in place.
std::string resizeFrame(const std::string& rgba, int width, int height, int newWidth, int newHeight);
void mirrorFrame(std::string& rgba, int width, int height, bool horizontal, bool vertical);
/// Draws the frame over an opaque colour, so that it has no transparency left.
void flattenFrame(std::string& rgba, std::uint8_t red, std::uint8_t green, std::uint8_t blue);
bool frameOpaque(const std::string& rgba);
/// The RGB bytes of an RGBA frame.
std::string dropAlpha(const std::string& rgba);

} // namespace Aglow
