// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

// A pane that joins another session of the daemon (tmux's join-pane): the
// window of its session sends Fn::JoinPane, and `afterglow ctl --session`
// moves a pane beside a pane of another session (DaemonControl.cpp). The
// pane goes with its PTY, its terminal and its history: its program runs on.
// The window that showed it drags it out, as for a break out, and the window
// that shows the other session places it, as for a split of `afterglow ctl`.

#include "Daemon.h"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace AfterglowMux {

namespace Json = AfterglowProto::Json;
using AfterglowProto::ipcError;
using AfterglowProto::ipcReply;
using AfterglowProto::IpcRequest;
namespace IpcErrorCode = AfterglowProto::IpcErrorCode;

namespace {

/// The side of @p placement, an empty one for "new-tab", nothing for a
/// placement that a join does not take.
std::optional<std::optional<Side>> joinPlacement(const std::string& placement)
{
    for (const Side side : {Side::Right, Side::Down, Side::Left, Side::Up}) {
        if (placement == sideName(side))
            return std::optional<std::optional<Side>>(std::in_place, side);
    }
    if (placement == "new-tab")
        return std::optional<std::optional<Side>>(std::in_place);
    return std::nullopt;
}

} // namespace

std::string Daemon::joinPane(const IpcRequest& request)
{
    if (request.args.size() < 5 || request.args.size() > 6 || !request.args[1].isString()
        || !request.args[4].isString()) {
        return ipcError(request.id, IpcErrorCode::InvalidArg,
                        "joinPane takes (protocol, name, pane, beside, placement[, window])");
    }
    if (auto error = versionError(request))
        return *error;
    const std::string& name = request.args[1].toString();
    const auto from = find(name);
    if (from == m_sessions.end())
        return ipcError(request.id, Error::NoSuchSession, "no session named '" + name + "'");
    // A window that no longer shows the session moves none of its panes.
    if (std::string refusal = windowRefusal(request, name, 5); !refusal.empty())
        return refusal;
    const auto id = paneIdFrom(request.args[2]);
    const auto beside = paneIdFrom(request.args[3]);
    if (!id || !beside)
        return ipcError(request.id, IpcErrorCode::InvalidArg, "a pane is a whole number from 1 to 4294967295");
    if (!(*from)->pane(*id))
        return ipcError(request.id, Error::NoSuchPane, "session '" + name + "' has no pane " + std::to_string(*id));
    Session* const to = sessionOfPane(*beside);
    if (!to)
        return ipcError(request.id, Error::NoSuchPane, "no session has pane " + std::to_string(*beside));
    // Within its session, the window moves the pane and sends its layout.
    if (to == from->get())
        return failed(request, "pane " + std::to_string(*beside) + " is in session '" + name + "' too");
    std::optional<Side> side;
    if (std::string refusal = joinRefusal(request, **from, *id, *to, *beside, request.args[4].toString(), false, &side);
        !refusal.empty()) {
        return refusal;
    }
    std::string target = to->name();
    // The window that asked drags the pane out when it has the answer: it
    // knows that it leaves (MuxClient::joinLater).
    joinPanes(**from, *id, *to, *beside, side, false);
    return ipcReply(request.id.value_or(0), Json::Object{{"name", std::move(target)}});
}

std::string Daemon::joinRefusal(const IpcRequest& request, const Session& from, std::uint32_t id, const Session& to,
                                std::uint32_t beside, const std::string& placement, bool tellFrom,
                                std::optional<Side>* side) const
{
    const auto place = joinPlacement(placement);
    if (!place) {
        // A new window makes a new session (Fn::BreakOut), and names no target.
        if (placement == "new-window")
            return failed(request, "new-window takes no target pane");
        return failed(request, "placement must be left, right, up, down, new-tab or new-window");
    }
    *side = *place;
    // An agent reads the text of each pane of its session: a pane whose
    // session keeps its text from clients (Control.allowReadText) does not
    // go there. A guard against a mistake, as Control.allowAdopt.
    if (to.options().agent && !from.options().control.allowReadText) {
        return failed(request,
                      "pane " + std::to_string(id) + " cannot join agent session '" + to.name() + "': session '"
                          + from.name() + "' keeps its text (Control.allowReadText)");
    }
    if (to.panes().size() >= MaxSessionPanes)
        return tooManyPanes(request, to);
    // The calls that tell the windows wait in no queue past Mux.eventQueue.
    if (tellFrom) {
        if (const Attachment* window = showing(from.name())) {
            if (std::string full = windowFull(request, *window); !full.empty())
                return full;
        }
    }
    const Attachment* const window = showing(to.name());
    // The window knows a pane ID once in its stream: a pane that left it
    // comes back only after the window let it go (PaneForgotten).
    if (window && window->stillLeaving(id)) {
        return failed(request,
                      "pane " + std::to_string(id) + " is still leaving the window of session '" + to.name() + "'");
    }
    if (window)
        return windowFull(request, *window);
    // The window that shows the session measures the room, and makes a tab
    // of a split that has none. Without one, such a move is refused, as a
    // window refuses it: two columns or one row, as half of the pane.
    if (*side && !roomBeside(to.pane(beside)->facts().size, **side))
        return failed(request, "There is not enough room beside that pane");
    return {};
}

void Daemon::joinPanes(Session& from, std::uint32_t id, Session& to, std::uint32_t beside, std::optional<Side> side,
                       bool tellFrom)
{
    // Each allocation before the pane leaves its session: a failure then
    // leaves it where it was, and no pane between two sessions, where its
    // program would end. The title and the command that the window of the
    // session gave the pane go with it, while the layout has room for them.
    std::map<std::uint32_t, PaneValues> carried;
    if (const auto values = from.layout().values.find(id); values != from.layout().values.end()) {
        if (to.layout().valueBytes() + values->second.bytes() <= MaxLayoutValueBytes)
            carried.insert(*values);
    }
    auto values = carried.extract(id);
    // The window of the other session keeps the command for a project, as
    // for a split of `afterglow ctl`, and shows the title that the user gave
    // the pane: its layout, which it sends next, then holds both.
    Json::Array command;
    std::string title;
    if (values) {
        command = Json::Array(values.mapped().command.begin(), values.mapped().command.end());
        title = values.mapped().title;
    }
    const char* const place = side ? sideName(*side) : "tab";
    // Its program ran before: the window takes its first frame as no news.
    Json::Array placed{std::int64_t(id), std::int64_t(beside), place, std::move(command), std::move(title), false};
    Json::Array left{std::int64_t(id), to.name()};
    const std::string fromName = from.name();
    std::vector<std::pair<std::uint32_t, MuxPane*>> removed;
    removed.reserve(1);
    to.reservePane();
    Layout& layout = to.changeLayout();
    LayoutTab alone;
    if (side) {
        std::vector<LayoutNode>& tree = layout.tabs.at(layout.tabOf(beside).value_or(0)).tree;
        tree.reserve(tree.size() + 2);
    } else {
        layout.tabs.reserve(layout.tabs.size() + 1);
        alone = Layout::soleTab(id);
    }
    // From here nothing allocates but the windows (guarded).
    std::unique_ptr<MuxPane> pane = from.take(id);
    MuxPane* const moved = pane.get();
    removed.emplace_back(id, moved);
    // Before its PaneClosed: the window closes the pane as one that left,
    // not as the end of its program.
    if (tellFrom)
        tellWindow(from, "paneLeft", std::move(left));
    if (Attachment* window = shown(fromName)) {
        guarded(*window, [&] {
            window->panesRemoved(removed);
            return true;
        });
    }
    to.join(std::move(pane));
    if (side)
        layout.split(beside, id, *side);
    else
        layout.insertTab(std::move(alone), beside);
    if (values)
        layout.values.insert(std::move(values));
    // Before its PaneOpened, which the next pass sends: the window then
    // knows the pane as its own.
    tellWindow(to, "placePane", std::move(placed));
    // The terminal values of the other session, which its window sent.
    // Out of memory, the pane keeps its own: it moved all the same.
    contained("the terminal values of a pane that moved", [&] {
        moved->setOptions(to.options().terminal);
    });
    // Its last pane: the session ends, as when its last program exits.
    if (from.empty()) {
        if (const auto ended = find(fromName); ended != m_sessions.end())
            endSession(ended);
    }
}

} // namespace AfterglowMux
