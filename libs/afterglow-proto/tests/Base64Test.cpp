// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Base64.h>

#include <gtest/gtest.h>

using namespace AfterglowProto;

TEST(Base64, LenientPaddingEndsEachSegment)
{
    EXPECT_EQ(base64Decode("QQ==Qg=="), "AB");
    EXPECT_EQ(base64Decode("QUJD\nREVG"), "ABCDEF");
}

TEST(Base64, EncodesWithAndWithoutPadding)
{
    EXPECT_EQ(base64Encode(""), "");
    EXPECT_EQ(base64Encode("A"), "QQ==");
    EXPECT_EQ(base64Encode("AB"), "QUI=");
    EXPECT_EQ(base64Encode("ABC"), "QUJD");
    EXPECT_EQ(base64Encode("A", false), "QQ");
    EXPECT_EQ(base64Encode("AB", false), "QUI");
    EXPECT_EQ(base64Encode("ABC", false), "QUJD");
}

// What the file transfer and drag and drop handlers accepted when they decoded with Qt
// (QByteArray::fromBase64Encoding with AbortOnBase64DecodingErrors).
TEST(Base64, StrictOrUnpaddedMatchesTheTransferRules)
{
    const auto decode = [](std::string_view in) { return base64Decode(in, Base64Check::StrictOrUnpadded); };
    EXPECT_EQ(decode(""), "");
    EXPECT_EQ(decode("QQ"), "A");
    EXPECT_EQ(decode("QQ=="), "A");
    EXPECT_EQ(decode("QUI"), "AB");
    EXPECT_EQ(decode("QUI="), "AB");
    EXPECT_EQ(decode("QUJDRA"), "ABCD");
    EXPECT_EQ(decode("QUJDR"), "ABC"); // a last character that cannot end a byte is dropped
    EXPECT_EQ(decode("A"), "");
    EXPECT_EQ(decode("QR=="), "A");
    for (std::string_view bad : {"QQ=", "QQ===", "QUJD=", "QUJDRA=", "QQ==QQ==", "Q=Q=", "QU JD", "QUJD\n", "QQ=a", "=", "=="}) {
        EXPECT_FALSE(decode(bad)) << bad;
    }
}
