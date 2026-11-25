// SPDX-FileCopyrightText: 2026 Phosphor Works Team
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

MuxCommands::MuxCommands(QString session, PhosphorChannel::StreamWriter* writer, std::function<void()> written,
                         QObject* parent)
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
                                                  const QString& session, std::string* error,
                                                  const std::function<std::optional<quint32>()>* focused)
{
    const std::string client = "a client of session '" + session.toStdString() + "'";
    // The Afterglow must read the request that the checks below read: a line
    // that the window cannot read, or that holds more lines, could pass
    // them with a request that they did not see.
    if (line.find('\n') != std::string::npos) {
        *error = AfterglowProto::ipcError(std::nullopt, AfterglowProto::IpcErrorCode::MalformedRequest,
                                          client + " sent a line with a newline in it");
        return std::nullopt;
    }
    std::string why;
    auto request = Json::parse(line, &why);
    if (!request || !request->isObject()) {
        *error = AfterglowProto::ipcError(std::nullopt, AfterglowProto::IpcErrorCode::MalformedRequest,
                                          client + " sent a line that is not a JSON object"
                                              + (why.empty() ? std::string() : ": " + why));
        return std::nullopt;
    }
    // Only the client's target: the Afterglow's other targets act on all
    // its windows, by its own pane IDs.
    if (const Json::Value* named = request->find("target"); named && named->toString() != target) {
        *error =
            AfterglowProto::ipcError((*request)["id"].toInteger(), AfterglowProto::IpcErrorCode::NoSuchTarget,
                                     client + " reaches only the target '" + std::string(target) + "' of its window");
        return std::nullopt;
    }
    // What is not a run goes as it came: the Afterglow answers it.
    if ((*request)["type"].toString() != "call" || (*request)["fn"].toString() != "run")
        return line;
    Json::Value* args = request->find("args");
    // phosphor-ipc refuses a call of another arity.
    if (!args || !args->isArray() || args->toArray().size() != 6)
        return line;
    Json::Array changed = args->toArray();
    // phosphor-ipc converts a string or a bool to the number that the
    // Afterglow takes ("7" is its pane 7, true its pane 1): not a pane of
    // the session.
    if (!changed[5].isNumber()) {
        *error = AfterglowProto::ipcError((*request)["id"].toInteger(), AfterglowProto::IpcErrorCode::InvalidArg,
                                          "the pane of a run from " + client + " is not a number");
        return std::nullopt;
    }
    const auto id = changed[5].toInteger();
    std::optional<quint32> windowPane;
    if (id == 0 && focused) {
        // The Afterglow's pane 0 is its focused pane, which can be a pane of
        // another window: the focused pane of the session's window instead.
        windowPane = *focused ? (*focused)() : std::nullopt;
        if (!windowPane) {
            *error = AfterglowProto::ipcError((*request)["id"].toInteger(), AfterglowProto::IpcErrorCode::CallFailed,
                                              "no pane of session '" + session.toStdString()
                                                  + "' has the focus in its window");
            return std::nullopt;
        }
    } else {
        // Pane 0 of a remote endpoint is the client's own pane.
        if (id == 0)
            return line;
        // Not a whole number of 32 bits (-1, 1.5, one past 2^53, or one
        // past 2^32 that a cast would make another pane): no pane of the session.
        if (!id || *id < 0 || *id > 0xffffffffLL) {
            *error = AfterglowProto::ipcError((*request)["id"].toInteger(), AfterglowProto::IpcErrorCode::InvalidArg,
                                              "the pane of a run from " + client + " is not a pane ID");
            return std::nullopt;
        }
        windowPane = pane ? pane(std::uint32_t(*id)) : std::nullopt;
    }
    if (!windowPane) {
        *error = AfterglowProto::ipcError((*request)["id"].toInteger(), AfterglowProto::IpcErrorCode::CallFailed,
                                          "no pane " + std::to_string(*id) + " of session '" + session.toStdString()
                                              + "' shows in its window");
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
                                              + std::to_string(AfterglowProto::IpcConnection::MaxRequestBytes >> 20)
                                              + " MiB");
        return std::nullopt;
    }
    return written;
}

std::optional<quint32> MuxCommands::relayPane(const Host& host, std::uint32_t pane, const QString& session,
                                              QString* error)
{
    // Pane 0: a client in no pane of the session, such as one that selected
    // an agent session. It runs on the focused pane of the session's window,
    // as on a session of this computer (forWindow).
    if (pane == 0) {
        const auto focused = host.focused ? host.focused() : std::nullopt;
        if (!focused)
            *error = QStringLiteral("no pane of session '%1' has the focus in its window").arg(session);
        return focused;
    }
    const auto shown = host.pane ? host.pane(pane) : std::nullopt;
    if (!shown)
        *error = QStringLiteral("no pane %1 of session '%2' shows in its window").arg(pane).arg(session);
    return shown;
}

void MuxCommands::setHost(Host host)
{
    m_host = std::move(host);
    openEarly([this](const Early& early) {
        return !m_host.binding || !m_host.binding(early.open.pane);
    });
}

void MuxCommands::bound(std::uint32_t pane)
{
    // Before setHost(), which opens them.
    if (!m_host.socket)
        return;
    openEarly([pane](const Early& early) {
        return early.open.pane == pane;
    });
}

void MuxCommands::openEarly(const std::function<bool(const Early& early)>& which)
{
    for (auto it = m_early.begin(); it != m_early.end();) {
        if (!which(it->second)) {
            ++it;
            continue;
        }
        const std::uint64_t id = it->first;
        Early early = std::move(it->second);
        it = m_early.erase(it);
        open(early.open);
        for (std::string& line : early.lines)
            bytes(id, std::move(line));
    }
}

bool MuxCommands::frame(Attach::RelayFrame frame)
{
    // closeAll() ran: the stream that carries the relays ended, and the
    // daemon ends them with it. A connection opened now could answer
    // nothing.
    if (!m_writer)
        return true;
    switch (frame.part) {
    case Attach::RelayPart::Open:
        if (m_relays.contains(frame.relay) || m_early.contains(frame.relay))
            return false;
        // Before the window gave its host, or made the client's pane (a
        // split of `afterglow ctl` whose program runs `aglow run` at once):
        // the connection waits for it.
        if (!m_host.socket || (m_host.binding && m_host.binding(frame.pane)))
            m_early[frame.relay] = Early{std::move(frame), {}, 0};
        else
            open(frame);
        return true;
    case Attach::RelayPart::Bytes:
        if (const auto early = m_early.find(frame.relay); early != m_early.end()) {
            if (early->second.bytes + frame.bytes.size() > MaxWaitingBytes) {
                send({frame.relay, Attach::RelayPart::Bytes,
                      AfterglowProto::ipcError(
                          std::nullopt, AfterglowProto::IpcErrorCode::CallFailed,
                          "the window of session '" + m_session.toStdString()
                              + "' holds no more lines of a client whose pane it did not make yet")});
                send({frame.relay, Attach::RelayPart::End, {}});
                m_early.erase(early);
                return true;
            }
            early->second.bytes += frame.bytes.size();
            early->second.lines.push_back(std::move(frame.bytes));
        } else {
            bytes(frame.relay, std::move(frame.bytes));
        }
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
        const auto pane = relayPane(m_host, open.pane, m_session, &error);
        if (pane && m_host.remote)
            path = m_host.remote(*pane, host, !ssh, &error);
    }
    if (path.isEmpty()) {
        if (error.isEmpty())
            error = QStringLiteral("the window of session '%1' runs no commands of scripts").arg(m_session);
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
    m_relays[id] = Relay{socket, ssh ? AfterglowProto::Remote::Target : AfterglowProto::Run::Target,
                         {},     !ssh && m_remoteHost.isEmpty(),
                         false,  {},
                         0};
    connect(socket, &QLocalSocket::connected, this, [this, id] {
        const auto relay = m_relays.find(id);
        if (relay == m_relays.end())
            return;
        relay->second.socket->write(relay->second.unsent.data(), qint64(relay->second.unsent.size()));
        relay->second.unsent.clear();
    });
    connect(socket, &QLocalSocket::readyRead, this, &MuxCommands::readSockets);
    connect(socket, &QLocalSocket::disconnected, this, [this, id] {
        endFromWindow(id);
    });
    connect(socket, &QLocalSocket::errorOccurred, this, [this, id](QLocalSocket::LocalSocketError) {
        const auto relay = m_relays.find(id);
        if (relay == m_relays.end())
            return;
        // Before it connected: the client hears why. After, the end of the
        // connection says it.
        if (relay->second.socket->state() != QLocalSocket::ConnectedState) {
            endFromWindow(id,
                          "cannot reach the Afterglow of the window of session '" + m_session.toStdString()
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
    if (relay == m_relays.end())
        return;
    // The daemon sends whole lines, one in each frame: forWindow refuses
    // a frame with another newline.
    if (bytes.ends_with('\n'))
        bytes.pop_back();
    std::string error;
    const auto line = forWindow(bytes, relay->second.target, m_host.pane, m_session, &error,
                                relay->second.local ? &m_host.focused : nullptr);
    if (!line) {
        answer(id, std::move(error));
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
    if (relay == m_relays.end())
        return;
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
    if (relay == m_relays.end())
        return;
    QLocalSocket* const socket = relay->second.socket;
    // What the Afterglow sent before it closed goes first.
    if (socket->bytesAvailable() > 0)
        forward(id, relay->second, socket->readAll().toStdString());
    std::vector<std::string> answers = std::move(relay->second.held);
    if (!reason.empty())
        answers.push_back(AfterglowProto::ipcError(std::nullopt, AfterglowProto::IpcErrorCode::CallFailed, reason));
    // The Afterglow ended inside a line: a newline ends that half, so that
    // the window's answers are lines of their own.
    if (relay->second.midLine && !answers.empty())
        send({id, Attach::RelayPart::Bytes, "\n"});
    m_relays.erase(relay);
    socket->disconnect(this);
    socket->deleteLater();
    for (std::string& line : answers)
        send({id, Attach::RelayPart::Bytes, std::move(line)});
    send({id, Attach::RelayPart::End, {}});
}

void MuxCommands::answer(std::uint64_t id, std::string line)
{
    const auto relay = m_relays.find(id);
    if (relay == m_relays.end())
        return;
    // The window reads the Afterglow's socket in any parts, and the daemon
    // joins the bytes of a relay into lines: an answer here, inside a line
    // of the Afterglow, would break both.
    if (!relay->second.midLine) {
        send({id, Attach::RelayPart::Bytes, std::move(line)});
        return;
    }
    if (relay->second.heldBytes + line.size() > MaxHeldAnswers) {
        endFromWindow(id,
                      "the Afterglow of the window of session '" + m_session.toStdString() + "' ended no line while "
                          + std::to_string(MaxHeldAnswers >> 10) + " KiB of the window's answers waited");
        return;
    }
    relay->second.heldBytes += line.size();
    relay->second.held.push_back(std::move(line));
}

void MuxCommands::forward(std::uint64_t id, Relay& relay, std::string bytes)
{
    if (!relay.held.empty()) {
        if (const std::size_t end = bytes.find('\n'); end != std::string::npos) {
            // The answers that waited go after the end of the line.
            std::string rest = bytes.substr(end + 1);
            bytes.resize(end + 1);
            send({id, Attach::RelayPart::Bytes, std::move(bytes)});
            for (std::string& line : relay.held)
                send({id, Attach::RelayPart::Bytes, std::move(line)});
            relay.held.clear();
            relay.heldBytes = 0;
            relay.midLine = false;
            if (rest.empty())
                return;
            bytes = std::move(rest);
        }
    }
    relay.midLine = !bytes.ends_with('\n');
    send({id, Attach::RelayPart::Bytes, std::move(bytes)});
}

void MuxCommands::readSockets()
{
    for (auto& [id, relay] : m_relays) {
        while (m_waitingBytes < MaxWaitingBytes && relay.socket->bytesAvailable() > 0) {
            const qint64 room = m_writer ? qint64(m_writer->maxFrame()) - 64 : 0;
            if (room <= 0)
                return;
            const QByteArray bytes = relay.socket->read(std::min({room, ReadBytes, relay.socket->bytesAvailable()}));
            if (bytes.isEmpty())
                break;
            forward(id, relay, bytes.toStdString());
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
        if (result == WriteResult::WouldBlock)
            break;
        // Closed: the stream ended, and the clients end with it.
        if (result == WriteResult::Closed) {
            m_waiting.clear();
            m_waitingBytes = 0;
            break;
        }
        // No frame is larger than ReadBytes and the end of a client line.
        // One that is never fits: kept, it would hold the frames of every
        // relay behind it, their ends included.
        if (result == WriteResult::TooLarge)
            qCWarning(lcMux) << "a command frame of session" << m_session << "is larger than the channel takes";
        else
            wrote = true;
        m_waitingBytes -= m_waiting.front().size();
        m_waiting.pop_front();
    }
    if (wrote && m_written)
        m_written();
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
