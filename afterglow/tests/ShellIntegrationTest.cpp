// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/ShellIntegration.h"
#include "SavedEnvironment.h"
#include "shell/Ssh.h"
#include "MuxConnect.h"
#include <gtest/gtest.h>
#include <AfterglowVt/Input.h>
#include <AfterglowVt/Terminal.h>
#include <QFile>
#include <QDir>
#include <QProcess>
#include <algorithm>
#include <atomic>
#include <utility>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <optional>
#include <pwd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace AfterglowPty;
namespace {
QString root()
{
    return QString::fromUtf8(AFTERGLOW_TEST_SHELL_DIR);
}
bool prepare(SpawnOptions& o, const QString& switches, const QString& scripts = {},
             const Afterglow::InheritedEnvironment* inherited = nullptr, bool promptCursorBlinks = true)
{
    Afterglow::ShellIntegration::Options integration;
    integration.switches = switches;
    integration.root = scripts;
    integration.inherited = inherited;
    integration.promptCursorBlinks = promptCursorBlinks;
    return Afterglow::ShellIntegration::prepare(o, integration);
}
void file(const QString& name, const QByteArray& data)
{
    QFile f(name);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    ASSERT_EQ(f.write(data), data.size());
}
struct Output : IPtyListener, AfterglowVt::ITerminalHost
{
    std::mutex mutex;
    std::condition_variable changed;
    std::string text;
    Pty* pty = nullptr;
    std::string pending;
    std::string command;
    int lastExit = -1;
    std::optional<int> closed;
    /// Set before the test ends the shell: a reply after that may find it gone.
    std::atomic<bool> exiting{false};
    void commandStarted(std::string_view value) override
    {
        command = value;
    }
    void commandFinished(int value) override
    {
        lastExit = value;
    }
    AfterglowVt::Terminal terminal{80, 24, *this};
    void writeToPty(std::string_view bytes) override
    {
        // The terminal's answers to the shell's queries. One can come after
        // the shell left (fish asks as it exits): it goes nowhere then, as
        // in a terminal, and the write fails.
        if (!pty) {
            pending += bytes;
        } else if (!pty->write(bytes)) {
            EXPECT_TRUE(exiting.load()) << "the shell refused a reply while it ran";
        }
    }
    void attach(Pty* value)
    {
        std::lock_guard lock(mutex);
        pty = value;
        EXPECT_TRUE(pty->write(std::exchange(pending, {})));
    }
    void resize(int cols, int rows)
    {
        std::lock_guard lock(mutex);
        terminal.resize(cols, rows, 8, 16);
        pty->resize(cols, rows, 0, 0);
    }
    void ptyData(std::string_view bytes) override
    {
        std::lock_guard lock(mutex);
        text += bytes;
        terminal.feed(bytes);
        changed.notify_all();
    }
    void ptyClosed(int status) override
    {
        std::lock_guard lock(mutex);
        closed = status;
        changed.notify_all();
    }
    bool wait(std::string_view needle)
    {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(8), [&] {
            return text.find(needle) != std::string::npos;
        });
    }
    std::optional<int> waitClosed(std::chrono::milliseconds limit)
    {
        std::unique_lock lock(mutex);
        changed.wait_for(lock, limit, [&] {
            return closed.has_value();
        });
        return closed;
    }
    std::string take()
    {
        std::lock_guard lock(mutex);
        return std::exchange(text, {});
    }
    /// Runs @p line, and gives everything since the last take(), up to the
    /// prompt that follows the command. What came before the command's line,
    /// the whole prompt that it was typed at, is in it.
    std::string run(const std::string& line, std::string_view prompt = "AG_READY>")
    {
        std::unique_lock lock(mutex);
        const auto count = [&] {
            std::size_t n = 0;
            for (auto at = text.find(prompt); at != std::string::npos; at = text.find(prompt, at + prompt.size()))
                ++n;
            return n;
        };
        const std::size_t before = count();
        EXPECT_TRUE(pty->write(line + "\r"));
        EXPECT_TRUE(changed.wait_for(lock, std::chrono::seconds(8),
                                     [&] {
                                         return count() > before;
                                     }))
            << line << ": " << text;
        return std::exchange(text, {});
    }
    void exit()
    {
        exiting = true;
        EXPECT_TRUE(pty->write("exit\r"));
    }
};
/// A shell of these tests: startup files, history and state in @p home,
/// never the user's, and @p home/bin first in PATH.
SpawnOptions isolatedShell(const QString& executable, const QString& home,
                           std::vector<std::pair<std::string, std::string>> extra = {})
{
    SpawnOptions o;
    o.argv = {executable.toStdString(), "-i"};
    o.environment = {{"HOME", home.toStdString()},
                     {"XDG_CONFIG_HOME", home.toStdString()},
                     {"XDG_DATA_HOME", (home + "/data").toStdString()},
                     {"XDG_STATE_HOME", (home + "/state").toStdString()},
                     {"XDG_CACHE_HOME", (home + "/cache").toStdString()},
                     {"ZDOTDIR", home.toStdString()},
                     {"PATH", (home + "/bin:" + qEnvironmentVariable("PATH")).toStdString()},
                     {"TERM", "xterm-256color"},
                     {"AFTERGLOW_SHELL_INTEGRATION", "1"}};
    for (auto& e : extra)
        AfterglowPty::setVariable(o, std::move(e.first), std::move(e.second));
    // The runner's history settings would change which lines Bash keeps,
    // and so the titles of commands.
    o.unsetEnvironment = {"HISTFILE",   "HISTSIZE",       "HISTFILESIZE", "HISTCONTROL",
                          "HISTIGNORE", "HISTTIMEFORMAT", "SSH_TTY"};
    return o;
}
/// Starts @p o, a prepared shell, with @p output reading it.
std::unique_ptr<Pty> startShell(SpawnOptions o, Output& output)
{
    std::string error;
    o.deferStart = true;
    auto pty = Pty::spawn(o, output, &error);
    EXPECT_NE(pty, nullptr) << error;
    if (!pty)
        return pty;
    output.attach(pty.get());
    EXPECT_TRUE(pty->start(&error)) << error;
    return pty;
}
std::string valueOf(const SpawnOptions& o, const std::string& name)
{
    for (auto it = o.environment.rbegin(); it != o.environment.rend(); ++it)
        if (it->first == name)
            return it->second;
    return "<unset>";
}
}
TEST(ShellIntegration, LeavesOptOutAndCommandsUntouched)
{
    for (auto args : std::vector<std::vector<std::string>>{{"bash", "-c", "echo hi"},
                                                           {"zsh", "-f"},
                                                           {"fish", "--no-config"},
                                                           {"bash", "script.sh"},
                                                           {"bash", "--rcfile", "myrc"},
                                                           {"sh"}}) {
        SpawnOptions o;
        o.argv = args;
        EXPECT_FALSE(prepare(o, QStringLiteral("enabled"), root()));
        EXPECT_EQ(o.argv, args);
        EXPECT_TRUE(o.environment.empty());
    }
    SpawnOptions o;
    o.argv = {"bash"};
    EXPECT_FALSE(prepare(o, QStringLiteral("disabled"), root()));
    o.environment = {{"AFTERGLOW_SHELL_INTEGRATION", "0"}};
    EXPECT_FALSE(prepare(o, QStringLiteral("enabled"), root()));
    EXPECT_EQ(o.argv, std::vector<std::string>{"bash"});
}
// The switches reach the scripts without "enabled", and the bar follows
// Terminal.cursorBlink. A Fish 4 drops its own marks for no-prompt-mark.
TEST(ShellIntegration, PassesTheSwitchesAndThePromptCursor)
{
    SpawnOptions o;
    o.argv = {"bash", "-i"};
    ASSERT_TRUE(prepare(o, QStringLiteral("enabled no-title no-sudo"), root(), nullptr, true));
    EXPECT_EQ(valueOf(o, "AFTERGLOW_SHELL_FEATURES"), "no-title no-sudo");
    EXPECT_EQ(valueOf(o, "AFTERGLOW_PROMPT_CURSOR"), "5");
    SpawnOptions steady;
    steady.argv = {"zsh"};
    ASSERT_TRUE(prepare(steady, QStringLiteral("enabled"), root(), nullptr, false));
    EXPECT_EQ(valueOf(steady, "AFTERGLOW_SHELL_FEATURES"), "");
    EXPECT_EQ(valueOf(steady, "AFTERGLOW_PROMPT_CURSOR"), "6");
    SpawnOptions fish;
    fish.argv = {"fish", "-l"};
    ASSERT_TRUE(prepare(fish, QStringLiteral("no-prompt-mark"), root()));
    ASSERT_GE(fish.argv.size(), 2u);
    EXPECT_EQ(fish.argv[1], "--features=no-mark-prompt");
    SpawnOptions marks;
    marks.argv = {"fish"};
    ASSERT_TRUE(prepare(marks, QStringLiteral("no-cwd"), root()));
    EXPECT_EQ(std::count(marks.argv.begin(), marks.argv.end(), "--features=no-mark-prompt"), 0);
}
// Kitty's no-rc: the scripts' environment, and no command line or startup
// file of the shell changed. no-complete changes nothing.
TEST(ShellIntegration, NoRcSetsTheEnvironmentOnly)
{
    for (const std::vector<std::string>& args :
         std::vector<std::vector<std::string>>{{"bash", "-i"}, {"zsh"}, {"fish", "-l"}}) {
        SpawnOptions o;
        o.argv = args;
        ASSERT_TRUE(prepare(o, QStringLiteral("no-rc no-cwd"), root(), nullptr, false));
        EXPECT_EQ(o.argv, args);
        EXPECT_EQ(valueOf(o, "AFTERGLOW_SHELL_FEATURES"), "no-rc no-cwd");
        EXPECT_EQ(valueOf(o, "AFTERGLOW_PROMPT_CURSOR"), "6");
        for (const char* name :
             {"ENV", "ZDOTDIR", "AFTERGLOW_BASH_INJECT", "AFTERGLOW_ORIGINAL_ENV", "AFTERGLOW_ORIGINAL_ZDOTDIR"})
            EXPECT_EQ(valueOf(o, name), "<unset>") << name;
    }
    // Not a shell that the integration knows: nothing.
    SpawnOptions sh;
    sh.argv = {"sh"};
    EXPECT_FALSE(prepare(sh, QStringLiteral("no-rc"), root()));
    EXPECT_TRUE(sh.environment.empty());
    SpawnOptions complete;
    complete.argv = {"bash"};
    SpawnOptions plain;
    plain.argv = {"bash"};
    ASSERT_TRUE(prepare(complete, QStringLiteral("no-complete"), root()));
    ASSERT_TRUE(prepare(plain, QStringLiteral("enabled"), root()));
    EXPECT_EQ(complete.argv, plain.argv);
    EXPECT_EQ(valueOf(complete, "ENV"), valueOf(plain, "ENV"));
}
// Bash expands its ENV: a path that it would change, or that would run a
// command, gets no integration.
TEST(ShellIntegration, RefusesABashPathThatBashWouldExpand)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    for (const QString& name : {QStringLiteral("a$HOME"), QStringLiteral("a`id`"), QStringLiteral("a\\b")}) {
        const QString dir = temp.filePath(name);
        ASSERT_TRUE(QDir().mkpath(dir));
        ASSERT_TRUE(QFile::copy(root() + "/bash", dir + "/bash"));
        SpawnOptions o;
        o.argv = {"bash"};
        EXPECT_FALSE(prepare(o, QStringLiteral("enabled"), dir)) << name.toStdString();
        EXPECT_TRUE(o.environment.empty());
    }
}
TEST(ShellIntegration, PreservesEnvironmentOverrides)
{
    SpawnOptions o;
    o.argv = {"zsh", "-il"};
    o.environment = {{"ZDOTDIR", "/custom rc"}};
    ASSERT_TRUE(prepare(o, QStringLiteral("enabled"), root()));
    EXPECT_NE(std::find(o.environment.begin(), o.environment.end(),
                        std::pair<std::string, std::string>{"AFTERGLOW_ORIGINAL_ZDOTDIR", "/custom rc"}),
              o.environment.end());
    EXPECT_EQ(std::count_if(o.environment.begin(), o.environment.end(),
                            [](const auto& e) {
                                return e.first == "ZDOTDIR";
                            }),
              1);
}
TEST(ShellIntegration, ReadsTheInheritedEnvironmentWhenGiven)
{
    // A pane of a mux session: the session's environment, not this
    // process's, names the shell, the opt-out and what the integration saves.
    const Afterglow::Test::Saved zdotdir("ZDOTDIR", "/window rc");
    const Afterglow::Test::Saved optOut("AFTERGLOW_SHELL_INTEGRATION", "0");
    const Afterglow::Test::Saved shell("SHELL", "/bin/sh");
    const std::vector<std::pair<std::string, std::string>> session{{"SHELL", "/usr/bin/zsh"},
                                                                   {"ZDOTDIR", "/session rc"}};
    SpawnOptions o;
    ASSERT_TRUE(prepare(o, QStringLiteral("enabled"), root(), &session));
    ASSERT_FALSE(o.argv.empty());
    EXPECT_EQ(o.argv.front(), "/usr/bin/zsh");
    const auto has = [&o](const std::string& name, const std::string& value) {
        return std::find(o.environment.begin(), o.environment.end(), std::pair{name, value}) != o.environment.end();
    };
    EXPECT_TRUE(has("AFTERGLOW_ORIGINAL_ZDOTDIR", "/session rc"));
    EXPECT_TRUE(has("AFTERGLOW_ORIGINAL_ZDOTDIR_SET", "1"));
    // A session without ZDOTDIR has none to restore, whatever this process has.
    const std::vector<std::pair<std::string, std::string>> bare{{"SHELL", "/usr/bin/zsh"}};
    SpawnOptions unset;
    ASSERT_TRUE(prepare(unset, QStringLiteral("enabled"), root(), &bare));
    EXPECT_NE(std::find(unset.environment.begin(), unset.environment.end(),
                        std::pair<std::string, std::string>{"AFTERGLOW_ORIGINAL_ZDOTDIR_SET", "0"}),
              unset.environment.end());
    // The session's own opt-out holds.
    const std::vector<std::pair<std::string, std::string>> off{{"SHELL", "/usr/bin/zsh"},
                                                               {"AFTERGLOW_SHELL_INTEGRATION", "0"}};
    SpawnOptions none;
    EXPECT_FALSE(prepare(none, QStringLiteral("enabled"), root(), &off));
}
class InteractiveShell : public testing::TestWithParam<std::pair<std::string, bool>>
{
};
TEST_P(InteractiveShell, UserStartupMarkersStatusAndResize)
{
    const auto [shell, login] = GetParam();
    const QString executable = QStandardPaths::findExecutable(QString::fromStdString(shell));
    if (executable.isEmpty())
        GTEST_SKIP() << shell << " not installed";
    QTemporaryDir home(QDir::tempPath() + "/afterglow shell-XXXXXX");
    ASSERT_TRUE(home.isValid());
    const QString dir = home.path();
    file(dir + "/.bashrc",
         login ? "PS1='AG_READY> '; PROMPT_COMMAND=('printf USER_HOOK'); export AG_USER_RC=yes\n"
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
    SpawnOptions o = isolatedShell(
        executable, dir, {{"ENV", "/original/env"}, {"AFTERGLOW_SSH_HELPER", (dir + "/helper").toStdString()}});
    if (login)
        o.argv.back() = "-il";
    ASSERT_TRUE(prepare(o, QStringLiteral("enabled"), root()));
    Output output;
    auto pty = startShell(o, output);
    ASSERT_NE(pty, nullptr);
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
    // The shell restores the variables the integration borrowed, and keeps
    // the integration's own out of the environment of its programs.
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
    ASSERT_TRUE(pty->write(
        "env | grep -c -e '^AFTERGLOW_PROMPT_CURSOR=' -e '^AFTERGLOW_SHELL_FEATURES=' | sed 's/^/EXPORTED_/'\r"));
    ASSERT_TRUE(output.wait("EXPORTED_0")) << output.take();
    ASSERT_TRUE(output.wait("\x1b]133;B")) << output.take();
    output.take();
    // The new size reaches the shell's terminal.
    output.resize(100, 40);
    ASSERT_TRUE(pty->write("stty size\r"));
    ASSERT_TRUE(output.wait("40 100")) << output.take();
    ASSERT_TRUE(output.wait("\x1b]133;B")) << output.take();
    // Integrated shells run ssh through the helper.
    ASSERT_TRUE(pty->write("ssh -p 1 'prod host'\r"));
    ASSERT_TRUE(output.wait("HELPER [ssh] [-p] [1] [prod host]")) << output.take();
    ASSERT_TRUE(output.wait("\x1b]133;B")) << output.take();
    output.exit();
}
// The parts of Terminal.shellIntegration, as Kitty's switches: a bar cursor
// at the prompt and the configured one for a command, titles, the working
// directory, the prompt marks, and a TERM for sudo when afterglow's terminfo
// entry is not the system's.
class ShellFeatures : public testing::TestWithParam<std::string>
{
protected:
    // The output first: the members go in the reverse order, so the PTY
    // stops its reader thread, which writes to the output, before the
    // output goes.
    struct Started
    {
        std::unique_ptr<Output> output;
        std::unique_ptr<Pty> pty;
    };
    // Starts the shell of the test's parameter in a temporary home with
    // Terminal.shellIntegration @p switches, waiting for its first prompt
    // (AG_READY>). @p shRc goes in .bashrc and .zshrc, @p zshEnv in .zshenv
    // and @p fishRc in config.fish.
    Started start(const QString& home, const QString& switches,
                  std::vector<std::pair<std::string, std::string>> extra = {}, const QByteArray& shRc = {},
                  const QByteArray& zshEnv = {}, const QByteArray& fishRc = {})
    {
        Started s;
        const QString executable = QStandardPaths::findExecutable(QString::fromStdString(GetParam()));
        file(home + "/.bashrc", "PS1='AG_READY> '\n" + shRc);
        file(home + "/.zshenv", zshEnv);
        file(home + "/.zshrc", "PS1='AG_READY> '\n" + shRc);
        QDir().mkpath(home + "/fish");
        file(home + "/fish/config.fish", "function fish_prompt; printf 'AG_READY> '; end\n" + fishRc);
        // A sudo and a run0 that show their TERM and arguments, ahead of the system's.
        QDir().mkpath(home + "/bin");
        for (const char* name : {"sudo", "run0"}) {
            const QString path = home + "/bin/" + name;
            file(path,
                 "#!/bin/sh\nprintf '" + QByteArray(name).toUpper()
                     + " TERM=%s' \"$TERM\"; printf ' [%s]' \"$@\"; printf '\\n'\n");
            EXPECT_TRUE(QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        }
        SpawnOptions o = isolatedShell(executable, home, std::move(extra));
        EXPECT_TRUE(prepare(o, switches, root(), nullptr, false));
        s.output = std::make_unique<Output>();
        s.pty = startShell(o, *s.output);
        if (s.pty) {
            EXPECT_TRUE(s.output->wait("AG_READY>")) << s.output->take();
        }
        return s;
    }
};
// The terminal reads what the shell does from its marks: its prompt, a
// command that runs, the exit code after it, and the secondary prompt of a
// command that goes on (bash and zsh: Fish edits such a command in place).
TEST_P(ShellFeatures, TheMarksTellTheShellsState)
{
    if (QStandardPaths::findExecutable(QString::fromStdString(GetParam())).isEmpty())
        GTEST_SKIP() << GetParam() << " not installed";
    const std::string shell = GetParam();
    QTemporaryDir home(QDir::tempPath() + "/afterglow features-XXXXXX");
    ASSERT_TRUE(home.isValid());
    Started s = start(home.path(), QStringLiteral("enabled"));
    ASSERT_TRUE(s.pty);
    Output& o = *s.output;
    using AfterglowVt::ShellState;
    const auto until = [&o](const std::function<bool(const AfterglowVt::Terminal&)>& done) {
        std::unique_lock lock(o.mutex);
        return o.changed.wait_for(lock, std::chrono::seconds(8), [&] {
            return done(o.terminal);
        });
    };
    const auto state = [](ShellState wanted) {
        return [wanted](const AfterglowVt::Terminal& t) {
            return t.shellState() == wanted;
        };
    };
    EXPECT_TRUE(until(state(ShellState::Prompt))) << o.take();
    o.run("false");
    EXPECT_TRUE(until([](const AfterglowVt::Terminal& t) {
        return t.shellState() == ShellState::Prompt && t.lastExitCode() == std::optional<int>(1);
    })) << o.take();
    // Typed text, then erased: input only while it stands.
    ASSERT_TRUE(s.pty->write("echo"));
    EXPECT_TRUE(until([](const AfterglowVt::Terminal& t) {
        return t.shellInput();
    })) << o.take();
    ASSERT_TRUE(s.pty->write("\x7f\x7f\x7f\x7f"));
    EXPECT_TRUE(until([](const AfterglowVt::Terminal& t) {
        return !t.shellInput() && t.shellState() == ShellState::Prompt;
    })) << o.take();
    // A command that runs.
    ASSERT_TRUE(s.pty->write("sleep 2\r"));
    EXPECT_TRUE(until(state(ShellState::Running))) << o.take();
    EXPECT_TRUE(until([](const AfterglowVt::Terminal& t) {
        return t.shellState() == ShellState::Prompt && t.lastExitCode() == std::optional<int>(0);
    })) << o.take();
    if (shell != "fish") {
        ASSERT_TRUE(s.pty->write("echo \"a\r"));
        EXPECT_TRUE(until(state(ShellState::Continuation))) << o.take();
        ASSERT_TRUE(s.pty->write("b\"\r"));
        EXPECT_TRUE(until(state(ShellState::Prompt))) << o.take();
        // Ctrl+C at the secondary prompt: the next primary one. The echo of a
        // typed key says that the line editor reads: the shell prints the
        // mark as it draws PS2, before the editor starts.
        ASSERT_TRUE(s.pty->write("echo \"\r"));
        EXPECT_TRUE(until(state(ShellState::Continuation))) << o.take();
        o.take();
        ASSERT_TRUE(s.pty->write("q"));
        EXPECT_TRUE(o.wait("q")) << o.take();
        ASSERT_TRUE(s.pty->write("\x03"));
        // Readline's SIGINT handler only sets a flag that its loop checks:
        // a signal between that check and the blocking read() waits for the
        // next key (1 run in 30 for bash here). A user's next key draws the
        // prompt; a second Ctrl+C, at a prompt or not, is that key here.
        std::unique_lock lock(o.mutex);
        if (!o.changed.wait_for(lock, std::chrono::seconds(3), [&o] {
                return o.terminal.shellState() == ShellState::Prompt;
            })) {
            lock.unlock();
            ASSERT_TRUE(s.pty->write("\x03"));
        } else {
            lock.unlock();
        }
        EXPECT_TRUE(until(state(ShellState::Prompt)))
            << shell << " screen:\n"
            << [&o] {
                   std::lock_guard screen(o.mutex);
                   return o.terminal.text(AfterglowVt::Terminal::TextScope::Screen);
               }();
    }
    o.exit();
}

// A paste of two lines, bracketed as the control call paste sends it, and
// Enter: the shell runs both lines, and their output regions say how many
// commands it saw.
TEST_P(ShellFeatures, APasteOfTwoLinesRunsBoth)
{
    if (QStandardPaths::findExecutable(QString::fromStdString(GetParam())).isEmpty())
        GTEST_SKIP() << GetParam() << " not installed";
    QTemporaryDir home(QDir::tempPath() + "/afterglow features-XXXXXX");
    ASSERT_TRUE(home.isValid());
    Started s = start(home.path(), QStringLiteral("enabled"));
    ASSERT_TRUE(s.pty);
    Output& o = *s.output;
    using AfterglowVt::ShellState;
    std::uint64_t before = 0;
    AfterglowVt::Modes modes;
    {
        std::unique_lock lock(o.mutex);
        ASSERT_TRUE(o.changed.wait_for(lock, std::chrono::seconds(8),
                                       [&] {
                                           return o.terminal.shellState() == ShellState::Prompt
                                               && o.terminal.modes().bracketedPaste;
                                       }))
            << "bracketed paste: " << o.terminal.modes().bracketedPaste;
        before = o.terminal.lastPromptId();
        modes = o.terminal.modes();
    }
    ASSERT_TRUE(s.pty->write(AfterglowVt::encodePaste("echo o''ne\nfalse", modes)));
    ASSERT_TRUE(s.pty->write("\r"));
    std::unique_lock lock(o.mutex);
    ASSERT_TRUE(o.changed.wait_for(lock, std::chrono::seconds(8), [&] {
        return o.terminal.shellState() == ShellState::Prompt && o.terminal.lastPromptId() > before
            && o.terminal.lastExitCode().has_value();
    })) << o.text;
    EXPECT_EQ(o.terminal.lastExitCode(), std::optional<int>(1));
    std::size_t outputs = 0;
    std::vector<std::optional<int>> codes;
    for (const AfterglowVt::SemanticRegion& region : o.terminal.semanticRegions()) {
        if (region.id > before && region.kind == AfterglowVt::SemanticZoneKind::Output) {
            ++outputs;
            codes.push_back(region.exitCode);
        }
    }
    // Each line's own exit code: bash's PS0 ends the line before with its D.
    if (GetParam() == "bash") {
        EXPECT_EQ(codes, (std::vector<std::optional<int>>{0, 1}));
    }
    // Bash runs each line as a command of its own. Zsh and Fish run the
    // command line that the paste made as one.
    EXPECT_EQ(outputs, GetParam() == "bash" ? 2u : 1u);
    EXPECT_NE(o.text.find("one"), std::string::npos) << o.text;
    lock.unlock();
    o.exit();
}

TEST_P(ShellFeatures, ACursorAndTitlesByDefaultAndNoneWithTheirSwitches)
{
    if (QStandardPaths::findExecutable(QString::fromStdString(GetParam())).isEmpty())
        GTEST_SKIP() << GetParam() << " not installed";
    const std::string shell = GetParam();
    QTemporaryDir home(QDir::tempPath() + "/afterglow features-XXXXXX");
    ASSERT_TRUE(home.isValid());
    {
        Started s = start(home.path(), QStringLiteral("enabled"));
        ASSERT_TRUE(s.pty);
        // The whole first prompt, the command, and the next prompt.
        const std::string all = s.output->run("true");
        // The steady bar of a configuration without blinking (prepare's
        // false) at the prompt, then the configured cursor for the command.
        const std::size_t bar = all.find("\x1b[6 q");
        EXPECT_NE(bar, std::string::npos) << all;
        EXPECT_NE(all.find("\x1b[0 q", bar), std::string::npos) << all;
        if (shell != "fish") { // Fish sets its own titles
            EXPECT_NE(all.find("\x1b]2;"), std::string::npos) << all;
            EXPECT_NE(all.find("\x1b]2;true\x07"), std::string::npos) << all;
        }
        EXPECT_NE(all.find("\x1b]7;file://"), std::string::npos) << all;
        EXPECT_NE(all.find("\x1b]133;A"), std::string::npos) << all;
        s.output->exit();
    }
    {
        Started s = start(home.path(), QStringLiteral("no-cursor no-title no-cwd no-prompt-mark"));
        ASSERT_TRUE(s.pty);
        const std::string all = s.output->run("true");
        EXPECT_NE(all.find("true"), std::string::npos) << all;
        EXPECT_EQ(all.find("\x1b[6 q"), std::string::npos) << all;
        EXPECT_EQ(all.find("\x1b[0 q"), std::string::npos) << all;
        // A Fish 4 drops its own marks too.
        EXPECT_EQ(all.find("\x1b]133;"), std::string::npos) << all;
        if (shell != "fish") {
            // Fish titles itself and reports its directory: the switches
            // turn off only what Afterglow adds.
            EXPECT_EQ(all.find("\x1b]7;"), std::string::npos) << all;
            EXPECT_EQ(all.find("\x1b]2;"), std::string::npos) << all;
        }
        s.output->exit();
    }
}
TEST_P(ShellFeatures, SudoGetsATermThatTheSystemKnows)
{
    const std::string shell = GetParam();
    if (QStandardPaths::findExecutable(QString::fromStdString(shell)).isEmpty())
        GTEST_SKIP() << shell << " not installed";
    if (!QFile::exists(QStringLiteral(AFTERGLOW_TEST_TERMINFO_DIR "/a/afterglow")))
        GTEST_SKIP() << "the build has no terminfo entry";
    // With afterglow's terminfo entry installed in the system, sudo needs no
    // help. The probe of the scripts: no ~/.terminfo, which sudo's user lacks.
    QProcess tput;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.remove(QStringLiteral("TERMINFO_DIRS"));
    env.remove(QStringLiteral("TERMINFO"));
    env.insert(QStringLiteral("HOME"), QStringLiteral("/nonexistent"));
    tput.setProcessEnvironment(env);
    tput.start(QStringLiteral("tput"), {QStringLiteral("-T"), QStringLiteral("afterglow"), QStringLiteral("longname")});
    if (tput.waitForFinished() && tput.exitStatus() == QProcess::NormalExit && tput.exitCode() == 0) {
        GTEST_SKIP() << "the system has afterglow's terminfo entry";
    }
    QTemporaryDir home(QDir::tempPath() + "/afterglow sudo-XXXXXX");
    ASSERT_TRUE(home.isValid());
    const std::vector<std::pair<std::string, std::string>> term{{"TERM", "afterglow"},
                                                                {"TERMINFO_DIRS", AFTERGLOW_TEST_TERMINFO_DIR}};
    // The fakes in the test's bin, never the system's sudo: a startup file
    // of the system can put other directories first.
    const std::string which = shell == "fish" ? "printf 'AG_PATH=%s:%s\\n' (command -s sudo) (command -s run0)"
        : shell == "zsh"                      ? "printf 'AG_PATH=%s:%s\\n' \"$(whence -p sudo)\" \"$(whence -p run0)\""
                                              : "printf 'AG_PATH=%s:%s\\n' \"$(type -P sudo)\" \"$(type -P run0)\"";
    const std::string fakes =
        "AG_PATH=" + home.path().toStdString() + "/bin/sudo:" + home.path().toStdString() + "/bin/run0";
    // An entry in this ~/.terminfo is not root's: sudo still needs the TERM.
    ASSERT_TRUE(QDir().mkpath(home.path() + "/.terminfo/a"));
    ASSERT_TRUE(QFile::copy(AFTERGLOW_TEST_TERMINFO_DIR "/a/afterglow", home.path() + "/.terminfo/a/afterglow"));
    {
        // Aliases of the user's do not stop the integration (the function
        // keyword), and sudo's options stay as typed: sudo keeps TERM from
        // its environment.
        const QByteArray aliases = "alias ssh='ssh -X'\nalias sudo='sudo '\nalias run0='run0 '\n";
        Started s = start(home.path(), QStringLiteral("enabled"), term, aliases, aliases);
        ASSERT_TRUE(s.pty);
        std::string out = s.output->run(which);
        ASSERT_NE(out.find(fakes), std::string::npos) << out;
        EXPECT_NE(out.find("\x1b]133;A"), std::string::npos) << out;
        out = s.output->run("sudo -u root -- vim 'a b'");
        EXPECT_NE(out.find("SUDO TERM=xterm-256color [-u] [root] [--] [vim] [a b]"), std::string::npos) << out;
        out = s.output->run("sudo -k");
        EXPECT_NE(out.find("SUDO TERM=xterm-256color [-k]"), std::string::npos) << out;
        out = s.output->run("run0 -u root id");
        EXPECT_NE(out.find("RUN0 TERM=xterm-256color [--setenv=TERM] [-u] [root] [id]"), std::string::npos) << out;
        s.output->exit();
    }
    // no-sudo, and an integration that the user's startup file turns off:
    // the commands get the pane's TERM.
    for (const bool optOut : {false, true}) {
        Started s = optOut
            ? start(home.path(), QStringLiteral("enabled"), term, "export AFTERGLOW_SHELL_INTEGRATION=0\n", {},
                    "set -gx AFTERGLOW_SHELL_INTEGRATION 0\n")
            : start(home.path(), QStringLiteral("no-sudo"), term);
        ASSERT_TRUE(s.pty);
        std::string out = s.output->run(which);
        ASSERT_NE(out.find(fakes), std::string::npos) << out;
        out = s.output->run("sudo -k");
        EXPECT_NE(out.find("SUDO TERM=afterglow [-k]"), std::string::npos) << optOut << out;
        out = s.output->run("run0 id");
        EXPECT_NE(out.find("RUN0 TERM=afterglow [id]"), std::string::npos) << optOut << out;
        s.output->exit();
    }
}
// A prompt with its own OSC 133 marks keeps them, and gets the rest: the
// directory, the titles and the cursor. The vi key bindings set their own
// cursors. Bash: a line that history did not keep gets no stale title, and
// PS0 holds no command substitution that promptvars off would show as text.
// Zsh: the user's options do not change how the hooks are read.
TEST_P(ShellFeatures, AnOwnPromptAndViBindingsKeepTheRest)
{
    const std::string shell = GetParam();
    if (shell == "fish")
        GTEST_SKIP() << "Fish 4 marks its own prompts";
    if (QStandardPaths::findExecutable(QString::fromStdString(shell)).isEmpty())
        GTEST_SKIP() << shell << " not installed";
    QTemporaryDir home(QDir::tempPath() + "/afterglow prompt-XXXXXX");
    ASSERT_TRUE(home.isValid());
    const bool bash = shell == "bash";
    {
        const QByteArray rc = bash ? "PS1='\\[\\e]133;A\\a\\]AG_READY> '\nHISTCONTROL=ignorespace\n"
                                   : "PS1=$'%{\\e]133;A\\a%}AG_READY> '\nsetopt ksh_arrays\n"
                                     "function userhook { printf USERHOOK; }\nprecmd_functions+=(userhook)\n";
        Started s = start(home.path(), QStringLiteral("enabled"), {}, rc);
        ASSERT_TRUE(s.pty);
        std::string all = s.output->run("true");
        EXPECT_EQ(all.find("\x1b]133;A;redraw=1"), std::string::npos) << all;
        EXPECT_EQ(all.find("\x1b]133;B"), std::string::npos) << all;
        EXPECT_NE(all.find("\x1b]7;file://"), std::string::npos) << all;
        EXPECT_NE(all.find("\x1b[6 q"), std::string::npos) << all;
        EXPECT_NE(all.find("\x1b]2;true\x07"), std::string::npos) << all;
        if (!bash) {
            EXPECT_NE(all.find("USERHOOK"), std::string::npos) << all;
        }
        if (bash) {
            all = s.output->run(" printf 'AG_%s\\n' HIDDEN");
            EXPECT_NE(all.find("AG_HIDDEN"), std::string::npos) << all;
            EXPECT_EQ(all.find("\x1b]2;true\x07"), std::string::npos) << all;
        }
        s.output->exit();
    }
    {
        const QByteArray rc = bash ? "set -o vi\nshopt -u promptvars\n" : "bindkey -v\n";
        Started s = start(home.path(), QStringLiteral("enabled"), {}, rc);
        ASSERT_TRUE(s.pty);
        const std::string all = s.output->run("true");
        EXPECT_NE(all.find("\x1b]133;A;redraw=1"), std::string::npos) << all;
        EXPECT_EQ(all.find("\x1b[6 q"), std::string::npos) << all;
        EXPECT_EQ(all.find("\x1b[0 q"), std::string::npos) << all;
        EXPECT_EQ(all.find("command_title"), std::string::npos) << all;
        s.output->exit();
    }
}
// A venv or conda activate puts its text before the prompt, and so before
// afterglow's marks of the last prompt: the next prompts keep their marks,
// with the text inside them.
TEST_P(ShellFeatures, TextPutBeforeThePromptKeepsTheMarks)
{
    const std::string shell = GetParam();
    if (shell == "fish")
        GTEST_SKIP() << "Fish marks its prompts with events, not with the prompt's text";
    if (QStandardPaths::findExecutable(QString::fromStdString(shell)).isEmpty())
        GTEST_SKIP() << shell << " not installed";
    QTemporaryDir home(QDir::tempPath() + "/afterglow venv-XXXXXX");
    ASSERT_TRUE(home.isValid());
    Started s = start(home.path(), QStringLiteral("enabled"));
    ASSERT_TRUE(s.pty);
    s.output->run("PS1=\"(venv) ${PS1-}\"");
    for (int i = 0; i < 2; ++i) {
        SCOPED_TRACE(i);
        const std::string all = s.output->run("true");
        EXPECT_NE(all.find("\x1b]133;C"), std::string::npos) << all;
        EXPECT_NE(all.find("\x1b]133;D;0"), std::string::npos) << all;
        const std::size_t mark = all.find("\x1b]133;A;redraw=1");
        ASSERT_NE(mark, std::string::npos) << all;
        EXPECT_NE(all.find("(venv) AG_READY>", mark), std::string::npos) << all;
        // Only the new prompt's own mark: none left from the last one.
        EXPECT_EQ(all.find("\x1b]133;A", mark + 1), std::string::npos) << all;
    }
    s.output->exit();
}
// Bash: the title of a command comes from its history entry. A line that
// history drops (ignorespace) gets no title of an older command, and with
// erasedups a line that took a lower number still gets its own. An empty
// history (HISTSIZE=0) gives no title, not an empty one.
TEST_P(ShellFeatures, BashTitlesFollowWhatHistoryKeeps)
{
    const std::string shell = GetParam();
    if (shell != "bash")
        GTEST_SKIP() << "only Bash reads the title from its history";
    if (QStandardPaths::findExecutable(QString::fromStdString(shell)).isEmpty())
        GTEST_SKIP() << shell << " not installed";
    QTemporaryDir home(QDir::tempPath() + "/afterglow history-XXXXXX");
    ASSERT_TRUE(home.isValid());
    Started s = start(home.path(), QStringLiteral("enabled"), {}, "HISTCONTROL=ignoreboth:erasedups\n");
    ASSERT_TRUE(s.pty);
    std::string all = s.output->run("true");
    EXPECT_NE(all.find("\x1b]2;true\x07"), std::string::npos) << all;
    all = s.output->run(" printf 'AG_%s\\n' HIDDEN");
    EXPECT_NE(all.find("AG_HIDDEN"), std::string::npos) << all;
    EXPECT_EQ(all.find("\x1b]2;true\x07"), std::string::npos) << all;
    EXPECT_EQ(all.find("\x1b]2; printf"), std::string::npos) << all;
    all = s.output->run("printf 'AG_%s\\n' SHOWN");
    EXPECT_NE(all.find("\x1b]2;printf 'AG_%s\\n' SHOWN\x07"), std::string::npos) << all;
    // Only where PS0 runs in the shell (Bash 5.3) does the integration see
    // that the entry changed.
    all = s.output->run("printf 'AG_FUNSUB_%s\\n' \"$__afterglow_funsub\"");
    const bool funsub = all.find("AG_FUNSUB_1") != std::string::npos;
    all = s.output->run("true");
    if (funsub) {
        EXPECT_NE(all.find("\x1b]2;true\x07"), std::string::npos) << all;
    }
    EXPECT_EQ(all.find("\x1b]2;printf"), std::string::npos) << all;
    s.output->exit();
    Started empty = start(home.path(), QStringLiteral("enabled"), {}, "HISTSIZE=0\nHISTCONTROL=ignoredups\n");
    ASSERT_TRUE(empty.pty);
    all = empty.output->run("printf 'AG_%s\\n' EMPTY");
    EXPECT_NE(all.find("AG_EMPTY"), std::string::npos) << all;
    EXPECT_EQ(all.find("\x1b]2;\x07"), std::string::npos) << all;
    // The prompt's title of the directory still comes.
    EXPECT_NE(all.find("\x1b]2;"), std::string::npos) << all;
    empty.output->exit();
}
// A startup file that sources the script by hand, as Kitty's no-rc asks:
// the integration loads with the switches, and the shell keeps its own
// startup (ZDOTDIR). With the script loaded by Afterglow too, the line of the
// startup file adds nothing: one mark for each command.
TEST_P(ShellFeatures, AStartupFileCanSourceTheScriptByHand)
{
    const std::string shell = GetParam();
    if (QStandardPaths::findExecutable(QString::fromStdString(shell)).isEmpty())
        GTEST_SKIP() << shell << " not installed";
    QTemporaryDir home(QDir::tempPath() + "/afterglow by hand-XXXXXX");
    ASSERT_TRUE(home.isValid());
    const QString script = root()
        + (shell == "zsh" ? QStringLiteral("/zsh/.zshenv") : QStringLiteral("/") + QString::fromStdString(shell));
    const QByteArray source = "source '" + script.toUtf8() + "'\n";
    for (const QString& switches : {QStringLiteral("no-rc no-title"), QStringLiteral("no-title")}) {
        SCOPED_TRACE(switches.toStdString());
        Started s = start(home.path(), switches, {}, source, {}, source);
        ASSERT_TRUE(s.pty);
        const std::string all = s.output->run("true");
        // The bar cursor of AFTERGLOW_PROMPT_CURSOR: the script ran.
        EXPECT_NE(all.find("\x1b[6 q"), std::string::npos) << all;
        if (shell != "fish") {
            // The switches reached it: no title. Fish titles itself.
            EXPECT_EQ(all.find("\x1b]2;"), std::string::npos) << all;
            EXPECT_NE(all.find("\x1b]7;file://"), std::string::npos) << all;
            const std::size_t command = all.find("\x1b]133;C");
            EXPECT_NE(command, std::string::npos) << all;
            EXPECT_EQ(all.find("\x1b]133;C", command + 1), std::string::npos) << all;
        }
        if (shell == "zsh") {
            const std::string dotdir = s.output->run("printf 'ZDOTDIR_%s_DONE\\n' \"$ZDOTDIR\"");
            EXPECT_NE(dotdir.find("ZDOTDIR_" + home.path().toStdString() + "_DONE"), std::string::npos) << dotdir;
        }
        s.output->exit();
    }
}
// AFTERGLOW_FISH_PROMPT_MARKS gives a Fish 4 the marks of a Fish 3, with
// fish_features turning off its own: the marks here end with BEL, which a
// Fish 4 does not use. The special key stays bound after Fish loads its key
// bindings at the first prompt, and in each mode of the vi bindings.
TEST(FishShell, OlderFishGetsMarksAndTheSpecialKey)
{
    const QString executable = QStandardPaths::findExecutable(QStringLiteral("fish"));
    if (executable.isEmpty())
        GTEST_SKIP() << "fish not installed";
    QTemporaryDir home(QDir::tempPath() + "/afterglow fish3-XXXXXX");
    ASSERT_TRUE(home.isValid());
    const QString dir = home.path();
    QDir().mkpath(dir + "/fish");
    file(dir + "/fish/config.fish", "function fish_prompt; printf 'AG_READY> '; end\n");
    SpawnOptions o =
        isolatedShell(executable, dir, {{"AFTERGLOW_FISH_PROMPT_MARKS", "1"}, {"fish_features", "no-mark-prompt"}});
    ASSERT_TRUE(prepare(o, QStringLiteral("enabled"), root()));
    Output output;
    auto pty = startShell(o, output);
    ASSERT_NE(pty, nullptr);
    ASSERT_TRUE(output.wait("\x1b]133;A;special_key=1\x07")) << output.take();
    ASSERT_TRUE(output.wait("AG_READY>")) << output.take();
    std::string out = output.run("false");
    EXPECT_NE(out.find("\x1b]133;C;cmdline_url=false\x07"), std::string::npos) << out;
    EXPECT_NE(out.find("\x1b]133;D;1\x07"), std::string::npos) << out;
    out = output.run("bind --preset | string match -q '*1u*forward-char*'; and printf 'AG_%s\\n' BOUND");
    EXPECT_NE(out.find("AG_BOUND"), std::string::npos) << out;
    output.run("fish_vi_key_bindings");
    out = output.run("bind --preset -M insert | string match -q '*1u*forward-char*'; and printf 'AG_%s\\n' INSERT");
    EXPECT_NE(out.find("AG_INSERT"), std::string::npos) << out;
    output.exit();
}
INSTANTIATE_TEST_SUITE_P(Shells, ShellFeatures, testing::Values("bash", "zsh", "fish"));
INSTANTIATE_TEST_SUITE_P(Shells, InteractiveShell,
                         testing::Values(std::pair{"bash", false}, std::pair{"bash", true}, std::pair{"zsh", false},
                                         std::pair{"zsh", true}, std::pair{"fish", false}, std::pair{"fish", true}));

TEST(Pty, DeferredStartBoundsQueueAndStopsContinuouslyWritingChild)
{
    struct Listener : IPtyListener
    {
        std::atomic<unsigned> calls{0};
        void ptyData(std::string_view) override
        {
            ++calls;
        }
        void ptyClosed(int) override
        {
            ++calls;
        }
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
    if (const auto job = text.find("JOB="); job != std::string::npos) {
        // Only a process ID: kill() takes 0 and -1 for groups of processes.
        const long pid = std::strtol(text.c_str() + job + 4, nullptr, 10);
        if (pid > 0)
            kill(pid_t(pid), SIGKILL);
    }
}

TEST(Pty, KeepsTheLastValueOfARepeatedVariable)
{
    Output output;
    SpawnOptions options;
    options.argv = {"/bin/sh", "-c",
                    "printf 'COUNT=%s VALUE=%s DONE' \"$(env | grep -c '^AG_REPEATED=')\" \"$AG_REPEATED\""};
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
    EXPECT_EQ(Pty::defaultShell(options),
              account && account->pw_shell && *account->pw_shell ? account->pw_shell : "/bin/sh");
}

TEST(Pty, ChildStartsWithDefaultSignalsAndUtf8Erase)
{
    struct sigaction ignore{}, previous{};
    ignore.sa_handler = SIG_IGN;
    sigaction(SIGUSR1, &ignore, &previous);
    Output status;
    SpawnOptions options;
    options.argv = {"cat", "/proc/self/status"};
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
    ASSERT_TRUE(QFile::setPermissions(directory.filePath("custom-helper"),
                                      QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
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
struct ScopedEnvironment
{
    std::vector<std::pair<QByteArray, std::optional<QByteArray>>> saved;
    void set(const char* name, const QByteArray& value)
    {
        saved.emplace_back(name, qEnvironmentVariableIsSet(name) ? std::optional(qgetenv(name)) : std::nullopt);
        qputenv(name, value);
    }
    void unset(const char* name)
    {
        saved.emplace_back(name, qEnvironmentVariableIsSet(name) ? std::optional(qgetenv(name)) : std::nullopt);
        qunsetenv(name);
    }
    ~ScopedEnvironment()
    {
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
            if (it->second)
                qputenv(it->first.constData(), *it->second);
            else
                qunsetenv(it->first.constData());
        }
    }
};

// A fake ssh on PATH: it answers -G and records every other run.
struct FakeSsh
{
    QTemporaryDir dir;
    ScopedEnvironment environment;
    QString log() const
    {
        return dir.filePath("log");
    }
    FakeSsh()
    {
        file(dir.filePath("ssh"),
             "#!/bin/sh\n"
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
        // The pane that runs the tests: its Afterglow must never get the
        // calls of the remote commands probe.
        environment.unset("AFTERGLOW_SOCKET");
        environment.unset("AFTERGLOW_PANE_ID");
    }
    // The recorded runs, emptying the log.
    QStringList take()
    {
        QFile f(log());
        if (!f.open(QIODevice::ReadOnly))
            return {};
        const QStringList lines = QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
        f.remove();
        return lines;
    }
    // Compiles afterglow's terminfo entry where infocmp finds it.
    bool compileTerminfo()
    {
        const QString tic = QStandardPaths::findExecutable("tic");
        if (tic.isEmpty() || QStandardPaths::findExecutable("infocmp").isEmpty())
            return false;
        QProcess process;
        process.start(tic, {"-x", "-o", dir.filePath("terminfo"), QString::fromUtf8(AFTERGLOW_TEST_TERMINFO_SOURCE)});
        if (!process.waitForFinished() || process.exitCode() != 0)
            return false;
        environment.set("TERMINFO_DIRS", dir.filePath("terminfo").toUtf8() + ':');
        // ncurses reads no TERMINFO_DIRS for root, but -A names the
        // directory for any user: an infocmp ahead of the real one on PATH.
        file(dir.filePath("infocmp"),
             QString("#!/bin/sh\nexec '" + QStandardPaths::findExecutable("infocmp") + "' -A '"
                     + dir.filePath("terminfo") + "' \"$@\"\n")
                 .toUtf8());
        QFile::setPermissions(dir.filePath("infocmp"), QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
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

TEST(Ssh, ASubsystemRunsNoSession)
{
    using Afterglow::Ssh::parseArguments;
    EXPECT_TRUE(parseArguments({"-s", "prod", "sftp"}).noSession);
    EXPECT_TRUE(parseArguments({"-vs", "prod", "sftp"}).noSession);
}

TEST(Ssh, TheProbeRunsInAnyShellAndMakesAPrivateDirectoryForEachHost)
{
    using namespace Afterglow::Ssh;
    const QByteArray script = probeScript();
    ASSERT_TRUE(script.startsWith("sh -c '") && script.endsWith('\''));
    // The remote login shell (fish too) reads it inside single quotes.
    const QByteArray body = script.mid(7, script.size() - 8);
    EXPECT_FALSE(body.contains('\''));
    EXPECT_FALSE(body.contains('\\'));

    QTemporaryDir dir(QStringLiteral("/tmp/agprobe-XXXXXX"));
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(QDir().mkpath(dir.filePath(QStringLiteral("bin"))));
    ASSERT_TRUE(QDir().mkpath(dir.filePath(QStringLiteral("cwd"))));
    QFile aglow(dir.filePath(QStringLiteral("bin/aglow")));
    ASSERT_TRUE(aglow.open(QIODevice::WriteOnly));
    // A prune that fails with 255: the probe still ran, and says so with 0.
    aglow.write("#!/bin/sh\ncase \"$1\" in --version) echo 'aglow 1.0 (protocol 3)';; remote-prune) exit 255;; esac\n");
    aglow.close();
    aglow.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QProcess uname;
    uname.start(QStringLiteral("uname"), {QStringLiteral("-n")});
    ASSERT_TRUE(uname.waitForFinished());
    const QString host = QString::fromUtf8(uname.readAllStandardOutput()).trimmed();

    const auto probe = [&](const QString& home, const QString& runtime, int* status = nullptr) {
        QProcess sh;
        QProcessEnvironment env;
        // The test's own PATH: /usr/bin and /bin hold no uname or mkdir on
        // every system (NixOS).
        env.insert(QStringLiteral("PATH"),
                   dir.filePath(QStringLiteral("bin")) + QLatin1Char(':') + qEnvironmentVariable("PATH"));
        env.insert(QStringLiteral("HOME"), home);
        if (!runtime.isEmpty())
            env.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime);
        sh.setProcessEnvironment(env);
        // Of the test's own: a relative home or runtime directory must make nothing there.
        sh.setWorkingDirectory(dir.filePath(QStringLiteral("cwd")));
        sh.start(QStringLiteral("sh"), {QStringLiteral("-c"), QString::fromUtf8(script)});
        EXPECT_TRUE(sh.waitForFinished());
        if (status)
            *status = sh.exitCode();
        return parseProbe(sh.readAllStandardOutput());
    };
    int status = -1;
    const Probe inHome = probe(dir.filePath(QStringLiteral("home")), {}, &status);
    EXPECT_EQ(status, 0);
    EXPECT_TRUE(inHome.ran);
    EXPECT_TRUE(inHome.aglow);
    EXPECT_EQ(inHome.protocol, 3);
    EXPECT_EQ(inHome.directory, dir.filePath(QStringLiteral("home/.local/state/afterglow/run/")) + host);
    EXPECT_EQ(QFile::permissions(inHome.directory)
                  & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup | QFileDevice::ReadOther
                     | QFileDevice::WriteOther | QFileDevice::ExeOther),
              QFileDevice::Permissions());
    // The runtime directory comes first, and a relative one counts as none.
    EXPECT_EQ(probe(dir.filePath(QStringLiteral("home")), dir.filePath(QStringLiteral("rt"))).directory,
              dir.filePath(QStringLiteral("rt/afterglow")));
    EXPECT_EQ(probe(dir.filePath(QStringLiteral("home")), QStringLiteral("rt")).directory, inHome.directory);
    // Without an absolute home or runtime directory, there is none.
    const Probe nowhere = probe(QStringLiteral("home"), {});
    EXPECT_TRUE(nowhere.ran);
    EXPECT_TRUE(nowhere.directory.isEmpty());
    EXPECT_TRUE(
        QDir(dir.filePath(QStringLiteral("cwd"))).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden));

    // A directory that is not text, or not absolute, is none.
    EXPECT_TRUE(parseProbe("runtime=\ndir=/a\x1b"
                           "b\n")
                    .directory.isEmpty());
    EXPECT_TRUE(parseProbe("runtime=\ndir=a/b\n").directory.isEmpty());
    EXPECT_FALSE(parseProbe("dir=/a\n").ran);
}

// The answer to openRemoteSocket: the socket of a window, or the one that
// the mux daemon makes for a session pane, named with the ID of its host.
TEST(Ssh, ForwardsOnlyThePairOfItsToken)
{
    using Afterglow::Ssh::forwardPair;
    const std::string token = "0123456789abcdef";
    const std::string directory = "/run/user/1000/afterglow";
    const std::string remote = directory + "/" + token + ".sock";
    EXPECT_TRUE(forwardPair("/run/user/1000/afterglow-remote-" + token + ".sock", remote, token, directory));
    EXPECT_TRUE(forwardPair("/run/user/1000/aglow-mux/remote-00112233445566778899aabbccddeeff-" + token + ".sock",
                            remote, token, directory));
    EXPECT_TRUE(
        forwardPair("/home/u/.local/state/aglow-mux/remote-myhost-" + token + ".sock", remote, token, directory));

    // Another token, a daemon's name outside its directory or without an ID.
    EXPECT_FALSE(forwardPair("/run/user/1000/afterglow-remote-fedcba9876543210.sock", remote, token, directory));
    EXPECT_FALSE(forwardPair("/run/user/1000/aglow-mux/remote-id-fedcba9876543210.sock", remote, token, directory));
    EXPECT_FALSE(forwardPair("/run/user/1000/remote-id-" + token + ".sock", remote, token, directory));
    EXPECT_FALSE(forwardPair("/run/user/1000/aglow-mux/remote-" + token + ".sock", remote, token, directory));
    EXPECT_FALSE(forwardPair("/run/user/1000/aglow-mux/socket", remote, token, directory));
    // ssh splits -R at ':' and reads '\' as an escape.
    EXPECT_FALSE(forwardPair("/run/user/1000/aglow-mux/remote-a:b-" + token + ".sock", remote, token, directory));
    EXPECT_FALSE(forwardPair("/run/user/1000/aglow-mux/remote-a\\b-" + token + ".sock", remote, token, directory));
    EXPECT_FALSE(forwardPair("afterglow-remote-" + token + ".sock", remote, token, directory));
    // The remote path is the one in the probe's directory.
    const std::string window = "/run/user/1000/afterglow-remote-" + token + ".sock";
    EXPECT_FALSE(forwardPair(window, directory + "/fedcba9876543210.sock", token, directory));
    EXPECT_FALSE(forwardPair(window, "/tmp/" + token + ".sock", token, directory));
    EXPECT_FALSE(forwardPair("/run/user/1000/afterglow-remote-.sock", directory + "/.sock", {}, directory));
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
    const Config multiplexed = parseConfig(
        "controlpath ~/.ssh/%C\ncontrolmaster auto\ncontrolpersist no\n"
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
    Config hostile;
    hostile.host = "evil;type=x\x1b]0;";
    hostile.user = "a b";
    EXPECT_EQ(contextStart("id", hostile), "\x1b]3008;start=id;type=remote;targetuser=ab;targethost=eviltypex0\x1b\\");
    // The terminal shows the context and clears it at the end.
    struct Host : AfterglowVt::ITerminalHost
    {
        void writeToPty(std::string_view) override
        {
        }
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
    EXPECT_EQ(ssh.take(),
              QStringList({"ARGS [-o] [SetEnv=COLORTERM=truecolor TERM_PROGRAM=afterglow TERM_PROGRAM_VERSION=1.0]"
                           " [-p] [2222] [prod] [uptime]",
                           "TERM=xterm-256color"}));
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
    if (!ssh.compileTerminfo())
        GTEST_SKIP() << "tic or infocmp not installed";
    ssh.environment.set("TERM", "afterglow");
    ssh.environment.set("FAKE_SETENV", "1");
    EXPECT_EQ(Afterglow::Ssh::run({"-p", "2222", "prod"}), 7);
    const QString shared = "[-o] [ControlMaster=auto] [-o] [ControlPath=" + ssh.dir.path()
        + "/afterglow-ssh-helper-%C] [-o] [ControlPersist=10]";
    auto runs = ssh.take();
    ASSERT_EQ(runs.size(), 4) << runs.join('\n').toStdString();
    EXPECT_TRUE(runs[0].startsWith("ARGS " + shared + " [-p] [2222] [-T] [--] [prod] [sh -c "))
        << runs[0].toStdString();
    EXPECT_EQ(runs[2], "ARGS " + shared + " [-p] [2222] [prod]");
    EXPECT_EQ(runs[3], "TERM=afterglow");
    QFile entry(ssh.log() + ".terminfo");
    ASSERT_TRUE(entry.open(QIODevice::ReadOnly));
    EXPECT_TRUE(entry.readAll().contains("\nafterglow|"));
    // The host is remembered: the next session connects directly.
    EXPECT_EQ(Afterglow::Ssh::run({"-p", "2222", "prod"}), 7);
    EXPECT_EQ(ssh.take(), QStringList({"ARGS [-p] [2222] [prod]", "TERM=afterglow"}));
    // The cache names no host, and only its owner can read it.
    QFile cache(ssh.dir.filePath("state/phosphor-works/afterglow/ssh-terminfo"));
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

TEST(Ssh, ADashMInstallsAsNoMasterAndSharesNothing)
{
    // ssh -M joins no master: a master that the install made would only
    // take the session's ControlPath.
    FakeSsh ssh;
    if (!ssh.compileTerminfo())
        GTEST_SKIP() << "tic or infocmp not installed";
    ssh.environment.set("TERM", "afterglow");
    ssh.environment.set("FAKE_SETENV", "1");
    EXPECT_EQ(Afterglow::Ssh::run({"-vM", "-p", "2222", "prod"}), 7);
    const auto runs = ssh.take();
    ASSERT_EQ(runs.size(), 4) << runs.join('\n').toStdString();
    EXPECT_TRUE(runs[0].startsWith("ARGS [-o] [ControlMaster=no] [-v] [-p] [2222] [-T] [--] [prod] [sh -c "))
        << runs[0].toStdString();
    EXPECT_EQ(runs[2], "ARGS [-vM] [-p] [2222] [prod]");
    EXPECT_EQ(runs[3], "TERM=afterglow");
}

TEST(Ssh, InstallsPastRemoteCommandWithLaterOptionsAndInput)
{
    FakeSsh ssh;
    if (!ssh.compileTerminfo())
        GTEST_SKIP() << "tic or infocmp not installed";
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
    if (!ssh.compileTerminfo())
        GTEST_SKIP() << "tic or infocmp not installed";
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
    if (!ssh.compileTerminfo())
        GTEST_SKIP() << "tic or infocmp not installed";
    ssh.environment.set("TERM", "afterglow");
    ssh.environment.set("FAKE_TIC_STATUS", "int");
    // A shell stops a loop only when its job died from SIGINT.
    const pid_t child = fork();
    if (child == 0)
        _exit(Afterglow::Ssh::run({"prod"}));
    ASSERT_GT(child, 0);
    int status = 0;
    ASSERT_EQ(waitpid(child, &status, 0), child);
    EXPECT_TRUE(WIFSIGNALED(status) && WTERMSIG(status) == SIGINT) << status;
    // No session starts after the interrupted install.
    EXPECT_EQ(ssh.take().size(), 2);
}

// The ssh command of `afterglow mux --ssh` and of a window of a session of
// another host: Afterglow's own connection in the private XDG_RUNTIME_DIR,
// never the user's, forwarding nothing, and the options that let its command
// run where the user's config runs none.
TEST(Ssh, SharedCommandOwnsItsConnectionAndRunsPastTheConfig)
{
    FakeSsh ssh;
    ssh.environment.set("FAKE_REMOTECOMMAND", "1");
    // The user's own master too, which the command never joins.
    ssh.environment.set("FAKE_CONFIG",
                        "stdinnull yes\nsessiontype none\ncontrolpath /tmp/user-master\ncontrolmaster auto");
    QString error;
    const QByteArrayList words =
        Afterglow::Ssh::sharedCommand(QStringLiteral("prod"), 15, AfterglowMux::currentEnvironment(), &error);
    const QByteArray path = "ControlPath=" + QFile::encodeName(ssh.dir.path()) + "/afterglow-ssh-%C";
    EXPECT_EQ(words, (QByteArrayList{"ssh", "-T",
                                     "-o",  "ControlMaster=auto",
                                     "-o",  path,
                                     "-o",  "ControlPersist=15",
                                     "-o",  "ClearAllForwardings=yes",
                                     "-o",  "PermitLocalCommand=no",
                                     "-o",  "RemoteCommand=none",
                                     "-o",  "StdinNull=no",
                                     "-o",  "SessionType=default",
                                     "-o",  "ForkAfterAuthentication=no",
                                     "--",  "prod"}))
        << error.toStdString();
    // No private runtime directory: no connection of Afterglow's own.
    ssh.environment.set("XDG_RUNTIME_DIR", "");
    error.clear();
    EXPECT_TRUE(Afterglow::Ssh::sharedCommand(QStringLiteral("prod"), 15, AfterglowMux::currentEnvironment(), &error)
                    .isEmpty());
    EXPECT_TRUE(error.startsWith(QLatin1String("no private XDG_RUNTIME_DIR"))) << error.toStdString();
    // ssh -G comes from the PATH of the environment given, which the window
    // takes on its GUI thread, not from this process's.
    error.clear();
    EXPECT_TRUE(Afterglow::Ssh::sharedCommand(QStringLiteral("prod"), 15,
                                              {"PATH=" + ssh.dir.filePath("none").toStdString()}, &error)
                    .isEmpty());
    EXPECT_TRUE(error.startsWith(QLatin1String("cannot run ssh"))) << error.toStdString();
    // A destination that ssh would read as an option, with its control
    // characters shown as text.
    error.clear();
    EXPECT_TRUE(
        Afterglow::Ssh::sharedCommand(QStringLiteral("-x"), 15, AfterglowMux::currentEnvironment(), &error).isEmpty());
    EXPECT_EQ(error, QStringLiteral("'-x' is no ssh destination"));
    error.clear();
    EXPECT_TRUE(
        Afterglow::Ssh::sharedCommand(QString::fromUtf8("-a\x1b[31m"), 15, AfterglowMux::currentEnvironment(), &error)
            .isEmpty());
    EXPECT_FALSE(error.contains(QChar(0x1b))) << error.toStdString();
    EXPECT_TRUE(error.endsWith(QLatin1String("is no ssh destination"))) << error.toStdString();
}

// A session of another host gets the terminal's variables, in their order,
// but not TERMINFO_DIRS: its directories are this computer's.
TEST(Ssh, RemoteVariablesLeaveOutTheTerminfoDirectoriesOfThisComputer)
{
    const std::vector<std::pair<std::string, std::string>> terminal{{"TERM", "afterglow"},
                                                                    {"TERMINFO_DIRS", "/build/terminfo:"},
                                                                    {"COLORTERM", "truecolor"},
                                                                    {"TERMINFO_DIRS", "/again"}};
    const std::vector<std::pair<std::string, std::string>> remote{{"TERM", "afterglow"}, {"COLORTERM", "truecolor"}};
    EXPECT_TRUE(Afterglow::Ssh::remoteVariables(terminal) == remote);
    EXPECT_TRUE(Afterglow::Ssh::remoteVariables({}).empty());
}

#ifdef AFTERGLOW_BINARY
// `afterglow ssh` from a launcher that left SIGCHLD ignored: the exit
// status of ssh still comes through, not the 127 of a lost child.
TEST(Ssh, AnIgnoredSigchldKeepsTheExitStatusOfSsh)
{
    FakeSsh ssh;
    // Nothing of the user's terminal: no pane, session or display.
    ssh.environment.unset("AFTERGLOW_SESSION");
    ssh.environment.unset("WAYLAND_DISPLAY");
    QProcess process;
    process.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    process.setChildProcessModifier([] {
        ::signal(SIGCHLD, SIG_IGN);
    });
    process.start(QStringLiteral(AFTERGLOW_BINARY), {QStringLiteral("ssh"), QStringLiteral("prod")});
    ASSERT_TRUE(process.waitForStarted());
    process.closeWriteChannel();
    const bool ended = process.waitForFinished(15000);
    if (!ended)
        process.kill();
    ASSERT_TRUE(ended);
    EXPECT_EQ(process.exitStatus(), QProcess::NormalExit);
    EXPECT_EQ(process.exitCode(), 7) << process.readAllStandardError().toStdString();
}
#endif
