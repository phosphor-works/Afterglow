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
#include <QJsonDocument>
#include <QJsonObject>
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

bool sameSize(const AfterglowMux::PaneSize& a, const AfterglowMux::PaneSize& b)
{
    return a.cols == b.cols && a.rows == b.rows && a.cellWidth == b.cellWidth && a.cellHeight == b.cellHeight;
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
    history = std::min<std::size_t>(history, std::size_t(AfterglowMux::MaxHistory));
    AfterglowProto::IpcConnection::Error callError;
    const auto result = reached.connection->call(AfterglowMux::Target, AfterglowMux::Fn::Attach,
                                                 Json::Array{AfterglowMux::ProtocolVersion, name.toStdString(),
                                                             Json::Object{{AfterglowMux::AttachOption::Takeover, takeover},
                                                                          {AfterglowMux::AttachOption::EventQueue, eventQueue},
                                                                          {AfterglowMux::AttachOption::History, history}}},
                                                 AfterglowMux::CallTimeoutMs, &callError);
    // The daemon's words, which the user reads in a terminal.
    if (!result) return fail(QString::fromStdString(AfterglowProto::printable(callError.message)));
    std::string environmentError;
    auto environment = AfterglowMux::environmentFrom((*result)["environment"], &environmentError);
    if (!environment) {
        return fail(QStringLiteral("the mux daemon named no environment of session '%1': %2")
                        .arg(name, QString::fromStdString(AfterglowProto::printable(environmentError))));
    }
    const AfterglowProto::Json::Value& ambiguous = (*result)["ambiguousWide"];
    if (!ambiguous.isBool()) return fail(QStringLiteral("the mux daemon did not say how session '%1' measures ambiguous characters").arg(name));
    // The connection last: a refusal after it would leak it, and the stream
    // that closes it takes it at once.
    QString connectError;
    const int fd = connectTo((*result)["socket"].toString(), sockets, &connectError);
    if (fd < 0) return fail(QStringLiteral("cannot show session '%1': %2").arg(name, connectError));

    std::shared_ptr<MuxClient> client(new MuxClient);
    client->m_session = name;
    client->m_eventQueue = eventQueue;
    client->m_history = history;
    client->m_sockets = sockets;
    client->m_environment = std::move(*environment);
    client->m_ambiguousWide = ambiguous.toBool();
    PhosphorChannel::Stream::Options options;
    options.maxChannelId = Attach::MaxChannelId;
    // The daemon writes the events and one channel for each pane, and a
    // session has at most MaxSessionPanes. A channel counts until the window
    // read it to Closed, so every channel is read to its end, also of a pane
    // that the window let go. The daemon can make the window hold this many
    // windows of frames.
    options.maxChannels = AfterglowMux::MaxSessionPanes + 1;
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
        const bool open = client->m_stream->pump(int(left.count())) && client->m_stream->open();
        client->handleFrames();
        // An end event, or an event that the window cannot take. A stream
        // that ended says why below.
        if (client->m_ended && (open || client->m_loss != Loss::Broken)) break;
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
    // A pane that the window let go answers nothing: its calls go, and so
    // do the parts of one. One that went to another session still answers
    // its program.
    const bool moved = m_movedOut.contains(event.pane);
    if (m_released.contains(event.pane) && !moved) {
        m_hostParts.erase(event.pane);
        return true;
    }
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
    if (moved) {
        answerMoved(event.pane, *call);
        return true;
    }
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
    if (m_waitingHostBytes + bytes > MaxWaitingHostCalls && m_holdingCalls) {
        // A refresh holds them while the daemon sends more: the panes answer
        // those whose frames came now, rather than the link going.
        m_holdingCalls = false;
        std::vector<std::uint32_t> waiting;
        for (const auto& [pane, calls] : m_waitingHostCalls) waiting.push_back(pane);
        for (const std::uint32_t pane : waiting) releaseHostCalls(pane);
    }
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
    if (m_holdingCalls) return;
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

void MuxClient::answerMoved(std::uint32_t id, const AfterglowMux::Host::Call& call)
{
    // The daemon gives the bytes to the pane in its new session.
    if (const std::string answer = AfterglowMux::Host::answerWithoutWindow(call); !answer.empty() && !sendInput(id, answer)) {
        qCDebug(lcMux) << "session" << m_session << "takes no answer for pane" << id << "of another session";
    }
}

void MuxClient::bind(std::uint32_t id, Pane& pane)
{
    m_panes[id] = &pane;
    m_unbound.erase(id);
    // A session that ended closes the pane at once: its prompts would
    // answer nobody. A pane whose exit is half known gets its last calls.
    const bool exiting = m_exits.contains(id) || m_closedChannels.contains(id);
    if (m_ended && !exiting) {
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
    if (m_ended && (!halfExit || m_streamGone)) {
        if (halfExit) {
            m_exits.erase(id);
            m_closedChannels.erase(id);
            if (auto waiting = m_waitingHostCalls.extract(id); !waiting.empty()) m_waitingHostBytes -= waiting.mapped().bytes;
        }
        if (pane.sessionLost(m_loss, !m_lossShown)) m_lossShown = true;
    }
}

void MuxClient::letGo(std::uint32_t id)
{
    if (auto waiting = m_waitingHostCalls.extract(id); !waiting.empty()) m_waitingHostBytes -= waiting.mapped().bytes;
    m_hostParts.erase(id);
    if (m_closedChannels.contains(id)) {
        // Its channel ended. Its exit came whole, and nothing more of it
        // comes, or its PaneClosed still comes, and paneExited forgets it.
        if (!m_channels.contains(id)) m_closedChannels.erase(id);
        return;
    }
    // Its frames still come until the daemon closes its channel: after a
    // ClosePane, or a break out of the session. Also for a pane whose
    // PaneOpened did not come yet: a split that closes at once. Once the
    // stream is gone, nothing more comes.
    if (m_stream && !m_streamGone) m_released.insert(id);
}

void MuxClient::release(std::uint32_t id, MuxRelease release)
{
    // A pane that moved out of the session (breakOut) left already.
    const bool bound = m_panes.erase(id) > 0;
    if (bound) letGo(id);
    m_sentSizes.erase(id);
    if (bound && release == MuxRelease::Pane && showing()) {
        Attach::Input input;
        input.type = Attach::InputType::ClosePane;
        input.pane = id;
        sendAction(input);
    }
    m_killOnEnd = m_killOnEnd || release == MuxRelease::Session;
    if (!m_panes.empty() || !m_stream) return;
    const bool kill = m_killOnEnd;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ReleaseWaitMs);
    const auto left = [&deadline] {
        return int(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
    };
    bool killQueued = false;
    if (kill && showing()) {
        // The session ends: the input that waits is for nothing. The kill
        // goes on a channel of its own, which the daemon reads also while
        // input waits for a full PTY there.
        m_waitingInput.clear();
        m_waitingActions.clear();
        {
            std::lock_guard lock(m_waitingMutex);
            m_waitingBytes.clear();
        }
        Attach::Input input;
        input.type = Attach::InputType::Kill;
        const std::string frame = Attach::encode(input);
        WriteResult result = m_actions->tryWrite(frame);
        while (result == WriteResult::WouldBlock && left() > 0 && m_stream->open()) {
            // Only before the daemon granted the channel, at the start.
            if (!m_stream->pump(left())) break;
            result = m_actions->tryWrite(frame);
        }
        killQueued = result == WriteResult::Written;
        // Mux.onClose said kill: a session that keeps running is worth a word.
        if (!killQueued) {
            qCWarning(lcMux) << "cannot end session" << m_session << "as Mux.onClose says: the daemon took no kill"
                             << QString::fromStdString(m_stream->error());
        }
    }
    // The layout goes first: the next window shows the tabs and splits of
    // this one, and the input that a slow program holds back can be lost.
    const auto layout = std::find_if(m_waitingInput.begin(), m_waitingInput.end(), [](const WaitingInput& w) { return w.pane == 0; });
    if (layout != m_waitingInput.end()) std::rotate(m_waitingInput.begin(), layout, std::next(layout));
    // The input that waits goes before the window lets the session go, for a
    // short time: the GUI thread waits here. What a slow program or daemon
    // has no room for by then is lost, as the input of a closed window is.
    if (showing()) sendWaitingInput();
    while (showing() && (!m_waitingInput.empty() || !m_waitingActions.empty()) && left() > 0 && m_stream->open()) {
        if (!m_stream->pump(left())) break;
        sendWaitingInput();
    }
    // Queued is not sent: the kill reached the socket only once the stream
    // flushed.
    const bool flushed = showing() && left() > 0 && m_stream->flush(left());
    if (killQueued && !flushed) {
        qCWarning(lcMux) << "cannot end session" << m_session << "as Mux.onClose says: the kill did not reach the daemon"
                         << QString::fromStdString(m_stream->error());
    }
    delete m_readNotifier;
    delete m_writeNotifier;
    m_readNotifier = m_writeNotifier = nullptr;
    m_stream.reset();
    m_input = m_actions = nullptr;
    m_waitingInput.clear();
    m_waitingActions.clear();
    std::lock_guard lock(m_waitingMutex);
    m_waitingBytes.clear();
}

std::optional<std::uint32_t> MuxClient::newPane(const AfterglowMux::Launch& launch, const AfterglowMux::PaneSize& size, QString* error)
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
    const auto pane = std::uint32_t(*id);
    // A pane ID that the window has: two panes would take one's frames.
    if (m_panes.contains(pane) || m_unbound.contains(pane) || m_released.contains(pane) || m_closedChannels.contains(pane)) {
        if (error) *error = QStringLiteral("the mux daemon gave a pane that the window has already");
        return std::nullopt;
    }
    // The window binds it before the stream runs again: its PaneOpened can
    // come now.
    m_unbound.insert(pane);
    m_sentSizes[pane] = size;
    return pane;
}

std::optional<QString> MuxClient::breakOut(const AfterglowMux::Layout& layout, QString* error)
{
    const auto result = callDaemon(AfterglowMux::Fn::BreakOut,
                                   Json::Array{AfterglowMux::ProtocolVersion, m_session.toStdString(), AfterglowMux::toJson(layout)}, error);
    if (!result) return std::nullopt;
    const std::string& name = (*result)["name"].toString();
    if (!AfterglowMux::validSessionName(name)) {
        if (error) *error = QStringLiteral("the mux daemon gave a session name that is not valid");
        return std::nullopt;
    }
    // The panes are the new session's: their channels here close, and
    // PaneClosed comes, which is no exit of their programs. Their window
    // closes them, after the stream ran again. The calls of theirs that
    // left the daemon before it moved them wait for no frame here: their
    // programs get the answers of a pane that no window shows, in the
    // order of these calls. The daemon answered the later calls, which did
    // not leave it, at the move: those answers can reach a program before
    // these, which go through the window's input.
    for (const std::uint32_t id : layout.panes()) {
        m_sentSizes.erase(id);
        if (m_panes.erase(id) == 0) continue;
        std::deque<WaitingHostCall> calls;
        if (auto waiting = m_waitingHostCalls.extract(id); !waiting.empty()) {
            m_waitingHostBytes -= waiting.mapped().bytes;
            calls = std::move(waiting.mapped().calls);
        }
        // Its exit came whole: no more of it comes.
        const bool open = !m_closedChannels.contains(id);
        if (open) {
            // The parts of a call stay: its last part may still come.
            auto parts = m_hostParts.extract(id);
            letGo(id);
            if (!parts.empty()) m_hostParts.insert(std::move(parts));
            if (m_released.contains(id)) m_movedOut.insert(id);
        } else {
            letGo(id);
        }
        for (const WaitingHostCall& waiting : calls) answerMoved(id, waiting.call);
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
    // it reads the input that came before it. A layout that still waits is
    // old: the daemon keeps only the newest.
    std::erase_if(m_waitingInput, [](const WaitingInput& w) { return w.pane == 0; });
    queueInput(Attach::encode(input), 0);
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
    if (pendingInput(id) + total > MaxWaitingInput) return false;
    for (auto& f : frames) queueInput(std::move(f), id);
    sendWaitingInput();
    return true;
}

void MuxClient::queueInput(std::string frame, std::uint32_t id)
{
    countWaiting(id, frame.size(), false);
    m_waitingInput.push_back(WaitingInput{std::move(frame), id});
    m_waitingResizeOf.reset();
}

void MuxClient::countWaiting(std::uint32_t id, std::size_t bytes, bool remove)
{
    // The layout is the window's, and counts against no pane.
    if (id == 0) return;
    std::lock_guard lock(m_waitingMutex);
    std::size_t& waiting = m_waitingBytes[id];
    waiting = remove ? waiting - std::min(waiting, bytes) : waiting + bytes;
    if (waiting == 0) m_waitingBytes.erase(id);
}

std::size_t MuxClient::pendingInput(std::uint32_t id) const
{
    std::lock_guard lock(m_waitingMutex);
    const auto it = m_waitingBytes.find(id);
    return it == m_waitingBytes.end() ? 0 : it->second;
}

void MuxClient::sendResize(std::uint32_t id, const AfterglowMux::PaneSize& size)
{
    if (!showing()) return;
    // The daemon has the pane at that size: the one that newPane started
    // it with, or the last one sent.
    if (const auto sent = m_sentSizes.find(id); sent != m_sentSizes.end() && sameSize(sent->second, size)) return;
    m_sentSizes[id] = size;
    Attach::Input input;
    input.type = Attach::InputType::Resize;
    input.pane = id;
    input.size = size;
    std::string frame = Attach::encode(input);
    // In order with the input: the program sees the keys of the old size
    // first. A size that still waits with nothing after it is old: the new
    // one takes its place.
    if (m_waitingResizeOf == id && !m_waitingInput.empty()) {
        std::string& last = m_waitingInput.back().frame;
        countWaiting(id, last.size(), true);
        countWaiting(id, frame.size(), false);
        last = std::move(frame);
    } else {
        queueInput(std::move(frame), id);
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
    input.pane = id;
    input.visible = visible;
    sendAction(input);
}

void MuxClient::sendPaneInput(std::uint32_t id, Attach::Input& input)
{
    if (!showing()) return;
    input.pane = id;
    queueInput(Attach::encode(input), id);
    sendWaitingInput();
}

void MuxClient::sendAction(const Attach::Input& input)
{
    if (!showing()) return;
    m_waitingActions.push_back(Attach::encode(input));
    sendWaitingInput();
}

void MuxClient::answerControlCalls()
{
    m_controlCallsQueued = false;
    while (!m_controlCalls.empty()) {
        const ControlCall call = std::move(m_controlCalls.front());
        m_controlCalls.pop_front();
        ControlAnswer answer;
        if (m_controlHandler) answer = m_controlHandler(call.fn, call.args);
        else answer.error = QStringLiteral("this window answers no control calls");
        // The answer goes after the layout that the call changed: the daemon
        // reads the input in its order.
        if (call.id == 0 || !showing()) continue;
        QJsonObject reply{{QStringLiteral("id"), call.id}};
        if (answer.error.isEmpty()) reply.insert(QStringLiteral("result"), answer.result);
        else reply.insert(QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), QStringLiteral("CALL_FAILED")}, {QStringLiteral("message"), answer.error}});
        Attach::Input input;
        input.type = Attach::InputType::ControlReply;
        input.bytes = QJsonDocument(reply).toJson(QJsonDocument::Compact).toStdString();
        queueInput(Attach::encode(input), false);
        sendWaitingInput();
    }
}

void MuxClient::sendWaitingInput()
{
    // A frame larger than the daemon's window never fits: kept, it would
    // hold every frame behind it for good.
    while (m_stream && m_actions && !m_waitingActions.empty()) {
        const WriteResult result = m_actions->tryWrite(m_waitingActions.front());
        if (result == WriteResult::TooLarge) {
            qCWarning(lcMux) << "session" << m_session << "takes no action of" << m_waitingActions.front().size() << "bytes";
        } else if (result != WriteResult::Written) {
            break;
        }
        m_waitingActions.pop_front();
    }
    while (m_stream && m_input && !m_waitingInput.empty()) {
        WaitingInput& next = m_waitingInput.front();
        const WriteResult result = m_input->tryWrite(next.frame);
        if (result == WriteResult::TooLarge) {
            qCWarning(lcMux) << "session" << m_session << "takes no input frame of" << next.frame.size() << "bytes"
                             << (next.pane == 0 ? "(a layout)" : "");
        } else if (result != WriteResult::Written) {
            break;
        }
        countWaiting(next.pane, next.frame.size(), true);
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
    return paneJobs({id}).front();
}

std::vector<QString> MuxClient::paneJobs(const std::vector<std::uint32_t>& ids) const
{
    std::vector<QString> jobs(ids.size());
    if (ids.empty() || !showing()) return jobs;
    std::string reason;
    AfterglowMux::Reached reached = AfterglowMux::connectAny(m_sockets, &reason);
    if (!reached.connection) {
        qCDebug(lcMux) << "no daemon of session" << m_session << "answers" << AfterglowMux::Fn::PaneJob << ":" << QString::fromStdString(reason);
        return jobs;
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        AfterglowProto::IpcConnection::Error callError;
        const auto result = reached.connection->call(AfterglowMux::Target, AfterglowMux::Fn::PaneJob,
                                                     Json::Array{m_session.toStdString(), std::int64_t(ids[i])}, AfterglowMux::CallTimeoutMs,
                                                     &callError);
        if (result && result->isString()) jobs[i] = QString::fromStdString(result->toString());
        if (result) continue;
        qCDebug(lcMux) << "the daemon of session" << m_session << "refused" << AfterglowMux::Fn::PaneJob << ":"
                       << QString::fromStdString(callError.code) << QString::fromStdString(callError.message);
        // No code: the connection failed or the answer did not come, and the
        // next call would wait as long. A refusal is of that pane alone.
        if (callError.code.empty()) break;
    }
    return jobs;
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
        // A stream that no longer works ends the link in handleFrames.
        m_stream->pump(int(left.count()));
        handleFrames();
    }
    updateNotifiers();
    return done();
}

std::uint64_t MuxClient::askRefresh(std::uint32_t id)
{
    if (!showing() || !m_panes.contains(id) || !m_channels.contains(id) || m_closedChannels.contains(id)) return 0;
    Attach::Input input;
    input.type = Attach::InputType::Refresh;
    input.pane = id;
    sendAction(input);
    return ++m_refreshes[id].asked;
}

bool MuxClient::waitRefreshed(const std::map<std::uint32_t, std::uint64_t>& tickets, int waitMs)
{
    if (!showing()) return false;
    const auto done = [&] {
        for (const auto& [id, ticket] : tickets) {
            // Its last frame came: nothing newer comes.
            if (!m_channels.contains(id) || m_closedChannels.contains(id)) continue;
            const auto refreshes = m_refreshes.find(id);
            const auto read = m_framesRead.find(id);
            if (refreshes == m_refreshes.end() || refreshes->second.answered < ticket || read == m_framesRead.end()
                || read->second < refreshes->second.frames) {
                return false;
            }
        }
        return true;
    };
    const bool holding = std::exchange(m_holdingCalls, true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    while (m_stream && !m_ended && !done()) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) break;
        // A stream that no longer works ends the link in handleFrames. The
        // Refresh may wait for room in the channel.
        m_stream->pump(int(left.count()));
        sendWaitingInput();
        handleFrames();
    }
    m_holdingCalls = holding;
    const bool refreshed = showing() && done();
    if (!refreshed) qCDebug(lcMux) << "the panes of session" << m_session << "did not refresh in" << waitMs << "ms";
    updateNotifiers();
    // The calls that came meanwhile, after the read.
    if (!holding && !m_waitingHostCalls.empty()) {
        QMetaObject::invokeMethod(this, [this] {
            std::vector<std::uint32_t> waiting;
            for (const auto& [id, calls] : m_waitingHostCalls) waiting.push_back(id);
            for (const std::uint32_t id : waiting) releaseHostCalls(id);
        }, Qt::QueuedConnection);
    }
    return refreshed;
}

void MuxClient::tellRefreshed()
{
    for (auto& [id, refreshes] : m_refreshes) {
        if (refreshes.told == refreshes.answered) continue;
        const auto read = m_framesRead.find(id);
        if ((read == m_framesRead.end() ? 0 : read->second) < refreshes.frames) continue;
        refreshes.told = refreshes.answered;
        if (const auto pane = m_panes.find(id); pane != m_panes.end() && pane->second) pane->second->sessionRefreshed(refreshes.told);
    }
}

void MuxClient::readable()
{
    m_readQueued = false;
    if (!m_stream || m_streamGone) return;
    // A stream that no longer works ends the link in handleFrames, after
    // what it read before.
    m_stream->readable();
    handleFrames();
}

void MuxClient::writable()
{
    if (!m_stream || m_streamGone) return;
    // A failed write: what the readers hold still applies, and
    // handleFrames then ends the link.
    if (!m_stream->writable()) handleFrames();
    updateNotifiers();
}

void MuxClient::updateNotifiers()
{
    if (m_writeNotifier) m_writeNotifier->setEnabled(m_stream && m_stream->open() && m_stream->wantsWrite());
    // A write of the window's own that failed leaves readFd() quiet: the
    // stream says so in the next readable(), which nothing else would call.
    if (m_readNotifier && m_stream && !m_streamGone && !m_stream->open() && !std::exchange(m_readQueued, true))
        QMetaObject::invokeMethod(this, &MuxClient::readable, Qt::QueuedConnection);
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
                std::optional<AfterglowMux::Layout> layout;
                if (m_layoutCame) error = "it came a second time";
                else if (const auto json = Json::parse(event->payload, &error)) layout = AfterglowMux::layoutFrom(*json, &error);
                bad = !layout;
                if (layout) {
                    m_layout = std::move(*layout);
                    m_layoutCame = true;
                    const std::vector<std::uint32_t> panes = m_layout.panes();
                    m_unbound.insert(panes.begin(), panes.end());
                    // The window lays it out as it came: no need to send it back.
                    m_sentLayout = Json::write(AfterglowMux::toJson(m_layout));
                } else {
                    qCWarning(lcMux) << "session" << m_session << "sent a layout that the window cannot take:" << QString::fromStdString(error);
                }
            } else if (event && event->type == Attach::EventType::Control) {
                QJsonParseError error{};
                const QJsonObject call = QJsonDocument::fromJson(QByteArray::fromStdString(event->payload), &error).object();
                const QJsonValue id = call.value(QLatin1String("id"));
                bad = error.error != QJsonParseError::NoError || !id.isDouble() || id.toInteger(-1) < 0 || !call.value(QLatin1String("fn")).isString()
                    || !call.value(QLatin1String("args")).isArray();
                if (!bad) {
                    // The pane that the daemon started for `afterglow ctl split`
                    // or `new-tab` is the window's from now: its PaneOpened
                    // comes behind this event, before the call below runs.
                    const qint64 placed = call.value(QLatin1String("args")).toArray().at(0).toInteger(-1);
                    if (call.value(QLatin1String("fn")).toString() == QLatin1String("placePane") && placed > 0 && placed <= 0xffffffffLL) {
                        const auto pane = std::uint32_t(placed);
                        if (!m_panes.contains(pane) && !m_released.contains(pane) && !m_closedChannels.contains(pane)) m_unbound.insert(pane);
                    }
                    m_controlCalls.push_back({id.toInteger(), call.value(QLatin1String("fn")).toString(), call.value(QLatin1String("args")).toArray()});
                    if (!std::exchange(m_controlCallsQueued, true)) QMetaObject::invokeMethod(this, &MuxClient::answerControlCalls, Qt::QueuedConnection);
                }
            } else if (event && event->type == Attach::EventType::PaneOpened) {
                bad = bad || !m_layoutCame;
                // A channel read to Closed is free: the daemon may open it again.
                for (const auto& [pane, channel] : m_channels)
                    bad = bad || pane == event->pane || (channel == event->channel && !m_closedChannels.contains(pane));
                if (!bad) {
                    m_channels[event->pane] = event->channel;
                    // Nobody's pane: a newPane that timed out after the daemon
                    // started it. Its program ends, as the window said that it
                    // did not start, and its frames and calls go nowhere.
                    if (!m_panes.contains(event->pane) && !m_unbound.contains(event->pane) && !m_released.contains(event->pane)) {
                        qCWarning(lcMux) << "session" << m_session << "opened pane" << event->pane << "that the window did not ask for: it ends";
                        m_released.insert(event->pane);
                        Attach::Input close;
                        close.type = Attach::InputType::ClosePane;
                        close.pane = event->pane;
                        sendAction(close);
                    }
                }
            } else if (event && event->type == Attach::EventType::PaneClosed) {
                bad = !m_channels.contains(event->pane) || m_exits.contains(event->pane);
                if (!bad) {
                    m_exits[event->pane] = event->status;
                    paneExited(event->pane);
                }
            } else if (event && event->type == Attach::EventType::Refreshed) {
                // In the order that the window asked: never more answers than
                // questions.
                Refreshes* refreshes = m_channels.contains(event->pane) ? &m_refreshes[event->pane] : nullptr;
                bad = !refreshes || refreshes->answered >= refreshes->asked;
                if (!bad) {
                    ++refreshes->answered;
                    refreshes->frames = std::max(refreshes->frames, event->frames);
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
    // A pane whose program exited leaves the map while it is read: the
    // next one is the first past its ID.
    for (auto it = m_channels.begin(); it != m_channels.end();) {
        const std::uint32_t id = it->first;
        if ((m_panes.contains(id) || m_released.contains(id)) && !readState(id)) return;
        it = m_channels.upper_bound(id);
    }
    tellRefreshed();
    if (ending) end(*ending);
    // The input ended or the stream failed, also in a write of the window's
    // own: what it read before was handled, and nothing more comes.
    if (m_stream && !m_stream->open()) lost();
    updateNotifiers();
}

bool MuxClient::readState(std::uint32_t id)
{
    // Read to its end: the daemon may have opened the ID again for another
    // pane, whose frames this one would take.
    const auto channel = m_channels.find(id);
    if (channel == m_channels.end() || m_closedChannels.contains(id)) return true;
    PhosphorChannel::StreamReader* state = m_stream->reader(channel->second);
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
        // Closed also when the stream ended or failed before the channel
        // did: handleFrames then ends the link, which forgets the half exits
        // of the panes that the window made.
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
    m_channels.erase(id);
    m_released.erase(id);
    m_movedOut.erase(id);
    m_sentSizes.erase(id);
    // No more of its calls come: the parts of one that did not end go. Its
    // last frame came, so no whole call waits.
    m_hostParts.erase(id);
    if (auto waiting = m_waitingHostCalls.extract(id); !waiting.empty()) m_waitingHostBytes -= waiting.mapped().bytes;
    m_framesRead.erase(id);
    const auto it = m_panes.find(id);
    // Its last frame came: a Refresh that waits has all that it gets.
    if (const auto refreshes = m_refreshes.extract(id); !refreshes.empty() && it != m_panes.end() && it->second) {
        if (refreshes.mapped().asked != refreshes.mapped().told) it->second->sessionRefreshed(refreshes.mapped().asked);
    }
    // A pane that the window let go is forgotten. A bound one keeps its
    // closed channel until the window lets it go (letGo).
    if (it == m_panes.end()) {
        m_closedChannels.erase(id);
        return;
    }
    if (it->second) it->second->sessionPaneExited(status);
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
        // A pane that closed already says nothing, and the next one says why.
        if (pane->sessionLost(m_loss, !m_lossShown)) m_lossShown = true;
    }
}

void MuxClient::lost()
{
    // Once: what came before it was handled then.
    if (m_streamGone) return;
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
