// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "Control.h"

#include <PhosphorIpc/IpcCallContext.h>
#include <PhosphorIpc/IpcRouter.h>

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QTimer>

#include <optional>

namespace Afterglow {

class SettingsStore;

/// The calls of the control socket's "control" target. A call that fails
/// sends an error reply (CALL_FAILED) with Control's message, and its return
/// value is dropped. The signals name the events that a client can subscribe
/// to. ControlSocket sends them; Qt never emits them.
class ControlApi : public QObject, protected PhosphorIpc::IpcCallContext
{
    Q_OBJECT

public:
    ControlApi(Control& control, SettingsStore& settings, QObject* parent = nullptr);

    /// The pane objects of Control::paneMap.
    Q_INVOKABLE QVariantList listPanes();
    Q_INVOKABLE uint focusedPane();
    // A pane argument is a double, the type of a JSON number: paneId() checks
    // that it is a whole number in the range of a pane ID.
    Q_INVOKABLE void sendText(double pane, const QString& text);
    /// @p scope: screen, scrollback, selection or last-output.
    Q_INVOKABLE QString getText(double pane, const QString& scope);
    /// @p direction: left, right, up or down. Returns the new pane.
    Q_INVOKABLE uint split(double pane, const QString& direction, const QStringList& command);
    Q_INVOKABLE uint newTab(double pane, const QString& workingDirectory, const QStringList& command);
    Q_INVOKABLE uint newWindow(const QString& workingDirectory, const QStringList& command);
    Q_INVOKABLE void focus(double pane);
    Q_INVOKABLE void close(double pane);
    Q_INVOKABLE void setTitle(double pane, const QString& title);
    Q_INVOKABLE void signal(double pane, double channel, double value);
    Q_INVOKABLE void notify(double pane, const QString& title, const QString& body);
    /// copy, paste, select_all and show_last_output need Control.allowReadText.
    Q_INVOKABLE void runAction(double pane, const QString& action);
    Q_INVOKABLE QVariantMap movePane(double pane, double target, const QString& placement);
    Q_INVOKABLE QVariantMap detachTab(double pane);
    Q_INVOKABLE QVariantMap openDirectory(const QString& directory, const QString& layout);
    Q_INVOKABLE QStringList projects();
    Q_INVOKABLE QVariantMap saveProject(double pane, const QString& name, bool overwrite);
    Q_INVOKABLE QVariantMap openProject(const QString& name, bool newInstance);
    Q_INVOKABLE QVariantList scriptDiagnostics();

Q_SIGNALS:
    void paneCreated(uint pane);
    void paneMoved(uint pane);
    void paneClosed(uint pane);
    /// Sent whatever Control.allowReadText says, like the titles of
    /// listPanes, although a title can show the running command.
    void titleChanged(uint pane, const QString& title);
    void bell(uint pane);
    /// The command text is empty unless Control.allowReadText.
    void commandStarted(uint pane, const QString& command);
    void commandFinished(uint pane, const QString& command, int exitCode, qint64 durationMs);
    void statusChanged(uint pane, const QString& state, const QString& text);
    void focusChanged(uint pane);
    /// The value is empty unless Control.allowReadText.
    void userVarChanged(uint pane, const QString& name, const QString& value);
    void contextChanged(uint pane, const QString& context);

private:
    /// Sends @p error as the call's error reply. True when there is one.
    bool failed(const QString& error);
    /// The pane ID in @p value, or nothing after an error reply.
    std::optional<quint32> paneId(double value);

    Control& m_control;
    SettingsStore& m_settings;
};

/// External control on the process's socket (InstanceSocket): the "control"
/// target, registered while Control.enabled is on. It sends Control's events
/// to the clients that subscribed, and merges the state events of each pane
/// that come faster than Control.eventInterval. What waits for a pane goes
/// out before the pane's next event that is never merged.
class ControlSocket : public QObject
{
    Q_OBJECT

public:
    static constexpr const char* Target = "control";

    ControlSocket(Control& control, SettingsStore& settings, PhosphorIpc::IpcRouter& router, QObject* parent = nullptr);
    ~ControlSocket() override;

    /// The control target is on the router. For the tests: the router is
    /// what clients see.
    bool enabled() const { return m_enabled; }

private:
    void applySettings();
    /// Applies Control.allowReadText as the event goes out.
    void send(const char* event, QJsonArray args);
    /// Sends an event that is never merged, after what waits for @p pane.
    void sendNow(const char* event, quint32 pane, const QJsonArray& args);
    /// Sends now, or later with the newest @p args when the last event of
    /// this @p event, @p pane and @p name went out less than
    /// Control.eventInterval ago.
    void sendMerged(const char* event, quint32 pane, const QString& name, const QJsonArray& args);
    void sendDue();
    /// Drops what the merge holds for a closed pane.
    void forgetPane(quint32 pane);
    void scheduleDue();

    struct Merged
    {
        const char* event = nullptr;
        quint32 pane = 0;
        QJsonArray args;
        qint64 sentAt = 0;
        bool waiting = false;
    };

    Control& m_control;
    SettingsStore& m_settings;
    ControlApi m_api;
    PhosphorIpc::IpcRouter& m_router;
    bool m_enabled = false;
    QHash<QString, Merged> m_merged;
    QElapsedTimer m_clock;
    QTimer m_due;
};

} // namespace Afterglow
