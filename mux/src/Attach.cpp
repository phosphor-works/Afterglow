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

void Attachment::sendEvent(const Attach::Event& event)
{
    m_waitingEvents.push_back(Attach::encode(event));
    flushEvents();
}

void Attachment::flushEvents()
{
    while (m_events && !m_waitingEvents.empty()) {
        // Events are few and small: they wait only until the window grants
        // the channel its window, at the start.
        if (m_events->tryWrite(m_waitingEvents.front()) != WriteResult::Written) break;
        m_waitingEvents.erase(m_waitingEvents.begin());
    }
}

void Attachment::paneClosed(std::unique_ptr<MuxPane> pane)
{
    // A window that never connected has nothing to learn of the pane.
    if (!m_stream || m_failed) return;
    if (PaneLink* l = link(*pane)) l->closed = std::move(pane);
}

void Attachment::finish(Attach::EventType type)
{
    if (m_finish) return;
    m_finish = type;
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
    // room for is lost, and what came after it still counts (a resize, the
    // end of the session).
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
            // The program reads slowly: the input waits for room, and the
            // window's input waits behind it in the channel.
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
        case Attach::InputType::Kill:
            break;
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
    const bool changed = pane.takeChanged();
    if (!changed && !l.state.pending() && l.started) return true;
    l.started = true;
    // History past half the window waits for later frames, and so do rows
    // past it, so that every frame fits.
    std::string frame = pane.withTerminal([&](const AfterglowVt::Terminal& t) { return l.state.update(t, l.writer->maxFrame() / 2); });
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
        sendHeldInput(*session);
        for (const auto& pane : session->panes()) {
            PaneLink* l = link(*pane);
            if (!l) return;
            sendFrame(*l, *pane);
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
    // A finished attachment sends no more of its live panes: only the last
    // frames of closed ones, then its last event, which can go at once when
    // no pane closes (a finish from a call: the next pass sends it).
    bool closing = false;
    for (const auto& [id, l] : m_panes) closing = closing || l.closed;
    if (m_finish && !m_finishSent && !closing && m_stream->open()) return true;
    for (const auto& [id, l] : m_panes) {
        if (l.waiting.empty() && l.writer && l.writer->maxFrame() > 0 && (l.closed || (!m_finish && l.state.pending()))) return true;
    }
    return false;
}

} // namespace AfterglowMux
