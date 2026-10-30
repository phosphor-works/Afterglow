// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "TestUtil.h"

#include <AfterglowVt/Search.h>

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
} // namespace

TEST(Search, FindsAllOccurrences)
{
    Term t(20, 4);
    t.t.feed("foo bar foo\r\nbaz foo");
    const auto m = findAll(t.t.screen(), U"foo");
    ASSERT_EQ(m.size(), 3u);
    EXPECT_EQ(m[1].start.col, 8);
    EXPECT_EQ(m[1].end.col, 10);
    EXPECT_EQ(m[2].start.row, m[0].start.row + 1);
}

TEST(Search, SmartCase)
{
    Term t(20, 2);
    t.t.feed("Error error ERROR");
    EXPECT_EQ(findAll(t.t.screen(), U"error").size(), 3u);
    EXPECT_EQ(findAll(t.t.screen(), U"Error").size(), 1u);
}

TEST(Search, AcrossSoftWrap)
{
    Term t(5, 3);
    t.t.feed("abcdefghij");
    const auto m = findAll(t.t.screen(), U"defg");
    ASSERT_EQ(m.size(), 1u);
    EXPECT_EQ(m[0].start.col, 3);
    EXPECT_EQ(m[0].end.row, m[0].start.row + 1);
    EXPECT_EQ(m[0].end.col, 1);
}

TEST(Search, IncludesHistory)
{
    Term t(10, 2);
    t.t.feed("needle\r\nx\r\ny\r\nz");
    const auto m = findAll(t.t.screen(), U"needle");
    ASSERT_EQ(m.size(), 1u);
    EXPECT_LT(t.t.screen().absoluteIndexOf(m[0].start.row), (long long)t.t.screen().historySize());
}

#include <AfterglowVt/TextSearch.h>

TEST(TextSearch, RegexSpansSoftWrapsAndPreservesWideGraphemes)
{
    Term t(6, 4);
    t.t.feed("ab\xe7\x95\x8c" "e\xcc\x81" "fghi");
    TextSnapshot snapshot;
    for (std::size_t i = 0; i < t.t.screen().totalRows(); ++i) appendTextRow(snapshot, t.t.screen(), i);
    std::atomic<bool> cancelled{false};
    auto result = searchText(snapshot, U"界e\\x{301}fgh", true, cancelled);
    ASSERT_TRUE(result.error.empty()) << result.error;
    ASSERT_EQ(result.matches.size(), 1u);
    EXPECT_EQ(result.matches[0].start.col, 2);
    EXPECT_EQ(result.matches[0].end.row, result.matches[0].start.row + 1);
    result = searchText(snapshot, U"界", false, cancelled);
    ASSERT_EQ(result.matches.size(), 1u);
    EXPECT_EQ(result.matches[0].end.col, 3);
}

TEST(TextSearch, InvalidComplexCancelledAndBoundedQueries)
{
    Term t(80, 3);
    t.t.feed(std::string(75, 'a') + "!");
    TextSnapshot snapshot;
    for (std::size_t i = 0; i < t.t.screen().totalRows(); ++i) appendTextRow(snapshot, t.t.screen(), i);
    std::atomic<bool> cancelled{false};
    EXPECT_FALSE(searchText(snapshot, U"[", true, cancelled).error.empty());
    EXPECT_FALSE(searchText(snapshot, U"(a+)+$", true, cancelled).error.empty());
    EXPECT_TRUE(searchText(snapshot, U"a", true, cancelled, 3).truncated);
    cancelled = true;
    EXPECT_TRUE(searchText(snapshot, U"a", true, cancelled).cancelled);
    TextSnapshot bounded;
    EXPECT_FALSE(appendTextRow(bounded, t.t.screen(), 0, 10));
    EXPECT_TRUE(bounded.truncated);
    EXPECT_EQ(bounded.characters, 10u);
}

TEST(Selection, KeyboardMovementSkipsWideTailsAndClampsHistory)
{
    Term t(6, 2);
    t.t.feed("a\xe7\x95\x8c" "e\xcc\x81");
    const auto& screen = t.t.screen();
    SelectionPoint p{screen.rowsDropped(), 0};
    p = moveSelectionCursor(screen, p, 1, 0);
    EXPECT_EQ(p.col, 1);
    p = moveSelectionCursor(screen, p, 1, 0);
    EXPECT_EQ(p.col, 3);
    p = moveSelectionCursor(screen, p, -1, 0);
    EXPECT_EQ(p.col, 1);
    EXPECT_EQ(moveSelectionCursor(screen, {screen.rowsDropped(), 0}, -1, -100).col, 0);
    EXPECT_EQ(moveSelectionCursor(screen, p, 0, 100).row, screen.rowsDropped() + screen.totalRows() - 1);
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
