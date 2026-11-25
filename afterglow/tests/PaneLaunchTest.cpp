// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PaneLaunch.h"
#include "SavedEnvironment.h"
#include "config/Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <string>

using Afterglow::Settings;
using AfterglowPty::SpawnOptions;

namespace {

/// The value that @p options give @p name, as the child gets it: the last one.
std::optional<std::string> valueOf(const SpawnOptions& options, const std::string& name)
{
    const auto set = std::find_if(options.environment.rbegin(), options.environment.rend(), [&](const auto& kv) {
        return kv.first == name;
    });
    if (set == options.environment.rend())
        return std::nullopt;
    return set->second;
}

int timesSet(const SpawnOptions& options, const std::string& name)
{
    return int(std::count_if(options.environment.begin(), options.environment.end(), [&](const auto& kv) {
        return kv.first == name;
    }));
}

bool unsets(const SpawnOptions& options, const std::string& name)
{
    return std::find(options.unsetEnvironment.begin(), options.unsetEnvironment.end(), name)
        != options.unsetEnvironment.end();
}

/// Settings that leave argv alone: shell integration would add its options.
Settings plainSettings()
{
    Settings settings;
    settings.shellIntegration = QStringLiteral("disabled");
    return settings;
}

/// Whether the build tree holds afterglow's terminfo entry, which the
/// TERMINFO_DIRS tests need.
bool buildEntry()
{
    return QFile::exists(QFile::decodeName(AFTERGLOW_BUILD_TERMINFO_DIR) + QStringLiteral("/a/afterglow"));
}

} // namespace

TEST(PaneLaunch, TheVariablesOfAnotherTerminalOrOfTheStartingShellGo)
{
    SpawnOptions options;
    Afterglow::offerTerminalEnvironment(options, QStringLiteral("xterm-256color"));
    for (const char* name : {"TMUX",
                             "TMUX_PANE",
                             "STY",
                             "WINDOWID",
                             "VTE_VERSION",
                             "KITTY_WINDOW_ID",
                             "KITTY_PID",
                             "KITTY_LISTEN_ON",
                             "KITTY_PUBLIC_KEY",
                             "WEZTERM_PANE",
                             "WEZTERM_UNIX_SOCKET",
                             "KONSOLE_DBUS_SERVICE",
                             "KONSOLE_DBUS_SESSION",
                             "KONSOLE_DBUS_WINDOW",
                             "KONSOLE_VERSION",
                             "ITERM_SESSION_ID",
                             "WT_SESSION",
                             "XDG_ACTIVATION_TOKEN",
                             "DESKTOP_STARTUP_ID",
                             "AFTERGLOW_RESULT",
                             "OLDPWD",
                             "SHLVL"}) {
        EXPECT_TRUE(unsets(options, name)) << name;
        EXPECT_EQ(timesSet(options, name), 0) << name;
    }
    // These describe the login or the directory, which a pane shares.
    for (const char* name : {"PWD", "SSH_TTY", "SSH_CONNECTION", "SSH_CLIENT", "GPG_TTY"})
        EXPECT_FALSE(unsets(options, name)) << name;
    EXPECT_EQ(valueOf(options, "TERM"), "xterm-256color");
    EXPECT_EQ(valueOf(options, "COLORTERM"), "truecolor");
    EXPECT_EQ(valueOf(options, "TERM_PROGRAM"), "afterglow");
    EXPECT_EQ(valueOf(options, "TERM_PROGRAM_VERSION"), AFTERGLOW_VERSION);
}

TEST(PaneLaunch, ANameThatTheLaunchSetsOrUnsetsKeepsItsValue)
{
    SpawnOptions options;
    options.environment = {{"TMUX", "/tmp/mine,1,0"}, {"COLORTERM", "24bit"}, {"SHLVL", "7"}};
    options.unsetEnvironment = {"TERM_PROGRAM"};
    // An overlay's result file, which PaneOverlay sets before Pane::start.
    AfterglowPty::setVariable(options, "AFTERGLOW_RESULT", "/tmp/result");
    Afterglow::offerTerminalEnvironment(options, QStringLiteral("xterm-256color"));
    for (const char* name : {"TMUX", "COLORTERM", "SHLVL", "AFTERGLOW_RESULT"}) {
        EXPECT_FALSE(unsets(options, name)) << name;
        EXPECT_EQ(timesSet(options, name), 1) << name;
    }
    EXPECT_EQ(valueOf(options, "TMUX"), "/tmp/mine,1,0");
    EXPECT_EQ(valueOf(options, "COLORTERM"), "24bit");
    EXPECT_EQ(valueOf(options, "AFTERGLOW_RESULT"), "/tmp/result");
    EXPECT_EQ(timesSet(options, "TERM_PROGRAM"), 0);
    EXPECT_EQ(std::count(options.unsetEnvironment.begin(), options.unsetEnvironment.end(), "TERM_PROGRAM"), 1);
}

TEST(PaneLaunch, TheTermThatTheChildGetsDecidesTerminfoDirs)
{
    if (!buildEntry())
        GTEST_SKIP() << "no terminfo entry in the build tree";
    const Afterglow::Test::Saved saved("TERMINFO_DIRS");
    qunsetenv("TERMINFO_DIRS");
    // A launch that sets TERM=afterglow under another Terminal.term.
    SpawnOptions own;
    own.environment = {{"TERM", "afterglow"}};
    Afterglow::offerTerminalEnvironment(own, QStringLiteral("xterm-256color"));
    EXPECT_EQ(valueOf(own, "TERM"), "afterglow");
    ASSERT_TRUE(valueOf(own, "TERMINFO_DIRS"));
    EXPECT_TRUE(valueOf(own, "TERMINFO_DIRS")->starts_with(AFTERGLOW_BUILD_TERMINFO_DIR))
        << *valueOf(own, "TERMINFO_DIRS");
    // And one that sets another TERM, or removes it, under Terminal.term=afterglow.
    SpawnOptions other;
    other.environment = {{"TERM", "xterm-256color"}};
    Afterglow::offerTerminalEnvironment(other, QStringLiteral("afterglow"));
    EXPECT_FALSE(valueOf(other, "TERMINFO_DIRS"));
    SpawnOptions none;
    none.unsetEnvironment = {"TERM"};
    Afterglow::offerTerminalEnvironment(none, QStringLiteral("afterglow"));
    EXPECT_FALSE(valueOf(none, "TERMINFO_DIRS"));
    EXPECT_FALSE(valueOf(none, "TERM"));
}

TEST(PaneLaunch, TerminfoDirsHoldEachDirectoryOnce)
{
    if (!buildEntry())
        GTEST_SKIP() << "no terminfo entry in the build tree";
    const Afterglow::Test::Saved saved("TERMINFO_DIRS");
    const auto offered = [] {
        SpawnOptions options;
        Afterglow::offerTerminalEnvironment(options, QStringLiteral("afterglow"));
        return valueOf(options, "TERMINFO_DIRS").value_or("(none)");
    };
    qunsetenv("TERMINFO_DIRS");
    const std::string first = offered();
    ASSERT_TRUE(first.ends_with(':')) << first;
    // An Afterglow started from a pane of one gets the same list, not a
    // second copy of each directory and a "::".
    qputenv("TERMINFO_DIRS", QByteArray::fromStdString(first));
    EXPECT_EQ(offered(), first);
    // Directories of the user's own come after ours, in their order, and
    // an empty entry of theirs keeps its place.
    const std::string ours = first.substr(0, first.size() - 1);
    qputenv("TERMINFO_DIRS", "/a:/b");
    EXPECT_EQ(offered(), ours + ":/a:/b:");
    qputenv("TERMINFO_DIRS", QByteArray::fromStdString("/a::" + ours + ":/b"));
    EXPECT_EQ(offered(), ours + ":/a::/b");
}

TEST(PaneLaunch, ASessionsPaneReadsTheSessionsEnvironment)
{
    if (!buildEntry())
        GTEST_SKIP() << "no terminfo entry in the build tree";
    // A window shows a session that another environment started: what the
    // launch reads of the inherited variables is the session's.
    const Afterglow::Test::Saved saved("TERMINFO_DIRS", "/window/terminfo");
    const Afterglow::InheritedEnvironment session{{"TERMINFO_DIRS", "/session/terminfo"}};
    SpawnOptions options;
    Afterglow::offerTerminalEnvironment(options, QStringLiteral("afterglow"), &session);
    const std::string dirs = valueOf(options, "TERMINFO_DIRS").value_or("(none)");
    EXPECT_NE(dirs.find("/session/terminfo"), std::string::npos) << dirs;
    EXPECT_EQ(dirs.find("/window/terminfo"), std::string::npos) << dirs;
    // A session without the variable has only ours.
    const Afterglow::InheritedEnvironment bare;
    SpawnOptions plain;
    Afterglow::offerTerminalEnvironment(plain, QStringLiteral("afterglow"), &bare);
    EXPECT_EQ(valueOf(plain, "TERMINFO_DIRS").value_or("(none)").find("/window/terminfo"), std::string::npos);
}

TEST(PaneLaunch, APaneWhoseDirectoryIsGoneStartsInTheHomeThatItGets)
{
    const QString gone =
        QDir::tempPath() + QStringLiteral("/afterglow-gone-%1").arg(QCoreApplication::applicationPid());
    ASSERT_FALSE(QFileInfo::exists(gone));
    const std::string launchHome = QDir::tempPath().toStdString();
    const std::string sessionHome = QDir::rootPath().toStdString();
    const Afterglow::InheritedEnvironment session{{"HOME", sessionHome}};
    // The HOME of the launch, which the child gets.
    const SpawnOptions launched = Afterglow::prepareLaunch(plainSettings(), gone, {QStringLiteral("/bin/true")},
                                                           {{QStringLiteral("HOME"), QDir::tempPath()}}, &session);
    EXPECT_EQ(launched.workingDirectory, launchHome);
    // A HOME of the launch that is no directory: the inherited one.
    const SpawnOptions inheritedHome = Afterglow::prepareLaunch(plainSettings(), gone, {QStringLiteral("/bin/true")},
                                                                {{QStringLiteral("HOME"), gone}}, &session);
    EXPECT_EQ(inheritedHome.workingDirectory, sessionHome);
    // Neither: this process's home.
    const Afterglow::InheritedEnvironment none;
    const SpawnOptions own = Afterglow::prepareLaunch(plainSettings(), gone, {QStringLiteral("/bin/true")}, {}, &none);
    EXPECT_EQ(own.workingDirectory, QFile::encodeName(QDir::homePath()).toStdString());
}

TEST(PaneLaunch, TheLaunchsVariablesWinOverTheConfiguredOnes)
{
    Settings settings = plainSettings();
    settings.environment = {{"EDITOR", "vi"}, {"PAGER", "less"}, {"KEPT", "1"}};
    const SpawnOptions options = Afterglow::prepareLaunch(
        settings, QDir::tempPath(), {QStringLiteral("/bin/true")},
        {{QStringLiteral("EDITOR"), QStringLiteral("nvim")}, {QStringLiteral("PAGER"), false}});
    EXPECT_EQ(timesSet(options, "EDITOR"), 1);
    EXPECT_EQ(valueOf(options, "EDITOR"), "nvim");
    EXPECT_EQ(timesSet(options, "PAGER"), 0);
    EXPECT_TRUE(unsets(options, "PAGER"));
    EXPECT_EQ(valueOf(options, "KEPT"), "1");
    EXPECT_EQ(options.argv, std::vector<std::string>{"/bin/true"});
    EXPECT_EQ(options.workingDirectory, QFile::encodeName(QDir::tempPath()).toStdString());
}

TEST(PaneLaunch, TheProgramIsTheCommandThenTheConfiguredShellThenTheUsersShell)
{
    Settings settings = plainSettings();
    EXPECT_TRUE(Afterglow::prepareLaunch(settings, {}, {}, {}).argv.empty());
    settings.shell = QStringLiteral("/bin/sh");
    EXPECT_EQ(Afterglow::prepareLaunch(settings, {}, {}, {}).argv, std::vector<std::string>{"/bin/sh"});
    EXPECT_EQ(Afterglow::prepareLaunch(settings, {}, {QStringLiteral("htop"), QStringLiteral("-d")}, {}).argv,
              (std::vector<std::string>{"htop", "-d"}));
    // A directory that is gone: the home directory.
    EXPECT_EQ(
        Afterglow::prepareLaunch(settings, QStringLiteral("/nonexistent/afterglow-test"), {}, {}).workingDirectory,
        QFile::encodeName(QDir::homePath()).toStdString());
}

TEST(PaneLaunch, TheSshHelperIsOfferedOrRemovedUnlessTheLaunchNamesIt)
{
    Settings settings = plainSettings();
    settings.sshIntegration = true;
    const std::string self = QFile::encodeName(QCoreApplication::applicationFilePath()).toStdString();
    EXPECT_EQ(valueOf(Afterglow::prepareLaunch(settings, {}, {}, {}), "AFTERGLOW_SSH_HELPER"), self);
    const SpawnOptions removed =
        Afterglow::prepareLaunch(settings, {}, {}, {{QStringLiteral("AFTERGLOW_SSH_HELPER"), false}});
    EXPECT_FALSE(valueOf(removed, "AFTERGLOW_SSH_HELPER"));
    EXPECT_TRUE(unsets(removed, "AFTERGLOW_SSH_HELPER"));
    // Off: one inherited from the Afterglow that started this one goes too,
    // unless the launch gives its own.
    settings.sshIntegration = false;
    const SpawnOptions off = Afterglow::prepareLaunch(settings, {}, {}, {});
    EXPECT_FALSE(valueOf(off, "AFTERGLOW_SSH_HELPER"));
    EXPECT_TRUE(unsets(off, "AFTERGLOW_SSH_HELPER"));
    const SpawnOptions given =
        Afterglow::prepareLaunch(settings, {}, {}, {{QStringLiteral("AFTERGLOW_SSH_HELPER"), QStringLiteral("/x")}});
    EXPECT_EQ(valueOf(given, "AFTERGLOW_SSH_HELPER"), "/x");
    EXPECT_FALSE(unsets(given, "AFTERGLOW_SSH_HELPER"));
    EXPECT_EQ(valueOf(off, "AFTERGLOW_SSH_TERMINFO"), settings.sshTerminfo.toStdString());
}

TEST(PaneLaunch, TheLaunchedProgramIsTheCommandsFileName)
{
    EXPECT_EQ(Afterglow::launchedProgramName({}), QString());
    EXPECT_EQ(Afterglow::launchedProgramName({QStringLiteral("/usr/bin/htop"), QStringLiteral("-d")}),
              QStringLiteral("htop"));
    EXPECT_EQ(Afterglow::launchedProgramName({QStringLiteral("less")}), QStringLiteral("less"));
}

// Terminal.shellIntegration's switches and Terminal.cursorBlink reach an
// integrated shell; "disabled" leaves it alone.
TEST(PaneLaunch, TheShellIntegrationSwitchesAndCursorReachTheShell)
{
    Settings settings;
    settings.shell = QStringLiteral("/bin/bash");
    settings.shellIntegration = QStringLiteral("no-title no-sudo");
    settings.terminal.cursorBlink = false;
    const SpawnOptions steady = Afterglow::prepareLaunch(settings, {}, {}, {});
    ASSERT_EQ(valueOf(steady, "AFTERGLOW_BASH_INJECT"), "1") << "the build tree's bash integration was not found";
    EXPECT_EQ(valueOf(steady, "AFTERGLOW_SHELL_FEATURES"), "no-title no-sudo");
    EXPECT_EQ(valueOf(steady, "AFTERGLOW_PROMPT_CURSOR"), "6");
    settings.terminal.cursorBlink = true;
    EXPECT_EQ(valueOf(Afterglow::prepareLaunch(settings, {}, {}, {}), "AFTERGLOW_PROMPT_CURSOR"), "5");
    settings.shellIntegration = QStringLiteral("disabled");
    const SpawnOptions off = Afterglow::prepareLaunch(settings, {}, {}, {});
    EXPECT_FALSE(valueOf(off, "AFTERGLOW_SHELL_FEATURES"));
    EXPECT_EQ(off.argv, std::vector<std::string>{"/bin/bash"});
}
