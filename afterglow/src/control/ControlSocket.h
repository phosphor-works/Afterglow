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
    Q_INVOKABLE void sendText(uint pane, const QString& text);
    /// @p scope: screen, scrollback, selection or last-output.
    Q_INVOKABLE QString getText(uint pane, const QString& scope);
    /// @p direction: left, right, up or down. Returns the new pane.
    Q_INVOKABLE uint split(uint pane, const QString& direction, const QStringList& command);
    Q_INVOKABLE uint newTab(uint pane, const QString& workingDirectory, const QStringList& command);
    Q_INVOKABLE uint newWindow(const QString& workingDirectory, const QStringList& command);
    Q_INVOKABLE void focus(uint pane);
    Q_INVOKABLE void close(uint pane);
    Q_INVOKABLE void setTitle(uint pane, const QString& title);
    Q_INVOKABLE void signal(uint pane, int channel, double value);
    Q_INVOKABLE void notify(uint pane, const QString& title, const QString& body);
    /// copy, paste, select_all and show_last_output need Control.allowReadText.
    Q_INVOKABLE void runAction(uint pane, const QString& action);
    Q_INVOKABLE QVariantMap movePane(uint pane, uint target, const QString& placement);
    Q_INVOKABLE QVariantMap detachTab(uint pane);
    Q_INVOKABLE QVariantMap openDirectory(const QString& directory, const QString& layout);
    Q_INVOKABLE QStringList projects();
    Q_INVOKABLE QVariantMap saveProject(uint pane, const QString& name, bool overwrite);
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
    QString readable(const QString& text) const;
    /// Sends @p error as the call's error reply. True when there is one.
    bool failed(const QString& error);

    Control& m_control;
    SettingsStore& m_settings;
};

/// External control on the process's socket (InstanceSocket): the "control"
/// target, registered while Control.enabled is on. It sends Control's events
/// to the clients that subscribed, and merges the state events of each pane
/// that come faster than Control.eventInterval.
class ControlSocket : public QObject
{
    Q_OBJECT

public:
    static constexpr const char* Target = "control";

    ControlSocket(Control& control, SettingsStore& settings, PhosphorIpc::IpcRouter& router, QObject* parent = nullptr);
    ~ControlSocket() override;

    /// The control target is on the router.
    bool enabled() const { return m_enabled; }

private:
    void applySettings();
    void send(const char* event, const QJsonArray& args);
    /// Sends now, or later with the newest @p args when the last event of
    /// this @p event, @p pane and @p name went out less than
    /// Control.eventInterval ago.
    void sendMerged(const char* event, quint32 pane, const QString& name, const QJsonArray& args);
    void sendDue();
    /// Sends what waits for @p pane and forgets the pane.
    void flushPane(quint32 pane);
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
