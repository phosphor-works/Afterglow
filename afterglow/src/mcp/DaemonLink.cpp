// SPDX-FileCopyrightText: 2026 fuddlesworth
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
#include <unistd.h>

namespace Afterglow {

namespace {

constexpr qsizetype MaxRequestBytes = qsizetype(AfterglowProto::IpcConnection::MaxRequestBytes);
/// The longest line that the daemon sends (its output limit).
constexpr qsizetype MaxFrameBytes = qsizetype(AfterglowProto::IpcConnection::MaxFrameBytes);

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

} // namespace

DaemonLink::DaemonLink(int fd, QObject* parent)
    : QObject(parent)
    , m_fd(fd)
{
    // The writes wait in m_out, never in the kernel's send().
    ::fcntl(m_fd, F_SETFL, ::fcntl(m_fd, F_GETFL) | O_NONBLOCK);
    ::fcntl(m_fd, F_SETFD, FD_CLOEXEC);
    m_readNotifier = new QSocketNotifier(m_fd, QSocketNotifier::Read, this);
    connect(m_readNotifier, &QSocketNotifier::activated, this, &DaemonLink::readable);
    m_writeNotifier = new QSocketNotifier(m_fd, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier, &QSocketNotifier::activated, this, &DaemonLink::writable);
}

DaemonLink::~DaemonLink()
{
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
    while (m_fd >= 0) {
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
            Q_EMIT event(*it, object.value(QLatin1String("args")).toArray());
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
    for (QSocketNotifier* notifier : {m_readNotifier, m_writeNotifier}) {
        notifier->setEnabled(false);
        notifier->deleteLater();
    }
    m_readNotifier = nullptr;
    m_writeNotifier = nullptr;
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
