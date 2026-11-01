// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "XkbKeyboard.h"
#include "config/Settings.h"
#include <QJsonDocument>
#include <cstdio>
#include "app/Application.h"
#include "app/Startup.h"
#include "control/Ctl.h"
#include "shell/Ssh.h"

#include <PhosphorDBus/Client.h>

#include <QCommandLineParser>
#include <QApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#include <QDir>
#include <QFileInfo>
#include <QJsonObject>

#include <cstring>
#include <QGuiApplication>
#include <QIcon>
#include <QQuickWindow>

#include <exception>
#include <memory>
#include <sys/stat.h>

namespace {

enum class ForwardResult { Forwarded, DifferentExecutable, Unavailable, Failed };

/// Share the GPU device with an instance of this executable. A development
/// build or an executable replaced by an upgrade must run its own code.
/// @p error receives the running instance's reason when it answers Failed.
ForwardResult forwardToRunningInstance(const QString& cwd, const QStringList& command, const QVariantMap& options, bool quick,
                                       QString* error)
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
    const PhosphorDBus::Client remote(bus, owner.value(), QLatin1String(Afterglow::Application::ObjectPath));
    const QString iface = QLatin1String(Afterglow::Application::ServiceName);
    const QDBusMessage reply = quick ? remote.syncCall(iface, QStringLiteral("ToggleQuickTerminal"))
                                     : remote.syncCall(iface, QStringLiteral("NewWindow"), {cwd, command, options});
    if (reply.type() != QDBusMessage::ErrorMessage) return ForwardResult::Forwarded;
    // The instance got the request and could not open the window (a bad -e
    // program): starting another process would fail the same way.
    if (reply.errorName() == QLatin1String("org.freedesktop.DBus.Error.Failed")) {
        *error = reply.errorMessage();
        return ForwardResult::Failed;
    }
    return ForwardResult::Unavailable;
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
    // afterglow ssh ...: ssh with the remote side prepared, no GUI.
    if (argc >= 2 && std::strcmp(argv[1], "ssh") == 0) {
        QByteArrayList args;
        for (int i = 2; i < argc; ++i) args << QByteArray(argv[i]);
        return Afterglow::Ssh::run(args);
    }
    Afterglow::prepareApplication();

    // Desktop Qt Quick styles can delegate their painting to the widget style.
    // QApplication initializes that style and follows the platform's palette.
    QApplication app(argc, argv);
    QGuiApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral(AFTERGLOW_APP_ID)));

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

    // Handled before Qt starts when it is the only argument.
    if (parser.isSet(QStringLiteral("config-reference"))) {
        qCritical("afterglow: --config-reference takes no other arguments");
        return 1;
    }
    if (!parser.isSet(execOption) && !parser.positionalArguments().isEmpty()) {
        qCritical("afterglow: use -e to run a program: afterglow -e %s",
                  qPrintable(parser.positionalArguments().join(QLatin1Char(' '))));
        return 1;
    }
    // Any QtWayland platform (wayland, wayland-egl) has the Wayland interface.
    if (!app.nativeInterface<QNativeInterface::QWaylandApplication>()) {
        qCritical("afterglow runs on Wayland only (platform is %s)", qPrintable(QGuiApplication::platformName()));
        return 1;
    }

    const bool cwdGiven = parser.isSet(cwdOption);
    const QString cwd = cwdGiven ? QDir(parser.value(cwdOption)).absolutePath() : QDir::currentPath();
    const QStringList command = parser.isSet(execOption) ? parser.positionalArguments() : QStringList{};
    const bool quick = parser.isSet(quickOption);
    // The launcher's token lets the running instance's new window take focus.
    const QVariantMap windowOptions{{QStringLiteral("directoryGiven"), cwdGiven},
                                    {QStringLiteral("title"), parser.value(titleOption)},
                                    {QStringLiteral("hold"), parser.isSet(holdOption)},
                                    {QStringLiteral("activationToken"), qEnvironmentVariable("XDG_ACTIVATION_TOKEN")}};

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
    const bool owner = !separate && QDBusConnection::sessionBus().registerService(QLatin1String(Afterglow::Application::ServiceName));
    // Launches that arrive while this process starts (the GPU device, QML
    // and scripts take a while) wait in the queue, not for an unknown object.
    std::unique_ptr<Afterglow::LaunchQueue> launches;
    if (owner) launches = std::make_unique<Afterglow::LaunchQueue>();
    if (!separate && !owner) {
        QString error;
        switch (forwardToRunningInstance(cwd, command, windowOptions, quick, &error)) {
        case ForwardResult::Forwarded:
            return 0;
        case ForwardResult::Failed:
            qCritical("afterglow: %s", qPrintable(error));
            return 1;
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
        if (launches) launches->replay(application);
        return app.exec();
    } catch (const std::exception& e) {
        qCritical("afterglow: %s", e.what());
        return 1;
    }
}
