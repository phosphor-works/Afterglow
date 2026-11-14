// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// `afterglow ctl watch` for a session: the events of its panes, which the
// daemon makes from what their terminals tell it, also while no window shows
// the session. A session's subscriptions end as a window's control target
// ends them, with an error: when the session ends, and when Control.enabled
// goes off.

#include "Daemon.h"

#include "MuxPane.h"
#include "Session.h"

#include <AfterglowProto/IpcServer.h>

#include <algorithm>
#include <set>

namespace AfterglowMux {

namespace Json = AfterglowProto::Json;
using AfterglowProto::IpcRequest;
using AfterglowProto::IpcServerConnection;
using AfterglowProto::ipcError;
using AfterglowProto::ipcEvent;
using AfterglowProto::ipcReply;
namespace IpcErrorCode = AfterglowProto::IpcErrorCode;

namespace {

/// The subscriptions of one connection, as PhosphorIpc takes them.
constexpr std::size_t MaxSubscriptions = 256;

} // namespace

Session* Daemon::sessionBySerial(std::uint64_t serial) const
{
    const auto it = std::find_if(m_sessions.begin(), m_sessions.end(), [serial](const auto& s) { return s->serial() == serial; });
    return it == m_sessions.end() ? nullptr : it->get();
}

std::string Daemon::subscribe(const IpcRequest& request, IpcServerConnection& connection, Session& session)
{
    // The checks and the words of PhosphorIpc's router.
    if (std::find(ControlEvents.begin(), ControlEvents.end(), request.signal) == ControlEvents.end()) {
        return ipcError(request.id, IpcErrorCode::NoSuchSignal, "target 'control' has no signal '" + request.signal + "'");
    }
    const std::int64_t id = request.id.value_or(0);
    std::size_t mine = 0;
    for (const Subscription& s : m_subscriptions) {
        if (s.connection != &connection) continue;
        ++mine;
        if (s.session == session.serial() && s.signal == request.signal) {
            return ipcError(request.id, IpcErrorCode::MalformedRequest,
                            "already subscribed to 'control." + request.signal + "' on this connection (subscriptionId " + std::to_string(s.id) + ")");
        }
        if (s.id == id) {
            return ipcError(request.id, IpcErrorCode::MalformedRequest,
                            "subscriptionId " + std::to_string(id) + " is already in use on this connection (by 'control." + s.signal + "')");
        }
    }
    if (mine >= MaxSubscriptions) {
        return ipcError(request.id, IpcErrorCode::MalformedRequest,
                        "subscription cap of " + std::to_string(MaxSubscriptions) + " per socket exceeded");
    }
    // What the panes are now is no event: the events start from here.
    if (!m_watches.contains(session.serial())) {
        SessionWatch& watch = m_watches[session.serial()];
        watch.name = session.name();
        session.setWatched(true);
        for (const auto& pane : session.panes()) watch.panes[pane->id()] = PaneWatch{pane->title(), pane->status()};
        const Layout& layout = session.layout();
        watch.focused = layout.current < layout.tabs.size() ? layout.tabs[layout.current].focused : 0;
    }
    // The events go to the subscription by the id of its request.
    m_subscriptions.push_back(Subscription{&connection, id, session.serial(), request.signal});
    return ipcReply(id, nullptr);
}

std::string Daemon::unsubscribe(const IpcRequest& request, IpcServerConnection& connection)
{
    const bool any = std::any_of(m_subscriptions.begin(), m_subscriptions.end(), [&](const Subscription& s) { return s.connection == &connection; });
    if (!any) return ipcError(request.id, IpcErrorCode::NoSuchSubscription, "no subscriptions on this connection");
    const auto it = std::find_if(m_subscriptions.begin(), m_subscriptions.end(), [&](const Subscription& s) {
        return s.connection == &connection && s.id == request.subscriptionId;
    });
    if (it == m_subscriptions.end()) {
        return ipcError(request.id, IpcErrorCode::NoSuchSubscription, "unknown subscriptionId " + std::to_string(request.subscriptionId));
    }
    m_subscriptions.erase(it);
    return ipcReply(request.id.value_or(0), nullptr);
}

void Daemon::forgetWatches(const IpcServerConnection* connection)
{
    std::erase_if(m_subscriptions, [connection](const Subscription& s) { return s.connection == connection; });
}

void Daemon::sendWatch(std::uint64_t serial, const SessionWatch& watch, const std::string& signal, Json::Array args)
{
    // Read access as the event goes out, also for one that waited.
    if (!watch.readText) {
        if (signal == "userVarChanged" && args.size() > 2) args[2] = "";
        if ((signal == "commandStarted" || signal == "commandFinished") && args.size() > 1) args[1] = "";
    }
    for (const Subscription& s : m_subscriptions) {
        if (s.session == serial && s.signal == signal) s.connection->send(ipcEvent(s.id, args));
    }
}

void Daemon::flushPane(std::uint64_t serial, SessionWatch& watch, std::uint32_t pane)
{
    const auto now = std::chrono::steady_clock::now();
    for (auto& [key, merged] : watch.merged) {
        if (merged.pane != pane || !merged.waiting) continue;
        merged.sent = now;
        sendWatch(serial, watch, merged.signal, std::exchange(merged.waiting, std::nullopt).value());
    }
}

void Daemon::watchNow(std::uint64_t serial, SessionWatch& watch, std::uint32_t pane, const std::string& signal, Json::Array args)
{
    // What waits for the pane goes first, as a window sends it.
    flushPane(serial, watch, pane);
    sendWatch(serial, watch, signal, std::move(args));
}

void Daemon::watchMerged(std::uint64_t serial, SessionWatch& watch, std::uint32_t pane, const std::string& key, const std::string& signal,
                         Json::Array args)
{
    // At most one event of a kind for a pane in Control.eventInterval: the
    // newest waits for the end of it, as in a window.
    if (watch.interval.count() == 0) return sendWatch(serial, watch, signal, std::move(args));
    const auto now = std::chrono::steady_clock::now();
    Merged& merged = watch.merged[signal + '\0' + std::to_string(pane) + '\0' + key];
    merged.signal = signal;
    merged.pane = pane;
    if (merged.waiting || (merged.sent && now - *merged.sent < watch.interval)) {
        merged.waiting = std::move(args);
        return;
    }
    merged.sent = now;
    sendWatch(serial, watch, signal, std::move(args));
}

void Daemon::pumpWatch()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_watches.begin(); it != m_watches.end();) {
        const std::uint64_t serial = it->first;
        SessionWatch& watch = it->second;
        Session* const session = sessionBySerial(serial);
        const bool open = session && session->options().control.enabled;
        // The session ended, or its control target went: its panes close
        // for the subscriptions, which then end with an error.
        if (!open) {
            for (const auto& [pane, state] : watch.panes) watchNow(serial, watch, pane, "paneClosed", Json::Array{std::int64_t(pane)});
            const std::string code = session ? IpcErrorCode::NoSuchTarget : Error::NoSuchSession;
            const std::string message = session ? "no target 'control'" : "session '" + watch.name + "' ended";
            for (const Subscription& s : m_subscriptions) {
                if (s.session == serial) s.connection->send(ipcError(s.id, code, message));
            }
            std::erase_if(m_subscriptions, [serial](const Subscription& s) { return s.session == serial; });
        }
        const bool subscribed = std::any_of(m_subscriptions.begin(), m_subscriptions.end(), [serial](const Subscription& s) { return s.session == serial; });
        if (!open || !subscribed) {
            if (session) session->setWatched(false);
            it = m_watches.erase(it);
            continue;
        }
        watch.readText = session->options().control.allowReadText;
        watch.interval = std::chrono::milliseconds(session->options().control.eventInterval);
        std::set<std::uint32_t> live;
        for (const auto& pane : session->panes()) {
            const std::uint32_t id = pane->id();
            live.insert(id);
            if (!watch.panes.contains(id)) {
                watch.panes[id] = PaneWatch{pane->title(), pane->status()};
                watchNow(serial, watch, id, "paneCreated", Json::Array{std::int64_t(id)});
            }
            PaneWatch& state = watch.panes[id];
            for (MuxPane::WatchEvent& event : pane->takeWatchEvents()) {
                using Kind = MuxPane::WatchEvent::Kind;
                switch (event.kind) {
                case Kind::Title:
                    if (state.title == event.text) break;
                    state.title = event.text;
                    watchMerged(serial, watch, id, {}, "titleChanged", Json::Array{std::int64_t(id), std::move(event.text)});
                    break;
                case Kind::Status: {
                    std::pair<std::string, std::string> status{std::move(event.name), std::move(event.text)};
                    if (state.status == status) break;
                    state.status = status;
                    watchMerged(serial, watch, id, {}, "statusChanged", Json::Array{std::int64_t(id), status.first, status.second});
                    break;
                }
                case Kind::Bell:
                    watchMerged(serial, watch, id, {}, "bell", Json::Array{std::int64_t(id)});
                    break;
                case Kind::UserVar:
                    watchMerged(serial, watch, id, event.name, "userVarChanged", Json::Array{std::int64_t(id), event.name, std::move(event.text)});
                    break;
                case Kind::CommandStarted:
                    watchNow(serial, watch, id, "commandStarted", Json::Array{std::int64_t(id), std::move(event.text)});
                    break;
                case Kind::CommandFinished:
                    watchNow(serial, watch, id, "commandFinished",
                             Json::Array{std::int64_t(id), std::move(event.text), event.exitCode, std::int64_t(event.milliseconds)});
                    break;
                }
            }
        }
        for (auto known = watch.panes.begin(); known != watch.panes.end();) {
            if (live.contains(known->first)) {
                ++known;
                continue;
            }
            const std::uint32_t pane = known->first;
            watchNow(serial, watch, pane, "paneClosed", Json::Array{std::int64_t(pane)});
            std::erase_if(watch.merged, [pane](const auto& merged) { return merged.second.pane == pane; });
            known = watch.panes.erase(known);
        }
        // The moves of a window's layout and of `afterglow ctl`, in their order.
        for (const std::uint32_t pane : session->takeMoved()) {
            if (live.contains(pane)) watchNow(serial, watch, pane, "paneMoved", Json::Array{std::int64_t(pane)});
        }
        const Layout& layout = session->layout();
        const std::uint32_t focused = layout.current < layout.tabs.size() ? layout.tabs[layout.current].focused : 0;
        if (focused != watch.focused && focused != 0) watchNow(serial, watch, focused, "focusChanged", Json::Array{std::int64_t(focused)});
        watch.focused = focused;
        // What waited for its interval goes. An interval that passed with
        // nothing waiting leaves no entry.
        for (auto merged = watch.merged.begin(); merged != watch.merged.end();) {
            const bool over = !merged->second.sent || now - *merged->second.sent >= watch.interval;
            if (over && merged->second.waiting) {
                merged->second.sent = now;
                sendWatch(serial, watch, merged->second.signal, std::exchange(merged->second.waiting, std::nullopt).value());
            } else if (over) {
                merged = watch.merged.erase(merged);
                continue;
            }
            ++merged;
        }
        ++it;
    }
}

int Daemon::watchDueMs() const
{
    const auto now = std::chrono::steady_clock::now();
    int due = -1;
    for (const auto& [serial, watch] : m_watches) {
        for (const auto& [key, merged] : watch.merged) {
            // An entry ends at the end of its interval, whether it waits or not.
            const auto left = merged.sent ? std::chrono::duration_cast<std::chrono::milliseconds>(*merged.sent + watch.interval - now).count() : 0;
            const int ms = int(std::max<std::int64_t>(0, left)) + 1;
            if (due < 0 || ms < due) due = ms;
        }
    }
    return due;
}

} // namespace AfterglowMux
