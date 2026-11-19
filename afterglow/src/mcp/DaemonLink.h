// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonValue>
#include <QObject>
#include <QString>

#include <functional>

class QSocketNotifier;

namespace Afterglow {

/// The connection of `afterglow mcp` to the mux daemon, after
/// AfterglowProto::IpcConnection made it, selected the session and bound it
/// (Mcp.cpp), and released its descriptor. AfterglowProto::IpcConnection
/// waits in send() while the daemon reads nothing, and its calls wait for
/// their answers: the server must go on reading the client and other
/// answers meanwhile, so that a run that waits does not stop a read in
/// another pane. Here a request goes into a queue that a QSocketNotifier
/// writes when the socket takes more, the answers come through another one,
/// and each call has a callback. Nothing waits.
///
/// The phosphor-ipc wire format: one JSON object per line. A call is
/// {"type":"call","id":N,"target":"control","fn":F,"args":[...]}, a
/// subscription {"type":"subscribe","id":N,"target":"control","signal":S},
/// and the daemon answers each with a reply or an error frame of its id. The
/// events of a subscription carry its id as "subscriptionId", and an error
/// frame of that id ends it.
class DaemonLink : public QObject
{
    Q_OBJECT

public:
    struct Error
    {
        QString code; ///< the daemon's error code, empty when the connection failed
        QString message;
    };
    /// Runs once for each call, from the event loop: with the result, or with
    /// @p error set. Never inside call().
    using Answer = std::function<void(const QJsonValue& result, const Error* error)>;

    /// Takes @p fd, a connected socket of the daemon, and closes it at the end.
    explicit DaemonLink(int fd, QObject* parent = nullptr);
    ~DaemonLink() override;
    DaemonLink(const DaemonLink&) = delete;
    DaemonLink& operator=(const DaemonLink&) = delete;

    bool isOpen() const
    {
        return m_fd >= 0;
    }
    /// Whether a call of the control target with @p fn and @p args fits in
    /// one request line (AfterglowProto::IpcConnection::MaxRequestBytes).
    static bool fits(const QString& fn, const QJsonArray& args);
    /// Calls @p fn of the control target of the selected session.
    void call(const QString& fn, const QJsonArray& args, Answer answer);
    /// Subscribes to the event @p event of the control target. @p answer
    /// tells whether the daemon took it. Its events come as event().
    void subscribe(const QString& event, Answer answer);

Q_SIGNALS:
    void event(const QString& name, const QJsonArray& args);
    /// The daemon ended the subscription to @p name: NO_SUCH_SESSION when
    /// the session ended, NO_SUCH_TARGET when Control.enabled went off.
    void subscriptionEnded(const QString& name, const QString& code, const QString& message);
    /// The connection closed, from the event loop: every call that waited
    /// gets an error.
    void closed(const QString& reason);

private:
    void send(const QByteArray& line, qint64 id, Answer answer);
    void readable();
    void writable();
    void frame(const QByteArray& line);
    void fail(const QString& reason);
    /// Runs @p answer from the event loop.
    void answerLater(Answer answer, Error error);

    int m_fd = -1;
    QSocketNotifier* m_readNotifier = nullptr;
    QSocketNotifier* m_writeNotifier = nullptr;
    QByteArray m_in;
    QByteArray m_out;
    qint64 m_nextId = 0;
    QHash<qint64, Answer> m_answers;
    /// The events of the subscriptions, by the id of their request.
    QHash<qint64, QString> m_subscriptions;
};

} // namespace Afterglow
