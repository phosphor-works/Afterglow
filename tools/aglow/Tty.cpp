// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "Tty.h"

#include <AfterglowProto/Clipboard.h>
#include <AfterglowProto/Text.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <iterator>
#include <poll.h>
#include <unistd.h>

namespace Aglow {

namespace {

// Escape codes from the terminal are bounded: the largest reply of the
// protocols here is a few KiB. A longer one is dropped whole.
constexpr std::size_t MaxEscape = 1024 * 1024;
// A lone ESC is the Esc key when no byte follows it in this time.
constexpr long long EscapeKeyMs = 50;
// restore() waits this long for the terminal to take the queued output, and
// then as long again for the reset and for the reply that ends the drain.
constexpr int RestoreMs = 2000;
// Written bytes leave the queue in one move once there are this many and
// they are at least half of it, so that each byte moves at most once.
constexpr std::size_t CompactAt = 64 * 1024;
// Kitty's keyboard protocol, flag 1 (disambiguate): Esc, Alt and Ctrl keys
// arrive as CSI ... u. Terminals without it ignore these codes.
constexpr std::string_view PushKeyboard = "\x1b[>1u";
constexpr std::string_view PopKeyboard = "\x1b[<u";

int signalPipe[2] = { -1, -1 };
std::atomic<int> caughtSignal { 0 };
std::atomic<bool> resized { false };
std::atomic<bool> continued { false };

void onSignal(int number)
{
    const int saved = errno;
    if (number == SIGWINCH) {
        resized = true;
    } else if (number == SIGCONT) {
        continued = true;
    } else {
        caughtSignal = number;
    }
    if (signalPipe[1] >= 0) {
        const char byte = 1;
        [[maybe_unused]] const auto n = ::write(signalPipe[1], &byte, 1);
    }
    errno = saved;
}

void catchSignals()
{
    if (signalPipe[0] >= 0) {
        return;
    }
    if (::pipe2(signalPipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        signalPipe[0] = signalPipe[1] = -1;
    }
    struct sigaction action { };
    action.sa_handler = onSignal;
    // The self-pipe wakes poll(): every other call goes on where it was.
    action.sa_flags = SA_RESTART;
    sigemptyset(&action.sa_mask);
    for (const int number : { SIGINT, SIGTERM, SIGHUP, SIGWINCH, SIGCONT }) {
        // A signal that the program started with ignored (nohup) stays ignored.
        struct sigaction inherited { };
        if (::sigaction(number, nullptr, &inherited) == 0 && inherited.sa_handler == SIG_IGN) {
            continue;
        }
        ::sigaction(number, &action, nullptr);
    }
    // A terminal that closes must not kill the program before it restores the tty.
    std::signal(SIGPIPE, SIG_IGN);
}

bool allDigits(std::string_view text)
{
    return std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// The number before the first ':' of a field of a key code, or -1.
int keyField(std::string_view field)
{
    field = field.substr(0, std::min(field.find(':'), field.size()));
    if (field.empty() || field.size() > 9 || !allDigits(field)) {
        return -1;
    }
    int value = 0;
    for (const char c : field) {
        value = value * 10 + (c - '0');
    }
    return value;
}

std::vector<std::string_view> fields(std::string_view text)
{
    std::vector<std::string_view> out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const auto end = std::min(text.find(';', start), text.size());
        out.push_back(text.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

// Ctrl alone, with Caps Lock and Num Lock (64 and 128) left out.
bool onlyCtrl(int modifiers) { return modifiers >= 1 && ((modifiers - 1) & ~(64 | 128)) == 4; }

} // namespace

long long nowMs()
{
    timespec now { };
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

std::string printable(std::string_view text)
{
    auto bidi = [](char32_t cp) {
        return cp == 0x061C || cp == 0x200E || cp == 0x200F || (cp >= 0x202A && cp <= 0x202E)
            || (cp >= 0x2066 && cp <= 0x2069) || cp == 0x2028 || cp == 0x2029;
    };
    std::string out;
    std::size_t i = 0;
    while (i < text.size()) {
        char32_t cp = 0;
        const std::size_t start = i;
        if (!AfterglowProto::nextUtf8(text, i, cp)) {
            out += '?';
            ++i;
            continue;
        }
        if (AfterglowProto::isControl(cp) || bidi(cp)) {
            out += '?';
        } else {
            out.append(text.substr(start, i - start));
        }
    }
    return out;
}

int cellWidth(char32_t cp)
{
    // Combining marks and zero width characters.
    if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) || (cp >= 0x1DC0 && cp <= 0x1DFF)
        || (cp >= 0x20D0 && cp <= 0x20FF) || (cp >= 0xFE20 && cp <= 0xFE2F) || (cp >= 0x200B && cp <= 0x200D)
        || (cp >= 0xFE00 && cp <= 0xFE0F)) {
        return 0;
    }
    // East Asian wide and full width characters, and the emoji blocks.
    if ((cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) || (cp >= 0x3041 && cp <= 0x33FF)
        || (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF)
        || (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F)
        || (cp >= 0xFF00 && cp <= 0xFF60) || (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F)
        || (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD)) {
        return 2;
    }
    return 1;
}

void InputParser::flushText(std::vector<Input>& out)
{
    if (!m_text.empty()) {
        out.push_back({ Input::Type::Text, std::move(m_text) });
        m_text.clear();
    }
}

bool InputParser::waitingAfterEscape() const
{
    if (m_unambiguous) {
        return false;
    }
    // ESC, or ESC and an introducer with nothing after it: Alt+[ sends ESC [.
    return m_state == State::Escape
        || ((m_state == State::Csi || m_state == State::Ss3 || m_state == State::String) && m_buffer.empty() && !m_overflow);
}

void InputParser::endCsi(std::vector<Input>& out)
{
    std::string data = std::move(m_buffer);
    m_buffer.clear();
    m_state = State::Ground;
    if (data == "200~") {
        m_state = State::Paste;
        m_pasteMatch = 0;
        m_overflow = false;
        return;
    }
    // The answer to CSI ? u: the terminal speaks kitty's keyboard protocol.
    if (data.size() >= 2 && data.front() == '?' && data.back() == 'u' && allDigits(std::string_view(data).substr(1, data.size() - 2))) {
        m_keyboardReply = true;
        return;
    }
    // Esc and Ctrl+C in kitty's keyboard protocol (CSI 27 u, CSI 99;5 u) and
    // in xterm's modifyOtherKeys (CSI 27;5;99 ~).
    int code = -1;
    int modifiers = 1;
    if (data.back() == 'u') {
        const auto f = fields(std::string_view(data).substr(0, data.size() - 1));
        code = keyField(f[0]);
        modifiers = f.size() > 1 ? keyField(f[1]) : 1;
    } else if (data.back() == '~' && data.starts_with("27;")) {
        const auto f = fields(std::string_view(data).substr(0, data.size() - 1));
        if (f.size() == 3) {
            modifiers = keyField(f[1]);
            code = keyField(f[2]);
        }
    }
    if (code == 27) {
        out.push_back({ Input::Type::Escape, {} });
    } else if (code == 99 && onlyCtrl(modifiers)) {
        out.push_back({ Input::Type::Interrupt, {} });
    } else {
        out.push_back({ Input::Type::Csi, std::move(data) });
    }
}

void InputParser::feed(std::string_view bytes, std::vector<Input>& out)
{
    static constexpr std::string_view PasteEnd = "\x1b[201~";
    auto appendPaste = [&](std::string_view text) {
        if (m_buffer.size() + text.size() > MaxEscape) {
            m_overflow = true;
        } else {
            m_buffer += text;
        }
    };
    // A byte that ends what came before and starts something else goes
    // through the loop again: --i, then the loop's ++i.
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const char c = bytes[i];
        const auto byte = static_cast<unsigned char>(c);
        switch (m_state) {
        case State::Ground:
            if (byte == 0x1b) {
                flushText(out);
                m_state = State::Escape;
            } else if (byte == 0x03) {
                flushText(out);
                out.push_back({ Input::Type::Interrupt, {} });
            } else {
                m_text += c;
            }
            break;
        case State::Escape:
            m_buffer.clear();
            m_overflow = false;
            m_introducer = c;
            if (c == '[') {
                m_state = State::Csi;
            } else if (c == 'O') {
                m_state = State::Ss3;
            } else if (c == ']' || c == '_' || c == 'P') {
                m_string = c == ']' ? Input::Type::Osc : c == '_' ? Input::Type::Apc : Input::Type::Dcs;
                m_state = State::String;
            } else {
                // Alt and a key, or Esc and then a key: the Esc key first.
                out.push_back({ Input::Type::Escape, {} });
                m_state = State::Ground;
                --i;
            }
            break;
        case State::Csi:
            if (byte == 0x1b) {
                // An ESC ends a code that was cut short and starts the next one.
                m_buffer.clear();
                m_state = State::Escape;
            } else if (byte == 0x18 || byte == 0x1a) {
                m_buffer.clear();
                m_state = State::Ground;
            } else if (byte == 0x03) {
                out.push_back({ Input::Type::Interrupt, {} });
            } else if (byte < 0x20 || byte == 0x7f) {
                // A control character inside a code is no part of it.
            } else {
                m_buffer += c;
                if (byte >= 0x40 && byte <= 0x7e) {
                    endCsi(out);
                } else if (m_buffer.size() > 256) {
                    m_buffer.clear();
                    m_state = State::Ground;
                }
            }
            break;
        case State::Ss3:
            if (byte >= 0x40 && byte <= 0x7e) {
                out.push_back({ Input::Type::Key, "O" + m_buffer + c });
                m_buffer.clear();
                m_state = State::Ground;
            } else if (byte >= 0x20 && byte < 0x40 && m_buffer.size() < 16) {
                m_buffer += c;
            } else {
                // Not a key: Alt+O, then this byte.
                out.push_back({ Input::Type::Escape, {} });
                m_text += 'O' + m_buffer;
                m_buffer.clear();
                m_state = State::Ground;
                --i;
            }
            break;
        case State::String:
            if (byte == 0x07 && m_string == Input::Type::Osc) {
                if (!m_overflow) {
                    out.push_back({ m_string, std::move(m_buffer) });
                }
                m_buffer.clear();
                m_state = State::Ground;
            } else if (byte == 0x1b) {
                m_state = State::StringEscape;
            } else if (byte == 0x18 || byte == 0x1a) {
                m_buffer.clear();
                m_state = State::Ground;
            } else if (m_buffer.size() < MaxEscape
                       || (m_string == Input::Type::Osc && m_buffer.size() < m_longLimit && m_buffer.starts_with(m_longPrefix))) {
                m_buffer += c;
            } else {
                m_overflow = true;
            }
            break;
        case State::StringEscape:
            // ST ends the string. Any other byte after ESC ends it too, and
            // starts an escape code: ESC ESC is no Esc key.
            if (!m_overflow) {
                out.push_back({ m_string, std::move(m_buffer) });
            }
            m_buffer.clear();
            m_state = State::Ground;
            if (byte == 0x1b) {
                m_state = State::Escape;
            } else if (c != '\\') {
                m_state = State::Escape;
                --i;
            }
            break;
        case State::Paste:
            if (c == PasteEnd[m_pasteMatch]) {
                if (++m_pasteMatch == PasteEnd.size()) {
                    if (!m_overflow) {
                        out.push_back({ Input::Type::Paste, std::move(m_buffer) });
                    }
                    m_buffer.clear();
                    m_pasteMatch = 0;
                    m_state = State::Ground;
                }
                break;
            }
            // The bytes that looked like the end were pasted text.
            appendPaste(PasteEnd.substr(0, m_pasteMatch));
            m_pasteMatch = c == PasteEnd[0] ? 1 : 0;
            if (m_pasteMatch == 0) {
                appendPaste(std::string_view(&c, 1));
            }
            break;
        }
    }
    if (m_state == State::Ground) {
        flushText(out);
    }
}

void InputParser::timeout(std::vector<Input>& out)
{
    if (!waitingAfterEscape()) {
        return;
    }
    out.push_back({ Input::Type::Escape, {} });
    if (m_state != State::Escape) {
        out.push_back({ Input::Type::Text, std::string(1, m_introducer) });
    }
    m_state = State::Ground;
}

Tty::~Tty() { restore(); }

bool Tty::open(std::string& error)
{
    m_fd = ::open("/dev/tty", O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (m_fd < 0) {
        error = std::string("Cannot open the controlling terminal: ") + std::strerror(errno);
        return false;
    }
    m_restored = false;
    if (::tcgetattr(m_fd, &m_saved) != 0) {
        error = std::string("The controlling terminal is not a terminal: ") + std::strerror(errno);
        return false;
    }
    catchSignals();
    termios raw = m_saved;
    // Output processing stays on, so that text written here ends lines with CR LF.
    raw.c_iflag &= ~tcflag_t(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    raw.c_lflag &= ~tcflag_t(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    raw.c_cflag &= ~tcflag_t(CSIZE | PARENB);
    raw.c_cflag |= CS8;
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    int result;
    do {
        result = ::tcsetattr(m_fd, TCSANOW, &raw);
    } while (result != 0 && errno == EINTR);
    if (result != 0) {
        error = std::string("Cannot set the terminal to raw mode: ") + std::strerror(errno);
        return false;
    }
    m_rawModes = raw;
    m_raw = true;
    // Pastes in brackets, so that a pasted line break answers no prompt, and
    // the keyboard protocol, with the query whose answer says that it is on.
    // The answer to DA1 comes after that answer, or alone without it.
    m_out += "\x1b[?2004h";
    m_out += PushKeyboard;
    m_out += "\x1b[?u\x1b[c";
    m_fences = 1;
    return true;
}

void Tty::enterModes(std::string_view set, std::string reset)
{
    // Kitty keeps a keyboard mode stack for each screen: set may switch to
    // the alternate one, which gets its own push.
    m_out += set;
    m_out += PushKeyboard;
    m_reset = std::string(PopKeyboard) + std::move(reset);
}

void Tty::allowClipboardReply()
{
    // "52;" and the selections, then the base64 of a whole clipboard write.
    m_parser.allowLongOsc("52;", (AfterglowProto::ClipboardAssembler::MaxWrite + 2) / 3 * 4 + 1024);
}

bool Tty::restore()
{
    if (m_fd < 0) {
        return m_restored;
    }
    bool ok = flush(RestoreMs);
    std::string reset;
    if (!ok) {
        // The terminal took no more in time. The rest goes, so that the
        // reset can, and ST ends an escape code that was cut short: in one,
        // the terminal would read the reset as part of it.
        m_out.clear();
        m_outAt = 0;
        reset = "\x1b\\";
    }
    reset += std::exchange(m_reset, {});
    if (m_raw) {
        reset += PopKeyboard;
        reset += "\x1b[?2004l";
    }
    m_out += reset;
    ok = flush(RestoreMs) && ok;
    if (ok && (m_drain || m_fences > 0)) {
        drain(RestoreMs);
    }
    if (m_raw) {
        // TCSAFLUSH: replies that arrive late are dropped, not typed into the shell.
        int result;
        do {
            result = ::tcsetattr(m_fd, TCSAFLUSH, &m_saved);
        } while (result != 0 && errno == EINTR);
        ok = result == 0 && ok;
        m_raw = false;
    }
    ::close(m_fd);
    m_fd = -1;
    m_restored = ok;
    return ok;
}

bool Tty::turn(int timeoutMs, std::vector<Input>& inputs)
{
    if (!m_kept.empty()) {
        inputs.insert(inputs.end(), std::make_move_iterator(m_kept.begin()), std::make_move_iterator(m_kept.end()));
        m_kept.clear();
        timeoutMs = 0;
    }
    if (m_parser.waitingAfterEscape()) {
        const long long left = std::max(0LL, EscapeKeyMs - (nowMs() - m_escapeSince));
        if (timeoutMs < 0 || timeoutMs > left) {
            timeoutMs = int(left);
        }
    }
    if (!step(timeoutMs, inputs)) {
        return false;
    }
    // Decided after the read: bytes that were queued meanwhile end the code
    // that the ESC started, however long the program took to read them.
    if (m_parser.waitingAfterEscape() && nowMs() - m_escapeSince >= EscapeKeyMs) {
        m_parser.timeout(inputs);
    }
    return true;
}

void Tty::takeReplies(std::vector<Input>& inputs, std::size_t from)
{
    if (m_parser.takeKeyboardReply()) {
        m_parser.setKeysUnambiguous(true);
    }
    // The answers to this Tty's own DA1 queries: the commands never see them.
    const auto own = std::remove_if(inputs.begin() + std::ptrdiff_t(from), inputs.end(), [&](const Input& input) {
        if (m_fences > 0 && input.type == Input::Type::Csi && input.data.starts_with('?') && input.data.ends_with('c')) {
            --m_fences;
            return true;
        }
        return false;
    });
    inputs.erase(own, inputs.end());
}

bool Tty::step(int timeoutMs, std::vector<Input>& inputs)
{
    pollfd fds[2] = { { m_fd, short(POLLIN | (pending() ? POLLOUT : 0)), 0 }, { signalPipe[0], POLLIN, 0 } };
    const int ready = ::poll(fds, signalPipe[0] >= 0 ? 2 : 1, timeoutMs);
    if (ready < 0) {
        return errno == EINTR;
    }
    if (fds[1].revents & POLLIN) {
        char drain[64];
        while (::read(signalPipe[0], drain, sizeof drain) > 0) {
        }
    }
    // Back from a stop: the shell set its own modes meanwhile.
    if (continued.exchange(false) && m_raw) {
        ::tcsetattr(m_fd, TCSANOW, &m_rawModes);
    }
    if (fds[0].revents & POLLOUT) {
        const ssize_t n = ::write(m_fd, m_out.data() + m_outAt, pending());
        if (n > 0) {
            m_outAt += std::size_t(n);
            if (m_outAt == m_out.size()) {
                m_out.clear();
                m_outAt = 0;
            } else if (m_outAt >= CompactAt && m_outAt * 2 >= m_out.size()) {
                m_out.erase(0, m_outAt);
                m_outAt = 0;
            }
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
            return false;
        }
    }
    if (fds[0].revents & POLLIN) {
        char buffer[65536];
        const ssize_t n = ::read(m_fd, buffer, sizeof buffer);
        if (n > 0) {
            const auto from = inputs.size();
            m_parser.feed(std::string_view(buffer, std::size_t(n)), inputs);
            if (m_parser.waitingAfterEscape()) {
                m_escapeSince = nowMs();
            }
            takeReplies(inputs, from);
        } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
            return false;
        }
    } else if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) {
        return false;
    }
    return true;
}

bool Tty::flush(int timeoutMs)
{
    const long long deadline = nowMs() + timeoutMs;
    while (pending() > 0) {
        const long long left = deadline - nowMs();
        if (left <= 0) {
            return false;
        }
        // step(), not turn(): the kept inputs would make each turn() poll
        // without waiting.
        std::vector<Input> inputs;
        if (!step(int(left), inputs)) {
            return false;
        }
        m_kept.insert(m_kept.end(), std::make_move_iterator(inputs.begin()), std::make_move_iterator(inputs.end()));
    }
    return true;
}

bool Tty::drain(int timeoutMs)
{
    // The terminal answers in order: what it sent before the answer to DA1
    // is read here, and dropped.
    m_kept.clear();
    m_out += "\x1b[c";
    ++m_fences;
    const long long deadline = nowMs() + timeoutMs;
    while (m_fences > 0) {
        const long long left = deadline - nowMs();
        if (left <= 0) {
            return false;
        }
        std::vector<Input> inputs;
        if (!step(int(left), inputs)) {
            return false;
        }
    }
    return true;
}

winsize Tty::size() const
{
    winsize size { };
    ::ioctl(m_fd, TIOCGWINSZ, &size);
    return size;
}

void Tty::watchSignals() { catchSignals(); }

int Tty::takeSignal() { return caughtSignal.exchange(0); }

bool Tty::signalPending() { return caughtSignal.load() != 0; }

bool Tty::takeResize() { return resized.exchange(false); }

} // namespace Aglow
