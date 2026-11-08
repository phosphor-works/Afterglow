// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "MuxPane.h"

#include <AfterglowProto/DragDrop.h>
#include <AfterglowProto/Transfer.h>

#include <array>

namespace AfterglowMux {

MuxPane::MuxPane(std::uint32_t id, const AfterglowVt::TerminalOptions& options, const PaneSize& size,
                 std::function<void()> exited)
    : m_id(id)
    , m_exitedCallback(std::move(exited))
    , m_terminal(size.cols, size.rows, *this, options)
{
    // Before the program starts: one that queries the cell size at once
    // (fastfetch with kitty graphics) gets the client's size, not zero.
    m_terminal.resize(size.cols, size.rows, size.cellWidth, size.cellHeight);
}

MuxPane::~MuxPane()
{
    stop();
}

bool MuxPane::start(AfterglowPty::SpawnOptions options, std::string launchedProgram, std::string* error)
{
    {
        std::lock_guard lock(m_mutex);
        options.cols = m_terminal.cols();
        options.rows = m_terminal.rows();
        options.pixelWidth = m_terminal.cols() * m_terminal.cellWidth();
        options.pixelHeight = m_terminal.rows() * m_terminal.cellHeight();
    }
    m_launchedProgram = std::move(launchedProgram);
    // Deferred: m_pty must be set before the reader thread can call
    // writeToPty() with the answer to a query.
    options.deferStart = true;
    m_pty = AfterglowPty::Pty::spawn(std::move(options), *this, error);
    if (m_pty && !m_pty->start(error)) {
        m_pty->stop();
        m_pty.reset();
    }
    if (!m_pty) m_exited = true;
    return m_pty != nullptr;
}

void MuxPane::stop()
{
    if (m_pty) m_pty->stop();
}

std::string MuxPane::foregroundJob() const
{
    if (!m_pty || m_exited) return {};
    if (std::string job = m_pty->foregroundProcess(); !job.empty()) return job;
    return m_launchedProgram;
}

std::string MuxPane::title() const
{
    std::lock_guard lock(m_mutex);
    return m_title;
}

MuxPane::Missed MuxPane::missed() const
{
    std::lock_guard lock(m_mutex);
    return m_missed;
}

void MuxPane::writeToPty(std::string_view bytes)
{
    // A reply that the PTY's input queue refuses is lost, as in a window:
    // the program reads its input too slowly to wait for it.
    if (m_pty) (void)m_pty->write(bytes);
}

void MuxPane::titleChanged(std::string_view title)
{
    m_title = title;
}

void MuxPane::workingDirectoryChanged(std::string_view directory)
{
    m_workingDirectory = directory;
}

void MuxPane::bell()
{
    ++m_missed.bells;
}

void MuxPane::clipboardWrite(std::string_view, std::string_view)
{
    ++m_missed.clipboardWrites;
}

void MuxPane::notify(std::string_view, std::string_view)
{
    ++m_missed.notifications;
}

void MuxPane::desktopNotification(const AfterglowVt::NotificationRequest& request)
{
    if (request.kind == AfterglowVt::NotificationRequest::Kind::Show) ++m_missed.notifications;
}

bool MuxPane::clipboardRequest(const AfterglowVt::ClipboardRequest& request)
{
    using AfterglowVt::ClipboardAssembler;
    // No client means no clipboard and no user to ask: the request gets the
    // answer of a denial. A write is counted for the next client and dropped.
    if (request.kind == AfterglowVt::ClipboardRequest::Kind::Write) {
        ++m_missed.clipboardWrites;
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
        // Without a client no drop arrives and no drag starts, as when the
        // user does neither, so the other commands need no answer.
        if (const std::string reply = AfterglowProto::dndQueryReply(payload); !reply.empty()) writeToPty(reply);
        return;
    }
    if (code != 5113 || payload.size() > AfterglowProto::TransferMaxCommand) return;
    // A transfer starts with ac=send or ac=receive and waits for the user's
    // answer: without a client, it is the answer of a denial.
    const auto fields = AfterglowProto::decodeTransfer(payload);
    if (!fields) return;
    const auto action = fields->find("ac");
    const auto id = fields->find("id");
    if (action == fields->end() || id == fields->end() || (action->second != "send" && action->second != "receive")) return;
    const std::array<std::pair<std::string_view, std::string_view>, 3> status{{
        {"ac", "status"},
        {"id", id->second},
        {"st", "EPERM"},
    }};
    writeToPty(AfterglowProto::encodeTransfer(status));
}

void MuxPane::ptyData(std::string_view bytes)
{
    std::lock_guard lock(m_mutex);
    m_terminal.feed(bytes);
}

void MuxPane::ptyClosed(int)
{
    m_exited = true;
    m_exitedCallback();
}

} // namespace AfterglowMux
