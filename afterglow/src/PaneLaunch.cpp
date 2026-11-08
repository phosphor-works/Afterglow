// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "PaneLaunch.h"

#include "Pane.h"
#include "app/BuildTree.h"
#include "config/Settings.h"
#include "shell/ShellIntegration.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

namespace Afterglow {

namespace {

bool named(const AfterglowPty::SpawnOptions& options, const std::string& name)
{
    return std::any_of(options.environment.begin(), options.environment.end(), [&](const auto& kv) { return kv.first == name; })
        || std::find(options.unsetEnvironment.begin(), options.unsetEnvironment.end(), name) != options.unsetEnvironment.end();
}

void offer(AfterglowPty::SpawnOptions& options, const std::string& name, const std::string& value)
{
    if (!named(options, name)) options.environment.emplace_back(name, value);
}

} // namespace

AfterglowPty::SpawnOptions prepareLaunch(const Settings& settings, const QString& workingDirectory, const QStringList& command,
                                         const QVariantMap& environment)
{
    AfterglowPty::SpawnOptions opts;
    // The child starts in its home directory rather than failing in one that is gone.
    opts.workingDirectory = (!workingDirectory.isEmpty() && QFileInfo(workingDirectory).isDir() ? workingDirectory : QDir::homePath()).toStdString();
    opts.environment = settings.environment;
    applyLaunchEnvironment(opts, environment);
    if (!command.isEmpty()) {
        for (const QString& arg : command) {
            opts.argv.push_back(arg.toStdString());
        }
    } else if (!settings.shell.isEmpty()) {
        opts.argv = {settings.shell.toStdString()};
    }
    ShellIntegration::prepare(opts, settings.shellIntegration);
    // Integrated shells wrap ssh when the helper is named; a launch's own
    // values win, including one that removes the variable.
    if (settings.sshIntegration) offer(opts, "AFTERGLOW_SSH_HELPER", QCoreApplication::applicationFilePath().toStdString());
    // Off: a helper inherited from the afterglow that started this one goes too.
    else if (!named(opts, "AFTERGLOW_SSH_HELPER")) opts.unsetEnvironment.emplace_back("AFTERGLOW_SSH_HELPER");
    offer(opts, "AFTERGLOW_SSH_TERMINFO", settings.sshTerminfo.toStdString());
    return opts;
}

void offerTerminalEnvironment(AfterglowPty::SpawnOptions& opts, const QString& term)
{
    offer(opts, "TERM", term.toStdString());
    if (term == QLatin1String("afterglow")) {
        // Our own entry: point ncurses at it, ahead of the system databases
        // (the trailing ':' keeps the defaults).
        QStringList dirs;
        for (const char* d : {AFTERGLOW_BUILD_TERMINFO_DIR, AFTERGLOW_INSTALLED_TERMINFO_DIR}) {
            // The build tree's entry is for the build's own binary.
            if (d == std::string_view(AFTERGLOW_BUILD_TERMINFO_DIR) && !runningFromBuildTree()) continue;
            if (QFile::exists(QString::fromUtf8(d) + QStringLiteral("/a/afterglow"))) {
                dirs << QString::fromUtf8(d);
            }
        }
        if (const QByteArray existing = qgetenv("TERMINFO_DIRS"); !existing.isEmpty()) {
            dirs << QString::fromLocal8Bit(existing);
        }
        offer(opts, "TERMINFO_DIRS", (dirs.join(QLatin1Char(':')) + QLatin1Char(':')).toStdString());
    }
    offer(opts, "COLORTERM", "truecolor");
    offer(opts, "TERM_PROGRAM", "afterglow");
    offer(opts, "TERM_PROGRAM_VERSION", AFTERGLOW_VERSION);
}

} // namespace Afterglow
