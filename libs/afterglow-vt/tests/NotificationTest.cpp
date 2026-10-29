// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "TestUtil.h"

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
    t.feed("\x1b]99;i=a\x1b" "b;c<d$e:p=?;\x1b\\");
    t.feed("\x1b]99;i=ok-1.2+3_x:p=?;\x1b\\");
    EXPECT_EQ(t.host.written, "\x1b]99;i=ok-1.2+3_x:p=?;p=title,body\x1b\\");
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
    t.feed("\x1b]99;i=n2:d=0:p=icon:g=cache1;iVBORw0K\x1b\\");
    t.feed("\x1b]99;i=n2:n=ZXJyb3I=:f=Y2FyZ28=;Failed\x1b\\");
    ASSERT_EQ(t.host.requests.size(), 3u);
    const NotificationRequest& r = t.host.requests[2];
    EXPECT_EQ(r.iconData, std::string("\x89PNG\r\n", 6));
    EXPECT_EQ(r.iconCacheId, "cache1");
    EXPECT_EQ(r.iconNames, (std::vector<std::string>{"error"}));
    EXPECT_EQ(r.appName, "cargo");
}

TEST(Notification, UnsafeTextIsRejected)
{
    Term t;
    t.feed("\x1b]99;e=1;aGkKdGhlcmU=\x1b\\"); // "hi\nthere"
    EXPECT_TRUE(t.host.requests.empty());
}
