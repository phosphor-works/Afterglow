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

TEST(FileDrop, BackslashCannotUnbalanceFishQuotes)
{
    QMimeData mime;
    mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/x\\")), QUrl::fromLocalFile(QStringLiteral("/tmp/(echo INJECTED)"))});
    // Every backslash and quote is escaped outside the quotes, so bash, zsh and fish agree.
    EXPECT_EQ(droppedFiles(&mime), QStringLiteral("'/tmp/x'\\\\'' '/tmp/(echo INJECTED)'"));
}
TEST(FileDrop, RemoteHostsAndFormatCharactersAreRejected)
{
    QMimeData mime;
    mime.setUrls({QUrl(QStringLiteral("file://server/share/a"))});
    EXPECT_TRUE(droppedFiles(&mime).isEmpty());
    EXPECT_TRUE(localFilePath(QUrl(QStringLiteral("file://server/share/a"))).isEmpty());
    EXPECT_EQ(localFilePath(QUrl(QStringLiteral("file://localhost/tmp/a"))), QStringLiteral("/tmp/a"));
    mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/a") + QChar(0x202e) + QStringLiteral("txt.sh"))});
    EXPECT_TRUE(droppedFiles(&mime).isEmpty());
    mime.setUrls({QUrl::fromLocalFile(QStringLiteral("/tmp/a") + QChar(0x9b) + QStringLiteral("b"))});
    EXPECT_TRUE(droppedFiles(&mime).isEmpty());
}

TEST(FileDrop, BoundsEncodedBytesIncludingSeparators)
{
    QMimeData mime;
    const QString prefix = QStringLiteral("/") + QString(349524, QChar(0x754c));
    mime.setUrls({QUrl::fromLocalFile(prefix)});
    ASSERT_FALSE(droppedFiles(&mime).isEmpty());
    mime.setUrls({QUrl::fromLocalFile(prefix + QChar(0x754c))});
    EXPECT_TRUE(droppedFiles(&mime).isEmpty());
    const QString half = QStringLiteral("/") + QString(524285, QLatin1Char('x'));
    mime.setUrls({QUrl::fromLocalFile(half), QUrl::fromLocalFile(half)});
    EXPECT_TRUE(droppedFiles(&mime).isEmpty());
}

TEST(DesktopText, ScrolledViewportReportsOnlyRemovedAndNewLines)
{
    const auto edits = textEdits(QStringLiteral("one\ntwo\n$ "), QStringLiteral("two\n$ ls\nfile"));
    ASSERT_EQ(edits.size(), 2);
    EXPECT_EQ(edits[0].position, 0);
    EXPECT_EQ(edits[0].removed, QStringLiteral("one\n"));
    EXPECT_TRUE(edits[0].inserted.isEmpty());
    EXPECT_EQ(edits[1].position, 6);
    EXPECT_TRUE(edits[1].removed.isEmpty());
    EXPECT_EQ(edits[1].inserted, QStringLiteral("ls\nfile"));
}
TEST(DesktopText, EditsTrimSharedPrefixAndSuffix)
{
    const auto edits = textEdits(QStringLiteral("abc\nxyz"), QStringLiteral("abc\nx1yz"));
    ASSERT_EQ(edits.size(), 1);
    EXPECT_EQ(edits[0].position, 5);
    EXPECT_TRUE(edits[0].removed.isEmpty());
    EXPECT_EQ(edits[0].inserted, QStringLiteral("1"));
    EXPECT_TRUE(textEdits(QStringLiteral("same"), QStringLiteral("same")).isEmpty());
}
TEST(DesktopText, EditsNeverSplitSurrogatePairs)
{
    const auto edits = textEdits(QString::fromUtf8("a\xf0\x9f\x98\x80"), QString::fromUtf8("a\xf0\x9f\x98\x81"));
    ASSERT_EQ(edits.size(), 1);
    EXPECT_EQ(edits[0].position, 1);
    EXPECT_EQ(edits[0].removed, QString::fromUtf8("\xf0\x9f\x98\x80"));
    EXPECT_EQ(edits[0].inserted, QString::fromUtf8("\xf0\x9f\x98\x81"));
}
