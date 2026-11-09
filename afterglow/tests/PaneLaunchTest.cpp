// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PaneLaunch.h"
#include "SavedEnvironment.h"
#include "config/Settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
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
    const auto set = std::find_if(options.environment.rbegin(), options.environment.rend(), [&](const auto& kv) { return kv.first == name; });
    if (set == options.environment.rend()) return std::nullopt;
    return set->second;
}

int timesSet(const SpawnOptions& options, const std::string& name)
{
    return int(std::count_if(options.environment.begin(), options.environment.end(), [&](const auto& kv) { return kv.first == name; }));
}

bool unsets(const SpawnOptions& options, const std::string& name)
{
    return std::find(options.unsetEnvironment.begin(), options.unsetEnvironment.end(), name) != options.unsetEnvironment.end();
}

/// Settings that leave argv alone: shell integration would add its options.
Settings plainSettings()
{
    Settings settings;
    settings.shellIntegration = false;
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
    for (const char* name : {"TMUX", "TMUX_PANE", "STY", "WINDOWID", "VTE_VERSION", "KITTY_WINDOW_ID", "KITTY_PID",
                             "KITTY_LISTEN_ON", "KITTY_PUBLIC_KEY", "WEZTERM_PANE", "WEZTERM_UNIX_SOCKET", "KONSOLE_DBUS_SERVICE",
                             "KONSOLE_DBUS_SESSION", "KONSOLE_DBUS_WINDOW", "KONSOLE_VERSION", "ITERM_SESSION_ID", "WT_SESSION",
                             "XDG_ACTIVATION_TOKEN", "DESKTOP_STARTUP_ID", "AFTERGLOW_RESULT", "OLDPWD", "SHLVL"}) {
        EXPECT_TRUE(unsets(options, name)) << name;
        EXPECT_EQ(timesSet(options, name), 0) << name;
    }
    // These describe the login or the directory, which a pane shares.
    for (const char* name : {"PWD", "SSH_TTY", "SSH_CONNECTION", "SSH_CLIENT", "GPG_TTY"}) EXPECT_FALSE(unsets(options, name)) << name;
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
    if (!buildEntry()) GTEST_SKIP() << "no terminfo entry in the build tree";
    const Afterglow::Test::Saved saved("TERMINFO_DIRS");
    qunsetenv("TERMINFO_DIRS");
    // A launch that sets TERM=afterglow under another Terminal.term.
    SpawnOptions own;
    own.environment = {{"TERM", "afterglow"}};
    Afterglow::offerTerminalEnvironment(own, QStringLiteral("xterm-256color"));
    EXPECT_EQ(valueOf(own, "TERM"), "afterglow");
    ASSERT_TRUE(valueOf(own, "TERMINFO_DIRS"));
    EXPECT_TRUE(valueOf(own, "TERMINFO_DIRS")->starts_with(AFTERGLOW_BUILD_TERMINFO_DIR)) << *valueOf(own, "TERMINFO_DIRS");
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
    if (!buildEntry()) GTEST_SKIP() << "no terminfo entry in the build tree";
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

TEST(PaneLaunch, TheLaunchsVariablesWinOverTheConfiguredOnes)
{
    Settings settings = plainSettings();
    settings.environment = {{"EDITOR", "vi"}, {"PAGER", "less"}, {"KEPT", "1"}};
    const SpawnOptions options = Afterglow::prepareLaunch(settings, QDir::tempPath(), {QStringLiteral("/bin/true")},
                                                          {{QStringLiteral("EDITOR"), QStringLiteral("nvim")},
                                                           {QStringLiteral("PAGER"), false}});
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
    EXPECT_EQ(Afterglow::prepareLaunch(settings, QStringLiteral("/nonexistent/afterglow-test"), {}, {}).workingDirectory,
              QFile::encodeName(QDir::homePath()).toStdString());
}

TEST(PaneLaunch, TheSshHelperIsOfferedOrRemovedUnlessTheLaunchNamesIt)
{
    Settings settings = plainSettings();
    settings.sshIntegration = true;
    const std::string self = QFile::encodeName(QCoreApplication::applicationFilePath()).toStdString();
    EXPECT_EQ(valueOf(Afterglow::prepareLaunch(settings, {}, {}, {}), "AFTERGLOW_SSH_HELPER"), self);
    const SpawnOptions removed = Afterglow::prepareLaunch(settings, {}, {}, {{QStringLiteral("AFTERGLOW_SSH_HELPER"), false}});
    EXPECT_FALSE(valueOf(removed, "AFTERGLOW_SSH_HELPER"));
    EXPECT_TRUE(unsets(removed, "AFTERGLOW_SSH_HELPER"));
    // Off: one inherited from the Afterglow that started this one goes too,
    // unless the launch gives its own.
    settings.sshIntegration = false;
    const SpawnOptions off = Afterglow::prepareLaunch(settings, {}, {}, {});
    EXPECT_FALSE(valueOf(off, "AFTERGLOW_SSH_HELPER"));
    EXPECT_TRUE(unsets(off, "AFTERGLOW_SSH_HELPER"));
    const SpawnOptions given = Afterglow::prepareLaunch(settings, {}, {}, {{QStringLiteral("AFTERGLOW_SSH_HELPER"), QStringLiteral("/x")}});
    EXPECT_EQ(valueOf(given, "AFTERGLOW_SSH_HELPER"), "/x");
    EXPECT_FALSE(unsets(given, "AFTERGLOW_SSH_HELPER"));
    EXPECT_EQ(valueOf(off, "AFTERGLOW_SSH_TERMINFO"), settings.sshTerminfo.toStdString());
}

TEST(PaneLaunch, TheLaunchedProgramIsTheCommandsFileName)
{
    EXPECT_EQ(Afterglow::launchedProgramName({}), QString());
    EXPECT_EQ(Afterglow::launchedProgramName({QStringLiteral("/usr/bin/htop"), QStringLiteral("-d")}), QStringLiteral("htop"));
    EXPECT_EQ(Afterglow::launchedProgramName({QStringLiteral("less")}), QStringLiteral("less"));
}
