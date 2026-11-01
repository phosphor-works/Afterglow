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
std::optional<std::string> env(const AfterglowPty::SpawnOptions& o, const std::string& name)
{
    for (auto it = o.environment.rbegin(); it != o.environment.rend(); ++it)
        if (it->first == name) return it->second;
    if (std::find(o.unsetEnvironment.begin(), o.unsetEnvironment.end(), name) != o.unsetEnvironment.end()) return {};
    if (const char* value = std::getenv(name.c_str())) return value;
    return {};
}
void put(AfterglowPty::SpawnOptions& o, const std::string& name, const std::string& value)
{
    std::erase_if(o.environment, [&](const auto& e) { return e.first == name; });
    o.environment.emplace_back(name, value);
}
void save(AfterglowPty::SpawnOptions& o, const std::string& name)
{
    const auto value = env(o, name);
    put(o, "AFTERGLOW_ORIGINAL_" + name + "_SET", value ? "1" : "0");
    put(o, "AFTERGLOW_ORIGINAL_" + name, value.value_or(""));
}
}
bool prepare(AfterglowPty::SpawnOptions& o, bool enabled, const QString& root)
{
    if (!enabled || env(o, "AFTERGLOW_SHELL_INTEGRATION") == "0") return false;
    auto args = o.argv;
    if (args.empty()) args.push_back(AfterglowPty::Pty::defaultShell(o));
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
        save(o, "ENV");
        put(o, "ENV", entry.toStdString());
        put(o, "AFTERGLOW_BASH_INJECT", "1");
        args.insert(args.begin() + 1, "--posix");
    } else if (shell == QLatin1String("zsh")) {
        save(o, "ZDOTDIR");
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
