// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include <AfterglowProto/Clipboard.h>

#include <gtest/gtest.h>

using namespace AfterglowProto;

namespace {

/// The payloads (after "5522;") of the OSC 5522 sequences in @p stream.
std::vector<std::string> payloads(std::string_view stream)
{
    std::vector<std::string> out;
    while (!stream.empty()) {
        EXPECT_TRUE(stream.starts_with("\x1b]5522;"));
        const auto end = stream.find("\x1b\\");
        EXPECT_NE(end, std::string_view::npos);
        out.emplace_back(stream.substr(7, end - 7));
        stream.remove_prefix(end + 2);
    }
    return out;
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
    request.name = "aglow";
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
    // The large type goes in chunks of at most 4096 base64 bytes.
    for (const auto& payload : payloads(stream)) {
        EXPECT_LE(payload.size() - std::min(payload.size(), payload.find(';') + 1), ClipboardAssembler::ReadChunk);
    }
    EXPECT_EQ(assemble(stream), request);
}

TEST(Clipboard, RepliesRoundTrip)
{
    ClipboardRequest request;
    request.id = "q";
    request.primary = true;
    const auto status = ClipboardReply::parse(payloads(ClipboardAssembler::readStatus("OK", request, "one-time")).at(0));
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
    int packets = 0;
    for (const auto& payload : payloads(ClipboardAssembler::readData("image/png", bytes, request))) {
        const auto reply = ClipboardReply::parse(payload);
        ASSERT_TRUE(reply);
        EXPECT_EQ(reply->status, "DATA");
        EXPECT_EQ(reply->mime, "image/png");
        received += reply->data;
        ++packets;
    }
    EXPECT_EQ(packets, 3);
    EXPECT_EQ(received, bytes);

    const auto written = ClipboardReply::parse(payloads(ClipboardAssembler::writeStatus("EPERM", "w")).at(0));
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
    EXPECT_EQ(Osc52Message::parse(std::string_view(reply).substr(5, reply.size() - 7))->text, "pasted");
}
