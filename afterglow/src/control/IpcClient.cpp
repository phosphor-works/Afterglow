// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "IpcClient.h"

#include <PhosphorIpc/IpcProtocol.h>

#include <QJsonArray>
#include <QJsonDocument>

#include <sys/socket.h>

namespace Afterglow {

namespace {

QLatin1String field(const char* name)
{
    return QLatin1String(name);
}

void set(IpcClient::Error* error, const QString& code, const QString& message)
{
    if (error) *error = {code, message};
}

bool isType(const QJsonObject& frame, const char* type)
{
    return frame.value(field(PhosphorIpc::Field::Type)) == QLatin1String(type);
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
    return credentials.pid;
}

std::optional<QJsonValue> IpcClient::call(const QString& target, const QString& fn, const QJsonArray& args, int timeoutMs,
                                          Error* error)
{
    const qint64 id = ++m_nextId;
    if (!write({{field(PhosphorIpc::Field::Type), field(PhosphorIpc::RequestType::Call)},
                {field(PhosphorIpc::Field::Id), id},
                {field(PhosphorIpc::Field::Target), target},
                {field(PhosphorIpc::Field::Fn), fn},
                {field(PhosphorIpc::Field::Args), args}},
               timeoutMs, error)) {
        return std::nullopt;
    }
    const auto frame = readReply(id, timeoutMs, error);
    if (!frame) return std::nullopt;
    return frame->value(field(PhosphorIpc::Field::Result));
}

bool IpcClient::subscribe(const QString& target, const QString& event, int timeoutMs, Error* error)
{
    const qint64 id = ++m_nextId;
    if (!write({{field(PhosphorIpc::Field::Type), field(PhosphorIpc::RequestType::Subscribe)},
                {field(PhosphorIpc::Field::Id), id},
                {field(PhosphorIpc::Field::Target), target},
                {field(PhosphorIpc::Field::Signal), event}},
               timeoutMs, error)
        || !readReply(id, timeoutMs, error)) {
        return false;
    }
    m_subscriptions.insert(id, event);
    return true;
}

std::optional<QJsonObject> IpcClient::readEvent(Error* error)
{
    while (true) {
        const auto frame = m_frames.isEmpty() ? readFrame(-1, error) : std::optional(m_frames.takeFirst());
        if (!frame) return std::nullopt;
        if (isType(*frame, PhosphorIpc::ResponseType::Event)) return frame;
        // The server ends a subscription with an error that carries its id,
        // for example when it unregisters the target.
        const qint64 id = frame->value(field(PhosphorIpc::Field::Id)).toInteger();
        if (isType(*frame, PhosphorIpc::ResponseType::Error) && m_subscriptions.remove(id) > 0) {
            set(error, frame->value(field(PhosphorIpc::Field::Code)).toString(),
                frame->value(field(PhosphorIpc::Field::Message)).toString());
            return std::nullopt;
        }
    }
}

bool IpcClient::write(const QJsonObject& request, int timeoutMs, Error* error)
{
    m_socket.write(PhosphorIpc::writeLine(request));
    if (m_socket.waitForBytesWritten(timeoutMs)) return true;
    set(error, {}, m_socket.errorString());
    return false;
}

std::optional<QJsonObject> IpcClient::readFrame(int timeoutMs, Error* error)
{
    while (!m_socket.canReadLine()) {
        if (m_socket.state() != QLocalSocket::ConnectedState && m_socket.bytesAvailable() == 0) {
            set(error, {}, QStringLiteral("the connection closed"));
            return std::nullopt;
        }
        if (!m_socket.waitForReadyRead(timeoutMs) && !m_socket.canReadLine()) {
            set(error, {}, m_socket.state() == QLocalSocket::ConnectedState ? QStringLiteral("no reply in time")
                                                                             : QStringLiteral("the connection closed"));
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

std::optional<QJsonObject> IpcClient::readReply(qint64 id, int timeoutMs, Error* error)
{
    while (true) {
        const auto frame = readFrame(timeoutMs, error);
        if (!frame) return std::nullopt;
        if (frame->value(field(PhosphorIpc::Field::Id)).toInteger() != id || isType(*frame, PhosphorIpc::ResponseType::Event)) {
            // An event, or a subscription that ended: readEvent's.
            m_frames.append(*frame);
            continue;
        }
        if (isType(*frame, PhosphorIpc::ResponseType::Error)) {
            set(error, frame->value(field(PhosphorIpc::Field::Code)).toString(),
                frame->value(field(PhosphorIpc::Field::Message)).toString());
            return std::nullopt;
        }
        return frame;
    }
}

} // namespace Afterglow
