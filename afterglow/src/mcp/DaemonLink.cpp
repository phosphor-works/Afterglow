// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mcp/DaemonLink.h"

#include <AfterglowProto/Ipc.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QSocketNotifier>
#include <QTimer>

#include <cerrno>
#include <cstring>
#include <utility>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace Afterglow {

namespace {

constexpr qsizetype MaxRequestBytes = qsizetype(AfterglowProto::IpcConnection::MaxRequestBytes);
/// The longest line that the daemon sends (its output limit).
constexpr qsizetype MaxFrameBytes = qsizetype(AfterglowProto::IpcConnection::MaxFrameBytes);
/// The most that one wake of the read notifier reads: a daemon that writes
/// without a pause leaves the event loop its timers and the client's
/// requests. The notifier wakes again for the rest.
constexpr int MaxReadsAtOnce = 16;

QByteArray callLine(qint64 id, const QString& target, const QString& fn, const QJsonArray& args)
{
    return QJsonDocument(QJsonObject{{QLatin1String("type"), QLatin1String("call")},
                                     {QLatin1String("id"), id},
                                     {QLatin1String("target"), target},
                                     {QLatin1String("fn"), fn},
                                     {QLatin1String("args"), args}})
               .toJson(QJsonDocument::Compact)
        + '\n';
}

/// The start time of process @p pid, when it runs: nothing for no process
/// and for one that ended and waits for its parent (a zombie).
std::optional<quint64> startOf(qint64 pid)
{
    const QByteArray path = "/proc/" + QByteArray::number(pid) + "/stat";
    const int fd = ::open(path.constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return std::nullopt;
    char buffer[4096];
    qsizetype size = 0;
    for (;;) {
        const ssize_t n = ::read(fd, buffer + size, sizeof buffer - 1 - std::size_t(size));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        size += n;
        if (std::size_t(size) == sizeof buffer - 1)
            break;
    }
    ::close(fd);
    // The name in parentheses can hold spaces and parentheses: the fields
    // after it start at the last ')'. They are the state (field 3) up to the
    // start time (field 22).
    const QByteArray line(buffer, size);
    const qsizetype end = line.lastIndexOf(')');
    if (end < 0)
        return std::nullopt;
    const QList<QByteArray> fields = line.mid(end + 1).simplified().split(' ');
    if (fields.size() < 20 || fields[0] == "Z" || fields[0] == "X")
        return std::nullopt;
    bool ok = false;
    const quint64 started = fields[19].toULongLong(&ok);
    if (!ok)
        return std::nullopt;
    return started;
}

} // namespace

std::optional<DaemonLink::Peer> DaemonLink::peerOf(int fd)
{
    struct ucred credentials = {};
    socklen_t length = sizeof credentials;
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0 || credentials.pid <= 0)
        return std::nullopt;
    const std::optional<quint64> started = startOf(credentials.pid);
    if (!started)
        return std::nullopt;
    return Peer{credentials.pid, *started};
}

bool DaemonLink::runs(const Peer& peer)
{
    return startOf(peer.pid) == peer.started;
}

DaemonLink::DaemonLink(int fd, QObject* parent)
    : QObject(parent)
    , m_fd(fd)
{
    // The writes wait in m_out, never in the kernel's send(): a socket that
    // would block there would stop the whole server.
    const int flags = ::fcntl(m_fd, F_GETFL);
    if (flags < 0 || ::fcntl(m_fd, F_SETFL, flags | O_NONBLOCK) != 0 || ::fcntl(m_fd, F_SETFD, FD_CLOEXEC) != 0) {
        const QString reason = QStringLiteral("cannot set up the connection to the mux daemon: %1")
                                   .arg(QString::fromLocal8Bit(std::strerror(errno)));
        ::close(m_fd);
        m_fd = -1;
        // From the event loop, as fail() says it.
        QTimer::singleShot(0, this, [this, reason] {
            Q_EMIT closed(reason);
        });
        return;
    }
    // Read now: fail() closes the descriptor before closed() asks.
    m_peer = peerOf(m_fd);
    m_readNotifier = std::make_unique<QSocketNotifier>(m_fd, QSocketNotifier::Read, this);
    connect(m_readNotifier.get(), &QSocketNotifier::activated, this, &DaemonLink::readable);
    m_writeNotifier = std::make_unique<QSocketNotifier>(m_fd, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier.get(), &QSocketNotifier::activated, this, &DaemonLink::writable);
}

DaemonLink::~DaemonLink()
{
    // The notifiers go before their descriptor: one of a closed descriptor
    // that a later socket reuses would watch that socket.
    m_readNotifier.reset();
    m_writeNotifier.reset();
    if (m_fd >= 0)
        ::close(m_fd);
}

bool DaemonLink::fits(const QString& fn, const QJsonArray& args, const QString& target)
{
    // The id of a call has at most 19 digits, and the one here has one.
    return callLine(0, target, fn, args).size() + 18 <= MaxRequestBytes;
}

void DaemonLink::call(const QString& fn, const QJsonArray& args, Answer answer)
{
    call(QStringLiteral("control"), fn, args, std::move(answer));
}

void DaemonLink::call(const QString& target, const QString& fn, const QJsonArray& args, Answer answer)
{
    const qint64 id = ++m_nextId;
    send(callLine(id, target, fn, args), id, std::move(answer));
}

void DaemonLink::notify(const QString& target, const QString& fn, const QJsonArray& args)
{
    const qint64 id = ++m_nextId;
    send(callLine(id, target, fn, args), id, {});
}

void DaemonLink::subscribe(const QString& event, Answer answer)
{
    const qint64 id = ++m_nextId;
    const QByteArray line = QJsonDocument(QJsonObject{{QLatin1String("type"), QLatin1String("subscribe")},
                                                      {QLatin1String("id"), id},
                                                      {QLatin1String("target"), QLatin1String("control")},
                                                      {QLatin1String("signal"), event}})
                                .toJson(QJsonDocument::Compact)
        + '\n';
    m_subscriptions.insert(id, event);
    send(line, id, std::move(answer));
}

void DaemonLink::send(const QByteArray& line, qint64 id, Answer answer)
{
    if (m_fd < 0) {
        if (answer)
            answerLater(std::move(answer), {{}, QStringLiteral("the connection to the mux daemon closed")});
        return;
    }
    // The router closes a connection whose request line is longer.
    if (line.size() > MaxRequestBytes) {
        m_subscriptions.remove(id);
        if (answer) {
            answerLater(std::move(answer),
                        {QLatin1String(AfterglowProto::IpcConnection::RequestTooLong),
                         QStringLiteral("the request is longer than the %1 MiB that one call carries")
                             .arg(MaxRequestBytes >> 20)});
        }
        return;
    }
    // A daemon that reads no requests: the queue does not grow without end.
    if (m_out.size() + line.size() > MaxQueuedBytes) {
        m_subscriptions.remove(id);
        if (answer) {
            answerLater(std::move(answer),
                        {QLatin1String(QueueFull),
                         QStringLiteral("the mux daemon has not read the %1 MiB of requests before this one")
                             .arg(m_out.size() >> 20)});
        }
        return;
    }
    if (answer)
        m_answers.insert(id, std::move(answer));
    m_out += line;
    writable();
}

void DaemonLink::writable()
{
    while (m_fd >= 0 && !m_out.isEmpty()) {
        // MSG_NOSIGNAL: a daemon that went away fails the write, and raises no SIGPIPE.
        const ssize_t sent = ::send(m_fd, m_out.constData(), std::size_t(m_out.size()), MSG_NOSIGNAL);
        if (sent < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            fail(
                QStringLiteral("cannot write to the mux daemon: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
            return;
        }
        m_out.remove(0, qsizetype(sent));
    }
    if (m_fd >= 0)
        m_writeNotifier->setEnabled(!m_out.isEmpty());
}

void DaemonLink::readable()
{
    char buffer[65536];
    for (int reads = 0; m_fd >= 0 && reads < MaxReadsAtOnce; ++reads) {
        const ssize_t got = ::recv(m_fd, buffer, sizeof buffer, 0);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            fail(QStringLiteral("cannot read from the mux daemon: %1")
                     .arg(QString::fromLocal8Bit(std::strerror(errno))));
            return;
        }
        if (got == 0) {
            fail(QStringLiteral("the mux daemon closed the connection"));
            return;
        }
        const qsizetype scanFrom = m_in.size();
        m_in.append(buffer, qsizetype(got));
        qsizetype start = 0;
        qsizetype end = m_in.indexOf('\n', scanFrom);
        while (end >= 0) {
            frame(m_in.mid(start, end - start));
            // A frame's answer can end the connection.
            if (m_fd < 0)
                return;
            start = end + 1;
            end = m_in.indexOf('\n', start);
        }
        m_in.remove(0, start);
        if (m_in.size() > MaxFrameBytes) {
            fail(QStringLiteral("the mux daemon sent a line longer than %1 MiB").arg(MaxFrameBytes >> 20));
            return;
        }
    }
}

void DaemonLink::frame(const QByteArray& line)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
    if (!document.isObject()) {
        fail(QStringLiteral("the mux daemon sent a line that is not a JSON object"));
        return;
    }
    const QJsonObject object = document.object();
    const QString type = object.value(QLatin1String("type")).toString();
    if (type == QLatin1String("event")) {
        const qint64 subscription = object.value(QLatin1String("subscriptionId")).toInteger(-1);
        if (const auto it = m_subscriptions.constFind(subscription); it != m_subscriptions.constEnd()) {
            // A copy: a slot that fails the connection clears m_subscriptions.
            const QString name = *it;
            Q_EMIT controlEvent(name, object.value(QLatin1String("args")).toArray());
        } else if (!object.contains(QLatin1String("subscriptionId"))) {
            Q_EMIT targetEvent(object.value(QLatin1String("target")).toString(),
                               object.value(QLatin1String("signal")).toString(),
                               object.value(QLatin1String("args")).toArray(), line.size() + 1);
        }
        return;
    }
    const QJsonValue idValue = object.value(QLatin1String("id"));
    const Error error{object.value(QLatin1String("code")).toString(),
                      object.value(QLatin1String("message")).toString()};
    // An error with no id: the daemon could not read a request, or ends the
    // connection, and says why.
    if (type == QLatin1String("error") && (idValue.isUndefined() || idValue.isNull())) {
        fail(error.message.isEmpty() ? QStringLiteral("the mux daemon refused a request") : error.message);
        return;
    }
    const qint64 id = idValue.toInteger(-1);
    if (const auto it = m_answers.find(id); it != m_answers.end()) {
        const Answer answer = std::move(*it);
        m_answers.erase(it);
        if (type == QLatin1String("reply")) {
            answer(object.value(QLatin1String("result")), nullptr);
        } else {
            m_subscriptions.remove(id);
            answer({}, &error);
        }
        return;
    }
    // The end of a subscription that the daemon took.
    if (type == QLatin1String("error")) {
        if (const auto it = m_subscriptions.find(id); it != m_subscriptions.end()) {
            const QString name = *it;
            m_subscriptions.erase(it);
            Q_EMIT subscriptionEnded(name, error.code, error.message);
        }
    }
}

void DaemonLink::fail(const QString& reason)
{
    if (m_fd < 0)
        return;
    // This can run in a notifier's own slot: they go from the event loop.
    for (std::unique_ptr<QSocketNotifier>* notifier : {&m_readNotifier, &m_writeNotifier}) {
        (*notifier)->setEnabled(false);
        notifier->release()->deleteLater();
    }
    ::close(m_fd);
    m_fd = -1;
    m_out.clear();
    m_in.clear();
    m_subscriptions.clear();
    const QHash<qint64, Answer> answers = std::exchange(m_answers, {});
    for (const Answer& answer : answers)
        answerLater(answer, {{}, reason});
    // From the event loop, as the answers: this can run inside call().
    QTimer::singleShot(0, this, [this, reason] {
        Q_EMIT closed(reason);
    });
}

void DaemonLink::answerLater(Answer answer, Error error)
{
    QTimer::singleShot(0, this, [answer = std::move(answer), error = std::move(error)] {
        answer({}, &error);
    });
}

} // namespace Afterglow
