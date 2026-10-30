// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "desktop/TerminalText.h"
#include "desktop/FileDrop.h"
#include <AfterglowVt/Terminal.h>
#include <gtest/gtest.h>
using namespace Afterglow;
namespace { struct Host : AfterglowVt::ITerminalHost { void writeToPty(std::string_view) override {} }; }
TEST(DesktopText, UnicodeCursorAndSelectionUseUtf16Offsets)
{
    Host h; AfterglowVt::Terminal t(20, 3, h);
    t.feed("A\xf0\x9f\x98\x80" "e\xcc\x81\xe7\x95\x8c");
    const auto s = TerminalText::capture(t.screen(), nullptr, true);
    EXPECT_EQ(s.text, QString::fromUtf8("A😀é界"));
    EXPECT_EQ(s.cursor, 6);
    ASSERT_EQ(s.cells.size(), 6);
    EXPECT_EQ(s.cells[1], s.cells[2]);
    EXPECT_EQ(s.cells[3], s.cells[4]);
    EXPECT_EQ(s.cells[5].col, 4);
    AfterglowVt::Selection sel;
    sel.active = true; sel.layoutEpoch = t.screen().layoutEpoch();
    sel.anchor = {t.screen().rowsDropped(), 1}; sel.head = {t.screen().rowsDropped(), 3};
    const auto selected = TerminalText::capture(t.screen(), &sel);
    EXPECT_EQ(selected.selectionStart, 1); EXPECT_EQ(selected.selectionEnd, 5);
}
TEST(DesktopText, CursorRowIsLiveWhileViewportIsScrolled)
{
    Host h; AfterglowVt::Terminal t(8, 2, h);
    t.feed("old\r\nprevious\r\nnew");
    t.screen().scrollViewport(1);
    EXPECT_TRUE(TerminalText::capture(t.screen()).text.startsWith(QStringLiteral("old")));
    EXPECT_EQ(TerminalText::capture(t.screen(), nullptr, true).text, QStringLiteral("new"));
    t.feed("\x1b[?1049h\x1b[Hsecret");
    EXPECT_TRUE(TerminalText::capture(t.screen()).text.startsWith(QStringLiteral("secret")));
}
TEST(DesktopText, SoftWrapDoesNotInventNewlineAndCursorCanBeInsideText)
{
    Host h; AfterglowVt::Terminal t(4, 3, h);
    t.feed("abcdef\x1b[1D");
    const auto s = TerminalText::capture(t.screen());
    EXPECT_TRUE(s.text.startsWith(QStringLiteral("abcdef")));
    EXPECT_EQ(s.cursor, 5);
}
TEST(FileDrop, QuotesPathsWithoutExecutingCommands)
{
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/a b'$(touch nope)")), QUrl::fromLocalFile(QStringLiteral("/tmp/界"))});
    EXPECT_EQ(droppedFiles(&mime), QStringLiteral("'/tmp/a b'\\''$(touch nope)' '/tmp/界'"));
    mime.setUrls({QUrl(QStringLiteral("https://example.com/a"))});
    EXPECT_TRUE(droppedFiles(&mime).isEmpty());
    mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/a\nb"))});
    EXPECT_TRUE(droppedFiles(&mime).isEmpty());
    mime.setUrls({QUrl::fromLocalFile(QStringLiteral("relative"))});
    EXPECT_TRUE(droppedFiles(&mime).isEmpty());
}
