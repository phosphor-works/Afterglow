// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// The input parser and the command line of aglow, without a terminal.

#include "../Cli.h"
#include "../Tty.h"

#include <gtest/gtest.h>

#include <string>
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
    EXPECT_TRUE(parser.takeKeyboardReply());
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
