// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxProtocol.h"

#include <gtest/gtest.h>

using namespace AfterglowMux;
namespace Json = AfterglowProto::Json;

namespace {

/// The keys of @p object, in their order.
std::vector<std::string> keys(const Json::Value& object)
{
    std::vector<std::string> list;
    for (const auto& [key, value] : object.toObject()) list.push_back(key);
    return list;
}

/// @p object with @p key set to @p value; a key that it lacks is added.
Json::Value with(const Json::Value& object, std::string_view key, Json::Value value)
{
    Json::Object copy = object.toObject();
    for (auto& [name, v] : copy) {
        if (name == key) {
            v = std::move(value);
            return Json::Value(std::move(copy));
        }
    }
    copy.emplace_back(std::string(key), std::move(value));
    return Json::Value(std::move(copy));
}

Json::Value without(const Json::Value& object, std::string_view key)
{
    Json::Object copy = object.toObject();
    std::erase_if(copy, [&](const auto& entry) { return entry.first == key; });
    return Json::Value(std::move(copy));
}

} // namespace

TEST(MuxProtocol, SessionNames)
{
    for (const char* name : {"1", "work", "a_b-C9", "x", "_x", "a-", "a--b"}) EXPECT_TRUE(validSessionName(name)) << name;
    // A name that starts with '-' would read as an option of `afterglow mux kill`.
    for (const char* name : {"", "a b", "a/b", "é", "a.b", "-\n", "-", "--", "-a", "--force"}) EXPECT_FALSE(validSessionName(name)) << name;
    EXPECT_TRUE(validSessionName(std::string(MaxSessionName, 'a')));
    EXPECT_FALSE(validSessionName(std::string(MaxSessionName + 1, 'a')));
    EXPECT_NE(std::string(SessionNameRule).find("ASCII letters"), std::string::npos);
}

// A key that one end sends and the other does not know is a change of the
// protocol: this list and ProtocolVersion change together.
TEST(MuxProtocol, TheKeysBelongToTheProtocolVersion)
{
    ASSERT_EQ(ProtocolVersion, 3) << "a new protocol version: update the key lists of this test";
    // A new field that toJson leaves out would still pass the key lists. The
    // bindings name every field, so a new field stops this test compiling
    // until it has its key, its place in the round trip and a new version.
    SessionOptions fields;
    [[maybe_unused]] auto& [name, launch, size, terminal, notificationCapabilities] = fields;
    [[maybe_unused]] auto& [argv, cwd, environment, launchedProgram, umask] = fields.launch;
    [[maybe_unused]] auto& [cols, rows, cellWidth, cellHeight] = fields.size;
    [[maybe_unused]] auto& [scrollback, foreground, background, palette, cursorColor, cursorTextColor, selectionForeground,
                            selectionBackground, boldIsBright, minimumContrast, wordBoundaries, ambiguousWide, bidi, cursorShape,
                            cursorBlink, allowScreenReadback, graphicsQuota, graphicsFileTransfer] = fields.terminal;
    SessionInfo info;
    [[maybe_unused]] auto& [infoName, attached, panes, jobs] = info;

    const Json::Value options = toJson(SessionOptions{});
    EXPECT_EQ(keys(options), (std::vector<std::string>{"name", "argv", "cwd", "environment", "launchedProgram", "umask", "cols", "rows",
                                                       "cellWidth", "cellHeight", "terminal", "notificationCapabilities"}));
    EXPECT_EQ(keys(options["terminal"]),
              (std::vector<std::string>{"scrollback", "foreground", "background", "palette", "cursorColor", "cursorTextColor",
                                        "selectionForeground", "selectionBackground", "boldIsBright", "minimumContrast", "wordBoundaries",
                                        "ambiguousWide", "bidi", "cursorShape", "cursorBlink", "allowScreenReadback", "graphicsQuota",
                                        "graphicsFileTransfer"}));
    EXPECT_EQ(keys(toJson(SessionInfo{})), (std::vector<std::string>{"name", "attached", "panes", "jobs"}));
}

TEST(MuxProtocol, SessionOptionsRoundTrip)
{
    // Every value differs from its default, so that a field that the wire
    // drops fails the comparison.
    SessionOptions o;
    o.name = "work";
    o.launch.argv = {"/bin/zsh", "-l"};
    o.launch.workingDirectory = "/tmp";
    o.launch.environment = {{"TERM", "afterglow"}, {"EMPTY", ""}};
    o.launch.launchedProgram = "htop";
    o.launch.umask = 027;
    o.size = {132, 43, 9, 18};
    o.notificationCapabilities = "a=focus:p=title";
    AfterglowVt::TerminalOptions& t = o.terminal;
    t.maxScrollback = 1234;
    t.defaultForeground = {4, 5, 6};
    t.defaultBackground = {1, 2, 3};
    t.basePalette[15] = {250, 251, 252};
    t.cursorColor = AfterglowVt::Rgb{9, 8, 7};
    t.cursorTextColor = AfterglowVt::Rgb{7, 8, 9};
    t.selectionForeground = AfterglowVt::Rgb{10, 11, 12};
    t.selectionBackground = AfterglowVt::Rgb{13, 14, 15};
    t.boldIsBright = true;
    t.minimumContrast = 4.5;
    t.wordBoundaries = U"│ ,";
    t.ambiguousWide = true;
    t.bidi = AfterglowVt::BidiMode::RightToLeft;
    t.cursorShape = AfterglowVt::CursorShape::Bar;
    t.cursorBlink = false;
    t.allowScreenReadback = true;
    t.graphicsQuota = std::size_t(64) << 20;
    t.graphicsFileTransfer = false;
    EXPECT_FALSE(t == AfterglowVt::TerminalOptions{});

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
    EXPECT_EQ(back->launch.launchedProgram, o.launch.launchedProgram);
    EXPECT_EQ(back->launch.umask, 027);
    EXPECT_EQ(back->size.cols, 132);
    EXPECT_EQ(back->size.rows, 43);
    EXPECT_EQ(back->size.cellWidth, 9);
    EXPECT_EQ(back->size.cellHeight, 18);
    EXPECT_EQ(back->notificationCapabilities, o.notificationCapabilities);
    EXPECT_TRUE(back->terminal == o.terminal);
    // Unset colors go as null and come back unset.
    SessionOptions unset;
    unset.terminal.cursorColor.reset();
    unset.terminal.selectionBackground.reset();
    const auto plain = sessionOptions(toJson(unset), &error);
    ASSERT_TRUE(plain) << error;
    EXPECT_TRUE(plain->terminal == unset.terminal);
}

TEST(MuxProtocol, SessionOptionsRefuseBadValues)
{
    const Json::Value good = toJson(SessionOptions{});
    std::string error;
    ASSERT_TRUE(sessionOptions(good, &error)) << error;
    const std::pair<const char*, Json::Value> bad[] = {
        {"name", "a b"},
        {"name", "-a"},
        {"name", std::string(MaxSessionName + 1, 'a')},
        {"name", 1},
        {"argv", Json::Array{1}},
        {"argv", "sh"},
        {"cwd", Json::Array{}},
        {"environment", Json::Array{Json::Array{"A=B", "c"}}},
        {"environment", Json::Array{Json::Array{"", "c"}}},
        {"environment", Json::Array{Json::Array{"A"}}},
        {"environment", Json::Array{Json::Array{std::string("A\0B", 3), "c"}}},
        {"environment", Json::Array{Json::Array{"A", 1}}},
        {"environment", Json::Array{"A=B"}},
        {"launchedProgram", false},
        {"umask", -2},
        {"umask", 01000},
        {"cols", 1},
        {"cols", 4097},
        {"cols", 2.5},
        {"rows", 0},
        {"rows", 1048576 / 2 + 1},
        {"cellWidth", -1},
        {"cellWidth", 65536},
        {"cellHeight", -1},
        {"cellHeight", 65536},
        {"terminal", 3},
        {"notificationCapabilities", nullptr},
        // A key of another version.
        {"unset", Json::Array{}},
    };
    for (const auto& [key, value] : bad) {
        error.clear();
        EXPECT_FALSE(sessionOptions(with(good, key, value), &error)) << key << " " << Json::write(value);
        EXPECT_NE(error.find(std::string("'") + key + "'"), std::string::npos) << key << ": " << error;
    }
    for (const char* key : {"name", "cwd", "umask", "terminal", "notificationCapabilities"}) {
        error.clear();
        EXPECT_FALSE(sessionOptions(without(good, key), &error)) << key;
        EXPECT_NE(error.find(std::string("'") + key + "'"), std::string::npos) << key << ": " << error;
    }
}

TEST(MuxProtocol, TerminalValuesOutsideTheirBoundsAreRefusedByName)
{
    const Json::Value good = toJson(SessionOptions{});
    const Json::Value terminal = good["terminal"];
    Json::Array palette = terminal["palette"].toArray();
    Json::Array overRange = palette;
    // The last of 16 colors has a component past 255.
    overRange.back() = Json::Array{0, 0, 256};
    Json::Array negative = palette;
    negative.front() = Json::Array{-1, 0, 0};
    Json::Array shortColor = palette;
    shortColor[3] = Json::Array{1, 2};
    const std::pair<const char*, Json::Value> bad[] = {
        {"scrollback", -1},
        {"scrollback", MaxScrollback + 1},
        {"foreground", Json::Array{1, 2, 3, 4}},
        {"background", nullptr},
        {"palette", Json::Array{Json::Array{0, 0, 0}}},
        {"palette", std::move(overRange)},
        {"palette", std::move(negative)},
        {"palette", std::move(shortColor)},
        {"cursorColor", Json::Array{0, 0, 300}},
        {"cursorTextColor", "red"},
        {"selectionForeground", Json::Array{}},
        {"selectionBackground", 1},
        {"boldIsBright", 1},
        {"minimumContrast", 0.5},
        {"minimumContrast", 21.5},
        {"wordBoundaries", 3},
        {"wordBoundaries", std::string("\xff")},
        {"ambiguousWide", "yes"},
        {"bidi", -1},
        {"bidi", int(AfterglowVt::BidiMode::RightToLeft) + 1},
        {"cursorShape", int(AfterglowVt::CursorShape::Bar) + 1},
        {"cursorBlink", nullptr},
        {"allowScreenReadback", 0},
        {"graphicsQuota", -1},
        {"graphicsQuota", MaxGraphicsQuota + 1},
        {"graphicsFileTransfer", "no"},
        {"unknown", 1},
    };
    std::string error;
    for (const auto& [key, value] : bad) {
        error.clear();
        EXPECT_FALSE(sessionOptions(with(good, "terminal", with(terminal, key, value)), &error)) << key;
        // The error names the key inside the terminal object.
        EXPECT_NE(error.find(std::string("'terminal.") + key + "'"), std::string::npos) << key << ": " << error;
    }
    for (const char* key : {"scrollback", "palette", "cursorColor", "allowScreenReadback"}) {
        error.clear();
        EXPECT_FALSE(sessionOptions(with(good, "terminal", without(terminal, key)), &error)) << key;
        EXPECT_NE(error.find(std::string("'terminal.") + key + "'"), std::string::npos) << key << ": " << error;
    }
    // The bounds themselves pass.
    for (const auto& [key, value] : {std::pair<const char*, Json::Value>{"scrollback", MaxScrollback}, {"minimumContrast", MinContrast},
                                     {"minimumContrast", MaxContrast}, {"graphicsQuota", MaxGraphicsQuota}}) {
        error.clear();
        EXPECT_TRUE(sessionOptions(with(good, "terminal", with(terminal, key, value)), &error)) << key << ": " << error;
    }
}

TEST(MuxProtocol, TheFirstErrorIsTheOneReported)
{
    const Json::Value good = toJson(SessionOptions{});
    Json::Value bad = with(good, "cols", 1);
    bad = with(bad, "terminal", with(good["terminal"], "scrollback", -1));
    std::string error;
    EXPECT_FALSE(sessionOptions(bad, &error));
    EXPECT_NE(error.find("'cols'"), std::string::npos) << error;
}

TEST(MuxProtocol, SessionInfoRoundTrip)
{
    SessionInfo info{"2", true, 3, {"vim", "make"}};
    const auto back = sessionInfo(toJson(info));
    ASSERT_TRUE(back);
    EXPECT_EQ(back->name, "2");
    EXPECT_TRUE(back->attached);
    EXPECT_EQ(back->panes, 3);
    EXPECT_EQ(back->jobs, info.jobs);
    EXPECT_FALSE(sessionInfo(Json::Object{{"name", "x"}}));
    EXPECT_FALSE(sessionInfo(with(toJson(info), "panes", -1)));
    EXPECT_FALSE(sessionInfo(with(toJson(info), "jobs", Json::Array{1})));
    // A key of another version.
    EXPECT_FALSE(sessionInfo(with(toJson(info), "title", "x")));
    // A key that version 2 had.
    EXPECT_FALSE(sessionInfo(with(toJson(info), "windows", 1)));
}
