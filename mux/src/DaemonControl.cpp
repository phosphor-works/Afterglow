// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

// The control target of the daemon: `afterglow ctl --session NAME`, and
// `afterglow ctl` in a session pane. The calls and their values are those of
// the control target of a window (ControlSocket in the application), so that
// one client speaks both. Only the daemon gives the layout call, the options
// prompt and enter of paste, and the option endSession of close.
//
// The daemon answers the calls on panes, text, keys, colours, user
// variables, signals and layout itself, with its own pane IDs, also while
// no window shows the session; a window that shows it moves the panes that
// the daemon places, and finds the neighbors of a pane in its own sizes.
// The calls that only a window can answer (the titles of panes and tabs,
// shader signals, notifications, actions, windows, directories, projects,
// script diagnostics, overlays, screenshots, pane sizes, scrolling, the font
// size, the opacity and the window's state) go to that window, whose answer
// the daemon passes back. A tab's title and a pane's size need the window
// too: it cleans the title as it shows it, and measures the panes.

#include "Daemon.h"

#include <AfterglowProto/Color.h>
#include <AfterglowPty/Pty.h>
#include <AfterglowVt/Input.h>
#include <AfterglowVt/KeyNames.h>
#include <AfterglowVt/Terminal.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <exception>
#include <limits>
#include <optional>
#include <set>
#include <string_view>

namespace AfterglowMux {

namespace Json = AfterglowProto::Json;
using AfterglowProto::ipcError;
using AfterglowProto::ipcReply;
using AfterglowProto::IpcRequest;
using AfterglowProto::IpcServerConnection;
namespace IpcErrorCode = AfterglowProto::IpcErrorCode;

namespace {

/// Takes back what the shell integration of the window put in the variables
/// of @p launch (ShellIntegration::prepare): a command such as `bash` gets
/// the options of the integration in its arguments, and a login shell that
/// runs without them must not get ENV or ZDOTDIR alone. A saved
/// AFTERGLOW_ORIGINAL_NAME_SET of 1 gives NAME its value back, and 0 removes
/// it.
void dropShellIntegration(Launch& launch)
{
    static constexpr std::string_view Prefix = "AFTERGLOW_ORIGINAL_";
    constexpr std::string_view SetSuffix = "_SET";
    auto& environment = launch.environment;
    const auto valueOf = [&environment](const std::string& name) -> const std::string* {
        for (const auto& [key, value] : environment) {
            if (key == name)
                return &value;
        }
        return nullptr;
    };
    std::vector<std::pair<std::string, std::optional<std::string>>> restored;
    for (const auto& [key, value] : environment) {
        if (!key.starts_with(Prefix) || !key.ends_with(SetSuffix) || key.size() <= Prefix.size() + SetSuffix.size())
            continue;
        std::string name = key.substr(Prefix.size(), key.size() - Prefix.size() - SetSuffix.size());
        const std::string* original = value == "1" ? valueOf(std::string(Prefix) + name) : nullptr;
        restored.emplace_back(std::move(name), original ? std::optional<std::string>(*original) : std::nullopt);
    }
    // And the variables that only the integration's scripts read.
    std::erase_if(environment, [](const auto& entry) {
        return entry.first.starts_with(Prefix) || entry.first == "AFTERGLOW_BASH_INJECT"
            || entry.first == "AFTERGLOW_SHELL_FEATURES" || entry.first == "AFTERGLOW_PROMPT_CURSOR";
    });
    for (auto& [name, original] : restored) {
        std::erase_if(environment, [&name](const auto& entry) {
            return entry.first == name;
        });
        if (original)
            environment.emplace_back(name, std::move(*original));
    }
}

bool among(std::string_view fn, std::initializer_list<std::string_view> names)
{
    return std::find(names.begin(), names.end(), fn) != names.end();
}

/// The calls whose first argument is a pane.
bool takesPane(std::string_view fn)
{
    return among(fn, {"sendText",    "getText",       "split",          "newTab",        "focus",        "close",
                      "setTitle",    "signal",        "notify",         "runAction",     "movePane",     "detachTab",
                      "saveProject", "overlay",       "screenshot",     "focusNeighbor", "resizePane",   "sendKeys",
                      "scroll",      "setTabTitle",   "getColors",      "setColors",     "setFontSize",  "setOpacity",
                      "setUserVar",  "signalProgram", "setWindowState", "mark",          "shellRegions", "paste"});
}

/// The calls that only a window answers.
bool windowCall(std::string_view fn)
{
    return among(fn,
                 {"setTitle", "signal", "notify", "runAction", "newWindow", "openDirectory", "projects", "saveProject",
                  "openProject", "scriptDiagnostics", "overlay", "overlayInput", "screenshot", "resizePane", "scroll",
                  "setTabTitle", "setFontSize", "setOpacity", "setWindowState"});
}

/// The longest name of a user variable, in bytes: as OSC 1337 SetUserVar
/// names one, and a window's control target (Control::MaxUserVarName).
constexpr std::size_t MaxUserVarName = AfterglowVt::ITerminalHost::MaxUserVarName;

/// A colour as a window's control target reads it (Themes::rgbFromName).
std::optional<AfterglowVt::Rgb> colorNamed(std::string_view name)
{
    AfterglowVt::Rgb rgb;
    if (!AfterglowProto::parseSettingColor(name, rgb.r, rgb.g, rgb.b))
        return std::nullopt;
    return rgb;
}

/// The number of arguments of a call that the daemon takes only as a
/// window's control target takes it, whose router refuses another count.
std::optional<std::size_t> arity(std::string_view fn)
{
    if (fn == "listPanes" || fn == "focusedPane" || fn == "layout")
        return 0;
    if (fn == "focus" || fn == "detachTab")
        return 1;
    return std::nullopt;
}

std::optional<std::uint32_t> paneArg(const Json::Value& value)
{
    const auto id = value.toInteger();
    if (!id || *id < 0 || *id > 0xffffffffLL)
        return std::nullopt;
    return std::uint32_t(*id);
}

/// A mark of the mark call: {row, col, epoch}, whole numbers that a double
/// holds exactly. Its keys and no other.
std::optional<AfterglowVt::TextMark> markFrom(const Json::Value& value)
{
    if (!value.isObject() || value.toObject().size() != 3)
        return std::nullopt;
    const Json::Value* row = value.find("row");
    const Json::Value* col = value.find("col");
    const Json::Value* epoch = value.find("epoch");
    const auto r = row ? row->toInteger() : std::nullopt;
    const auto c = col ? col->toInteger() : std::nullopt;
    const auto e = epoch ? epoch->toInteger() : std::nullopt;
    if (!r || !c || !e || *r < 0 || *r > (1LL << 53) || *c < 0 || *c > std::numeric_limits<int>::max() || *e < 0
        || *e > (1LL << 53))
        return std::nullopt;
    return AfterglowVt::TextMark{std::uint64_t(*r), int(*c), std::uint64_t(*e)};
}

Json::Value markJson(const AfterglowVt::TextMark& mark)
{
    return Json::Object{{"row", std::int64_t(mark.row)}, {"col", mark.col}, {"epoch", std::int64_t(mark.epoch)}};
}

/// A region of shellRegions: its ID, which the after argument takes, its
/// kind, whether it is still open, its exit code (an output region that D
/// closed with one), whether its start left the scrollback, and whether its
/// text can still be read.
Json::Value regionJson(const AfterglowVt::SemanticRegion& region)
{
    return Json::Object{
        {"id", std::int64_t(region.id)},
        {"kind", std::string(ShellRegionKinds[std::size_t(region.kind)])},
        {"open", region.open},
        {"exit_code", region.exitCode ? Json::Value(*region.exitCode) : Json::Value()},
        {"start_trimmed", region.startTrimmed},
        {"available", region.available},
    };
}

std::optional<Side> sideOf(const Json::Value& value)
{
    const std::string& name = value.toString();
    if (name == "right")
        return Side::Right;
    if (name == "down")
        return Side::Down;
    if (name == "left")
        return Side::Left;
    if (name == "up")
        return Side::Up;
    return std::nullopt;
}

/// User variables can carry secrets: their values leave only with read
/// access, as those of userVarChanged.
Json::Value userVarsJson(const std::map<std::string, std::string>& vars, bool readText)
{
    Json::Object out;
    for (const auto& [name, value] : vars)
        out.emplace_back(name, readText ? value : std::string());
    return out;
}

/// The command line of the program in the foreground needs read access, as
/// a pane's command does. Its directory goes, as the pane's does.
Json::Value foregroundJson(const std::optional<AfterglowPty::ForegroundProcess>& process, bool readText)
{
    if (!process)
        return nullptr;
    Json::Array cmdline;
    if (readText)
        cmdline.assign(process->cmdline.begin(), process->cmdline.end());
    return Json::Object{{"pid", std::int64_t(process->pid)}, {"cmdline", std::move(cmdline)}, {"cwd", process->cwd}};
}

/// The neighbors of a pane of @p size in @p layout (Layout::neighbors), in
/// the pixels that a window measures them in: in cells while no window
/// measured the cells.
std::array<std::uint32_t, 4> neighborsOf(const Layout& layout, std::uint32_t pane, const PaneSize& size)
{
    const double width = double(size.cols) * (size.cellWidth > 0 ? size.cellWidth : 1);
    const double height = double(size.rows) * (size.cellHeight > 0 ? size.cellHeight : 1);
    return layout.neighbors(pane, width, height);
}

/// The pane beside @p pane on each side in the layout, 0 for none.
Json::Value neighborsJson(const Layout& layout, std::uint32_t pane, const PaneSize& size)
{
    const std::array<std::uint32_t, 4> found = neighborsOf(layout, pane, size);
    Json::Object out;
    for (const Side side : {Side::Left, Side::Right, Side::Up, Side::Down})
        out.emplace_back(sideName(side), std::int64_t(found[std::size_t(side)]));
    return out;
}

/// "no pane PHRASE the pane", as a window says it.
const char* sidePhrase(Side side)
{
    switch (side) {
    case Side::Left:
        return "to the left of";
    case Side::Right:
        return "to the right of";
    case Side::Up:
        return "above";
    case Side::Down:
        break;
    }
    return "below";
}

/// What `afterglow ctl ls` says of @p pane, as a window's control target says
/// it: the window is 0, and the ID of a tab is its place from 1. With
/// @p listed, the facts that only listPanes gives, as a window gives them.
Json::Value paneMap(const Session& session, const MuxPane& pane, bool readText, bool listed = false)
{
    const Layout& layout = session.layout();
    const std::size_t tab = layout.tabOf(pane.id()).value_or(0);
    const MuxPane::Facts facts = pane.facts(listed);
    Json::Object map{
        {"id", std::int64_t(pane.id())},
        {"window", 0},
        {"tab", std::int64_t(tab)},
        {"tab_id", std::int64_t(tab + 1)},
        {"title", facts.title},
        {"cwd", facts.directory},
        // Commands can carry secrets: they leave only with read access.
        {"command", readText ? facts.command : std::string()},
        {"cols", facts.size.cols},
        {"rows", facts.size.rows},
        {"focused", session.focusedPane() == pane.id()},
        {"context", ""},
    };
    if (!listed)
        return map;
    map.emplace_back("shell", std::string(AfterglowVt::shellStateName(facts.shell)));
    map.emplace_back("input", facts.input);
    map.emplace_back("last_exit", facts.lastExit ? Json::Value(*facts.lastExit) : Json::Value());
    map.emplace_back("user_vars", userVarsJson(facts.userVars, readText));
    // The arguments only with read access: their read can wait on the
    // memory lock of a stuck process.
    map.emplace_back("foreground", foregroundJson(pane.foreground(readText), readText));
    map.emplace_back("neighbors", neighborsJson(layout, pane.id(), facts.size));
    return map;
}

} // namespace

std::string Daemon::select(const IpcRequest& request, IpcServerConnection& connection)
{
    if (request.args.size() != 2)
        return ipcError(request.id, IpcErrorCode::InvalidArg,
                        request.fn + " takes (protocol, " + (request.fn == Fn::SelectSession ? "name)" : "pane)"));
    if (auto error = versionError(request))
        return *error;
    const Session* session = nullptr;
    if (request.fn == Fn::SelectSession) {
        if (!request.args[1].isString())
            return ipcError(request.id, IpcErrorCode::InvalidArg, "selectSession takes (protocol, name)");
        const std::string& name = request.args[1].toString();
        const auto it = find(name);
        if (it == m_sessions.end())
            return ipcError(request.id, Error::NoSuchSession, "no session named '" + name + "'");
        session = it->get();
    } else {
        const auto id = paneArg(request.args[1]);
        if (!id)
            return ipcError(request.id, IpcErrorCode::InvalidArg, "selectPane takes (protocol, pane)");
        session = sessionOfPane(*id);
        if (!session)
            return ipcError(request.id, Error::NoSuchPane, "no session has pane " + std::to_string(*id));
    }
    m_selected[&connection] = Selected{session->serial(), session->name()};
    // The session's environment, over which `afterglow ctl` prepares the
    // launch of a split or a new tab, as for the session's first pane.
    return ipcReply(
        request.id.value_or(0),
        Json::Object{{"name", session->name()}, {"environment", environmentJson(session->options().environment)}});
}

std::string Daemon::control(const IpcRequest& request, IpcServerConnection& connection, pid_t caller)
{
    const std::string& fn = request.fn;
    // `afterglow ssh` in a session pane: the daemon keeps the sockets of its
    // remote commands, and their pane names its own session.
    if (among(fn, {"remoteCommandsEnabled", "openRemoteSocket", "remoteSocketForwarded", "remoteSocketFailed"})) {
        if (request.type != "call")
            return ipcError(request.id, IpcErrorCode::MalformedRequest, "the mux daemon answers only calls");
        return remoteCall(request, caller);
    }
    const auto selected = m_selected.find(&connection);
    if (selected == m_selected.end()) {
        return failed(
            request,
            "no session is selected: afterglow ctl --session NAME, or afterglow ctl in a session pane, selects one");
    }
    // The session that the connection selected, not a later one of its name.
    const auto it = std::find_if(m_sessions.begin(), m_sessions.end(), [&selected](const auto& s) {
        return s->serial() == selected->second.serial;
    });
    if (it == m_sessions.end())
        return ipcError(request.id, Error::NoSuchSession, "session '" + selected->second.name + "' ended");
    Session& session = **it;
    // As the control target of a window that Control.enabled turns off.
    if (!session.options().control.enabled)
        return ipcError(request.id, IpcErrorCode::NoSuchTarget, "no target 'control'");
    if (request.type == "subscribe")
        return subscribe(request, connection, session);
    if (request.type != "call")
        return ipcError(request.id, IpcErrorCode::MalformedRequest,
                        "the control target answers calls, not '" + request.type + "'");
    if (const auto count = arity(fn); count && request.args.size() != *count) {
        return ipcError(request.id, IpcErrorCode::InvalidArg,
                        "argument count mismatch: 'control." + fn + "' expects " + std::to_string(*count) + ", got "
                            + std::to_string(request.args.size()));
    }
    const bool readText = session.options().control.allowReadText;
    // As a window refuses them: first, also for a pane that does not exist.
    if (fn == "screenshot" && !readText)
        return ipcError(request.id, "denied", "screenshots are off (Control.allowReadText)");
    if (fn == "getText" && !readText)
        return failed(request, "reading pane text is off (Control.allowReadText)");
    if (fn == "split" && request.args.size() >= 2 && request.args[1].isString() && !sideOf(request.args[1])) {
        return failed(request, "direction must be left, right, up or down");
    }

    // As a window checks them: the name of the signal before the pane.
    if (fn == "signalProgram" && request.args.size() == 2 && paneArg(request.args[0]) && request.args[1].isString()
        && !AfterglowPty::signalNumber(request.args[1].toString())) {
        return failed(request, "no signal is named " + request.args[1].toString());
    }

    Json::Array args = request.args;
    MuxPane* pane = nullptr;
    if (takesPane(fn)) {
        const auto id = args.empty() ? std::nullopt : paneArg(args[0]);
        if (!id)
            return ipcError(request.id, IpcErrorCode::InvalidArg, "a pane ID is a whole number from 0 to 4294967295");
        // 0: the pane that has the focus, as a window takes it.
        const std::uint32_t chosen = *id != 0 ? *id : session.focusedPane();
        pane = session.pane(chosen);
        if (!pane)
            return failed(request, "no pane " + std::to_string(chosen) + " in session '" + session.name() + "'");
        args[0] = std::int64_t(chosen);
    }
    // The window of the session, also one that the daemon still waits for,
    // and the window that shows it now.
    Attachment* const shownWindow = shown(session.name());
    Attachment* const window = shownWindow && shownWindow->showing() ? shownWindow : nullptr;

    if (fn == "listPanes") {
        Json::Array panes;
        for (const std::uint32_t id : session.layout().panes()) {
            if (const MuxPane* p = session.pane(id))
                panes.push_back(paneMap(session, *p, readText, true));
        }
        std::string reply = ipcReply(request.id.value_or(0), std::move(panes));
        // A connection takes no more at once, with what still waits on it: it
        // would close with no answer. The user variables, which can be
        // megabytes each, are what usually makes the list that large.
        if (reply.size() > IpcServerConnection::MaxWaitingBytes - connection.waitingBytes()) {
            return failed(request,
                          "the list of the panes of session '" + session.name()
                              + "' is larger than one answer can hold: their user variables can be large");
        }
        return reply;
    }
    if (fn == "focusedPane")
        return ipcReply(request.id.value_or(0), std::int64_t(session.focusedPane()));
    if (fn == "layout") {
        // The tabs and splits that the daemon keeps, which the window that
        // shows the session sends after each change: an agent saves a
        // project of a session that no window shows from them.
        Layout layout = session.layout();
        layout.moved.clear();
        // Commands can carry secrets: they leave only with read access.
        if (!readText) {
            for (auto& [id, values] : layout.values)
                values.command.clear();
        }
        std::string reply = ipcReply(request.id.value_or(0), toJson(layout));
        if (reply.size() > IpcServerConnection::MaxWaitingBytes - connection.waitingBytes()) {
            return failed(request, "the layout of session '" + session.name() + "' is larger than one answer can hold");
        }
        return reply;
    }
    if (fn == "sendText") {
        if (args.size() != 2 || !args[1].isString())
            return ipcError(request.id, IpcErrorCode::InvalidArg, "sendText takes (pane, text)");
        // The window's input that waits for room in the PTY goes first: the
        // text would pass it. The words of a window's refusal.
        if (shownWindow && shownWindow->holdsInput(pane->id()))
            return failed(request, "program closed or input queue full");
        if (pane->input(args[1].toString()) == MuxPane::Input::Written)
            return ipcReply(request.id.value_or(0), nullptr);
        return failed(request, "program closed or input queue full");
    }
    if (fn == "sendKeys") {
        if (args.size() != 2 || !args[1].isArray())
            return ipcError(request.id, IpcErrorCode::InvalidArg, "sendKeys takes (pane, keys)");
        // Every name first: a bad one sends none of the keys.
        if (args[1].toArray().size() > AfterglowVt::MaxKeysAtOnce) {
            return failed(request,
                          "sendKeys sends at most " + std::to_string(AfterglowVt::MaxKeysAtOnce) + " keys at once");
        }
        std::vector<AfterglowVt::NamedKey> keys;
        for (const Json::Value& name : args[1].toArray()) {
            auto key = name.isString() ? AfterglowVt::parseKeyName(name.toString()) : std::nullopt;
            if (!key)
                return failed(request,
                              "no key is named '" + (name.isString() ? name.toString() : Json::write(name)) + "'");
            keys.push_back(std::move(*key));
        }
        if (keys.empty())
            return failed(request, "sendKeys needs a key");
        const auto modes = pane->modes();
        if (!modes)
            return decoding(request, pane->id());
        std::string bytes;
        for (const AfterglowVt::NamedKey& key : keys)
            bytes += AfterglowVt::encodeNamedKey(key, *modes);
        // As sendText: after the window's input that waits.
        if (shownWindow && shownWindow->holdsInput(pane->id()))
            return failed(request, "program closed or input queue full");
        if (bytes.empty() || pane->input(bytes) == MuxPane::Input::Written)
            return ipcReply(request.id.value_or(0), nullptr);
        return failed(request, "program closed or input queue full");
    }
    if (fn == "paste") {
        if (args.size() < 2 || args.size() > 3 || !args[1].isString() || (args.size() == 3 && !args[2].isObject()))
            return ipcError(request.id, IpcErrorCode::InvalidArg, "paste takes (pane, text[, {prompt, enter}])");
        // prompt: only into an empty command line at a primary prompt, so
        // that the text never reaches a program that runs. enter: the Enter
        // key after the paste, in the same write.
        bool atPrompt = false;
        bool enter = false;
        if (args.size() == 3) {
            for (const auto& [key, value] : args[2].toObject()) {
                bool* flag = key == "prompt" ? &atPrompt : key == "enter" ? &enter : nullptr;
                if (!flag || !value.isBool()) {
                    return ipcError(request.id, IpcErrorCode::InvalidArg,
                                    "the options of paste are prompt and enter, true or false");
                }
                *flag = value.toBool();
            }
        }
        // An empty paste sends nothing, as in a window: not the paste marks
        // alone. An Enter that the caller asked for still goes: an empty
        // line at a prompt runs nothing, and the shell prompts again.
        if (args[1].toString().empty() && !enter)
            return ipcReply(request.id.value_or(0), nullptr);
        // As a paste in a window: bracketed when the program asked for it
        // (mode 2004), with the line ends as Enter and no other control.
        const auto modes = pane->modes();
        if (!modes)
            return decoding(request, pane->id());
        std::string bytes =
            args[1].toString().empty() ? std::string() : AfterglowVt::encodePaste(args[1].toString(), *modes);
        if (enter)
            bytes += AfterglowVt::encodeNamedKey(*AfterglowVt::parseKeyName("enter"), *modes);
        // As sendText: after the window's input that waits.
        if (shownWindow && shownWindow->holdsInput(pane->id()))
            return failed(request, "program closed or input queue full");
        if (bytes.empty())
            return ipcReply(request.id.value_or(0), nullptr);
        MuxPane::Input written = MuxPane::Input::Gone;
        if (atPrompt) {
            AfterglowVt::ShellState state = AfterglowVt::ShellState::None;
            bool busy = false;
            const auto done = pane->inputAtPrompt(bytes, state, busy);
            if (busy)
                return decoding(request, pane->id());
            if (!done)
                return ipcError(request.id, Error::NotAtPrompt, notAtPrompt(pane->id(), state));
            written = *done;
        } else {
            written = pane->input(bytes);
        }
        if (written == MuxPane::Input::Written)
            return ipcReply(request.id.value_or(0), nullptr);
        return failed(request, "program closed or input queue full");
    }
    if (fn == "getColors") {
        if (args.size() != 1)
            return ipcError(request.id, IpcErrorCode::InvalidArg, "getColors takes (pane)");
        const auto colors = pane->colors();
        if (!colors)
            return decoding(request, pane->id());
        Json::Object out;
        for (const auto& color : *colors) {
            char hex[8];
            if (color.rgb)
                std::snprintf(hex, sizeof hex, "#%02x%02x%02x", color.rgb->r, color.rgb->g, color.rgb->b);
            out.emplace_back(color.name, color.rgb ? Json::Value(std::string(hex)) : Json::Value(nullptr));
        }
        return ipcReply(request.id.value_or(0), std::move(out));
    }
    if (fn == "setColors") {
        if (args.size() != 3 || !args[1].isObject() || !args[2].isBool()) {
            return ipcError(request.id, IpcErrorCode::InvalidArg, "setColors takes (pane, colors, reset)");
        }
        // Every colour first, as a window reads them: a bad one sets none.
        std::vector<std::string> payloads =
            args[2].toBool() ? AfterglowVt::Terminal::colorResetOscs() : std::vector<std::string>();
        for (const auto& [name, value] : args[1].toObject()) {
            if (!value.isString())
                return failed(request, "the color of " + name + " is a string");
            const auto rgb = colorNamed(value.toString());
            if (!rgb)
                return failed(request, value.toString() + " is no color");
            auto payload = AfterglowVt::Terminal::colorOscOf(name, *rgb);
            if (!payload)
                return failed(request, "no color is named " + name);
            payloads.push_back(std::move(*payload));
        }
        switch (pane->applyColors(payloads)) {
        case MuxPane::Colors::Applied:
            break;
        case MuxPane::Colors::Decoding:
            return decoding(request, pane->id());
        case MuxPane::Colors::NotAColor:
            return failed(request, "pane " + std::to_string(pane->id()) + " took only some of the colors");
        }
        return ipcReply(request.id.value_or(0), nullptr);
    }
    if (fn == "setUserVar") {
        if (args.size() != 3 || !args[1].isString() || !args[2].isString()) {
            return ipcError(request.id, IpcErrorCode::InvalidArg, "setUserVar takes (pane, name, value)");
        }
        const std::string& name = args[1].toString();
        // As OSC 1337 SetUserVar names one, and a window's control target.
        if (name.empty() || name.size() > MaxUserVarName || name.find('=') != std::string::npos) {
            return failed(request,
                          "a user variable's name has 1 to " + std::to_string(MaxUserVarName) + " bytes and no =");
        }
        // In a window's words (Control::userVarsFull).
        if (!pane->setUserVar(name, args[2].toString())) {
            return failed(request,
                          "pane " + std::to_string(pane->id()) + " has " + std::to_string(MuxPane::MaxUserVars)
                              + " user variables: it takes no new name");
        }
        return ipcReply(request.id.value_or(0), nullptr);
    }
    if (fn == "signalProgram") {
        if (args.size() != 2 || !args[1].isString())
            return ipcError(request.id, IpcErrorCode::InvalidArg, "signalProgram takes (pane, signal)");
        // The name was checked before the pane, as a window checks them.
        // kill(2) is the last call of a refusal that comes from the kernel:
        // its errno says why, in a window's words.
        errno = 0;
        if (!pane->signalProgram(*AfterglowPty::signalNumber(args[1].toString()))) {
            if (errno == EPERM) {
                return failed(request,
                              "the program of pane " + std::to_string(pane->id())
                                  + " runs as another user: the kernel refused the signal");
            }
            return failed(request, "pane " + std::to_string(pane->id()) + " has no program to signal");
        }
        return ipcReply(request.id.value_or(0), nullptr);
    }
    if (fn == "focusNeighbor") {
        // The window finds it in its pixels: the daemon, which keeps the
        // layout, in the cells and pixels that the window measured last.
        if (window)
            return forward(request, connection, session, args);
        if (args.size() != 2 || !args[1].isString())
            return ipcError(request.id, IpcErrorCode::InvalidArg, "focusNeighbor takes (pane, direction)");
        const auto side = sideOf(args[1]);
        if (!side)
            return failed(request, "direction must be left, right, up or down");
        const std::uint32_t found = neighborsOf(session.layout(), pane->id(), pane->facts().size)[std::size_t(*side)];
        // An error at the edge: a client such as smart-splits.nvim then
        // passes the move on, or wraps.
        if (found == 0)
            return failed(request, std::string("no pane ") + sidePhrase(*side) + " the pane");
        session.changeLayout().focus(found);
        return ipcReply(request.id.value_or(0), std::int64_t(found));
    }
    if (fn == "getText") {
        if (args.size() < 2 || args.size() > 3 || !args[1].isString() || (args.size() == 3 && !args[2].isObject())) {
            return ipcError(request.id, IpcErrorCode::InvalidArg, "getText takes (pane, scope[, options])");
        }
        const std::string& scope = args[1].toString();
        AfterglowVt::TextFormat format;
        std::optional<AfterglowVt::TextMark> mark;
        if (args.size() == 3) {
            // As a window reads them (Control::textFormat).
            for (const auto& [key, value] : args[2].toObject()) {
                if (key == "mark") {
                    mark = markFrom(value);
                    if (!mark)
                        return failed(request, MarkOptionError);
                    continue;
                }
                bool* flag = key == "ansi"  ? &format.ansi
                    : key == "cursor"       ? &format.cursor
                    : key == "wrap_markers" ? &format.wrapMarkers
                    : key == "keep_end"     ? &format.keepEnd
                                            : nullptr;
                if (!flag)
                    return failed(request, std::string("unknown getText option ") + key + ": " + TextOptionNames);
                if (!value.isBool())
                    return failed(request, "the getText option " + key + " is true or false");
                *flag = value.toBool();
            }
        }
        if ((scope == "since") != mark.has_value())
            return failed(request, MarkScopeError);
        // The text of one output region ends before the cursor's place, as a
        // window refuses it. So does the text since a mark on the primary
        // screen.
        if ((scope == "first-output-on-screen" || scope == "last-visited-output" || scope == "since") && format.cursor)
            return failed(request, CursorScopeError);
        // A selection is the window's, and so is the output that a prompt
        // jump in it visited last. Without a window there is none to read:
        // an empty text would say that there is none to have.
        if (scope == "selection" || scope == "last-visited-output") {
            if (window)
                return forward(request, connection, session, args);
            return failed(request, "only a window that shows session '" + session.name() + "' has its " + scope);
        }
        // The output at the top of the screen: the window's view, else the
        // daemon's, which shows the bottom.
        if (scope == "first-output-on-screen" && window)
            return forward(request, connection, session, args);
        // The end of a command's output and of what came since a mark holds
        // its result: a cut keeps it. keep_end asks it of the other scopes.
        format.keepEnd = format.keepEnd || scope == "last-output" || scope == "since";
        std::optional<MuxPane::Text> text;
        if (scope == "first-output-on-screen") {
            text = pane->firstOutputOnScreen(format);
        } else if (scope == "since") {
            text = pane->textSince(*mark, format);
            // Its rows hold other text since: a reflow, an unscroll, a clear
            // of the screen or a reset gave them new content.
            if (text && text->stale)
                return ipcError(request.id, Error::StaleMark, StaleMarkMessage);
        } else {
            std::optional<AfterglowVt::Terminal::TextScope> textScope;
            if (scope == "screen")
                textScope = AfterglowVt::Terminal::TextScope::Screen;
            else if (scope == "scrollback")
                textScope = AfterglowVt::Terminal::TextScope::All;
            else if (scope == "last-output")
                textScope = AfterglowVt::Terminal::TextScope::LastOutput;
            if (!textScope) {
                return failed(request, std::string("scope is ") + GetTextScopeNames);
            }
            text = pane->text(*textScope, format);
        }
        if (!text)
            return decoding(request, pane->id());
        Json::Object answer{{"text", std::move(text->text)}, {"truncated", text->truncated}};
        if (scope == "since")
            answer.emplace_back("trimmed", text->trimmed);
        std::string reply = ipcReply(request.id.value_or(0), std::move(answer));
        // A connection takes no more at once, with what still waits on it: it
        // would close with no answer.
        if (reply.size() > IpcServerConnection::MaxWaitingBytes - connection.waitingBytes()) {
            return failed(request,
                          "the text of pane " + std::to_string(pane->id())
                              + " is larger than one answer can hold: read a smaller scope");
        }
        return reply;
    }
    if (fn == "mark") {
        if (args.size() != 1)
            return ipcError(request.id, IpcErrorCode::InvalidArg, "mark takes (pane)");
        const auto mark = pane->textMark();
        if (!mark)
            return decoding(request, pane->id());
        return ipcReply(request.id.value_or(0), markJson(*mark));
    }
    if (fn == "shellRegions") {
        const auto after = args.size() == 2 ? args[1].toInteger() : std::optional<std::int64_t>(0);
        if (args.size() < 1 || args.size() > 2 || !after || *after < 0)
            return ipcError(request.id, IpcErrorCode::InvalidArg, "shellRegions takes (pane[, after])");
        const auto regions = pane->semanticRegions(std::uint64_t(*after));
        if (!regions)
            return decoding(request, pane->id());
        Json::Array list;
        for (const AfterglowVt::SemanticRegion& region : *regions)
            list.push_back(regionJson(region));
        return ipcReply(request.id.value_or(0), std::move(list));
    }
    if (fn == "split" || fn == "newTab") {
        const bool split = fn == "split";
        if (args.size() < 3 || args.size() > 4 || !args[1].isString() || !args[2].isArray()) {
            return ipcError(request.id, IpcErrorCode::InvalidArg,
                            split ? "split takes (pane, direction, command[, launch])"
                                  : "newTab takes (pane, directory, command[, launch])");
        }
        const Side side = split ? *sideOf(args[1]) : Side::Right;
        if (session.panes().size() >= MaxSessionPanes)
            return tooManyPanes(request, session);
        // The window places the pane: a call that it has no room for waits in no queue.
        if (window) {
            if (std::string full = windowFull(request, *window); !full.empty())
                return full;
        }
        // The program that `afterglow ctl` prepared with its settings, as
        // `afterglow mux new` does. A client that prepares none gets the
        // shell of the session's first launch, whole: the shell integration
        // put its options in the arguments and its variables in the
        // environment, and one without the other breaks (bash reads ENV only
        // with --posix, and the sh programs that it starts read ENV too).
        // After a command, the user's login shell with the variables of that
        // launch. Never the first launch's directory: the pane's own comes
        // first, as in a window.
        Launch launch = session.firstLaunch();
        if (!launch.command.empty() || !launch.launchedProgram.empty()) {
            launch.argv.clear();
            launch.launchedProgram.clear();
            launch.command.clear();
            // A command that is a shell (`afterglow mux new -- bash`) got
            // the integration's variables with its options.
            dropShellIntegration(launch);
        }
        launch.workingDirectory.clear();
        if (args.size() == 4) {
            std::string error;
            auto prepared = launchFrom(args[3], &error);
            if (!prepared)
                return ipcError(request.id, IpcErrorCode::InvalidArg, error);
            launch = std::move(*prepared);
        } else if (!args[2].toArray().empty()) {
            return failed(request, "a command in a session runs with the launch that afterglow ctl --session prepares");
        }
        if (!split && !args[1].toString().empty())
            launch.workingDirectory = args[1].toString();
        const MuxPane::Facts facts = pane->facts();
        // A new tab or split starts where its pane is, as in a window.
        if (launch.workingDirectory.empty())
            launch.workingDirectory = facts.directory;
        if (launch.workingDirectory.empty())
            launch.workingDirectory = session.firstLaunch().workingDirectory;
        // Half of the pane's room, or all of it in a tab: the window that
        // shows the session measures it again, and makes a tab of a split
        // that has no room. Without a window, such a split is refused, as a
        // window refuses one.
        PaneSize size = facts.size;
        if (split) {
            if (!window && !roomBeside(size, side))
                return ipcError(request.id, Error::NoRoom, "not enough room for another pane");
            if (side == Side::Right || side == Side::Left)
                size.cols = std::max(2, size.cols / 2);
            else
                size.rows = std::max(1, size.rows / 2);
        }
        // The pane's calls wait for the window that shows the session from
        // the start, as those of a pane that the window asked for.
        const std::uint32_t id = nextPaneId();
        std::string error;
        if (!session.addPane(id, launch, size, paneSocket(), window ? window->serial() : 0,
                             window ? window->eventQueue() : 0, waker(), &error)) {
            return ipcError(request.id, Error::SpawnFailed,
                            "cannot start the program of a pane of session '" + session.name() + "': " + error);
        }
        // addPane made it a tab of its own at the end, with its values. The
        // window keeps the command that the layout kept, for the layouts that
        // it sends.
        Layout& layout = session.changeLayout();
        Json::Array command;
        if (const auto kept = layout.values.find(id); kept != layout.values.end()) {
            command = Json::Array(kept->second.command.begin(), kept->second.command.end());
        }
        // Its place has room, and its new tab is made, before it leaves that
        // tab: out of memory, it stays there, and the layout holds each pane.
        LayoutTab alone;
        if (split) {
            std::vector<LayoutNode>& tree = layout.tabs.at(layout.tabOf(pane->id()).value_or(0)).tree;
            tree.reserve(tree.size() + 2);
        } else {
            layout.tabs.reserve(layout.tabs.size() + 1);
            alone = Layout::soleTab(id);
        }
        // remove() would drop the pane's values: they move with it.
        auto values = layout.values.extract(id);
        layout.remove(id);
        if (split)
            layout.split(pane->id(), id, side);
        else
            layout.insertTab(std::move(alone), pane->id());
        if (values)
            layout.values.insert(std::move(values));
        const char* const place = split ? sideName(side) : "tab";
        // Its program starts here: its first prompt is news to the window.
        tellWindow(session, "placePane",
                   Json::Array{std::int64_t(id), std::int64_t(pane->id()), place, std::move(command), "", true});
        return ipcReply(request.id.value_or(0), std::int64_t(id));
    }
    if (fn == "close") {
        // endSession false: a close that would end the session closes
        // nothing. With the option, the answer says whether the session
        // ended.
        std::optional<bool> endsSession;
        if (args.size() == 2) {
            const Json::Value* value = args[1].isObject() ? args[1].find("endSession") : nullptr;
            if (!value || !value->isBool() || args[1].toObject().size() != 1)
                return ipcError(request.id, IpcErrorCode::InvalidArg, "close takes (pane[, {endSession}])");
            endsSession = value->toBool();
        } else if (args.size() != 1) {
            return ipcError(request.id, IpcErrorCode::InvalidArg, "close takes (pane[, {endSession}])");
        }
        const std::uint32_t id = pane->id();
        // A pane whose program exited goes at the next reap(), and the
        // session ends with its last running program: those do not count.
        const auto runsBeside = [&session](std::uint32_t closing) {
            return std::ranges::any_of(session.panes(), [closing](const auto& other) {
                return other->id() != closing && !other->exited();
            });
        };
        const bool last = !runsBeside(id);
        if (last && endsSession == false) {
            return ipcError(request.id, Error::LastPane,
                            "pane " + std::to_string(id) + " is the last pane of session '" + session.name()
                                + "': closing it ends the session");
        }
        if (session.closePane(id) && shownWindow) {
            guarded(*shownWindow, [&] {
                shownWindow->paneRemoved(id);
                return true;
            });
        }
        // Its last pane: the session ends, as when its last program exits.
        // With only exited panes left, reap() ends it at the next pass of
        // the loop, which their exit woke: their output and their watch
        // events go first.
        const bool ended = !runsBeside(id);
        if (session.empty())
            endSession(it);
        if (endsSession)
            return ipcReply(request.id.value_or(0), Json::Object{{"sessionEnded", ended}});
        return ipcReply(request.id.value_or(0), nullptr);
    }
    if (fn == "focus") {
        if (window) {
            if (std::string full = windowFull(request, *window); !full.empty())
                return full;
        }
        session.changeLayout().focus(pane->id());
        tellWindow(session, "focus", Json::Array{std::int64_t(pane->id())});
        return ipcReply(request.id.value_or(0), nullptr);
    }
    if (fn == "movePane" && args.size() == 3 && args[2].isString()) {
        // A target in another session of the daemon: the pane joins that
        // session with its program running (DaemonJoin.cpp), whether a
        // window shows either session or not. Its window did not ask, and
        // hears it (paneLeft).
        const auto target = paneArg(args[1]);
        Session* const other = target && *target != 0 && !session.pane(*target) ? sessionOfPane(*target) : nullptr;
        if (other) {
            if (!other->options().control.enabled) {
                return failed(request, "session '" + other->name() + "' takes no control calls (Control.enabled)");
            }
            const std::uint32_t id = pane->id();
            std::optional<Side> side;
            if (std::string refused =
                    joinRefusal(request, session, id, *other, *target, args[2].toString(), true, &side);
                !refused.empty()) {
                return refused;
            }
            // The session ends when that was its last pane: `session` and
            // `it` are used no more. The pane answers as the other session's.
            const bool otherReadText = other->options().control.allowReadText;
            joinPanes(session, id, *other, *target, side, true);
            return ipcReply(request.id.value_or(0), paneMap(*other, *other->pane(id), otherReadText));
        }
    }
    if (fn == "movePane" || fn == "detachTab") {
        // The window moves its panes, a new window included, and sends its
        // layout back.
        if (window) {
            if (fn == "movePane" && args.size() == 3) {
                const auto target = paneArg(args[1]);
                if (target && *target != 0 && !session.pane(*target)) {
                    return failed(request,
                                  "no pane " + std::to_string(*target) + " in session '" + session.name() + "'");
                }
            }
            return forward(request, connection, session, args);
        }
        const std::uint32_t id = pane->id();
        const Layout& current = session.layout();
        const std::size_t tab = current.tabOf(id).value_or(0);
        // A pane that leaves for a new session keeps its title and command.
        const auto movedOut = [&](Layout moved) -> std::string {
            for (const std::uint32_t p : moved.panes()) {
                if (const auto values = current.values.find(p); values != current.values.end())
                    moved.values.insert(*values);
            }
            std::string error;
            const auto fresh = breakOutPanes(session, std::move(moved), &error);
            if (!fresh)
                return failed(request, error);
            const auto now = find(*fresh);
            return ipcReply(request.id.value_or(0), paneMap(**now, *(*now)->pane(id), readText));
        };
        if (fn == "detachTab") {
            Layout moved;
            moved.tabs.push_back(current.tabs.at(tab));
            return movedOut(std::move(moved));
        }
        if (args.size() != 3 || !args[2].isString())
            return ipcError(request.id, IpcErrorCode::InvalidArg, "movePane takes (pane, target, placement)");
        const auto target = paneArg(args[1]);
        const std::string& placement = args[2].toString();
        const auto side = sideOf(args[2]);
        if (!side && placement != "new-tab" && placement != "new-window") {
            return failed(request, "placement must be left, right, up, down, new-tab or new-window");
        }
        if (!target)
            return ipcError(request.id, IpcErrorCode::InvalidArg, "a pane ID is a whole number from 0 to 4294967295");
        if (side && *target == 0)
            return failed(request, "a target pane is required");
        if (placement == "new-window" && *target != 0)
            return failed(request, "new-window takes no target pane");
        if (placement == "new-window") {
            Layout moved;
            moved.addTab(id);
            return movedOut(std::move(moved));
        }
        if (placement == "new-tab") {
            // After the tab of the target, else after its own tab.
            std::uint32_t beside = *target != id ? *target : 0;
            if (beside != 0 && !session.pane(beside)) {
                return failed(request, "no pane " + std::to_string(beside) + " in session '" + session.name() + "'");
            }
            for (const LayoutNode& node : current.tabs.at(tab).tree) {
                if (node.pane != 0 && node.pane != id && beside == 0)
                    beside = node.pane;
            }
            if (beside == 0)
                return failed(request, "The pane is alone in its tab already");
            // The room for the tab, and the tab, before the pane leaves its
            // place, and its values with it.
            Layout& layout = session.changeLayout();
            layout.tabs.reserve(layout.tabs.size() + 1);
            LayoutTab alone = Layout::soleTab(id);
            auto values = layout.values.extract(id);
            layout.remove(id);
            layout.insertTab(std::move(alone), beside);
            if (values)
                layout.values.insert(std::move(values));
        } else {
            if (*target == id)
                return failed(request, "cannot move a pane beside itself");
            const MuxPane* beside = session.pane(*target);
            if (!beside)
                return failed(request, "no pane " + std::to_string(*target) + " in session '" + session.name() + "'");
            if (!roomBeside(beside->facts().size, *side))
                return failed(request, "There is not enough room beside that pane");
            Layout& layout = session.changeLayout();
            std::vector<LayoutNode>& tree = layout.tabs.at(layout.tabOf(*target).value_or(0)).tree;
            tree.reserve(tree.size() + 2);
            auto values = layout.values.extract(id);
            layout.remove(id);
            layout.split(*target, id, *side);
            if (values)
                layout.values.insert(std::move(values));
        }
        // With no window: `afterglow ctl watch` hears of it here.
        session.noteMoved(id);
        return ipcReply(request.id.value_or(0), paneMap(session, *pane, readText));
    }
    if (windowCall(fn)) {
        if (!window) {
            return ipcError(request.id, Error::NotAttached,
                            "no window shows session '" + session.name() + "', and only a window can do " + fn);
        }
        return forward(request, connection, session, args);
    }
    return ipcError(request.id, IpcErrorCode::NoSuchFn, "no function '" + fn + "' on target 'control' of a session");
}

std::string Daemon::windowFull(const IpcRequest& request, const Attachment& window)
{
    if (window.relayRoom())
        return {};
    return ipcError(request.id, Error::TryAgain,
                    "the window of session '" + window.session()
                        + "' has not taken the calls before this one yet: try again");
}

std::string Daemon::forward(const IpcRequest& request, IpcServerConnection& connection, Session& session,
                            const Json::Array& args)
{
    Attachment* const window = showing(session.name());
    if (!window)
        return ipcError(request.id, Error::NotAttached, "no window shows session '" + session.name() + "'");
    // A window that does not read its events gets no more calls: they would
    // wait in the daemon without a bound.
    if (std::string full = windowFull(request, *window); !full.empty())
        return full;
    const std::uint64_t id = m_nextForward++;
    auto [client, added] = m_clients.try_emplace(&connection, m_nextClient);
    if (added)
        ++m_nextClient;
    const bool sent = guarded(*window, [&] {
        window->sendControl(Json::write(Json::Object{{"id", std::int64_t(id)},
                                                     {"fn", request.fn},
                                                     {"args", args},
                                                     {"client", std::int64_t(client->second)}}));
        return true;
    });
    if (!sent)
        return failed(request, "the window of session '" + session.name() + "' failed");
    if (request.fn == "overlay" || request.fn == "overlayInput")
        m_overlaySessions[&connection].insert(session.name());
    m_forwards[id] = Forward{&connection, request.id, window->serial(), session.name()};
    // A client that ends its side after its call still gets the answer.
    connection.keepAtEnd(true);
    return {};
}

void Daemon::tellWindow(const Session& session, const std::string& fn, Json::Array args)
{
    Attachment* const window = showing(session.name());
    if (!window)
        return;
    // ID 0: the window answers nothing.
    guarded(*window, [&] {
        window->sendControl(Json::write(Json::Object{{"id", 0}, {"fn", fn}, {"args", std::move(args)}}));
        return true;
    });
}

void Daemon::passReplies(Attachment& window)
{
    for (const std::string& text : window.takeControlReplies()) {
        // The client of an answer that fails (out of memory) loses its
        // connection, which tells it, and the other answers still go. One
        // that fails before its client is known waits for the end of the
        // window, as an answer that never came.
        IpcServerConnection* client = nullptr;
        const bool passed = contained("an answer of a window", [&] {
            auto reply = Json::parse(text);
            const auto id = reply ? (*reply)["id"].toInteger() : std::nullopt;
            const auto forward = id && *id > 0 ? m_forwards.find(std::uint64_t(*id)) : m_forwards.end();
            if (forward == m_forwards.end() && id && *id > 0) {
                // A client that went: the answer of the window that got its
                // call goes nowhere.
                const auto abandoned = m_abandonedForwards.find(std::uint64_t(*id));
                if (abandoned != m_abandonedForwards.end() && abandoned->second == window.serial()) {
                    m_abandonedForwards.erase(abandoned);
                    return;
                }
            }
            // Only the window that got the call answers it.
            if (forward == m_forwards.end() || forward->second.window != window.serial()) {
                std::fprintf(stderr, "aglow-mux: the window of session %s answered a call that it did not get\n",
                             window.session().c_str());
                return;
            }
            const Forward& f = forward->second;
            IpcServerConnection* const connection = f.connection;
            client = connection;
            std::string answer;
            if (const Json::Value* error = reply->find("error")) {
                const std::string& code = (*error)["code"].toString();
                answer =
                    ipcError(f.request, code.empty() ? IpcErrorCode::CallFailed : code, (*error)["message"].toString());
            } else {
                // Moved: an answer can be a screenshot of many megabytes.
                Json::Value* result = reply->find("result");
                answer = ipcReply(f.request.value_or(0), result ? std::move(*result) : Json::Value(nullptr));
            }
            // A window's answer fits one connection alone
            // (Attach::MaxControlReply), but not always with what still
            // waits on it: the connection would close with no answer.
            if (answer.size() > IpcServerConnection::MaxWaitingBytes - connection->waitingBytes()) {
                answer = ipcError(f.request, IpcErrorCode::CallFailed,
                                  "the answer of the window of session '" + window.session()
                                      + "' is larger than the connection can take now");
            }
            sendTo(*connection, std::move(answer));
            m_forwards.erase(forward);
            holdForAnswers(*connection);
        });
        // The end of the pass forgets it, and its calls that wait.
        if (!passed && client)
            client->close();
    }
}

void Daemon::answerForwarded()
{
    for (const auto& window : m_attachments)
        passReplies(*window);
    // A window that went answers nothing more.
    std::erase_if(m_abandonedForwards, [this](const auto& abandoned) {
        return std::none_of(m_attachments.begin(), m_attachments.end(), [&](const auto& window) {
            return window->serial() == abandoned.second && window->showing();
        });
    });
    for (auto it = m_forwards.begin(); it != m_forwards.end();) {
        const bool waiting = std::any_of(m_attachments.begin(), m_attachments.end(), [&](const auto& window) {
            return window->serial() == it->second.window && window->showing();
        });
        if (waiting) {
            ++it;
            continue;
        }
        IpcServerConnection* const connection = it->second.connection;
        // Out of memory, the client gets no error: its connection closes,
        // which tells it, and the other calls still get theirs.
        const bool told = contained("an answer of a window", [&] {
            sendTo(*connection,
                   ipcError(it->second.request, IpcErrorCode::CallFailed,
                            "the window of session '" + it->second.session + "' closed before it answered"));
        });
        if (!told)
            connection->close();
        it = m_forwards.erase(it);
        holdForAnswers(*connection);
    }
}

void Daemon::holdForAnswers(IpcServerConnection& connection)
{
    const bool waits = std::any_of(m_forwards.begin(), m_forwards.end(),
                                   [&](const auto& f) {
                                       return f.second.connection == &connection;
                                   })
        || std::any_of(m_subscriptions.begin(), m_subscriptions.end(), [&](const Subscription& s) {
                           return s.connection == &connection;
                       });
    connection.keepAtEnd(waits);
}

void Daemon::forget(const IpcServerConnection* connection)
{
    // First what names the connection, which needs no allocation: a failure
    // below (out of memory) leaves no pointer to it behind. A binding set
    // that this empties ends its session after the loop over the
    // connections (run()), unless the daemon closed the connection because
    // its client did not read its output in time: a stopped client is still
    // there, and its session runs on, bound to no client.
    const bool overflowed = m_overflowed.erase(connection) > 0;
    for (auto bound = m_bound.begin(); bound != m_bound.end();) {
        if (!bound->second.erase(connection) || !bound->second.empty() || !overflowed) {
            ++bound;
            continue;
        }
        if (const Session* session = sessionBySerial(bound->first)) {
            std::fprintf(stderr,
                         "aglow-mux: the client of agent session %s did not read its output in time, and its "
                         "connection closed: the session runs on, and no client ends it\n",
                         session->name().c_str());
        }
        bound = m_bound.erase(bound);
    }
    m_selected.erase(connection);
    m_closing.erase(connection);
    m_callerEnvironments.erase(connection);
    forgetWatches(connection);
    std::optional<std::uint64_t> client;
    if (const auto it = m_clients.find(connection); it != m_clients.end()) {
        client = it->second;
        m_clients.erase(it);
    }
    std::set<std::string> overlays;
    if (const auto it = m_overlaySessions.find(connection); it != m_overlaySessions.end()) {
        overlays = std::move(it->second);
        m_overlaySessions.erase(it);
    }
    for (auto it = m_forwards.begin(); it != m_forwards.end();) {
        if (it->second.connection != connection) {
            ++it;
            continue;
        }
        // Its window can still answer it, and that answer goes nowhere. Out
        // of memory, the answer counts as one of a call that it did not get.
        try {
            m_abandonedForwards.emplace(it->first, it->second.window);
        } catch (const std::exception&) {
        }
        it = m_forwards.erase(it);
    }
    // The window's connection of a client that went closes too: as the
    // connection is gone, it closes nothing here.
    if (const auto relay = m_relayOf.find(connection); relay != m_relayOf.end())
        endRelay(relay->second, true);
    m_closing.erase(connection);
    // Its overlays end, and its inputs go.
    if (!client)
        return;
    for (const std::string& name : overlays) {
        if (const auto session = find(name); session != m_sessions.end())
            tellWindow(**session, "clientGone", Json::Array{std::int64_t(*client)});
    }
}

} // namespace AfterglowMux
