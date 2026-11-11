// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mux/MuxClient.h"

#include "Pane.h"

#include "MuxConnect.h"
#include "MuxStream.h"

#include <AfterglowProto/Text.h>
#include <AfterglowVt/StateSync.h>
#include <PhosphorChannel/Stream.h>

#include <QFile>
#include <QLoggingCategory>
#include <QSocketNotifier>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

Q_LOGGING_CATEGORY(lcMux, "afterglow.mux")

namespace Afterglow {

namespace Json = AfterglowProto::Json;
namespace Attach = AfterglowMux::Attach;
using PhosphorChannel::ReadResult;
using PhosphorChannel::WriteResult;

namespace {

/// How long a window that lets a session go waits for its input to leave.
constexpr int ReleaseWaitMs = 200;

/// Connects to the attach socket @p path that the daemon named. It must be
/// in the directory of one of the daemon's sockets, which only the user
/// opens: another path could be anyone's.
int connectTo(const std::string& path, const AfterglowProto::MuxSockets& sockets, QString* error)
{
    bool inDirectory = false;
    for (const AfterglowProto::MuxSocket& socket : sockets.candidates) {
        inDirectory = inDirectory || (path.size() > socket.directory.size() + 1 && path.starts_with(socket.directory)
                                      && path[socket.directory.size()] == '/'
                                      && path.find('/', socket.directory.size() + 1) == std::string::npos);
    }
    if (!inDirectory) {
        *error = QStringLiteral("the daemon named an attach socket outside its directory: '%1'").arg(QString::fromStdString(path));
        return -1;
    }
    if (const std::string reason = AfterglowProto::socketPathError(path); !reason.empty()) {
        *error = QString::fromStdString(reason);
        return -1;
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        *error = QString::fromUtf8(std::strerror(errno));
        return -1;
    }
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    int result;
    do {
        result = ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        *error = QStringLiteral("cannot connect to %1: %2").arg(QString::fromStdString(path), QString::fromUtf8(std::strerror(errno)));
        ::close(fd);
        return -1;
    }
    return fd;
}

QString lossText(MuxClient::Loss loss, const QString& name)
{
    switch (loss) {
    case MuxClient::Loss::Ended:
        return QStringLiteral("session '%1' ended").arg(name);
    case MuxClient::Loss::Detached:
        return QStringLiteral("session '%1' was detached").arg(name);
    case MuxClient::Loss::TakenOver:
        return QStringLiteral("another window took session '%1'").arg(name);
    case MuxClient::Loss::Broken:
        break;
    }
    return QStringLiteral("the connection to session '%1' broke").arg(name);
}

} // namespace

std::shared_ptr<MuxClient> MuxClient::attach(const QString& name, bool takeover, QString* error,
                                              const AfterglowProto::MuxSockets& sockets, std::size_t eventQueue,
                                              std::size_t history)
{
    const auto fail = [error](const QString& text) -> std::shared_ptr<MuxClient> {
        if (error) *error = text;
        return nullptr;
    };
    if (sockets.candidates.empty()) return fail(QString::fromStdString(sockets.error));
    std::string reason;
    AfterglowMux::Reached reached = AfterglowMux::connectAny(sockets, &reason);
    if (!reached.connection) {
        return fail(reason.empty() ? QStringLiteral("no mux daemon runs, so there is no session '%1'").arg(name)
                                   : QString::fromStdString(AfterglowProto::printable(reason)));
    }
    AfterglowProto::IpcConnection::Error callError;
    const auto result = reached.connection->call(AfterglowMux::Target, AfterglowMux::Fn::Attach,
                                                 Json::Array{AfterglowMux::ProtocolVersion, name.toStdString(),
                                                             Json::Object{{AfterglowMux::AttachOption::Takeover, takeover},
                                                                          {AfterglowMux::AttachOption::EventQueue, eventQueue},
                                                                          {AfterglowMux::AttachOption::History,
                                                                           std::min<std::size_t>(history, std::size_t(AfterglowMux::MaxHistory))}}},
                                                 AfterglowMux::CallTimeoutMs, &callError);
    // The daemon's words, which the user reads in a terminal.
    if (!result) return fail(QString::fromStdString(AfterglowProto::printable(callError.message)));
    QString connectError;
    const int fd = connectTo((*result)["socket"].toString(), sockets, &connectError);
    if (fd < 0) return fail(QStringLiteral("cannot show session '%1': %2").arg(name, connectError));

    std::shared_ptr<MuxClient> client(new MuxClient);
    client->m_session = name;
    client->m_eventQueue = eventQueue;
    client->m_sockets = sockets;
    PhosphorChannel::Stream::Options options;
    options.maxChannelId = Attach::MaxChannelId;
    client->m_stream = std::make_unique<PhosphorChannel::Stream>(fd, fd, options);
    std::string streamError;
    client->m_input = client->m_stream->openWriter(Attach::ControlChannel, Attach::InputCodec, &streamError);
    if (client->m_input) client->m_actions = client->m_stream->openWriter(Attach::ActionChannel, Attach::ActionCodec, &streamError);
    if (!client->m_input || !client->m_actions) return fail(QString::fromStdString(streamError));

    // The daemon names the panes first: the window shows them once it knows them.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(Attach::ConnectWaitMs);
    while (client->m_paneIds.empty() && !client->m_ended) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return fail(QStringLiteral("session '%1' did not answer").arg(name));
        // The event that ends the stream comes before its end: read it first.
        const bool open = client->m_stream->pump(int(left.count()));
        client->handleFrames();
        if (client->m_ended) break;
        if (!open) {
            const std::string& why = client->m_stream->error();
            return fail(why.empty() ? QStringLiteral("session '%1' closed the connection").arg(name)
                                    : QStringLiteral("session '%1' closed the connection: %2").arg(name, QString::fromStdString(AfterglowProto::printable(why))));
        }
    }
    if (client->m_ended) return fail(lossText(client->m_loss, name));

    client->m_readNotifier = new QSocketNotifier(client->m_stream->readFd(), QSocketNotifier::Read, client.get());
    connect(client->m_readNotifier, &QSocketNotifier::activated, client.get(), &MuxClient::readable);
    client->m_writeNotifier = new QSocketNotifier(client->m_stream->writeFd(), QSocketNotifier::Write, client.get());
    connect(client->m_writeNotifier, &QSocketNotifier::activated, client.get(), &MuxClient::writable);
    client->m_stream->onWritable = [raw = client.get()](PhosphorChannel::StreamWriter&) { raw->sendWaitingInput(); };
    client->updateNotifiers();
    return client;
}

MuxClient::~MuxClient() = default;

bool MuxClient::hostEvent(Attach::Event& event, bool* lostLink)
{
    const auto held = m_hostParts.find(event.pane);
    const std::size_t heldBytes = held == m_hostParts.end() ? 0 : held->second.size();
    if (heldBytes + event.payload.size() > AfterglowMux::Host::MaxCall) return false;
    if (event.status == Attach::HostPart) {
        m_hostParts[event.pane] += event.payload;
        return true;
    }
    std::string encoded;
    if (held == m_hostParts.end()) {
        encoded = std::move(event.payload);
    } else {
        encoded = std::move(held->second) + event.payload;
        m_hostParts.erase(held);
    }
    auto call = AfterglowMux::Host::decode(encoded);
    if (!call) return false;
    // After the frames that the daemon sent before the call: a bell after the
    // text that rang it.
    if (m_panes.contains(event.pane) && !readState(event.pane)) {
        *lostLink = true;
        return true;
    }
    return hostCall(event.pane, std::move(*call), encoded.size());
}

bool MuxClient::hostCall(std::uint32_t id, AfterglowMux::Host::Call call, std::size_t bytes)
{
    const auto it = m_panes.find(id);
    if (it != m_panes.end()) {
        if (it->second) it->second->applySessionHostCall(call);
        return true;
    }
    // The window makes the pane after the daemon names it: the call waits.
    if (m_waitingHostBytes + bytes > MaxWaitingHostCalls) return false;
    WaitingHostCalls& waiting = m_waitingHostCalls[id];
    waiting.calls.push_back(std::move(call));
    waiting.bytes += bytes;
    m_waitingHostBytes += bytes;
    return true;
}

void MuxClient::bind(std::uint32_t id, Pane* pane)
{
    m_panes[id] = pane;
    if (auto waiting = m_waitingHostCalls.extract(id); !waiting.empty()) {
        m_waitingHostBytes -= waiting.mapped().bytes;
        // A session that ended closes the pane at once: its prompts would
        // answer nobody.
        if (pane && !m_ended) {
            for (const auto& call : waiting.mapped().calls) pane->applySessionHostCall(call);
        }
    }
    // Frames that came before the pane existed wait in the stream's reader.
    handleFrames();
    // A pane that the session lost before it existed closes at once, unless
    // half of its exit came: the rest closes it as a program's exit.
    if (m_ended && pane && !m_exits.contains(id) && !m_closedChannels.contains(id)) pane->sessionLost(m_loss);
}

void MuxClient::release(std::uint32_t id, bool kill)
{
    m_panes.erase(id);
    if (!m_panes.empty() || !m_stream) return;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ReleaseWaitMs);
    const auto left = [&deadline] {
        return int(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
    };
    if (kill && showing()) {
        // The session ends: the input that waits is for nothing. The kill
        // goes on a channel of its own, which the daemon reads also while
        // input waits for a full PTY there.
        m_waitingInput.clear();
        m_waitingBytes.store(0, std::memory_order_relaxed);
        Attach::Input input;
        input.type = Attach::InputType::Kill;
        const std::string frame = Attach::encode(input);
        WriteResult result = m_actions->tryWrite(frame);
        while (result == WriteResult::WouldBlock && left() > 0 && m_stream->open()) {
            // Only before the daemon granted the channel, at the start.
            if (!m_stream->pump(left())) break;
            result = m_actions->tryWrite(frame);
        }
        // Mux.onClose said kill: a session that keeps running is worth a word.
        if (result != WriteResult::Written) {
            qCWarning(lcMux) << "cannot end session" << m_session << "as Mux.onClose says: the daemon took no kill"
                             << QString::fromStdString(m_stream->error());
        }
    }
    // The input that waits goes before the window lets the session go, for a
    // short time: the GUI thread waits here. What a slow program or daemon
    // has no room for by then is lost, as the input of a closed window is.
    if (showing()) sendWaitingInput();
    while (showing() && !m_waitingInput.empty() && left() > 0 && m_stream->open()) {
        if (!m_stream->pump(left())) break;
        sendWaitingInput();
    }
    if (showing() && left() > 0) m_stream->flush(left());
    delete m_readNotifier;
    delete m_writeNotifier;
    m_readNotifier = m_writeNotifier = nullptr;
    m_stream.reset();
    m_input = m_actions = nullptr;
    m_waitingInput.clear();
    m_waitingBytes.store(0, std::memory_order_relaxed);
}

bool MuxClient::sendInput(std::uint32_t id, std::string_view bytes)
{
    if (!showing()) return false;
    Attach::Input input;
    input.pane = id;
    // Large input goes in parts that fit the window of the channel.
    constexpr std::size_t Part = 64 * 1024;
    std::vector<std::string> frames;
    for (std::size_t at = 0; at < bytes.size() || frames.empty(); at += Part) {
        input.bytes.assign(bytes.substr(at, Part));
        frames.push_back(Attach::encode(input));
        if (at + Part >= bytes.size()) break;
    }
    std::size_t total = 0;
    for (const auto& f : frames) total += f.size();
    if (m_waitingBytes.load(std::memory_order_relaxed) + total > MaxWaitingInput) return false;
    for (auto& f : frames) queueInput(std::move(f), false);
    sendWaitingInput();
    return true;
}

void MuxClient::queueInput(std::string frame, bool resize)
{
    m_waitingBytes.fetch_add(frame.size(), std::memory_order_relaxed);
    m_waitingInput.push_back(std::move(frame));
    if (!resize) m_waitingResizeOf.reset();
}

void MuxClient::sendResize(std::uint32_t id, const AfterglowMux::PaneSize& size)
{
    if (!showing()) return;
    Attach::Input input;
    input.type = Attach::InputType::Resize;
    input.pane = id;
    input.size = size;
    std::string frame = Attach::encode(input);
    // In order with the input: the program sees the keys of the old size
    // first. A size that still waits with nothing after it is old: the new
    // one takes its place.
    if (m_waitingResizeOf == id && !m_waitingInput.empty()) {
        std::string& last = m_waitingInput.back();
        m_waitingBytes.fetch_sub(last.size(), std::memory_order_relaxed);
        m_waitingBytes.fetch_add(frame.size(), std::memory_order_relaxed);
        last = std::move(frame);
    } else {
        queueInput(std::move(frame), true);
        m_waitingResizeOf = id;
    }
    sendWaitingInput();
}

void MuxClient::sendReset(std::uint32_t id)
{
    Attach::Input input;
    input.type = Attach::InputType::Reset;
    sendPaneInput(id, input);
}

void MuxClient::sendClear(std::uint32_t id)
{
    Attach::Input input;
    input.type = Attach::InputType::Clear;
    sendPaneInput(id, input);
}

void MuxClient::sendVisible(std::uint32_t id, bool visible)
{
    Attach::Input input;
    input.type = Attach::InputType::Visible;
    input.visible = visible;
    sendPaneInput(id, input);
}

void MuxClient::sendPaneInput(std::uint32_t id, Attach::Input& input)
{
    if (!showing()) return;
    input.pane = id;
    queueInput(Attach::encode(input), false);
    sendWaitingInput();
}

void MuxClient::sendWaitingInput()
{
    while (m_stream && m_input && !m_waitingInput.empty()) {
        if (m_input->tryWrite(m_waitingInput.front()) != WriteResult::Written) break;
        m_waitingBytes.fetch_sub(m_waitingInput.front().size(), std::memory_order_relaxed);
        m_waitingInput.pop_front();
    }
    if (m_waitingInput.empty()) m_waitingResizeOf.reset();
    updateNotifiers();
}

std::optional<Json::Value> MuxClient::callDaemon(const char* fn, Json::Array args) const
{
    // A session that ended can have a new one of its name already.
    if (!showing()) return std::nullopt;
    std::string reason;
    AfterglowMux::Reached reached = AfterglowMux::connectAny(m_sockets, &reason);
    if (!reached.connection) {
        qCDebug(lcMux) << "no daemon of session" << m_session << "answers" << fn << ":" << QString::fromStdString(reason);
        return std::nullopt;
    }
    AfterglowProto::IpcConnection::Error callError;
    auto result = reached.connection->call(AfterglowMux::Target, fn, std::move(args), AfterglowMux::CallTimeoutMs, &callError);
    if (!result) {
        qCDebug(lcMux) << "the daemon of session" << m_session << "refused" << fn << ":"
                       << QString::fromStdString(callError.code) << QString::fromStdString(callError.message);
    }
    return result;
}

QString MuxClient::foregroundJob() const
{
    const auto list = callDaemon(AfterglowMux::Fn::ListSessions, {});
    if (!list) return {};
    for (const Json::Value& item : list->toArray()) {
        const auto info = AfterglowMux::sessionInfo(item);
        if (info && QString::fromStdString(info->name) == m_session && !info->jobs.empty()) {
            return QString::fromStdString(info->jobs.front());
        }
    }
    return {};
}

QString MuxClient::directory(std::uint32_t id) const
{
    const auto result = callDaemon(AfterglowMux::Fn::PaneDirectory, Json::Array{m_session.toStdString(), std::int64_t(id)});
    if (!result || !result->isString()) return {};
    return QFile::decodeName(QByteArray::fromStdString(result->toString()));
}

bool MuxClient::waitForHostCalls(int waitMs, const std::function<bool()>& done)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    while (m_stream && !m_ended && !done()) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        const bool open = m_stream->pump(int(left.count()));
        handleFrames();
        if (!open) {
            lost();
            break;
        }
    }
    updateNotifiers();
    return done();
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
    if (m_writeNotifier) m_writeNotifier->setEnabled(m_stream && m_stream->open() && m_stream->wantsWrite());
}

void MuxClient::handleFrames()
{
    if (!m_stream) return;
    std::string frame;
    std::optional<Loss> ending;
    if (PhosphorChannel::StreamReader* events = m_stream->reader(Attach::ControlChannel)) {
        if (events->codec() != Attach::EventCodec) {
            qCWarning(lcMux) << "session" << m_session << "sends events of another kind:" << QString::fromStdString(events->codec());
            lost();
            return;
        }
        while (events->read(frame) == ReadResult::Frame) {
            auto event = Attach::decodeEvent(frame);
            bool bad = !event;
            if (event && event->type == Attach::EventType::PaneOpened) {
                for (const auto& [pane, channel] : m_channels) bad = bad || pane == event->pane || channel == event->channel;
                if (!bad) {
                    m_channels[event->pane] = event->channel;
                    m_paneIds.push_back(event->pane);
                }
            } else if (event && event->type == Attach::EventType::PaneClosed) {
                bad = !m_channels.contains(event->pane) || m_exits.contains(event->pane);
                if (!bad) {
                    m_exits[event->pane] = event->status;
                    paneExited(event->pane);
                }
            } else if (event && event->type == Attach::EventType::Host) {
                bad = !m_channels.contains(event->pane);
                bool lostLink = false;
                if (!bad) bad = !hostEvent(*event, &lostLink);
                // A frame of the pane did not apply.
                if (lostLink) return;
            } else if (event) {
                ending = event->type == Attach::EventType::SessionEnded ? Loss::Ended
                    : event->type == Attach::EventType::TakenOver      ? Loss::TakenOver
                                                                       : Loss::Detached;
            }
            if (bad) {
                qCWarning(lcMux) << "session" << m_session << "sent an event that the window cannot take";
                lost();
                return;
            }
        }
    }
    // A copy: a pane whose program exited leaves the map.
    const std::map<std::uint32_t, std::uint32_t> channels = m_channels;
    for (const auto& [id, channel] : channels) {
        if (m_panes.contains(id) && !readState(id)) return;
    }
    if (ending) end(*ending);
    updateNotifiers();
}

bool MuxClient::readState(std::uint32_t id)
{
    PhosphorChannel::StreamReader* state = m_stream->reader(m_channels[id]);
    if (!state) return true;
    if (state->codec() != AfterglowVt::StateCodec) {
        qCWarning(lcMux) << "session" << m_session << "sends the state of pane" << id << "in another kind:"
                         << QString::fromStdString(state->codec());
        lost();
        return false;
    }
    const QPointer<Pane> pane = m_panes[id];
    std::string frame;
    while (true) {
        const ReadResult result = state->read(frame);
        if (result == ReadResult::Frame) {
            // The replica no longer matches what the daemon sends: the
            // window cannot show the session any more (Pane says why).
            if (pane && !pane->applySessionFrame(frame)) {
                lost();
                return false;
            }
            continue;
        }
        if (result == ReadResult::Closed) {
            m_closedChannels.insert(id);
            paneExited(id);
        } else if (result != ReadResult::Empty) {
            lost();
            return false;
        }
        return true;
    }
}

void MuxClient::paneExited(std::uint32_t id)
{
    const auto exit = m_exits.find(id);
    if (exit == m_exits.end() || !m_closedChannels.contains(id)) return;
    const int status = exit->second;
    m_exits.erase(exit);
    m_closedChannels.erase(id);
    m_channels.erase(id);
    // No more of its calls come: the parts of one that did not end go.
    m_hostParts.erase(id);
    if (const auto it = m_panes.find(id); it != m_panes.end() && it->second) it->second->sessionPaneExited(status);
}

void MuxClient::end(Loss loss)
{
    if (!m_ended) {
        m_ended = true;
        m_loss = loss;
    }
    // No pane takes the calls that wait any more (bind()).
    m_waitingHostCalls.clear();
    m_waitingHostBytes = 0;
    m_hostParts.clear();
    // A pane whose exit is half known waits for the rest: its last frame or
    // its status still comes, and it closes as a pane whose program
    // exited. A copy: a pane that goes releases itself from the map.
    const std::map<std::uint32_t, QPointer<Pane>> panes = m_panes;
    for (const auto& [id, pane] : panes) {
        if (!pane || m_exits.contains(id) || m_closedChannels.contains(id)) continue;
        pane->sessionLost(m_loss);
    }
}

void MuxClient::lost()
{
    // The daemon ended, or the stream broke or ended after the end event:
    // the session may still run, the window no longer shows it. Nothing
    // more comes, so the exits that wait for their other half end too.
    if (!m_ended) {
        qCWarning(lcMux) << "the connection to session" << m_session << "ended"
                         << (m_stream ? QString::fromStdString(m_stream->error()) : QString());
    }
    // The stream stays readable at its end: the notifier would fire on each
    // pass of the event loop until the last pane goes.
    if (m_readNotifier) m_readNotifier->setEnabled(false);
    if (m_writeNotifier) m_writeNotifier->setEnabled(false);
    m_exits.clear();
    m_closedChannels.clear();
    end(Loss::Broken);
}

} // namespace Afterglow
