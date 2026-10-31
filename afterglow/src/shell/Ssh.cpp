// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "Ssh.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSaveFile>
#include <cerrno>
#include <csignal>
#include <optional>
#include <string_view>
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

[[noreturn]] void exec(const QStringList& argv, const IgnoredSignals& signals, const QByteArray& term)
{
    signals.restore();
    if (!term.isEmpty() && qgetenv("TERM") != term) setenv("TERM", term.constData(), 1);
    std::vector<QByteArray> storage;
    for (const QString& arg : argv) storage.push_back(arg.toLocal8Bit());
    std::vector<char*> pointers;
    for (QByteArray& arg : storage) pointers.push_back(arg.data());
    pointers.push_back(nullptr);
    execvp(pointers.front(), pointers.data());
    _exit(127);
}

int wait(pid_t pid)
{
    int status = 0;
    while (true) {
        if (waitpid(pid, &status, WUNTRACED) < 0) {
            if (errno == EINTR) continue;
            return 127;
        }
        if (WIFSTOPPED(status)) {
            // ssh suspended itself (~^Z): suspend with it so the shell sees the job stop.
            kill(getpid(), SIGSTOP);
            kill(pid, SIGCONT);
            continue;
        }
        return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
}

struct Captured {
    int status = 0;
    QByteArray output;
};

// Runs ARGV with INPUT on its standard input, collecting its standard output.
// Standard error stays on the terminal, so ssh's prompts and errors show.
std::optional<Captured> capture(const QStringList& argv, const IgnoredSignals& signals, const QByteArray& input = {})
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

QString stateFile()
{
    const QString state = qEnvironmentVariable("XDG_STATE_HOME", QDir::homePath() + QStringLiteral("/.local/state"));
    return state + QStringLiteral("/afterglow/ssh-terminfo");
}

// ssh's own failure status, distinct from the remote command's.
constexpr int SshFailed = 255;

enum class Install { Installed, Unavailable, ConnectionFailed };

// Puts afterglow's terminfo entry on the host once per entry version. On
// success OPTIONS gains the connection sharing the session should reuse.
Install installTerminfo(const QStringList& args, const Arguments& parsed, const Config& config,
                     const IgnoredSignals& signals, QStringList& options)
{
    const auto source = capture({QStringLiteral("infocmp"), QStringLiteral("-0"), QStringLiteral("-x"), QStringLiteral("afterglow")}, signals);
    if (!source || source->status != 0 || source->output.isEmpty()) return Install::Unavailable;
    const QString key = config.user + QLatin1Char('@') + config.hostname + QLatin1Char(':') + config.port + QLatin1Char(' ')
        + QString::fromLatin1(QCryptographicHash::hash(source->output, QCryptographicHash::Sha256).toHex().left(16));
    QFile cache(stateFile());
    QStringList hosts;
    if (cache.open(QIODevice::ReadOnly)) {
        hosts = QString::fromUtf8(cache.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        cache.close();
    }
    if (hosts.contains(key)) return Install::Installed;
    // One connection for the install and the session: the user authenticates once.
    QStringList shared;
    if (!config.controlPath) {
        const QString runtime = qEnvironmentVariable("XDG_RUNTIME_DIR", QDir::tempPath());
        shared << QStringLiteral("-o") << QStringLiteral("ControlMaster=auto")
               << QStringLiteral("-o") << QStringLiteral("ControlPath=") + runtime + QStringLiteral("/afterglow-ssh-%C")
               << QStringLiteral("-o") << QStringLiteral("ControlPersist=10");
    }
    // The connection options and the destination, without a tty or the remote command.
    QStringList install{QStringLiteral("ssh")};
    install << shared;
    const int beforeDestination = parsed.destination > 0 && args[parsed.destination - 1] == QLatin1String("--")
        ? parsed.destination - 1 : parsed.destination;
    install << args.mid(0, beforeDestination) << QStringLiteral("-T") << args.mid(beforeDestination, parsed.destination + 1 - beforeDestination)
            << QStringLiteral("sh -c 'command -v tic >/dev/null 2>&1 && mkdir -p \"$HOME/.terminfo\""
                              " && tic -x -o \"$HOME/.terminfo\" - >/dev/null 2>&1'");
    const auto installed = capture(install, signals, source->output);
    // An unreachable host would fail the session the same way: report it once.
    if (installed && installed->status == SshFailed) return Install::ConnectionFailed;
    if (!installed || installed->status != 0) return Install::Unavailable;
    options << shared;
    hosts.append(key);
    QDir().mkpath(QFileInfo(cache).absolutePath());
    QSaveFile save(cache.fileName());
    if (save.open(QIODevice::WriteOnly)) {
        save.write(hosts.mid(std::max<qsizetype>(0, hosts.size() - MaxCachedHosts)).join(QLatin1Char('\n')).toUtf8() + '\n');
        save.commit();
    }
    return Install::Installed;
}
} // namespace

Arguments parseArguments(const QStringList& args)
{
    Arguments result;
    for (int i = 0; i < args.size(); ++i) {
        const QString& arg = args[i];
        if (arg == QLatin1String("--")) {
            if (i + 1 < args.size()) result.destination = i + 1;
            break;
        }
        if (arg.size() < 2 || arg[0] != QLatin1Char('-')) {
            result.destination = i;
            break;
        }
        for (qsizetype c = 1; c < arg.size(); ++c) {
            const char flag = arg[c].toLatin1();
            if (PassThroughFlags.find(flag) != std::string_view::npos) result.passThrough = true;
            if (NoSessionFlags.find(flag) != std::string_view::npos) result.noSession = true;
            if (FlagsWithValue.find(flag) != std::string_view::npos) {
                if (c == arg.size() - 1) ++i; // the value is the next argument
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
        else if (key == "setenv") config.setEnv = true;
        else if (key == "controlpath") config.controlPath = value != QLatin1String("none");
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

int run(const QStringList& args)
{
    const IgnoredSignals signals;
    const Arguments parsed = parseArguments(args);
    QStringList options;
    QByteArray term = qgetenv("TERM");
    std::optional<Config> config;
    if (!parsed.passThrough && parsed.destination >= 0) {
        // ssh resolves the destination itself; on failure it reports the error below.
        const auto resolved = capture(QStringList{QStringLiteral("ssh"), QStringLiteral("-G")} + args, signals);
        if (resolved && resolved->status == 0) config = parseConfig(resolved->output);
    }
    if (config) {
        if (term == "afterglow" && !parsed.noSession) {
            const Install install = qgetenv("AFTERGLOW_SSH_TERMINFO") == "fallback"
                ? Install::Unavailable : installTerminfo(args, parsed, *config, signals, options);
            if (install == Install::ConnectionFailed) return SshFailed;
            if (install == Install::Unavailable) term = "xterm-256color";
        }
        // Servers accept these only when their AcceptEnv allows it.
        QStringList variables;
        for (const char* name : {"COLORTERM", "TERM_PROGRAM", "TERM_PROGRAM_VERSION"}) {
            if (const QString value = qEnvironmentVariable(name); !value.isEmpty() && !value.contains(QLatin1Char(' ')))
                variables << QString::fromLatin1(name) + QLatin1Char('=') + value;
        }
        if (!config->setEnv && !variables.isEmpty()) options << QStringLiteral("-o") << QStringLiteral("SetEnv=") + variables.join(QLatin1Char(' '));
    }
    const bool label = config && !parsed.noSession && isatty(STDOUT_FILENO);
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
    int status = 127;
    if (const pid_t pid = fork(); pid == 0) {
        exec(QStringList{QStringLiteral("ssh")} + options + args, signals, term);
    } else if (pid > 0) {
        status = wait(pid);
    }
    if (label) send(contextEnd(id));
    return status;
}

} // namespace Afterglow::Ssh
