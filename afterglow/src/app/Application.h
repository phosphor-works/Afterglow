// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../TerminalWindow.h"
#include "../audio/AudioController.h"
#include "../control/Control.h"
#include "../control/ControlBus.h"
#include "../config/Settings.h"
#include "../scripting/ScriptHost.h"
#include "../shaders/ShaderLibrary.h"
#include "Services.h"

#include <AfterglowRender/GpuDevice.h>

#include <QDBusContext>
#include <QDBusMessage>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QStringList>

#include <memory>

namespace PhosphorShortcuts {
class IBackend;
class Registry;
} // namespace PhosphorShortcuts

namespace Afterglow {

/// The afterglow process, normally one per session. Launches of the same
/// executable with the same settings hand their request over D-Bus, so every
/// window shares this process's GPU device. A launch with --config, -o or
/// --class, or of a different build, runs on its own.
class Application : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.phosphorworks.Afterglow")

public:
    static constexpr const char* ServiceName = "com.phosphorworks.Afterglow";
    static constexpr const char* ObjectPath = "/com/phosphorworks/Afterglow";

    /// @p configPath (empty for the usual one) and @p overrides (afterglow -o)
    /// set up the settings.
    explicit Application(const QString& configPath = {}, const QJsonObject& overrides = {});
    ~Application() override;

    TerminalWindow* newWindow(const WindowOptions& options);
    /// NewWindow's work: an error message, empty on success.
    QString openForwarded(const QString& workingDirectory, const QStringList& command, const QVariantMap& options);

public Q_SLOTS:
    /// A window for another launch of afterglow, started from @p workingDirectory.
    /// @p options: directoryGiven (afterglow -d: start in it, whatever the
    /// workingDirectory setting), title, hold, and activationToken (the
    /// launcher's XDG_ACTIVATION_TOKEN, which lets the window take focus).
    /// Replies with an error when the window's program cannot start.
    Q_SCRIPTABLE void NewWindow(const QString& workingDirectory, const QStringList& command, const QVariantMap& options);
    Q_SCRIPTABLE void ToggleQuickTerminal();

private:
    void bindShortcuts();
    void quitWhenNoWindowsRemain();

    SettingsStore m_settings;
    AfterglowRender::GpuDevice m_device;
    QQmlEngine m_engine;
    ShaderLibrary m_shaders;
    // Before the services, which hand the script host to windows.
    Control m_control;
    ControlBus m_controlBus;
    ScriptHost m_scripts;
    AudioController m_audio;
    Services m_services;

    QPointer<TerminalWindow> m_quick;
    std::unique_ptr<PhosphorShortcuts::IBackend> m_shortcutBackend;
    std::unique_ptr<PhosphorShortcuts::Registry> m_shortcuts;
    QString m_boundQuickShortcut;
};

/// Answers launches that arrive while this process is still starting. main()
/// registers it at Application::ObjectPath once it owns the service name,
/// before the GPU device, the QML engine and the scripts load. Without it a
/// second launch in that time gets an unknown-object error and starts its
/// own process.
class LaunchQueue : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "com.phosphorworks.Afterglow")

public:
    explicit LaunchQueue(QObject* parent = nullptr);
    /// Hands the object path to @p application and answers every queued call through it.
    void replay(Application& application);

public Q_SLOTS:
    Q_SCRIPTABLE void NewWindow(const QString& workingDirectory, const QStringList& command, const QVariantMap& options);
    Q_SCRIPTABLE void ToggleQuickTerminal();

private:
    struct Call
    {
        QDBusMessage message;
        bool quick = false;
        QString workingDirectory;
        QStringList command;
        QVariantMap options;
    };
    QList<Call> m_calls;
    bool m_registered = false;
};

} // namespace Afterglow
