// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: LGPL-2.1-or-later

#include "External.h"

#include <AfterglowProto/Graphics.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace Aglow {

namespace {

/// The full path of @p name on PATH: a regular file that this user may run.
std::string findProgram(std::string_view name)
{
    const char* path = std::getenv("PATH");
    std::string_view dirs = path ? path : "/usr/local/bin:/usr/bin:/bin";
    while (true) {
        const std::size_t colon = dirs.find(':');
        std::string dir(dirs.substr(0, colon));
        // An empty entry is the current directory, which a program found by
        // name must not come from.
        if (!dir.empty() && dir.front() == '/') {
            std::string candidate = dir + '/' + std::string(name);
            struct stat st { };
            if (::stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(candidate.c_str(), X_OK) == 0) {
                return candidate;
            }
        }
        if (colon == std::string_view::npos) {
            return {};
        }
        dirs.remove_prefix(colon + 1);
    }
}

struct Limits
{
    rlim_t cpuSeconds = 60;
    rlim_t addressSpace = RLIM_INFINITY;
    int wallSeconds = 60;
    std::uint64_t maxOutput = 0;
};

struct Result
{
    bool started = false;
    int status = 0;
    bool tooLarge = false; ///< the output passed Limits::maxOutput
    bool timedOut = false;
    bool interrupted = false;
    std::string out;
    std::string err; ///< its first 64 KiB
};

void setNonBlocking(int fd) { ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK); }

/// Runs @p program with @p args and @p environment, @p input on its standard
/// input, its standard output and error read back. The child ends with
/// aglow, has no core file, and cannot gain privileges.
Result run(const std::string& program, const std::vector<std::string>& args, const std::vector<std::string>& environment,
           std::string_view input, const Limits& limits, const std::function<bool()>& stop)
{
    Result result;
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    std::vector<char*> envp;
    for (const auto& entry : environment) envp.push_back(const_cast<char*>(entry.c_str()));
    envp.push_back(nullptr);

    int in[2], out[2], err[2];
    if (::pipe2(in, O_CLOEXEC) != 0) return result;
    if (::pipe2(out, O_CLOEXEC) != 0) {
        ::close(in[0]);
        ::close(in[1]);
        return result;
    }
    if (::pipe2(err, O_CLOEXEC) != 0) {
        for (const int fd : {in[0], in[1], out[0], out[1]}) ::close(fd);
        return result;
    }
    const pid_t parent = ::getpid();
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (::getppid() != parent) ::_exit(127);
        ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
        sigset_t none;
        sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        for (const int number : {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGPIPE, SIGWINCH, SIGCONT, SIGCHLD}) std::signal(number, SIG_DFL);
        const rlimit core { 0, 0 };
        ::setrlimit(RLIMIT_CORE, &core);
        const rlimit cpu { limits.cpuSeconds, limits.cpuSeconds + 1 };
        ::setrlimit(RLIMIT_CPU, &cpu);
        const rlimit space { limits.addressSpace, limits.addressSpace };
        ::setrlimit(RLIMIT_AS, &space);
        if (::dup2(in[0], STDIN_FILENO) < 0 || ::dup2(out[1], STDOUT_FILENO) < 0 || ::dup2(err[1], STDERR_FILENO) < 0) ::_exit(127);
        ::closefrom(3);
        ::execve(program.c_str(), argv.data(), envp.data());
        ::_exit(127);
    }
    ::close(in[0]);
    ::close(out[1]);
    ::close(err[1]);
    if (pid < 0) {
        for (const int fd : {in[1], out[0], err[0]}) ::close(fd);
        return result;
    }
    result.started = true;
    for (const int fd : {in[1], out[0], err[0]}) setNonBlocking(fd);
    // aglow ignores SIGPIPE (Tty.cpp): a child that exits before it reads
    // its input gives EPIPE here.
    int toChild = in[1];
    if (input.empty()) {
        ::close(toChild);
        toChild = -1;
    }
    int fromChild = out[0];
    int errors = err[0];
    std::size_t written = 0;
    bool killed = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(limits.wallSeconds);
    std::array<char, 65536> buffer {};
    while (fromChild >= 0 || errors >= 0) {
        if (!killed) {
            if (stop && stop()) {
                result.interrupted = killed = true;
            } else if (std::chrono::steady_clock::now() >= deadline) {
                result.timedOut = killed = true;
            } else if (result.tooLarge) {
                killed = true;
            }
            if (killed) {
                ::kill(pid, SIGKILL);
                if (toChild >= 0) {
                    ::close(toChild);
                    toChild = -1;
                }
            }
        }
        std::vector<pollfd> fds;
        if (toChild >= 0) fds.push_back({toChild, POLLOUT, 0});
        if (fromChild >= 0) fds.push_back({fromChild, POLLIN, 0});
        if (errors >= 0) fds.push_back({errors, POLLIN, 0});
        if (::poll(fds.data(), fds.size(), 100) < 0 && errno != EINTR) break;
        for (const pollfd& p : fds) {
            if (p.revents == 0) continue;
            if (p.fd == toChild) {
                const ssize_t n = ::write(toChild, input.data() + written, std::min<std::size_t>(input.size() - written, buffer.size()));
                if (n > 0) written += std::size_t(n);
                if ((n < 0 && errno != EAGAIN && errno != EINTR) || written == input.size()) {
                    ::close(toChild);
                    toChild = -1;
                }
                continue;
            }
            const ssize_t n = ::read(p.fd, buffer.data(), buffer.size());
            if (n > 0) {
                std::string& to = p.fd == fromChild ? result.out : result.err;
                if (p.fd == fromChild && to.size() + std::size_t(n) > limits.maxOutput) {
                    result.tooLarge = true;
                } else if (p.fd == fromChild || to.size() < 65536) {
                    to.append(buffer.data(), std::size_t(n));
                }
            } else if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                ::close(p.fd);
                (p.fd == fromChild ? fromChild : errors) = -1;
            }
        }
    }
    if (toChild >= 0) ::close(toChild);
    while (::waitpid(pid, &result.status, 0) < 0 && errno == EINTR) {
    }
    return result;
}

/// The first line of a program's error output without its program name
/// ("magick: ", "curl: (22) ") and ImageMagick's source place (" @ error/...").
std::string message(std::string_view text)
{
    text = text.substr(0, text.find('\n'));
    for (const std::string_view prefix : {"magick: ", "curl: "}) {
        if (text.starts_with(prefix)) text.remove_prefix(prefix.size());
    }
    if (text.starts_with('(')) {
        const std::size_t close = text.find(") ");
        if (close != std::string_view::npos) text.remove_prefix(close + 2);
    }
    if (const std::size_t place = text.find(" @ "); place != std::string_view::npos) text = text.substr(0, place);
    std::string out(text.substr(0, 300));
    for (char& c : out) {
        if (std::uint8_t(c) < 0x20 || c == 0x7f) c = '?';
    }
    return out;
}

std::vector<std::string> currentEnvironment(std::initializer_list<std::string_view> drop)
{
    std::vector<std::string> out;
    for (char** e = environ; e && *e; ++e) {
        const std::string_view entry(*e);
        if (std::none_of(drop.begin(), drop.end(), [&](std::string_view name) {
                return entry.starts_with(name) && entry.size() > name.size() && entry[name.size()] == '=';
            })) {
            out.emplace_back(entry);
        }
    }
    return out;
}

bool lowerEndsWith(std::string_view text, std::string_view suffix)
{
    if (text.size() < suffix.size()) return false;
    return std::equal(suffix.begin(), suffix.end(), text.end() - std::ptrdiff_t(suffix.size()),
                      [](char a, char b) { return a == char(std::tolower(std::uint8_t(b))); });
}

std::uint32_t little(std::string_view bytes, std::size_t at, int count)
{
    std::uint32_t value = 0;
    for (int i = count; i-- > 0;) value = value << 8 | std::uint8_t(bytes[at + std::size_t(i)]);
    return value;
}

/// The private directory of one run of magick: its policy and its
/// temporary files, removed with what magick left there.
class MagickHome
{
public:
    MagickHome()
    {
        const char* base = std::getenv("TMPDIR");
        std::string pattern = std::string(base && *base == '/' ? base : "/tmp") + "/aglow-magick-XXXXXX";
        if (::mkdtemp(pattern.data())) m_path = pattern;
    }
    ~MagickHome()
    {
        if (m_path.empty()) return;
        // Only plain files: magick writes its pixel cache and temporary
        // copies here.
        if (DIR* dir = ::opendir(m_path.c_str())) {
            while (const dirent* entry = ::readdir(dir)) {
                const std::string_view name(entry->d_name);
                if (name != "." && name != "..") ::unlinkat(::dirfd(dir), entry->d_name, 0);
            }
            ::closedir(dir);
        }
        ::rmdir(m_path.c_str());
    }
    MagickHome(const MagickHome&) = delete;
    MagickHome& operator=(const MagickHome&) = delete;
    const std::string& path() const { return m_path; }

private:
    std::string m_path;
};

} // namespace

std::string magickCoder(std::string_view bytes, std::string_view name)
{
    // ISO base media files (HEIF): the brands of their ftyp box.
    if (bytes.size() >= 16 && bytes.substr(4, 4) == "ftyp") {
        const std::size_t box = std::min<std::size_t>(std::max<std::uint32_t>(std::uint32_t(std::uint8_t(bytes[0])) << 24
                                                                               | std::uint32_t(std::uint8_t(bytes[1])) << 16
                                                                               | std::uint32_t(std::uint8_t(bytes[2])) << 8
                                                                               | std::uint8_t(bytes[3]), 16),
                                                      std::min<std::size_t>(bytes.size(), 256));
        std::vector<std::string_view> brands { bytes.substr(8, 4) };
        for (std::size_t at = 16; at + 4 <= box; at += 4) brands.push_back(bytes.substr(at, 4));
        const auto any = [&](std::initializer_list<std::string_view> wanted) {
            return std::any_of(brands.begin(), brands.end(), [&](std::string_view b) {
                return std::find(wanted.begin(), wanted.end(), b) != wanted.end();
            });
        };
        if (any({"avif", "avis"})) return "AVIF";
        if (any({"heic", "heix", "hevc", "hevx", "heim", "heis", "mif1", "msf1"})) return "HEIC";
        return {};
    }
    if (bytes.starts_with("\xff\x0a") || bytes.starts_with(std::string_view("\0\0\0\x0cJXL \r\n\x87\n", 12))) return "JXL";
    if (bytes.starts_with("qoif")) return "QOI";
    // ICO and CUR: a reserved 0, the kind, and at least one image.
    if (bytes.size() >= 6 && bytes.starts_with(std::string_view("\0\0", 2)) && bytes[3] == 0 && (bytes[2] == 1 || bytes[2] == 2)
        && little(bytes, 4, 2) > 0) {
        return bytes[2] == 1 ? "ICO" : "CUR";
    }
    // A BMP that aglow's reader refuses: run-length encoded, or JPEG or PNG
    // data in a BMP.
    if (bytes.size() >= 34 && bytes.starts_with("BM") && little(bytes, 14, 4) >= 40) {
        const std::uint32_t compression = little(bytes, 30, 4);
        if (compression == 1 || compression == 2 || compression == 4 || compression == 5) return "BMP";
    }
    // SVG: XML whose first element is svg, in the first 4 KiB.
    std::string_view head = bytes.substr(0, 4096);
    if (head.starts_with("\xef\xbb\xbf")) head.remove_prefix(3);
    while (!head.empty() && (head.front() == ' ' || head.front() == '\t' || head.front() == '\r' || head.front() == '\n')) {
        head.remove_prefix(1);
    }
    if (head.starts_with("<svg") || ((head.starts_with("<?xml") || head.starts_with("<!--") || head.starts_with("<!DOCTYPE"))
                                     && head.find("<svg") != std::string_view::npos)) {
        return "RSVG";
    }
    // TGA has no signature: its footer in version 2, else its name.
    if ((bytes.size() >= 26 && bytes.substr(bytes.size() - 18) == std::string_view("TRUEVISION-XFILE.\0", 18))
        || lowerEndsWith(name, ".tga")) {
        // The image types of TGA: colour-mapped, true-colour and grey,
        // raw or run-length encoded.
        const std::uint8_t type = bytes.size() >= 18 ? std::uint8_t(bytes[2]) : 0;
        if (type == 1 || type == 2 || type == 3 || type == 9 || type == 10 || type == 11) return "TGA";
    }
    return {};
}

std::string_view magickFormat(std::string_view coder)
{
    if (coder == "RSVG") return "SVG";
    if (coder == "JXL") return "JPEG XL";
    if (coder == "BMP") return "compressed BMP";
    return coder;
}

bool magickInstalled() { return !findProgram("magick").empty(); }

bool magickToPng(std::string_view bytes, const std::string& coder, std::string& png, std::string& error,
                 const std::function<bool()>& stop)
{
    const std::string program = findProgram("magick");
    if (program.empty()) {
        error = "aglow reads " + std::string(magickFormat(coder)) + " images with ImageMagick, and magick is not installed";
        return false;
    }
    MagickHome home;
    if (home.path().empty()) {
        error = std::string("cannot make a directory for ImageMagick: ") + std::strerror(errno);
        return false;
    }
    // The policy of this run: the one coder reads, PNG32 writes, and nothing
    // else: no delegate program (Ghostscript, Inkscape), no file list (@),
    // and resource limits. The system's own policy applies as well.
    const std::string policy = "<policymap>\n"
        "  <policy domain=\"delegate\" rights=\"none\" pattern=\"*\"/>\n"
        "  <policy domain=\"coder\" rights=\"none\" pattern=\"*\"/>\n"
        "  <policy domain=\"coder\" rights=\"read\" pattern=\"" + coder + "\"/>\n"
        "  <policy domain=\"coder\" rights=\"write\" pattern=\"PNG32\"/>\n"
        "  <policy domain=\"path\" rights=\"none\" pattern=\"@*\"/>\n"
        "  <policy domain=\"resource\" name=\"memory\" value=\"512MiB\"/>\n"
        "  <policy domain=\"resource\" name=\"map\" value=\"1GiB\"/>\n"
        "  <policy domain=\"resource\" name=\"disk\" value=\"1GiB\"/>\n"
        "  <policy domain=\"resource\" name=\"area\" value=\"100MP\"/>\n"
        "  <policy domain=\"resource\" name=\"width\" value=\"" + std::to_string(AfterglowProto::GraphicsMaxSide) + "\"/>\n"
        "  <policy domain=\"resource\" name=\"height\" value=\"" + std::to_string(AfterglowProto::GraphicsMaxSide) + "\"/>\n"
        "  <policy domain=\"resource\" name=\"time\" value=\"60\"/>\n"
        "  <policy domain=\"resource\" name=\"thread\" value=\"2\"/>\n"
        "  <policy domain=\"resource\" name=\"list-length\" value=\"64\"/>\n"
        "</policymap>\n";
    const std::string policyPath = home.path() + "/policy.xml";
    const int fd = ::open(policyPath.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    const bool saved = fd >= 0 && ::write(fd, policy.data(), policy.size()) == ssize_t(policy.size());
    if (fd >= 0) ::close(fd);
    if (!saved) {
        error = "cannot write the policy of ImageMagick";
        return false;
    }
    std::vector<std::string> environment = currentEnvironment({"MAGICK_CONFIGURE_PATH", "MAGICK_TEMPORARY_PATH", "MAGICK_TMPDIR"});
    environment.push_back("MAGICK_CONFIGURE_PATH=" + home.path());
    environment.push_back("MAGICK_TEMPORARY_PATH=" + home.path());
    // The data on standard input, read by the coder that its signature
    // gave: never a name, which ImageMagick reads as a coder (label:), a
    // file list (@) or a command (|) of older versions.
    Limits limits;
    limits.cpuSeconds = 60;
    limits.addressSpace = rlim_t(4) << 30;
    limits.wallSeconds = 90;
    limits.maxOutput = 2 * std::uint64_t(AfterglowProto::GraphicsDefaultQuota);
    // PNG32: 8-bit RGBA always, as ImageMagick's PNG writer drops the alpha
    // of an image that is all transparent. A transparent background, so that
    // an SVG keeps its empty areas.
    Result result = run(program, {"-background", "none", coder + ":-[0]", "PNG32:-"}, environment, bytes, limits, stop);
    if (!result.started) {
        error = std::string("cannot run magick: ") + std::strerror(errno);
        return false;
    }
    if (result.interrupted) {
        error = "interrupted";
        return false;
    }
    if (result.timedOut || (WIFSIGNALED(result.status) && WTERMSIG(result.status) == SIGXCPU)) {
        error = "ImageMagick takes longer than aglow allows";
        return false;
    }
    if (result.tooLarge) {
        error = "ImageMagick gives a larger image than aglow takes";
        return false;
    }
    if (!WIFEXITED(result.status) || WEXITSTATUS(result.status) != 0 || result.out.empty()) {
        const std::string why = message(result.err);
        error = "ImageMagick cannot read this " + std::string(magickFormat(coder)) + " image" + (why.empty() ? "" : ": " + why);
        return false;
    }
    png = std::move(result.out);
    return true;
}

bool isRemoteUrl(std::string_view text)
{
    const auto scheme = [&](std::string_view prefix) {
        return text.size() > prefix.size() && std::equal(prefix.begin(), prefix.end(), text.begin(),
                                                         [](char a, char b) { return a == char(std::tolower(std::uint8_t(b))); });
    };
    return scheme("http://") || scheme("https://") || scheme("ftp://");
}

bool download(const std::string& url, std::uint64_t maxBytes, std::string& out, std::string& error,
              const std::function<bool()>& stop)
{
    const std::string program = findProgram("curl");
    if (program.empty()) {
        error = "aglow downloads images with curl, and curl is not installed";
        return false;
    }
    // -q first: no .curlrc. Only http, https and ftp, also after a
    // redirect, no globbing of [] and {}, and the URL after --, so that it
    // is never an option.
    const std::vector<std::string> args { "-q", "--silent", "--show-error", "--fail", "--location", "--max-redirs", "10",
        "--proto", "=http,https,ftp", "--proto-redir", "=http,https,ftp", "--globoff",
        "--max-filesize", std::to_string(maxBytes), "--connect-timeout", "30", "--max-time", "300", "--", url };
    Limits limits;
    limits.cpuSeconds = 300;
    limits.wallSeconds = 310;
    limits.maxOutput = maxBytes;
    Result result = run(program, args, currentEnvironment({}), {}, limits, stop);
    if (!result.started) {
        error = std::string("cannot run curl: ") + std::strerror(errno);
        return false;
    }
    if (result.interrupted) {
        error = "interrupted";
        return false;
    }
    if (result.tooLarge || (WIFEXITED(result.status) && WEXITSTATUS(result.status) == 63)) {
        error = "it is larger than aglow reads as an image";
        return false;
    }
    if (result.timedOut) {
        error = "the download takes longer than aglow allows";
        return false;
    }
    if (!WIFEXITED(result.status) || WEXITSTATUS(result.status) != 0) {
        const std::string why = message(result.err);
        error = why.empty() ? "curl cannot download it" : why;
        return false;
    }
    out = std::move(result.out);
    return true;
}

} // namespace Aglow
