// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxHost.h"
#include "MuxProtocol.h"
#include "MuxStream.h"

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
    ASSERT_EQ(ProtocolVersion, 5) << "a new protocol version: update the key lists and the calls of this test";
    // Version 5 added the host calls of a pane's terminal (Host events) and
    // the eventQueue of the attach call.
    Attach::Event bell;
    bell.type = Attach::EventType::Host;
    bell.pane = 1;
    bell.payload = Host::encode(Host::make(Host::Kind::Bell));
    EXPECT_EQ(Attach::encode(bell), std::string("\x06\x01\x00\x00\x01", 5));
    // Version 4 added attach and detach, and the attach stream: its events
    // and input, byte for byte (the keys of the attach call are in
    // Mux.MuxAttach.TheAttachCallAnswersWithItsKeys).
    EXPECT_EQ(std::string(Fn::Attach), "attach");
    EXPECT_EQ(std::string(Fn::Detach), "detach");
    Attach::Event opened;
    opened.pane = 300;
    opened.channel = 2;
    EXPECT_EQ(Attach::encode(opened), std::string("\x01\xac\x02\x02\x00", 5));
    Attach::Input resize;
    resize.type = Attach::InputType::Resize;
    resize.pane = 1;
    resize.size = {80, 24, 8, 16};
    EXPECT_EQ(Attach::encode(resize), std::string("\x02\x01\x50\x18\x08\x10", 6));
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

TEST(MuxProtocol, HostCallsRoundTrip)
{
    std::vector<Host::Call> calls;
    calls.push_back(Host::make(Host::Kind::Bell));
    calls.push_back(Host::make(Host::Kind::Flash));
    calls.push_back(Host::make(Host::Kind::Notify, "title", "body\nline"));
    calls.push_back(Host::make(Host::Kind::ClipboardWrite, "cp", std::string("data\0with nul", 14)));
    calls.push_back(Host::make(Host::Kind::TerminalProtocol, {}, "ac=send;id=x", 5113));
    calls.push_back(Host::make(Host::Kind::PointerShape, {}, "pointer"));
    calls.push_back(Host::make(Host::Kind::UserVar, "name", "value"));
    calls.push_back(Host::make(Host::Kind::CommandStarted, {}, "make -j4"));
    calls.push_back(Host::make(Host::Kind::CommandFinished, {}, {}, -1));
    calls.push_back(Host::make(Host::Kind::CommandFinished, {}, {}, 255));
    calls.push_back(Host::make(Host::Kind::Download, "file.txt", "contents"));
    Host::Call notification = Host::make(Host::Kind::DesktopNotification);
    notification.notification.kind = AfterglowVt::NotificationRequest::Kind::Close;
    notification.notification.id = "n1";
    notification.notification.title = "t";
    notification.notification.body = "b";
    notification.notification.buttons = {"yes", "no"};
    notification.notification.iconNames = {"dialog-information"};
    notification.notification.iconData = std::string("\x89PNG", 4);
    notification.notification.iconCacheId = "cache";
    notification.notification.appName = "app";
    notification.notification.types = {"im"};
    notification.notification.sound = "silent";
    notification.notification.focusOnActivate = false;
    notification.notification.reportActivation = true;
    notification.notification.reportClose = true;
    notification.notification.occasion = AfterglowVt::NotificationRequest::Occasion::Invisible;
    notification.notification.urgency = 2;
    notification.notification.expireMs = 0;
    calls.push_back(notification);
    Host::Call clipboard = Host::make(Host::Kind::ClipboardRequest);
    clipboard.clipboard.kind = AfterglowVt::ClipboardRequest::Kind::Write;
    clipboard.clipboard.primary = true;
    clipboard.clipboard.id = "c1";
    clipboard.clipboard.password = "pw";
    clipboard.clipboard.name = "editor";
    clipboard.clipboard.mimes = {"text/plain", "image/png"};
    clipboard.clipboard.data = {{"text/plain", "hello"}};
    clipboard.clipboard.aliases = {{"text/html", "text/plain"}};
    clipboard.clipboard.osc52 = true;
    clipboard.clipboard.osc52Bel = true;
    clipboard.clipboard.osc52Selection = "cp";
    calls.push_back(clipboard);
    for (const Host::Call& call : calls) {
        const std::string bytes = Host::encode(call);
        const auto back = Host::decode(bytes);
        ASSERT_TRUE(back) << int(call.kind);
        EXPECT_EQ(back->kind, call.kind);
        EXPECT_EQ(back->title, call.title);
        EXPECT_EQ(back->text, call.text);
        EXPECT_EQ(back->number, call.number);
        EXPECT_TRUE(Host::encode(*back) == bytes) << int(call.kind);
        // Cut short anywhere, a call does not decode.
        for (std::size_t n = 0; n < bytes.size(); ++n) EXPECT_FALSE(Host::decode(bytes.substr(0, n))) << int(call.kind) << " " << n;
        EXPECT_FALSE(Host::decode(bytes + "x")) << int(call.kind);
    }
    EXPECT_TRUE(Host::decode(Host::encode(notification))->notification.buttons == notification.notification.buttons);
    EXPECT_TRUE(Host::decode(Host::encode(clipboard))->clipboard == clipboard.clipboard);
    for (const char* bad : {"", "\x00", "\x0d", "\x04\x07"}) EXPECT_FALSE(Host::decode(bad)) << bad;
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

TEST(MuxProtocol, AttachEventsRoundTrip)
{
    for (const int status : {-1, 0, 3 << 8, 0x7fffffff}) {
        Attach::Event closed;
        closed.type = Attach::EventType::PaneClosed;
        closed.pane = 7;
        closed.status = status;
        const auto back = Attach::decodeEvent(Attach::encode(closed));
        ASSERT_TRUE(back) << status;
        EXPECT_EQ(back->type, Attach::EventType::PaneClosed);
        EXPECT_EQ(back->pane, 7u);
        // -1 is the status that is unknown, and stays so.
        EXPECT_EQ(back->status, status) << status;
    }
    for (const auto type : {Attach::EventType::SessionEnded, Attach::EventType::TakenOver, Attach::EventType::Detached}) {
        Attach::Event event;
        event.type = type;
        const auto back = Attach::decodeEvent(Attach::encode(event));
        ASSERT_TRUE(back);
        EXPECT_EQ(back->type, type);
    }
}

TEST(MuxProtocol, AttachEventsThatDoNotDecodeAreRefused)
{
    const std::string_view bad[] = {
        {},                         // empty
        std::string_view("\x09\x01\x00\x00", 4), // an unknown type
        std::string_view("\x00\x01\x00\x00", 4), // type 0
        std::string_view("\x02\x01\x00\x00\x00", 5), // a byte after the event
        std::string_view("\x02\x81", 2),         // a varint that does not end
        std::string_view("\x02\x81\x00\x00\x00", 5), // a varint in a longer form than needed
        std::string_view("\x02\xff\xff\xff\xff\x10\x00\x00", 8), // a pane id past 32 bits
        std::string_view("\x01\x00\x01\x00", 4), // PaneOpened of pane 0
        std::string_view("\x01\x01\x00\x00", 4), // PaneOpened on the channel of the events
        std::string_view("\x01\x01\x80\x20\x00", 5), // PaneOpened on channel 4096
    };
    for (const std::string_view frame : bad) EXPECT_FALSE(Attach::decodeEvent(frame)) << testing::PrintToString(std::string(frame));
}

TEST(MuxProtocol, AttachInputRoundTripsAndClampsOnlyTheCellSize)
{
    Attach::Input bytes;
    bytes.pane = 2;
    bytes.bytes = std::string("a\0b", 3);
    const auto back = Attach::decodeInput(Attach::encode(bytes));
    ASSERT_TRUE(back);
    EXPECT_EQ(back->type, Attach::InputType::Bytes);
    EXPECT_EQ(back->bytes, bytes.bytes);
    for (const auto type : {Attach::InputType::Reset, Attach::InputType::Clear, Attach::InputType::Kill}) {
        Attach::Input action;
        action.type = type;
        action.pane = 2;
        const auto decoded = Attach::decodeInput(Attach::encode(action));
        ASSERT_TRUE(decoded);
        EXPECT_EQ(decoded->type, type);
        EXPECT_EQ(decoded->pane, 2u);
    }
    // A size past what a terminal takes still decodes: the terminal clamps
    // it, and the window does not lose its session for it.
    Attach::Input resize;
    resize.type = Attach::InputType::Resize;
    resize.pane = 1;
    resize.size = {100000, 0, 70000, 1};
    const auto size = Attach::decodeInput(Attach::encode(resize));
    ASSERT_TRUE(size);
    EXPECT_EQ(size->size.cols, 100000);
    EXPECT_EQ(size->size.rows, 0);
    EXPECT_EQ(size->size.cellWidth, 65535);
}

TEST(MuxProtocol, AttachInputThatDoesNotDecodeIsRefused)
{
    const std::string_view bad[] = {
        {},
        std::string_view("\x09\x01", 2),         // an unknown type
        std::string_view("\x01", 1),             // no pane
        std::string_view("\x02\x01\x50\x18\x08", 5), // a Resize that ends early
        std::string_view("\x02\x01\x50\x18\x08\x10\x00", 7), // a byte after the Resize
        std::string_view("\x03\x01\x00", 3),     // a byte after a Reset
        std::string_view("\x02\x01\x80\x00\x18\x08\x10", 7), // a varint in a longer form than needed
    };
    for (const std::string_view frame : bad) EXPECT_FALSE(Attach::decodeInput(frame)) << testing::PrintToString(std::string(frame));
}
