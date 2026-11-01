// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <AfterglowVt/Search.h>
#include <AfterglowVt/afterglowvt_export.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace AfterglowVt {

/// The cells that one code point of a TextLine comes from: a row and a run
/// of columns, more than one for a wide character or a multicell block.
struct TextCell
{
    std::uint64_t row = 0; ///< stable row id
    std::int32_t col = 0;
    std::int32_t lastCol = 0;

    SelectionPoint start() const { return {row, col}; }
    SelectionPoint end() const { return {row, lastCol}; }
};

/// A logical line: the text of soft-wrapped rows, each code point mapped to
/// its cells.
struct TextLine
{
    std::u32string text;
    std::vector<TextCell> cells;
};

struct TextSnapshot
{
    std::vector<TextLine> lines;
    std::size_t characters = 0;
    std::uint64_t nextRow = 0; ///< the row id that continues the last line
    bool wrapped = false;
    bool truncated = false;
};

/// Appends the row at @p index (0 is the oldest history row; not a row id).
/// Call in bounded batches while holding the screen lock. Only the row that
/// directly follows a soft-wrapped row continues its line, since history may
/// drop rows between two batches. Returns false at the end of the screen or
/// when @p maxCharacters is reached.
AFTERGLOWVT_EXPORT bool appendTextRow(TextSnapshot& out, const Screen& screen, std::size_t index,
                                      std::size_t maxCharacters = 4 * 1024 * 1024);

/// Why a search stopped. Callers translate this, not the English text.
enum class SearchError : std::uint8_t {
    None,
    PatternTooLong, ///< over 4096 characters
    NoMemory,
    TimeLimit,
    ComplexityLimit,
    InvalidPattern, ///< at TextSearchResult::errorPosition
};

struct TextSearchResult
{
    std::vector<SearchMatch> matches;
    SearchError errorKind = SearchError::None;
    std::size_t errorPosition = 0; ///< 1-based character of an InvalidPattern
    std::string error; ///< errorKind in English, for logs
    bool truncated = false;
    bool cancelled = false;
};

/// Bounded PCRE2 search, suitable for a worker thread. Empty matches are
/// skipped. With @p smartCase, a pattern with no upper-case letter (regex
/// syntax such as \S or \p{Lu} does not count) ignores case; without it,
/// case always matters, and a pattern can ask for (?i).
AFTERGLOWVT_EXPORT TextSearchResult searchText(const TextSnapshot& text, std::u32string_view pattern, bool regex,
                                               const std::atomic<bool>& cancelled, std::size_t limit = 10000,
                                               bool smartCase = true);

} // namespace AfterglowVt
