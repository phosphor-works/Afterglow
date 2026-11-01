// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "TestUtil.h"
#include "../src/Base64.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace AfterglowVt;
using namespace AfterglowVt::Test;

namespace {

struct ClipHost : RecordingHost
{
    std::vector<ClipboardRequest> requests;
    std::vector<std::pair<std::string, std::string>> osc52Writes;
    bool supported = true;
    void clipboardWrite(std::string_view selection, std::string_view data) override
    {
        osc52Writes.emplace_back(selection, data);
    }
    bool clipboardRequest(const ClipboardRequest& r) override
    {
        requests.push_back(r);
        return supported;
    }
};

struct Term
{
    ClipHost host;
    Terminal t{20, 5, host};
    void feed(std::string_view s) { t.feed(s); }
};

} // namespace

TEST(Clipboard, ReadRequestWithTypesAndPassword)
{
    Term t;
    // "text/plain image/png", pw "secret", name "vim", id with a bad character.
    t.feed("\x1b]5522;type=read:loc=primary:pw=c2VjcmV0:name=dmlt:id=r1$;dGV4dC9wbGFpbiBpbWFnZS9wbmc=\x1b\\");
    ASSERT_EQ(t.host.requests.size(), 1u);
    const ClipboardRequest& r = t.host.requests[0];
    EXPECT_EQ(r.kind, ClipboardRequest::Kind::Read);
    EXPECT_TRUE(r.primary);
    EXPECT_EQ(r.id, "r1");
    EXPECT_EQ(r.password, "secret");
    EXPECT_EQ(r.name, "vim");
    EXPECT_EQ(r.mimes, (std::vector<std::string>{"text/plain", "image/png"}));
}

TEST(Clipboard, WriteAssemblesTypesAndAliases)
{
    Term t;
    t.feed("\x1b]5522;type=write:id=w\x1b\\");
    // "hello world" split inside a base64 group: only the whole stream is padded.
    t.feed("\x1b]5522;type=wdata:mime=dGV4dC9wbGFpbg==;aGVsb\x1b\\");
    t.feed("\x1b]5522;type=wdata:mime=dGV4dC9wbGFpbg==;G8gd29ybGQ=\x1b\\");
    t.feed("\x1b]5522;type=walias:mime=dGV4dC9wbGFpbg==;VVRGOF9TVFJJTkcgU1RSSU5H\x1b\\"); // UTF8_STRING STRING
    t.feed("\x1b]5522;type=wdata:mime=dGV4dC9odG1s;PGI+aGk8L2I+\x1b\\"); // text/html "<b>hi</b>"
    EXPECT_TRUE(t.host.requests.empty());
    t.feed("\x1b]5522;type=wdata\x1b\\");
    ASSERT_EQ(t.host.requests.size(), 1u);
    const ClipboardRequest& r = t.host.requests[0];
    EXPECT_EQ(r.kind, ClipboardRequest::Kind::Write);
    EXPECT_EQ(r.id, "w");
    ASSERT_EQ(r.data.size(), 2u);
    EXPECT_EQ(r.data[0], (std::pair<std::string, std::string>{"text/plain", "hello world"}));
    EXPECT_EQ(r.data[1], (std::pair<std::string, std::string>{"text/html", "<b>hi</b>"}));
    ASSERT_EQ(r.aliases.size(), 2u);
    EXPECT_EQ(r.aliases[0].first, "UTF8_STRING");
    EXPECT_EQ(r.aliases[0].second, "text/plain");
    EXPECT_EQ(r.aliases[1], (std::pair<std::string, std::string>{"STRING", "text/plain"}));
    EXPECT_TRUE(t.host.written.empty()); // the host replies DONE
}

TEST(Clipboard, WriteMayBeCutBetweenItsPaddingCharacters)
{
    Term t;
    t.feed("\x1b]5522;type=write\x1b\\");
    t.feed("\x1b]5522;type=wdata:mime=dGV4dC9wbGFpbg==;QQ=\x1b\\");
    t.feed("\x1b]5522;type=wdata:mime=dGV4dC9wbGFpbg==;=\x1b\\");
    t.feed("\x1b]5522;type=wdata\x1b\\");
    EXPECT_TRUE(t.host.written.empty());
    ASSERT_EQ(t.host.requests.size(), 1u);
    ASSERT_EQ(t.host.requests[0].data.size(), 1u);
    EXPECT_EQ(t.host.requests[0].data[0], (std::pair<std::string, std::string>{"text/plain", "A"}));
    // Padding that the stream never completes is still refused at the end.
    t.feed("\x1b]5522;type=write:id=w\x1b\\");
    t.feed("\x1b]5522;type=wdata:mime=dGV4dC9wbGFpbg==;QQ=\x1b\\");
    t.feed("\x1b]5522;type=wdata\x1b\\");
    EXPECT_EQ(t.host.written, "\x1b]5522;type=write:status=EINVAL:id=w\x1b\\");
    EXPECT_EQ(t.host.requests.size(), 1u);
}

TEST(Clipboard, BadBase64FailsTheWriteUntilTheNext)
{
    Term t;
    t.feed("\x1b]5522;type=write:id=w\x1b\\\x1b]5522;type=wdata:mime=dGV4dC9wbGFpbg==;a@b=\x1b\\");
    t.feed("\x1b]5522;type=wdata\x1b\\");
    EXPECT_EQ(t.host.written, "\x1b]5522;type=write:status=EINVAL:id=w\x1b\\");
    EXPECT_TRUE(t.host.requests.empty());
    t.host.written.clear();
    // Missing padding for a type is an error too.
    t.feed("\x1b]5522;type=write\x1b\\\x1b]5522;type=wdata:mime=dGV4dC9wbGFpbg==;aGk\x1b\\\x1b]5522;type=wdata\x1b\\");
    EXPECT_EQ(t.host.written, "\x1b]5522;type=write:status=EINVAL\x1b\\");
}

TEST(Clipboard, NoSupportAnswersEnosys)
{
    Term t;
    t.host.supported = false;
    t.feed("\x1b]5522;type=read:id=x;Lg==\x1b\\");
    EXPECT_EQ(t.host.written, "\x1b]5522;type=read:status=ENOSYS:id=x\x1b\\");
}

TEST(Clipboard, ReplyEncoders)
{
    ClipboardRequest r;
    r.id = "q";
    r.primary = true;
    EXPECT_EQ(ClipboardAssembler::readStatus("OK", r), "\x1b]5522;type=read:status=OK:id=q:loc=primary\x1b\\");
    EXPECT_EQ(ClipboardAssembler::readData("text/plain", "hi", r),
              "\x1b]5522;type=read:status=DATA:id=q:mime=dGV4dC9wbGFpbg==;aGk=\x1b\\");
    const std::string big(5000, 'x');
    const std::string chunks = ClipboardAssembler::readData("a", big, r);
    // 5000 bytes in 4096-byte chunks: two packets.
    std::size_t packets = 0;
    for (std::size_t at = chunks.find("type=read:status=DATA"); at != std::string::npos;
         at = chunks.find("type=read:status=DATA", at + 1)) {
        ++packets;
    }
    EXPECT_EQ(packets, 2u);
}

TEST(Clipboard, PasteEventsModeIsReported)
{
    Term t;
    t.feed("\x1b[?5522$p\x1b[?5522h\x1b[?5522$p");
    EXPECT_EQ(t.host.written, "\x1b[?5522;2$y\x1b[?5522;1$y");
    EXPECT_TRUE(t.t.modes().pasteEvents);
}

TEST(Clipboard, PaddingEndsATypesStream)
{
    Term t;
    t.feed("\x1b]5522;type=write:id=w\x1b\\");
    t.feed("\x1b]5522;type=wdata:mime=dGV4dC9wbGFpbg==;aGk=\x1b\\");
    t.feed("\x1b]5522;type=wdata:mime=dGV4dC9wbGFpbg==;aGk=\x1b\\");
    EXPECT_EQ(t.host.written, "\x1b]5522;type=write:status=EINVAL:id=w\x1b\\");
    t.feed("\x1b]5522;type=wdata\x1b\\");
    EXPECT_TRUE(t.host.requests.empty());
}

TEST(Clipboard, ATypeSentInTwoRunsIsInvalid)
{
    Term t;
    t.feed("\x1b]5522;type=write:id=w\x1b\\");
    t.feed("\x1b]5522;type=wdata:mime=dGV4dC9wbGFpbg==;aGVs\x1b\\"); // text/plain "hel"
    t.feed("\x1b]5522;type=wdata:mime=dGV4dC9odG1s;PGI+\x1b\\"); // text/html "<b>"
    t.feed("\x1b]5522;type=wdata:mime=dGV4dC9wbGFpbg==;bG8h\x1b\\"); // text/plain "lo!"
    t.feed("\x1b]5522;type=wdata\x1b\\");
    EXPECT_EQ(t.host.written, "\x1b]5522;type=write:status=EINVAL:id=w\x1b\\");
    EXPECT_TRUE(t.host.requests.empty());
}

TEST(Clipboard, OversizedMetadataOnAReadLeavesTheWriteAlone)
{
    Term t;
    t.feed("\x1b]5522;type=write:id=w\x1b\\");
    t.feed("\x1b]5522;type=read:x=" + std::string(16384, 'a') + ";Lg==\x1b\\");
    EXPECT_TRUE(t.host.written.empty());
    t.feed("\x1b]5522;type=wdata:mime=dGV4dC9wbGFpbg==;aGk=\x1b\\\x1b]5522;type=wdata\x1b\\");
    ASSERT_EQ(t.host.requests.size(), 1u);
    EXPECT_EQ(t.host.requests[0].data[0].second, "hi");
    // On a write packet it fails the write.
    t.feed("\x1b]5522;type=write:id=v\x1b\\");
    t.feed("\x1b]5522;type=wdata:x=" + std::string(16384, 'a') + ";aGk=\x1b\\");
    EXPECT_EQ(t.host.written, "\x1b]5522;type=write:status=EFBIG:id=v\x1b\\");
}

TEST(Clipboard, NamesAndTypesMustBePlainText)
{
    Term t;
    // A name with a newline would forge a line in the permission prompt.
    t.feed("\x1b]5522;type=read:pw=eA==:name=" + detail::base64Encode("make\nVerified by Afterglow")
           + ";dGV4dC9wbGFpbg==\x1b\\");
    // A type with a control character or a space.
    t.feed("\x1b]5522;type=read;" + detail::base64Encode("text/plain\x1b[1m") + "\x1b\\");
    EXPECT_TRUE(t.host.requests.empty());
    t.feed("\x1b]5522;type=write:id=w\x1b\\\x1b]5522;type=wdata:mime=" + detail::base64Encode("text/\tplain")
           + ";aGk=\x1b\\");
    EXPECT_EQ(t.host.written, "\x1b]5522;type=write:status=EINVAL:id=w\x1b\\");
    t.host.written.clear();
    t.feed("\x1b]5522;type=write:id=a\x1b\\\x1b]5522;type=walias:mime=dGV4dC9wbGFpbg==;"
           + detail::base64Encode("STR\x7fING") + "\x1b\\");
    EXPECT_EQ(t.host.written, "\x1b]5522;type=write:status=EINVAL:id=a\x1b\\");
}

TEST(Clipboard, APasswordWithoutANameIsNoPassword)
{
    Term t;
    t.feed("\x1b]5522;type=read:pw=c2VjcmV0;dGV4dC9wbGFpbg==\x1b\\");
    ASSERT_EQ(t.host.requests.size(), 1u);
    EXPECT_TRUE(t.host.requests[0].password.empty());
    EXPECT_TRUE(t.host.requests[0].name.empty());
}

TEST(Clipboard, Osc52WritesPassTheirDataThrough)
{
    Term t;
    t.feed("\x1b]52;c;aGk=\x07");
    t.feed("\x1b]52;;aGk=\x07");
    t.feed("\x1b]52;p;\x07");
    ASSERT_EQ(t.host.osc52Writes.size(), 3u);
    EXPECT_EQ(t.host.osc52Writes[0], (std::pair<std::string, std::string>{"c", "aGk="}));
    EXPECT_EQ(t.host.osc52Writes[1], (std::pair<std::string, std::string>{"", "aGk="}));
    EXPECT_EQ(t.host.osc52Writes[2], (std::pair<std::string, std::string>{"p", ""})); // clears
}

TEST(Clipboard, ResetDropsAWriteInProgress)
{
    Term t;
    t.feed("\x1b]5522;type=write:id=w\x1b\\\x1b]5522;type=wdata:mime=dGV4dC9wbGFpbg==;aGk=\x1b\\");
    t.feed("\x1b" "c\x1b]5522;type=wdata\x1b\\");
    EXPECT_TRUE(t.host.requests.empty());
}

TEST(Clipboard, AWriteHoldsAtMost64Types)
{
    Term t;
    t.feed("\x1b]5522;type=write:id=w\x1b\\");
    for (int i = 0; i <= 64; ++i) {
        t.feed("\x1b]5522;type=wdata:mime=" + detail::base64Encode("type/" + std::to_string(i)) + ";aGk=\x1b\\");
    }
    EXPECT_TRUE(t.host.written.empty());
    t.feed("\x1b]5522;type=wdata\x1b\\"); // the end reports the 65th type
    EXPECT_EQ(t.host.written, "\x1b]5522;type=write:status=EFBIG:id=w\x1b\\");
    EXPECT_TRUE(t.host.requests.empty());
}

TEST(Clipboard, WriteLimitCountsSmallChunksAcrossTypes)
{
    // The assembler directly: the limit is its own, and 128 MiB in OSC
    // sequences would only time the parser.
    ClipboardAssembler a;
    ClipboardRequest out;
    std::string reply;
    const std::string plain = "type=wdata:mime=" + detail::base64Encode("text/plain") + ";";
    const std::string html = "type=wdata:mime=" + detail::base64Encode("text/html") + ";";
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
