// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "Tty.h"

#include <AfterglowProto/Text.h>

#include <atomic>
#include <utility>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace Aglow {

namespace {

// Escape codes from the terminal are bounded: the largest reply of the
// protocols here is a few KiB. A longer one is dropped whole.
constexpr std::size_t MaxEscape = 1024 * 1024;
// An OSC 52 answer holds the whole clipboard text in one sequence: as much
// as a terminal takes in an OSC 5522 write, in base64.
constexpr std::size_t MaxOsc52 = 176 * 1024 * 1024;
// A lone ESC is the Esc key when no byte follows it in this time.
constexpr long long EscapeKeyMs = 50;

int signalPipe[2] = { -1, -1 };
std::atomic<int> caughtSignal { 0 };
std::atomic<bool> resized { false };

void onSignal(int number)
{
    const int saved = errno;
    if (number == SIGWINCH) {
        resized = true;
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
    sigemptyset(&action.sa_mask);
    for (const int number : { SIGINT, SIGTERM, SIGHUP, SIGWINCH }) {
        ::sigaction(number, &action, nullptr);
    }
    // A terminal that closes must not kill the program before it restores the tty.
    std::signal(SIGPIPE, SIG_IGN);
}

} // namespace

long long nowMs()
{
    timespec now { };
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

std::string printable(std::string_view text)
{
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
        if (AfterglowProto::isControl(cp)) {
            out += '?';
        } else {
            out.append(text.substr(start, i - start));
        }
    }
    return out;
}

void InputParser::flushText(std::vector<Input>& out)
{
    if (!m_text.empty()) {
        out.push_back({ Input::Type::Text, std::move(m_text) });
        m_text.clear();
    }
}

void InputParser::feed(std::string_view bytes, std::vector<Input>& out)
{
    for (const char c : bytes) {
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
            if (c == '[') {
                m_state = State::Csi;
            } else if (c == ']' || c == '_' || c == 'P') {
                m_string = c == ']' ? Input::Type::Osc : c == '_' ? Input::Type::Apc : Input::Type::Dcs;
                m_state = State::String;
            } else {
                // Alt and a key, or Esc and then a key: the Esc key first.
                out.push_back({ Input::Type::Escape, {} });
                m_state = State::Ground;
                if (byte == 0x1b) {
                    m_state = State::Escape;
                } else if (byte == 0x03) {
                    out.push_back({ Input::Type::Interrupt, {} });
                } else {
                    m_text += c;
                }
            }
            break;
        case State::Csi:
            m_buffer += c;
            if (byte >= 0x40 && byte <= 0x7e) {
                out.push_back({ Input::Type::Csi, std::move(m_buffer) });
                m_buffer.clear();
                m_state = State::Ground;
            } else if (m_buffer.size() > 256) {
                m_buffer.clear();
                m_state = State::Ground;
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
            } else if (m_buffer.size() < MaxEscape
                       || (m_string == Input::Type::Osc && m_buffer.size() < MaxOsc52 && m_buffer.starts_with("52;"))) {
                m_buffer += c;
            } else {
                m_overflow = true;
            }
            break;
        case State::StringEscape:
            // ST ends the string. Any other byte after ESC ends it too.
            if (!m_overflow) {
                out.push_back({ m_string, std::move(m_buffer) });
            }
            m_buffer.clear();
            m_state = State::Ground;
            if (c != '\\') {
                m_state = State::Escape;
                feed(std::string_view(&c, 1), out);
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
    if (m_state == State::Escape) {
        out.push_back({ Input::Type::Escape, {} });
        m_state = State::Ground;
    }
}

Tty::~Tty() { restore(); }

bool Tty::open(std::string& error)
{
    m_fd = ::open("/dev/tty", O_RDWR | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
    if (m_fd < 0) {
        error = std::string("Cannot open the controlling terminal: ") + std::strerror(errno);
        return false;
    }
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
    if (::tcsetattr(m_fd, TCSANOW, &raw) != 0) {
        error = std::string("Cannot set the terminal to raw mode: ") + std::strerror(errno);
        return false;
    }
    m_raw = true;
    return true;
}

void Tty::restore()
{
    if (m_fd < 0) {
        return;
    }
    m_out += std::exchange(m_reset, {});
    flush(2000);
    if (m_raw) {
        // TCSAFLUSH: replies that arrive late are dropped, not typed into the shell.
        ::tcsetattr(m_fd, TCSAFLUSH, &m_saved);
        m_raw = false;
    }
    ::close(m_fd);
    m_fd = -1;
}

bool Tty::turn(int timeoutMs, std::vector<Input>& inputs)
{
    if (!m_kept.empty()) {
        inputs.insert(inputs.end(), std::make_move_iterator(m_kept.begin()), std::make_move_iterator(m_kept.end()));
        m_kept.clear();
        timeoutMs = 0;
    }
    if (m_parser.waitingAfterEscape()) {
        const long long left = EscapeKeyMs - (nowMs() - m_escapeSince);
        if (left <= 0) {
            m_parser.timeout(inputs);
        } else if (timeoutMs < 0 || timeoutMs > left) {
            timeoutMs = int(left);
        }
    }
    pollfd fds[2] = { { m_fd, short(POLLIN | (m_out.empty() ? 0 : POLLOUT)), 0 }, { signalPipe[0], POLLIN, 0 } };
    const int ready = ::poll(fds, signalPipe[0] >= 0 ? 2 : 1, timeoutMs);
    if (ready < 0) {
        return errno == EINTR;
    }
    if (fds[1].revents & POLLIN) {
        char drain[64];
        while (::read(signalPipe[0], drain, sizeof drain) > 0) {
        }
    }
    if (fds[0].revents & POLLOUT) {
        const ssize_t n = ::write(m_fd, m_out.data(), m_out.size());
        if (n > 0) {
            m_out.erase(0, std::size_t(n));
        } else if (n < 0 && errno != EAGAIN && errno != EINTR) {
            return false;
        }
    }
    if (fds[0].revents & POLLIN) {
        char buffer[65536];
        const ssize_t n = ::read(m_fd, buffer, sizeof buffer);
        if (n > 0) {
            m_parser.feed(std::string_view(buffer, std::size_t(n)), inputs);
            if (m_parser.waitingAfterEscape()) {
                m_escapeSince = nowMs();
            }
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
    while (!m_out.empty()) {
        const long long left = deadline - nowMs();
        if (left <= 0) {
            return false;
        }
        std::vector<Input> inputs;
        if (!turn(int(left), inputs)) {
            return false;
        }
        m_kept.insert(m_kept.end(), std::make_move_iterator(inputs.begin()), std::make_move_iterator(inputs.end()));
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

bool Tty::takeResize() { return resized.exchange(false); }

} // namespace Aglow
