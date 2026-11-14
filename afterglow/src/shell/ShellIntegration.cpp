// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "ShellIntegration.h"
#include "app/BuildTree.h"
#include <QFileInfo>
#include <algorithm>
#include <cstdlib>
#include <optional>

namespace Afterglow::ShellIntegration {
namespace {
using Environment = std::vector<std::pair<std::string, std::string>>;
/// The value of @p name in the child: what @p o sets, else nothing when it
/// unsets it, else what the child inherits.
std::optional<std::string> env(const AfterglowPty::SpawnOptions& o, const std::string& name, const Environment* inherited)
{
    for (auto it = o.environment.rbegin(); it != o.environment.rend(); ++it)
        if (it->first == name) return it->second;
    if (std::find(o.unsetEnvironment.begin(), o.unsetEnvironment.end(), name) != o.unsetEnvironment.end()) return {};
    if (inherited) {
        for (auto it = inherited->rbegin(); it != inherited->rend(); ++it)
            if (it->first == name) return it->second;
        return {};
    }
    if (const char* value = std::getenv(name.c_str())) return value;
    return {};
}
/// The shell that the child gets without a command (Pty::defaultShell),
/// with SHELL from what it inherits.
std::string defaultShell(const AfterglowPty::SpawnOptions& o, const Environment* inherited)
{
    if (!inherited) return AfterglowPty::Pty::defaultShell(o);
    AfterglowPty::SpawnOptions child;
    child.inheritEnvironment = false;
    if (auto shell = env(o, "SHELL", inherited)) child.environment.emplace_back("SHELL", std::move(*shell));
    return AfterglowPty::Pty::defaultShell(child);
}
void put(AfterglowPty::SpawnOptions& o, const std::string& name, const std::string& value)
{
    std::erase_if(o.environment, [&](const auto& e) { return e.first == name; });
    o.environment.emplace_back(name, value);
}
void save(AfterglowPty::SpawnOptions& o, const std::string& name, const Environment* inherited)
{
    const auto value = env(o, name, inherited);
    put(o, "AFTERGLOW_ORIGINAL_" + name + "_SET", value ? "1" : "0");
    put(o, "AFTERGLOW_ORIGINAL_" + name, value.value_or(""));
}
}
bool prepare(AfterglowPty::SpawnOptions& o, bool enabled, const QString& root, const Environment* inherited)
{
    if (!enabled || env(o, "AFTERGLOW_SHELL_INTEGRATION", inherited) == "0") return false;
    auto args = o.argv;
    if (args.empty()) args.push_back(defaultShell(o, inherited));
    const QString shell = QFileInfo(QString::fromStdString(args.front())).fileName();
    if (shell != QLatin1String("bash") && shell != QLatin1String("zsh") && shell != QLatin1String("fish")) return false;
    // Scripts, -c, explicit startup overrides and unusual modes retain their exact semantics.
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto& a = args[i];
        if (a == "--login" || a == "--interactive") continue;
        if (a.size() < 2 || a[0] != '-' || a.find_first_not_of("il", 1) != std::string::npos) return false;
    }
    QString directory = root;
    if (directory.isEmpty()) {
        directory = runningFromBuildTree() ? QStringLiteral(AFTERGLOW_SOURCE_SHELL_DIR)
                                           : QStringLiteral(AFTERGLOW_INSTALLED_SHELL_DIR);
    }
    const QString entry = directory + (shell == QLatin1String("zsh") ? QStringLiteral("/zsh/.zshenv") : QLatin1Char('/') + shell);
    if (!QFileInfo(entry).isReadable()) return false;
    if (shell == QLatin1String("bash")) {
        save(o, "ENV", inherited);
        put(o, "ENV", entry.toStdString());
        put(o, "AFTERGLOW_BASH_INJECT", "1");
        args.insert(args.begin() + 1, "--posix");
    } else if (shell == QLatin1String("zsh")) {
        save(o, "ZDOTDIR", inherited);
        put(o, "ZDOTDIR", (directory + QStringLiteral("/zsh")).toStdString());
    } else {
        QString quoted = entry;
        quoted.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        quoted.replace(QLatin1Char('\''), QStringLiteral("\\'"));
        args.insert(args.begin() + 1, {"--init-command", (QStringLiteral("source '") + quoted + QLatin1Char('\'')).toStdString()});
    }
    o.argv = std::move(args);
    return true;
}
}
