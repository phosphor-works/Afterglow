// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// aglow transfer: kitten transfer's command line over the client session of
// afterglow-proto. This file only drives the terminal and shows progress.

#include "Cli.h"
#include "Commands.h"
#include "Tty.h"

#include <AfterglowProto/Fd.h>
#include <AfterglowProto/TransferClient.h>
#include <AfterglowProto/TransferPath.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <iterator>
#include <limits>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

namespace Aglow {

namespace {

using AfterglowProto::TransferClient;
using AfterglowProto::TransferClientOptions;
using AfterglowProto::TransferItem;
using State = TransferClient::State;

// Time that a canceled session waits for the terminal's answer.
constexpr long long CancelWaitMs = 5000;
// Output that waits for the terminal before more file data is read.
constexpr std::size_t WriteAhead = 256 * 1024;
constexpr long long ProgressMs = 200;
// The longest password that --permissions-bypass reads from a file or a descriptor.
constexpr std::size_t MaxPassword = 4096;

const Cli::Command command {
    "transfer",
    "source_files_or_directories destination_path",
    "Transfer files over the TTY device.",
    "Copies files between this computer and the computer of the terminal, through the terminal itself: over SSH, "
    "in a container or on a serial line. Directories are copied with their contents. Symbolic links, hard links, "
    "permissions and modification times are kept.\n"
    "\n"
    "To copy a file from this computer to the terminal's computer:\n"
    "  aglow transfer some-file /path/on/the/terminal/computer\n"
    "\n"
    "To copy a file from the terminal's computer to this computer:\n"
    "  aglow transfer --direction=receive /path/on/the/terminal/computer local-file\n"
    "\n"
    "With more than one source, the destination must be a directory. Relative paths start at the working "
    "directory here, and at the home directory on the terminal's computer. The terminal asks for permission "
    "before any file moves.",
    {
        { "--direction -d", Cli::Option::Kind::Choice, "download", { "upload", "download", "send", "receive" },
          "Whether to send or receive files. send or download copy files from the computer where aglow runs to the "
          "computer of the terminal. receive or upload copy files from the computer of the terminal to the computer "
          "where aglow runs." },
        { "--mode -m", Cli::Option::Kind::Choice, "normal", { "normal", "mirror" },
          "How to read the arguments. In mirror mode every argument is a file or directory on the sending computer, "
          "copied to the same path on the receiving computer. When aglow sends, a relative path starts at the working "
          "directory, and a path under the home directory goes under the home directory of the other computer. When "
          "aglow receives, a path given as ~, as ~/ and a path, or as a relative path is under the home directory on "
          "both computers. An absolute path stays the same. In normal mode the "
          "last argument is the destination on the receiving computer. With more than one source, the destination "
          "is a directory that receives them. With one source, end the destination with a slash to put the source "
          "in that directory. Without the slash, the source is copied to the destination path itself. A received "
          "source also goes into the destination when it is an existing directory here." },
        { "--compress", Cli::Option::Kind::Choice, "auto", { "auto", "never", "always" },
          "Whether to compress the data. auto leaves out files that are compressed already, such as archives, "
          "images, audio and video." },
        { "--permissions-bypass -p", Cli::Option::Kind::String, "", {},
          "The password that skips the permission prompt of the terminal. It must match the password of the "
          "terminal. White space at its start and end is removed. A value that starts with ., / or ~ is a file to "
          "read the password from. A value of - reads it from standard input, without echo on a terminal. A number below 256 "
          "is a file descriptor to read it from. A password given as the value itself is visible to other users in "
          "the list of processes. Give a file or standard input instead." },
        { "--confirm-paths -c", Cli::Option::Kind::Flag, "", {},
          "Before any file data moves, show where each file goes and ask for confirmation." },
        { "--transmit-deltas -x", Cli::Option::Kind::Flag, "", {},
          "When a file exists on the receiving side, send only the changes (the rsync algorithm). This is slower "
          "for small files or on a fast link. When aglow receives, the terminal learns which destination files "
          "exist here, their sizes and the hashes of their blocks." },
    },
};

std::string trim(std::string text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

volatile sig_atomic_t g_secretSignal = 0;

extern "C" void onSecretSignal(int number)
{
    g_secretSignal = number;
}

// Reads at most MaxPassword bytes of @p fd. A terminal gives one line,
// without echo.
//
// On a terminal, the signals that end or stop the program stay blocked
// except while it waits for the line: a signal then ends the wait, the
// terminal gets its echo back, and the signal has its usual effect. After a
// stop the prompt starts again, since the terminal dropped the typed text.
bool readSecret(int fd, std::string& text, std::string& error)
{
    termios saved { };
    if (!::isatty(fd) || ::tcgetattr(fd, &saved) != 0) {
        int failure = 0;
        char buffer[512];
        for (;;) {
            const ssize_t n = ::read(fd, buffer, sizeof buffer);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                failure = n < 0 ? errno : 0;
                break;
            }
            text.append(buffer, std::size_t(n));
            if (text.size() > MaxPassword) {
                error = "The password is longer than " + std::to_string(MaxPassword) + " bytes";
                return false;
            }
        }
        if (failure) {
            error = "Cannot read the password: " + std::string(std::strerror(failure));
            return false;
        }
        return true;
    }
    constexpr int Signals[] = { SIGINT, SIGTERM, SIGHUP, SIGTSTP };
    sigset_t held { };
    sigemptyset(&held);
    for (const int number : Signals) {
        sigaddset(&held, number);
    }
    for (;;) {
        sigset_t old { };
        ::sigprocmask(SIG_BLOCK, &held, &old);
        // No SA_RESTART: the wait must end. A signal that the program
        // started with ignored stays ignored.
        struct sigaction catcher { };
        catcher.sa_handler = onSecretSignal;
        sigemptyset(&catcher.sa_mask);
        struct sigaction previous[std::size(Signals)] { };
        for (std::size_t i = 0; i < std::size(Signals); ++i) {
            ::sigaction(Signals[i], nullptr, &previous[i]);
            if (previous[i].sa_handler != SIG_IGN) {
                ::sigaction(Signals[i], &catcher, nullptr);
            }
        }
        g_secretSignal = 0;
        termios quiet = saved;
        quiet.c_lflag &= ~tcflag_t(ECHO);
        quiet.c_lflag |= ICANON;
        ::tcsetattr(fd, TCSAFLUSH, &quiet);
        AfterglowProto::writeAll(STDERR_FILENO, "Password: ");
        int failure = 0;
        char buffer[512];
        text.clear();
        while (!g_secretSignal) {
            // ppoll unblocks the signals only for the wait, so that none
            // comes between a check and a read that would block.
            pollfd ready { fd, POLLIN, 0 };
            if (::ppoll(&ready, 1, nullptr, &old) < 0) {
                if (errno == EINTR) {
                    continue;
                }
                failure = errno;
                break;
            }
            const ssize_t n = ::read(fd, buffer, sizeof buffer);
            if (n < 0 && errno == EINTR) {
                continue;
            }
            if (n <= 0) {
                failure = n < 0 ? errno : 0;
                break;
            }
            text.append(buffer, std::size_t(n));
            if (text.size() > MaxPassword) {
                error = "The password is longer than " + std::to_string(MaxPassword) + " bytes";
                break;
            }
            if (text.find('\n') != std::string::npos) {
                break;
            }
        }
        ::tcsetattr(fd, TCSAFLUSH, &saved);
        AfterglowProto::writeAll(STDERR_FILENO, "\n");
        for (std::size_t i = 0; i < std::size(Signals); ++i) {
            ::sigaction(Signals[i], &previous[i], nullptr);
        }
        const int caught = g_secretSignal;
        ::sigprocmask(SIG_SETMASK, &old, nullptr);
        if (caught) {
            // With the disposition back, the signal ends or stops the
            // program here, as it would have without the prompt.
            ::raise(caught);
            if (caught == SIGTSTP) {
                continue;
            }
            text.clear();
            error = "The password prompt was interrupted";
            return false;
        }
        if (failure) {
            error = "Cannot read the password: " + std::string(std::strerror(failure));
        }
        return error.empty();
    }
}

bool readPassword(const std::string& location, std::string& password, std::string& error)
{
    if (location.empty()) {
        return true;
    }
    int fd = -1;
    bool close = false;
    unsigned number = 0;
    const auto [end, ec] = std::from_chars(location.data(), location.data() + location.size(), number);
    if (ec == std::errc() && end == location.data() + location.size() && number < 256) {
        fd = int(number);
    } else if (location == "-") {
        fd = STDIN_FILENO;
    } else if (location[0] == '.' || location[0] == '/' || location[0] == '~') {
        const auto path = AfterglowProto::TransferPath::absolute(location, homeDirectory(), currentDirectory());
        fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
        close = true;
        if (fd < 0) {
            error = "Cannot read the password from " + location + ": " + std::strerror(errno);
            return false;
        }
    } else {
        password = trim(location);
        return true;
    }
    std::string text;
    const bool ok = readSecret(fd, text, error);
    if (close) {
        ::close(fd);
    }
    if (!ok) {
        return false;
    }
    password = trim(std::move(text));
    return true;
}

// "3 files and 1 directory": what a list of items holds.
std::string describe(std::size_t files, std::size_t directories)
{
    const auto count = [](std::size_t n, const char* one, const char* many) {
        return std::to_string(n) + " " + (n == 1 ? one : many);
    };
    if (files && directories) {
        return count(files, "file", "files") + " and " + count(directories, "directory", "directories");
    }
    return directories ? count(directories, "directory", "directories") : count(files, "file", "files");
}

std::string describe(const std::vector<TransferItem>& items, bool failedOnly)
{
    std::size_t files = 0;
    std::size_t directories = 0;
    for (const auto& item : items) {
        if (failedOnly && item.error.empty()) {
            continue;
        }
        ++(item.type == TransferItem::Type::Directory ? directories : files);
    }
    return describe(files, directories);
}

// The first @p columns - 1 characters of @p line, so that it never wraps.
// The line holds characters of one column each.
std::string fit(std::string line, std::size_t columns)
{
    if (columns < 2) {
        return line;
    }
    std::size_t characters = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        // A UTF-8 continuation byte is part of the character before it.
        if ((static_cast<unsigned char>(line[i]) & 0xc0) != 0x80 && ++characters == columns) {
            line.resize(i);
            break;
        }
    }
    return line;
}

std::string_view typeLabel(TransferItem::Type type)
{
    switch (type) {
    case TransferItem::Type::Directory:
        return "dir ";
    case TransferItem::Type::Symlink:
        return "sym ";
    case TransferItem::Type::Link:
        return "link";
    case TransferItem::Type::Regular:
        break;
    }
    return "file";
}

class Progress
{
public:
    Progress(Tty& tty, const TransferClient& client, bool sending)
        : m_tty(tty)
        , m_client(client)
        , m_sending(sending)
    {
        m_reported.assign(client.items().size(), false);
    }

    void update(bool force)
    {
        const auto now = nowMs();
        if (!force && now - m_last < ProgressMs) {
            return;
        }
        m_last = now;
        const auto& items = m_client.items();
        if (m_reported.size() < items.size()) {
            m_reported.resize(items.size(), false);
        }
        std::string out = "\r\x1b[K";
        // Sizes come from the terminal: the sums saturate instead of wrapping.
        constexpr auto Most = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t total = 0;
        std::uint64_t done = 0;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto& item = items[i];
            total = item.size > Most - total ? Most : total + item.size;
            const auto moved = std::min(item.done, item.size);
            done = moved > Most - done ? Most : done + moved;
            if (item.finished && !m_reported[i] && !item.local.empty()) {
                m_reported[i] = true;
                const auto& shown = item.local;
                if (item.error.empty()) {
                    out += "\x1b[32m✔\x1b[39m " + printable(shown) + "\n";
                } else {
                    out += "\x1b[31m✘\x1b[39m " + printable(shown) + ": " + printable(item.error) + "\n\x1b[K";
                }
            }
        }
        static const char* spinner[] = { "⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏" };
        // The rate counts from the start of the data, not from the permission prompt.
        if (m_start < 0 && m_client.state() == State::Running) {
            m_start = now;
        }
        const double seconds = m_start < 0 ? 0.0 : double(now - m_start) / 1000.0;
        std::string line = std::string(spinner[(now / 100) % 10]) + " ";
        if (m_client.state() == State::Canceling) {
            line += "Canceling the transfer…";
        } else if (m_client.state() == State::Asking) {
            line += m_sending || !items.empty() ? "Waiting for the terminal…" : "Waiting for the file list…";
        } else if (total == 0) {
            line += "Transferring metadata…";
        } else {
            line += humanSize(done) + " of " + humanSize(total);
            if (seconds > 0.5) {
                line += " @ " + humanSize(std::uint64_t(double(done) / seconds)) + "/s";
            }
            const int width = 20;
            const int filled = std::clamp(int(double(width) * double(done) / double(total)), 0, width);
            line += " [" + std::string(std::size_t(filled), '#') + std::string(std::size_t(width - filled), '.') + "]";
        }
        m_tty.write(out + fit(std::move(line), std::size_t(m_tty.size().ws_col)));
    }

    void clear() { m_tty.write("\r\x1b[K"); }

private:
    Tty& m_tty;
    const TransferClient& m_client;
    bool m_sending;
    std::vector<bool> m_reported;
    long long m_start = -1;
    long long m_last = 0;
};

} // namespace

int runTransfer(int argc, char** argv)
{
    std::string error;
    bool help = false;
    const auto parsed = Cli::parse(command, argc, argv, error, help);
    if (!parsed) {
        if (help) {
            std::fputs(Cli::helpText(command).c_str(), stdout);
            return 0;
        }
        report("transfer", error);
        return 1;
    }
    TransferClientOptions options;
    options.home = homeDirectory();
    options.cwd = currentDirectory();
    options.deltas = parsed->flag("transmit-deltas");
    options.confirmPaths = parsed->flag("confirm-paths");
    options.mode = parsed->value("mode") == "mirror" ? TransferClientOptions::Mode::Mirror : TransferClientOptions::Mode::Normal;
    const auto compress = parsed->value("compress");
    options.compress = compress == "never"    ? TransferClientOptions::Compress::Never
        : compress == "always"                ? TransferClientOptions::Compress::Always
                                              : TransferClientOptions::Compress::Auto;
    // Before the password: a read of standard input for a command that cannot run is wasted.
    if (parsed->args.empty()) {
        report("transfer", "Must specify at least one file to transfer");
        return 1;
    }
    if (options.mode == TransferClientOptions::Mode::Normal && parsed->args.size() < 2) {
        report("transfer", "Must specify at least one source and a destination");
        return 1;
    }
    if (!readPassword(parsed->value("permissions-bypass"), options.password, error)) {
        report("transfer", error);
        return 1;
    }
    const auto direction = parsed->value("direction");
    const bool sending = direction == "send" || direction == "download";
    if (sending) {
        std::puts("Scanning files…");
    }
    auto client = sending ? AfterglowProto::makeTransferSender(options, parsed->args, error)
                          : AfterglowProto::makeTransferReceiver(options, parsed->args, error);
    if (!client) {
        report("transfer", error);
        return 1;
    }
    if (sending) {
        std::printf("Found %s. Asking the terminal for permission…\n", describe(client->items(), false).c_str());
    }
    std::fflush(stdout);
    Tty tty;
    if (!tty.open(error)) {
        report("transfer", error);
        return 1;
    }
    tty.enterModes("\x1b[?25l", "\x1b[?25h", "\x1b[?25l");
    // Statuses that the terminal still sends when the loop stops are read,
    // not typed into the shell.
    tty.drainOnRestore();
    Progress progress(tty, *client, sending);
    long long cancelAt = -1;
    bool asked = false;
    bool ttyGone = false;
    // A second cancel while the terminal has not answered the first.
    bool abandoned = false;
    // The signal that canceled the session: the exit status tells it, as a shell expects.
    int caught = 0;
    const auto ended = [&] {
        const auto state = client->state();
        return state == State::Done || state == State::Failed || state == State::Canceled;
    };
    auto cancel = [&](std::string_view why) {
        if (ended()) {
            return;
        }
        if (client->state() == State::Canceling) {
            abandoned = true;
            return;
        }
        client->cancel(why);
    };
    try {
        while (!client->over() && !abandoned) {
            if (client->state() == State::Confirm && !asked) {
                asked = true;
                progress.clear();
                std::string list = "The following file transfers will be performed:\n";
                for (const auto& item : client->items()) {
                    const auto& from = sending ? item.local : item.remote;
                    const auto& to = sending ? item.remote : item.local;
                    if (!from.empty() && !to.empty()) {
                        list += std::string(typeLabel(item.type)) + " " + printable(from) + " → " + printable(to) + "\n";
                    }
                }
                list += "Press \x1b[32my\x1b[39m to continue or \x1b[31mn\x1b[39m to abort\n";
                tty.write(list);
            }
            if (tty.pending() < WriteAhead) {
                std::string out;
                client->pull(out, WriteAhead);
                tty.write(out);
            }
            std::vector<Input> inputs;
            // Work left in the client (a delta of a large file) goes on at once.
            if (!tty.turn(tty.pending() ? 50 : client->hasWork() ? 0 : 100, inputs)) {
                ttyGone = true;
                break;
            }
            for (const auto& input : inputs) {
                if (input.type == Input::Type::Osc && input.data.starts_with("5113;")) {
                    client->receive(std::string_view(input.data).substr(5));
                } else if (input.type == Input::Type::Interrupt) {
                    // Ctrl+C in raw mode is the key, not SIGINT: the exit status is the same.
                    if (!ended() && !caught) {
                        caught = SIGINT;
                    }
                    cancel("Canceled by an interrupt");
                } else if (input.type == Input::Type::Escape) {
                    if (client->state() == State::Confirm) {
                        client->confirm(false);
                    } else {
                        cancel("Canceled by the user");
                    }
                } else if (input.type == Input::Type::Text && client->state() == State::Confirm) {
                    // The first answer in the text: a quick "y" can arrive with more keys.
                    const auto answer = std::find_if(input.data.begin(), input.data.end(),
                                                     [](char c) { return c == 'y' || c == 'Y' || c == 'n' || c == 'N'; });
                    if (answer != input.data.end()) {
                        client->confirm(*answer == 'y' || *answer == 'Y');
                    }
                }
            }
            if (const int signal = tty.takeSignal()) {
                if (!ended() && !caught) {
                    caught = signal;
                }
                cancel(signal == SIGINT    ? "Canceled by an interrupt"
                           : signal == SIGHUP ? "Canceled because the terminal hung up"
                                              : "Canceled by a termination signal");
            }
            tty.takeResize();
            // Also a cancel that the library sent for a reason of its own.
            if (client->state() == State::Canceling && cancelAt < 0) {
                cancelAt = nowMs();
            }
            if (cancelAt >= 0 && nowMs() - cancelAt > CancelWaitMs) {
                break;
            }
            if (client->state() != State::Confirm) {
                progress.update(false);
            }
        }
    } catch (...) {
        // The terminal ends the session too, else it waits for files that never come.
        client->cancel("Internal error");
        std::string out;
        client->pull(out, WriteAhead);
        // restore() writes it as the Tty goes out of scope.
        tty.write(out);
        throw;
    }
    if (!ttyGone) {
        progress.update(true);
        progress.clear();
    }
    tty.restore();
    int code = 0;
    const auto& items = client->items();
    const bool failed = std::any_of(items.begin(), items.end(), [](const TransferItem& item) { return !item.error.empty(); });
    if (client->state() != State::Done) {
        std::string why = client->error();
        if (why.empty()) {
            why = ttyGone ? "The terminal closed" : "The transfer was canceled";
        }
        // Completed files took their names. The others left no file.
        if (std::any_of(items.begin(), items.end(), [](const TransferItem& item) { return item.finished && item.error.empty(); })) {
            why += ". The files that were complete before it are in place";
        }
        report("transfer", why);
        code = caught ? 128 + caught : 1;
    } else {
        if (client->deltaFileBytes() > 0) {
            const auto moved = client->deltaBytes() + client->signatureBytes();
            std::printf("Rsync stats:\n  Delta size: %s Signature size: %s\n  Transmitted: %s of a total of %s (%.1f%%)\n",
                        humanSize(client->deltaBytes()).c_str(), humanSize(client->signatureBytes()).c_str(),
                        humanSize(moved).c_str(), humanSize(client->deltaFileBytes()).c_str(),
                        100.0 * double(moved) / double(client->deltaFileBytes()));
        }
        if (failed) {
            std::fprintf(stderr, "Failed to transfer %s:\n", describe(items, true).c_str());
            for (const auto& item : items) {
                if (!item.error.empty()) {
                    std::fprintf(stderr, "%s\n  %s\n", printable(item.local.empty() ? item.remote : item.local).c_str(),
                                 printable(item.error).c_str());
                }
            }
            code = 1;
        }
    }
    return code;
}

} // namespace Aglow
