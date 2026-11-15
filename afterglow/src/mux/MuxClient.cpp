// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "mux/MuxClient.h"

#include "Pane.h"
#include "mux/MuxSessions.h"

#include "MuxConnect.h"
#include "MuxStream.h"

#include <AfterglowProto/Text.h>
#include <AfterglowVt/StateSync.h>
#include <PhosphorChannel/Stream.h>

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QScopeGuard>
#include <QSocketNotifier>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <system_error>
#include <thread>

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

/// The threads of MuxClient::CallThread. Never freed: a thread that runs on
/// past waitForCallThreads() still reaches it while the statics go.
struct CallThreads
{
    std::mutex mutex;
    std::condition_variable ended;
    int running = 0;
};

CallThreads& callThreads()
{
    static auto* const threads = new CallThreads;
    return *threads;
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
    case MuxClient::Loss::MovedOut:
        return QStringLiteral("a pane of session '%1' went to another session").arg(name);
    case MuxClient::Loss::Broken:
        break;
    }
    return QStringLiteral("the connection to session '%1' broke").arg(name);
}

/// Lets @p proxy go off the calling thread, which may be the GUI thread.
/// Once its connection closed, ssh ends by itself, but with a host that does
/// not answer that can take long, and ~ProxyProcess waits up to two seconds
/// for it, then ends it.
void reapLater(std::shared_ptr<AfterglowMux::ProxyProcess> proxy)
{
    // Another owner lets it go later.
    if (!proxy || proxy.use_count() > 1) return;
    AfterglowMux::endInBackground(std::move(proxy));
}

/// Closes the connection of @p reached, and lets its proxy go (reapLater).
void closeReached(AfterglowMux::Reached& reached)
{
    reached.connection.reset();
    reapLater(std::move(reached.proxy));
}

bool sameSize(const AfterglowMux::PaneSize& a, const AfterglowMux::PaneSize& b)
{
    return a.cols == b.cols && a.rows == b.rows && a.cellWidth == b.cellWidth && a.cellHeight == b.cellHeight;
}

} // namespace

SessionLink& SessionLink::operator=(SessionLink&& other) noexcept
{
    if (this == &other) return *this;
    if (fd >= 0) ::close(fd);
    fd = std::exchange(other.fd, -1);
    // Its connection closed: the old proxy ends by itself.
    reapLater(std::exchange(proxy, std::move(other.proxy)));
    environment = std::move(other.environment);
    window = other.window;
    route = std::move(other.route);
    return *this;
}

SessionLink::~SessionLink()
{
    if (fd >= 0) ::close(fd);
    reapLater(std::move(proxy));
}

std::optional<SessionLink> attachSession(const QString& name, bool takeover, QString* error, const AfterglowMux::Route& route,
                                         std::size_t eventQueue, std::size_t history, int signInWaitMs, const AfterglowMux::Environment* environment)
{
    const auto fail = [error](const QString& text) -> std::optional<SessionLink> {
        if (error) *error = text;
        return std::nullopt;
    };
    if (route.proxy.empty() && route.sockets.candidates.empty()) return fail(QString::fromStdString(route.sockets.error));
    std::string reason;
    // A copy of the environment for a caller off the GUI thread, which
    // changes it.
    AfterglowMux::Reached reached = environment ? AfterglowMux::reach(route, false, signInWaitMs, *environment, &reason)
                                                : AfterglowMux::reach(route, false, signInWaitMs, &reason);
    // However this ends: a link that it makes holds the proxy too.
    const auto reap = qScopeGuard([&reached] { closeReached(reached); });
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
    if (!result) return fail(QString::fromStdString(callFailure(callError, AfterglowMux::Fn::Attach)));
    std::string environmentError;
    auto sessionEnvironment = AfterglowMux::environmentFrom((*result)["environment"], &environmentError);
    if (!sessionEnvironment) {
        return fail(QStringLiteral("the mux daemon named no environment of session '%1': %2")
                        .arg(name, QString::fromStdString(AfterglowProto::printable(environmentError))));
    }
    const auto window = (*result)["window"].toInteger();
    if (!window || *window <= 0) return fail(QStringLiteral("the mux daemon gave no window for session '%1'").arg(name));
    // The connection of the call carries the session's stream from here.
    AfterglowProto::IpcConnection::Error releaseError;
    SessionLink link;
    link.proxy = reached.proxy;
    link.environment = std::move(*sessionEnvironment);
    link.window = std::uint64_t(*window);
    link.route = route;
    link.fd = reached.connection->release(&releaseError);
    if (link.fd < 0) return fail(QStringLiteral("cannot show session '%1': %2").arg(name, QString::fromStdString(releaseError.message)));
    return link;
}

std::shared_ptr<MuxClient> MuxClient::attach(const QString& name, bool takeover, QString* error, const AfterglowMux::Route& route,
                                              std::size_t eventQueue, std::size_t history, int signInWaitMs)
{
    auto link = attachSession(name, takeover, error, route, eventQueue, history, signInWaitMs);
    return link ? adopt(name, std::move(*link), error, eventQueue) : nullptr;
}

std::shared_ptr<MuxClient> MuxClient::adopt(const QString& name, SessionLink link, QString* error, std::size_t eventQueue)
{
    const auto fail = [error](const QString& text) -> std::shared_ptr<MuxClient> {
        if (error) *error = text;
        return nullptr;
    };
    std::shared_ptr<MuxClient> client(new MuxClient);
    client->m_session = name;
    client->m_eventQueue = eventQueue;
    client->m_route = std::move(link.route);
    client->m_proxy = link.proxy;
    client->m_environment = std::move(link.environment);
    client->m_window = link.window;
    const int fd = std::exchange(link.fd, -1);
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
    PhosphorChannel::StreamWriter* commands =
        client->m_actions ? client->m_stream->openWriter(Attach::CommandsChannel, Attach::CommandsCodec, &streamError) : nullptr;
    if (!client->m_input || !client->m_actions || !commands) return fail(QString::fromStdString(streamError));
    client->m_commands = std::make_unique<MuxCommands>(name, commands, [raw = client.get()] { raw->updateNotifiers(); });
    // The programs of another host get what an ssh session of that host
    // gets: the commands that a script marks remote.
    if (client->remote()) client->m_commands->setRemoteHost(client->host());

    // The daemon sends the layout first, as soon as the stream starts: the
    // window lays out the session's tabs and splits, and makes their panes.
    // The GUI thread waits for it here, at most Attach::StreamStartWaitMs.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(Attach::StreamStartWaitMs);
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
    if (client->m_proxy && client->m_proxy->errorFd() >= 0) {
        MuxClient* const raw = client.get();
        raw->m_errorNotifier = new QSocketNotifier(raw->m_proxy->errorFd(), QSocketNotifier::Read, raw);
        connect(raw->m_errorNotifier, &QSocketNotifier::activated, raw, [raw] {
            if (raw->m_proxy && raw->m_proxy->drainError()) return;
            // The pipe ended: nothing more comes, and it stays readable.
            raw->m_errorNotifier->setEnabled(false);
            raw->m_errorNotifier->deleteLater();
            raw->m_errorNotifier = nullptr;
        });
    }
    client->updateNotifiers();
    return client;
}

MuxClient::~MuxClient()
{
    {
        // The calls that still run drop their answers from here.
        const std::lock_guard lock(m_mailbox->mutex);
        m_mailbox->client = nullptr;
    }
    // Their callers hear it from the event loop: this object goes while its
    // window, or a pane of it, goes.
    for (auto& waiting : std::exchange(m_waitingCalls, {})) {
        if (!waiting.second.abandon || !QCoreApplication::instance()) continue;
        QMetaObject::invokeMethod(
            QCoreApplication::instance(),
            [abandon = std::move(waiting.second.abandon), session = m_session] {
                abandon(QStringLiteral("session '%1' no longer shows in this window").arg(session));
            },
            Qt::QueuedConnection);
    }
    // The relays use the stream's channel, and the proxy ends after the
    // stream, off the GUI thread, which watches its pipe no more.
    m_commands.reset();
    m_stream.reset();
    delete m_errorNotifier;
    m_errorNotifier = nullptr;
    reapLater(std::move(m_proxy));
}

void MuxClient::setCommandsHost(MuxCommands::Host host)
{
    if (!m_commands) return;
    // The panes that the window bound, also one that bind() reads the
    // frames of before the window's splits hold it.
    host.pane = [this, shown = std::move(host.pane)](std::uint32_t id) -> std::optional<quint32> {
        if (const auto bound = m_panes.find(id); bound != m_panes.end()) {
            if (!bound->second) return std::nullopt;
            return bound->second->id();
        }
        return shown ? shown(id) : std::nullopt;
    };
    // A pane that the window makes next (a split of `afterglow ctl`, whose
    // program can run `aglow run` before it binds): the connections of its
    // clients wait until bind().
    host.binding = [this](std::uint32_t id) { return m_unbound.contains(id); };
    m_commands->setHost(std::move(host));
}

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

void MuxClient::releaseHostCalls(std::uint32_t id, bool evenHeld)
{
    if (m_holdingCalls && !evenHeld) return;
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
    // The clients of its program that waited for it.
    if (m_commands) m_commands->bound(id);
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
    m_maybeMovedOut.erase(id);
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
    // A pane that moved out of the session (breakOutLater) left already.
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
        m_waitingReplyBytes = 0;
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
    const auto layout = std::find_if(m_waitingInput.begin(), m_waitingInput.end(), [](const WaitingInput& w) { return w.layout; });
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
    // Before the stream, which holds their channel: the clients' runs end,
    // and the daemon tells them why.
    if (m_commands) m_commands->closeAll();
    m_stream.reset();
    // Its stream's connection closed: the proxy ends by itself.
    delete m_errorNotifier;
    m_errorNotifier = nullptr;
    reapLater(std::move(m_proxy));
    m_input = m_actions = nullptr;
    m_waitingInput.clear();
    m_waitingReplyBytes = 0;
    m_waitingActions.clear();
    std::lock_guard lock(m_waitingMutex);
    m_waitingBytes.clear();
}

std::optional<std::uint32_t> MuxClient::newPane(const AfterglowMux::Launch& launch, const AfterglowMux::PaneSize& size, QString* error)
{
    // The daemon waits for the program to start, also behind other starts.
    const auto result = callDaemon(AfterglowMux::Fn::NewPane,
                                   Json::Array{AfterglowMux::ProtocolVersion, m_session.toStdString(), AfterglowMux::toJson(launch),
                                               AfterglowMux::toJson(size), std::int64_t(m_window)},
                                   error, AfterglowMux::SpawnCallTimeoutMs);
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

std::optional<Json::Array> MuxClient::breakOutArguments(const AfterglowMux::Layout& layout, QString* error) const
{
    // The layout goes in a request, which has a limit of its own: the
    // values of a tab full of control characters, which JSON escapes, can
    // pass it. The commands go first, then the titles, which a project
    // saves but the panes do without. A command that does not go is one
    // that the daemon keeps (PaneValues::keepsCommand): it has the one of
    // the last layout, and an entry that keeps it still goes.
    AfterglowMux::Layout sent = layout;
    const auto arguments = [&] {
        return Json::Array{AfterglowMux::ProtocolVersion, m_session.toStdString(), AfterglowMux::toJson(sent), std::int64_t(m_window)};
    };
    const auto dropEmpty = [&sent] {
        std::erase_if(sent.values, [](const auto& entry) { return entry.second.empty() && !entry.second.keepsCommand; });
    };
    Json::Array args = arguments();
    if (!AfterglowProto::IpcConnection::fits(AfterglowMux::Target, AfterglowMux::Fn::BreakOut, args)) {
        for (auto& [id, values] : sent.values) {
            if (values.command.empty()) continue;
            values.command.clear();
            values.keepsCommand = true;
        }
        dropEmpty();
        args = arguments();
    }
    if (!AfterglowProto::IpcConnection::fits(AfterglowMux::Target, AfterglowMux::Fn::BreakOut, args)) {
        for (auto& [id, values] : sent.values) values.title.clear();
        dropEmpty();
        for (AfterglowMux::LayoutTab& tab : sent.tabs) tab.title.clear();
        args = arguments();
    }
    if (!AfterglowProto::IpcConnection::fits(AfterglowMux::Target, AfterglowMux::Fn::BreakOut, args)) {
        if (error) *error = QStringLiteral("the tab is too large to move to another session");
        return std::nullopt;
    }
    return args;
}

QString MuxClient::breakOutLater(const AfterglowMux::Layout& layout, BrokenOut done)
{
    QString error;
    auto args = breakOutArguments(layout, &error);
    if (!args) return error;
    WaitingCall call;
    call.finish = [this, layout, done](DaemonAnswer answer) {
        for (const std::uint32_t id : layout.panes()) {
            if (const auto it = m_leaving.find(id); it != m_leaving.end()) m_leaving.erase(it);
        }
        QString why = answer.error;
        const std::optional<QString> name = brokeOut(layout, answer, &why);
        // An exit that came while the call ran: a pane that moved is let go
        // and forgotten, and one that did not hears it now.
        for (const std::uint32_t id : layout.panes()) paneExited(id);
        done(name, name ? QString() : why);
    };
    call.abandon = [done](const QString& why) { done(std::nullopt, why); };
    const auto work = [sent = std::move(*args)](const AfterglowMux::Route& route, const QString& session,
                                                const AfterglowMux::Environment& environment) {
        return callRoute(route, session, AfterglowMux::Fn::BreakOut, sent, AfterglowMux::CallTimeoutMs, environment);
    };
    if (const QString refused = callLater(work, std::move(call)); !refused.isEmpty()) return refused;
    // The answer comes from the event loop, after this.
    for (const std::uint32_t id : layout.panes()) m_leaving.insert(id);
    return {};
}

std::optional<QString> MuxClient::brokeOut(const AfterglowMux::Layout& layout, const DaemonAnswer& answer, QString* error)
{
    if (!answer.result) {
        // The daemon may have moved them: their PaneClosed then says -1, and
        // the window must not show it as the exit of their programs.
        if (answer.unanswered) {
            for (const std::uint32_t id : layout.panes()) {
                if (m_panes.contains(id)) m_maybeMovedOut.insert(id);
            }
            m_movedOutShown = false;
        }
        if (error) *error = answer.error;
        return std::nullopt;
    }
    const std::string& name = (*answer.result)["name"].toString();
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

void MuxClient::sendSettings(const AfterglowMux::SessionSettings& settings)
{
    if (!showing()) return;
    std::string json = Json::write(AfterglowMux::toJson(settings));
    if (json == m_sentSettings) return;
    Attach::Input input;
    input.type = Attach::InputType::Settings;
    input.bytes = json;
    m_sentSettings = std::move(json);
    queueInput(Attach::encode(input), 0);
    sendWaitingInput();
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
    std::erase_if(m_waitingInput, [](const WaitingInput& w) { return w.layout; });
    queueInput(Attach::encode(input), 0, true);
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

void MuxClient::queueInput(std::string frame, std::uint32_t id, bool layout, bool reply)
{
    countWaiting(id, frame.size(), false);
    if (reply) m_waitingReplyBytes += frame.size();
    m_waitingInput.push_back(WaitingInput{std::move(frame), id, layout, reply});
    m_waitingResizeOf.reset();
}

void MuxClient::countWaiting(std::uint32_t id, std::size_t bytes, bool remove)
{
    // The window's own frames count against no pane.
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
    // The pane drops the ticket of a Refresh that it asked before (Pane::
    // setSessionVisible): its next read asks a new one, also while the last
    // answer is late.
    if (const auto refreshes = m_refreshes.find(id); refreshes != m_refreshes.end()) refreshes->second.late = false;
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
        // The session no longer shows: the daemon told the clients so, and
        // a call run now would act on a window that is not the session's
        // (openProject, newWindow) or reach its panes late.
        if (!showing()) {
            m_controlCalls.clear();
            return;
        }
        const ControlCall call = std::move(m_controlCalls.front());
        m_controlCalls.pop_front();
        ControlReply reply = [self = QPointer<MuxClient>(this), id = call.id](const ControlAnswer& answer) {
            if (self) self->sendControlReply(id, answer);
        };
        if (m_controlHandler) m_controlHandler(call.fn, call.args, call.client, std::move(reply));
        else reply({{}, QStringLiteral("this window answers no control calls")});
    }
}

void MuxClient::sendControlReply(qint64 id, const ControlAnswer& answer)
{
    if (id == 0 || !showing()) return;
    // The answer goes after the layout that the call changed: the daemon
    // reads the input in its order, and the window sends its layout after a
    // delay, which a call does not wait for.
    if (m_flushLayout) m_flushLayout();
    QJsonObject reply{{QStringLiteral("id"), id}};
    if (answer.error.isEmpty()) reply.insert(QStringLiteral("result"), answer.result);
    else
        reply.insert(QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), answer.code.isEmpty() ? QStringLiteral("CALL_FAILED") : answer.code},
                                                          {QStringLiteral("message"), answer.error}});
    std::string json = QJsonDocument(reply).toJson(QJsonDocument::Compact).toStdString();
    const auto failed = [id](const QString& message) {
        QJsonObject error{{QStringLiteral("id"), id}};
        error.insert(QStringLiteral("error"), QJsonObject{{QStringLiteral("code"), QStringLiteral("failed")}, {QStringLiteral("message"), message}});
        return QJsonDocument(error).toJson(QJsonDocument::Compact).toStdString();
    };
    // One that does not fit a frame (a screenshot) goes in parts. One past
    // what the daemon takes would end the link there: the client gets this
    // error instead. The daemon reads no answer while the input of a pane
    // waits for its program, and the window holds those that wait up to a
    // bound, as it does the input of each pane.
    if (json.size() > Attach::MaxControlReply) {
        json = failed(QStringLiteral("the answer is larger than %1 MiB").arg(Attach::MaxControlReply >> 20));
    } else if (m_waitingReplyBytes + json.size() > MaxWaitingReplies) {
        json = failed(QStringLiteral("the window holds %1 MiB of answers that the mux daemon did not take yet").arg(MaxWaitingReplies >> 20));
    }
    for (std::size_t at = 0;;) {
        Attach::Input input;
        const bool last = json.size() - at <= Attach::ControlReplyPartBytes;
        input.type = last ? Attach::InputType::ControlReply : Attach::InputType::ControlReplyPart;
        input.bytes = json.substr(at, last ? std::string::npos : Attach::ControlReplyPartBytes);
        queueInput(Attach::encode(input), 0, false, true);
        if (last) break;
        at += Attach::ControlReplyPartBytes;
    }
    sendWaitingInput();
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
                             << (next.layout ? "(a layout)" : "");
        } else if (result != WriteResult::Written) {
            break;
        }
        countWaiting(next.pane, next.frame.size(), true);
        if (next.reply) m_waitingReplyBytes -= next.frame.size();
        m_waitingInput.pop_front();
    }
    if (m_waitingInput.empty()) m_waitingResizeOf.reset();
    if (m_stream && m_commands) m_commands->writable();
    updateNotifiers();
}

std::optional<Json::Value> MuxClient::callDaemon(const char* fn, Json::Array args, QString* error, int timeoutMs, bool* unanswered) const
{
    if (unanswered) *unanswered = false;
    // A session that ended can have a new one of its name already.
    if (!showing()) {
        if (error) *error = QStringLiteral("session '%1' no longer shows in this window").arg(m_session);
        return std::nullopt;
    }
    DaemonAnswer answer = callRoute(m_route, m_session, fn, std::move(args), timeoutMs, AfterglowMux::currentEnvironment());
    if (error && !answer.result) *error = answer.error;
    if (unanswered) *unanswered = answer.unanswered;
    return std::move(answer.result);
}

MuxClient::DaemonAnswer MuxClient::callRoute(const AfterglowMux::Route& route, const QString& session, const char* fn, Json::Array args,
                                             int timeoutMs, const AfterglowMux::Environment& environment)
{
    DaemonAnswer answer;
    std::string reason;
    // The stream's ssh signed in: a call shares its connection.
    AfterglowMux::Reached reached = AfterglowMux::reach(route, false, AfterglowMux::CallTimeoutMs, environment, &reason);
    const auto reap = qScopeGuard([&reached] { closeReached(reached); });
    if (!reached.connection) {
        qCDebug(lcMux) << "no daemon of session" << session << "answers" << fn << ":" << QString::fromStdString(reason);
        answer.error = reason.empty() ? QStringLiteral("no mux daemon runs")
                                      : QStringLiteral("the mux daemon does not answer: %1").arg(QString::fromStdString(AfterglowProto::printable(reason)));
        return answer;
    }
    AfterglowProto::IpcConnection::Error callError;
    answer.result = reached.connection->call(AfterglowMux::Target, fn, std::move(args), timeoutMs, &callError);
    if (!answer.result) {
        qCDebug(lcMux) << "the daemon of session" << session << "refused" << fn << ":"
                       << QString::fromStdString(callError.code) << QString::fromStdString(callError.message);
        // The daemon's words, which the user reads in a notice. Never empty:
        // the callers read an empty error as success.
        answer.error = QString::fromStdString(callFailure(callError, fn));
        // No code: the connection failed or the answer did not come.
        answer.unanswered = callError.code.empty();
    }
    return answer;
}

MuxClient::CallThread::CallThread()
{
    const std::lock_guard lock(callThreads().mutex);
    ++callThreads().running;
}

MuxClient::CallThread::CallThread(CallThread&& other) noexcept
    : m_counted(std::exchange(other.m_counted, false))
{
}

MuxClient::CallThread::~CallThread()
{
    if (!m_counted) return;
    CallThreads& threads = callThreads();
    const std::lock_guard lock(threads.mutex);
    if (--threads.running == 0) threads.ended.notify_all();
}

bool MuxClient::waitForCallThreads(int waitMs)
{
    CallThreads& threads = callThreads();
    std::unique_lock lock(threads.mutex);
    return threads.ended.wait_for(lock, std::chrono::milliseconds(std::max(0, waitMs)), [&threads] { return threads.running == 0; });
}

QString MuxClient::callLater(DaemonWork work, WaitingCall call)
{
    // A session that ended can have a new one of its name already.
    if (!showing()) return QStringLiteral("session '%1' no longer shows in this window").arg(m_session);
    const quint64 id = ++m_lastCall;
    try {
        // What the thread reads is its own: the route, and the environment
        // that the GUI thread changes (an activation token), taken here.
        std::thread([counted = CallThread(), mailbox = m_mailbox, id, route = m_route, session = m_session, work = std::move(work),
                     environment = AfterglowMux::currentEnvironment()]() mutable {
            // Counted to the end of the body: a quit waits for it
            // (waitForCallThreads).
            const CallThread running = std::move(counted);
            auto answer = std::make_shared<DaemonAnswer>(work(route, session, environment));
            const std::lock_guard lock(mailbox->mutex);
            if (!mailbox->client) return;
            QMetaObject::invokeMethod(
                mailbox->client, [client = mailbox->client, id, answer] { client->callAnswered(id, *answer); }, Qt::QueuedConnection);
        }).detach();
    } catch (const std::system_error& e) {
        return QStringLiteral("cannot call the mux daemon of session '%1': %2").arg(m_session, QString::fromLocal8Bit(e.what()));
    }
    m_waitingCalls.emplace(id, std::move(call));
    return {};
}

void MuxClient::callAnswered(quint64 id, DaemonAnswer& answer)
{
    auto waiting = m_waitingCalls.extract(id);
    if (waiting.empty()) return;
    waiting.mapped().finish(std::move(answer));
}

QString MuxClient::newPaneLater(const AfterglowMux::Launch& launch, const AfterglowMux::PaneSize& size, PaneStarted done,
                                std::optional<std::uint32_t> directoryOf)
{
    WaitingCall call;
    call.finish = [this, size, done](DaemonAnswer answer) {
        --m_newPanesWaiting;
        std::optional<std::uint32_t> pane;
        QString error = answer.error;
        const auto id = answer.result ? (*answer.result)["pane"].toInteger() : std::nullopt;
        if (answer.result && (!id || *id <= 0 || *id > 0xffffffffLL)) {
            error = QStringLiteral("the mux daemon gave no pane");
        } else if (id && !showing()) {
            error = QStringLiteral("session '%1' no longer shows in this window").arg(m_session);
        } else if (id) {
            const auto started = std::uint32_t(*id);
            // Its PaneOpened came first: it waited for this answer. Else a
            // pane ID that the window has: two panes would take one's frames.
            const bool opened = m_unclaimed.erase(started) > 0;
            if (!opened
                && (m_panes.contains(started) || m_unbound.contains(started) || m_released.contains(started) || m_closedChannels.contains(started))) {
                error = QStringLiteral("the mux daemon gave a pane that the window has already");
            } else {
                // The window binds it at once, from done.
                m_unbound.insert(started);
                m_sentSizes[started] = size;
                pane = started;
            }
        }
        if (m_newPanesWaiting == 0) releaseUnclaimed();
        done(pane, pane ? QString() : error);
    };
    call.abandon = [done](const QString& why) { done(std::nullopt, why); };
    const auto work = [launch, size, window = m_window, directoryOf](const AfterglowMux::Route& route, const QString& session,
                                                                      const AfterglowMux::Environment& environment) {
        AfterglowMux::Launch started = launch;
        if (directoryOf && started.workingDirectory.empty()) {
            // Without an answer, the daemon starts it in the home directory.
            const DaemonAnswer directory = callRoute(route, session, AfterglowMux::Fn::PaneDirectory,
                                                     Json::Array{session.toStdString(), std::int64_t(*directoryOf)},
                                                     AfterglowMux::CallTimeoutMs, environment);
            if (directory.result && directory.result->isString()) started.workingDirectory = directory.result->toString();
        }
        // The daemon waits for the program to start, also behind other starts.
        return callRoute(route, session, AfterglowMux::Fn::NewPane,
                         Json::Array{AfterglowMux::ProtocolVersion, session.toStdString(), AfterglowMux::toJson(started),
                                     AfterglowMux::toJson(size), std::int64_t(window)},
                         AfterglowMux::SpawnCallTimeoutMs, environment);
    };
    const QString refused = callLater(work, std::move(call));
    if (refused.isEmpty()) ++m_newPanesWaiting;
    return refused;
}

void MuxClient::endUnbound(std::uint32_t id)
{
    if (m_unbound.erase(id) == 0) return;
    m_sentSizes.erase(id);
    // Its frames still come until the daemon closes its channel, as for a
    // pane that the window let go (letGo).
    if (m_stream && !m_streamGone) m_released.insert(id);
    // host.binding no longer holds the connections of its clients.
    if (m_commands) m_commands->bound(id);
    Attach::Input close;
    close.type = Attach::InputType::ClosePane;
    close.pane = id;
    sendAction(close);
}

void MuxClient::releaseUnclaimed()
{
    // Nobody's pane: a newPaneLater whose answer did not come in time, after
    // the daemon started it. Its program ends, as the window said that it
    // did not start, and its frames and calls go nowhere.
    for (const std::uint32_t id : std::exchange(m_unclaimed, {})) {
        qCWarning(lcMux) << "session" << m_session << "opened pane" << id << "that the window did not ask for: it ends";
        m_unbound.erase(id);
        m_released.insert(id);
        // The clients of its program that waited for it find no pane.
        if (m_commands) m_commands->bound(id);
        Attach::Input close;
        close.type = Attach::InputType::ClosePane;
        close.pane = id;
        sendAction(close);
    }
}

QString MuxClient::paneJob(std::uint32_t id) const
{
    return paneJobs({id}).front().value_or(QString());
}

std::vector<MuxClient::PaneAnswer> MuxClient::askEachPane(const char* fn, const std::vector<std::uint32_t>& ids) const
{
    std::vector<PaneAnswer> answers(ids.size());
    if (ids.empty() || !showing()) return answers;
    std::string reason;
    AfterglowMux::Reached reached = AfterglowMux::reach(m_route, false, AfterglowMux::CallTimeoutMs, &reason);
    const auto reap = qScopeGuard([&reached] { closeReached(reached); });
    if (!reached.connection) {
        qCDebug(lcMux) << "no daemon of session" << m_session << "answers" << fn << ":" << QString::fromStdString(reason);
        return answers;
    }
    for (std::size_t i = 0; i < ids.size(); ++i) {
        AfterglowProto::IpcConnection::Error callError;
        auto result = reached.connection->call(AfterglowMux::Target, fn, Json::Array{m_session.toStdString(), std::int64_t(ids[i])},
                                               AfterglowMux::CallTimeoutMs, &callError);
        if (result) {
            answers[i] = {true, std::move(*result)};
            continue;
        }
        // A refusal is the answer for that pane alone: it is gone.
        answers[i].answered = !callError.code.empty();
        qCDebug(lcMux) << "the daemon of session" << m_session << "refused" << fn << ":" << QString::fromStdString(callError.code)
                       << QString::fromStdString(callError.message);
        // No code: the connection failed or the answer did not come, and the
        // next call would wait as long. A refusal is of that pane alone.
        if (callError.code.empty()) break;
    }
    return answers;
}

std::vector<std::optional<QString>> MuxClient::paneJobs(const std::vector<std::uint32_t>& ids) const
{
    std::vector<std::optional<QString>> jobs(ids.size());
    const auto answers = askEachPane(AfterglowMux::Fn::PaneJob, ids);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        // A pane that the daemon refused is gone, and runs nothing.
        if (answers[i].answered) jobs[i] = answers[i].value.isString() ? QString::fromStdString(answers[i].value.toString()) : QString();
    }
    return jobs;
}

QString MuxClient::directory(std::uint32_t id) const
{
    return directories({id}).front();
}

std::vector<QString> MuxClient::directories(const std::vector<std::uint32_t>& ids) const
{
    std::vector<QString> paths(ids.size());
    const auto answers = askEachPane(AfterglowMux::Fn::PaneDirectory, ids);
    for (std::size_t i = 0; i < ids.size(); ++i) {
        if (answers[i].value.isString()) paths[i] = QFile::decodeName(QByteArray::fromStdString(answers[i].value.toString()));
    }
    return paths;
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
    Refreshes& refreshes = m_refreshes[id];
    // Another would only wait behind it: its answer serves.
    if (refreshes.late) return refreshes.asked;
    Attach::Input input;
    input.type = Attach::InputType::Refresh;
    input.pane = id;
    sendAction(input);
    return ++refreshes.asked;
}

bool MuxClient::waitRefreshed(const std::map<std::uint32_t, std::uint64_t>& tickets, int waitMs)
{
    if (!showing()) return false;
    // Pane @p id waits for the answer of @p ticket, or for its frames.
    const auto waits = [this](std::uint32_t id, std::uint64_t ticket) {
        // Its last frame came: nothing newer comes.
        if (!m_channels.contains(id) || m_closedChannels.contains(id)) return false;
        const auto refreshes = m_refreshes.find(id);
        const auto read = m_framesRead.find(id);
        return refreshes == m_refreshes.end() || refreshes->second.answered < ticket || read == m_framesRead.end()
            || read->second < refreshes->second.frames;
    };
    const auto late = [this](std::uint32_t id) {
        const auto refreshes = m_refreshes.find(id);
        return m_refreshesLate || (refreshes != m_refreshes.end() && refreshes->second.late);
    };
    // With @p evenLate, also the panes whose answers are late, which the
    // loop below does not wait for.
    const auto done = [&](bool evenLate) {
        return std::none_of(tickets.begin(), tickets.end(),
                            [&](const auto& ticket) { return waits(ticket.first, ticket.second) && (evenLate || !late(ticket.first)); });
    };
    const bool holding = std::exchange(m_holdingCalls, true);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(waitMs);
    bool timedOut = false;
    while (m_stream && !m_ended && !done(false)) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) {
            timedOut = true;
            break;
        }
        // A stream that no longer works ends the link in handleFrames. The
        // Refresh may wait for room in the channel.
        m_stream->pump(int(left.count()));
        sendWaitingInput();
        handleFrames();
    }
    m_holdingCalls = holding;
    // A daemon that stopped, or an ssh that stalls, keeps the stream open
    // and answers nothing: the reads that follow wait for nothing until an
    // answer comes (tellRefreshed). A pane whose answer this left behind
    // asks no other meanwhile: none piles up behind it.
    if (timedOut && showing()) m_refreshesLate = true;
    if (m_refreshesLate && showing()) {
        for (const auto& [id, ticket] : tickets) {
            if (waits(id, ticket)) m_refreshes[id].late = true;
        }
    }
    const bool refreshed = showing() && done(true);
    if (!refreshed) qCDebug(lcMux) << "the panes of session" << m_session << "did not refresh in time:" << (timedOut ? "a wait ran out" : "late");
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
        // An answer came: the reads wait for answers again, and this pane
        // asks again once it heard of its last.
        m_refreshesLate = false;
        if (refreshes.told == refreshes.asked) refreshes.late = false;
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
    // lost(): writable() writes nothing more, and the notifier would fire on
    // each pass of the event loop.
    if (m_writeNotifier) m_writeNotifier->setEnabled(m_stream && !m_streamGone && m_stream->open() && m_stream->wantsWrite());
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
    // After lost() (an event that the window refused, a frame that did not
    // apply), no event applies: those behind the one that failed would act
    // on a link that is gone. The panes that bind() makes still read their
    // frames.
    PhosphorChannel::StreamReader* events = m_streamGone ? nullptr : m_stream->reader(Attach::ControlChannel);
    if (events) {
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
                    m_controlCalls.push_back({id.toInteger(), call.value(QLatin1String("fn")).toString(), call.value(QLatin1String("args")).toArray(),
                                              call.value(QLatin1String("client")).toInteger(0)});
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
                    if (!m_panes.contains(event->pane) && !m_unbound.contains(event->pane) && !m_released.contains(event->pane)
                        && m_newPanesWaiting > 0) {
                        // A newPaneLater() whose answer did not come yet may
                        // have started it.
                        m_unclaimed.insert(event->pane);
                        m_unbound.insert(event->pane);
                    } else if (!m_panes.contains(event->pane) && !m_unbound.contains(event->pane) && !m_released.contains(event->pane)) {
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
            } else if (event && event->type == Attach::EventType::Commands) {
                auto relay = Attach::decodeRelay(event->payload);
                bad = !relay || !m_commands || !m_commands->frame(std::move(*relay));
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
                switch (event->type) {
                case Attach::EventType::SessionEnded: ending = Loss::Ended; break;
                case Attach::EventType::TakenOver: ending = Loss::TakenOver; break;
                case Attach::EventType::Detached: ending = Loss::Detached; break;
                // Each has its branch above.
                case Attach::EventType::PaneOpened:
                case Attach::EventType::PaneClosed:
                case Attach::EventType::Host:
                case Attach::EventType::Layout:
                case Attach::EventType::Refreshed:
                case Attach::EventType::Control:
                case Attach::EventType::Commands: bad = true; break;
                }
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
    // It may have moved: the answer of the break out tells (breakOutLater).
    if (m_leaving.contains(id)) return;
    const auto exit = m_exits.find(id);
    if (exit == m_exits.end() || !m_closedChannels.contains(id)) return;
    const int status = exit->second;
    m_exits.erase(exit);
    m_channels.erase(id);
    m_released.erase(id);
    m_movedOut.erase(id);
    m_sentSizes.erase(id);
    // No more of its calls come: the parts of one that did not end go. Its
    // last frame came, so its calls that wait reach it now, also those that
    // a refresh holds (waitRefreshed): it closes before the hold ends. Those
    // of a pane that the window has not made go.
    m_hostParts.erase(id);
    releaseHostCalls(id, true);
    if (auto waiting = m_waitingHostCalls.extract(id); !waiting.empty()) m_waitingHostBytes -= waiting.mapped().bytes;
    m_framesRead.erase(id);
    // A breakOutLater whose answer did not come moved it, and its program runs on.
    const bool movedOut = m_maybeMovedOut.erase(id) > 0 && status == -1;
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
    if (!it->second) return;
    if (!movedOut) {
        it->second->sessionPaneExited(status);
    } else if (it->second->sessionLost(Loss::MovedOut, !m_movedOutShown)) {
        // One notice for the panes of the break out.
        m_movedOutShown = true;
    }
}

void MuxClient::end(Loss loss)
{
    if (!m_ended) {
        m_ended = true;
        m_loss = loss;
    }
    // The window serves the clients of the session no more: their runs end,
    // and the daemon tells them why.
    if (m_commands) m_commands->closeAll();
    // The calls whose frames came reach their panes, also those that a
    // refresh holds (waitRefreshed), as they would have without it.
    std::vector<std::uint32_t> held;
    for (const auto& [id, calls] : m_waitingHostCalls) held.push_back(id);
    for (const std::uint32_t id : held) releaseHostCalls(id, true);
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
