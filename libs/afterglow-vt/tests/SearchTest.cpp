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
