// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "TestUtil.h"

#include <AfterglowVt/Search.h>
#include <AfterglowVt/TextSearch.h>

#include <gtest/gtest.h>

using namespace AfterglowVt;
using namespace AfterglowVt::Test;

namespace {
struct Term
{
    RecordingHost host;
    Terminal t;
    Term(int cols, int rows)
        : t(cols, rows, host)
    {
    }
};

TextSnapshot snapshotOf(const Screen& screen)
{
    TextSnapshot snapshot;
    for (std::size_t i = 0; i < screen.totalRows(); ++i) {
        appendTextRow(snapshot, screen, i);
    }
    return snapshot;
}

std::vector<SearchMatch> search(const Term& t, std::u32string_view pattern, bool regex = false, bool smartCase = true)
{
    const std::atomic<bool> cancelled{false};
    const TextSearchResult result = searchText(snapshotOf(t.t.screen()), pattern, regex, cancelled, 10000, smartCase);
    EXPECT_TRUE(result.error.empty()) << result.error;
    return result.matches;
}
} // namespace

TEST(Search, FindsAllOccurrences)
{
    Term t(20, 4);
    t.t.feed("foo bar foo\r\nbaz foo");
    const auto m = search(t, U"foo");
    ASSERT_EQ(m.size(), 3u);
    EXPECT_EQ(m[1].start.col, 8);
    EXPECT_EQ(m[1].end.col, 10);
    EXPECT_EQ(m[2].start.row, m[0].start.row + 1);
    // findAll is the same search over the whole screen.
    EXPECT_EQ(findAll(t.t.screen(), U"foo").size(), 3u);
}

TEST(Search, SmartCase)
{
    Term t(20, 2);
    t.t.feed("Error error ERROR");
    EXPECT_EQ(search(t, U"error").size(), 3u);
    EXPECT_EQ(search(t, U"Error").size(), 1u);
}

TEST(Search, SmartCaseIsTheSameInEveryLocale)
{
    // The tests run in the C locale, where std::towlower knows ASCII only.
    Term t(20, 2);
    t.t.feed("\xc3\xa4rger \xc3\x84rger");
    EXPECT_EQ(search(t, U"Ärger").size(), 1u);
    EXPECT_EQ(search(t, U"ärger").size(), 2u);
}

TEST(Search, SmartCaseSkipsRegexSyntax)
{
    Term t(30, 2);
    t.t.feed("12xFOO (bar) A");
    // \S, \D and a named group are syntax, not upper-case text.
    EXPECT_EQ(search(t, U"\\d+\\Sfoo", true).size(), 1u);
    EXPECT_EQ(search(t, U"(?<Name>bar)", true).size(), 1u);
    EXPECT_EQ(search(t, U"\\(\\D\\D\\D\\)", true).size(), 1u);
    // A class that names a case keeps it: caseless, it would match any letter.
    EXPECT_EQ(search(t, U"\\p{Lu}", true).size(), 4u);
    EXPECT_EQ(search(t, U"[[:lower:]]", true).size(), 4u);
    EXPECT_EQ(search(t, U"\\p{L}", true).size(), 8u);
    // A literal upper-case letter still makes case matter.
    EXPECT_EQ(search(t, U"\\d+\\SFoo", true).size(), 0u);
}

TEST(Search, WithoutSmartCaseCaseAlwaysMatters)
{
    Term t(20, 2);
    t.t.feed("ERROR");
    EXPECT_EQ(search(t, U"error", true, false).size(), 0u);
    EXPECT_EQ(search(t, U"(?i)error", true, false).size(), 1u);
}

TEST(Search, AcrossSoftWrap)
{
    Term t(5, 3);
    t.t.feed("abcdefghij");
    const auto m = search(t, U"defg");
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m[0].start.col, 3);
    EXPECT_EQ(m[0].end.row, m[0].start.row + 1);
    EXPECT_EQ(m[0].end.col, 1);
}

TEST(Search, IncludesHistory)
{
    Term t(10, 2);
    t.t.feed("needle\r\nx\r\ny\r\nz");
    const auto m = search(t, U"needle");
    ASSERT_EQ(m.size(), 1u);
    EXPECT_LT(t.t.screen().absoluteIndexOf(m[0].start.row), (long long)t.t.screen().historySize());
}

TEST(Search, ContainsIsInclusiveAtBothEnds)
{
    const SearchMatch m{{10, 3}, {11, 2}};
    EXPECT_FALSE(contains(m, 10, 2));
    EXPECT_TRUE(contains(m, 10, 3));
    EXPECT_TRUE(contains(m, 10, 79)); // the rest of the first row
    EXPECT_TRUE(contains(m, 11, 0));
    EXPECT_TRUE(contains(m, 11, 2));
    EXPECT_FALSE(contains(m, 11, 3));
    EXPECT_FALSE(contains(m, 9, 5));
    EXPECT_FALSE(contains(m, 12, 0));
}

TEST(TextSearch, RegexSpansSoftWrapsAndPreservesWideGraphemes)
{
    Term t(6, 4);
    t.t.feed("ab\xe7\x95\x8c" "e\xcc\x81" "fghi");
    auto m = search(t, U"界e\\x{301}fgh", true);
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m[0].start.col, 2);
    EXPECT_EQ(m[0].end.row, m[0].start.row + 1);
    m = search(t, U"界");
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m[0].end.col, 3);
}

TEST(TextSearch, InvalidComplexCancelledAndBoundedQueries)
{
    Term t(80, 3);
    t.t.feed(std::string(75, 'a') + "!");
    const TextSnapshot snapshot = snapshotOf(t.t.screen());
    std::atomic<bool> cancelled{false};
    // Character positions count from 1 and name the character at fault.
    EXPECT_EQ(searchText(snapshot, U"a)", true, cancelled).error, "Invalid regular expression at character 2");
    EXPECT_EQ(searchText(snapshot, U"*a", true, cancelled).error, "Invalid regular expression at character 1");
    const char32_t surrogate[] = {U'a', char32_t(0xD800), 0};
    EXPECT_EQ(searchText(snapshot, surrogate, true, cancelled).error, "Invalid regular expression at character 2");
    EXPECT_EQ(searchText(snapshot, U"(a+)+$", true, cancelled).error, "Search complexity limit reached");
    // Callers translate the kind and the position, not the English text.
    const TextSearchResult invalid = searchText(snapshot, U"a)", true, cancelled);
    EXPECT_EQ(invalid.errorKind, SearchError::InvalidPattern);
    EXPECT_EQ(invalid.errorPosition, 2u);
    EXPECT_EQ(searchText(snapshot, U"(a+)+$", true, cancelled).errorKind, SearchError::ComplexityLimit);
    EXPECT_EQ(searchText(snapshot, std::u32string(4097, U'a'), true, cancelled).errorKind, SearchError::PatternTooLong);
    EXPECT_EQ(searchText(snapshot, U"a", true, cancelled).errorKind, SearchError::None);
    EXPECT_TRUE(searchText(snapshot, U"a", true, cancelled, 3).truncated);
    cancelled = true;
    EXPECT_TRUE(searchText(snapshot, U"a", true, cancelled).cancelled);
    TextSnapshot bounded;
    EXPECT_FALSE(appendTextRow(bounded, t.t.screen(), 0, 10));
    EXPECT_TRUE(bounded.truncated);
    EXPECT_EQ(bounded.characters, 10u);
}

TEST(TextSearch, DroppedHistoryEndsTheLineBetweenBatches)
{
    // Rows A, B and C are soft-wrapped; A and B are history.
    Term t(5, 2);
    t.t.feed("AAAAABBBBBCCCCCDD");
    TextSnapshot snapshot;
    ASSERT_TRUE(appendTextRow(snapshot, t.t.screen(), 0)); // first batch: A
    t.t.feed("\x1b[3J"); // history goes: C is not the row after A
    for (std::size_t i = 0; i < t.t.screen().totalRows(); ++i) {
        appendTextRow(snapshot, t.t.screen(), i);
    }
    const std::atomic<bool> cancelled{false};
    EXPECT_TRUE(searchText(snapshot, U"AC", false, cancelled).matches.empty());
    EXPECT_EQ(searchText(snapshot, U"CD", false, cancelled).matches.size(), 1u);
}

TEST(TextSearch, DoubleWidthRowsShowTheirLeftHalfOnly)
{
    Term t(20, 3);
    t.t.feed("\x1b[1;12Hsecret\x1b#6\x1b[2;1Hshown\x1b#6");
    EXPECT_TRUE(search(t, U"secret").empty());
    EXPECT_EQ(search(t, U"shown").size(), 1u);
}

TEST(TextSearch, MulticellMatchCoversTheBlockWidth)
{
    Term t(20, 3);
    t.t.feed("\x1b]66;s=2;ab\x07");
    const auto m = search(t, U"b");
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m[0].start.col, 2);
    EXPECT_EQ(m[0].end.col, 3);
}

TEST(TextSearch, BoundedOutputNeverSplitsUtf8AndReportsMissingMarkers)
{
    Term t(20, 3);
    EXPECT_FALSE(t.t.hasCommandOutput());
    t.t.feed("\x1b]133;C\a" "a\xe7\x95\x8c" "bc\x1b]133;D;0\a");
    EXPECT_TRUE(t.t.hasCommandOutput());
    bool truncated = false;
    EXPECT_EQ(t.t.text(Terminal::TextScope::LastOutput, 3, &truncated), "a");
    EXPECT_TRUE(truncated);
    EXPECT_EQ(t.t.text(Terminal::TextScope::LastOutput, 5, &truncated), "a\xe7\x95\x8c" "b");
    EXPECT_TRUE(truncated);
    EXPECT_EQ(t.t.text(Terminal::TextScope::LastOutput, 20, &truncated), "a\xe7\x95\x8c" "bc");
    EXPECT_FALSE(truncated);
}
