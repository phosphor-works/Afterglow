// SPDX-FileCopyrightText: 2026 Phosphor Works Team
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
#include <cstdio>
#include <exception>
#include <set>
#include <utility>
#include <vector>

namespace AfterglowMux {

namespace Json = AfterglowProto::Json;
using AfterglowProto::ipcError;
using AfterglowProto::ipcEvent;
using AfterglowProto::ipcReply;
using AfterglowProto::IpcRequest;
using AfterglowProto::IpcServerConnection;
namespace IpcErrorCode = AfterglowProto::IpcErrorCode;

namespace {

/// The subscriptions of one connection, as PhosphorIpc takes them.
constexpr std::size_t MaxSubscriptions = 256;

} // namespace

Session* Daemon::sessionBySerial(std::uint64_t serial) const
{
    const auto it = std::find_if(m_sessions.begin(), m_sessions.end(), [serial](const auto& s) {
        return s->serial() == serial;
    });
    return it == m_sessions.end() ? nullptr : it->get();
}

std::string Daemon::subscribe(const IpcRequest& request, IpcServerConnection& connection, Session& session)
{
    // The checks and the words of PhosphorIpc's router, in its order: the
    // cap before the others.
    if (std::find(ControlEvents.begin(), ControlEvents.end(), request.signal) == ControlEvents.end()) {
        return ipcError(request.id, IpcErrorCode::NoSuchSignal,
                        "target 'control' has no signal '" + request.signal + "'");
    }
    const std::int64_t id = request.id.value_or(0);
    const auto mine =
        std::count_if(m_subscriptions.begin(), m_subscriptions.end(), [&connection](const Subscription& s) {
            return s.connection == &connection;
        });
    if (std::size_t(mine) >= MaxSubscriptions) {
        return ipcError(request.id, IpcErrorCode::MalformedRequest,
                        "subscription cap of " + std::to_string(MaxSubscriptions) + " per socket exceeded");
    }
    for (const Subscription& s : m_subscriptions) {
        if (s.connection != &connection)
            continue;
        if (s.session == session.serial() && s.signal == request.signal) {
            return ipcError(request.id, IpcErrorCode::MalformedRequest,
                            "already subscribed to 'control." + request.signal + "' on this connection (subscriptionId "
                                + std::to_string(s.id) + ")");
        }
        if (s.id == id) {
            return ipcError(request.id, IpcErrorCode::MalformedRequest,
                            "subscriptionId " + std::to_string(id)
                                + " is already in use on this connection (by 'control." + s.signal + "')");
        }
    }
    // What the panes are now is no event: the events start from here.
    if (!m_watches.contains(session.serial())) {
        SessionWatch& watch = m_watches[session.serial()];
        watch.name = session.name();
        session.setWatched(true, false);
        for (const auto& pane : session.panes())
            watch.panes[pane->id()] = PaneWatch{pane->title(), pane->status()};
        watch.focused = session.focusedPane();
    }
    // The events go to the subscription by the id of its request.
    m_subscriptions.push_back(Subscription{&connection, id, session.serial(), request.signal});
    // The panes keep the changes of their text only for a watcher of them
    // (pumpWatchOf() stops that when the last one goes).
    session.setWatched(true, textWatched(session.serial()));
    // A client that ends its side still hears its events, until they end.
    connection.keepAtEnd(true);
    return ipcReply(id, nullptr);
}

std::string Daemon::unsubscribe(const IpcRequest& request, IpcServerConnection& connection)
{
    const bool any = std::any_of(m_subscriptions.begin(), m_subscriptions.end(), [&](const Subscription& s) {
        return s.connection == &connection;
    });
    if (!any)
        return ipcError(request.id, IpcErrorCode::NoSuchSubscription, "no subscriptions on this connection");
    const auto it = std::find_if(m_subscriptions.begin(), m_subscriptions.end(), [&](const Subscription& s) {
        return s.connection == &connection && s.id == request.subscriptionId;
    });
    if (it == m_subscriptions.end()) {
        return ipcError(request.id, IpcErrorCode::NoSuchSubscription,
                        "unknown subscriptionId " + std::to_string(request.subscriptionId));
    }
    m_subscriptions.erase(it);
    // The answer goes before the connection is let go: a client that ended
    // its side gets it, then the connection closes. Sent here, so read()
    // sends nothing more.
    sendTo(connection, ipcReply(request.id.value_or(0), nullptr));
    holdForAnswers(connection);
    return {};
}

bool Daemon::textWatched(std::uint64_t serial) const
{
    return std::any_of(m_subscriptions.begin(), m_subscriptions.end(), [serial](const Subscription& s) {
        return s.session == serial && s.signal == "textChanged";
    });
}

void Daemon::forgetWatches(const IpcServerConnection* connection)
{
    std::erase_if(m_subscriptions, [connection](const Subscription& s) {
        return s.connection == connection;
    });
}

void Daemon::sendWatch(std::uint64_t serial, const SessionWatch& watch, const std::string& signal, Json::Array args)
{
    // Read access as the event goes out, also for one that waited.
    if (!watch.readText) {
        if (signal == "userVarChanged" && args.size() > 2)
            args[2] = "";
        if ((signal == "commandStarted" || signal == "commandFinished") && args.size() > 1)
            args[1] = "";
    }
    for (const Subscription& s : m_subscriptions) {
        if (s.session != serial || s.signal != signal)
            continue;
        // A watcher whose event fails (out of memory) loses its connection,
        // which tells it that it missed one. The others still get theirs.
        if (!contained("an event of afterglow ctl watch", [&] {
                sendTo(*s.connection, ipcEvent(s.id, args));
            }))
            s.connection->close();
    }
}

void Daemon::flushPane(std::uint64_t serial, SessionWatch& watch, std::uint32_t pane)
{
    const auto now = std::chrono::steady_clock::now();
    for (auto& [key, merged] : watch.merged) {
        if (merged.pane != pane || !merged.waiting)
            continue;
        merged.sent = now;
        sendWatch(serial, watch, merged.signal, std::exchange(merged.waiting, std::nullopt).value());
    }
}

void Daemon::watchNow(std::uint64_t serial, SessionWatch& watch, std::uint32_t pane, const std::string& signal,
                      Json::Array args)
{
    // What waits for the pane goes first, as a window sends it.
    flushPane(serial, watch, pane);
    sendWatch(serial, watch, signal, std::move(args));
}

void Daemon::watchMerged(std::uint64_t serial, SessionWatch& watch, std::uint32_t pane, const std::string& key,
                         const std::string& signal, Json::Array args)
{
    // At most one event of a kind for a pane in Control.eventInterval: the
    // newest waits for the end of it, as in a window.
    std::string name = signal + '\0' + std::to_string(pane) + '\0' + key;
    if (watch.interval.count() == 0) {
        // An older one that waits from an interval that ended now would go
        // after this one: it is no news.
        watch.merged.erase(name);
        return sendWatch(serial, watch, signal, std::move(args));
    }
    const auto now = std::chrono::steady_clock::now();
    Merged& merged = watch.merged[std::move(name)];
    merged.signal = signal;
    merged.pane = pane;
    if (merged.waiting || (merged.sent && now - *merged.sent < watch.interval)) {
        merged.waiting = std::move(args);
        return;
    }
    merged.sent = now;
    sendWatch(serial, watch, signal, std::move(args));
}

void Daemon::watchEvent(std::uint64_t serial, SessionWatch& watch, std::uint32_t pane, PaneWatch& state,
                        MuxPane::WatchEvent& event)
{
    using Kind = MuxPane::WatchEvent::Kind;
    switch (event.kind) {
    case Kind::Title:
        if (state.title == event.text)
            break;
        state.title = event.text;
        watchMerged(serial, watch, pane, {}, "titleChanged", Json::Array{std::int64_t(pane), std::move(event.text)});
        break;
    case Kind::Status: {
        std::pair<std::string, std::string> status{std::move(event.name), std::move(event.text)};
        if (state.status == status)
            break;
        state.status = status;
        watchMerged(serial, watch, pane, {}, "statusChanged",
                    Json::Array{std::int64_t(pane), status.first, status.second});
        break;
    }
    case Kind::Bell:
        watchMerged(serial, watch, pane, {}, "bell", Json::Array{std::int64_t(pane)});
        break;
    case Kind::Text:
        watchMerged(serial, watch, pane, {}, "textChanged", Json::Array{std::int64_t(pane)});
        break;
    case Kind::UserVar:
        watchMerged(serial, watch, pane, event.name, "userVarChanged",
                    Json::Array{std::int64_t(pane), event.name, std::move(event.text)});
        break;
    case Kind::CommandStarted:
        watchNow(serial, watch, pane, "commandStarted", Json::Array{std::int64_t(pane), std::move(event.text)});
        break;
    case Kind::CommandFinished:
        watchNow(
            serial, watch, pane, "commandFinished",
            Json::Array{std::int64_t(pane), std::move(event.text), event.exitCode, std::int64_t(event.milliseconds)});
        break;
    case Kind::Shell:
        // At once, as the end of a command: a client waits for the prompt.
        // The same state again is a new prompt.
        watchNow(serial, watch, pane, "shellChanged", Json::Array{std::int64_t(pane), std::move(event.name)});
        break;
    }
}

void Daemon::flushWatch(const Session& session)
{
    // As the step of the loop, which fails watched session by watched
    // session (pumpWatch()): the session ends all the same.
    if (m_watches.contains(session.serial()))
        pumpWatch();
}

void Daemon::pumpWatch()
{
    const auto now = std::chrono::steady_clock::now();
    for (auto it = m_watches.begin(); it != m_watches.end();) {
        const auto watched = it;
        // pumpWatchOf() erases only this one.
        ++it;
        const std::uint64_t serial = watched->first;
        // The watchers of a session whose events fail (out of memory) lose
        // their connections, which tells them that they missed events. The
        // watchers of the other sessions still get theirs, and the next pass
        // ends the watch without watchers.
        if (!contained("afterglow ctl watch", [&] {
                pumpWatchOf(watched, now);
            })) {
            for (const Subscription& s : m_subscriptions) {
                if (s.session == serial)
                    s.connection->close();
            }
        }
    }
}

void Daemon::pumpWatchOf(std::map<std::uint64_t, SessionWatch>::iterator it, std::chrono::steady_clock::time_point now)
{
    const std::uint64_t serial = it->first;
    SessionWatch& watch = it->second;
    Session* const session = sessionBySerial(serial);
    const bool open = session && session->options().control.enabled;
    // The session ended, or its control target went: the subscriptions end
    // with an error. The panes of a session that ended close for them first.
    // Those of one that turned control off do not: they run on, and a
    // window's control target drops what waits for them.
    if (!open) {
        if (!session) {
            for (const auto& [pane, state] : watch.panes)
                watchNow(serial, watch, pane, "paneClosed", Json::Array{std::int64_t(pane)});
        }
        const std::string code = session ? IpcErrorCode::NoSuchTarget : Error::NoSuchSession;
        const std::string message = session ? "no target 'control'" : "session '" + watch.name + "' ended";
        for (std::size_t i = 0; i < m_subscriptions.size();) {
            if (m_subscriptions[i].session != serial) {
                ++i;
                continue;
            }
            IpcServerConnection* const connection = m_subscriptions[i].connection;
            const std::int64_t id = m_subscriptions[i].id;
            m_subscriptions.erase(m_subscriptions.begin() + std::ptrdiff_t(i));
            // Out of memory, the watcher's connection closes, which tells
            // it.
            if (!contained("an event of afterglow ctl watch", [&] {
                    sendTo(*connection, ipcError(id, code, message));
                }))
                connection->close();
            holdForAnswers(*connection);
        }
    }
    const bool subscribed =
        std::any_of(m_subscriptions.begin(), m_subscriptions.end(), [serial](const Subscription& s) {
            return s.session == serial;
        });
    if (!open || !subscribed) {
        if (session)
            session->setWatched(false, false);
        m_watches.erase(it);
        return;
    }
    session->setWatched(true, textWatched(serial));
    watch.readText = session->options().control.allowReadText;
    watch.interval = std::chrono::milliseconds(session->options().control.eventInterval);
    std::set<std::uint32_t> live;
    for (const auto& pane : session->panes()) {
        const std::uint32_t id = pane->id();
        live.insert(id);
        if (!watch.panes.contains(id)) {
            // A pane that started or came since: what it tells from now
            // on is news, its first title too (Session::addPane).
            watch.panes[id] = PaneWatch{};
            watchNow(serial, watch, id, "paneCreated", Json::Array{std::int64_t(id)});
        }
        PaneWatch& state = watch.panes[id];
        for (MuxPane::WatchEvent& event : pane->takeWatchEvents())
            watchEvent(serial, watch, id, state, event);
    }
    // What the panes that left since told before they left (an exit, a
    // close, a move to another session): before their end below.
    for (Session::LeftEvents& left : session->takeLeftEvents()) {
        const auto known = watch.panes.find(left.pane);
        if (known == watch.panes.end())
            continue;
        for (MuxPane::WatchEvent& event : left.events)
            watchEvent(serial, watch, left.pane, known->second, event);
    }
    for (auto known = watch.panes.begin(); known != watch.panes.end();) {
        if (live.contains(known->first)) {
            ++known;
            continue;
        }
        const std::uint32_t pane = known->first;
        watchNow(serial, watch, pane, "paneClosed", Json::Array{std::int64_t(pane)});
        std::erase_if(watch.merged, [pane](const auto& merged) {
            return merged.second.pane == pane;
        });
        known = watch.panes.erase(known);
    }
    // The moves of a window's layout and of `afterglow ctl`, in their order.
    for (const std::uint32_t pane : session->takeMoved()) {
        if (live.contains(pane))
            watchNow(serial, watch, pane, "paneMoved", Json::Array{std::int64_t(pane)});
    }
    const std::uint32_t focused = session->focusedPane();
    if (focused != watch.focused && focused != 0)
        watchNow(serial, watch, focused, "focusChanged", Json::Array{std::int64_t(focused)});
    watch.focused = focused;
    // What waited for its interval goes. An interval that passed with
    // nothing waiting leaves no entry.
    for (auto merged = watch.merged.begin(); merged != watch.merged.end();) {
        const bool over = !merged->second.sent || now - *merged->second.sent >= watch.interval;
        if (over && merged->second.waiting) {
            merged->second.sent = now;
            sendWatch(serial, watch, merged->second.signal,
                      std::exchange(merged->second.waiting, std::nullopt).value());
        } else if (over) {
            merged = watch.merged.erase(merged);
            continue;
        }
        ++merged;
    }
}

int Daemon::watchDueMs() const
{
    const auto now = std::chrono::steady_clock::now();
    int due = -1;
    for (const auto& [serial, watch] : m_watches) {
        for (const auto& [key, merged] : watch.merged) {
            // An entry ends at the end of its interval, whether it waits or not.
            const auto left = merged.sent
                ? std::chrono::duration_cast<std::chrono::milliseconds>(*merged.sent + watch.interval - now).count()
                : 0;
            const int ms = int(std::max<std::int64_t>(0, left)) + 1;
            if (due < 0 || ms < due)
                due = ms;
        }
    }
    return due;
}

} // namespace AfterglowMux
