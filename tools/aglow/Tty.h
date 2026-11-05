// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <termios.h>
#include <utility>
#include <vector>

namespace Aglow {

/// One thing that the terminal sent: text, a key, or an escape code. An OSC,
/// APC or DCS string comes without its introducer and terminator. A CSI
/// keeps its parameters and its final byte ("?62;22c").
struct Input
{
    enum class Type {
        Text, ///< typed text, also Enter as "\r"
        Osc,
        Apc,
        Dcs,
        Csi, ///< a reply, or a key that sends CSI (the arrows, F5 and up)
        Interrupt, ///< Ctrl+C
        Escape, ///< the Esc key
        Key, ///< a key that sends SS3 (F1 to F4, the arrows in application mode), as "OP"
        Paste, ///< text that was pasted, which no prompt takes as an answer
    };
    Type type = Type::Text;
    std::string data;
};

/// Splits the bytes that the terminal sends into Input, across reads.
class InputParser
{
public:
    void feed(std::string_view bytes, std::vector<Input>& out);
    /// The bytes since the last ESC may be a key (Esc, or Alt and a key)
    /// as well as the start of an escape code: no other byte followed them.
    /// They are that key when none follows in a short time: timeout().
    void timeout(std::vector<Input>& out);
    bool waitingAfterEscape() const;
    /// Forgets a code or paste cut short and the keyboard mode, as for a
    /// new terminal session. The settings of allowLongOsc() stay.
    void reset();
    /// The terminal reports keys in kitty's keyboard protocol: the Esc key
    /// arrives as CSI 27 u, so a lone ESC is always the start of an escape code.
    void setKeysUnambiguous(bool on) { m_unambiguous = on; }
    /// Takes OSC strings that start with @p prefix up to @p limit bytes.
    /// Every other string is dropped past 1 MiB.
    void allowLongOsc(std::string prefix, std::size_t limit)
    {
        m_longPrefix = std::move(prefix);
        m_longLimit = limit;
    }
    /// The keyboard protocol flags that the terminal said are on (CSI ?
    /// flags u) since the last call, or -1.
    int takeKeyboardReply() { return std::exchange(m_keyboardReply, -1); }

private:
    enum class State { Ground, Escape, Csi, Ss3, String, StringEscape, Paste };
    void flushText(std::vector<Input>& out);
    void endCsi(std::vector<Input>& out);
    State m_state = State::Ground;
    Input::Type m_string = Input::Type::Osc;
    char m_introducer = 0;
    std::string m_buffer;
    std::string m_text;
    bool m_overflow = false;
    bool m_unambiguous = false;
    int m_keyboardReply = -1;
    std::size_t m_pasteMatch = 0;
    std::string m_longPrefix;
    std::size_t m_longLimit = 0;
};

/// The controlling terminal, in raw mode while the object lives. Writes are
/// queued and go out as the terminal takes them, so that a program never
/// blocks on a write while the terminal waits for it to read a reply.
///
/// While it is open, the terminal sends pastes in brackets and, when it
/// supports kitty's keyboard protocol, the Esc key as an escape code of its
/// own. SIGPIPE is ignored: a terminal that closes gives EPIPE, and the
/// program still restores the terminal.
class Tty
{
public:
    Tty() = default;
    ~Tty();
    Tty(const Tty&) = delete;
    Tty& operator=(const Tty&) = delete;

    /// Opens /dev/tty and puts it in raw mode: no echo, no line editing, and
    /// Ctrl-C arrives as a key. SIGINT, SIGTERM, SIGHUP and SIGWINCH are
    /// caught, unless the program started with them ignored.
    bool open(std::string& error);
    /// Leaves raw mode. True when all queued output and the bytes that turn
    /// the modes off were written, and the terminal is back in its mode. When
    /// the terminal does not take the queued output in time, the rest is
    /// dropped, so that the reset still goes. Idempotent: a second call gives
    /// the result of the first. The destructor calls it.
    bool restore();

    void write(std::string_view bytes) { m_out += bytes; }
    /// Asks the terminal for its primary device attributes (DA1). The answer
    /// comes as Input::Type::Csi "?...c", after the answers to everything
    /// written before. A command asks through here, never with write(), so
    /// that restore() knows which answers are still on their way.
    void queryAttributes() { query(false); }
    /// Sets the modes that a command turns on (@p set) and the bytes that
    /// turn them off (@p reset). restore() writes @p reset, also when the
    /// command ends with an exception. After a stop, the shell may have
    /// changed modes: @p resume turns those of @p set back on that a second
    /// write of @p set would change otherwise (a title push). The keyboard
    /// mode is set after @p resume, so that it goes to the stack of the
    /// screen that @p resume switches to.
    void enterModes(std::string_view set, std::string reset, std::string resume = {});
    /// The bytes queued and not written yet.
    std::size_t pending() const { return m_out.size() - m_outAt; }
    /// Writes what the terminal takes and reads what it sends, for up to
    /// @p timeoutMs (0: once, -1: until something happens). False when the
    /// terminal is gone.
    bool turn(int timeoutMs, std::vector<Input>& inputs);
    /// Takes an OSC 52 reply as large as a clipboard write (OSC 5522's
    /// limit, in base64). Without it, every escape code stops at 1 MiB.
    void allowClipboardReply();
    /// restore() asks the terminal for its attributes after all other
    /// output and reads up to the answer: the replies that are still on
    /// their way are read, not typed into the shell. For a command whose
    /// terminal may still be sending. Without it, restore() reads up to the
    /// answers of the DA1 queries that have none yet. It holds until
    /// restore(): a Tty that opens again starts without it.
    void drainOnRestore() { m_drain = true; }
    winsize size() const;
    /// Catches SIGINT, SIGTERM, SIGHUP and SIGWINCH without a terminal in
    /// raw mode, so that a command can stop between two writes.
    static void watchSignals();
    /// A signal caught since the last call (SIGINT, SIGTERM or SIGHUP), or 0.
    int takeSignal();
    /// A signal was caught that takeSignal() has not taken yet: for work
    /// that runs long between two turns, and stops on it.
    static bool signalPending();
    /// The window changed size, or the program went on after a stop, since
    /// the last call. Either way the command draws its screen again: the
    /// shell drew over it, and a stopped program gets no SIGWINCH.
    bool takeResize();

private:
    /// One poll: writes, then reads into @p inputs.
    bool step(int timeoutMs, std::vector<Input>& inputs);
    /// Writes everything queued, for up to @p timeoutMs. Inputs that arrive
    /// meanwhile are kept for the next turn().
    bool flush(int timeoutMs);
    /// Reads until the DA1 queries have their answers. The terminal gets
    /// @p timeoutMs for each query from when it was written.
    bool drain(int timeoutMs);
    /// Queues a DA1 query. @p own: the Tty's own, whose answer no command sees.
    void query(bool own);
    /// Takes the answers to the Tty's own queries out of @p inputs, from @p from on.
    void takeReplies(std::vector<Input>& inputs, std::size_t from);

    int m_fd = -1;
    bool m_raw = false;
    bool m_drain = false;
    /// restore() has queued the reset: a continue sets the raw modes again,
    /// and the modes of the command stay off.
    bool m_restoring = false;
    /// A DA1 query that has no answer yet. The terminal answers in order.
    struct Query
    {
        bool own = false;
        unsigned long long end = 0; ///< m_written once its last byte is written
        long long writtenAt = -1;
    };
    std::deque<Query> m_queries;
    unsigned long long m_written = 0; ///< the bytes written since open()
    std::string m_resume;
    bool m_restored = true;
    termios m_saved { };
    termios m_rawModes { };
    std::string m_out;
    std::size_t m_outAt = 0; ///< the bytes of m_out before it are written
    std::string m_reset;
    InputParser m_parser;
    std::vector<Input> m_kept;
    long long m_escapeSince = -1;
};

/// Milliseconds of a monotonic clock.
long long nowMs();
/// Prints @p text on the terminal as text: control characters and the
/// controls of bidirectional text become '?', so a file name cannot move the
/// cursor, change colours or reorder the text around it.
std::string printable(std::string_view text);
/// The cells that @p cp takes on a terminal: 0 for a combining mark, 2 for a
/// wide character (East Asian, most emoji), 1 for the others.
int cellWidth(char32_t cp);

} // namespace Aglow
