// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/TextSearch.h>

#include <AfterglowVt/Screen.h>

#define PCRE2_CODE_UNIT_WIDTH 32
#include <pcre2.h>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>

namespace AfterglowVt {

namespace {

using Clock = std::chrono::steady_clock;
using Code = std::unique_ptr<pcre2_code, decltype(&pcre2_code_free)>;
using MatchData = std::unique_ptr<pcre2_match_data, decltype(&pcre2_match_data_free)>;

constexpr std::size_t MaxPattern = 4096;
constexpr auto TimeBudget = std::chrono::milliseconds(500);
// The callout runs at every pattern item at every start position.
constexpr unsigned CalloutsPerClockRead = 1024;

struct Budget
{
    const std::atomic<bool>& cancelled;
    Clock::time_point until;
    unsigned calls = 0;
    bool timedOut = false;
};

int checkBudget(pcre2_callout_block*, void* user)
{
    auto& budget = *static_cast<Budget*>(user);
    if (budget.cancelled.load(std::memory_order_relaxed)) {
        return PCRE2_ERROR_CALLOUT;
    }
    if (++budget.calls % CalloutsPerClockRead == 0 && Clock::now() >= budget.until) {
        budget.timedOut = true;
        return PCRE2_ERROR_CALLOUT;
    }
    return 0;
}

/// The position after the first @p close at or after @p from.
std::size_t skipPast(std::u32string_view s, std::size_t from, char32_t close)
{
    const auto at = s.find(close, from);
    return at == std::u32string_view::npos ? s.size() : at + 1;
}

/// Whether a Unicode property or POSIX class name is a case: Lu, Ll, Lt,
/// Uppercase, lower and their long and loose spellings (gc=Lu, ^Upper).
bool isCaseName(std::u32string_view name)
{
    if (const auto eq = name.find_first_of(U"=:"); eq != std::u32string_view::npos) {
        name.remove_prefix(eq + 1);
    }
    std::string folded;
    for (char32_t c : name) {
        if (c >= U'A' && c <= U'Z') {
            folded += char(c - U'A' + U'a');
        } else if (c >= U'a' && c <= U'z') {
            folded += char(c);
        } else if (c != U' ' && c != U'_' && c != U'-' && c != U'^') {
            return false;
        }
    }
    static constexpr std::string_view Names[] = {"lu",        "ll",        "lt",    "uppercaseletter", "lowercaseletter",
                                                 "titlecaseletter", "uppercase", "lowercase", "upper", "lower"};
    return std::find(std::begin(Names), std::end(Names), folded) != std::end(Names);
}

struct PatternText
{
    /// The characters the pattern matches as text.
    std::u32string literal;
    /// The pattern uses a class that names a case (\p{Lu}, [:lower:]).
    bool namesCase = false;
};

/// What smart case reads in @p pattern. In a regular expression, escapes
/// (\S, \p{Lu}, \x{FF}, \cA), group names, inline options and verbs ((?U),
/// (*UTF)) are syntax: their letters say nothing about the case the user
/// wants. A class that names a case does: caseless matching turns \p{Lu}
/// into any cased letter (PCRE2 follows Perl there), so it must keep case.
PatternText patternText(std::u32string_view pattern, bool regex)
{
    if (!regex) {
        return {std::u32string(pattern)};
    }
    PatternText result;
    std::u32string& out = result.literal;
    std::size_t i = 0;
    while (i < pattern.size()) {
        const char32_t c = pattern[i];
        if (c == U'\\' && i + 1 < pattern.size()) {
            const char32_t escape = pattern[i + 1];
            i += 2;
            if (escape == U'Q') {
                // \Q...\E quotes text.
                const auto end = pattern.find(U"\\E", i);
                const std::size_t stop = end == std::u32string_view::npos ? pattern.size() : end;
                out.append(pattern.substr(i, stop - i));
                i = std::min(pattern.size(), stop + 2);
            } else if (escape == U'c') {
                ++i; // \cX: a control character
            } else if (i < pattern.size() && (pattern[i] == U'{' || pattern[i] == U'<' || pattern[i] == U'\'')) {
                // An argument: \p{Lu}, \x{FF}, \N{U+41}, \k<Name>, \g'Name'.
                const char32_t open = pattern[i];
                const std::size_t end = skipPast(pattern, i + 1, open == U'{' ? U'}' : open == U'<' ? U'>' : U'\'');
                if ((escape == U'p' || escape == U'P') && open == U'{') {
                    result.namesCase |= isCaseName(pattern.substr(i + 1, end - i - 2));
                }
                i = end;
            }
            continue;
        }
        if (c == U'[' && i + 1 < pattern.size() && pattern[i + 1] == U':') {
            // A POSIX class: [:upper:], [:^lower:].
            const auto close = pattern.find(U":]", i + 2);
            if (close != std::u32string_view::npos) {
                result.namesCase |= isCaseName(pattern.substr(i + 2, close - i - 2));
                i = close + 2;
                continue;
            }
        }
        if (c == U'(' && i + 1 < pattern.size() && pattern[i + 1] == U'*') {
            i = skipPast(pattern, i + 2, U')'); // a verb: (*UTF), (*SKIP)
            continue;
        }
        if (c == U'(' && i + 1 < pattern.size() && pattern[i + 1] == U'?') {
            i += 2;
            const char32_t next = i < pattern.size() ? pattern[i] : U'\0';
            const char32_t after = i + 1 < pattern.size() ? pattern[i + 1] : U'\0';
            if (next == U'P' && after == U'<') {
                i = skipPast(pattern, i + 2, U'>'); // (?P<Name>
            } else if (next == U'P' || next == U'&' || next == U'C') {
                i = skipPast(pattern, i, U')'); // (?P=Name), (?P>Name), (?&Name), (?C"text")
            } else if (next == U'<' && after != U'=' && after != U'!') {
                i = skipPast(pattern, i + 1, U'>'); // (?<Name>, not a lookbehind
            } else if (next == U'\'') {
                i = skipPast(pattern, i + 1, U'\''); // (?'Name'
            } else {
                // Inline options: (?i), (?U), (?-x:...).
                while (i < pattern.size()
                       && ((pattern[i] >= U'a' && pattern[i] <= U'z') || (pattern[i] >= U'A' && pattern[i] <= U'Z')
                           || pattern[i] == U'-' || pattern[i] == U'^')) {
                    ++i;
                }
            }
            continue;
        }
        out += c;
        ++i;
    }
    return result;
}

/// Unicode upper or title case, the same in every locale (std::towlower is
/// not: under LANG=C it knows ASCII only).
bool hasUpperCase(std::u32string_view text)
{
    static constexpr char32_t Upper[] = U"[\\p{Lu}\\p{Lt}]";
    int error = 0;
    PCRE2_SIZE offset = 0;
    const Code code(pcre2_compile(reinterpret_cast<PCRE2_SPTR>(Upper), PCRE2_ZERO_TERMINATED, PCRE2_UTF | PCRE2_UCP,
                                  &error, &offset, nullptr),
                    pcre2_code_free);
    if (!code) {
        return false;
    }
    const MatchData data(pcre2_match_data_create_from_pattern(code.get(), nullptr), pcre2_match_data_free);
    return data
        && pcre2_match(code.get(), reinterpret_cast<PCRE2_SPTR>(text.data()), text.size(), 0, 0, data.get(), nullptr) >= 0;
}

void fail(TextSearchResult& result, SearchError kind, std::size_t position = 0)
{
    result.errorKind = kind;
    result.errorPosition = position;
    switch (kind) {
    case SearchError::None:
        result.error.clear();
        break;
    case SearchError::PatternTooLong:
        result.error = "Pattern exceeds 4096 characters";
        break;
    case SearchError::NoMemory:
        result.error = "Cannot allocate search context";
        break;
    case SearchError::TimeLimit:
        result.error = "Search time limit reached";
        break;
    case SearchError::ComplexityLimit:
        result.error = "Search complexity limit reached";
        break;
    case SearchError::InvalidPattern:
        result.error = "Invalid regular expression at character " + std::to_string(position);
        break;
    }
}

} // namespace

bool appendTextRow(TextSnapshot& out, const Screen& screen, std::size_t index, std::size_t maxCharacters)
{
    if (index >= screen.totalRows()) {
        return false;
    }
    const std::uint64_t id = screen.rowsDropped() + index;
    if (!out.wrapped || out.lines.empty() || id != out.nextRow) {
        out.lines.emplace_back();
    }
    TextLine& line = out.lines.back();
    const Row& row = screen.absoluteRow(index);
    // A double-width or double-height row shows its left half only.
    const int visible = row.lineSize == LineSize::Single ? screen.cols() : screen.cols() / 2;
    int columns = visible;
    if (!row.wrapped) {
        while (columns > 0) {
            const Cell& last = row.at(columns - 1);
            if ((!last.isEmpty() && last.codepoint() != U' ') || last.isTail()
                || !screen.graphemeExtra(row, columns - 1).empty()) {
                break;
            }
            --columns;
        }
    }
    for (int x = 0; x < columns; ++x) {
        const Cell& cell = row.at(x);
        if (cell.isTail() || cell.isSpacerHead() || (cell.isMulticell() && (cell.multicellDx() || cell.multicellDy()))) {
            continue;
        }
        const auto extra = screen.graphemeExtra(row, x);
        if (out.characters + 1 + extra.size() > maxCharacters) {
            out.truncated = true;
            return false;
        }
        // A match covers every column of its last character. A multicell
        // block's rows below this one are not covered: a match is one run
        // of cells.
        int last = x;
        if (cell.isMulticell()) {
            if (const MulticellParams* params = screen.multicellParams(row, x)) {
                last = x + params->columns() - 1;
            }
        } else if (x + 1 < visible && row.at(x + 1).isTail()) {
            last = x + 1;
        }
        const TextCell at{id, x, std::min(last, visible - 1)};
        line.text += cell.isEmpty() ? U' ' : cell.codepoint();
        line.cells.push_back(at);
        for (char32_t cp : extra) {
            line.text += cp;
            line.cells.push_back(at);
        }
        out.characters += 1 + extra.size();
    }
    out.wrapped = row.wrapped;
    out.nextRow = id + 1;
    return true;
}

TextSearchResult searchText(const TextSnapshot& text, std::u32string_view pattern, bool regex,
                            const std::atomic<bool>& cancelled, std::size_t limit, bool smartCase)
{
    TextSearchResult result;
    result.truncated = text.truncated;
    if (pattern.empty()) {
        return result;
    }
    if (pattern.size() > MaxPattern) {
        fail(result, SearchError::PatternTooLong);
        return result;
    }
    std::u32string expression;
    expression.reserve(pattern.size());
    for (char32_t c : pattern) {
        if (!regex && std::u32string_view(U"\\.^$|()[]{}*+?").find(c) != std::u32string_view::npos) {
            expression += U'\\';
        }
        expression += c;
    }
    const PatternText syntax = patternText(pattern, regex);
    const bool caseless = smartCase && !syntax.namesCase && !hasUpperCase(syntax.literal);

    const std::unique_ptr<pcre2_compile_context, decltype(&pcre2_compile_context_free)> compile(
        pcre2_compile_context_create(nullptr), pcre2_compile_context_free);
    if (!compile) {
        fail(result, SearchError::NoMemory);
        return result;
    }
    pcre2_set_parens_nest_limit(compile.get(), 64);
    int error = 0;
    PCRE2_SIZE offset = 0;
    const Code code(pcre2_compile(reinterpret_cast<PCRE2_SPTR>(expression.data()), expression.size(),
                                  PCRE2_UTF | PCRE2_UCP | PCRE2_AUTO_CALLOUT | (caseless ? PCRE2_CASELESS : 0), &error,
                                  &offset, compile.get()),
                    pcre2_code_free);
    if (!code) {
        // A syntax error's offset is just past the item at fault, which
        // counted from 1 is that item's last character. A UTF error (a
        // negative code) gives the bad character's own 0-based index.
        const PCRE2_SIZE position = error < 0 ? offset + 1 : std::max<PCRE2_SIZE>(offset, 1);
        fail(result, SearchError::InvalidPattern, position);
        return result;
    }
    const MatchData data(pcre2_match_data_create_from_pattern(code.get(), nullptr), pcre2_match_data_free);
    const std::unique_ptr<pcre2_match_context, decltype(&pcre2_match_context_free)> context(
        pcre2_match_context_create(nullptr), pcre2_match_context_free);
    if (!data || !context) {
        fail(result, SearchError::NoMemory);
        return result;
    }
    pcre2_set_match_limit(context.get(), 50000);
    pcre2_set_depth_limit(context.get(), 1024);
    pcre2_set_heap_limit(context.get(), 8192);
    Budget budget{cancelled, Clock::now() + TimeBudget};
    pcre2_set_callout(context.get(), checkBudget, &budget);

    for (const TextLine& line : text.lines) {
        const auto* subject = reinterpret_cast<PCRE2_SPTR>(line.text.data());
        // The first match on a line checks its UTF. Checking again at every
        // match would cost the rest of the line each time.
        std::uint32_t options = 0;
        PCRE2_SIZE start = 0;
        while (start <= line.text.size()) {
            if (cancelled.load()) {
                result.cancelled = true;
                return result;
            }
            if (Clock::now() >= budget.until) {
                fail(result, SearchError::TimeLimit);
                return result;
            }
            const int rc = pcre2_match(code.get(), subject, line.text.size(), start, options, data.get(), context.get());
            options = PCRE2_NO_UTF_CHECK;
            if (rc == PCRE2_ERROR_NOMATCH) {
                break;
            }
            if (rc < 0) {
                result.cancelled = cancelled.load();
                if (!result.cancelled) {
                    fail(result, budget.timedOut ? SearchError::TimeLimit : SearchError::ComplexityLimit);
                }
                return result;
            }
            const PCRE2_SIZE* positions = pcre2_get_ovector_pointer(data.get());
            if (positions[1] > positions[0]) {
                result.matches.push_back({line.cells[positions[0]].start(), line.cells[positions[1] - 1].end()});
                if (result.matches.size() >= limit) {
                    result.truncated = true;
                    return result;
                }
            }
            start = positions[1] > start ? positions[1] : start + 1;
        }
    }
    return result;
}

} // namespace AfterglowVt
