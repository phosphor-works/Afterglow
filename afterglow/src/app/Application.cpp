// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Application.h"

#include "../chrome/IconProvider.h"
#include "../scripting/ScriptControl.h"

#include <PhosphorShortcuts/Factory.h>
#include <PhosphorShortcuts/IBackend.h>
#include <PhosphorShortcuts/Registry.h>

#include <QDBusConnection>
#include <QGuiApplication>
#include <QKeySequence>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(lcApp, "afterglow.app", QtInfoMsg)

namespace Afterglow {

namespace {
const QString QuickShortcutId = QStringLiteral("toggle-quick-terminal");
}

Application::Application(const QString& configPath, const QJsonObject& overrides)
    : m_settings(configPath.isEmpty() ? SettingsStore::defaultPath() : configPath, overrides)
    , m_device(m_settings.settings().font)
    , m_control(m_settings, [this](const QString& cwd, const QStringList& command) {
        return newWindow({cwd, command, false, !cwd.isEmpty(), {}, false});
    })
    , m_controlBus(m_control, m_settings)
    , m_scripts(scriptCallbacks(m_control))
    , m_services{m_device, m_settings, m_engine, m_shaders, m_scripts, m_audio}
{
    connect(&m_audio, &AudioController::spectrumChanged, &m_device, [this](const QVector<float>& spectrum) {
        m_device.setAudioSpectrum(std::span<const float>(spectrum.constData(), std::size_t(spectrum.size())));
    });
    m_audio.configure(m_settings.settings().audio);
    m_engine.addImageProvider(QStringLiteral("icon"), new IconProvider); // the engine owns it
    connectScriptEvents(m_scripts, m_control);
    m_control.setScriptActionRunner([this](const QString& action, quint32 pane) { return m_scripts.runAction(action, pane); });
    m_scripts.setEnabled(m_settings.settings().scripts);

    connect(&m_settings, &SettingsStore::changed, this, [this](const Settings& previous) {
        if (m_settings.settings().font != previous.font) {
            m_device.setFontSpec(m_settings.settings().font);
        }
        if (m_settings.settings().quickTerminal.shortcut != previous.quickTerminal.shortcut) {
            bindShortcuts();
        }
        m_scripts.setEnabled(m_settings.settings().scripts);
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
    if (!options.quick) {
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
