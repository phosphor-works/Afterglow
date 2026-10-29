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
                                p->runningCommand(), p->cols(), p->rows(), w->isActive() && w->focusedPane() == p});
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
    TerminalWindow* w = m_newWindow ? m_newWindow(workingDirectory, command) : nullptr;
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
    for (Action a : Keybinds::allActions()) {
        if (Keybinds::token(a) == action) {
            w->runAction(p, a);
            return {};
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

} // namespace Afterglow
