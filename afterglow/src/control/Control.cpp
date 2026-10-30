// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Control.h"

#include "../Pane.h"
#include "../TerminalWindow.h"
#include "../app/NotificationCenter.h"
#include "../config/Keybinds.h"
#include "../config/Settings.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QUrl>

#include <cmath>
#include <limits>

namespace Afterglow {

Control::Control(SettingsStore& settings, WindowFactory newWindow, QObject* parent)
    : QObject(parent)
    , m_settings(settings)
    , m_newWindow(std::move(newWindow))
{
}

void Control::attachWindow(TerminalWindow* window)
{
    m_windows.append(window);
    connect(window, &TerminalWindow::projectRequested, this, &Control::projectDialog);
    connect(window, &TerminalWindow::paneMoved, this, [this](Pane* p) { Q_EMIT paneMoved(p->id()); });
    connect(window, &TerminalWindow::transferRequested, this, [this](Pane* pane, bool tab) {
        QVariantMap result;
        const auto error = tab ? detachTab(pane->id(), result) : movePane(pane->id(), 0, QStringLiteral("new-window"), result);
        if (!error.isEmpty()) notify(pane->id(), QStringLiteral("Move failed"), error);
    });
    connect(window, &TerminalWindow::paneCreated, this, [this, window](Pane* p) { attachPane(window, p); });
    connect(window, &TerminalWindow::focusedPaneChanged, this, [this](Pane* p) {
        if (p) {
            Q_EMIT focusChanged(p->id());
        }
    });
    // Panes made before the window was handed over.
    for (Pane* p : window->panes()) {
        attachPane(window, p);
    }
}

void Control::attachPane(TerminalWindow* window, Pane* pane)
{
    const quint32 id = pane->id();
    connect(pane, &QObject::destroyed, this, [this, id] { Q_EMIT paneClosed(id); });
    connect(pane, &Pane::titleUpdated, this, [this, id](const QString& t) { Q_EMIT titleChanged(id, t); });
    connect(pane, &Pane::bellRang, this, [this, id] { Q_EMIT bell(id); });
    connect(pane, &Pane::userVarUpdated, this,
            [this, id](const QString& name, const QString& value) { Q_EMIT userVarChanged(id, name, value); });
    connect(pane, &Pane::contextUpdated, this, [this, id, guard = QPointer<Pane>(pane)] {
        if (guard) {
            Q_EMIT contextChanged(id, guard->contextLabel());
        }
    });
    connect(pane, &Pane::commandBegan, this, [this, id](const QString& c) { Q_EMIT commandStarted(id, c); });
    connect(pane, &Pane::commandEnded, this,
            [this, id](const QString& c, int code, qint64 ms) { Q_EMIT commandFinished(id, c, code, ms); });
    connect(pane, &Pane::statusChanged, this, [this, id, guard = QPointer<Pane>(pane)] {
        if (!guard) {
            return;
        }
        // The newest record's state and text stand for the pane.
        QString state;
        QString text;
        quint64 serial = 0;
        for (const PaneStatusRecord& r : guard->status()) {
            if (r.serial >= serial) {
                serial = r.serial;
                static const char* const Names[] = {"idle", "working", "done", "blocked", "error"};
                state = QString::fromLatin1(Names[int(r.state)]);
                text = r.message.isEmpty() ? r.title : r.message;
            }
        }
        Q_EMIT statusChanged(id, state, text);
    });
    Q_UNUSED(window);
    Q_EMIT paneCreated(id);
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

QList<PaneInfo> Control::panes() const
{
    QList<PaneInfo> out;
    for (const QPointer<TerminalWindow>& w : m_windows) {
        if (!w) {
            continue;
        }
        for (Pane* p : w->panes()) {
            out.append(PaneInfo{p->id(), w->id(), w->tabIndexOf(p), p->title(), p->workingDirectory(),
                                p->runningCommand(), p->cols(), p->rows(), w->isActive() && w->focusedPane() == p,
                                p->contextLabel(), w->tabIdOf(p)});
        }
    }
    return out;
}

std::optional<PaneInfo> Control::paneInfo(quint32 id) const
{
    for (const PaneInfo& p : panes()) {
        if (p.id == id) {
            return p;
        }
    }
    return std::nullopt;
}

QVariantMap Control::paneMap(const PaneInfo& p)
{
    return {{QStringLiteral("id"), p.id}, {QStringLiteral("window"), p.window},
            {QStringLiteral("tab"), p.tab}, {QStringLiteral("tab_id"), p.tabId},
            {QStringLiteral("title"), p.title}, {QStringLiteral("cwd"), p.workingDirectory},
            {QStringLiteral("command"), p.command}, {QStringLiteral("cols"), p.cols},
            {QStringLiteral("rows"), p.rows}, {QStringLiteral("focused"), p.focused},
            {QStringLiteral("context"), p.context}};
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
        windows.append(QVariantMap{{QStringLiteral("id"), w->id()}, {QStringLiteral("project"), w->projectName()}, {QStringLiteral("project_directory"), w->projectDirectory()}, {QStringLiteral("focused"), w->isActive()},
                                   {QStringLiteral("active_tab"), w->activeTabId()}, {QStringLiteral("active_pane"), focused}});
        for (const auto& tab : w->tabs()) {
            tabs.append(QVariantMap{{QStringLiteral("id"), tab.id}, {QStringLiteral("window"), w->id()},
                                    {QStringLiteral("index"), tab.index}, {QStringLiteral("title"), tab.title},
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
    return {{QStringLiteral("windows"), windows}, {QStringLiteral("tabs"), tabs}, {QStringLiteral("panes"), paneList},
            {QStringLiteral("active_window"), activeWindow}, {QStringLiteral("active_tab"), activeTab},
            {QStringLiteral("active_pane"), activePane}};
}

bool Control::isStructuralAction(const QString& action)
{
    for (Action a : Keybinds::allActions()) {
        if (Keybinds::token(a) == action) {
            return Keybinds::isStructural(a);
        }
    }
    return false;
}

namespace {
QString noPane(quint32 id)
{
    return QStringLiteral("no pane %1").arg(id);
}
} // namespace

QString Control::sendText(quint32 id, const QString& text)
{
    Pane* p = find(id);
    if (!p) {
        return noPane(id);
    }
    p->sendInput(text.toStdString());
    return {};
}

QString Control::readText(quint32 id, TextScope scope, QString& out, bool trusted)
{
    if (!trusted && !m_settings.settings().control.allowReadText) {
        return QStringLiteral("reading pane text is off (Control.allowReadText)");
    }
    Pane* p = find(id);
    if (!p) {
        return noPane(id);
    }
    using AfterglowVt::Terminal;
    switch (scope) {
    case TextScope::Screen:
        out = p->text(Terminal::TextScope::Screen);
        break;
    case TextScope::Scrollback:
        out = p->text(Terminal::TextScope::All);
        break;
    case TextScope::LastOutput:
        out = p->text(Terminal::TextScope::LastOutput);
        break;
    case TextScope::Selection:
        out = p->selectedText();
        break;
    }
    return {};
}

QString Control::readTextBounded(quint32 id, const QString& scope, int limit, QVariantMap& result)
{
    if (limit < 1 || limit > 1024 * 1024) return QStringLiteral("max_bytes must be between 1 and 1048576");
    auto* pane = find(id);
    if (!pane) return noPane(id);
    using Scope = AfterglowVt::Terminal::TextScope;
    Scope kind;
    if (scope == QLatin1String("screen")) kind = Scope::Screen;
    else if (scope == QLatin1String("scrollback")) kind = Scope::All;
    else if (scope == QLatin1String("last-output")) kind = Scope::LastOutput;
    else return QStringLiteral("text scope must be screen, scrollback or last-output");
    result = pane->readText(kind, std::size_t(limit));
    return {};
}

QString Control::split(quint32 id, bool right, const QStringList& command, quint32& created)
{
    TerminalWindow* w = nullptr;
    Pane* p = find(id, &w);
    if (!p) {
        return noPane(id);
    }
    Pane* added = w->splitPane(p, right ? SplitTree::Direction::Right : SplitTree::Direction::Down, command);
    if (!added) {
        return QStringLiteral("cannot split pane %1").arg(id);
    }
    created = added->id();
    return {};
}

QString Control::newTab(quint32 id, const QString& workingDirectory, const QStringList& command, quint32& created)
{
    TerminalWindow* w = nullptr;
    if (!find(id, &w)) {
        return noPane(id);
    }
    Pane* added = w->openTab(workingDirectory, command);
    if (!added) {
        return QStringLiteral("cannot open a tab");
    }
    created = added->id();
    return {};
}

QString Control::newWindow(const QString& workingDirectory, const QStringList& command, quint32& created)
{
    TerminalWindow* w = m_newWindow ? m_newWindow(workingDirectory, command, false) : nullptr;
    if (!w || w->panes().isEmpty()) {
        return QStringLiteral("cannot open a window");
    }
    created = w->panes().first()->id();
    return {};
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
    p->setUserTitle(title.left(256));
    return {};
}

QString Control::signal(quint32 id, int channel, double value)
{
    Pane* p = find(id);
    if (!p) {
        return noPane(id);
    }
    if (channel < 0 || channel >= Pane::SignalCount) {
        return QStringLiteral("signal channels are 0 to %1").arg(Pane::SignalCount - 1);
    }
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
        return QStringLiteral("a signal value must fit in a finite float");
    }
    p->setSignal(channel, float(value));
    return {};
}

QString Control::signalsBatch(quint32 id, const QString& scope, const QVariantList& values, QVariantList& updated)
{
    updated.clear();
    TerminalWindow* window = nullptr;
    Pane* anchor = find(id, &window);
    if (!anchor) {
        return noPane(id);
    }
    if (scope != QLatin1String("pane") && scope != QLatin1String("tab") && scope != QLatin1String("window")) {
        return QStringLiteral("signal scope must be pane, tab or window");
    }
    if (values.isEmpty() || values.size() > Pane::SignalCount) {
        return QStringLiteral("a signal batch needs 1 to 8 channels");
    }
    QList<QPair<int, float>> channels;
    QSet<int> seen;
    for (const auto& v : values) {
        const auto entry = v.toMap();
        bool channelOk = false, valueOk = false;
        const double channel = entry.value(QStringLiteral("channel")).toDouble(&channelOk);
        const double value = entry.value(QStringLiteral("value")).toDouble(&valueOk);
        if (!channelOk || !valueOk || !std::isfinite(channel) || channel != std::floor(channel)
            || channel < 0 || channel >= Pane::SignalCount || seen.contains(int(channel))
            || !std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
            return QStringLiteral("invalid or duplicate signal channel/value");
        }
        seen.insert(int(channel));
        channels.append({int(channel), float(value)});
    }
    QList<Pane*> targets;
    const quint32 tab = window->tabIdOf(anchor);
    for (Pane* p : window->panes()) {
        if (scope == QLatin1String("window") || (scope == QLatin1String("tab") && window->tabIdOf(p) == tab)
            || (scope == QLatin1String("pane") && p == anchor)) {
            targets.append(p);
        }
    }
    if (targets.size() > MaxSignalPanes) {
        return QStringLiteral("a signal batch supports at most %1 panes").arg(MaxSignalPanes);
    }
    // No event-loop turn between validation and application: paint sees the
    // whole batch. All channels and panes share the same animation epoch.
    const auto now = std::chrono::steady_clock::now();
    for (Pane* p : targets) {
        p->setSignals(channels, now);
        updated.append(p->id());
    }
    return {};
}

QString Control::notify(quint32 id, const QString& title, const QString& body)
{
    TerminalWindow* w = nullptr;
    Pane* p = find(id, &w);
    if (!p) {
        return noPane(id);
    }
    NotificationCenter::instance().showSimple(p, w, title.left(256), body.left(2048));
    return {};
}

QString Control::copy(const QString& text)
{
    QGuiApplication::clipboard()->setText(text.left(1 << 22));
    return {};
}

QString Control::openUrl(const QString& url)
{
    const QUrl u(url);
    const QString scheme = u.scheme();
    if (!u.isValid() || !(scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("mailto"))) {
        return QStringLiteral("only http, https and mailto URLs open");
    }
    QDesktopServices::openUrl(u);
    return {};
}

QString Control::runAction(quint32 id, const QString& action)
{
    TerminalWindow* w = nullptr;
    Pane* p = find(id, &w);
    if (!p) {
        return noPane(id);
    }
    if (action.startsWith(QLatin1String(Keybinds::ScriptPrefix))) {
        const QString script = action.mid(int(qstrlen(Keybinds::ScriptPrefix)));
        return m_scriptActions && m_scriptActions(script, id) ? QString() : QStringLiteral("no script action %1").arg(script);
    }
    for (Action a : Keybinds::allActions()) {
        if (Keybinds::token(a) == action) {
            return w->runAction(p, a) ? QString() : QStringLiteral("action %1 is unavailable here").arg(action);
        }
    }
    return QStringLiteral("no action %1").arg(action);
}

QString Control::scopeName(TextScope scope)
{
    switch (scope) {
    case TextScope::Screen:
        return QStringLiteral("screen");
    case TextScope::Scrollback:
        return QStringLiteral("scrollback");
    case TextScope::Selection:
        return QStringLiteral("selection");
    case TextScope::LastOutput:
        return QStringLiteral("last-output");
    }
    return {};
}

std::optional<Control::TextScope> Control::scopeFromName(const QString& name)
{
    for (TextScope s : {TextScope::Screen, TextScope::Scrollback, TextScope::Selection, TextScope::LastOutput}) {
        if (scopeName(s) == name) {
            return s;
        }
    }
    return std::nullopt;
}

QString Control::scriptDialog(const QString& owner, quint32 id, const QVariantMap& effect, ScriptReply reply)
{
    TerminalWindow* window = nullptr;
    auto* pane = find(id, &window);
    return pane ? window->showScriptDialog(owner, pane, effect, std::move(reply)) : QStringLiteral("Pane closed");
}
QString Control::scriptAppearance(const QString& owner, quint32 id, const QVariantMap& effect)
{
    TerminalWindow* window = nullptr;
    auto* pane = find(id, &window);
    return pane ? window->setScriptAppearance(owner, pane, effect) : QStringLiteral("Pane closed");
}
void Control::clearScriptOwner(const QString& owner)
{
    for (auto window : m_windows) if (window) window->clearScriptOwner(owner);
}

QString Control::movePane(quint32 id, quint32 target, const QString& placement, QVariantMap& result)
{
    TerminalWindow *source = nullptr, *destination = nullptr;
    auto* pane = find(id, &source);
    if (!pane) return QStringLiteral("Pane closed");
    Pane* beside = target ? find(target, &destination) : nullptr;
    if (target && !beside) return QStringLiteral("Target pane closed");
    SplitTree::Direction direction = SplitTree::Direction::Right;
    if (placement == QLatin1String("left")) direction = SplitTree::Direction::Left;
    else if (placement == QLatin1String("up")) direction = SplitTree::Direction::Up;
    else if (placement == QLatin1String("down")) direction = SplitTree::Direction::Down;
    else if (placement != QLatin1String("right") && placement != QLatin1String("new-tab") && placement != QLatin1String("new-window"))
        return QStringLiteral("Invalid pane placement");
    const bool newTab = placement == QLatin1String("new-tab");
    const bool newWindow = placement == QLatin1String("new-window");
    if ((newTab || newWindow) && target) return QStringLiteral("New destinations require target 0");
    if (!newTab && !newWindow && !beside) return QStringLiteral("A target pane is required");
    if (pane == beside) return QStringLiteral("Cannot move a pane beside itself");
    if (newTab) destination = source;
    if (newWindow) destination = m_newWindow ? m_newWindow({}, {}, true) : nullptr;
    if (!destination) return QStringLiteral("Cannot create destination window");
    const auto error = source->movePaneTo(pane, destination, beside, direction);
    if (error.isEmpty()) if (const auto info = paneInfo(id)) result = paneMap(*info);
    return error;
}

QString Control::detachTab(quint32 id, QVariantMap& result)
{
    TerminalWindow* source = nullptr;
    auto* pane = find(id, &source);
    if (!pane) return QStringLiteral("Pane closed");
    auto* destination = m_newWindow ? m_newWindow({}, {}, true) : nullptr;
    if (!destination) return QStringLiteral("Cannot create destination window");
    const auto error = source->detachTabTo(pane, destination);
    if (error.isEmpty()) if (const auto info = paneInfo(id)) result = paneMap(*info);
    return error;
}

} // namespace Afterglow
