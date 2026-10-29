// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/afterglowvt_export.h>

#include <cstdint>

namespace AfterglowVt::Unicode {

/// Grapheme_Cluster_Break values (UAX #29). Order matches tools/gen_unicode_tables.py.
enum class GraphemeBreak : std::uint8_t {
    Other,
    CR,
    LF,
    Control,
    Extend,
    ZWJ,
    RegionalIndicator,
    Prepend,
    SpacingMark,
    L,
    V,
    T,
    LV,
    LVT,
};

/// Indic_Conjunct_Break values, used by GB9c.
enum class ConjunctBreak : std::uint8_t {
    None,
    Linker,
    Consonant,
    Extend,
};

struct Props
{
    GraphemeBreak graphemeBreak = GraphemeBreak::Other;
    ConjunctBreak conjunctBreak = ConjunctBreak::None;
    bool extendedPictographic = false;
    /// Emoji with text-default presentation: VS16 (U+FE0F) widens it to two cells.
    bool widensWithVs16 = false;
    /// Terminal cell width: 0, 1 or 2.
    std::uint8_t width = 1;
};

inline constexpr char32_t VariationSelector16 = 0xFE0F;
inline constexpr char32_t ReplacementCharacter = 0xFFFD;

AFTERGLOWVT_EXPORT Props props(char32_t cp) noexcept;
AFTERGLOWVT_EXPORT int width(char32_t cp) noexcept;
AFTERGLOWVT_EXPORT const char* ucdVersion() noexcept;

/// Incremental grapheme cluster boundary detection (UAX #29, extended clusters).
/// Feed codepoints in order; each call reports whether a cluster boundary sits
/// immediately before that codepoint.
class AFTERGLOWVT_EXPORT GraphemeSegmenter
{
public:
    bool breaksBefore(char32_t cp) noexcept;
    bool breaksBefore(const Props& p) noexcept;
    void reset() noexcept;
    /// Whether printable ASCII (grapheme class Other) fed next would join the
    /// current cluster: only after a Prepend (GB9b).
    bool joinsAscii() const noexcept { return m_started && m_prev == GraphemeBreak::Prepend; }

private:
    enum class EmojiState : std::uint8_t { None, Pictographic, PictographicZwj };
    enum class ConjunctState : std::uint8_t { None, Consonant, ConsonantLinker };

    bool m_started = false;
    bool m_riOdd = false;
    GraphemeBreak m_prev = GraphemeBreak::Other;
    EmojiState m_emoji = EmojiState::None;
    ConjunctState m_conjunct = ConjunctState::None;
};

} // namespace AfterglowVt::Unicode
