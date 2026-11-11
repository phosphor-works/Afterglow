// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Application.h"
#include "InstanceSocket.h"

#include "NotificationCenter.h"
#include "ScriptSettings.h"
#include "WindowStart.h"
#include "../chrome/ChromeLayer.h"
#include "../desktop/DragDrop.h"
#include "../mux/MuxClient.h"

#include <QIcon>
#include <QStandardPaths>
#include "../scripting/ScriptControl.h"

#include <PhosphorShortcuts/Factory.h>
#include <PhosphorShortcuts/IBackend.h>
#include <PhosphorShortcuts/Registry.h>

#include <QFileInfo>
#include <QGuiApplication>
#include <QKeySequence>
#include <QLoggingCategory>
#include <QQuickStyle>

#include <functional>

Q_LOGGING_CATEGORY(lcApp, "afterglow.app", QtInfoMsg)

namespace Afterglow {

namespace {
const QString QuickShortcutId = QStringLiteral("toggle-quick-terminal");
} // namespace

Application::Application(InstanceSocket& socket, const QString& configPath, const QJsonObject& overrides)
    : m_settings(configPath.isEmpty() ? SettingsStore::defaultPath() : configPath, overrides)
    , m_device(m_settings.settings().font)
    , m_chromeComponent(&m_engine)
    // Packs and presets beside the config file, as its scripts and themes are.
    , m_shaders(QFileInfo(m_settings.path()).absolutePath())
    , m_control(m_settings, [this](const QString& cwd, const QStringList& command, bool empty) {
        return newWindow({cwd, command, false, !cwd.isEmpty(), {}, false, empty});
    })
    , m_socket(socket)
    , m_controlSocket(m_control, m_settings, socket.router())
    , m_scripts(scriptCallbacks(m_control))
    , m_commandSocket(m_control, m_scripts, m_settings, socket.router())
    , m_remoteSockets(m_control, m_scripts, m_settings,
                      [this](quint32 pane, const AfterglowProto::Remote::Answer& answer,
                             const std::optional<AfterglowProto::Remote::Answer>& overlay) {
                          return m_control.setRemoteSocket(pane, answer, overlay);
                      },
                      // One budget for every command socket: the limits hold for the process.
                      m_commandSocket.budget())
    , m_services{m_device, m_settings, m_chromeComponent, m_shaders, m_scripts, m_audio}
    , m_launch(*this)
{
    // Some platform-theme versions leave Qt Quick at its default Fusion style.
    // Prefer the desktop bridge on Plasma, preserving explicit style choices
    // and falling back normally when the optional module is not installed.
    const QString desktop = qEnvironmentVariable("XDG_CURRENT_DESKTOP").toLower();
    if (qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")
        && qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_CONF")
        && QQuickStyle::name() == QLatin1String("Fusion")
        && (desktop.contains(QLatin1String("kde")) || desktop.contains(QLatin1String("plasma")))) {
        for (const QString& path : m_engine.importPathList()) {
            if (QFileInfo::exists(path + QStringLiteral("/org/kde/desktop/qmldir"))) {
                QQuickStyle::setStyle(QStringLiteral("org.kde.desktop"));
                break;
            }
        }
    }
    // The chrome type, its imports and their plugins load on the QML loader
    // thread from now on, with the style chosen, for every window to come.
    m_chromeComponent.loadUrl(ChromeLayer::sceneUrl(), QQmlComponent::Asynchronous);

    m_controlSocket.setRemoteSockets(&m_remoteSockets);
    connect(&m_control, &Control::paneClosed, &m_remoteSockets, &RemoteSockets::paneClosed);

    connect(&m_audio, &AudioController::spectrumChanged, &m_device, [this](const QVector<float>& spectrum) {
        m_device.setAudioSpectrum(std::span<const float>(spectrum.constData(), std::size_t(spectrum.size())));
    });
    m_audio.configure(m_settings.settings().audio);
    NotificationCenter::instance().setRateLimit(m_settings.settings().notificationRateLimit,
                                                qint64(m_settings.settings().notificationRateWindow) * 1000);
    DragTrees::instance().setLifetime(std::chrono::minutes(m_settings.settings().dragTreeLifetime));
    QStringList iconPaths = QIcon::themeSearchPaths();
    for (const auto& path : QStandardPaths::locateAll(QStandardPaths::GenericDataLocation,
            QStringLiteral("icons"), QStandardPaths::LocateDirectory))
        if (!iconPaths.contains(path)) iconPaths.append(path);
    QIcon::setThemeSearchPaths(iconPaths);
    if (QIcon::themeName().isEmpty()) QIcon::setThemeName(QStringLiteral("breeze"));
    if (QIcon::fallbackThemeName().isEmpty()) QIcon::setFallbackThemeName(QStringLiteral("hicolor"));
    connectScriptEvents(m_scripts, m_control);
    // Script packages bring their shader packs with them.
    connect(&m_scripts, &ScriptHost::packageShadersChanged, &m_shaders, &ShaderLibrary::setPackageShaders);
    m_control.setScriptCommandRunner([this](const QString& command, quint32 pane, const QString& source) {
        // Off, the scripts have no commands: "no command" would hide why.
        if (!m_scripts.isEnabled()) return QStringLiteral("scripts are off (Scripts.enabled)");
        return m_scripts.runCommand(command, pane, source);
    });
    m_control.setScriptDiagnostics([this] { return m_scripts.diagnostics(); });
    auto scriptPaths = ScriptHost::defaultSearchPaths();
    scriptPaths.last() = QFileInfo(m_settings.path()).absolutePath() + QStringLiteral("/scripts");
    m_scripts.setSearchPaths(scriptPaths);
    m_scripts.setStateDirectory(QFileInfo(m_settings.path()).absolutePath() + QStringLiteral("/state/scripts"));
    // After the command socket, whose slot for the same settings runs first.
    followScriptSettings(m_scripts, m_settings);

    connect(&m_settings, &SettingsStore::changed, this, [this](const Settings& previous) {
        if (m_settings.settings().font != previous.font) {
            m_device.setFontSpec(m_settings.settings().font);
        }
        if (m_settings.settings().quickTerminal.shortcut != previous.quickTerminal.shortcut) {
            bindShortcuts();
        }
        QTimer::singleShot(0, &m_scripts, [this] { m_scripts.dispatch(QStringLiteral("on_config_changed"), 0); });
        if (const AudioSettings& audio = m_settings.settings().audio; audio != previous.audio) {
            m_audio.configure(audio);
        }
        NotificationCenter::instance().setRateLimit(m_settings.settings().notificationRateLimit,
                                                    qint64(m_settings.settings().notificationRateWindow) * 1000);
        DragTrees::instance().setLifetime(std::chrono::minutes(m_settings.settings().dragTreeLifetime));
    });

    // Hidden windows (a tucked-away quick terminal) still count as open.
    QGuiApplication::setQuitOnLastWindowClosed(false);

    bindShortcuts();
    // Last: from here launches that waited on the socket get their windows.
    if (!m_socket.router().registerTarget(QLatin1String(LaunchApi::Target), &m_launch)) {
        qCWarning(lcApp) << "another object holds the launch target";
    }
}

Application::~Application()
{
    // What quitting ends is no news: the script command runs without a
    // client, which the scripts end as they go, show no notice.
    m_control.endNotices();
    // No new windows while the others close.
    m_socket.router().unregisterTarget(QLatin1String(LaunchApi::Target), &m_launch);
    m_controlSocket.setRemoteSockets(nullptr);
    // A quit (logout, a signal) is not the user closing a session's window:
    // every session keeps running, whatever Mux.onClose says.
    for (TerminalWindow* window : TerminalWindow::windows()) {
        for (Pane* pane : window->findChildren<Pane*>()) pane->setKillSessionOnClose(false);
    }
    // Windows render on the device and the chrome uses the QML engine.
    qDeleteAll(TerminalWindow::windows());
    // The scripts go before Control: its callbacks into them must not outlive them.
    m_control.setScriptCommandRunner({});
    m_control.setScriptDiagnostics({});
}

void Application::bindShortcuts()
{
    if (!m_shortcutBackend) {
        m_shortcutBackend = PhosphorShortcuts::createBackend(PhosphorShortcuts::BackendHint::Auto, this);
        if (!m_shortcutBackend) {
            qCWarning(lcApp) << "no global shortcut backend; use `afterglow --quick-terminal` instead";
            return;
        }
        m_shortcuts = std::make_unique<PhosphorShortcuts::Registry>(m_shortcutBackend.get(), this);
    }
    const QString seq = m_settings.settings().quickTerminal.shortcut;
    if (seq == m_boundQuickShortcut) {
        return;
    }
    if (seq.isEmpty()) {
        m_shortcuts->unbind(QuickShortcutId);
    } else {
        m_shortcuts->bind(QuickShortcutId, QKeySequence::fromString(seq, QKeySequence::PortableText),
                          QStringLiteral("Toggle the quick terminal"), [this] {
                              if (const QString error = toggleQuickTerminal(); !error.isEmpty()) qCWarning(lcApp).noquote() << error;
                          });
    }
    m_shortcuts->flush();
    m_boundQuickShortcut = seq;
}

TerminalWindow* Application::newWindow(const WindowOptions& options)
{
    auto* window = new TerminalWindow(m_services, options);
    if (!options.empty && !window->focusedPane()) { delete window; return nullptr; }
    m_control.attachWindow(window);
    connect(window, &TerminalWindow::newWindowRequested, this,
            [this](const QString& cwd) { newWindow({cwd, {}, false, false, {}, false}); });
    connect(window, &QObject::destroyed, this, &Application::quitWhenNoWindowsRemain, Qt::QueuedConnection);
    if (!options.quick) {
        // Before the first show, also for the empty windows that ctl,
        // scripts, projects and moved panes fill and show later.
        const WindowSettings& ws = m_settings.settings().window;
        window->setFlag(Qt::FramelessWindowHint, !ws.decorations);
        if (!options.empty) showForStartMode(*window, ws);
    }
    return window;
}

namespace {

/// Runs @p activate with the launcher's activation token of @p options in the
/// environment, where Qt reads it when a window asks for the focus. An unused
/// token must not reach the shells of later panes.
void withActivationToken(const QVariantMap& options, const std::function<void()>& activate)
{
    const QByteArray token = options.value(QStringLiteral("activationToken")).toString().toUtf8();
    if (!token.isEmpty()) qputenv("XDG_ACTIVATION_TOKEN", token);
    activate();
    if (!token.isEmpty()) qunsetenv("XDG_ACTIVATION_TOKEN");
}

} // namespace

QString Application::openLaunch(const QString& workingDirectory, const QStringList& command, const QVariantMap& options)
{
    WindowOptions window{workingDirectory, command, false, options.value(QStringLiteral("directoryGiven")).toBool(),
                         options.value(QStringLiteral("title")).toString(), options.value(QStringLiteral("hold")).toBool()};
    // afterglow mux attach: the window shows a session of the mux daemon.
    const QString session = options.value(QStringLiteral("session")).toString();
    if (!session.isEmpty()) {
        // A window of this process shows it already: the attach would take
        // the session from it, while this thread, the only reader of its
        // stream, waits for the daemon. That pane comes to the front, with
        // the launcher's token that lets it take the focus. Panes of tabs
        // only: a closed pane waits for its delete as a child too.
        for (TerminalWindow* shown : TerminalWindow::windows()) {
            for (Pane* pane : shown->panes()) {
                if (pane->showsSession(session)) {
                    withActivationToken(options, [&] { shown->revealPane(pane); });
                    return {};
                }
            }
        }
        QString error;
        window.session = MuxClient::attach(session, options.value(QStringLiteral("takeover"), true).toBool(), &error,
                                           AfterglowProto::muxSockets(), m_settings.settings().mux.eventQueue,
                                           m_settings.settings().terminal.maxScrollback);
        if (!window.session) return error;
    }
    TerminalWindow* w = newWindow(window);
    if (!w && window.session) return QStringLiteral("cannot show session '%1'").arg(session);
    if (!w) return QStringLiteral("cannot start %1").arg(command.isEmpty() ? QStringLiteral("the shell") : command.first());
    withActivationToken(options, [w] { w->requestActivate(); });
    return {};
}

QString Application::toggleQuickTerminal()
{
    if (!m_quick) {
        m_quick = newWindow({{}, {}, true, false, {}, false});
        if (!m_quick) return QStringLiteral("cannot start the shell of the quick terminal");
    }
    m_quick->toggleQuick();
    return {};
}

void Application::quitWhenNoWindowsRemain()
{
    if (TerminalWindow::windows().isEmpty()) {
        QGuiApplication::quit();
    }
}

LaunchApi::LaunchApi(Application& application)
    : m_application(application)
{
}

void LaunchApi::newWindow(const QString& workingDirectory, const QStringList& command, const QVariantMap& options)
{
    if (const QString error = m_application.openLaunch(workingDirectory, command, options); !error.isEmpty()) {
        sendErrorReply(error);
    }
}

void LaunchApi::toggleQuickTerminal()
{
    if (const QString error = m_application.toggleQuickTerminal(); !error.isEmpty()) {
        sendErrorReply(error);
    }
}

} // namespace Afterglow
