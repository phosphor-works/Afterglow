// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mux/MuxClient.h"

#include "Pane.h"

#include "MuxConnect.h"
#include "MuxStream.h"

#include <AfterglowVt/StateSync.h>
#include <PhosphorChannel/Stream.h>

#include <QLoggingCategory>
#include <QSocketNotifier>

#include <chrono>
#include <cstring>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcMux, "afterglow.mux")

namespace Afterglow {

namespace Json = AfterglowProto::Json;
using PhosphorChannel::ReadResult;
using PhosphorChannel::WriteResult;

namespace {

int connectTo(const std::string& path)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.size() >= sizeof address.sun_path) {
        ::close(fd);
        return -1;
    }
    std::memcpy(address.sun_path, path.data(), path.size());
    if (::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

} // namespace

std::shared_ptr<MuxClient> MuxClient::attach(const QString& name, bool takeover, QString* error,
                                              const AfterglowProto::MuxSockets& sockets)
{
    const auto fail = [error](const QString& text) -> std::shared_ptr<MuxClient> {
        if (error) *error = text;
        return nullptr;
    };
    std::string reason;
    AfterglowMux::Reached reached = AfterglowMux::connectAny(sockets, &reason);
    if (!reached.connection) {
        return fail(reason.empty() ? QStringLiteral("no mux daemon runs, so there is no session '%1'").arg(name)
                                   : QString::fromStdString(reason));
    }
    AfterglowProto::IpcConnection::Error callError;
    const auto result = reached.connection->call(AfterglowMux::Target, AfterglowMux::Fn::Attach,
                                                 Json::Array{AfterglowMux::ProtocolVersion, name.toStdString(),
                                                             Json::Object{{"takeover", takeover}}},
                                                 AfterglowMux::CallTimeoutMs, &callError);
    if (!result) return fail(QString::fromStdString(callError.message));
    const int fd = connectTo((*result)["socket"].toString());
    if (fd < 0) return fail(QStringLiteral("cannot connect to the attach socket of session '%1'").arg(name));

    std::shared_ptr<MuxClient> client(new MuxClient);
    client->m_session = name;
    client->m_sockets = sockets;
    client->m_stream = std::make_unique<PhosphorChannel::Stream>(fd);
    std::string streamError;
    client->m_input = client->m_stream->openWriter(AfterglowMux::Attach::ControlChannel, AfterglowMux::Attach::InputCodec, &streamError);
    if (!client->m_input) return fail(QString::fromStdString(streamError));

    // The daemon names the panes first: the window shows them once it knows them.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(AfterglowMux::Attach::ConnectWaitMs);
    while (client->m_paneIds.empty() && !client->m_ended) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return fail(QStringLiteral("session '%1' did not answer").arg(name));
        if (!client->m_stream->pump(int(left.count()))) {
            return fail(QStringLiteral("session '%1' closed the connection: %2")
                            .arg(name, QString::fromStdString(client->m_stream->error())));
        }
        client->handleFrames();
    }
    if (client->m_ended) return fail(QStringLiteral("session '%1' ended").arg(name));

    client->m_readNotifier = new QSocketNotifier(client->m_stream->readFd(), QSocketNotifier::Read, client.get());
    connect(client->m_readNotifier, &QSocketNotifier::activated, client.get(), &MuxClient::readable);
    client->m_writeNotifier = new QSocketNotifier(client->m_stream->writeFd(), QSocketNotifier::Write, client.get());
    connect(client->m_writeNotifier, &QSocketNotifier::activated, client.get(), &MuxClient::writable);
    client->m_stream->onWritable = [raw = client.get()](PhosphorChannel::StreamWriter&) { raw->sendWaitingInput(); };
    client->updateNotifiers();
    return client;
}

MuxClient::~MuxClient() = default;

void MuxClient::bind(std::uint32_t id, Pane* pane)
{
    m_panes[id] = pane;
    // Frames that came before the pane existed wait in the stream's reader.
    handleFrames();
}

void MuxClient::release(std::uint32_t id, bool kill)
{
    m_panes.erase(id);
    if (!m_panes.empty() || !m_stream) return;
    if (kill && !m_ended) {
        std::string reason;
        AfterglowMux::Reached reached = AfterglowMux::connectAny(m_sockets, &reason);
        AfterglowProto::IpcConnection::Error callError;
        if (!reached.connection
            || !reached.connection->call(AfterglowMux::Target, AfterglowMux::Fn::KillSession, Json::Array{m_session.toStdString()},
                                         AfterglowMux::CallTimeoutMs, &callError)) {
            const std::string why = !reached.connection ? (reason.empty() ? std::string("no daemon answers") : reason) : callError.message;
            qCWarning(lcMux) << "cannot end session" << m_session << QString::fromStdString(why);
        }
    }
    // The input that waits goes before the window detaches.
    m_stream->flush(200);
    delete m_readNotifier;
    delete m_writeNotifier;
    m_readNotifier = m_writeNotifier = nullptr;
    m_stream.reset();
}

bool MuxClient::sendInput(std::uint32_t id, std::string_view bytes)
{
    if (!m_stream || m_ended) return false;
    AfterglowMux::Attach::Input input;
    input.pane = id;
    // Large input goes in parts that fit the window of the channel.
    constexpr std::size_t Part = 64 * 1024;
    std::vector<std::string> frames;
    for (std::size_t at = 0; at < bytes.size() || frames.empty(); at += Part) {
        input.bytes.assign(bytes.substr(at, Part));
        frames.push_back(AfterglowMux::Attach::encode(input));
        if (at + Part >= bytes.size()) break;
    }
    std::size_t total = 0;
    for (const auto& f : frames) total += f.size();
    if (m_waitingBytes + total > MaxWaitingInput) return false;
    for (auto& f : frames) {
        m_waitingBytes += f.size();
        m_waitingInput.push_back(std::move(f));
    }
    sendWaitingInput();
    return true;
}

void MuxClient::sendResize(std::uint32_t id, const AfterglowMux::PaneSize& size)
{
    if (!m_stream || m_ended) return;
    AfterglowMux::Attach::Input input;
    input.type = AfterglowMux::Attach::InputType::Resize;
    input.pane = id;
    input.size = size;
    // In order with the input: the program sees the keys of the old size first.
    std::string frame = AfterglowMux::Attach::encode(input);
    m_waitingBytes += frame.size();
    m_waitingInput.push_back(std::move(frame));
    sendWaitingInput();
}

void MuxClient::sendWaitingInput()
{
    while (m_stream && m_input && !m_waitingInput.empty()) {
        if (m_input->tryWrite(m_waitingInput.front()) != WriteResult::Written) break;
        m_waitingBytes -= m_waitingInput.front().size();
        m_waitingInput.pop_front();
    }
    updateNotifiers();
}

QString MuxClient::foregroundJob() const
{
    std::string reason;
    AfterglowMux::Reached reached = AfterglowMux::connectAny(m_sockets, &reason);
    if (!reached.connection) return {};
    AfterglowProto::IpcConnection::Error callError;
    const auto list = reached.connection->call(AfterglowMux::Target, AfterglowMux::Fn::ListSessions, {},
                                               AfterglowMux::CallTimeoutMs, &callError);
    if (!list) return {};
    for (const Json::Value& item : list->toArray()) {
        const auto info = AfterglowMux::sessionInfo(item);
        if (info && QString::fromStdString(info->name) == m_session && !info->jobs.empty()) {
            return QString::fromStdString(info->jobs.front());
        }
    }
    return {};
}

void MuxClient::readable()
{
    if (!m_stream) return;
    const bool open = m_stream->readable();
    handleFrames();
    if (!open) lost();
    updateNotifiers();
}

void MuxClient::writable()
{
    if (!m_stream) return;
    if (!m_stream->writable()) lost();
    updateNotifiers();
}

void MuxClient::updateNotifiers()
{
    if (m_writeNotifier) m_writeNotifier->setEnabled(m_stream && m_stream->wantsWrite());
}

void MuxClient::handleFrames()
{
    if (!m_stream) return;
    std::string frame;
    if (PhosphorChannel::StreamReader* events = m_stream->reader(AfterglowMux::Attach::ControlChannel)) {
        while (events->read(frame) == ReadResult::Frame) {
            const auto event = AfterglowMux::Attach::decodeEvent(frame);
            if (!event) continue;
            using AfterglowMux::Attach::EventType;
            switch (event->type) {
            case EventType::PaneOpened:
                m_paneIds.push_back(event->pane);
                break;
            case EventType::PaneClosed:
                if (const auto it = m_panes.find(event->pane); it != m_panes.end() && it->second) {
                    it->second->sessionPaneExited(event->status);
                }
                break;
            case EventType::SessionEnded:
            case EventType::TakenOver:
            case EventType::Detached: {
                m_ended = true;
                // A copy: a pane that goes releases itself from the map.
                const std::vector<QPointer<Pane>> panes = shownPanes();
                for (const QPointer<Pane>& pane : panes) {
                    if (pane) pane->sessionLost(event->type == EventType::TakenOver);
                }
                break;
            }
            }
        }
    }
    for (const auto& [id, pane] : m_panes) {
        PhosphorChannel::StreamReader* state = m_stream ? m_stream->reader(id) : nullptr;
        if (!state || !pane) continue;
        while (state->read(frame) == ReadResult::Frame) {
            if (!pane->applySessionFrame(frame)) {
                // The replica no longer matches what the daemon sends: the
                // window cannot show the session any more, and detaches.
                qCWarning(lcMux) << "session" << m_session << "sent a frame that pane" << id << "cannot apply";
                lost();
                return;
            }
        }
    }
}

void MuxClient::lost()
{
    if (m_ended) return;
    // The daemon ended, or the stream broke: the session may still run, the
    // window no longer shows it.
    m_ended = true;
    qCWarning(lcMux) << "the connection to session" << m_session << "ended"
                     << (m_stream ? QString::fromStdString(m_stream->error()) : QString());
    const std::vector<QPointer<Pane>> panes = shownPanes();
    for (const QPointer<Pane>& pane : panes) {
        if (pane) pane->sessionLost(false);
    }
}

std::vector<QPointer<Pane>> MuxClient::shownPanes() const
{
    std::vector<QPointer<Pane>> panes;
    for (const auto& [id, pane] : m_panes) panes.push_back(pane);
    return panes;
}

} // namespace Afterglow
