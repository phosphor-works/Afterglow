// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Parser.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

using namespace AfterglowVt;

namespace {
struct Recorder
{
    std::u32string printed;
    std::vector<std::string> events;

    void print(char32_t cp) { printed.push_back(cp); }
    void printAscii(const char* text, std::size_t n)
    {
        for (std::size_t i = 0; i < n; ++i) {
            printed.push_back(char32_t(text[i]));
        }
    }
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
    ASSERT_GE(f.r.events.size(), 3u);
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

TEST(Parser, ResetRecoversFromUnterminatedOverflow)
{
    Fixture f;
    f.p.feed("\x1b]2;");
    const std::string chunk(1024 * 1024, 'a');
    for (std::size_t i = 0; i <= Parser<Recorder>::MaxStringPayload / chunk.size(); ++i) f.p.feed(chunk);
    f.p.reset();
    // Ground again: text prints instead of joining the dropped string.
    f.p.feed("x");
    EXPECT_EQ(f.r.printed, U"x");
    f.p.feed("\x1b]2;recovered\x07");
    ASSERT_EQ(f.r.events.size(), 1u);
    EXPECT_EQ(f.r.events.front(), "osc 2;recovered bel");
}

TEST(Parser, ResetDropsAPartialUtf8Sequence)
{
    Fixture f;
    f.p.feed("\xE2");
    f.p.reset();
    f.p.feed("x");
    EXPECT_EQ(f.r.printed, U"x"); // no U+FFFD for the dropped lead byte
}

TEST(Parser, OverlongStringsAreDroppedWhole)
{
    // An OSC or DCS past the payload limit is never dispatched cut short,
    // even when it ends normally.
    const std::string big(Parser<Recorder>::MaxStringPayload + 1, 'a');
    Fixture f;
    f.p.feed("\x1b]2;");
    f.p.feed(big);
    f.p.feed("\x07");
    EXPECT_TRUE(f.r.events.empty());
    f.p.feed("\x1bPq");
    f.p.feed(big);
    f.p.feed("\x1b\\");
    for (const std::string& e : f.r.events) {
        EXPECT_FALSE(e.starts_with("dcs")) << e.substr(0, 20);
    }
    f.r.events.clear();
    f.p.feed("\x1b]2;ok\x07");
    ASSERT_EQ(f.r.events.size(), 1u);
    EXPECT_EQ(f.r.events[0], "osc 2;ok bel");
}

TEST(Parser, CsiParameterCountAndValueBounds)
{
    std::string exact = "\x1b[";
    for (int i = 0; i < Params::Max - 1; ++i) {
        exact += "1;";
    }
    Fixture f;
    f.p.feed(exact + "1m");
    ASSERT_EQ(f.r.events.size(), 1u);
    EXPECT_EQ(std::count(f.r.events[0].begin(), f.r.events[0].end(), ';'), Params::Max - 1);
    f.r.events.clear();
    f.p.feed(exact + "1;1m"); // one more: the whole sequence goes
    EXPECT_TRUE(f.r.events.empty());
    f.p.feed("\x1b[70000;65536m"); // values saturate
    ASSERT_EQ(f.r.events.size(), 1u);
    EXPECT_EQ(f.r.events[0], "csi 65535;65535m");
    f.r.events.clear();
    f.p.feed("\x1b[1$$$p"); // a third intermediate overflows
    EXPECT_TRUE(f.r.events.empty());
}

TEST(Parser, ParameterFlagsDoNotLeakIntoTheNextSequence)
{
    // clearSequence() keeps the old bits; each parameter writes its own.
    Fixture f;
    f.p.feed("\x1b[1:2:3m\x1b[4;5;6m\x1b[;7m");
    ASSERT_EQ(f.r.events.size(), 3u);
    EXPECT_EQ(f.r.events[0], "csi 1:2:3m");
    EXPECT_EQ(f.r.events[1], "csi 4;5;6m");
    EXPECT_EQ(f.r.events[2], "csi 0;7m");
}

TEST(Parser, StringsSplitAcrossFeedsMatchWholeOnes)
{
    // The payload fast path and the per-byte path must agree, including an
    // ST split between its ESC and its backslash.
    const std::string input = "\x1b]2;title\x1b\\\x1bP$qm\x1b\\\x1b_Ga=q,i=1;AAAA\x1b\\\x1b]8;;u\x07";
    Fixture whole;
    whole.p.feed(input);
    Fixture bytewise;
    for (char c : input) {
        bytewise.p.feed(std::string_view(&c, 1));
    }
    EXPECT_EQ(whole.r.events, bytewise.r.events);
    EXPECT_EQ(whole.r.events.front(), "osc 2;title st");
}

TEST(Parser, CsiSplitAnywhereMatchesWholeOnes)
{
    // The whole-sequence fast path and the per-byte path must agree, and the
    // fast path must leave a sequence cut off by the end of a feed, or one
    // with intermediates, controls or an ignored byte, to the per-byte path.
    const std::string input = "a\x1b[38:5:196;48;5;17mb\x1b[?25l\x1b[>0;2:3:1;4:1:3:2:4 q\x1b[1;5\rH"
                              "\x1b[1?2m\x1b[m\x1b[;5H\x1b[3\x1b[2J\x1b[!pc\x1b[<1;2;3M";
    Fixture whole;
    whole.p.feed(input);
    ASSERT_EQ(whole.r.events.front(), "csi 38:5:196;48;5;17m");
    for (std::size_t split = 1; split < input.size(); ++split) {
        Fixture parts;
        parts.p.feed(std::string_view(input).substr(0, split));
        parts.p.feed(std::string_view(input).substr(split));
        EXPECT_EQ(parts.r.events, whole.r.events) << "split at " << split;
        EXPECT_EQ(parts.r.printed, whole.r.printed) << "split at " << split;
    }
    Fixture bytewise;
    for (char c : input) {
        bytewise.p.feed(std::string_view(&c, 1));
    }
    EXPECT_EQ(bytewise.r.events, whole.r.events);
}

TEST(Parser, EscWithTooManyIntermediatesIsDropped)
{
    Fixture f;
    f.p.feed("\x1b(((B\x1b(B");
    ASSERT_EQ(f.r.events.size(), 1u);
    EXPECT_EQ(f.r.events[0], "esc (B");
}

TEST(Parser, CancelEndsAPartialUtf8SequenceWithReplacement)
{
    for (const char* cancel : {"\x18", "\x1a"}) {
        Fixture f;
        f.p.feed(std::string("\xE2\x82") + cancel + "x");
        EXPECT_EQ(f.r.printed, U"\uFFFDx");
    }
}

TEST(Parser, DcsFinalByteAboveAsciiIsIgnored)
{
    Fixture f;
    f.p.feed("\x1bP1\xC3" "data\x1b\\\x1bP$\xC3" "data\x1b\\");
    for (const std::string& e : f.r.events) {
        EXPECT_FALSE(e.starts_with("dcs")) << e;
    }
}
