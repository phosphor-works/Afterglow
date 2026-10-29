// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "TerminalWindow.h"
#include "XkbKeyboard.h"

#include <AfterglowRender/GpuDevice.h>

#include <QGuiApplication>
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
        AfterglowRender::GpuDevice device{AfterglowRender::FontSpec{}};
        auto* window = new Afterglow::TerminalWindow(device);
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
