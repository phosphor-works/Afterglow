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
        DndAssembler::Message whole;
        if (assembler.feed(*meta, payload, whole) == DndAssembler::Result::Ready) {
            messages.emplace_back(std::move(whole.meta), std::move(whole.payload));
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
    // The whole message has the m of its last chunk, and no key of the assembler's own.
    EXPECT_EQ(messages[0].first, (DndMeta{{"t", "p"}, {"x", "0"}, {"i", "7"}, {"m", "0"}}));
    EXPECT_FALSE(assembler.active());
}

TEST(DragDrop, WriterSplitsAfterAFullPacket)
{
    EXPECT_EQ(writeDnd({{"t", "p"}}, std::string(DndMaxPacket, 'a')).size(), 1u);
    const auto packets = writeDnd({{"t", "p"}}, std::string(DndMaxPacket + 1, 'a'));
    ASSERT_EQ(packets.size(), 2u);
    EXPECT_EQ(packets[1], "\x1b]72;m=0:t=p;a\x1b\\");
}

TEST(DragDrop, AssemblerLimitsAndIds)
{
    using R = DndAssembler::Result;
    DndAssembler assembler(1000, 10);
    DndAssembler::Message out;
    // A control payload at its limit, then over it.
    EXPECT_EQ(assembler.feed({{"t", "a"}, {"m", "1"}}, "12345678", out), R::Pending);
    EXPECT_EQ(assembler.type(), "a");
    EXPECT_EQ(assembler.feed({{"m", "0"}}, "90", out), R::Ready);
    EXPECT_EQ(out.payload, "1234567890");
    EXPECT_EQ(assembler.feed({{"t", "a"}, {"m", "1"}}, "12345678", out), R::Pending);
    EXPECT_EQ(assembler.feed({{"m", "0"}}, "123", out), R::TooLarge);
    EXPECT_FALSE(assembler.active());
    EXPECT_FALSE(assembler.discarding());
    // A first packet over the limit, chunked or not.
    EXPECT_EQ(assembler.feed({{"t", "m"}}, "12345678901", out), R::TooLarge);
    EXPECT_FALSE(assembler.discarding());
    EXPECT_EQ(assembler.feed({{"t", "m"}}, "1234567890", out), R::Ready);
    // A data payload has the larger limit.
    EXPECT_EQ(assembler.feed({{"t", "p"}, {"m", "1"}}, std::string(999, 'a'), out), R::Pending);
    EXPECT_EQ(assembler.feed({{"m", "0"}}, "a", out), R::Ready);
    EXPECT_EQ(out.payload.size(), 1000u);
    EXPECT_EQ(assembler.feed({{"t", "p"}, {"m", "1"}}, std::string(999, 'a'), out), R::Pending);
    EXPECT_EQ(assembler.feed({{"m", "0"}}, "ab", out), R::TooLarge);
    // The failing chunk was the last: the next packet starts a new message.
    EXPECT_FALSE(assembler.discarding());
    // Chunks may repeat the type and the id, but not name others.
    EXPECT_EQ(assembler.feed({{"t", "p"}, {"i", "1"}, {"m", "1"}}, "abc", out), R::Pending);
    EXPECT_EQ(assembler.feed({{"t", "p"}, {"i", "1"}, {"m", "1"}}, "def", out), R::Pending);
    EXPECT_EQ(assembler.feed({{"m", "0"}}, "g", out), R::Ready);
    EXPECT_EQ(out.payload, "abcdefg");
    EXPECT_EQ(assembler.feed({{"t", "p"}, {"i", "1"}, {"m", "1"}}, "abc", out), R::Pending);
    EXPECT_EQ(assembler.feed({{"i", "2"}, {"m", "0"}}, "abc", out), R::Mismatch);
    EXPECT_FALSE(assembler.discarding());
    EXPECT_EQ(assembler.feed({{"t", "p"}, {"m", "1"}}, "abc", out), R::Pending);
    EXPECT_EQ(assembler.feed({{"i", "1"}, {"m", "0"}}, "abc", out), R::Mismatch);
    EXPECT_EQ(assembler.feed({{"t", "a"}, {"m", "1"}}, "text/", out), R::Pending);
    EXPECT_EQ(assembler.feed({{"t", "a"}, {"m", "1"}}, "plain", out), R::Pending);
    EXPECT_EQ(assembler.feed({{"t", "r"}, {"x", "1"}}, "", out), R::Mismatch);
    // A message that gives no type is "a", so a chunk may name t=a.
    EXPECT_EQ(assembler.feed({{"m", "1"}}, "text/", out), R::Pending);
    EXPECT_EQ(assembler.feed({{"t", "a"}, {"m", "0"}}, "plain", out), R::Ready);
}

TEST(DragDrop, ChunksOfAFailedMessageAreDropped)
{
    using R = DndAssembler::Result;
    DndAssembler assembler(1000, 10);
    DndAssembler::Message out;
    EXPECT_EQ(assembler.feed({{"t", "m"}, {"m", "1"}}, "12345678", out), R::Pending);
    EXPECT_EQ(assembler.feed({{"m", "1"}}, "123", out), R::TooLarge);
    EXPECT_TRUE(assembler.discarding());
    // The tail starts no new message and reports no second error.
    EXPECT_EQ(assembler.feed({{"m", "1"}}, "123456789012", out), R::Dropped);
    EXPECT_EQ(assembler.feed({{"m", "0"}}, "1", out), R::Dropped);
    EXPECT_FALSE(assembler.discarding());
    EXPECT_EQ(assembler.feed({{"t", "a"}}, "text/plain", out), R::Ready);
    EXPECT_EQ(out.meta.at("t"), "a");
    // Another id: the rest of the message goes too.
    EXPECT_EQ(assembler.feed({{"t", "p"}, {"i", "1"}, {"m", "1"}}, "abc", out), R::Pending);
    EXPECT_EQ(assembler.feed({{"i", "2"}, {"m", "1"}}, "abc", out), R::Mismatch);
    EXPECT_EQ(assembler.feed({{"m", "0"}}, "abc", out), R::Dropped);
    // An error found by the caller: a first packet with m=1 drops its tail.
    assembler.discard(dndMore("t=a:m=1"));
    EXPECT_EQ(assembler.feed({{"m", "0"}}, "x", out), R::Dropped);
    assembler.discard(dndMore("t=a"));
    EXPECT_FALSE(assembler.discarding());
    // The packet decides, not the message being put together.
    EXPECT_EQ(assembler.feed({{"t", "m"}, {"m", "1"}}, "abc", out), R::Pending);
    assembler.discard(false);
    EXPECT_FALSE(assembler.discarding());
    EXPECT_FALSE(assembler.active());
    EXPECT_EQ(assembler.feed({{"t", "a"}}, "text/plain", out), R::Ready);
    // The protocol ended: a message being put together runs on to its end.
    assembler.abandon();
    EXPECT_FALSE(assembler.discarding());
    EXPECT_EQ(assembler.feed({{"t", "m"}, {"m", "1"}}, "abc", out), R::Pending);
    assembler.abandon();
    EXPECT_TRUE(assembler.discarding());
    EXPECT_FALSE(assembler.active());
    // A message that is being dropped stays so.
    assembler.abandon();
    EXPECT_TRUE(assembler.discarding());
    assembler.reset();
    EXPECT_FALSE(assembler.discarding());
    EXPECT_EQ(assembler.feed({{"m", "0"}}, "x", out), R::Ready);
}

TEST(DragDrop, MoreReadsMetadataThatDoesNotParse)
{
    EXPECT_TRUE(dndMore("m=1"));
    EXPECT_TRUE(dndMore("t=p:m=2"));
    EXPECT_TRUE(dndMore("i=bad:m=1"));
    EXPECT_TRUE(dndMore("t=e:t=e:m=-1"));
    EXPECT_TRUE(dndMore("m= 1 "));
    EXPECT_FALSE(dndMore(""));
    EXPECT_FALSE(dndMore("t=p"));
    EXPECT_FALSE(dndMore("m=0"));
    EXPECT_FALSE(dndMore("m=x"));
    EXPECT_FALSE(dndMore("mm=1"));
    // The first m counts, as parseDndMeta refuses a repeated key.
    EXPECT_FALSE(dndMore("m=0:m=1"));
    // It agrees with metadata that parses.
    for (const auto* raw : {"m=1", "m=0", "t=a:i=5", "m=+7:t=p", "m=00"}) {
        const auto meta = parseDndMeta(raw);
        ASSERT_TRUE(meta) << raw;
        const auto m = meta->find("m");
        EXPECT_EQ(dndMore(raw), m != meta->end() && m->second != "0") << raw;
    }
}

TEST(DragDrop, MoreIsANumberAndIdsCompareByValue)
{
    using R = DndAssembler::Result;
    DndAssembler assembler(1000, 100);
    DndAssembler::Message out;
    const auto packet = [](std::string_view raw) {
        const auto meta = parseDndMeta(raw);
        EXPECT_TRUE(meta);
        return meta.value_or(DndMeta{});
    };
    // kitty goes on while m is not 0.
    EXPECT_EQ(assembler.feed(packet("t=a:i=5:m=01"), "a", out), R::Pending);
    EXPECT_EQ(assembler.feed(packet("i=+5:m=2"), "b", out), R::Pending);
    EXPECT_EQ(assembler.feed(packet("i= 5 :m=00"), "c", out), R::Ready);
    EXPECT_EQ(out.payload, "abc");
    EXPECT_EQ(out.meta.at("i"), "5");
    EXPECT_EQ(out.meta.at("m"), "0");
}

TEST(DragDrop, TheLastPacketTellsWhetherItWasEmpty)
{
    DndAssembler assembler(1000, 100);
    DndAssembler::Message out;
    EXPECT_EQ(assembler.feed({{"t", "p"}, {"m", "1"}}, "abc", out), DndAssembler::Result::Pending);
    EXPECT_EQ(assembler.feed({{"m", "0"}}, "", out), DndAssembler::Result::Ready);
    EXPECT_TRUE(out.emptyEnd);
    EXPECT_EQ(out.payload, "abc");
    EXPECT_FALSE(out.meta.contains("_end"));
    EXPECT_EQ(assembler.feed({{"t", "p"}, {"m", "1"}}, "abc", out), DndAssembler::Result::Pending);
    EXPECT_EQ(assembler.feed({{"m", "0"}}, "d", out), DndAssembler::Result::Ready);
    EXPECT_FALSE(out.emptyEnd);
    EXPECT_EQ(assembler.feed({{"t", "p"}}, "", out), DndAssembler::Result::Ready);
    EXPECT_TRUE(out.emptyEnd);
    EXPECT_EQ(assembler.feed({{"t", "p"}}, "abc", out), DndAssembler::Result::Ready);
    EXPECT_FALSE(out.emptyEnd);
}

TEST(DragDrop, MetadataRules)
{
    EXPECT_TRUE(parseDndMeta("t=a:i= 5 :x=+3:zz=1:q:=1"));
    const auto normalized = parseDndMeta("t=a:i= 5 :x=+03:y=-0:zz=1");
    ASSERT_TRUE(normalized);
    EXPECT_EQ(*normalized, (DndMeta{{"t", "a"}, {"i", "5"}, {"x", "3"}, {"y", "0"}}));
    EXPECT_TRUE(parseDndMeta("x=4294967295:y=-2147483648"));
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
    EXPECT_FALSE(parseDndInteger("-9223372036854775809"));
    EXPECT_EQ(parseDndInteger("+7"), 7);
    EXPECT_EQ(parseDndInteger("\t42\r\n"), 42);
    EXPECT_EQ(parseDndInteger("-0"), 0);
    EXPECT_EQ(parseDndInteger("007"), 7);
    EXPECT_FALSE(parseDndInteger(""));
    EXPECT_FALSE(parseDndInteger("   "));
    EXPECT_FALSE(parseDndInteger("+"));
    EXPECT_FALSE(parseDndInteger("-"));
    EXPECT_FALSE(parseDndInteger("+-1"));
    EXPECT_FALSE(parseDndInteger("1 2"));
    EXPECT_FALSE(parseDndInteger("1e3"));
    EXPECT_FALSE(parseDndInteger(std::string_view("4\0", 2)));
}

TEST(DragDrop, TypeRoles)
{
    for (const auto* type : {"a", "A", "m", "r"}) {
        EXPECT_TRUE(dndReceiverType(type)) << type;
        EXPECT_FALSE(dndDataType(type)) << type;
    }
    for (const auto* type : {"p", "e", "k"}) {
        EXPECT_FALSE(dndReceiverType(type)) << type;
        EXPECT_TRUE(dndDataType(type)) << type;
    }
    for (const auto* type : {"o", "P", "E", "M", "R", "q", ""}) {
        EXPECT_FALSE(dndReceiverType(type)) << type;
        EXPECT_FALSE(dndDataType(type)) << type;
    }
}

TEST(DragDrop, QueryReplies)
{
    EXPECT_EQ(dndQueryReply("t=q:i=4294967295;"), "\x1b]72;t=q:i=4294967295;\x1b\\");
    EXPECT_EQ(dndQueryReply("t=Q"), "\x1b]72;t=Q;\x1b\\");
    EXPECT_EQ(dndQueryReply("t=q:i=4294967296;"), "");
    EXPECT_EQ(dndQueryReply("t=o:x=1;"), "");
    EXPECT_EQ(dndQueryReply("i=3:t=q"), "\x1b]72;t=q:i=3;\x1b\\");
    // The id goes back in plain decimal.
    EXPECT_EQ(dndQueryReply("t=q:i= +07 ;"), "\x1b]72;t=q:i=7;\x1b\\");
    // A query that breaks the rules of every message is left to the terminal, which reports it.
    EXPECT_EQ(dndQueryReply("t=q:t=q;"), "");
    EXPECT_EQ(dndQueryReply("t=q:x=bad;"), "");
    EXPECT_EQ(dndQueryReply("t=q:i=1:i=1;"), "");
    EXPECT_EQ(dndQueryReply("t=q;" + std::string(DndMaxPacket, 'a')), "\x1b]72;t=q;\x1b\\");
    EXPECT_EQ(dndQueryReply("t=q;" + std::string(DndMaxPacket + 1, 'a')), "");
    const std::string longMeta = "t=q:" + std::string(DndMaxCommand, 'z') + ";";
    EXPECT_EQ(dndQueryReply(longMeta), "");
}

TEST(DragDrop, MimeNames)
{
    EXPECT_TRUE(dndValidMime("text/plain"));
    EXPECT_FALSE(dndValidMime("text"));
    EXPECT_FALSE(dndValidMime("text/pl ain"));
    EXPECT_FALSE(dndValidMime(""));
    EXPECT_TRUE(dndValidMime("a/" + std::string(253, 'b')));
    EXPECT_FALSE(dndValidMime("a/" + std::string(254, 'b')));
    EXPECT_TRUE(dndValidMime("!/~"));
    EXPECT_FALSE(dndValidMime("text/plain\x7f"));
    EXPECT_FALSE(dndValidMime("text/pl\tain"));
    EXPECT_FALSE(dndValidMime("text/pl\xc3\xa4in"));
}

TEST(DragDrop, MachineIdIsAKeyedHashOfTheTrimmedFile)
{
    // HMAC-SHA256 keyed with "tty-dnd-protocol-machine-id", as kitty computes it.
    const std::string expected = "1:a81b6c3c9d37b0caa4a9c6b7de41059238bc101829db2975223b93d39d663b08";
    EXPECT_EQ(dndMachineId("0123456789abcdef0123456789abcdef"), expected);
    EXPECT_EQ(dndMachineId("0123456789abcdef0123456789abcdef\n"), expected);
    EXPECT_EQ(dndMachineId(" \t0123456789abcdef0123456789abcdef\r\n"), expected);
    // An empty or blank file (and a directory, which reads as empty) gives no ID to compare.
    EXPECT_EQ(dndMachineId(std::string_view()), "");
    EXPECT_EQ(dndMachineId(" \n"), "");
}
