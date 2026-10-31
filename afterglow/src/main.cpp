// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "XkbKeyboard.h"
#include "config/Settings.h"
#include <QJsonDocument>
#include <cstdio>
#include "app/Application.h"
#include "control/Ctl.h"

#include <PhosphorWayland/LayerShellPluginLoader.h>

#include <QCommandLineParser>
#include <QApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusReply>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>

#include <cstring>
#include <QGuiApplication>
#include <QIcon>
#include <QQuickWindow>

#include <exception>
#include <sys/stat.h>

namespace {

enum class ForwardResult { Forwarded, DifferentExecutable, Unavailable };

/// Share the GPU device with an instance of this executable. A development
/// build or an executable replaced by an upgrade must run its own code.
ForwardResult forwardToRunningInstance(const QString& cwd, const QStringList& command, const QVariantMap& options, bool quick)
{
    const QDBusConnection bus = QDBusConnection::sessionBus();
    QDBusConnectionInterface* interface = bus.interface();
    if (!interface) {
        return ForwardResult::Unavailable;
    }
    // Pin the unique owner so a service restart cannot change the process
    // between checking its executable and sending the window request.
    const QDBusReply<QString> owner = interface->serviceOwner(QLatin1String(Afterglow::Application::ServiceName));
    if (!owner.isValid()) {
        return ForwardResult::Unavailable;
    }
    const QDBusReply<uint> pid = interface->servicePid(owner.value());
    if (!pid.isValid()) {
        return ForwardResult::Unavailable;
    }
    const QByteArray executable = QStringLiteral("/proc/%1/exe").arg(pid.value()).toLocal8Bit();
    struct stat self{}, running{};
    if (::stat("/proc/self/exe", &self) != 0 || ::stat(executable.constData(), &running) != 0) {
        return ForwardResult::Unavailable;
    }
    // Compare the files themselves: a rebuild can replace the binary while
    // an old process still runs at the same pathname.
    if (self.st_dev != running.st_dev || self.st_ino != running.st_ino) {
        return ForwardResult::DifferentExecutable;
    }
    QDBusInterface remote(owner.value(),
                          QLatin1String(Afterglow::Application::ObjectPath),
                          QLatin1String(Afterglow::Application::ServiceName), bus);
    if (!remote.isValid()) {
        return ForwardResult::Unavailable;
    }
    const QDBusMessage reply = quick ? remote.call(QStringLiteral("ToggleQuickTerminal"))
                                     : remote.call(QStringLiteral("NewWindow"), cwd, command, options);
    return reply.type() != QDBusMessage::ErrorMessage ? ForwardResult::Forwarded : ForwardResult::Unavailable;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--config-reference") == 0) {
        QCoreApplication app(argc, argv);
        const QByteArray json = QJsonDocument(Afterglow::SettingsStore::reference()).toJson();
        std::fwrite(json.constData(), 1, std::size_t(json.size()), stdout);
        return 0;
    }
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

    // Desktop Qt Quick styles can delegate their painting to the widget style.
    // QApplication initializes that style and follows the platform's palette.
    QApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("afterglow"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("Afterglow"));
    QGuiApplication::setApplicationVersion(QStringLiteral(AFTERGLOW_VERSION));
    QGuiApplication::setDesktopFileName(QStringLiteral("com.phosphorworks.Afterglow"));
    QGuiApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("com.phosphorworks.Afterglow")));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("A GPU accelerated terminal"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addOption({QStringLiteral("config-reference"), QStringLiteral("Print the configuration reference as JSON. Use alone. No desktop required.")});
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
        QStringLiteral("Override a setting: Group.key=value, e.g. Font.size=14 (repeatable). Starts a separate afterglow."),
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

    // Reuse the session process only when it runs this executable.
    if (!separate && !QDBusConnection::sessionBus().registerService(QLatin1String(Afterglow::Application::ServiceName))) {
        switch (forwardToRunningInstance(cwd, command, windowOptions, quick)) {
        case ForwardResult::Forwarded:
            return 0;
        case ForwardResult::DifferentExecutable:
            qInfo("afterglow: the running instance uses a different executable; starting this build separately");
            break;
        case ForwardResult::Unavailable:
            qWarning("afterglow: could not verify or contact the running instance; starting separately");
            break;
        }
    }

    try {
        Afterglow::XkbKeyboard::instance(); // start following the compositor's keymap
        // The chrome's Qt Quick scenes render on afterglow's Vulkan device.
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
        Afterglow::Application application(configPath, overrides);
        if (quick) {
            application.ToggleQuickTerminal();
        } else {
            if (!application.newWindow({cwd, command, false, cwdGiven, parser.value(titleOption), parser.isSet(holdOption)})) return 1;
        }
        return app.exec();
    } catch (const std::exception& e) {
        qCritical("afterglow: %s", e.what());
        return 1;
    }
}
