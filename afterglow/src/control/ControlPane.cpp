// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

// Control: what a client sets on a pane and its window, its program and
// its signals, and the actions that it runs.

#include "Control.h"

#include "../Pane.h"
#include "../ShownText.h"
#include "../TerminalWindow.h"
#include "../app/NotificationCenter.h"
#include "../config/Keybinds.h"
#include "../config/Settings.h"
#include "../config/Theme.h"

#include <QClipboard>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QSet>
#include <QUrl>

#include <AfterglowPty/Pty.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <limits>

namespace Afterglow {

QString Control::colors(quint32 id, QVariantMap& out)
{
    Pane* p = find(id);
    if (!p)
        return noPane(id);
    out = p->colors();
    return {};
}

QString Control::setColors(quint32 id, const QVariantMap& colors, bool reset, QString* code)
{
    Pane* p = find(id);
    if (!p)
        return noPane(id);
    if (p->isSessionPane()) {
        if (code)
            *code = QLatin1String(SessionPaneCode);
        return SessionPaneColors;
    }
    // Every color first: a bad one sets none.
    std::vector<std::string> payloads = reset ? AfterglowVt::Terminal::colorResetOscs() : std::vector<std::string>();
    for (auto it = colors.cbegin(); it != colors.cend(); ++it) {
        if (it.value().typeId() != QMetaType::QString)
            return QStringLiteral("the color of %1 is a string").arg(it.key());
        const auto rgb = Themes::rgbFromName(it.value().toString());
        if (!rgb)
            return QStringLiteral("%1 is no color").arg(it.value().toString());
        auto payload = AfterglowVt::Terminal::colorOscOf(it.key().toStdString(), *rgb);
        if (!payload)
            return QStringLiteral("no color is named %1").arg(it.key());
        payloads.push_back(std::move(*payload));
    }
    // Every payload was checked: one that the terminal refuses is a bug,
    // said as the daemon says it.
    if (!p->applyColors(payloads))
        return QStringLiteral("pane %1 took only some of the colors").arg(id);
    return {};
}

const QString Control::SessionPaneColors =
    QStringLiteral("the colors of a pane of a mux session are its daemon's: use afterglow ctl --session");

QString Control::setFontSize(quint32 id, const QString& text)
{
    TerminalWindow* w = nullptr;
    if (!find(id, &w))
        return noPane(id);
    // " +3" is relative too: toDouble() skips the space.
    const QString size = text.trimmed();
    const bool relative = size.startsWith(QLatin1Char('+')) || size.startsWith(QLatin1Char('-'));
    bool ok = false;
    const double points = size.toDouble(&ok);
    const double min = AfterglowRender::FontSpec::MinPoints, max = AfterglowRender::FontSpec::MaxPoints;
    if (!ok || !std::isfinite(points) || (!relative && (points < min || points > max))
        || (relative && std::abs(points) > max))
        return QStringLiteral("a font size is %1 to %2 points, or +N or -N points more or fewer").arg(min).arg(max);
    w->setFontSize(points, relative);
    return {};
}

QString Control::setOpacity(quint32 id, double opacity)
{
    TerminalWindow* w = nullptr;
    if (!find(id, &w))
        return noPane(id);
    if (!std::isfinite(opacity) || opacity < 0 || opacity > 1)
        return QStringLiteral("the opacity is from 0 to 1");
    w->setOpacityOverride(opacity);
    return {};
}

QString Control::setUserVar(quint32 id, const QString& name, const QString& value)
{
    Pane* p = find(id);
    if (!p)
        return noPane(id);
    if (const QString error = userVarName(name); !error.isEmpty())
        return error;
    if (p->isSessionPane())
        return QStringLiteral(
            "the user variables of a pane of a mux session are its daemon's: use afterglow ctl --session");
    if (!p->setUserVar(name, value))
        return userVarsFull(id);
    return {};
}

QString Control::userVarsFull(quint32 id)
{
    return QStringLiteral("pane %1 has %2 user variables: it takes no new name")
        .arg(id)
        .arg(AfterglowVt::ITerminalHost::MaxUserVars);
}

QString Control::userVarName(const QString& name)
{
    const qsizetype bytes = name.toUtf8().size();
    if (bytes == 0 || bytes > MaxUserVarName || name.contains(QLatin1Char('=')))
        return QStringLiteral("a user variable's name has 1 to %1 bytes and no =").arg(MaxUserVarName);
    return {};
}

QString Control::signalProgram(quint32 id, const QString& signal)
{
    const auto number = AfterglowPty::signalNumber(signal.toStdString());
    if (!number)
        return QStringLiteral("no signal is named %1").arg(signal);
    Pane* p = find(id);
    if (!p)
        return noPane(id);
    if (p->isSessionPane())
        return QStringLiteral("the program of a pane of a mux session runs in its daemon: use afterglow ctl --session");
    // kill(2) is the last call of a refusal that comes from the kernel:
    // its errno says why.
    errno = 0;
    if (!p->signalProgram(*number)) {
        if (errno == EPERM)
            return QStringLiteral("the program of pane %1 runs as another user: the kernel refused the signal").arg(id);
        return QStringLiteral("pane %1 has no program to signal").arg(id);
    }
    return {};
}

QString Control::setWindowState(quint32 id, const QString& state, double cols, double rows)
{
    TerminalWindow* w = nullptr;
    if (!find(id, &w))
        return noPane(id);
    const auto cells = [](double v) {
        return isWhole(v) && v >= 0 && v <= MaxWindowCells;
    };
    if (!cells(cols) || !cells(rows) || (cols > 0) != (rows > 0))
        return QStringLiteral("a size is COLSxROWS, each from 1 to %1, or 0x0 for none").arg(MaxWindowCells);
    return w->setWindowState(state, int(cols), int(rows));
}

QString Control::setTabTitle(quint32 id, const QString& title)
{
    TerminalWindow* w = nullptr;
    Pane* p = find(id, &w);
    if (!p)
        return noPane(id);
    // As a pane's title: it shows in the tab bar, and a session's layout keeps it.
    w->setTabTitle(p, sanitizedLine(title, 256));
    return {};
}

QString Control::signal(quint32 id, double channel, double value)
{
    Pane* p = find(id);
    if (!p) {
        return noPane(id);
    }
    if (!isWhole(channel) || channel < 0 || channel >= Pane::SignalCount) {
        return QStringLiteral("signal channels are 0 to %1").arg(Pane::SignalCount - 1);
    }
    if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max()) {
        return QStringLiteral("a signal value must fit in a finite float");
    }
    p->setSignal(int(channel), float(value));
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
        if (!channelOk || !valueOk || !isWhole(channel) || channel < 0 || channel >= Pane::SignalCount
            || seen.contains(int(channel)) || !std::isfinite(value)
            || std::abs(value) > std::numeric_limits<float>::max()) {
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
    if (m_noticesEnded)
        return;
    // A script can run a failing command again and again: the notices of
    // its runs count as a program's notifications do.
    NotificationCenter::instance().showLimitedNotice(find(id), textPrefix(title, 256), textPrefix(body, 2048));
}

QString Control::copy(const QString& text)
{
    // Refused, not cut: the caller would not know the clipboard holds less.
    if (text.size() > MaxCopyLength)
        return QStringLiteral("copy text is longer than %1 UTF-16 units").arg(MaxCopyLength);
    QGuiApplication::clipboard()->setText(text);
    return {};
}

QString Control::openUrl(const QString& url)
{
    const QUrl u(url);
    const QString scheme = u.scheme();
    if (!u.isValid()
        || !(scheme == QLatin1String("http") || scheme == QLatin1String("https")
             || scheme == QLatin1String("mailto"))) {
        return QStringLiteral("only http, https and mailto URLs open");
    }
    return QDesktopServices::openUrl(u) ? QString() : QStringLiteral("the desktop could not open the URL");
}

QString Control::runAction(quint32 id, const QString& action)
{
    if (action.startsWith(Keybinds::ScriptPrefix)) {
        if (!Keybinds::isScriptTarget(action))
            return QStringLiteral("a script command action is script:ID.COMMAND");
        const QString command = action.mid(Keybinds::ScriptPrefix.size());
        if (id && !find(id))
            return noPane(id);
        // As `aglow run` does: a command also runs with no pane.
        const quint32 pane = id ? id : focusedPane().value_or(0);
        return m_scriptCommands ? m_scriptCommands(command, pane, QStringLiteral("control"))
                                : QStringLiteral("scripts are off");
    }
    TerminalWindow* w = nullptr;
    Pane* p = find(id, &w);
    if (!p) {
        return noPane(id);
    }
    const Action a = Keybinds::fromToken(action);
    if (a == Action::None)
        return QStringLiteral("no action %1").arg(action);
    return w->runAction(p, a) ? QString() : QStringLiteral("action %1 is unavailable here").arg(action);
}

QString Control::runScriptAction(quint32 id, const QString& action)
{
    // ScriptHost refuses these when it checks the effect; this keeps a script
    // from starting a run that would count as one from afterglow ctl.
    if (action.startsWith(Keybinds::ScriptPrefix))
        return QStringLiteral("scripts cannot run script commands");
    // The user's ways out of what scripts and control clients hold: they end
    // the key mode or the overlay of whoever owns it. A script ends its own
    // with end_key_mode, and its overlay ends with its program.
    if (action == Keybinds::token(Action::EndScriptKeyMode) || action == Keybinds::token(Action::CancelOverlay))
        return QStringLiteral("action %1 is the user's, and scripts cannot run it").arg(action);
    if (!m_settings.settings().scriptsAllowReadText && readsPaneText(action))
        return QStringLiteral("action %1 reads pane text, which is off (Scripts.allowReadText)").arg(action);
    return runAction(id, action);
}

QString Control::runClientAction(quint32 id, const QString& action, bool agentSession)
{
    // These move pane text to the clipboard, into other panes or into a pager
    // file: the same reading that getText needs permission for.
    if (!agentSession && !m_settings.settings().control.allowReadText && readsPaneText(action))
        return QStringLiteral("action %1 reads pane text, which is off (Control.allowReadText)").arg(action);
    return runAction(id, action);
}

QVariantMap Control::clientPaneMap(QVariantMap pane, bool agentSession) const
{
    if (agentSession || m_settings.settings().control.allowReadText)
        return pane;
    if (pane.contains(QStringLiteral("command")))
        pane.insert(QStringLiteral("command"), QString());
    // The values of user variables and the command line of the foreground
    // program can carry secrets too.
    if (pane.contains(QStringLiteral("user_vars"))) {
        QVariantMap vars = pane.value(QStringLiteral("user_vars")).toMap();
        for (QVariant& value : vars)
            value = QString();
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
    return Keybinds::readsPaneText(Keybinds::fromToken(action));
}

} // namespace Afterglow
