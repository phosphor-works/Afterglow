// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: LGPL-2.1-or-later

// The shell's state from its marks (OSC 133 and 633), the text since a mark,
// and the counter of changes to the text (contentGeneration()).

#include "TestUtil.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace AfterglowVt;
using namespace AfterglowVt::Test;

namespace {

struct Term
{
    RecordingHost host;
    Terminal t;
    Term(int cols, int rows, std::size_t scrollback = 100)
        : t(cols, rows, host, [scrollback] {
            TerminalOptions o;
            o.maxScrollback = scrollback;
            return o;
        }())
    {
    }
    void feed(std::string_view s)
    {
        t.feed(s);
    }
};

std::string osc133(std::string_view args)
{
    return "\x1b]133;" + std::string(args) + "\x07";
}

} // namespace

// Output that changes what the text reads bumps the counter; output that
// leaves it does not. Whole, and a byte at a time.
TEST(ShellMarks, ContentGenerationCountsTheText)
{
    struct Case
    {
        const char* name;
        std::string bytes;
        bool bumps;
    };
    const std::vector<Case> cases{
        {"a character", "x", true},
        {"a cursor move", "\x1b[H", true},
        {"a carriage return", "\r", true},
        {"an erase of the screen", "\x1b[2J", true},
        {"an erase of the line", "\x1b[1K", true},
        {"the alternate screen", "\x1b[?1049h", true},
        {"a double-width line", "\x1b#6", true},
        {"DECCARA", "\x1b[1;1;1;3;1$r", true},
        {"a line feed", "\n", true},
        {"a title", "\x1b]0;title\x07", false},
        {"a bell", "\x07", false},
        {"a working directory", "\x1b]7;file:///tmp\x07", false},
        {"a cursor report", "\x1b[6n", false},
        {"a rendition", "\x1b[31;1m", false},
        {"a mode", "\x1b[?2004h", false},
        {"a hidden cursor", "\x1b[?25l", false},
        {"a cursor shape", "\x1b[5 q", false},
        {"a colour", "\x1b]4;1;#ff0000\x07\x1b]11;#101010\x07", false},
        {"a prompt mark", osc133("A"), false},
        {"an input mark", osc133("A") + osc133("B"), false},
        {"a command", osc133("C") + osc133("D;0"), false},
        {"VS Code's marks", "\x1b]633;A\x07\x1b]633;B\x07\x1b]633;E;ls\x07\x1b]633;P;Cwd=/tmp\x07", false},
        {"extra cursors", "\x1b[>1;2:2:3 q", false},
    };
    for (const bool bytewise : {false, true}) {
        for (const Case& c : cases) {
            Term t(20, 5);
            t.feed("hello");
            const std::uint64_t before = t.t.contentGeneration();
            if (bytewise) {
                for (const char b : c.bytes) {
                    t.feed(std::string_view(&b, 1));
                    (void)t.t.contentGeneration();
                }
            } else {
                t.feed(c.bytes);
            }
            EXPECT_EQ(t.t.contentGeneration() != before, c.bumps) << c.name << (bytewise ? ", a byte at a time" : "");
        }
    }
}

// The host's calls: a new size and a moved view change the text, a new size
// of the cells and colours do not.
TEST(ShellMarks, ContentGenerationOfTheHostsCalls)
{
    Term t(20, 4, 100);
    for (int i = 0; i < 10; ++i)
        t.feed("line " + std::to_string(i) + "\r\n");
    t.t.resize(20, 4, 8, 16);
    std::uint64_t g = t.t.contentGeneration();
    t.t.resize(20, 4, 9, 18);
    EXPECT_EQ(t.t.contentGeneration(), g);
    EXPECT_TRUE(t.t.applyColorOsc("4;2;#00ff00"));
    EXPECT_EQ(t.t.contentGeneration(), g);
    t.t.setViewportOffset(3);
    EXPECT_NE(t.t.contentGeneration(), g);
    g = t.t.contentGeneration();
    t.t.resetViewport();
    EXPECT_NE(t.t.contentGeneration(), g);
    g = t.t.contentGeneration();
    // A view at the bottom that scrolls down stays: nothing changed, for the
    // renderer either.
    const std::uint64_t generation = t.t.generation();
    t.t.scrollViewport(-5);
    t.t.setViewportOffset(0);
    EXPECT_EQ(t.t.generation(), generation);
    EXPECT_EQ(t.t.contentGeneration(), g);
    t.t.resize(30, 4, 9, 18);
    EXPECT_NE(t.t.contentGeneration(), g);
}

// A prompt that the shell draws again (a resize, Ctrl+L) is the same prompt:
// its region and its id stay. One after a command, or after an empty line,
// is new.
TEST(ShellMarks, APromptDrawnAgainKeepsItsRegionAndId)
{
    Term t(30, 6);
    t.feed(osc133("A") + "$ " + osc133("B"));
    const std::uint64_t id = t.t.lastPromptId();
    const std::vector<SemanticRegion> first = t.t.semanticRegions();
    ASSERT_EQ(first.size(), 2u);
    // The same prompt again, after the shell went back to its start.
    t.feed("\r\x1b[K" + osc133("A") + "$ " + osc133("B") + "ls");
    EXPECT_EQ(t.t.lastPromptId(), id);
    std::vector<SemanticRegion> again = t.t.semanticRegions();
    ASSERT_EQ(again.size(), 2u);
    EXPECT_EQ(again[0].id, first[0].id);
    EXPECT_EQ(t.t.shellState(), ShellState::Prompt);
    EXPECT_TRUE(t.t.shellInput());
    // Ctrl+L: at the top of the screen now.
    t.feed("\r\n\r\n\x1b[H\x1b[2J" + osc133("A") + "$ " + osc133("B"));
    EXPECT_EQ(t.t.lastPromptId(), id);
    again = t.t.semanticRegions();
    ASSERT_EQ(again.size(), 2u);
    EXPECT_EQ(again[0].id, first[0].id);
    EXPECT_EQ(again[0].start.row, t.t.primaryScreen().rowsDropped() + t.t.primaryScreen().historySize());
    // An empty line: a D with no command, and a new prompt.
    t.feed("\r\n" + osc133("D;0"));
    EXPECT_EQ(t.t.shellState(), ShellState::Prompt);
    t.feed(osc133("A") + "$ " + osc133("B"));
    EXPECT_GT(t.t.lastPromptId(), id);
    // A command, and the prompt after it.
    const std::uint64_t empty = t.t.lastPromptId();
    t.feed("true\r\n" + osc133("C") + osc133("A") + "$ ");
    EXPECT_GT(t.t.lastPromptId(), empty);
    EXPECT_FALSE(t.t.lastExitCode());
}

// A secondary prompt, as OSC 133 A;k=s, P;k=s and VS Code's 633 F mark it,
// is no new prompt. Another part of the prompt (P;k=r) leaves the cursor in
// the input. Nothing of it counts while a command runs.
TEST(ShellMarks, PartsOfAPromptAreNoNewPrompt)
{
    for (const std::string& secondary : {osc133("A;k=s"), osc133("P;k=s"), std::string("\x1b]633;F\x07")}) {
        Term t(30, 6);
        t.feed(osc133("A") + "$ " + osc133("B") + "echo \"\r\n");
        const std::uint64_t id = t.t.lastPromptId();
        const std::size_t regions = t.t.semanticRegions().size();
        t.feed(secondary + "> ");
        EXPECT_EQ(t.t.shellState(), ShellState::Continuation) << secondary.substr(1);
        EXPECT_EQ(t.t.lastPromptId(), id);
        EXPECT_EQ(t.t.semanticRegions().size(), regions);
        t.feed("\x1b]633;G\x07");
        EXPECT_EQ(t.t.shellState(), ShellState::Continuation);
    }
    Term right(30, 6);
    right.feed(osc133("A") + "$ " + osc133("B") + "make");
    const std::size_t regions = right.t.semanticRegions().size();
    right.feed("\x1b[s\x1b[25G" + osc133("P;k=r") + "10:00\x1b[u");
    EXPECT_EQ(right.t.shellState(), ShellState::Prompt);
    EXPECT_TRUE(right.t.shellInput());
    EXPECT_EQ(right.t.semanticRegions().size(), regions);
    // While a command runs, its output can hold marks: no prompt.
    right.feed("\r\n" + osc133("C") + osc133("P;k=s") + osc133("P"));
    EXPECT_EQ(right.t.shellState(), ShellState::Running);
}

// N ends the last command by itself: each N is a new prompt, also one with
// no C or D before it. An A after it is the same prompt drawn again.
TEST(ShellMarks, EachNIsANewPrompt)
{
    Term t(30, 6);
    t.feed(osc133("N") + "$ " + osc133("B"));
    const std::uint64_t first = t.t.lastPromptId();
    EXPECT_EQ(first, 1u);
    t.feed("\r\n" + osc133("N") + "$ " + osc133("B"));
    EXPECT_EQ(t.t.lastPromptId(), first + 1);
    EXPECT_EQ(t.t.shellState(), ShellState::Prompt);
    t.feed("\r" + osc133("A") + "$ " + osc133("B"));
    EXPECT_EQ(t.t.lastPromptId(), first + 1);
}

// A shell that sends no A starts its prompts with P: the first, and each
// one after the output of a command, is new. The marks since the last
// prompt start again there, so an A that draws it again keeps its id.
TEST(ShellMarks, APMarkAfterACommandIsANewPrompt)
{
    Term t(30, 6);
    t.feed(osc133("P;k=i") + "$ " + osc133("B"));
    const std::uint64_t first = t.t.lastPromptId();
    EXPECT_EQ(first, 1u);
    EXPECT_EQ(t.t.shellState(), ShellState::Prompt);
    t.feed("true\r\n" + osc133("C") + osc133("D;0") + osc133("P;k=i") + "$ " + osc133("B"));
    EXPECT_EQ(t.t.lastPromptId(), first + 1);
    EXPECT_EQ(t.t.shellState(), ShellState::Prompt);
    EXPECT_EQ(t.t.lastExitCode(), 0);
    t.feed("\r" + osc133("A") + "$ " + osc133("B"));
    EXPECT_EQ(t.t.lastPromptId(), first + 1);
    // A command that the next P ends without a D: no new prompt while it
    // runs, as its output can hold marks.
    t.feed("sleep 1\r\n" + osc133("C") + osc133("P;k=i"));
    EXPECT_EQ(t.t.lastPromptId(), first + 1);
    EXPECT_EQ(t.t.shellState(), ShellState::Running);
}

// A P that starts a prompt marks its row as A does: jumps to prompts, clicks
// that move the cursor and a resize that keeps the prompt find it. Only on
// the primary screen.
TEST(ShellMarks, APMarkThatStartsAPromptMarksItsRow)
{
    Term t(30, 6);
    t.feed(osc133("P;k=i") + "$ " + osc133("B"));
    EXPECT_TRUE(t.t.screen().row(0).promptStart);
    t.feed("true\r\n" + osc133("C") + "out\r\n" + osc133("D;0") + osc133("P;k=i") + "$ " + osc133("B"));
    EXPECT_FALSE(t.t.screen().row(1).promptStart);
    EXPECT_TRUE(t.t.screen().row(2).promptStart);
    // A part of the prompt that it already started marks no other row.
    t.feed("\r\n" + osc133("P;k=r"));
    EXPECT_FALSE(t.t.screen().row(3).promptStart);
    t.feed("\x1b[?1049h\r\n" + osc133("C") + osc133("D;0") + osc133("P;k=i"));
    EXPECT_EQ(t.t.shellState(), ShellState::Prompt);
    for (int y = 0; y < 6; ++y)
        EXPECT_FALSE(t.t.screen().row(y).promptStart) << y;
}

// D with no command (Enter on an empty line) leaves the shell at its prompt.
// A D gives its code to the command's output, also when a program's marks
// opened another region in it.
TEST(ShellMarks, DEndsOnlyACommand)
{
    Term t(30, 6);
    t.feed(osc133("A") + "$ " + osc133("B") + "\r\n" + osc133("D;0"));
    EXPECT_EQ(t.t.shellState(), ShellState::Prompt);
    EXPECT_FALSE(t.t.lastExitCode());
    t.feed(osc133("A") + "$ " + osc133("B") + "cat marks\r\n" + osc133("C") + "out\r\n" + osc133("B") + "more\r\n"
           + osc133("D;5"));
    EXPECT_EQ(t.t.lastExitCode(), 5);
    EXPECT_EQ(t.t.shellState(), ShellState::Running);
    std::optional<int> code;
    for (const SemanticRegion& region : t.t.semanticRegions()) {
        if (region.kind == SemanticZoneKind::Output) {
            EXPECT_FALSE(region.open);
            code = region.exitCode;
        }
    }
    EXPECT_EQ(code, 5);
}

// A full-screen program that was killed leaves the shell on the alternate
// screen: its commands still start and end, and its prompts are new.
TEST(ShellMarks, CommandsRunOnTheAlternateScreen)
{
    Term t(30, 6);
    t.feed(osc133("A") + "$ " + osc133("B") + "vim\r\n" + osc133("C") + "\x1b[?1049h");
    const std::uint64_t id = t.t.lastPromptId();
    t.feed(osc133("D;137") + osc133("A") + "$ " + osc133("B"));
    EXPECT_EQ(t.t.shellState(), ShellState::Prompt);
    EXPECT_GT(t.t.lastPromptId(), id);
    EXPECT_EQ(t.t.lastExitCode(), 137);
    t.feed("ls\r\n" + osc133("C;cmdline_url=ls"));
    EXPECT_EQ(t.t.shellState(), ShellState::Running);
    ASSERT_EQ(t.host.commandsStarted.size(), 2u);
    EXPECT_EQ(t.host.commandsStarted[1], "ls");
    // Its output is not on the primary screen, which has no rows of it.
    EXPECT_FALSE(t.t.hasCommandOutput());
    t.feed(osc133("D;0") + osc133("A") + "$ ");
    EXPECT_EQ(t.host.commandsFinished, (std::vector<int>{137, 0}));
    EXPECT_EQ(t.t.lastExitCode(), 0);
    EXPECT_EQ(t.t.shellState(), ShellState::Prompt);
    // Its secondary prompt is one too, with no mark of a row there.
    t.feed(osc133("B") + "echo \"\r\n" + osc133("A;k=s") + "> ");
    EXPECT_EQ(t.t.shellState(), ShellState::Continuation);
    t.feed("\"\r\n" + osc133("C"));
    EXPECT_EQ(t.t.shellState(), ShellState::Running);
    t.feed(osc133("D;0") + osc133("A") + "$ ");
    EXPECT_EQ(t.t.shellState(), ShellState::Prompt);
}

// The newest regions stay, oldest first, at most MaxSemanticRegions.
TEST(ShellMarks, TheNewestRegionsStay)
{
    Term t(30, 6, 0);
    const std::string cycle = osc133("A") + "$ " + osc133("B") + "x\r\n" + osc133("C") + osc133("D;1");
    for (std::size_t i = 0; i < Terminal::MaxSemanticRegions; ++i)
        t.feed(cycle);
    const std::vector<SemanticRegion> regions = t.t.semanticRegions();
    ASSERT_EQ(regions.size(), Terminal::MaxSemanticRegions);
    EXPECT_EQ(regions.back().id, 3 * Terminal::MaxSemanticRegions);
    EXPECT_EQ(regions.front().id, 2 * Terminal::MaxSemanticRegions + 1);
    EXPECT_EQ(t.t.lastExitCode(), 1);
}

// A mark is of an epoch of the row ids: a new width, an unscroll and a clear
// make the rows of its ids other ones. A new height does not, nor kitty's
// clear that keeps the screen in the scrollback, nor an erase of a part of
// the screen or of the alternate screen.
TEST(ShellMarks, AMarkGoesStale)
{
    Term t(20, 4, 100);
    t.feed("x\r\n");
    TextMark mark = t.t.textMark();
    t.feed("y");
    bool stale = true;
    t.t.resize(20, 6, 8, 16);
    EXPECT_EQ(t.t.textSince(mark, 100, nullptr, nullptr, &stale, {}), "y");
    EXPECT_FALSE(stale);
    t.feed("\x1b[22J");
    EXPECT_EQ(t.t.textSince(mark, 100, nullptr, nullptr, &stale, {}), "y");
    EXPECT_FALSE(stale);
    t.t.resize(30, 6, 8, 16);
    EXPECT_EQ(t.t.textSince(mark, 100, nullptr, nullptr, &stale, {}), "");
    EXPECT_TRUE(stale);
    mark = t.t.textMark();
    EXPECT_EQ(mark.epoch, t.t.textEpoch());
    for (int i = 0; i < 10; ++i)
        t.feed("\r\nline");
    const TextMark low = t.t.textMark();
    t.feed("\x1b[2+T");
    EXPECT_EQ(t.t.textSince(mark, 100, nullptr, nullptr, &stale, {}), "");
    EXPECT_TRUE(stale);
    EXPECT_NE(low.epoch, t.t.textEpoch());
    // A reset erases every row.
    const TextMark before = t.t.textMark();
    t.feed(
        "\x1b"
        "c");
    EXPECT_NE(before.epoch, t.t.textEpoch());
    // An erase from the top left corner (clear as CSI H CSI J) erases every
    // row too, as ED 2.
    const TextMark cleared = t.t.textMark();
    t.feed("\x1b[2;1H\x1b[J\x1b[?1049h\x1b[H\x1b[J\x1b[?1049l\x1b[H\x1b[?J");
    EXPECT_EQ(cleared.epoch, t.t.textEpoch());
    t.feed("\x1b[H\x1b[J");
    EXPECT_NE(cleared.epoch, t.t.textEpoch());
}

// When the cursor went above the mark's row, the text is the live screen,
// not the history that the view shows. A carriage return on the mark's row
// gives what the row holds after the mark.
TEST(ShellMarks, TextSinceAMarkThatTheCursorLeft)
{
    Term t(20, 4, 100);
    for (int i = 0; i < 10; ++i)
        t.feed("line " + std::to_string(i) + "\r\n");
    const TextMark mark = t.t.textMark();
    t.feed("\x1b[1;1Habove");
    t.t.scrollViewport(5);
    const std::string since = t.t.textSince(mark, 1024, nullptr, nullptr, nullptr, {});
    t.t.resetViewport();
    EXPECT_EQ(since, t.t.text(Terminal::TextScope::Screen));
    EXPECT_TRUE(since.starts_with("above")) << since;

    Term row(20, 4);
    row.feed("$ ");
    const TextMark prompt = row.t.textMark();
    row.feed("abc\rX");
    EXPECT_EQ(row.t.textSince(prompt, 1024, nullptr, nullptr, nullptr, {}), "abc");
}

// The spaces that a program wrote before the cursor on its row are text: a
// prompt such as ">>> " or "(gdb) " keeps its space, also with keepEnd and
// in ANSI. The blanks after the cursor, the cells that no program wrote, and
// the blanks at the end of the rows above it go.
TEST(ShellMarks, TextSinceKeepsTheBlanksBeforeTheCursor)
{
    TextFormat end;
    end.keepEnd = true;
    TextFormat ansi;
    ansi.ansi = true;
    for (const TextFormat& format : {TextFormat{}, end, ansi}) {
        Term t(20, 4);
        const TextMark mark = t.t.textMark();
        t.feed("42   \r\n>>> ");
        EXPECT_EQ(t.t.textSince(mark, 1024, nullptr, nullptr, nullptr, format), "42\n>>> ");
        t.feed("   \x1b[3D");
        EXPECT_EQ(t.t.textSince(mark, 1024, nullptr, nullptr, nullptr, format), "42\n>>> ");
    }

    // A row of blanks before the cursor alone: no row of text.
    Term blank(20, 4);
    const TextMark top = blank.t.textMark();
    blank.feed("x\r\n  ");
    EXPECT_EQ(blank.t.textSince(top, 1024, nullptr, nullptr, nullptr, {}), "x\n  ");
    blank.feed("\r");
    EXPECT_EQ(blank.t.textSince(top, 1024, nullptr, nullptr, nullptr, {}), "x");
    // A cursor that moved over cells that no program wrote.
    Term moved(20, 4);
    const TextMark start = moved.t.textMark();
    moved.feed("x\r\n\r\nab\x1b[3C");
    EXPECT_EQ(moved.t.textSince(start, 1024, nullptr, nullptr, nullptr, {}), "x\n\nab");
    moved.feed("\r\n\x1b[2C");
    EXPECT_EQ(moved.t.textSince(start, 1024, nullptr, nullptr, nullptr, {}), "x\n\nab");
}

// The cuts of a long text: the rows before the mark are no part of the text,
// a last row longer than the limit keeps its start, blank rows at the end
// and a right prompt after the cursor take no room.
TEST(ShellMarks, KeepEndCutsOnlyTheText)
{
    TextFormat end;
    end.keepEnd = true;
    bool truncated = true;
    Term prompt(20, 4, 100);
    prompt.feed("$ ");
    const TextMark afterPrompt = prompt.t.textMark();
    prompt.feed("\r\nabc\r\ndef");
    EXPECT_EQ(prompt.t.textSince(afterPrompt, 7, &truncated, nullptr, nullptr, end), "abc\ndef");
    EXPECT_FALSE(truncated);

    Term wide(40, 4, 100);
    const TextMark top = wide.t.textMark();
    wide.feed("short\r\n" + std::string(30, 'x'));
    EXPECT_EQ(wide.t.textSince(top, 10, &truncated, nullptr, nullptr, end), std::string(10, 'x'));
    EXPECT_TRUE(truncated);

    Term blank(20, 4, 100);
    const TextMark result = blank.t.textMark();
    blank.feed("result");
    for (int i = 0; i < 10; ++i)
        blank.feed("\r\n");
    EXPECT_EQ(blank.t.textSince(result, 10, &truncated, nullptr, nullptr, end), "result");
    EXPECT_FALSE(truncated);

    Term rprompt(30, 4, 100);
    const TextMark lines = rprompt.t.textMark();
    rprompt.feed("line1\r\nline2\r\n\x1b[s\x1b[25GRPROM\x1b[u");
    for (const TextFormat& format : {TextFormat{}, end}) {
        EXPECT_EQ(rprompt.t.textSince(lines, 11, &truncated, nullptr, nullptr, format), "line1\nline2");
        EXPECT_FALSE(truncated);
    }
}

// What the screen shows of a row is its text: the left half of a
// double-width line, a block of text sizing at the first row of a text, and
// no mark of a cluster without its character.
TEST(ShellMarks, TheTextIsWhatTheScreenShows)
{
    Term dwl(20, 3);
    dwl.feed("0123456789ABCDEFGHIJ\x1b#6");
    EXPECT_EQ(dwl.t.text(Terminal::TextScope::Screen), "0123456789");

    Term block(20, 6);
    block.feed("\x1b]66;s=2:w=2;AB\x07\x1b[2;1H");
    const TextMark lower = block.t.textMark();
    block.feed("\x1b[3;1Hz");
    EXPECT_EQ(block.t.textSince(lower, 100, nullptr, nullptr, nullptr, {}), "AB\nz");

    // e and a combining acute accent: three bytes that go whole or not.
    Term accent(20, 3);
    accent.feed(
        "ab"
        "e\xcc\x81");
    bool truncated = false;
    EXPECT_EQ(accent.t.text(Terminal::TextScope::Screen, 4, &truncated), "ab");
    EXPECT_TRUE(truncated);
    EXPECT_EQ(accent.t.text(Terminal::TextScope::Screen, 5, &truncated),
              "ab"
              "e\xcc\x81");
}

// A mark at a pending wrap is before the next row: no lone wrap marker. The
// last output whose start left the scrollback is cut.
TEST(ShellMarks, TheEdgesOfATextSinceAMark)
{
    Term wrap(5, 3);
    wrap.feed("abcde");
    const TextMark pending = wrap.t.textMark();
    wrap.feed("fg");
    TextFormat markers;
    markers.wrapMarkers = true;
    EXPECT_EQ(wrap.t.textSince(pending, 100, nullptr, nullptr, nullptr, markers), "fg");

    Term out(20, 4, 2);
    out.feed(osc133("A") + "$ " + osc133("B") + "seq\r\n" + osc133("C"));
    for (int i = 0; i < 20; ++i)
        out.feed(std::to_string(i) + "\r\n");
    out.feed(osc133("D;0"));
    bool truncated = false;
    const std::string last = out.t.text(Terminal::TextScope::LastOutput, 4096, &truncated);
    EXPECT_TRUE(truncated);
    EXPECT_TRUE(last.ends_with("19\n")) << last;
}

// At a secondary prompt a resize keeps only its rows for the shell to draw
// again: the lines that the shell accepted before it reflow. The shell's
// secondary prompt ends the wait.
TEST(ShellMarks, AResizeAtASecondaryPromptReflowsTheLinesBefore)
{
    Term t(30, 6);
    t.feed(osc133("A;redraw=1") + "$ " + osc133("B") + "echo \"abcdefghijklmnopqrstu\r\n" + osc133("A;k=s") + "> ");
    t.t.resize(10, 6, 8, 16);
    EXPECT_TRUE(t.t.promptRedrawPending());
    std::string all = t.t.text(Terminal::TextScope::All);
    std::erase(all, '\n');
    EXPECT_NE(all.find("abcdefghijklmnopqrstu"), std::string::npos) << all;
    t.feed("\r\x1b[K" + osc133("A;k=s") + "> ");
    EXPECT_FALSE(t.t.promptRedrawPending());
    EXPECT_EQ(t.t.shellState(), ShellState::Continuation);
}
