// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// Runs a command in a pseudo-terminal driven by the terminal core alone: the
// command's output is fed to a Terminal and its replies (device reports,
// checksums) go back to the command. No window, no GPU.
//
//   afterglow_headless [--cols N] [--rows N] [--dump] [--idle-dump DIR]
//                      [--reply TEXT=KEYS]... -- command [args...]
//
// --dump prints the final screen to stderr. For interactive programs
// (vttest), --idle-dump writes the screen to DIR/0001.txt, 0002.txt... each
// time the program goes quiet, and --reply then answers the first TEXT found
// on the screen with KEYS (C escapes; "a|b|c" sends a, then b, then c on
// later matches). --responder CMD instead runs "CMD SCREENFILE" at each
// quiet moment and sends what it prints (C escapes; nothing sends nothing),
// for scripted walks through menus. Exits with the command's status.

#include <AfterglowVt/Terminal.h>

#include <poll.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <csignal>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

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
        while (!bytes.empty()) {
            const ssize_t n = ::write(m_fd, bytes.data(), bytes.size());
            if (n < 0) {
                if (errno == EINTR || errno == EAGAIN) {
                    continue;
                }
                return;
            }
            bytes.remove_prefix(std::size_t(n));
        }
    }

    // Size changes asked for by the program (DECCOLM, XTWINOPS) are granted,
    // as xterm does with window ops allowed; the pty follows.
    bool requestResize(int cols, int rows) override
    {
        winsize ws{};
        ws.ws_col = std::uint16_t(cols);
        ws.ws_row = std::uint16_t(rows);
        ws.ws_xpixel = std::uint16_t(cols * CellWidth);
        ws.ws_ypixel = std::uint16_t(rows * CellHeight);
        ::ioctl(m_fd, TIOCSWINSZ, &ws);
        return true;
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

private:
    int m_fd;
};

std::string screenText(const AfterglowVt::Terminal& terminal)
{
    std::string out;
    const AfterglowVt::Screen& screen = terminal.screen();
    for (int y = 0; y < screen.rows(); ++y) {
        std::string line;
        const AfterglowVt::Row& row = screen.row(y);
        for (int x = 0; x < screen.cols(); ++x) {
            char32_t c = row.at(x).codepoint();
            if (c == 0) {
                c = ' ';
            }
            if (c < 0x80) {
                line += char(c);
            } else if (c < 0x800) {
                line += char(0xC0 | (c >> 6));
                line += char(0x80 | (c & 0x3F));
            } else if (c < 0x10000) {
                line += char(0xE0 | (c >> 12));
                line += char(0x80 | ((c >> 6) & 0x3F));
                line += char(0x80 | (c & 0x3F));
            } else {
                line += char(0xF0 | (c >> 18));
                line += char(0x80 | ((c >> 12) & 0x3F));
                line += char(0x80 | ((c >> 6) & 0x3F));
                line += char(0x80 | (c & 0x3F));
            }
        }
        out += line;
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
        case 'x':
            if (i + 2 < s.size() + 1) {
                out += char(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
                i += 2;
            }
            break;
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
            cols = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--rows") && i + 1 < argc) {
            rows = std::atoi(argv[++i]);
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
        } else {
            break;
        }
    }
    if (i >= argc) {
        std::fprintf(stderr, "usage: %s [--cols N] [--rows N] [--dump] -- command [args...]\n", argv[0]);
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

    Host host(master);
    AfterglowVt::Terminal terminal(cols, rows, host);
    terminal.resize(cols, rows, CellWidth, CellHeight);

    char buf[65536];
    const bool interactive = !idleDir.empty() || !replies.empty() || !responder.empty();
    bool changed = false;
    int dumps = 0;
    int quietRounds = 0;
    for (;;) {
        pollfd pfd{master, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, interactive ? 300 : -1);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (ready == 0) {
            // Quiet: the program waits for input. Record the screen, answer.
            if (!changed) {
                // Nothing new since the last answer (a prompt that does not
                // echo): ask again after a while, give up after many tries.
                ++quietRounds;
                if (quietRounds > 40) {
                    std::fprintf(stderr, "the program stopped responding; stopping\n");
                    ::kill(child, SIGKILL);
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
                if (FILE* f = std::fopen(dumpPath.c_str(), "w")) {
                    std::fputs(text.c_str(), f);
                    std::fclose(f);
                }
            }
            if (!responder.empty() && !dumpPath.empty()) {
                std::string keys;
                if (FILE* pipe = ::popen((responder + " " + dumpPath).c_str(), "r")) {
                    char line[4096];
                    while (std::fgets(line, sizeof line, pipe)) {
                        keys += line;
                    }
                    ::pclose(pipe);
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
    int status = 0;
    ::waitpid(child, &status, 0);
    if (dumpScreen) {
        std::fputs(screenText(terminal).c_str(), stderr);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
