// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "TestUtil.h"
#include "../src/Base64.h"

#include <gtest/gtest.h>

using namespace AfterglowVt;
using namespace AfterglowVt::Test;

namespace {

struct NotifyHost : RecordingHost
{
    std::vector<NotificationRequest> requests;
    std::string caps;
    void desktopNotification(const NotificationRequest& r) override { requests.push_back(r); }
    std::string notificationCapabilities() override { return caps; }
};

struct Term
{
    NotifyHost host;
    Terminal t{20, 5, host};
    void feed(std::string_view s) { t.feed(s); }
};

} // namespace

TEST(Notification, TitleOnly)
{
    Term t;
    t.feed("\x1b]99;;Hello world\x1b\\");
    ASSERT_EQ(t.host.requests.size(), 1u);
    EXPECT_EQ(t.host.requests[0].kind, NotificationRequest::Kind::Show);
    EXPECT_EQ(t.host.requests[0].title, "Hello world");
    EXPECT_TRUE(t.host.requests[0].focusOnActivate);
    EXPECT_FALSE(t.host.requests[0].reportActivation);
}

TEST(Notification, MetadataLimitRejectsWholeChunkAndPreservesPendingRequest)
{
    NotificationAssembler assembler;
    NotificationRequest request;
    EXPECT_EQ(assembler.feed("i=job:d=0;start", request), NotificationAssembler::Result::Pending);
    const std::string excessive = "i=job:u=2:x=" + std::string(4096, 'x') + ";discard";
    EXPECT_EQ(assembler.feed(excessive, request), NotificationAssembler::Result::Ignored);
    EXPECT_EQ(assembler.feed("i=job:x=unknown; end", request), NotificationAssembler::Result::Ready);
    EXPECT_EQ(request.title, "start end");
    EXPECT_EQ(request.urgency, 1);
    // A metadata field exactly at the limit remains accepted.
    EXPECT_EQ(assembler.feed("x=" + std::string(4094, 'x') + ";valid", request), NotificationAssembler::Result::Ready);
    EXPECT_EQ(request.title, "valid");
}

TEST(Notification, ChunksAssembleByIdWithMetadata)
{
    Term t;
    t.feed("\x1b]99;i=job1:d=0:u=2:a=report,-focus;Build \x1b\\");
    t.feed("\x1b]99;i=other:d=0;unrelated\x1b\\");
    t.feed("\x1b]99;i=job1:d=0;done\x1b\\");
    // Body in base64 ("All 42 tests passed"), buttons split on U+2028.
    t.feed("\x1b]99;i=job1:d=0:p=body:e=1;QWxsIDQyIHRlc3RzIHBhc3NlZA==\x1b\\");
    t.feed("\x1b]99;i=job1:p=buttons:c=1:w=5000;Open\xe2\x80\xa8" "Dismiss\x1b\\");
    ASSERT_EQ(t.host.requests.size(), 1u);
    const NotificationRequest& r = t.host.requests[0];
    EXPECT_EQ(r.id, "job1");
    EXPECT_EQ(r.title, "Build done");
    EXPECT_EQ(r.body, "All 42 tests passed");
    EXPECT_EQ(r.buttons, (std::vector<std::string>{"Open", "Dismiss"}));
    EXPECT_EQ(r.urgency, 2);
    EXPECT_TRUE(r.reportActivation);
    EXPECT_FALSE(r.focusOnActivate);
    EXPECT_TRUE(r.reportClose);
    EXPECT_EQ(r.expireMs, 5000);
}

TEST(Notification, BodyBecomesTitleAndEmptyIsIgnored)
{
    Term t;
    t.feed("\x1b]99;p=body;only a body\x1b\\");
    t.feed("\x1b]99;p=body;\x1b\\");
    ASSERT_EQ(t.host.requests.size(), 1u);
    EXPECT_EQ(t.host.requests[0].title, "only a body");
    EXPECT_EQ(t.host.requests[0].body, "");
}

TEST(Notification, IdsAreSanitizedBeforeEcho)
{
    Term t;
    t.host.caps = "p=title,body";
    // Characters the OSC string keeps, C1 ST included, but the id may not.
    t.feed("\x1b]99;i=a<b$c\xc2\x9c/d:p=?;\x1b\\");
    EXPECT_EQ(t.host.written, "\x1b]99;i=abcd:p=?;p=title,body\x1b\\");
    t.host.written.clear();
    t.feed("\x1b]99;i=ok-1.2+3_x:p=?;\x1b\\");
    EXPECT_EQ(t.host.written, "\x1b]99;i=ok-1.2+3_x:p=?;p=title,body\x1b\\");
    t.host.written.clear();
    // Ids are cut at 128 bytes.
    t.feed("\x1b]99;i=" + std::string(200, 'z') + ":p=?;\x1b\\");
    EXPECT_EQ(t.host.written, "\x1b]99;i=" + std::string(128, 'z') + ":p=?;p=title,body\x1b\\");
}

TEST(Notification, QueryWithoutSupportIsSilent)
{
    Term t;
    t.feed("\x1b]99;i=q:p=?;\x1b\\");
    EXPECT_TRUE(t.host.written.empty());
}

TEST(Notification, CloseAliveAndIcons)
{
    Term t;
    t.feed("\x1b]99;i=n1:p=close;\x1b\\\x1b]99;p=close;\x1b\\\x1b]99;i=n1:p=alive;\x1b\\");
    ASSERT_EQ(t.host.requests.size(), 2u); // a close without an id does nothing
    EXPECT_EQ(t.host.requests[0].kind, NotificationRequest::Kind::Close);
    EXPECT_EQ(t.host.requests[1].kind, NotificationRequest::Kind::Alive);
    // Icon data is always base64; names and app are base64 metadata.
    t.feed("\x1b]99;i=n2:d=0:p=icon:g=cache1;iVBORw0KGgo=\x1b\\");
    t.feed("\x1b]99;i=n2:n=ZXJyb3I=:f=Y2FyZ28=;Failed\x1b\\");
    ASSERT_EQ(t.host.requests.size(), 3u);
    const NotificationRequest& r = t.host.requests[2];
    EXPECT_EQ(r.iconData, std::string("\x89PNG\r\n\x1a\n", 8));
    EXPECT_EQ(r.iconCacheId, "cache1");
    EXPECT_EQ(r.iconNames, (std::vector<std::string>{"error"}));
    EXPECT_EQ(r.appName, "cargo");
}

TEST(Notification, UnsafeTextIsRejected)
{
    Term t;
    t.feed("\x1b]99;e=1;aGkKdGhlcmU=\x1b\\"); // "hi\nthere"
    // DEL survives the OSC string; the whole notification is checked at its end.
    t.feed("\x1b]99;i=d:d=0;A\x7f\x1b\\\x1b]99;i=d;B\x1b\\");
    EXPECT_TRUE(t.host.requests.empty());
}

TEST(Notification, IconDataMustBePngJpegOrGif)
{
    Term t;
    // An SVG document: dropped, the notification still shows.
    t.feed("\x1b]99;i=s:d=0:p=icon;" + detail::base64Encode("<svg xmlns=\"http://www.w3.org/2000/svg\"/>") + "\x1b\\");
    t.feed("\x1b]99;i=s;Title\x1b\\");
    ASSERT_EQ(t.host.requests.size(), 1u);
    EXPECT_TRUE(t.host.requests[0].iconData.empty());
    t.feed("\x1b]99;i=j:d=0:p=icon;" + detail::base64Encode("\xFF\xD8\xFF\xE0") + "\x1b\\\x1b]99;i=j;T\x1b\\");
    t.feed("\x1b]99;i=g:d=0:p=icon;" + detail::base64Encode("GIF89a") + "\x1b\\\x1b]99;i=g;T\x1b\\");
    ASSERT_EQ(t.host.requests.size(), 3u);
    EXPECT_EQ(t.host.requests[1].iconData, "\xFF\xD8\xFF\xE0");
    EXPECT_EQ(t.host.requests[2].iconData, "GIF89a");
}

TEST(Notification, Base64MayBeCutAnywhere)
{
    Term t;
    // "Hello world" cut inside a base64 group.
    t.feed("\x1b]99;i=a:d=0:e=1;SGVsbG8gd2\x1b\\\x1b]99;i=a:e=1;9ybGQ=\x1b\\");
    // The euro sign cut between its UTF-8 bytes, each chunk padded on its own.
    t.feed("\x1b]99;i=b:d=0:e=1;4g==\x1b\\\x1b]99;i=b:e=1;gqw=\x1b\\");
    ASSERT_EQ(t.host.requests.size(), 2u);
    EXPECT_EQ(t.host.requests[0].title, "Hello world");
    EXPECT_EQ(t.host.requests[1].title, "\xe2\x82\xac");
}

TEST(Notification, EncodedBodyKeepsNewlines)
{
    Term t;
    t.feed("\x1b]99;i=1:d=0;Build\x1b\\");
    t.feed("\x1b]99;i=1:e=1:p=body;" + detail::base64Encode("line1\nline2\tend") + "\x1b\\");
    ASSERT_EQ(t.host.requests.size(), 1u);
    EXPECT_EQ(t.host.requests[0].title, "Build");
    EXPECT_EQ(t.host.requests[0].body, "line1\nline2 end");
    // A body that becomes the title is put on one line.
    t.feed("\x1b]99;e=1:p=body;" + detail::base64Encode("a\nb") + "\x1b\\");
    ASSERT_EQ(t.host.requests.size(), 2u);
    EXPECT_EQ(t.host.requests[1].title, "a b");
}

TEST(Notification, FailedChunkDropsTheRestOfItsNotification)
{
    Term t;
    // Bad base64 fails the first chunk.
    t.feed("\x1b]99;i=x:d=0:e=1;!!!!\x1b\\");
    t.feed("\x1b]99;i=x:d=0:p=body;more\x1b\\");
    t.feed("\x1b]99;i=x:p=body;tail\x1b\\");
    EXPECT_TRUE(t.host.requests.empty());
    // After its last chunk the id is free again.
    t.feed("\x1b]99;i=x;new\x1b\\");
    ASSERT_EQ(t.host.requests.size(), 1u);
    EXPECT_EQ(t.host.requests[0].title, "new");
}

TEST(Notification, CompleteAndBadChunksDoNotEvictHalfSentOnes)
{
    NotificationAssembler assembler;
    NotificationRequest request;
    for (int i = 0; i < 32; ++i) {
        const std::string id = std::to_string(i);
        ASSERT_EQ(assembler.feed("i=" + id + ":d=0;T" + id, request), NotificationAssembler::Result::Pending);
    }
    EXPECT_EQ(assembler.feed("i=single;complete", request), NotificationAssembler::Result::Ready);
    EXPECT_EQ(assembler.feed("i=bad;x\x7f", request), NotificationAssembler::Result::Ignored);
    EXPECT_EQ(assembler.feed("i=worse:e=1;!!!!", request), NotificationAssembler::Result::Ignored);
    ASSERT_EQ(assembler.feed("i=0;rest", request), NotificationAssembler::Result::Ready);
    EXPECT_EQ(request.title, "T0rest");
    // With 32 half-sent again, a 33rd evicts the oldest.
    EXPECT_EQ(assembler.feed("i=extra:d=0;E", request), NotificationAssembler::Result::Pending);
    EXPECT_EQ(assembler.feed("i=new:d=0;N", request), NotificationAssembler::Result::Pending);
    ASSERT_EQ(assembler.feed("i=1;rest", request), NotificationAssembler::Result::Ready);
    EXPECT_EQ(request.title, "rest");
}

TEST(Notification, PendingIconDataHasATotalLimit)
{
    NotificationAssembler assembler;
    NotificationRequest request;
    const std::string chunk(8192, 'A'); // 6 KiB of icon data
    for (const char* id : {"a", "b"}) {
        for (int i = 0; i < 682; ++i) { // just under 4 MiB each
            ASSERT_EQ(assembler.feed(std::string("i=") + id + ":d=0:p=icon;" + chunk, request),
                      NotificationAssembler::Result::Pending);
        }
    }
    EXPECT_EQ(assembler.feed("i=c:d=0:p=icon;" + chunk, request), NotificationAssembler::Result::Pending);
    EXPECT_EQ(assembler.feed("i=c:d=0:p=icon;" + chunk, request), NotificationAssembler::Result::Ignored);
    // The others are untouched, and completing one frees its share.
    EXPECT_EQ(assembler.feed("i=a;A", request), NotificationAssembler::Result::Ready);
    EXPECT_EQ(assembler.feed("i=d:d=0:p=icon;" + chunk, request), NotificationAssembler::Result::Pending);
    assembler.reset();
    EXPECT_EQ(assembler.feed("i=b;B", request), NotificationAssembler::Result::Ready);
    EXPECT_EQ(request.title, "B");
    EXPECT_TRUE(request.iconData.empty());
}

TEST(Notification, MetadataEdgeCases)
{
    Term t;
    // A bad or empty f= keeps the app name an earlier chunk set; c= is any non-zero value.
    t.feed("\x1b]99;i=1:d=0:f=Y2FyZ28=;T\x1b\\\x1b]99;i=1:f=!!:c=2;x\x1b\\");
    ASSERT_EQ(t.host.requests.size(), 1u);
    EXPECT_EQ(t.host.requests[0].appName, "cargo");
    EXPECT_TRUE(t.host.requests[0].reportClose);
    // Long values are cut at a character boundary.
    t.feed("\x1b]99;f=" + detail::base64Encode(std::string(255, 'a') + "\xc3\xa9") + ";T\x1b\\");
    ASSERT_EQ(t.host.requests.size(), 2u);
    EXPECT_EQ(t.host.requests[1].appName, std::string(255, 'a'));
    // Occasion, sound and types.
    t.feed("\x1b]99;o=unfocused:s=" + detail::base64Encode("bell") + ":t=" + detail::base64Encode("im.received")
           + ";T\x1b\\");
    t.feed("\x1b]99;o=invisible;T\x1b\\\x1b]99;o=sometimes;T\x1b\\");
    ASSERT_EQ(t.host.requests.size(), 5u);
    EXPECT_EQ(t.host.requests[2].occasion, NotificationRequest::Occasion::Unfocused);
    EXPECT_EQ(t.host.requests[2].sound, "bell");
    EXPECT_EQ(t.host.requests[2].types, (std::vector<std::string>{"im.received"}));
    EXPECT_EQ(t.host.requests[3].occasion, NotificationRequest::Occasion::Invisible);
    EXPECT_EQ(t.host.requests[4].occasion, NotificationRequest::Occasion::Always);
    EXPECT_EQ(t.host.requests[4].sound, "system");
}

TEST(Notification, ResetDropsHalfSentNotifications)
{
    Term t;
    t.feed("\x1b]99;i=a:d=0;start\x1b\\\x1b" "c\x1b]99;i=a;end\x1b\\");
    ASSERT_EQ(t.host.requests.size(), 1u);
    EXPECT_EQ(t.host.requests[0].title, "end");
}
