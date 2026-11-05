// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

// aglow clipboard: kitten clipboard's command line. Reads and writes go
// through OSC 5522 with MIME types. A terminal without it gets OSC 52, which
// carries plain text only.

#include "Cli.h"
#include "Commands.h"
#include "Tty.h"

#include <AfterglowProto/Clipboard.h>
#include <AfterglowProto/FdDelta.h>
#include <AfterglowProto/Random.h>
#include <AfterglowProto/Text.h>
#include <AfterglowProto/TransferClient.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fnmatch.h>
#include <map>
#include <sys/stat.h>
#include <unistd.h>

namespace Aglow {

namespace {

using AfterglowProto::ClipboardReply;
using AfterglowProto::ClipboardRequest;

const Cli::Command command {
    "clipboard",
    "[files to copy to or from]",
    "Read or write the system clipboard, also over SSH.",
    "Without file arguments, aglow clipboard works as a filter: it copies standard input to the clipboard, or "
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
          "When reading the clipboard, wildcards match types: 'text/*' copies the first textual type. The type . "
          "lists the types on the clipboard." },
        { "--alias -a", Cli::Option::Kind::List, "", {},
          "Two MIME types that mean the same data, as text/plain=text/x-rst. A write offers both types. A read "
          "uses the other type when the first one is not on the clipboard. When the data has a textual type but "
          "no text/plain, text/plain is added as an alias." },
        { "--wait-for-completion", Cli::Option::Kind::Flag, "", {},
          "Wait until the terminal has the data before exiting. Only needed in filter mode with OSC 52: an OSC "
          "5522 write always waits." },
        { "--password", Cli::Option::Kind::String, "", {},
          "A password for the clipboard. When the user accepts it, later requests with it need no permission in "
          "this terminal. It is text:PASSWORD, fd:NUMBER (a file descriptor to read it from) or file:PATH. Give "
          "--human-name as well." },
        { "--human-name", Cli::Option::Kind::String, "", {},
          "The name of the program that the terminal shows when it asks for permission." },
        { "--detection-timeout", Cli::Option::Kind::Float, "10", {},
          "Seconds to wait for the terminal's answer." },
    },
};

std::string guessMime(const std::string& path)
{
    static const std::map<std::string, std::string, std::less<>> types {
        { "txt", "text/plain" }, { "text", "text/plain" }, { "md", "text/markdown" }, { "rst", "text/x-rst" },
        { "html", "text/html" }, { "htm", "text/html" }, { "css", "text/css" }, { "csv", "text/csv" },
        { "xml", "application/xml" }, { "json", "application/json" }, { "js", "text/javascript" },
        { "rtf", "text/rtf" }, { "uri", "text/uri-list" }, { "uris", "text/uri-list" },
        { "png", "image/png" }, { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" }, { "gif", "image/gif" },
        { "webp", "image/webp" }, { "bmp", "image/bmp" }, { "svg", "image/svg+xml" }, { "tif", "image/tiff" },
        { "tiff", "image/tiff" }, { "avif", "image/avif" }, { "pdf", "application/pdf" },
        { "zip", "application/zip" }, { "sh", "text/x-shellscript" }, { "py", "text/x-python" },
        { "c", "text/x-c" }, { "h", "text/x-c" }, { "cpp", "text/x-c++" }, { "log", "text/plain" },
    };
    const auto name = AfterglowProto::TransferPath::baseName(path);
    const auto dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0) {
        return {};
    }
    std::string extension = name.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return char(c >= 'A' && c <= 'Z' ? c + 32 : c); });
    const auto it = types.find(extension);
    return it == types.end() ? std::string() : it->second;
}

bool textual(std::string_view mime)
{
    return mime.starts_with("text/") || mime == "application/json" || mime == "application/xml"
        || mime == "application/javascript";
}

bool stream(const std::string& arg) { return arg == "-" || arg == "/dev/stdout" || arg == "/dev/stderr" || arg == "/dev/stdin"; }

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
        error = "Invalid password: no password type in " + spec;
        return false;
    }
    const auto type = spec.substr(0, colon);
    const auto value = spec.substr(colon + 1);
    if (type == "text") {
        password = value;
        return true;
    }
    int fd = -1;
    if (type == "fd") {
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), fd);
        if (ec != std::errc() || end != value.data() + value.size() || fd < 0) {
            error = "Not a valid file descriptor number: " + value;
            return false;
        }
    } else if (type == "file") {
        fd = ::open(value.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            error = "Failed to read from file " + value + ": " + std::strerror(errno);
            return false;
        }
    } else {
        error = "Invalid password type: " + type;
        return false;
    }
    std::string text;
    const bool ok = readAll(fd, text);
    ::close(fd);
    if (!ok) {
        error = "Failed to read the password: " + std::string(std::strerror(errno));
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
        return "The clipboard is busy, try again";
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
                return false;
            }
            if (input.data.starts_with("?5522;") && input.data.ends_with("$y")) {
                const char state = input.data.size() > 6 ? input.data[6] : '0';
                supported = state != '0' && state != '4';
                return false;
            }
            return input.data.starts_with('?') && input.data.ends_with('c');
        }, error);
        return answered && supported;
    }

    // Runs until @p done returns true for an input. False on a timeout, an
    // interrupt or a closed terminal, with @p error set.
    template<typename Done>
    bool wait(Done done, std::string& error)
    {
        const long long deadline = nowMs() + m_timeout;
        for (;;) {
            std::vector<Input> inputs;
            if (!m_tty.turn(100, inputs)) {
                error = "The terminal closed";
                return false;
            }
            for (const auto& input : inputs) {
                if (input.type == Input::Type::Interrupt) {
                    error = "Interrupted";
                    return false;
                }
                if (done(input)) {
                    return true;
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
    bool received = false;
};

bool writeOutput(const Output& output, std::string& error)
{
    if (stream(output.arg)) {
        const int fd = output.arg == "/dev/stderr" ? STDERR_FILENO : STDOUT_FILENO;
        if (!AfterglowProto::writeAll(fd, output.data)) {
            error = "Failed to write the data: " + std::string(std::strerror(errno));
            return false;
        }
        return true;
    }
    const auto path = AfterglowProto::TransferPath::absolute(output.arg, homeDirectory(), currentDirectory());
    std::string temporary = AfterglowProto::TransferPath::dirName(path) + "/.aglow-clipboard-XXXXXX";
    const int fd = ::mkostemp(temporary.data(), O_CLOEXEC);
    if (fd < 0 || !AfterglowProto::writeAll(fd, output.data) || ::fchmod(fd, 0644) != 0 || ::close(fd) != 0
        || ::rename(temporary.c_str(), path.c_str()) != 0) {
        error = "Failed to write " + output.arg + ": " + std::strerror(errno);
        if (fd >= 0) {
            ::unlink(temporary.c_str());
        }
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
        if (eq == std::string::npos) {
            report("clipboard", spec + " is not a valid MIME alias");
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
    for (std::size_t i = 0; i < args.size(); ++i) {
        Output file;
        file.arg = args[i];
        file.mime = i < mimes.size() ? mimes[i] : stream(args[i]) ? "text/plain" : guessMime(args[i]);
        if (file.mime.empty()) {
            report("clipboard", "Could not detect the MIME type for " + args[i] + ": use --mime to give it");
            return 1;
        }
        files.push_back(std::move(file));
    }
    if (!get) {
        // Read the inputs before the terminal is touched: a command piped
        // into aglow can itself write to the terminal.
        for (auto& file : files) {
            if (stream(file.arg)) {
                if (::isatty(STDIN_FILENO) && filter) {
                    report("clipboard", "Nothing to copy: pipe the data into aglow clipboard, or give files");
                    return 1;
                }
                if (!readAll(STDIN_FILENO, file.data)) {
                    report("clipboard", "Failed to read standard input: " + std::string(std::strerror(errno)));
                    return 1;
                }
                continue;
            }
            const auto path = AfterglowProto::TransferPath::absolute(file.arg, homeDirectory(), currentDirectory());
            const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
            if (fd < 0 || !readAll(fd, file.data)) {
                report("clipboard", "Failed to read " + file.arg + ": " + std::strerror(errno));
                if (fd >= 0) {
                    ::close(fd);
                }
                return 1;
            }
            ::close(fd);
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
                session.wait([](const Input& input) { return input.type == Input::Type::Csi && input.data.ends_with('c'); }, error);
            }
            tty.restore();
            return error.empty() ? 0 : (report("clipboard", error), 1);
        }
        message.query = true;
        tty.write(message.write());
        std::string text;
        const bool ok = session.wait([&](const Input& input) {
            if (input.type != Input::Type::Osc || !input.data.starts_with("52;")) {
                return false;
            }
            const auto reply = AfterglowProto::Osc52Message::parse(std::string_view(input.data).substr(3));
            if (reply) {
                text = reply->text;
            }
            return true;
        }, error);
        tty.restore();
        if (!ok) {
            report("clipboard", error);
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
        tty.write(AfterglowProto::clipboardWriteRequest(request));
        std::string status;
        const bool ok = session.wait([&](const Input& input) {
            if (input.type != Input::Type::Osc || !input.data.starts_with("5522;")) {
                return false;
            }
            const auto reply = ClipboardReply::parse(std::string_view(input.data).substr(5));
            if (!reply || reply->type != "write" || reply->id != AfterglowProto::sanitizeId(request.id)) {
                return false;
            }
            status = reply->status;
            return true;
        }, error);
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
    // A read: first the list of types, which needs no permission, then the types that match.
    request.kind = ClipboardRequest::Kind::Read;
    auto read = [&](const std::vector<std::string>& types, std::map<std::string, std::string>& data, std::string& status) {
        request.mimes = types;
        tty.write(AfterglowProto::clipboardReadRequest(request));
        return session.wait([&](const Input& input) {
            if (input.type != Input::Type::Osc || !input.data.starts_with("5522;")) {
                return false;
            }
            const auto reply = ClipboardReply::parse(std::string_view(input.data).substr(5));
            if (!reply || reply->type != "read" || reply->id != AfterglowProto::sanitizeId(request.id)) {
                return false;
            }
            if (reply->status == "DATA") {
                data[reply->mime] += reply->data;
                return false;
            }
            if (reply->status == "OK") {
                return false;
            }
            status = reply->status;
            return true;
        }, error);
    };
    std::map<std::string, std::string> listing;
    std::string status;
    if (!read({ "." }, listing, status) || status != "DONE") {
        tty.restore();
        report("clipboard", error.empty() ? statusError(status) : error);
        return 1;
    }
    std::vector<std::string> available;
    for (const auto& [mime, bytes] : listing) {
        std::size_t start = 0;
        while (start < bytes.size()) {
            auto end = bytes.find_first_of(" \n", start);
            if (end == std::string::npos) {
                end = bytes.size();
            }
            if (end > start) {
                available.push_back(bytes.substr(start, end - start));
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
        auto has = [&](const std::string& mime) { return std::find(available.begin(), available.end(), mime) != available.end(); };
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
            report("clipboard", "The MIME type " + file.mime + " for " + file.arg + " is not on the clipboard");
            return 1;
        }
        if (std::find(wanted.begin(), wanted.end(), file.remote) == wanted.end()) {
            wanted.push_back(file.remote);
        }
    }
    std::map<std::string, std::string> data;
    if (!wanted.empty()) {
        status.clear();
        if (!read(wanted, data, status) || status != "DONE") {
            tty.restore();
            report("clipboard", error.empty() ? statusError(status) : error);
            return 1;
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
            report("clipboard", "No data for " + file.arg + " with MIME type " + file.mime);
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
