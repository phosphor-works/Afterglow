// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Base64.h>
#include <AfterglowProto/Clipboard.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

using namespace AfterglowProto;

namespace {

/// The payloads (after "5522;") of the OSC 5522 sequences in @p stream. A
/// malformed stream is a failure, and splitting stops there.
std::vector<std::string> payloads(std::string_view stream)
{
    std::vector<std::string> out;
    while (!stream.empty()) {
        const auto end = stream.find("\x1b\\");
        if (!stream.starts_with("\x1b]5522;") || end == std::string_view::npos) {
            ADD_FAILURE() << "not a whole OSC 5522 sequence: " << stream.substr(0, 16);
            break;
        }
        out.emplace_back(stream.substr(7, end - 7));
        stream.remove_prefix(end + 2);
    }
    return out;
}

/// Feeds @p payloads to @p assembler, expecting each to be pending.
void feedPending(ClipboardAssembler& assembler, std::initializer_list<std::string_view> payloads)
{
    for (const auto payload : payloads) {
        ClipboardRequest out;
        std::string reply;
        EXPECT_EQ(assembler.feed(payload, out, reply), ClipboardAssembler::Result::Pending) << payload << ": " << reply;
    }
}

/// Feeds a program's packets to a terminal's assembler; the request it makes of them.
std::optional<ClipboardRequest> assemble(std::string_view stream)
{
    ClipboardAssembler assembler;
    std::optional<ClipboardRequest> ready;
    for (const auto& payload : payloads(stream)) {
        ClipboardRequest request;
        std::string reply;
        const auto result = assembler.feed(payload, request, reply);
        EXPECT_NE(result, ClipboardAssembler::Result::Error) << reply;
        if (result == ClipboardAssembler::Result::Ready) {
            EXPECT_FALSE(ready);
            ready = std::move(request);
        }
    }
    return ready;
}

} // namespace

TEST(Clipboard, ReadRequestRoundTrip)
{
    ClipboardRequest request;
    request.kind = ClipboardRequest::Kind::Read;
    request.primary = true;
    request.id = "r-1";
    request.password = "secret";
    request.name = "editor";
    request.mimes = {"text/plain", "image/png"};
    EXPECT_EQ(assemble(clipboardReadRequest(request)), request);
    // A password without a name is not sent: the terminal would ignore it.
    request.name.clear();
    const std::string anonymous = clipboardReadRequest(request);
    EXPECT_EQ(anonymous.find("pw="), std::string::npos);
    request.password.clear();
    EXPECT_EQ(assemble(anonymous), request);
}

TEST(Clipboard, WriteRequestRoundTrip)
{
    ClipboardRequest request;
    request.kind = ClipboardRequest::Kind::Write;
    request.id = "w";
    std::string big;
    for (int i = 0; i < 20000; ++i) {
        big += char(i * 31);
    }
    request.data = {{"text/plain", "hello world"}, {"image/png", big}, {"text/html", ""}};
    request.aliases = {{"UTF8_STRING", "text/plain"}, {"STRING", "text/plain"}, {"image/x-png", "image/png"}};
    const std::string stream = clipboardWriteRequest(request);
    // The large type goes in chunks of at most 4096 bytes before base64, and
    // only its last chunk is padded.
    std::string joined;
    for (const auto& payload : payloads(stream)) {
        const auto semi = payload.find(';');
        const auto data = semi == std::string::npos ? std::string_view{} : std::string_view(payload).substr(semi + 1);
        EXPECT_LE(data.size() / 4 * 3, ClipboardAssembler::MaxChunk);
        if (payload.starts_with("type=wdata:mime=" + base64Encode("image/png") + ";")) {
            joined += data;
        }
    }
    EXPECT_EQ(base64Decode(joined, Base64Check::Strict), big);
    EXPECT_EQ(assemble(stream), request);
}

TEST(Clipboard, RepliesRoundTrip)
{
    ClipboardRequest request;
    request.id = "q";
    request.primary = true;
    const auto statusPackets = payloads(ClipboardAssembler::readStatus("OK", request, "one-time"));
    ASSERT_EQ(statusPackets.size(), 1u);
    const auto status = ClipboardReply::parse(statusPackets[0]);
    ASSERT_TRUE(status);
    EXPECT_EQ(status->type, "read");
    EXPECT_EQ(status->status, "OK");
    EXPECT_EQ(status->id, "q");
    EXPECT_TRUE(status->primary);
    EXPECT_EQ(status->password, "one-time");

    std::string bytes;
    for (int i = 0; i < 10000; ++i) {
        bytes += char(i);
    }
    std::string received;
    std::string joined;
    int packets = 0;
    for (const auto& payload : payloads(ClipboardAssembler::readData("image/png", bytes, request))) {
        const auto reply = ClipboardReply::parse(payload);
        ASSERT_TRUE(reply);
        EXPECT_EQ(reply->status, "DATA");
        EXPECT_EQ(reply->mime, "image/png");
        EXPECT_LE(reply->data.size(), ClipboardAssembler::MaxChunk);
        received += reply->data;
        joined += payload.substr(payload.find(';') + 1);
        ++packets;
    }
    EXPECT_EQ(packets, 3); // 10,000 bytes in chunks of 4095
    EXPECT_EQ(received, bytes);
    // A client that joins the packets of a type before it decodes them gets
    // one valid stream: only the last packet is padded.
    EXPECT_EQ(base64Decode(joined, Base64Check::Strict), bytes);

    const auto writtenPackets = payloads(ClipboardAssembler::writeStatus("EPERM", "w"));
    ASSERT_EQ(writtenPackets.size(), 1u);
    const auto written = ClipboardReply::parse(writtenPackets[0]);
    ASSERT_TRUE(written);
    EXPECT_EQ(written->type, "write");
    EXPECT_EQ(written->status, "EPERM");
    EXPECT_EQ(written->id, "w");
    EXPECT_FALSE(ClipboardReply::parse("type=read:status=DATA:mime=!!;"));
    EXPECT_FALSE(ClipboardReply::parse("type=read:status=DATA;QQ="));
}

TEST(Clipboard, Osc52RoundTrip)
{
    const Osc52Message write{"c", false, "hello\nworld"};
    const std::string out = write.write();
    EXPECT_EQ(out, "\x1b]52;c;aGVsbG8Kd29ybGQ=\x1b\\");
    EXPECT_EQ(Osc52Message::parse(std::string_view(out).substr(5, out.size() - 7)), write);
    const Osc52Message query{"p", true, ""};
    EXPECT_EQ(query.write(), "\x1b]52;p;?\x1b\\");
    EXPECT_EQ(Osc52Message::parse("p;?"), query);
    EXPECT_FALSE(Osc52Message::parse("c"));
    // A terminal's answer to a read is what ClipboardRequest::osc52Reply writes.
    ClipboardRequest read;
    read.osc52Selection = "c";
    const std::string reply = read.osc52Reply("pasted");
    const auto answer = Osc52Message::parse(std::string_view(reply).substr(5, reply.size() - 7));
    ASSERT_TRUE(answer);
    EXPECT_EQ(answer->text, "pasted");
}

TEST(Clipboard, WritersSendOnlyWhatATerminalAccepts)
{
    ClipboardRequest request;
    request.kind = ClipboardRequest::Kind::Write;
    request.id = "w:1;\x1b]52;c;";
    request.data = {{"text/plain", "kept"}, {"text/ plain", "dropped"}, {"", "dropped"}};
    request.aliases = {{"UTF8 STRING", "text/plain"}, {"STRING", "text/\x1bplain"}, {"TEXT", "text/plain"}};
    const auto received = assemble(clipboardWriteRequest(request));
    ASSERT_TRUE(received);
    EXPECT_EQ(received->id, "w152c");
    EXPECT_EQ(received->data, (std::vector<std::pair<std::string, std::string>>{{"text/plain", "kept"}}));
    EXPECT_EQ(received->aliases, (std::vector<std::pair<std::string, std::string>>{{"TEXT", "text/plain"}}));

    ClipboardRequest read;
    read.id = "r\x07";
    read.mimes = {"text/plain", "bad type", "image/png"};
    const auto asked = assemble(clipboardReadRequest(read));
    ASSERT_TRUE(asked);
    EXPECT_EQ(asked->id, "r");
    EXPECT_EQ(asked->mimes, (std::vector<std::string>{"text/plain", "image/png"}));
}

TEST(Clipboard, RepliesKeepOnlySafeIdsAndSelections)
{
    const auto reply = ClipboardReply::parse("type=write:status=OK:id=a\x1b[1mb");
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->id, "a1mb");
    const auto osc52 = Osc52Message::parse("c\x1b]p;aGk=");
    ASSERT_TRUE(osc52);
    EXPECT_EQ(osc52->selection, "cp");
    EXPECT_EQ(osc52->text, "hi");
    EXPECT_EQ((Osc52Message{"s;\x07", false, "hi"}.write()), "\x1b]52;s;aGk=\x1b\\");
    EXPECT_EQ(osc52Letters("cpqs01234567xyz;"), "cpqs01234567");
    EXPECT_EQ(osc52Letters(std::string(16, 'x') + "c"), ""); // only the first 16 bytes count
}

TEST(Clipboard, OversizedWriteMetadataNamesTheWrite)
{
    ClipboardAssembler a;
    ClipboardRequest out;
    std::string reply;
    const std::string metadata = "type=write:id=big:x=" + std::string(16384, 'a');
    EXPECT_EQ(a.feed(metadata, out, reply), ClipboardAssembler::Result::Error);
    EXPECT_EQ(reply, "\x1b]5522;type=write:status=EFBIG:id=big\x1b\\");
    // The rest of that write is ignored.
    reply.clear();
    EXPECT_EQ(a.feed("type=wdata:mime=dGV4dC9wbGFpbg==;aGk=", out, reply), ClipboardAssembler::Result::Ignored);
    EXPECT_TRUE(reply.empty());
}

TEST(Clipboard, ChunksSplitAnywhereAssembleTheSameData)
{
    const std::string data = "the quick brown fox jumps"; // 25 bytes: padded with "=="
    const std::string encoded = base64Encode(data);
    const std::string prefix = "type=wdata:mime=" + base64Encode("text/plain") + ";";
    for (std::size_t cut = 0; cut <= encoded.size(); ++cut) {
        for (std::size_t second = cut; second <= encoded.size(); ++second) {
            ClipboardAssembler a;
            ClipboardRequest out;
            std::string reply;
            feedPending(a, {"type=write:id=s", prefix + encoded.substr(0, cut), prefix + encoded.substr(cut, second - cut),
                            prefix + encoded.substr(second)});
            const auto result = a.feed("type=wdata", out, reply);
            EXPECT_EQ(result, ClipboardAssembler::Result::Ready) << cut << " " << second << ": " << reply;
            EXPECT_EQ(out.data.size(), 1u) << cut << " " << second;
            if (result != ClipboardAssembler::Result::Ready || out.data.size() != 1) {
                continue;
            }
            EXPECT_EQ(out.data[0].second, data) << cut << " " << second;
        }
    }
}

TEST(Clipboard, MalformedWritesFail)
{
    const std::string plain = "type=wdata:mime=" + base64Encode("text/plain") + ";";
    const std::string html = "type=wdata:mime=" + base64Encode("text/html") + ";";
    struct Case
    {
        std::vector<std::string> packets;
        std::string status;
    };
    const std::vector<Case> cases = {
        {{plain + "QQ==QUJD"}, "EINVAL"}, // data after padding, in one chunk
        {{plain + "QQ=", plain + "=QUJD"}, "EINVAL"}, // data after padding, across chunks
        {{plain + "QQ==", plain + "=QUJ"}, "EINVAL"}, // a padded chunk, then a group that starts with padding
        {{plain + "QUI=", plain + "="}, "EINVAL"}, // a padded chunk, then padding alone
        {{plain + "QUJ"}, "EINVAL"}, // the stream ends short of a group
        {{plain + "Q===", "type=wdata"}, "EINVAL"}, // malformed padding
        {{plain + "QU!D"}, "EINVAL"}, // outside the alphabet
        {{plain + "aGk=", html + "aGk=", plain + "aGk="}, "EINVAL"}, // a type sent again
        {{"type=wdata;aGk="}, "EINVAL"}, // data without a type
        {{"type=wdata:mime=!!!;aGk="}, "EINVAL"}, // a type that is not base64
        {{"type=walias;" + base64Encode("STRING")}, "EINVAL"}, // an alias without a type
        {{"type=walias:mime=" + base64Encode("text/plain") + ";" + base64Encode("BAD\tALIAS")}, "EINVAL"},
    };
    for (const auto& c : cases) {
        ClipboardAssembler a;
        ClipboardRequest out;
        std::string reply;
        auto result = a.feed("type=write:id=m", out, reply);
        for (const auto& packet : c.packets) {
            if (result != ClipboardAssembler::Result::Pending) {
                break;
            }
            result = a.feed(packet, out, reply);
        }
        if (result == ClipboardAssembler::Result::Pending) {
            result = a.feed("type=wdata", out, reply);
        }
        EXPECT_EQ(result, ClipboardAssembler::Result::Error) << c.packets.back();
        EXPECT_EQ(reply, "\x1b]5522;type=write:status=" + c.status + ":id=m\x1b\\") << c.packets.back();
    }
}

TEST(Clipboard, ChunksPaddedOneByOneAssembleAsKittenSendsThem)
{
    // kitten clipboard reads 4096 bytes at a time and encodes each read on its
    // own, so every chunk ends with padding (4096 is not a multiple of 3).
    const std::string first(4096, 'a');
    const std::string second(10, 'b');
    const std::string plain = "type=wdata:mime=" + base64Encode("text/plain") + ";";
    ClipboardAssembler a;
    ClipboardRequest out;
    std::string reply;
    ASSERT_EQ(a.feed("type=write:id=k", out, reply), ClipboardAssembler::Result::Pending);
    ASSERT_TRUE(base64Encode(first).ends_with("=="));
    EXPECT_EQ(a.feed(plain + base64Encode(first), out, reply), ClipboardAssembler::Result::Pending);
    EXPECT_EQ(a.feed(plain + base64Encode(second), out, reply), ClipboardAssembler::Result::Pending);
    ASSERT_EQ(a.feed("type=wdata", out, reply), ClipboardAssembler::Result::Ready) << reply;
    ASSERT_EQ(out.data.size(), 1u);
    EXPECT_EQ(out.data[0].first, "text/plain");
    EXPECT_EQ(out.data[0].second, first + second);
}

TEST(Clipboard, MalformedReadsAndStrayPacketsAreIgnored)
{
    ClipboardAssembler a;
    ClipboardRequest out;
    std::string reply;
    for (const std::string& payload : {
             "type=read;" + base64Encode("text/plain\x1b[1m"), // a control character
             std::string("type=read;QQ="), // not strict base64
             std::string("type=read;"), // no types
             "type=read:mime=!!;" + base64Encode("text/plain"), // a bad field
             std::string("type=wdata:mime=dGV4dC9wbGFpbg==;aGk="), // data without a write
             std::string("type=wdata"), // an end without a write
             std::string("type=unknown"),
         }) {
        EXPECT_EQ(a.feed(payload, out, reply), ClipboardAssembler::Result::Ignored) << payload;
        EXPECT_TRUE(reply.empty()) << payload;
    }
}

TEST(Clipboard, WriteLimitCountsSmallChunksAcrossTypes)
{
    // 128 MiB in OSC sequences through a terminal would only time its parser.
    ClipboardAssembler a;
    ClipboardRequest out;
    std::string reply;
    const std::string plain = "type=wdata:mime=" + base64Encode("text/plain") + ";";
    const std::string html = "type=wdata:mime=" + base64Encode("text/html") + ";";
    ASSERT_EQ(a.feed("type=write:id=w", out, reply), ClipboardAssembler::Result::Pending);
    // text/plain: all but 3,000 bytes of the limit, in large chunks.
    constexpr std::size_t Rest = 3000;
    const std::size_t first = ClipboardAssembler::MaxWrite - Rest;
    ASSERT_EQ(first % 3, 2u); // whole groups, then one padded group of two bytes
    const std::string chunk(4u << 20, 'A');
    std::size_t groups = (first - 2) / 3;
    while (groups > 0) {
        const std::size_t n = std::min(groups, chunk.size() / 4);
        ASSERT_EQ(a.feed(plain + chunk.substr(0, n * 4), out, reply), ClipboardAssembler::Result::Pending);
        groups -= n;
    }
    ASSERT_EQ(a.feed(plain + "AAA=", out, reply), ClipboardAssembler::Result::Pending);
    // text/html: the rest, in chunks of one to three characters, reaches the limit exactly.
    std::size_t fed = 0;
    for (std::size_t i = 0; fed < Rest / 3 * 4; ++i) {
        const std::size_t n = std::min<std::size_t>(1 + i % 3, Rest / 3 * 4 - fed);
        ASSERT_EQ(a.feed(html + std::string(n, 'A'), out, reply), ClipboardAssembler::Result::Pending) << fed;
        fed += n;
    }
    EXPECT_TRUE(reply.empty());
    // One more group goes over, whichever chunk completes it.
    for (int i = 0; i < 3; ++i) {
        ASSERT_EQ(a.feed(html + "A", out, reply), ClipboardAssembler::Result::Pending);
    }
    EXPECT_EQ(a.feed(html + "A", out, reply), ClipboardAssembler::Result::Error);
    EXPECT_EQ(reply, "\x1b]5522;type=write:status=EFBIG:id=w\x1b\\");
}
