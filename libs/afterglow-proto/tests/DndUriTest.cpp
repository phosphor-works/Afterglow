// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/DndUri.h>

#include <gtest/gtest.h>

#include <string>

using namespace AfterglowProto;

TEST(DndUri, PercentDecodeRefusesANulInAPath)
{
    EXPECT_EQ(dndPercentDecode("/a%20b%2Fc"), std::optional<std::string>("/a b/c"));
    EXPECT_EQ(dndPercentDecode("/a%00b"), std::nullopt);
    EXPECT_EQ(dndPercentDecode("a%00b", true), std::optional<std::string>(std::string("a\0b", 3)));
    // A '%' without two hex digits stays, also at the end of the text.
    EXPECT_EQ(dndPercentDecode("100%"), std::optional<std::string>("100%"));
    EXPECT_EQ(dndPercentDecode("%4"), std::optional<std::string>("%4"));
    EXPECT_EQ(dndPercentDecode("%zz%41"), std::optional<std::string>("%zzA"));
}

TEST(DndUri, EncodeAndParseRoundTrip)
{
    const std::string path = "/tmp/a b/%name\xc3\xa9\n#?";
    const auto uri = dndFileUri(path, false);
    EXPECT_EQ(uri.find_first_of(" \n#?"), std::string::npos) << uri;
    const auto parsed = parseDndUriList(uri + "\r\n");
    ASSERT_EQ(parsed.size(), 1u);
    EXPECT_EQ(parsed[0].kind, DndUri::Kind::File);
    EXPECT_EQ(parsed[0].path, path);
    EXPECT_EQ(dndFileUri("/tmp/dir", true), "file:///tmp/dir/");
    EXPECT_EQ(dndFileUri("/", true), "file:///");
}

TEST(DndUri, OnlyFilesOfThisComputerAreFiles)
{
    const auto parsed = parseDndUriList("file:///a\r\n"
                                        "file://localhost/b\r\n"
                                        "file:/c\n"
                                        "file://otherhost/d\r\n"
                                        "file:///e%00f\r\n"
                                        "https://example.org/g\r\n"
                                        "# a comment\r\n"
                                        "\r\n"
                                        "  file:///h/../i?x#y  \r\n");
    ASSERT_EQ(parsed.size(), 7u);
    EXPECT_EQ(parsed[0].kind, DndUri::Kind::File);
    EXPECT_EQ(parsed[0].path, "/a");
    EXPECT_EQ(parsed[1].kind, DndUri::Kind::File);
    EXPECT_EQ(parsed[1].path, "/b");
    EXPECT_EQ(parsed[2].kind, DndUri::Kind::File);
    EXPECT_EQ(parsed[2].path, "/c");
    // Deleting /d after a move would delete a file here that has the other host's path.
    EXPECT_EQ(parsed[3].kind, DndUri::Kind::Other);
    EXPECT_EQ(parsed[4].kind, DndUri::Kind::Other);
    EXPECT_EQ(parsed[5].kind, DndUri::Kind::Other);
    EXPECT_EQ(parsed[6].kind, DndUri::Kind::File);
    EXPECT_EQ(parsed[6].path, "/i");
}

TEST(DndUri, DataUris)
{
    const auto parsed = parseDndUriList("data:,a%20b%00\r\n"
                                        "data:image/png;base64,iVBO\r\n"
                                        "data:text/html;charset=utf-8,%3Cp%3E\r\n"
                                        "data:;base64,!!!\r\n"
                                        "data:nocomma\r\n");
    ASSERT_EQ(parsed.size(), 5u);
    EXPECT_EQ(parsed[0].kind, DndUri::Kind::Data);
    EXPECT_EQ(parsed[0].mime, "text/plain");
    EXPECT_EQ(parsed[0].data, std::string("a b\0", 4));
    EXPECT_EQ(parsed[1].kind, DndUri::Kind::Data);
    EXPECT_EQ(parsed[1].mime, "image/png");
    EXPECT_EQ(parsed[1].data, "\x89PN");
    EXPECT_EQ(parsed[2].mime, "text/html");
    EXPECT_EQ(parsed[2].data, "<p>");
    EXPECT_EQ(parsed[3].kind, DndUri::Kind::Other);
    EXPECT_EQ(parsed[4].kind, DndUri::Kind::Other);
}
