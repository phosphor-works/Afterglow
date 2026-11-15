// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "control/SessionControl.h"

#include "Pane.h"
#include "TerminalWindow.h"
#include "config/Settings.h"
#include "config/Keybinds.h"
#include "control/Control.h"

#include <QImage>
#include <QJsonObject>
#include <QPointer>

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

/// A session of another host reaches what its programs could reach over
/// ssh, and no more: no program of this computer runs for it.
const QString OtherHostRunsNothingHere = QStringLiteral("a session of another host runs no program on this computer");

MuxClient::ControlAnswer failed(const QString& error, const QString& code = {})
{
    return {QJsonValue(), error, code};
}

/// Who owns the overlays and the inputs of @p client of the session that
/// @p window shows.
QString sessionOwner(const TerminalWindow& window, qint64 client)
{
    return Control::sessionClientOwner(window.sessionHost(), window.sessionName(), client);
}

/// The answer of every call but openProject, which can come later.
MuxClient::ControlAnswer answerNow(Control& control, const Settings& settings, TerminalWindow& window, const QString& fn,
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
        QStringList command;
        for (const QJsonValue& word : args.at(3).toArray()) command << word.toString();
        window.placeSessionPane(std::uint32_t(id), paneAt(1), direction, command);
        return {};
    }
    // The calls without a pane.
    if (fn == QLatin1String("newWindow")) {
        quint32 created = 0;
        QStringList command;
        for (const QJsonValue& word : args.at(1).toArray()) command << word.toString();
        if (!command.isEmpty() && !window.sessionHost().isEmpty()) return failed(OtherHostRunsNothingHere);
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
        // A script command of another host's session goes through `aglow
        // run`, which runs only the commands that a script marks remote.
        if (action.startsWith(Keybinds::ScriptPrefix) && !window.sessionHost().isEmpty()) {
            return failed(QStringLiteral("a session of another host runs only the script commands that a script marks remote, with aglow run"));
        }
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

} // namespace

void answerSessionCall(Control& control, const Settings& settings, TerminalWindow& window, const QString& fn, const QJsonArray& args,
                       qint64 client, MuxClient::ControlReply reply)
{
    const auto paneAt = [&](qsizetype index) -> Pane* {
        const qint64 id = args.at(index).toInteger(-1);
        return id > 0 ? window.sessionPane(std::uint32_t(id)) : nullptr;
    };
    // The daemon tells it when a client that opened overlays or sent their
    // input went (ID 0: no answer).
    if (fn == QLatin1String("clientGone")) {
        control.endSessionClient(sessionOwner(window, args.at(0).toInteger(0)), QStringLiteral("The client went away"));
        return reply({});
    }
    if ((fn == QLatin1String("overlay") || fn == QLatin1String("overlayInput")) && !window.sessionHost().isEmpty()) {
        return reply(failed(OtherHostRunsNothingHere));
    }
    if (fn == QLatin1String("overlayInput")) {
        const QString error = control.addSessionOverlayInput(sessionOwner(window, client), args.at(0).toString(), args.at(1).toString(),
                                                             args.at(2).toBool());
        return reply(error.isEmpty() ? MuxClient::ControlAnswer{true, {}} : failed(error));
    }
    if (fn == QLatin1String("overlay")) {
        // As the control target of a window takes one: its input goes, also
        // for a pane that it refuses.
        const QString owner = sessionOwner(window, client);
        QVariantMap options = args.at(2).toObject().toVariantMap();
        QString error = control.takeSessionOverlayInput(owner, options);
        if (!error.isEmpty()) return reply(failed(error));
        QStringList argv;
        for (const QJsonValue& word : args.at(1).toArray()) argv << word.toString();
        auto spec = OverlaySpec::read(argv, options, &error);
        if (spec) error = Control::localDirectory(spec->cwd);
        if (!error.isEmpty()) return reply(failed(error));
        Pane* pane = paneAt(0);
        if (!pane) return reply(failed(QStringLiteral("no pane %1 in this window").arg(args.at(0).toInteger(-1)), QStringLiteral("no_pane")));
        QString code;
        error = control.overlay(
            owner, {}, pane->id(), *spec,
            [reply, &settings](const QString& error, const QString& code, const QVariantMap& result) {
                if (!error.isEmpty()) return reply(failed(error, code));
                // Gated as it goes out: read access can go while the program runs.
                QVariantMap answer = result;
                if (!settings.control.allowReadText) answer.remove(QStringLiteral("stdout"));
                reply({QJsonObject::fromVariantMap(answer), {}});
            },
            code);
        if (!error.isEmpty()) return reply(failed(error, code));
        control.sessionOverlayOpened(owner);
        return;
    }
    if (fn == QLatin1String("screenshot")) {
        const auto scope = Control::captureScopeFromName(args.at(1).toString());
        if (!scope) return reply(failed(QStringLiteral("scope is pane, tab or window")));
        QString code;
        if (const QString refused = control.captureRefusal(code); !refused.isEmpty()) return reply(failed(refused, code));
        Pane* pane = paneAt(0);
        if (!pane) return reply(failed(QStringLiteral("no pane %1 in this window").arg(args.at(0).toInteger(-1)), QStringLiteral("no_pane")));
        if (!control.startSessionScreenshot()) {
            return reply(failed(QStringLiteral("%1 screenshots are in progress").arg(Control::MaxSessionScreenshots), QStringLiteral("busy")));
        }
        const QPointer<Control> guard(&control);
        const QString error = control.capture(
            pane->id(), *scope,
            [reply, guard, &settings](const QString& error, const QString& code, const QImage& image) {
                if (!guard) return reply(failed(QStringLiteral("Afterglow quits"), QStringLiteral("failed")));
                if (!error.isEmpty()) {
                    guard->endSessionScreenshot();
                    return reply(failed(error, code));
                }
                // A 4K PNG takes a moment to encode: not on the thread that
                // draws the windows.
                guard->encodePng(image, [reply, guard, &settings, size = image.size()](const QString& base64) {
                    if (guard) guard->endSessionScreenshot();
                    // Checked again as the picture goes out, as the control
                    // target checks it.
                    QString code;
                    const QString refused = guard ? guard->captureRefusal(code) : QStringLiteral("Afterglow quits");
                    if (!refused.isEmpty()) return reply(failed(refused, code.isEmpty() ? QStringLiteral("failed") : code));
                    if (!settings.control.enabled) return reply(failed(QStringLiteral("Control.enabled is off"), QStringLiteral("cancelled")));
                    if (base64.isEmpty()) return reply(failed(QStringLiteral("the PNG could not be written"), QStringLiteral("failed")));
                    reply({QJsonObject{{QStringLiteral("png"), base64}, {QStringLiteral("width"), size.width()},
                                       {QStringLiteral("height"), size.height()}},
                           {}});
                });
            },
            code);
        if (!error.isEmpty()) {
            control.endSessionScreenshot();
            return reply(failed(error, code));
        }
        return;
    }
    if (fn != QLatin1String("openProject")) return reply(answerNow(control, settings, window, fn, args));
    control.openProject(args.at(0).toString(), args.at(1).toBool(), args.at(2).toString(),
                        [reply = std::move(reply)](const QString& error, const QVariantMap& result) {
                            reply(error.isEmpty() ? MuxClient::ControlAnswer{QJsonObject::fromVariantMap(result), {}} : failed(error));
                        });
}

} // namespace Afterglow
