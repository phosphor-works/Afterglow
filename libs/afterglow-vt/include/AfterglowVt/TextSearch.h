// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#pragma once
#include <AfterglowVt/Search.h>
#include <atomic>
#include <string>

namespace AfterglowVt {
/// A logical line, with codepoints mapped to complete grapheme cells.
struct TextLine {
    std::u32string text;
    std::vector<SearchMatch> cells;
};
struct TextSnapshot {
    std::vector<TextLine> lines;
    std::size_t characters = 0;
    bool wrapped = false;
    bool truncated = false;
};
/// Append a physical row. Call in bounded batches while holding the screen lock.
AFTERGLOWVT_EXPORT bool appendTextRow(TextSnapshot&, const Screen&, std::size_t row,
                                      std::size_t maxCharacters = 4 * 1024 * 1024);
struct TextSearchResult {
    std::vector<SearchMatch> matches;
    std::string error;
    bool truncated = false;
    bool cancelled = false;
};
/// Bounded PCRE2 search, suitable for a worker thread. Empty matches are skipped.
AFTERGLOWVT_EXPORT TextSearchResult searchText(const TextSnapshot&, std::u32string_view pattern,
    bool regex, const std::atomic<bool>& cancelled, std::size_t limit = 10000);
}
