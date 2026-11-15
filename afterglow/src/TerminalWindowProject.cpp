// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerminalWindow.h"
#include "Pane.h"
#include "ProjectStore.h"
#include "mux/MuxClient.h"
#include <QDir>
#include <QJsonArray>
#include <QSet>

namespace Afterglow {
namespace {
/// A project keeps titles this long (ProjectStore::validate); a longer
/// one would make the whole save fail.
constexpr qsizetype ProjectTitleLimit = 256;
}

QJsonObject TerminalWindow::projectLayout() const
{
    QJsonArray tabs;
    for (std::size_t i = 0; i < m_tabs.size(); ++i) {
        const auto& tab = *m_tabs[i];
        Pane* focused = int(i) == m_currentTab ? m_focused.data() : tab.focused.data();
        if (!focused) tab.splits->forEachPane([&](Pane* p, const QRect&) { if (!focused) focused = p; });
        const auto tree = tab.splits->snapshot([](Pane* p) {
            auto cwd = p->workingDirectory();
            if (cwd.isEmpty()) cwd = p->launchDirectory();
            if (cwd.isEmpty()) cwd = QDir::homePath();
            return QJsonObject{{QStringLiteral("id"), QString::number(p->id())}, {QStringLiteral("cwd"), cwd},
                               {QStringLiteral("title"), p->userTitle().left(ProjectTitleLimit)},
                               {QStringLiteral("command"), QJsonArray::fromStringList(p->launchCommand())}};
        });
        tabs.append(QJsonObject{{QStringLiteral("id"), QString::number(tab.id)}, {QStringLiteral("title"), tab.title.left(ProjectTitleLimit)},
                                {QStringLiteral("active_pane"), QString::number(focused->id())}, {QStringLiteral("tree"), tree}});
    }
    QJsonObject document{{QStringLiteral("version"), 1}, {QStringLiteral("active_tab"), QString::number(activeTabId())},
                         {QStringLiteral("tabs"), tabs}};
    // A window of a session of this computer: the project opens into its
    // session again.
    if (m_session && !m_session->remote()) document.insert(QStringLiteral("session"), true);
    return document;
}

QVariantMap TerminalWindow::restoreProject(const QJsonObject& document, const QList<Pane*>& started)
{
    // A window of a new session shows the panes that the session started
    // for the project, and no other: the project's tabs take their place.
    // An entry with no pane is a pane whose program ended before the window
    // opened: its place goes.
    const QList<Pane*> shown = panes();
    QList<Pane*> live = started;
    live.removeAll(nullptr);
    const bool empty = started.isEmpty() ? m_tabs.empty()
                                         : m_session && live.size() == shown.size()
                                               && QSet<Pane*>(live.begin(), live.end()) == QSet<Pane*>(shown.begin(), shown.end());
    if (!empty) return {{QStringLiteral("error"), QStringLiteral("Project destination must be empty")}};
    if (const auto error = ProjectStore::validate(document); !error.isEmpty())
        return {{QStringLiteral("error"), error}};
    if (!started.isEmpty() && started.size() != ProjectStore::panes(document).size())
        return {{QStringLiteral("error"), QStringLiteral("The session started %1 panes for a project of %2").arg(started.size()).arg(ProjectStore::panes(document).size())}};
    qsizetype next = 0;
    // Their trees go once the project's tabs are there: they do not own the panes.
    std::vector<std::unique_ptr<Tab>> previous = std::move(m_tabs);
    m_tabs.clear();
    m_splits = nullptr;
    QVariantMap paneIds, tabIds;
    QStringList warnings;
    QString failed;
    const auto tabs = document.value(QStringLiteral("tabs")).toArray();
    int active = 0;
    for (const auto tabValue : tabs) {
        const auto saved = tabValue.toObject();
        auto tab = std::make_unique<Tab>();
        tab->title = saved.value(QStringLiteral("title")).toString();
        tab->splits = SplitTree::restore(saved.value(QStringLiteral("tree")).toObject(), [&](const QJsonObject& data) -> Pane* {
            const auto savedId = data.value(QStringLiteral("id")).toString();
            const ProjectStore::Launch launch = ProjectStore::launchOf(data, warnings);
            // The session started the pane with this launch already.
            Pane* pane = started.isEmpty() ? nullptr : started.value(next++);
            if (!started.isEmpty() && !pane) {
                warnings << QStringLiteral("Pane %1: its program ended before the window opened").arg(savedId);
                return nullptr;
            }
            // A session that refused a pane refuses the rest: none is asked for.
            if (!pane && !failed.isEmpty()) return nullptr;
            if (!pane) {
                // A session pane ends with its program, as in any session.
                m_holdNextPane = !m_session && !launch.configured.isEmpty();
                QString failure;
                pane = createPane(launch.cwd, paneArea(), launch.command, &failure, {}, true);
                if (!pane) {
                    if (failed.isEmpty()) failed = QStringLiteral("Pane %1: %2").arg(savedId, failure);
                    return nullptr;
                }
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
    if (m_tabs.empty() && failed.isEmpty()) failed = QStringLiteral("The programs of all the panes of the project ended");
    if (!failed.isEmpty()) {
        // Only a session refuses a pane: the panes that it started end, and
        // the window shows what it showed.
        for (const auto& tab : m_tabs) {
            tab->splits->forEachPane([&](Pane* pane, const QRect&) {
                if (!pane || started.contains(pane)) return;
                pane->setSessionRelease(MuxRelease::Pane);
                pane->deleteLater();
            });
        }
        m_tabs = std::move(previous);
        finishTransfer();
        return {{QStringLiteral("error"), failed}};
    }
    previous.clear();
    m_currentTab = active;
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
