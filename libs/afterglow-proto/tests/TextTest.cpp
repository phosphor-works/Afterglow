// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Text.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

using namespace AfterglowProto;

TEST(Text, NextUtf8DecodesEveryLength)
{
    const std::string_view s = "a\xc3\xa9\xe2\x82\xac\xf0\x9f\x90\x88";
    std::size_t i = 0;
    char32_t cp = 0;
    ASSERT_TRUE(nextUtf8(s, i, cp));
    EXPECT_EQ(cp, U'a');
    EXPECT_EQ(i, 1u);
    ASSERT_TRUE(nextUtf8(s, i, cp));
    EXPECT_EQ(cp, U'é');
    EXPECT_EQ(i, 3u);
    ASSERT_TRUE(nextUtf8(s, i, cp));
    EXPECT_EQ(cp, U'€');
    EXPECT_EQ(i, 6u);
    ASSERT_TRUE(nextUtf8(s, i, cp));
    EXPECT_EQ(cp, U'\U0001f408');
    EXPECT_EQ(i, s.size());
    // At the end there is nothing to read, and the index stays.
    EXPECT_FALSE(nextUtf8(s, i, cp));
    EXPECT_EQ(i, s.size());
    std::size_t empty = 0;
    EXPECT_FALSE(nextUtf8({}, empty, cp));
    EXPECT_EQ(empty, 0u);
}

TEST(Text, NextUtf8RejectsIllFormedSequences)
{
    const std::string_view bad[] = {
        "\xc0\x80", // overlong NUL
        "\xe0\x80\x80", // overlong in three bytes
        "\xed\xa0\x80", // surrogate U+D800
        "\xe2\x82", // truncated
        "\xf4\x90\x80\x80", // U+110000, over U+10FFFF
        "\x80", // a continuation byte first
        "\xff", // no UTF-8 lead byte
        "\xc3\x28", // the second byte is no continuation
    };
    for (const std::string_view s : bad) {
        std::size_t i = 0;
        char32_t cp = 0;
        EXPECT_FALSE(nextUtf8(s, i, cp)) << testing::PrintToString(std::string(s));
        EXPECT_EQ(i, 0u) << testing::PrintToString(std::string(s));
        EXPECT_FALSE(isCleanText(s)) << testing::PrintToString(std::string(s));
    }
    // The highest code point is valid.
    std::size_t i = 0;
    char32_t cp = 0;
    EXPECT_TRUE(nextUtf8("\xf4\x8f\xbf\xbf", i, cp));
    EXPECT_EQ(cp, U'\U0010ffff');
}

TEST(Text, ControlsAreC0DelAndC1)
{
    EXPECT_TRUE(isControl(0x00));
    EXPECT_TRUE(isControl(0x1f));
    EXPECT_FALSE(isControl(0x20));
    EXPECT_FALSE(isControl(0x7e));
    EXPECT_TRUE(isControl(0x7f));
    EXPECT_TRUE(isControl(0x80));
    EXPECT_TRUE(isControl(0x85));
    EXPECT_TRUE(isControl(0x9f));
    EXPECT_FALSE(isControl(0xa0));
    EXPECT_FALSE(isControl(0x2028));
}

TEST(Text, CleanTextRejectsControls)
{
    EXPECT_TRUE(isCleanText(""));
    EXPECT_TRUE(isCleanText("plain text, caf\xc3\xa9"));
    EXPECT_FALSE(isCleanText("a\x1b[1mb"));
    EXPECT_FALSE(isCleanText("a\tb"));
    EXPECT_FALSE(isCleanText("a\x7f" "b")); // DEL
    EXPECT_FALSE(isCleanText("a\xc2\x85" "b")); // C1 NEL, encoded
    EXPECT_FALSE(isCleanText("a\xc2\x9b" "1m")); // C1 CSI, encoded
}

TEST(Text, CleanTextAllowsNewlinesAndLineSeparatorsOnRequest)
{
    const std::string_view lineSeparator = "a\xe2\x80\xa8" "b"; // U+2028
    const std::string_view paragraphSeparator = "a\xe2\x80\xa9" "b"; // U+2029
    EXPECT_FALSE(isCleanText(lineSeparator));
    EXPECT_FALSE(isCleanText(paragraphSeparator));
    EXPECT_TRUE(isCleanText(lineSeparator, TextAllow::LineSeparators));
    EXPECT_TRUE(isCleanText(paragraphSeparator, TextAllow::LineSeparators));
    EXPECT_FALSE(isCleanText(lineSeparator, TextAllow::Newline));

    EXPECT_FALSE(isCleanText("a\nb"));
    EXPECT_TRUE(isCleanText("a\nb", TextAllow::Newline));
    EXPECT_FALSE(isCleanText("a\nb", TextAllow::LineSeparators));
    // Newline lets LF through, not CR or another control.
    EXPECT_FALSE(isCleanText("a\r\nb", TextAllow::Newline));
    EXPECT_TRUE(isCleanText("a\n\xe2\x80\xa8" "b", TextAllow::Newline | TextAllow::LineSeparators));
}

TEST(Text, PrintableHidesControlsBidiControlsAndLineSeparators)
{
    EXPECT_EQ(printable("plain text, caf\xc3\xa9 \xe4\xb8\x80"), "plain text, caf\xc3\xa9 \xe4\xb8\x80");
    EXPECT_EQ(printable("a\x1b[31mb\x07\tc\n"), "a?[31mb??c?");
    EXPECT_EQ(printable("a\x7f" "b\xc2\x9b" "c"), "a?b?c"); // DEL and C1 CSI, encoded
    // Each control of bidirectional text: ALM, LRM, RLM, LRE to RLO, LRI to PDI.
    for (const char32_t cp : {U'\u061c', U'\u200e', U'\u200f', U'\u202a', U'\u202b', U'\u202c', U'\u202d', U'\u202e',
                              U'\u2066', U'\u2067', U'\u2068', U'\u2069'}) {
        std::string text = "a";
        appendUtf8(text, cp);
        text += "b";
        EXPECT_EQ(printable(text), "a?b") << std::hex << std::uint32_t(cp);
    }
    // Their neighbours stay: ZWJ and the word joiner shape text, and move nothing.
    EXPECT_EQ(printable("a\xe2\x80\x8d" "b\xe2\x81\xa0" "c"), "a\xe2\x80\x8d" "b\xe2\x81\xa0" "c");
    EXPECT_EQ(printable("a\xe2\x80\xa8" "b\xe2\x80\xa9" "c"), "a?b?c"); // U+2028, U+2029
    // One '?' for each byte that is not UTF-8, and the text after it stays.
    EXPECT_EQ(printable("a\xff\xfe" "b\xf0\x9f" "c"), "a??b??c");
    EXPECT_EQ(printable(""), "");
}

TEST(Text, CellWidthTellsCombiningAndWideCharacters)
{
    EXPECT_EQ(cellWidth(U'a'), 1);
    EXPECT_EQ(cellWidth(U'é'), 1);
    EXPECT_EQ(cellWidth(U'́'), 0); // combining acute accent
    EXPECT_EQ(cellWidth(U'‍'), 0); // ZWJ
    EXPECT_EQ(cellWidth(U'一'), 2);
    EXPECT_EQ(cellWidth(U'가'), 2);
    EXPECT_EQ(cellWidth(U'\U0001f600'), 2);
}

TEST(Text, SanitizeIdKeepsSafeCharactersOfTheFirst128Bytes)
{
    EXPECT_EQ(sanitizeId("Az09_-+."), "Az09_-+.");
    EXPECT_EQ(sanitizeId("a b;c:d\x1b]e\xc3\xa9"), "abcde");
    EXPECT_EQ(sanitizeId(""), "");
    const std::string id(128, 'x');
    EXPECT_EQ(sanitizeId(id), id);
    EXPECT_EQ(sanitizeId(id + "yz"), id);
    // The cut is made before characters are dropped: 127 kept bytes, a dropped
    // one, then bytes past 128 that do not count.
    EXPECT_EQ(sanitizeId(std::string(127, 'x') + ";yz"), std::string(127, 'x'));
}
