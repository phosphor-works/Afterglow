// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// The control target of the daemon: `afterglow ctl --session NAME`, and
// `afterglow ctl` in a session pane. The calls and their values are those of
// the control target of a window (ControlSocket in the application), so that
// one client speaks both. The daemon answers the calls on panes, text and
// layout itself, with its own pane IDs, also while no window shows the
// session; a window that shows it moves the panes that the daemon places.
// The calls that only a window can answer (titles, shader signals, actions,
// projects) go to that window, whose answer the daemon passes back.

#include "Daemon.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <string_view>

namespace AfterglowMux {

namespace Json = AfterglowProto::Json;
using AfterglowProto::ipcError;
using AfterglowProto::ipcReply;
using AfterglowProto::IpcRequest;
using AfterglowProto::IpcServerConnection;
namespace IpcErrorCode = AfterglowProto::IpcErrorCode;

namespace {

bool among(std::string_view fn, std::initializer_list<std::string_view> names)
{
    return std::find(names.begin(), names.end(), fn) != names.end();
}

/// The calls whose first argument is a pane.
bool takesPane(std::string_view fn)
{
    return among(fn, {"sendText", "getText", "split", "newTab", "focus", "close", "setTitle", "signal", "notify", "runAction", "movePane",
                      "detachTab", "saveProject", "overlay", "screenshot"});
}

/// The calls that only a window answers.
bool windowCall(std::string_view fn)
{
    return among(fn, {"setTitle", "signal", "notify", "runAction", "newWindow", "openDirectory", "projects", "saveProject", "openProject",
                      "scriptDiagnostics", "overlay", "overlayInput", "screenshot"});
}

std::optional<std::uint32_t> paneArg(const Json::Value& value)
{
    const auto id = value.toInteger();
    if (!id || *id < 0 || *id > 0xffffffffLL) return std::nullopt;
    return std::uint32_t(*id);
}

std::optional<Side> sideOf(const Json::Value& value)
{
    const std::string& name = value.toString();
    if (name == "right") return Side::Right;
    if (name == "down") return Side::Down;
    if (name == "left") return Side::Left;
    if (name == "up") return Side::Up;
    return std::nullopt;
}

/// What `afterglow ctl ls` says of @p pane, as a window's control target says
/// it: the window is 0, and the ID of a tab is its place from 1.
Json::Value paneMap(const Session& session, const MuxPane& pane, bool readText)
{
    const Layout& layout = session.layout();
    const std::size_t tab = layout.tabOf(pane.id()).value_or(0);
    const bool focused = layout.current < layout.tabs.size() && layout.tabs[layout.current].focused == pane.id();
    const MuxPane::Facts facts = pane.facts().value_or(MuxPane::Facts{});
    return Json::Object{
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
        {"focused", focused},
        {"context", ""},
    };
}

std::string failed(const IpcRequest& request, const std::string& message)
{
    return ipcError(request.id, IpcErrorCode::CallFailed, message);
}

} // namespace

std::string Daemon::select(const IpcRequest& request, IpcServerConnection& connection)
{
    if (request.args.size() != 2) return ipcError(request.id, IpcErrorCode::InvalidArg, request.fn + " takes (protocol, " + (request.fn == Fn::SelectSession ? "name)" : "pane)"));
    if (auto error = versionError(request)) return *error;
    std::string name;
    if (request.fn == Fn::SelectSession) {
        if (!request.args[1].isString()) return ipcError(request.id, IpcErrorCode::InvalidArg, "selectSession takes (protocol, name)");
        name = request.args[1].toString();
        if (find(name) == m_sessions.end()) return ipcError(request.id, Error::NoSuchSession, "no session named '" + name + "'");
    } else {
        const auto id = paneArg(request.args[1]);
        if (!id) return ipcError(request.id, IpcErrorCode::InvalidArg, "selectPane takes (protocol, pane)");
        for (const auto& session : m_sessions) {
            if (session->pane(*id)) name = session->name();
        }
        if (name.empty()) return ipcError(request.id, Error::NoSuchPane, "no session has pane " + std::to_string(*id));
    }
    m_selected[&connection] = name;
    return ipcReply(request.id.value_or(0), Json::Object{{"name", name}});
}

std::string Daemon::control(const IpcRequest& request, IpcServerConnection& connection, pid_t caller)
{
    const std::string& fn = request.fn;
    // `afterglow ssh` in a session pane: the daemon keeps the sockets of its
    // remote commands, and their pane names its own session.
    if (among(fn, {"remoteCommandsEnabled", "openRemoteSocket", "remoteSocketForwarded", "remoteSocketFailed"})) {
        if (request.type != "call") return ipcError(request.id, IpcErrorCode::MalformedRequest, "the mux daemon answers only calls");
        return remoteCall(request, caller);
    }
    const auto selected = m_selected.find(&connection);
    if (selected == m_selected.end()) {
        return failed(request, "no session is selected: afterglow ctl --session NAME, or afterglow ctl in a session pane, selects one");
    }
    const auto it = find(selected->second);
    if (it == m_sessions.end()) return ipcError(request.id, Error::NoSuchSession, "session '" + selected->second + "' ended");
    Session& session = **it;
    // As the control target of a window that Control.enabled turns off.
    if (!session.options().control.enabled) return ipcError(request.id, IpcErrorCode::NoSuchTarget, "no target 'control'");
    if (request.type == "subscribe") return subscribe(request, connection, session);
    if (request.type != "call") return ipcError(request.id, IpcErrorCode::MalformedRequest, "the control target answers calls, not '" + request.type + "'");
    // As a window refuses one: first, also for a pane that does not exist.
    if (fn == "screenshot" && !session.options().control.allowReadText) {
        return ipcError(request.id, "denied", "screenshots are off (Control.allowReadText)");
    }

    Json::Array args = request.args;
    MuxPane* pane = nullptr;
    if (takesPane(fn)) {
        const auto id = args.empty() ? std::nullopt : paneArg(args[0]);
        if (!id) return ipcError(request.id, IpcErrorCode::InvalidArg, "a pane ID is a whole number from 0 to 4294967295");
        // 0: the pane that has the focus, as a window takes it.
        const std::uint32_t chosen = *id != 0 ? *id : session.layout().tabs.at(session.layout().current).focused;
        pane = session.pane(chosen);
        if (!pane) return failed(request, "no pane " + std::to_string(chosen) + " in session '" + session.name() + "'");
        args[0] = std::int64_t(chosen);
    }
    const bool readText = session.options().control.allowReadText;
    Attachment* const window = shown(session.name());
    const bool showing = window && window->showing();

    if (fn == "listPanes") {
        Json::Array panes;
        for (const std::uint32_t id : session.layout().panes()) {
            if (const MuxPane* p = session.pane(id)) panes.push_back(paneMap(session, *p, readText));
        }
        return ipcReply(request.id.value_or(0), std::move(panes));
    }
    if (fn == "focusedPane") {
        return ipcReply(request.id.value_or(0), std::int64_t(session.layout().tabs.at(session.layout().current).focused));
    }
    if (fn == "sendText") {
        if (args.size() != 2 || !args[1].isString()) return ipcError(request.id, IpcErrorCode::InvalidArg, "sendText takes (pane, text)");
        switch (pane->input(args[1].toString())) {
        case MuxPane::Input::Written: return ipcReply(request.id.value_or(0), nullptr);
        case MuxPane::Input::Full: return failed(request, "the program of pane " + std::to_string(pane->id()) + " reads no more input now");
        case MuxPane::Input::Gone: break;
        }
        return failed(request, "the program of pane " + std::to_string(pane->id()) + " exited");
    }
    if (fn == "getText") {
        if (args.size() != 2 || !args[1].isString()) return ipcError(request.id, IpcErrorCode::InvalidArg, "getText takes (pane, scope)");
        if (!readText) return failed(request, "reading pane text is off (Control.allowReadText)");
        const std::string& scope = args[1].toString();
        // A selection is the window's.
        if (scope == "selection") return showing ? forward(request, connection, session, args) : ipcReply(request.id.value_or(0), "");
        std::optional<AfterglowVt::Terminal::TextScope> textScope;
        if (scope == "screen") textScope = AfterglowVt::Terminal::TextScope::Screen;
        else if (scope == "scrollback") textScope = AfterglowVt::Terminal::TextScope::All;
        else if (scope == "last-output") textScope = AfterglowVt::Terminal::TextScope::LastOutput;
        if (!textScope) return failed(request, "scope is screen, scrollback, selection or last-output");
        const auto text = pane->text(*textScope);
        if (!text) return failed(request, "pane " + std::to_string(pane->id()) + " decodes an image: try again");
        return ipcReply(request.id.value_or(0), *text);
    }
    if (fn == "split" || fn == "newTab") {
        const bool split = fn == "split";
        if (args.size() < 3 || args.size() > 4 || !args[1].isString() || !args[2].isArray()) {
            return ipcError(request.id, IpcErrorCode::InvalidArg, split ? "split takes (pane, direction, command[, launch])"
                                                                        : "newTab takes (pane, directory, command[, launch])");
        }
        const auto side = split ? sideOf(args[1]) : std::optional<Side>(Side::Right);
        if (!side) return failed(request, "direction is left, right, up or down");
        // The program that `afterglow ctl` prepared with its settings, as
        // `afterglow mux new` does. A client that prepares none gets the
        // program of the session's first pane, its shell, but not its
        // directory: the pane's own comes first, as in a window.
        Launch launch = session.firstLaunch();
        launch.workingDirectory.clear();
        if (args.size() == 4) {
            std::string error;
            auto prepared = launchFrom(args[3], &error);
            if (!prepared) return ipcError(request.id, IpcErrorCode::InvalidArg, error);
            launch = std::move(*prepared);
        } else if (!args[2].toArray().empty()) {
            return failed(request, "a command in a session runs with the launch that afterglow ctl --session prepares");
        }
        if (!split && !args[1].toString().empty()) launch.workingDirectory = args[1].toString();
        const auto facts = pane->facts();
        // A new tab or split starts where its pane is, as in a window.
        if (launch.workingDirectory.empty()) launch.workingDirectory = facts ? facts->directory : pane->directory();
        if (launch.workingDirectory.empty()) launch.workingDirectory = session.firstLaunch().workingDirectory;
        // Half of the pane's room, or all of it in a tab: the window that
        // shows the session measures it again.
        PaneSize size = facts ? facts->size : session.firstSize();
        if (*side == Side::Right || *side == Side::Left) size.cols = std::max(2, size.cols / (split ? 2 : 1));
        else size.rows = std::max(1, size.rows / 2);
        const std::uint32_t id = m_nextPane++;
        std::string error;
        if (!session.addPane(id, launch, size, paneSocket(), waker(), &error)) {
            return ipcError(request.id, Error::SpawnFailed, "cannot start the program of a pane of session '" + session.name() + "': " + error);
        }
        // addPane made it a tab of its own at the end, with its values.
        auto values = session.layout().values.extract(id);
        session.layout().remove(id);
        static constexpr std::array<const char*, 4> SideNames{"right", "down", "left", "up"};
        // The window keeps the command that the layout kept, for the
        // layouts that it sends.
        Json::Array command;
        if (values) command = Json::Array(values.mapped().command.begin(), values.mapped().command.end());
        if (split) {
            session.layout().split(pane->id(), id, *side);
            tellWindow(session, "placePane",
                       Json::Array{std::int64_t(id), std::int64_t(pane->id()), SideNames[std::size_t(*side)], std::move(command)});
        } else {
            session.layout().insertTab(id, pane->id());
            tellWindow(session, "placePane", Json::Array{std::int64_t(id), std::int64_t(pane->id()), "tab", std::move(command)});
        }
        if (values && session.layout().contains(id)) session.layout().values.insert(std::move(values));
        return ipcReply(request.id.value_or(0), std::int64_t(id));
    }
    if (fn == "close") {
        const std::uint32_t id = pane->id();
        if (session.closePane(id) && window) {
            guarded(*window, [&] {
                window->paneRemoved(id);
                return true;
            });
        }
        // Its last pane: the session ends, as when its last program exits.
        if (session.empty()) endSession(it);
        return ipcReply(request.id.value_or(0), nullptr);
    }
    if (fn == "focus") {
        session.layout().focus(pane->id());
        tellWindow(session, "focus", Json::Array{std::int64_t(pane->id())});
        return ipcReply(request.id.value_or(0), nullptr);
    }
    if (fn == "movePane" || fn == "detachTab") {
        // The window moves its panes, a new window included, and sends its
        // layout back.
        if (showing) {
            if (fn == "movePane" && args.size() == 3) {
                const auto target = paneArg(args[1]);
                if (target && *target != 0 && !session.pane(*target)) {
                    return failed(request, "no pane " + std::to_string(*target) + " in session '" + session.name() + "'");
                }
            }
            return forward(request, connection, session, args);
        }
        const std::uint32_t id = pane->id();
        Layout& layout = session.layout();
        const std::size_t tab = layout.tabOf(id).value_or(0);
        if (fn == "detachTab") {
            Layout moved;
            moved.tabs.push_back(layout.tabs.at(tab));
            std::string error;
            const auto fresh = breakOutPanes(session, std::move(moved), &error);
            if (!fresh) return failed(request, error);
            const auto now = find(*fresh);
            return ipcReply(request.id.value_or(0), paneMap(**now, *(*now)->pane(id), readText));
        }
        if (args.size() != 3 || !args[2].isString()) return ipcError(request.id, IpcErrorCode::InvalidArg, "movePane takes (pane, target, placement)");
        const auto target = paneArg(args[1]);
        const std::string& placement = args[2].toString();
        const auto side = sideOf(args[2]);
        if (!side && placement != "new-tab" && placement != "new-window") {
            return failed(request, "placement must be left, right, up, down, new-tab or new-window");
        }
        if (!target || ((placement == "new-tab" || placement == "new-window") != (*target == 0))) {
            return failed(request, side ? "a target pane is required" : "new-tab and new-window need target 0");
        }
        if (placement == "new-window") {
            Layout moved;
            moved.addTab(id);
            std::string error;
            const auto fresh = breakOutPanes(session, std::move(moved), &error);
            if (!fresh) return failed(request, error);
            const auto now = find(*fresh);
            return ipcReply(request.id.value_or(0), paneMap(**now, *(*now)->pane(id), readText));
        }
        if (placement == "new-tab") {
            std::uint32_t beside = 0;
            for (const LayoutNode& node : layout.tabs.at(tab).tree) {
                if (node.pane != 0 && node.pane != id && beside == 0) beside = node.pane;
            }
            if (beside == 0) return failed(request, "The pane is alone in its tab already");
            layout.remove(id);
            layout.insertTab(id, beside);
        } else {
            if (*target == id) return failed(request, "cannot move a pane beside itself");
            if (!session.pane(*target)) return failed(request, "no pane " + std::to_string(*target) + " in session '" + session.name() + "'");
            layout.remove(id);
            layout.split(*target, id, *side);
        }
        // With no window: `afterglow ctl watch` hears of it here.
        session.noteMoved(id);
        return ipcReply(request.id.value_or(0), paneMap(session, *pane, readText));
    }
    if (windowCall(fn)) {
        if (!showing) return failed(request, "no window shows session '" + session.name() + "', and only a window can do " + fn);
        return forward(request, connection, session, args);
    }
    return ipcError(request.id, IpcErrorCode::NoSuchFn, "no function '" + fn + "' on target 'control' of a session");
}

std::string Daemon::forward(const IpcRequest& request, IpcServerConnection& connection, Session& session, const Json::Array& args)
{
    Attachment* const window = shown(session.name());
    if (!window || !window->showing()) return failed(request, "no window shows session '" + session.name() + "'");
    const std::uint64_t id = m_nextForward++;
    auto [client, added] = m_clients.try_emplace(&connection, m_nextClient);
    if (added) ++m_nextClient;
    const bool sent = guarded(*window, [&] {
        window->sendControl(Json::write(
            Json::Object{{"id", std::int64_t(id)}, {"fn", request.fn}, {"args", args}, {"client", std::int64_t(client->second)}}));
        return true;
    });
    if (!sent) return failed(request, "the window of session '" + session.name() + "' failed");
    if (request.fn == "overlay" || request.fn == "overlayInput") m_overlaySessions[&connection].insert(session.name());
    m_forwards[id] = Forward{&connection, request.id, window->serial(), session.name()};
    return {};
}

void Daemon::tellWindow(const Session& session, const std::string& fn, Json::Array args)
{
    Attachment* const window = shown(session.name());
    if (!window || !window->showing()) return;
    // ID 0: the window answers nothing.
    guarded(*window, [&] {
        window->sendControl(Json::write(Json::Object{{"id", 0}, {"fn", fn}, {"args", std::move(args)}}));
        return true;
    });
}

void Daemon::answerForwarded()
{
    for (const auto& window : m_attachments) {
        for (const std::string& text : window->takeControlReplies()) {
            const auto reply = Json::parse(text);
            const auto id = reply ? (*reply)["id"].toInteger() : std::nullopt;
            const auto forward = id && *id > 0 ? m_forwards.find(std::uint64_t(*id)) : m_forwards.end();
            // A client that went: its answer goes nowhere.
            if (forward == m_forwards.end() && id && m_abandonedForwards.erase(std::uint64_t(*id))) continue;
            // Only the window that got the call answers it.
            if (forward == m_forwards.end() || forward->second.window != window->serial()) {
                std::fprintf(stderr, "aglow-mux: the window of session %s answered a call that it did not get\n",
                             window->session().c_str());
                continue;
            }
            const Forward& f = forward->second;
            if (const Json::Value* error = reply->find("error")) {
                const std::string& code = (*error)["code"].toString();
                f.connection->send(ipcError(f.request, code.empty() ? IpcErrorCode::CallFailed : code, (*error)["message"].toString()));
            } else {
                f.connection->send(ipcReply(f.request.value_or(0), (*reply)["result"]));
            }
            m_forwards.erase(forward);
        }
    }
    // A window that went answers nothing more.
    std::erase_if(m_abandonedForwards, [this](const auto& abandoned) {
        return std::none_of(m_attachments.begin(), m_attachments.end(),
                            [&](const auto& window) { return window->serial() == abandoned.second && window->showing(); });
    });
    for (auto it = m_forwards.begin(); it != m_forwards.end();) {
        const bool waiting = std::any_of(m_attachments.begin(), m_attachments.end(), [&](const auto& window) {
            return window->serial() == it->second.window && window->showing();
        });
        if (waiting) {
            ++it;
            continue;
        }
        it->second.connection->send(ipcError(it->second.request, IpcErrorCode::CallFailed,
                                             "the window of session '" + it->second.session + "' closed before it answered"));
        it = m_forwards.erase(it);
    }
}

void Daemon::forget(const IpcServerConnection* connection)
{
    // Its overlays end, and its inputs go.
    if (const auto sessions = m_overlaySessions.find(connection); sessions != m_overlaySessions.end()) {
        for (const std::string& name : sessions->second) {
            if (const auto session = find(name); session != m_sessions.end()) {
                tellWindow(**session, "clientGone", Json::Array{std::int64_t(m_clients.at(connection))});
            }
        }
        m_overlaySessions.erase(sessions);
    }
    m_clients.erase(connection);
    m_selected.erase(connection);
    forgetWatches(connection);
    std::erase_if(m_forwards, [this, connection](const auto& forward) {
        if (forward.second.connection != connection) return false;
        m_abandonedForwards.emplace(forward.first, forward.second.window);
        return true;
    });
    // The window's connection of a client that went closes too.
    if (const auto relay = m_relayOf.find(connection); relay != m_relayOf.end()) endRelay(relay->second, true);
    m_closing.erase(connection);
}

} // namespace AfterglowMux
