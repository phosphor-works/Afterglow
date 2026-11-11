// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

/// X11 colour specs, as OSC 4, 10-19 and kitty's colour control take them,
/// and as a client reads them from its command line.
namespace AfterglowProto {

/// An OKLCH colour (L 0-1, C, H in degrees) in 8-bit sRGB. A colour outside
/// sRGB is brought in with the gamut mapping of CSS Color 4: lightness and
/// hue stay, chroma goes down until the clipped colour is less than one
/// just noticeable difference (deltaE OK 0.02) away.
void oklchToSrgb(double l, double c, double h, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b);
/// A CIE LAB colour (D50, L 0-100, as CSS has it) in 8-bit sRGB, with the
/// same gamut mapping.
void labToSrgb(double l, double a, double bAxis, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b);
/// Reads "oklch(L C H)" or "lab(L a b)" as kitty and CSS Color 4 write them:
/// the function name in any case, the three values apart by spaces or
/// commas, L as a number or a percentage, a hue in degrees or with the CSS
/// angle units, "none" for 0. A percentage of C is of 0.4, and of a and b of
/// 125, as in CSS. An alpha ("/ 0.5") is refused: terminal colours have none.
/// On failure @p r, @p g and @p b are left as they were.
bool parseWideGamutColor(std::string_view spec, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b);

namespace detail {

struct ColorValue
{
    std::uint8_t r, g, b;
};

// The X11 colour names, lowercase and sorted, for binary search.
inline constexpr std::pair<std::string_view, ColorValue> ColorNames[] = {
#include "ColorNames.inc"
};
static_assert(std::is_sorted(std::begin(ColorNames), std::end(ColorNames),
                             [](const auto& a, const auto& b) { return a.first < b.first; }));

/// The longest colour name: a longer spec is no name, and is not copied to look it up.
inline constexpr std::size_t MaxColorName = [] {
    std::size_t longest = 0;
    for (const auto& entry : ColorNames) {
        longest = std::max(longest, entry.first.size());
    }
    return longest;
}();

inline int colorHexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/// One "rgb:" component of 1-4 hex digits, scaled to 8 bits.
inline bool parseColorScaled(std::string_view s, std::uint8_t& out)
{
    if (s.empty() || s.size() > 4) return false;
    unsigned v = 0;
    for (const char c : s) {
        const int d = colorHexDigit(c);
        if (d < 0) return false;
        v = v * 16 + unsigned(d);
    }
    const unsigned max = (1u << (4 * s.size())) - 1;
    out = std::uint8_t((v * 255 + max / 2) / max);
    return true;
}

/// One "#" component: per XParseColor its digits are the high-order bits,
/// so "#fff" is f0/f0/f0, not white.
inline bool parseColorHigh(std::string_view s, std::uint8_t& out)
{
    unsigned v = 0;
    for (const char c : s) {
        const int d = colorHexDigit(c);
        if (d < 0) return false;
        v = v * 16 + unsigned(d);
    }
    const int bits = 4 * int(s.size());
    out = std::uint8_t(bits >= 8 ? v >> (bits - 8) : v << (8 - bits));
    return true;
}

/// An "rgbi:" intensity in [0, 1], read with a '.' whatever the locale.
inline bool parseColorIntensity(std::string_view s, std::uint8_t& out)
{
    double v = 0;
    const auto parsed = std::from_chars(s.data(), s.data() + s.size(), v);
    if (s.empty() || parsed.ec != std::errc{} || parsed.ptr != s.data() + s.size() || !std::isfinite(v)) {
        return false;
    }
    out = std::uint8_t(std::lround(std::clamp(v, 0.0, 1.0) * 255.0));
    return true;
}

/// Splits "a/b/c" and reads each part with @p component.
template<typename Component>
bool parseColorTriple(std::string_view spec, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b, Component component)
{
    const auto s1 = spec.find('/');
    const auto s2 = s1 == std::string_view::npos ? s1 : spec.find('/', s1 + 1);
    if (s2 == std::string_view::npos) return false;
    return component(spec.substr(0, s1), r) && component(spec.substr(s1 + 1, s2 - s1 - 1), g)
        && component(spec.substr(s2 + 1), b);
}

} // namespace detail

/// Reads an X11 colour spec: "rgb:r/g/b" (1-4 hex digits each, scaled),
/// "rgbi:r/g/b" (intensities), "#rgb", "#rrggbb", "#rrrgggbbb",
/// "#rrrrggggbbbb", or an X11 colour name in any case. The device-independent
/// CIE and TekHVC spaces of Xcms are not supported. kitty's "oklch(...)" and
/// "lab(...)" are (parseWideGamutColor). On failure @p r, @p g and @p b are
/// left as they were.
inline bool parseColorSpec(std::string_view spec, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b)
{
    std::uint8_t cr = 0, cg = 0, cb = 0;
    bool ok = false;
    if (spec.find('(') != std::string_view::npos) {
        ok = parseWideGamutColor(spec, cr, cg, cb);
    } else if (spec.starts_with("rgbi:")) {
        ok = detail::parseColorTriple(spec.substr(5), cr, cg, cb, detail::parseColorIntensity);
    } else if (spec.starts_with("rgb:")) {
        ok = detail::parseColorTriple(spec.substr(4), cr, cg, cb, detail::parseColorScaled);
    } else if (spec.starts_with('#')) {
        spec.remove_prefix(1);
        if (!spec.empty() && spec.size() % 3 == 0 && spec.size() <= 12) {
            const std::size_t n = spec.size() / 3;
            ok = detail::parseColorHigh(spec.substr(0, n), cr) && detail::parseColorHigh(spec.substr(n, n), cg)
                && detail::parseColorHigh(spec.substr(2 * n, n), cb);
        }
    } else if (spec.size() <= detail::MaxColorName) {
        std::string lower(spec);
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : char(c); });
        const auto* end = std::end(detail::ColorNames);
        const auto* it = std::lower_bound(std::begin(detail::ColorNames), end, std::string_view(lower),
                                          [](const auto& entry, std::string_view value) { return entry.first < value; });
        if (it != end && it->first == lower) {
            cr = it->second.r;
            cg = it->second.g;
            cb = it->second.b;
            ok = true;
        }
    }
    if (ok) {
        r = cr;
        g = cg;
        b = cb;
    }
    return ok;
}

} // namespace AfterglowProto
