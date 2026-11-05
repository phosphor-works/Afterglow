// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <termios.h>
#include <vector>

namespace Aglow {

/// One thing that the terminal sent: an escape code without its introducer
/// and terminator, a key, or text.
struct Input
{
    enum class Type { Text, Osc, Apc, Dcs, Csi, Interrupt, Escape };
    Type type = Type::Text;
    std::string data;
};

/// Splits the bytes that the terminal sends into Input, across reads.
class InputParser
{
public:
    void feed(std::string_view bytes, std::vector<Input>& out);
    /// A lone ESC that no other byte followed: the Esc key.
    void timeout(std::vector<Input>& out);
    bool waitingAfterEscape() const { return m_state == State::Escape; }

private:
    enum class State { Ground, Escape, Csi, String, StringEscape };
    void flushText(std::vector<Input>& out);
    State m_state = State::Ground;
    Input::Type m_string = Input::Type::Osc;
    std::string m_buffer;
    std::string m_text;
    bool m_overflow = false;
};

/// The controlling terminal, in raw mode while the object lives. Writes are
/// queued and go out as the terminal takes them, so that a program never
/// blocks on a write while the terminal waits for it to read a reply.
class Tty
{
public:
    Tty() = default;
    ~Tty();
    Tty(const Tty&) = delete;
    Tty& operator=(const Tty&) = delete;

    /// Opens /dev/tty and puts it in raw mode: no echo, no line editing, and
    /// Ctrl-C arrives as a key. SIGINT, SIGTERM, SIGHUP and SIGWINCH are caught.
    bool open(std::string& error);
    /// Leaves raw mode. Idempotent; the destructor calls it.
    void restore();
    int fd() const { return m_fd; }

    void write(std::string_view bytes) { m_out += bytes; }
    /// Sets the modes that a command turns on (@p set) and the bytes that
    /// turn them off (@p reset). restore() writes @p reset, also when the
    /// command ends with an exception.
    void enterModes(std::string_view set, std::string reset)
    {
        m_out += set;
        m_reset = std::move(reset);
    }
    std::size_t pending() const { return m_out.size(); }
    /// Writes what the terminal takes and reads what it sends, for up to
    /// @p timeoutMs (0: once, -1: until something happens). False when the
    /// terminal is gone.
    bool turn(int timeoutMs, std::vector<Input>& inputs);
    /// Writes everything queued, for up to @p timeoutMs. Inputs that arrive
    /// meanwhile are kept for the next turn().
    bool flush(int timeoutMs);
    winsize size() const;
    /// Catches SIGINT, SIGTERM, SIGHUP and SIGWINCH without a terminal in
    /// raw mode, so that a command can stop between two writes.
    static void watchSignals();
    /// A signal caught since the last call (SIGINT, SIGTERM or SIGHUP), or 0.
    int takeSignal();
    /// The window changed size since the last call.
    bool takeResize();

private:
    int m_fd = -1;
    bool m_raw = false;
    termios m_saved { };
    std::string m_out;
    std::string m_reset;
    InputParser m_parser;
    std::vector<Input> m_kept;
    long long m_escapeSince = -1;
};

/// Milliseconds of a monotonic clock.
long long nowMs();
/// Prints @p text on the terminal as text: control characters become '?',
/// so a file name cannot move the cursor or change colours.
std::string printable(std::string_view text);

} // namespace Aglow
