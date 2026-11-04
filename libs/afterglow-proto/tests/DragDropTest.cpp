// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/DragDrop.h>

#include <gtest/gtest.h>

using namespace AfterglowProto;

namespace {

/// Reads packets as a terminal or a client does: each one parsed, chunks put back together.
std::vector<std::pair<DndMeta, std::string>> readAll(const std::vector<std::string>& packets, DndAssembler& assembler)
{
    std::vector<std::pair<DndMeta, std::string>> messages;
    for (const auto& packet : packets) {
        EXPECT_TRUE(packet.starts_with("\x1b]72;"));
        EXPECT_TRUE(packet.ends_with("\x1b\\"));
        const auto [raw, payload] = splitDnd(std::string_view(packet).substr(5, packet.size() - 7));
        const auto meta = parseDndMeta(raw);
        EXPECT_TRUE(meta);
        if (!meta) {
            continue;
        }
        DndMeta whole;
        std::string bytes;
        if (assembler.feed(*meta, payload, whole, bytes) == DndAssembler::Result::Ready) {
            messages.emplace_back(std::move(whole), std::move(bytes));
        }
    }
    return messages;
}

} // namespace

TEST(DragDrop, MessagesRoundTrip)
{
    DndAssembler assembler(1 << 20, 1 << 20);
    const DndMeta meta{{"t", "m"}, {"i", "42"}, {"x", "3"}, {"y", "-1"}, {"X", "4294967295"}, {"Y", "-2147483648"}, {"o", "3"}};
    const auto packets = writeDnd(meta, "text/plain text/uri-list");
    ASSERT_EQ(packets.size(), 1u);
    EXPECT_EQ(packets[0], "\x1b]72;X=4294967295:Y=-2147483648:i=42:m=0:o=3:t=m:x=3:y=-1;text/plain text/uri-list\x1b\\");
    const auto messages = readAll(packets, assembler);
    ASSERT_EQ(messages.size(), 1u);
    DndMeta expected = meta;
    expected["m"] = "0";
    EXPECT_EQ(messages[0].first, expected);
    EXPECT_EQ(messages[0].second, "text/plain text/uri-list");
    // No payload: one packet.
    EXPECT_EQ(writeDnd({{"t", "a"}}, ""), std::vector<std::string>{"\x1b]72;m=0:t=a;\x1b\\"});
}

TEST(DragDrop, LongPayloadsGoInChunks)
{
    std::string data;
    for (int i = 0; i < 10000; ++i) {
        data += char('A' + i % 26);
    }
    const auto packets = writeDnd({{"t", "p"}, {"x", "0"}, {"i", "7"}}, data);
    ASSERT_EQ(packets.size(), 3u); // 4096 + 4096 + 1808
    EXPECT_NE(packets[0].find("m=1"), std::string::npos);
    EXPECT_NE(packets[1].find("m=1"), std::string::npos);
    EXPECT_NE(packets[2].find("m=0"), std::string::npos);
    DndAssembler assembler(1 << 20, 100);
    const auto messages = readAll(packets, assembler);
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].second, data);
    EXPECT_EQ(messages[0].first.at("t"), "p");
    EXPECT_FALSE(messages[0].first.contains("_end"));
    EXPECT_FALSE(assembler.active());
}

TEST(DragDrop, AssemblerLimitsAndIds)
{
    DndAssembler assembler(1000, 10);
    DndMeta out;
    std::string bytes;
    // A control payload over its limit.
    EXPECT_EQ(assembler.feed({{"t", "a"}, {"m", "1"}}, "12345678", out, bytes), DndAssembler::Result::Pending);
    EXPECT_EQ(assembler.type(), "a");
    EXPECT_EQ(assembler.feed({{"m", "1"}}, "123", out, bytes), DndAssembler::Result::TooLarge);
    assembler.reset();
    // A chunk that names another id.
    EXPECT_EQ(assembler.feed({{"t", "p"}, {"i", "1"}, {"m", "1"}}, "abc", out, bytes), DndAssembler::Result::Pending);
    EXPECT_EQ(assembler.feed({{"i", "2"}, {"m", "1"}}, "abc", out, bytes), DndAssembler::Result::Mismatch);
    assembler.reset();
    // An empty last chunk marks the end.
    EXPECT_EQ(assembler.feed({{"t", "p"}, {"m", "1"}}, "abc", out, bytes), DndAssembler::Result::Pending);
    EXPECT_EQ(assembler.feed({{"m", "0"}}, "", out, bytes), DndAssembler::Result::Ready);
    EXPECT_EQ(out.at("_end"), "1");
    EXPECT_EQ(bytes, "abc");
}

TEST(DragDrop, MetadataRules)
{
    EXPECT_TRUE(parseDndMeta("t=a:i= 5 :x=+3:zz=1:q:=1"));
    EXPECT_EQ(parseDndMeta("t=a:i= 5 :x=+3:zz=1")->size(), 3u);
    EXPECT_FALSE(parseDndMeta("t=a:t=b"));
    EXPECT_FALSE(parseDndMeta("t=ab"));
    EXPECT_FALSE(parseDndMeta("t="));
    EXPECT_FALSE(parseDndMeta("x=4294967296"));
    EXPECT_FALSE(parseDndMeta("x=-2147483649"));
    EXPECT_FALSE(parseDndMeta("x=0x10"));
    EXPECT_FALSE(parseDndMeta("x="));
    EXPECT_EQ(dndType("x=bad:t=e:t=k"), "e");
    EXPECT_EQ(parseDndInteger("9223372036854775807"), INT64_MAX);
    EXPECT_FALSE(parseDndInteger("9223372036854775808"));
    EXPECT_EQ(parseDndInteger("-9223372036854775808"), INT64_MIN);
}

TEST(DragDrop, QueryReplies)
{
    EXPECT_EQ(dndQueryReply("t=q:i=4294967295;"), "\x1b]72;t=q:i=4294967295;\x1b\\");
    EXPECT_EQ(dndQueryReply("t=Q"), "\x1b]72;t=Q;\x1b\\");
    EXPECT_EQ(dndQueryReply("t=q:i=4294967296;"), "");
    EXPECT_EQ(dndQueryReply("t=o:x=1;"), "");
}

TEST(DragDrop, MimeNames)
{
    EXPECT_TRUE(dndValidMime("text/plain"));
    EXPECT_FALSE(dndValidMime("text"));
    EXPECT_FALSE(dndValidMime("text/pl ain"));
    EXPECT_FALSE(dndValidMime(""));
    EXPECT_FALSE(dndValidMime("a/" + std::string(254, 'b')));
}

TEST(DragDrop, MachineIdIsAKeyedHashOfTheTrimmedFile)
{
    // HMAC-SHA256 keyed with "tty-dnd-protocol-machine-id", as kitty computes it.
    const std::string expected = "1:a81b6c3c9d37b0caa4a9c6b7de41059238bc101829db2975223b93d39d663b08";
    EXPECT_EQ(dndMachineId("0123456789abcdef0123456789abcdef"), expected);
    EXPECT_EQ(dndMachineId("0123456789abcdef0123456789abcdef\n"), expected);
    EXPECT_EQ(dndMachineId(" \t0123456789abcdef0123456789abcdef\r\n"), expected);
}
