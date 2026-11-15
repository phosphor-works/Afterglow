// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerminalWindow.h"
#include "PaneLaunch.h"
#include "ProjectStore.h"
#include "mux/MuxClient.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QSet>

#include <unordered_map>

namespace Afterglow {

QJsonObject TerminalWindow::projectLayout(QStringList* warnings) const
{
    // The directories of the session panes whose shells reported none here
    // come from the daemon in one call: one that does not answer makes the
    // GUI thread wait once, not once for each pane. A session of another
    // host has its directories there, not here.
    std::unordered_map<std::uint32_t, QString> daemonDirectories;
    if (m_session && !m_session->remote()) {
        std::vector<std::uint32_t> asked;
        for (Pane* p : panes()) {
            if (p->isSessionPane() && p->knownDirectory().isEmpty()) asked.push_back(p->sessionPaneId());
        }
        const std::vector<QString> answers = asked.empty() ? std::vector<QString>() : m_session->directories(asked);
        for (std::size_t i = 0; i < asked.size() && i < answers.size(); ++i) {
            if (!answers[i].isEmpty() && QFileInfo(answers[i]).isDir()) daemonDirectories.emplace(asked[i], answers[i]);
        }
    }
    QJsonArray tabs;
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
        const auto& tab = *m_tabs[i];
        Pane* focused = int(i) == m_currentTab ? m_focused.data() : tab.focused.data();
        if (!focused) tab.splits->forEachPane([&](Pane* p, const QRect&) { if (!focused) focused = p; });
        const auto tree = tab.splits->snapshot([&daemonDirectories, warnings](Pane* p) {
            QString cwd = p->knownDirectory();
            if (const auto asked = daemonDirectories.find(p->sessionPaneId()); cwd.isEmpty() && p->isSessionPane() && asked != daemonDirectories.end())
                cwd = asked->second;
            if (cwd.isEmpty()) cwd = p->launchDirectory();
            if (cwd.isEmpty()) cwd = QDir::homePath();
            // A command that a project cannot keep would fail the whole save
            // (ProjectStore::validate): the pane saves as the shell.
            const QStringList command = p->launchCommand();
            const bool kept = ProjectStore::keepsCommand(command);
            if (!kept && warnings) {
                *warnings << QStringLiteral("Pane %1: a project keeps a command of at most %2 arguments of %3 bytes each. Saved without its command")
                                 .arg(p->id())
                                 .arg(ProjectStore::MaxCommandArgs)
                                 .arg(ProjectStore::MaxCommandArgBytes);
            }
            return QJsonObject{{QStringLiteral("id"), QString::number(p->id())}, {QStringLiteral("cwd"), cwd},
                               {QStringLiteral("title"), textPrefix(p->userTitle(), ProjectStore::MaxTitle)},
                               {QStringLiteral("command"), QJsonArray::fromStringList(kept ? command : QStringList())}};
        });
        tabs.append(QJsonObject{{QStringLiteral("id"), QString::number(tab.id)}, {QStringLiteral("title"), textPrefix(tab.title, ProjectStore::MaxTitle)},
                                {QStringLiteral("active_pane"), QString::number(focused->id())}, {QStringLiteral("tree"), tree}});
    }
    QJsonObject document{{QStringLiteral("version"), 1}, {QStringLiteral("active_tab"), QString::number(activeTabId())},
                         {QStringLiteral("tabs"), tabs}};
    // A window of a session of this computer: the project opens into its
    // session again.
    if (m_session && !m_session->remote()) document.insert(QStringLiteral("session"), true);
    return document;
}

QVariantMap TerminalWindow::restoreProject(const QJsonObject& document, const QList<Pane*>& started, const QList<ProjectStore::Launch>& launches,
                                           const QStringList& launchWarnings)
{
    // A window of a new session shows the panes that the session started
    // for the project, and no other: the project's tabs take their place.
    // An entry with no pane is a pane whose program ended before the window
    // opened: its place goes.
    const QList<Pane*> shown = panes();
    QList<Pane*> live = started;
    live.removeAll(nullptr);
    // A session's window takes a project only with the panes that its
    // session started for it: the window starts none (createPane would ask
    // the daemon, which can refuse).
    const bool empty = started.isEmpty() ? m_tabs.empty() && !m_session
                                         : m_session && live.size() == shown.size()
                                               && QSet<Pane*>(live.begin(), live.end()) == QSet<Pane*>(shown.begin(), shown.end());
    if (!empty) return {{QStringLiteral("error"), QStringLiteral("Project destination must be empty")}};
    if (const auto error = ProjectStore::validate(document); !error.isEmpty())
        return {{QStringLiteral("error"), error}};
    const qsizetype count = ProjectStore::panes(document).size();
    if (!started.isEmpty() && started.size() != count)
        return {{QStringLiteral("error"), QStringLiteral("The session started %1 panes for a project of %2").arg(started.size()).arg(count)}};
    if (!launches.isEmpty() && launches.size() != count)
        return {{QStringLiteral("error"), QStringLiteral("%1 launches for a project of %2 panes").arg(launches.size()).arg(count)}};
    // Programs without a slash are looked for in the PATH that the panes get.
    const QStringList searchPath = launches.isEmpty() ? launchSearchPath(m_settings.settings()) : QStringList();
    // The place of the pane in ProjectStore::panes, which is the order of
    // the leaves here.
    qsizetype next = 0;
    // Their trees go once the project's tabs are there: they do not own the panes.
    std::vector<std::unique_ptr<Tab>> previous = std::move(m_tabs);
    m_tabs.clear();
    m_splits = nullptr;
    QVariantMap paneIds, tabIds;
    QStringList warnings = launchWarnings;
    const auto tabs = document.value(QStringLiteral("tabs")).toArray();
    int active = 0;
    for (const auto tabValue : tabs) {
        const auto saved = tabValue.toObject();
        auto tab = std::make_unique<Tab>();
        tab->title = saved.value(QStringLiteral("title")).toString();
        tab->splits = SplitTree::restore(saved.value(QStringLiteral("tree")).toObject(), [&](const QJsonObject& data) -> Pane* {
            const auto savedId = data.value(QStringLiteral("id")).toString();
            const qsizetype index = next++;
            const ProjectStore::Launch launch = launches.isEmpty() ? ProjectStore::launchOf(data, warnings, searchPath) : launches.value(index);
            // The session started the pane with this launch already.
            Pane* pane = started.isEmpty() ? nullptr : started.value(index);
            if (!started.isEmpty() && !pane) {
                warnings << QStringLiteral("Pane %1: its program ended before the window opened").arg(savedId);
                return nullptr;
            }
            if (!pane) {
                // A window that shows no session: with the fallback, createPane
                // leaves a held sh for a program that does not start, never null.
                m_holdNextPane = !launch.configured.isEmpty();
                QString failure;
                pane = createPane(launch.cwd, paneArea(), launch.command, &failure, {}, true);
                if (!failure.isEmpty()) warnings << QStringLiteral("Pane %1: %2. Attempted /bin/sh recovery").arg(savedId, failure);
            }
            pane->setLaunch(launch.cwd, launch.configured); // retain explicit intent if the launch used a fallback
            pane->setUserTitle(data.value(QStringLiteral("title")).toString());
            paneIds.insert(savedId, pane->id());
            if (savedId == saved.value(QStringLiteral("active_pane")).toString()) tab->focused = pane;
            return pane;
        });
        // A tab whose panes all ended goes.
        if (tab->splits->paneCount() == 0) continue;
        if (!tab->focused) tab->splits->forEachPane([&tab](Pane* pane, const QRect&) { if (!tab->focused) tab->focused = pane; });
        const auto savedId = saved.value(QStringLiteral("id")).toString();
        tabIds.insert(savedId, tab->id);
        if (savedId == document.value(QStringLiteral("active_tab")).toString()) active = int(m_tabs.size());
        m_tabs.push_back(std::move(tab));
    }
    if (m_tabs.empty()) {
        // Only the panes that a session started can be gone: the window
        // shows what it showed.
        m_tabs = std::move(previous);
        finishTransfer();
        return {{QStringLiteral("error"), QStringLiteral("The programs of all the panes of the project ended")}};
    }
    previous.clear();
    m_currentTab = active;
    // Each pane started at the whole area. A tab that does not show keeps
    // the sizes of its last layout until it shows: the split panes of the
    // others get their share of the area now, not twice their width.
    const QRect area = paneArea();
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
        if (int(i) == m_currentTab || m_tabs[i]->splits->paneCount() < 2) continue;
        m_tabs[i]->splits->layout(area, dividerPixels());
        m_tabs[i]->splits->forEachPane([this](Pane* pane, const QRect& rect) { layoutPane(pane, rect); });
    }
    finishTransfer();
    // The panes that the session started are in the project's tabs now.
    for (Pane* pane : started) {
        if (pane) Q_EMIT paneMoved(pane);
    }
    requestActivate();
    return {{QStringLiteral("window_id"), id()}, {QStringLiteral("pane_ids"), paneIds},
            {QStringLiteral("tab_ids"), tabIds}, {QStringLiteral("warnings"), warnings}, {QStringLiteral("reused"), false}};
}
} // namespace Afterglow
