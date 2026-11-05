// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// aglow clipboard: kitten clipboard's command line. Reads and writes go
// through OSC 5522 with MIME types. A terminal without it gets OSC 52, which
// carries plain text only.

#include "Cli.h"
#include "Commands.h"
#include "Mime.h"
#include "Tty.h"

#include <AfterglowProto/Clipboard.h>
#include <AfterglowProto/Fd.h>
#include <AfterglowProto/Random.h>
#include <AfterglowProto/Text.h>
#include <AfterglowProto/TransferClient.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fnmatch.h>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace Aglow {

namespace {

using AfterglowProto::ClipboardAssembler;
using AfterglowProto::ClipboardReply;
using AfterglowProto::ClipboardRequest;

// The types of the clipboard that aglow looks at, of the list that the
// terminal gives: enough for any clipboard, and a bound on a list that is not.
constexpr std::size_t MaxListed = 1024;
// A password is a line of text.
constexpr std::size_t MaxPassword = 64 * 1024;

const Cli::Command command {
    "clipboard",
    "[files to copy to or from]",
    "Read or write the system clipboard, also over SSH.",
    "Without file arguments, aglow clipboard works as a filter. It copies standard input to the clipboard, or "
    "with --get-clipboard it writes the text of the clipboard to standard output.\n"
    "\n"
    "With file arguments it copies data of any MIME type. The MIME type of each file comes from its name, or "
    "from --mime. For example:\n"
    "  aglow clipboard picture.png\n"
    "  aglow clipboard --get-clipboard picture.png notes.txt\n"
    "  aglow clipboard --get-clipboard --mime . /dev/stdout\n"
    "\n"
    "The terminal can ask for permission before a program reads the clipboard. A terminal without OSC 5522 gets "
    "OSC 52, which carries plain text only.",
    {
        { "--get-clipboard -g", Cli::Option::Kind::Flag, "", {},
          "Write the contents of the clipboard to standard output, or to the files given. The terminal can ask "
          "for permission first." },
        { "--use-primary -p", Cli::Option::Kind::Flag, "", {},
          "Use the primary selection rather than the clipboard, on systems that have one." },
        { "--mime -m", Cli::Option::Kind::List, "", {},
          "The MIME type of a file. Give it once for each file, in order, when the name does not tell the type. "
          "When reading the clipboard, a wildcard matches types, so 'text/*' copies the first textual type. The "
          "type . lists the types on the clipboard." },
        { "--alias -a", Cli::Option::Kind::List, "", {},
          "Two MIME types that mean the same data, as text/plain=text/x-rst. A write offers both types. A read "
          "uses the other type when the first one is not on the clipboard. When the data has a textual type but "
          "no text/plain, text/plain is added as an alias." },
        { "--wait-for-completion", Cli::Option::Kind::Flag, "", {},
          "Wait until the terminal has the data before exiting. This matters only for a terminal that gets OSC "
          "52, in filter mode or with one text file. aglow always waits for the answer to an OSC 5522 write." },
        { "--password", Cli::Option::Kind::String, "", {},
          "A password for the clipboard. When the user accepts it, later requests with it need no permission in "
          "this terminal. It is text:PASSWORD, fd:NUMBER (a file descriptor to read it from) or file:PATH. Give "
          "--human-name as well." },
        { "--human-name", Cli::Option::Kind::String, "", {},
          "The name of the program that the terminal shows when it asks for permission." },
        { "--detection-timeout", Cli::Option::Kind::Float, "10", {}, "Seconds to wait for the terminal's answer.",
          0, 3600 },
    },
};

// A MIME type as the protocol carries it: printable ASCII without spaces.
bool typeName(std::string_view mime)
{
    return !mime.empty() && std::all_of(mime.begin(), mime.end(), [](char c) { return c > 0x20 && c < 0x7F; });
}

bool textual(std::string_view mime)
{
    return mime.starts_with("text/") || mime == "application/json" || mime == "application/xml"
        || mime == "application/javascript";
}

// Standard input as a source of data, and standard output and error as
// places for it. Each name means only the stream that it names.
bool inputStream(const std::string& arg) { return arg == "-" || arg == "/dev/stdin"; }
bool outputStream(const std::string& arg) { return arg == "-" || arg == "/dev/stdout" || arg == "/dev/stderr"; }

// How a message names @p arg.
std::string displayName(const std::string& arg, bool get)
{
    if (arg == "-") {
        return get ? "standard output" : "standard input";
    }
    if (arg == "/dev/stdin") {
        return "standard input";
    }
    if (arg == "/dev/stdout") {
        return "standard output";
    }
    if (arg == "/dev/stderr") {
        return "standard error";
    }
    return printable(arg);
}

// The file that @p arg names: ~ under the home directory, a relative path
// under the working directory (or as it is, when that has no path).
std::string localPath(const std::string& arg)
{
    const auto cwd = currentDirectory();
    if (cwd.empty() && !arg.starts_with('/') && !arg.starts_with('~')) {
        return arg;
    }
    return AfterglowProto::TransferPath::absolute(arg, homeDirectory(), cwd);
}

// Reads all of @p fd, up to @p limit bytes. A directory and anything larger fail.
bool readInput(int fd, std::size_t limit, std::string& out, std::string& error)
{
    struct stat st { };
    if (::fstat(fd, &st) != 0) {
        error = std::strerror(errno);
        return false;
    }
    if (S_ISDIR(st.st_mode)) {
        error = "it is a directory";
        return false;
    }
    const std::string tooLarge = "it is larger than " + humanSize(limit) + ", the most that aglow takes";
    if (S_ISREG(st.st_mode) && std::uint64_t(st.st_size) > limit) {
        error = tooLarge;
        return false;
    }
    char buffer[65536];
    for (;;) {
        const ssize_t n = ::read(fd, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0) {
            error = std::strerror(errno);
            return false;
        }
        if (n == 0) {
            return true;
        }
        if (out.size() + std::size_t(n) > limit) {
            error = tooLarge;
            return false;
        }
        out.append(buffer, std::size_t(n));
    }
}

std::string trimRight(std::string text)
{
    while (!text.empty() && (text.back() == ' ' || text.back() == '\n' || text.back() == '\r' || text.back() == '\t')) {
        text.pop_back();
    }
    return text;
}

bool readPassword(std::string spec, std::string& password, std::string& error)
{
    const auto colon = spec.find(':');
    if (colon == std::string::npos) {
        error = "Invalid password " + spec + ". Give it as text:PASSWORD, fd:NUMBER or file:PATH.";
        return false;
    }
    const auto type = spec.substr(0, colon);
    const auto value = spec.substr(colon + 1);
    if (type == "text") {
        password = value;
        return true;
    }
    // A descriptor that the caller passed stays open: it is the caller's.
    AfterglowProto::UniqueFd owned;
    int fd = -1;
    if (type == "fd") {
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), fd);
        if (ec != std::errc() || end != value.data() + value.size() || fd < 0) {
            error = "Not a valid file descriptor number: " + value;
            return false;
        }
    } else if (type == "file") {
        owned.reset(::open(localPath(value).c_str(), O_RDONLY | O_CLOEXEC));
        fd = owned.get();
        if (fd < 0) {
            error = "Failed to read from file " + printable(value) + ": " + std::strerror(errno);
            return false;
        }
    } else {
        error = "Invalid password type " + type + ". Give text, fd or file.";
        return false;
    }
    std::string text;
    std::string reason;
    if (!readInput(fd, MaxPassword, text, reason)) {
        error = "Failed to read the password: " + reason;
        return false;
    }
    password = trimRight(std::move(text));
    return true;
}

std::string statusError(const std::string& status)
{
    if (status == "ENOSYS") {
        return "The terminal does not have that clipboard (the primary selection may not exist)";
    }
    if (status == "EPERM") {
        return "Permission to use the clipboard was denied";
    }
    if (status == "EBUSY") {
        return "The clipboard is busy. Try again.";
    }
    if (status == "EFBIG") {
        return "The data is too large for the terminal";
    }
    return "The terminal reported an error: " + status;
}

class Session
{
public:
    Session(Tty& tty, long long timeoutMs)
        : m_tty(tty)
        , m_timeout(timeoutMs)
    {
    }

    // DECRQM 5522: kitty's way to ask whether OSC 5522 works. DA1 after it
    // answers in every terminal, so a missing answer is a no.
    bool supports5522(std::string& error)
    {
        m_tty.write("\x1b[?5522$p\x1b[c");
        bool supported = false;
        const bool answered = wait([&](const Input& input) {
            if (input.type != Input::Type::Csi) {
                return Step::Ignore;
            }
            if (input.data.starts_with("?5522;") && input.data.ends_with("$y")) {
                const char state = input.data.size() > 6 ? input.data[6] : '0';
                supported = state != '0' && state != '4';
                return Step::Ignore;
            }
            return input.data.starts_with('?') && input.data.ends_with('c') ? Step::Done : Step::Ignore;
        }, error, Limit::Answer);
        return answered && supported;
    }

    enum class Step { Ignore, More, Done };
    enum class Limit {
        Answer, ///< a query that every terminal answers at once: the timeout runs from the start
        Request, ///< a request the terminal may hold for a permission prompt: the timeout runs from its last packet
    };

    // Runs until @p step returns Done for an input, More for each packet of
    // the request, Ignore for the rest. False on a timeout, an interrupt or a
    // closed terminal, with @p error set. A request waits for its first
    // packet as long as the user takes to answer the terminal's prompt, as
    // kitten does: a timeout then would let the late answer reach the shell.
    // The timeout starts once the terminal has taken all that aglow queued:
    // a large write over a slow link is not an answer that is late.
    template<typename Stepper>
    bool wait(Stepper step, std::string& error, Limit limit)
    {
        long long deadline = limit == Limit::Answer ? nowMs() + m_timeout : LLONG_MAX;
        for (;;) {
            std::vector<Input> inputs;
            if (!m_tty.turn(100, inputs)) {
                error = "The terminal closed";
                return false;
            }
            if (m_tty.pending() > 0 && deadline != LLONG_MAX) {
                deadline = nowMs() + m_timeout;
            }
            for (const auto& input : inputs) {
                if (input.type == Input::Type::Interrupt) {
                    error = "Interrupted";
                    return false;
                }
                switch (step(input)) {
                case Step::Done:
                    return true;
                case Step::More:
                    deadline = nowMs() + m_timeout;
                    break;
                case Step::Ignore:
                    break;
                }
            }
            if (m_tty.takeSignal()) {
                error = "Interrupted";
                return false;
            }
            if (nowMs() > deadline) {
                error = "Timed out waiting for the terminal";
                return false;
            }
        }
    }

private:
    Tty& m_tty;
    long long m_timeout;
};

struct Output
{
    std::string arg;
    std::string mime;
    std::string remote;
    std::string data;
};

// Writes the data as a shell redirection does: a file is truncated and keeps
// its owner, mode, ACL and links; a new file gets 0666 less the umask; a link
// is followed, also to a file that does not exist yet; a pipe or a device
// takes the data as it is.
bool writeOutput(const Output& output, std::string& error)
{
    if (outputStream(output.arg)) {
        const int fd = output.arg == "/dev/stderr" ? STDERR_FILENO : STDOUT_FILENO;
        if (!AfterglowProto::writeAll(fd, output.data)) {
            error = "Failed to write the data: " + std::string(std::strerror(errno));
            return false;
        }
        return true;
    }
    const int fd = ::open(localPath(output.arg).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0) {
        error = "Failed to write " + printable(output.arg) + ": " + std::strerror(errno);
        return false;
    }
    const bool written = AfterglowProto::writeAll(fd, output.data);
    const int saved = errno;
    const bool closed = ::close(fd) == 0;
    if (!written || !closed) {
        error = "Failed to write " + printable(output.arg) + ": " + std::strerror(written ? errno : saved);
        return false;
    }
    return true;
}

} // namespace

int runClipboard(int argc, char** argv)
{
    std::string error;
    bool help = false;
    const auto parsed = Cli::parse(command, argc, argv, error, help);
    if (!parsed) {
        if (help) {
            std::fputs(Cli::helpText(command).c_str(), stdout);
            return 0;
        }
        report("clipboard", error);
        return 1;
    }
    const bool get = parsed->flag("get-clipboard");
    const bool primary = parsed->flag("use-primary");
    std::string password;
    const auto name = parsed->value("human-name");
    if (!parsed->value("password").empty()) {
        if (name.empty()) {
            report("clipboard", "Must specify --human-name when using a password");
            return 1;
        }
        if (!readPassword(parsed->value("password"), password, error)) {
            report("clipboard", error);
            return 1;
        }
    }
    // Aliases go both ways.
    std::multimap<std::string, std::string> aliases;
    for (const auto& spec : parsed->list("alias")) {
        const auto eq = spec.find('=');
        if (eq == std::string::npos || !typeName(spec.substr(0, eq)) || !typeName(spec.substr(eq + 1))) {
            report("clipboard", printable(spec) + " is not a valid MIME alias. Give two MIME types as TYPE=TYPE.");
            return 1;
        }
        aliases.emplace(spec.substr(0, eq), spec.substr(eq + 1));
        aliases.emplace(spec.substr(eq + 1), spec.substr(0, eq));
    }
    // The outputs or inputs, with their types.
    std::vector<Output> files;
    const auto mimes = parsed->list("mime");
    std::vector<std::string> args = parsed->args;
    const bool filter = args.empty();
    if (filter) {
        args.emplace_back(get ? "/dev/stdout" : "/dev/stdin");
    }
    bool stdinTaken = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        Output file;
        file.arg = args[i];
        if (!get && outputStream(file.arg) && !inputStream(file.arg)) {
            report("clipboard", "Cannot copy from " + displayName(file.arg, get) + ", which is an output");
            return 1;
        }
        if (get && file.arg == "/dev/stdin") {
            report("clipboard", "Cannot write the clipboard to standard input");
            return 1;
        }
        if (!get && inputStream(file.arg)) {
            if (stdinTaken) {
                report("clipboard", "Standard input can be read once only");
                return 1;
            }
            stdinTaken = true;
        }
        const bool streamed = get ? outputStream(file.arg) : inputStream(file.arg);
        file.mime = i < mimes.size() ? mimes[i] : streamed ? "text/plain" : Mime::typeOfName(file.arg);
        if (file.mime.empty()) {
            report("clipboard", "Could not detect the MIME type of " + displayName(file.arg, get) + ". Give it with --mime.");
            return 1;
        }
        // A read takes "." and wildcards. Every type must be one that the protocol carries.
        if (!typeName(file.mime) || (!get && (file.mime == "." || file.mime.find_first_of("*?[") != std::string::npos))) {
            report("clipboard", printable(file.mime) + " is not a valid MIME type for " + displayName(file.arg, get));
            return 1;
        }
        files.push_back(std::move(file));
    }
    if (!get) {
        for (std::size_t i = 0; i < files.size(); ++i) {
            for (std::size_t j = 0; j < i; ++j) {
                if (files[i].mime == files[j].mime) {
                    report("clipboard", displayName(files[j].arg, get) + " and " + displayName(files[i].arg, get)
                                            + " have the same MIME type " + files[i].mime + ". Give each a type of its own with --mime.");
                    return 1;
                }
            }
        }
        // Read the inputs before the terminal is touched: a command piped
        // into aglow can itself write to the terminal.
        std::size_t total = 0;
        for (auto& file : files) {
            int fd = STDIN_FILENO;
            AfterglowProto::UniqueFd owned;
            if (inputStream(file.arg)) {
                if (::isatty(STDIN_FILENO) && filter) {
                    report("clipboard", "Nothing to copy. Pipe the data into aglow clipboard, or give files.");
                    return 1;
                }
            } else {
                owned.reset(::open(localPath(file.arg).c_str(), O_RDONLY | O_CLOEXEC));
                fd = owned.get();
                if (fd < 0) {
                    report("clipboard", "Failed to read " + displayName(file.arg, get) + ": " + std::strerror(errno));
                    return 1;
                }
            }
            // A write carries at most MaxWrite bytes of all its types together.
            if (!readInput(fd, ClipboardAssembler::MaxWrite - total, file.data, error)) {
                report("clipboard", "Failed to read " + displayName(file.arg, get) + ": " + error);
                return 1;
            }
            total += file.data.size();
        }
    }
    Tty tty;
    if (!tty.open(error)) {
        report("clipboard", error);
        return 1;
    }
    Session session(tty, (long long)(parsed->real("detection-timeout") * 1000));
    const auto id = AfterglowProto::randomHex(6);
    ClipboardRequest request;
    request.primary = primary;
    request.id = "aglow-" + id.value_or("0");
    request.password = password;
    request.name = name;
    const bool modern = session.supports5522(error);
    if (!error.empty()) {
        tty.restore();
        report("clipboard", error);
        return 1;
    }
    if (!modern) {
        // OSC 52: plain text only.
        const bool allText = std::all_of(files.begin(), files.end(), [](const Output& f) { return textual(f.mime) || f.mime == "."; });
        if (files.size() != 1 || !allText || files[0].mime == ".") {
            tty.restore();
            report("clipboard", "The terminal does not support OSC 5522, and OSC 52 carries a single text only");
            return 1;
        }
        AfterglowProto::Osc52Message message;
        message.selection = primary ? "p" : "c";
        if (!get) {
            message.text = files[0].data;
            tty.write(message.write());
            if (parsed->flag("wait-for-completion")) {
                tty.write("\x1b[c");
                session.wait([](const Input& input) {
                    return input.type == Input::Type::Csi && input.data.ends_with('c') ? Session::Step::Done : Session::Step::Ignore;
                }, error, Session::Limit::Answer);
            }
            // The data is the terminal's only once it has taken all of it.
            if (!tty.restore() && error.empty()) {
                error = "The terminal did not take all of the data";
            }
            return error.empty() ? 0 : (report("clipboard", error), 1);
        }
        message.query = true;
        tty.allowClipboardReply();
        tty.drainOnRestore();
        // DA1 after the query: a terminal that answers it but not the query
        // has had the time it needs. The timeout runs from its answer.
        tty.write(message.write() + "\x1b[c");
        std::string text;
        bool valid = false;
        const bool ok = session.wait([&](const Input& input) {
            if (input.type == Input::Type::Csi && input.data.starts_with('?') && input.data.ends_with('c')) {
                return Session::Step::More;
            }
            if (input.type != Input::Type::Osc || !input.data.starts_with("52;")) {
                return Session::Step::Ignore;
            }
            const auto reply = AfterglowProto::Osc52Message::parse(std::string_view(input.data).substr(3));
            if (reply) {
                valid = true;
                text = reply->text;
            }
            return Session::Step::Done;
        }, error, Session::Limit::Request);
        tty.restore();
        if (!ok) {
            report("clipboard", error.starts_with("Timed out")
                                    ? "The terminal did not answer the OSC 52 read in time. It may not let programs read the clipboard."
                                    : error);
            return 1;
        }
        if (!valid) {
            report("clipboard", "The terminal answered with an OSC 52 reply that is not valid");
            return 1;
        }
        files[0].data = text;
        return writeOutput(files[0], error) ? 0 : (report("clipboard", error), 1);
    }
    if (!get) {
        request.kind = ClipboardRequest::Kind::Write;
        bool anyText = false;
        bool plain = false;
        for (const auto& file : files) {
            request.data.emplace_back(file.mime, file.data);
            const auto [first, last] = aliases.equal_range(file.mime);
            for (auto it = first; it != last; ++it) {
                request.aliases.emplace_back(it->second, file.mime);
                plain |= it->second == "text/plain";
            }
            anyText |= textual(file.mime);
            plain |= file.mime == "text/plain";
        }
        if (anyText && !plain) {
            for (const auto& file : files) {
                if (textual(file.mime)) {
                    request.aliases.emplace_back("text/plain", file.mime);
                    break;
                }
            }
        }
        // The limits of a write, checked here: a terminal refuses a write that breaks them.
        if (request.data.size() > ClipboardAssembler::MaxParts || request.aliases.size() > ClipboardAssembler::MaxParts) {
            tty.restore();
            report("clipboard", "A write takes at most " + std::to_string(ClipboardAssembler::MaxParts)
                                    + " MIME types and as many aliases");
            return 1;
        }
        tty.write(AfterglowProto::clipboardWriteRequest(request));
        std::string status;
        const bool ok = session.wait([&](const Input& input) {
            if (input.type != Input::Type::Osc || !input.data.starts_with("5522;")) {
                return Session::Step::Ignore;
            }
            const auto reply = ClipboardReply::parse(std::string_view(input.data).substr(5));
            if (!reply || reply->type != "write" || reply->id != AfterglowProto::sanitizeId(request.id)) {
                return Session::Step::Ignore;
            }
            status = reply->status;
            return Session::Step::Done;
        }, error, Session::Limit::Request);
        tty.restore();
        if (!ok) {
            report("clipboard", error);
            return 1;
        }
        if (status != "DONE" && status != "OK") {
            report("clipboard", statusError(status));
            return 1;
        }
        return 0;
    }
    // A read: first the list of types, which needs no permission, then the
    // types that match. Replies still on their way when aglow stops are read
    // before the terminal goes back to the shell.
    tty.drainOnRestore();
    request.kind = ClipboardRequest::Kind::Read;
    bool tooLarge = false;
    auto read = [&](const std::vector<std::string>& types, std::map<std::string, std::string>& data, std::string& status) {
        request.mimes = types;
        tty.write(AfterglowProto::clipboardReadRequest(request));
        std::size_t total = 0;
        const bool listing = types.size() == 1 && types[0] == ".";
        return session.wait([&](const Input& input) {
            if (input.type != Input::Type::Osc || !input.data.starts_with("5522;")) {
                return Session::Step::Ignore;
            }
            const auto reply = ClipboardReply::parse(std::string_view(input.data).substr(5));
            if (!reply || reply->type != "read" || reply->id != AfterglowProto::sanitizeId(request.id)) {
                return Session::Step::Ignore;
            }
            if (reply->status == "DATA") {
                // Only the types asked for. Past what a terminal takes in a
                // write the data is no clipboard, and the rest of it is read
                // and dropped, so that none of it reaches the shell.
                const bool wanted = listing ? data.contains(reply->mime) || data.size() < ClipboardAssembler::MaxParts
                                            : std::find(types.begin(), types.end(), reply->mime) != types.end();
                total += reply->data.size();
                tooLarge |= total > ClipboardAssembler::MaxWrite;
                if (wanted && !tooLarge) {
                    data[reply->mime] += reply->data;
                }
                return Session::Step::More;
            }
            if (reply->status == "OK") {
                return Session::Step::More;
            }
            status = reply->status;
            return Session::Step::Done;
        }, error, Session::Limit::Request);
    };
    auto failed = [&](const std::string& status) {
        tty.restore();
        if (tooLarge) {
            report("clipboard", "The clipboard holds more than " + humanSize(ClipboardAssembler::MaxWrite)
                                    + ", the most that aglow reads");
        } else {
            report("clipboard", error.empty() ? statusError(status) : error);
        }
        return 1;
    };
    std::map<std::string, std::string> listing;
    std::string status;
    if (!read({ "." }, listing, status) || status != "DONE" || tooLarge) {
        return failed(status);
    }
    std::vector<std::string> available;
    std::set<std::string> listed;
    for (const auto& [mime, bytes] : listing) {
        std::size_t start = 0;
        while (start < bytes.size() && available.size() < MaxListed) {
            auto end = bytes.find_first_of(" \n", start);
            if (end == std::string::npos) {
                end = bytes.size();
            }
            const auto type = bytes.substr(start, end - start);
            if (typeName(type) && listed.insert(type).second) {
                available.push_back(type);
            }
            start = end + 1;
        }
    }
    std::vector<std::string> wanted;
    for (auto& file : files) {
        if (file.mime == ".") {
            file.remote = ".";
            continue;
        }
        auto has = [&](const std::string& mime) { return listed.contains(mime); };
        if (has(file.mime)) {
            file.remote = file.mime;
        }
        const auto [first, last] = aliases.equal_range(file.mime);
        for (auto it = first; it != last && file.remote.empty(); ++it) {
            if (has(it->second)) {
                file.remote = it->second;
            }
        }
        for (const auto& mime : available) {
            if (file.remote.empty() && ::fnmatch(file.mime.c_str(), mime.c_str(), 0) == 0) {
                file.remote = mime;
            }
        }
        if (file.remote.empty() && textual(file.mime) && has("text/plain")) {
            file.remote = "text/plain";
        }
        if (file.remote.empty()) {
            tty.restore();
            report("clipboard", "The MIME type " + file.mime + " for " + displayName(file.arg, get) + " is not on the clipboard");
            return 1;
        }
        if (std::find(wanted.begin(), wanted.end(), file.remote) == wanted.end()) {
            wanted.push_back(file.remote);
        }
    }
    if (wanted.size() > ClipboardAssembler::MaxParts) {
        tty.restore();
        report("clipboard", "A read takes at most " + std::to_string(ClipboardAssembler::MaxParts) + " MIME types");
        return 1;
    }
    std::map<std::string, std::string> data;
    if (!wanted.empty()) {
        status.clear();
        if (!read(wanted, data, status) || status != "DONE" || tooLarge) {
            return failed(status);
        }
    }
    tty.restore();
    int code = 0;
    for (auto& file : files) {
        if (file.remote == ".") {
            for (const auto& mime : available) {
                file.data += mime + "\n";
            }
        } else if (const auto it = data.find(file.remote); it != data.end()) {
            file.data = it->second;
        } else {
            report("clipboard", "No data for " + displayName(file.arg, get) + " with MIME type " + file.mime);
            code = 1;
            continue;
        }
        if (!writeOutput(file, error)) {
            report("clipboard", error);
            code = 1;
        }
    }
    return code;
}

} // namespace Aglow
