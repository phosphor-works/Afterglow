// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "control/SessionControl.h"

#include "Pane.h"
#include "TerminalWindow.h"
#include "config/Settings.h"
#include "control/Control.h"

#include <QJsonObject>

namespace Afterglow {

namespace {

/// The daemon's ID of pane @p id of any window, 0 for a pane of no session.
qint64 daemonId(quint32 id)
{
    for (TerminalWindow* window : TerminalWindow::windows()) {
        for (Pane* pane : window->panes()) {
            if (pane->id() == id) return pane->sessionPaneId();
        }
    }
    return 0;
}

/// A pane map of Control, with the daemon's ID of the pane.
QJsonObject daemonPaneMap(QVariantMap map)
{
    map.insert(QStringLiteral("id"), daemonId(map.value(QStringLiteral("id")).toUInt()));
    return QJsonObject::fromVariantMap(map);
}

MuxClient::ControlAnswer failed(const QString& error)
{
    return {QJsonValue(), error};
}

} // namespace

MuxClient::ControlAnswer answerSessionCall(Control& control, const Settings& settings, TerminalWindow& window, const QString& fn,
                                           const QJsonArray& args)
{
    // The pane of argument @p index, by its ID in the daemon.
    const auto paneAt = [&](qsizetype index) -> Pane* {
        const qint64 id = args.at(index).toInteger(-1);
        return id > 0 ? window.sessionPane(std::uint32_t(id)) : nullptr;
    };
    const auto noPane = [&](qsizetype index) { return failed(QStringLiteral("no pane %1 in this window").arg(args.at(index).toInteger(-1))); };

    if (fn == QLatin1String("placePane")) {
        // The daemon started it for `afterglow ctl split` or `new-tab`.
        const qint64 id = args.at(0).toInteger(-1);
        if (id <= 0 || id > 0xffffffffLL) return failed(QStringLiteral("placePane takes a pane"));
        const QString side = args.at(2).toString();
        std::optional<SplitTree::Direction> direction;
        if (side == QLatin1String("right")) direction = SplitTree::Direction::Right;
        else if (side == QLatin1String("down")) direction = SplitTree::Direction::Down;
        else if (side == QLatin1String("left")) direction = SplitTree::Direction::Left;
        else if (side == QLatin1String("up")) direction = SplitTree::Direction::Up;
        window.placeSessionPane(std::uint32_t(id), paneAt(1), direction);
        return {};
    }
    // The calls without a pane.
    if (fn == QLatin1String("newWindow")) {
        quint32 created = 0;
        QStringList command;
        for (const QJsonValue& word : args.at(1).toArray()) command << word.toString();
        if (const QString error = control.newWindow(args.at(0).toString(), command, created); !error.isEmpty()) return failed(error);
        // A window of its own, outside the session: the ID of its pane there.
        return {qint64(created), {}};
    }
    if (fn == QLatin1String("openDirectory")) {
        QVariantMap result;
        if (const QString error = control.openDirectory(args.at(0).toString(), args.at(1).toString(), result); !error.isEmpty()) return failed(error);
        return {QJsonObject::fromVariantMap(result), {}};
    }
    if (fn == QLatin1String("projects")) return {QJsonArray::fromStringList(control.projectNames()), {}};
    if (fn == QLatin1String("openProject")) {
        QVariantMap result;
        if (const QString error = control.openProject(args.at(0).toString(), args.at(1).toBool(), args.at(2).toString(), result); !error.isEmpty())
            return failed(error);
        return {QJsonObject::fromVariantMap(result), {}};
    }
    if (fn == QLatin1String("scriptDiagnostics")) return {QJsonArray::fromVariantList(control.scriptDiagnostics()), {}};

    // The calls on a pane: the daemon named one of its session.
    Pane* pane = paneAt(0);
    if (!pane) return noPane(0);
    const quint32 id = pane->id();
    if (fn == QLatin1String("focus")) {
        if (const QString error = control.focus(id); !error.isEmpty()) return failed(error);
        return {};
    }
    if (fn == QLatin1String("getText")) {
        // Only a selection comes here: the daemon has the other text.
        QString text;
        if (const QString error = control.readText(id, Control::TextScope::Selection, text); !error.isEmpty()) return failed(error);
        return {text, {}};
    }
    if (fn == QLatin1String("setTitle")) {
        if (const QString error = control.setTitle(id, args.at(1).toString()); !error.isEmpty()) return failed(error);
        return {};
    }
    if (fn == QLatin1String("signal")) {
        if (const QString error = control.signal(id, int(args.at(1).toDouble(-1)), args.at(2).toDouble()); !error.isEmpty()) return failed(error);
        return {};
    }
    if (fn == QLatin1String("notify")) {
        if (const QString error = control.notify(id, args.at(1).toString(), args.at(2).toString()); !error.isEmpty()) return failed(error);
        return {};
    }
    if (fn == QLatin1String("runAction")) {
        const QString action = args.at(1).toString();
        // As the control target of the window: these read pane text.
        if (!settings.control.allowReadText && Control::readsPaneText(action)) {
            return failed(QStringLiteral("action %1 reads pane text, which is off (Control.allowReadText)").arg(action));
        }
        if (const QString error = control.runAction(id, action); !error.isEmpty()) return failed(error);
        return {};
    }
    if (fn == QLatin1String("movePane")) {
        quint32 target = 0;
        if (args.at(1).toInteger(-1) != 0) {
            Pane* beside = paneAt(1);
            if (!beside) return noPane(1);
            target = beside->id();
        }
        QVariantMap result;
        if (const QString error = control.movePane(id, target, args.at(2).toString(), result); !error.isEmpty()) return failed(error);
        return {daemonPaneMap(result), {}};
    }
    if (fn == QLatin1String("detachTab")) {
        QVariantMap result;
        if (const QString error = control.detachTab(id, result); !error.isEmpty()) return failed(error);
        return {daemonPaneMap(result), {}};
    }
    if (fn == QLatin1String("saveProject")) {
        QVariantMap result;
        if (const QString error = control.saveProject(id, args.at(1).toString(), args.at(2).toBool(), result); !error.isEmpty()) return failed(error);
        return {QJsonObject::fromVariantMap(result), {}};
    }
    return failed(QStringLiteral("the window takes no call '%1' for a session").arg(fn));
}

} // namespace Afterglow
