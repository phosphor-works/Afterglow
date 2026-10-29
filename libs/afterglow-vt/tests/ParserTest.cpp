// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Parser.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace AfterglowVt;

namespace {
struct Recorder
{
    std::u32string printed;
    std::vector<std::string> events;

    void print(char32_t cp) { printed.push_back(cp); }
    void execute(std::uint8_t c) { events.push_back("exec " + std::to_string(c)); }
    void escDispatch(const Intermediates& i, std::uint8_t f)
    {
        events.push_back(std::string("esc ") + (i.count ? std::string(1, i.bytes[0]) : "") + char(f));
    }
    void csiDispatch(const Params& p, const Intermediates& i, std::uint8_t f)
    {
        std::string s = "csi ";
        if (i.marker) {
            s += i.marker;
        }
        for (int k = 0; k < p.count; ++k) {
            if (k > 0) {
                s += p.isSub(k) ? ':' : ';';
            }
            s += std::to_string(p.values[k]);
        }
        for (int k = 0; k < i.count; ++k) {
            s += i.bytes[k];
        }
        s += char(f);
        events.push_back(s);
    }
    void oscDispatch(std::string_view payload, bool bel)
    {
        events.push_back(std::string("osc ") + std::string(payload) + (bel ? " bel" : " st"));
    }
    void dcsDispatch(const Params&, const Intermediates& i, std::uint8_t f, std::string_view data)
    {
        events.push_back(std::string("dcs ") + (i.marker ? std::string(1, i.marker) : "") + char(f) + " "
                         + std::string(data));
    }
    void apcDispatch(std::string_view payload) { events.push_back("apc " + std::string(payload)); }
};

struct Fixture
{
    Recorder r;
    Parser<Recorder> p{r};
};
} // namespace

TEST(Parser, PrintsAsciiAndUtf8)
{
    Fixture f;
    f.p.feed("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80");
    EXPECT_EQ(f.r.printed, U"aé€\U0001F600");
}

TEST(Parser, Utf8SplitAcrossFeeds)
{
    Fixture f;
    f.p.feed("\xE2\x82");
    f.p.feed("\xAC");
    EXPECT_EQ(f.r.printed, U"€");
}

TEST(Parser, InvalidUtf8BecomesReplacement)
{
    Fixture f;
    f.p.feed("\xC0\xAF"); // overlong / invalid lead
    f.p.feed("\xE2\x82x"); // truncated then ASCII
    f.p.feed("\xED\xA0\x80"); // surrogate
    EXPECT_EQ(f.r.printed, U"���x�");
}

TEST(Parser, CsiParamsAndSubparams)
{
    Fixture f;
    f.p.feed("\x1b[1;31m\x1b[4:3m\x1b[38:2::10:20:30m\x1b[?1049h\x1b[m\x1b[;5H");
    ASSERT_EQ(f.r.events.size(), 6u);
    EXPECT_EQ(f.r.events[0], "csi 1;31m");
    EXPECT_EQ(f.r.events[1], "csi 4:3m");
    EXPECT_EQ(f.r.events[2], "csi 38:2:0:10:20:30m");
    EXPECT_EQ(f.r.events[3], "csi ?1049h");
    EXPECT_EQ(f.r.events[4], "csi m");
    EXPECT_EQ(f.r.events[5], "csi 0;5H");
}

TEST(Parser, CsiIntermediates)
{
    Fixture f;
    f.p.feed("\x1b[2 q\x1b[?2026$p");
    ASSERT_EQ(f.r.events.size(), 2u);
    EXPECT_EQ(f.r.events[0], "csi 2 q");
    EXPECT_EQ(f.r.events[1], "csi ?2026$p");
}

TEST(Parser, ControlInsideCsiExecutes)
{
    Fixture f;
    f.p.feed("\x1b[1\n2H");
    ASSERT_EQ(f.r.events.size(), 2u);
    EXPECT_EQ(f.r.events[0], "exec 10");
    EXPECT_EQ(f.r.events[1], "csi 12H");
}

TEST(Parser, OscTerminators)
{
    Fixture f;
    f.p.feed("\x1b]0;hello\x07\x1b]2;t\xC3\x9C\x1b\\");
    ASSERT_GE(f.r.events.size(), 2u);
    EXPECT_EQ(f.r.events[0], "osc 0;hello bel");
    EXPECT_EQ(f.r.events[1], "osc 2;t\xC3\x9C st");
}

TEST(Parser, DcsAndApc)
{
    Fixture f;
    f.p.feed("\x1bP>|data\x1b\\\x1b_Gf=100;AAAA\x1b\\");
    EXPECT_EQ(f.r.events[0], "dcs >| data");
    EXPECT_EQ(f.r.events[2], "apc Gf=100;AAAA");
}

TEST(Parser, CancelAbortsSequence)
{
    Fixture f;
    f.p.feed("\x1b[12\x18x");
    ASSERT_EQ(f.r.events.size(), 1u);
    EXPECT_EQ(f.r.events[0], "exec 24");
    EXPECT_EQ(f.r.printed, U"x");
}

TEST(Parser, EscDispatch)
{
    Fixture f;
    f.p.feed("\x1b" "7\x1b(0\x1b#8");
    ASSERT_EQ(f.r.events.size(), 3u);
    EXPECT_EQ(f.r.events[0], "esc 7");
    EXPECT_EQ(f.r.events[1], "esc (0");
    EXPECT_EQ(f.r.events[2], "esc #8");
}
