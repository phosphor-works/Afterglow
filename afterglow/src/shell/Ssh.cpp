// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "Ssh.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
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
constexpr std::string_view NoSessionFlags = "NWf";
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
// unless QUIET discards it.
std::optional<Captured> capture(const QByteArrayList& argv, const IgnoredSignals& signals, const QByteArray& input = {},
                                bool quiet = false)
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
        result.output.append(buffer, n);
    }
    close(out[0]);
    result.status = wait(pid);
    return result;
}

// An XDG base directory. The specification treats empty and relative values as unset.
QString baseDirectory(const char* name)
{
    const QString value = qEnvironmentVariable(name);
    return QDir::isAbsolutePath(value) ? value : QString();
}

QString stateFile()
{
    QString state = baseDirectory("XDG_STATE_HOME");
    if (state.isEmpty()) state = QDir::homePath() + QStringLiteral("/.local/state");
    return state + QStringLiteral("/afterglow/ssh-terminfo");
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

// Options that let the install and the session share one connection, so the
// user authenticates once.
QByteArrayList sharing(const Config& config)
{
    QByteArrayList shared;
    if (!config.controlPath) {
        // Only in the private runtime directory: in a shared one, another
        // user could create the socket first.
        const QString runtime = baseDirectory("XDG_RUNTIME_DIR");
        if (runtime.isEmpty()) return shared;
        shared << "-o" << "ControlMaster=auto" << "-o"
               << "ControlPath=" + QFile::encodeName(runtime) + "/afterglow-ssh-%C" << "-o" << "ControlPersist=10";
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
    const QByteArrayList shared = sharing(config);
    // The connection options and the destination, without a tty, the remote command or -n.
    QByteArrayList install{"ssh"};
    install << shared;
    // A command-line value wins over the config, and the config's RemoteCommand
    // would refuse the install's command.
    if (config.remoteCommand) install << "-o" << "RemoteCommand=none";
    // The config's StdinNull would send tic an empty entry. Only an ssh that
    // printed the key knows it, so older ones never see the option.
    if (config.stdinNull) install << "-o" << "StdinNull=no";
    bool isValue = false;
    for (const int i : parsed.options) {
        if (isValue) {
            install << args[i];
            isValue = false;
        } else if (const QByteArray word = withoutNullInput(args[i], isValue); !word.isEmpty()) {
            install << word;
        }
    }
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
        if (term == "afterglow" && !noSession) {
            const InstallResult install = qgetenv("AFTERGLOW_SSH_TERMINFO") == "fallback"
                ? InstallResult{} : installTerminfo(args, parsed, *config, signals, options);
            if (install.result == Install::Ended) return finish(install.status, signals);
            if (install.result == Install::ConnectionFailed) return SshFailed;
            if (install.result == Install::Unavailable) term = "xterm-256color";
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
