// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "XkbKeyboard.h"
#include "app/Application.h"
#include "control/Ctl.h"

#include <PhosphorWayland/LayerShellPluginLoader.h>

#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>

#include <cstring>
#include <QGuiApplication>
#include <QQuickWindow>

#include <exception>

namespace {

/// Hands the request to an afterglow already running in this session, so
/// every window lives in one process on one GPU device. Returns true if
/// another instance took it.
bool forwardToRunningInstance(const QString& cwd, const QStringList& command, const QVariantMap& options, bool quick)
{
    QDBusInterface remote(QLatin1String(Afterglow::Application::ServiceName),
                          QLatin1String(Afterglow::Application::ObjectPath),
                          QLatin1String(Afterglow::Application::ServiceName));
    if (!remote.isValid()) {
        return false;
    }
    const QDBusMessage reply = quick ? remote.call(QStringLiteral("ToggleQuickTerminal"))
                                     : remote.call(QStringLiteral("NewWindow"), cwd, command, options);
    return reply.type() != QDBusMessage::ErrorMessage;
}

} // namespace

int main(int argc, char** argv)
{
    // afterglow ctl ...: a D-Bus client for a running afterglow, no GUI.
    if (argc >= 2 && std::strcmp(argv[1], "ctl") == 0) {
        return Afterglow::runCtl(argc - 1, argv + 1);
    }
    // The quick terminal is a layer surface; this shell integration also gives
    // ordinary windows their usual xdg-shell role. Chosen before Qt starts.
    // The plugin lives in afterglow's private directory when installed, in
    // the build tree during development (which, added last, wins).
#ifdef AFTERGLOW_INSTALLED_PLUGIN_DIR
    if (QDir(QStringLiteral(AFTERGLOW_INSTALLED_PLUGIN_DIR)).exists()) {
        QCoreApplication::addLibraryPath(QStringLiteral(AFTERGLOW_INSTALLED_PLUGIN_DIR));
    }
#endif
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
    const QCommandLineOption titleOption(QStringLiteral("title"), QStringLiteral("Give the window <title>, whatever its programs set."),
                                         QStringLiteral("title"));
    const QCommandLineOption holdOption(QStringLiteral("hold"),
                                        QStringLiteral("Keep the window open after the program exits, until a key is pressed."));
    const QCommandLineOption configOption(QStringLiteral("config"),
                                          QStringLiteral("Read settings from <file> (starts a separate afterglow)."),
                                          QStringLiteral("file"));
    const QCommandLineOption overrideOption(
        {QStringLiteral("o"), QStringLiteral("override")},
        QStringLiteral("Override a setting: Group.key=value, e.g. Font.size=14 (repeatable; starts a separate afterglow)."),
        QStringLiteral("setting"));
    const QCommandLineOption classOption(QStringLiteral("class"),
                                         QStringLiteral("Wayland app id for window rules (starts a separate afterglow)."),
                                         QStringLiteral("id"));
    parser.addOptions({cwdOption, quickOption, execOption, titleOption, holdOption, configOption, overrideOption, classOption});
    // Everything after the program -e runs is its own, options included.
    parser.setOptionsAfterPositionalArgumentsMode(QCommandLineParser::ParseAsPositionalArguments);
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("With -e: the program and its arguments."),
                                 QStringLiteral("[command...]"));
    parser.process(app);

    if (QGuiApplication::platformName() != QLatin1String("wayland")) {
        qCritical("afterglow runs on Wayland only (platform is %s)", qPrintable(QGuiApplication::platformName()));
        return 1;
    }

    const bool cwdGiven = parser.isSet(cwdOption);
    const QString cwd = cwdGiven ? QDir(parser.value(cwdOption)).absolutePath() : QDir::currentPath();
    const QStringList command = parser.isSet(execOption) ? parser.positionalArguments() : QStringList{};
    const bool quick = parser.isSet(quickOption);
    const QVariantMap windowOptions{{QStringLiteral("directoryGiven"), cwdGiven},
                                    {QStringLiteral("title"), parser.value(titleOption)},
                                    {QStringLiteral("hold"), parser.isSet(holdOption)}};

    QJsonObject overrides;
    for (const QString& assignment : parser.values(overrideOption)) {
        if (!Afterglow::SettingsStore::addOverride(overrides, assignment)) {
            qCritical("afterglow: -o %s: expected Group.key=value, e.g. Font.size=14", qPrintable(assignment));
            return 1;
        }
    }
    const QString configPath = parser.isSet(configOption) ? QFileInfo(parser.value(configOption)).absoluteFilePath() : QString();
    if (parser.isSet(classOption)) {
        QGuiApplication::setDesktopFileName(parser.value(classOption));
    }
    // Settings and the app id belong to the whole process, so a launch that
    // changes them runs on its own instead of handing over.
    const bool separate = !configPath.isEmpty() || !overrides.isEmpty() || parser.isSet(classOption);

    // One process per session: if afterglow already runs, it opens the window.
    if (!separate && !QDBusConnection::sessionBus().registerService(QLatin1String(Afterglow::Application::ServiceName))) {
        if (forwardToRunningInstance(cwd, command, windowOptions, quick)) {
            return 0;
        }
        qWarning("afterglow: another instance owns the session name but did not answer; starting anyway");
    }

    try {
        Afterglow::XkbKeyboard::instance(); // start following the compositor's keymap
        // The chrome's Qt Quick scenes render on afterglow's Vulkan device.
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
        Afterglow::Application application(configPath, overrides);
        if (quick) {
            application.ToggleQuickTerminal();
        } else {
            application.newWindow({cwd, command, false, cwdGiven, parser.value(titleOption), parser.isSet(holdOption)});
        }
        return app.exec();
    } catch (const std::exception& e) {
        qCritical("afterglow: %s", e.what());
        return 1;
    }
}
