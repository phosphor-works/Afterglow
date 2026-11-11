// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxHost.h"
#include "MuxProtocol.h"
#include "MuxStream.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

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
    ASSERT_EQ(ProtocolVersion, 6) << "a new protocol version: update the key lists and the calls of this test";
    // Version 6 added the visibility of a pane in the window's input, the
    // paneDirectory call, the history option of the attach call, and the
    // shell marks, status, contexts, extra cursors, region scrolls, layout
    // epochs and first visible rows of the state codec (StateSyncTest).
    EXPECT_EQ(std::string(Fn::PaneDirectory), "paneDirectory");
    EXPECT_EQ(std::string(Error::NoSuchPane), "NO_SUCH_PANE");
    Attach::Input visible;
    visible.type = Attach::InputType::Visible;
    visible.pane = 3;
    visible.visible = true;
    EXPECT_EQ(Attach::encode(visible), std::string("\x06\x03\x01", 3));
    // Version 5 added the host calls of a pane's terminal (Host events, whole
    // or in parts), each layout byte for byte, and the eventQueue of the
    // attach call.
    Attach::Event bell;
    bell.type = Attach::EventType::Host;
    bell.pane = 1;
    bell.payload = Host::encode(Host::make(Host::Kind::Bell));
    EXPECT_EQ(Attach::encode(bell), std::string("\x06\x01\x00\x00\x01", 5));
    bell.status = Attach::HostPart;
    EXPECT_EQ(Attach::encode(bell), std::string("\x06\x01\x00\x02\x01", 5));
    EXPECT_EQ(Attach::HostPartBytes, std::size_t(1) << 20);
    // The number of each kind.
    for (const auto& [kind, number] : std::vector<std::pair<Host::Kind, int>>{
             {Host::Kind::Bell, 1}, {Host::Kind::Flash, 2}, {Host::Kind::Notify, 3}, {Host::Kind::DesktopNotification, 4},
             {Host::Kind::ClipboardWrite, 5}, {Host::Kind::ClipboardRequest, 6}, {Host::Kind::TerminalProtocol, 7},
             {Host::Kind::PointerShape, 8}, {Host::Kind::UserVar, 9}, {Host::Kind::CommandStarted, 10},
             {Host::Kind::CommandFinished, 11}, {Host::Kind::Download, 12}, {Host::Kind::Report, 13}, {Host::Kind::ProtocolLost, 14}}) {
        EXPECT_EQ(int(kind), number);
    }
    EXPECT_EQ(Host::encode(Host::make(Host::Kind::Notify, "t", "b")), std::string("\x03\x01t\x01" "b"));
    EXPECT_EQ(Host::encode(Host::make(Host::Kind::UserVar, "n", "v")), std::string("\x09\x01n\x01v"));
    EXPECT_EQ(Host::encode(Host::make(Host::Kind::TerminalProtocol, {}, "x", 5113)), std::string("\x07\xf2\x4f\x01x"));
    EXPECT_EQ(Host::encode(Host::make(Host::Kind::Report, {}, "\x1b[?62c")), std::string("\x0d\x06\x1b[?62c"));
    Host::Call finished = Host::make(Host::Kind::CommandFinished, {}, {}, -1);
    finished.value = 1500;
    EXPECT_EQ(Host::encode(finished), std::string("\x0b\x01\xdc\x0b"));
    Host::Call lost = Host::make(Host::Kind::ProtocolLost, {}, {}, Host::LostPackets);
    lost.value = 1;
    EXPECT_EQ(Host::encode(lost), std::string("\x0e\x04\x01"));
    Host::Call alive = Host::make(Host::Kind::DesktopNotification);
    alive.notification.kind = AfterglowVt::NotificationRequest::Kind::Alive;
    alive.notification.id = "a";
    EXPECT_EQ(Host::encode(alive), std::string("\x04\x02\x01" "a" "\x00\x00\x00\x00\x00\x00\x00\x00\x06system\x01\x00\x00\x00\x02\x01", 25));
    Host::Call read = Host::make(Host::Kind::ClipboardRequest);
    read.clipboard.id = "r";
    read.clipboard.mimes = {"text/plain"};
    EXPECT_EQ(Host::encode(read), std::string("\x06\x00\x00\x01r\x00\x00\x01\x0atext/plain\x00\x00\x00\x00\x00", 24));
    // The calls name every field of what they carry: a new field stops this
    // test compiling until MuxHost.cpp carries it and the version changes.
    Host::Call call;
    [[maybe_unused]] auto& [callKind, title, text, number, value, notification, clipboard] = call;
    [[maybe_unused]] auto& [nKind, nId, nTitle, nBody, buttons, iconNames, iconData, iconCacheId, appName, types, sound, focusOnActivate,
                            reportActivation, reportClose, occasion, urgency, expireMs] = call.notification;
    [[maybe_unused]] auto& [cKind, primary, cId, password, cName, mimes, data, aliases, osc52, osc52Bel, osc52Selection] = call.clipboard;
    // The options of the attach call, which the daemon refuses others of.
    EXPECT_EQ(std::string(AttachOption::Takeover), "takeover");
    EXPECT_EQ(std::string(AttachOption::EventQueue), "eventQueue");
    EXPECT_EQ(std::string(AttachOption::History), "history");
    // Version 4 added attach and detach, and the attach stream: its events
    // and input, byte for byte (the keys of the answer of the attach call are
    // in Mux.MuxAttach.TheAttachCallAnswersWithItsKeys).
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

namespace {

/// Every field of a notification, to compare two.
auto fieldsOf(const AfterglowVt::NotificationRequest& n)
{
    return std::tie(n.kind, n.id, n.title, n.body, n.buttons, n.iconNames, n.iconData, n.iconCacheId, n.appName, n.types, n.sound,
                    n.focusOnActivate, n.reportActivation, n.reportClose, n.occasion, n.urgency, n.expireMs);
}

} // namespace

TEST(MuxProtocol, HostCallsRoundTrip)
{
    std::vector<Host::Call> calls;
    calls.push_back(Host::make(Host::Kind::Bell));
    calls.push_back(Host::make(Host::Kind::Flash));
    calls.push_back(Host::make(Host::Kind::Notify, "title", "body \xe2\x80\xa8 line"));
    calls.push_back(Host::make(Host::Kind::ClipboardWrite, "cp", std::string("data\0with nul", 13)));
    calls.push_back(Host::make(Host::Kind::TerminalProtocol, {}, "ac=send;id=x", 5113));
    calls.push_back(Host::make(Host::Kind::TerminalProtocol, {}, "t=q", 72));
    calls.push_back(Host::make(Host::Kind::TerminalProtocol, {}, {}, 0));
    calls.push_back(Host::make(Host::Kind::PointerShape, {}, "pointer"));
    calls.push_back(Host::make(Host::Kind::UserVar, "name", "value"));
    calls.push_back(Host::make(Host::Kind::CommandStarted, {}, "make -j4"));
    for (const int code : {-1, 255, std::numeric_limits<int>::min(), std::numeric_limits<int>::max()}) {
        Host::Call finished = Host::make(Host::Kind::CommandFinished, {}, {}, code);
        finished.value = std::uint64_t(code < 0 ? 0 : code) * 1000;
        calls.push_back(finished);
    }
    calls.push_back(Host::make(Host::Kind::Download, "file.txt", "contents"));
    calls.push_back(Host::make(Host::Kind::Report, {}, "\x1b[?62;22c"));
    for (const int held : {Host::LostNothing, Host::LostReset, Host::LostPackets}) {
        Host::Call lost = Host::make(Host::Kind::ProtocolLost, {}, {}, held);
        lost.value = held == Host::LostReset ? 1 : 0;
        calls.push_back(lost);
    }
    Host::Call notification = Host::make(Host::Kind::DesktopNotification);
    notification.notification.kind = AfterglowVt::NotificationRequest::Kind::Close;
    notification.notification.id = "n1";
    notification.notification.title = "t";
    notification.notification.body = "b\nline";
    notification.notification.buttons = {"yes", "no"};
    notification.notification.iconNames = {"dialog-information"};
    notification.notification.iconData = std::string("\x89PNG", 4);
    notification.notification.iconCacheId = "cache";
    notification.notification.appName = "app";
    notification.notification.types = {"im"};
    notification.notification.sound = "silent";
    // Two flags that trade places fail in this row or in the next.
    notification.notification.focusOnActivate = false;
    notification.notification.reportActivation = true;
    notification.notification.reportClose = false;
    notification.notification.occasion = AfterglowVt::NotificationRequest::Occasion::Invisible;
    notification.notification.urgency = 2;
    notification.notification.expireMs = 0;
    calls.push_back(notification);
    // The same flags the other way: two that trade places fail in one row.
    Host::Call flags = notification;
    flags.notification.focusOnActivate = true;
    flags.notification.reportActivation = false;
    flags.notification.reportClose = true;
    calls.push_back(flags);
    Host::Call clipboard = Host::make(Host::Kind::ClipboardRequest);
    clipboard.clipboard.kind = AfterglowVt::ClipboardRequest::Kind::Write;
    clipboard.clipboard.primary = true;
    clipboard.clipboard.id = "c1";
    clipboard.clipboard.password = "pw";
    clipboard.clipboard.name = "editor";
    clipboard.clipboard.mimes = {"text/plain", "image/png"};
    clipboard.clipboard.data = {{"text/plain", "hello"}};
    clipboard.clipboard.aliases = {{"text/html", "text/plain"}};
    calls.push_back(clipboard);
    Host::Call nonPrimary = clipboard;
    nonPrimary.clipboard.primary = false;
    calls.push_back(nonPrimary);
    Host::Call osc52 = Host::make(Host::Kind::ClipboardRequest);
    osc52.clipboard.mimes = {"text/plain"};
    osc52.clipboard.osc52 = true;
    osc52.clipboard.osc52Bel = true;
    osc52.clipboard.osc52Selection = "cp";
    calls.push_back(osc52);
    for (const Host::Call& call : calls) {
        const std::string bytes = Host::encode(call);
        const auto back = Host::decode(bytes);
        if (!back) {
            ADD_FAILURE() << "kind " << int(call.kind) << " does not decode";
            continue;
        }
        EXPECT_EQ(back->kind, call.kind);
        EXPECT_EQ(back->title, call.title);
        EXPECT_EQ(back->text, call.text);
        EXPECT_EQ(back->number, call.number);
        EXPECT_EQ(back->value, call.value);
        EXPECT_TRUE(fieldsOf(back->notification) == fieldsOf(call.notification)) << int(call.kind);
        EXPECT_TRUE(back->clipboard == call.clipboard) << int(call.kind);
        EXPECT_TRUE(Host::encode(*back) == bytes) << int(call.kind);
        // Cut short anywhere, a call does not decode.
        for (std::size_t n = 0; n < bytes.size(); ++n) EXPECT_FALSE(Host::decode(bytes.substr(0, n))) << int(call.kind) << " " << n;
        EXPECT_FALSE(Host::decode(bytes + "x")) << int(call.kind);
    }
}

// The window answers the program with what it decodes: what the terminal's
// parser never makes is refused, whole calls and not only short ones.
TEST(MuxProtocol, HostCallsThatTheParserNeverMakesAreRefused)
{
    for (const std::string_view kind : {std::string_view("", 0), std::string_view("\x00", 1), std::string_view("\x0f", 1)}) {
        EXPECT_FALSE(Host::decode(kind)) << testing::PrintToString(std::string(kind));
    }
    const auto refused = [](const Host::Call& call) { return !Host::decode(Host::encode(call)); };
    EXPECT_TRUE(refused(Host::make(Host::Kind::TerminalProtocol, {}, "x", 99)));
    EXPECT_TRUE(refused(Host::make(Host::Kind::Notify, "a\x1b]0;b", "body")));
    EXPECT_TRUE(refused(Host::make(Host::Kind::Notify, "title", "line\nline")));
    EXPECT_TRUE(refused(Host::make(Host::Kind::ProtocolLost, {}, {}, 3)));
    Host::Call reset = Host::make(Host::Kind::ProtocolLost);
    reset.value = 2;
    EXPECT_TRUE(refused(reset));

    Host::Call good = Host::make(Host::Kind::DesktopNotification);
    good.notification.id = "n";
    good.notification.buttons = {"ok"};
    ASSERT_FALSE(refused(good));
    const auto with = [&good](auto change) {
        Host::Call call = good;
        change(call.notification);
        return call;
    };
    using Request = AfterglowVt::NotificationRequest;
    EXPECT_TRUE(refused(with([](Request& n) { n.kind = Request::Kind(3); })));
    EXPECT_TRUE(refused(with([](Request& n) { n.occasion = Request::Occasion(3); })));
    EXPECT_TRUE(refused(with([](Request& n) { n.urgency = 3; })));
    EXPECT_TRUE(refused(with([](Request& n) { n.id = "a b"; })));
    EXPECT_TRUE(refused(with([](Request& n) { n.iconCacheId = "a;b"; })));
    EXPECT_TRUE(refused(with([](Request& n) { n.title = "\x07"; })));
    EXPECT_TRUE(refused(with([](Request& n) { n.buttons = {""}; })));
    EXPECT_TRUE(refused(with([](Request& n) { n.iconNames.assign(Request::MaxNames + 1, "icon"); })));
    EXPECT_TRUE(refused(with([](Request& n) { n.body.assign(Request::MaxText + 1, 'x'); })));
    // A flag of 2.
    std::string flag = Host::encode(good);
    flag[flag.size() - 6] = 2;
    EXPECT_FALSE(Host::decode(flag));

    Host::Call request = Host::make(Host::Kind::ClipboardRequest);
    request.clipboard.id = "c";
    ASSERT_FALSE(refused(request));
    request.clipboard.kind = AfterglowVt::ClipboardRequest::Kind(2);
    EXPECT_TRUE(refused(request));
    request.clipboard.kind = AfterglowVt::ClipboardRequest::Kind::Read;
    request.clipboard.id = "c d";
    EXPECT_TRUE(refused(request));
    request.clipboard.id = "c";
    request.clipboard.mimes.assign(AfterglowVt::ClipboardAssembler::MaxParts + 1, "text/plain");
    EXPECT_TRUE(refused(request));
    request.clipboard.mimes = {"text/plain"};
    // OSC 52: reads of the selections that the answer echoes.
    request.clipboard.osc52 = true;
    request.clipboard.osc52Selection = "c";
    ASSERT_FALSE(refused(request));
    for (const char* selection : {"", "c;x", "\x1b"}) {
        request.clipboard.osc52Selection = selection;
        EXPECT_TRUE(refused(request)) << selection;
    }
    request.clipboard.osc52Selection = "c";
    request.clipboard.kind = AfterglowVt::ClipboardRequest::Kind::Write;
    EXPECT_TRUE(refused(request));
    // OSC 5522 has none of the fields of OSC 52.
    request.clipboard.kind = AfterglowVt::ClipboardRequest::Kind::Read;
    request.clipboard.osc52 = false;
    EXPECT_TRUE(refused(request));
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
    // A host call takes the rest of the frame, whatever its bytes look like.
    for (const int status : {-1, Attach::HostPart}) {
        for (const std::string& payload : {std::string(), Host::encode(Host::make(Host::Kind::Notify, "t", "b")), std::string("\x80\x00\xff", 3)}) {
            Attach::Event host;
            host.type = Attach::EventType::Host;
            host.pane = 3;
            host.status = status;
            host.payload = payload;
            const auto back = Attach::decodeEvent(Attach::encode(host));
            ASSERT_TRUE(back) << status;
            EXPECT_EQ(back->type, Attach::EventType::Host);
            EXPECT_EQ(back->pane, 3u);
            EXPECT_EQ(back->status, status);
            EXPECT_TRUE(back->payload == payload);
        }
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
        std::string_view("\x06\x00\x00\x00\x01", 5), // Host of pane 0
        std::string_view("\x06\x01\x01\x00\x01", 5), // Host on a channel
        std::string_view("\x06\x01\x00\x01\x01", 5), // Host with status 0
        std::string_view("\x06\x01\x00\x04\x01", 5), // Host with status 3
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
    // The value follows a pane of one varint byte, and one of two.
    for (const std::uint32_t pane : {2u, 300u}) {
        for (const bool visible : {false, true}) {
            Attach::Input input;
            input.type = Attach::InputType::Visible;
            input.pane = pane;
            input.visible = visible;
            const auto decoded = Attach::decodeInput(Attach::encode(input));
            ASSERT_TRUE(decoded);
            EXPECT_EQ(decoded->type, Attach::InputType::Visible);
            EXPECT_EQ(decoded->pane, pane);
            EXPECT_EQ(decoded->visible, visible);
        }
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
        std::string_view("\x07\x01", 2),         // the type after Visible
        std::string_view("\x01", 1),             // no pane
        std::string_view("\x02\x01\x50\x18\x08", 5), // a Resize that ends early
        std::string_view("\x02\x01\x50\x18\x08\x10\x00", 7), // a byte after the Resize
        std::string_view("\x03\x01\x00", 3),     // a byte after a Reset
        std::string_view("\x06\x01", 2),         // a Visible without its value
        std::string_view("\x06\x01\x02", 3),     // a Visible that is neither
        std::string_view("\x06\x01\x01\x00", 4), // a byte after a Visible
        std::string_view("\x02\x01\x80\x00\x18\x08\x10", 7), // a varint in a longer form than needed
    };
    for (const std::string_view frame : bad) EXPECT_FALSE(Attach::decodeInput(frame)) << testing::PrintToString(std::string(frame));
}
