// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Application.h"

#include "../chrome/IconProvider.h"
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
}

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
    m_engine.addImageProvider(QStringLiteral("icon"), new IconProvider); // the engine owns it
    connectScriptEvents(m_scripts, m_control);
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
        if (m_settings.settings().audio != previous.audio) {
            m_audio.configure(m_settings.settings().audio);
        }
    });

    // Hidden windows (a tucked-away quick terminal) still count as open.
    QGuiApplication::setQuitOnLastWindowClosed(false);

    QDBusConnection::sessionBus().registerObject(QLatin1String(ObjectPath), this,
                                                 QDBusConnection::ExportScriptableSlots);
    bindShortcuts();
}

Application::~Application()
{
    // Windows render on the device and the chrome uses the QML engine.
    qDeleteAll(TerminalWindow::windows());
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
    m_control.attachWindow(window);
    connect(window, &TerminalWindow::newWindowRequested, this,
            [this](const QString& cwd) { newWindow({cwd, {}, false, false, {}, false}); });
    connect(window, &QObject::destroyed, this, &Application::quitWhenNoWindowsRemain, Qt::QueuedConnection);
    if (!options.quick && !options.empty) {
        const WindowSettings& ws = m_settings.settings().window;
        window->setFlag(Qt::FramelessWindowHint, !ws.decorations);
        switch (ws.startMode) {
        case WindowSettings::StartMode::Maximized:
            window->showMaximized();
            break;
        case WindowSettings::StartMode::Fullscreen:
            window->showFullScreen();
            break;
        case WindowSettings::StartMode::Normal:
            window->show();
            break;
        }
    }
    return window;
}

void Application::NewWindow(const QString& workingDirectory, const QStringList& command, const QVariantMap& options)
{
    TerminalWindow* w = newWindow({workingDirectory, command, false, options.value(QStringLiteral("directoryGiven")).toBool(),
                                   options.value(QStringLiteral("title")).toString(),
                                   options.value(QStringLiteral("hold")).toBool()});
    w->requestActivate();
}

void Application::ToggleQuickTerminal()
{
    if (!m_quick) {
        m_quick = newWindow({{}, {}, true, false, {}, false});
    }
    m_quick->toggleQuick();
}

void Application::quitWhenNoWindowsRemain()
{
    if (TerminalWindow::windows().isEmpty()) {
        QGuiApplication::quit();
    }
}

} // namespace Afterglow
