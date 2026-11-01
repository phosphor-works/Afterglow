// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Control.h"

#include "../Pane.h"
#include "../TerminalWindow.h"
#include "../app/NotificationCenter.h"
#include "../app/WindowStart.h"
#include "../config/Keybinds.h"
#include "../config/Settings.h"

#include <QDir>
#include <QFileInfo>
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

namespace {
QString noPane(quint32 id)
{
    return QStringLiteral("no pane %1").arg(id);
}

// At most LIMIT UTF-16 units, without splitting a surrogate pair.
QString leftWhole(const QString& text, qsizetype limit)
{
    if (text.size() <= limit) return text;
    return text.left(limit > 0 && text.at(limit - 1).isHighSurrogate() ? limit - 1 : limit);
}
} // namespace

void Control::attachWindow(TerminalWindow* window)
{
    m_windows.removeAll(nullptr);
    m_windows.append(window);
    connect(window, &QWindow::activeChanged, this, [this, guard = QPointer<TerminalWindow>(window)] {
        if (guard && guard->isActive()) m_lastActive = guard;
    });
    connect(window, &TerminalWindow::paneClosing, this, [this](quint32 pane, quint32 tab, quint32 window) {
        m_closingPanes.insert(pane, {tab, window});
    });
    connect(window, &TerminalWindow::projectRequested, this, &Control::projectDialog);
    connect(window, &TerminalWindow::paneMoved, this, [this](Pane* p) { Q_EMIT paneMoved(p->id()); });
    connect(window, &TerminalWindow::transferRequested, this, [this](Pane* pane, bool tab) {
        QVariantMap result;
        const auto error = tab ? detachTab(pane->id(), result) : movePane(pane->id(), 0, QStringLiteral("new-window"), result);
        if (!error.isEmpty()) notify(pane->id(), QStringLiteral("Move failed"), error);
    });
    connect(window, &TerminalWindow::paneCreated, this, [this, window](Pane* p) { attachPane(window, p); });
    connect(window, &TerminalWindow::scriptKey, this, &Control::scriptKey);
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
        attachPane(window, p);
    }
}

void Control::attachPane(TerminalWindow* window, Pane* pane)
{
    const quint32 id = pane->id();
    connect(pane, &QObject::destroyed, this, [this, id] {
        m_status.remove(id);
        const auto identity = m_closingPanes.take(id);
        Q_EMIT paneClosedDetailed(id, identity.first, identity.second);
        Q_EMIT paneClosed(id);
    });
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
        // The newest record's state and text stand for the pane. A record
        // also changes for progress alone, which this signal does not carry.
        QString state;
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
        if (const auto last = m_status.constFind(id); last != m_status.cend() && *last == current) return;
        m_status.insert(id, current);
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

PaneInfo Control::describe(TerminalWindow* w, Pane* p)
{
    return PaneInfo{p->id(), w->id(), w->tabIndexOf(p), p->title(), p->workingDirectory(),
                    p->runningCommand(), p->cols(), p->rows(), w->isActive() && w->focusedPane() == p,
                    p->contextLabel(), w->tabIdOf(p)};
}

QList<PaneInfo> Control::panes() const
{
    QList<PaneInfo> out;
    for (const QPointer<TerminalWindow>& w : m_windows) {
        if (!w) {
            continue;
        }
        for (Pane* p : w->panes()) {
            out.append(describe(w, p));
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

std::optional<quint32> Control::focusedPane() const
{
    for (const QPointer<TerminalWindow>& w : m_windows) {
        if (w && w->isActive() && w->focusedPane()) return w->focusedPane()->id();
    }
    if (m_lastActive && m_lastActive->focusedPane()) return m_lastActive->focusedPane()->id();
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
                                   {QStringLiteral("active_tab"), w->activeTabId()}, {QStringLiteral("active_pane"), focused},
                                   {QStringLiteral("overview"), w->overviewState()}});
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

QString Control::sendText(quint32 id, const QString& text)
{
    Pane* p = find(id);
    if (!p) {
        return noPane(id);
    }
    return p->sendInput(text.toStdString()) ? QString() : QStringLiteral("program closed or input queue full");
}

QString Control::readText(quint32 id, TextScope scope, QString& out)
{
    if (!m_settings.settings().control.allowReadText) {
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
    if (!m_settings.settings().scriptsAllowReadText)
        return QStringLiteral("reading pane text is off (Scripts.allowReadText)");
    if (limit < 1 || limit > 1024 * 1024) return QStringLiteral("max_bytes must be between 1 and 1048576");
    auto* pane = find(id);
    if (!pane) return noPane(id);
    if (scope == QLatin1String("selection")) {
        result = pane->readSelection(std::size_t(limit));
        return {};
    }
    using Scope = AfterglowVt::Terminal::TextScope;
    Scope kind;
    if (scope == QLatin1String("screen")) kind = Scope::Screen;
    else if (scope == QLatin1String("scrollback")) kind = Scope::All;
    else if (scope == QLatin1String("last-output")) kind = Scope::LastOutput;
    else return QStringLiteral("text scope must be screen, scrollback, last-output or selection");
    result = pane->readText(kind, std::size_t(limit));
    return {};
}

QString Control::semanticRegions(quint32 id, QVariantMap& result)
{
    if (!m_settings.settings().scriptsAllowReadText)
        return QStringLiteral("reading pane text is off (Scripts.allowReadText)");
    auto* pane = find(id);
    if (!pane) return noPane(id);
    result = pane->semanticRegions();
    return {};
}

QString Control::readRegion(quint32 id, const QString& region, int limit, QVariantMap& result)
{
    if (!m_settings.settings().scriptsAllowReadText)
        return QStringLiteral("reading pane text is off (Scripts.allowReadText)");
    if (limit < 1 || limit > 1024 * 1024) return QStringLiteral("max_bytes must be between 1 and 1048576");
    bool ok = false;
    const auto regionId = region.toULongLong(&ok);
    if (!ok || !regionId || QString::number(regionId) != region) return QStringLiteral("invalid region ID");
    auto* pane = find(id);
    if (!pane) return noPane(id);
    result = pane->readRegion(regionId, std::size_t(limit));
    return {};
}

namespace {
QString launchOptions(const QString& cwd, const QStringList& command, const QVariantMap& input, PaneLaunchOptions& out)
{
    out.cwd = cwd;
    out.command = command;
    if (input.contains(QStringLiteral("cwd"))) {
        if (input.value(QStringLiteral("cwd")).metaType().id() != QMetaType::QString) return QStringLiteral("cwd must be a string");
        out.cwd = input.value(QStringLiteral("cwd")).toString();
    }
    if (out.cwd.contains(QChar::Null)) return QStringLiteral("cwd contains a null character");
    if (out.cwd == QLatin1String("~") || out.cwd.startsWith(QLatin1String("~/"))) out.cwd = QDir::homePath() + out.cwd.mid(1);
    if (!out.cwd.isEmpty() && (!QDir::isAbsolutePath(out.cwd) || !QFileInfo(out.cwd).isDir())) return QStringLiteral("cwd must name an existing absolute directory");
    for (const auto& arg : command) if (arg.contains(QChar::Null)) return QStringLiteral("command contains a null character");
    if (!command.isEmpty() && command.first().isEmpty()) return QStringLiteral("command must name a program");
    if (input.contains(QStringLiteral("size"))) {
        bool ok = false;
        out.fraction = input.value(QStringLiteral("size")).toDouble(&ok);
        if (!ok || !std::isfinite(out.fraction) || out.fraction <= 0 || out.fraction >= 1) return QStringLiteral("size must be a fraction between 0 and 1");
    }
    if (input.contains(QStringLiteral("env"))) {
        if (input.value(QStringLiteral("env")).metaType().id() != QMetaType::QVariantMap) return QStringLiteral("env must be a table");
        out.environment = input.value(QStringLiteral("env")).toMap();
        for (auto it = out.environment.cbegin(); it != out.environment.cend(); ++it) {
            if (it.key().isEmpty() || it.key().contains(QLatin1Char('=')) || it.key().contains(QChar::Null)) return QStringLiteral("invalid environment name");
            const auto& value = it.value();
            if (value.metaType().id() == QMetaType::Bool && !value.toBool()) continue;
            if (value.metaType().id() != QMetaType::QString || value.toString().contains(QChar::Null)) return QStringLiteral("environment values must be strings or false");
        }
    }
    return {};
}
}
QString Control::split(quint32 id, const QString& direction, const QStringList& command, const QVariantMap& input, quint32& created)
{
    created = 0;
    SplitTree::Direction dir;
    if (direction == QLatin1String("right")) dir = SplitTree::Direction::Right;
    else if (direction == QLatin1String("left")) dir = SplitTree::Direction::Left;
    else if (direction == QLatin1String("up")) dir = SplitTree::Direction::Up;
    else if (direction == QLatin1String("down")) dir = SplitTree::Direction::Down;
    else return QStringLiteral("direction must be left, right, up or down");
    PaneLaunchOptions options;
    if (const auto error = launchOptions({}, command, input, options); !error.isEmpty()) return error;
    TerminalWindow* window = nullptr;
    auto* pane = find(id, &window);
    if (!pane) return noPane(id);
    QString error;
    auto* added = window->splitPane(pane, dir, options, &error);
    if (!added) return error.isEmpty() ? QStringLiteral("cannot split pane") : error;
    created = added->id();
    return {};
}
QString Control::newTab(quint32 id, const QString& cwd, const QStringList& command, quint32& created)
{
    return newTab(id, cwd, command, {}, created);
}
QString Control::newTab(quint32 id, const QString& cwd, const QStringList& command, const QVariantMap& input, quint32& created)
{
    created = 0;
    PaneLaunchOptions options;
    if (const auto error = launchOptions(cwd, command, input, options); !error.isEmpty()) return error;
    TerminalWindow* window = nullptr;
    if (!find(id, &window)) return noPane(id);
    QString error;
    auto* added = window->openTab(options, &error);
    if (!added) return error.isEmpty() ? QStringLiteral("cannot open a tab") : error;
    created = added->id();
    return {};
}
QString Control::newWindow(const QString& cwd, const QStringList& command, quint32& created)
{
    return newWindow(cwd, command, {}, created);
}
QString Control::newWindow(const QString& cwd, const QStringList& command, const QVariantMap& input, quint32& created)
{
    created = 0;
    PaneLaunchOptions options;
    if (const auto error = launchOptions(cwd, command, input, options); !error.isEmpty()) return error;
    auto* window = m_newWindow ? m_newWindow({}, {}, true) : nullptr;
    if (!window) return QStringLiteral("cannot open a window");
    QString error;
    auto* added = window->openTab(options, &error);
    if (!added) { window->deleteLater(); return error.isEmpty() ? QStringLiteral("cannot start program") : error; }
    created = added->id();
    showNew(window);
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
    p->setUserTitle(leftWhole(title, 256));
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
        return QStringLiteral("a signal batch needs 1 to %1 channels").arg(Pane::SignalCount);
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
    Pane* p = find(id);
    if (!p) {
        return noPane(id);
    }
    NotificationCenter::instance().showNotice(p, leftWhole(title, 256), leftWhole(body, 2048));
    return {};
}

QString Control::copy(const QString& text)
{
    QGuiApplication::clipboard()->setText(leftWhole(text, 1 << 22));
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
    return pane ? window->showScriptDialog(owner, pane, effect, std::move(reply)) : noPane(id);
}
QString Control::scriptAppearance(const QString& owner, quint32 id, const QVariantMap& effect)
{
    TerminalWindow* window = nullptr;
    auto* pane = find(id, &window);
    return pane ? window->setScriptAppearance(owner, pane, effect) : noPane(id);
}
QString Control::scriptShader(const QString& owner, quint32 id, const QVariantMap& effect, QVariantMap& result)
{
    TerminalWindow* window = nullptr;
    Pane* pane = find(id, &window);
    return pane ? window->scriptShader(owner, pane, effect, result) : noPane(id);
}

void Control::clearScriptOwner(const QString& owner)
{
    for (auto window : m_windows) if (window) window->clearScriptOwner(owner);
}

QString Control::scriptKeyMode(const QString& owner, quint32 id, const QString& label)
{
    TerminalWindow* window = nullptr;
    Pane* pane = find(id, &window);
    if (!pane) return noPane(id);
    // One key mode per script: starting one elsewhere ends the old one, but
    // only once the new one holds the keyboard.
    const QString error = window->startScriptKeyMode(owner, pane, label);
    if (error.isEmpty()) {
        for (auto other : m_windows) if (other && other != window) other->endScriptKeyMode(owner);
    }
    return error;
}

void Control::endScriptKeyMode(const QString& owner)
{
    for (auto window : m_windows) if (window) window->endScriptKeyMode(owner);
}

QString Control::overview(quint32 id, const QString& operation, QVariantMap& result)
{
    TerminalWindow* window = nullptr;
    Pane* pane = find(id, &window);
    return pane ? window->scriptOverview(pane, operation, result) : noPane(id);
}

QString Control::overviewMove(quint32 id, quint32 target, int step, QVariantMap& result)
{
    TerminalWindow* window = nullptr;
    Pane* pane = find(id, &window);
    if (!pane) return noPane(id);
    TerminalWindow* targetWindow = nullptr;
    Pane* to = target ? find(target, &targetWindow) : nullptr;
    if (target && (!to || targetWindow != window)) return QStringLiteral("pane %1 is not in this window's overview").arg(target);
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
    if (!pane) return {};
    const auto origin = window->paneOrigin(pane);
    if (!origin.from) return {};
    return {{QStringLiteral("split_from"), origin.from}, {QStringLiteral("edge"), origin.edge}};
}

QString Control::movePane(quint32 id, quint32 target, const QString& placement, QVariantMap& result)
{
    TerminalWindow *source = nullptr, *destination = nullptr;
    auto* pane = find(id, &source);
    if (!pane) return noPane(id);
    Pane* beside = target ? find(target, &destination) : nullptr;
    if (target && !beside) return noPane(target);
    SplitTree::Direction direction = SplitTree::Direction::Right;
    if (placement == QLatin1String("left")) direction = SplitTree::Direction::Left;
    else if (placement == QLatin1String("up")) direction = SplitTree::Direction::Up;
    else if (placement == QLatin1String("down")) direction = SplitTree::Direction::Down;
    else if (placement != QLatin1String("right") && placement != QLatin1String("new-tab") && placement != QLatin1String("new-window"))
        return QStringLiteral("placement must be left, right, up, down, new-tab or new-window");
    const bool newTab = placement == QLatin1String("new-tab");
    const bool newWindow = placement == QLatin1String("new-window");
    if ((newTab || newWindow) && target) return QStringLiteral("new-tab and new-window need target 0");
    if (!newTab && !newWindow && !beside) return QStringLiteral("a target pane is required");
    if (pane == beside) return QStringLiteral("cannot move a pane beside itself");
    if (newTab) destination = source;
    if (newWindow) destination = m_newWindow ? m_newWindow({}, {}, true) : nullptr;
    if (!destination) return QStringLiteral("cannot open a window");
    const auto error = source->movePaneTo(pane, destination, beside, direction);
    // A window made for the move must not stay hidden and keep afterglow running.
    if (!error.isEmpty() && newWindow) destination->deleteLater();
    if (error.isEmpty()) if (const auto info = paneInfo(id)) result = paneMap(*info);
    return error;
}

QString Control::detachTab(quint32 id, QVariantMap& result)
{
    TerminalWindow* source = nullptr;
    auto* pane = find(id, &source);
    if (!pane) return noPane(id);
    auto* destination = m_newWindow ? m_newWindow({}, {}, true) : nullptr;
    if (!destination) return QStringLiteral("cannot open a window");
    const auto error = source->detachTabTo(pane, destination);
    if (!error.isEmpty()) destination->deleteLater();
    if (error.isEmpty()) if (const auto info = paneInfo(id)) result = paneMap(*info);
    return error;
}

void Control::showNew(TerminalWindow* window) const
{
    showForStartMode(*window, m_settings.settings().window);
}

} // namespace Afterglow
