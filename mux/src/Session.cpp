// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Session.h"

#include <algorithm>

namespace AfterglowMux {

Session::Session(std::string name, SessionOptions options, long fileLimit, ImageDecoder& images)
    : m_name(std::move(name))
    , m_options(std::move(options))
    , m_fileLimit(fileLimit)
    , m_images(images)
{
}

bool Session::addPane(std::uint32_t id, const std::string& daemonSocket, std::function<void()> wake, std::string* error)
{
    AfterglowPty::SpawnOptions spawn;
    spawn.argv = m_options.launch.argv;
    spawn.workingDirectory = m_options.launch.workingDirectory;
    spawn.environment = m_options.launch.environment;
    // The session's environment is the client's, which it sent whole: the
    // daemon's own (a systemd unit's INVOCATION_ID, for one) stays out. So
    // do the daemon's umask and its raised descriptor limit.
    spawn.inheritEnvironment = false;
    spawn.umask = m_options.launch.umask;
    spawn.fileLimit = m_fileLimit;
    // `afterglow ctl` and `aglow run` in a session pane reach the daemon,
    // which answers their calls with CALL_FAILED, also while a window shows
    // the session. The pane ID is the daemon's, whichever client sent it.
    AfterglowPty::setVariable(spawn, "AFTERGLOW_SOCKET", daemonSocket);
    AfterglowPty::setVariable(spawn, "AFTERGLOW_PANE_ID", std::to_string(id));
    AfterglowPty::setVariable(spawn, "AFTERGLOW_SESSION", m_name);
    AfterglowPty::unsetVariable(spawn, "AFTERGLOW_OVERLAY");
    auto pane = std::make_unique<MuxPane>(id, m_options.terminal, m_options.size, m_options.notificationCapabilities, m_images,
                                          std::move(wake));
    if (!pane->start(std::move(spawn), m_options.launch.launchedProgram, error)) return false;
    m_panes.push_back(std::move(pane));
    return true;
}

std::vector<std::unique_ptr<MuxPane>> Session::reap()
{
    std::vector<std::unique_ptr<MuxPane>> closed;
    for (auto& pane : m_panes) {
        if (pane->exited()) closed.push_back(std::move(pane));
    }
    std::erase(m_panes, nullptr);
    return closed;
}

void Session::kill()
{
    m_panes.clear();
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
