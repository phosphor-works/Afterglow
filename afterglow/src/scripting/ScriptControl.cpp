// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ScriptControl.h"

#include "../control/Control.h"
#include <cmath>
#include <memory>
#include <optional>

namespace Afterglow {

namespace {

/// The result of an effect that gives a pane: @p pane, a paneMap, with its
/// IDs on their own.
QVariantMap paneResult(const QVariantMap& pane)
{
    return {{QStringLiteral("pane"), pane},
            {QStringLiteral("pane_id"), pane.value(QStringLiteral("id"))},
            {QStringLiteral("tab_id"), pane.value(QStringLiteral("tab_id"))},
            {QStringLiteral("window_id"), pane.value(QStringLiteral("window"))}};
}

using Completion = std::function<void(ScriptHost::EffectResult)>;

/// Runs @p start, which gives its completion a result once: before it
/// returns, which is the result now, or later, which goes to @p complete
/// and makes the result now a deferred one.
ScriptHost::EffectResult deferrable(const std::function<void(Completion finish)>& start, Completion complete)
{
    struct Outcome
    {
        bool returned = false;
        std::optional<ScriptHost::EffectResult> now;
    };
    const auto outcome = std::make_shared<Outcome>();
    start([outcome, complete = std::move(complete)](ScriptHost::EffectResult result) {
        if (outcome->returned)
            complete(std::move(result));
        else
            outcome->now = std::move(result);
    });
    outcome->returned = true;
    if (outcome->now)
        return *outcome->now;
    return {{}, {}, {}, true};
}

/// The result of a split, tab or move, at once or later: operation_failed on
/// an error, a code that ScriptHost gives only to a result that comes at once.
ScriptHost::EffectResult laterResult(const QString& error, const QVariantMap& data)
{
    return {error, error.isEmpty() ? data : QVariantMap{},
            error.isEmpty() ? QString() : QStringLiteral("operation_failed")};
}

} // namespace

ScriptHost::Callbacks scriptCallbacks(Control& control)
{
    ScriptHost::Callbacks c;
    c.paneInfo = [&control](quint32 pane) {
        const auto info = control.paneInfo(pane);
        return info ? Control::paneMap(*info) : QVariantMap{};
    };
    c.paneExists = [&control](quint32 pane) {
        return control.hasPane(pane);
    };
    c.state = [&control] {
        return control.state();
    };
    c.structuralAction = &Control::isStructuralAction;
    c.apply = [&control](const QVariantMap& e) -> ScriptHost::EffectResult {
        const QString kind = e.value(QStringLiteral("kind")).toString();
        const quint32 pane = e.value(QStringLiteral("pane")).toUInt();
        auto s = [&e](const char* key) {
            return e.value(QLatin1String(key)).toString();
        };
        const QStringList command = e.value(QStringLiteral("command")).toStringList();
        quint32 created = 0;
        QVariantMap data;
        const QString error = [&]() -> QString {
            if (kind == QLatin1String("open_directory"))
                return control.openDirectory(s("cwd"), s("layout"), data);
            if (kind == QLatin1String("save_project"))
                return control.saveProject(pane, s("name"), e.value(QStringLiteral("overwrite")).toBool(), data);
            if (kind == QLatin1String("list_projects")) {
                data.insert(QStringLiteral("names"), control.projectNames());
                return {};
            }
            // ScriptHost marks the reads of a run whose client, a process
            // outside the window, can get the text.
            const bool clientRun = e.value(QStringLiteral("client_run")).toBool();
            if (kind == QLatin1String("semantic_regions"))
                return control.semanticRegions(pane, data, clientRun);
            if (kind == QLatin1String("read_text") || kind == QLatin1String("read_region")) {
                const double limit = e.value(QStringLiteral("max_bytes")).toDouble();
                if (!std::isfinite(limit) || limit < 1 || limit > ScriptHost::MaxReadTextBytes
                    || limit != std::floor(limit))
                    return ScriptHost::maxBytesError();
                return kind == QLatin1String("read_region")
                    ? control.readRegion(pane, s("region"), int(limit), data, clientRun)
                    : control.readTextBounded(pane, s("scope"), int(limit), data, clientRun);
            }
            if (kind == QLatin1String("send_text")) {
                return control.sendText(pane, s("text"));
            }
            if (kind == QLatin1String("notify")) {
                return control.notify(pane, s("title"), s("body"));
            }
            if (kind == QLatin1String("new_window")) {
                return control.newWindow(s("cwd"), command, e.value(QStringLiteral("options")).toMap(), created);
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
                const QString error =
                    control.signalsBatch(pane, s("scope"), e.value(QStringLiteral("values")).toList(), updated);
                if (error.isEmpty()) {
                    data.insert(QStringLiteral("panes"), updated);
                }
                return error;
            }
            if (kind == QLatin1String("action")) {
                return control.runScriptAction(pane, s("name"));
            }
            if (kind == QLatin1String("move_tab"))
                return control.moveTab(pane, int(e.value(QStringLiteral("index")).toDouble()));
            if (kind == QLatin1String("overview_move")) {
                return control.overviewMove(pane, e.value(QStringLiteral("target")).toUInt(),
                                            int(e.value(QStringLiteral("step")).toDouble()), data);
            }
            if (kind == QLatin1String("pulse")) {
                // ScriptHost has turned a pulse name into its number.
                return control.pulse(pane, s("effect").toInt(), int(e.value(QStringLiteral("duration_ms")).toDouble()),
                                     int(e.value(QStringLiteral("color")).toDouble()));
            }
            return QStringLiteral("unknown effect \"%1\"").arg(kind);
        }();
        if (error.isEmpty() && created) {
            if (const auto p = control.paneInfo(created))
                data = paneResult(Control::paneMap(*p));
        }
        if (error.isEmpty())
            return {error, data, {}};
        return {error, data, QStringLiteral("operation_failed")};
    };
    c.applyOwned = [&control, apply = c.apply](
                       const QString& owner, const QVariantMap& e,
                       std::function<void(ScriptHost::EffectResult)> complete) -> ScriptHost::EffectResult {
        const auto kind = e.value(QStringLiteral("kind")).toString();
        const auto pane = e.value(QStringLiteral("pane")).toUInt();
        // A refusal for a pane that has gone is no_pane, as everywhere else.
        // Only the error on a live pane depends on the kind.
        auto code = [&control, pane](const QString& error, const QString& live) {
            return error.isEmpty() ? QString() : control.hasPane(pane) ? live : QStringLiteral("no_pane");
        };
        // A dialog, key mode or tab menu is refused because another item
        // holds the window.
        auto refusal = [&code](const QString& error) {
            return code(error, QStringLiteral("busy"));
        };
        if (kind == QLatin1String("shaders") || kind == QLatin1String("shader_parameters")
            || kind == QLatin1String("shader_presets") || kind == QLatin1String("shader_preset")
            || kind == QLatin1String("save_shader_preset") || kind == QLatin1String("scene_state")) {
            QVariantMap data;
            const auto error = control.scriptShader(owner, pane, e, data);
            return {error, data, code(error, QStringLiteral("operation_failed"))};
        }
        if (kind == QLatin1String("select") || kind == QLatin1String("prompt")) {
            const auto error =
                control.scriptDialog(owner, pane, e, [complete = std::move(complete)](QString error, QVariantMap data) {
                    complete({error, data, error.isEmpty() ? QString() : QStringLiteral("cancelled")});
                });
            return {error, {}, refusal(error), error.isEmpty()};
        }
        if (kind == QLatin1String("overlay")) {
            // ScriptHost read the spec by OverlaySpec's rules.
            const QVariant spec = e.value(QStringLiteral("spec"));
            if (!spec.canConvert<OverlaySpec>())
                return {QStringLiteral("The overlay has no spec"), {}, QStringLiteral("operation_failed")};
            QString refusalCode;
            const QString error = control.overlay(
                owner, e.value(QStringLiteral("request_id")).toString(), pane, spec.value<OverlaySpec>(),
                [complete = std::move(complete)](const QString& message, const QString& code, const QVariantMap& data) {
                    complete({message, data, code});
                },
                refusalCode);
            return {error, {}, error.isEmpty() ? QString() : refusalCode, error.isEmpty()};
        }
        if (kind == QLatin1String("open_project")) {
            // openProject answers at once for a window, a refusal or a reuse.
            // A session opens off the GUI thread, and an open that waits for
            // another one answers later: the result comes then.
            return deferrable(
                [&](Completion finish) {
                    control.openProject(
                        e.value(QStringLiteral("name")).toString(), e.value(QStringLiteral("new_instance")).toBool(),
                        {}, [finish](const QString& error, const QVariantMap& data) {
                            finish({error, data, error.isEmpty() ? QString() : QStringLiteral("operation_failed")});
                        });
                },
                std::move(complete));
        }
        if (kind == QLatin1String("split") || kind == QLatin1String("new_tab")) {
            // In the window of a mux session, the daemon starts the program
            // off the GUI thread, and the result comes then. Elsewhere it
            // comes at once.
            const QStringList command = e.value(QStringLiteral("command")).toStringList();
            const QVariantMap options = e.value(QStringLiteral("options")).toMap();
            return deferrable(
                [&](Completion finish) {
                    const Control::PaneMade made = [&control, finish](const QString& error, quint32 created) {
                        QVariantMap data;
                        if (error.isEmpty() && created) {
                            if (const auto p = control.paneInfo(created))
                                data = paneResult(Control::paneMap(*p));
                        }
                        finish(laterResult(error, data));
                    };
                    if (kind == QLatin1String("split")) {
                        control.split(pane, e.value(QStringLiteral("direction")).toString(), command, options, made);
                    } else {
                        control.newTab(pane, e.value(QStringLiteral("cwd")).toString(), command, options, made);
                    }
                },
                std::move(complete));
        }
        if (kind == QLatin1String("move_pane") || kind == QLatin1String("detach_tab")) {
            // ScriptHost checks the effect's pane, not the target: a target
            // that has gone is no_pane too.
            const quint32 target = e.value(QStringLiteral("target")).toUInt();
            if (kind == QLatin1String("move_pane") && target != 0 && !control.hasPane(target)) {
                return {QStringLiteral("no pane %1").arg(target), {}, QStringLiteral("no_pane")};
            }
            // A pane of a mux session that moves to a new window leaves in a
            // new session, and one that moves to the window of another
            // session joins it, off the GUI thread, and the pane of the window
            // that shows it has another id: the result comes then. Control
            // fails when no window shows it.
            return deferrable(
                [&](Completion finish) {
                    const Control::PaneMoved moved = [finish](const QString& error, const QVariantMap& result) {
                        finish(laterResult(error, error.isEmpty() ? paneResult(result) : QVariantMap{}));
                    };
                    if (kind == QLatin1String("move_pane")) {
                        control.movePane(pane, target, e.value(QStringLiteral("placement")).toString(), moved);
                    } else {
                        control.detachTab(pane, moved);
                    }
                },
                std::move(complete));
        }
        if (kind == QLatin1String("overview")) {
            // The window knows which script's overview it shows.
            // Another script's key mode holds the keyboard: busy, as for a
            // dialog or a key mode.
            QVariantMap data;
            QString held;
            const auto error =
                control.overview(owner, pane, e.value(QStringLiteral("operation")).toString(), data, &held);
            return {error, data,
                    error.isEmpty() || held.isEmpty() ? code(error, QStringLiteral("operation_failed")) : held};
        }
        if (kind == QLatin1String("key_mode")) {
            const auto error = control.scriptKeyMode(owner, pane, e.value(QStringLiteral("label")).toString());
            return {error, {}, refusal(error)};
        }
        if (kind == QLatin1String("tab_menu")) {
            const auto error = control.tabMenu(pane);
            return {error, {}, refusal(error)};
        }
        if (kind == QLatin1String("end_key_mode")) {
            control.endScriptKeyMode(owner);
            return {};
        }
        if (kind == QLatin1String("dock") || kind == QLatin1String("undock")) {
            // On a live pane, the window refuses a dock only past its count
            // limits: a capacity, which tasks also report as rate_limited.
            // A pane that has gone, also for a pane area, is no_pane.
            const auto error = control.scriptDock(owner, pane, e);
            return {error, {}, code(error, QStringLiteral("rate_limited"))};
        }
        if (kind == QLatin1String("status") || kind == QLatin1String("override")) {
            const auto error = control.scriptAppearance(owner, pane, e);
            return {error, {}, code(error, QStringLiteral("operation_failed"))};
        }
        return apply(e);
    };
    c.cleanup = [&control](const QString& owner) {
        control.clearScriptOwner(owner);
    };
    c.closeRunWaits = [&control](const QString& owner, const QString& prefix) {
        control.closeRunWaits(owner, prefix);
    };
    c.notice = [&control](quint32 pane, const QString& title, const QString& body) {
        control.notice(pane, title, body);
    };
    // A failing script never keeps the keyboard.
    c.failed = [&control](const QString& owner) {
        control.endScriptKeyMode(owner);
    };
    return c;
}

void connectScriptEvents(ScriptHost& host, Control& control)
{
    auto on = [&host](const char* handler) {
        return [&host, handler](quint32 pane) {
            host.dispatch(QLatin1String(handler), pane);
        };
    };
    constexpr auto Queued = Qt::QueuedConnection;
    QObject::connect(&control, &Control::paneMoved, &host, on("on_pane_moved"), Queued);
    QObject::connect(
        &control, &Control::paneCreated, &host,
        [&host, &control](quint32 pane) {
            // A split says where it came from; the origin is known once the split finishes.
            host.dispatch(QStringLiteral("on_pane_created"), pane, control.paneOrigin(pane));
        },
        Queued);
    QObject::connect(
        &control, &Control::scriptKey, &host,
        [&host](const QString& owner, quint32 pane, const QVariantMap& key) {
            host.dispatchTo(owner, QStringLiteral("on_key"), pane, key);
        },
        Queued);
    QObject::connect(
        &control, &Control::scriptDockEvent, &host,
        [&host](const QString& owner, const QString& handler, quint32 pane, const QVariantMap& event, bool limited) {
            host.dispatchTo(owner, handler, pane, event, limited);
        },
        Queued);
    QObject::connect(
        &control, &Control::tabSwitched, &host,
        [&host](quint32 pane, quint32 from, quint32 to, int direction) {
            host.dispatch(QStringLiteral("on_tab_switch"), pane,
                          {{QStringLiteral("from_tab"), from},
                           {QStringLiteral("to_tab"), to},
                           {QStringLiteral("direction"), direction}});
        },
        Queued);
    QObject::connect(
        &control, &Control::paneClosedDetailed, &host,
        [&host](quint32 pane, quint32 tab, quint32 window) {
            host.dispatch(QStringLiteral("on_pane_closed"), pane,
                          {{QStringLiteral("pane"),
                            QVariantMap{{QStringLiteral("id"), pane},
                                        {QStringLiteral("tab_id"), tab},
                                        {QStringLiteral("window"), window}}}});
        },
        Queued);
    QObject::connect(&control, &Control::bell, &host, on("on_bell"), Queued);
    QObject::connect(&control, &Control::focusChanged, &host, on("on_focus"), Queued);
    QObject::connect(
        &control, &Control::titleChanged, &host,
        [&host](quint32 pane, const QString& title) {
            host.dispatch(QStringLiteral("on_title"), pane, {{QStringLiteral("title"), title}});
        },
        Queued);
    QObject::connect(
        &control, &Control::commandStarted, &host,
        [&host](quint32 pane, const QString& command) {
            host.dispatch(QStringLiteral("on_command_started"), pane, {{QStringLiteral("command"), command}});
        },
        Queued);
    QObject::connect(
        &control, &Control::commandFinished, &host,
        [&host](quint32 pane, const QString& command, int exitCode, qint64 durationMs) {
            host.dispatch(QStringLiteral("on_command_finished"), pane,
                          {{QStringLiteral("command"), command},
                           {QStringLiteral("exit_code"), exitCode},
                           {QStringLiteral("duration_ms"), durationMs}});
        },
        Queued);
    QObject::connect(
        &control, &Control::userVarChanged, &host,
        [&host](quint32 pane, const QString& name, const QString& value) {
            host.dispatch(QStringLiteral("on_user_var"), pane,
                          {{QStringLiteral("name"), name}, {QStringLiteral("value"), value}});
        },
        Queued);
    QObject::connect(
        &control, &Control::contextChanged, &host,
        [&host](quint32 pane, const QString& context) {
            host.dispatch(QStringLiteral("on_context"), pane, {{QStringLiteral("context"), context}});
        },
        Queued);
    QObject::connect(
        &control, &Control::statusChanged, &host,
        [&host](quint32 pane, const QString& state, const QString& text) {
            host.dispatch(QStringLiteral("on_status"), pane,
                          {{QStringLiteral("state"), state}, {QStringLiteral("text"), text}});
        },
        Queued);
}

} // namespace Afterglow
