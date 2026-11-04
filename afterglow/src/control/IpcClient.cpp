// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "IpcClient.h"

#include <PhosphorIpc/IpcProtocol.h>

#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>

#include <sys/socket.h>
#include <unistd.h>

namespace Afterglow {

namespace {

QLatin1String field(const char* name)
{
    return QLatin1String(name);
}

void set(IpcClient::Error* error, const QString& code, const QString& message)
{
    if (error) *error = {code, message, false, false};
}

void timedOut(IpcClient::Error* error)
{
    if (error) *error = {{}, QStringLiteral("no reply in time"), true, false};
}

bool isType(const QJsonObject& frame, const char* type)
{
    return frame.value(field(PhosphorIpc::Field::Type)) == QLatin1String(type);
}

void setFromFrame(IpcClient::Error* error, const QJsonObject& frame)
{
    set(error, frame.value(field(PhosphorIpc::Field::Code)).toString(), frame.value(field(PhosphorIpc::Field::Message)).toString());
}

// QIODevice waits forever on -1: a deadline that has passed waits not at all.
int waitTime(const QDeadlineTimer& deadline)
{
    return deadline.isForever() ? -1 : int(std::max<qint64>(0, deadline.remainingTime()));
}

} // namespace

bool IpcClient::connectTo(const QString& path, int timeoutMs, Error* error)
{
    m_socket.connectToServer(path);
    if (m_socket.waitForConnected(timeoutMs)) return true;
    set(error, {}, m_socket.errorString());
    return false;
}

pid_t IpcClient::peerPid() const
{
    ucred credentials{};
    socklen_t size = sizeof(credentials);
    const auto fd = int(m_socket.socketDescriptor());
    if (fd < 0 || getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0) return 0;
    return credentials.uid == getuid() ? credentials.pid : 0;
}

std::optional<QJsonValue> IpcClient::call(const QString& target, const QString& fn, const QJsonArray& args, int timeoutMs,
                                          Error* error)
{
    const QDeadlineTimer deadline(timeoutMs);
    const qint64 id = ++m_nextId;
    if (!write({{field(PhosphorIpc::Field::Type), field(PhosphorIpc::RequestType::Call)},
                {field(PhosphorIpc::Field::Id), id},
                {field(PhosphorIpc::Field::Target), target},
                {field(PhosphorIpc::Field::Fn), fn},
                {field(PhosphorIpc::Field::Args), args}},
               deadline, error)) {
        return std::nullopt;
    }
    const auto frame = readReply(id, deadline, error);
    if (!frame) return std::nullopt;
    return frame->value(field(PhosphorIpc::Field::Result));
}

std::optional<QStringList> IpcClient::targets(int timeoutMs, Error* error)
{
    const QDeadlineTimer deadline(timeoutMs);
    const qint64 id = ++m_nextId;
    if (!write({{field(PhosphorIpc::Field::Type), field(PhosphorIpc::RequestType::List)}, {field(PhosphorIpc::Field::Id), id}},
               deadline, error)) {
        return std::nullopt;
    }
    const auto frame = readReply(id, deadline, error);
    if (!frame) return std::nullopt;
    QStringList names;
    for (const QJsonValue& name : frame->value(field(PhosphorIpc::Field::Result)).toArray()) names << name.toString();
    return names;
}

bool IpcClient::subscribe(const QString& target, const QString& event, int timeoutMs, Error* error)
{
    const QDeadlineTimer deadline(timeoutMs);
    const qint64 id = ++m_nextId;
    if (!write({{field(PhosphorIpc::Field::Type), field(PhosphorIpc::RequestType::Subscribe)},
                {field(PhosphorIpc::Field::Id), id},
                {field(PhosphorIpc::Field::Target), target},
                {field(PhosphorIpc::Field::Signal), event}},
               deadline, error)
        || !readReply(id, deadline, error)) {
        return false;
    }
    m_subscriptions.insert(id, event);
    return true;
}

std::optional<QJsonObject> IpcClient::readEvent(Error* error)
{
    while (true) {
        const auto frame = m_frames.isEmpty() ? readFrame(QDeadlineTimer::Forever, error) : std::optional(m_frames.takeFirst());
        if (!frame) return std::nullopt;
        if (isType(*frame, PhosphorIpc::ResponseType::Event)) return frame;
        // The server ends a subscription with an error that carries its id,
        // for example when it unregisters the target.
        const qint64 id = frame->value(field(PhosphorIpc::Field::Id)).toInteger();
        if (isType(*frame, PhosphorIpc::ResponseType::Error) && m_subscriptions.remove(id) > 0) {
            setFromFrame(error, *frame);
            return std::nullopt;
        }
    }
}

bool IpcClient::write(const QJsonObject& request, const QDeadlineTimer& deadline, Error* error)
{
    const QByteArray line = PhosphorIpc::writeLine(request);
    if (line.size() > MaxLineBytes) {
        if (error) *error = {{}, QStringLiteral("the request is over %1 MiB").arg(MaxLineBytes >> 20), false, true};
        return false;
    }
    m_socket.write(line);
    while (m_socket.bytesToWrite() > 0) {
        if (!m_socket.waitForBytesWritten(waitTime(deadline))) {
            if (m_socket.state() == QLocalSocket::ConnectedState) {
                timedOut(error);
            } else {
                set(error, {}, m_socket.errorString());
            }
            return false;
        }
    }
    return true;
}

std::optional<QJsonObject> IpcClient::readFrame(const QDeadlineTimer& deadline, Error* error)
{
    while (!m_socket.canReadLine()) {
        if (m_socket.state() != QLocalSocket::ConnectedState && m_socket.bytesAvailable() == 0) {
            set(error, {}, QStringLiteral("the connection closed"));
            return std::nullopt;
        }
        if (!m_socket.waitForReadyRead(waitTime(deadline)) && !m_socket.canReadLine()) {
            if (m_socket.state() == QLocalSocket::ConnectedState) {
                timedOut(error);
            } else {
                set(error, {}, QStringLiteral("the connection closed"));
            }
            return std::nullopt;
        }
    }
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(m_socket.readLine(), &parse);
    if (!doc.isObject()) {
        set(error, {}, QStringLiteral("an unreadable reply: %1").arg(parse.errorString()));
        return std::nullopt;
    }
    return doc.object();
}

std::optional<QJsonObject> IpcClient::readReply(qint64 id, const QDeadlineTimer& deadline, Error* error)
{
    while (true) {
        const auto frame = readFrame(deadline, error);
        if (!frame) return std::nullopt;
        const QJsonValue frameId = frame->value(field(PhosphorIpc::Field::Id));
        // The server cannot name the request of a line that it could not
        // read. Calls run one at a time, so it is this one.
        if (frameId.isUndefined() && isType(*frame, PhosphorIpc::ResponseType::Error)) {
            setFromFrame(error, *frame);
            return std::nullopt;
        }
        if (frameId.toInteger() != id || isType(*frame, PhosphorIpc::ResponseType::Event)) {
            // An event, or a subscription that ended: readEvent's.
            m_frames.append(*frame);
            continue;
        }
        if (isType(*frame, PhosphorIpc::ResponseType::Error)) {
            setFromFrame(error, *frame);
            return std::nullopt;
        }
        return frame;
    }
}

} // namespace Afterglow
