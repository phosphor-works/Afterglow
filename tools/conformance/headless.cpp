// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// Runs a command in a pseudo-terminal driven by the terminal core alone: the
// command's output is fed to a Terminal and its replies (device reports,
// checksums) go back to the command. No window, no GPU.
//
//   afterglow_headless [--cols N] [--rows N] [--dump] [--idle-dump DIR]
//                      [--reply TEXT=KEYS]... | [--responder CMD]
//                      -- command [args...]
//
// --dump prints the final screen to stderr. For interactive programs
// (vttest), --idle-dump writes the screen to DIR/0001.txt, 0002.txt... each
// time the program goes quiet, and --reply then answers the first TEXT found
// on the screen with KEYS (C escapes; "a|b|c" sends a, then b, then c on
// later matches). --responder CMD instead runs "CMD SCREENFILE" at each
// quiet moment and sends what it prints (C escapes; nothing sends nothing),
// for scripted walks through menus; it cannot be combined with --reply.
// Screen readback (DECRQCRA) is on, as the suites read the screen through it.
// Exits with the command's status, or 2 when the runner itself fails.

#include <AfterglowVt/Terminal.h>

#include <poll.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <csignal>
#include <unistd.h>
#include <fcntl.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>
#include <charconv>
#include <chrono>

namespace {

class Host : public AfterglowVt::ITerminalHost
{
public:
    explicit Host(int fd)
        : m_fd(fd)
    {
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
        winsize ws{};
        ws.ws_col = std::uint16_t(cols);
        ws.ws_row = std::uint16_t(rows);
        ws.ws_xpixel = std::uint16_t(cols * CellWidth);
        ws.ws_ypixel = std::uint16_t(rows * CellHeight);
        return ::ioctl(m_fd, TIOCSWINSZ, &ws) == 0;
    }

    // A 1920x1080 display, so maximum sizes and screen reports have answers.
    bool screenSize(int& width, int& height) override
    {
        width = 1920;
        height = 1080;
        return true;
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

bool dimension(const char* value, int minimum, int maximum, int& result)
{
    const char* end = value + std::strlen(value);
    const auto parsed = std::from_chars(value, end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end && result >= minimum && result <= maximum;
}

std::string shellQuote(const std::string& value)
{
    std::string out = "'";
    for (char c : value) out += c == '\'' ? "'\\''" : std::string(1, c);
    return out + "'";
}

/// Runs the responder on the screen file. Returns nullptr when it succeeded,
/// or why it did not, so each failure has its own message.
const char* respond(const std::string& command, const std::string& path, std::string& output)
{
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) return "responder could not start";
    const std::string invocation = command + " " + shellQuote(path);
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::setpgid(0, 0);
        ::dup2(fds[1], STDOUT_FILENO);
        ::close(fds[0]); ::close(fds[1]);
        ::execl("/bin/sh", "sh", "-c", invocation.c_str(), nullptr);
        _exit(127);
    }
    ::close(fds[1]);
    if (pid < 0) { ::close(fds[0]); return "responder could not start"; }
    ::setpgid(pid, pid);
    const char* why = ::fcntl(fds[0], F_SETFL, O_NONBLOCK) == 0 ? nullptr : "responder output could not be read";
    bool ok = !why;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (ok) {
        if (std::chrono::steady_clock::now() >= deadline) { ok = false; why = "responder timed out"; break; }
        pollfd pfd{fds[0], POLLIN, 0};
        const int ready = ::poll(&pfd, 1, 100);
        if (ready < 0) { if (errno == EINTR) continue; ok = false; why = "responder output could not be read"; break; }
        if (!ready) continue;
        char buffer[4096];
        const auto count = ::read(fds[0], buffer, sizeof buffer);
        if (count == 0) break;
        if (count < 0) { if (errno == EINTR || errno == EAGAIN) continue; ok = false; why = "responder output could not be read"; break; }
        if (output.size() + std::size_t(count) > 1024 * 1024) { ok = false; why = "responder output exceeded 1 MiB"; break; }
        output.append(buffer, std::size_t(count));
    }
    ::close(fds[0]);
    int status = 0;
    // A responder can close stdout and then hang. Its process lifetime shares
    // the same deadline as output collection.
    for (;;) {
        const auto done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid) break;
        if (done < 0 && errno != EINTR) { ok = false; why = "responder could not be waited for"; break; }
        if (!ok || std::chrono::steady_clock::now() >= deadline) {
            if (ok) why = "responder timed out";
            ok = false;
            ::kill(-pid, SIGKILL); ::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
            break;
        }
        ::poll(nullptr, 0, 20);
    }
    if (!ok) return why;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? nullptr : "responder failed";
}

void appendUtf8(std::string& out, char32_t c)
{
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

/// The screen as the user reads it, so that --reply text in CJK or with
/// combining marks matches: wide tails are skipped, cluster marks kept.
std::string screenText(const AfterglowVt::Terminal& terminal)
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
            appendUtf8(out, cell.codepoint() == 0 ? U' ' : cell.codepoint());
            for (char32_t extra : screen.graphemeExtra(row, x)) {
                appendUtf8(out, extra);
            }
        }
        out += '\n';
    }
    return out;
}

/// Expands C escapes (\r, \n, \t, \e, \xHH, \\).
std::string unescape(const std::string& s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 == s.size()) {
            out += s[i];
            continue;
        }
        const char c = s[++i];
        switch (c) {
        case 'r':
            out += '\r';
            break;
        case 'n':
            out += '\n';
            break;
        case 't':
            out += '\t';
            break;
        case 'e':
            out += '\x1b';
            break;
        case 'x': {
            // Exactly two hex digits; anything else stays as written.
            int value = 0;
            const char* first = s.data() + i + 1;
            if (i + 2 < s.size() && std::from_chars(first, first + 2, value, 16).ptr == first + 2
                && std::isxdigit(static_cast<unsigned char>(first[0]))) {
                out += char(value);
                i += 2;
            } else {
                out += "\\x";
            }
            break;
        }
        default:
            out += c;
            break;
        }
    }
    return out;
}

struct Reply
{
    std::string text;
    std::deque<std::string> keys;
};

void usage(const char* name)
{
    std::fprintf(stderr,
                 "usage: %s [--cols N] [--rows N] [--dump] [--idle-dump DIR]\n"
                 "       [--reply TEXT=KEYS]... | [--responder CMD] -- command [args...]\n",
                 name);
}

} // namespace

int main(int argc, char** argv)
{
    int cols = 80;
    int rows = 25;
    bool dumpScreen = false;
    std::string idleDir;
    std::vector<Reply> replies;
    std::string responder;
    int i = 1;
    for (; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--cols") && i + 1 < argc) {
            if (!dimension(argv[++i], Host::MinCols, 1000, cols)) {
                std::fprintf(stderr, "--cols must be an integer in 2..1000\n"); return 2;
            }
        } else if (!std::strcmp(argv[i], "--rows") && i + 1 < argc) {
            if (!dimension(argv[++i], 1, 500, rows)) {
                std::fprintf(stderr, "--rows must be an integer in 1..500\n"); return 2;
            }
        } else if (!std::strcmp(argv[i], "--dump")) {
            dumpScreen = true;
        } else if (!std::strcmp(argv[i], "--idle-dump") && i + 1 < argc) {
            idleDir = argv[++i];
        } else if (!std::strcmp(argv[i], "--responder") && i + 1 < argc) {
            responder = argv[++i];
        } else if (!std::strcmp(argv[i], "--reply") && i + 1 < argc) {
            const std::string spec = argv[++i];
            const auto eq = spec.find('=');
            if (eq == std::string::npos) {
                std::fprintf(stderr, "--reply needs TEXT=KEYS\n");
                return 2;
            }
            Reply r{spec.substr(0, eq), {}};
            std::string keys = spec.substr(eq + 1);
            std::size_t pos = 0;
            for (;;) {
                const auto bar = keys.find('|', pos);
                r.keys.push_back(unescape(keys.substr(pos, bar == std::string::npos ? bar : bar - pos)));
                if (bar == std::string::npos) {
                    break;
                }
                pos = bar + 1;
            }
            replies.push_back(std::move(r));
        } else if (!std::strcmp(argv[i], "--")) {
            ++i;
            break;
        } else if (argv[i][0] == '-') {
            // An unknown option, or a value option with nothing after it:
            // never run it as the command.
            std::fprintf(stderr, "unknown option or missing value: %s\n", argv[i]);
            usage(argv[0]);
            return 2;
        } else {
            break;
        }
    }
    if (i >= argc) {
        usage(argv[0]);
        return 2;
    }
    if (!responder.empty() && !replies.empty()) {
        std::fprintf(stderr, "--responder replaces --reply; give one of them\n");
        return 2;
    }

    constexpr int CellWidth = Host::CellWidth;
    constexpr int CellHeight = Host::CellHeight;
    winsize ws{};
    ws.ws_col = std::uint16_t(cols);
    ws.ws_row = std::uint16_t(rows);
    ws.ws_xpixel = std::uint16_t(cols * CellWidth);
    ws.ws_ypixel = std::uint16_t(rows * CellHeight);
    int master = -1;
    const pid_t child = forkpty(&master, nullptr, nullptr, &ws);
    if (child < 0) {
        std::perror("forkpty");
        return 2;
    }
    if (child == 0) {
        setenv("TERM", "xterm", 1);
        execvp(argv[i], argv + i);
        std::perror("exec");
        _exit(127);
    }

    // The responder runs while the command does: it must not hold the master.
    if (::fcntl(master, F_SETFL, O_NONBLOCK) < 0 || ::fcntl(master, F_SETFD, FD_CLOEXEC) < 0) {
        std::perror("fcntl"); ::kill(child, SIGKILL); ::close(master);
        while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {};
        return 2;
    }
    Host host(master);
    AfterglowVt::TerminalOptions options;
    options.allowScreenReadback = true;
    AfterglowVt::Terminal terminal(cols, rows, host, options);
    terminal.resize(cols, rows, CellWidth, CellHeight);

    char buf[65536];
    const bool interactive = !idleDir.empty() || !replies.empty() || !responder.empty();
    bool changed = false;
    int dumps = 0;
    int quietRounds = 0;
    bool failed = false;
    struct TemporaryScreen {
        std::string path;
        ~TemporaryScreen() { if (!path.empty()) ::unlink(path.c_str()); }
    } temporary;
    if (!responder.empty() && idleDir.empty()) {
        const char* tmpdir = std::getenv("TMPDIR");
        std::string name = std::string(tmpdir && *tmpdir ? tmpdir : "/tmp") + "/afterglow-screen-XXXXXX";
        const int fd = ::mkstemp(name.data());
        if (fd < 0) { std::perror("screen file"); failed = true; }
        else { temporary.path = name; ::close(fd); }
    }
    int status = 0;
    bool reaped = false;
    for (;;) {
        if (failed) break;
        host.flush();
        if (host.overflowed) {
            std::fprintf(stderr, "PTY reply queue exceeded 1 MiB\n");
            failed = true;
            break;
        }
        if (host.failed || host.stalled()) {
            std::fprintf(stderr, "PTY reply queue failed or stopped making progress\n");
            failed = true;
            break;
        }
        pollfd pfd{master, short(POLLIN | (host.pending() ? POLLOUT : 0)), 0};
        const int ready = ::poll(&pfd, 1, interactive || host.pending() ? 300 : 1000);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            failed = true; break;
        }
        if (ready == 0) {
            // A background process can keep the pty open after the command
            // exits, so the hangup never comes: quiet and exited ends the run.
            const pid_t done = ::waitpid(child, &status, WNOHANG);
            if (done == child) {
                reaped = true;
                break;
            }
            if (host.pending() || !interactive) continue;
            // Quiet: the program waits for input. Record the screen, answer.
            if (!changed) {
                // Nothing new since the last answer (a prompt that does not
                // echo): ask again after a while, give up after many tries.
                ++quietRounds;
                if (quietRounds > 40) {
                    std::fprintf(stderr, "the program stopped responding; stopping\n");
                    failed = true;
                    break;
                }
                if (quietRounds % 4 != 0) {
                    continue;
                }
            } else {
                quietRounds = 0;
            }
            changed = false;
            const std::string text = screenText(terminal);
            std::string dumpPath;
            if (!idleDir.empty()) {
                char name[64];
                std::snprintf(name, sizeof name, "/%04d.txt", ++dumps);
                dumpPath = idleDir + name;
            } else dumpPath = temporary.path;
            if (!dumpPath.empty()) {
                FILE* f = std::fopen(dumpPath.c_str(), "w");
                if (!f) { std::perror("screen dump"); failed = true; break; }
                const bool written = std::fwrite(text.data(), 1, text.size(), f) == text.size();
                const bool closed = std::fclose(f) == 0;
                if (!written || !closed) { failed = true; break; }
            }
            if (!responder.empty() && !dumpPath.empty()) {
                std::string keys;
                if (const char* why = respond(responder, dumpPath, keys)) {
                    std::fprintf(stderr, "%s\n", why);
                    failed = true; break;
                }
                while (!keys.empty() && (keys.back() == '\n')) {
                    keys.pop_back();
                }
                host.writeToPty(unescape(keys));
                continue;
            }
            for (Reply& r : replies) {
                if (text.find(r.text) != std::string::npos && !r.keys.empty()) {
                    const std::string keys = r.keys.front();
                    if (r.keys.size() > 1) {
                        r.keys.pop_front();
                    }
                    host.writeToPty(keys);
                    break;
                }
            }
            continue;
        }
        if (pfd.revents & POLLOUT) host.flush();
        if (!(pfd.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) continue;
        changed = true;
        const ssize_t n = ::read(master, buf, sizeof buf);
        if (n > 0) {
            terminal.feed(std::string_view(buf, std::size_t(n)));
        } else if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        } else {
            break; // EIO: the child closed its side
        }
    }
    ::close(master);
    if (failed && !reaped) ::kill(child, SIGKILL);
    while (!reaped && ::waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) { std::perror("waitpid"); return 2; }
    }
    if (dumpScreen) {
        std::fputs(screenText(terminal).c_str(), stderr);
    }
    return failed ? 2 : WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
