// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// OKLCH and CIE LAB, as kitty takes them in colour settings and escape codes,
// converted with the sample code and the gamut mapping of CSS Color 4
// (https://www.w3.org/TR/css-color-4/#color-conversion-code and
// #css-gamut-mapping). kitty 0.50 clips a colour outside sRGB where its
// documentation names this mapping, and its LAB conversion gives lab(10 0 0)
// as a middle gray: the CSS results are the reference here.

#include <AfterglowProto/Color.h>

#include <array>
#include <cmath>
#include <numbers>

namespace AfterglowProto {

namespace {

using Vec3 = std::array<double, 3>;
using Matrix = std::array<Vec3, 3>;

Vec3 multiply(const Matrix& m, const Vec3& v)
{
    return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}

// The matrices of the CSS sample code.
constexpr Matrix OklabToLms{{{1.0, 0.3963377773761749, 0.2158037573099136},
                             {1.0, -0.1055613458156586, -0.0638541728258133},
                             {1.0, -0.0894841775298119, -1.2914855480194092}}};
constexpr Matrix LmsToXyz{{{1.2268798758459243, -0.5578149944602171, 0.2813910456659647},
                           {-0.0405757452148008, 1.1122868032803170, -0.0717110580655164},
                           {-0.0763729366746601, -0.4214933324022432, 1.5869240198367816}}};
constexpr Matrix XyzToLms{{{0.8190224379967030, 0.3619062600528904, -0.1288737815209879},
                           {0.0329836539323885, 0.9292868615863434, 0.0361446663506424},
                           {0.0481771893596242, 0.2642395317527308, 0.6335478284694309}}};
constexpr Matrix LmsToOklab{{{0.2104542683093140, 0.7936177747023054, -0.0040720430116193},
                             {1.9779985324311684, -2.4285922420485799, 0.4505937096174110},
                             {0.0259040424655478, 0.7827717124575296, -0.8086757549230774}}};
constexpr Matrix XyzToLinearSrgb{{{12831.0 / 3959, -329.0 / 214, -1974.0 / 3959},
                                  {-851781.0 / 878810, 1648619.0 / 878810, 36519.0 / 878810},
                                  {705.0 / 12673, -2585.0 / 12673, 705.0 / 667}}};
constexpr Matrix LinearSrgbToXyz{{{506752.0 / 1228815, 87881.0 / 245763, 12673.0 / 70218},
                                  {87098.0 / 409605, 175762.0 / 245763, 12673.0 / 175545},
                                  {7918.0 / 409605, 87881.0 / 737289, 1001167.0 / 1053270}}};
// Bradford chromatic adaptation from the D50 white of LAB to the D65 of sRGB.
constexpr Matrix D50ToD65{{{0.955473421488075, -0.02309845494876471, 0.06325924320057072},
                           {-0.0283697093338637, 1.0099953980813041, 0.021041441191917323},
                           {0.012314014864481998, -0.020507649298898964, 1.330365926242124}}};

Vec3 oklabToLinear(const Vec3& lab)
{
    Vec3 lms = multiply(OklabToLms, lab);
    for (double& v : lms) v = v * v * v;
    return multiply(XyzToLinearSrgb, multiply(LmsToXyz, lms));
}

Vec3 linearToOklab(const Vec3& rgb)
{
    Vec3 lms = multiply(XyzToLms, multiply(LinearSrgbToXyz, rgb));
    for (double& v : lms) v = std::cbrt(v);
    return multiply(LmsToOklab, lms);
}

Vec3 oklchToOklab(double l, double c, double h)
{
    const double radians = h * std::numbers::pi / 180.0;
    return {l, c * std::cos(radians), c * std::sin(radians)};
}

/// The sRGB transfer function, extended to negative values as CSS does.
Vec3 encode(Vec3 v)
{
    for (double& x : v) {
        const double magnitude = std::abs(x);
        const double encoded = magnitude > 0.0031308 ? 1.055 * std::pow(magnitude, 1 / 2.4) - 0.055 : 12.92 * magnitude;
        x = std::copysign(encoded, x);
    }
    return v;
}

Vec3 decode(Vec3 v)
{
    for (double& x : v) {
        const double magnitude = std::abs(x);
        const double decoded = magnitude > 0.04045 ? std::pow((magnitude + 0.055) / 1.055, 2.4) : magnitude / 12.92;
        x = std::copysign(decoded, x);
    }
    return v;
}

bool inGamut(const Vec3& srgb)
{
    // Rounding in the matrices puts a colour on the edge a hair outside.
    constexpr double Tolerance = 1e-9;
    for (double v : srgb) {
        if (v < -Tolerance || v > 1 + Tolerance) return false;
    }
    return true;
}

Vec3 clip(Vec3 srgb)
{
    for (double& v : srgb) v = std::clamp(v, 0.0, 1.0);
    return srgb;
}

double deltaEOK(const Vec3& a, const Vec3& b)
{
    return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

/// CSS Color 4 gamut mapping of an OKLCH colour into sRGB (encoded, 0-1).
Vec3 gamutMap(double l, double c, double h)
{
    if (l >= 1) return {1, 1, 1};
    if (l <= 0) return {0, 0, 0};
    const auto srgbAt = [&](double chroma) { return encode(oklabToLinear(oklchToOklab(l, chroma, h))); };
    const auto clippedDistance = [&](double chroma, const Vec3& current, Vec3& clipped) {
        clipped = clip(current);
        return deltaEOK(linearToOklab(decode(clipped)), oklchToOklab(l, chroma, h));
    };
    const Vec3 origin = srgbAt(c);
    if (inGamut(origin)) return clip(origin);
    constexpr double Jnd = 0.02;
    constexpr double Epsilon = 0.0001;
    Vec3 clipped;
    if (clippedDistance(c, origin, clipped) < Jnd) return clipped;
    double low = 0;
    double high = c;
    bool lowInGamut = true;
    while (high - low > Epsilon) {
        const double chroma = (low + high) / 2;
        const Vec3 current = srgbAt(chroma);
        if (lowInGamut && inGamut(current)) {
            low = chroma;
            continue;
        }
        const double distance = clippedDistance(chroma, current, clipped);
        if (distance < Jnd) {
            if (Jnd - distance < Epsilon) return clipped;
            lowInGamut = false;
            low = chroma;
        } else {
            high = chroma;
        }
    }
    return clipped;
}

void store(const Vec3& srgb, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b)
{
    const auto byte = [](double v) { return std::uint8_t(std::lround(std::clamp(v, 0.0, 1.0) * 255.0)); };
    r = byte(srgb[0]);
    g = byte(srgb[1]);
    b = byte(srgb[2]);
}

double finite(double v)
{
    return std::isfinite(v) ? v : 0.0;
}

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f';
}

char lower(char c)
{
    return c >= 'A' && c <= 'Z' ? char(c + 32) : c;
}

bool equalsLower(std::string_view text, std::string_view lowercase)
{
    if (text.size() != lowercase.size()) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (lower(text[i]) != lowercase[i]) return false;
    }
    return true;
}

enum class Unit { Number, Percent, Angle };

/// One value: a CSS number, with "%" or an angle unit after it, or "none".
bool parseValue(std::string_view token, double& value, Unit& unit)
{
    if (equalsLower(token, "none")) {
        value = 0;
        unit = Unit::Number;
        return true;
    }
    if (token.starts_with('+')) token.remove_prefix(1);
    const auto parsed = std::from_chars(token.data(), token.data() + token.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr == token.data() || !std::isfinite(value)) return false;
    const std::string_view suffix(parsed.ptr, std::size_t(token.data() + token.size() - parsed.ptr));
    unit = Unit::Number;
    if (suffix.empty()) return true;
    if (suffix == "%") {
        unit = Unit::Percent;
        return true;
    }
    unit = Unit::Angle;
    if (equalsLower(suffix, "deg")) return true;
    if (equalsLower(suffix, "grad")) value *= 0.9;
    else if (equalsLower(suffix, "rad")) value *= 180.0 / std::numbers::pi;
    else if (equalsLower(suffix, "turn")) value *= 360.0;
    else return false;
    return std::isfinite(value);
}

} // namespace

void oklchToSrgb(double l, double c, double h, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b)
{
    h = std::fmod(finite(h), 360.0);
    if (h < 0) h += 360.0;
    store(gamutMap(std::clamp(finite(l), 0.0, 1.0), std::max(finite(c), 0.0), h), r, g, b);
}

void labToSrgb(double l, double a, double bAxis, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b)
{
    l = std::clamp(finite(l), 0.0, 100.0);
    a = finite(a);
    bAxis = finite(bAxis);
    constexpr double Kappa = 24389.0 / 27;
    constexpr double Epsilon = 216.0 / 24389;
    const double f1 = (l + 16) / 116;
    const double f0 = a / 500 + f1;
    const double f2 = f1 - bAxis / 200;
    const double x = f0 * f0 * f0 > Epsilon ? f0 * f0 * f0 : (116 * f0 - 16) / Kappa;
    const double y = l > Kappa * Epsilon ? f1 * f1 * f1 : l / Kappa;
    const double z = f2 * f2 * f2 > Epsilon ? f2 * f2 * f2 : (116 * f2 - 16) / Kappa;
    constexpr Vec3 D50{0.3457 / 0.3585, 1.0, (1.0 - 0.3457 - 0.3585) / 0.3585};
    const Vec3 linear = multiply(XyzToLinearSrgb, multiply(D50ToD65, Vec3{x * D50[0], y * D50[1], z * D50[2]}));
    const Vec3 srgb = encode(linear);
    if (inGamut(srgb)) {
        store(srgb, r, g, b);
        return;
    }
    // Outside sRGB: the same mapping, from the colour's OKLCH.
    const Vec3 oklab = linearToOklab(linear);
    const double hue = std::atan2(oklab[2], oklab[1]) * 180.0 / std::numbers::pi;
    oklchToSrgb(oklab[0], std::hypot(oklab[1], oklab[2]), hue, r, g, b);
}

bool parseWideGamutColor(std::string_view spec, std::uint8_t& r, std::uint8_t& g, std::uint8_t& b)
{
    // Long enough for any spelling with spaces; a longer spec is no colour.
    if (spec.size() > 256) return false;
    while (!spec.empty() && isSpace(spec.front())) spec.remove_prefix(1);
    while (!spec.empty() && isSpace(spec.back())) spec.remove_suffix(1);
    const auto open = spec.find('(');
    if (open == std::string_view::npos || !spec.ends_with(')')) return false;
    const std::string_view name = spec.substr(0, open);
    const bool oklch = equalsLower(name, "oklch");
    if (!oklch && !equalsLower(name, "lab")) return false;
    const std::string_view body = spec.substr(open + 1, spec.size() - open - 2);
    // Three values apart by spaces or commas, as kitty reads them.
    std::array<double, 3> values{};
    std::array<Unit, 3> units{};
    std::size_t count = 0;
    std::size_t at = 0;
    while (at < body.size()) {
        if (isSpace(body[at]) || body[at] == ',') {
            ++at;
            continue;
        }
        std::size_t end = at;
        while (end < body.size() && !isSpace(body[end]) && body[end] != ',') ++end;
        // A fourth value, or the "/ alpha" of CSS, is no terminal colour.
        if (count == 3 || !parseValue(body.substr(at, end - at), values[count], units[count])) return false;
        ++count;
        at = end;
    }
    if (count != 3) return false;
    if (oklch) {
        // CSS: 100% of L is 1 and 100% of C is 0.4. A hue takes no percentage.
        if (units[0] == Unit::Angle || units[1] == Unit::Angle || units[2] == Unit::Percent) return false;
        const double l = units[0] == Unit::Percent ? values[0] / 100 : values[0];
        const double c = units[1] == Unit::Percent ? values[1] * 0.4 / 100 : values[1];
        oklchToSrgb(l, c, values[2], r, g, b);
        return true;
    }
    // CSS: 100% of L is 100, and 100% of a and b is 125.
    for (Unit unit : units) {
        if (unit == Unit::Angle) return false;
    }
    labToSrgb(values[0], units[1] == Unit::Percent ? values[1] * 1.25 : values[1],
              units[2] == Unit::Percent ? values[2] * 1.25 : values[2], r, g, b);
    return true;
}

} // namespace AfterglowProto
