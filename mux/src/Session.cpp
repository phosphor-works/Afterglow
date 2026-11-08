// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Session.h"

#include <algorithm>

namespace AfterglowMux {

namespace {

/// Sets @p name to @p value, in place of the launch's own value or its unset.
void own(AfterglowPty::SpawnOptions& options, const std::string& name, const std::string& value)
{
    std::erase_if(options.environment, [&](const auto& entry) { return entry.first == name; });
    std::erase(options.unsetEnvironment, name);
    options.environment.emplace_back(name, value);
}

void unset(AfterglowPty::SpawnOptions& options, const std::string& name)
{
    std::erase_if(options.environment, [&](const auto& entry) { return entry.first == name; });
    std::erase(options.unsetEnvironment, name);
    options.unsetEnvironment.push_back(name);
}

} // namespace

Session::Session(std::string name, SessionOptions options)
    : m_name(std::move(name))
    , m_options(std::move(options))
{
    m_options.name = m_name;
}

MuxPane* Session::addPane(std::uint32_t id, const std::string& daemonSocket, std::function<void()> exited, std::string* error)
{
    AfterglowPty::SpawnOptions spawn;
    spawn.argv = m_options.launch.argv;
    spawn.workingDirectory = m_options.launch.workingDirectory;
    spawn.environment = m_options.launch.environment;
    spawn.unsetEnvironment = m_options.launch.unsetEnvironment;
    // The session's environment is the client's, which it sent whole: the
    // daemon's own (a systemd unit's INVOCATION_ID, for one) stays out.
    spawn.inheritEnvironment = false;
    // `afterglow ctl` and `aglow run` in a session pane reach the daemon,
    // which knows the pane by its daemon ID, whichever client is attached.
    own(spawn, "AFTERGLOW_SOCKET", daemonSocket);
    own(spawn, "AFTERGLOW_PANE_ID", std::to_string(id));
    own(spawn, "AFTERGLOW_SESSION", m_name);
    unset(spawn, "AFTERGLOW_OVERLAY");
    auto pane = std::make_unique<MuxPane>(id, m_options.terminal, m_options.size, std::move(exited));
    if (!pane->start(std::move(spawn), m_options.launch.launchedProgram, error)) return nullptr;
    m_panes.push_back(std::move(pane));
    return m_panes.back().get();
}

bool Session::reap()
{
    std::erase_if(m_panes, [](const auto& pane) { return pane->exited(); });
    return m_panes.empty();
}

void Session::kill()
{
    m_panes.clear();
}

SessionInfo Session::info() const
{
    SessionInfo info;
    info.name = m_name;
    info.windows = m_panes.empty() ? 0 : 1;
    info.panes = int(m_panes.size());
    for (const auto& pane : m_panes) {
        if (std::string job = pane->foregroundJob(); !job.empty() && std::find(info.jobs.begin(), info.jobs.end(), job) == info.jobs.end()) {
            info.jobs.push_back(std::move(job));
        }
    }
    return info;
}

} // namespace AfterglowMux
