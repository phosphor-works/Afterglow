// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "TestUtil.h"
#include "../src/Base64.h"

#include <gtest/gtest.h>

#include <algorithm>

using namespace AfterglowVt;
using namespace AfterglowVt::Test;

namespace {

struct StatusHost : RecordingHost
{
    int changes = 0;
    void programStatusChanged() override { ++changes; }
};

struct Term
{
    StatusHost host;
    Terminal t{20, 5, host};
    void feed(std::string_view s) { t.feed(s); }
    const std::vector<StatusRecord>& records() const { return t.programStatus().records(); }
};

} // namespace

TEST(ProgramStatus, ReportReplacesTheRecord)
{
    Term t;
    // msg "Apply 3?" in base64, unpadded; unknown keys are ignored.
    t.feed("\x1b]7501;state=blocked:kind=permission:app=terraform:msg=QXBwbHkgMz8:future=x\x1b\\");
    ASSERT_EQ(t.records().size(), 1u);
    const StatusRecord& r = t.records()[0];
    EXPECT_EQ(r.id, "");
    EXPECT_EQ(r.state, StatusState::Blocked);
    EXPECT_EQ(r.kind, BlockKind::Permission);
    EXPECT_EQ(r.app, "terraform");
    EXPECT_EQ(r.msg, "Apply 3?");
    EXPECT_EQ(t.host.changes, 1);
    // A report replaces the record whole: keys left out are dropped.
    t.feed("\x1b]7501; state = working : progress=40\x07");
    ASSERT_EQ(t.records().size(), 1u);
    EXPECT_EQ(t.records()[0].state, StatusState::Working);
    EXPECT_EQ(t.records()[0].progress, 40);
    EXPECT_EQ(t.records()[0].app, "");
    EXPECT_EQ(t.records()[0].kind, BlockKind::None);
}

TEST(ProgramStatus, KindAndProgressOnlyWhereTheyApply)
{
    Term t;
    t.feed("\x1b]7501;state=done:kind=auth:progress=50\x1b\\");
    EXPECT_EQ(t.records()[0].kind, BlockKind::None);
    EXPECT_EQ(t.records()[0].progress, -1);
    t.feed("\x1b]7501;state=working:progress=101\x1b\\"); // out of range: indeterminate
    EXPECT_EQ(t.records()[0].progress, -1);
    t.feed("\x1b]7501;state=blocked:kind=telepathy\x1b\\"); // unknown kind: absent
    EXPECT_EQ(t.records()[0].kind, BlockKind::None);
}

TEST(ProgramStatus, BadReportsAreDiscardedWhole)
{
    Term t;
    t.feed("\x1b]7501;state=working:app=cargo\x1b\\");
    const auto good = t.records()[0];
    for (const char* bad : {
             "\x1b]7501;state=sleeping\x1b\\", // unknown state
             "\x1b]7501;app=cargo\x1b\\", // no state
             "\x1b]7501;state=done:msg=@@@@\x1b\\", // invalid base64
             "\x1b]7501;state=done:msg=aGkKdGhlcmU=\x1b\\", // "hi\nthere": control character
             "\x1b]7501;state=done:app=abcdefghijklmnopqrstuvwxyz0123456789\x1b\\", // app over 32 bytes
             "\x1b]7501;state=done:averyveryverylongkey=1\x1b\\", // key over 16 bytes
             "\x1b]7501;state=done:id=a//b\x1b\\", // invalid id must not hit the root
             "\x1b]7501;state=done:id=1/2/3/4/5/6/7/8/9\x1b\\", // nine levels
         }) {
        t.feed(bad);
        ASSERT_EQ(t.records().size(), 1u) << bad;
        EXPECT_EQ(t.records()[0].state, good.state) << bad;
        EXPECT_EQ(t.records()[0].serial, good.serial) << bad;
    }
}

TEST(ProgramStatus, SizeLimits)
{
    ProgramStatus status;
    auto repeat = [](std::string_view s, int n) {
        std::string out;
        for (int i = 0; i < n; ++i) {
            out += s;
        }
        return out;
    };
    // msg: 2048 bytes decoded at most ("QUFB" is "AAA").
    EXPECT_EQ(status.apply("state=done:msg=" + repeat("QUFB", 682) + "QUE="), ProgramStatus::Result::Changed);
    EXPECT_EQ(status.records()[0].msg.size(), 2048u);
    EXPECT_EQ(status.apply("state=working:msg=" + repeat("QUFB", 683)), ProgramStatus::Result::Ignored);
    EXPECT_EQ(status.records()[0].state, StatusState::Done);
    // The whole sequence: 4096 bytes with its 7-byte introducer and terminator.
    const std::string body = "state=working:x=" + std::string(4072, 'a'); // 4088 bytes
    EXPECT_EQ(status.apply(body, 2), ProgramStatus::Result::Ignored); // ST: 4097
    EXPECT_EQ(status.apply(body, 1), ProgramStatus::Result::Changed); // BEL: 4096
}

TEST(ProgramStatus, TitleIsCheckedLikeMsg)
{
    Term t;
    t.feed("\x1b]7501;state=blocked:title=QXBwbHk\x1b\\");
    ASSERT_EQ(t.records().size(), 1u);
    EXPECT_EQ(t.records()[0].title, "Apply");
    const auto serial = t.records()[0].serial;
    for (const std::string& bad : {
             "state=done:title=" + detail::base64Encode(std::string(193, 'x')), // over 256 encoded bytes
             "state=done:title=" + detail::base64Encode("a\nb"),
             "state=done:msg=" + detail::base64Encode("ok\xe2\x80\xa8" "Approve? Press y"), // U+2028
         }) {
        t.feed("\x1b]7501;" + bad + "\x1b\\");
        EXPECT_EQ(t.records()[0].serial, serial) << bad;
    }
    t.feed("\x1b]7501;state=done:title=" + detail::base64Encode(std::string(192, 'x')) + "\x1b\\");
    EXPECT_EQ(t.records()[0].title.size(), 192u);
}

TEST(ProgramStatus, RepeatsThatChangeNothingAreSilent)
{
    Term t;
    t.feed("\x1b]7501;state=clear\x1b\\\x1b]7501;state=clear\x1b\\");
    EXPECT_EQ(t.host.changes, 0);
    t.feed("\x1b]7501;state=working:id=a\x1b\\\x1b]7501;state=clear:id=b\x1b\\");
    EXPECT_EQ(t.host.changes, 1);
    t.feed("\x1b]7501;state=clear:id=a\x1b\\");
    EXPECT_EQ(t.host.changes, 2);
    t.t.programExited(); // lets ConEmu progress through again
    t.feed("\x1b]9;4;1;30\x1b\\\x1b]9;4;1;30\x1b\\");
    EXPECT_EQ(t.host.changes, 3);
    t.feed("\x1b]9;4;1;31\x1b\\");
    EXPECT_EQ(t.host.changes, 4);
}

TEST(ProgramStatus, HierarchyClearAndAppInheritance)
{
    Term t;
    t.feed("\x1b]7501;state=idle:app=claude-code\x1b\\");
    t.feed("\x1b]7501;state=blocked:id=agent\x1b\\");
    t.feed("\x1b]7501;state=working:id=agent/tool\x1b\\");
    t.feed("\x1b]7501;state=working:id=agents\x1b\\"); // a sibling, not a child
    ASSERT_EQ(t.records().size(), 4u);
    const auto& recs = t.records();
    EXPECT_EQ(t.t.programStatus().effectiveApp(recs[2]), "claude-code"); // agent/tool via the root
    t.feed("\x1b]7501;state=clear:id=agent\x1b\\");
    ASSERT_EQ(t.records().size(), 2u);
    EXPECT_EQ(t.records()[0].id, "");
    EXPECT_EQ(t.records()[1].id, "agents");
    t.feed("\x1b]7501;state=clear\x1b\\");
    EXPECT_TRUE(t.records().empty());
}

TEST(ProgramStatus, QueryIsAnswered)
{
    Term t;
    t.feed("\x1b]7501;?\x1b\\\x1b]7501;?\x07");
    EXPECT_EQ(t.host.written, "\x1b]7501;?\x1b\\\x1b]7501;?\x07");
    EXPECT_TRUE(t.records().empty());
}

TEST(ProgramStatus, PromptAndExitEndActivityButKeepResults)
{
    Term t;
    t.feed("\x1b]7501;state=working:id=a\x1b\\\x1b]7501;state=done:id=b\x1b\\");
    t.feed("\x1b]7501;state=blocked:id=c\x1b\\\x1b]7501;state=error:id=d\x1b\\");
    t.feed("\x1b]133;A\x1b\\");
    ASSERT_EQ(t.records().size(), 2u);
    EXPECT_EQ(t.records()[0].state, StatusState::Done);
    EXPECT_EQ(t.records()[1].state, StatusState::Error);
    t.feed("\x1b]7501;state=working\x1b\\");
    t.t.programExited();
    EXPECT_EQ(t.records().size(), 2u);
    // Screens do not matter; RIS clears everything.
    t.feed("\x1b[?1049h\x1b[?1049l");
    EXPECT_EQ(t.records().size(), 2u);
    t.feed("\x1b" "c");
    EXPECT_TRUE(t.records().empty());
}

TEST(ProgramStatus, LegacyProgressUntilTheRealProtocol)
{
    Term t;
    t.feed("\x1b]9;4;1;30\x1b\\");
    ASSERT_EQ(t.records().size(), 1u);
    EXPECT_EQ(t.records()[0].state, StatusState::Working);
    EXPECT_EQ(t.records()[0].progress, 30);
    t.feed("\x1b]9;4;2\x1b\\");
    EXPECT_EQ(t.records()[0].state, StatusState::Error);
    t.feed("\x1b]9;4;0\x1b\\");
    EXPECT_TRUE(t.records().empty());
    EXPECT_TRUE(t.host.notified.empty()); // never a notification
    // Once a program speaks OSC 7501, 9;4 no longer overwrites its records.
    t.feed("\x1b]7501;state=blocked:kind=question\x1b\\\x1b]9;4;1;50\x1b\\");
    EXPECT_EQ(t.records()[0].state, StatusState::Blocked);
    // That lasts as long as the program: the next one may use 9;4 again.
    t.feed("\x1b]7501;state=done\x1b\\\x1b]133;A\x1b\\");
    const int changes = t.host.changes;
    t.feed("\x1b]9;4;1;50\x1b\\");
    ASSERT_EQ(t.records().size(), 1u);
    EXPECT_EQ(t.records()[0].state, StatusState::Working);
    EXPECT_EQ(t.records()[0].progress, 50);
    EXPECT_EQ(t.host.changes, changes + 1);
}

TEST(ProgramStatus, ResultsKeepTheAppTheyInherited)
{
    Term t;
    t.feed("\x1b]7501;state=working:app=cargo\x1b\\\x1b]7501;state=done:id=build\x1b\\");
    t.feed("\x1b]133;A\x1b\\");
    ASSERT_EQ(t.records().size(), 1u);
    EXPECT_EQ(t.records()[0].id, "build");
    EXPECT_EQ(t.t.programStatus().effectiveApp(t.records()[0]), "cargo");
}

TEST(ProgramStatus, RecordCapEvictsTheLeastRecentlyUpdated)
{
    Term t;
    for (int i = 0; i < 300; ++i) {
        t.feed("\x1b]7501;state=working:id=r" + std::to_string(i) + "\x1b\\");
    }
    EXPECT_EQ(t.records().size(), ProgramStatus::MaxRecords);
    bool hasFirst = false;
    bool hasLast = false;
    for (const auto& r : t.records()) {
        hasFirst |= r.id == "r0";
        hasLast |= r.id == "r299";
    }
    EXPECT_FALSE(hasFirst);
    EXPECT_TRUE(hasLast);
    EXPECT_TRUE(std::is_sorted(t.records().begin(), t.records().end(),
                               [](const auto& a, const auto& b) { return a.id < b.id; }));
}

TEST(ProgramStatus, InvalidNativeReportDoesNotDisableLegacy)
{
    ProgramStatus status;
    EXPECT_EQ(status.apply("state=bogus"), ProgramStatus::Result::Ignored);
    EXPECT_TRUE(status.applyLegacyProgress(1, 40));
    EXPECT_EQ(status.apply("state=working:msg=invalid!"), ProgramStatus::Result::Ignored);
    EXPECT_TRUE(status.applyLegacyProgress(1, 50));
    for (int i = 0; i < 300; ++i) status.apply("state=working:id=" + std::to_string(i));
    EXPECT_EQ(status.records().size(), ProgramStatus::MaxRecords);
}

TEST(ProgramStatus, RecordCapEvictsByUpdateNotByCreation)
{
    ProgramStatus status;
    for (int i = 0; i < 256; ++i) {
        status.apply("state=working:id=r" + std::to_string(i));
    }
    status.apply("state=done:id=r0"); // r0 is now the most recent
    status.apply("state=working:id=r256");
    auto has = [&](std::string_view id) {
        return std::any_of(status.records().begin(), status.records().end(), [&](const auto& r) { return r.id == id; });
    };
    EXPECT_TRUE(has("r0"));
    EXPECT_FALSE(has("r1"));
    EXPECT_TRUE(has("r256"));
}
