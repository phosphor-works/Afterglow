// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxProtocol.h"

#include <gtest/gtest.h>

using namespace AfterglowMux;
namespace Json = AfterglowProto::Json;

TEST(MuxProtocol, SessionNames)
{
    for (const char* name : {"1", "work", "a_b-C9", "x"}) EXPECT_TRUE(validSessionName(name)) << name;
    for (const char* name : {"", "a b", "a/b", "é", "a.b", "-\n"}) EXPECT_FALSE(validSessionName(name)) << name;
    EXPECT_TRUE(validSessionName(std::string(MaxSessionName, 'a')));
    EXPECT_FALSE(validSessionName(std::string(MaxSessionName + 1, 'a')));
}

TEST(MuxProtocol, SessionOptionsRoundTrip)
{
    SessionOptions o;
    o.name = "work";
    o.launch.argv = {"/bin/zsh", "-l"};
    o.launch.workingDirectory = "/tmp";
    o.launch.environment = {{"TERM", "afterglow"}, {"EMPTY", ""}};
    o.launch.unsetEnvironment = {"TMUX"};
    o.launch.launchedProgram = "htop";
    o.size = {132, 43, 9, 18};
    o.terminal.maxScrollback = 1234;
    o.terminal.defaultBackground = {1, 2, 3};
    o.terminal.basePalette[15] = {250, 251, 252};
    o.terminal.cursorColor = AfterglowVt::Rgb{9, 8, 7};
    o.terminal.selectionBackground.reset();
    o.terminal.minimumContrast = 4.5;
    o.terminal.wordBoundaries = U"│ ,";
    o.terminal.bidi = AfterglowVt::BidiMode::RightToLeft;
    o.terminal.cursorShape = AfterglowVt::CursorShape::Bar;
    o.terminal.graphicsQuota = std::size_t(64) << 20;
    o.terminal.graphicsFileTransfer = false;
    o.terminal.ambiguousWide = true;

    // Through the wire's text, as the daemon reads it.
    const auto parsed = Json::parse(Json::write(toJson(o)));
    ASSERT_TRUE(parsed);
    std::string error;
    const auto back = sessionOptions(*parsed, &error);
    ASSERT_TRUE(back) << error;
    EXPECT_EQ(back->name, o.name);
    EXPECT_EQ(back->launch.argv, o.launch.argv);
    EXPECT_EQ(back->launch.workingDirectory, o.launch.workingDirectory);
    EXPECT_EQ(back->launch.environment, o.launch.environment);
    EXPECT_EQ(back->launch.unsetEnvironment, o.launch.unsetEnvironment);
    EXPECT_EQ(back->launch.launchedProgram, o.launch.launchedProgram);
    EXPECT_EQ(back->size.cols, 132);
    EXPECT_EQ(back->size.rows, 43);
    EXPECT_EQ(back->size.cellWidth, 9);
    EXPECT_EQ(back->size.cellHeight, 18);
    EXPECT_TRUE(back->terminal == o.terminal);
}

TEST(MuxProtocol, SessionOptionsRefuseBadValues)
{
    const Json::Value good = toJson(SessionOptions{});
    const auto with = [&good](std::string_view key, Json::Value value) {
        Json::Object object = good.toObject();
        for (auto& [name, v] : object) {
            if (name == key) v = std::move(value);
        }
        return Json::Value(std::move(object));
    };
    std::string error;
    ASSERT_TRUE(sessionOptions(good, &error)) << error;
    const std::pair<const char*, Json::Value> bad[] = {
        {"name", "a b"},
        {"argv", Json::Array{1}},
        {"environment", Json::Array{Json::Array{"A=B", "c"}}},
        {"environment", Json::Array{Json::Array{"", "c"}}},
        {"environment", Json::Array{Json::Array{"A"}}},
        {"cols", 1},
        {"cols", 4097},
        {"rows", 0},
        {"cellWidth", -1},
        {"terminal", 3},
    };
    for (const auto& [key, value] : bad) {
        error.clear();
        EXPECT_FALSE(sessionOptions(with(key, value), &error)) << key;
        EXPECT_NE(error.find(key), std::string::npos) << error;
    }
    Json::Object terminal = good["terminal"].toObject();
    for (auto& [name, v] : terminal) {
        if (name == "palette") v = Json::Array{Json::Array{0, 0, 256}};
    }
    EXPECT_FALSE(sessionOptions(with("terminal", std::move(terminal)), &error));
    Json::Object missing = good.toObject();
    std::erase_if(missing, [](const auto& entry) { return entry.first == "cwd"; });
    EXPECT_FALSE(sessionOptions(Json::Value(std::move(missing)), &error));
    EXPECT_NE(error.find("cwd"), std::string::npos) << error;
}

TEST(MuxProtocol, SessionInfoRoundTrip)
{
    SessionInfo info{"2", true, 1, 3, {"vim", "make"}};
    const auto back = sessionInfo(toJson(info));
    ASSERT_TRUE(back);
    EXPECT_EQ(back->name, "2");
    EXPECT_TRUE(back->attached);
    EXPECT_EQ(back->windows, 1);
    EXPECT_EQ(back->panes, 3);
    EXPECT_EQ(back->jobs, info.jobs);
    EXPECT_FALSE(sessionInfo(Json::Object{{"name", "x"}}));
}
