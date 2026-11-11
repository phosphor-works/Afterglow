// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// afterglow_tuifuzz: runs a full-screen program in a pseudo-terminal against
// the terminal core alone, sends it random input, and checks the screen after
// each step. Real programs write what users see: this finds the screens that
// the fuzzers and the conformance suites do not make.
//
//   afterglow_tuifuzz [--cols N] [--rows N] [--seed N] [--steps N]
//                     [--seconds N] [--record DIR] [--home DIR]
//                     [--env NAME=VALUE]... -- command [args...]
//
// A step is one random action: text, a key (encoded by Input.cpp as the
// program asked, kitty protocol included), a Ctrl key, mouse reports while
// the program tracks the mouse, a paste, a focus change, a resize, a scroll
// of the scrollback, or a selection and a search over it. Then the tool reads
// what the program writes until it is quiet, and checks:
//
//   - the size of each screen, and the cursor inside it;
//   - the width of each row of the screen, and of each row of the scrollback;
//   - each wide character has its tail cell, and no tail cell is alone;
//   - each block of sized text (OSC 66) is whole.
//
// A failure prints the seed, the step, the action and the screen, and exits
// with 1. The input is the same for each run of a seed; the program's timing
// can make its output differ.
//
// The program runs in bubblewrap: a read-only root, private /tmp, /run and
// /home, its own home directory (--home, or an empty one that the run
// deletes), its own process, network and IPC namespaces, and a cleared
// environment (TERM, PATH, HOME, LANG, and the --env values). Random keys can
// make a program run shell commands, write files or kill processes: the
// sandbox keeps those inside. Without bwrap the tool does not run.
//
// When the program exits, it starts again in the same terminal, as a user
// would start it again.
//
// --record DIR writes what the program printed to DIR/SEED-NNNN-COLSxROWS.vt,
// a new file at each resize. afterglow_vtdiff reads the size from the name.
//
// Exit status: 0 when every step passed, 1 on a failed check, 2 when the run
// could not go on (no bwrap, a program that does not start).

#include "PtyHost.h"
#include "ScreenChecks.h"

#include <AfterglowVt/Input.h>
#include <AfterglowVt/Search.h>
#include <AfterglowVt/Selection.h>
#include <AfterglowVt/Terminal.h>

#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using AfterglowConformance::PtyHost;
using AfterglowConformance::checkTerminal;
using Clock = std::chrono::steady_clock;

struct Options
{
    int cols = 80;
    int rows = 24;
    std::uint64_t seed = 1;
    long steps = 1000;
    long seconds = 0;
    std::string record;
    std::string home;
    std::vector<std::string> env;
    std::vector<std::string> command;
};

/// The quiet time that ends a step, and the longest step: a program that
/// never goes quiet (btop) still gets input.
constexpr auto Quiet = std::chrono::milliseconds(30);
constexpr auto LongestStep = std::chrono::milliseconds(400);
/// A program that ends before any input, this many times in a row, does
/// not start at all (or not at this size).
constexpr int UnfedRunsAllowed = 3;

/// The random input. Each draw takes values from the one generator, in the
/// same order for a seed.
class Input
{
public:
    explicit Input(std::uint64_t seed)
        : m_rng(seed)
    {
    }
    std::uint64_t below(std::uint64_t n) { return n ? m_rng() % n : 0; }
    bool chance(int percent) { return int(below(100)) < percent; }

    std::string text()
    {
        // Clusters whose width the terminal changes after their first
        // character: a spacing mark, a conjunct, Thai SARA AM, VS15, VS16.
        static constexpr std::array<std::string_view, 30> Words = {
            "a",  "hello", "q",  " ",   "x",   "jk", ":",      "/",  "0",   "$", "w",  "G",
            "gg", "dd",    "u",  "é",   "中文", "😀", "é", "→", "👨‍👩‍👧", "\t", "ab", "?",
            "\u0915\u093E", "\u0915\u094D\u0930", "\u0E15\u0E48\u0E33", "\u231A\uFE0E", "\u2764\uFE0F", "\u0601\u06F1"};
        std::string out;
        for (std::uint64_t n = 1 + below(4); n > 0; --n) out += Words[below(Words.size())];
        return out;
    }
    AfterglowVt::Key key()
    {
        using AfterglowVt::Key;
        static constexpr std::array<Key, 20> Keys = {
            Key::Enter, Key::Tab,      Key::Backspace, Key::Escape, Key::Up,       Key::Down,     Key::Left,
            Key::Right, Key::Home,     Key::End,       Key::Insert, Key::Delete,   Key::PageUp,   Key::PageDown,
            Key::F1,    Key::F2,       Key::F5,        Key::F10,    Key::KeypadEnter, Key::KeypadAdd};
        return Keys[below(Keys.size())];
    }
    std::uint8_t mods()
    {
        using namespace AfterglowVt::Mod;
        static constexpr std::array<std::uint8_t, 8> Mods = {0, 0, 0, 0, Shift, Ctrl, Alt, Ctrl | Shift};
        return Mods[below(Mods.size())];
    }

private:
    std::mt19937_64 m_rng;
};

/// A program in a pseudo-terminal, inside bubblewrap.
struct Program
{
    pid_t pid = -1;
    int master = -1;
    bool fed = false; ///< it had input: it can have ended because of it
};

std::vector<std::string> sandbox(const Options& o, const std::string& home)
{
    const char* path = std::getenv("PATH");
    std::vector<std::string> args = {
        "bwrap",
        "--ro-bind", "/", "/",
        "--dev", "/dev",
        "--proc", "/proc",
        "--tmpfs", "/tmp",
        "--tmpfs", "/run",
        "--tmpfs", "/home",
    };
    // The user's own directories, where a system keeps them elsewhere: they
    // hold the sockets of the user's sessions, and a read-only mount does
    // not stop a connect.
    for (const char* name : {"HOME", "XDG_RUNTIME_DIR", "XDG_STATE_HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME"}) {
        const char* dir = std::getenv(name);
        if (!dir || dir[0] != '/') continue;
        const std::string_view d = dir;
        if (d.starts_with("/home/") || d.starts_with("/run/") || d.starts_with("/tmp/") || d == "/") continue;
        args.insert(args.end(), {"--tmpfs", dir});
    }
    args.insert(args.end(), {
        "--bind", home, "/tmp/home",
        "--perms", "0700", "--dir", "/tmp/run",
        "--chdir", "/tmp/home",
        "--unshare-all",
        "--die-with-parent",
        "--clearenv",
        "--setenv", "HOME", "/tmp/home",
        "--setenv", "XDG_RUNTIME_DIR", "/tmp/run",
        "--setenv", "TMUX_TMPDIR", "/tmp/run",
        "--setenv", "PATH", path && *path ? path : "/usr/local/bin:/usr/bin:/bin",
        "--setenv", "TERM", "xterm-256color",
        "--setenv", "COLORTERM", "truecolor",
        "--setenv", "LANG", "C.UTF-8",
        "--setenv", "SHELL", "/bin/sh",
    });
    for (const std::string& assignment : o.env) {
        const auto eq = assignment.find('=');
        args.insert(args.end(), {"--setenv", assignment.substr(0, eq), assignment.substr(eq + 1)});
    }
    args.push_back("--");
    args.insert(args.end(), o.command.begin(), o.command.end());
    return args;
}

std::optional<Program> start(const std::vector<std::string>& args, int cols, int rows)
{
    winsize ws{};
    ws.ws_col = std::uint16_t(cols);
    ws.ws_row = std::uint16_t(rows);
    ws.ws_xpixel = std::uint16_t(cols * PtyHost::CellWidth);
    ws.ws_ypixel = std::uint16_t(rows * PtyHost::CellHeight);
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    Program p;
    p.pid = forkpty(&p.master, nullptr, nullptr, &ws);
    if (p.pid < 0) {
        std::perror("forkpty");
        return std::nullopt;
    }
    if (p.pid == 0) {
        ::execvp(argv[0], argv.data());
        std::perror("bwrap");
        _exit(127);
    }
    if (::fcntl(p.master, F_SETFL, O_NONBLOCK) < 0 || ::fcntl(p.master, F_SETFD, FD_CLOEXEC) < 0) {
        std::perror("fcntl");
        ::kill(p.pid, SIGKILL);
        ::close(p.master);
        while (::waitpid(p.pid, nullptr, 0) < 0 && errno == EINTR) {}
        return std::nullopt;
    }
    return p;
}

/// Ends the program and its sandbox: bwrap's init in the namespace dies with
/// it (--die-with-parent), and the rest of the namespace with the init.
int stop(Program& p)
{
    int status = 0;
    if (p.master >= 0) ::close(p.master);
    p.master = -1;
    if (p.pid > 0) {
        if (::waitpid(p.pid, &status, WNOHANG) == 0) {
            ::kill(p.pid, SIGKILL);
            while (::waitpid(p.pid, &status, 0) < 0 && errno == EINTR) {}
        }
    }
    p.pid = -1;
    return status;
}

/// The files of --record: one for each size of the terminal.
class Recorder
{
public:
    Recorder(std::string dir, std::uint64_t seed)
        : m_dir(std::move(dir))
        , m_seed(seed)
    {
    }
    ~Recorder() { close(); }
    void size(int cols, int rows)
    {
        if (m_dir.empty()) return;
        close();
        char name[96];
        std::snprintf(name, sizeof name, "/%llu-%04d-%dx%d.vt", static_cast<unsigned long long>(m_seed), ++m_count,
                      cols, rows);
        m_file = std::fopen((m_dir + name).c_str(), "wb");
        if (!m_file) std::perror((m_dir + name).c_str());
    }
    void write(std::string_view bytes)
    {
        if (m_file) std::fwrite(bytes.data(), 1, bytes.size(), m_file);
    }

private:
    void close()
    {
        if (m_file) std::fclose(m_file);
        m_file = nullptr;
    }
    std::string m_dir;
    std::uint64_t m_seed;
    int m_count = 0;
    std::FILE* m_file = nullptr;
};

bool number(const char* value, long long minimum, long long maximum, long long& result)
{
    const char* end = value + std::strlen(value);
    const auto parsed = std::from_chars(value, end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end && result >= minimum && result <= maximum;
}

void usage()
{
    std::fprintf(stderr, "usage: afterglow_tuifuzz [--cols N] [--rows N] [--seed N] [--steps N] [--seconds N]\n"
                         "       [--record DIR] [--home DIR] [--env NAME=VALUE]... -- command [args...]\n");
}

bool haveBwrap()
{
    const char* path = std::getenv("PATH");
    std::string_view dirs = path ? path : "";
    while (!dirs.empty()) {
        const auto colon = dirs.find(':');
        const std::string dir(dirs.substr(0, colon));
        if (!dir.empty() && ::access((dir + "/bwrap").c_str(), X_OK) == 0) return true;
        if (colon == std::string_view::npos) break;
        dirs.remove_prefix(colon + 1);
    }
    return false;
}

} // namespace

int main(int argc, char** argv)
{
    Options o;
    int i = 1;
    while (i < argc) {
        const std::string_view arg = argv[i];
        long long value = 0;
        const bool hasValue = i + 1 < argc;
        if (arg == "--") {
            ++i;
            break;
        }
        if (arg == "--cols" && hasValue && number(argv[i + 1], PtyHost::MinCols, 1000, value)) {
            o.cols = int(value);
        } else if (arg == "--rows" && hasValue && number(argv[i + 1], 1, 500, value)) {
            o.rows = int(value);
        } else if (arg == "--seed" && hasValue && number(argv[i + 1], 0, std::numeric_limits<long long>::max(), value)) {
            o.seed = std::uint64_t(value);
        } else if (arg == "--steps" && hasValue && number(argv[i + 1], 1, 100000000, value)) {
            o.steps = long(value);
        } else if (arg == "--seconds" && hasValue && number(argv[i + 1], 1, 86400, value)) {
            o.seconds = long(value);
        } else if (arg == "--record" && hasValue) {
            o.record = argv[i + 1];
        } else if (arg == "--home" && hasValue) {
            o.home = argv[i + 1];
        } else if (arg == "--env" && hasValue && std::strchr(argv[i + 1], '=') && argv[i + 1][0] != '=') {
            o.env.emplace_back(argv[i + 1]);
        } else {
            std::fprintf(stderr, "afterglow_tuifuzz: bad option or value: %s\n", argv[i]);
            usage();
            return 2;
        }
        i += 2; // every option takes a value
    }
    for (; i < argc; ++i) o.command.emplace_back(argv[i]);
    if (o.command.empty()) {
        usage();
        return 2;
    }
    if (!haveBwrap()) {
        std::fprintf(stderr, "afterglow_tuifuzz: bwrap (bubblewrap) is not in PATH. The programs run only in its "
                             "sandbox.\n");
        return 2;
    }

    // The home of the program: given, or an empty one that the run deletes.
    std::string home = o.home;
    bool ownHome = false;
    if (home.empty()) {
        const char* tmpdir = std::getenv("TMPDIR");
        std::string name = std::string(tmpdir && *tmpdir ? tmpdir : "/tmp") + "/afterglow-tuifuzz-XXXXXX";
        if (!::mkdtemp(name.data())) {
            std::perror("afterglow_tuifuzz: home");
            return 2;
        }
        home = name;
        ownHome = true;
    }
    struct RemoveHome
    {
        std::string path;
        ~RemoveHome()
        {
            std::error_code ignored;
            if (!path.empty()) std::filesystem::remove_all(path, ignored);
        }
    } removeHome{ownHome ? home : std::string()};
    home = std::filesystem::absolute(home).string();
    if (!o.record.empty()) std::filesystem::create_directories(o.record);

    const std::vector<std::string> args = sandbox(o, home);
    std::signal(SIGPIPE, SIG_IGN);

    int cols = o.cols;
    int rows = o.rows;
    auto program = start(args, cols, rows);
    if (!program) return 2;
    PtyHost host(program->master);
    AfterglowVt::TerminalOptions options;
    options.maxScrollback = 2000;
    AfterglowVt::Terminal terminal(cols, rows, host, options);
    terminal.resize(cols, rows, PtyHost::CellWidth, PtyHost::CellHeight);
    Recorder recorder(o.record, o.seed);
    recorder.size(cols, rows);
    Input input(o.seed);

    long restarts = 0;
    int unfedRuns = 0;
    std::size_t bytesRead = 0;
    std::string action = "start";
    char buffer[65536];

    // Reads what the program writes until it is quiet, or the step is over.
    // Returns false when the program ended.
    const auto settle = [&]() -> bool {
        const auto begin = Clock::now();
        auto lastOutput = begin;
        for (;;) {
            host.flush();
            const auto now = Clock::now();
            if (now - lastOutput >= Quiet || now - begin >= LongestStep) return true;
            pollfd pfd{program->master, short(POLLIN | (host.pending() ? POLLOUT : 0)), 0};
            const int ready = ::poll(&pfd, 1, 10);
            if (ready < 0 && errno != EINTR) return false;
            if (ready <= 0) continue;
            if (pfd.revents & POLLOUT) host.flush();
            if (!(pfd.revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const ssize_t n = ::read(program->master, buffer, sizeof buffer);
            if (n > 0) {
                terminal.feed(std::string_view(buffer, std::size_t(n)));
                recorder.write(std::string_view(buffer, std::size_t(n)));
                bytesRead += std::size_t(n);
                lastOutput = Clock::now();
            } else if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
                continue;
            } else {
                return false; // EIO: the program closed its side
            }
        }
    };

    const auto failed = [&](long step, const std::string& what) {
        std::fprintf(stderr,
                     "afterglow_tuifuzz: %s: seed %llu, step %ld, after %s: %s\n"
                     "The screen (%dx%d):\n%s",
                     o.command.front().c_str(), static_cast<unsigned long long>(o.seed), step, action.c_str(),
                     what.c_str(), terminal.cols(), terminal.rows(), AfterglowConformance::screenText(terminal).c_str());
        stop(*program);
        return 1;
    };

    const auto deadline = o.seconds ? Clock::now() + std::chrono::seconds(o.seconds) : Clock::time_point::max();
    long step = 0;
    for (; step < o.steps && Clock::now() < deadline; ++step) {
        if (!settle()) {
            // The program ended: start it again, unless it cannot run at all.
            const bool fed = program->fed;
            const int status = stop(*program);
            unfedRuns = fed ? 0 : unfedRuns + 1;
            if (unfedRuns >= UnfedRunsAllowed) {
                if (cols == o.cols && rows == o.rows) {
                    std::fprintf(stderr, "afterglow_tuifuzz: %s ends at once (status %d); the screen:\n%s",
                                 o.command.front().c_str(), status, AfterglowConformance::screenText(terminal).c_str());
                    return 2;
                }
                // Some programs refuse a tiny terminal: back to the size
                // the run started with.
                cols = o.cols;
                rows = o.rows;
                terminal.resize(cols, rows, PtyHost::CellWidth, PtyHost::CellHeight);
                recorder.size(cols, rows);
                unfedRuns = 0;
            }
            // Ctrl+C and a quit are not news; a crash of the
            // program is, though the bug can be the program's own.
            static constexpr std::array<int, 6> Crashes = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL, SIGTRAP};
            if (WIFSIGNALED(status) && std::find(Crashes.begin(), Crashes.end(), WTERMSIG(status)) != Crashes.end()) {
                std::fprintf(stderr, "afterglow_tuifuzz: %s crashed (signal %d) at step %ld, after %s\n",
                             o.command.front().c_str(), WTERMSIG(status), step, action.c_str());
            }
            program = start(args, cols, rows);
            if (!program) return 2;
            host.setFd(program->master);
            ++restarts;
            action = "a start again";
            continue;
        }
        if (host.overflowed || host.failed || host.stalled()) return failed(step, "the replies to the program stalled");
        if (terminal.cols() != cols || terminal.rows() != rows) {
            // The program changed the size (DECCOLM, XTWINOPS).
            cols = terminal.cols();
            rows = terminal.rows();
            recorder.size(cols, rows);
        }
        if (auto wrong = checkTerminal(terminal)) return failed(step, *wrong);

        const AfterglowVt::Modes& modes = terminal.modes();
        const std::uint64_t kind = input.below(100);
        std::string bytes;
        if (kind < 35) {
            const std::string text = input.text();
            AfterglowVt::KeyEvent key;
            key.text = text;
            bytes = AfterglowVt::encodeKey(key, modes);
            action = "text";
        } else if (kind < 65) {
            AfterglowVt::KeyEvent key;
            key.key = input.key();
            key.mods = input.mods();
            bytes = AfterglowVt::encodeKey(key, modes);
            if (modes.kittyKeyboard != 0 && input.chance(50)) {
                key.action = AfterglowVt::KeyAction::Release;
                bytes += AfterglowVt::encodeKey(key, modes);
            }
            action = "a key";
        } else if (kind < 72) {
            // Ctrl and a letter. Not Ctrl+Z: with no shell to resume it, a
            // stopped program waits forever. Not Ctrl+S: XOFF stops output.
            static constexpr std::string_view Letters = "abcdefghijklmnopqrtuvwxy";
            const char letter[2] = {Letters[input.below(Letters.size())], 0};
            AfterglowVt::KeyEvent key;
            key.text = letter;
            key.unshifted = char32_t(letter[0]);
            key.mods = AfterglowVt::Mod::Ctrl;
            bytes = AfterglowVt::encodeKey(key, modes);
            action = std::string("Ctrl+") + letter;
        } else if (kind < 82) {
            AfterglowVt::MouseEvent mouse;
            static constexpr std::array<AfterglowVt::MouseButton, 6> Buttons = {
                AfterglowVt::MouseButton::Left,    AfterglowVt::MouseButton::Middle,  AfterglowVt::MouseButton::Right,
                AfterglowVt::MouseButton::WheelUp, AfterglowVt::MouseButton::WheelDown, AfterglowVt::MouseButton::Left};
            mouse.button = Buttons[input.below(Buttons.size())];
            mouse.col = int(input.below(std::uint64_t(terminal.cols())));
            mouse.row = int(input.below(std::uint64_t(terminal.rows())));
            mouse.pixelX = mouse.col * PtyHost::CellWidth + 3;
            mouse.pixelY = mouse.row * PtyHost::CellHeight + 7;
            bytes = AfterglowVt::encodeMouse(mouse, modes);
            if (mouse.button < AfterglowVt::MouseButton::None) {
                // A drag to another cell, then the release there.
                mouse.action = AfterglowVt::MouseAction::Motion;
                mouse.buttonHeld = true;
                mouse.col = int(input.below(std::uint64_t(terminal.cols())));
                mouse.row = int(input.below(std::uint64_t(terminal.rows())));
                bytes += AfterglowVt::encodeMouse(mouse, modes);
                mouse.action = AfterglowVt::MouseAction::Release;
                bytes += AfterglowVt::encodeMouse(mouse, modes);
            }
            action = "the mouse";
        } else if (kind < 85) {
            std::string text;
            for (std::uint64_t n = 1 + input.below(5); n > 0; --n) text += input.text() + "\n";
            bytes = AfterglowVt::encodePaste(text, modes);
            action = "a paste";
        } else if (kind < 87) {
            bytes = AfterglowVt::encodeFocus(input.chance(50), modes);
            action = "a focus change";
        } else if (kind < 91) {
            // Mostly the sizes of windows, sometimes a tiny one.
            cols = input.chance(85) ? 20 + int(input.below(180)) : PtyHost::MinCols + int(input.below(10));
            rows = input.chance(85) ? 5 + int(input.below(55)) : 1 + int(input.below(4));
            terminal.resize(cols, rows, PtyHost::CellWidth, PtyHost::CellHeight);
            cols = terminal.cols();
            rows = terminal.rows();
            PtyHost::setPtySize(program->master, cols, rows);
            recorder.size(cols, rows);
            action = "a resize to " + std::to_string(cols) + "x" + std::to_string(rows);
        } else if (kind < 95) {
            const std::uint64_t how = input.below(3);
            if (how == 0) terminal.scrollViewport(long(input.below(200)) - 100);
            else if (how == 1) terminal.setViewportOffset(std::size_t(input.below(3000)));
            else terminal.resetViewport();
            action = "a scroll of the scrollback";
        } else {
            // As the window does: a selection over the scrollback and its
            // text, a search, and the text of the whole screen.
            const AfterglowVt::Screen& s = terminal.screen();
            AfterglowVt::Selection selection;
            const auto point = [&] {
                return AfterglowVt::SelectionPoint{s.rowsDropped() + input.below(s.totalRows()),
                                                   int(input.below(std::uint64_t(s.cols())))};
            };
            selection.start(point(), AfterglowVt::SelectionMode(input.below(4)), s.layoutEpoch(), true);
            selection.head = point();
            (void)selection.text(s);
            static constexpr std::array<std::u32string_view, 4> Needles = {U"a", U"e", U"中", U"😀"};
            (void)AfterglowVt::findAll(s, Needles[input.below(Needles.size())]);
            (void)terminal.text(AfterglowVt::Terminal::TextScope::All);
            action = "a selection and a search";
        }
        if (!bytes.empty()) program->fed = true;
        host.writeToPty(bytes);
    }
    // The last step's output gets its check too.
    if (settle()) {
        if (auto wrong = checkTerminal(terminal)) return failed(step, *wrong);
    }
    stop(*program);
    std::printf("afterglow_tuifuzz: %s: seed %llu, %ld steps, %ld restarts, %zu bytes read, all checks passed\n",
                o.command.front().c_str(), static_cast<unsigned long long>(o.seed), step, restarts, bytesRead);
    return 0;
}
