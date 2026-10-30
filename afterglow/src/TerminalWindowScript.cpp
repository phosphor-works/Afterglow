// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerminalWindow.h"
#include "chrome/ChromeController.h"
#include <algorithm>

namespace Afterglow {
QString TerminalWindow::showScriptDialog(const QString& owner, Pane* pane, const QVariantMap& effect, ScriptReply reply)
{
    if (m_scriptDialog || m_controller->wantsKeyboard()) return QStringLiteral("Another dialog is already open");
    if (!pane || tabOf(pane) < 0) return QStringLiteral("Pane closed");
    stopCopyMode(); cancelKeyMode(); stopUrlMode();
    if (tabOf(pane) != m_currentTab) switchTab(tabOf(pane));
    setFocusedPane(pane);
    m_scriptDialog = ScriptDialog{owner, pane, std::move(reply)};
    QVariantMap model = effect;
    model.insert(QStringLiteral("open"), true);
    m_controller->setScriptDialog(model);
    scheduleRender();
    return {};
}
void TerminalWindow::finishScriptDialog(bool accepted, const QString& value)
{
    if (!m_scriptDialog) return;
    const auto model = m_controller->scriptDialog();
    auto pending = std::move(*m_scriptDialog);
    m_scriptDialog.reset();
    m_controller->setScriptDialog({});
    QVariantMap data;
    QString error;
    if (!accepted || !pending.pane) error = QStringLiteral("Dialog cancelled");
    else if (model.value(QStringLiteral("kind")) == QLatin1String("select")) {
        for (const auto& item : model.value(QStringLiteral("items")).toList()) {
            if (item.toMap().value(QStringLiteral("id")).toString() == value) { data = item.toMap(); break; }
        }
        if (data.isEmpty()) error = QStringLiteral("Dialog cancelled");
    } else data.insert(QStringLiteral("text"), value.left(4096));
    if (pending.reply) pending.reply(error, data);
    scheduleRender();
}
QString TerminalWindow::setScriptAppearance(const QString& owner, Pane* pane, const QVariantMap& effect)
{
    if (!pane || tabOf(pane) < 0) return QStringLiteral("Pane closed");
    auto found = std::find_if(m_scriptAppearance.begin(), m_scriptAppearance.end(), [&](const auto& item) {
        return item.owner == owner && item.pane == pane;
    });
    if (found == m_scriptAppearance.end()) {
        if (m_scriptAppearance.size() >= 256) return QStringLiteral("Too many UI contributions");
        m_scriptAppearance.push_back({owner, pane, {}, {}, {}, 0});
        found = std::prev(m_scriptAppearance.end());
    }
    if (effect.value(QStringLiteral("kind")) == QLatin1String("override")) {
        found->values = effect.value(QStringLiteral("values")).toMap();
        found->order = ++m_scriptAppearanceOrder;
    } else {
        found->status = effect.value(QStringLiteral("text")).toString();
        found->tab = effect.value(QStringLiteral("tab_title")).toString();
    }
    if (found->values.isEmpty() && found->status.isEmpty() && found->tab.isEmpty()) m_scriptAppearance.erase(found);
    updateChrome(); scheduleRender();
    return {};
}
QVariantMap TerminalWindow::scriptOverrides(Pane* pane) const
{
    QVariantMap values;
    QHash<QString, quint64> order;
    for (const auto& item : m_scriptAppearance) {
        if (item.pane != pane) continue;
        for (auto it = item.values.begin(); it != item.values.end(); ++it) {
            if (item.order >= order.value(it.key())) { order[it.key()] = item.order; values[it.key()] = it.value(); }
        }
    }
    return values;
}
QString TerminalWindow::scriptShader(const QString& owner, Pane* pane, const QVariantMap& effect, QVariantMap& result)
{
    if (!pane || tabOf(pane) < 0) return QStringLiteral("Pane closed");
    if (effect.value(QStringLiteral("kind")) == QLatin1String("shaders")) {
        result.insert(QStringLiteral("shaders"), m_shaderControls.describe(pane->id()));
        return {};
    }
    const auto scope = effect.value(QStringLiteral("scope")).toString();
    QList<quint32> targets;
    QVariantList ids;
    for (Pane* p : panes()) {
        if (scope == QLatin1String("window") || (scope == QLatin1String("tab") && tabOf(p) == tabOf(pane))
            || (scope == QLatin1String("pane") && p == pane)) { targets.append(p->id()); ids.append(p->id()); }
    }
    const auto error = m_shaderControls.update(owner, targets, effect.value(QStringLiteral("instance")).toString(),
                                               scope, effect.value(QStringLiteral("values")).toMap());
    if (!error.isEmpty()) return error;
    result.insert(QStringLiteral("panes"), ids);
    scheduleRender();
    return {};
}
void TerminalWindow::clearScriptOwner(const QString& owner)
{
    m_shaderControls.clearOwner(owner);
    if (m_scriptDialog && m_scriptDialog->owner == owner) finishScriptDialog(false, {});
    std::erase_if(m_scriptAppearance, [&](const auto& item) { return item.owner == owner; });
    updateChrome(); scheduleRender();
}
}
