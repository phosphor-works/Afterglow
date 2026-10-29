// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/afterglowvt_export.h>

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace AfterglowVt {

/// A terminal colour: the default colour, a palette index, or a direct RGB value.
/// Packed into 32 bits: [31:24] kind, [23:0] RGB or the palette index.
class Color
{
public:
    enum class Kind : std::uint8_t { Default, Palette, Rgb };

    constexpr Color() = default;

    static constexpr Color palette(std::uint8_t index) { return Color(Kind::Palette, index); }
    static constexpr Color rgb(std::uint8_t r, std::uint8_t g, std::uint8_t b)
    {
        return Color(Kind::Rgb, (std::uint32_t(r) << 16) | (std::uint32_t(g) << 8) | b);
    }

    constexpr Kind kind() const { return static_cast<Kind>(m_bits >> 24); }
    constexpr bool isDefault() const { return kind() == Kind::Default; }
    constexpr std::uint8_t index() const { return static_cast<std::uint8_t>(m_bits & 0xFF); }
    constexpr std::uint8_t r() const { return static_cast<std::uint8_t>((m_bits >> 16) & 0xFF); }
    constexpr std::uint8_t g() const { return static_cast<std::uint8_t>((m_bits >> 8) & 0xFF); }
    constexpr std::uint8_t b() const { return static_cast<std::uint8_t>(m_bits & 0xFF); }
    constexpr std::uint32_t bits() const { return m_bits; }

    constexpr bool operator==(const Color&) const = default;

private:
    constexpr Color(Kind kind, std::uint32_t value)
        : m_bits((std::uint32_t(kind) << 24) | (value & 0xFFFFFF))
    {
    }

    std::uint32_t m_bits = 0;
};

enum class UnderlineStyle : std::uint8_t { None, Single, Double, Curly, Dotted, Dashed };

namespace Attr {
inline constexpr std::uint16_t Bold = 1 << 0;
inline constexpr std::uint16_t Faint = 1 << 1;
inline constexpr std::uint16_t Italic = 1 << 2;
inline constexpr std::uint16_t Blink = 1 << 3;
inline constexpr std::uint16_t Inverse = 1 << 4;
inline constexpr std::uint16_t Invisible = 1 << 5;
inline constexpr std::uint16_t Strikethrough = 1 << 6;
inline constexpr std::uint16_t Overline = 1 << 7;
} // namespace Attr

struct Style
{
    Color fg;
    Color bg;
    Color underlineColor;
    std::uint16_t attrs = 0;
    UnderlineStyle underline = UnderlineStyle::None;

    bool has(std::uint16_t attr) const { return (attrs & attr) != 0; }
    bool operator==(const Style&) const = default;
};

struct StyleHash
{
    std::size_t operator()(const Style& s) const noexcept
    {
        std::uint64_t h = s.fg.bits();
        h = h * 0x9E3779B97F4A7C15ull ^ s.bg.bits();
        h = h * 0x9E3779B97F4A7C15ull ^ s.underlineColor.bits();
        h = h * 0x9E3779B97F4A7C15ull ^ ((std::uint64_t(s.attrs) << 8) | std::uint64_t(s.underline));
        return static_cast<std::size_t>(h ^ (h >> 29));
    }
};

/// Interned, reference-counted styles. Cells hold a 24-bit id instead of the
/// full style. Id 0 is the default style and is never freed.
class AFTERGLOWVT_EXPORT StyleTable
{
public:
    static constexpr std::uint32_t DefaultId = 0;
    static constexpr std::uint32_t MaxId = (1u << 24) - 1;

    StyleTable();

    /// Interns @p style and takes one reference to it.
    std::uint32_t acquire(const Style& style);
    void retain(std::uint32_t id) noexcept;
    void release(std::uint32_t id) noexcept;

    const Style& get(std::uint32_t id) const noexcept { return m_styles[id]; }
    std::size_t liveCount() const noexcept { return m_lookup.size() + 1; }

private:
    std::vector<Style> m_styles;
    std::vector<std::uint32_t> m_refs;
    std::vector<std::uint32_t> m_free;
    std::unordered_map<Style, std::uint32_t, StyleHash> m_lookup;
};

} // namespace AfterglowVt
