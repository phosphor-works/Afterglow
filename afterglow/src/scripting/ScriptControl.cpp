// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ScriptControl.h"

#include "../control/Control.h"

namespace Afterglow {

ScriptHost::Callbacks scriptCallbacks(Control& control)
{
    ScriptHost::Callbacks c;
    c.paneInfo = [&control](quint32 pane) {
        const auto info = control.paneInfo(pane);
        return info ? Control::paneMap(*info) : QVariantMap{};
    };
    c.state = [&control] { return control.state(); };
    c.structuralAction = &Control::isStructuralAction;
    c.apply = [&control](const QVariantMap& e) -> ScriptHost::EffectResult {
        const QString kind = e.value(QStringLiteral("kind")).toString();
        const quint32 pane = e.value(QStringLiteral("pane")).toUInt();
        auto s = [&e](const char* key) { return e.value(QLatin1String(key)).toString(); };
        const QStringList command = e.value(QStringLiteral("command")).toStringList();
        quint32 created = 0;
        QVariantMap data;
        const QString error = [&]() -> QString {
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
            if (kind == QLatin1String("signal")) {
                return control.signal(pane, e.value(QStringLiteral("channel")).toInt(),
                                      e.value(QStringLiteral("value")).toDouble());
            }
            if (kind == QLatin1String("signals")) {
                QVariantList updated;
                const QString error = control.signalsBatch(pane, s("scope"), e.value(QStringLiteral("values")).toList(), updated);
                if (error.isEmpty()) {
                    data.insert(QStringLiteral("panes"), updated);
                }
                return error;
            }
            if (kind == QLatin1String("action")) {
                return control.runAction(pane, s("name"));
            }
            return QStringLiteral("unknown effect \"%1\"").arg(kind);
        }();
        if (error.isEmpty() && created) {
            if (const auto p = control.paneInfo(created)) {
                data = {{QStringLiteral("pane"), Control::paneMap(*p)}, {QStringLiteral("pane_id"), p->id},
                        {QStringLiteral("tab_id"), p->tabId}, {QStringLiteral("window_id"), p->window}};
            }
        }
        return {error, data, error.isEmpty() ? QString() : QStringLiteral("operation_failed")};
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
    QObject::connect(&control, &Control::userVarChanged, &host,
                     [&host](quint32 pane, const QString& name, const QString& value) {
        host.dispatch(QStringLiteral("on_user_var"), pane, {{QStringLiteral("name"), name}, {QStringLiteral("value"), value}});
    }, Queued);
    QObject::connect(&control, &Control::contextChanged, &host, [&host](quint32 pane, const QString& context) {
        host.dispatch(QStringLiteral("on_context"), pane, {{QStringLiteral("context"), context}});
    }, Queued);
    QObject::connect(&control, &Control::statusChanged, &host,
                     [&host](quint32 pane, const QString& state, const QString& text) {
        host.dispatch(QStringLiteral("on_status"), pane, {{QStringLiteral("state"), state}, {QStringLiteral("text"), text}});
    }, Queued);
}

} // namespace Afterglow
