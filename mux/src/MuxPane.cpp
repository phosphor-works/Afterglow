// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxPane.h"

#include <AfterglowProto/DragDrop.h>
#include <AfterglowProto/Transfer.h>

#include <algorithm>
#include <array>

namespace AfterglowMux {

namespace {

/// The pixels of @p cells cells of @p cellSize: the winsize of a PTY holds
/// at most 65535, and the product of the protocol's bounds would overflow an
/// int.
int pixels(int cells, int cellSize)
{
    return int(std::min<std::int64_t>(std::int64_t(cells) * cellSize, 65535));
}

/// Whether @p pid leads the foreground process group of its terminal, in the
/// session whose leader is @p session. Not only a member of that group: a
/// shell escape of an editor (vim's :!) runs in the editor's group, and the
/// editor is a job to ask about.
///
/// The pid is SO_PEERCRED's, of the process that connected. A client that
/// passed its connection on and exited could have its pid reused; the
/// clients of the daemon keep their connection to themselves.
bool leadsForeground(pid_t pid, pid_t session)
{
    const auto stat = AfterglowPty::processStat(pid);
    return stat && stat->session == session && stat->foregroundGroup == pid;
}

} // namespace

MuxPane::MuxPane(std::uint32_t id, const AfterglowVt::TerminalOptions& options, const PaneSize& size,
                 std::string notificationCapabilities, std::function<void()> wake)
    : m_id(id)
    , m_wake(std::move(wake))
    , m_notificationCapabilities(std::move(notificationCapabilities))
    , m_terminal(size.cols, size.rows, *this, options)
{
    // Before the program starts: a program that queries the cell size at
    // once gets the size that the client sent. No window measured it yet,
    // so that is 0 (unknown) from `afterglow mux new`.
    m_terminal.resize(size.cols, size.rows, size.cellWidth, size.cellHeight);
}

MuxPane::~MuxPane()
{
    // The reader thread may call back into this pane, writeToPty() among
    // them, until it stops: it stops while m_pty still points at the PTY
    // (reset() clears the pointer before the PTY's destructor runs). Then
    // the PTY closes, which hangs up the program.
    if (m_pty) m_pty->stop();
    m_pty.reset();
}

bool MuxPane::start(AfterglowPty::SpawnOptions options, std::string launchedProgram, std::string* error)
{
    {
        std::lock_guard lock(m_mutex);
        options.cols = m_terminal.cols();
        options.rows = m_terminal.rows();
        options.pixelWidth = pixels(m_terminal.cols(), m_terminal.cellWidth());
        options.pixelHeight = pixels(m_terminal.rows(), m_terminal.cellHeight());
    }
    m_launchedProgram = std::move(launchedProgram);
    // Deferred: m_pty must be set before the reader thread can call
    // writeToPty() with the answer to a query.
    options.deferStart = true;
    m_pty = AfterglowPty::Pty::spawn(std::move(options), *this, error);
    if (m_pty && !m_pty->start(error)) m_pty.reset();
    if (!m_pty) m_exited = true;
    return m_pty != nullptr;
}

std::string MuxPane::foregroundJob(pid_t caller) const
{
    if (!m_pty || m_exited) return {};
    if (caller > 0 && leadsForeground(caller, m_pty->pid())) return {};
    if (std::string job = m_pty->foregroundProcess(); !job.empty()) return job;
    return m_launchedProgram;
}

void MuxPane::writeToPty(std::string_view bytes)
{
    // A reply that the PTY's input queue refuses is lost, as in a window:
    // the program reads its input too slowly to wait for it.
    if (m_pty) (void)m_pty->write(bytes);
}

void MuxPane::setAttached(bool attached, std::size_t queueBytes)
{
    std::deque<std::string> left;
    {
        std::lock_guard lock(m_hostMutex);
        if (m_attached == attached && m_hostLimit == queueBytes) return;
        m_attached = attached;
        m_hostLimit = queueBytes;
        if (attached) return;
        left.swap(m_hostCalls);
        m_hostBytes = 0;
    }
    // The window went before it got them: a request waits for an answer,
    // which it gets as when no window shows the pane. The rest go.
    for (const std::string& encoded : left) {
        const auto call = Host::decode(encoded);
        if (!call) continue;
        if (call->kind == Host::Kind::ClipboardRequest) clipboardWithoutWindow(call->clipboard);
        else if (call->kind == Host::Kind::TerminalProtocol) protocolWithoutWindow(call->number, call->text);
        else if (call->kind == Host::Kind::DesktopNotification) notificationWithoutWindow(call->notification);
    }
}

bool MuxPane::forward(const Host::Call& call)
{
    std::string encoded = Host::encode(call);
    bool first = false;
    {
        std::lock_guard lock(m_hostMutex);
        if (!m_attached || encoded.size() > Host::MaxCall || m_hostBytes + encoded.size() > m_hostLimit) return false;
        first = m_hostCalls.empty();
        m_hostBytes += encoded.size();
        m_hostCalls.push_back(std::move(encoded));
    }
    // The daemon sends the calls when it wakes: one wake for each batch.
    if (first) m_wake();
    return true;
}

std::vector<std::string> MuxPane::takeHostCalls(std::size_t budget)
{
    std::vector<std::string> calls;
    std::lock_guard lock(m_hostMutex);
    std::size_t taken = 0;
    while (!m_hostCalls.empty() && (calls.empty() || taken + m_hostCalls.front().size() <= budget)) {
        taken += m_hostCalls.front().size();
        m_hostBytes -= m_hostCalls.front().size();
        calls.push_back(std::move(m_hostCalls.front()));
        m_hostCalls.pop_front();
    }
    return calls;
}

bool MuxPane::hasHostCalls() const
{
    std::lock_guard lock(m_hostMutex);
    return !m_hostCalls.empty();
}

void MuxPane::bell()
{
    forward(Host::make(Host::Kind::Bell));
}

void MuxPane::flash()
{
    forward(Host::make(Host::Kind::Flash));
}

void MuxPane::notify(std::string_view title, std::string_view body)
{
    forward(Host::make(Host::Kind::Notify, std::string(title), std::string(body)));
}

void MuxPane::clipboardWrite(std::string_view selection, std::string_view data)
{
    forward(Host::make(Host::Kind::ClipboardWrite, std::string(selection), std::string(data)));
}

void MuxPane::pointerShapeChanged(std::string_view shape)
{
    forward(Host::make(Host::Kind::PointerShape, {}, std::string(shape)));
}

void MuxPane::userVarChanged(std::string_view name, std::string_view value)
{
    forward(Host::make(Host::Kind::UserVar, std::string(name), std::string(value)));
}

void MuxPane::commandStarted(std::string_view command)
{
    forward(Host::make(Host::Kind::CommandStarted, {}, std::string(command)));
}

void MuxPane::commandFinished(int exitCode)
{
    forward(Host::make(Host::Kind::CommandFinished, {}, {}, exitCode));
}

void MuxPane::downloadFile(std::string_view name, std::string_view data)
{
    // Without a window, the download is dropped: no user can be asked.
    forward(Host::make(Host::Kind::Download, std::string(name), std::string(data)));
}

void MuxPane::desktopNotification(const AfterglowVt::NotificationRequest& request)
{
    Host::Call call = Host::make(Host::Kind::DesktopNotification);
    call.notification = request;
    if (!forward(call)) notificationWithoutWindow(request);
}

void MuxPane::notificationWithoutWindow(const AfterglowVt::NotificationRequest& request)
{
    // No notification is on screen: a program that asks which are gets an
    // empty list, in the reply of a window. The others are dropped.
    if (request.kind != AfterglowVt::NotificationRequest::Kind::Alive) return;
    writeToPty("\x1b]99;i=" + (request.id.empty() ? std::string("0") : request.id) + ":p=alive;\x1b\\");
}

bool MuxPane::clipboardRequest(const AfterglowVt::ClipboardRequest& request)
{
    using AfterglowVt::ClipboardAssembler;
    // The window asks the user and answers, as for a terminal of its own.
    Host::Call call = Host::make(Host::Kind::ClipboardRequest);
    call.clipboard = request;
    if (!forward(call)) clipboardWithoutWindow(request);
    return true;
}

void MuxPane::clipboardWithoutWindow(const AfterglowVt::ClipboardRequest& request)
{
    using AfterglowVt::ClipboardAssembler;
    // No window has a clipboard to give: the request gets the answer of a
    // denial. A write is dropped.
    if (request.kind == AfterglowVt::ClipboardRequest::Kind::Write) {
        writeToPty(ClipboardAssembler::writeStatus("EPERM", request.id));
    } else if (request.osc52) {
        writeToPty(request.osc52Reply({}));
    } else {
        writeToPty(ClipboardAssembler::readStatus("EPERM", request));
    }
}

void MuxPane::terminalProtocol(int code, std::string_view payload)
{
    // The window runs the transfer or the drag, and answers the queries
    // too: the program then hears of one terminal.
    if (!forward(Host::make(Host::Kind::TerminalProtocol, {}, std::string(payload), code))) protocolWithoutWindow(code, payload);
}

void MuxPane::protocolWithoutWindow(int code, std::string_view payload)
{
    if (code == 72) {
        // A program asks which drag and drop types the terminal supports. It
        // gets the answer of a window: a client may attach before a drag.
        if (const std::string reply = AfterglowProto::dndQueryReply(payload); !reply.empty()) {
            writeToPty(reply);
            return;
        }
        // A drag starts from an offer (t=o without x=1 or x=2) while the
        // user holds the mouse button. Without a window no drag can start,
        // so the offer gets the window's EPERM. A drop never arrives, so
        // the other commands need no answer. A chunked offer is answered
        // once, at its last chunk.
        const std::string_view raw = AfterglowProto::splitDnd(payload).first;
        const auto meta = AfterglowProto::parseDndMeta(raw);
        if (!meta || AfterglowProto::dndMore(raw)) return;
        const auto type = meta->find("t");
        const auto x = meta->find("x");
        if (type == meta->end() || type->second != "o" || (x != meta->end() && (x->second == "1" || x->second == "2"))) return;
        AfterglowProto::DndMeta error{{"t", "E"}};
        if (const auto id = meta->find("i"); id != meta->end()) error.emplace("i", id->second);
        for (const std::string& packet : AfterglowProto::writeDnd(std::move(error), "EPERM")) writeToPty(packet);
        return;
    }
    if (code != 5113 || payload.size() > AfterglowProto::TransferMaxCommand) return;
    // A transfer starts with ac=send or ac=receive and waits for the user's
    // answer. Without a window no user answers: it is the answer of a
    // denial. A session with
    // q=2 asked for no status, errors included.
    const auto fields = AfterglowProto::decodeTransfer(payload);
    if (!fields) return;
    const auto action = fields->find("ac");
    const auto id = fields->find("id");
    if (action == fields->end() || id == fields->end() || (action->second != "send" && action->second != "receive")) return;
    if (const auto quiet = fields->find("q"); quiet != fields->end() && quiet->second == "2") return;
    const std::array<std::pair<std::string_view, std::string_view>, 3> status{{
        {"ac", "status"},
        {"id", id->second},
        {"st", "EPERM"},
    }};
    writeToPty(AfterglowProto::encodeTransfer(status));
}

void MuxPane::ptyData(std::string_view bytes)
{
    {
        std::lock_guard lock(m_mutex);
        m_terminal.feed(bytes);
    }
    // One wake for each burst: the daemon takes the flag when it sends.
    if (!m_changed.exchange(true)) m_wake();
}

void MuxPane::ptyClosed(int status)
{
    m_exitStatus = status;
    m_exited = true;
    m_wake();
}

MuxPane::Input MuxPane::input(std::string_view bytes)
{
    if (!m_pty || m_exited) return Input::Gone;
    if (m_pty->write(bytes)) return Input::Written;
    // A terminal that takes no more input (its child side is gone, while the
    // child may still run) never has room again: only a full queue does.
    return m_exited || !m_pty->accepting() ? Input::Gone : Input::Full;
}

void MuxPane::reset()
{
    {
        std::lock_guard lock(m_mutex);
        m_terminal.reset();
    }
    if (!m_changed.exchange(true)) m_wake();
}

void MuxPane::clear()
{
    bool atPrompt = false;
    {
        std::lock_guard lock(m_mutex);
        m_terminal.clearScreenAndHistory();
        atPrompt = m_terminal.atShellPrompt() && !m_terminal.modes().altScreen;
    }
    // At a prompt the shell redraws it on the cleared screen, as in a window.
    if (atPrompt) writeToPty("\x0c");
    if (!m_changed.exchange(true)) m_wake();
}

void MuxPane::resize(const PaneSize& size)
{
    int cols = 0;
    int rows = 0;
    {
        std::lock_guard lock(m_mutex);
        m_terminal.resize(size.cols, size.rows, size.cellWidth, size.cellHeight);
        cols = m_terminal.cols();
        rows = m_terminal.rows();
    }
    if (m_pty) m_pty->resize(cols, rows, pixels(cols, size.cellWidth), pixels(rows, size.cellHeight));
    // The window needs the frame of the new size.
    if (!m_changed.exchange(true)) m_wake();
}

} // namespace AfterglowMux
