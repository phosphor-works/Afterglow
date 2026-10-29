// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "XkbKeyboard.h"
#include "app/Application.h"

#include <PhosphorWayland/LayerShellPluginLoader.h>

#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QGuiApplication>
#include <QQuickWindow>

#include <exception>

namespace {

/// Hands the request to an afterglow already running in this session, so
/// every window lives in one process on one GPU device. Returns true if
/// another instance took it.
bool forwardToRunningInstance(const QString& cwd, const QStringList& command, bool quick)
{
    QDBusInterface remote(QLatin1String(Afterglow::Application::ServiceName),
                          QLatin1String(Afterglow::Application::ObjectPath),
                          QLatin1String(Afterglow::Application::ServiceName));
    if (!remote.isValid()) {
        return false;
    }
    const QDBusMessage reply = quick ? remote.call(QStringLiteral("ToggleQuickTerminal"))
                                     : remote.call(QStringLiteral("NewWindow"), cwd, command);
    return reply.type() != QDBusMessage::ErrorMessage;
}

} // namespace

int main(int argc, char** argv)
{
    // The quick terminal is a layer surface; this shell integration also gives
    // ordinary windows their usual xdg-shell role. Chosen before Qt starts.
#ifdef AFTERGLOW_BUILD_PLUGIN_DIR
    if (QDir(QStringLiteral(AFTERGLOW_BUILD_PLUGIN_DIR)).exists()) {
        QCoreApplication::addLibraryPath(QStringLiteral(AFTERGLOW_BUILD_PLUGIN_DIR));
    }
#endif
    PhosphorWayland::registerLayerShellPlugin();

    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("afterglow"));
    QGuiApplication::setApplicationVersion(QStringLiteral(AFTERGLOW_VERSION));
    QGuiApplication::setDesktopFileName(QStringLiteral("io.github.fuddlesworth.Afterglow"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("A GPU accelerated terminal"));
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption cwdOption({QStringLiteral("d"), QStringLiteral("working-directory")},
                                       QStringLiteral("Start in <dir>."), QStringLiteral("dir"));
    const QCommandLineOption quickOption(QStringLiteral("quick-terminal"),
                                         QStringLiteral("Show or hide the quick terminal."));
    const QCommandLineOption execOption({QStringLiteral("e"), QStringLiteral("execute")},
                                        QStringLiteral("Run the rest of the command line instead of the shell."));
    parser.addOptions({cwdOption, quickOption, execOption});
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("With -e: the program and its arguments."),
                                 QStringLiteral("[command...]"));
    parser.process(app);

    if (QGuiApplication::platformName() != QLatin1String("wayland")) {
        qCritical("afterglow runs on Wayland only (platform is %s)", qPrintable(QGuiApplication::platformName()));
        return 1;
    }

    const QString cwd = parser.isSet(cwdOption) ? QDir(parser.value(cwdOption)).absolutePath() : QDir::currentPath();
    const QStringList command = parser.isSet(execOption) ? parser.positionalArguments() : QStringList{};
    const bool quick = parser.isSet(quickOption);

    // One process per session: if afterglow already runs, it opens the window.
    if (!QDBusConnection::sessionBus().registerService(QLatin1String(Afterglow::Application::ServiceName))) {
        if (forwardToRunningInstance(cwd, command, quick)) {
            return 0;
        }
        qWarning("afterglow: another instance owns the session name but did not answer; starting anyway");
    }

    try {
        Afterglow::XkbKeyboard::instance(); // start following the compositor's keymap
        // The chrome's Qt Quick scenes render on afterglow's Vulkan device.
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
        Afterglow::Application application;
        if (quick) {
            application.ToggleQuickTerminal();
        } else {
            application.newWindow({cwd, command, false});
        }
        return app.exec();
    } catch (const std::exception& e) {
        qCritical("afterglow: %s", e.what());
        return 1;
    }
}
