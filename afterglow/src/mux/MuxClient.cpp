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

    // The daemon sends the layout first: the window lays out the session's
    // tabs and splits, and makes their panes.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(Attach::ConnectWaitMs);
    while (!client->m_layoutCame && !client->m_ended) {
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
    // text that rang it. A frame that is still on its way holds the call.
    if (m_panes.contains(event.pane) && !readState(event.pane)) {
        *lostLink = true;
        return true;
    }
    return hostCall(event.pane, std::move(*call), encoded.size(), event.frames);
}

bool MuxClient::hostCall(std::uint32_t id, AfterglowMux::Host::Call call, std::size_t bytes, std::uint64_t frames)
{
    // It waits for its pane, which the window makes after the daemon names
    // it, and for the frames before it.
    if (m_waitingHostBytes + bytes > MaxWaitingHostCalls) return false;
    WaitingHostCalls& waiting = m_waitingHostCalls[id];
    waiting.calls.push_back(WaitingHostCall{std::move(call), frames, bytes});
    waiting.bytes += bytes;
    m_waitingHostBytes += bytes;
    releaseHostCalls(id);
    return true;
}

void MuxClient::releaseHostCalls(std::uint32_t id)
{
    const auto pane = m_panes.find(id);
    const auto it = m_waitingHostCalls.find(id);
    if (pane == m_panes.end() || it == m_waitingHostCalls.end()) return;
    const std::uint64_t read = m_framesRead[id];
    // Out of the map before the pane answers them: an answer can reach this
    // client again.
    std::vector<AfterglowMux::Host::Call> ready;
    std::deque<WaitingHostCall>& calls = it->second.calls;
    while (!calls.empty() && calls.front().frames <= read) {
        it->second.bytes -= calls.front().bytes;
        m_waitingHostBytes -= calls.front().bytes;
        ready.push_back(std::move(calls.front().call));
        calls.pop_front();
    }
    if (calls.empty()) m_waitingHostCalls.erase(it);
    const QPointer<Pane> target = pane->second;
    for (const AfterglowMux::Host::Call& call : ready) {
        // Once the session ended, the daemon reads no input: a request of the
        // program would ask the user for nothing. A terminal reset (code 0)
        // still clears a drag or a drop target of the pane.
        using Kind = AfterglowMux::Host::Kind;
        const bool answers = call.kind == Kind::Report || call.kind == Kind::ClipboardRequest
            || (call.kind == Kind::TerminalProtocol && call.number != 0);
        if (target && !(m_ended && answers)) target->applySessionHostCall(call);
    }
}

void MuxClient::bind(std::uint32_t id, Pane* pane)
{
    m_panes[id] = pane;
    // A session that ended closes the pane at once: its prompts would
    // answer nobody. A pane whose exit is half known gets its last calls.
    const bool exiting = m_exits.contains(id) || m_closedChannels.contains(id);
    if (!pane || (m_ended && !exiting)) {
        if (auto waiting = m_waitingHostCalls.extract(id); !waiting.empty()) m_waitingHostBytes -= waiting.mapped().bytes;
    }
    // Frames that came before the pane existed wait in the stream's reader,
    // and the calls that waited go after the frames before them.
    handleFrames();
    releaseHostCalls(id);
    // A pane that the session lost before it existed closes at once, unless
    // half of its exit came: the rest closes it as a program's exit. Once
    // the stream is gone, the rest that did not come by now never does.
    const bool halfExit = m_exits.contains(id) || m_closedChannels.contains(id);
    if (m_ended && pane && (!halfExit || m_streamGone)) {
        if (halfExit) {
            m_exits.erase(id);
            m_closedChannels.erase(id);
            if (auto waiting = m_waitingHostCalls.extract(id); !waiting.empty()) m_waitingHostBytes -= waiting.mapped().bytes;
        }
        pane->sessionLost(m_loss, !std::exchange(m_lossShown, true));
    }
}

void MuxClient::release(std::uint32_t id, MuxRelease release)
{
    m_panes.erase(id);
    // Its frames still come until the daemon closes its channel: after a
    // ClosePane, or a break out of the session. Also for a pane whose
    // PaneOpened did not come yet: a split that closes at once.
    if (showing() && !m_exits.contains(id)) m_released.insert(id);
    if (release == MuxRelease::Pane && showing()) {
        Attach::Input input;
        input.type = Attach::InputType::ClosePane;
        input.pane = id;
        m_waitingActions.push_back(Attach::encode(input));
        sendWaitingInput();
    }
    m_killOnEnd = m_killOnEnd || release == MuxRelease::Session;
    if (!m_panes.empty() || !m_stream) return;
    const bool kill = m_killOnEnd;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ReleaseWaitMs);
    const auto left = [&deadline] {
        return int(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
    };
    if (kill && showing()) {
        // The session ends: the input that waits is for nothing. The kill
        // goes on a channel of its own, which the daemon reads also while
        // input waits for a full PTY there.
        m_waitingInput.clear();
        m_waitingActions.clear();
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
    while (showing() && (!m_waitingInput.empty() || !m_waitingActions.empty()) && left() > 0 && m_stream->open()) {
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
    m_waitingActions.clear();
    m_waitingBytes.store(0, std::memory_order_relaxed);
}

std::optional<std::uint32_t> MuxClient::newPane(const AfterglowMux::Launch& launch, const AfterglowMux::PaneSize& size, QString* error) const
{
    const auto result = callDaemon(AfterglowMux::Fn::NewPane,
                                   Json::Array{AfterglowMux::ProtocolVersion, m_session.toStdString(), AfterglowMux::toJson(launch),
                                               AfterglowMux::toJson(size)},
                                   error);
    const auto id = result ? (*result)["pane"].toInteger() : std::nullopt;
    if (!id || *id <= 0 || *id > 0xffffffffLL) {
        if (result && error) *error = QStringLiteral("the mux daemon gave no pane");
        return std::nullopt;
    }
    return std::uint32_t(*id);
}

std::optional<QString> MuxClient::breakOut(const AfterglowMux::Layout& layout, QString* error) const
{
    const auto result = callDaemon(AfterglowMux::Fn::BreakOut,
                                   Json::Array{AfterglowMux::ProtocolVersion, m_session.toStdString(), AfterglowMux::toJson(layout)}, error);
    if (!result) return std::nullopt;
    const std::string& name = (*result)["name"].toString();
    if (!AfterglowMux::validSessionName(name)) {
        if (error) *error = QStringLiteral("the mux daemon gave a session name that is not valid");
        return std::nullopt;
    }
    return QString::fromStdString(name);
}

void MuxClient::sendLayout(const AfterglowMux::Layout& layout)
{
    if (!showing()) return;
    std::string json = Json::write(AfterglowMux::toJson(layout));
    if (json == m_sentLayout) return;
    Attach::Input input;
    input.type = Attach::InputType::Layout;
    input.bytes = json;
    m_sentLayout = std::move(json);
    // In order with the input, and not part of it: the daemon reads it as
    // it reads the input that came before it.
    queueInput(Attach::encode(input), false);
    sendWaitingInput();
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
    while (m_stream && m_actions && !m_waitingActions.empty()) {
        if (m_actions->tryWrite(m_waitingActions.front()) != WriteResult::Written) break;
        m_waitingActions.pop_front();
    }
    while (m_stream && m_input && !m_waitingInput.empty()) {
        if (m_input->tryWrite(m_waitingInput.front()) != WriteResult::Written) break;
        m_waitingBytes.fetch_sub(m_waitingInput.front().size(), std::memory_order_relaxed);
        m_waitingInput.pop_front();
    }
    if (m_waitingInput.empty()) m_waitingResizeOf.reset();
    updateNotifiers();
}

std::optional<Json::Value> MuxClient::callDaemon(const char* fn, Json::Array args, QString* error) const
{
    // A session that ended can have a new one of its name already.
    if (!showing()) {
        if (error) *error = QStringLiteral("session '%1' no longer shows in this window").arg(m_session);
        return std::nullopt;
    }
    std::string reason;
    AfterglowMux::Reached reached = AfterglowMux::connectAny(m_sockets, &reason);
    if (!reached.connection) {
        qCDebug(lcMux) << "no daemon of session" << m_session << "answers" << fn << ":" << QString::fromStdString(reason);
        if (error) *error = QStringLiteral("the mux daemon does not answer: %1").arg(QString::fromStdString(AfterglowProto::printable(reason)));
        return std::nullopt;
    }
    AfterglowProto::IpcConnection::Error callError;
    auto result = reached.connection->call(AfterglowMux::Target, fn, std::move(args), AfterglowMux::CallTimeoutMs, &callError);
    if (!result) {
        qCDebug(lcMux) << "the daemon of session" << m_session << "refused" << fn << ":"
                       << QString::fromStdString(callError.code) << QString::fromStdString(callError.message);
        // The daemon's words, which the user reads in a notice.
        if (error) *error = QString::fromStdString(AfterglowProto::printable(callError.message));
    }
    return result;
}

QString MuxClient::paneJob(std::uint32_t id) const
{
    const auto result = callDaemon(AfterglowMux::Fn::PaneJob, Json::Array{m_session.toStdString(), std::int64_t(id)});
    if (!result || !result->isString()) return {};
    return QString::fromStdString(result->toString());
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
            if (event && event->type == Attach::EventType::Layout) {
                // Once, before the panes: the window lays it out when it
                // attaches.
                std::string error;
                const auto json = Json::parse(event->payload, &error);
                auto layout = json && !m_layoutCame && m_paneIds.empty() ? AfterglowMux::layoutFrom(*json, &error) : std::nullopt;
                bad = !layout;
                if (layout) {
                    m_layout = std::move(*layout);
                    m_layoutCame = true;
                } else {
                    qCWarning(lcMux) << "session" << m_session << "sent a layout that the window cannot take:" << QString::fromStdString(error);
                }
            } else if (event && event->type == Attach::EventType::PaneOpened) {
                bad = bad || !m_layoutCame;
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
        if ((m_panes.contains(id) || m_released.contains(id)) && !readState(id)) return;
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
    // A pane that the user closed: its frames go nowhere.
    const auto bound = m_panes.find(id);
    const QPointer<Pane> pane = bound == m_panes.end() ? QPointer<Pane>() : bound->second;
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
            // The calls that the daemon sent after this frame.
            ++m_framesRead[id];
            releaseHostCalls(id);
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
    m_released.erase(id);
    // No more of its calls come: the parts of one that did not end go. Its
    // last frame came, so no whole call waits.
    m_hostParts.erase(id);
    if (auto waiting = m_waitingHostCalls.extract(id); !waiting.empty()) m_waitingHostBytes -= waiting.mapped().bytes;
    m_framesRead.erase(id);
    if (const auto it = m_panes.find(id); it != m_panes.end() && it->second) it->second->sessionPaneExited(status);
}

void MuxClient::end(Loss loss)
{
    if (!m_ended) {
        m_ended = true;
        m_loss = loss;
    }
    // The calls that wait go, but those of a pane whose exit is half known:
    // its last frames still come, and a notification after the last output
    // of a session shows with them. No other pane takes calls any more.
    for (auto it = m_waitingHostCalls.begin(); it != m_waitingHostCalls.end();) {
        // Also of a pane that the window binds later (bind()).
        const auto pane = m_panes.find(it->first);
        const bool exiting = (pane == m_panes.end() || pane->second) && (m_exits.contains(it->first) || m_closedChannels.contains(it->first));
        if (exiting) {
            ++it;
            continue;
        }
        m_waitingHostBytes -= it->second.bytes;
        it = m_waitingHostCalls.erase(it);
    }
    m_hostParts.clear();
    // A pane whose exit is half known waits for the rest: its last frame or
    // its status still comes, and it closes as a pane whose program
    // exited. A copy: a pane that goes releases itself from the map. One
    // notice says why, not one for each pane.
    const std::map<std::uint32_t, QPointer<Pane>> panes = m_panes;
    for (const auto& [id, pane] : panes) {
        if (!pane || m_exits.contains(id) || m_closedChannels.contains(id)) continue;
        pane->sessionLost(m_loss, !std::exchange(m_lossShown, true));
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
    // A pane that the window did not make yet keeps the half of its exit
    // and its calls: the rest can wait in its reader, which bind() reads.
    m_streamGone = true;
    std::erase_if(m_exits, [this](const auto& exit) { return m_panes.contains(exit.first); });
    std::erase_if(m_closedChannels, [this](std::uint32_t id) { return m_panes.contains(id); });
    end(Loss::Broken);
}

} // namespace Afterglow
