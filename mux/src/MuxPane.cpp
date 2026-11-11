// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxPane.h"

#include <AfterglowProto/DragDrop.h>
#include <AfterglowProto/Text.h>
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
    // No window shows it yet: one that does says so (setVisible).
    m_terminal.setVisible(false);
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

std::string MuxPane::directory() const
{
    if (!m_pty || m_exited) return {};
    std::string path = m_pty->currentWorkingDirectory();
    // The answer is JSON text, which would replace each byte that is not
    // UTF-8 and so name another path.
    for (std::size_t i = 0; i < path.size();) {
        char32_t cp = 0;
        if (!AfterglowProto::nextUtf8(path, i, cp)) return {};
    }
    return path;
}

void MuxPane::writeDirect(std::string_view bytes)
{
    // A reply that the PTY's input queue refuses is lost, as in a window:
    // the program reads its input too slowly to wait for it.
    if (m_pty) (void)m_pty->write(bytes);
}

void MuxPane::writeToPty(std::string_view bytes)
{
    // A report goes through the window, which holds it behind the replies to
    // the commands that the program wrote before the query (Pane::writeToPty).
    // A report that does not fit is dropped, as the window drops one over its
    // budget.
    if (forward(Host::make(Host::Kind::Report, {}, std::string(bytes))) == Forward::NoWindow) writeDirect(bytes);
}

bool MuxPane::pushLocked(WaitingCall call)
{
    const bool first = m_hostCalls.empty();
    if (call.cost == 0) call.cost = call.encoded.size() + Host::CallCost;
    const std::size_t cost = call.cost;
    // Counted once it waits: a push that runs out of memory counts nothing.
    m_hostCalls.push_back(std::move(call));
    m_hostBytes += cost;
    return first;
}

std::string MuxPane::encodeLocked(const WaitingCall& waiting) const
{
    switch (waiting.kind) {
    case Host::Kind::PointerShape:
        return Host::encode(Host::make(Host::Kind::PointerShape, {}, m_pointerShape));
    case Host::Kind::UserVar: {
        const auto it = m_userVars.find(waiting.name);
        return Host::encode(Host::make(Host::Kind::UserVar, waiting.name, it == m_userVars.end() ? std::string() : it->second));
    }
    case Host::Kind::ProtocolLost: {
        Host::Call call = Host::make(Host::Kind::ProtocolLost, {}, {}, m_protocolLost);
        call.value = m_protocolLostReset ? 1 : 0;
        return Host::encode(call);
    }
    default:
        return waiting.encoded;
    }
}

bool MuxPane::queueLocked(std::string encoded, Host::Kind kind, Fit fit, bool* wake)
{
    if (!m_window || encoded.size() > Host::MaxCall) return false;
    const std::size_t cost = encoded.size() + Host::CallCost;
    if (fit == Fit::Queue && !m_hostCalls.empty() && m_hostBytes + cost > m_hostLimit) return false;
    *wake = pushLocked(WaitingCall{kind, std::move(encoded), {}, cost});
    return true;
}

MuxPane::Forward MuxPane::forward(const Host::Call& call)
{
    {
        // Not encoded for a pane that no window shows: a download or a
        // clipboard write can be many megabytes.
        std::lock_guard lock(m_hostMutex);
        if (!m_window) return Forward::NoWindow;
    }
    std::string encoded = Host::encode(call);
    bool wake = false;
    Forward result = Forward::Queued;
    {
        // Decided under one lock: a window may come or go since the check.
        std::lock_guard lock(m_hostMutex);
        if (!m_window) result = Forward::NoWindow;
        else if (!queueLocked(std::move(encoded), call.kind, Fit::Queue, &wake)) result = Forward::NoRoom;
    }
    // The daemon sends the calls when it wakes: one wake for each batch.
    if (wake) m_wake();
    return result;
}

void MuxPane::setWindow(std::uint64_t window, std::size_t queueBytes)
{
    bool wake = false;
    {
        std::lock_guard lock(m_hostMutex);
        m_hostLimit = queueBytes;
        if (m_window == window) return;
        // The window went before it got them. Answered under the lock, before
        // the answers to newer calls (Pty::write calls nothing back). Each one
        // goes once answered: a failure leaves the rest for the next try.
        while (!m_hostCalls.empty()) {
            const WaitingCall& waiting = m_hostCalls.front();
            if (!waiting.encoded.empty()) answerWithoutWindow(waiting.encoded);
            m_hostBytes -= waiting.cost;
            m_hostCalls.pop_front();
        }
        m_bellWaiting = m_flashWaiting = m_pointerWaiting = false;
        m_userVarsWaiting.clear();
        m_commandForwarded = false;
        m_protocolLostWaiting = false;
        if (window) {
            // A new window knows nothing of the pane yet: what the program set
            // before it came.
            if (!m_pointerShape.empty()) {
                wake = pushLocked(WaitingCall{Host::Kind::PointerShape, {}, {}, Host::CallCost}) || wake;
                m_pointerWaiting = true;
            }
            for (const auto& [name, value] : m_userVars) {
                wake = pushLocked(WaitingCall{Host::Kind::UserVar, {}, name, Host::CallCost + name.size() + value.size()}) || wake;
                m_userVarsWaiting.insert(name);
            }
            if (m_commandRunning) {
                wake = pushLocked(WaitingCall{Host::Kind::CommandStarted, Host::encode(Host::make(Host::Kind::CommandStarted, {}, m_command)), {}, 0}) || wake;
                m_commandForwarded = true;
            }
        }
        // Last: a failure above leaves the old window, and the next pass tries
        // again from the start.
        m_window = window;
    }
    if (wake) m_wake();
    // After m_window: a report of the change goes to the program, or through
    // the window that came, not to the window that went. Whether the user can
    // see the pane is for the new window to say. A report that fails (no
    // memory to queue it) is not tried again: the next window says anew.
    setVisible(false, window);
}

std::string MuxPane::frame(AfterglowVt::StateWriter& writer, std::size_t maxBytes)
{
    std::lock_guard lock(m_mutex);
    std::string frame = writer.update(m_terminal, maxBytes, &m_scrolls);
    // The frame carries them, also one that waits for room in the channel.
    m_scrolls.clear();
    return frame;
}

void MuxPane::setVisible(bool visible, std::uint64_t window)
{
    std::lock_guard lock(m_mutex);
    bool current = false;
    {
        // After m_mutex, as the reader thread takes them, and released before
        // the terminal's report, which forward() takes it for.
        std::lock_guard host(m_hostMutex);
        current = window != 0 && m_window == window;
    }
    // A pane that no window shows cannot be seen, and a window that is not
    // the pane's says nothing about it.
    m_terminal.setVisible(visible && current);
}

std::vector<std::string> MuxPane::takeHostCalls(std::uint64_t window, std::size_t budget)
{
    std::vector<std::string> calls;
    std::lock_guard lock(m_hostMutex);
    // A setWindow() that failed part of the way left calls for a window that
    // the pane does not have yet.
    if (m_window != window) return calls;
    std::size_t taken = 0;
    while (!m_hostCalls.empty()) {
        WaitingCall& front = m_hostCalls.front();
        // Moved, not copied: a call can be many megabytes. Only the kinds that
        // take their newest value encode now.
        const bool late = front.encoded.empty();
        std::string encoded = late ? encodeLocked(front) : std::string();
        const std::size_t size = late ? encoded.size() : front.encoded.size();
        if (!calls.empty() && taken + size + Host::CallCost > budget) break;
        if (!late) encoded = std::move(front.encoded);
        taken += size + Host::CallCost;
        switch (front.kind) {
        case Host::Kind::Bell: m_bellWaiting = false; break;
        case Host::Kind::Flash: m_flashWaiting = false; break;
        case Host::Kind::PointerShape: m_pointerWaiting = false; break;
        case Host::Kind::UserVar: m_userVarsWaiting.erase(front.name); break;
        case Host::Kind::ProtocolLost:
            m_protocolLostWaiting = false;
            m_protocolLost = Host::LostNothing;
            m_protocolLostReset = false;
            break;
        default: break;
        }
        m_hostBytes -= front.cost;
        m_hostCalls.pop_front();
        calls.push_back(std::move(encoded));
    }
    return calls;
}

bool MuxPane::hasHostCalls(std::uint64_t window) const
{
    std::lock_guard lock(m_hostMutex);
    return m_window == window && !m_hostCalls.empty();
}


void MuxPane::answerWithoutWindow(std::string_view encoded)
{
    // Only these get an answer: the others go undecoded, a clipboard write
    // or a download of many megabytes among them.
    if (encoded.empty()) return;
    switch (Host::Kind(std::uint8_t(encoded.front()))) {
    case Host::Kind::ClipboardRequest:
    case Host::Kind::TerminalProtocol:
    case Host::Kind::DesktopNotification:
    case Host::Kind::Report:
        break;
    default:
        return;
    }
    const auto call = Host::decode(encoded);
    if (!call) return;
    switch (call->kind) {
    case Host::Kind::ClipboardRequest: clipboardWithoutWindow(call->clipboard); break;
    case Host::Kind::TerminalProtocol: protocolWithoutWindow(call->number, call->text); break;
    case Host::Kind::DesktopNotification: notificationWithoutWindow(call->notification); break;
    case Host::Kind::Report: writeDirect(call->text); break;
    default: break;
    }
}

void MuxPane::bell()
{
    bool wake = false;
    {
        // One bell waits at a time, as a window rings one for a burst.
        std::lock_guard lock(m_hostMutex);
        if (!m_window || m_bellWaiting) return;
        m_bellWaiting = true;
        wake = pushLocked(WaitingCall{Host::Kind::Bell, Host::encode(Host::make(Host::Kind::Bell)), {}, 0});
    }
    if (wake) m_wake();
}

void MuxPane::flash()
{
    bool wake = false;
    {
        std::lock_guard lock(m_hostMutex);
        if (!m_window || m_flashWaiting) return;
        m_flashWaiting = true;
        wake = pushLocked(WaitingCall{Host::Kind::Flash, Host::encode(Host::make(Host::Kind::Flash)), {}, 0});
    }
    if (wake) m_wake();
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
    bool wake = false;
    {
        // Only the newest shape matters: the one call that waits takes it.
        std::lock_guard lock(m_hostMutex);
        m_pointerShape = shape;
        if (!m_window || m_pointerWaiting) return;
        m_pointerWaiting = true;
        wake = pushLocked(WaitingCall{Host::Kind::PointerShape, {}, {}, Host::CallCost});
    }
    if (wake) m_wake();
}

void MuxPane::userVarChanged(std::string_view name, std::string_view value)
{
    bool wake = false;
    {
        std::lock_guard lock(m_hostMutex);
        const auto it = m_userVars.find(std::string(name));
        // Shells set the same values at every prompt.
        if (it != m_userVars.end() && it->second == value) return;
        if (it == m_userVars.end() && m_userVars.size() >= MaxUserVars) return;
        m_userVars[std::string(name)] = value;
        if (!m_window || m_userVarsWaiting.contains(std::string(name))) return;
        m_userVarsWaiting.emplace(name);
        wake = pushLocked(WaitingCall{Host::Kind::UserVar, {}, std::string(name), Host::CallCost + name.size() + value.size()});
    }
    if (wake) m_wake();
}

void MuxPane::commandStarted(std::string_view command)
{
    std::string encoded = Host::encode(Host::make(Host::Kind::CommandStarted, {}, std::string(command)));
    bool wake = false;
    {
        // With the state that a new window gets, under one lock: setWindow()
        // sends the start again.
        std::lock_guard lock(m_hostMutex);
        m_command = command;
        m_commandRunning = true;
        m_commandStart = std::chrono::steady_clock::now();
        // A start that went before still has its end to come.
        m_commandForwarded = queueLocked(std::move(encoded), Host::Kind::CommandStarted, Fit::Queue, &wake) || m_commandForwarded;
    }
    if (wake) m_wake();
}

void MuxPane::commandFinished(int exitCode)
{
    bool wake = false;
    {
        std::lock_guard lock(m_hostMutex);
        Host::Call call = Host::make(Host::Kind::CommandFinished, {}, {}, exitCode);
        // Measured here: the window may get the start and the end together.
        call.value = std::uint64_t(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - m_commandStart).count());
        m_commandRunning = false;
        m_command.clear();
        // An end alone would end a command that, for the window, never ran.
        // The end of a start that went always goes: one for each start.
        if (std::exchange(m_commandForwarded, false)) (void)queueLocked(Host::encode(call), Host::Kind::CommandFinished, Fit::Always, &wake);
    }
    if (wake) m_wake();
}

void MuxPane::regionScrolled(const AfterglowVt::RegionScroll& scroll)
{
    // Reader thread, m_mutex held. Kept while no window shows the pane too:
    // the first frame of the next window leaves them out.
    m_scrolls.add(m_terminal.modes().altScreen, scroll);
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
    if (forward(call) != Forward::Queued) notificationWithoutWindow(request);
}

void MuxPane::notificationWithoutWindow(const AfterglowVt::NotificationRequest& request)
{
    // No notification is on screen: a program that asks which are gets an
    // empty list, in the reply of a window. The others are dropped.
    if (request.kind != AfterglowVt::NotificationRequest::Kind::Alive) return;
    writeDirect(request.aliveReply());
}

bool MuxPane::clipboardRequest(const AfterglowVt::ClipboardRequest& request)
{
    // The window asks the user and answers, as for a terminal of its own.
    Host::Call call = Host::make(Host::Kind::ClipboardRequest);
    call.clipboard = request;
    if (forward(call) != Forward::Queued) clipboardWithoutWindow(request);
    return true;
}

void MuxPane::clipboardWithoutWindow(const AfterglowVt::ClipboardRequest& request)
{
    // No window has a clipboard to give: the request gets the answer of a
    // denial.
    if (const std::string refusal = request.refusal("EPERM"); !refusal.empty()) writeDirect(refusal);
}

namespace {
/// What a gap holds for drag and drop after it also took a command of
/// protocol @p code, which the program wrote after the others: the window's
/// rule (Pane.cpp).
int lostAfter(int lost, int code)
{
    return code == 72 ? Host::LostPackets : code == 0 ? Host::LostReset : lost;
}
} // namespace

void MuxPane::terminalProtocol(int code, std::string_view payload)
{
    // The window runs the transfer or the drag, and answers the queries too:
    // the program then hears of one terminal. A command is small
    // (TransferMaxCommand, DndMaxCommand).
    std::string encoded = Host::encode(Host::make(Host::Kind::TerminalProtocol, {}, std::string(payload), code));
    bool wake = false;
    bool noWindow = false;
    {
        // Decided under one lock: a window may come or go meanwhile.
        std::lock_guard lock(m_hostMutex);
        if (!m_window) {
            noWindow = true;
        } else if (m_protocolLostWaiting || !queueLocked(std::move(encoded), Host::Kind::TerminalProtocol, Fit::Queue, &wake)) {
            // The window drops the commands of a gap and ends what they
            // belonged to. The gap lasts until its call goes: the commands
            // until then go into it.
            if (!m_protocolLostWaiting) {
                m_protocolLost = Host::LostNothing;
                m_protocolLostReset = false;
                wake = pushLocked(WaitingCall{Host::Kind::ProtocolLost, {}, {}, Host::CallCost});
                m_protocolLostWaiting = true;
            }
            m_protocolLost = lostAfter(m_protocolLost, code);
            m_protocolLostReset = m_protocolLostReset || code == 0;
        }
    }
    if (wake) m_wake();
    if (noWindow) protocolWithoutWindow(code, payload);
}

void MuxPane::protocolWithoutWindow(int code, std::string_view payload)
{
    if (code == 72) {
        // A program asks which drag and drop types the terminal supports. It
        // gets the answer of a window: a client may attach before a drag.
        if (const std::string reply = AfterglowProto::dndQueryReply(payload); !reply.empty()) {
            writeDirect(reply);
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
        for (const std::string& packet : AfterglowProto::writeDnd(std::move(error), "EPERM")) writeDirect(packet);
        return;
    }
    if (code != 5113 || payload.size() > AfterglowProto::TransferMaxCommand) return;
    // A transfer starts with ac=send or ac=receive and waits for the user's
    // answer. Without a window no user answers: it is the answer of a
    // denial. A session with q=2 asked for no status, errors included. The
    // commands of a transfer that a window ran, which went, get none: the
    // program learns of the end when its terminal answers it no more, as for
    // a window that closed.
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
    writeDirect(AfterglowProto::encodeTransfer(status));
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
    if (atPrompt) writeDirect("\x0c");
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
