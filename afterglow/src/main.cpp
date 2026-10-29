// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TerminalWindow.h"
#include "XkbKeyboard.h"
#include "config/Settings.h"
#include "shaders/ShaderLibrary.h"

#include <AfterglowRender/GpuDevice.h>

#include <QGuiApplication>
#include <QQmlEngine>
#include <QQuickWindow>
#include <QLoggingCategory>

#include <exception>

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("afterglow"));
    QGuiApplication::setApplicationVersion(QStringLiteral(AFTERGLOW_VERSION));
    QGuiApplication::setDesktopFileName(QStringLiteral("afterglow"));

    if (QGuiApplication::platformName() != QLatin1String("wayland")) {
        qCritical("afterglow runs on Wayland only (platform is %s)", qPrintable(QGuiApplication::platformName()));
        return 1;
    }

    int rc = 0;
    try {
        Afterglow::XkbKeyboard::instance(); // start following the compositor's keymap
        Afterglow::SettingsStore settings;
        AfterglowRender::GpuDevice device{settings.settings().font};
        QObject::connect(&settings, &Afterglow::SettingsStore::changed, &device,
                         [&settings, &device](const Afterglow::Settings& previous) {
                             if (settings.settings().font != previous.font) {
                                 device.setFontSpec(settings.settings().font);
                             }
                         });
        // The chrome's Qt Quick scenes render on afterglow's Vulkan device.
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
        QQmlEngine engine;
        Afterglow::ShaderLibrary shaders;
        auto* window = new Afterglow::TerminalWindow(device, settings, engine, shaders);
        window->show();
        rc = app.exec();
        // Windows hold swapchains on the device: they go first.
        qDeleteAll(Afterglow::TerminalWindow::windows());
    } catch (const std::exception& e) {
        qCritical("afterglow: %s", e.what());
        return 1;
    }
    return rc;
}
