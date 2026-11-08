// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RemoteSockets.h"

#include "CommandSocket.h"
#include "../config/Settings.h"

#include <AfterglowProto/Random.h>
#include <AfterglowProto/Socket.h>
#include <AfterglowProto/Version.h>

#include <PhosphorIpc/IpcRouter.h>

#include <QFile>
#include <QLoggingCategory>
#include <QProcess>
#include <QSocketNotifier>

#include <algorithm>
#include <cerrno>
#include <climits>

#include <sys/syscall.h>
#include <unistd.h>

Q_DECLARE_LOGGING_CATEGORY(lcCommands)

namespace Afterglow {

namespace Remote = AfterglowProto::Remote;

namespace {

/// The options of `ssh -O cancel` that a client can give, at most.
constexpr qsizetype MaxSshOptions = 256;

QString runtimeDirectory()
{
    const QString directory = qEnvironmentVariable("XDG_RUNTIME_DIR");
    // A '/' and not QDir::isAbsolutePath, which takes a Qt resource path too.
    return directory.startsWith(QLatin1Char('/')) ? directory : QString();
}

} // namespace

struct RemoteSockets::Session
{
    QString token;
    quint32 pane = 0;
    quint64 order = 0;
    int pidfd = -1;
    std::unique_ptr<QSocketNotifier> watch;
    QString socket; ///< the local socket, empty once closed
    QString path; ///< on the remote host
    QStringList ssh;
    QString destination;
    Remote::Answer answer;
    std::unique_ptr<PhosphorIpc::IpcRouter> router;
    std::unique_ptr<CommandSocket> commands;

    /// Closes the local socket. The runs from it end, as when their client goes.
    void closeSocket()
    {
        if (commands) commands->cancelAll(QStringLiteral("the ssh session of the remote command ended"));
        if (router) router->stop();
        commands.reset();
        router.reset();
        if (!socket.isEmpty()) QFile::remove(socket);
        socket.clear();
    }

    ~Session()
    {
        closeSocket();
        watch.reset();
        if (pidfd >= 0) ::close(pidfd);
    }
};

RemoteSockets::RemoteSockets(Control& control, ScriptHost& scripts, SettingsStore& settings, PaneAnswer answer, QObject* parent)
    : QObject(parent)
    , m_control(control)
    , m_scripts(scripts)
    , m_settings(settings)
    , m_answer(std::move(answer))
{
    connect(&settings, &SettingsStore::changed, this, &RemoteSockets::applySettings);
}

RemoteSockets::~RemoteSockets()
{
    const QStringList all = m_sessions.keys();
    for (const QString& token : all) close(token);
}

bool RemoteSockets::enabled() const
{
    const Settings& s = m_settings.settings();
    return s.control.enabled && s.scripts && s.sshIntegration && s.sshRemoteCommands;
}

void RemoteSockets::applySettings()
{
    if (enabled()) return;
    const QStringList all = m_sessions.keys();
    for (const QString& token : all) close(token);
}

QString RemoteSockets::open(const Request& request, Opened& out)
{
    out = {};
    if (!enabled()) return QStringLiteral("remote commands are off (Terminal.sshRemoteCommands)");
    if (request.pid <= 0 || request.pid > INT_MAX) return QStringLiteral("a process ID is a whole number from 1");
    // The path goes into the pane's reply and into ssh's -R, which ends the
    // remote path at the first ':'.
    const bool control = std::any_of(request.directory.cbegin(), request.directory.cend(),
                                     [](QChar c) { return c.unicode() < 0x20 || c.unicode() == 0x7f; });
    if (!request.directory.startsWith(QLatin1Char('/')) || request.directory.contains(QLatin1Char(':')) || control)
        return QStringLiteral("the remote directory must be an absolute path with no ':' and no control characters");
    if (request.ssh.size() > MaxSshOptions) return QStringLiteral("ssh takes at most %1 options here").arg(MaxSshOptions);
    if (request.destination.isEmpty() || request.destination.startsWith(QLatin1Char('-')))
        return QStringLiteral("the ssh destination must not be empty or start with '-'");
    const auto token = AfterglowProto::randomHex(Remote::TokenBytes);
    if (!token) return QStringLiteral("the kernel gave no random bytes for the token");

    auto session = std::make_shared<Session>();
    session->token = QString::fromStdString(*token);
    session->pane = request.pane;
    session->order = ++m_opened;
    session->ssh = request.ssh;
    session->destination = request.destination;
    session->pidfd = int(::syscall(SYS_pidfd_open, pid_t(request.pid), 0));
    if (session->pidfd < 0) return QStringLiteral("no process %1: %2").arg(request.pid).arg(qt_error_string(errno));
    session->path = QString::fromStdString(Remote::socketPath(request.directory.toStdString(), *token));

    if (request.protocol != AfterglowProto::ProtocolVersion) {
        session->answer.error = Remote::Error::Version;
    } else if (std::size_t(session->path.toUtf8().size()) > Remote::MaxPathBytes) {
        session->answer.error = Remote::Error::Length;
    } else {
        const QString runtime = runtimeDirectory();
        if (runtime.isEmpty()) return QStringLiteral("XDG_RUNTIME_DIR is not set, so there is no private place for the socket");
        const QString local = runtime + QStringLiteral("/afterglow-remote-") + session->token + QStringLiteral(".sock");
        if (std::size_t(QFile::encodeName(local).size()) > AfterglowProto::MaxSocketPathBytes)
            return QStringLiteral("the socket path %1 is too long").arg(local);
        session->router = std::make_unique<PhosphorIpc::IpcRouter>();
        // Only the user can connect: the router binds the socket with mode 0600.
        if (!session->router->start(local)) return QStringLiteral("cannot listen on %1").arg(local);
        session->socket = local;
        session->commands = std::make_unique<CommandSocket>(m_control, m_scripts, m_settings, *session->router,
                                                            CommandSocket::RemoteSession{request.pane, request.host});
        session->answer.path = session->path.toStdString();
    }
    if (!m_answer(request.pane, session->answer)) return QStringLiteral("no pane %1").arg(request.pane);

    session->watch = std::make_unique<QSocketNotifier>(session->pidfd, QSocketNotifier::Read);
    // Queued: the notifier cannot go inside its own signal.
    connect(session->watch.get(), &QSocketNotifier::activated, this,
            [this, token = session->token] { close(token); }, Qt::QueuedConnection);
    m_sessions.insert(session->token, session);
    refresh(request.pane);

    out.token = session->token;
    out.socket = session->socket;
    out.path = session->path;
    if (session->answer.path.empty()) out.error = QString::fromLatin1(Remote::errorName(session->answer.error));
    return {};
}

QString RemoteSockets::failed(const QString& token, const QString& reason)
{
    const std::shared_ptr<Session> session = m_sessions.value(token);
    if (!session) return QStringLiteral("no remote socket %1").arg(token);
    if (reason != QLatin1String("forward")) return QStringLiteral("the reason of a failed forward is forward");
    // Nothing to cancel: the forward was never added.
    session->closeSocket();
    session->answer = {{}, Remote::Error::Forward};
    refresh(session->pane);
    return {};
}

void RemoteSockets::paneClosed(quint32 pane)
{
    QStringList ended;
    for (const auto& session : std::as_const(m_sessions)) {
        if (session->pane == pane) ended << session->token;
    }
    for (const QString& token : std::as_const(ended)) close(token);
}

QString RemoteSockets::socket(const QString& token) const
{
    const std::shared_ptr<Session> session = m_sessions.value(token);
    return session ? session->socket : QString();
}

void RemoteSockets::close(const QString& token)
{
    const std::shared_ptr<Session> session = m_sessions.take(token);
    if (!session) return;
    if (!session->socket.isEmpty()) cancel(*session);
    session->closeSocket();
    refresh(session->pane);
}

void RemoteSockets::refresh(quint32 pane)
{
    const Session* newest = nullptr;
    for (const auto& session : std::as_const(m_sessions)) {
        if (session->pane == pane && (!newest || session->order > newest->order)) newest = session.get();
    }
    m_answer(pane, newest ? newest->answer : Remote::Answer{});
}

void RemoteSockets::cancel(const Session& session) const
{
    // Without waiting: when the shared connection has gone, there is
    // nothing to cancel, and the file on the remote host stays until the
    // next probe removes it.
    QProcess ssh;
    ssh.setProgram(m_ssh);
    ssh.setArguments(session.ssh
                     + QStringList{QStringLiteral("-O"), QStringLiteral("cancel"), QStringLiteral("-R"),
                                   session.path + QLatin1Char(':') + session.socket, QStringLiteral("--"),
                                   session.destination});
    ssh.setStandardInputFile(QProcess::nullDevice());
    ssh.setStandardOutputFile(QProcess::nullDevice());
    ssh.setStandardErrorFile(QProcess::nullDevice());
    if (!ssh.startDetached()) qCWarning(lcCommands) << "cannot run" << m_ssh << "to cancel a remote forward";
}

} // namespace Afterglow
