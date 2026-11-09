// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../TerminalWindow.h"
#include "../audio/AudioController.h"
#include "../control/Control.h"
#include "../control/CommandSocket.h"
#include "../control/RemoteSockets.h"
#include "../control/ControlSocket.h"
#include "../config/Settings.h"
#include "../scripting/ScriptHost.h"
#include "../shaders/ShaderLibrary.h"
#include "Services.h"

#include <AfterglowRender/GpuDevice.h>
#include <PhosphorIpc/IpcCallContext.h>

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QStringList>

#include <memory>

namespace PhosphorShortcuts {
class IBackend;
class Registry;
} // namespace PhosphorShortcuts

namespace Afterglow {

class Application;
class InstanceSocket;

/// The "launch" target of the process's socket. Later launches of the same
/// executable hand their window over to this process, so every window shares
/// its GPU device. The application registers it once it is ready: a launch
/// that arrives before gets NO_SUCH_TARGET and asks again.
class LaunchApi : public QObject, protected PhosphorIpc::IpcCallContext
{
    Q_OBJECT

public:
    static constexpr const char* Target = "launch";

    explicit LaunchApi(Application& application);

    /// A window for another launch, started from @p workingDirectory.
    /// @p options: directoryGiven (afterglow -d: start in it, whatever the
    /// workingDirectory setting), title, hold, activationToken (the
    /// launcher's XDG_ACTIVATION_TOKEN, which lets the window take focus),
    /// session (afterglow mux attach: show that mux session) and takeover
    /// (true by default: take the session from a window that shows it).
    /// Fails when the window's program cannot start, or the session cannot
    /// show.
    Q_INVOKABLE void newWindow(const QString& workingDirectory, const QStringList& command, const QVariantMap& options);
    /// Fails when the quick terminal's shell cannot start.
    Q_INVOKABLE void toggleQuickTerminal();

private:
    Application& m_application;
};

/// The afterglow process, normally one for each Wayland display. Launches of
/// the same executable with the same settings hand their request over the
/// process's socket (InstanceSocket), so every window shares this process's
/// GPU device. A launch with --config, -o or --class, or of a different
/// build, runs on its own.
class Application : public QObject
{
    Q_OBJECT

public:
    /// @p socket carries launches and external control. @p configPath (empty
    /// for the usual one) and @p overrides (afterglow -o) set up the settings.
    explicit Application(InstanceSocket& socket, const QString& configPath = {}, const QJsonObject& overrides = {});
    ~Application() override;

    TerminalWindow* newWindow(const WindowOptions& options);
    /// The window of a launch, this process's own or a later one's (the
    /// options of LaunchApi::newWindow): an error message, empty on success.
    QString openLaunch(const QString& workingDirectory, const QStringList& command, const QVariantMap& options);
    /// An error message, empty on success.
    QString toggleQuickTerminal();

private:
    void bindShortcuts();
    void quitWhenNoWindowsRemain();

    SettingsStore m_settings;
    AfterglowRender::GpuDevice m_device;
    QQmlEngine m_engine;
    // The chrome scene, one for all windows. It outlives every window
    // (~Application deletes them first) and goes just before the engine it
    // loads into: a window's own component could be freed while its
    // asynchronous load still runs.
    QQmlComponent m_chromeComponent;
    ShaderLibrary m_shaders;
    // Before the services, which hand the script host to windows.
    Control m_control;
    InstanceSocket& m_socket;
    ControlSocket m_controlSocket;
    ScriptHost m_scripts;
    // After the scripts: it goes first and fails the replies that wait, and
    // the scripts then end its runs without a client.
    CommandSocket m_commandSocket;
    // After the scripts and the control target, which calls it.
    RemoteSockets m_remoteSockets;
    AudioController m_audio;
    Services m_services;

    QPointer<TerminalWindow> m_quick;
    std::unique_ptr<PhosphorShortcuts::IBackend> m_shortcutBackend;
    std::unique_ptr<PhosphorShortcuts::Registry> m_shortcuts;
    QString m_boundQuickShortcut;
    LaunchApi m_launch;
};

} // namespace Afterglow
