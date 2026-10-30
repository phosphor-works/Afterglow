// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later
#include <AfterglowVt/TextSearch.h>
#include <AfterglowVt/Screen.h>
#define PCRE2_CODE_UNIT_WIDTH 32
#include <pcre2.h>
#include <algorithm>
#include <chrono>
#include <cwctype>
#include <memory>

namespace AfterglowVt {
bool appendTextRow(TextSnapshot& out, const Screen& screen, std::size_t index, std::size_t maxCharacters)
{
    if (index >= screen.totalRows()) return false;
    if (!out.wrapped || out.lines.empty()) out.lines.emplace_back();
    auto& line = out.lines.back();
    const auto& row = screen.absoluteRow(index);
    int columns = screen.cols();
    if (!row.wrapped) {
        while (columns > 0) {
            const auto& last = row.at(columns - 1);
            if ((!last.isEmpty() && last.codepoint() != U' ') || last.isTail()
                || !screen.graphemeExtra(row, columns - 1).empty()) break;
            --columns;
        }
    }
    for (int x = 0; x < columns; ++x) {
        const auto& cell = row.at(x);
        if (cell.isTail() || cell.isSpacerHead() || (cell.isMulticell() && (cell.multicellDx() || cell.multicellDy()))) continue;
        const auto extra = screen.graphemeExtra(row, x);
        if (out.characters + 1 + extra.size() > maxCharacters) { out.truncated = true; return false; }
        const SelectionPoint start{screen.rowsDropped() + index, x};
        const SelectionPoint end{start.row, x + (x + 1 < screen.cols() && row.at(x + 1).isTail() ? 1 : 0)};
        line.text += cell.isEmpty() ? U' ' : cell.codepoint();
        line.cells.push_back({start, end});
        for (char32_t cp : extra) { line.text += cp; line.cells.push_back({start, end}); }
        out.characters += 1 + extra.size();
    }
    out.wrapped = row.wrapped;
    return true;
}
TextSearchResult searchText(const TextSnapshot& text, std::u32string_view pattern, bool regex,
                           const std::atomic<bool>& cancelled, std::size_t limit)
{
    TextSearchResult result;
    result.truncated = text.truncated;
    if (pattern.empty()) return result;
    if (pattern.size() > 4096) { result.error = "Pattern exceeds 4096 characters"; return result; }
    std::u32string expression;
    for (char32_t c : pattern) {
        if (!regex && std::u32string_view(U"\\.^$|()[]{}*+?").find(c) != std::u32string_view::npos) expression += U'\\';
        expression += c;
    }
    using Clock = std::chrono::steady_clock;
    struct Budget { const std::atomic<bool>& cancelled; Clock::time_point until; } budget{cancelled, Clock::now() + std::chrono::milliseconds(500)};
    int error = 0; PCRE2_SIZE offset = 0;
    auto compile = std::unique_ptr<pcre2_compile_context, decltype(&pcre2_compile_context_free)>(pcre2_compile_context_create(nullptr), pcre2_compile_context_free);
    if (!compile) { result.error = "Cannot allocate search context"; return result; }
    pcre2_set_parens_nest_limit(compile.get(), 64);
    const bool upper = std::any_of(pattern.begin(), pattern.end(), [](char32_t c) { return std::towlower(wint_t(c)) != wint_t(c); });
    auto code = std::unique_ptr<pcre2_code, decltype(&pcre2_code_free)>(pcre2_compile(
        reinterpret_cast<PCRE2_SPTR>(expression.data()), expression.size(),
        PCRE2_UTF | PCRE2_UCP | PCRE2_AUTO_CALLOUT | (upper ? 0 : PCRE2_CASELESS), &error, &offset, compile.get()), pcre2_code_free);
    if (!code) { result.error = "Invalid regular expression at character " + std::to_string(offset); return result; }
    auto data = std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)>(pcre2_match_data_create_from_pattern(code.get(), nullptr), pcre2_match_data_free);
    auto context = std::unique_ptr<pcre2_match_context, decltype(&pcre2_match_context_free)>(pcre2_match_context_create(nullptr), pcre2_match_context_free);
    if (!data || !context) { result.error = "Cannot allocate search context"; return result; }
    pcre2_set_match_limit(context.get(), 50000);
    pcre2_set_depth_limit(context.get(), 1024);
    pcre2_set_heap_limit(context.get(), 8192);
    pcre2_set_callout(context.get(), [](pcre2_callout_block*, void* user) {
        const auto& b = *static_cast<Budget*>(user);
        return b.cancelled.load() || Clock::now() >= b.until ? PCRE2_ERROR_CALLOUT : 0;
    }, &budget);
    for (const auto& line : text.lines) {
        PCRE2_SIZE start = 0;
        while (start <= line.text.size()) {
            if (cancelled.load()) { result.cancelled = true; return result; }
            if (Clock::now() >= budget.until) { result.error = "Search time limit reached"; return result; }
            const int rc = pcre2_match(code.get(), reinterpret_cast<PCRE2_SPTR>(line.text.data()), line.text.size(), start, 0, data.get(), context.get());
            if (rc == PCRE2_ERROR_NOMATCH) break;
            if (rc < 0) {
                result.cancelled = cancelled.load();
                if (!result.cancelled) result.error = "Search complexity limit reached";
                return result;
            }
            const auto* positions = pcre2_get_ovector_pointer(data.get());
            if (positions[1] > positions[0]) {
                result.matches.push_back({line.cells[positions[0]].start, line.cells[positions[1] - 1].end});
                if (result.matches.size() >= limit) { result.truncated = true; return result; }
            }
            start = positions[1] > start ? positions[1] : start + 1;
        }
    }
    return result;
}
}
