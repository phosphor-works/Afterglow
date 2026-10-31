// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerminalWindow.h"
#include "ProjectStore.h"
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QStandardPaths>

namespace Afterglow {
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
                               {QStringLiteral("title"), p->userTitle()},
                               {QStringLiteral("command"), QJsonArray::fromStringList(p->launchCommand())}};
        });
        tabs.append(QJsonObject{{QStringLiteral("id"), QString::number(tab.id)}, {QStringLiteral("title"), tab.title},
                                {QStringLiteral("active_pane"), QString::number(focused->id())}, {QStringLiteral("tree"), tree}});
    }
    return {{QStringLiteral("version"), 1}, {QStringLiteral("active_tab"), QString::number(activeTabId())},
            {QStringLiteral("tabs"), tabs}};
}

QVariantMap TerminalWindow::restoreProject(const QJsonObject& document)
{
    if (!m_tabs.empty()) return {{QStringLiteral("error"), QStringLiteral("Project destination must be empty")}};
    if (const auto error = ProjectStore::validate(document); !error.isEmpty())
        return {{QStringLiteral("error"), error}};
    QVariantMap paneIds, tabIds;
    QStringList warnings;
    const auto tabs = document.value(QStringLiteral("tabs")).toArray();
    int active = 0;
    for (const auto tabValue : tabs) {
        const auto saved = tabValue.toObject();
        auto tab = std::make_unique<Tab>();
        tab->title = saved.value(QStringLiteral("title")).toString();
        tab->splits = SplitTree::restore(saved.value(QStringLiteral("tree")).toObject(), [&](const QJsonObject& data) {
            const auto savedId = data.value(QStringLiteral("id")).toString();
            auto cwd = data.value(QStringLiteral("cwd")).toString();
            if (cwd == QLatin1String("~")) cwd = QDir::homePath();
            else if (cwd.startsWith(QLatin1String("~/"))) cwd = QDir::homePath() + cwd.mid(1);
            if (!QFileInfo(cwd).isDir()) {
                warnings << QStringLiteral("Pane %1: directory missing (%2). Using home").arg(savedId, cwd);
                cwd = QDir::homePath();
            }
            QStringList command;
            for (const auto arg : data.value(QStringLiteral("command")).toArray()) command << arg.toString();
            const auto configured = command;
            if (!command.isEmpty()) {
                const auto program = command.first();
                const auto resolved = program.contains(QLatin1Char('/')) ? QDir(cwd).absoluteFilePath(program)
                                                                        : QStandardPaths::findExecutable(program);
                if (resolved.isEmpty() || !QFileInfo(resolved).isFile() || !QFileInfo(resolved).isExecutable()) {
                    warnings << QStringLiteral("Pane %1: command unavailable (%2). Starting a shell").arg(savedId, program);
                    command.clear();
                }
            }
            m_holdNextPane = !configured.isEmpty();
            QString failure;
            auto* pane = createPane(cwd, paneArea(), command, &failure, {}, true);
            pane->setLaunch(cwd, configured); // retain explicit intent if the launch used a fallback
            pane->setUserTitle(data.value(QStringLiteral("title")).toString());
            if (!failure.isEmpty()) warnings << QStringLiteral("Pane %1: %2. Attempted /bin/sh recovery").arg(savedId, failure);
            paneIds.insert(savedId, pane->id());
            if (savedId == saved.value(QStringLiteral("active_pane")).toString()) tab->focused = pane;
            return pane;
        });
        const auto savedId = saved.value(QStringLiteral("id")).toString();
        tabIds.insert(savedId, tab->id);
        if (savedId == document.value(QStringLiteral("active_tab")).toString()) active = int(m_tabs.size());
        m_tabs.push_back(std::move(tab));
    }
    m_currentTab = active;
    finishTransfer();
    requestActivate();
    return {{QStringLiteral("window_id"), id()}, {QStringLiteral("pane_ids"), paneIds},
            {QStringLiteral("tab_ids"), tabIds}, {QStringLiteral("warnings"), warnings}, {QStringLiteral("reused"), false}};
}
} // namespace Afterglow
