// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Session.h"

#include <algorithm>

namespace AfterglowMux {

namespace {

/// The environment of a pane: the session's, without what @p launch unsets,
/// then with what it sets.
std::vector<std::pair<std::string, std::string>> paneEnvironment(std::vector<std::pair<std::string, std::string>> environment,
                                                                 const Launch& launch)
{
    const auto drop = [&environment](const std::string& name) {
        std::erase_if(environment, [&name](const auto& variable) { return variable.first == name; });
    };
    for (const std::string& name : launch.unset) drop(name);
    for (const auto& [name, value] : launch.environment) {
        drop(name);
        environment.emplace_back(name, value);
    }
    return environment;
}

} // namespace

Session::Session(std::string name, SessionOptions options, long fileLimit, ImageDecoder& images)
    : m_name(std::move(name))
    , m_options(std::move(options))
    , m_fileLimit(fileLimit)
    , m_images(images)
{
}

bool Session::addPane(std::uint32_t id, const Launch& launch, const PaneSize& size, const std::string& daemonSocket,
                      std::function<void()> wake, std::string* error)
{
    AfterglowPty::SpawnOptions spawn;
    spawn.argv = launch.argv;
    spawn.workingDirectory = launch.workingDirectory;
    spawn.environment = paneEnvironment(m_options.environment, launch);
    if (m_options.callerEnvironment) {
        // The environment of a command over ssh, which no login read: the
        // shell reads the login's files, as an interactive ssh login does.
        if (spawn.argv.empty()) spawn.argv = {AfterglowPty::Pty::defaultShell(spawn), "-l"};
        if (spawn.workingDirectory.empty()) {
            spawn.workingDirectory = "/";
            for (const auto& [name, value] : spawn.environment) {
                if (name == "HOME" && !value.empty()) spawn.workingDirectory = value;
            }
        }
    }
    // The session's environment is the client's, which it sent whole: the
    // daemon's own (a systemd unit's INVOCATION_ID, for one) stays out. So
    // do the daemon's umask and its raised descriptor limit.
    spawn.inheritEnvironment = false;
    spawn.umask = m_options.umask;
    spawn.fileLimit = m_fileLimit;
    // The programs write kitty's temporary files in their TMPDIR, which is
    // the client's, not the daemon's: /tmp when they have none.
    std::string temporaryDirectory = "/tmp";
    for (const auto& [name, value] : spawn.environment) {
        if (name == "TMPDIR" && !value.empty()) temporaryDirectory = value;
    }
    // `afterglow ctl` and `aglow run` in a session pane reach the daemon,
    // which answers their calls with CALL_FAILED, also while a window shows
    // the session. The pane ID is the daemon's, whichever client sent it.
    AfterglowPty::setVariable(spawn, "AFTERGLOW_SOCKET", daemonSocket);
    AfterglowPty::setVariable(spawn, "AFTERGLOW_PANE_ID", std::to_string(id));
    AfterglowPty::setVariable(spawn, "AFTERGLOW_SESSION", m_name);
    AfterglowPty::unsetVariable(spawn, "AFTERGLOW_OVERLAY");
    auto pane = std::make_unique<MuxPane>(id, m_options.terminal, size, m_options.notificationCapabilities, m_images, std::move(wake));
    pane->setTemporaryDirectory(temporaryDirectory);
    if (!pane->start(std::move(spawn), launch.launchedProgram, error)) return false;
    m_panes.push_back(std::move(pane));
    m_layout.addTab(id);
    // The next window saves the command in a project, as the client that
    // started the pane would: while the layout has room for it.
    std::size_t bytes = 0;
    for (const std::string& arg : launch.command) bytes += arg.size();
    if (!launch.command.empty() && m_layout.valueBytes() + bytes <= MaxLayoutValueBytes) m_layout.values[id].command = launch.command;
    return true;
}

std::vector<std::unique_ptr<MuxPane>> Session::reap()
{
    std::vector<std::unique_ptr<MuxPane>> closed;
    for (auto& pane : m_panes) {
        if (pane->exited()) {
            m_layout.remove(pane->id());
            closed.push_back(std::move(pane));
        }
    }
    std::erase(m_panes, nullptr);
    return closed;
}

bool Session::closePane(std::uint32_t id)
{
    const auto it = std::find_if(m_panes.begin(), m_panes.end(), [id](const auto& pane) { return pane->id() == id; });
    if (it == m_panes.end()) return false;
    // Its terminal closes, which hangs up the program, and the PTY's reaper
    // kills it when it stays.
    m_panes.erase(it);
    m_layout.remove(id);
    return true;
}

std::unique_ptr<MuxPane> Session::take(std::uint32_t id)
{
    const auto it = std::find_if(m_panes.begin(), m_panes.end(), [id](const auto& pane) { return pane->id() == id; });
    if (it == m_panes.end()) return nullptr;
    std::unique_ptr<MuxPane> pane = std::move(*it);
    m_panes.erase(it);
    m_layout.remove(id);
    return pane;
}

void Session::adopt(std::unique_ptr<MuxPane> pane)
{
    m_layout.addTab(pane->id());
    m_panes.push_back(std::move(pane));
}

void Session::kill()
{
    m_panes.clear();
    m_layout = {};
}

void Session::setLayout(Layout layout)
{
    layout.reconcile(paneIds());
    m_layout = std::move(layout);
}

std::vector<std::uint32_t> Session::paneIds() const
{
    std::vector<std::uint32_t> ids;
    for (const auto& pane : m_panes) ids.push_back(pane->id());
    return ids;
}

SessionInfo Session::info(pid_t caller) const
{
    SessionInfo info;
    info.name = m_name;
    info.panes = int(m_panes.size());
    for (const auto& pane : m_panes) {
        if (std::string job = pane->foregroundJob(caller); !job.empty() && std::find(info.jobs.begin(), info.jobs.end(), job) == info.jobs.end()) {
            info.jobs.push_back(std::move(job));
        }
    }
    return info;
}

} // namespace AfterglowMux
