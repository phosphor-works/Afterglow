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
#include <QHash>
#include <QStringList>

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace PhosphorShortcuts {
class IBackend;
class Registry;
} // namespace PhosphorShortcuts

namespace Afterglow {

class Application;
class InstanceSocket;
struct LaunchMailbox;
struct SessionAttached;

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
    /// session (afterglow mux attach: show that mux session), sessionHost
    /// (the ssh destination whose daemon runs the session, empty for this
    /// computer) and takeover (true by default: take the session from a
    /// window that shows it). Fails when the window's program cannot start,
    /// or the session cannot show. The answer for a session comes once its
    /// window shows: the attach waits for the daemon off the GUI thread.
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
    /// Where the scripts come from (Scripts.enabled).
    enum class ScriptDirectories : std::uint8_t {
        /// The data directories (XDG_DATA_HOME, XDG_DATA_DIRS), then the one
        /// beside the config file.
        System,
        /// Only the one beside the config file: for the native window tests,
        /// which no script that the system installs may change.
        ConfigOnly,
    };

    /// @p socket carries launches and external control. @p configPath (empty
    /// for the usual one) and @p overrides (afterglow -o) set up the settings.
    /// @p scriptDirectories says where scripts come from.
    explicit Application(InstanceSocket& socket, const QString& configPath = {}, const QJsonObject& overrides = {},
                         ScriptDirectories scriptDirectories = ScriptDirectories::System);
    ~Application() override;

    TerminalWindow* newWindow(const WindowOptions& options);
    /// The end of openLaunch(): an error message, empty on success.
    using LaunchDone = std::function<void(const QString& error)>;
    /// The window of a launch, this process's own or a later one's (the
    /// options of LaunchApi::newWindow). @p done gets the outcome once. The
    /// window of a mux session waits for its daemon, over ssh for another
    /// host, off the GUI thread: @p done runs later, unless this object
    /// goes first. Every other launch ends before this returns.
    void openLaunch(const QString& workingDirectory, const QStringList& command, const QVariantMap& options, LaunchDone done);
    /// An error message, empty on success.
    QString toggleQuickTerminal();
    /// The control of the windows, as `afterglow ctl` reaches it. Only the
    /// native window tests call it: the application itself reaches Control
    /// through its own members.
    Control& control() { return m_control; }

protected:
    /// Sees the application's QEvent::Quit before Qt closes the windows.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    /// A launch whose session window waits for the daemon off the GUI thread.
    struct SessionLaunch
    {
        WindowOptions window;
        QVariantMap options; ///< openLaunch()'s
        LaunchDone done;
        QString session;
        QString host;
        /// A later launch of the same session while the attach waits. It
        /// gets the window of this attach, as a launch whose session a
        /// window shows already gets that window, raised with its own
        /// activation token.
        struct Joined
        {
            QString workingDirectory;
            QStringList command;
            QVariantMap options; ///< openLaunch()'s
            LaunchDone done;
        };
        QList<Joined> joined;
    };

    void bindShortcuts();
    void quitWhenNoWindowsRemain();
    /// The end of the attach of launch @p id, on the GUI thread.
    void sessionAttached(quint64 id, SessionAttached& attached);
    /// Shows session @p session in a new window of @p window once its attach
    /// (@p attached) answered, and brings it to the front with the
    /// activation token of @p options: an error message, empty on success.
    QString showAttached(const QString& session, SessionAttached& attached, WindowOptions window, const QVariantMap& options);
    /// Shows @p window, made with @p options, as a launch does.
    QString showLaunch(const WindowOptions& window, const QVariantMap& options);
    /// The window of this process that shows session @p session of @p host,
    /// or null. The host is compared as given.
    static TerminalWindow* sessionWindow(const QString& session, const QString& host);
    /// Brings @p window to the front as it is, its tab and its focus kept,
    /// with the launcher's activation token of @p options.
    static void raiseForLaunch(TerminalWindow* window, const QVariantMap& options);

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
    /// The launches whose session attaches wait, by their numbers.
    QHash<quint64, SessionLaunch> m_sessionLaunches;
    quint64 m_lastSessionLaunch = 0;
    /// Where those attaches answer: this object while it lives.
    std::shared_ptr<LaunchMailbox> m_mailbox;
};

} // namespace Afterglow
