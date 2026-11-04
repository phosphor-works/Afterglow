// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QDeadlineTimer>
#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QLocalSocket>
#include <QString>
#include <QStringList>

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
        bool timedOut = false; ///< connected, but no answer in time
        bool tooLong = false; ///< not sent: over MaxLineBytes
    };

    /// phosphor-ipc's router closes the connection on a longer request line.
    static constexpr qsizetype MaxLineBytes = 1024 * 1024;

    bool connectTo(const QString& path, int timeoutMs, Error* error);
    bool connected() const { return m_socket.state() == QLocalSocket::ConnectedState; }
    /// The process at the other end (SO_PEERCRED), or 0 when it runs as
    /// another user.
    pid_t peerPid() const;

    /// The result of @p target.@p fn, or nothing with @p error set.
    /// @p timeoutMs bounds the whole call, whatever arrives meanwhile.
    std::optional<QJsonValue> call(const QString& target, const QString& fn, const QJsonArray& args, int timeoutMs, Error* error);
    bool subscribe(const QString& target, const QString& event, int timeoutMs, Error* error);
    /// The names of the targets on the socket.
    std::optional<QStringList> targets(int timeoutMs, Error* error);
    /// The event name of a subscription.
    QString eventName(qint64 subscription) const { return m_subscriptions.value(subscription); }

    /// The next event, or nothing when the server closed the connection or
    /// ended the stream (@p error then says why).
    std::optional<QJsonObject> readEvent(Error* error);

private:
    bool write(const QJsonObject& request, const QDeadlineTimer& deadline, Error* error);
    std::optional<QJsonObject> readFrame(const QDeadlineTimer& deadline, Error* error);
    std::optional<QJsonObject> readReply(qint64 id, const QDeadlineTimer& deadline, Error* error);

    QLocalSocket m_socket;
    qint64 m_nextId = 0;
    QHash<qint64, QString> m_subscriptions;
    QList<QJsonObject> m_frames; ///< events and stream ends that came during a call
};

} // namespace Afterglow
