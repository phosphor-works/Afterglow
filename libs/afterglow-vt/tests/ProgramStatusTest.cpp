// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "TestUtil.h"

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
    std::string huge = "\x1b]7501;state=done:msg=" + std::string(4100, 'A') + "\x1b\\";
    t.feed(huge);
    EXPECT_EQ(t.records()[0].state, StatusState::Working);
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
