// SPDX-FileCopyrightText: 2026 Phosphor Works Team
// SPDX-License-Identifier: GPL-3.0-or-later

#include "Session.h"

#include <algorithm>
#include <exception>
#include <map>
#include <set>
#include <string_view>
#include <utility>

namespace AfterglowMux {

namespace {

/// The environment of a pane: the session's, without what @p launch unsets,
/// then with what it sets, the last value of each name. Each list once: a
/// request line (1 MiB) can hold many variables.
std::vector<std::pair<std::string, std::string>>
paneEnvironment(std::vector<std::pair<std::string, std::string>> environment, const Launch& launch)
{
    std::set<std::string_view> dropped(launch.unset.begin(), launch.unset.end());
    std::map<std::string_view, std::size_t> last;
    for (std::size_t i = 0; i < launch.environment.size(); ++i) {
        dropped.insert(launch.environment[i].first);
        last[launch.environment[i].first] = i;
    }
    std::erase_if(environment, [&dropped](const auto& variable) {
        return dropped.contains(variable.first);
    });
    for (std::size_t i = 0; i < launch.environment.size(); ++i) {
        if (last[launch.environment[i].first] == i)
            environment.push_back(launch.environment[i]);
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
    // An agent session reads its text from the start (applySettings).
    if (m_options.agent)
        m_options.control.allowReadText = true;
}

bool Session::addPane(std::uint32_t id, const Launch& launch, const PaneSize& size, const std::string& daemonSocket,
                      std::uint64_t window, std::size_t windowQueue, std::function<void()> wake, std::string* error)
{
    AfterglowPty::SpawnOptions spawn;
    spawn.argv = launch.argv;
    spawn.workingDirectory = launch.workingDirectory;
    spawn.environment = paneEnvironment(m_options.environment, launch);
    // The session's environment is the client's, which it sent whole: the
    // daemon's own (a systemd unit's INVOCATION_ID, for one) stays out. So
    // do the daemon's umask and its raised descriptor limit. Before the
    // shell's name: the daemon's SHELL is not the session's.
    spawn.inheritEnvironment = false;
    if (m_options.callerEnvironment) {
        // The environment of a command over ssh, which no login read: the
        // shell reads the login's files, as an interactive ssh login does.
        if (spawn.argv.empty())
            spawn.argv = {AfterglowPty::Pty::defaultShell(spawn), "-l"};
        if (spawn.workingDirectory.empty()) {
            spawn.workingDirectory = "/";
            for (const auto& [name, value] : spawn.environment) {
                if (name == "HOME" && !value.empty())
                    spawn.workingDirectory = value;
            }
        }
    }
    spawn.umask = m_options.umask;
    spawn.fileLimit = m_fileLimit;
    // The programs write kitty's temporary files in their TMPDIR, which is
    // the client's, not the daemon's: /tmp when they have none.
    std::string temporaryDirectory = "/tmp";
    for (const auto& [name, value] : spawn.environment) {
        if (name == "TMPDIR" && !value.empty())
            temporaryDirectory = value;
    }
    // `afterglow ctl` and `aglow run` in a session pane reach the daemon: it
    // answers the control calls of the session, and passes a client of the
    // commands target to the window that shows the session. The pane ID is
    // the daemon's, whichever client sent it.
    AfterglowPty::setVariable(spawn, "AFTERGLOW_SOCKET", daemonSocket);
    AfterglowPty::setVariable(spawn, "AFTERGLOW_PANE_ID", std::to_string(id));
    AfterglowPty::setVariable(spawn, "AFTERGLOW_SESSION", m_name);
    AfterglowPty::unsetVariable(spawn, "AFTERGLOW_OVERLAY");
    auto pane = std::make_unique<MuxPane>(id, m_options.terminal, size, m_options.notificationCapabilities, m_images,
                                          std::move(wake));
    pane->setTemporaryDirectory(temporaryDirectory);
    pane->setKeptNotifications(std::size_t(m_options.keptNotifications));
    // Before the program runs: a bell or a clipboard request that it makes
    // at once waits for the window that asked for the pane.
    pane->setWindow(window, windowQueue);
    // The room for the pane before its program starts, and its tab before
    // it joins the panes: a failure of either ends the new program and
    // leaves the session as it was, each pane in the layout.
    m_panes.reserve(m_panes.size() + 1);
    // Before the program runs too: `afterglow ctl watch` hears its first
    // title.
    pane->setWatched(m_watched, m_watchText);
    if (!pane->start(std::move(spawn), launch.launchedProgram, error))
        return false;
    m_layout.addTab(id);
    m_panes.push_back(std::move(pane));
    // The next window saves the command in a project, as the client that
    // started the pane would: while the layout has room for it.
    std::size_t bytes = 0;
    for (const std::string& arg : launch.command)
        bytes += arg.size();
    if (!launch.command.empty() && m_layout.valueBytes() + bytes <= MaxLayoutValueBytes)
        m_layout.values[id].command = launch.command;
    return true;
}

std::vector<std::unique_ptr<MuxPane>> Session::reap()
{
    std::vector<std::unique_ptr<MuxPane>> closed;
    // Room for each pane first: a failure (out of memory) leaves every pane
    // in the session, and no null one.
    closed.reserve(m_panes.size());
    for (auto& pane : m_panes) {
        if (pane->exited()) {
            // The program told them before it exited: they wait whole.
            keepLeftEvents(*pane);
            m_layout.remove(pane->id());
            closed.push_back(std::move(pane));
        }
    }
    std::erase(m_panes, nullptr);
    return closed;
}

std::unique_ptr<MuxPane> Session::take(std::uint32_t id)
{
    const auto it = std::find_if(m_panes.begin(), m_panes.end(), [id](const auto& pane) {
        return pane->id() == id;
    });
    if (it == m_panes.end())
        return nullptr;
    // For closePane(), the caller drops it: its terminal closes, which hangs
    // up the program, and the PTY's reaper kills it when it stays.
    std::unique_ptr<MuxPane> pane = std::move(*it);
    m_panes.erase(it);
    m_layout.remove(id);
    // Another session takes it, which may not be watched: what it told while
    // it was here is this session's.
    keepLeftEvents(*pane);
    pane->setWatched(false, false);
    return pane;
}

void Session::keepLeftEvents(MuxPane& pane) noexcept
{
    if (!m_watched)
        return;
    try {
        std::vector<MuxPane::WatchEvent> events = pane.takeWatchEvents();
        if (!events.empty())
            m_left.push_back({pane.id(), std::move(events)});
    } catch (const std::exception&) {
        // The watch misses them: the pane still leaves whole.
    }
}

void Session::setWatched(bool watched, bool text)
{
    text = watched && text;
    if (watched == m_watched && text == m_watchText)
        return;
    if (!watched) {
        m_moved.clear();
        m_left.clear();
    }
    m_watched = watched;
    m_watchText = text;
    for (const auto& pane : m_panes)
        pane->setWatched(watched, text);
}

void Session::adopt(std::vector<std::unique_ptr<MuxPane>> panes, Layout layout) noexcept
{
    m_panes = std::move(panes);
    m_layout = std::move(layout);
    // The new session's watch: `afterglow ctl watch` of the old one hears
    // nothing more of them.
    for (const auto& pane : m_panes)
        pane->setWatched(m_watched, m_watchText);
}

void Session::join(std::unique_ptr<MuxPane> pane) noexcept
{
    // This session's watch, as for adopt(), and its bound of kept
    // notifications. The calls that the old window did not get yet are kept
    // when the daemon gives the pane this session's window
    // (MuxPane::setWindow), which then gets them, or the next one does.
    pane->setWatched(m_watched, m_watchText);
    pane->setKeptNotifications(std::size_t(m_options.keptNotifications));
    // And the answer to OSC 99's query of this session's window. Out of
    // memory, the pane keeps the old one: it still joins whole.
    try {
        pane->setNotificationCapabilities(m_options.notificationCapabilities);
    } catch (const std::exception&) {
    }
    m_panes.push_back(std::move(pane));
}

void Session::kill()
{
    m_panes.clear();
    m_layout = {};
}

void Session::applySettings(const SessionSettings& settings)
{
    m_options.control = settings.control;
    // An agent reads its session's text whatever a window says: the window
    // marks the session, and Control.enabled still stops the agent.
    if (m_options.agent)
        m_options.control.allowReadText = true;
    if (m_options.notificationCapabilities != settings.notificationCapabilities) {
        m_options.notificationCapabilities = settings.notificationCapabilities;
        for (const auto& pane : m_panes)
            pane->setNotificationCapabilities(settings.notificationCapabilities);
    }
    if (m_options.keptNotifications != settings.keptNotifications) {
        m_options.keptNotifications = settings.keptNotifications;
        for (const auto& pane : m_panes)
            pane->setKeptNotifications(std::size_t(settings.keptNotifications));
    }
    for (const auto& [name, value] : settings.environment) {
        std::erase_if(m_options.environment, [&name](const auto& variable) {
            return variable.first == name;
        });
        if (value)
            m_options.environment.emplace_back(name, *value);
    }
    if (m_options.terminal == settings.terminal)
        return;
    m_options.terminal = settings.terminal;
    for (const auto& pane : m_panes)
        pane->setOptions(settings.terminal);
}

void Session::noteMoved(std::uint32_t pane)
{
    if (m_watched && m_moved.size() < MaxSessionPanes)
        m_moved.push_back(pane);
}

void Session::setLayout(Layout layout)
{
    for (const std::uint32_t pane : std::exchange(layout.moved, {}))
        noteMoved(pane);
    keepCommands(layout, m_layout);
    // Those of panes that the layout does not hold go in reconcile().
    layout.reconcile(paneIds());
    m_layout = std::move(layout);
}

void Session::keepCommands(Layout& layout, const Layout& kept)
{
    // A window of a session of another host knows no command of its panes
    // (PaneValues::keepsCommand): each keeps the daemon's, while the values
    // have room, and the next window saves it in a project. A value with a
    // command, an empty one too, and a pane without values give theirs.
    std::size_t bytes = layout.valueBytes();
    for (auto& [pane, values] : layout.values) {
        if (!values.keepsCommand)
            continue;
        values.keepsCommand = false;
        const auto known = kept.values.find(pane);
        if (known == kept.values.end())
            continue;
        std::size_t size = 0;
        for (const std::string& arg : known->second.command)
            size += arg.size();
        if (bytes + size > MaxLayoutValueBytes)
            continue;
        values.command = known->second.command;
        bytes += size;
    }
    // No keepsCommand stays for the next window, and an entry with no title
    // and no command that it kept is no entry.
    std::erase_if(layout.values, [](const auto& entry) {
        return entry.second.empty();
    });
}

std::vector<std::uint32_t> Session::paneIds() const
{
    std::vector<std::uint32_t> ids;
    for (const auto& pane : m_panes)
        ids.push_back(pane->id());
    return ids;
}

SessionInfo Session::info(pid_t caller) const
{
    SessionInfo info;
    info.name = m_name;
    info.panes = int(m_panes.size());
    info.agent = m_options.agent;
    for (const auto& pane : m_panes) {
        if (std::string job = pane->foregroundJob(caller);
            !job.empty() && std::find(info.jobs.begin(), info.jobs.end(), job) == info.jobs.end()) {
            info.jobs.push_back(std::move(job));
        }
    }
    return info;
}

} // namespace AfterglowMux
