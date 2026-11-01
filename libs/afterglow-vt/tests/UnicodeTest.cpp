// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowVt/Unicode.h>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace AfterglowVt::Unicode;

namespace {
std::vector<std::u32string> segment(std::u32string_view text)
{
    std::vector<std::u32string> out;
    GraphemeSegmenter seg;
    for (char32_t cp : text) {
        if (seg.breaksBefore(cp) || out.empty()) {
            out.emplace_back();
        }
        out.back().push_back(cp);
    }
    return out;
}
} // namespace

TEST(Unicode, Widths)
{
    EXPECT_EQ(props(U'a').width, 1);
    EXPECT_EQ(props(U'中').width, 2); // CJK
    EXPECT_EQ(props(U'́').width, 0); // combining acute
    EXPECT_EQ(props(U'‍').width, 0); // ZWJ
    EXPECT_EQ(props(U'\U0001F600').width, 2); // emoji, presentation by default
    EXPECT_EQ(props(U'❤').width, 1); // heart: text presentation by default
    EXPECT_EQ(props(U'­').width, 1); // soft hyphen
    EXPECT_EQ(props(U'ᅠ').width, 0); // Hangul medial vowel filler
    EXPECT_TRUE(props(U'❤').widensWithVs16);
    // Past the last code point: the replacement character's width.
    EXPECT_EQ(props(char32_t(0x110000)).width, props(ReplacementCharacter).width);
    EXPECT_EQ(props(char32_t(0xFFFFFFFF)).width, props(ReplacementCharacter).width);
    EXPECT_STREQ(ucdVersion(), "17.0.0");
}

TEST(Unicode, EastAsianAmbiguous)
{
    EXPECT_TRUE(props(U'α').ambiguous); // Greek
    EXPECT_TRUE(props(U'─').ambiguous); // box drawing
    EXPECT_TRUE(props(U'±').ambiguous);
    EXPECT_EQ(props(U'α').width, 1); // narrow unless the terminal says otherwise
    EXPECT_FALSE(props(U'a').ambiguous);
    EXPECT_FALSE(props(U'中').ambiguous); // already wide
    EXPECT_FALSE(props(U'́').ambiguous); // combining marks stay zero width
}

TEST(Unicode, CombiningMarkJoinsBase)
{
    auto g = segment(U"éx");
    ASSERT_EQ(g.size(), 2u);
    EXPECT_EQ(g[0], U"é");
}

TEST(Unicode, EmojiZwjSequenceIsOneCluster)
{
    // family: man ZWJ woman ZWJ girl
    auto g = segment(U"\U0001F468‍\U0001F469‍\U0001F467a");
    ASSERT_EQ(g.size(), 2u);
    EXPECT_EQ(g[1], U"a");
}

TEST(Unicode, RegionalIndicatorsPairUp)
{
    // three RIs: a flag pair then a lone indicator
    auto g = segment(U"\U0001F1FA\U0001F1F8\U0001F1E9");
    ASSERT_EQ(g.size(), 2u);
    EXPECT_EQ(g[0].size(), 2u);
}

TEST(Unicode, HangulSyllableSequence)
{
    // L V T jamo form one cluster
    auto g = segment(U"각");
    EXPECT_EQ(g.size(), 1u);
}

TEST(Unicode, IndicConjunct)
{
    // Devanagari KA + VIRAMA + SSA forms one cluster under GB9c
    auto g = segment(U"क्ष");
    EXPECT_EQ(g.size(), 1u);
}
