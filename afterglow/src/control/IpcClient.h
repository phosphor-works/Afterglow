// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QLocalSocket>
#include <QString>

#include <optional>

#include <sys/types.h>

namespace Afterglow {

/// A blocking client of a phosphor-ipc socket, for `afterglow ctl` and the
/// launch handoff. Calls wait for their reply. Events that arrive meanwhile
/// wait for readEvent().
class IpcClient
{
public:
    /// Why a request failed. code is a phosphor-ipc error code (CALL_FAILED,
    /// NO_SUCH_TARGET...) when the server answered with an error, else empty:
    /// the connection failed, closed or timed out.
    struct Error
    {
        QString code;
        QString message;
    };

    bool connectTo(const QString& path, int timeoutMs, Error* error);
    bool connected() const { return m_socket.state() == QLocalSocket::ConnectedState; }
    /// The process at the other end (SO_PEERCRED), or 0.
    pid_t peerPid() const;

    /// The result of @p target.@p fn, or nothing with @p error set.
    std::optional<QJsonValue> call(const QString& target, const QString& fn, const QJsonArray& args, int timeoutMs, Error* error);
    bool subscribe(const QString& target, const QString& event, int timeoutMs, Error* error);
    /// The event name of a subscription.
    QString eventName(qint64 subscription) const { return m_subscriptions.value(subscription); }

    /// The next event, or nothing when the server closed the connection or
    /// ended the stream (@p error then says why).
    std::optional<QJsonObject> readEvent(Error* error);

private:
    bool write(const QJsonObject& request, int timeoutMs, Error* error);
    std::optional<QJsonObject> readFrame(int timeoutMs, Error* error);
    std::optional<QJsonObject> readReply(qint64 id, int timeoutMs, Error* error);

    QLocalSocket m_socket;
    qint64 m_nextId = 0;
    QHash<qint64, QString> m_subscriptions;
    QList<QJsonObject> m_frames; ///< events and stream ends that came during a call
};

} // namespace Afterglow
