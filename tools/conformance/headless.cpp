// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// Runs a command in a pseudo-terminal driven by the terminal core alone: the
// command's output is fed to a Terminal and its replies (device reports,
// checksums) go back to the command. No window, no GPU.
//
//   afterglow_headless [--cols N] [--rows N] [--dump] -- command [args...]
//
// --dump prints the final screen to stderr. Exits with the command's status.

#include <AfterglowVt/Terminal.h>

#include <poll.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

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

    static constexpr int CellWidth = 10;
    static constexpr int CellHeight = 20;

private:
    int m_fd;
};

void dump(const AfterglowVt::Terminal& terminal)
{
    const AfterglowVt::Screen& screen = terminal.screen();
    for (int y = 0; y < screen.rows(); ++y) {
        std::string line;
        const AfterglowVt::Row& row = screen.row(y);
        for (int x = 0; x < screen.cols(); ++x) {
            char32_t c = row.at(x).codepoint();
            if (c == 0) {
                c = '.';
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
        std::fprintf(stderr, "%s\n", line.c_str());
    }
}

} // namespace

int main(int argc, char** argv)
{
    int cols = 80;
    int rows = 25;
    bool dumpScreen = false;
    int i = 1;
    for (; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--cols") && i + 1 < argc) {
            cols = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--rows") && i + 1 < argc) {
            rows = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--dump")) {
            dumpScreen = true;
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
    for (;;) {
        pollfd pfd{master, POLLIN, 0};
        if (::poll(&pfd, 1, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
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
        dump(terminal);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
