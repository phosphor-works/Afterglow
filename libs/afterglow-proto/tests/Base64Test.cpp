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

TEST(Base64, StrictIsRfc4648)
{
    const auto decode = [](std::string_view in) { return base64Decode(in, Base64Check::Strict); };
    EXPECT_EQ(decode(""), "");
    EXPECT_EQ(decode("QQ=="), "A");
    EXPECT_EQ(decode("QUI="), "AB");
    EXPECT_EQ(decode("QUJD"), "ABC");
    for (std::string_view bad : {"QQ", "QQ=", "QQ===", "Q===", "QQ==QUJD", "QU=D", "QU JD", "QUJD\n", "QU!D", "===="}) {
        EXPECT_FALSE(decode(bad)) << bad;
    }
}

TEST(Base64, PaddingOptionalIgnoresTrailingPadding)
{
    const auto decode = [](std::string_view in) { return base64Decode(in, Base64Check::PaddingOptional); };
    EXPECT_EQ(decode("QQ"), "A");
    EXPECT_EQ(decode("QQ=="), "A");
    EXPECT_EQ(decode("QQ="), "A");
    EXPECT_EQ(decode("QQ====="), "A"); // any run of '=' at the end
    EXPECT_EQ(decode("QUJD"), "ABC");
    for (std::string_view bad : {"Q", "QUJDR", "Q=Q=", "QU JD", "QUJD\n"}) {
        EXPECT_FALSE(decode(bad)) << bad;
    }
}

TEST(Base64, AppendLeavesTheOutputAloneOnFailure)
{
    std::string out = "kept";
    EXPECT_FALSE(base64DecodeAppend(out, "QUJDRA!=", Base64Check::Strict));
    EXPECT_EQ(out, "kept");
    // A failure found after some groups were decoded rolls them back too.
    EXPECT_FALSE(base64DecodeAppend(out, "QUJDQUJDQU!D", Base64Check::PaddingOptional));
    EXPECT_EQ(out, "kept");
    EXPECT_TRUE(base64DecodeAppend(out, "QUJD", Base64Check::Strict));
    EXPECT_EQ(out, "keptABC");
    EXPECT_TRUE(base64DecodeAppend(out, "", Base64Check::Strict));
    EXPECT_EQ(out, "keptABC");
}

TEST(Base64, LenientDecodesWrappedLinesWhole)
{
    // Line breaks every 76 characters, as MIME wraps base64, and a stray
    // byte inside a group: the groups after each still decode.
    std::string data;
    for (int i = 0; i < 1000; ++i) {
        data += char(i * 37);
    }
    const std::string encoded = base64Encode(data);
    std::string wrapped;
    for (std::size_t at = 0; at < encoded.size(); at += 76) {
        wrapped += encoded.substr(at, 76);
        wrapped += "\r\n";
    }
    EXPECT_EQ(base64Decode(wrapped), data);
    std::string stray = encoded;
    stray.insert(2, " ");
    stray.insert(stray.size() / 2 + 1, "!");
    EXPECT_EQ(base64Decode(stray), data);
}
