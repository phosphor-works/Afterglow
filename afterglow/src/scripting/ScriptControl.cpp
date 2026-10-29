// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ScriptControl.h"

#include "../control/Control.h"

namespace Afterglow {

namespace {

QVariantMap paneMap(const PaneInfo& p)
{
    return {
        {QStringLiteral("id"), p.id},
        {QStringLiteral("window"), p.window},
        {QStringLiteral("tab"), p.tab},
        {QStringLiteral("title"), p.title},
        {QStringLiteral("cwd"), p.workingDirectory},
        {QStringLiteral("command"), p.command},
        {QStringLiteral("cols"), p.cols},
        {QStringLiteral("rows"), p.rows},
        {QStringLiteral("focused"), p.focused},
    };
}

} // namespace

ScriptHost::Callbacks scriptCallbacks(Control& control)
{
    ScriptHost::Callbacks c;
    c.paneInfo = [&control](quint32 pane) {
        const auto info = control.paneInfo(pane);
        return info ? paneMap(*info) : QVariantMap{};
    };
    c.apply = [&control](const QVariantMap& e) -> QString {
        const QString kind = e.value(QStringLiteral("kind")).toString();
        const quint32 pane = e.value(QStringLiteral("pane")).toUInt();
        auto s = [&e](const char* key) { return e.value(QLatin1String(key)).toString(); };
        const QStringList command = e.value(QStringLiteral("command")).toStringList();
        quint32 created = 0;
        if (kind == QLatin1String("send_text")) {
            return control.sendText(pane, s("text"));
        }
        if (kind == QLatin1String("notify")) {
            return control.notify(pane, s("title"), s("body"));
        }
        if (kind == QLatin1String("split")) {
            return control.split(pane, s("direction") == QLatin1String("right"), command, created);
        }
        if (kind == QLatin1String("new_tab")) {
            return control.newTab(pane, s("cwd"), command, created);
        }
        if (kind == QLatin1String("new_window")) {
            return control.newWindow(s("cwd"), command, created);
        }
        if (kind == QLatin1String("focus")) {
            return control.focus(pane);
        }
        if (kind == QLatin1String("close")) {
            return control.close(pane);
        }
        if (kind == QLatin1String("set_title")) {
            return control.setTitle(pane, s("title"));
        }
        if (kind == QLatin1String("copy")) {
            return control.copy(s("text"));
        }
        if (kind == QLatin1String("open_url")) {
            return control.openUrl(s("url"));
        }
        if (kind == QLatin1String("action")) {
            return control.runAction(pane, s("name"));
        }
        return QStringLiteral("unknown effect \"%1\"").arg(kind);
    };
    return c;
}

void connectScriptEvents(ScriptHost& host, Control& control)
{
    auto on = [&host](const char* handler) {
        return [&host, handler](quint32 pane) { host.dispatch(QLatin1String(handler), pane); };
    };
    constexpr auto Queued = Qt::QueuedConnection;
    QObject::connect(&control, &Control::paneCreated, &host, on("on_pane_created"), Queued);
    QObject::connect(&control, &Control::paneClosed, &host, on("on_pane_closed"), Queued);
    QObject::connect(&control, &Control::bell, &host, on("on_bell"), Queued);
    QObject::connect(&control, &Control::focusChanged, &host, on("on_focus"), Queued);
    QObject::connect(&control, &Control::titleChanged, &host, [&host](quint32 pane, const QString& title) {
        host.dispatch(QStringLiteral("on_title"), pane, {{QStringLiteral("title"), title}});
    }, Queued);
    QObject::connect(&control, &Control::commandStarted, &host, [&host](quint32 pane, const QString& command) {
        host.dispatch(QStringLiteral("on_command_started"), pane, {{QStringLiteral("command"), command}});
    }, Queued);
    QObject::connect(&control, &Control::commandFinished, &host,
                     [&host](quint32 pane, const QString& command, int exitCode, qint64 durationMs) {
        host.dispatch(QStringLiteral("on_command_finished"), pane,
                      {{QStringLiteral("command"), command},
                       {QStringLiteral("exit_code"), exitCode},
                       {QStringLiteral("duration_ms"), durationMs}});
    }, Queued);
    QObject::connect(&control, &Control::statusChanged, &host,
                     [&host](quint32 pane, const QString& state, const QString& text) {
        host.dispatch(QStringLiteral("on_status"), pane, {{QStringLiteral("state"), state}, {QStringLiteral("text"), text}});
    }, Queued);
}

} // namespace Afterglow
