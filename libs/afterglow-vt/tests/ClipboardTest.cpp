// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "TestUtil.h"

#include <gtest/gtest.h>

using namespace AfterglowVt;
using namespace AfterglowVt::Test;

namespace {

struct ClipHost : RecordingHost
{
    std::vector<ClipboardRequest> requests;
    bool supported = true;
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
    // "hello world" split across packets not on 4-byte boundaries.
    t.feed("\x1b]5522;type=wdata:mime=dGV4dC9wbGFpbg==;aGVsbG8g\x1b\\");
    t.feed("\x1b]5522;type=wdata:mime=dGV4dC9wbGFpbg==;d29ybGQ=\x1b\\");
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
    EXPECT_TRUE(t.host.written.empty()); // the host replies DONE
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
