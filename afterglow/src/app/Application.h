// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../TerminalWindow.h"
#include "../control/Control.h"
#include "../control/ControlBus.h"
#include "../config/Settings.h"
#include "../shaders/ShaderLibrary.h"
#include "Services.h"

#include <AfterglowRender/GpuDevice.h>

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

/// The afterglow process: one per session. Further launches hand their
/// request over D-Bus, so every window shares this process's GPU device.
class Application : public QObject
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.fuddlesworth.Afterglow")

public:
    static constexpr const char* ServiceName = "io.github.fuddlesworth.Afterglow";
    static constexpr const char* ObjectPath = "/Afterglow";

    Application();
    ~Application() override;

    TerminalWindow* newWindow(const WindowOptions& options);

public Q_SLOTS:
    Q_SCRIPTABLE void NewWindow(const QString& workingDirectory, const QStringList& command);
    Q_SCRIPTABLE void ToggleQuickTerminal();

private:
    void bindShortcuts();
    void quitWhenNoWindowsRemain();

    SettingsStore m_settings;
    AfterglowRender::GpuDevice m_device;
    QQmlEngine m_engine;
    ShaderLibrary m_shaders;
    Services m_services;

    QPointer<TerminalWindow> m_quick;
    std::unique_ptr<PhosphorShortcuts::IBackend> m_shortcutBackend;
    std::unique_ptr<PhosphorShortcuts::Registry> m_shortcuts;
    Control m_control;
    ControlBus m_controlBus;
    QString m_boundQuickShortcut;
};

} // namespace Afterglow
