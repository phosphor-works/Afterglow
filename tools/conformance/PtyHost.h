// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// What afterglow_headless and afterglow_tuifuzz share: the host that sends
// the terminal's replies to the program through the pseudo-terminal, and the
// screen as text.

#pragma once

#include <AfterglowProto/Text.h>
#include <AfterglowVt/Terminal.h>

#include <sys/ioctl.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace AfterglowConformance {

/// Queues what the terminal writes back (device reports, checksums) and
/// sends it to the pty master as it takes it, without blocking.
class PtyHost : public AfterglowVt::ITerminalHost
{
public:
    explicit PtyHost(int fd)
        : m_fd(fd)
    {
    }
    /// Another master, for a program that started again.
    void setFd(int fd)
    {
        m_fd = fd;
        m_pending.clear();
        m_offset = 0;
    }
    void writeToPty(std::string_view bytes) override
    {
        if (bytes.size() > MaxPending - (m_pending.size() - m_offset)) {
            overflowed = true;
            return;
        }
        if (!pending()) m_progress = std::chrono::steady_clock::now();
        m_pending.erase(0, m_offset);
        m_offset = 0;
        m_pending.append(bytes);
    }
    bool pending() const { return m_offset < m_pending.size(); }
    void flush()
    {
        if (!pending()) return;
        const ssize_t n = ::write(m_fd, m_pending.data() + m_offset, m_pending.size() - m_offset);
        if (n > 0) {
            m_offset += std::size_t(n);
            m_progress = std::chrono::steady_clock::now();
            if (!pending()) { m_pending.clear(); m_offset = 0; }
        } else if (n < 0 && errno == EIO) {
            // The program closed its side after a last query: nobody reads
            // the reply. The read path sees the hangup and ends the run.
            m_pending.clear();
            m_offset = 0;
        } else if (n == 0 || (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)) failed = true;
    }
    bool stalled() const
    {
        return pending() && std::chrono::steady_clock::now() - m_progress > std::chrono::seconds(5);
    }
    bool failed = false;
    bool overflowed = false;

    // Size changes asked for by the program (DECCOLM, XTWINOPS) are granted,
    // as xterm does with window ops allowed; the pty follows.
    bool requestResize(int cols, int rows) override
    {
        if (cols < MinCols || cols > 1000 || rows < 1 || rows > 500) return false;
        return setPtySize(m_fd, cols, rows);
    }

    // A 1920x1080 display, so maximum sizes and screen reports have answers.
    bool screenSize(int& width, int& height) override
    {
        width = 1920;
        height = 1080;
        return true;
    }

    static bool setPtySize(int fd, int cols, int rows)
    {
        winsize ws{};
        ws.ws_col = std::uint16_t(cols);
        ws.ws_row = std::uint16_t(rows);
        ws.ws_xpixel = std::uint16_t(cols * CellWidth);
        ws.ws_ypixel = std::uint16_t(rows * CellHeight);
        return ::ioctl(fd, TIOCSWINSZ, &ws) == 0;
    }

    static constexpr int CellWidth = 10;
    static constexpr int CellHeight = 20;
    /// The terminal core keeps at least two columns; the pty must agree.
    static constexpr int MinCols = 2;

private:
    static constexpr std::size_t MaxPending = 1024 * 1024;
    int m_fd;
    std::string m_pending;
    std::size_t m_offset = 0;
    std::chrono::steady_clock::time_point m_progress{};
};

/// The screen as the user reads it, so that text in CJK or with combining
/// marks matches: wide tails are skipped, cluster marks kept.
inline std::string screenText(const AfterglowVt::Terminal& terminal)
{
    std::string out;
    const AfterglowVt::Screen& screen = terminal.screen();
    for (int y = 0; y < screen.rows(); ++y) {
        const AfterglowVt::Row& row = screen.row(y);
        for (int x = 0; x < screen.cols(); ++x) {
            const AfterglowVt::Cell& cell = row.at(x);
            if (cell.isTail()) {
                continue;
            }
            if (cell.isMulticell() && (cell.multicellDx() != 0 || cell.multicellDy() != 0)) {
                out += ' '; // a sized block's text is in its top-left cell
                continue;
            }
            AfterglowProto::appendUtf8(out, cell.codepoint() == 0 ? U' ' : cell.codepoint());
            for (char32_t extra : screen.graphemeExtra(row, x)) {
                AfterglowProto::appendUtf8(out, extra);
            }
        }
        out += '\n';
    }
    return out;
}

} // namespace AfterglowConformance
