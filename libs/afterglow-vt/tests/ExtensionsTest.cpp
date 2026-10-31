// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// OSC extensions from other terminals and the shell world: OSC 133 extras,
// VS Code's 633, iTerm2's 1337, systemd's 3008, OSC 52 reads, ConEmu's 9.

#include "TestUtil.h"
#include "../src/ITerm2.h"

#include <AfterglowVt/Terminal.h>

#include <gtest/gtest.h>

using namespace AfterglowVt;
using namespace AfterglowVt::Test;

namespace {

struct Host : RecordingHost
{
    std::vector<std::string> started;
    std::vector<int> finished;
    std::string cwd;
    int contextChanges = 0;
    std::vector<std::pair<std::string, std::string>> userVars;
    std::vector<ClipboardRequest> clipboardReads;
    bool answerClipboard = false;
    std::string clipboardWritten;

    void commandStarted(std::string_view c) override { started.emplace_back(c); }
    void commandFinished(int code) override { finished.push_back(code); }
    void workingDirectoryChanged(std::string_view d) override { cwd.assign(d); }
    void contextChanged() override { ++contextChanges; }
    void userVarChanged(std::string_view n, std::string_view v) override { userVars.emplace_back(n, v); }
    bool clipboardRequest(const ClipboardRequest& r) override
    {
        clipboardReads.push_back(r);
        return answerClipboard;
    }
    void clipboardWrite(std::string_view, std::string_view data) override { clipboardWritten.assign(data); }
    // Any "image": a 4x2 opaque red picture.
    bool decodeImage(std::string_view data, std::uint32_t& w, std::uint32_t& h, std::vector<std::uint8_t>& rgba) override
    {
        if (data != "picture") {
            return false;
        }
        w = 4;
        h = 2;
        rgba.clear();
        for (int i = 0; i < 8; ++i) {
            rgba.insert(rgba.end(), {255, 0, 0, 255});
        }
        return true;
    }
};

} // namespace

TEST(Extensions, ShellReportedCommandLine)
{
    Host host;
    Terminal t(40, 6, host);
    // fish: the command line as cmdline_url, which beats the screen text.
    t.feed("\x1b]133;A;click_events=1\x1b\\$ \x1b]133;B\x1b\\echo  hi\r\n\x1b]133;C;cmdline_url=echo%20%22hi%22\x1b\\");
    t.feed("\x1b]133;D;0\x1b\\");
    ASSERT_EQ(host.started.size(), 1u);
    EXPECT_EQ(host.started[0], "echo \"hi\"");
    // The next command without one falls back to the screen, not the old line.
    t.feed("\x1b]133;A\x07$ \x1b]133;B\x07ls\r\n\x1b]133;C\x07");
    ASSERT_EQ(host.started.size(), 2u);
    EXPECT_EQ(host.started[1], "ls");
}

TEST(Extensions, PromptClickEvents)
{
    Host host;
    Terminal t(40, 6, host);
    t.feed("\x1b]133;A;click_events=1\x07$ \x1b]133;B\x07");
    EXPECT_TRUE(t.promptTakesClicks());
    t.feed("x\r\n\x1b]133;C\x07");
    EXPECT_FALSE(t.promptTakesClicks()); // running, not at the prompt
    t.feed("\x1b]133;D;0\x07\x1b]133;A\x07$ \x1b]133;B\x07");
    EXPECT_FALSE(t.promptTakesClicks()); // this prompt did not ask
}

TEST(Extensions, VSCodeShellIntegration)
{
    Host host;
    Terminal t(40, 6, host);
    t.feed("\x1b]633;A\x07$ \x1b]633;B\x07ls; pwd\r\n");
    t.feed("\x1b]633;E;ls\\x3b pwd\\\\x;nonce123\x07\x1b]633;C\x07out\r\n\x1b]633;D;3\x07");
    ASSERT_EQ(host.started.size(), 1u);
    EXPECT_EQ(host.started[0], "ls; pwd\\x");
    ASSERT_EQ(host.finished.size(), 1u);
    EXPECT_EQ(host.finished[0], 3);
    t.feed("\x1b]633;P;Cwd=/srv/www\x07");
    EXPECT_EQ(host.cwd, "/srv/www");
    EXPECT_EQ(t.workingDirectory(), "/srv/www");
}

TEST(Extensions, SystemdContexts)
{
    Host host;
    Terminal t(40, 6, host);
    t.feed("\x1b]3008;start=a1;type=shell;user=fuddles;hostname=box\x1b\\");
    t.feed("\x1b]3008;start=b2;type=elevate;user=fuddles;targetuser=root;hostname=box\x1b\\");
    ASSERT_EQ(t.contexts().stack().size(), 2u);
    EXPECT_EQ(t.contexts().stack()[1].type, "elevate");
    EXPECT_EQ(t.contexts().stack()[1].targetUser, "root");
    // Ending the outer context ends the one inside it too.
    t.feed("\x1b]3008;end=a1;exit=success\x1b\\");
    EXPECT_TRUE(t.contexts().stack().empty());
    EXPECT_EQ(host.contextChanges, 3);
    // Malformed: both or neither of start and end.
    t.feed("\x1b]3008;type=shell\x1b\\\x1b]3008;start=x;end=x\x1b\\");
    EXPECT_TRUE(t.contexts().stack().empty());
    // iTerm2's RemoteHost lands there too.
    t.feed("\x1b]1337;RemoteHost=deploy@prod-1\x07");
    ASSERT_EQ(t.contexts().stack().size(), 1u);
    EXPECT_EQ(t.contexts().stack()[0].type, "remote");
    EXPECT_EQ(t.contexts().stack()[0].user, "deploy");
    EXPECT_EQ(t.contexts().stack()[0].hostname, "prod-1");
}

TEST(Extensions, ITerm2Commands)
{
    Host host;
    Terminal t(20, 4, host);
    t.resize(20, 4, 8, 16);
    t.feed("\x1b]1337;SetUserVar=branch=bWFpbg==\x07");
    ASSERT_EQ(host.userVars.size(), 1u);
    EXPECT_EQ(host.userVars[0], (std::pair<std::string, std::string>{"branch", "main"}));
    t.feed("\x1b]1337;CurrentDir=/home/fuddles\x07");
    EXPECT_EQ(host.cwd, "/home/fuddles");
    t.feed("\x1b]1337;ReportCellSize\x1b\\");
    EXPECT_EQ(host.written, "\x1b]1337;ReportCellSize=16.0;8.0;1.0\x1b\\");
    t.feed("a\r\nb\r\nc\r\nd\r\ne\r\nf");
    ASSERT_GT(t.screen().historySize(), 0u);
    t.feed("\x1b]1337;ClearScrollback\x07");
    EXPECT_EQ(t.screen().historySize(), 0u);
    t.feed("\x1b]1337;Copy=:aGk=\x07");
    EXPECT_EQ(host.clipboardWritten, "aGk=");
    t.feed("\x1b]1337;CursorShape=1\x07");
    EXPECT_EQ(t.cursorShape(), CursorShape::Bar);
}

TEST(Extensions, ITerm2InlineImages)
{
    Host host;
    Terminal t(20, 6, host);
    t.resize(20, 6, 8, 16);
    const std::string picture = "cGljdHVyZQ=="; // "picture"
    // A download (no inline=1) is not shown.
    t.feed("\x1b]1337;File=name=eA==;size=7:" + picture + "\x07");
    EXPECT_TRUE(t.graphics().placements().empty());
    // Three cells wide, keeping the 2:1 shape: 24 by 12 pixels.
    t.feed("\x1b]1337;File=inline=1;width=3:" + picture + "\x07");
    ASSERT_EQ(t.graphics().placements().size(), 1u);
    const Image& img = t.graphics().images().at(t.graphics().placements()[0].imageId);
    EXPECT_EQ(img.width, 24u);
    EXPECT_EQ(img.height, 12u);
    // One row high, so the cursor stays on it, right of the image.
    EXPECT_EQ(t.screen().cursor.y, 0);
    EXPECT_EQ(t.screen().cursor.x, 3);
    // In parts, and natural size.
    t.feed("\r\n\x1b]1337;MultipartFile=inline=1\x07\x1b]1337;FilePart=cGljdH\x07\x1b]1337;FilePart=VyZQ==\x07"
           "\x1b]1337;FileEnd\x07");
    ASSERT_EQ(t.graphics().placements().size(), 2u);
    const Image& second = t.graphics().images().at(t.graphics().placements()[1].imageId);
    EXPECT_EQ(second.width, 4u);
    EXPECT_EQ(second.height, 2u);
}

TEST(Extensions, ITerm2ImageSizes)
{
    detail::ITerm2File f;
    std::uint32_t w = 0;
    std::uint32_t h = 0;
    // Natural size, shrunk to the terminal's width (80 x 8 = 640 px).
    ASSERT_TRUE(detail::iterm2ImageSize(f, 1280, 640, 8, 16, 80, 24, w, h));
    EXPECT_EQ(w, 640u);
    EXPECT_EQ(h, 320u);
    // Pixels and percentages; a box keeps the shape.
    f.width = "100px";
    f.height = "50%";
    ASSERT_TRUE(detail::iterm2ImageSize(f, 200, 100, 8, 16, 80, 24, w, h));
    EXPECT_EQ(w, 100u);
    EXPECT_EQ(h, 50u);
    f.preserveAspectRatio = false;
    ASSERT_TRUE(detail::iterm2ImageSize(f, 200, 100, 8, 16, 80, 24, w, h));
    EXPECT_EQ(w, 100u);
    EXPECT_EQ(h, 192u); // half of 24 rows of 16 px
}

TEST(Extensions, ScaleFrame)
{
    ImageFrame in;
    in.width = 2;
    in.height = 1;
    in.rgba = {255, 0, 0, 255, 0, 0, 255, 255};
    const ImageFrame down = detail::scaleFrame(in, 1, 1);
    ASSERT_EQ(down.rgba.size(), 4u);
    EXPECT_NEAR(down.rgba[0], 127, 1);
    EXPECT_NEAR(down.rgba[2], 127, 1);
    EXPECT_EQ(down.rgba[3], 255);
    const ImageFrame up = detail::scaleFrame(in, 4, 2);
    ASSERT_EQ(up.rgba.size(), 32u);
    EXPECT_EQ(up.rgba[0], 255); // the left edge stays red
    EXPECT_EQ(up.rgba[3 * 4 + 2], 255); // the right edge blue
}

TEST(Extensions, ClipboardReadsAsk)
{
    Host host;
    Terminal t(20, 4, host);
    t.feed("\x1b]52;c;?\x07");
    ASSERT_EQ(host.clipboardReads.size(), 1u);
    const ClipboardRequest& r = host.clipboardReads[0];
    EXPECT_TRUE(r.osc52);
    EXPECT_FALSE(r.primary);
    EXPECT_EQ(r.osc52Selection, "c");
    // A host without the support answers with nothing, not silence.
    EXPECT_EQ(host.written, "\x1b]52;c;\x07");
    EXPECT_EQ(r.osc52Reply("hi"), "\x1b]52;c;aGk=\x07");
    host.written.clear();
    host.answerClipboard = true;
    t.feed("\x1b]52;p;?\x1b\\");
    ASSERT_EQ(host.clipboardReads.size(), 2u);
    EXPECT_TRUE(host.clipboardReads[1].primary);
    EXPECT_EQ(host.clipboardReads[1].osc52Reply("x"), "\x1b]52;p;eA==\x1b\\");
    EXPECT_TRUE(host.written.empty()); // the host answers later
}

TEST(Extensions, ConEmuCommands)
{
    Host host;
    Terminal t(20, 4, host);
    t.feed("\x1b]9;9;\"C:\\Users\\fuddles\"\x07");
    EXPECT_EQ(host.cwd, "C:\\Users\\fuddles");
    EXPECT_TRUE(host.notified.empty());
    t.feed("\x1b]9;12\x07\x1b]9;45;x\x07");
    EXPECT_TRUE(host.notified.empty()); // other ConEmu commands are not notifications
    t.feed("\x1b]9;42 is the answer\x07");
    EXPECT_EQ(host.notified, "|42 is the answer");
}

TEST(Extensions, DownloadsRequireCompleteValidBoundedData)
{
    struct Host : RecordingHost {
        int count = 0; std::string name, data;
        void downloadFile(std::string_view n, std::string_view d) override { ++count; name = n; data = d; }
    } host;
    Terminal t(20, 4, host);
    t.feed("\x1b]1337;File=name=dGVzdC50eHQ=;size=5:aGVsbG8=\x07");
    ASSERT_EQ(host.count, 1); EXPECT_EQ(host.name, "test.txt"); EXPECT_EQ(host.data, "hello");
    t.feed("\x1b]1337;File=size=6:aGVsbG8=\x07");
    t.feed("\x1b]1337;File=size=99999999:aGVsbG8=\x07");
    t.feed("\x1b]1337;File=:aGVsbG8!\x07");
    EXPECT_EQ(host.count, 1);
    t.feed("\x1b]1337;MultipartFile=name=dGVzdC50eHQ=;size=5\x07\x1b]1337;FilePart=aGVs\x07");
    EXPECT_EQ(host.count, 1);
    t.feed("\x1b]1337;FilePart=bG8=\x07\x1b]1337;FileEnd\x07");
    EXPECT_EQ(host.count, 2); EXPECT_EQ(host.data, "hello");
    t.feed("\x1b]1337;FileEnd\x07"); EXPECT_EQ(host.count, 2);
}

TEST(Extensions, MultipleCursorsQueryCoordinatesAndLifecycle)
{
    RecordingHost host; Terminal t(10, 5, host);
    t.feed("\x1b[> q\x1b[c");
    EXPECT_TRUE(host.written.starts_with("\x1b[>1;2;3;29;30;40;100;101 q\x1b[?"));
    t.feed("\x1b[>1;2:2:3:99:99:4 q");
    ASSERT_EQ(t.extraCursors().size(), 1u);
    EXPECT_EQ(t.extraCursors().at({1, 2}), 1);
    t.feed("\x1b[>2;4:4:9:9:12 q");
    EXPECT_EQ(t.extraCursors().size(), 5u);
    t.feed("\x1b[>0;2:2:3 q"); EXPECT_EQ(t.extraCursors().size(), 4u);
    host.written.clear(); t.feed("\x1b[>100 q");
    EXPECT_EQ(host.written, "\x1b[>100;2:2:4:9;2:2:4:10;2:2:5:9;2:2:5:10 q");
    t.feed("\x1b[5;1H\n\x1b[?25l"); EXPECT_EQ(t.extraCursors().size(), 4u);
    t.feed("\x1b[>0;4 q"); EXPECT_TRUE(t.extraCursors().empty());
    t.feed("\x1b[>29;0 q\x1b[?1049h"); EXPECT_TRUE(t.extraCursors().empty());
    t.feed("\x1b[>1;0 q\x1b[2J"); EXPECT_TRUE(t.extraCursors().empty());
    t.feed("\x1b[>1;0 q\x1b" "c"); EXPECT_TRUE(t.extraCursors().empty());
}

TEST(Extensions, ExtraCursorColorsRoundTripAndReset)
{
    RecordingHost host;
    Terminal t(20, 10, host);
    t.feed("\x1b[>30;2:12:34:56 q\x1b[>40;5:123 q\x1b[>101 q");
    EXPECT_EQ(host.written, "\x1b[>101;30:2:12:34:56;40:5:123 q");
    EXPECT_EQ(t.extraCursorColor().color, Color::palette(123));
    t.feed("\x1b[>40;2:256:0:0 q");
    EXPECT_EQ(t.extraCursorColor().color, Color::palette(123));
    t.feed("\x1b[>1;0 q\x1b" "c");
    EXPECT_TRUE(t.extraCursors().empty());
    EXPECT_EQ(t.extraCursorColor().mode, ExtraCursorColor::Mode::Main);
    EXPECT_EQ(t.extraCursorTextColor().mode, ExtraCursorColor::Mode::Main);
}

TEST(Extensions, MultipleCursorLongListsAndOverflowAreAtomic)
{
    RecordingHost host;
    Terminal t(100, 10, host);
    std::string command = "\x1b[>1;2";
    for (int i = 1; i <= 100; ++i) command += ":1:" + std::to_string(i);
    t.feed(command + " q");
    EXPECT_EQ(t.extraCursors().size(), 100u);
    t.feed("\x1b[>0;4 q");
    command = "\x1b[>1;2";
    for (int i = 0; i < 3000; ++i) command += ":1:1";
    t.feed(command + " q");
    EXPECT_TRUE(t.extraCursors().empty());
}

TEST(Extensions, PixelReportsAndImagePercentagesUseWideGeometry)
{
    RecordingHost host;
    Terminal t(2, 32769, host);
    t.resize(2, 32769, 65535, 65535);
    t.feed("\x1b[?2048h\x1b[14t\x1b[?2;1S");
    const auto pixels = std::to_string(32769LL * 65535);
    EXPECT_EQ(host.written, "\x1b[48;32769;2;" + pixels + ";131070t"
        "\x1b[4;" + pixels + ";131070t\x1b[?2;0;131070;" + pixels + "S");
    detail::ITerm2File file;
    file.width = "1px"; file.height = "100%"; file.preserveAspectRatio = false;
    std::uint32_t w = 0, h = 0;
    ASSERT_TRUE(detail::iterm2ImageSize(file, 1, 1, 65535, 65535, 2, 32769, w, h));
    EXPECT_EQ(w, 1u); EXPECT_EQ(h, 10000u);
}

TEST(Extensions, PointerShapeStacksQueriesAndReset)
{
    RecordingHost host;
    Terminal t(40, 6, host);
    t.feed("\x1b]22;?__current__,__default__,__grabbed__,zoom-in,alias,invalid\x1b\\");
    EXPECT_EQ(host.written, "\x1b]22;0,text,text,1,1,0\x1b\\");
    t.feed("\x1b]22;pointer\x07\x1b]22;>wait,zoom-out\x07");
    EXPECT_EQ(host.pointer, "zoom-out");
    t.feed("\x1b]22;<\x07"); EXPECT_EQ(host.pointer, "wait");
    t.feed("\x1b[?1049h"); EXPECT_EQ(host.pointer, "");
    t.feed("\x1b]22;copy\x07\x1b[?1049l"); EXPECT_EQ(host.pointer, "wait");
    t.feed("\x1b]22;=cell,vertical-text\x07"); EXPECT_EQ(host.pointer, "vertical-text");
    t.feed("\x1b]22;bogus\x07"); EXPECT_EQ(host.pointer, "vertical-text");
    t.feed("\x1b]22;<\x07"); EXPECT_EQ(host.pointer, "pointer");
    t.feed("\x1b]22;\x07"); EXPECT_EQ(host.pointer, "");
    for (int i = 0; i < 40; ++i) t.feed("\x1b]22;>wait\x07");
    for (int i = 0; i < 32; ++i) t.feed("\x1b]22;<\x07");
    EXPECT_EQ(host.pointer, "");
    t.feed("\x1b" "c\x1b[?1049h"); EXPECT_EQ(host.pointer, "");
    host.written.clear();
    t.feed("\x1b]22;?__current__\x07"); EXPECT_EQ(host.written, "\x1b]22;0\x07");
}

TEST(Extensions, KittyColorControlNamesDynamicValuesAndStack)
{
    RecordingHost host;
    TerminalOptions options;
    options.cursorTextColor = Rgb{10, 20, 30};
    options.selectionBackground = Rgb{40, 50, 60};
    Terminal t(10, 2, host, options);
    t.feed("\x1b]21;foreground=ReD;cursor_text=AliceBlue;visual_bell=green;selection_background=blue;transparent_background_color1=#123456@.25\x07");
    EXPECT_EQ(t.defaultForeground(), (Rgb{255, 0, 0}));
    ASSERT_NE(t.cursorTextColor(), nullptr);
    EXPECT_EQ(*t.cursorTextColor(), (Rgb{240, 248, 255}));
    ASSERT_NE(t.visualBellColor(), nullptr);
    ASSERT_TRUE(t.transparentColors()[0]);
    EXPECT_DOUBLE_EQ(t.transparentColors()[0]->opacity, .25);
    t.feed("\x1b]30001\x07\x1b]21;cursor_text=;selection_background=;visual_bell=;transparent_background_color1=\x07");
    EXPECT_EQ(t.cursorTextColor(), nullptr);
    EXPECT_EQ(t.selectionBackground(), nullptr);
    EXPECT_EQ(t.visualBellColor(), nullptr);
    EXPECT_FALSE(t.transparentColors()[0]);
    t.feed("\x1b]30101\x07");
    ASSERT_NE(t.selectionBackground(), nullptr);
    EXPECT_EQ(*t.selectionBackground(), (Rgb{0, 0, 255}));
    ASSERT_TRUE(t.transparentColors()[0]);
    host.written.clear();
    t.feed("\x1b]21;transparent_background_color1=?;visual_bell=?;bad=x;1bad=?\x07");
    EXPECT_EQ(host.written, "\x1b]21;transparent_background_color1=rgb:12/34/56@0.25;visual_bell=rgb:00/ff/00;unknown=YmFk;unknown=MWJhZA\x07");
    t.feed("\x1b]21;cursor_text;selection_background;foreground=rgbi:-1/2/.5;transparent_background_color8=red@-1\x07");
    EXPECT_EQ(*t.cursorTextColor(), (Rgb{10, 20, 30}));
    EXPECT_EQ(*t.selectionBackground(), (Rgb{40, 50, 60}));
    EXPECT_EQ(t.defaultForeground(), (Rgb{0, 255, 128}));
    ASSERT_TRUE(t.transparentColors()[7]);
    EXPECT_DOUBLE_EQ(t.transparentColors()[7]->opacity, -1);
    t.feed("\x1b" "c");
    EXPECT_FALSE(t.transparentColors()[0]);
    EXPECT_FALSE(t.transparentColors()[7]);
    EXPECT_EQ(*t.cursorTextColor(), (Rgb{10, 20, 30}));
}
