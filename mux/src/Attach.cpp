// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Attach.h"

#include "Session.h"

#include <AfterglowProto/Socket.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <iterator>

#include <sys/random.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace AfterglowMux {

using PhosphorChannel::WriteResult;

namespace {

std::string randomName()
{
    unsigned char bytes[8];
    if (::getrandom(bytes, sizeof bytes, 0) != ssize_t(sizeof bytes)) return {};
    static const char hex[] = "0123456789abcdef";
    std::string out = "attach-";
    for (const unsigned char b : bytes) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 15]);
    }
    return out + ".sock";
}

/// The serial of the last attachment: the daemon has one thread.
std::uint64_t lastSerial = 0;

// The largest Host event, a part and its header, fits a frame of the window's
// channel: its grant is the default window (MuxClient).
static_assert(Attach::HostPartBytes + 32 <= PhosphorChannel::Stream::Options{}.window - PhosphorChannel::Stream::FrameCost);

} // namespace

std::unique_ptr<Attachment> Attachment::listen(const std::string& directory, std::string session, pid_t caller,
                                                std::string* error)
{
    const auto fail = [error](std::string text) -> std::unique_ptr<Attachment> {
        if (error) *error = std::move(text);
        return nullptr;
    };
    const std::string name = randomName();
    if (name.empty()) return fail("cannot make the name of the attach socket");
    const std::string path = directory + "/" + name;
    if (path.size() > AfterglowProto::MaxSocketPathBytes) return fail("the attach socket path is too long: " + path);
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return fail(std::string("cannot make the attach socket: ") + std::strerror(errno));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.data(), path.size());
    // 0600 from the moment it exists, as the daemon's own sockets.
    const mode_t mask = ::umask(0177);
    const bool bound = ::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
    const int bindErrno = errno;
    ::umask(mask);
    if (!bound || ::listen(fd, 1) != 0) {
        const int failure = bound ? errno : bindErrno;
        ::close(fd);
        if (bound) ::unlink(path.c_str());
        return fail("cannot listen on " + path + ": " + std::strerror(failure));
    }
    std::unique_ptr<Attachment> attachment(new Attachment);
    attachment->m_listenFd = fd;
    attachment->m_session = std::move(session);
    attachment->m_path = path;
    attachment->m_caller = caller;
    attachment->m_serial = ++lastSerial;
    attachment->m_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(Attach::ConnectWaitMs);
    return attachment;
}

Attachment::~Attachment()
{
    if (m_listenFd >= 0) {
        ::close(m_listenFd);
        ::unlink(m_path.c_str());
    }
}

pollfd Attachment::pollFd() const
{
    if (!m_stream) return {m_listenFd, POLLIN, 0};
    return {m_stream->readFd(), short(POLLIN | (m_stream->wantsWrite() ? POLLOUT : 0)), 0};
}

int Attachment::timeoutMs() const
{
    if (m_stream && !m_finish) return -1;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(m_deadline - std::chrono::steady_clock::now());
    return left.count() > 0 ? int(left.count()) : 0;
}

void Attachment::accept()
{
    while (true) {
        const int fd = ::accept4(m_listenFd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            // Out of descriptors or memory: the connection stays in the
            // backlog and the socket stays readable, so the poll would spin.
            // The window hears that its connection ended.
            if (errno != EAGAIN && errno != EWOULDBLOCK) m_failed = true;
            return;
        }
        ucred peer{};
        socklen_t size = sizeof peer;
        // Only the process that made the attach call: the socket's name is
        // no secret from other processes of the user.
        if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != ::geteuid() || peer.pid != m_caller) {
            ::close(fd);
            continue;
        }
        ::close(m_listenFd);
        ::unlink(m_path.c_str());
        m_listenFd = -1;
        // The window writes its input and its layout on the control channel,
        // and its actions on the action channel (Attach::isAction).
        PhosphorChannel::Stream::Options options;
        options.maxChannels = 2;
        options.maxChannelId = Attach::ActionChannel + 1;
        m_stream = std::make_unique<PhosphorChannel::Stream>(fd, fd, options);
        std::string error;
        m_events = m_stream->openWriter(Attach::ControlChannel, Attach::EventCodec, &error);
        if (!m_events) m_failed = true;
        // A finished attachment that the window reaches only now: the
        // window still learns why the session does not show.
        if (m_finish) m_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(FinishWaitMs);
        return;
    }
}

void Attachment::sendEvent(const Attach::Event& event, std::shared_ptr<const std::string> call)
{
    const bool part = event.type == Attach::EventType::Host && event.status == Attach::HostPart;
    WaitingEvent waiting{Attach::encode(event), event.pane, std::move(call), !part};
    m_waitingBytes += waiting.encoded.size() + Host::CallCost;
    m_waitingEvents.push_back(std::move(waiting));
    flushEvents();
}

void Attachment::flushEvents()
{
    while (m_events && !m_waitingEvents.empty()) {
        // They wait for the window to grant the channel its window, at the
        // start, and while host calls fill it.
        const WriteResult result = m_events->tryWrite(m_waitingEvents.front().encoded);
        if (result == WriteResult::WouldBlock) break;
        if (result != WriteResult::Written) {
            // No event is larger than a frame (the parts of host calls): the
            // stream failed.
            m_failed = true;
            return;
        }
        m_waitingBytes -= m_waitingEvents.front().encoded.size() + Host::CallCost;
        m_partSent = m_waitingEvents.front().last ? nullptr : m_waitingEvents.front().call;
        m_waitingEvents.pop_front();
    }
}

void Attachment::sendHostCalls(const PaneLink& link, MuxPane& pane, std::size_t budget)
{
    if (m_waitingBytes >= m_eventQueue) return;
    for (std::string& encoded : pane.takeHostCalls(m_serial, std::min(budget, m_eventQueue - m_waitingBytes))) {
        auto call = std::make_shared<const std::string>(std::move(encoded));
        Attach::Event host;
        host.type = Attach::EventType::Host;
        host.pane = pane.id();
        host.frames = link.frames;
        for (std::size_t at = 0; at < call->size() || at == 0; at += Attach::HostPartBytes) {
            const bool last = call->size() - at <= Attach::HostPartBytes;
            host.payload.assign(*call, at, Attach::HostPartBytes);
            host.status = last ? -1 : Attach::HostPart;
            sendEvent(host, call);
            if (last) break;
        }
    }
}

void Attachment::returnHostCalls(Session* session, bool windowGone)
{
    // The calls of a pane whose program exited still go to a window that
    // stays: they go before its last frames and its PaneClosed, which the
    // finish waits for, and no pane of the session answers them. A pane
    // without a link is one of those: only a linked pane sends calls, and
    // its link goes once its PaneClosed waits behind them.
    const auto returned = [&](const WaitingEvent& waiting) {
        if (!waiting.call) return false;
        if (windowGone) return true;
        const auto link = m_panes.find(waiting.pane);
        return link != m_panes.end() && !link->second.closed;
    };
    // First what goes back, then the queue in one pass that allocates
    // nothing: a failure leaves the queue whole.
    std::vector<std::pair<std::uint32_t, std::shared_ptr<const std::string>>> calls;
    // A call whose first parts went goes whole to a window that stays: it
    // would join the parts of the next call to it. The window answers it
    // before it reads the last event, and the daemon still reads that
    // answer (readLate()).
    const auto goesWhole = [&](const WaitingEvent& waiting) { return waiting.call == m_partSent && !windowGone; };
    for (const WaitingEvent& waiting : m_waitingEvents) {
        if (!returned(waiting) || goesWhole(waiting)) continue;
        // The parts of one call wait in a row. One that got its answer here
        // already gets no second.
        if (waiting.call != m_answeredPart && (calls.empty() || calls.back().second != waiting.call)) calls.emplace_back(waiting.pane, waiting.call);
    }
    std::size_t bytes = 0;
    const auto kept = std::remove_if(m_waitingEvents.begin(), m_waitingEvents.end(), [&](const WaitingEvent& waiting) {
        if (!returned(waiting) || goesWhole(waiting)) return false;
        bytes += waiting.encoded.size() + Host::CallCost;
        return true;
    });
    m_waitingEvents.erase(kept, m_waitingEvents.end());
    m_waitingBytes -= bytes;
    if (!session) return;
    for (const auto& [id, call] : calls) {
        if (call == m_partSent) m_answeredPart = call;
        if (MuxPane* pane = session->pane(id)) pane->answerWithoutWindow(*call);
    }
}

void Attachment::paneClosed(std::unique_ptr<MuxPane> pane)
{
    // A window that never connected has nothing to learn of the pane.
    if (!m_stream || m_failed) return;
    if (PaneLink* l = link(*pane)) {
        l->closed = std::move(pane);
        return;
    }
    // No channel is free yet: the window still learns of the exit, with the
    // screen that the program left, once one is (sendClosed()).
    if (!m_failed) m_closedUnlinked.push_back(std::move(pane));
}

void Attachment::sendControl(std::string json)
{
    Attach::Event control;
    control.type = Attach::EventType::Control;
    control.payload = std::move(json);
    sendEvent(control);
}

void Attachment::panesRemoved(const std::vector<std::pair<std::uint32_t, MuxPane*>>& panes)
{
    std::map<std::uint32_t, MuxPane*> moved;
    for (const auto& [id, pane] : panes) {
        if (pane) moved.emplace(id, pane);
    }
    if (!moved.empty()) {
        // A call whose first parts went is answered too: the window drops
        // its parts with the pane. What goes first, then the queue in one
        // pass that allocates nothing, as in returnHostCalls().
        const auto movedPane = [&moved](const WaitingEvent& waiting) -> MuxPane* {
            if (!waiting.call) return nullptr;
            const auto it = moved.find(waiting.pane);
            return it == moved.end() ? nullptr : it->second;
        };
        std::vector<std::pair<MuxPane*, std::shared_ptr<const std::string>>> calls;
        for (const WaitingEvent& waiting : m_waitingEvents) {
            MuxPane* pane = movedPane(waiting);
            if (pane && waiting.call != m_answeredPart && (calls.empty() || calls.back().second != waiting.call)) {
                calls.emplace_back(pane, waiting.call);
            }
        }
        std::size_t bytes = 0;
        const auto kept = std::remove_if(m_waitingEvents.begin(), m_waitingEvents.end(), [&](const WaitingEvent& waiting) {
            if (!movedPane(waiting)) return false;
            bytes += waiting.encoded.size() + Host::CallCost;
            return true;
        });
        m_waitingEvents.erase(kept, m_waitingEvents.end());
        m_waitingBytes -= bytes;
        for (const auto& [pane, call] : calls) pane->answerWithoutWindow(*call);
    }
    for (const auto& [id, pane] : panes) {
        if (pane) m_movedPanes.insert(id);
        m_visible.erase(id);
        if (const auto held = m_heldInput.find(id); held != m_heldInput.end()) {
            // The program of a pane that went to another session runs on:
            // the bytes that the user typed still go, as its PTY takes them
            // (deliver() finds the pane). Its size and the rest are this
            // window's. A pane that the window closed takes nothing.
            std::erase_if(held->second, [&](const Attach::Input& message) {
                if (pane && message.type == Attach::InputType::Bytes) return false;
                m_heldBytes -= message.bytes.size() + PhosphorChannel::Stream::FrameCost;
                return true;
            });
            if (held->second.empty()) m_heldInput.erase(held);
        }
        const auto it = m_panes.find(id);
        if (it == m_panes.end()) continue;
        it->second.writer->close();
        m_panes.erase(it);
        Attach::Event closed;
        closed.type = Attach::EventType::PaneClosed;
        closed.pane = id;
        sendEvent(closed);
    }
}

void Attachment::finish(Attach::EventType type, Session* session)
{
    if (m_finish) return;
    m_finish = type;
    // The last event does not wait behind calls that the window would answer
    // after it no longer shows the session.
    returnHostCalls(session);
    // A window that did not connect yet keeps its connect deadline.
    if (m_stream) m_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(FinishWaitMs);
    // The input that waits for a PTY is for a session that no longer shows
    // here: as for a window that left, it waits for nothing, and goes before
    // what the window sent after it (readLate()). The bytes of a pane that
    // went to another session too, also when this session ended.
    for (const auto& [id, waiting] : m_heldInput) {
        for (const Attach::Input& message : waiting) {
            if (message.type != Attach::InputType::Resize) deliver(session, message, true);
        }
    }
    m_heldInput.clear();
    m_heldBytes = 0;
}

bool Attachment::finished() const
{
    return m_finishSent && m_waitingEvents.empty() && !m_stream->wantsWrite();
}

bool Attachment::deliver(Session* session, const Attach::Input& message, bool windowLeft)
{
    MuxPane* pane = session ? session->pane(message.pane) : nullptr;
    // A pane that went to another session since the window wrote: its
    // answers to the calls that the window took, and what the user typed,
    // still reach its program. Not its size, which its next window measures.
    if (!pane && message.type == Attach::InputType::Bytes && m_findPane && m_movedPanes.contains(message.pane)) pane = m_findPane(message.pane);
    // A pane that exited since the window wrote: its input is lost, as a
    // window's input to a program that ended.
    if (!pane) return true;
    switch (message.type) {
    case Attach::InputType::Bytes:
        // The program reads slowly, or the pane's input that waits behind a
        // decode reached its bound: the input waits for room, and the pane's
        // input after it waits behind it.
        return pane->input(message.bytes) != MuxPane::Input::Full || windowLeft;
    case Attach::InputType::Resize:
        pane->resize(message.size);
        break;
    case Attach::InputType::Reset:
        pane->reset();
        break;
    case Attach::InputType::Clear:
        pane->clear();
        break;
    case Attach::InputType::Visible:
        // Only while this window is the pane's: one that went, or failed,
        // says nothing about what the user sees now.
        pane->setVisible(message.visible, m_serial);
        // The screens that waited go in this pass (sendFrame). The window
        // says it once: a pane that links later starts with it.
        m_visible[message.pane] = message.visible;
        if (const auto link = m_panes.find(message.pane); link != m_panes.end()) link->second.shown = message.visible;
        break;
    case Attach::InputType::Refresh:
        // The window asks only for a pane whose frames it reads: one without
        // a channel yet has nothing to read. Each gets its answer: the
        // window counts them.
        if (const auto link = m_panes.find(message.pane); link != m_panes.end() && !link->second.closed) ++link->second.refreshes;
        break;
    case Attach::InputType::Kill:
    case Attach::InputType::ClosePane:
    case Attach::InputType::Layout:
    case Attach::InputType::ControlReply:
        break; // not a pane's
    }
    return true;
}

void Attachment::hold(Attach::Input message)
{
    const std::size_t cost = message.bytes.size() + PhosphorChannel::Stream::FrameCost;
    m_heldInput[message.pane].push_back(std::move(message));
    m_heldBytes += cost;
}

bool Attachment::sendHeldInput(Session& session, bool windowLeft)
{
    bool went = false;
    for (auto it = m_heldInput.begin(); it != m_heldInput.end();) {
        std::deque<Attach::Input>& waiting = it->second;
        while (!waiting.empty() && deliver(&session, waiting.front(), windowLeft)) {
            m_heldBytes -= waiting.front().bytes.size() + PhosphorChannel::Stream::FrameCost;
            waiting.pop_front();
            went = true;
        }
        it = waiting.empty() ? m_heldInput.erase(it) : std::next(it);
    }
    return went;
}

void Attachment::readInput(Session& session)
{
    if (m_finish) {
        readLate(&session);
        return;
    }
    std::string frame;
    // A window that left waits for nothing: the input that the PTY has no
    // room for, or the pane's waiting input past its bound, is lost, and
    // what came after it still counts (a resize, the end of the session).
    const bool windowLeft = !m_stream->open();
    // First, and whatever input waits: a Kill does not queue behind it. What
    // the user sees and a read of the text are not for the program: they go
    // at once, also for a pane whose program reads nothing, and also once
    // the input that waits holds the control channel (MaxHeldInput). A
    // Refresh so goes ahead of a Clear, Reset or Resize that the window
    // sent before it and that waits behind the pane's input, or on its
    // way: the read gets the text that the program has now, which they
    // change only once they reach the terminal. A Visible likewise goes
    // ahead of a Resize on its way: the screens that it shows have the size
    // that the terminal has then, and the next frame has the new size.
    if (PhosphorChannel::StreamReader* actions = m_stream->reader(Attach::ActionChannel)) {
        if (actions->codec() != Attach::ActionCodec) {
            m_failed = true;
            return;
        }
        while (actions->read(frame) == PhosphorChannel::ReadResult::Frame) {
            const auto action = Attach::decodeInput(frame);
            if (!action || !Attach::isAction(action->type)) {
                m_failed = true;
                return;
            }
            if (action->type == Attach::InputType::Kill) {
                m_kill = true;
                continue;
            }
            if (action->type != Attach::InputType::ClosePane) {
                deliver(&session, *action, windowLeft);
                continue;
            }
            // A pane whose program exited since the window closed it is gone
            // already.
            if (session.closePane(action->pane)) paneRemoved(action->pane);
        }
        if (m_kill) return;
    }
    PhosphorChannel::StreamReader* input = m_stream->reader(Attach::ControlChannel);
    if (!input) return;
    if (input->codec() != Attach::InputCodec) {
        m_failed = true;
        return;
    }
    sendHeldInput(session, windowLeft);
    // Past its bound, the input that waits holds the channel: the window's
    // input waits in the window, which refuses more past its own bound.
    while (m_heldBytes < MaxHeldInput && input->read(frame) == PhosphorChannel::ReadResult::Frame) {
        auto message = Attach::decodeInput(frame);
        if (!message) {
            m_failed = true;
            return;
        }
        // The actions come on the action channel only.
        if (Attach::isAction(message->type)) {
            m_failed = true;
            return;
        }
        if (message->type == Attach::InputType::ControlReply) {
            m_controlReplies.push_back(std::move(message->bytes));
            continue;
        }
        if (message->type == Attach::InputType::Layout) {
            std::string error;
            const auto json = AfterglowProto::Json::parse(message->bytes, &error);
            auto layout = json ? layoutFrom(*json, &error) : std::nullopt;
            if (!layout) {
                std::fprintf(stderr, "afterglow-mux: the window of session %s sent a layout that is not valid: %s\n", m_session.c_str(),
                             error.c_str());
                m_failed = true;
                return;
            }
            session.setLayout(std::move(*layout));
            continue;
        }
        // Behind the input that waits for the pane, in order. The input of
        // the other panes goes meanwhile.
        if (m_heldInput.contains(message->pane) || !deliver(&session, *message, windowLeft)) hold(std::move(*message));
    }
}

void Attachment::readLate(Session* session)
{
    std::string frame;
    if (PhosphorChannel::StreamReader* actions = m_stream->reader(Attach::ActionChannel)) {
        if (actions->codec() != Attach::ActionCodec) {
            m_failed = true;
            return;
        }
        while (actions->read(frame) == PhosphorChannel::ReadResult::Frame) {
            const auto action = Attach::decodeInput(frame);
            if (!action || !Attach::isAction(action->type)) {
                m_failed = true;
                return;
            }
            // A window that no longer shows the session cannot end it, and
            // the daemon closes the pane for the window that shows it now.
            // The sight and the reads of a window that went are nothing.
            if (action->type == Attach::InputType::ClosePane) m_late.closedPanes.push_back(action->pane);
        }
    }
    PhosphorChannel::StreamReader* input = m_stream->reader(Attach::ControlChannel);
    if (!input) return;
    if (input->codec() != Attach::InputCodec) {
        m_failed = true;
        return;
    }
    while (input->read(frame) == PhosphorChannel::ReadResult::Frame) {
        auto message = Attach::decodeInput(frame);
        if (!message || Attach::isAction(message->type)) {
            m_failed = true;
            return;
        }
        switch (message->type) {
        case Attach::InputType::Layout: {
            std::string error;
            const auto json = AfterglowProto::Json::parse(message->bytes, &error);
            if (auto layout = json ? layoutFrom(*json, &error) : std::nullopt) m_late.layout = std::move(*layout);
            break;
        }
        case Attach::InputType::Bytes:
        case Attach::InputType::Reset:
        case Attach::InputType::Clear:
            // The answers of the calls that the window got, and what the user
            // typed, before it heard: nothing waits for a window that went.
            // Without the session, only for a pane that went to another one.
            deliver(session, *message, true);
            break;
        case Attach::InputType::Resize:
        case Attach::InputType::Visible:
        case Attach::InputType::Refresh:
        case Attach::InputType::Kill:
        case Attach::InputType::ClosePane:
        case Attach::InputType::ControlReply:
            // The size of a window that went, the actions (refused above), and
            // its answers: answerForwarded failed their calls when it went.
            break;
        }
    }
}

void Attachment::catchUp(Session& session)
{
    if (!m_stream || m_failed || m_finish || !m_stream->open()) return;
    // What the socket holds now, without a wait.
    (void)m_stream->readable();
    if (m_stream->ready()) readInput(session);
}

bool Attachment::handle(short revents, Session* session)
{
    if (!m_stream) {
        if (revents & POLLIN) accept();
        if (m_failed) return false;
        if (!m_stream) return timeoutMs() > 0;
    } else {
        if (revents & (POLLIN | POLLHUP | POLLERR)) m_stream->readable();
        if (revents & POLLOUT) m_stream->writable();
    }
    m_inputDue = false;
    // A finished attachment of a session that ended still reads what the
    // window sent before it heard: the bytes of the panes that went to
    // another session reach their programs.
    if (m_stream->ready()) {
        if (session) readInput(*session);
        else if (m_finish) readLate(nullptr);
    }
    if (m_failed || !m_stream->open()) return false;
    pump(m_finish ? nullptr : session);
    if (m_failed || !m_stream->open()) return false;
    if (!m_finish) return true;
    if (finished() && !m_outputEnded) {
        // Only the output ends, and the window reads to its end. A close
        // would fail the window's next write (credit, input), and a stream
        // whose write failed reads no more: the last frames and the event
        // in its socket would be lost. Its close ends the attachment.
        m_outputEnded = true;
        (void)m_stream->shutdown(0);
    }
    return m_stream->open() && timeoutMs() > 0;
}

Attachment::PaneLink* Attachment::link(MuxPane& pane)
{
    if (const auto it = m_panes.find(pane.id()); it != m_panes.end()) return &it->second;
    // Refused once in this pump: a channel frees only when the window reads
    // one that closed to its end, which wakes the poll. Before the window's
    // hello, its limits are unknown, and a channel past them would fail the
    // stream: the hello wakes the poll too.
    if (m_noChannel || !m_stream->ready()) {
        m_noChannel = true;
        return nullptr;
    }
    // The lowest channel that is free. One that still closes is refused, and
    // the next one is tried.
    std::vector<bool> taken(Attach::MaxChannelId, false);
    for (const auto& [id, other] : m_panes) {
        if (other.channel < taken.size()) taken[other.channel] = true;
    }
    std::string error;
    for (std::uint32_t channel = Attach::ControlChannel + 1; channel < Attach::MaxChannelId; ++channel) {
        if (taken[channel]) continue;
        PhosphorChannel::StreamWriter* writer = m_stream->openWriter(channel, std::string(AfterglowVt::StateCodec), &error);
        if (!writer) {
            if (!m_stream->open()) {
                m_failed = true;
                return nullptr;
            }
            continue;
        }
        PaneLink& l = m_panes[pane.id()];
        l.channel = channel;
        l.writer = writer;
        if (const auto visible = m_visible.find(pane.id()); visible != m_visible.end()) l.shown = visible->second;
        l.state.setReplicaHistory(m_replicaHistory);
        Attach::Event opened;
        opened.type = Attach::EventType::PaneOpened;
        opened.pane = pane.id();
        opened.channel = channel;
        sendEvent(opened);
        return &l;
    }
    // Every channel that the window takes is open, or still closing: the
    // window has not read those of panes that went to their end yet. The
    // pane links in a later pump, and the others go on meanwhile.
    m_noChannel = true;
    return nullptr;
}

bool Attachment::sendFrame(PaneLink& l, MuxPane& pane)
{
    // No window granted yet: the first frame waits for it.
    if (l.writer->maxFrame() == 0) return false;
    if (!l.waiting.empty()) {
        const WriteResult result = l.writer->tryWrite(l.waiting);
        if (result == WriteResult::WouldBlock) return false;
        l.waiting.clear();
        if (result != WriteResult::Written) {
            m_failed = true;
            return false;
        }
        ++l.frames;
    }
    // The first frame of a pane carries everything; later ones what
    // changed, and the history that waits.
    l.deferred = false;
    // A pane that the window does not show (a tab in the background, a
    // window that nobody sees) gets what the window shows of it: its title,
    // its directory, its status. Its screens catch up once it shows, before
    // its host calls, which follow the text before them, and before the
    // exit, so that the window has the screen that the program left, and
    // for a read of its text (Refresh).
    l.screens = l.shown || l.closed || l.refreshes > 0 || l.marked > 0 || pane.callsWaiting(m_serial);
    // A read of the text needs the rows as they are now: this frame takes
    // them, also when nothing changed.
    const bool marking = l.refreshes > 0;
    if (marking) {
        l.state.markText();
        l.marked += std::exchange(l.refreshes, 0);
    }
    const bool changed = pane.takeChanged();
    if (!changed && !marking && l.started && !(l.screens && (l.state.pending() || l.state.screensHeld()))) return true;
    // History past half the window waits for later frames, and so do rows
    // past it, so that every frame fits.
    std::optional<std::string> next = pane.frame(l.state, l.writer->maxFrame() / 2, l.screens);
    if (!next) {
        // An image decodes: the pane wakes the daemon when it is done.
        l.deferred = true;
        return false;
    }
    l.started = true;
    std::string frame = std::move(*next);
    if (!frame.empty()) {
        const WriteResult result = l.writer->tryWrite(frame);
        if (result == WriteResult::WouldBlock) {
            l.waiting = std::move(frame);
            return false;
        }
        if (result != WriteResult::Written) {
            m_failed = true;
            return false;
        }
        ++l.frames;
    }
    return !l.state.pending();
}

void Attachment::sendClosed()
{
    // Those that waited for a channel.
    for (auto it = m_closedUnlinked.begin(); it != m_closedUnlinked.end();) {
        PaneLink* l = link(**it);
        if (m_failed) return;
        if (!l) break;
        l->closed = std::move(*it);
        it = m_closedUnlinked.erase(it);
    }
    // A pane whose program exited: its channel closes after its last frame,
    // and PaneClosed follows, so the window shows the screen that the
    // program left before it learns of the exit.
    for (auto it = m_panes.begin(); it != m_panes.end();) {
        PaneLink& l = it->second;
        if (!l.closed) {
            ++it;
            continue;
        }
        if (!sendFrame(l, *l.closed)) {
            if (m_failed) return;
            ++it;
            continue;
        }
        // The calls that the program made before it exited, after its last
        // frame and before PaneClosed.
        sendHostCalls(l, *l.closed, m_eventQueue);
        if (m_failed) return;
        if (l.closed->hasHostCalls(m_serial)) {
            ++it;
            continue;
        }
        l.writer->close();
        Attach::Event closed;
        closed.type = Attach::EventType::PaneClosed;
        closed.pane = it->first;
        closed.status = l.closed->exitStatus();
        sendEvent(closed);
        m_visible.erase(it->first);
        it = m_panes.erase(it);
    }
}

void Attachment::pump(Session* session)
{
    if (!m_stream || m_failed || !m_stream->open()) return;
    m_noChannel = false;
    // The events that waited for the window to grant the channel.
    flushEvents();
    if (m_failed) return;
    if (session && !m_finish && !m_layoutSent) {
        // The window lays out the session's tabs and splits before it makes
        // the panes that PaneOpened names.
        Attach::Event layout;
        layout.type = Attach::EventType::Layout;
        layout.payload = AfterglowProto::Json::write(toJson(session->layout()));
        sendEvent(layout);
        m_layoutSent = true;
    }
    // The panes that exited first: their last calls and their PaneClosed do
    // not wait behind the calls of the live ones.
    sendClosed();
    if (m_failed) return;
    if (session && !m_finish) {
        // Input that went now leaves the frames behind it in the channel's
        // reader, which only readInput() reads: the next pass does, at once.
        if (sendHeldInput(*session, false)) m_inputDue = true;
        // Each pump starts with the next pane, and each pane's calls take its
        // share of what is left of the queue: a pane whose program floods
        // calls does not keep the calls of the others waiting.
        const auto& panes = session->panes();
        const std::size_t count = panes.size();
        for (std::size_t i = 0; i < count; ++i) {
            MuxPane& pane = *panes[(m_turn + i) % count];
            PaneLink* l = link(pane);
            if (m_failed) return;
            if (!l) continue;
            sendFrame(*l, pane);
            if (m_failed) return;
            if (l->started && !l->deferred) {
                // A read of the pane's text waits for the frames that hold
                // its rows and its history as they were at the Refresh, not
                // for the rows that a flood adds meanwhile. The frame that
                // the channel refused counts: it goes before any newer one,
                // and the window waits for the frames that the answer names.
                // A flood that fills the channel in each pass, which leaves
                // a frame waiting after each, so keeps no read waiting. The
                // answer goes before the calls, which could fill the queue
                // in each pass: the answers that do not fit wait in the count.
                if (l->marked > 0 && l->state.textMarked()) {
                    Attach::Event refreshed;
                    refreshed.type = Attach::EventType::Refreshed;
                    refreshed.pane = pane.id();
                    refreshed.frames = l->frames + (l->waiting.empty() ? 0 : 1);
                    for (; l->marked > 0 && !m_failed && m_waitingBytes < m_eventQueue; --l->marked) sendEvent(refreshed);
                    if (m_failed) return;
                }
                // The calls of the pane's terminal follow its frame, which
                // went: a bell after the text that rang it. The window reads
                // the frames of a pane before it makes a call of it. What
                // does not fit the queue waits in the pane.
                if (l->waiting.empty()) {
                    const std::size_t left = m_waitingBytes < m_eventQueue ? m_eventQueue - m_waitingBytes : 0;
                    sendHostCalls(*l, pane, left / (count - i));
                }
            }
            if (m_failed) return;
        }
        if (count > 0) m_turn = (m_turn + 1) % count;
    }
    if (m_finish && !m_finishSent) {
        bool closing = !m_closedUnlinked.empty();
        for (const auto& [id, l] : m_panes) closing = closing || l.closed;
        if (!closing) {
            Attach::Event last;
            last.type = *m_finish;
            sendEvent(last);
            m_finishSent = true;
        }
    }
}

bool Attachment::busy() const
{
    if (!m_stream || m_failed) return false;
    if (m_inputDue) return true;
    // A finished attachment sends no more of its live panes: only the last
    // frames of closed ones, then its last event, which can go at once when
    // no pane closes (a finish from a call: the next pass sends it).
    bool closing = !m_closedUnlinked.empty();
    for (const auto& [id, l] : m_panes) closing = closing || l.closed;
    if (m_finish && !m_finishSent && !closing && m_stream->open()) return true;
    for (const auto& [id, l] : m_panes) {
        if (!l.waiting.empty() || !l.writer || l.writer->maxFrame() == 0) continue;
        // A closed pane whose calls wait for room in the queue waits for the
        // window's credit, which wakes the poll.
        if (l.closed && !(l.closed->hasHostCalls(m_serial) && m_waitingBytes >= m_eventQueue)) return true;
        // A frame that waits for a decode is no work: the pane wakes the
        // daemon when the decode's feed ends. Neither are the history and
        // the images of a pane that the window does not show: they wait for
        // its screens.
        if (!l.closed && !m_finish && !l.deferred && l.screens && l.state.pending()) return true;
        // A pane that shows now: its screens catch up.
        if (!l.closed && !m_finish && !l.deferred && l.shown && l.state.screensHeld()) return true;
    }
    return false;
}

} // namespace AfterglowMux
