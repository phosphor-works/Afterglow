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
#include <optional>

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
    void ptyClosed(int) override { changed.notify_all(); }
    bool wait(std::string_view needle) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(8), [&] { return text.find(needle) != std::string::npos; });
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
    o.environment = {{"HOME", dir.toStdString()}, {"XDG_CONFIG_HOME", dir.toStdString()},
        {"TERM", "xterm-256color"}, {"AFTERGLOW_SHELL_INTEGRATION", "1"}, {"ZDOTDIR", dir.toStdString()},
        {"ENV", "/original/env"}, {"AFTERGLOW_SSH_HELPER", (dir + "/helper").toStdString()}};
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
    output.resize(100, 40);
    ASSERT_TRUE(pty->write("printf 'AFTER_%s_DONE\\n' resize\r"));
    ASSERT_TRUE(output.wait("AFTER_resize_DONE")) << output.take();
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
    EXPECT_FALSE(error.empty());
    options.argv = {"/bin/sh"};
    options.workingDirectory = "/nonexistent/afterglow-test-directory";
    EXPECT_EQ(Pty::spawn(options, output, &error), nullptr);
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
            " [ -n \"$FAKE_SETENV\" ] && echo 'setenv A=B'; exit 0; }; done\n"
            "{ printf 'ARGS'; printf ' [%s]' \"$@\"; printf '\\nTERM=%s\\n' \"$TERM\"; } >> \"$FAKE_LOG\"\n"
            "case \"$*\" in *tic*) cat > \"$FAKE_LOG.terminfo\"; exit \"${FAKE_TIC_STATUS:-0}\";; esac\n"
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
    EXPECT_TRUE(parseConfig("setenv A=B\ncontrolpath ~/.ssh/%C\n").controlPath);
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
    EXPECT_TRUE(runs[0].startsWith("ARGS " + shared + " [-p] [2222] [-T] [prod] [sh -c ")) << runs[0].toStdString();
    EXPECT_EQ(runs[2], "ARGS " + shared + " [-p] [2222] [prod]");
    EXPECT_EQ(runs[3], "TERM=afterglow");
    QFile entry(ssh.log() + ".terminfo");
    ASSERT_TRUE(entry.open(QIODevice::ReadOnly));
    EXPECT_TRUE(entry.readAll().contains("\nafterglow|"));
    // The host is remembered: the next session connects directly.
    EXPECT_EQ(Afterglow::Ssh::run({"-p", "2222", "prod"}), 7);
    EXPECT_EQ(ssh.take(), QStringList({"ARGS [-p] [2222] [prod]", "TERM=afterglow"}));
    // A host without tic, or the fallback setting, gets xterm-256color.
    ssh.environment.set("XDG_STATE_HOME", ssh.dir.filePath("other-state").toUtf8());
    ssh.environment.set("FAKE_TIC_STATUS", "1");
    EXPECT_EQ(Afterglow::Ssh::run({"prod"}), 7);
    runs = ssh.take();
    ASSERT_EQ(runs.size(), 4);
    EXPECT_EQ(runs[3], "TERM=xterm-256color");
    // An unreachable host fails once, without a second connection attempt.
    ssh.environment.set("FAKE_TIC_STATUS", "255");
    EXPECT_EQ(Afterglow::Ssh::run({"prod"}), 255);
    EXPECT_EQ(ssh.take().size(), 2);
    ssh.environment.set("AFTERGLOW_SSH_TERMINFO", "fallback");
    EXPECT_EQ(Afterglow::Ssh::run({"prod"}), 7);
    EXPECT_EQ(ssh.take(), QStringList({"ARGS [prod]", "TERM=xterm-256color"}));
}
