// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mux/MuxCommands.h"

#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/IpcServer.h>
#include <AfterglowProto/Json.h>
#include <AfterglowProto/Remote.h>
#include <AfterglowProto/Run.h>

#include <PhosphorChannel/Stream.h>

#include <QLocalSocket>
#include <QLoggingCategory>

#include <algorithm>

Q_DECLARE_LOGGING_CATEGORY(lcMux)

namespace Afterglow {

namespace Attach = AfterglowMux::Attach;
namespace Json = AfterglowProto::Json;
using PhosphorChannel::WriteResult;

namespace {

/// The bytes of one read of a connection: a frame of the channel holds them
/// with room to spare.
constexpr qint64 ReadBytes = 256 * 1024;

} // namespace

MuxCommands::MuxCommands(QString session, PhosphorChannel::StreamWriter* writer, std::function<void()> written, QObject* parent)
    : QObject(parent)
    , m_session(std::move(session))
    , m_writer(writer)
    , m_written(std::move(written))
{
}

MuxCommands::~MuxCommands()
{
    closeAll();
}

std::optional<std::string> MuxCommands::forWindow(const std::string& line, std::string_view target,
                                                  const std::function<std::optional<quint32>(std::uint32_t)>& pane,
                                                  const QString& session, std::string* error)
{
    // What is not a run of the target goes as it came: the Afterglow
    // answers it, a malformed line included.
    auto request = Json::parse(line);
    if (!request || !request->isObject()) return line;
    // Only the client's target: the Afterglow's other targets act on all
    // its windows, by its own pane IDs.
    if (const Json::Value* named = request->find("target"); named && named->toString() != target) {
        *error = AfterglowProto::ipcError((*request)["id"].toInteger(), AfterglowProto::IpcErrorCode::NoSuchTarget,
                                          "a client of session '" + session.toStdString() + "' reaches only the target '"
                                              + std::string(target) + "' of its window");
        return std::nullopt;
    }
    if ((*request)["type"].toString() != "call" || (*request)["fn"].toString() != "run") return line;
    Json::Value* args = request->find("args");
    if (!args || !args->isArray() || args->toArray().size() != 6) return line;
    Json::Array changed = args->toArray();
    const auto id = changed[5].toInteger();
    // The Afterglow refuses an ID that is not one, with its own message, and
    // pane 0 is its focused pane, as for any client, or the pane of a
    // remote client.
    if (!id || *id <= 0 || *id > 0xffffffffLL) return line;
    const auto windowPane = pane ? pane(std::uint32_t(*id)) : std::nullopt;
    if (!windowPane) {
        *error = AfterglowProto::ipcError((*request)["id"].toInteger(), AfterglowProto::IpcErrorCode::CallFailed,
                                          "no pane " + std::to_string(*id) + " of session '" + session.toStdString() + "' shows in its window");
        return std::nullopt;
    }
    changed[5] = Json::Value(double(*windowPane));
    *args = Json::Value(std::move(changed));
    std::string written = Json::write(*request);
    // A longer ID can pass the Afterglow's limit, which would close the
    // connection: `aglow run` leaves room for the longest, another client
    // gets this.
    if (written.size() + 1 > AfterglowProto::IpcConnection::MaxRequestBytes) {
        *error = AfterglowProto::ipcError((*request)["id"].toInteger(), AfterglowProto::IpcErrorCode::CallFailed,
                                          "with the window's ID of its pane, the run request is over "
                                              + std::to_string(AfterglowProto::IpcConnection::MaxRequestBytes >> 20) + " MiB");
        return std::nullopt;
    }
    return written;
}

void MuxCommands::setHost(Host host)
{
    m_host = std::move(host);
    for (auto& [id, early] : std::exchange(m_early, {})) {
        open(early.first);
        for (std::string& line : early.second) bytes(id, std::move(line));
    }
}

bool MuxCommands::frame(Attach::RelayFrame frame)
{
    switch (frame.part) {
    case Attach::RelayPart::Open:
        if (m_relays.contains(frame.relay) || m_early.contains(frame.relay)) return false;
        // Before the window gave its host: the connection waits for it.
        if (!m_host.socket) m_early[frame.relay] = {std::move(frame), {}};
        else open(frame);
        return true;
    case Attach::RelayPart::Bytes:
        if (const auto early = m_early.find(frame.relay); early != m_early.end()) early->second.second.push_back(std::move(frame.bytes));
        else bytes(frame.relay, std::move(frame.bytes));
        return true;
    case Attach::RelayPart::End:
        m_early.erase(frame.relay);
        endFromDaemon(frame.relay);
        return true;
    }
    return false;
}

void MuxCommands::open(const Attach::RelayFrame& open)
{
    const std::uint64_t id = open.relay;
    const bool ssh = open.kind == Attach::RelayKind::Remote;
    QString error;
    QString path;
    if (!ssh && m_remoteHost.isEmpty()) {
        path = m_host.socket ? m_host.socket(&error) : QString();
    } else {
        // A client of another host gets what an ssh session of that host
        // gets: the commands marked remote, on its own pane.
        const QString host = ssh ? QString::fromStdString(open.host) : m_remoteHost;
        const auto pane = m_host.pane ? m_host.pane(open.pane) : std::nullopt;
        if (!pane) error = QStringLiteral("no pane %1 of session '%2' shows in its window").arg(open.pane).arg(m_session);
        else if (m_host.remote) path = m_host.remote(*pane, host, !ssh, &error);
    }
    if (path.isEmpty()) {
        if (error.isEmpty()) error = QStringLiteral("the window of session '%1' runs no commands of scripts").arg(m_session);
        // Not in m_relays: the bytes that follow are dropped.
        send({id, Attach::RelayPart::Bytes,
              AfterglowProto::ipcError(std::nullopt, AfterglowProto::IpcErrorCode::CallFailed, error.toStdString())});
        send({id, Attach::RelayPart::End, {}});
        return;
    }
    auto* socket = new QLocalSocket(this);
    // A client that reads slowly holds this much here, then the Afterglow's
    // socket fills.
    socket->setReadBufferSize(ReadBytes);
    m_relays[id] = Relay{socket, ssh ? AfterglowProto::Remote::Target : AfterglowProto::Run::Target, {}};
    connect(socket, &QLocalSocket::connected, this, [this, id] {
        const auto relay = m_relays.find(id);
        if (relay == m_relays.end()) return;
        relay->second.socket->write(relay->second.unsent.data(), qint64(relay->second.unsent.size()));
        relay->second.unsent.clear();
    });
    connect(socket, &QLocalSocket::readyRead, this, &MuxCommands::readSockets);
    connect(socket, &QLocalSocket::disconnected, this, [this, id] { endFromWindow(id); });
    connect(socket, &QLocalSocket::errorOccurred, this, [this, id](QLocalSocket::LocalSocketError) {
        const auto relay = m_relays.find(id);
        if (relay == m_relays.end()) return;
        // Before it connected: the client hears why. After, the end of the
        // connection says it.
        if (relay->second.socket->state() != QLocalSocket::ConnectedState) {
            endFromWindow(id, "cannot reach the Afterglow of the window of session '" + m_session.toStdString()
                                  + "': " + relay->second.socket->errorString().toStdString());
        }
    });
    socket->connectToServer(path);
}

void MuxCommands::bytes(std::uint64_t id, std::string bytes)
{
    const auto relay = m_relays.find(id);
    // A relay that the window refused or ended: the daemon learns it from
    // the End that went.
    if (relay == m_relays.end()) return;
    // The daemon sends whole lines, one in each frame.
    if (bytes.ends_with('\n')) bytes.pop_back();
    std::string error;
    const auto line = forWindow(bytes, relay->second.target, m_host.pane, m_session, &error);
    if (!line) {
        send({id, Attach::RelayPart::Bytes, std::move(error)});
        return;
    }
    QLocalSocket* const socket = relay->second.socket;
    if (socket->state() == QLocalSocket::ConnectedState) {
        socket->write(line->data(), qint64(line->size()));
        socket->write("\n", 1);
    } else {
        relay->second.unsent += *line;
        relay->second.unsent += '\n';
    }
}

void MuxCommands::endFromDaemon(std::uint64_t id)
{
    const auto relay = m_relays.find(id);
    if (relay == m_relays.end()) return;
    QLocalSocket* const socket = relay->second.socket;
    m_relays.erase(relay);
    // Nothing more goes to the client. The lines that it sent before it
    // ended still reach the Afterglow, whose runs end with the connection.
    // A connect that has not finished is given up: the lines for it go
    // with it.
    socket->disconnect(this);
    if (socket->state() != QLocalSocket::ConnectedState) {
        socket->abort();
        socket->deleteLater();
        return;
    }
    connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
    socket->disconnectFromServer();
}

void MuxCommands::endFromWindow(std::uint64_t id, const std::string& reason)
{
    const auto relay = m_relays.find(id);
    if (relay == m_relays.end()) return;
    QLocalSocket* const socket = relay->second.socket;
    // What the Afterglow sent before it closed goes first.
    if (socket->bytesAvailable() > 0) {
        const QByteArray rest = socket->readAll();
        send({id, Attach::RelayPart::Bytes, rest.toStdString()});
    }
    m_relays.erase(relay);
    socket->disconnect(this);
    socket->deleteLater();
    if (!reason.empty()) {
        send({id, Attach::RelayPart::Bytes,
              AfterglowProto::ipcError(std::nullopt, AfterglowProto::IpcErrorCode::CallFailed, reason)});
    }
    send({id, Attach::RelayPart::End, {}});
}

void MuxCommands::readSockets()
{
    for (auto& [id, relay] : m_relays) {
        while (m_waitingBytes < MaxWaitingBytes && relay.socket->bytesAvailable() > 0) {
            const qint64 room = m_writer ? qint64(m_writer->maxFrame()) - 64 : 0;
            if (room <= 0) return;
            const QByteArray bytes = relay.socket->read(std::min({room, ReadBytes, relay.socket->bytesAvailable()}));
            if (bytes.isEmpty()) break;
            send({id, Attach::RelayPart::Bytes, bytes.toStdString()});
        }
    }
}

void MuxCommands::send(Attach::RelayFrame frame)
{
    std::string encoded = Attach::encode(frame);
    m_waitingBytes += encoded.size();
    m_waiting.push_back(std::move(encoded));
    flush();
}

void MuxCommands::flush()
{
    bool wrote = false;
    while (m_writer && !m_waiting.empty()) {
        const WriteResult result = m_writer->tryWrite(m_waiting.front());
        if (result != WriteResult::Written) {
            // No frame is larger than ReadBytes and the end of a client line:
            // the stream's own end is the only other cause.
            if (result == WriteResult::TooLarge) qCWarning(lcMux) << "a command frame of session" << m_session << "is larger than the channel takes";
            // Closed: the stream ended, and the clients end with it.
            if (result != WriteResult::WouldBlock) {
                m_waiting.clear();
                m_waitingBytes = 0;
            }
            break;
        }
        m_waitingBytes -= m_waiting.front().size();
        m_waiting.pop_front();
        wrote = true;
    }
    if (wrote && m_written) m_written();
}

void MuxCommands::writable()
{
    flush();
    readSockets();
}

void MuxCommands::closeAll()
{
    for (auto& [id, relay] : m_relays) {
        relay.socket->disconnect(this);
        relay.socket->abort();
        relay.socket->deleteLater();
    }
    m_relays.clear();
    m_early.clear();
    m_waiting.clear();
    m_waitingBytes = 0;
    m_writer = nullptr;
}

} // namespace Afterglow
