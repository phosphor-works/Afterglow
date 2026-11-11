// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Attach.h"

#include "Session.h"

#include <AfterglowProto/Socket.h>

#include <cerrno>
#include <cstring>

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
        // The window writes only its input, on the control channel, and its
        // Kill on the action channel.
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

void Attachment::sendHostCalls(MuxPane& pane)
{
    if (m_waitingBytes >= m_eventQueue) return;
    for (std::string& encoded : pane.takeHostCalls(m_serial, m_eventQueue - m_waitingBytes)) {
        auto call = std::make_shared<const std::string>(std::move(encoded));
        Attach::Event host;
        host.type = Attach::EventType::Host;
        host.pane = pane.id();
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
    std::vector<std::pair<std::uint32_t, std::shared_ptr<const std::string>>> calls;
    for (auto it = m_waitingEvents.begin(); it != m_waitingEvents.end();) {
        if (!it->call) {
            ++it;
            continue;
        }
        // The parts of one call wait in a row. One that got its answer here
        // already gets no second.
        if (it->call != m_answeredPart && (calls.empty() || calls.back().second != it->call)) calls.emplace_back(it->pane, it->call);
        // A call whose first parts went goes whole: the window would join
        // the parts of the next call to it. Its answer is this one all the
        // same, as the daemon reads the window's input no more.
        if (it->call == m_partSent && !windowGone) {
            ++it;
            continue;
        }
        m_waitingBytes -= it->encoded.size() + Host::CallCost;
        it = m_waitingEvents.erase(it);
    }
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
    if (PaneLink* l = link(*pane)) l->closed = std::move(pane);
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
    // here.
    m_heldInput.reset();
}

bool Attachment::finished() const
{
    return m_finishSent && m_waitingEvents.empty() && !m_stream->wantsWrite();
}

bool Attachment::sendHeldInput(Session& session)
{
    if (!m_heldInput) return true;
    MuxPane* pane = session.pane(m_heldInput->pane);
    if (pane && pane->input(m_heldInput->bytes) == MuxPane::Input::Full) return false;
    m_heldInput.reset();
    return true;
}

void Attachment::readInput(Session& session)
{
    std::string frame;
    // First, and whatever input waits: a Kill does not queue behind it.
    if (PhosphorChannel::StreamReader* actions = m_stream->reader(Attach::ActionChannel)) {
        if (actions->codec() != Attach::ActionCodec) {
            m_failed = true;
            return;
        }
        while (actions->read(frame) == PhosphorChannel::ReadResult::Frame) {
            const auto action = Attach::decodeInput(frame);
            if (!action || action->type != Attach::InputType::Kill) {
                m_failed = true;
                return;
            }
            m_kill = true;
        }
        if (m_kill) return;
    }
    PhosphorChannel::StreamReader* input = m_stream->reader(Attach::ControlChannel);
    if (!input) return;
    if (input->codec() != Attach::InputCodec) {
        m_failed = true;
        return;
    }
    // A window that left waits for nothing: the input that the PTY has no
    // room for, or the pane's waiting input past its bound, is lost, and
    // what came after it still counts (a resize, the end of the session).
    const bool windowLeft = !m_stream->open();
    if (!windowLeft && !sendHeldInput(session)) return;
    while (input->read(frame) == PhosphorChannel::ReadResult::Frame) {
        auto message = Attach::decodeInput(frame);
        if (!message) {
            m_failed = true;
            return;
        }
        // A Kill comes on the action channel only.
        if (message->type == Attach::InputType::Kill) {
            m_failed = true;
            return;
        }
        MuxPane* pane = session.pane(message->pane);
        // A pane that exited since the window wrote: its input is lost, as
        // a window's input to a program that ended.
        if (!pane) continue;
        switch (message->type) {
        case Attach::InputType::Bytes:
            // The program reads slowly, or the pane's input that waits
            // behind a decode reached its bound: the input waits for room,
            // and the window's input waits behind it in the channel.
            if (pane->input(message->bytes) == MuxPane::Input::Full && !windowLeft) {
                m_heldInput = std::move(message);
                return;
            }
            break;
        case Attach::InputType::Resize:
            pane->resize(message->size);
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
            pane->setVisible(message->visible, m_serial);
            break;
        case Attach::InputType::Kill:
            break; // refused above: a Kill comes on the action channel only
        }
    }
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
    if (!m_finish && session && m_stream->ready()) readInput(*session);
    if (m_failed || !m_stream->open()) return false;
    pump(session);
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
    // The lowest channel that is free: the window's stream takes ids below
    // Attach::MaxChannelId, however many panes the daemon ran. One that
    // still closes is refused, and the next one is tried.
    std::string error;
    for (std::uint32_t channel = Attach::ControlChannel + 1; channel < Attach::MaxChannelId; ++channel) {
        bool taken = false;
        for (const auto& [id, other] : m_panes) taken = taken || other.channel == channel;
        if (taken) continue;
        PhosphorChannel::StreamWriter* writer = m_stream->openWriter(channel, std::string(AfterglowVt::StateCodec), &error);
        if (!writer) {
            if (!m_stream->open()) break;
            continue;
        }
        PaneLink& l = m_panes[pane.id()];
        l.channel = channel;
        l.writer = writer;
        l.state.setReplicaHistory(m_replicaHistory);
        Attach::Event opened;
        opened.type = Attach::EventType::PaneOpened;
        opened.pane = pane.id();
        opened.channel = channel;
        sendEvent(opened);
        return &l;
    }
    m_failed = true;
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
    }
    // The first frame of a pane carries everything; later ones what
    // changed, and the history that waits.
    l.deferred = false;
    const bool changed = pane.takeChanged();
    if (!changed && !l.state.pending() && l.started) return true;
    // History past half the window waits for later frames, and so do rows
    // past it, so that every frame fits.
    std::optional<std::string> next = pane.frame(l.state, l.writer->maxFrame() / 2);
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
    }
    return !l.state.pending();
}

void Attachment::pump(Session* session)
{
    if (!m_stream || m_failed || !m_stream->open()) return;
    // The events that waited for the window to grant the channel.
    flushEvents();
    if (session && !m_finish) {
        // Input that went now leaves the frames behind it in the channel's
        // reader, which only readInput() reads: the next pass does, at once.
        const bool held = m_heldInput.has_value();
        if (sendHeldInput(*session) && held) m_inputDue = true;
        for (const auto& pane : session->panes()) {
            PaneLink* l = link(*pane);
            if (!l) return;
            sendFrame(*l, *pane);
            if (m_failed) return;
            // The calls of the pane's terminal follow its frame, which went: a
            // bell after the text that rang it. The window reads the frames of
            // a pane before it makes a call of it. What does not fit the queue
            // waits in the pane.
            if (l->started && l->waiting.empty() && !l->deferred) sendHostCalls(*pane);
            if (m_failed) return;
        }
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
        sendHostCalls(*l.closed);
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
        it = m_panes.erase(it);
    }
    if (m_finish && !m_finishSent) {
        bool closing = false;
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
    bool closing = false;
    for (const auto& [id, l] : m_panes) closing = closing || l.closed;
    if (m_finish && !m_finishSent && !closing && m_stream->open()) return true;
    for (const auto& [id, l] : m_panes) {
        if (!l.waiting.empty() || !l.writer || l.writer->maxFrame() == 0) continue;
        // A closed pane whose calls wait for room in the queue waits for the
        // window's credit, which wakes the poll.
        if (l.closed && !(l.closed->hasHostCalls(m_serial) && m_waitingBytes >= m_eventQueue)) return true;
        // A frame that waits for a decode is no work: the pane wakes the
        // daemon when the decode's feed ends.
        if (!l.closed && !m_finish && !l.deferred && l.state.pending()) return true;
    }
    return false;
}

} // namespace AfterglowMux
