// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "Ssh.h"
#include <AfterglowProto/Ipc.h>
#include <AfterglowProto/IpcServer.h>
#include <AfterglowProto/Remote.h>
#include <AfterglowProto/Text.h>
#include <AfterglowProto/Version.h>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <optional>
#include <string_view>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace Afterglow::Ssh {
namespace {
// OpenSSH's options that take a value, and the ones that change what a run is.
constexpr std::string_view FlagsWithValue = "BbcDEeFIiJLlmOoPpQRSWw";
constexpr std::string_view PassThroughFlags = "GVQO";
constexpr std::string_view NoSessionFlags = "NWfs";
constexpr qsizetype MaxField = 255;
constexpr qsizetype MaxCachedHosts = 512;

// OSC 3008 fields end at ';' and the sequence at ESC, so keep to name characters.
QString clean(const QString& value)
{
    QString out;
    for (const QChar c : value) {
        if (c.isLetterOrNumber() || QStringView(u".-_@:+%").contains(c)) out += c;
        if (out.size() == MaxField) break;
    }
    return out;
}

// Signals the helper ignores while ssh runs, restored in every child.
struct IgnoredSignals {
    struct sigaction interrupt {}, quit {}, brokenPipe {};
    IgnoredSignals()
    {
        struct sigaction ignore {};
        ignore.sa_handler = SIG_IGN;
        sigaction(SIGINT, &ignore, &interrupt);
        sigaction(SIGQUIT, &ignore, &quit);
        sigaction(SIGPIPE, &ignore, &brokenPipe);
    }
    ~IgnoredSignals() { restore(); }
    void restore() const
    {
        sigaction(SIGINT, &interrupt, nullptr);
        sigaction(SIGQUIT, &quit, nullptr);
        sigaction(SIGPIPE, &brokenPipe, nullptr);
    }
};

int exitCode(int status)
{
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 127;
}

// Ends the helper the way ssh ended. An interactive shell stops a loop only
// when the foreground job died from SIGINT, so an exit status of 130 is not
// the same. Signals that dump core are not passed on.
int finish(int status, const IgnoredSignals& signals)
{
    if (WIFSIGNALED(status)) {
        const int sig = WTERMSIG(status);
        if (sig == SIGINT || sig == SIGHUP || sig == SIGTERM || sig == SIGPIPE || sig == SIGKILL) {
            signals.restore();
            struct sigaction fallback {};
            fallback.sa_handler = SIG_DFL;
            sigaction(sig, &fallback, nullptr);
            sigset_t set;
            sigemptyset(&set);
            sigaddset(&set, sig);
            sigprocmask(SIG_UNBLOCK, &set, nullptr);
            kill(getpid(), sig);
        }
    }
    return exitCode(status);
}

[[noreturn]] void exec(const QByteArrayList& argv, const IgnoredSignals& signals, const QByteArray& term)
{
    signals.restore();
    if (!term.isEmpty() && qgetenv("TERM") != term) setenv("TERM", term.constData(), 1);
    QByteArrayList storage = argv;
    std::vector<char*> pointers;
    for (QByteArray& arg : storage) pointers.push_back(arg.data());
    pointers.push_back(nullptr);
    execvp(pointers.front(), pointers.data());
    const int code = errno;
    std::fprintf(stderr, "afterglow ssh: %s: %s\n", pointers.front(), std::strerror(code));
    _exit(127);
}

// ssh stopped (~^Z, or SIGTTIN in the background): stop with the same signal,
// so the shell reports the job as it would report plain ssh.
void stopLike(int sig)
{
    struct sigaction current {};
    sigaction(sig, nullptr, &current);
    kill(getpid(), current.sa_handler == SIG_DFL ? sig : SIGSTOP);
}

// ssh's wait status. The helper stops and continues with it.
int wait(pid_t pid)
{
    int status = 0;
    while (true) {
        if (waitpid(pid, &status, WUNTRACED) < 0) {
            if (errno == EINTR) continue;
            return 127 << 8; // as if it exited with 127
        }
        if (WIFSTOPPED(status)) {
            stopLike(WSTOPSIG(status));
            kill(pid, SIGCONT);
            continue;
        }
        return status;
    }
}

struct Captured {
    int status = 0; ///< as waitpid reports it
    QByteArray output;
};

// Runs ARGV with INPUT on its standard input, collecting its standard output.
// Standard error stays on the terminal, so ssh's prompts and errors show,
// unless QUIET discards it. Output past LIMIT bytes (-1: none) is read and
// dropped: a remote host decides how much it writes.
std::optional<Captured> capture(const QByteArrayList& argv, const IgnoredSignals& signals, const QByteArray& input = {},
                                bool quiet = false, qsizetype limit = -1)
{
    int out[2];
    int in[2];
    if (pipe(out) != 0) return std::nullopt;
    if (pipe(in) != 0) {
        close(out[0]); close(out[1]);
        return std::nullopt;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        for (int fd : {out[0], out[1], in[0], in[1]}) close(fd);
        return std::nullopt;
    }
    if (pid == 0) {
        dup2(in[0], STDIN_FILENO);
        dup2(out[1], STDOUT_FILENO);
        for (int fd : {out[0], out[1], in[0], in[1]}) close(fd);
        if (quiet) {
            if (const int null = open("/dev/null", O_WRONLY); null >= 0) {
                dup2(null, STDERR_FILENO);
                if (null != STDERR_FILENO) close(null);
            }
        }
        exec(argv, signals, {});
    }
    close(in[0]);
    close(out[1]);
    for (qsizetype written = 0; written < input.size();) {
        const ssize_t n = write(in[1], input.constData() + written, std::size_t(input.size() - written));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break; // the program exited without reading it all
        written += n;
    }
    close(in[1]);
    Captured result;
    char buffer[4096];
    while (true) {
        const ssize_t n = read(out[0], buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        if (limit < 0 || result.output.size() < limit)
            result.output.append(buffer, limit < 0 ? qsizetype(n) : std::min(qsizetype(n), limit - result.output.size()));
    }
    close(out[0]);
    result.status = wait(pid);
    return result;
}

// An XDG base directory. The specification treats empty and relative values as unset.
QString baseDirectory(const char* name)
{
    const QString value = qEnvironmentVariable(name);
    // Not QDir::isAbsolutePath: it takes ":dir" for a Qt resource.
    return value.startsWith(QLatin1Char('/')) ? value : QString();
}

QString stateFile(const QString& name = QStringLiteral("ssh-terminfo"))
{
    QString state = baseDirectory("XDG_STATE_HOME");
    if (state.isEmpty()) state = QDir::homePath() + QStringLiteral("/.local/state");
    return state + QStringLiteral("/afterglow/") + name;
}

// The hosts the helper has tried, oldest first: one hash for each, and
// " fallback" after the hash of a host that cannot take the entry.
QStringList readCache(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
}

// Puts LINE last, in place of the host's old line. The file is read again
// just before the write, so a run that finished meanwhile keeps its host.
void remember(const QString& path, const QString& hash, const QString& line)
{
    QStringList lines = readCache(path);
    lines.removeIf([&hash](const QString& l) { return l.section(QLatin1Char(' '), 0, 0) == hash; });
    lines.append(line);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile save(path);
    if (!save.open(QIODevice::WriteOnly)) return;
    save.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    save.write(lines.mid(std::max<qsizetype>(0, lines.size() - MaxCachedHosts)).join(QLatin1Char('\n')).toUtf8() + '\n');
    save.commit();
}

// Who has the entry: the account, the address, the route there and the
// entry's version. Only a hash is stored, so the file does not list hosts.
QString cacheKey(const Config& config, const QByteArray& entry)
{
    // infocmp starts with a comment that names the file it read: the same
    // entry from the build tree and from the installed database is one version.
    QByteArray body;
    for (const QByteArray& line : entry.split('\n')) {
        if (!line.startsWith('#')) body += line + '\n';
    }
    const QString host = config.user + QLatin1Char('@') + config.hostname + QLatin1Char(':') + config.port
        + QLatin1Char('\n') + config.proxyJump + QLatin1Char('\n') + config.proxyCommand + QLatin1Char('\n')
        + QString::fromLatin1(QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex());
    return QString::fromLatin1(QCryptographicHash::hash(host.toUtf8(), QCryptographicHash::Sha256).toHex());
}

// The helper's own ControlPath, without %C expanded: only in the private
// runtime directory, as in a shared one another user could create the
// socket first. Empty without one, and for a directory whose name ssh would
// read as more than a path (a space ends the value, % and ${ expand, a
// quote or a backslash quotes).
QByteArray ownControlPath()
{
    const QByteArray runtime = QFile::encodeName(baseDirectory("XDG_RUNTIME_DIR"));
    for (const char c : runtime) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (!plain && c != '/' && c != '.' && c != '_' && c != '-') return {};
    }
    return runtime.isEmpty() ? QByteArray() : runtime + "/afterglow-ssh-%C";
}

// Options that let the install and the session share one connection, so the
// user authenticates once.
QByteArrayList sharing(const Config& config)
{
    QByteArrayList shared;
    if (!config.controlPath) {
        const QByteArray path = ownControlPath();
        if (path.isEmpty()) return shared;
        shared << "-o" << "ControlMaster=auto" << "-o" << "ControlPath=" + path << "-o" << "ControlPersist=10";
    } else if (config.controlMaster && !config.controlPersist) {
        // The user's own master would close with the install's command.
        shared << "-o" << "ControlPersist=10";
    }
    return shared;
}

// An option word without -n: the install sends the entry on standard input.
// VALUE_FOLLOWS tells whether the next word is this option's value.
QByteArray withoutNullInput(const QByteArray& word, bool& valueFollows)
{
    valueFollows = false;
    QByteArray kept = "-";
    for (qsizetype c = 1; c < word.size(); ++c) {
        const char flag = word[c];
        if (FlagsWithValue.find(flag) != std::string_view::npos) {
            valueFollows = c == word.size() - 1;
            return kept + word.mid(c);
        }
        if (flag != 'n') kept += flag;
    }
    return kept.size() > 1 ? kept : QByteArray();
}

// The words of ARGS for the probe and the install, which open the connection
// that the session then shares: the options and their values, without -n and
// -M. ssh takes -M as it comes, past an -o ControlMaster before it: the run
// would try to be a master of its own, and sign in again.
QByteArrayList connectionOptions(const QByteArrayList& args, const Arguments& parsed)
{
    QByteArrayList out;
    bool isValue = false;
    for (const int i : parsed.options) {
        if (isValue) {
            out << args[i];
            isValue = false;
            continue;
        }
        const QByteArray word = withoutNullInput(args[i], isValue);
        QByteArray kept = "-";
        for (qsizetype c = 1; c < word.size(); ++c) {
            if (FlagsWithValue.find(word[c]) != std::string_view::npos) {
                kept += word.mid(c);
                break;
            }
            if (word[c] != 'M') kept += word[c];
        }
        if (kept.size() > 1) out << kept;
    }
    return out;
}

// Whether ARGS have the flag FLAG, which takes no value.
bool hasFlag(const QByteArrayList& args, const Arguments& parsed, char flag)
{
    bool isValue = false;
    for (const int i : parsed.options) {
        if (isValue) {
            isValue = false;
            continue;
        }
        const QByteArray& word = args[i];
        for (qsizetype c = 1; c < word.size(); ++c) {
            if (word[c] == flag) return true;
            if (FlagsWithValue.find(word[c]) != std::string_view::npos) {
                isValue = c == word.size() - 1;
                break;
            }
        }
    }
    return false;
}

// ssh's own failure status, distinct from the remote command's.
constexpr int SshFailed = 255;

enum class Install {
    Installed,
    Unavailable,      ///< no entry to send, or the host cannot take it
    ConnectionFailed, ///< ssh could not connect: the session would fail the same way
    Ended,            ///< ssh died from a signal, such as Ctrl+C at a password prompt
};

struct InstallResult {
    Install result = Install::Unavailable;
    int status = 0; ///< the install's wait status, for Install::Ended
};

// Puts afterglow's terminfo entry on the host once per entry version. When
// the install connects, OPTIONS gains the connection sharing the session reuses.
InstallResult installTerminfo(const QByteArrayList& args, const Arguments& parsed, const Config& config,
                              const IgnoredSignals& signals, QByteArrayList& options)
{
    const auto source = capture({"infocmp", "-0", "-x", "afterglow"}, signals);
    if (!source || source->status != 0 || source->output.isEmpty()) return {};
    const QString key = cacheKey(config, source->output);
    const QString path = stateFile();
    const QStringList cached = readCache(path);
    for (const QString& line : cached) {
        if (line.section(QLatin1Char(' '), 0, 0) != key) continue;
        // Hosts in use stay when the file is trimmed.
        if (line != cached.last()) remember(path, key, line);
        return {line == key ? Install::Installed : Install::Unavailable};
    }
    // ssh -M joins no master: the session cannot share the install's
    // connection, and a master that the install made would hold the
    // session's ControlPath, which -M wants for itself. Then the install is
    // no master, and the session gets nothing to share.
    const bool dashM = hasFlag(args, parsed, 'M');
    const QByteArrayList shared = dashM ? QByteArrayList() : sharing(config);
    // The connection options and the destination, without a tty, the remote command, -n or -M.
    QByteArrayList install{"ssh"};
    install << shared;
    // A command-line value wins over the config, and the config's RemoteCommand
    // would refuse the install's command.
    if (config.remoteCommand) install << "-o" << "RemoteCommand=none";
    // The config's StdinNull would send tic an empty entry. Only an ssh that
    // printed the key knows it, so older ones never see the option.
    if (config.stdinNull) install << "-o" << "StdinNull=no";
    if (dashM) install << "-o" << "ControlMaster=no";
    install << connectionOptions(args, parsed);
    install << "-T" << "--" << args[parsed.destination]
            << "sh -c 'command -v tic >/dev/null 2>&1 && mkdir -p \"$HOME/.terminfo\""
               " && tic -x -o \"$HOME/.terminfo\" - >/dev/null 2>&1"
               " && { [ -e \"$HOME/.terminfo/a/afterglow\" ] || [ -e \"$HOME/.terminfo/61/afterglow\" ]; }'";
    const auto installed = capture(install, signals, source->output);
    if (!installed) return {};
    if (WIFSIGNALED(installed->status)) return {Install::Ended, installed->status};
    const int status = exitCode(installed->status);
    if (status == SshFailed) return {Install::ConnectionFailed};
    // It connected: the session reuses the connection either way, and a host
    // without tic is not asked again.
    options << shared;
    remember(path, key, status == 0 ? key : key + QStringLiteral(" fallback"));
    return {status == 0 ? Install::Installed : Install::Unavailable};
}

// Whether ARGS ask for ssh's debug output (-v): the helper then says why a
// session gets no remote commands.
bool verbose(const QByteArrayList& args, const Arguments& parsed)
{
    return hasFlag(args, parsed, 'v');
}

// What the user's configuration and command line say about connection
// sharing, which `ssh -G` alone cannot tell from the defaults: it prints
// nothing for ControlPath none and "false" for ControlMaster no, as for
// options that nothing sets. ssh takes the first value of an option, the
// command line's before the files'. Thus ssh -G reads the user's files again
// through a file of the helper's that ends with a Host * block of its own
// values: a value of the helper's shows only where nothing else set one.
struct UserSharing {
    QByteArray controlPath;  ///< as ssh expands it; empty for ControlPath none
    bool own = false;        ///< the path is the helper's: nothing set ControlPath
    bool masterNo = false;   ///< ControlMaster no, on the command line or in a file
};

// The marker of a ControlMaster that nothing set. A user's autoask reads as
// unset too, which changes nothing: the helper's own connection replaces it
// only where no ControlPath is set, and there ControlMaster does nothing.
constexpr QByteArrayView UnsetMaster = "autoask";

// The ssh -G for UserSharing: ARGS without -F, and the file of -F (or the
// user's and the system's files) read through an Include. Empty when a word
// cannot go into that file.
std::optional<std::pair<QByteArrayList, QByteArray>> sharingQuery(const QByteArrayList& args, const Arguments& parsed,
                                                                  const QByteArray& ownPath)
{
    QByteArrayList words{"ssh", "-G", "-F", "/dev/stdin"};
    std::optional<QByteArray> file;
    bool isValue = false;
    bool fileValue = false;
    for (int i = 0; i < args.size(); ++i) {
        const bool option = parsed.options.contains(i);
        if (option && isValue) {
            isValue = false;
            if (fileValue) {
                file = args[i];
                fileValue = false;
                continue;
            }
            words << args[i];
            continue;
        }
        if (!option) {
            words << args[i];
            continue;
        }
        // An option word: -F and its value go, the other flags stay.
        const QByteArray& word = args[i];
        QByteArray kept = "-";
        for (qsizetype c = 1; c < word.size(); ++c) {
            const char flag = word[c];
            if (FlagsWithValue.find(flag) == std::string_view::npos) {
                kept += flag;
                continue;
            }
            if (flag == 'F') {
                if (c + 1 < word.size()) file = word.mid(c + 1);
                else fileValue = isValue = true;
            } else {
                kept += word.mid(c);
                isValue = c == word.size() - 1;
            }
            break;
        }
        if (kept.size() > 1) words << kept;
    }
    // A double quote ends a word of ssh's configuration, and a line ends at a newline.
    const auto quotable = [](const QByteArray& path) { return !path.contains('"') && !path.contains('\n') && !path.contains('\r'); };
    QByteArray config;
    if (!file) {
        config = "Include ~/.ssh/config\nInclude /etc/ssh/ssh_config\n";
    } else if (*file != "none") {
        const QByteArray absolute = file->startsWith('/') ? *file : QFile::encodeName(QDir::currentPath()) + '/' + *file;
        if (!quotable(absolute)) return std::nullopt;
        config = "Include \"" + absolute + "\"\n";
    }
    config += "Host *\n  ControlPath " + (ownPath.isEmpty() ? QByteArray("none") : ownPath) + "\n  ControlMaster "
        + UnsetMaster.toByteArray() + "\n";
    return std::pair{words, config};
}

// How the remote step ends for the helper.
struct RemoteResult {
    std::optional<int> exit; ///< the helper ends with this status: the probe could not connect, or a run died
};

// Says once for each host and version that the remote aglow speaks another
// protocol, on standard error.
void reportVersion(const Config& config, const Probe& probe)
{
    const QString host = config.user + QLatin1Char('@') + config.hostname + QLatin1Char(':') + config.port;
    const QString key = QString::fromLatin1(
        QCryptographicHash::hash((host + QLatin1Char('\n') + probe.version).toUtf8(), QCryptographicHash::Sha256).toHex());
    const QString path = stateFile(QStringLiteral("ssh-remote"));
    const QStringList cached = readCache(path);
    if (cached.contains(key)) {
        if (key != cached.last()) remember(path, key, key);
        return;
    }
    remember(path, key, key);
    const QByteArray name = clean(config.host.isEmpty() ? config.hostname : config.host).toLocal8Bit();
    if (probe.protocol < 0) {
        // An aglow that names no protocol: its version line, cut to name characters.
        std::fprintf(stderr, "afterglow ssh: aglow on %s names no protocol (%s), and this Afterglow speaks protocol %d. "
                             "aglow run there cannot reach this terminal until the two match.\n",
                     name.constData(), clean(probe.version).toLocal8Bit().constData(), AfterglowProto::ProtocolVersion);
        return;
    }
    std::fprintf(stderr, "afterglow ssh: aglow on %s speaks protocol %d, and this Afterglow speaks protocol %d. "
                         "aglow run there cannot reach this terminal until the two match.\n",
                 name.constData(), probe.protocol, AfterglowProto::ProtocolVersion);
}

// Probes the host and forwards a socket of the terminal there, when the
// terminal allows remote commands. Unless SHARED (the install connected and
// added it), OPTIONS gains the connection sharing, which the session reuses,
// once the probe ran there. A session that gets no remote commands goes on as
// it would without them: the pane's answer to the query is "off", and with -v
// the helper says why on standard error.
RemoteResult forwardRemote(const QByteArrayList& args, const Arguments& parsed, const Config& config, const IgnoredSignals& signals,
                           QByteArrayList& options, bool shared)
{
    namespace Json = AfterglowProto::Json;
    const bool debug = verbose(args, parsed);
    const auto skip = [debug](const char* why) {
        if (debug) std::fprintf(stderr, "afterglow ssh: no remote commands in this session: %s\n", why);
        return RemoteResult{};
    };
    // A run that died from a signal, such as Ctrl+C at a prompt: the helper
    // ends the same way, and the session does not start.
    const auto died = [&signals](const Captured& run) -> std::optional<int> {
        if (WIFSIGNALED(run.status)) return finish(run.status, signals);
        return std::nullopt;
    };

    const QByteArray socket = qgetenv("AFTERGLOW_SOCKET");
    const QByteArray paneText = qgetenv("AFTERGLOW_PANE_ID");
    // Digits only: toUInt would take spaces and a sign too.
    const bool digits = !paneText.isEmpty() && paneText.size() <= 10
        && std::all_of(paneText.cbegin(), paneText.cend(), [](char c) { return c >= '0' && c <= '9'; });
    bool paneOk = false;
    const uint pane = digits ? paneText.toUInt(&paneOk) : 0;
    if (socket.isEmpty() || !paneOk || pane == 0) return {};
    // The variables of a tmux server are those of the pane that started it,
    // whichever pane shows this tmux client now.
    if (qEnvironmentVariableIsSet("TMUX")) return skip("in tmux, AFTERGLOW_PANE_ID can name another pane");
    // aglow run asks the pane through the terminal on the host, which ssh
    // makes as ssh.c decides: always for -tt (force), else only when its
    // standard input is a terminal, for -t (true), or without a remote
    // command (auto).
    const bool automatic = config.requestTty.isEmpty() || config.requestTty == QLatin1String("auto");
    const bool remoteTty = config.requestTty == QLatin1String("force")
        || (isatty(STDIN_FILENO)
            && (config.requestTty == QLatin1String("true") || (automatic && parsed.command < 0 && !config.remoteCommand)));
    if (!remoteTty) return skip("the session has no terminal on the host");

    // One wait for the connection and the call together: a busy Afterglow
    // holds the session back no longer than that.
    constexpr int CallTimeoutMs = 2000;
    QElapsedTimer clock;
    clock.start();
    AfterglowProto::IpcConnection ipc;
    AfterglowProto::IpcConnection::Error error;
    if (!ipc.connect(socket.toStdString(), &error, CallTimeoutMs)) return skip("Afterglow does not answer");
    const int left = int(std::max<qint64>(1, CallTimeoutMs - clock.elapsed()));
    const auto enabled = ipc.call("control", "remoteCommandsEnabled", {}, left, &error);
    // An error that the socket's server answered says why: a session pane
    // of the mux daemon takes no control call while no window shows it. The
    // router of Afterglow has no control target while Control.enabled is
    // off, and its message for that names only the target.
    if (!enabled && error.code == AfterglowProto::IpcErrorCode::NoSuchTarget) {
        return skip("Afterglow has no control target: Control.enabled is off");
    }
    if (!enabled && !error.code.empty() && !error.message.empty()) {
        return skip(AfterglowProto::printable(error.message).c_str());
    }
    if (!enabled) return skip("Afterglow does not answer");
    if (!enabled->toBool()) return skip("remote commands are off in Afterglow");

    // A shared connection: the helper's own, else the user's ControlPath.
    const QByteArray ownPath = ownControlPath();
    if (!config.controlPath && ownPath.isEmpty()) return skip("no private XDG_RUNTIME_DIR for a shared connection");
    const auto query = sharingQuery(args, parsed, ownPath);
    if (!query) return skip("the -F file has a name that ssh's Include cannot take");
    const auto resolved = capture(query->first, signals, query->second, true);
    if (!resolved) return skip("ssh -G did not run");
    if (const auto exit = died(*resolved)) return {exit};
    if (resolved->status != 0) return skip("ssh -G failed");
    UserSharing user;
    for (const QByteArray& line : resolved->output.split('\n')) {
        if (line.startsWith("controlpath ")) user.controlPath = line.mid(12);
        else if (line.startsWith("controlmaster ")) user.masterNo = line.mid(14) == "false";
    }
    user.own = !config.controlPath;
    // The user's choice stays: no connection of the helper's in its place.
    if (user.controlPath.isEmpty()) return skip("ControlPath is none");
    if (user.masterNo) return skip("ControlMaster is no");
    // -M makes ControlMaster yes (ask for -MM) past any -o, and ssh then
    // joins no master and finds the helper's socket taken. The master that
    // the probe makes would hold the forward with no session, and end after
    // its ControlPersist. A ControlMaster of an -o or a file gives way to the
    // helper's -o ControlMaster=auto, which comes first.
    if (user.own && hasFlag(args, parsed, 'M')) return skip("-M needs a ControlPath of your own");
    // The control commands run with that path alone and no configuration:
    // -O forward and -O cancel would also add and cancel the forwards of the
    // command line and of the files. ssh expands % and ${} in -S too.
    if (!user.controlPath.startsWith('/') || user.controlPath.contains("${")
        || !AfterglowProto::isCleanText(std::string_view(user.controlPath.constData(), std::size_t(user.controlPath.size()))))
        return skip("the ControlPath is not an absolute path of UTF-8 text");
    QByteArray escaped = user.controlPath;
    escaped.replace('%', "%%");
    const QByteArrayList control{"ssh", "-F", "none", "-S", escaped};
    const QByteArray destination = args[parsed.destination];
    if (!AfterglowProto::isCleanText(std::string_view(destination.constData(), std::size_t(destination.size()))))
        return skip("the destination is not UTF-8 text");

    const QByteArrayList shares = sharing(config);
    QByteArrayList probe{"ssh"};
    probe << shares;
    if (!user.own && !config.controlMaster) {
        // The user's ControlPath, whose ControlMaster makes no master on its
        // own: only a master that runs already saves the second sign-in.
        // ssh -G shows a -M as ControlMaster yes, so a -M comes here too.
        const auto check = capture(control + QByteArrayList{"-O", "check", "--", destination}, signals, {}, true);
        if (!check) return skip("ssh -O check did not run");
        if (const auto exit = died(*check)) return {exit};
        if (check->status != 0) {
            return skip(hasFlag(args, parsed, 'M') ? "no master runs on the ControlPath, which -M needs"
                                                   : "no master runs on the ControlPath, and ControlMaster is not auto");
        }
        // As a client of that master, whatever ControlMaster says.
        probe << "-o" << "ControlMaster=no";
    }
    // Before the user's options: ssh takes the first value.
    if (config.remoteCommand) probe << "-o" << "RemoteCommand=none";
    probe << connectionOptions(args, parsed) << "-T" << "--" << destination << probeScript();
    // The report is a few short lines: a host that writes more gets no more room.
    constexpr qsizetype ProbeOutputLimit = 64 * 1024;
    const auto probed = capture(probe, signals, {}, false, ProbeOutputLimit);
    if (!probed) return skip("the probe did not run");
    if (const auto exit = died(*probed)) return {exit};
    const Probe found = parseProbe(probed->output);
    // ssh's own failure, unless the script ran and a command there gave 255.
    if (exitCode(probed->status) == SshFailed && !found.ran) return {SshFailed};
    if (!shared && found.ran) options << shares;
    if (!found.aglow) return skip("no aglow on the PATH of the remote login shell");
    if (found.directory.isEmpty()) return skip("the remote directory could not be made");

    // The master must be there for -O forward, and for the session to share it.
    const auto check = capture(control + QByteArrayList{"-O", "check", "--", destination}, signals, {}, true);
    if (!check) return skip("ssh -O check did not run");
    if (const auto exit = died(*check)) return {exit};
    if (check->status != 0) return skip("no master runs on the ControlPath");

    const QString host = config.host.isEmpty() ? config.hostname : config.host;
    Json::Array words;
    for (const QByteArray& word : control.mid(1)) words.emplace_back(word.toStdString());
    const auto opened = ipc.call("control", "openRemoteSocket",
                                 {std::int64_t(pane), std::int64_t(getpid()), host.toStdString(), found.directory.toStdString(),
                                  std::int64_t(found.protocol), std::move(words), destination.toStdString(),
                                  qgetenv("AFTERGLOW_OVERLAY") == "1"},
                                 CallTimeoutMs, &error);
    if (!opened) return skip("Afterglow refused the socket");
    const std::string token = (*opened)["token"].toString();
    if ((*opened)["error"].toString() == "version") {
        reportVersion(config, found);
        return {};
    }
    if ((*opened)["error"].toString() == "length") return skip("the remote socket path would be over 103 bytes");
    const std::string local = (*opened)["socket"].toString();
    const std::string remote = (*opened)["path"].toString();
    // Only the pair that this session asked for goes into ssh's -R, which
    // splits it at ':'.
    const std::string name = token + ".sock";
    const bool localOk = local.starts_with('/') && local.ends_with("/afterglow-remote-" + name)
        && local.find_first_of(":\\") == std::string::npos && AfterglowProto::isCleanText(local);
    const bool remoteOk = !token.empty() && AfterglowProto::Remote::validPath(remote)
        && remote == AfterglowProto::Remote::socketPath(found.directory.toStdString(), token);
    if (!localOk || !remoteOk) return skip("Afterglow answered with another socket");
    const QByteArray spec = QByteArray::fromStdString(remote) + ':' + QByteArray::fromStdString(local);
    const auto forward = capture(control + QByteArrayList{"-O", "forward", "-R", spec, "--", destination}, signals, {}, true);
    if (forward && WIFSIGNALED(forward->status)) {
        // The forward can be there: the socket closes with this process, and Afterglow cancels it.
        return {finish(forward->status, signals)};
    }
    if (!forward || forward->status != 0) {
        ipc.call("control", "remoteSocketFailed", {token, "forward"}, CallTimeoutMs, &error);
        return skip("ssh -O forward failed: the ssh server can refuse it (AllowStreamLocalForwarding)");
    }
    // The session can have ended meanwhile, which cancelled nothing: the
    // forward came after. Then it goes now, or it would last as long as the master.
    if (!ipc.call("control", "remoteSocketForwarded", {token}, CallTimeoutMs, &error)) {
        capture(control + QByteArrayList{"-O", "cancel", "-R", spec, "--", destination}, signals, {}, true);
        // A busy Afterglow can still take the call late, and answer with a
        // path that no forward serves now. It takes this after it, in order.
        const bool late = error.timedOut;
        if (late) ipc.send("control", "remoteSocketFailed", {token, "forward"}, &error);
        return skip(late ? "Afterglow did not answer, and the forward is cancelled" : "the socket closed before its forward came");
    }
    return {};
}
} // namespace

Arguments parseArguments(const QStringList& args)
{
    Arguments result;
    // ssh parses options again after the destination, unless "--" came first.
    for (int i = 0; i < args.size(); ++i) {
        const QString& arg = args[i];
        if (arg == QLatin1String("--")) {
            if (result.destination < 0) {
                if (i + 1 < args.size()) result.destination = i + 1;
                if (i + 2 < args.size()) result.command = i + 2;
            } else if (i + 1 < args.size()) {
                result.command = i + 1;
            }
            break;
        }
        if (arg.size() < 2 || arg[0] != QLatin1Char('-')) {
            if (result.destination >= 0) {
                result.command = i;
                break;
            }
            result.destination = i;
            continue;
        }
        result.options << i;
        for (qsizetype c = 1; c < arg.size(); ++c) {
            const char flag = arg[c].toLatin1();
            if (PassThroughFlags.find(flag) != std::string_view::npos) result.passThrough = true;
            if (NoSessionFlags.find(flag) != std::string_view::npos) result.noSession = true;
            if (FlagsWithValue.find(flag) != std::string_view::npos) {
                if (c == arg.size() - 1 && i + 1 < args.size()) result.options << ++i; // the value is the next argument
                break;
            }
        }
    }
    return result;
}

Config parseConfig(const QByteArray& resolved)
{
    Config config;
    for (const QByteArray& line : resolved.split('\n')) {
        const qsizetype space = line.indexOf(' ');
        if (space <= 0) continue;
        const QByteArray key = line.left(space);
        const QString value = QString::fromUtf8(line.mid(space + 1)).trimmed();
        if (key == "host") config.host = value;
        else if (key == "hostname") config.hostname = value;
        else if (key == "user") config.user = value;
        else if (key == "port") config.port = value;
        else if (key == "proxyjump") config.proxyJump = value;
        else if (key == "proxycommand") config.proxyCommand = value;
        else if (key == "setenv") config.setEnv = true;
        else if (key == "controlpath") config.controlPath = value != QLatin1String("none");
        else if (key == "controlmaster") config.controlMaster = value == QLatin1String("auto") || value == QLatin1String("autoask");
        else if (key == "controlpersist") config.controlPersist = value != QLatin1String("no");
        else if (key == "remotecommand") config.remoteCommand = value != QLatin1String("none");
        else if (key == "sessiontype") config.noSession = config.noSession || value != QLatin1String("default");
        else if (key == "forkafterauthentication") config.noSession = config.noSession || value == QLatin1String("yes");
        else if (key == "stdinnull") config.stdinNull = value == QLatin1String("yes");
        else if (key == "requesttty") config.requestTty = value;
    }
    return config;
}

QByteArray contextStart(const QString& id, const Config& config)
{
    QByteArray out = "\x1b]3008;start=" + clean(id).toUtf8() + ";type=remote";
    if (const QString user = clean(config.user); !user.isEmpty()) out += ";targetuser=" + user.toUtf8();
    out += ";targethost=" + clean(config.host.isEmpty() ? config.hostname : config.host).toUtf8();
    return out + "\x1b\\";
}

QByteArray contextEnd(const QString& id)
{
    return "\x1b]3008;end=" + clean(id).toUtf8() + "\x1b\\";
}

QByteArray probeScript()
{
    // One line for sh -c: the remote login shell (sh, bash, zsh, fish) reads
    // it first, so it holds no single quote and no backslash. The first line
    // tells that the script ran, whatever the status of the commands after
    // it. The directory is made only where aglow is, and the prune clears
    // the sockets of sessions that ended, which sshd leaves. Without a
    // runtime directory, the home can be on NFS and shared by many hosts: a
    // socket there is live only on its own host, so each host has its own
    // directory.
    return "sh -c '"
           "r=; case \"$XDG_RUNTIME_DIR\" in /*) r=$XDG_RUNTIME_DIR;; esac; "
           "echo \"runtime=$r\"; "
           "if v=$(aglow --version 2>/dev/null); then echo \"aglow=$v\"; "
           "d=; if [ -n \"$r\" ]; then d=$r/afterglow; "
           "else case \"$HOME\" in /*) d=$HOME/.local/state/afterglow/run/$(uname -n);; esac; fi; "
           "if [ -n \"$d\" ] && (umask 077 && mkdir -p \"$d\") && chmod 700 \"$d\"; then echo \"dir=$d\"; "
           "aglow remote-prune \"$d\" >/dev/null 2>&1; fi; fi; exit 0'";
}

Probe parseProbe(const QByteArray& output)
{
    Probe probe;
    for (const QByteArray& line : output.split('\n')) {
        const qsizetype equals = line.indexOf('=');
        if (equals <= 0) continue;
        const QByteArray key = line.left(equals);
        const QByteArray bytes = line.mid(equals + 1);
        const QString value = QString::fromUtf8(bytes);
        if (key == "runtime") probe.ran = true;
        else if (key == "dir") {
            // The path goes into the pane's reply and into ssh's -R: text only, and absolute.
            const bool text = AfterglowProto::isCleanText(std::string_view(bytes.constData(), std::size_t(bytes.size())));
            probe.directory = text && bytes.startsWith('/') ? value : QString();
        }
        else if (key == "aglow") {
            probe.aglow = true;
            probe.version = value;
            static const QRegularExpression protocol(QStringLiteral("\\(protocol (\\d{1,9})\\)"));
            if (const auto match = protocol.match(value); match.hasMatch()) probe.protocol = match.captured(1).toInt();
        }
    }
    return probe;
}

int run(const QByteArrayList& args)
{
    // A pane names its foreground job by the process name when it asks before
    // closing: the shell's ssh function runs this helper in ssh's place.
    prctl(PR_SET_NAME, "ssh", 0, 0, 0);
    const IgnoredSignals signals;
    QStringList words;
    for (const QByteArray& arg : args) words << QString::fromLocal8Bit(arg);
    const Arguments parsed = parseArguments(words);
    QByteArrayList options;
    QByteArray term = qgetenv("TERM");
    std::optional<Config> config;
    if (!parsed.passThrough && parsed.destination >= 0) {
        // ssh resolves the destination itself. On failure the session run
        // below reports the error, so this one stays quiet.
        const auto resolved = capture(QByteArrayList{"ssh", "-G"} + args, signals, {}, true);
        if (resolved && resolved->status == 0) config = parseConfig(resolved->output);
    }
    // The config can turn off the session as -N, -s or -f do.
    const bool noSession = parsed.noSession || (config && config->noSession);
    if (config) {
        const qsizetype before = options.size();
        if (term == "afterglow" && !noSession) {
            const InstallResult install = qgetenv("AFTERGLOW_SSH_TERMINFO") == "fallback"
                ? InstallResult{} : installTerminfo(args, parsed, *config, signals, options);
            if (install.result == Install::Ended) return finish(install.status, signals);
            if (install.result == Install::ConnectionFailed) return SshFailed;
            if (install.result == Install::Unavailable) term = "xterm-256color";
        }
        // The install connected: the session shares its connection.
        const bool shared = options.size() > before;
        if (!noSession) {
            if (const RemoteResult remote = forwardRemote(args, parsed, *config, signals, options, shared); remote.exit)
                return *remote.exit;
        }
        // Servers accept these only when their AcceptEnv allows it.
        QByteArrayList variables;
        for (const char* name : {"COLORTERM", "TERM_PROGRAM", "TERM_PROGRAM_VERSION"}) {
            if (const QByteArray value = qgetenv(name); !value.isEmpty() && !value.contains(' '))
                variables << QByteArray(name) + '=' + value;
        }
        if (!config->setEnv && !variables.isEmpty()) options << "-o" << "SetEnv=" + variables.join(' ');
    }
    const bool label = config && !noSession && isatty(STDOUT_FILENO);
    const QString id = QStringLiteral("afterglow-ssh-") + QString::number(getpid());
    const auto send = [](const QByteArray& sequence) {
        for (qsizetype written = 0; written < sequence.size();) {
            const ssize_t n = write(STDOUT_FILENO, sequence.constData() + written, std::size_t(sequence.size() - written));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return;
            written += n;
        }
    };
    if (label) send(contextStart(id, *config));
    int status = 127 << 8;
    if (const pid_t pid = fork(); pid == 0) {
        exec(QByteArrayList{"ssh"} + options + args, signals, term);
    } else if (pid > 0) {
        status = wait(pid);
    }
    if (label) send(contextEnd(id));
    return finish(status, signals);
}

} // namespace Afterglow::Ssh
