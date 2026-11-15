// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PaneLaunch.h"

#include "app/BuildTree.h"
#include "config/Settings.h"
#include "shell/ShellIntegration.h"

#include "MuxLayout.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>
#include <array>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace Afterglow {

namespace {

/// Variables that name another terminal (tmux, screen, X11's window, VTE,
/// kitty, WezTerm, Konsole, iTerm2, Windows Terminal) or the instance of the
/// launcher or shell that started this Afterglow (its activation token, the
/// result file of the overlay it ran in, its OLDPWD and SHLVL). A pane's
/// shell is none of these: inherited, they send a program to a terminal it
/// is not in (tmux passthrough for an image) or tell it a stale value. A
/// window started from tmux and a mux session started from a pane lose them
/// alike.
constexpr std::array<std::string_view, 22> ForeignVariables{
    "TMUX", "TMUX_PANE", "STY", "WINDOWID", "VTE_VERSION", "KITTY_WINDOW_ID", "KITTY_PID", "KITTY_LISTEN_ON",
    "KITTY_PUBLIC_KEY", "WEZTERM_PANE", "WEZTERM_UNIX_SOCKET", "KONSOLE_DBUS_SERVICE", "KONSOLE_DBUS_SESSION",
    "KONSOLE_DBUS_WINDOW", "KONSOLE_VERSION", "ITERM_SESSION_ID", "WT_SESSION", "XDG_ACTIVATION_TOKEN",
    "DESKTOP_STARTUP_ID", "AFTERGLOW_RESULT", "OLDPWD", "SHLVL",
};

bool named(const AfterglowPty::SpawnOptions& options, std::string_view name)
{
    return std::any_of(options.environment.begin(), options.environment.end(), [&](const auto& kv) { return kv.first == name; })
        || std::find(options.unsetEnvironment.begin(), options.unsetEnvironment.end(), name) != options.unsetEnvironment.end();
}

void offer(AfterglowPty::SpawnOptions& options, const std::string& name, const std::string& value)
{
    if (!named(options, name)) options.environment.emplace_back(name, value);
}

/// The value @p name will have in the child when @p options name it, as
/// Pty::spawn decides: the last value set, which wins over an unset. Empty
/// for an unset; nullopt when @p options leave it alone.
std::optional<std::string> givenValue(const AfterglowPty::SpawnOptions& options, std::string_view name)
{
    const auto set = std::find_if(options.environment.rbegin(), options.environment.rend(), [&](const auto& kv) { return kv.first == name; });
    if (set != options.environment.rend()) return set->second;
    if (std::find(options.unsetEnvironment.begin(), options.unsetEnvironment.end(), name) != options.unsetEnvironment.end())
        return std::string();
    return std::nullopt;
}

/// Afterglow's own terminfo directories ahead of the inherited
/// TERMINFO_DIRS, each once, and an empty entry (the system databases)
/// once: an Afterglow started from a pane of one inherits the list that
/// this made, with its directories and its empty entry. Nullopt when no
/// directory holds the entry.
std::optional<std::string> terminfoDirectories()
{
    std::vector<std::string> dirs;
    for (const char* d : {AFTERGLOW_BUILD_TERMINFO_DIR, AFTERGLOW_INSTALLED_TERMINFO_DIR}) {
        // The build tree's entry is for the build's own binary.
        if (d == std::string_view(AFTERGLOW_BUILD_TERMINFO_DIR) && !runningFromBuildTree()) continue;
        if (QFile::exists(QFile::decodeName(d) + QStringLiteral("/a/afterglow"))) dirs.emplace_back(d);
    }
    if (dirs.empty()) return std::nullopt;
    bool defaults = false;
    const std::string inherited = qgetenv("TERMINFO_DIRS").toStdString();
    for (std::size_t start = 0; !inherited.empty() && start <= inherited.size();) {
        const std::size_t end = std::min(inherited.find(':', start), inherited.size());
        std::string entry = inherited.substr(start, end - start);
        start = end + 1;
        if (entry.empty()) {
            // Where the system databases come in the inherited order.
            if (!std::exchange(defaults, true)) dirs.emplace_back();
        } else if (std::find(dirs.begin(), dirs.end(), entry) == dirs.end()) {
            dirs.push_back(std::move(entry));
        }
    }
    // The trailing empty entry keeps the system databases after ours.
    if (!defaults) dirs.emplace_back();
    std::string joined;
    for (const std::string& dir : dirs) joined += (&dir == &dirs.front() ? "" : ":") + dir;
    return joined;
}

} // namespace

AfterglowPty::SpawnOptions prepareLaunch(const Settings& settings, const QString& workingDirectory, const QStringList& command,
                                         const QVariantMap& environment)
{
    AfterglowPty::SpawnOptions opts;
    // The child starts in its home directory rather than failing in one that is gone.
    opts.workingDirectory =
        QFile::encodeName(!workingDirectory.isEmpty() && QFileInfo(workingDirectory).isDir() ? workingDirectory : QDir::homePath())
            .toStdString();
    opts.environment = settings.environment;
    applyLaunchEnvironment(opts, environment);
    if (!command.isEmpty()) {
        for (const QString& arg : command) {
            opts.argv.push_back(arg.toStdString());
        }
    } else if (!settings.shell.isEmpty()) {
        opts.argv = {QFile::encodeName(settings.shell).toStdString()};
    }
    ShellIntegration::prepare(opts, settings.shellIntegration);
    // Integrated shells wrap ssh when the helper is named; a launch's own
    // values win, including one that removes the variable.
    if (settings.sshIntegration) offer(opts, "AFTERGLOW_SSH_HELPER", QFile::encodeName(QCoreApplication::applicationFilePath()).toStdString());
    // Off: a helper inherited from the afterglow that started this one goes too.
    else if (!named(opts, "AFTERGLOW_SSH_HELPER")) opts.unsetEnvironment.emplace_back("AFTERGLOW_SSH_HELPER");
    offer(opts, "AFTERGLOW_SSH_TERMINFO", settings.sshTerminfo.toStdString());
    return opts;
}

void offerTerminalEnvironment(AfterglowPty::SpawnOptions& opts, const QString& term)
{
    for (const std::string_view name : ForeignVariables) {
        if (!named(opts, name)) opts.unsetEnvironment.emplace_back(name);
    }
    offer(opts, "TERM", term.toStdString());
    // The TERM that the child gets: a launch's own wins over Terminal.term.
    if (givenValue(opts, "TERM") == "afterglow") {
        // Our own entry: point ncurses at it, ahead of the system databases.
        if (const auto dirs = terminfoDirectories()) offer(opts, "TERMINFO_DIRS", *dirs);
    }
    offer(opts, "COLORTERM", "truecolor");
    offer(opts, "TERM_PROGRAM", "afterglow");
    offer(opts, "TERM_PROGRAM_VERSION", AFTERGLOW_VERSION);
}

void applyLaunchEnvironment(AfterglowPty::SpawnOptions& options, const QVariantMap& environment)
{
    for (auto it = environment.cbegin(); it != environment.cend(); ++it) {
        if (it.value().metaType().id() == QMetaType::Bool) AfterglowPty::unsetVariable(options, it.key().toStdString());
        else AfterglowPty::setVariable(options, it.key().toStdString(), it.value().toString().toStdString());
    }
}

std::vector<std::string> sessionCommand(const QStringList& command)
{
    std::vector<std::string> out;
    for (const QString& word : command) out.push_back(word.toStdString());
    if (!out.empty() && !AfterglowMux::validPaneCommand(out)) out.clear();
    return out;
}

QString launchedProgramName(const QStringList& command)
{
    return command.isEmpty() ? QString() : QFileInfo(command.first()).fileName();
}

} // namespace Afterglow
