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

std::unique_ptr<Attachment> Attachment::listen(const std::string& directory, pid_t caller, std::string* error)
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

int Attachment::connectTimeoutMs() const
{
    if (m_stream) return -1;
    const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(m_deadline - std::chrono::steady_clock::now());
    return left.count() > 0 ? int(left.count()) : 0;
}

bool Attachment::accept()
{
    while (true) {
        const int fd = ::accept4(m_listenFd, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0 && errno == EINTR) continue;
        if (fd < 0) return false;
        ucred peer{};
        socklen_t size = sizeof peer;
        // Only the process that made the attach call: the socket's name is
        // no secret from other processes of the user.
        if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != ::getuid() || peer.pid != m_caller) {
            ::close(fd);
            continue;
        }
        ::close(m_listenFd);
        ::unlink(m_path.c_str());
        m_listenFd = -1;
        m_stream = std::make_unique<PhosphorChannel::Stream>(fd);
        std::string error;
        m_events = m_stream->openWriter(Attach::ControlChannel, Attach::EventCodec, &error);
        if (!m_events) m_failed = true;
        return true;
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

void Attachment::paneClosed(std::uint32_t pane, int status)
{
    if (!m_stream) return;
    sendEvent({Attach::EventType::PaneClosed, pane, status});
    if (auto it = m_panes.find(pane); it != m_panes.end()) {
        if (it->second.writer) it->second.writer->close();
        m_panes.erase(it);
    }
}

void Attachment::finish(Attach::EventType type, Session* session)
{
    if (!m_stream) return;
    if (session) pump(*session);
    sendEvent({type, 0, 0});
    // What the window has not read yet still goes, for a short time: the
    // window learns why the stream ends.
    m_stream->flush(200);
}

void Attachment::readInput(Session& session)
{
    PhosphorChannel::StreamReader* input = m_stream->reader(Attach::ControlChannel);
    if (!input) return;
    std::string frame;
    while (input->read(frame) == PhosphorChannel::ReadResult::Frame) {
        const auto message = Attach::decodeInput(frame);
        if (!message) {
            m_failed = true;
            return;
        }
        MuxPane* pane = session.pane(message->pane);
        // A pane that exited since the window wrote: its input is lost, as
        // a window's input to a program that ended.
        if (!pane) continue;
        if (message->type == Attach::InputType::Bytes) {
            (void)pane->input(message->bytes);
        } else {
            pane->resize(message->size);
        }
    }
}

bool Attachment::handle(short revents, Session& session)
{
    if (!m_stream) {
        if (revents & POLLIN) accept();
        if (!m_stream) return connectTimeoutMs() > 0;
    } else {
        if (revents & (POLLIN | POLLHUP | POLLERR)) m_stream->readable();
        if (revents & POLLOUT) m_stream->writable();
        if (m_stream->ready()) readInput(session);
    }
    if (m_failed || !m_stream->open()) return false;
    pump(session);
    return !m_failed && m_stream->open();
}

void Attachment::pump(Session& session)
{
    if (!m_stream || !m_stream->open()) return;
    // The events that waited for the window to grant the channel.
    flushEvents();
    for (const auto& pane : session.panes()) {
        auto [it, added] = m_panes.try_emplace(pane->id());
        PaneLink& link = it->second;
        if (added) {
            std::string error;
            link.writer = m_stream->openWriter(pane->id(), std::string(AfterglowVt::StateCodec), &error);
            if (!link.writer) {
                m_failed = true;
                return;
            }
            sendEvent({Attach::EventType::PaneOpened, pane->id(), 0});
        }
        // No window granted yet: the first frame waits for it.
        if (link.writer->maxFrame() == 0) continue;
        if (!link.waiting.empty()) {
            const WriteResult result = link.writer->tryWrite(link.waiting);
            if (result == WriteResult::WouldBlock) continue;
            link.waiting.clear();
            if (result != WriteResult::Written) {
                m_failed = true;
                return;
            }
        }
        // The first frame of a pane carries everything; later ones what
        // changed, and the history that waits.
        const bool changed = pane->takeChanged();
        if (!changed && !link.state.pending() && link.started) continue;
        link.started = true;
        // History past half the window waits for later frames, so that a
        // frame of visible rows always fits.
        std::string frame = pane->withTerminal(
            [&](const AfterglowVt::Terminal& t) { return link.state.update(t, link.writer->maxFrame() / 2); });
        if (frame.empty()) continue;
        const WriteResult result = link.writer->tryWrite(frame);
        if (result == WriteResult::WouldBlock) {
            link.waiting = std::move(frame);
        } else if (result != WriteResult::Written) {
            m_failed = true;
            return;
        }
    }
}

bool Attachment::busy() const
{
    if (!m_stream) return false;
    for (const auto& [id, link] : m_panes) {
        if (link.waiting.empty() && link.writer && link.writer->maxFrame() > 0 && link.state.pending()) return true;
    }
    return false;
}

} // namespace AfterglowMux
