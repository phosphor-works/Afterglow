// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Control.h"

#include "../Pane.h"
#include "../PaneOverlay.h"
#include "../ShownText.h"
#include "../TerminalWindow.h"
#include "../app/NotificationCenter.h"
#include "../app/WindowStart.h"
#include "../config/Keybinds.h"
#include "../config/Settings.h"
#include "../mux/MuxClient.h"

#include <AfterglowPty/Pty.h>
#include <AfterglowVt/KeyNames.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <mutex>
#include <system_error>
#include <thread>

namespace Afterglow {

/// Where the calls of offThread give their results: the Control while it
/// lives. A call to the daemon can wait for seconds, and a quit waits for
/// it only a bounded time.
struct ControlMailbox
{
    std::mutex mutex;
    Control* control = nullptr;
};

Control::Control(SettingsStore& settings, WindowFactory newWindow, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_newWindow(std::move(newWindow))
    , m_mailbox(std::make_shared<ControlMailbox>())
{
    m_mailbox->control = this;
    m_encoder.setMaxThreadCount(1);
    // The overlays and inputs of the clients of sessions end with control,
    // as those of the control socket do, whether or not that socket holds
    // its target.
    connect(&settings, &SettingsStore::changed, this, [this](const Settings& previous) {
        if (previous.control.enabled && !m_settings.settings().control.enabled)
            endSessionClients(QStringLiteral("Control.enabled is off"));
    });
}

namespace {

using Neighbor = SplitTree::Neighbor;

/// The sides of PaneInfo::neighbors, in their order.
constexpr std::array<Neighbor, 4> Neighbors{Neighbor::Left, Neighbor::Right, Neighbor::Up, Neighbor::Down};

QString neighborName(Neighbor side)
{
    switch (side) {
    case Neighbor::Left:
        return QStringLiteral("left");
    case Neighbor::Right:
        return QStringLiteral("right");
    case Neighbor::Up:
        return QStringLiteral("up");
    case Neighbor::Down:
        break;
    }
    return QStringLiteral("down");
}

std::optional<Neighbor> neighborNamed(const QString& name)
{
    for (const Neighbor side : Neighbors) {
        if (neighborName(side) == name)
            return side;
    }
    return std::nullopt;
}

/// "no pane PHRASE the pane".
QString sidePhrase(Neighbor side)
{
    switch (side) {
    case Neighbor::Left:
        return QStringLiteral("to the left of");
    case Neighbor::Right:
        return QStringLiteral("to the right of");
    case Neighbor::Up:
        return QStringLiteral("above");
    case Neighbor::Down:
        break;
    }
    return QStringLiteral("below");
}

} // namespace

bool Control::isWhole(double value)
{
    return std::isfinite(value) && std::floor(value) == value;
}

QString Control::noPane(quint32 id)
{
    return QStringLiteral("no pane %1").arg(id);
}

void Control::attachWindow(TerminalWindow* window)
{
    m_windows.removeAll(nullptr);
    m_windows.append(window);
    connect(window, &QWindow::activeChanged, this, [this, guard = QPointer<TerminalWindow>(window)] {
        if (guard && guard->isActive())
            m_lastActive = guard;
    });
    connect(window, &TerminalWindow::paneClosing, this, [this](quint32 pane, quint32 tab, quint32 window) {
        m_closingPanes.insert(pane, {tab, window});
    });
    connect(window, &TerminalWindow::projectRequested, this, &Control::projectDialog);
    connect(window, &TerminalWindow::paneMoved, this, [this](Pane* p) {
        Q_EMIT paneMoved(p->id());
    });
    connect(window, &TerminalWindow::transferRequested, this, [this](Pane* pane, bool tab) {
        const QPointer<Pane> guard(pane);
        const quint32 id = pane->id();
        const PaneMoved moved = [this, guard, id](const QString& error, const QVariantMap&) {
            // A move out of a session that failed after the daemon took the
            // panes left the pane in no window: the notice names none.
            if (!error.isEmpty())
                NotificationCenter::instance().showNotice(guard && find(id) ? guard.data() : nullptr,
                                                          QStringLiteral("Move failed"), textPrefix(error, 2048));
        };
        if (tab)
            detachTab(id, moved);
        else
            movePane(id, 0, QStringLiteral("new-window"), moved);
    });
    connect(window, &TerminalWindow::paneCreated, this, &Control::attachPane);
    // The window of a session: what its clients left goes with it, as no
    // daemon tells it of them any more. Its overlays close with it. The
    // owners are the window's own, not the session's: a window that shows
    // the session now keeps its clients.
    if (window->showsSession()) {
        connect(window, &QObject::destroyed, this, [this, prefix = sessionClientOwner(window->id())] {
            m_sessionInputs.dropPrefix(prefix);
            m_sessionOverlayOwners.removeIf([&prefix](const QString& owner) {
                return owner.startsWith(prefix);
            });
        });
    }
    connect(window, &TerminalWindow::scriptKey, this, &Control::scriptKey);
    connect(window, &TerminalWindow::scriptDockEvent, this, &Control::scriptDockEvent);
    connect(window, &TerminalWindow::tabSwitched, this, [this, window](quint32 from, quint32 to, int direction) {
        Q_EMIT tabSwitched(window->focusedPane() ? window->focusedPane()->id() : 0, from, to, direction);
    });
    connect(window, &TerminalWindow::focusedPaneChanged, this, [this](Pane* p) {
        if (p) {
            Q_EMIT focusChanged(p->id());
        }
    });
    // Panes made before the window was handed over.
    for (Pane* p : window->panes()) {
        attachPane(p);
    }
}

void Control::attachPane(Pane* pane)
{
    const quint32 id = pane->id();
    // A pane made before its window was handed over may have a context already.
    m_context.insert(id, pane->contextLabel());
    connect(pane, &QObject::destroyed, this, [this, id] {
        m_status.remove(id);
        m_context.remove(id);
        const auto identity = m_closingPanes.take(id);
        Q_EMIT paneClosedDetailed(id, identity.first, identity.second);
        Q_EMIT paneClosed(id);
    });
    connect(pane, &Pane::titleUpdated, this, [this, id](const QString& t) {
        Q_EMIT titleChanged(id, t);
    });
    connect(pane, &Pane::bellRang, this, [this, id] {
        Q_EMIT bell(id);
    });
    connect(pane, &Pane::userVarUpdated, this, [this, id](const QString& name, const QString& value) {
        Q_EMIT userVarChanged(id, name, value);
    });
    connect(pane, &Pane::contextUpdated, this, [this, id, guard = QPointer<Pane>(pane)] {
        if (!guard) {
            return;
        }
        // The pane also tells of a new kind with the same label, and of a
        // change that went back before this delivery.
        const QString label = guard->contextLabel();
        if (const auto last = m_context.constFind(id); last != m_context.cend() && *last == label)
            return;
        m_context.insert(id, label);
        Q_EMIT contextChanged(id, label);
    });
    // The pane emits these on the GUI thread, when it delivers the terminal
    // events that its reader thread queued.
    connect(pane, &Pane::commandBegan, this, [this, id, guard = QPointer<Pane>(pane)](const QString& c) {
        if (guard)
            Q_EMIT commandStarted(id, c);
    });
    connect(pane, &Pane::commandEnded, this,
            [this, id, guard = QPointer<Pane>(pane)](const QString& c, int code, qint64 ms) {
                if (guard)
                    Q_EMIT commandFinished(id, c, code, ms);
            });
    connect(pane, &Pane::statusChanged, this, [this, id, guard = QPointer<Pane>(pane)] {
        if (!guard) {
            return;
        }
        // The newest record's state and text stand for the pane. A record
        // also changes for progress alone, which this signal does not carry.
        // With no record left (cleared, or a reset), the pane is idle.
        QString state = statusStateName(AfterglowVt::StatusState::Idle);
        QString text;
        quint64 serial = 0;
        for (const PaneStatusRecord& r : guard->status()) {
            if (r.serial >= serial) {
                serial = r.serial;
                state = statusStateName(r.state);
                text = r.message.isEmpty() ? r.title : r.message;
            }
        }
        const QPair<QString, QString> current{state, text};
        if (const auto last = m_status.constFind(id); last != m_status.cend() && *last == current)
            return;
        m_status.insert(id, current);
        Q_EMIT statusChanged(id, state, text);
    });
    Q_EMIT paneCreated(id);
}

bool Control::setRemoteSocket(quint32 pane, const AfterglowProto::Remote::Answer& answer,
                              const std::optional<AfterglowProto::Remote::Answer>& overlay)
{
    Pane* target = find(pane);
    if (!target)
        return false;
    target->setRemoteSocket(answer, overlay);
    return true;
}

Pane* Control::find(quint32 id, TerminalWindow** window) const
{
    for (const QPointer<TerminalWindow>& w : m_windows) {
        if (!w) {
            continue;
        }
        for (Pane* p : w->panes()) {
            if (p->id() == id) {
                if (window) {
                    *window = w;
                }
                return p;
            }
        }
    }
    return nullptr;
}

PaneInfo Control::describe(TerminalWindow* w, Pane* p, bool facts, bool cmdline)
{
    PaneInfo info;
    info.id = p->id();
    info.window = w->id();
    info.tab = w->tabIndexOf(p);
    info.title = p->title();
    info.workingDirectory = p->knownDirectory();
    info.command = p->runningCommand();
    info.cols = p->cols();
    info.rows = p->rows();
    info.focused = w->isActive() && w->focusedPane() == p;
    info.context = p->contextLabel();
    info.tabId = w->tabIdOf(p);
    if (!facts)
        return info;
    info.facts = true;
    info.atPrompt = p->atShellPrompt();
    info.userVars = p->userVars();
    if (const auto process = p->foreground(cmdline)) {
        PaneInfo::Foreground foreground{process->pid, {}, QString::fromStdString(process->cwd)};
        for (const std::string& argument : process->cmdline)
            foreground.cmdline << QString::fromStdString(argument);
        info.foreground = std::move(foreground);
    }
    // One layout for the four sides. Neighbors is in the order of
    // SplitTree::Neighbor, which neighborsOf() indexes by.
    const std::array<Pane*, 4> around = w->neighborsOf(p);
    for (std::size_t side = 0; side < info.neighbors.size(); ++side) {
        if (const Pane* n = around[std::size_t(Neighbors[side])])
            info.neighbors[side] = n->id();
    }
    return info;
}

QList<PaneInfo> Control::panes(bool facts, bool cmdlines) const
{
    QList<PaneInfo> out;
    for (const QPointer<TerminalWindow>& w : m_windows) {
        if (!w) {
            continue;
        }
        for (Pane* p : w->panes()) {
            out.append(describe(w, p, facts, cmdlines));
        }
    }
    return out;
}

std::optional<PaneInfo> Control::paneInfo(quint32 id) const
{
    // Each pane's directory and size cost syscalls and its lock: describe only this one.
    TerminalWindow* window = nullptr;
    Pane* pane = find(id, &window);
    return pane ? std::optional(describe(window, pane)) : std::nullopt;
}

bool Control::runsIn(quint32 id, qint64 process, bool overlay) const
{
    // A terminal in a terminal (tmux, an editor) and a shell's nesting are
    // far fewer.
    constexpr int MaxAncestors = 64;
    const Pane* pane = find(id);
    if (pane && overlay)
        pane = pane->overlay() ? pane->overlay()->pane() : nullptr;
    const pid_t leader = pane ? pane->programPid() : 0;
    if (leader <= 0 || process <= 1 || process > std::numeric_limits<pid_t>::max())
        return false;
    pid_t at = pid_t(process);
    for (int up = 0; at > 1 && up < MaxAncestors; ++up) {
        const auto stat = AfterglowPty::processStat(at);
        if (!stat)
            return false;
        if (stat->session == leader)
            return true;
        at = pid_t(stat->parent);
    }
    return false;
}

void Control::endSessionClient(const QString& owner, const QString& message)
{
    m_sessionInputs.drop(owner);
    if (m_sessionOverlayOwners.remove(owner))
        closeOverlays(owner, message);
}

void Control::endSessionClients(const QString& message)
{
    m_sessionInputs.clear();
    for (const QString& owner : std::exchange(m_sessionOverlayOwners, {}))
        closeOverlays(owner, message);
}

Control::~Control()
{
    m_closing->store(true);
    const std::lock_guard lock(m_mailbox->mutex);
    m_mailbox->control = nullptr;
}

QString Control::startOffThread(std::function<OffThreadResult()> work, std::function<void(OffThreadResult)> then)
{
    // Each call waits for the daemon on a thread of its own, for seconds at
    // most: a bound, as the pool before it had, for clients and scripts
    // that open projects in a loop.
    if (m_offThread.size() >= MaxOffThreadCalls) {
        return QStringLiteral("cannot call the mux daemon: %1 calls wait for it already").arg(MaxOffThreadCalls);
    }
    const quint64 id = ++m_lastOffThread;
    m_offThread.insert(id, std::move(then));
    try {
        std::thread([counted = MuxClient::CallThread(), mailbox = m_mailbox, id, work = std::move(work)]() mutable {
            // Counted to the end of the body: a quit waits for it
            // (MuxClient::waitForCallThreads), before the statics go.
            const MuxClient::CallThread running = std::move(counted);
            OffThreadResult result = work();
            const std::lock_guard lock(mailbox->mutex);
            if (!mailbox->control)
                return;
            QMetaObject::invokeMethod(
                mailbox->control,
                [control = mailbox->control, id, result] {
                    control->offThreadDone(id, result);
                },
                Qt::QueuedConnection);
        }).detach();
    } catch (const std::system_error& e) {
        m_offThread.remove(id);
        return QStringLiteral("cannot call the mux daemon: %1").arg(QString::fromLocal8Bit(e.what()));
    }
    return {};
}

void Control::offThreadDone(quint64 id, OffThreadResult result)
{
    if (auto then = m_offThread.take(id))
        then(std::move(result));
}

std::optional<quint32> Control::focusedPane() const
{
    for (const QPointer<TerminalWindow>& w : m_windows) {
        if (w && w->isActive() && w->focusedPane())
            return w->focusedPane()->id();
    }
    if (m_lastActive && m_lastActive->focusedPane())
        return m_lastActive->focusedPane()->id();
    return std::nullopt;
}

QString Control::statusStateName(AfterglowVt::StatusState state)
{
    using AfterglowVt::StatusState;
    switch (state) {
    case StatusState::Working:
        return QStringLiteral("working");
    case StatusState::Blocked:
        return QStringLiteral("blocked");
    case StatusState::Done:
        return QStringLiteral("done");
    case StatusState::Error:
        return QStringLiteral("error");
    case StatusState::Idle:
        break;
    }
    return QStringLiteral("idle");
}

QVariantMap Control::paneMap(const PaneInfo& p)
{
    QVariantMap map{{QStringLiteral("id"), p.id},           {QStringLiteral("window"), p.window},
                    {QStringLiteral("tab"), p.tab},         {QStringLiteral("tab_id"), p.tabId},
                    {QStringLiteral("title"), p.title},     {QStringLiteral("cwd"), p.workingDirectory},
                    {QStringLiteral("command"), p.command}, {QStringLiteral("cols"), p.cols},
                    {QStringLiteral("rows"), p.rows},       {QStringLiteral("focused"), p.focused},
                    {QStringLiteral("context"), p.context}};
    if (!p.facts)
        return map;
    QVariantMap userVars;
    for (auto it = p.userVars.constBegin(); it != p.userVars.constEnd(); ++it)
        userVars.insert(it.key(), it.value());
    // JSON null for none.
    QVariant foreground = QVariant::fromValue(nullptr);
    if (p.foreground) {
        foreground = QVariantMap{{QStringLiteral("pid"), p.foreground->pid},
                                 {QStringLiteral("cmdline"), p.foreground->cmdline},
                                 {QStringLiteral("cwd"), p.foreground->cwd}};
    }
    QVariantMap neighbors;
    for (std::size_t side = 0; side < p.neighbors.size(); ++side)
        neighbors.insert(neighborName(Neighbors[side]), p.neighbors[side]);
    map.insert(QStringLiteral("at_prompt"), p.atPrompt);
    map.insert(QStringLiteral("user_vars"), userVars);
    map.insert(QStringLiteral("foreground"), foreground);
    map.insert(QStringLiteral("neighbors"), neighbors);
    return map;
}

QVariantMap Control::state() const
{
    QVariantList windows, tabs, paneList;
    quint32 activeWindow = 0, activeTab = 0, activePane = 0;
    for (const auto& w : m_windows) {
        if (!w) {
            continue;
        }
        const quint32 focused = w->focusedPane() ? w->focusedPane()->id() : 0;
        const QSize grid = w->gridSize();
        windows.append(QVariantMap{{QStringLiteral("id"), w->id()},
                                   {QStringLiteral("project"), w->projectName()},
                                   {QStringLiteral("project_directory"), w->projectDirectory()},
                                   {QStringLiteral("focused"), w->isActive()},
                                   {QStringLiteral("active_tab"), w->activeTabId()},
                                   {QStringLiteral("active_pane"), focused},
                                   {QStringLiteral("overview"), w->overviewState()},
                                   {QStringLiteral("session"), w->sessionName()},
                                   {QStringLiteral("session_host"), w->sessionHost()},
                                   {QStringLiteral("cols"), grid.width()},
                                   {QStringLiteral("rows"), grid.height()}});
        for (const auto& tab : w->tabs()) {
            tabs.append(QVariantMap{{QStringLiteral("id"), tab.id},
                                    {QStringLiteral("window"), w->id()},
                                    {QStringLiteral("index"), tab.index},
                                    {QStringLiteral("title"), tab.title},
                                    {QStringLiteral("active"), tab.id == w->activeTabId()},
                                    {QStringLiteral("active_pane"), tab.activePane}});
        }
        if (w->isActive()) {
            activeWindow = w->id();
            activeTab = w->activeTabId();
            activePane = focused;
        }
    }
    for (const auto& p : panes()) {
        paneList.append(paneMap(p));
    }
    return {{QStringLiteral("windows"), windows},      {QStringLiteral("tabs"), tabs},
            {QStringLiteral("panes"), paneList},       {QStringLiteral("active_window"), activeWindow},
            {QStringLiteral("active_tab"), activeTab}, {QStringLiteral("active_pane"), activePane}};
}

bool Control::isStructuralAction(const QString& action)
{
    return Keybinds::isStructural(Keybinds::fromToken(action));
}

QString Control::sendText(quint32 id, const QString& text)
{
    Pane* p = find(id);
    if (!p) {
        return noPane(id);
    }
    // The caller hears of a refusal; the user gets no notice as well.
    return p->sendInput(text.toStdString(), false) ? QString() : QStringLiteral("program closed or input queue full");
}

QString Control::focus(quint32 id)
{
    TerminalWindow* w = nullptr;
    Pane* p = find(id, &w);
    if (!p) {
        return noPane(id);
    }
    w->revealPane(p);
    return {};
}

QString Control::close(quint32 id)
{
    TerminalWindow* w = nullptr;
    Pane* p = find(id, &w);
    if (!p) {
        return noPane(id);
    }
    w->requestClosePane(p);
    return {};
}

QString Control::setTitle(quint32 id, const QString& title)
{
    Pane* p = find(id);
    if (!p) {
        return noPane(id);
    }
    // It shows in the tab bar, and a session's layout keeps it: no control
    // characters, and nothing that disguises text.
    p->setUserTitle(sanitizedLine(title, 256));
    return {};
}

QString Control::focusNeighbor(quint32 id, const QString& direction, quint32& focused)
{
    const auto side = neighborNamed(direction);
    if (!side)
        return badDirection();
    TerminalWindow* w = nullptr;
    Pane* p = find(id, &w);
    if (!p)
        return noPane(id);
    Pane* n = w->neighborOf(p, *side);
    // An error at the edge: a client such as smart-splits.nvim then passes
    // the move on, or wraps.
    // No ID in the words: a client of a session knows the pane by the
    // daemon's.
    if (!n)
        return QStringLiteral("no pane %1 the pane").arg(sidePhrase(*side));
    w->revealPane(n);
    focused = n->id();
    return {};
}

QString Control::resizePane(quint32 id, const QString& direction, double cells)
{
    const auto side = neighborNamed(direction);
    if (!side)
        return badDirection();
    if (!isWhole(cells) || cells < 1 || cells > MaxResizeCells)
        return QStringLiteral("cells is a whole number from 1 to %1").arg(MaxResizeCells);
    TerminalWindow* w = nullptr;
    Pane* p = find(id, &w);
    if (!p)
        return noPane(id);
    if (!w->resizePane(p, *side, int(cells)))
        return QStringLiteral("the pane has no edge that can move %1").arg(neighborName(*side));
    return {};
}

QString Control::sendKeys(quint32 id, const QStringList& keys)
{
    if (keys.isEmpty())
        return QStringLiteral("sendKeys needs a key");
    if (std::size_t(keys.size()) > AfterglowVt::MaxKeysAtOnce)
        return QStringLiteral("sendKeys sends at most %1 keys at once").arg(AfterglowVt::MaxKeysAtOnce);
    // Every name first: a bad one sends none of the keys.
    std::vector<AfterglowVt::NamedKey> named;
    named.reserve(std::size_t(keys.size()));
    for (const QString& name : keys) {
        auto key = AfterglowVt::parseKeyName(name.toStdString());
        if (!key)
            return QStringLiteral("no key is named '%1'").arg(name);
        named.push_back(std::move(*key));
    }
    Pane* p = find(id);
    if (!p)
        return noPane(id);
    // Encoded for the program's keyboard mode, as a typed key is.
    const std::string bytes = p->readTerminal([&named](const AfterglowVt::Terminal& t) {
        std::string out;
        for (const AfterglowVt::NamedKey& key : named)
            out += AfterglowVt::encodeNamedKey(key, t.modes());
        return out;
    });
    if (bytes.empty())
        return {};
    return p->sendInput(bytes, false) ? QString() : QStringLiteral("program closed or input queue full");
}

QString Control::scroll(quint32 id, const QString& unit, double amount)
{
    if (!isWhole(amount) || std::abs(amount) > double(MaxScrollAmount))
        return QStringLiteral("the amount is a whole number from -%1 to %1").arg(MaxScrollAmount);
    TerminalWindow* w = nullptr;
    Pane* p = find(id, &w);
    if (!p)
        return noPane(id);
    return w->scrollFromControl(p, unit, qint64(amount));
}

QString Control::scriptDialog(const QString& owner, quint32 id, const QVariantMap& effect, ScriptReply reply)
{
    TerminalWindow* window = nullptr;
    auto* pane = find(id, &window);
    return pane ? window->showScriptDialog(owner, pane, effect, std::move(reply)) : noPane(id);
}
QString Control::scriptAppearance(const QString& owner, quint32 id, const QVariantMap& effect)
{
    TerminalWindow* window = nullptr;
    auto* pane = find(id, &window);
    return pane ? window->setScriptAppearance(owner, pane, effect) : noPane(id);
}
QString Control::scriptDock(const QString& owner, quint32 id, const QVariantMap& effect)
{
    // Every dock and undock comes through here, so the records match the
    // windows' areas. A closed window's records go.
    m_docks.removeIf([](const DockRecord& r) {
        return !r.window;
    });
    // A pane's areas go where it goes, and close with it: no record.
    if (effect.value(QStringLiteral("scope")) == QLatin1String("pane")) {
        TerminalWindow* window = nullptr;
        Pane* pane = find(id, &window);
        return pane ? window->setScriptDock(owner, effect, pane) : noPane(id);
    }
    const QString dock = effect.value(QStringLiteral("id")).toString();
    auto record = [&](auto matches) {
        return std::find_if(m_docks.begin(), m_docks.end(), [&](const DockRecord& r) {
            return r.owner == owner && r.id == dock && matches(r);
        });
    };
    TerminalWindow* window = nullptr;
    const bool open = find(id, &window);
    if (effect.value(QStringLiteral("kind")) == QLatin1String("undock")) {
        // The pane's window first. Else the area this pane docked, which
        // stays in its window when the pane moves away or closes.
        auto it = open ? record([window](const DockRecord& r) {
            return r.window == window;
        })
                       : m_docks.end();
        if (it == m_docks.end()) {
            // The records are in the order they were last docked through,
            // so the last match is the area this pane docked last.
            const auto last = std::find_if(m_docks.rbegin(), m_docks.rend(), [&](const DockRecord& r) {
                return r.owner == owner && r.id == dock && r.pane == id;
            });
            if (last != m_docks.rend())
                it = std::prev(last.base());
        }
        if (it == m_docks.end())
            return open ? window->setScriptDock(owner, effect) : noPane(id);
        const QPointer<TerminalWindow> target = it->window;
        m_docks.erase(it);
        return target->setScriptDock(owner, effect);
    }
    if (!open)
        return noPane(id);
    const QString error = window->setScriptDock(owner, effect);
    if (error.isEmpty()) {
        // The newest dock goes last, for the undock above.
        if (auto it = record([window](const DockRecord& r) {
                return r.window == window;
            });
            it != m_docks.end())
            m_docks.erase(it);
        m_docks.append({owner, dock, window, id});
    }
    return error;
}
QString Control::tabMenu(quint32 id)
{
    TerminalWindow* window = nullptr;
    Pane* pane = find(id, &window);
    return pane ? window->openScriptTabMenu(pane) : noPane(id);
}
QString Control::moveTab(quint32 id, int index)
{
    TerminalWindow* window = nullptr;
    Pane* pane = find(id, &window);
    return pane ? window->moveTabOf(pane, index) : noPane(id);
}
QString Control::scriptShader(const QString& owner, quint32 id, const QVariantMap& effect, QVariantMap& result)
{
    TerminalWindow* window = nullptr;
    Pane* pane = find(id, &window);
    return pane ? window->scriptShader(owner, pane, effect, result) : noPane(id);
}

void Control::closeRunWaits(const QString& owner, const QString& prefix)
{
    for (auto window : m_windows)
        if (window)
            window->closeRunWaits(owner, prefix);
}

QString Control::overlay(const QString& owner, const QString& request, quint32 id, const OverlaySpec& spec,
                         OverlayReply reply, QString& code)
{
    TerminalWindow* window = nullptr;
    Pane* pane = find(id, &window);
    if (!pane) {
        code = QStringLiteral("no_pane");
        return noPane(id);
    }
    return window->openOverlay(owner, request, pane, spec, std::move(reply), code);
}

std::optional<OverlaySpec> Control::clientOverlaySpec(const QStringList& argv, const QVariantMap& options,
                                                      QString& error)
{
    auto spec = OverlaySpec::read(argv, options, &error);
    // A client names a directory of this machine: one that is missing is
    // its mistake, where a script's remote path starts in $HOME.
    if (spec)
        error = localDirectory(spec->cwd);
    if (!error.isEmpty())
        return std::nullopt;
    return spec;
}

QString Control::clientOverlay(const QString& owner, quint32 pane, const OverlaySpec& spec, OverlayReply reply,
                               QString& code)
{
    return overlay(
        owner, {}, pane, spec,
        [reply = std::move(reply), self = QPointer<Control>(this)](const QString& error, const QString& code,
                                                                   const QVariantMap& result) {
            if (!error.isEmpty())
                return reply(error, code, {});
            QVariantMap answer = result;
            if (!self || !self->m_settings.settings().control.allowReadText)
                answer.remove(QStringLiteral("stdout"));
            reply({}, {}, answer);
        },
        code);
}

void Control::closeOverlays(const QString& owner, const QString& message)
{
    for (auto window : m_windows)
        if (window)
            window->closeOverlays(owner, {}, message);
}

void Control::clearScriptOwner(const QString& owner)
{
    m_docks.removeIf([&owner](const DockRecord& r) {
        return r.owner == owner || !r.window;
    });
    for (auto window : m_windows)
        if (window)
            window->clearScriptOwner(owner);
}

QString Control::scriptKeyMode(const QString& owner, quint32 id, const QString& label)
{
    TerminalWindow* window = nullptr;
    Pane* pane = find(id, &window);
    if (!pane)
        return noPane(id);
    // One key mode per script: starting one elsewhere ends the old one, but
    // only once the new one holds the keyboard.
    const QString error = window->startScriptKeyMode(owner, pane, label);
    if (error.isEmpty()) {
        for (auto other : m_windows)
            if (other && other != window)
                other->endScriptKeyMode(owner);
    }
    return error;
}

void Control::endScriptKeyMode(const QString& owner)
{
    for (auto window : m_windows)
        if (window)
            window->endScriptKeyMode(owner);
}

QString Control::overview(const QString& owner, quint32 id, const QString& operation, QVariantMap& result,
                          QString* code)
{
    TerminalWindow* window = nullptr;
    Pane* pane = find(id, &window);
    return pane ? window->scriptOverview(owner, pane, operation, result, code) : noPane(id);
}

QString Control::overviewMove(quint32 id, quint32 target, int step, QVariantMap& result)
{
    TerminalWindow* window = nullptr;
    Pane* pane = find(id, &window);
    if (!pane)
        return noPane(id);
    TerminalWindow* targetWindow = nullptr;
    Pane* to = target ? find(target, &targetWindow) : nullptr;
    if (target && (!to || targetWindow != window))
        return QStringLiteral("pane %1 is not in this window's overview").arg(target);
    return window->scriptOverviewMove(pane, to, step, result);
}

QString Control::pulse(quint32 id, int effect, int durationMs, int color)
{
    TerminalWindow* window = nullptr;
    Pane* pane = find(id, &window);
    return pane ? window->pulsePane(pane, effect, durationMs, color) : noPane(id);
}

QVariantMap Control::paneOrigin(quint32 id) const
{
    TerminalWindow* window = nullptr;
    Pane* pane = find(id, &window);
    if (!pane)
        return {};
    const auto origin = window->paneOrigin(pane);
    if (!origin.from)
        return {};
    return {{QStringLiteral("split_from"), origin.from}, {QStringLiteral("edge"), origin.edge}};
}

void Control::movePane(quint32 id, quint32 target, const QString& placement, PaneMoved done)
{
    TerminalWindow *source = nullptr, *destination = nullptr;
    auto* pane = find(id, &source);
    if (!pane)
        return done(noPane(id), {});
    Pane* beside = target ? find(target, &destination) : nullptr;
    if (target && !beside)
        return done(noPane(target), {});
    const bool newTab = placement == QLatin1String("new-tab");
    const bool newWindow = placement == QLatin1String("new-window");
    const auto side = splitDirection(placement);
    if (!side && !newTab && !newWindow)
        return done(QStringLiteral("placement must be left, right, up, down, new-tab or new-window"), {});
    const SplitTree::Direction direction = side.value_or(SplitTree::Direction::Right);
    if (newWindow && target)
        return done(QStringLiteral("new-window takes no target pane"), {});
    if (!newTab && !newWindow && !beside)
        return done(QStringLiteral("a target pane is required"), {});
    if (!newTab && pane == beside)
        return done(QStringLiteral("cannot move a pane beside itself"), {});
    // A new tab goes to the window of its target, else to the pane's own.
    if (newTab && !beside)
        destination = source;
    // A session's panes run under its daemon: they leave in a session of
    // their own, which a window of its own shows.
    if (newWindow && source->showsSession())
        return breakOut(source, pane, false, std::move(done));
    // Or they join the session of another window, whose daemon moves them.
    if (destination && destination != source && source->showsSession() && destination->showsSession())
        return joinSession(source, pane, destination, beside, placement, std::move(done));
    if (newWindow)
        destination = m_newWindow ? m_newWindow({}, {}, true) : nullptr;
    if (!destination)
        return done(QStringLiteral("cannot open a window"), {});
    const auto error =
        source->movePaneTo(pane, destination, newTab ? nullptr : beside, direction, newTab ? beside : nullptr);
    // A window made for the move must not stay hidden and keep afterglow running.
    if (!error.isEmpty() && newWindow)
        destination->deleteLater();
    QVariantMap result;
    if (error.isEmpty())
        if (const auto info = paneInfo(id))
            result = paneMap(*info);
    done(error, result);
}

void Control::detachTab(quint32 id, PaneMoved done)
{
    TerminalWindow* source = nullptr;
    auto* pane = find(id, &source);
    if (!pane)
        return done(noPane(id), {});
    if (source->showsSession())
        return breakOut(source, pane, true, std::move(done));
    auto* destination = m_newWindow ? m_newWindow({}, {}, true) : nullptr;
    if (!destination)
        return done(QStringLiteral("cannot open a window"), {});
    const auto error = source->detachTabTo(pane, destination);
    if (!error.isEmpty())
        destination->deleteLater();
    QVariantMap result;
    if (error.isEmpty())
        if (const auto info = paneInfo(id))
            result = paneMap(*info);
    done(error, result);
}

void Control::breakOut(TerminalWindow* source, Pane* pane, bool tab, PaneMoved done)
{
    if (!m_openSessionWindow)
        return done(QStringLiteral("cannot open a window"), {});
    const std::uint32_t daemonPane = pane->sessionPaneId();
    // The new session is on the host of the one that the panes leave.
    const QString host = pane->session() ? pane->session()->host() : QString();
    // The daemon's call and the new window's attach run off the GUI thread.
    // What answers later finds this object gone when Afterglow quits: the
    // caller then hears nothing (its script, or its client, ends too).
    const auto showIt = [self = QPointer<Control>(this), daemonPane, host, tab,
                         done](const QString& error, const QString& session,
                               std::shared_ptr<const TerminalWindow::BreakOut> kept) {
        if (!self)
            return;
        if (!error.isEmpty())
            return done(error, {});
        const auto unshown = [tab, session](const QString& why) {
            // The daemon moved them already: what is left is to show them.
            return (tab ? QStringLiteral("the tab went to session %1, but no window can show it (%2)")
                        : QStringLiteral("the pane went to session %1, but no window can show it (%2)"))
                .arg(session, why);
        };
        if (!self->m_openSessionWindow)
            return done(unshown(QStringLiteral("cannot open a window")), {});
        self->m_openSessionWindow(
            session, host, [self, daemonPane, host, tab, session, kept, done, unshown](const QString& error) {
                if (!self)
                    return;
                if (!error.isEmpty())
                    return done(unshown(error), {});
                // The window that shows the new session, and in it the pane that
                // shows the same program.
                QVariantMap result;
                for (TerminalWindow* window : TerminalWindow::windows()) {
                    const QList<Pane*> shown = window->panes();
                    if (std::none_of(shown.cbegin(), shown.cend(), [&](const Pane* p) {
                            return p->showsSession(session, host);
                        }))
                        continue;
                    window->adoptBreakOut(kept);
                    for (Pane* p : shown) {
                        if (p->sessionPaneId() == daemonPane) {
                            if (const auto info = self->paneInfo(p->id()))
                                result = paneMap(*info);
                        }
                    }
                }
                // The window opened, but its pane is gone: its program ended
                // meanwhile. A caller that takes success would name the pane
                // that left.
                if (result.isEmpty()) {
                    return done((tab ? QStringLiteral("the tab went to session %1, but no window shows its pane")
                                     : QStringLiteral("the pane went to session %1, but no window shows it"))
                                    .arg(session),
                                {});
                }
                done({}, result);
            });
    };
    if (const QString refused = source->breakOutOfSession(pane, tab, showIt); !refused.isEmpty())
        done(refused, {});
}

void Control::joinSession(TerminalWindow* source, Pane* pane, TerminalWindow* destination, Pane* beside,
                          const QString& placement, PaneMoved done)
{
    // None for new-tab.
    const std::optional<SplitTree::Direction> side = splitDirection(placement);
    const std::uint32_t daemonPane = pane->sessionPaneId();
    // A project of the other session saves the command, as this one did.
    const QStringList command = pane->launchCommand();
    // The daemon's call runs off the GUI thread. What answers later finds
    // this object gone when Afterglow quits: the caller then hears nothing.
    const auto placed = [self = QPointer<Control>(this), destination = QPointer<TerminalWindow>(destination),
                         beside = QPointer<Pane>(beside), side, daemonPane, command,
                         done](const QString& error, const QString& session,
                               std::shared_ptr<const TerminalWindow::BreakOut> kept) {
        if (!self)
            return;
        if (!error.isEmpty())
            return done(error, {});
        if (!destination || destination->closing() || destination->sessionName() != session) {
            return done(QStringLiteral("the pane went to session %1, but its window no longer shows it").arg(session),
                        {});
        }
        // The daemon tells the window to place the pane too (placePane): the
        // first of the two places it, and the other finds it there.
        Pane* shown = destination->sessionPane(daemonPane);
        if (!shown) {
            Pane* const next = beside && destination->tabIndexOf(beside) >= 0 ? beside.data() : nullptr;
            shown = destination->placeSessionPane(daemonPane, next, side, command);
        }
        if (!shown)
            return done(QStringLiteral("the pane went to session %1, but its window cannot show it").arg(session), {});
        destination->adoptBreakOut(kept);
        destination->requestActivate();
        QVariantMap result;
        if (const auto info = self->paneInfo(shown->id()))
            result = paneMap(*info);
        done({}, result);
    };
    if (const QString refused = source->joinSession(pane, destination, beside, side, placed); !refused.isEmpty())
        done(refused, {});
}

void Control::showNew(TerminalWindow* window) const
{
    showForStartMode(*window, m_settings.settings().window);
}

} // namespace Afterglow
