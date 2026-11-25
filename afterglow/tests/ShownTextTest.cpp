// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ShownText.h"

#include <gtest/gtest.h>

using namespace Afterglow;

// The invisible characters past the BMP go too: a tag character or a
// supplementary variation selector is a surrogate pair, which a check of
// each UTF-16 unit never matched.
TEST(ShownText, TheInvisibleCodePointsPastTheBmpGo)
{
    // U+202E RIGHT-TO-LEFT OVERRIDE as a number: a literal would be an
    // unpaired bidirectional control in the source.
    const char32_t codes[] = {U'a', 0xE0041, U'b', 0xE0100, U'c', 0x202E, U'd'};
    const QString tag = QString::fromUcs4(codes, 7);
    EXPECT_EQ(sanitizedText(tag, 100), QStringLiteral("abcd"));
    EXPECT_EQ(sanitizedLine(tag + QChar(u'\n'), 100), QStringLiteral("abcd"));
    // Visible characters past the BMP stay whole.
    const QString emoji = QString::fromUcs4(U"x\U0001F600y", 3);
    EXPECT_EQ(sanitizedText(emoji, 100), emoji);
    // The limit never splits a pair.
    EXPECT_EQ(sanitizedText(emoji, 2), QStringLiteral("x"));
    // A lone surrogate is a code point of its own, which stays.
    const QString lone = QString(QChar(u'a')) + QChar(char16_t(0xD800)) + QChar(u'b');
    EXPECT_EQ(sanitizedText(lone, 100), lone);
}

TEST(ShownText, ACodePointDisguisesTextAsItsUnitDoes)
{
    EXPECT_TRUE(disguisesCodePoint(0x200B));
    EXPECT_TRUE(disguisesCodePoint(0xE0001));
    EXPECT_TRUE(disguisesCodePoint(0xE01EF));
    EXPECT_FALSE(disguisesCodePoint(0xE01F0));
    EXPECT_FALSE(disguisesCodePoint(0x1F600));
    EXPECT_FALSE(disguisesCodePoint(U'a'));
}
