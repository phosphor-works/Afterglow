// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/ShellIntegration.h"
#include "shell/Ssh.h"
#include <gtest/gtest.h>
#include <AfterglowVt/Terminal.h>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <algorithm>
#include <utility>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <condition_variable>
#include <mutex>
#include <chrono>
#include <csignal>
#include <optional>
#include <pwd.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace AfterglowPty;
using Afterglow::ShellIntegration::prepare;
namespace {
QString root() { return QString::fromUtf8(AFTERGLOW_TEST_SHELL_DIR); }
void file(const QString& name, const QByteArray& data)
{
    QFile f(name);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    ASSERT_EQ(f.write(data), data.size());
}
struct Output : IPtyListener, AfterglowVt::ITerminalHost {
    std::mutex mutex;
    std::condition_variable changed;
    std::string text;
    Pty* pty = nullptr;
    std::string pending;
    std::string command;
    int lastExit = -1;
    std::optional<int> closed;
    void commandStarted(std::string_view value) override { command = value; }
    void commandFinished(int value) override { lastExit = value; }
    AfterglowVt::Terminal terminal{80, 24, *this};
    void writeToPty(std::string_view bytes) override {
        if (pty) EXPECT_TRUE(pty->write(bytes)); else pending += bytes;
    }
    void attach(Pty* value) {
        std::lock_guard lock(mutex); pty = value; EXPECT_TRUE(pty->write(std::exchange(pending, {})));
    }
    void resize(int cols, int rows) {
        std::lock_guard lock(mutex); terminal.resize(cols, rows, 8, 16); pty->resize(cols, rows, 0, 0);
    }
    void ptyData(std::string_view bytes) override {
        std::lock_guard lock(mutex); text += bytes; terminal.feed(bytes); changed.notify_all();
    }
    void ptyClosed(int status) override { std::lock_guard lock(mutex); closed = status; changed.notify_all(); }
    bool wait(std::string_view needle) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(8), [&] { return text.find(needle) != std::string::npos; });
    }
    std::optional<int> waitClosed(std::chrono::milliseconds limit) {
        std::unique_lock lock(mutex);
        changed.wait_for(lock, limit, [&] { return closed.has_value(); });
        return closed;
    }
    std::string take() { std::lock_guard lock(mutex); return std::exchange(text, {}); }
};
}
TEST(ShellIntegration, LeavesOptOutAndCommandsUntouched)
{
    for (auto args : std::vector<std::vector<std::string>>{{"bash", "-c", "echo hi"}, {"zsh", "-f"},
            {"fish", "--no-config"}, {"bash", "script.sh"}, {"bash", "--rcfile", "myrc"}, {"sh"}}) {
        SpawnOptions o; o.argv = args;
        EXPECT_FALSE(prepare(o, true, root())); EXPECT_EQ(o.argv, args); EXPECT_TRUE(o.environment.empty());
    }
    SpawnOptions o; o.argv = {"bash"};
    EXPECT_FALSE(prepare(o, false, root()));
    o.environment = {{"AFTERGLOW_SHELL_INTEGRATION", "0"}};
    EXPECT_FALSE(prepare(o, true, root()));
    EXPECT_EQ(o.argv, std::vector<std::string>{"bash"});
}
TEST(ShellIntegration, PreservesEnvironmentOverrides)
{
    SpawnOptions o; o.argv = {"zsh", "-il"}; o.environment = {{"ZDOTDIR", "/custom rc"}};
    ASSERT_TRUE(prepare(o, true, root()));
    EXPECT_NE(std::find(o.environment.begin(), o.environment.end(), std::pair<std::string,std::string>{"AFTERGLOW_ORIGINAL_ZDOTDIR", "/custom rc"}), o.environment.end());
    EXPECT_EQ(std::count_if(o.environment.begin(), o.environment.end(), [](const auto& e) { return e.first == "ZDOTDIR"; }), 1);
}
class InteractiveShell : public testing::TestWithParam<std::pair<std::string, bool>> {};
TEST_P(InteractiveShell, UserStartupMarkersStatusAndResize)
{
    const auto [shell, login] = GetParam();
    const QString executable = QStandardPaths::findExecutable(QString::fromStdString(shell));
    if (executable.isEmpty()) GTEST_SKIP() << shell << " not installed";
    QTemporaryDir home(QDir::tempPath() + "/afterglow shell-XXXXXX");
    ASSERT_TRUE(home.isValid());
    const QString dir = home.path();
    file(dir + "/.bashrc", login
        ? "PS1='AG_READY> '; PROMPT_COMMAND=('printf USER_HOOK'); export AG_USER_RC=yes\n"
        : "PS1='AG_READY> '; PROMPT_COMMAND='printf USER_HOOK'; export AG_USER_RC=yes\n");
    file(dir + "/.bash_profile", "export AG_LOGIN=yes; source ~/.bashrc\n");
    file(dir + "/.zshenv", "export AG_ENV=yes\n");
    file(dir + "/.zprofile", "export AG_LOGIN=yes\n");
    file(dir + "/.zshrc", "PS1='AG_READY> '; precmd() { printf USER_HOOK; }; export AG_USER_RC=yes\n");
    file(dir + "/.zlogin", "export AG_LOGIN_LAST=yes\n");
    QDir().mkpath(dir + "/fish");
    file(dir + "/fish/config.fish", "function fish_prompt; printf 'AG_READY> '; end; set -gx AG_USER_RC yes\n");
    file(dir + "/helper", "#!/bin/sh\nprintf 'HELPER'; printf ' [%s]' \"$@\"; printf '\\n'\n");
    ASSERT_TRUE(QFile::setPermissions(dir + "/helper", QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    SpawnOptions o;
    o.argv = {executable.toStdString(), login ? "-il" : "-i"};
    // History and state stay in the temporary home, never the user's.
    o.environment = {{"HOME", dir.toStdString()}, {"XDG_CONFIG_HOME", dir.toStdString()},
        {"XDG_DATA_HOME", (dir + "/data").toStdString()}, {"XDG_STATE_HOME", (dir + "/state").toStdString()},
        {"XDG_CACHE_HOME", (dir + "/cache").toStdString()},
        {"TERM", "xterm-256color"}, {"AFTERGLOW_SHELL_INTEGRATION", "1"}, {"ZDOTDIR", dir.toStdString()},
        {"ENV", "/original/env"}, {"AFTERGLOW_SSH_HELPER", (dir + "/helper").toStdString()}};
    o.unsetEnvironment = {"HISTFILE"};
    ASSERT_TRUE(prepare(o, true, root()));
    Output output; std::string error;
    o.deferStart = true;
    auto pty = Pty::spawn(o, output, &error);
    ASSERT_NE(pty, nullptr) << error;
    output.attach(pty.get());
    ASSERT_TRUE(pty->start(&error)) << error;
    ASSERT_TRUE(output.wait("\x1b]133;B")) << output.take();
    auto initial = output.take();
    EXPECT_NE(initial.find("AG_READY>"), std::string::npos) << initial;
    if (shell != "fish") {
        EXPECT_NE(initial.find("USER_HOOK"), std::string::npos) << initial;
    }
    EXPECT_NE(initial.find("\x1b]133;A"), std::string::npos) << initial;
    ASSERT_TRUE(pty->write("false\r"));
    ASSERT_TRUE(output.wait("\x1b]133;D;1")) << output.take();
    ASSERT_TRUE(output.wait("\x1b]133;B")) << output.take();
    {
        std::lock_guard lock(output.mutex);
        EXPECT_EQ(output.command, "false");
        EXPECT_EQ(output.lastExit, 1);
    }
    auto command = output.take();
    EXPECT_NE(command.find("\x1b]133;C"), std::string::npos) << command;
    ASSERT_TRUE(pty->write("printf 'RC_%s_DONE\\n' \"$AG_USER_RC\"\r"));
    ASSERT_TRUE(output.wait("RC_yes_DONE")) << output.take();
    ASSERT_TRUE(output.wait("\x1b]133;B")) << output.take();
    output.take();
    if (login && shell != "fish") {
        ASSERT_TRUE(pty->write("printf 'LOGIN_%s_DONE\\n' \"$AG_LOGIN\"\r"));
        ASSERT_TRUE(output.wait("LOGIN_yes_DONE")) << output.take();
        ASSERT_TRUE(output.wait("\x1b]133;B")) << output.take();
        output.take();
    }
    // The shell restores the variables the integration borrowed.
    if (shell == "bash") {
        ASSERT_TRUE(pty->write("printf 'ENV_%s_DONE\\n' \"$ENV\"\r"));
        ASSERT_TRUE(output.wait("ENV_/original/env_DONE")) << output.take();
        ASSERT_TRUE(output.wait("\x1b]133;B")) << output.take();
        output.take();
    } else if (shell == "zsh") {
        ASSERT_TRUE(pty->write("printf 'ZDOTDIR_%s_DONE\\n' \"$ZDOTDIR\"\r"));
        ASSERT_TRUE(output.wait("ZDOTDIR_" + dir.toStdString() + "_DONE")) << output.take();
        ASSERT_TRUE(output.wait("\x1b]133;B")) << output.take();
        output.take();
    }
    // The new size reaches the shell's terminal.
    output.resize(100, 40);
    ASSERT_TRUE(pty->write("stty size\r"));
    ASSERT_TRUE(output.wait("40 100")) << output.take();
    ASSERT_TRUE(output.wait("\x1b]133;B")) << output.take();
    // Integrated shells run ssh through the helper.
    ASSERT_TRUE(pty->write("ssh -p 1 'prod host'\r"));
    ASSERT_TRUE(output.wait("HELPER [ssh] [-p] [1] [prod host]")) << output.take();
    ASSERT_TRUE(output.wait("\x1b]133;B")) << output.take();
    ASSERT_TRUE(pty->write("exit\r"));
}
INSTANTIATE_TEST_SUITE_P(Shells, InteractiveShell, testing::Values(
    std::pair{"bash", false}, std::pair{"bash", true}, std::pair{"zsh", false},
    std::pair{"zsh", true}, std::pair{"fish", false}, std::pair{"fish", true}));

TEST(Pty, DeferredStartBoundsQueueAndStopsContinuouslyWritingChild)
{
    struct Listener : IPtyListener {
        std::atomic<unsigned> calls{0};
        void ptyData(std::string_view) override { ++calls; }
        void ptyClosed(int) override { ++calls; }
    } listener;
    SpawnOptions options;
    options.argv = {"/bin/sh", "-c", "while :; do printf output; done"};
    options.deferStart = true;
    auto pty = Pty::spawn(options, listener);
    ASSERT_NE(pty, nullptr);
    EXPECT_EQ(listener.calls.load(), 0u);
    EXPECT_TRUE(pty->write(std::string(32u * 1024 * 1024, 'x')));
    EXPECT_FALSE(pty->write("rejected"));
    ASSERT_TRUE(pty->start());
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!listener.calls.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_GT(listener.calls.load(), 0u);
    const auto started = std::chrono::steady_clock::now();
    pty->stop();
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
    pty->stop();
    EXPECT_FALSE(pty->write("closed"));
    EXPECT_FALSE(pty->start());
}

TEST(Pty, RemovesInheritedEnvironmentAndExplicitValuesWin)
{
    Output output;
    SpawnOptions options;
    options.argv = {"/bin/sh", "-c", "printf 'HOME=%s PATH=%s DONE' \"${HOME-unset}\" \"$PATH\""};
    options.unsetEnvironment = {"HOME", "PATH"};
    options.environment = {{"PATH", "/test/explicit"}};
    options.deferStart = true;
    auto pty = Pty::spawn(options, output);
    ASSERT_NE(pty, nullptr);
    output.attach(pty.get());
    ASSERT_TRUE(pty->start());
    EXPECT_TRUE(output.wait("HOME=unset PATH=/test/explicit DONE")) << output.take();
    pty->stop();
}

TEST(Pty, SpawnReportsExecAndWorkingDirectoryFailuresSynchronously)
{
    Output output;
    SpawnOptions options;
    options.argv = {"/nonexistent/afterglow-test-program"};
    std::string error;
    EXPECT_EQ(Pty::spawn(options, output, &error), nullptr);
    EXPECT_NE(error.find("cannot run /nonexistent/afterglow-test-program"), std::string::npos) << error;
    error.clear();
    options.argv = {"/bin/sh"};
    options.workingDirectory = "/nonexistent/afterglow-test-directory";
    EXPECT_EQ(Pty::spawn(options, output, &error), nullptr);
    EXPECT_NE(error.find("cannot enter directory /nonexistent/afterglow-test-directory"), std::string::npos) << error;
}

TEST(Pty, ClosesWhenTheShellExitsWhileABackgroundJobHoldsTheTerminal)
{
    Output output;
    SpawnOptions options;
    options.argv = {"/bin/sh", "-c", "sleep 30 & printf 'JOB=%s LAST_OUTPUT' $!; exit 3"};
    options.deferStart = true;
    auto pty = Pty::spawn(options, output);
    ASSERT_NE(pty, nullptr);
    output.attach(pty.get());
    ASSERT_TRUE(pty->start());
    const auto status = output.waitClosed(std::chrono::seconds(5));
    ASSERT_TRUE(status.has_value()) << "the pane waited for the background job";
    EXPECT_TRUE(WIFEXITED(*status) && WEXITSTATUS(*status) == 3) << *status;
    const std::string text = output.take();
    EXPECT_NE(text.find("LAST_OUTPUT"), std::string::npos) << text;
    if (const auto job = text.find("JOB="); job != std::string::npos) kill(std::stoi(text.substr(job + 4)), SIGKILL);
}

TEST(Pty, KeepsTheLastValueOfARepeatedVariable)
{
    Output output;
    SpawnOptions options;
    options.argv = {"/bin/sh", "-c", "printf 'COUNT=%s VALUE=%s DONE' \"$(env | grep -c '^AG_REPEATED=')\" \"$AG_REPEATED\""};
    options.environment = {{"AG_REPEATED", "first"}, {"AG_REPEATED", "second"}};
    options.deferStart = true;
    auto pty = Pty::spawn(options, output);
    ASSERT_NE(pty, nullptr);
    output.attach(pty.get());
    ASSERT_TRUE(pty->start());
    EXPECT_TRUE(output.wait("COUNT=1 VALUE=second DONE")) << output.take();
}

TEST(Pty, DefaultShellFollowsTheChildEnvironment)
{
    SpawnOptions options;
    options.environment = {{"SHELL", "/custom/first"}, {"SHELL", "/custom/zsh"}};
    EXPECT_EQ(Pty::defaultShell(options), "/custom/zsh");
    options.environment.clear();
    options.unsetEnvironment = {"SHELL"};
    const passwd* account = getpwuid(getuid());
    EXPECT_EQ(Pty::defaultShell(options), account && account->pw_shell && *account->pw_shell ? account->pw_shell : "/bin/sh");
}

TEST(Pty, ChildStartsWithDefaultSignalsAndUtf8Erase)
{
    struct sigaction ignore {}, previous {};
    ignore.sa_handler = SIG_IGN;
    sigaction(SIGUSR1, &ignore, &previous);
    Output status;
    SpawnOptions options;
    options.argv = {"/bin/cat", "/proc/self/status"};
    options.deferStart = true;
    auto pty = Pty::spawn(options, status);
    sigaction(SIGUSR1, &previous, nullptr);
    ASSERT_NE(pty, nullptr);
    status.attach(pty.get());
    ASSERT_TRUE(pty->start());
    EXPECT_TRUE(status.wait("SigIgn:\t0000000000000000")) << status.take();
    Output modes;
    options.argv = {"/bin/sh", "-c", "stty -a; printf MODES_DONE"};
    auto shell = Pty::spawn(options, modes);
    ASSERT_NE(shell, nullptr);
    modes.attach(shell.get());
    ASSERT_TRUE(shell->start());
    ASSERT_TRUE(modes.wait("MODES_DONE")) << modes.take();
    const std::string text = modes.take();
    EXPECT_NE(text.find(" iutf8"), std::string::npos) << text;
}

TEST(Pty, ReportsOnlyAWorkingDirectoryThatExists)
{
    QTemporaryDir root;
    ASSERT_TRUE(root.isValid());
    ASSERT_TRUE(QDir(root.path()).mkdir("gone"));
    const QString gone = QDir(root.filePath("gone")).canonicalPath();
    Output output;
    SpawnOptions options;
    options.argv = {"/bin/sh", "-c", "printf READY; exec sleep 30"};
    options.workingDirectory = gone.toStdString();
    options.deferStart = true;
    auto pty = Pty::spawn(options, output);
    ASSERT_NE(pty, nullptr);
    output.attach(pty.get());
    ASSERT_TRUE(pty->start());
    ASSERT_TRUE(output.wait("READY"));
    EXPECT_EQ(pty->currentWorkingDirectory(), gone.toStdString());
    ASSERT_TRUE(QDir(root.path()).rmdir("gone"));
    EXPECT_EQ(pty->currentWorkingDirectory(), "");
}

TEST(Pty, ClosingDoesNotWaitForAChildThatIgnoresHangup)
{
    Output output;
    SpawnOptions options;
    options.argv = {"/bin/sh", "-c", "trap '' HUP; printf READY; while :; do sleep 1; done"};
    options.deferStart = true;
    auto pty = Pty::spawn(options, output);
    ASSERT_NE(pty, nullptr);
    output.attach(pty.get());
    ASSERT_TRUE(pty->start());
    ASSERT_TRUE(output.wait("READY"));
    const pid_t child = pty->pid();
    pty.reset();
    // The old destructor waited out the 100 ms grace and killed the child
    // before it returned. Now the grace runs on the reaper: the child that
    // ignores SIGHUP still runs. No wall-clock bound on the reset itself,
    // which a loaded machine can exceed without a bug.
    EXPECT_EQ(kill(child, 0), 0);
    // The child is still killed and reaped, off this thread.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (kill(child, 0) == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_NE(kill(child, 0), 0);
}

TEST(Pty, ExecutableSearchUsesChildPathAndWorkingDirectory)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    file(directory.filePath("custom-helper"), "printf 'CUSTOM_PATH_DONE'\n");
    ASSERT_TRUE(QFile::setPermissions(directory.filePath("custom-helper"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    Output output;
    SpawnOptions options;
    options.argv = {"custom-helper"};
    options.workingDirectory = directory.path().toStdString();
    options.environment = {{"PATH", "."}};
    options.deferStart = true;
    auto pty = Pty::spawn(options, output);
    ASSERT_NE(pty, nullptr);
    output.attach(pty.get());
    ASSERT_TRUE(pty->start());
    EXPECT_TRUE(output.wait("CUSTOM_PATH_DONE"));
    pty->stop();
}

namespace {
// Sets environment variables for one test and restores them afterwards.
struct ScopedEnvironment {
    std::vector<std::pair<QByteArray, std::optional<QByteArray>>> saved;
    void set(const char* name, const QByteArray& value)
    {
        saved.emplace_back(name, qEnvironmentVariableIsSet(name) ? std::optional(qgetenv(name)) : std::nullopt);
        qputenv(name, value);
    }
    ~ScopedEnvironment()
    {
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
            if (it->second) qputenv(it->first.constData(), *it->second);
            else qunsetenv(it->first.constData());
        }
    }
};

// A fake ssh on PATH: it answers -G and records every other run.
struct FakeSsh {
    QTemporaryDir dir;
    ScopedEnvironment environment;
    QString log() const { return dir.filePath("log"); }
    FakeSsh()
    {
        file(dir.filePath("ssh"), "#!/bin/sh\n"
            "for a; do [ \"$a\" = -G ] && { printf 'host prod\\nuser alice\\nhostname 10.0.0.5\\nport 22\\n';"
            " [ -n \"$FAKE_SETENV\" ] && echo 'setenv A=B';"
            " [ -n \"$FAKE_REMOTECOMMAND\" ] && echo 'remotecommand tmux attach';"
            " [ -n \"$FAKE_CONFIG\" ] && printf '%s\\n' \"$FAKE_CONFIG\"; exit 0; }; done\n"
            "{ printf 'ARGS'; printf ' [%s]' \"$@\"; printf '\\nTERM=%s\\n' \"$TERM\"; } >> \"$FAKE_LOG\"\n"
            "case \"$*\" in *tic*) cat > \"$FAKE_LOG.terminfo\";"
            " [ \"$FAKE_TIC_STATUS\" = int ] && kill -INT $$; exit \"${FAKE_TIC_STATUS:-0}\";; esac\n"
            "exit 7\n");
        QFile::setPermissions(dir.filePath("ssh"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        environment.set("PATH", dir.path().toUtf8() + ':' + qgetenv("PATH"));
        environment.set("FAKE_LOG", log().toUtf8());
        environment.set("XDG_STATE_HOME", dir.filePath("state").toUtf8());
        environment.set("XDG_RUNTIME_DIR", dir.path().toUtf8());
        environment.set("COLORTERM", "truecolor");
        environment.set("TERM_PROGRAM", "afterglow");
        environment.set("TERM_PROGRAM_VERSION", "1.0");
        environment.set("TERM", "xterm-256color");
        environment.set("AFTERGLOW_SSH_TERMINFO", "install");
    }
    // The recorded runs, emptying the log.
    QStringList take()
    {
        QFile f(log());
        if (!f.open(QIODevice::ReadOnly)) return {};
        const QStringList lines = QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
        f.remove();
        return lines;
    }
    // Compiles afterglow's terminfo entry where infocmp finds it.
    bool compileTerminfo()
    {
        const QString tic = QStandardPaths::findExecutable("tic");
        if (tic.isEmpty() || QStandardPaths::findExecutable("infocmp").isEmpty()) return false;
        QProcess process;
        process.start(tic, {"-x", "-o", dir.filePath("terminfo"), QString::fromUtf8(AFTERGLOW_TEST_TERMINFO_SOURCE)});
        if (!process.waitForFinished() || process.exitCode() != 0) return false;
        environment.set("TERMINFO_DIRS", dir.filePath("terminfo").toUtf8() + ':');
        return true;
    }
};
}

TEST(Ssh, FindsTheDestinationPastOptionsAndTheirValues)
{
    using Afterglow::Ssh::parseArguments;
    EXPECT_EQ(parseArguments({"prod"}).destination, 0);
    EXPECT_EQ(parseArguments({"-p", "2222", "-v", "prod", "uptime"}).destination, 3);
    EXPECT_EQ(parseArguments({"-vp2222", "prod"}).destination, 1);
    EXPECT_EQ(parseArguments({"-tt", "-L", "8080:localhost:80", "alice@prod"}).destination, 3);
    EXPECT_EQ(parseArguments({"-o", "User=bob", "--", "prod", "-x"}).destination, 3);
    EXPECT_EQ(parseArguments({"-v"}).destination, -1);
    EXPECT_TRUE(parseArguments({"-G", "prod"}).passThrough);
    EXPECT_TRUE(parseArguments({"-V"}).passThrough);
    EXPECT_TRUE(parseArguments({"-O", "exit", "prod"}).passThrough);
    EXPECT_FALSE(parseArguments({"-o", "G", "prod"}).passThrough);
    EXPECT_TRUE(parseArguments({"-fN", "prod"}).noSession);
    EXPECT_FALSE(parseArguments({"-t", "prod"}).noSession);
}

TEST(Ssh, ReadsOptionsAfterTheDestinationUpToTheCommand)
{
    using Afterglow::Ssh::parseArguments;
    const auto after = parseArguments({"prod", "-p", "2222"});
    EXPECT_EQ(after.destination, 0);
    EXPECT_EQ(after.command, -1);
    EXPECT_EQ(after.options, QList<int>({1, 2}));
    EXPECT_TRUE(parseArguments({"prod", "-N"}).noSession);
    EXPECT_EQ(parseArguments({"-v", "prod", "uptime", "-N"}).command, 2);
    EXPECT_FALSE(parseArguments({"-v", "prod", "uptime", "-N"}).noSession);
    // "--" before the destination ends the options for good.
    EXPECT_EQ(parseArguments({"--", "prod", "-N"}).command, 2);
    EXPECT_FALSE(parseArguments({"--", "prod", "-N"}).noSession);
    EXPECT_EQ(parseArguments({"prod", "-v", "--", "-N"}).command, 3);
}

TEST(Ssh, ReadsResolvedConfigAndLabelsThePane)
{
    using namespace Afterglow::Ssh;
    const Config config = parseConfig("host prod\nuser alice\nhostname 10.0.0.5\nport 2222\ncontrolpath none\n");
    EXPECT_EQ(config.host, "prod");
    EXPECT_EQ(config.hostname, "10.0.0.5");
    EXPECT_EQ(config.user, "alice");
    EXPECT_EQ(config.port, "2222");
    EXPECT_FALSE(config.setEnv);
    EXPECT_FALSE(config.controlPath);
    EXPECT_FALSE(config.remoteCommand);
    EXPECT_TRUE(parseConfig("setenv A=B\ncontrolpath ~/.ssh/%C\n").controlPath);
    const Config multiplexed = parseConfig("controlpath ~/.ssh/%C\ncontrolmaster auto\ncontrolpersist no\n"
                                           "remotecommand tmux attach\nproxyjump bastion\n");
    EXPECT_TRUE(multiplexed.controlMaster);
    EXPECT_FALSE(multiplexed.controlPersist);
    EXPECT_TRUE(multiplexed.remoteCommand);
    EXPECT_EQ(multiplexed.proxyJump, "bastion");
    EXPECT_FALSE(config.noSession);
    EXPECT_FALSE(config.stdinNull);
    EXPECT_FALSE(parseConfig("sessiontype default\nstdinnull no\nforkafterauthentication no\n").noSession);
    EXPECT_TRUE(parseConfig("sessiontype none\nforkafterauthentication no\n").noSession);
    EXPECT_TRUE(parseConfig("sessiontype subsystem\n").noSession);
    EXPECT_TRUE(parseConfig("sessiontype default\nforkafterauthentication yes\n").noSession);
    EXPECT_TRUE(parseConfig("stdinnull yes\n").stdinNull);
    EXPECT_FALSE(parseConfig("controlmaster false\ncontrolpersist 600\n").controlMaster);
    EXPECT_TRUE(parseConfig("controlmaster false\ncontrolpersist 600\n").controlPersist);
    EXPECT_EQ(contextStart("id1", config), "\x1b]3008;start=id1;type=remote;targetuser=alice;targethost=prod\x1b\\");
    EXPECT_EQ(contextEnd("id1"), "\x1b]3008;end=id1\x1b\\");
    // Separators and escapes cannot reach the sequence.
    Config hostile; hostile.host = "evil;type=x\x1b]0;"; hostile.user = "a b";
    EXPECT_EQ(contextStart("id", hostile), "\x1b]3008;start=id;type=remote;targetuser=ab;targethost=eviltypex0\x1b\\");
    // The terminal shows the context and clears it at the end.
    struct Host : AfterglowVt::ITerminalHost {
        void writeToPty(std::string_view) override {}
    } host;
    AfterglowVt::Terminal terminal(80, 24, host);
    terminal.feed(contextStart("id1", config).toStdString());
    ASSERT_EQ(terminal.contexts().stack().size(), 1u);
    EXPECT_EQ(terminal.contexts().stack().front().targetHost, "prod");
    terminal.feed(contextEnd("id1").toStdString());
    EXPECT_TRUE(terminal.contexts().stack().empty());
}

TEST(Ssh, OffersTheEnvironmentAndKeepsTheExitStatus)
{
    FakeSsh ssh;
    EXPECT_EQ(Afterglow::Ssh::run({"-p", "2222", "prod", "uptime"}), 7);
    EXPECT_EQ(ssh.take(), QStringList({"ARGS [-o] [SetEnv=COLORTERM=truecolor TERM_PROGRAM=afterglow TERM_PROGRAM_VERSION=1.0]"
                                       " [-p] [2222] [prod] [uptime]", "TERM=xterm-256color"}));
    // The user's own SetEnv stays in charge.
    ssh.environment.set("FAKE_SETENV", "1");
    EXPECT_EQ(Afterglow::Ssh::run({"prod"}), 7);
    EXPECT_EQ(ssh.take(), QStringList({"ARGS [prod]", "TERM=xterm-256color"}));
    // Runs that open no session reach ssh unchanged.
    EXPECT_EQ(Afterglow::Ssh::run({"-V"}), 7);
    EXPECT_EQ(ssh.take(), QStringList({"ARGS [-V]", "TERM=xterm-256color"}));
}

TEST(Ssh, InstallsTheTerminfoEntryOnceOrFallsBack)
{
    FakeSsh ssh;
    if (!ssh.compileTerminfo()) GTEST_SKIP() << "tic or infocmp not installed";
    ssh.environment.set("TERM", "afterglow");
    ssh.environment.set("FAKE_SETENV", "1");
    EXPECT_EQ(Afterglow::Ssh::run({"-p", "2222", "prod"}), 7);
    const QString shared = "[-o] [ControlMaster=auto] [-o] [ControlPath=" + ssh.dir.path() + "/afterglow-ssh-%C] [-o] [ControlPersist=10]";
    auto runs = ssh.take();
    ASSERT_EQ(runs.size(), 4) << runs.join('\n').toStdString();
    EXPECT_TRUE(runs[0].startsWith("ARGS " + shared + " [-p] [2222] [-T] [--] [prod] [sh -c ")) << runs[0].toStdString();
    EXPECT_EQ(runs[2], "ARGS " + shared + " [-p] [2222] [prod]");
    EXPECT_EQ(runs[3], "TERM=afterglow");
    QFile entry(ssh.log() + ".terminfo");
    ASSERT_TRUE(entry.open(QIODevice::ReadOnly));
    EXPECT_TRUE(entry.readAll().contains("\nafterglow|"));
    // The host is remembered: the next session connects directly.
    EXPECT_EQ(Afterglow::Ssh::run({"-p", "2222", "prod"}), 7);
    EXPECT_EQ(ssh.take(), QStringList({"ARGS [-p] [2222] [prod]", "TERM=afterglow"}));
    // The cache names no host, and only its owner can read it.
    QFile cache(ssh.dir.filePath("state/afterglow/ssh-terminfo"));
    ASSERT_TRUE(cache.open(QIODevice::ReadOnly));
    const QByteArray cached = cache.readAll();
    EXPECT_FALSE(cached.contains("10.0.0.5") || cached.contains("alice")) << cached.toStdString();
    EXPECT_FALSE(cache.permissions() & (QFile::ReadGroup | QFile::ReadOther));
    // A host without tic, or the fallback setting, gets xterm-256color. The
    // session reuses the install's connection, and the host is not asked again.
    ssh.environment.set("XDG_STATE_HOME", ssh.dir.filePath("other-state").toUtf8());
    ssh.environment.set("FAKE_TIC_STATUS", "1");
    EXPECT_EQ(Afterglow::Ssh::run({"prod"}), 7);
    runs = ssh.take();
    ASSERT_EQ(runs.size(), 4);
    EXPECT_EQ(runs[2], "ARGS " + shared + " [prod]");
    EXPECT_EQ(runs[3], "TERM=xterm-256color");
    EXPECT_EQ(Afterglow::Ssh::run({"prod"}), 7);
    EXPECT_EQ(ssh.take(), QStringList({"ARGS [prod]", "TERM=xterm-256color"}));
    // An unreachable host fails once, without a second connection attempt.
    ssh.environment.set("XDG_STATE_HOME", ssh.dir.filePath("third-state").toUtf8());
    ssh.environment.set("FAKE_TIC_STATUS", "255");
    EXPECT_EQ(Afterglow::Ssh::run({"prod"}), 255);
    EXPECT_EQ(ssh.take().size(), 2);
    ssh.environment.set("AFTERGLOW_SSH_TERMINFO", "fallback");
    EXPECT_EQ(Afterglow::Ssh::run({"prod"}), 7);
    EXPECT_EQ(ssh.take(), QStringList({"ARGS [prod]", "TERM=xterm-256color"}));
}

TEST(Ssh, InstallsPastRemoteCommandWithLaterOptionsAndInput)
{
    FakeSsh ssh;
    if (!ssh.compileTerminfo()) GTEST_SKIP() << "tic or infocmp not installed";
    ssh.environment.set("TERM", "afterglow");
    ssh.environment.set("FAKE_SETENV", "1");
    ssh.environment.set("FAKE_REMOTECOMMAND", "1");
    // Without a private runtime directory, no connection is shared.
    ssh.environment.set("XDG_RUNTIME_DIR", "");
    EXPECT_EQ(Afterglow::Ssh::run({"-nv", "prod", "-p", "2222"}), 7);
    const auto runs = ssh.take();
    ASSERT_EQ(runs.size(), 4) << runs.join('\n').toStdString();
    // The install drops -n, which would send it an empty entry.
    EXPECT_TRUE(runs[0].startsWith("ARGS [-o] [RemoteCommand=none] [-v] [-p] [2222] [-T] [--] [prod] [sh -c "))
        << runs[0].toStdString();
    EXPECT_EQ(runs[2], "ARGS [-nv] [prod] [-p] [2222]");
    EXPECT_EQ(runs[3], "TERM=afterglow");
}

TEST(Ssh, FollowsTheConfigsSessionTypeAndStdinNull)
{
    FakeSsh ssh;
    if (!ssh.compileTerminfo()) GTEST_SKIP() << "tic or infocmp not installed";
    ssh.environment.set("TERM", "afterglow");
    ssh.environment.set("FAKE_SETENV", "1");
    ssh.environment.set("XDG_RUNTIME_DIR", "");
    // StdinNull in the config would give tic an empty entry: the install turns it off.
    ssh.environment.set("FAKE_CONFIG", "stdinnull yes");
    EXPECT_EQ(Afterglow::Ssh::run({"prod"}), 7);
    auto runs = ssh.take();
    ASSERT_EQ(runs.size(), 4) << runs.join('\n').toStdString();
    EXPECT_TRUE(runs[0].startsWith("ARGS [-o] [StdinNull=no] [-T] [--] [prod] [sh -c ")) << runs[0].toStdString();
    EXPECT_EQ(runs[2], "ARGS [prod]");
    // SessionType none and ForkAfterAuthentication run no shell: nothing is installed.
    ssh.environment.set("XDG_STATE_HOME", ssh.dir.filePath("other-state").toUtf8());
    for (const char* config : {"sessiontype none", "forkafterauthentication yes"}) {
        ssh.environment.set("FAKE_CONFIG", config);
        EXPECT_EQ(Afterglow::Ssh::run({"prod"}), 7);
        EXPECT_EQ(ssh.take(), QStringList({"ARGS [prod]", "TERM=afterglow"})) << config;
    }
}

TEST(Ssh, DiesFromCtrlCAtTheInstallPrompt)
{
    FakeSsh ssh;
    if (!ssh.compileTerminfo()) GTEST_SKIP() << "tic or infocmp not installed";
    ssh.environment.set("TERM", "afterglow");
    ssh.environment.set("FAKE_TIC_STATUS", "int");
    // A shell stops a loop only when its job died from SIGINT.
    const pid_t child = fork();
    if (child == 0) _exit(Afterglow::Ssh::run({"prod"}));
    ASSERT_GT(child, 0);
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGINT) << status;
    // No session starts after the interrupted install.
    EXPECT_EQ(ssh.take().size(), 2);
}
