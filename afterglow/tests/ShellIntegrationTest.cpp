// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "shell/ShellIntegration.h"
#include <gtest/gtest.h>
#include <AfterglowVt/Terminal.h>
#include <QFile>
#include <QDir>
#include <algorithm>
#include <utility>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <condition_variable>
#include <mutex>
#include <chrono>

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
    SpawnOptions o;
    o.argv = {executable.toStdString(), login ? "-il" : "-i"};
    o.environment = {{"HOME", dir.toStdString()}, {"XDG_CONFIG_HOME", dir.toStdString()},
        {"TERM", "xterm-256color"}, {"AFTERGLOW_SHELL_INTEGRATION", "1"}, {"ZDOTDIR", dir.toStdString()},
        {"ENV", "/original/env"}};
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
    if (shell != "fish") EXPECT_NE(initial.find("USER_HOOK"), std::string::npos) << initial;
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
