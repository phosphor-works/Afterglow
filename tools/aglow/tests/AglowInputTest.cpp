// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The input parser and the command line of aglow, without a terminal, and
// the Tty against a terminal that the test plays in a pseudo-terminal.

#include "../Cli.h"
#include "../Tty.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <csignal>
#include <fcntl.h>
#include <functional>
#include <poll.h>
#include <pty.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace Aglow;

namespace {

std::vector<Input> parse(InputParser& parser, std::string_view bytes)
{
    std::vector<Input> out;
    parser.feed(bytes, out);
    return out;
}

std::optional<Cli::Parsed> run(const Cli::Command& command, std::vector<std::string> args, std::string& error)
{
    std::vector<char*> argv;
    for (auto& a : args) {
        argv.push_back(a.data());
    }
    bool help = false;
    return Cli::parse(command, int(argv.size()), argv.data(), error, help);
}

const Cli::Command command {
    "test",
    "",
    "",
    "",
    {
        { "--flag -f", Cli::Option::Kind::Flag, "", {}, "" },
        { "--other -o", Cli::Option::Kind::Flag, "", {}, "" },
        { "--direction -d", Cli::Option::Kind::String, "", {}, "" },
        { "--size", Cli::Option::Kind::Int, "512", {}, "", 16, 4096 },
        { "--timeout", Cli::Option::Kind::Float, "10", {}, "" },
    },
};

// A program whose controlling terminal is a new pseudo-terminal. The test
// plays the terminal on the master side. The program runs in a child
// process, and its result is the exit status.
struct PlayedTerminal
{
    pid_t pid = -1;
    int master = -1;
    std::string output;

    ~PlayedTerminal()
    {
        if (pid > 0) {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
        }
        if (master >= 0) {
            ::close(master);
        }
    }
    bool start(const std::function<int()>& program)
    {
        winsize size { 24, 80, 0, 0 };
        pid = ::forkpty(&master, nullptr, nullptr, &size);
        if (pid == 0) {
            ::_exit(program());
        }
        return pid > 0;
    }
    // Reads what the program writes until @p done, for up to @p ms.
    bool readUntil(const std::function<bool()>& done, int ms = 5000)
    {
        const long long deadline = nowMs() + ms;
        while (!done()) {
            const long long left = deadline - nowMs();
            if (left <= 0) {
                return false;
            }
            pollfd fd { master, POLLIN, 0 };
            if (::poll(&fd, 1, int(left)) > 0) {
                char buffer[4096];
                const auto n = ::read(master, buffer, sizeof buffer);
                if (n <= 0) {
                    return done();
                }
                output.append(buffer, std::size_t(n));
            }
        }
        return true;
    }
    void settle(int ms) { readUntil([] { return false; }, ms); }
    void say(std::string_view bytes) { ASSERT_EQ(::write(master, bytes.data(), bytes.size()), ssize_t(bytes.size())); }
    std::size_t count(std::string_view text, std::size_t from = 0) const
    {
        std::size_t n = 0;
        for (auto at = output.find(text, from); at != std::string::npos; at = output.find(text, at + text.size())) {
            ++n;
        }
        return n;
    }
    // The exit status of the program, or -1 when it does not end in @p ms.
    int wait(int ms = 8000)
    {
        const long long deadline = nowMs() + ms;
        while (nowMs() < deadline) {
            int status = 0;
            if (::waitpid(pid, &status, WNOHANG) == pid) {
                pid = -1;
                return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            }
            settle(20);
        }
        return -1;
    }
};

constexpr std::string_view Da1 = "\x1b[c";

// Turns @p tty for @p ms, and keeps what came in @p inputs.
void turnFor(Tty& tty, int ms, std::vector<Input>& inputs)
{
    const long long deadline = nowMs() + ms;
    while (nowMs() < deadline) {
        tty.turn(int(std::max(1LL, deadline - nowMs())), inputs);
    }
}

bool isDa1Answer(const Input& input)
{
    return input.type == Input::Type::Csi && input.data.starts_with('?') && input.data.ends_with('c');
}

} // namespace

TEST(AglowInput, Ss3KeysAreKeysNotEscape)
{
    InputParser parser;
    const auto out = parse(parser, "\x1bOP\x1bOA");
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].type, Input::Type::Key);
    EXPECT_EQ(out[0].data, "OP");
    EXPECT_EQ(out[1].data, "OA");
}

TEST(AglowInput, KittyAndModifyOtherKeysEscAndCtrlC)
{
    InputParser parser;
    const auto out = parse(parser, "\x1b[27u\x1b[99;5u\x1b[27;5;99~\x1b[99;69u\x1b[97;5u");
    ASSERT_EQ(out.size(), 5u);
    EXPECT_EQ(out[0].type, Input::Type::Escape);
    EXPECT_EQ(out[1].type, Input::Type::Interrupt);
    EXPECT_EQ(out[2].type, Input::Type::Interrupt);
    // Ctrl with Caps Lock is still Ctrl+C.
    EXPECT_EQ(out[3].type, Input::Type::Interrupt);
    EXPECT_EQ(out[4].type, Input::Type::Csi);
}

TEST(AglowInput, EscEscAfterAStringIsNoEscKey)
{
    InputParser parser;
    const auto out = parse(parser, "\x1b]52;c;YQ==\x1b\x1b[?62c");
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].type, Input::Type::Osc);
    EXPECT_EQ(out[1].type, Input::Type::Csi);
    EXPECT_EQ(out[1].data, "?62c");
}

TEST(AglowInput, ControlsInsideCsi)
{
    InputParser parser;
    // ESC cuts the code short and starts the next one; other controls are no part of a code.
    auto out = parse(parser, "\x1b[?6\x1b[?62;\n22c");
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].data, "?62;22c");
}

TEST(AglowInput, AltBracketTimesOutAsKeys)
{
    InputParser parser;
    auto out = parse(parser, "\x1b]");
    EXPECT_TRUE(out.empty());
    EXPECT_TRUE(parser.waitingAfterEscape());
    parser.timeout(out);
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].type, Input::Type::Escape);
    EXPECT_EQ(out[1].data, "]");
    // The keys after it are keys again.
    out = parse(parser, "y");
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].data, "y");
}

TEST(AglowInput, UnambiguousKeysNeverTimeOut)
{
    InputParser parser;
    parser.setKeysUnambiguous(true);
    auto out = parse(parser, "\x1b");
    EXPECT_FALSE(parser.waitingAfterEscape());
    parser.timeout(out);
    EXPECT_TRUE(out.empty());
    out = parse(parser, "[?1u");
    EXPECT_TRUE(out.empty());
    EXPECT_EQ(parser.takeKeyboardReply(), 1);
    EXPECT_EQ(parser.takeKeyboardReply(), -1);
    // The protocol, with no flag on: the push did not take.
    parse(parser, "\x1b[?0u");
    EXPECT_EQ(parser.takeKeyboardReply(), 0);
}

TEST(AglowInput, PasteIsNoAnswer)
{
    InputParser parser;
    const auto out = parse(parser, "\x1b[200~a\r\x1b[2b\x1b[201~\r");
    ASSERT_EQ(out.size(), 2u);
    EXPECT_EQ(out[0].type, Input::Type::Paste);
    EXPECT_EQ(out[0].data, "a\r\x1b[2b");
    EXPECT_EQ(out[1].type, Input::Type::Text);
    EXPECT_EQ(out[1].data, "\r");
}

TEST(AglowInput, LongOscOnlyWhenAllowed)
{
    const std::string body = "52;c;" + std::string(2 * 1024 * 1024, 'A');
    InputParser parser;
    EXPECT_TRUE(parse(parser, "\x1b]" + body + "\x07").empty());
    parser.allowLongOsc("52;", 4 * 1024 * 1024);
    const auto out = parse(parser, "\x1b]" + body + "\x07");
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].data.size(), body.size());
}

TEST(AglowInput, PrintableHidesBidiControls)
{
    EXPECT_EQ(printable("a" "\u202e" "b" "\u2066" "c\x1b"), "a?b?c?");
    EXPECT_EQ(cellWidth(U'a'), 1);
    EXPECT_EQ(cellWidth(U'一'), 2);
    EXPECT_EQ(cellWidth(U'́'), 0);
}

TEST(AglowCli, NumbersAreCheckedAtParseTime)
{
    std::string error;
    EXPECT_TRUE(run(command, { "--size=16", "--timeout=2.5" }, error)) << error;
    EXPECT_FALSE(run(command, { "--size=8" }, error));
    EXPECT_FALSE(run(command, { "--size=99999" }, error));
    EXPECT_FALSE(run(command, { "--timeout=inf" }, error));
    EXPECT_FALSE(run(command, { "--timeout=nan" }, error));
    EXPECT_FALSE(run(command, { "--timeout=1e30" }, error));
    EXPECT_FALSE(run(command, { "--timeout=1e400" }, error));
}

TEST(AglowCli, IntegerBoundsPrintInFull)
{
    const Cli::Command wide { "test", "", "", "", { { "--id", Cli::Option::Kind::Int, "0", {}, "", 0, 4294967295.0 } } };
    std::string error;
    EXPECT_FALSE(run(wide, { "--id=-1" }, error));
    EXPECT_EQ(error, "The value of --id must be from 0 to 4294967295, not -1");
    EXPECT_NE(Cli::helpText(wide).find("From 0 to 4294967295."), std::string::npos);
}

TEST(AglowCli, ShortOptionsTogether)
{
    std::string error;
    auto parsed = run(command, { "-fod", "send" }, error);
    ASSERT_TRUE(parsed) << error;
    EXPECT_TRUE(parsed->flag("flag"));
    EXPECT_TRUE(parsed->flag("other"));
    EXPECT_EQ(parsed->value("direction"), "send");
    parsed = run(command, { "-fdsend" }, error);
    ASSERT_TRUE(parsed) << error;
    EXPECT_EQ(parsed->value("direction"), "send");
    EXPECT_FALSE(run(command, { "-xyz" }, error));
    EXPECT_EQ(error, "Unknown option: -xyz");
}

TEST(AglowCli, AValueOptionDoesNotTakeTheNextOption)
{
    std::string error;
    EXPECT_FALSE(run(command, { "--direction", "--flag" }, error));
    EXPECT_FALSE(run(command, { "--direction", "--" }, error));
    const auto parsed = run(command, { "--direction=--flag" }, error);
    ASSERT_TRUE(parsed) << error;
    EXPECT_EQ(parsed->value("direction"), "--flag");
}

TEST(AglowCli, AMissingValueNamesTheFormThatTakesIt)
{
    std::string error;
    EXPECT_FALSE(run(command, { "-d" }, error));
    EXPECT_EQ(error, "The option -d needs a value. A value that starts with -- goes as -dVALUE");
    EXPECT_FALSE(run(command, { "--direction" }, error));
    EXPECT_EQ(error, "The option --direction needs a value. A value that starts with -- goes as --direction=VALUE");
}

// The answer to a command's own DA1 query reaches the command, and the
// answer to the Tty's query at open does not.
TEST(AglowTty, ACommandGetsTheAnswerToItsQueryOnly)
{
    PlayedTerminal terminal;
    ASSERT_TRUE(terminal.start([] {
        Tty tty;
        std::string error;
        if (!tty.open(error)) {
            return 100;
        }
        tty.queryAttributes();
        std::vector<Input> inputs;
        turnFor(tty, 500, inputs);
        tty.restore();
        return int(std::count_if(inputs.begin(), inputs.end(), isDa1Answer));
    }));
    ASSERT_TRUE(terminal.readUntil([&] { return terminal.count(Da1) >= 2; }));
    terminal.say("\x1b[?62;1c\x1b[?62;2c");
    EXPECT_EQ(terminal.wait(), 1);
}

// restore() reads up to the answer of every DA1 query on its way, also of
// those that a command made: the last answer is not typed into the shell.
TEST(AglowTty, TheDrainWaitsForTheAnswersToCommandQueries)
{
    PlayedTerminal terminal;
    ASSERT_TRUE(terminal.start([] {
        Tty tty;
        std::string error;
        if (!tty.open(error)) {
            return 100;
        }
        tty.drainOnRestore();
        tty.queryAttributes();
        std::vector<Input> inputs;
        turnFor(tty, 200, inputs);
        if (!tty.restore()) {
            return 101;
        }
        // What came after the restore waits for the shell. In canonical
        // mode, FIONREAD counts only whole lines.
        ::usleep(600 * 1000);
        const int fd = ::open("/dev/tty", O_RDONLY | O_NOCTTY | O_CLOEXEC);
        termios modes { };
        ::tcgetattr(fd, &modes);
        modes.c_lflag &= ~tcflag_t(ICANON);
        ::tcsetattr(fd, TCSANOW, &modes);
        int waiting = 0;
        ::ioctl(fd, FIONREAD, &waiting);
        return std::min(waiting, 99);
    }));
    ASSERT_TRUE(terminal.readUntil([&] { return terminal.count(Da1) >= 1; }));
    terminal.say("\x1b[?62;1c");
    // The reset: every query that the drain waits for went before it, or right after it.
    ASSERT_TRUE(terminal.readUntil([&] { return terminal.output.find("\x1b[?2004l") != std::string::npos; }));
    terminal.settle(100);
    // The answers to the queries still open, one at a time: a drain that
    // ends at the first leaves the second to the shell.
    const auto open = terminal.count(Da1) - 1;
    ASSERT_GE(open, 1u);
    for (std::size_t i = 0; i < open; ++i) {
        terminal.say("\x1b[?62;" + std::to_string(i + 2) + "c");
        terminal.settle(300);
    }
    EXPECT_EQ(terminal.wait(), 0);
}

// A terminal that never answers DA1 gets the time of one wait from the
// query: a program that ran longer than that ends at once.
TEST(AglowTty, NoWaitForATerminalThatDoesNotAnswer)
{
    PlayedTerminal terminal;
    ASSERT_TRUE(terminal.start([] {
        Tty tty;
        std::string error;
        if (!tty.open(error)) {
            return 100;
        }
        tty.drainOnRestore();
        std::vector<Input> inputs;
        turnFor(tty, 2300, inputs);
        const long long start = nowMs();
        tty.restore();
        // Tenths of a second.
        return int(std::min(99LL, (nowMs() - start) / 100));
    }));
    EXPECT_LT(terminal.wait(), 5);
}

// CSI ? 0 u: the terminal knows kitty's keyboard protocol, and the push
// did not take. The Esc key is then a lone ESC.
TEST(AglowTty, AKeyboardReplyWithoutTheFlagKeepsTheEscTimeout)
{
    PlayedTerminal terminal;
    ASSERT_TRUE(terminal.start([] {
        Tty tty;
        std::string error;
        if (!tty.open(error)) {
            return 100;
        }
        const long long deadline = nowMs() + 3000;
        while (nowMs() < deadline) {
            std::vector<Input> inputs;
            tty.turn(50, inputs);
            if (std::any_of(inputs.begin(), inputs.end(), [](const Input& i) { return i.type == Input::Type::Escape; })) {
                return 0;
            }
        }
        return 1;
    }));
    ASSERT_TRUE(terminal.readUntil([&] { return terminal.count(Da1) >= 1; }));
    terminal.say("\x1b[?0u\x1b[?62c");
    terminal.settle(200);
    terminal.say("\x1b");
    EXPECT_EQ(terminal.wait(), 0);
}

// Stops @p terminal's program and lets it go on.
static void stopAndContinue(PlayedTerminal& terminal)
{
    ::kill(terminal.pid, SIGSTOP);
    int status = 0;
    ASSERT_EQ(::waitpid(terminal.pid, &status, WUNTRACED), terminal.pid);
    ASSERT_TRUE(WIFSTOPPED(status));
    ::kill(terminal.pid, SIGCONT);
}

// After a stop, the modes that the shell may have changed come back, and
// the command draws its screen again. The keyboard mode is set after the
// resume, on the screen that it switches to, and no stop pushes it again.
TEST(AglowTty, AContinueRestoresTheModesAndRedraws)
{
    PlayedTerminal terminal;
    ASSERT_TRUE(terminal.start([] {
        Tty tty;
        std::string error;
        if (!tty.open(error)) {
            return 100;
        }
        tty.enterModes("\x1b[22;2t\x1b[?1049h\x1b[?1002h", "\x1b[?1002l\x1b[?1049l\x1b[23;2t", "\x1b[?1049h\x1b[?1002h");
        int continues = 0;
        const long long deadline = nowMs() + 8000;
        while (nowMs() < deadline) {
            std::vector<Input> inputs;
            tty.turn(50, inputs);
            if (tty.takeResize() && ++continues == 2) {
                turnFor(tty, 200, inputs);
                return 0;
            }
        }
        return 1;
    }));
    ASSERT_TRUE(terminal.readUntil([&] { return terminal.count(Da1) >= 1; }));
    terminal.say("\x1b[?62c");
    terminal.settle(100);
    constexpr std::string_view Resumed = "\x1b[?2004h\x1b[?1049h\x1b[?1002h\x1b[=1;1u";
    for (int i = 0; i < 2; ++i) {
        const auto before = terminal.output.size();
        stopAndContinue(terminal);
        EXPECT_TRUE(terminal.readUntil([&] { return terminal.output.find(Resumed, before) != std::string::npos; }));
    }
    EXPECT_EQ(terminal.wait(), 0);
    // The title and the keyboard mode are pushed once each: the reset pops
    // them once each.
    EXPECT_EQ(terminal.count("\x1b[22;2t"), 1u);
    EXPECT_EQ(terminal.count("\x1b[>1u"), 2u);
    EXPECT_EQ(terminal.count("\x1b[<u"), 2u);
}

// A continue while restore() waits for the drain sets no mode again: the
// reset is out, and the modes would outlast the program.
TEST(AglowTty, AContinueDuringTheRestoreSetsNoModes)
{
    PlayedTerminal terminal;
    ASSERT_TRUE(terminal.start([] {
        Tty tty;
        std::string error;
        if (!tty.open(error)) {
            return 100;
        }
        tty.enterModes("\x1b[?1002h", "\x1b[?1002l", "\x1b[?1002h");
        tty.drainOnRestore();
        std::vector<Input> inputs;
        turnFor(tty, 200, inputs);
        return tty.restore() ? 0 : 101;
    }));
    ASSERT_TRUE(terminal.readUntil([&] { return terminal.count(Da1) >= 1; }));
    terminal.say("\x1b[?62;1c");
    ASSERT_TRUE(terminal.readUntil([&] { return terminal.output.find("\x1b[?2004l") != std::string::npos; }));
    const auto reset = terminal.output.find("\x1b[?2004l");
    stopAndContinue(terminal);
    terminal.settle(300);
    terminal.say("\x1b[?62;2c");
    EXPECT_EQ(terminal.wait(), 0);
    EXPECT_EQ(terminal.output.find("\x1b[?2004h", reset), std::string::npos);
    EXPECT_EQ(terminal.output.find("\x1b[=1;1u", reset), std::string::npos);
    EXPECT_EQ(terminal.output.find("\x1b[?1002h", reset), std::string::npos);
}

// With drainOnRestore(), the drain reads past the replies to what was
// written after the last query on its way, also when that query has no
// answer yet: a terminal that sends them late does not type them into the
// shell.
TEST(AglowTty, TheDrainReadsPastOutputAfterTheLastQuery)
{
    PlayedTerminal terminal;
    ASSERT_TRUE(terminal.start([] {
        Tty tty;
        std::string error;
        if (!tty.open(error)) {
            return 100;
        }
        tty.drainOnRestore();
        tty.queryAttributes();
        // A cursor position report, which the terminal answers after the query.
        tty.write("\x1b[6n");
        if (!tty.restore()) {
            return 101;
        }
        ::usleep(600 * 1000);
        const int fd = ::open("/dev/tty", O_RDONLY | O_NOCTTY | O_CLOEXEC);
        termios modes { };
        ::tcgetattr(fd, &modes);
        modes.c_lflag &= ~tcflag_t(ICANON);
        ::tcsetattr(fd, TCSANOW, &modes);
        int waiting = 0;
        ::ioctl(fd, FIONREAD, &waiting);
        return std::min(waiting, 99);
    }));
    ASSERT_TRUE(terminal.readUntil([&] { return terminal.output.find("\x1b[?2004l") != std::string::npos; }));
    terminal.settle(100);
    // The answers to the queries of the open and of the command, then the
    // report a little later, then the answers to the queries after it.
    terminal.say("\x1b[?62;1c\x1b[?62;2c");
    terminal.settle(300);
    terminal.say("\x1b[1;1R");
    const auto after = terminal.output.find("\x1b[6n");
    ASSERT_NE(after, std::string::npos);
    ASSERT_TRUE(terminal.readUntil([&] { return terminal.count(Da1, after) >= 1; }));
    const auto fences = terminal.count(Da1, after);
    EXPECT_GE(fences, 1u);
    for (std::size_t i = 0; i < fences; ++i) {
        terminal.say("\x1b[?62;" + std::to_string(i + 3) + "c");
    }
    EXPECT_EQ(terminal.wait(), 0);
}

// A Tty that opens again after a drain that gave up starts with no query
// and no resume of the last session: the next answers go to their own
// queries, and restore() waits for no query of the last session.
TEST(AglowTty, AReopenStartsWithoutTheLastSession)
{
    PlayedTerminal terminal;
    ASSERT_TRUE(terminal.start([] {
        Tty tty;
        std::string error;
        if (!tty.open(error)) {
            return 100;
        }
        tty.enterModes("", "", "\x1b[?1002h");
        tty.queryAttributes();
        std::vector<Input> inputs;
        // Past the wait for the command's query, which gets no answer.
        turnFor(tty, 2100, inputs);
        tty.restore();
        if (!tty.open(error)) {
            return 100;
        }
        tty.queryAttributes();
        std::vector<std::string> answers;
        const long long deadline = nowMs() + 5000;
        bool continued = false;
        // The key comes after the answers: once it is read, so are they.
        bool key = false;
        while (!(continued && key) && nowMs() < deadline) {
            inputs.clear();
            tty.turn(50, inputs);
            for (const auto& input : inputs) {
                if (isDa1Answer(input)) {
                    answers.push_back(input.data);
                }
                key |= input.type == Input::Type::Text && input.data == "k";
            }
            continued |= tty.takeResize();
        }
        turnFor(tty, 200, inputs);
        const long long start = nowMs();
        tty.restore();
        if (!continued || !key) {
            return 1;
        }
        if (answers != std::vector<std::string> { "?62;2c" }) {
            return 2;
        }
        return nowMs() - start < 1000 ? 0 : 3;
    }));
    ASSERT_TRUE(terminal.readUntil([&] { return terminal.count(Da1) >= 2; }));
    terminal.say("\x1b[?62;1c");
    // The second open, and its command's query.
    ASSERT_TRUE(terminal.readUntil([&] { return terminal.count(Da1) >= 4; }));
    terminal.say("\x1b[?62;1c\x1b[?62;2ck");
    terminal.settle(100);
    const auto before = terminal.output.size();
    stopAndContinue(terminal);
    EXPECT_TRUE(terminal.readUntil([&] { return terminal.output.find("\x1b[=1;1u", before) != std::string::npos; }));
    EXPECT_EQ(terminal.wait(), 0);
    EXPECT_EQ(terminal.output.find("\x1b[?1002h", before), std::string::npos);
}

// The output that restore() writes while the terminal reads nothing.
// Larger than the buffers of a pseudo-terminal, so that the first flush
// waits.
constexpr std::size_t Blocking = 256 * 1024;

// A continue while restore() writes the output that came before the reset
// sets the modes again: they go out before the reset, which turns them off.
TEST(AglowTty, AContinueBeforeTheResetSetsTheModes)
{
    PlayedTerminal terminal;
    ASSERT_TRUE(terminal.start([] {
        Tty tty;
        std::string error;
        if (!tty.open(error)) {
            return 100;
        }
        tty.enterModes("\x1b[?1002h", "\x1b[?1002l", "\x1b[?1002h");
        tty.write(std::string(Blocking, 'x'));
        return tty.restore() ? 0 : 101;
    }));
    // The output of restore(): the program waits in its first flush, which
    // one read does not end.
    ASSERT_TRUE(terminal.readUntil([&] { return terminal.output.find('x') != std::string::npos; }));
    stopAndContinue(terminal);
    ASSERT_TRUE(terminal.readUntil([&] { return terminal.output.find("\x1b[?2004l") != std::string::npos; }));
    terminal.say("\x1b[?62c");
    EXPECT_EQ(terminal.wait(), 0);
    const auto resumed = terminal.output.find("\x1b[?2004h\x1b[?1002h\x1b[=1;1u");
    ASSERT_NE(resumed, std::string::npos);
    EXPECT_LT(resumed, terminal.output.find("\x1b[?1002l"));
}

// The keys and the code cut short that restore() read go with its session:
// a Tty that opens again gets neither.
TEST(AglowTty, AReopenGetsNoInputOfTheLastSession)
{
    auto typed = [](const std::vector<Input>& inputs, char c) {
        return std::any_of(inputs.begin(), inputs.end(), [&](const Input& i) {
            return i.type == Input::Type::Text && i.data.find(c) != std::string::npos;
        });
    };
    PlayedTerminal terminal;
    ASSERT_TRUE(terminal.start([&] {
        Tty tty;
        std::string error;
        if (!tty.open(error)) {
            return 100;
        }
        // The key after the answer to the query of the open.
        std::vector<Input> inputs;
        const long long answered = nowMs() + 3000;
        while (!typed(inputs, 'a') && nowMs() < answered) {
            tty.turn(50, inputs);
        }
        tty.write(std::string(Blocking, 'x'));
        tty.restore();
        if (!tty.open(error)) {
            return 100;
        }
        inputs.clear();
        const long long deadline = nowMs() + 3000;
        while (!typed(inputs, 'z') && nowMs() < deadline) {
            tty.turn(50, inputs);
        }
        tty.restore();
        if (typed(inputs, 'q')) {
            return 1;
        }
        return typed(inputs, 'z') ? 0 : 2;
    }));
    ASSERT_TRUE(terminal.readUntil([&] { return terminal.count(Da1) >= 1; }));
    terminal.say("\x1b[?62ca");
    // The program waits in the first flush of restore(), and reads a key and
    // the start of a title.
    ASSERT_TRUE(terminal.readUntil([&] { return terminal.output.find('x') != std::string::npos; }));
    terminal.say("q");
    ::usleep(100 * 1000);
    terminal.say("\x1b]2;partial");
    ::usleep(100 * 1000);
    // The second open. A parser that still reads the title takes the key.
    ASSERT_TRUE(terminal.readUntil([&] { return terminal.count(Da1) >= 2; }));
    terminal.say("z");
    terminal.settle(100);
    terminal.say("\x1b[?62c");
    EXPECT_EQ(terminal.wait(), 0);
}

// A continue that wakes the poll of an idle program is taken by restore()
// when no step() took it: the modes go out before the reset.
TEST(AglowTty, AContinueJustBeforeTheRestoreSetsTheModes)
{
    PlayedTerminal terminal;
    ASSERT_TRUE(terminal.start([] {
        Tty tty;
        std::string error;
        if (!tty.open(error)) {
            return 100;
        }
        tty.enterModes("\x1b[?1002h", "\x1b[?1002l", "\x1b[?1002h");
        std::vector<Input> inputs;
        const long long answered = nowMs() + 3000;
        while (std::none_of(inputs.begin(), inputs.end(), [](const Input& i) { return i.data == "a"; })
               && nowMs() < answered) {
            tty.turn(50, inputs);
        }
        tty.write("ready");
        while (tty.pending() > 0) {
            tty.turn(50, inputs);
        }
        // Idle: the continue ends this poll with EINTR.
        tty.turn(5000, inputs);
        return tty.restore() ? 0 : 101;
    }));
    ASSERT_TRUE(terminal.readUntil([&] { return terminal.count(Da1) >= 1; }));
    terminal.say("\x1b[?62ca");
    ASSERT_TRUE(terminal.readUntil([&] { return terminal.output.find("ready") != std::string::npos; }));
    terminal.settle(100);
    const auto before = terminal.output.size();
    stopAndContinue(terminal);
    ASSERT_TRUE(terminal.readUntil([&] { return terminal.output.find("\x1b[?2004l", before) != std::string::npos; }));
    EXPECT_EQ(terminal.wait(), 0);
    const auto resumed = terminal.output.find("\x1b[?2004h\x1b[?1002h\x1b[=1;1u", before);
    ASSERT_NE(resumed, std::string::npos);
    EXPECT_LT(resumed, terminal.output.find("\x1b[?1002l", before));
}

// A reset that the terminal did not take in time goes with its session: a
// Tty that opens again does not write the rest of it after what the
// program wrote meanwhile.
TEST(AglowTty, AReopenWritesNoResetOfTheLastSession)
{
    int sync[2];
    ASSERT_EQ(::pipe(sync), 0);
    PlayedTerminal terminal;
    ASSERT_TRUE(terminal.start([&] {
        ::close(sync[0]);
        Tty tty;
        std::string error;
        if (!tty.open(error)) {
            return 100;
        }
        tty.enterModes("\x1b[?1002h", "\x1b[?1002l");
        tty.write(std::string(Blocking, 'x'));
        // The terminal reads nothing: the output and then the reset time out.
        const char restored = tty.restore() ? '1' : '0';
        [[maybe_unused]] const auto n = ::write(sync[1], &restored, 1);
        if (!tty.open(error)) {
            return 100;
        }
        return tty.restore() ? 0 : 101;
    }));
    ::close(sync[1]);
    pollfd fd { sync[0], POLLIN, 0 };
    ASSERT_EQ(::poll(&fd, 1, 10000), 1);
    char restored = 0;
    ASSERT_EQ(::read(sync[0], &restored, 1), 1);
    ::close(sync[0]);
    EXPECT_EQ(restored, '0');
    // The reset of the second session, and the answer that its drain waits for.
    ASSERT_TRUE(terminal.readUntil([&] { return terminal.output.find("\x1b[?2004l") != std::string::npos; }));
    terminal.say("\x1b[?62c");
    EXPECT_EQ(terminal.wait(), 0);
    EXPECT_EQ(terminal.count("\x1b[?2004l"), 1u);
    EXPECT_EQ(terminal.output.find("\x1b[?1002l"), std::string::npos);
}
