// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// aglow transfer: kitten transfer's command line over the client session of
// afterglow-proto. This file only drives the terminal and shows progress.

#include "Cli.h"
#include "Commands.h"
#include "Tty.h"

#include <AfterglowProto/TransferClient.h>

#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
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
          "copied to the same path on the receiving computer. Files under the home directory go under the home "
          "directory of the receiving computer. In normal mode the last argument is the destination on the "
          "receiving computer. It must be an existing directory unless a single file is copied, and it should "
          "then end with a slash." },
        { "--compress", Cli::Option::Kind::Choice, "auto", { "auto", "never", "always" },
          "Whether to compress the data. auto leaves out files that are compressed already, such as archives, "
          "images and video." },
        { "--permissions-bypass -p", Cli::Option::Kind::String, "", {},
          "The password that skips the permission prompt of the terminal. It must match the password of the "
          "terminal. White space at its start and end is removed. A value that starts with ., / or ~ is a file to "
          "read the password from. - reads it from standard input. A number below 256 is a file descriptor to read "
          "it from." },
        { "--confirm-paths -c", Cli::Option::Kind::Flag, "", {},
          "Before any file data moves, show where each file goes and ask for confirmation." },
        { "--transmit-deltas -x", Cli::Option::Kind::Flag, "", {},
          "When a file exists on the receiving side, send only the changes (the rsync algorithm). This is slower "
          "for small files or on a fast link." },
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
    const bool ok = readAll(fd, text);
    if (close) {
        ::close(fd);
    }
    if (!ok) {
        error = "Cannot read the password: " + std::string(std::strerror(errno));
        return false;
    }
    password = trim(std::move(text));
    return true;
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
        m_start = nowMs();
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
        std::uint64_t total = 0;
        std::uint64_t done = 0;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const auto& item = items[i];
            total += item.size;
            done += std::min(item.done, item.size);
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
        const double seconds = double(now - m_start) / 1000.0;
        std::string line = std::string(spinner[(now / 100) % 10]) + " ";
        if (m_client.state() == State::Asking) {
            line += m_sending || !items.empty() ? "Waiting for the terminal…" : "Waiting for the file list…";
        } else if (total == 0) {
            line += "Transferring metadata…";
        } else {
            line += humanSize(done) + " of " + humanSize(total);
            if (seconds > 0.5) {
                line += " @ " + humanSize(std::uint64_t(double(done) / seconds)) + "/s";
            }
            const int width = 20;
            const int filled = int(double(width) * double(done) / double(total));
            line += " [" + std::string(std::size_t(filled), '#') + std::string(std::size_t(width - filled), '.') + "]";
        }
        const auto columns = std::size_t(m_tty.size().ws_col);
        if (columns > 1 && line.size() > columns * 3) {
            line.resize(columns);
        }
        m_tty.write(out + line);
    }

    void clear() { m_tty.write("\r\x1b[K"); }

private:
    Tty& m_tty;
    const TransferClient& m_client;
    bool m_sending;
    std::vector<bool> m_reported;
    long long m_start = 0;
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
    if (!readPassword(parsed->value("permissions-bypass"), options.password, error)) {
        report("transfer", error);
        return 1;
    }
    if (parsed->args.empty()) {
        report("transfer", "Must specify at least one file to transfer");
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
        std::printf("Found %zu files and directories, requesting transfer permission…\n", client->items().size());
    }
    std::fflush(stdout);
    Tty tty;
    if (!tty.open(error)) {
        report("transfer", error);
        return 1;
    }
    tty.enterModes("\x1b[?25l", "\x1b[?25h");
    Progress progress(tty, *client, sending);
    long long cancelAt = -1;
    bool asked = false;
    bool ttyGone = false;
    auto cancel = [&](std::string_view why) {
        if (client->state() == State::Canceling) {
            progress.clear();
            tty.write("Waiting for the terminal to cancel the transfer…\n");
            return;
        }
        progress.clear();
        tty.write("\x1b[31m" + std::string(why) + "\x1b[39m\n");
        client->cancel(why);
        cancelAt = nowMs();
    };
    while (!client->over()) {
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
        if (!tty.turn(tty.pending() ? 50 : 100, inputs)) {
            ttyGone = true;
            break;
        }
        for (const auto& input : inputs) {
            if (input.type == Input::Type::Osc && input.data.starts_with("5113;")) {
                client->receive(std::string_view(input.data).substr(5));
            } else if (input.type == Input::Type::Interrupt) {
                cancel("Interrupt requested, cancelling transfer, transferred files are in undefined state");
            } else if (input.type == Input::Type::Escape) {
                if (client->state() == State::Confirm) {
                    client->confirm(false);
                    cancelAt = nowMs();
                } else {
                    cancel("Interrupt requested, cancelling transfer, transferred files are in undefined state");
                }
            } else if (input.type == Input::Type::Text && client->state() == State::Confirm) {
                if (input.data == "y" || input.data == "Y") {
                    client->confirm(true);
                } else if (input.data == "n" || input.data == "N") {
                    client->confirm(false);
                    cancelAt = nowMs();
                }
            }
        }
        if (const int signal = tty.takeSignal()) {
            cancel(signal == SIGINT ? "Interrupt requested, cancelling transfer" : "Terminate requested, cancelling transfer");
        }
        tty.takeResize();
        if (cancelAt >= 0 && nowMs() - cancelAt > CancelWaitMs) {
            break;
        }
        if (client->state() != State::Confirm) {
            progress.update(false);
        }
    }
    if (!ttyGone) {
        progress.update(true);
        progress.clear();
    }
    tty.restore();
    int code = 0;
    const auto& items = client->items();
    std::size_t failed = 0;
    for (const auto& item : items) {
        failed += item.error.empty() ? 0 : 1;
    }
    if (client->state() != State::Done) {
        const auto& why = client->error();
        report("transfer", why.empty() ? (ttyGone ? "The terminal closed" : "The transfer was canceled") : why);
        code = 1;
    } else {
        if (client->deltaFileBytes() > 0) {
            const auto moved = client->deltaBytes() + client->signatureBytes();
            std::printf("Rsync stats:\n  Delta size: %s Signature size: %s\n  Transmitted: %s of a total of %s (%.1f%%)\n",
                        humanSize(client->deltaBytes()).c_str(), humanSize(client->signatureBytes()).c_str(),
                        humanSize(moved).c_str(), humanSize(client->deltaFileBytes()).c_str(),
                        100.0 * double(moved) / double(client->deltaFileBytes()));
        }
        if (failed) {
            std::fprintf(stderr, "Transfer of %zu out of %zu files failed\n", failed, items.size());
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
