// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "control/SessionControl.h"

#include "Pane.h"
#include "TerminalWindow.h"
#include "config/Settings.h"
#include "config/Keybinds.h"
#include "control/Control.h"
#include "control/ControlSocket.h"

#include <AfterglowProto/IpcServer.h>

#include <QJsonObject>
#include <QMetaMethod>

#include <cmath>
#include <limits>

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

/// A pane map of Control as the daemon writes one: the daemon's ID of the
/// pane, window 0, the ID of a tab its place from 1, and no context, which
/// the daemon does not know. The command leaves only with read access.
QJsonObject daemonPaneMap(const Control& control, QVariantMap map)
{
    map = control.clientPaneMap(std::move(map));
    map.insert(QStringLiteral("id"), daemonId(map.value(QStringLiteral("id")).toUInt()));
    map.insert(QStringLiteral("window"), 0);
    map.insert(QStringLiteral("tab_id"), map.value(QStringLiteral("tab")).toInt() + 1);
    map.insert(QStringLiteral("context"), QString());
    QVariantMap neighbours = map.value(QStringLiteral("neighbours")).toMap();
    for (QVariant& id : neighbours) id = id.toUInt() ? daemonId(id.toUInt()) : 0;
    map.insert(QStringLiteral("neighbours"), neighbours);
    return QJsonObject::fromVariantMap(map);
}

/// The pane of argument @p index of @p window, by its ID in the daemon.
Pane* paneAt(const TerminalWindow& window, const QJsonArray& args, qsizetype index)
{
    const qint64 id = args.at(index).toInteger(-1);
    return id > 0 && id <= 0xffffffffLL ? window.sessionPane(std::uint32_t(id)) : nullptr;
}

QString noPane(const QJsonArray& args, qsizetype index)
{
    return QStringLiteral("no pane %1 in this window").arg(args.at(index).toInteger(-1));
}

/// A session of another host reaches what its programs could reach over
/// ssh, and no more: no program of this computer runs for it, and it reads
/// and writes nothing of this computer.
const QString OtherHostRunsNothingHere = QStringLiteral("a session of another host runs no program on this computer");

MuxClient::ControlAnswer failed(const QString& error, const QString& code = {})
{
    return {QJsonValue(), error, code};
}

/// The calls that a session of another host does not get: they start a
/// program here, open a window here, or read or write the projects, the
/// scripts or the screen of this computer.
bool localOnly(const QString& fn)
{
    static const QStringList Names{QStringLiteral("overlay"),      QStringLiteral("overlayInput"), QStringLiteral("screenshot"),
                                   QStringLiteral("newWindow"),    QStringLiteral("openDirectory"), QStringLiteral("openProject"),
                                   QStringLiteral("saveProject"),  QStringLiteral("projects"),     QStringLiteral("scriptDiagnostics")};
    return Names.contains(fn);
}

/// Whether keybind action @p action reaches past the window of the session
/// and its panes: it starts a program here, opens a window or a dialog of
/// this computer, reads or writes its clipboard or its config, or ends what
/// the user holds. No default: a new action must take a side here.
bool reachesThisComputer(Action action)
{
    switch (action) {
    case Action::NewWindow:
    case Action::Copy:
    case Action::Paste:
    case Action::SelectAll:
    case Action::ShowLastOutput:
    case Action::ShowFirstOutputOnScreen:
    case Action::ShowLastVisitedOutput:
    case Action::OpenConfig:
    case Action::ReloadConfig:
    case Action::UsePackageShaders:
    case Action::SaveProject:
    case Action::SwitchProject:
    case Action::EndScriptKeyMode:
    case Action::CancelOverlay:
        return true;
    case Action::None:
    case Action::SplitRight:
    case Action::SplitDown:
    case Action::SplitLeft:
    case Action::SplitUp:
    case Action::ClosePane:
    case Action::NewTab:
    case Action::CloseTab:
    case Action::CloseWindow:
    case Action::NextTab:
    case Action::PreviousTab:
    case Action::FocusLeft:
    case Action::FocusRight:
    case Action::FocusUp:
    case Action::FocusDown:
    case Action::EqualizeSplits:
    case Action::FontBigger:
    case Action::FontSmaller:
    case Action::FontReset:
    case Action::ScrollPageUp:
    case Action::ScrollPageDown:
    case Action::ScrollTop:
    case Action::ScrollBottom:
    case Action::JumpPromptUp:
    case Action::JumpPromptDown:
    case Action::Search:
    case Action::CommandPalette:
    case Action::UrlsOpen:
    case Action::UrlsCopy:
    case Action::UnicodeInput:
    case Action::ClearScreen:
    case Action::ResetTerminal:
    case Action::RenamePane:
    case Action::RenameTab:
    case Action::NotifyOnFinish:
    case Action::MoveTabLeft:
    case Action::MoveTabRight:
    case Action::GotoTab1:
    case Action::GotoTab2:
    case Action::GotoTab3:
    case Action::GotoTab4:
    case Action::GotoTab5:
    case Action::GotoTab6:
    case Action::GotoTab7:
    case Action::GotoTab8:
    case Action::GotoTab9:
    case Action::ToggleFullscreen:
    case Action::TogglePaneZoom:
    case Action::ResizeLeft:
    case Action::ResizeRight:
    case Action::ResizeUp:
    case Action::ResizeDown:
    case Action::CopyMode:
    case Action::ShowHints:
    case Action::SwapLeft:
    case Action::SwapRight:
    case Action::SwapUp:
    case Action::SwapDown:
    case Action::RotatePanesClockwise:
    case Action::RotatePanesCounterclockwise:
    case Action::PaneOverview:
    case Action::MovePaneToTab:
    case Action::MovePaneToWindow:
    case Action::DetachTab:
        return false;
    }
    return true;
}

/// Whether @p action, by its config name, reaches past the window of the
/// session (reachesThisComputer). A name that is no action is not refused
/// here: Control says that it is none.
bool reachesThisComputer(const QString& action)
{
    for (Action a : Keybinds::allActions()) {
        if (Keybinds::token(a) == action) return reachesThisComputer(a);
    }
    return false;
}

/// Why the call @p fn with @p args does not fit the method of the same name
/// of ControlApi, which the control target of a window has: a wrong number
/// of arguments, or one that does not convert to its type, in the words of
/// the router of the control socket. Nothing for a call that fits, and for
/// the daemon's own calls, which ControlApi does not have. The daemon passes
/// the arguments of a client as they came.
std::optional<MuxClient::ControlAnswer> argumentError(const QString& fn, const QJsonArray& args)
{
    const QMetaObject& api = ControlApi::staticMetaObject;
    const QByteArray name = fn.toLatin1();
    std::optional<QMetaMethod> method;
    for (int i = api.methodOffset(); i < api.methodCount(); ++i) {
        const QMetaMethod candidate = api.method(i);
        if (candidate.methodType() != QMetaMethod::Method || candidate.name() != name) continue;
        method = candidate;
        if (candidate.parameterCount() == args.size()) break;
    }
    if (!method) return std::nullopt;
    const QString invalid = QLatin1String(AfterglowProto::IpcErrorCode::InvalidArg);
    if (method->parameterCount() != args.size()) {
        return failed(QStringLiteral("argument count mismatch: 'control.%1' expects %2, got %3").arg(fn).arg(method->parameterCount()).arg(args.size()),
                      invalid);
    }
    for (qsizetype i = 0; i < args.size(); ++i) {
        QVariant value = args.at(i).toVariant();
        const QMetaType expected(method->parameterType(int(i)));
        if (value.metaType() == expected) continue;
        const char* given = value.typeName();
        if (!value.convert(expected)) {
            return failed(QStringLiteral("arg %1 for 'control.%2': cannot convert %3 to %4")
                              .arg(i)
                              .arg(fn, QLatin1String(given ? given : "<null-variant>"), QLatin1String(expected.name())),
                          invalid);
        }
    }
    return std::nullopt;
}

/// Who owns the overlays and the inputs of @p client of the session that
/// @p window shows.
QString sessionOwner(const TerminalWindow& window, qint64 client)
{
    return Control::sessionClientOwner(window.id(), client);
}

/// The answer of the calls that answer at once: all but clientGone, overlay,
/// overlayInput, screenshot, movePane, detachTab and openProject, which
/// answerSessionCall takes.
MuxClient::ControlAnswer answerNow(Control& control, TerminalWindow& window, const QString& fn, const QJsonArray& args)
{
    const bool otherHost = !window.sessionHost().isEmpty();
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
        // A refusal goes back for a call that waits for an answer: the
        // daemon's own placement (ID 0) gets none.
        Pane* placed = window.placeSessionPane(std::uint32_t(id), paneAt(window, args, 1), direction, command);
        if (!placed) placed = window.sessionPane(std::uint32_t(id));
        if (!placed) {
            return failed(window.closing() ? QStringLiteral("this window of session %1 is closing").arg(window.sessionName())
                                           : QStringLiteral("the window cannot show pane %1").arg(id));
        }
        // A pane that joined from another session brings the title that the
        // user gave it there, which any client of a session can set: cleaned
        // as a title of the layout is.
        if (const QString title = args.at(4).toString(); !title.isEmpty()) {
            placed->setUserTitle(sanitizedLine(title, std::numeric_limits<qsizetype>::max()));
        }
        return {};
    }
    // The calls without a pane: a session of another host has none of them
    // (localOnly).
    if (fn == QLatin1String("newWindow")) {
        quint32 created = 0;
        QStringList command;
        for (const QJsonValue& word : args.at(1).toArray()) command << word.toString();
        // With the options of a control client, as the window's own target takes them.
        const QString error = args.size() == 3
            ? control.clientNewWindow(args.at(0).toString(), command, args.at(2).toObject().toVariantMap(), created)
            : control.newWindow(args.at(0).toString(), command, created);
        if (!error.isEmpty()) return failed(error);
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
    Pane* pane = paneAt(window, args, 0);
    if (!pane) return failed(noPane(args, 0));
    const quint32 id = pane->id();
    if (fn == QLatin1String("focus")) {
        if (const QString error = control.focus(id); !error.isEmpty()) return failed(error);
        return {};
    }
    if (fn == QLatin1String("getText")) {
        // Only what the window knows comes here: the selection, and the
        // outputs that its view gives. The daemon has the other text.
        const auto scope = Control::scopeFromName(args.at(1).toString());
        if (!scope || (*scope != Control::TextScope::Selection && *scope != Control::TextScope::FirstOutputOnScreen
                       && *scope != Control::TextScope::LastVisitedOutput)) {
            return failed(Control::BadTextScope);
        }
        AfterglowVt::TextFormat format;
        if (const QString error = Control::textFormat(args.at(2).toObject().toVariantMap(), format); !error.isEmpty()) return failed(error);
        QString text;
        if (const QString error = control.readText(id, *scope, text, format); !error.isEmpty()) return failed(error);
        return {text, {}};
    }
    if (fn == QLatin1String("setTitle")) {
        if (const QString error = control.setTitle(id, args.at(1).toString()); !error.isEmpty()) return failed(error);
        return {};
    }
    if (fn == QLatin1String("focusNeighbour")) {
        quint32 focused = 0;
        if (const QString error = control.focusNeighbour(id, args.at(1).toString(), focused); !error.isEmpty()) return failed(error);
        // The daemon's ID of the pane: the client knows no other.
        return {daemonId(focused), {}};
    }
    if (fn == QLatin1String("resizePane")) {
        // As the control target reads the cells: a whole number, which
        // Control bounds.
        const double cells = args.at(2).toDouble(0);
        const int count = std::isfinite(cells) && std::floor(cells) == cells && cells >= 1 && cells <= Control::MaxResizeCells ? int(cells) : 0;
        if (const QString error = control.resizePane(id, args.at(1).toString(), count); !error.isEmpty()) return failed(error);
        return {};
    }
    if (fn == QLatin1String("scroll")) {
        const double amount = args.at(2).toDouble(0);
        if (!std::isfinite(amount) || std::floor(amount) != amount || std::abs(amount) > double(Control::MaxScrollAmount)) {
            return failed(QStringLiteral("the amount is a whole number from -%1 to %1").arg(Control::MaxScrollAmount));
        }
        if (const QString error = control.scroll(id, args.at(1).toString(), qint64(amount)); !error.isEmpty()) return failed(error);
        return {};
    }
    if (fn == QLatin1String("setTabTitle")) {
        if (const QString error = control.setTabTitle(id, args.at(1).toString()); !error.isEmpty()) return failed(error);
        return {};
    }
    if (fn == QLatin1String("signal")) {
        // As the control target reads a channel: a whole number up to 1024,
        // which Control names the valid channels for, else -1.
        const double channel = args.at(1).toDouble(-1);
        const int index = std::isfinite(channel) && std::floor(channel) == channel && channel >= 0 && channel <= 1024 ? int(channel) : -1;
        if (const QString error = control.signal(id, index, args.at(2).toDouble()); !error.isEmpty()) return failed(error);
        return {};
    }
    if (fn == QLatin1String("notify")) {
        // Another host can send them as often as it likes: they count as a
        // program's notifications of the pane do (Notifications.rateLimit).
        if (otherHost) {
            control.notice(id, args.at(1).toString(), args.at(2).toString());
            return {};
        }
        if (const QString error = control.notify(id, args.at(1).toString(), args.at(2).toString()); !error.isEmpty()) return failed(error);
        return {};
    }
    if (fn == QLatin1String("runAction")) {
        const QString action = args.at(1).toString();
        // A script command of another host's session goes through `aglow
        // run`, which runs only the commands that a script marks remote.
        if (otherHost && action.startsWith(Keybinds::ScriptPrefix)) {
            return failed(QStringLiteral("a session of another host runs only the script commands that a script marks remote, with aglow run"));
        }
        if (otherHost && reachesThisComputer(action)) return failed(OtherHostRunsNothingHere);
        // As the control target of the window: some read pane text.
        if (const QString error = control.runClientAction(id, action); !error.isEmpty()) return failed(error);
        return {};
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
    // The daemon tells it when a client that opened overlays or sent their
    // input went (ID 0: no answer).
    if (fn == QLatin1String("clientGone")) {
        control.endSessionClient(sessionOwner(window, args.at(0).toInteger(0)), QStringLiteral("The client went away"));
        return reply({});
    }
    // As the control target of the window, which Control.enabled takes
    // away: the daemon checks the settings that the window sent it last,
    // and a daemon of another host checks nothing for this computer. The
    // daemon's own changes still reach the window, which shows its layout
    // (ID 0: no answer).
    if (!settings.control.enabled && fn != QLatin1String("placePane") && fn != QLatin1String("focus")) {
        return reply(failed(QStringLiteral("no target 'control'"), QLatin1String(AfterglowProto::IpcErrorCode::NoSuchTarget)));
    }
    // As the router of the control socket refuses it: setTitle(pane) must
    // not clear the title.
    if (auto refused = argumentError(fn, args)) return reply(*refused);
    if (!window.sessionHost().isEmpty() && localOnly(fn)) return reply(failed(OtherHostRunsNothingHere));
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
        const auto spec = Control::clientOverlaySpec(argv, options, error);
        if (!error.isEmpty()) return reply(failed(error));
        Pane* pane = paneAt(window, args, 0);
        if (!pane) return reply(failed(noPane(args, 0), QStringLiteral("no_pane")));
        QString code;
        error = control.clientOverlay(
            owner, pane->id(), *spec,
            [reply](const QString& error, const QString& code, const QVariantMap& result) {
                reply(error.isEmpty() ? MuxClient::ControlAnswer{QJsonObject::fromVariantMap(result), {}} : failed(error, code));
            },
            code);
        if (!error.isEmpty()) return reply(failed(error, code));
        control.sessionOverlayOpened(owner);
        return;
    }
    if (fn == QLatin1String("screenshot")) {
        const auto scope = Control::captureScopeFromName(args.at(1).toString());
        if (!scope) return reply(failed(QStringLiteral("scope is pane, tab or window")));
        // The permission goes first, as for the control target: it holds
        // for every pane.
        QString code;
        if (const QString refused = control.captureRefusal(code); !refused.isEmpty()) return reply(failed(refused, code));
        Pane* pane = paneAt(window, args, 0);
        if (!pane) return reply(failed(noPane(args, 0), QStringLiteral("no_pane")));
        const QString error = control.screenshot(
            pane->id(), *scope,
            [reply](const QString& error, const QString& code, const QJsonObject& result) {
                reply(error.isEmpty() ? MuxClient::ControlAnswer{result, {}} : failed(error, code));
            },
            code);
        if (!error.isEmpty()) return reply(failed(error, code));
        return;
    }
    if (fn == QLatin1String("movePane") || fn == QLatin1String("detachTab")) {
        // A pane that leaves for a new session waits for the daemon off the
        // GUI thread: the answer goes then.
        Pane* pane = paneAt(window, args, 0);
        if (!pane) return reply(failed(noPane(args, 0)));
        quint32 target = 0;
        if (fn == QLatin1String("movePane") && args.at(1).toInteger(-1) != 0) {
            Pane* beside = paneAt(window, args, 1);
            if (!beside) return reply(failed(noPane(args, 1)));
            target = beside->id();
        }
        const Control::PaneMoved moved = [&control, reply](const QString& error, const QVariantMap& result) {
            reply(error.isEmpty() ? MuxClient::ControlAnswer{daemonPaneMap(control, result), {}} : failed(error));
        };
        if (fn == QLatin1String("movePane")) control.movePane(pane->id(), target, args.at(2).toString(), moved);
        else control.detachTab(pane->id(), moved);
        return;
    }
    if (fn != QLatin1String("openProject")) return reply(answerNow(control, window, fn, args));
    control.openProject(args.at(0).toString(), args.at(1).toBool(), args.at(2).toString(),
                        [reply = std::move(reply)](const QString& error, const QVariantMap& result) {
                            reply(error.isEmpty() ? MuxClient::ControlAnswer{QJsonObject::fromVariantMap(result), {}} : failed(error));
                        });
}

} // namespace Afterglow
