// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ScriptControl.h"

#include "../control/Control.h"
#include <cmath>

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
            if (kind == QLatin1String("open_directory")) return control.openDirectory(s("cwd"), s("layout"), data);
            if (kind == QLatin1String("save_project")) return control.saveProject(pane, s("name"), e.value(QStringLiteral("overwrite")).toBool(), data);
            if (kind == QLatin1String("open_project")) return control.openProject(s("name"), e.value(QStringLiteral("new_instance")).toBool(), data);
            if (kind == QLatin1String("list_projects")) { data.insert(QStringLiteral("names"), control.projectNames()); return {}; }
            if (kind == QLatin1String("move_pane") || kind == QLatin1String("detach_tab")) {
                QVariantMap moved;
                const auto error = kind == QLatin1String("move_pane")
                    ? control.movePane(pane, e.value(QStringLiteral("target")).toUInt(), s("placement"), moved)
                    : control.detachTab(pane, moved);
                if (error.isEmpty()) data = {{QStringLiteral("pane"), moved},
                    {QStringLiteral("pane_id"), pane}, {QStringLiteral("tab_id"), moved.value(QStringLiteral("tab_id"))},
                    {QStringLiteral("window_id"), moved.value(QStringLiteral("window"))}};
                return error;
            }
            if (kind == QLatin1String("read_text")) {
                const double limit = e.value(QStringLiteral("max_bytes")).toDouble();
                if (limit < 1 || limit > 1024 * 1024 || limit != std::floor(limit)) return QStringLiteral("invalid max_bytes");
                return control.readTextBounded(pane, s("scope"), int(limit), data);
            }
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
    c.applyOwned = [&control, apply = c.apply](const QString& owner, const QVariantMap& e,
                                               std::function<void(ScriptHost::EffectResult)> complete) -> ScriptHost::EffectResult {
        const auto kind = e.value(QStringLiteral("kind")).toString();
        const auto pane = e.value(QStringLiteral("pane")).toUInt();
        if (kind == QLatin1String("shaders") || kind == QLatin1String("shader_parameters")
            || kind == QLatin1String("shader_presets") || kind == QLatin1String("shader_preset")
            || kind == QLatin1String("save_shader_preset") || kind == QLatin1String("scene_state")) {
            QVariantMap data;
            const auto error = control.scriptShader(owner, pane, e, data);
            return {error, data, error.isEmpty() ? QString() : QStringLiteral("operation_failed")};
        }
        if (kind == QLatin1String("select") || kind == QLatin1String("prompt")) {
            const auto error = control.scriptDialog(owner, pane, e, [complete = std::move(complete)](QString error, QVariantMap data) {
                complete({error, data, error.isEmpty() ? QString() : QStringLiteral("cancelled")});
            });
            return {error, {}, error.isEmpty() ? QString() : QStringLiteral("busy"), error.isEmpty()};
        }
        if (kind == QLatin1String("status") || kind == QLatin1String("override")) {
            const auto error = control.scriptAppearance(owner, pane, e);
            return {error, {}, error.isEmpty() ? QString() : QStringLiteral("operation_failed")};
        }
        return apply(e);
    };
    c.cleanup = [&control](const QString& owner) { control.clearScriptOwner(owner); };
    return c;
}

void connectScriptEvents(ScriptHost& host, Control& control)
{
    auto on = [&host](const char* handler) {
        return [&host, handler](quint32 pane) { host.dispatch(QLatin1String(handler), pane); };
    };
    constexpr auto Queued = Qt::QueuedConnection;
    QObject::connect(&control, &Control::paneMoved, &host, on("on_pane_moved"), Queued);
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
