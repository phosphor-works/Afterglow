// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "XkbKeyboard.h"
#include "config/Settings.h"
#include <QJsonDocument>
#include <cstdio>
#include "app/Application.h"
#include "app/InstanceSocket.h"
#include "app/QuitSignals.h"
#include "app/Startup.h"
#include "control/Ctl.h"
#include "control/Mux.h"
#include "control/IpcClient.h"
#include "shell/Aglow.h"
#include "shell/Ssh.h"

#include <PhosphorIpc/IpcProtocol.h>

#include <QCommandLineParser>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QThread>

#include <cstring>
#include <QGuiApplication>
#include <QIcon>
#include <QQuickWindow>

#include <cerrno>
#include <csignal>
#include <exception>
#include <optional>
#include <sys/stat.h>

Q_DECLARE_LOGGING_CATEGORY(lcApp)

namespace {

/// Ends a launch with @p message. Not a log: when stderr is not a terminal,
/// Qt sends its messages to the journal, and the caller (a script, a
/// launcher) would not see why the command failed.
int fail(const QString& message)
{
    std::fprintf(stderr, "afterglow: %s\n", qPrintable(message));
    return 1;
}

/// Unavailable: no answer (the main process is starting its socket or
/// quitting). Unverified: its executable cannot be read. TooLong: the command
/// does not fit in a request.
enum class ForwardResult { Forwarded, DifferentExecutable, Unverified, Unavailable, TooLong, Failed };

/// Share the GPU device with the main process when it runs this executable. A
/// development build or an executable replaced by an upgrade must run its own
/// code. @p error receives the running process's reason when it answers Failed.
ForwardResult forwardToRunningInstance(const QString& path, const QString& cwd, const QStringList& command,
                                       const QVariantMap& options, bool quick, QString* error)
{
    Afterglow::IpcClient client;
    if (!client.connectTo(path, 2000, nullptr)) {
        return ForwardResult::Unavailable;
    }
    // The process at the other end of this connection is the one that will
    // answer: a restart cannot put another between the check and the request.
    const pid_t pid = client.peerPid();
    if (pid <= 0) {
        return ForwardResult::Unverified;
    }
    const QByteArray executable = QStringLiteral("/proc/%1/exe").arg(pid).toLocal8Bit();
    struct stat self{}, running{};
    if (::stat("/proc/self/exe", &self) != 0) return ForwardResult::Unverified;
    if (::stat(executable.constData(), &running) != 0) {
        // Gone since it answered the connection: it was quitting.
        return errno == ENOENT || errno == ESRCH ? ForwardResult::Unavailable : ForwardResult::Unverified;
    }
    // Compare the files themselves: a rebuild can replace the binary while
    // an old process still runs at the same pathname.
    if (self.st_dev != running.st_dev || self.st_ino != running.st_ino) {
        return ForwardResult::DifferentExecutable;
    }
    // The main process registers the launch target once it is ready (the GPU
    // device, QML and scripts take a while). A request that it reads before
    // gets NO_SUCH_TARGET: ask again. A timeout would start a second process
    // next to a slow one, so wait as long as any startup can take.
    constexpr qint64 ForwardTimeoutMs = 120'000;
    QElapsedTimer clock;
    clock.start();
    const QString target = QLatin1String(Afterglow::LaunchApi::Target);
    while (true) {
        const qint64 left = ForwardTimeoutMs - clock.elapsed();
        if (left <= 0) break;
        Afterglow::IpcClient::Error failure;
        const auto result = quick
            ? client.call(target, QStringLiteral("toggleQuickTerminal"), {}, int(left), &failure)
            : client.call(target, QStringLiteral("newWindow"),
                          {cwd, QJsonArray::fromStringList(command), QJsonObject::fromVariantMap(options)}, int(left), &failure);
        if (result) return ForwardResult::Forwarded;
        // The process got the request and could not open the window (a bad -e
        // program): starting another process would fail the same way.
        if (failure.code == QLatin1String(PhosphorIpc::ErrorCode::CallFailed)) {
            *error = failure.message;
            return ForwardResult::Failed;
        }
        if (failure.tooLong) return ForwardResult::TooLong;
        if (failure.code.isEmpty()) break; // no answer
        // Another error does not change when asked again (an argument that
        // the running process refuses, INVALID_ARG).
        if (failure.code != QLatin1String(PhosphorIpc::ErrorCode::NoSuchTarget)) {
            *error = QStringLiteral("the running instance refused the launch: %1").arg(failure.message);
            return ForwardResult::Failed;
        }
        QThread::msleep(100);
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
    // afterglow ctl ...: a control socket client for a running afterglow, no GUI.
    if (argc >= 2 && std::strcmp(argv[1], "ctl") == 0) {
        return Afterglow::runCtl(argc - 1, argv + 1);
    }
    // afterglow mux ...: the sessions of the mux daemon, no GUI.
    if (argc >= 2 && std::strcmp(argv[1], "mux") == 0) {
        return Afterglow::runMux(argc - 1, argv + 1);
    }
    // afterglow COMMAND ... for each of Aglow::Commands: the aglow next to this executable, no GUI.
    if (argc >= 2 && Afterglow::Aglow::handles(argv[1])) {
        return Afterglow::Aglow::run(argc, argv);
    }
    // afterglow ssh ...: ssh with the remote side prepared, no GUI.
    if (argc >= 2 && std::strcmp(argv[1], "ssh") == 0) {
        QByteArrayList args;
        for (int i = 2; i < argc; ++i) args << QByteArray(argv[i]);
        return Afterglow::Ssh::run(args);
    }
    Afterglow::prepareApplication();

    QCommandLineParser parser;
    // The commands that run before Qt starts take no window and no options of their own here.
    QString description = QStringLiteral(
        "A GPU accelerated terminal\n\n"
        "Commands that run before a window starts:\n"
        "  afterglow ctl ...       Control a running Afterglow.\n"
        "  afterglow mux ...       Keep terminal sessions running, and show them in windows.\n"
        "  afterglow ssh ...       Run ssh with shell integration on the remote computer.\n");
    if (Afterglow::Aglow::built()) {
        for (const Afterglow::Aglow::Command& command : Afterglow::Aglow::Commands) {
            description += QStringLiteral("  ") + QStringLiteral("afterglow %1 ...").arg(QLatin1String(command.name)).leftJustified(24)
                + QLatin1String(command.summary) + QStringLiteral(".\n");
        }
    }
    parser.setApplicationDescription(description);
    const QCommandLineOption helpOption = parser.addHelpOption();
    const QCommandLineOption versionOption = parser.addVersionOption();
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
                                          QStringLiteral("Read settings from <file> (starts a separate Afterglow)."),
                                          QStringLiteral("file"));
    const QCommandLineOption overrideOption(
        {QStringLiteral("o"), QStringLiteral("override")},
        QStringLiteral("Override a setting: Group.key=value, e.g. Font.size=14 (repeatable). Starts a separate Afterglow."),
        QStringLiteral("setting"));
    const QCommandLineOption classOption(QStringLiteral("class"),
                                         QStringLiteral("Wayland app id for window rules (starts a separate Afterglow)."),
                                         QStringLiteral("id"));
    // afterglow mux attach runs afterglow with these: the window of a session
    // opens in the main process of the display, as the window of any launch.
    QCommandLineOption muxAttachOption(QStringLiteral("mux-attach"), QStringLiteral("Show the mux session <name>."),
                                       QStringLiteral("name"));
    muxAttachOption.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption muxNoTakeoverOption(QStringLiteral("mux-no-takeover"),
                                           QStringLiteral("Refuse when another window shows the session."));
    muxNoTakeoverOption.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption muxHostOption(QStringLiteral("mux-host"), QStringLiteral("The session runs on this ssh destination."),
                                     QStringLiteral("destination"));
    muxHostOption.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOptions({cwdOption, quickOption, execOption, titleOption, holdOption, configOption, overrideOption, classOption,
                       muxAttachOption, muxNoTakeoverOption, muxHostOption});
    // Everything after the program -e runs is its own, options included.
    parser.setOptionsAfterPositionalArgumentsMode(QCommandLineParser::ParseAsPositionalArguments);
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("With -e: the program and its arguments."),
                                 QStringLiteral("[command...]"));

    // The help and the version need no desktop, without which QApplication
    // aborts: a first parse finds them before it exists. Qt's own options
    // (-platform) are unknown here until QApplication takes them out, so a
    // command line that does not parse yet waits for QApplication. So does
    // --help-all, whose options of Qt's GUI only QApplication adds.
    QStringList arguments;
    for (int i = 0; i < argc; ++i) arguments << QString::fromLocal8Bit(argv[i]);
    if (parser.parse(arguments) && (parser.isSet(helpOption) || parser.isSet(versionOption))) {
        QCoreApplication core(argc, argv);
        parser.process(core); // prints them and exits
        return 0;
    }

    // Desktop Qt Quick styles can delegate their painting to the widget style.
    // QApplication initializes that style and follows the platform's palette.
    // It ends before a quit signal is raised again, as at a return.
    std::optional<QApplication> app;
    app.emplace(argc, argv);
    QGuiApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral(AFTERGLOW_APP_ID)));
    parser.process(*app);

    // Handled before Qt starts when it is the only argument.
    if (parser.isSet(QStringLiteral("config-reference"))) {
        return fail(QStringLiteral("--config-reference takes no other arguments"));
    }
    if (!parser.isSet(execOption) && !parser.positionalArguments().isEmpty()) {
        return fail(QStringLiteral("use -e to run a program: afterglow -e %1").arg(parser.positionalArguments().join(QLatin1Char(' '))));
    }
    if (parser.isSet(execOption) && parser.positionalArguments().isEmpty()) {
        return fail(QStringLiteral("-e needs a program"));
    }
    if (parser.isSet(quickOption)
        && (parser.isSet(execOption) || parser.isSet(cwdOption) || parser.isSet(titleOption) || parser.isSet(holdOption)
            || parser.isSet(muxAttachOption))) {
        return fail(QStringLiteral("--quick-terminal takes no -e, -d, --title, --hold or --mux-attach"));
    }
    // The program and its directory are the session's.
    if (parser.isSet(muxAttachOption) && (parser.isSet(execOption) || parser.isSet(cwdOption) || parser.isSet(holdOption))) {
        return fail(QStringLiteral("--mux-attach takes no -e, -d or --hold"));
    }
    if (parser.isSet(muxNoTakeoverOption) && !parser.isSet(muxAttachOption)) {
        return fail(QStringLiteral("--mux-no-takeover needs --mux-attach"));
    }
    if (parser.isSet(muxHostOption) && (!parser.isSet(muxAttachOption) || parser.value(muxHostOption).isEmpty())) {
        return fail(QStringLiteral("--mux-host needs --mux-attach and a destination"));
    }
    if (parser.isSet(muxAttachOption) && parser.value(muxAttachOption).isEmpty()) {
        return fail(QStringLiteral("--mux-attach needs the name of a session"));
    }
    if (parser.isSet(configOption) && parser.value(configOption).isEmpty()) return fail(QStringLiteral("--config needs a file"));
    if (parser.isSet(classOption) && parser.value(classOption).isEmpty()) return fail(QStringLiteral("--class needs an app id"));
    // Any QtWayland platform (wayland, wayland-egl) has the Wayland interface.
    if (!app->nativeInterface<QNativeInterface::QWaylandApplication>()) {
        return fail(QStringLiteral("Afterglow runs on Wayland only (platform is %1)").arg(QGuiApplication::platformName()));
    }

    const bool cwdGiven = parser.isSet(cwdOption);
    const QString cwd = cwdGiven ? QDir(parser.value(cwdOption)).absolutePath() : QDir::currentPath();
    const QStringList command = parser.isSet(execOption) ? parser.positionalArguments() : QStringList{};
    const bool quick = parser.isSet(quickOption);
    // The launcher's token lets the new window take focus, in this process or
    // in the running one. The window uses it once: the shells must not get it.
    const QVariantMap windowOptions{{QStringLiteral("directoryGiven"), cwdGiven},
                                    {QStringLiteral("title"), parser.value(titleOption)},
                                    {QStringLiteral("hold"), parser.isSet(holdOption)},
                                    {QStringLiteral("activationToken"), qEnvironmentVariable("XDG_ACTIVATION_TOKEN")},
                                    {QStringLiteral("session"), parser.value(muxAttachOption)},
                                    {QStringLiteral("sessionHost"), parser.value(muxHostOption)},
                                    {QStringLiteral("takeover"), !parser.isSet(muxNoTakeoverOption)}};
    qunsetenv("XDG_ACTIVATION_TOKEN");
    // An Afterglow started from a pane of another one, or of a mux session,
    // must not hand that pane's identity to its scripts' programs. Its own
    // panes get their own, and are in no session.
    qunsetenv("AFTERGLOW_SOCKET");
    qunsetenv("AFTERGLOW_PANE_ID");
    qunsetenv("AFTERGLOW_SESSION");

    QJsonObject overrides;
    for (const QString& assignment : parser.values(overrideOption)) {
        if (!Afterglow::SettingsStore::addOverride(overrides, assignment)) {
            return fail(QStringLiteral("-o %1: expected Group.key=value, e.g. Font.size=14").arg(assignment));
        }
    }
    const QString configPath = parser.isSet(configOption) ? QFileInfo(parser.value(configOption)).absoluteFilePath() : QString();
    if (parser.isSet(classOption)) {
        QGuiApplication::setDesktopFileName(parser.value(classOption));
    }
    // Settings and the app id belong to the whole process, so a launch that
    // changes them runs on its own instead of handing over.
    const bool separate = !configPath.isEmpty() || !overrides.isEmpty() || parser.isSet(classOption);

    int code = 0;
    int quitSignal = 0;
    {
        // The first process to listen on the display's socket is the main
        // one: of two launches at the same moment, one gets the path and the
        // other hands its window over. Launches that arrive while the main
        // process starts wait on the socket until it is ready.
        Afterglow::InstanceSocket::removeStaleSockets();
        Afterglow::InstanceSocket socket;
        const QString shared = Afterglow::InstanceSocket::sharedPath();
        if (shared.isEmpty()) {
            qCWarning(lcApp) << "XDG_RUNTIME_DIR is not set or not an absolute path: no socket for launches and control";
        } else if (!separate) {
            using Listen = Afterglow::InstanceSocket::Listen;
            // A main process that quits stops answering, then frees the path.
            constexpr qint64 QuitWaitMs = 5000;
            QElapsedTimer waited;
            waited.start();
            Listen listened = socket.listen(shared);
            while (listened == Listen::Held) {
                QString error;
                const ForwardResult result = forwardToRunningInstance(shared, cwd, command, windowOptions, quick, &error);
                if (result == ForwardResult::Forwarded) return 0;
                if (result == ForwardResult::Failed) return fail(error);
                if (result == ForwardResult::DifferentExecutable) {
                    qCInfo(lcApp) << "the running instance uses a different executable; starting this build separately";
                    break;
                }
                if (result == ForwardResult::Unverified) {
                    qCWarning(lcApp) << "could not verify the running instance; starting separately";
                    break;
                }
                if (result == ForwardResult::TooLong) {
                    qCInfo(lcApp) << "the command is too long to hand over; starting separately";
                    break;
                }
                // A main process that quits closes its connections just before
                // it frees the path: a late attempt still waits once for that.
                const bool late = waited.elapsed() >= QuitWaitMs;
                QThread::msleep(100);
                listened = socket.listen(shared);
                if (late && listened == Listen::Held) {
                    qCWarning(lcApp) << "the running instance does not answer; starting separately";
                    break;
                }
            }
        }
        // A process's own path is held only for the moment that another
        // launch's sweep checks its lock.
        for (int attempt = 0; !shared.isEmpty() && !socket.listening() && attempt < 20; ++attempt) {
            if (socket.listen(Afterglow::InstanceSocket::ownPath()) != Afterglow::InstanceSocket::Listen::Held) break;
            QThread::msleep(10);
        }
        if (!shared.isEmpty() && !socket.listening()) {
            qCWarning(lcApp) << "cannot listen on a socket, so afterglow ctl, aglow run, afterglow run and new launches cannot "
                                "reach this process";
        }

        // From here a termination signal closes the windows and the socket,
        // as the last window does. Before, it ends the process: a launch that
        // waits for a running instance stays easy to stop.
        Afterglow::QuitSignals quitSignals;
        try {
            Afterglow::XkbKeyboard::instance(); // start following the compositor's keymap
            // The chrome's Qt Quick scenes render on afterglow's Vulkan device.
            QQuickWindow::setGraphicsApi(QSGRendererInterface::Vulkan);
            Afterglow::Application application(socket, configPath, overrides);
            // The quick terminal is a layer surface, which takes no activation token.
            const QString error = quick ? application.toggleQuickTerminal() : application.openLaunch(cwd, command, windowOptions);
            if (!error.isEmpty()) return fail(error);
            // A signal during the start ends the loop at its first pass:
            // QuitSignals hears it there.
            code = app->exec();
            quitSignal = quitSignals.signal();
        } catch (const std::exception& e) {
            return fail(QString::fromLocal8Bit(e.what()));
        }
    }
    // After the clean shutdown, end as the signal does: the parent (a shell,
    // a session manager) sees that a signal ended Afterglow. The raise skips
    // the destructors that a return runs, so QApplication ends first: its
    // children hold files to remove (the trees of remote drags). What is left
    // (the keyboard singleton) holds nothing to save.
    if (quitSignal != 0) {
        app.reset();
        std::signal(quitSignal, SIG_DFL);
        std::raise(quitSignal);
    }
    return code;
}
