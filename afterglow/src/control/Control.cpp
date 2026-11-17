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
#include "../scripting/ScriptHost.h"

#include <QBuffer>
#include <QDir>
#include <QImage>
#include <QFileInfo>
#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QSet>
#include <QUrl>

#include <AfterglowPty/Pty.h>
#include <AfterglowVt/KeyNames.h>

#include <algorithm>
#include <array>
#include <chrono>
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
        if (previous.control.enabled && !m_settings.settings().control.enabled) endSessionClients(QStringLiteral("Control.enabled is off"));
    });
}

namespace {

using Neighbour = SplitTree::Neighbour;

/// The sides of PaneInfo::neighbours, in their order.
constexpr std::array<Neighbour, 4> Neighbours{Neighbour::Left, Neighbour::Right, Neighbour::Up, Neighbour::Down};

QString neighbourName(Neighbour side)
{
    switch (side) {
    case Neighbour::Left: return QStringLiteral("left");
    case Neighbour::Right: return QStringLiteral("right");
    case Neighbour::Up: return QStringLiteral("up");
    case Neighbour::Down: break;
    }
    return QStringLiteral("down");
}

std::optional<Neighbour> neighbourNamed(const QString& name)
{
    for (const Neighbour side : Neighbours) {
        if (neighbourName(side) == name) return side;
    }
    return std::nullopt;
}

/// "no pane PHRASE pane 3".
QString sidePhrase(Neighbour side)
{
    switch (side) {
    case Neighbour::Left: return QStringLiteral("to the left of");
    case Neighbour::Right: return QStringLiteral("to the right of");
    case Neighbour::Up: return QStringLiteral("above");
    case Neighbour::Down: break;
    }
    return QStringLiteral("below");
}

QString noPane(quint32 id)
{
    return QStringLiteral("no pane %1").arg(id);
}

// ScriptHost checks the effect's max_bytes first, in the same words.
QString maxBytesRefusal(int limit)
{
    if (limit >= 1 && limit <= ScriptHost::MaxReadTextBytes) return {};
    return ScriptHost::maxBytesError();
}

// The text a script command reads can reach the client of its run: then
// Control.allowReadText, which guards text for clients, applies too.
QString scriptReadRefusal(const Settings& settings, bool clientRun)
{
    if (!settings.scriptsAllowReadText) return QStringLiteral("reading pane text is off (Scripts.allowReadText)");
    if (clientRun && !settings.control.allowReadText)
        return QStringLiteral("reading pane text for a command run with a client is off (Control.allowReadText)");
    return {};
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
        const QPointer<Pane> guard(pane);
        const quint32 id = pane->id();
        const PaneMoved moved = [this, guard, id](const QString& error, const QVariantMap&) {
            // A move out of a session that failed after the daemon took the
            // panes left the pane in no window: the notice names none.
            if (!error.isEmpty())
                NotificationCenter::instance().showNotice(guard && find(id) ? guard.data() : nullptr, QStringLiteral("Move failed"),
                                                          textPrefix(error, 2048));
        };
        if (tab) detachTab(id, moved);
        else movePane(id, 0, QStringLiteral("new-window"), moved);
    });
    connect(window, &TerminalWindow::paneCreated, this, &Control::attachPane);
    // The window of a session: what its clients left goes with it, as no
    // daemon tells it of them any more. Its overlays close with it. The
    // owners are the window's own, not the session's: a window that shows
    // the session now keeps its clients.
    if (window->showsSession()) {
        connect(window, &QObject::destroyed, this, [this, prefix = sessionClientOwner(window->id())] {
            m_sessionInputs.dropPrefix(prefix);
            m_sessionOverlayOwners.removeIf([&prefix](const QString& owner) { return owner.startsWith(prefix); });
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
    connect(pane, &Pane::titleUpdated, this, [this, id](const QString& t) { Q_EMIT titleChanged(id, t); });
    connect(pane, &Pane::bellRang, this, [this, id] { Q_EMIT bell(id); });
    connect(pane, &Pane::userVarUpdated, this,
            [this, id](const QString& name, const QString& value) { Q_EMIT userVarChanged(id, name, value); });
    connect(pane, &Pane::contextUpdated, this, [this, id, guard = QPointer<Pane>(pane)] {
        if (!guard) {
            return;
        }
        // The pane also tells of a new kind with the same label, and of a
        // change that went back before this delivery.
        const QString label = guard->contextLabel();
        if (const auto last = m_context.constFind(id); last != m_context.cend() && *last == label) return;
        m_context.insert(id, label);
        Q_EMIT contextChanged(id, label);
    });
    // The pane emits these on the GUI thread, when it delivers the terminal
    // events that its reader thread queued.
    connect(pane, &Pane::commandBegan, this, [this, id, guard = QPointer<Pane>(pane)](const QString& c) {
        if (guard) Q_EMIT commandStarted(id, c);
    });
    connect(pane, &Pane::commandEnded, this, [this, id, guard = QPointer<Pane>(pane)](const QString& c, int code, qint64 ms) {
        if (guard) Q_EMIT commandFinished(id, c, code, ms);
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
        if (const auto last = m_status.constFind(id); last != m_status.cend() && *last == current) return;
        m_status.insert(id, current);
        Q_EMIT statusChanged(id, state, text);
    });
    Q_EMIT paneCreated(id);
}

bool Control::setRemoteSocket(quint32 pane, const AfterglowProto::Remote::Answer& answer,
                              const std::optional<AfterglowProto::Remote::Answer>& overlay)
{
    Pane* target = find(pane);
    if (!target) return false;
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

PaneInfo Control::describe(TerminalWindow* w, Pane* p, bool facts)
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
    if (!facts) return info;
    info.facts = true;
    info.atPrompt = p->atShellPrompt();
    info.userVars = p->userVars();
    if (const auto process = p->foreground()) {
        PaneInfo::Foreground foreground{process->pid, {}, QString::fromStdString(process->cwd)};
        for (const std::string& argument : process->cmdline) foreground.cmdline << QString::fromStdString(argument);
        info.foreground = std::move(foreground);
    }
    for (std::size_t side = 0; side < info.neighbours.size(); ++side) {
        if (const Pane* n = w->neighbourOf(p, Neighbours[side])) info.neighbours[side] = n->id();
    }
    return info;
}

QList<PaneInfo> Control::panes(bool facts) const
{
    QList<PaneInfo> out;
    for (const QPointer<TerminalWindow>& w : m_windows) {
        if (!w) {
            continue;
        }
        for (Pane* p : w->panes()) {
            out.append(describe(w, p, facts));
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
    if (pane && overlay) pane = pane->overlay() ? pane->overlay()->pane() : nullptr;
    const pid_t leader = pane ? pane->programPid() : 0;
    if (leader <= 0 || process <= 1 || process > std::numeric_limits<pid_t>::max()) return false;
    pid_t at = pid_t(process);
    for (int up = 0; at > 1 && up < MaxAncestors; ++up) {
        const auto stat = AfterglowPty::processStat(at);
        if (!stat) return false;
        if (stat->session == leader) return true;
        at = pid_t(stat->parent);
    }
    return false;
}

void Control::endSessionClient(const QString& owner, const QString& message)
{
    m_sessionInputs.drop(owner);
    if (m_sessionOverlayOwners.remove(owner)) closeOverlays(owner, message);
}

void Control::endSessionClients(const QString& message)
{
    m_sessionInputs.clear();
    for (const QString& owner : std::exchange(m_sessionOverlayOwners, {})) closeOverlays(owner, message);
}

void Control::encodePng(QImage image, std::function<void(const QString& base64)> done)
{
    // As offThread, on the pool of the encodes. The answer is posted to this
    // object, and m_encoder waits for the work before it goes.
    m_encoder.start([this, image = std::move(image), done = std::move(done)]() mutable {
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        QString base64 = image.save(&buffer, "PNG") ? QString::fromLatin1(png.toBase64()) : QString();
        QMetaObject::invokeMethod(
            this, [done = std::move(done), base64 = std::move(base64)] { done(base64); }, Qt::QueuedConnection);
    });
}

QString Control::screenshot(quint32 id, CaptureScope scope, ScreenshotReply reply, QString& code)
{
    // The permission goes first: it holds for every pane.
    if (QString refused = captureRefusal(code); !refused.isEmpty()) return refused;
    if (m_screenshots >= MaxScreenshots) {
        code = QStringLiteral("busy");
        return QStringLiteral("%1 screenshots are in progress").arg(MaxScreenshots);
    }
    ++m_screenshots;
    const QPointer<Control> self(this);
    const QString error = capture(
        id, scope,
        [self, reply](const QString& error, const QString& code, const QImage& image) {
            if (!self) return reply(QStringLiteral("Afterglow quits"), QStringLiteral("failed"), {});
            if (!error.isEmpty()) {
                --self->m_screenshots;
                return reply(error, code, {});
            }
            // A 4K PNG takes a moment to encode: not on the thread that draws
            // the windows. encodePng answers only while this object lives.
            Control* control = self.data();
            control->encodePng(image, [control, reply, size = image.size()](const QString& base64) {
                --control->m_screenshots;
                // Checked again as the picture goes out: read access, or
                // control itself, can go off while the frame is drawn and
                // encoded.
                QString code;
                if (const QString refused = control->captureRefusal(code); !refused.isEmpty()) return reply(refused, code, {});
                if (!control->m_settings.settings().control.enabled) {
                    return reply(QStringLiteral("Control.enabled is off"), QStringLiteral("cancelled"), {});
                }
                if (base64.isEmpty()) return reply(QStringLiteral("the PNG could not be written"), QStringLiteral("failed"), {});
                reply({}, {},
                      QJsonObject{{QStringLiteral("png"), base64}, {QStringLiteral("width"), size.width()},
                                  {QStringLiteral("height"), size.height()}});
            });
        },
        code);
    if (!error.isEmpty()) --m_screenshots;
    return error;
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
            if (!mailbox->control) return;
            QMetaObject::invokeMethod(
                mailbox->control, [control = mailbox->control, id, result] { control->offThreadDone(id, result); }, Qt::QueuedConnection);
        }).detach();
    } catch (const std::system_error& e) {
        m_offThread.remove(id);
        return QStringLiteral("cannot call the mux daemon: %1").arg(QString::fromLocal8Bit(e.what()));
    }
    return {};
}

void Control::offThreadDone(quint64 id, OffThreadResult result)
{
    if (auto then = m_offThread.take(id)) then(std::move(result));
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
    QVariantMap map{{QStringLiteral("id"), p.id}, {QStringLiteral("window"), p.window},
            {QStringLiteral("tab"), p.tab}, {QStringLiteral("tab_id"), p.tabId},
            {QStringLiteral("title"), p.title}, {QStringLiteral("cwd"), p.workingDirectory},
            {QStringLiteral("command"), p.command}, {QStringLiteral("cols"), p.cols},
            {QStringLiteral("rows"), p.rows}, {QStringLiteral("focused"), p.focused},
            {QStringLiteral("context"), p.context}};
    if (!p.facts) return map;
    QVariantMap userVars;
    for (auto it = p.userVars.constBegin(); it != p.userVars.constEnd(); ++it) userVars.insert(it.key(), it.value());
    // JSON null for none.
    QVariant foreground = QVariant::fromValue(nullptr);
    if (p.foreground) {
        foreground = QVariantMap{{QStringLiteral("pid"), p.foreground->pid}, {QStringLiteral("cmdline"), p.foreground->cmdline},
                                 {QStringLiteral("cwd"), p.foreground->cwd}};
    }
    QVariantMap neighbours;
    for (std::size_t side = 0; side < p.neighbours.size(); ++side) neighbours.insert(neighbourName(Neighbours[side]), p.neighbours[side]);
    map.insert(QStringLiteral("at_prompt"), p.atPrompt);
    map.insert(QStringLiteral("user_vars"), userVars);
    map.insert(QStringLiteral("foreground"), foreground);
    map.insert(QStringLiteral("neighbours"), neighbours);
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
        windows.append(QVariantMap{{QStringLiteral("id"), w->id()}, {QStringLiteral("project"), w->projectName()}, {QStringLiteral("project_directory"), w->projectDirectory()}, {QStringLiteral("focused"), w->isActive()},
                                   {QStringLiteral("active_tab"), w->activeTabId()}, {QStringLiteral("active_pane"), focused},
                                   {QStringLiteral("overview"), w->overviewState()},
                                   {QStringLiteral("session"), w->sessionName()}, {QStringLiteral("session_host"), w->sessionHost()},
                                   {QStringLiteral("cols"), grid.width()}, {QStringLiteral("rows"), grid.height()}});
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
    // The caller hears of a refusal; the user gets no notice as well.
    return p->sendInput(text.toStdString(), false) ? QString() : QStringLiteral("program closed or input queue full");
}

QString Control::readText(quint32 id, TextScope scope, QString& out, const AfterglowVt::TextFormat& format)
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
        out = p->text(Terminal::TextScope::Screen, format);
        break;
    case TextScope::Scrollback:
        out = p->text(Terminal::TextScope::All, format);
        break;
    case TextScope::LastOutput:
        out = p->text(Terminal::TextScope::LastOutput, format);
        break;
    case TextScope::FirstOutputOnScreen:
    case TextScope::LastVisitedOutput: {
        // Empty when there is none, as the last output of a pane that ran nothing.
        const auto region = scope == TextScope::FirstOutputOnScreen ? p->firstOutputOnScreen() : p->lastVisitedOutput();
        out = region ? p->regionText(*region, format) : QString();
        break;
    }
    case TextScope::Selection:
        out = p->selectedText();
        break;
    }
    return {};
}

QString Control::readTextBounded(quint32 id, const QString& scope, int limit, QVariantMap& result, bool clientRun)
{
    if (const QString refusal = scriptReadRefusal(m_settings.settings(), clientRun); !refusal.isEmpty()) return refusal;
    if (const QString refusal = maxBytesRefusal(limit); !refusal.isEmpty()) return refusal;
    auto* pane = find(id);
    if (!pane) return noPane(id);
    if (scope == QLatin1String("selection")) {
        result = pane->readSelection(std::size_t(limit));
        return {};
    }
    // A command's output region, as kitty's first_cmd_output_on_screen and
    // last_visited_cmd_output: unavailable when there is none.
    if (scope == QLatin1String("first-output-on-screen") || scope == QLatin1String("last-visited-output")) {
        const auto region = scope == QLatin1String("first-output-on-screen") ? pane->firstOutputOnScreen() : pane->lastVisitedOutput();
        result = region ? pane->readRegion(*region, std::size_t(limit))
                        : QVariantMap{{QStringLiteral("text"), QString()}, {QStringLiteral("available"), false},
                                      {QStringLiteral("truncated"), false}};
        return {};
    }
    using Scope = AfterglowVt::Terminal::TextScope;
    Scope kind;
    if (scope == QLatin1String("screen")) kind = Scope::Screen;
    else if (scope == QLatin1String("scrollback")) kind = Scope::All;
    else if (scope == QLatin1String("last-output")) kind = Scope::LastOutput;
    else return QStringLiteral("text scope must be screen, scrollback, last-output, first-output-on-screen, last-visited-output or selection");
    result = pane->readText(kind, std::size_t(limit));
    return {};
}

QString Control::semanticRegions(quint32 id, QVariantMap& result, bool clientRun)
{
    if (const QString refusal = scriptReadRefusal(m_settings.settings(), clientRun); !refusal.isEmpty()) return refusal;
    auto* pane = find(id);
    if (!pane) return noPane(id);
    result = pane->semanticRegions();
    return {};
}

QString Control::readRegion(quint32 id, const QString& region, int limit, QVariantMap& result, bool clientRun)
{
    if (const QString refusal = scriptReadRefusal(m_settings.settings(), clientRun); !refusal.isEmpty()) return refusal;
    if (const QString refusal = maxBytesRefusal(limit); !refusal.isEmpty()) return refusal;
    bool ok = false;
    const auto regionId = region.toULongLong(&ok);
    if (!ok || !regionId || QString::number(regionId) != region) return QStringLiteral("invalid region ID");
    auto* pane = find(id);
    if (!pane) return noPane(id);
    result = pane->readRegion(regionId, std::size_t(limit));
    return {};
}

QString Control::localDirectory(QString& cwd)
{
    if (cwd == QLatin1String("~") || cwd.startsWith(QLatin1String("~/"))) cwd = QDir::homePath() + cwd.mid(1);
    // A leading /, not QDir::isAbsolutePath: that takes a Qt resource path
    // (":/"), which no program can start in.
    if (!cwd.isEmpty() && (!cwd.startsWith(QLatin1Char('/')) || !QFileInfo(cwd).isDir())) return QStringLiteral("cwd must name an existing absolute directory");
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
    for (const auto& arg : command) if (arg.contains(QChar::Null)) return QStringLiteral("command contains a null character");
    if (!command.isEmpty() && command.first().isEmpty()) return QStringLiteral("command must name a program");
    if (input.contains(QStringLiteral("size"))) {
        // A number only: toDouble also reads a string such as "0.5".
        const QVariant size = input.value(QStringLiteral("size"));
        const int type = size.metaType().id();
        const bool number = type == QMetaType::Double || type == QMetaType::Float || type == QMetaType::Int
            || type == QMetaType::UInt || type == QMetaType::LongLong || type == QMetaType::ULongLong;
        bool ok = false;
        out.fraction = number ? size.toDouble(&ok) : 0;
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

/// The options of a new pane in @p window: launchOptions, then a refusal
/// for a session of another host, whose panes take no cwd or env of this
/// computer, then the directory, which must exist here.
QString paneOptions(const TerminalWindow& window, const QString& cwd, const QStringList& command, const QVariantMap& input,
                    PaneLaunchOptions& out)
{
    if (const QString error = launchOptions(cwd, command, input, out); !error.isEmpty()) return error;
    if (const QString error = window.remoteLaunchError(out); !error.isEmpty()) return error;
    return Control::localDirectory(out.cwd);
}

/// The side of a split that @p name names: left, right, up or down.
std::optional<SplitTree::Direction> splitDirection(const QString& name)
{
    if (name == QLatin1String("right")) return SplitTree::Direction::Right;
    if (name == QLatin1String("left")) return SplitTree::Direction::Left;
    if (name == QLatin1String("up")) return SplitTree::Direction::Up;
    if (name == QLatin1String("down")) return SplitTree::Direction::Down;
    return std::nullopt;
}

QString badSplitDirection()
{
    return QStringLiteral("direction must be left, right, up or down");
}
}
QString Control::split(quint32 id, const QString& direction, const QStringList& command, const QVariantMap& input, quint32& created)
{
    created = 0;
    const auto side = splitDirection(direction);
    if (!side) return badSplitDirection();
    const SplitTree::Direction dir = *side;
    TerminalWindow* window = nullptr;
    auto* pane = find(id, &window);
    if (!pane) return noPane(id);
    PaneLaunchOptions options;
    if (const auto error = paneOptions(*window, {}, command, input, options); !error.isEmpty()) return error;
    QString error;
    auto* added = window->splitPane(pane, dir, options, &error);
    if (!added && error == TerminalWindow::noRoomForPane()) return QStringLiteral("not enough room for another pane");
    if (!added) return error.isEmpty() ? QStringLiteral("cannot split pane") : error;
    created = added->id();
    return {};
}
void Control::split(quint32 id, const QString& direction, const QStringList& command, const QVariantMap& input, PaneMade done)
{
    TerminalWindow* window = nullptr;
    Pane* pane = find(id, &window);
    // A window of its own starts the program at once.
    if (!pane || !window->showsSession()) {
        quint32 created = 0;
        const QString error = split(id, direction, command, input, created);
        return done(error, created);
    }
    const auto side = splitDirection(direction);
    if (!side) return done(badSplitDirection(), 0);
    PaneLaunchOptions options;
    if (const auto error = paneOptions(*window, {}, command, input, options); !error.isEmpty()) return done(error, 0);
    const QString refused = window->startSessionPane(pane, side, options, [done](Pane* added, const QString& error) {
        done(added ? QString() : error.isEmpty() ? QStringLiteral("cannot split pane") : error, added ? added->id() : 0);
    });
    if (refused == TerminalWindow::noRoomForPane()) done(QStringLiteral("not enough room for another pane"), 0);
    else if (!refused.isEmpty()) done(refused, 0);
}

void Control::newTab(quint32 id, const QString& cwd, const QStringList& command, const QVariantMap& input, PaneMade done)
{
    TerminalWindow* window = nullptr;
    Pane* from = find(id, &window);
    if (!from || !window->showsSession()) {
        quint32 created = 0;
        const QString error = newTab(id, cwd, command, input, created);
        return done(error, created);
    }
    PaneLaunchOptions options;
    if (const auto error = paneOptions(*window, cwd, command, input, options); !error.isEmpty()) return done(error, 0);
    // Without a directory, the tab starts where the pane is, as a split does.
    const QString refused = window->startSessionPane(from, std::nullopt, options, [done](Pane* added, const QString& error) {
        done(added ? QString() : error.isEmpty() ? QStringLiteral("cannot open a tab") : error, added ? added->id() : 0);
    });
    if (!refused.isEmpty()) done(refused, 0);
}

QString Control::newTab(quint32 id, const QString& cwd, const QStringList& command, quint32& created)
{
    return newTab(id, cwd, command, {}, created);
}
QString Control::newTab(quint32 id, const QString& cwd, const QStringList& command, const QVariantMap& input, quint32& created)
{
    created = 0;
    TerminalWindow* window = nullptr;
    Pane* from = find(id, &window);
    if (!from) return noPane(id);
    PaneLaunchOptions options;
    if (const auto error = paneOptions(*window, cwd, command, input, options); !error.isEmpty()) return error;
    QString error;
    // Without a directory, the tab starts where the pane is, as a split does.
    auto* added = window->openTab(options, &error, from);
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
    if (const auto error = localDirectory(options.cwd); !error.isEmpty()) return error;
    auto* window = m_newWindow ? m_newWindow({}, {}, true) : nullptr;
    if (!window) return QStringLiteral("cannot open a window");
    QString error;
    auto* added = window->openTab(options, &error);
    if (!added) { window->deleteLater(); return error.isEmpty() ? QStringLiteral("cannot start program") : error; }
    created = added->id();
    showNew(window);
    return {};
}

QString Control::launchExtras(const QVariantMap& options, LaunchExtras& extras)
{
    extras = {};
    const auto flag = [&options](const char* key, bool& out) -> QString {
        const QVariant value = options.value(QLatin1String(key));
        if (!value.isValid()) return {};
        if (value.typeId() != QMetaType::Bool) return QStringLiteral("%1 is true or false").arg(QLatin1String(key));
        out = value.toBool();
        return {};
    };
    for (const char* key : {"focus", "hold", "stdin_ansi"}) {
        bool& out = key == std::string_view("focus") ? extras.focus : key == std::string_view("hold") ? extras.hold : extras.stdinAnsi;
        if (const QString error = flag(key, out); !error.isEmpty()) return error;
    }
    if (const QVariant vars = options.value(QStringLiteral("vars")); vars.isValid()) {
        if (vars.typeId() != QMetaType::QVariantMap) return QStringLiteral("vars must be a table");
        extras.vars = vars.toMap();
        for (auto it = extras.vars.cbegin(); it != extras.vars.cend(); ++it) {
            // As OSC 1337 SetUserVar takes a name.
            const qsizetype bytes = it.key().toUtf8().size();
            if (bytes == 0 || bytes > MaxUserVarName || it.key().contains(QLatin1Char('=')))
                return QStringLiteral("a user variable's name has 1 to %1 bytes and no =").arg(MaxUserVarName);
            if (it.value().typeId() != QMetaType::QString) return QStringLiteral("user variable values must be strings");
        }
    }
    if (const QVariant scope = options.value(QStringLiteral("stdin_text")); scope.isValid()) {
        extras.stdinScope = scope.typeId() == QMetaType::QString ? scopeFromName(scope.toString()) : std::nullopt;
        if (!extras.stdinScope) return QStringLiteral("stdin_text is a text scope: ") + BadTextScope.mid(QStringLiteral("scope is ").size());
    }
    if (extras.stdinAnsi && !extras.stdinScope) return QStringLiteral("stdin_ansi needs stdin_text");
    return {};
}

namespace {

/// The daemon runs the program of a session's pane: no hold and no input
/// of this window reach it.
QString sessionLaunchRefusal(const TerminalWindow& window, const Control::LaunchExtras& extras)
{
    if (!window.showsSession() || (!extras.hold && !extras.stdinScope)) return {};
    return QStringLiteral("a pane of a mux session takes no hold or stdin_text: the daemon runs its program");
}

/// The launch of @p extras: the text of @p source as the program's input.
QString applyLaunchExtras(Control& control, quint32 source, const Control::LaunchExtras& extras, PaneLaunchOptions& options)
{
    options.hold = extras.hold;
    if (!extras.stdinScope) return {};
    AfterglowVt::TextFormat format;
    format.ansi = extras.stdinAnsi;
    QString text;
    if (const QString error = control.readText(source, *extras.stdinScope, text, format); !error.isEmpty()) return error;
    options.input = text.toStdString();
    return {};
}

/// What @p extras ask of the pane @p added once it runs: its user
/// variables, and the focus back on @p before.
void afterLaunch(Pane* added, Pane* before, TerminalWindow* window, const Control::LaunchExtras& extras)
{
    for (auto it = extras.vars.cbegin(); it != extras.vars.cend(); ++it) added->setUserVar(it.key(), it.value().toString());
    if (!extras.focus && before && window) window->keepFocus(before);
}

} // namespace

QString Control::clientSplit(quint32 id, const QString& direction, const QStringList& command, const QVariantMap& input, quint32& created)
{
    created = 0;
    LaunchExtras extras;
    if (const QString error = launchExtras(input, extras); !error.isEmpty()) return error;
    const auto side = splitDirection(direction);
    if (!side) return badSplitDirection();
    TerminalWindow* window = nullptr;
    auto* pane = find(id, &window);
    if (!pane) return noPane(id);
    PaneLaunchOptions options;
    if (const auto error = paneOptions(*window, {}, command, input, options); !error.isEmpty()) return error;
    if (const auto error = sessionLaunchRefusal(*window, extras); !error.isEmpty()) return error;
    if (const auto error = applyLaunchExtras(*this, id, extras, options); !error.isEmpty()) return error;
    Pane* const before = window->focusedPane();
    QString error;
    auto* added = window->splitPane(pane, *side, options, &error);
    if (!added && error == TerminalWindow::noRoomForPane()) return QStringLiteral("not enough room for another pane");
    if (!added) return error.isEmpty() ? QStringLiteral("cannot split pane") : error;
    afterLaunch(added, before, window, extras);
    created = added->id();
    return {};
}

QString Control::clientNewTab(quint32 id, const QString& cwd, const QStringList& command, const QVariantMap& input, quint32& created)
{
    created = 0;
    LaunchExtras extras;
    if (const QString error = launchExtras(input, extras); !error.isEmpty()) return error;
    TerminalWindow* window = nullptr;
    Pane* from = find(id, &window);
    if (!from) return noPane(id);
    PaneLaunchOptions options;
    if (const auto error = paneOptions(*window, cwd, command, input, options); !error.isEmpty()) return error;
    if (const auto error = sessionLaunchRefusal(*window, extras); !error.isEmpty()) return error;
    if (const auto error = applyLaunchExtras(*this, id, extras, options); !error.isEmpty()) return error;
    Pane* const before = window->focusedPane();
    QString error;
    auto* added = window->openTab(options, &error, from);
    if (!added) return error.isEmpty() ? QStringLiteral("cannot open a tab") : error;
    afterLaunch(added, before, window, extras);
    created = added->id();
    return {};
}

QString Control::clientNewWindow(const QString& cwd, const QStringList& command, const QVariantMap& input, quint32& created)
{
    created = 0;
    LaunchExtras extras;
    if (const QString error = launchExtras(input, extras); !error.isEmpty()) return error;
    // A new window has no pane to read, and takes the focus as a window does.
    if (extras.stdinScope) return QStringLiteral("newWindow reads no pane: stdin_text is for split and newTab");
    if (!extras.focus) return QStringLiteral("newWindow takes the focus: focus is for split and newTab");
    PaneLaunchOptions options;
    if (const auto error = launchOptions(cwd, command, input, options); !error.isEmpty()) return error;
    if (const auto error = localDirectory(options.cwd); !error.isEmpty()) return error;
    options.hold = extras.hold;
    auto* window = m_newWindow ? m_newWindow({}, {}, true) : nullptr;
    if (!window) return QStringLiteral("cannot open a window");
    QString error;
    auto* added = window->openTab(options, &error);
    if (!added) { window->deleteLater(); return error.isEmpty() ? QStringLiteral("cannot start program") : error; }
    afterLaunch(added, nullptr, window, extras);
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
    // It shows in the tab bar, and a session's layout keeps it: no control
    // characters, and nothing that disguises text.
    p->setUserTitle(sanitizedLine(title, 256));
    return {};
}

QString Control::focusNeighbour(quint32 id, const QString& direction, quint32& focused)
{
    const auto side = neighbourNamed(direction);
    if (!side) return QStringLiteral("direction must be left, right, up or down");
    TerminalWindow* w = nullptr;
    Pane* p = find(id, &w);
    if (!p) return noPane(id);
    Pane* n = w->neighbourOf(p, *side);
    // An error at the edge: a client such as smart-splits.nvim then passes
    // the move on, or wraps.
    if (!n) return QStringLiteral("no pane %1 pane %2").arg(sidePhrase(*side)).arg(id);
    w->revealPane(n);
    focused = n->id();
    return {};
}

QString Control::resizePane(quint32 id, const QString& direction, int cells)
{
    const auto side = neighbourNamed(direction);
    if (!side) return QStringLiteral("direction must be left, right, up or down");
    if (cells < 1 || cells > MaxResizeCells) return QStringLiteral("cells is a whole number from 1 to %1").arg(MaxResizeCells);
    TerminalWindow* w = nullptr;
    Pane* p = find(id, &w);
    if (!p) return noPane(id);
    if (!w->resizePane(p, *side, cells)) return QStringLiteral("pane %1 has no edge that can move %2").arg(id).arg(neighbourName(*side));
    return {};
}

QString Control::sendKeys(quint32 id, const QStringList& keys)
{
    if (keys.isEmpty()) return QStringLiteral("sendKeys needs a key");
    if (std::size_t(keys.size()) > AfterglowVt::MaxKeysAtOnce)
        return QStringLiteral("sendKeys sends at most %1 keys at once").arg(AfterglowVt::MaxKeysAtOnce);
    // Every name first: a bad one sends none of the keys.
    std::vector<AfterglowVt::NamedKey> named;
    named.reserve(std::size_t(keys.size()));
    for (const QString& name : keys) {
        auto key = AfterglowVt::parseKeyName(name.toStdString());
        if (!key) return QStringLiteral("no key is named '%1'").arg(name);
        named.push_back(std::move(*key));
    }
    Pane* p = find(id);
    if (!p) return noPane(id);
    // Encoded for the program's keyboard mode, as a typed key is.
    const std::string bytes = p->withTerminal([&named](AfterglowVt::Terminal& t) {
        std::string out;
        for (const AfterglowVt::NamedKey& key : named) out += AfterglowVt::encodeNamedKey(key, t.modes());
        return out;
    });
    if (bytes.empty()) return {};
    return p->sendInput(bytes, false) ? QString() : QStringLiteral("program closed or input queue full");
}

QString Control::scroll(quint32 id, const QString& unit, qint64 amount)
{
    if (std::abs(amount) > MaxScrollAmount) return QStringLiteral("the amount is a whole number from -%1 to %1").arg(MaxScrollAmount);
    TerminalWindow* w = nullptr;
    Pane* p = find(id, &w);
    if (!p) return noPane(id);
    return w->scrollFromControl(p, unit, amount);
}

QString Control::setTabTitle(quint32 id, const QString& title)
{
    TerminalWindow* w = nullptr;
    Pane* p = find(id, &w);
    if (!p) return noPane(id);
    // As a pane's title: it shows in the tab bar, and a session's layout keeps it.
    w->setTabTitle(p, sanitizedLine(title, 256));
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
    NotificationCenter::instance().showNotice(p, textPrefix(title, 256), textPrefix(body, 2048));
    return {};
}

void Control::notice(quint32 id, const QString& title, const QString& body)
{
    if (m_noticesEnded) return;
    // A script can run a failing command again and again: the notices of
    // its runs count as a program's notifications do.
    NotificationCenter::instance().showLimitedNotice(find(id), textPrefix(title, 256), textPrefix(body, 2048));
}

QString Control::copy(const QString& text)
{
    // Refused, not cut: the caller would not know the clipboard holds less.
    if (text.size() > MaxCopyLength) return QStringLiteral("copy text is longer than %1 UTF-16 units").arg(MaxCopyLength);
    QGuiApplication::clipboard()->setText(text);
    return {};
}

QString Control::openUrl(const QString& url)
{
    const QUrl u(url);
    const QString scheme = u.scheme();
    if (!u.isValid() || !(scheme == QLatin1String("http") || scheme == QLatin1String("https") || scheme == QLatin1String("mailto"))) {
        return QStringLiteral("only http, https and mailto URLs open");
    }
    return QDesktopServices::openUrl(u) ? QString() : QStringLiteral("the desktop could not open the URL");
}

QString Control::runAction(quint32 id, const QString& action)
{
    if (action.startsWith(Keybinds::ScriptPrefix)) {
        if (!Keybinds::isScriptTarget(action)) return QStringLiteral("a script command action is script:ID.COMMAND");
        const QString command = action.mid(Keybinds::ScriptPrefix.size());
        if (id && !find(id)) return noPane(id);
        // As `aglow run` does: a command also runs with no pane.
        const quint32 pane = id ? id : focusedPane().value_or(0);
        return m_scriptCommands ? m_scriptCommands(command, pane, QStringLiteral("control")) : QStringLiteral("scripts are off");
    }
    TerminalWindow* w = nullptr;
    Pane* p = find(id, &w);
    if (!p) {
        return noPane(id);
    }
    for (Action a : Keybinds::allActions()) {
        if (Keybinds::token(a) == action) {
            return w->runAction(p, a) ? QString() : QStringLiteral("action %1 is unavailable here").arg(action);
        }
    }
    return QStringLiteral("no action %1").arg(action);
}

QString Control::runScriptAction(quint32 id, const QString& action)
{
    // ScriptHost refuses these when it checks the effect; this keeps a script
    // from starting a run that would count as one from afterglow ctl.
    if (action.startsWith(Keybinds::ScriptPrefix)) return QStringLiteral("scripts cannot run script commands");
    // The user's ways out of what scripts and control clients hold: they end
    // the key mode or the overlay of whoever owns it. A script ends its own
    // with end_key_mode, and its overlay ends with its program.
    if (action == Keybinds::token(Action::EndScriptKeyMode) || action == Keybinds::token(Action::CancelOverlay))
        return QStringLiteral("action %1 is the user's, and scripts cannot run it").arg(action);
    if (!m_settings.settings().scriptsAllowReadText && readsPaneText(action))
        return QStringLiteral("action %1 reads pane text, which is off (Scripts.allowReadText)").arg(action);
    return runAction(id, action);
}

QString Control::runClientAction(quint32 id, const QString& action)
{
    // These move pane text to the clipboard, into other panes or into a pager
    // file: the same reading that getText needs permission for.
    if (!m_settings.settings().control.allowReadText && readsPaneText(action))
        return QStringLiteral("action %1 reads pane text, which is off (Control.allowReadText)").arg(action);
    return runAction(id, action);
}

QVariantMap Control::clientPaneMap(QVariantMap pane) const
{
    if (m_settings.settings().control.allowReadText) return pane;
    if (pane.contains(QStringLiteral("command"))) pane.insert(QStringLiteral("command"), QString());
    // The values of user variables and the command line of the foreground
    // program can carry secrets too.
    if (pane.contains(QStringLiteral("user_vars"))) {
        QVariantMap vars = pane.value(QStringLiteral("user_vars")).toMap();
        for (QVariant& value : vars) value = QString();
        pane.insert(QStringLiteral("user_vars"), vars);
    }
    if (QVariantMap foreground = pane.value(QStringLiteral("foreground")).toMap(); !foreground.isEmpty()) {
        foreground.insert(QStringLiteral("cmdline"), QStringList());
        pane.insert(QStringLiteral("foreground"), foreground);
    }
    return pane;
}

bool Control::readsPaneText(const QString& action)
{
    return action == QLatin1String("copy") || action == QLatin1String("paste") || action == QLatin1String("select_all")
        || action == QLatin1String("show_last_output") || action == QLatin1String("show_first_output_on_screen")
        || action == QLatin1String("show_last_visited_output");
}

const QString Control::BadTextScope =
    QStringLiteral("scope is screen, scrollback, selection, last-output, first-output-on-screen or last-visited-output");

QString Control::textFormat(const QVariantMap& options, AfterglowVt::TextFormat& format)
{
    format = {};
    for (auto it = options.constBegin(); it != options.constEnd(); ++it) {
        bool* flag = it.key() == QLatin1String("ansi") ? &format.ansi
            : it.key() == QLatin1String("cursor")      ? &format.cursor
            : it.key() == QLatin1String("wrap_markers") ? &format.wrapMarkers
                                                         : nullptr;
        if (!flag) return QStringLiteral("unknown getText option %1: the options are ansi, cursor and wrap_markers").arg(it.key());
        if (it.value().typeId() != QMetaType::Bool) return QStringLiteral("the getText option %1 is true or false").arg(it.key());
        *flag = it.value().toBool();
    }
    return {};
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
    case TextScope::FirstOutputOnScreen:
        return QStringLiteral("first-output-on-screen");
    case TextScope::LastVisitedOutput:
        return QStringLiteral("last-visited-output");
    }
    return {};
}

QString Control::captureRefusal(QString& code) const
{
    if (!m_settings.settings().control.allowReadText) {
        code = QStringLiteral("denied");
        return QStringLiteral("screenshots are off (Control.allowReadText)");
    }
    return {};
}

QString Control::capture(quint32 id, CaptureScope scope, CaptureReply reply, QString& code)
{
    if (QString refused = captureRefusal(code); !refused.isEmpty()) {
        return refused;
    }
    TerminalWindow* window = nullptr;
    Pane* pane = find(id, &window);
    if (!pane) {
        code = QStringLiteral("no_pane");
        return noPane(id);
    }
    window->captureFrame(pane, scope, std::move(reply));
    return {};
}

std::optional<CaptureScope> Control::captureScopeFromName(const QString& name)
{
    if (name == QLatin1String("pane")) return CaptureScope::Pane;
    if (name == QLatin1String("tab")) return CaptureScope::Tab;
    if (name == QLatin1String("window")) return CaptureScope::Window;
    return std::nullopt;
}

std::optional<Control::TextScope> Control::scopeFromName(const QString& name)
{
    for (TextScope s : {TextScope::Screen, TextScope::Scrollback, TextScope::Selection, TextScope::LastOutput, TextScope::FirstOutputOnScreen,
                        TextScope::LastVisitedOutput}) {
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
QString Control::scriptDock(const QString& owner, quint32 id, const QVariantMap& effect)
{
    // Every dock and undock comes through here, so the records match the
    // windows' areas. A closed window's records go.
    m_docks.removeIf([](const DockRecord& r) { return !r.window; });
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
        auto it = open ? record([window](const DockRecord& r) { return r.window == window; }) : m_docks.end();
        if (it == m_docks.end()) {
            // The records are in the order they were last docked through,
            // so the last match is the area this pane docked last.
            const auto last = std::find_if(m_docks.rbegin(), m_docks.rend(), [&](const DockRecord& r) {
                return r.owner == owner && r.id == dock && r.pane == id;
            });
            if (last != m_docks.rend()) it = std::prev(last.base());
        }
        if (it == m_docks.end()) return open ? window->setScriptDock(owner, effect) : noPane(id);
        const QPointer<TerminalWindow> target = it->window;
        m_docks.erase(it);
        return target->setScriptDock(owner, effect);
    }
    if (!open) return noPane(id);
    const QString error = window->setScriptDock(owner, effect);
    if (error.isEmpty()) {
        // The newest dock goes last, for the undock above.
        if (auto it = record([window](const DockRecord& r) { return r.window == window; }); it != m_docks.end())
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
    for (auto window : m_windows) if (window) window->closeRunWaits(owner, prefix);
}

QString Control::overlay(const QString& owner, const QString& request, quint32 id, const OverlaySpec& spec, OverlayReply reply,
                         QString& code)
{
    TerminalWindow* window = nullptr;
    Pane* pane = find(id, &window);
    if (!pane) {
        code = QStringLiteral("no_pane");
        return noPane(id);
    }
    return window->openOverlay(owner, request, pane, spec, std::move(reply), code);
}

std::optional<OverlaySpec> Control::clientOverlaySpec(const QStringList& argv, const QVariantMap& options, QString& error)
{
    auto spec = OverlaySpec::read(argv, options, &error);
    // A client names a directory of this machine: one that is missing is
    // its mistake, where a script's remote path starts in $HOME.
    if (spec) error = localDirectory(spec->cwd);
    if (!error.isEmpty()) return std::nullopt;
    return spec;
}

QString Control::clientOverlay(const QString& owner, quint32 pane, const OverlaySpec& spec, OverlayReply reply, QString& code)
{
    return overlay(
        owner, {}, pane, spec,
        [reply = std::move(reply), self = QPointer<Control>(this)](const QString& error, const QString& code, const QVariantMap& result) {
            if (!error.isEmpty()) return reply(error, code, {});
            QVariantMap answer = result;
            if (!self || !self->m_settings.settings().control.allowReadText) answer.remove(QStringLiteral("stdout"));
            reply({}, {}, answer);
        },
        code);
}

void Control::closeOverlays(const QString& owner, const QString& message)
{
    for (auto window : m_windows) if (window) window->closeOverlays(owner, {}, message);
}

void Control::clearScriptOwner(const QString& owner)
{
    m_docks.removeIf([&owner](const DockRecord& r) { return r.owner == owner || !r.window; });
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

QString Control::overview(const QString& owner, quint32 id, const QString& operation, QVariantMap& result, QString* code)
{
    TerminalWindow* window = nullptr;
    Pane* pane = find(id, &window);
    return pane ? window->scriptOverview(owner, pane, operation, result, code) : noPane(id);
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

void Control::movePane(quint32 id, quint32 target, const QString& placement, PaneMoved done)
{
    TerminalWindow *source = nullptr, *destination = nullptr;
    auto* pane = find(id, &source);
    if (!pane) return done(noPane(id), {});
    Pane* beside = target ? find(target, &destination) : nullptr;
    if (target && !beside) return done(noPane(target), {});
    SplitTree::Direction direction = SplitTree::Direction::Right;
    if (placement == QLatin1String("left")) direction = SplitTree::Direction::Left;
    else if (placement == QLatin1String("up")) direction = SplitTree::Direction::Up;
    else if (placement == QLatin1String("down")) direction = SplitTree::Direction::Down;
    else if (placement != QLatin1String("right") && placement != QLatin1String("new-tab") && placement != QLatin1String("new-window"))
        return done(QStringLiteral("placement must be left, right, up, down, new-tab or new-window"), {});
    const bool newTab = placement == QLatin1String("new-tab");
    const bool newWindow = placement == QLatin1String("new-window");
    if (newWindow && target) return done(QStringLiteral("new-window takes no target pane"), {});
    if (!newTab && !newWindow && !beside) return done(QStringLiteral("a target pane is required"), {});
    if (!newTab && pane == beside) return done(QStringLiteral("cannot move a pane beside itself"), {});
    // A new tab goes to the window of its target, else to the pane's own.
    if (newTab && !beside) destination = source;
    // A session's panes run under its daemon: they leave in a session of
    // their own, which a window of its own shows.
    if (newWindow && source->showsSession()) return breakOut(source, pane, false, std::move(done));
    // Or they join the session of another window, whose daemon moves them.
    if (destination && destination != source && source->showsSession() && destination->showsSession())
        return joinSession(source, pane, destination, beside, placement, std::move(done));
    if (newWindow) destination = m_newWindow ? m_newWindow({}, {}, true) : nullptr;
    if (!destination) return done(QStringLiteral("cannot open a window"), {});
    const auto error = source->movePaneTo(pane, destination, newTab ? nullptr : beside, direction, newTab ? beside : nullptr);
    // A window made for the move must not stay hidden and keep afterglow running.
    if (!error.isEmpty() && newWindow) destination->deleteLater();
    QVariantMap result;
    if (error.isEmpty()) if (const auto info = paneInfo(id)) result = paneMap(*info);
    done(error, result);
}

void Control::detachTab(quint32 id, PaneMoved done)
{
    TerminalWindow* source = nullptr;
    auto* pane = find(id, &source);
    if (!pane) return done(noPane(id), {});
    if (source->showsSession()) return breakOut(source, pane, true, std::move(done));
    auto* destination = m_newWindow ? m_newWindow({}, {}, true) : nullptr;
    if (!destination) return done(QStringLiteral("cannot open a window"), {});
    const auto error = source->detachTabTo(pane, destination);
    if (!error.isEmpty()) destination->deleteLater();
    QVariantMap result;
    if (error.isEmpty()) if (const auto info = paneInfo(id)) result = paneMap(*info);
    done(error, result);
}

void Control::breakOut(TerminalWindow* source, Pane* pane, bool tab, PaneMoved done)
{
    if (!m_openSessionWindow) return done(QStringLiteral("cannot open a window"), {});
    const std::uint32_t daemonPane = pane->sessionPaneId();
    // The new session is on the host of the one that the panes leave.
    const QString host = pane->session() ? pane->session()->host() : QString();
    // The daemon's call and the new window's attach run off the GUI thread.
    // What answers later finds this object gone when Afterglow quits: the
    // caller then hears nothing (its script, or its client, ends too).
    const auto showIt = [self = QPointer<Control>(this), daemonPane, host, tab, done](const QString& error, const QString& session,
                                                                                     std::shared_ptr<const TerminalWindow::BreakOut> kept) {
        if (!self) return;
        if (!error.isEmpty()) return done(error, {});
        const auto unshown = [tab, session](const QString& why) {
            // The daemon moved them already: what is left is to show them.
            return (tab ? QStringLiteral("the tab went to session %1, but no window can show it (%2)")
                        : QStringLiteral("the pane went to session %1, but no window can show it (%2)"))
                .arg(session, why);
        };
        if (!self->m_openSessionWindow) return done(unshown(QStringLiteral("cannot open a window")), {});
        self->m_openSessionWindow(session, host, [self, daemonPane, host, tab, session, kept, done, unshown](const QString& error) {
            if (!self) return;
            if (!error.isEmpty()) return done(unshown(error), {});
            // The window that shows the new session, and in it the pane that
            // shows the same program.
            QVariantMap result;
            for (TerminalWindow* window : TerminalWindow::windows()) {
                const QList<Pane*> shown = window->panes();
                if (std::none_of(shown.cbegin(), shown.cend(), [&](const Pane* p) { return p->showsSession(session, host); })) continue;
                window->adoptBreakOut(kept);
                for (Pane* p : shown) {
                    if (p->sessionPaneId() == daemonPane) {
                        if (const auto info = self->paneInfo(p->id())) result = paneMap(*info);
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
    if (const QString refused = source->breakOutOfSession(pane, tab, showIt); !refused.isEmpty()) done(refused, {});
}

void Control::joinSession(TerminalWindow* source, Pane* pane, TerminalWindow* destination, Pane* beside, const QString& placement,
                          PaneMoved done)
{
    // None for new-tab.
    const std::optional<SplitTree::Direction> side = splitDirection(placement);
    const std::uint32_t daemonPane = pane->sessionPaneId();
    // A project of the other session saves the command, as this one did.
    const QStringList command = pane->launchCommand();
    // The daemon's call runs off the GUI thread. What answers later finds
    // this object gone when Afterglow quits: the caller then hears nothing.
    const auto placed = [self = QPointer<Control>(this), destination = QPointer<TerminalWindow>(destination), beside = QPointer<Pane>(beside),
                         side, daemonPane, command, done](const QString& error, const QString& session,
                                                          std::shared_ptr<const TerminalWindow::BreakOut> kept) {
        if (!self) return;
        if (!error.isEmpty()) return done(error, {});
        if (!destination || destination->closing() || destination->sessionName() != session) {
            return done(QStringLiteral("the pane went to session %1, but its window no longer shows it").arg(session), {});
        }
        // The daemon tells the window to place the pane too (placePane): the
        // first of the two places it, and the other finds it there.
        Pane* shown = destination->sessionPane(daemonPane);
        if (!shown) {
            Pane* const next = beside && destination->tabIndexOf(beside) >= 0 ? beside.data() : nullptr;
            shown = destination->placeSessionPane(daemonPane, next, side, command);
        }
        if (!shown) return done(QStringLiteral("the pane went to session %1, but its window cannot show it").arg(session), {});
        destination->adoptBreakOut(kept);
        destination->requestActivate();
        QVariantMap result;
        if (const auto info = self->paneInfo(shown->id())) result = paneMap(*info);
        done({}, result);
    };
    if (const QString refused = source->joinSession(pane, destination, beside, side, placed); !refused.isEmpty()) done(refused, {});
}

void Control::showNew(TerminalWindow* window) const
{
    showForStartMode(*window, m_settings.settings().window);
}

} // namespace Afterglow
