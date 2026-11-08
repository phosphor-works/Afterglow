// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RemoteSockets.h"

#include "CommandSocket.h"
#include "../app/InstanceSocket.h"
#include "../config/Settings.h"

#include <AfterglowProto/Random.h>
#include <AfterglowProto/Socket.h>
#include <AfterglowProto/Text.h>
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

bool cleanText(const QString& text)
{
    return AfterglowProto::isCleanText(text.toStdString());
}

/// A path that can go into ssh's -R REMOTE:LOCAL: ssh splits it at ':' and
/// takes a backslash for an escape.
bool forwardable(const QString& path)
{
    return path.startsWith(QLatin1Char('/')) && !path.contains(QLatin1Char(':')) && !path.contains(QLatin1Char('\\'))
        && cleanText(path);
}

} // namespace

QString remoteCommandsOff(const Settings& s)
{
    const char* off = !s.sshRemoteCommands ? "Terminal.sshRemoteCommands"
        : !s.control.enabled               ? "Control.enabled"
        : !s.scripts                       ? "Scripts.enabled"
                                           : nullptr;
    return off ? QStringLiteral("remote commands are off (%1)").arg(QLatin1String(off)) : QString();
}

struct RemoteSockets::Session
{
    QString token;
    quint32 pane = 0;
    quint64 order = 0;
    int pidfd = -1;
    int lock = -1; ///< InstanceSocket::lock of the local socket
    std::unique_ptr<QSocketNotifier> watch;
    QString socket; ///< the local socket, empty once closed
    QString path; ///< on the remote host
    QStringList ssh;
    QString destination;
    Remote::Answer answer;
    /// The pane gives the answer: an error at once, a path once its forward came.
    bool answering = false;
    // The target goes before the router that it unregisters from.
    std::unique_ptr<PhosphorIpc::IpcRouter> router;
    std::unique_ptr<CommandSocket> commands;

    /// Closes the local socket. The runs from it end, as when their client
    /// goes. Their replies fail with @p reason, but IpcRouter::stop() aborts
    /// the connections and drops what waits to be written: the clients see
    /// only the close until phosphor-ipc can flush on stop. The router and the target are destroyed from
    /// the event loop of @p context, or now without one: this can run
    /// inside a call of that router (a remote run's handler that turns the
    /// setting off or closes the pane), and the router must not be destroyed
    /// inside its own call (IpcRouter's contract).
    void closeSocket(const QString& reason, QObject* context)
    {
        if (commands) commands->cancelAll(reason);
        if (router) router->stop();
        if (!socket.isEmpty()) QFile::remove(socket);
        if (lock >= 0) InstanceSocket::unlock(socket, lock);
        lock = -1;
        socket.clear();
        if (!router && !commands) return;
        auto retired = std::make_shared<std::pair<std::unique_ptr<PhosphorIpc::IpcRouter>, std::unique_ptr<CommandSocket>>>(
            std::move(router), std::move(commands));
        // A pair destroys its second first: the target, then its router.
        if (context) QMetaObject::invokeMethod(context, [retired] { retired->second.reset(); }, Qt::QueuedConnection);
    }

    ~Session()
    {
        closeSocket(QStringLiteral("afterglow is quitting"), nullptr);
        watch.reset();
        if (pidfd >= 0) ::close(pidfd);
    }
};

RemoteSockets::RemoteSockets(Control& control, ScriptHost& scripts, SettingsStore& settings, PaneAnswer answer,
                             std::shared_ptr<CommandBudget> budget, QObject* parent)
    : QObject(parent)
    , m_control(control)
    , m_scripts(scripts)
    , m_settings(settings)
    , m_answer(std::move(answer))
    , m_budget(budget ? std::move(budget) : std::make_shared<CommandBudget>())
{
    connect(&settings, &SettingsStore::changed, this, &RemoteSockets::applySettings);
}

RemoteSockets::~RemoteSockets()
{
    // Not inside a call of a session's router: the sessions go now.
    for (const std::shared_ptr<Session>& session : std::exchange(m_sessions, {})) {
        if (!session->socket.isEmpty()) cancel(*session);
        session->closeSocket(QStringLiteral("afterglow is quitting"), nullptr);
    }
}

bool RemoteSockets::enabled() const
{
    return remoteCommandsOff(m_settings.settings()).isEmpty();
}

void RemoteSockets::applySettings()
{
    const QString off = remoteCommandsOff(m_settings.settings());
    if (off.isEmpty()) return;
    const QStringList all = m_sessions.keys();
    for (const QString& token : all) close(token, off + QStringLiteral(" in the running afterglow"));
}

QString RemoteSockets::open(const Request& request, Opened& out)
{
    out = {};
    if (const QString off = remoteCommandsOff(m_settings.settings()); !off.isEmpty()) return off;
    if (request.pid <= 0 || request.pid > INT_MAX) return QStringLiteral("a process ID is a whole number from 1");
    // ev.remote.host: a name, as `afterglow ssh` shows it.
    if (request.host.isEmpty() || request.host.size() > MaxHostLength || !cleanText(request.host))
        return QStringLiteral("the host is 1 to %1 characters of text with no control characters").arg(MaxHostLength);
    // The path goes into the pane's reply and into ssh's -R.
    if (!forwardable(request.directory))
        return QStringLiteral("the remote directory must be an absolute path of text with no ':', '\\' and control characters");
    if (request.ssh.size() > MaxSshOptions) return QStringLiteral("ssh takes at most %1 options here").arg(MaxSshOptions);
    if (request.destination.isEmpty() || request.destination.startsWith(QLatin1Char('-')))
        return QStringLiteral("the ssh destination must not be empty or start with '-'");
    if (m_sessions.size() >= MaxSessions) return QStringLiteral("afterglow has %1 remote sessions already").arg(MaxSessions);
    const auto ofPane = std::count_if(m_sessions.cbegin(), m_sessions.cend(), [&](const auto& s) { return s->pane == request.pane; });
    if (ofPane >= MaxSessionsPerPane)
        return QStringLiteral("pane %1 has %2 remote sessions already").arg(request.pane).arg(MaxSessionsPerPane);
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
        session->answering = true;
    } else if (std::size_t(session->path.toUtf8().size()) > Remote::MaxPathBytes) {
        session->answer.error = Remote::Error::Length;
        session->answering = true;
    } else {
        const QString runtime = InstanceSocket::runtimeDirectory();
        if (runtime.isEmpty()) return QStringLiteral("XDG_RUNTIME_DIR is not set, so there is no private place for the socket");
        const QString local = runtime + QStringLiteral("/afterglow-remote-") + session->token + QStringLiteral(".sock");
        if (std::size_t(QFile::encodeName(local).size()) > AfterglowProto::MaxSocketPathBytes)
            return QStringLiteral("the socket path %1 is too long").arg(local);
        if (!forwardable(local)) return QStringLiteral("XDG_RUNTIME_DIR %1 has a ':', a '\\' or a control character").arg(runtime);
        // Held while the socket listens: another launch's removeStaleSockets passes it by.
        session->lock = InstanceSocket::lock(local);
        if (session->lock < 0) return QStringLiteral("cannot lock %1.lock").arg(local);
        session->socket = local;
        session->router = std::make_unique<PhosphorIpc::IpcRouter>();
        // Only the user can connect: the router binds the socket with mode 0600.
        if (!session->router->start(local)) return QStringLiteral("cannot listen on %1").arg(local);
        session->commands = std::make_unique<CommandSocket>(m_control, m_scripts, m_settings, *session->router,
                                                            CommandSocket::RemoteSession{request.pane, request.host}, m_budget);
        session->answer.path = session->path.toStdString();
    }

    session->watch = std::make_unique<QSocketNotifier>(session->pidfd, QSocketNotifier::Read);
    // Queued: the notifier cannot go inside its own signal.
    connect(session->watch.get(), &QSocketNotifier::activated, this,
            [this, token = session->token] { close(token, QStringLiteral("the ssh session of the remote command ended")); },
            Qt::QueuedConnection);
    m_sessions.insert(session->token, session);
    if (!refresh(request.pane)) {
        // Not inside a call of the session's router: it goes now.
        m_sessions.remove(session->token);
        return QStringLiteral("no pane %1").arg(request.pane);
    }

    out.token = session->token;
    out.socket = session->socket;
    out.path = session->path;
    if (session->answer.path.empty()) out.error = QString::fromLatin1(Remote::errorName(session->answer.error));
    return {};
}

QString RemoteSockets::forwarded(const QString& token)
{
    const std::shared_ptr<Session> session = m_sessions.value(token);
    if (!session) return QStringLiteral("no remote socket %1: its session has ended").arg(token);
    if (session->socket.isEmpty()) return QStringLiteral("remote socket %1 is closed").arg(token);
    if (!session->answering) {
        session->answering = true;
        refresh(session->pane);
    }
    return {};
}

QString RemoteSockets::failed(const QString& token, const QString& reason)
{
    const std::shared_ptr<Session> session = m_sessions.value(token);
    if (!session) return QStringLiteral("no remote socket %1").arg(token);
    if (reason != QLatin1String("forward")) return QStringLiteral("the reason of a failed forward is forward");
    if (session->socket.isEmpty()) return QStringLiteral("remote socket %1 has no forward").arg(token);
    // Nothing to cancel: the forward was never added, or the client
    // cancelled it after its remoteSocketForwarded got no answer in time,
    // which can come before this.
    session->closeSocket(QStringLiteral("the ssh server refused the forward of the socket"), this);
    session->answer = {{}, Remote::Error::Forward};
    session->answering = true;
    refresh(session->pane);
    return {};
}

void RemoteSockets::paneClosed(quint32 pane)
{
    QStringList ended;
    for (const auto& session : std::as_const(m_sessions)) {
        if (session->pane == pane) ended << session->token;
    }
    for (const QString& token : std::as_const(ended)) close(token, QStringLiteral("the pane of the ssh session closed"));
}

QString RemoteSockets::socket(const QString& token) const
{
    const std::shared_ptr<Session> session = m_sessions.value(token);
    return session ? session->socket : QString();
}

void RemoteSockets::close(const QString& token, const QString& reason)
{
    const std::shared_ptr<Session> session = m_sessions.take(token);
    if (!session) return;
    if (!session->socket.isEmpty()) cancel(*session);
    session->closeSocket(reason, this);
    refresh(session->pane);
}

bool RemoteSockets::refresh(quint32 pane)
{
    const Session* newest = nullptr;
    for (const auto& session : std::as_const(m_sessions)) {
        if (session->pane == pane && session->answering && (!newest || session->order > newest->order)) newest = session.get();
    }
    return m_answer(pane, newest ? newest->answer : Remote::Answer{});
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
