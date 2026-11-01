// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Application.h"

#include "NotificationCenter.h"
#include "WindowStart.h"

#include <QIcon>
#include <QStandardPaths>
#include "../scripting/ScriptControl.h"

#include <PhosphorShortcuts/Factory.h>
#include <PhosphorShortcuts/IBackend.h>
#include <PhosphorShortcuts/Registry.h>

#include <QDBusConnection>
#include <QFileInfo>
#include <QGuiApplication>
#include <QKeySequence>
#include <QLoggingCategory>
#include <QQuickStyle>

Q_LOGGING_CATEGORY(lcApp, "afterglow.app", QtInfoMsg)

namespace Afterglow {

namespace {
const QString QuickShortcutId = QStringLiteral("toggle-quick-terminal");

} // namespace

Application::Application(const QString& configPath, const QJsonObject& overrides)
    : m_settings(configPath.isEmpty() ? SettingsStore::defaultPath() : configPath, overrides)
    , m_device(m_settings.settings().font)
    , m_control(m_settings, [this](const QString& cwd, const QStringList& command, bool empty) {
        return newWindow({cwd, command, false, !cwd.isEmpty(), {}, false, empty});
    })
    , m_controlBus(m_control, m_settings)
    , m_scripts(scriptCallbacks(m_control))
    , m_services{m_device, m_settings, m_engine, m_shaders, m_scripts, m_audio}
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

    connect(&m_audio, &AudioController::spectrumChanged, &m_device, [this](const QVector<float>& spectrum) {
        m_device.setAudioSpectrum(std::span<const float>(spectrum.constData(), std::size_t(spectrum.size())));
    });
    m_audio.configure(m_settings.settings().audio);
    NotificationCenter::instance().setRateLimit(m_settings.settings().notificationRateLimit);
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
    m_control.setScriptActionRunner([this](const QString& action, quint32 pane) { return m_scripts.runAction(action, pane); });
    m_control.setScriptDiagnostics([this] { return m_scripts.diagnostics(); });
    auto scriptPaths = ScriptHost::defaultSearchPaths();
    scriptPaths.last() = QFileInfo(m_settings.path()).absolutePath() + QStringLiteral("/scripts");
    m_scripts.setSearchPaths(scriptPaths);
    m_scripts.setStateDirectory(QFileInfo(m_settings.path()).absolutePath() + QStringLiteral("/state/scripts"));
    m_scripts.setEnabled(m_settings.settings().scripts);

    connect(&m_settings, &SettingsStore::changed, this, [this](const Settings& previous) {
        if (m_settings.settings().font != previous.font) {
            m_device.setFontSpec(m_settings.settings().font);
        }
        if (m_settings.settings().quickTerminal.shortcut != previous.quickTerminal.shortcut) {
            bindShortcuts();
        }
        m_scripts.setEnabled(m_settings.settings().scripts);
        QTimer::singleShot(0, &m_scripts, [this] { m_scripts.dispatch(QStringLiteral("on_config_changed"), 0); });
        if (const AudioSettings& audio = m_settings.settings().audio; audio != previous.audio) {
            m_audio.configure(audio);
        }
        NotificationCenter::instance().setRateLimit(m_settings.settings().notificationRateLimit);
    });

    // Hidden windows (a tucked-away quick terminal) still count as open.
    QGuiApplication::setQuitOnLastWindowClosed(false);

    // A LaunchQueue from main() may hold the path: it hands it over when it replays.
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.objectRegisteredAt(QLatin1String(ObjectPath))) {
        bus.registerObject(QLatin1String(ObjectPath), this, QDBusConnection::ExportScriptableSlots);
    }
    bindShortcuts();
}

Application::~Application()
{
    // Windows render on the device and the chrome uses the QML engine.
    qDeleteAll(TerminalWindow::windows());
    // The scripts go before Control: its callbacks into them must not outlive them.
    m_control.setScriptActionRunner({});
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
                          QStringLiteral("Toggle the quick terminal"), [this] { ToggleQuickTerminal(); });
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

QString Application::openForwarded(const QString& workingDirectory, const QStringList& command, const QVariantMap& options)
{
    TerminalWindow* w = newWindow({workingDirectory, command, false, options.value(QStringLiteral("directoryGiven")).toBool(),
                                   options.value(QStringLiteral("title")).toString(),
                                   options.value(QStringLiteral("hold")).toBool()});
    if (!w) return QStringLiteral("cannot start %1").arg(command.isEmpty() ? QStringLiteral("the shell") : command.first());
    // Qt reads the token from the environment when the window asks for focus.
    const QByteArray token = options.value(QStringLiteral("activationToken")).toString().toUtf8();
    if (!token.isEmpty()) qputenv("XDG_ACTIVATION_TOKEN", token);
    w->requestActivate();
    // An unused token must not reach the shells of later panes.
    if (!token.isEmpty()) qunsetenv("XDG_ACTIVATION_TOKEN");
    return {};
}

void Application::NewWindow(const QString& workingDirectory, const QStringList& command, const QVariantMap& options)
{
    if (const QString error = openForwarded(workingDirectory, command, options); !error.isEmpty() && calledFromDBus()) {
        sendErrorReply(QDBusError::Failed, error);
    }
}

void Application::ToggleQuickTerminal()
{
    if (!m_quick) {
        m_quick = newWindow({{}, {}, true, false, {}, false});
    }
    if (m_quick) m_quick->toggleQuick();
}

void Application::quitWhenNoWindowsRemain()
{
    if (TerminalWindow::windows().isEmpty()) {
        QGuiApplication::quit();
    }
}

} // namespace Afterglow

namespace Afterglow {

LaunchQueue::LaunchQueue(QObject* parent)
    : QObject(parent)
{
    m_registered = QDBusConnection::sessionBus().registerObject(QLatin1String(Application::ObjectPath), this,
                                                                QDBusConnection::ExportScriptableSlots);
}

void LaunchQueue::NewWindow(const QString& workingDirectory, const QStringList& command, const QVariantMap& options)
{
    if (m_replayed) {
        const QString error = m_application ? m_application->openForwarded(workingDirectory, command, options)
                                            : QStringLiteral("afterglow is quitting");
        if (!error.isEmpty()) sendErrorReply(QDBusError::Failed, error);
        return;
    }
    setDelayedReply(true);
    m_calls.append({message(), false, workingDirectory, command, options});
}

void LaunchQueue::ToggleQuickTerminal()
{
    if (m_replayed) {
        if (m_application) m_application->ToggleQuickTerminal();
        else sendErrorReply(QDBusError::Failed, QStringLiteral("afterglow is quitting"));
        return;
    }
    setDelayedReply(true);
    m_calls.append({message(), true, {}, {}, {}});
}

void LaunchQueue::replay(Application& application)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (m_registered) {
        bus.unregisterObject(QLatin1String(Application::ObjectPath));
        m_registered = false;
        bus.registerObject(QLatin1String(Application::ObjectPath), &application, QDBusConnection::ExportScriptableSlots);
    }
    m_replayed = true;
    m_application = &application;
    for (const Call& call : std::exchange(m_calls, {})) {
        QString error;
        if (call.quick) application.ToggleQuickTerminal();
        else error = application.openForwarded(call.workingDirectory, call.command, call.options);
        bus.send(error.isEmpty() ? call.message.createReply() : call.message.createErrorReply(QDBusError::Failed, error));
    }
}

} // namespace Afterglow
