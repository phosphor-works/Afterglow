// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#include "External.h"

#include <AfterglowProto/DndUri.h>
#include <AfterglowProto/Fd.h>
#include <AfterglowProto/Graphics.h>
#include <AfterglowProto/Limits.h>
#include <AfterglowPty/CloseFrom.h>

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
#if defined(__linux__)
#include <sys/prctl.h>
#elif defined(__FreeBSD__)
#include <sys/procctl.h>
#endif
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
            struct stat st{};
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

/// The first bytes of a program's error output that aglow keeps.
constexpr std::size_t MaxErrorOutput = 65536;

struct Result
{
    bool started = false;
    int startError = 0; ///< errno, when the program did not start
    int waitError = 0; ///< errno of a poll() that failed: the child was killed
    int status = 0;
    bool tooLarge = false; ///< the output passed Limits::maxOutput
    bool timedOut = false;
    bool interrupted = false;
    std::string out;
    std::string err; ///< its first MaxErrorOutput bytes
};

void setNonBlocking(int fd)
{
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
}

/// Writes errno to @p report, for the parent, and ends the child.
[[noreturn]] void failChild(int report)
{
    const int code = errno;
    (void)!::write(report, &code, sizeof code);
    ::_exit(127);
}

/// The child of run(), from fork() to exec(): only system calls. @p report
/// takes the errno of a step that fails, as the exec() of a program that
/// cannot run.
[[noreturn]] void startChild(const std::string& program, char** argv, char** envp, const int (&ends)[3], int report,
                             const Limits& limits, pid_t parent)
{
#if defined(__linux__)
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() != parent)
        ::_exit(127);
    ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
#elif defined(__FreeBSD__)
    int deathSignal = SIGKILL;
    ::procctl(P_PID, 0, PROC_PDEATHSIG_CTL, &deathSignal);
    if (::getppid() != parent)
        ::_exit(127);
    int noNewPrivileges = PROC_NO_NEW_PRIVS_ENABLE;
    ::procctl(P_PID, 0, PROC_NO_NEW_PRIVS_CTL, &noNewPrivileges);
#endif
    sigset_t none;
    sigemptyset(&none);
    ::sigprocmask(SIG_SETMASK, &none, nullptr);
    // exec() puts the handlers that aglow set back to the default, and keeps
    // a signal that aglow started with ignored (nohup) ignored. aglow
    // ignores SIGPIPE itself, and a program that ignores SIGCHLD cannot
    // wait for its own children.
    std::signal(SIGPIPE, SIG_DFL);
    std::signal(SIGCHLD, SIG_DFL);
    AfterglowProto::limit(RLIMIT_CORE, 0, 0);
    AfterglowProto::limit(RLIMIT_CPU, limits.cpuSeconds, limits.cpuSeconds + 1);
    AfterglowProto::limit(RLIMIT_AS, limits.addressSpace, limits.addressSpace);
    // Each end moves above standard error first: one that took a descriptor
    // that aglow started without (a closed standard input) would be
    // overwritten by the next dup2(), or keep O_CLOEXEC through a dup2() to
    // itself.
    report = ::fcntl(report, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
    if (report < 0)
        ::_exit(127);
    int moved[3];
    for (int i = 0; i < 3; ++i) {
        moved[i] = ::fcntl(ends[i], F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (moved[i] < 0)
            failChild(report);
    }
    for (int i = 0; i < 3; ++i) {
        if (::dup2(moved[i], i) < 0)
            failChild(report);
    }
    // The report on the first descriptor past standard error, and nothing
    // else of aglow: the terminal that it opened, its sockets.
    constexpr int ReportFd = STDERR_FILENO + 1;
    if (report != ReportFd) {
        if (::dup3(report, ReportFd, O_CLOEXEC) < 0)
            failChild(report);
        report = ReportFd;
    }
    AfterglowPty::closeFrom(ReportFd + 1);
    ::execve(program.c_str(), argv, envp);
    failChild(report);
}

/// Runs @p program with @p args and @p environment, @p input on its standard
/// input, its standard output and error read back. The child ends with
/// aglow, has no core file, and cannot gain privileges.
Result run(const std::string& program, const std::vector<std::string>& args,
           const std::vector<std::string>& environment, std::string_view input, const Limits& limits,
           const std::function<bool()>& stop)
{
    Result result;
    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(program.c_str()));
    for (const auto& arg : args)
        argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    std::vector<char*> envp;
    for (const auto& entry : environment)
        envp.push_back(const_cast<char*>(entry.c_str()));
    envp.push_back(nullptr);

    // Standard input, output and error of the child, and the pipe that
    // reports why it did not start.
    std::array<AfterglowProto::UniqueFd, 4> ours;
    std::array<AfterglowProto::UniqueFd, 4> theirs;
    for (std::size_t i = 0; i < ours.size(); ++i) {
        int fds[2];
        if (::pipe2(fds, O_CLOEXEC) != 0) {
            result.startError = errno;
            return result;
        }
        // The child reads its standard input and writes the others.
        ours[i].reset(i == 0 ? fds[1] : fds[0]);
        theirs[i].reset(i == 0 ? fds[0] : fds[1]);
    }
    const int ends[3] = {theirs[0].get(), theirs[1].get(), theirs[2].get()};
    const pid_t parent = ::getpid();
    const pid_t pid = ::fork();
    if (pid == 0)
        startChild(program, argv.data(), envp.data(), ends, theirs[3].get(), limits, parent);
    if (pid < 0) {
        result.startError = errno;
        return result;
    }
    for (auto& fd : theirs)
        fd.reset();
    // EOF once exec() closed the report, else the errno of the step that
    // failed.
    int code = 0;
    ssize_t n = 0;
    do {
        n = ::read(ours[3].get(), &code, sizeof code);
    } while (n < 0 && errno == EINTR);
    ours[3].reset();
    if (n == ssize_t(sizeof code)) {
        result.startError = code;
        while (::waitpid(pid, &result.status, 0) < 0 && errno == EINTR) { }
        return result;
    }
    result.started = true;
    for (std::size_t i = 0; i < 3; ++i)
        setNonBlocking(ours[i].get());
    // aglow ignores SIGPIPE (Tty.cpp): a child that exits before it reads
    // its input gives EPIPE here.
    if (input.empty())
        ours[0].reset();
    std::size_t written = 0;
    bool killed = false;
    bool reaped = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(limits.wallSeconds);
    // Kills the child once: on Ctrl+C, at the deadline, or past the output's limit.
    const auto check = [&] {
        if (killed)
            return;
        if (stop && stop()) {
            result.interrupted = killed = true;
        } else if (std::chrono::steady_clock::now() >= deadline) {
            result.timedOut = killed = true;
        } else if (result.tooLarge) {
            killed = true;
        }
        if (killed) {
            ::kill(pid, SIGKILL);
            ours[0].reset();
        }
    };
    std::array<char, 65536> buffer{};
    while (ours[1].valid() || ours[2].valid()) {
        check();
        // A process that the program left behind (a wrapper script's) can
        // hold the pipes open: once the killed child is gone, nothing more
        // is read.
        if (killed && ::waitpid(pid, &result.status, WNOHANG) == pid) {
            reaped = true;
            break;
        }
        std::vector<pollfd> fds;
        for (const auto& fd : ours) {
            if (fd.valid())
                fds.push_back({fd.get(), short(&fd == &ours[0] ? POLLOUT : POLLIN), 0});
        }
        if (::poll(fds.data(), fds.size(), 100) < 0) {
            if (errno == EINTR)
                continue;
            result.waitError = errno;
            ::kill(pid, SIGKILL);
            killed = true;
            break;
        }
        for (const pollfd& p : fds) {
            if (p.revents == 0)
                continue;
            if (p.fd == ours[0].get()) {
                const ssize_t sent =
                    ::write(p.fd, input.data() + written, std::min<std::size_t>(input.size() - written, buffer.size()));
                if (sent > 0)
                    written += std::size_t(sent);
                if ((sent < 0 && errno != EAGAIN && errno != EINTR) || written == input.size())
                    ours[0].reset();
                continue;
            }
            const bool output = p.fd == ours[1].get();
            const ssize_t got = ::read(p.fd, buffer.data(), buffer.size());
            if (got > 0) {
                if (output && result.out.size() + std::size_t(got) > limits.maxOutput) {
                    result.tooLarge = true;
                } else if (output) {
                    result.out.append(buffer.data(), std::size_t(got));
                } else {
                    result.err.append(
                        buffer.data(),
                        std::min(std::size_t(got), MaxErrorOutput - std::min(result.err.size(), MaxErrorOutput)));
                }
            } else if (got == 0 || (errno != EAGAIN && errno != EINTR)) {
                ours[output ? 1 : 2].reset();
            }
        }
    }
    for (auto& fd : ours)
        fd.reset();
    // A child that closed its output can still run: the same deadline and
    // Ctrl+C end it.
    while (!reaped) {
        const pid_t done = ::waitpid(pid, &result.status, killed ? 0 : WNOHANG);
        if (done == pid || (done < 0 && errno != EINTR))
            break;
        if (done == 0) {
            check();
            if (!killed)
                ::poll(nullptr, 0, 10);
        }
    }
    // Ctrl+C reaches the child too, which can end before aglow's own check
    // saw the signal: the ends of its pipes then end the loop above.
    if (!result.interrupted && !result.timedOut && stop && stop())
        result.interrupted = true;
    if (WIFSIGNALED(result.status) && !killed) {
        const int number = WTERMSIG(result.status);
        result.interrupted |= number == SIGINT || number == SIGTERM || number == SIGHUP;
    }
    return result;
}

/// The first line of a program's error output without its program name
/// ("magick: ", "curl: (22) ") and ImageMagick's source place (" @ error/...").
std::string message(std::string_view text)
{
    text = text.substr(0, text.find('\n'));
    for (const std::string_view prefix : {"magick: ", "curl: "}) {
        if (text.starts_with(prefix))
            text.remove_prefix(prefix.size());
    }
    if (text.starts_with('(')) {
        const std::size_t close = text.find(") ");
        if (close != std::string_view::npos)
            text.remove_prefix(close + 2);
    }
    if (const std::size_t place = text.find(" @ "); place != std::string_view::npos)
        text = text.substr(0, place);
    std::string out(text.substr(0, 300));
    for (char& c : out) {
        if (std::uint8_t(c) < 0x20 || c == 0x7f)
            c = '?';
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
    if (text.size() < suffix.size())
        return false;
    return std::equal(suffix.begin(), suffix.end(), text.end() - std::ptrdiff_t(suffix.size()), [](char a, char b) {
        return a == char(std::tolower(std::uint8_t(b)));
    });
}

std::uint32_t little(std::string_view bytes, std::size_t at, int count)
{
    std::uint32_t value = 0;
    for (int i = count; i-- > 0;)
        value = value << 8 | std::uint8_t(bytes[at + std::size_t(i)]);
    return value;
}

/// The coders that @p coder reads the images in its file with: ImageMagick
/// checks its policy for each of them too.
std::vector<std::string_view> innerCoders(std::string_view coder)
{
    if (coder == "ICO" || coder == "CUR")
        return {"PNG"};
    if (coder == "BMP")
        return {"PNG", "JPEG"};
    return {};
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
        if (::mkdtemp(pattern.data()))
            m_path = pattern;
    }
    ~MagickHome()
    {
        if (m_path.empty())
            return;
        // Only plain files: magick writes its pixel cache and temporary
        // copies here.
        if (DIR* dir = ::opendir(m_path.c_str())) {
            while (const dirent* entry = ::readdir(dir)) {
                const std::string_view name(entry->d_name);
                if (name != "." && name != "..")
                    ::unlinkat(::dirfd(dir), entry->d_name, 0);
            }
            ::closedir(dir);
        }
        ::rmdir(m_path.c_str());
    }
    MagickHome(const MagickHome&) = delete;
    MagickHome& operator=(const MagickHome&) = delete;
    const std::string& path() const
    {
        return m_path;
    }

private:
    std::string m_path;
};

} // namespace

std::string magickCoder(std::string_view bytes, std::string_view name)
{
    // ISO base media files (HEIF): the brands of their ftyp box.
    if (bytes.size() >= 16 && bytes.substr(4, 4) == "ftyp") {
        const std::size_t box = std::min<std::size_t>(
            std::max<std::uint32_t>(std::uint32_t(std::uint8_t(bytes[0])) << 24
                                        | std::uint32_t(std::uint8_t(bytes[1])) << 16
                                        | std::uint32_t(std::uint8_t(bytes[2])) << 8 | std::uint8_t(bytes[3]),
                                    16),
            std::min<std::size_t>(bytes.size(), 256));
        std::vector<std::string_view> brands{bytes.substr(8, 4)};
        for (std::size_t at = 16; at + 4 <= box; at += 4)
            brands.push_back(bytes.substr(at, 4));
        const auto any = [&](std::initializer_list<std::string_view> wanted) {
            return std::any_of(brands.begin(), brands.end(), [&](std::string_view b) {
                return std::find(wanted.begin(), wanted.end(), b) != wanted.end();
            });
        };
        if (any({"avif", "avis"}))
            return "AVIF";
        if (any({"heic", "heix", "hevc", "hevx", "heim", "heis", "mif1", "msf1"}))
            return "HEIC";
        return {};
    }
    if (bytes.starts_with("\xff\x0a") || bytes.starts_with(std::string_view("\0\0\0\x0cJXL \r\n\x87\n", 12)))
        return "JXL";
    if (bytes.starts_with("qoif"))
        return "QOI";
    // ICO and CUR: a reserved 0, the kind, and at least one image.
    if (bytes.size() >= 6 && bytes.starts_with(std::string_view("\0\0", 2)) && bytes[3] == 0
        && (bytes[2] == 1 || bytes[2] == 2) && little(bytes, 4, 2) > 0) {
        return bytes[2] == 1 ? "ICO" : "CUR";
    }
    // A BMP that aglow's reader refuses: JPEG or PNG data in a BMP, or a
    // run-length encoding at a depth that it does not have (RLE8 at 8 bits
    // and RLE4 at 4 bits read natively).
    if (bytes.size() >= 34 && bytes.starts_with("BM") && little(bytes, 14, 4) >= 40) {
        const std::uint32_t compression = little(bytes, 30, 4);
        const std::uint32_t bits = little(bytes, 28, 2);
        if (compression == 4 || compression == 5 || (compression == 1 && bits != 8) || (compression == 2 && bits != 4))
            return "BMP";
    }
    // SVG: XML whose first element is svg, in the first 4 KiB.
    std::string_view head = bytes.substr(0, 4096);
    if (head.starts_with("\xef\xbb\xbf"))
        head.remove_prefix(3);
    while (!head.empty()
           && (head.front() == ' ' || head.front() == '\t' || head.front() == '\r' || head.front() == '\n')) {
        head.remove_prefix(1);
    }
    if (head.starts_with("<svg")
        || ((head.starts_with("<?xml") || head.starts_with("<!--") || head.starts_with("<!DOCTYPE"))
            && head.find("<svg") != std::string_view::npos)) {
        return "RSVG";
    }
    // TGA has no signature: its footer in version 2, else its name.
    if ((bytes.size() >= 26 && bytes.substr(bytes.size() - 18) == std::string_view("TRUEVISION-XFILE.\0", 18))
        || lowerEndsWith(name, ".tga")) {
        // The image types of TGA: colour-mapped, true-colour and grey,
        // raw or run-length encoded.
        const std::uint8_t type = bytes.size() >= 18 ? std::uint8_t(bytes[2]) : 0;
        if (type == 1 || type == 2 || type == 3 || type == 9 || type == 10 || type == 11)
            return "TGA";
    }
    return {};
}

std::string_view magickFormat(std::string_view coder)
{
    if (coder == "RSVG")
        return "SVG";
    if (coder == "JXL")
        return "JPEG XL";
    if (coder == "BMP")
        return "compressed BMP";
    return coder;
}

bool magickInstalled()
{
    return !findProgram("magick").empty();
}

bool magickToPng(std::string_view bytes, const std::string& coder, std::string& png, std::string& error,
                 const std::function<bool()>& stop)
{
    const std::string program = findProgram("magick");
    if (program.empty()) {
        error =
            "aglow reads " + std::string(magickFormat(coder)) + " images with ImageMagick, and magick is not installed";
        return false;
    }
    MagickHome home;
    if (home.path().empty()) {
        error = std::string("cannot make a directory for ImageMagick: ") + std::strerror(errno);
        return false;
    }
    // The policy of this run: the one coder reads, with the coders that it
    // reads the images in it with (PNG in an icon, PNG or JPEG in a BMP),
    // PNG32 writes, and nothing else: no delegate program (Ghostscript,
    // Inkscape), no file list (@), and resource limits. The system's own
    // policy applies as well.
    std::string readers = "  <policy domain=\"coder\" rights=\"read\" pattern=\"" + coder + "\"/>\n";
    for (const std::string_view inner : innerCoders(coder)) {
        readers += "  <policy domain=\"coder\" rights=\"read\" pattern=\"" + std::string(inner) + "\"/>\n";
    }
    const std::string policy = "<policymap>\n"
        "  <policy domain=\"delegate\" rights=\"none\" pattern=\"*\"/>\n"
        "  <policy domain=\"coder\" rights=\"none\" pattern=\"*\"/>\n"
        + readers +
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
    if (fd >= 0)
        ::close(fd);
    if (!saved) {
        error = "cannot write the policy of ImageMagick";
        return false;
    }
    std::vector<std::string> environment =
        currentEnvironment({"MAGICK_CONFIGURE_PATH", "MAGICK_TEMPORARY_PATH", "MAGICK_TMPDIR"});
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
    // an SVG keeps its empty areas. -auto-orient turns the pixels upright:
    // an orientation outside EXIF (a JPEG XL's own) would reach aglow only
    // in a chunk of ImageMagick's that it does not read. The PNG then says
    // orientation 1, so that aglow does not turn it again.
    Result result = run(program, {"-background", "none", coder + ":-[0]", "-auto-orient", "PNG32:-"}, environment,
                        bytes, limits, stop);
    if (!result.started) {
        error = std::string("cannot run magick: ") + std::strerror(result.startError);
        return false;
    }
    if (result.interrupted) {
        error = "interrupted";
        return false;
    }
    if (result.waitError != 0) {
        error = std::string("cannot read what magick gives: ") + std::strerror(result.waitError);
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
        // A build of ImageMagick without the coder: its librsvg for SVG,
        // libheif for AVIF and HEIC, libjxl for JPEG XL.
        if (result.err.find("no decode delegate for this image format") != std::string::npos) {
            error = coder == "RSVG"
                ? "aglow reads SVG images with ImageMagick's librsvg coder (RSVG), which this ImageMagick does not have"
                : "this ImageMagick cannot read " + std::string(magickFormat(coder)) + " images";
            return false;
        }
        const std::string why = message(result.err);
        error = "ImageMagick cannot read this " + std::string(magickFormat(coder)) + " image"
            + (why.empty() ? "" : ": " + why);
        return false;
    }
    png = std::move(result.out);
    return true;
}

bool isRemoteUrl(std::string_view text)
{
    const auto scheme = [&](std::string_view prefix) {
        return text.size() > prefix.size()
            && std::equal(prefix.begin(), prefix.end(), text.begin(), [](char a, char b) {
                   return a == char(std::tolower(std::uint8_t(b)));
               });
    };
    return scheme("http://") || scheme("https://") || scheme("ftp://");
}

std::string urlFileName(std::string_view url)
{
    url = url.substr(0, url.find_first_of("?#"));
    const std::size_t scheme = url.find("://");
    if (scheme == std::string_view::npos)
        return {};
    url.remove_prefix(scheme + 3);
    const std::size_t path = url.find('/');
    if (path == std::string_view::npos)
        return {};
    url.remove_prefix(path);
    const std::string_view name = url.substr(url.rfind('/') + 1);
    // The server's file is the decoded name. A name with an escaped NUL
    // (%00) stays as the URL writes it.
    return AfterglowProto::dndPercentDecode(name).value_or(std::string(name));
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
    const std::vector<std::string> args{"-q",
                                        "--silent",
                                        "--show-error",
                                        "--fail",
                                        "--location",
                                        "--max-redirs",
                                        "10",
                                        "--proto",
                                        "=http,https,ftp",
                                        "--proto-redir",
                                        "=http,https,ftp",
                                        "--globoff",
                                        "--max-filesize",
                                        std::to_string(maxBytes),
                                        "--connect-timeout",
                                        "30",
                                        "--max-time",
                                        "300",
                                        "--",
                                        url};
    Limits limits;
    limits.cpuSeconds = 300;
    limits.wallSeconds = 310;
    limits.maxOutput = maxBytes;
    Result result = run(program, args, currentEnvironment({}), {}, limits, stop);
    if (!result.started) {
        error = std::string("cannot run curl: ") + std::strerror(result.startError);
        return false;
    }
    if (result.interrupted) {
        error = "interrupted";
        return false;
    }
    if (result.waitError != 0) {
        error = std::string("cannot read what curl gives: ") + std::strerror(result.waitError);
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
