// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Unicode.h>

#include <cstdint>

namespace AfterglowVt::Unicode {

namespace {
#include "UnicodeTables.inc"

// Packed word layout, written by tools/gen_unicode_tables.py:
//   [3:0] Grapheme_Cluster_Break   [4] Extended_Pictographic
//   [6:5] InCB                     [8:7] width   [9] widens with VS16
//   [10] East Asian Ambiguous
constexpr std::uint16_t packedFor(char32_t cp) noexcept
{
    if (cp >= 0x110000) {
        return PropValues[Stage2[Stage1[0xFFFD >> 8] * 256 + (0xFFFD & 0xFF)]];
    }
    return PropValues[Stage2[Stage1[cp >> 8] * 256 + (cp & 0xFF)]];
}
} // namespace

Props props(char32_t cp) noexcept
{
    if (cp >= 0x20 && cp < 0x7F) {
        Props p;
        p.widensWithVs16 = cp == U'#' || cp == U'*' || (cp >= U'0' && cp <= U'9');
        return p;
    }
    const std::uint16_t v = packedFor(cp);
    Props p;
    p.graphemeBreak = static_cast<GraphemeBreak>(v & 0xF);
    p.extendedPictographic = (v >> 4) & 1;
    p.conjunctBreak = static_cast<ConjunctBreak>((v >> 5) & 3);
    p.width = static_cast<std::uint8_t>((v >> 7) & 3);
    p.widensWithVs16 = (v >> 9) & 1;
    p.ambiguous = (v >> 10) & 1;
    return p;
}

const char* ucdVersion() noexcept
{
    return UcdVersion;
}

bool GraphemeSegmenter::breaksBefore(char32_t cp) noexcept
{
    return breaksBefore(props(cp));
}

bool GraphemeSegmenter::breaksBefore(const Props& p) noexcept
{
    using GB = GraphemeBreak;
    const GB cur = p.graphemeBreak;

    bool brk = true;
    if (m_started) {
        const GB prev = m_prev;
        if (prev == GB::CR && cur == GB::LF) {
            brk = false; // GB3
        } else if (prev == GB::CR || prev == GB::LF || prev == GB::Control) {
            brk = true; // GB4
        } else if (cur == GB::CR || cur == GB::LF || cur == GB::Control) {
            brk = true; // GB5
        } else if (prev == GB::L && (cur == GB::L || cur == GB::V || cur == GB::LV || cur == GB::LVT)) {
            brk = false; // GB6
        } else if ((prev == GB::LV || prev == GB::V) && (cur == GB::V || cur == GB::T)) {
            brk = false; // GB7
        } else if ((prev == GB::LVT || prev == GB::T) && cur == GB::T) {
            brk = false; // GB8
        } else if (cur == GB::Extend || cur == GB::ZWJ) {
            brk = false; // GB9
        } else if (cur == GB::SpacingMark) {
            brk = false; // GB9a
        } else if (prev == GB::Prepend) {
            brk = false; // GB9b
        } else if (m_conjunct == ConjunctState::ConsonantLinker && p.conjunctBreak == ConjunctBreak::Consonant) {
            brk = false; // GB9c
        } else if (m_emoji == EmojiState::PictographicZwj && p.extendedPictographic) {
            brk = false; // GB11
        } else if (prev == GB::RegionalIndicator && cur == GB::RegionalIndicator && m_riOdd) {
            brk = false; // GB12, GB13
        }
    }

    // Advance the multi-codepoint rule states.
    if (p.extendedPictographic) {
        m_emoji = EmojiState::Pictographic;
    } else if (m_emoji == EmojiState::Pictographic && cur == GB::Extend) {
        // Extend* keeps the sequence alive.
    } else if (m_emoji == EmojiState::Pictographic && cur == GB::ZWJ) {
        m_emoji = EmojiState::PictographicZwj;
    } else {
        m_emoji = EmojiState::None;
    }

    switch (p.conjunctBreak) {
    case ConjunctBreak::Consonant:
        m_conjunct = ConjunctState::Consonant;
        break;
    case ConjunctBreak::Linker:
        if (m_conjunct != ConjunctState::None) {
            m_conjunct = ConjunctState::ConsonantLinker;
        }
        break;
    case ConjunctBreak::Extend:
        break;
    case ConjunctBreak::None:
        m_conjunct = ConjunctState::None;
        break;
    }

    if (cur == GB::RegionalIndicator) {
        m_riOdd = (m_started && m_prev == GB::RegionalIndicator) ? !m_riOdd : true;
    } else {
        m_riOdd = false;
    }

    m_prev = cur;
    m_started = true;
    return brk;
}

void GraphemeSegmenter::reset() noexcept
{
    *this = GraphemeSegmenter{};
}

} // namespace AfterglowVt::Unicode
