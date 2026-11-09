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

void MuxPane::desktopNotification(const AfterglowVt::NotificationRequest& request)
{
    // No notification is on screen: a program that asks which are gets an
    // empty list, in the reply of a window. The others are dropped.
    if (request.kind != AfterglowVt::NotificationRequest::Kind::Alive) return;
    writeToPty("\x1b]99;i=" + (request.id.empty() ? std::string("0") : request.id) + ":p=alive;\x1b\\");
}

bool MuxPane::clipboardRequest(const AfterglowVt::ClipboardRequest& request)
{
    using AfterglowVt::ClipboardAssembler;
    // The daemon has no clipboard, and the attach stream carries none yet:
    // the request gets the answer of a denial. A write is dropped.
    if (request.kind == AfterglowVt::ClipboardRequest::Kind::Write) {
        writeToPty(ClipboardAssembler::writeStatus("EPERM", request.id));
    } else if (request.osc52) {
        writeToPty(request.osc52Reply({}));
    } else {
        writeToPty(ClipboardAssembler::readStatus("EPERM", request));
    }
    return true;
}

void MuxPane::terminalProtocol(int code, std::string_view payload)
{
    if (code == 72) {
        // A program asks which drag and drop types the terminal supports. It
        // gets the answer of a window: a client may attach before a drag.
        if (const std::string reply = AfterglowProto::dndQueryReply(payload); !reply.empty()) {
            writeToPty(reply);
            return;
        }
        // A drag starts from an offer (t=o without x=1 or x=2) while the
        // user holds the mouse button. The attach stream carries no drag
        // yet, so the offer gets the window's EPERM. A drop never arrives, so
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
    // answer. The attach stream carries no transfer yet: it is the answer of
    // a denial. A session with
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
    // The write refused it for a full queue, or the child side is gone and
    // its exit follows.
    return m_exited ? Input::Gone : Input::Full;
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
