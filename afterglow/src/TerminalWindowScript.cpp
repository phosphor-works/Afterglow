// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later
#include "TerminalWindow.h"
#include "chrome/ChromeController.h"
#include <AfterglowRender/GpuDevice.h>
#include <QKeyEvent>
#include <QKeySequence>
#include <algorithm>
#include <cmath>

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
        m_shaderControls.advance();
        result.insert(QStringLiteral("shaders"), m_shaderControls.describe(pane->id()));
        return {};
    }
    const auto kind = effect.value(QStringLiteral("kind")).toString();
    if (kind == QLatin1String("scene_state")) {
        const qint64 now = std::chrono::duration_cast<std::chrono::milliseconds>(m_clock->now()).count();
        if (now - m_sceneSampleMs >= 100 || m_sceneSample.value(QStringLiteral("tab_id")).toUInt() != activeTabId()) {
            quint64 rows = 0;
            for (Pane* p : panes()) if (tabIdOf(p) == activeTabId())
                rows += p->withTerminal([](auto& t) { return t.primaryScreen().historySize(); });
            const auto& audio = m_device.audioLevels();
            m_sceneSample = {{QStringLiteral("tab_id"), activeTabId()}, {QStringLiteral("history_rows"), rows},
                {QStringLiteral("history_normalized"), double(rows) / (double(rows) + 5000.)},
                {QStringLiteral("sample_ms"), now}, {QStringLiteral("audio_available"), m_device.audioSpectrumSize() > 0},
                {QStringLiteral("bass"), audio[0]}, {QStringLiteral("mids"), audio[1]},
                {QStringLiteral("treble"), audio[2]}, {QStringLiteral("overall"), audio[3]}};
            m_sceneSampleMs = now;
        }
        result.insert(QStringLiteral("scene"), m_sceneSample);
        return {};
    }
    QVariantMap values = effect.value(QStringLiteral("values")).toMap();
    const auto instanceId = effect.value(QStringLiteral("instance")).toString();
    if (kind == QLatin1String("shader_presets") || kind == QLatin1String("shader_preset") || kind == QLatin1String("save_shader_preset")) {
        m_shaderControls.advance();
        QVariantMap instance;
        for (const auto& item : m_shaderControls.describe(pane->id()))
            if (item.toMap().value(QStringLiteral("instance")) == instanceId) { instance = item.toMap(); break; }
        if (instance.isEmpty()) return QStringLiteral("Shader instance expired. Query shaders again");
        const auto ref = instance.value(QStringLiteral("shader")).toString();
        if (kind == QLatin1String("shader_presets")) { result.insert(QStringLiteral("presets"), m_shaders.presets(ref)); return {}; }
        if (kind == QLatin1String("save_shader_preset"))
            return m_shaders.savePreset(ref, effect.value(QStringLiteral("name")).toString(), instance.value(QStringLiteral("values")).toMap(), result);
        const auto error = m_shaders.presetParameters(ref, effect.value(QStringLiteral("preset")).toString(), values);
        if (!error.isEmpty()) return error;
    }
    const auto scope = effect.value(QStringLiteral("scope")).toString();
    QList<quint32> targets;
    QVariantList ids;
    for (Pane* p : panes()) {
        if (scope == QLatin1String("window") || (scope == QLatin1String("tab") && tabOf(p) == tabOf(pane))
            || (scope == QLatin1String("pane") && p == pane)) { targets.append(p->id()); ids.append(p->id()); }
    }
    const auto error = m_shaderControls.update(owner, targets, effect.value(QStringLiteral("instance")).toString(),
                                               scope, values, effect.value(QStringLiteral("transition")).toMap());
    if (!error.isEmpty()) return error;
    result.insert(QStringLiteral("panes"), ids);
    scheduleRender();
    return {};
}
namespace {
/// A key press as a script's on_key sees it.
QVariantMap scriptKeyEvent(const QKeyEvent* e)
{
    static const QHash<int, QString> names{
        {Qt::Key_Left, QStringLiteral("Left")}, {Qt::Key_Right, QStringLiteral("Right")},
        {Qt::Key_Up, QStringLiteral("Up")}, {Qt::Key_Down, QStringLiteral("Down")},
        {Qt::Key_Return, QStringLiteral("Enter")}, {Qt::Key_Enter, QStringLiteral("Enter")},
        {Qt::Key_Escape, QStringLiteral("Escape")}, {Qt::Key_Tab, QStringLiteral("Tab")}, {Qt::Key_Backtab, QStringLiteral("Tab")},
        {Qt::Key_Backspace, QStringLiteral("Backspace")}, {Qt::Key_Delete, QStringLiteral("Delete")},
        {Qt::Key_Insert, QStringLiteral("Insert")}, {Qt::Key_Home, QStringLiteral("Home")}, {Qt::Key_End, QStringLiteral("End")},
        {Qt::Key_PageUp, QStringLiteral("PageUp")}, {Qt::Key_PageDown, QStringLiteral("PageDown")}, {Qt::Key_Space, QStringLiteral("Space")}};
    QString text = e->text();
    // Control characters (Ctrl+A) report the key, not the character.
    if (std::any_of(text.cbegin(), text.cend(), [](QChar c) { return c.category() == QChar::Other_Control; })) text.clear();
    QString key = names.value(e->key());
    if (key.isEmpty() && e->key() >= Qt::Key_F1 && e->key() <= Qt::Key_F35) key = QStringLiteral("F%1").arg(e->key() - Qt::Key_F1 + 1);
    if (key.isEmpty()) key = !text.isEmpty() ? text : QKeySequence(e->key()).toString(QKeySequence::PortableText).toLower();
    const auto mods = e->modifiers();
    return {{QStringLiteral("key"), key}, {QStringLiteral("text"), text},
            {QStringLiteral("ctrl"), bool(mods & Qt::ControlModifier)}, {QStringLiteral("shift"), bool(mods & Qt::ShiftModifier)},
            {QStringLiteral("alt"), bool(mods & Qt::AltModifier)}, {QStringLiteral("meta"), bool(mods & Qt::MetaModifier)},
            {QStringLiteral("repeat"), e->isAutoRepeat()}};
}
} // namespace

bool TerminalWindow::scriptKeyModeKey(QKeyEvent* e)
{
    if (!m_scriptKeyMode) return false;
    Pane* pane = m_scriptKeyMode->pane ? m_scriptKeyMode->pane.data() : m_focused.data();
    Q_EMIT scriptKey(m_scriptKeyMode->owner, pane ? pane->id() : 0, scriptKeyEvent(e));
    return true;
}

QString TerminalWindow::startScriptKeyMode(const QString& owner, Pane* pane, const QString& label)
{
    if (m_scriptKeyMode && m_scriptKeyMode->owner != owner) return QStringLiteral("Another script holds the keyboard");
    if (m_scriptDialog || m_controller->wantsKeyboard()) return QStringLiteral("A dialog has the keyboard");
    stopCopyMode(); cancelKeyMode(); stopUrlMode();
    m_scriptKeyMode = ScriptKeyMode{owner, label.left(160), pane};
    updateKeyMode();
    return {};
}

void TerminalWindow::endScriptKeyMode(const QString& owner)
{
    if (!m_scriptKeyMode || m_scriptKeyMode->owner != owner) return;
    m_scriptKeyMode.reset();
    updateKeyMode();
}

QVariantMap TerminalWindow::overviewState() const
{
    QVariantList cards;
    for (Pane* pane : overviewPanes()) cards.append(pane->id());
    return {{QStringLiteral("open"), m_overview.open},
            {QStringLiteral("highlighted"), m_overview.open && m_overview.highlighted ? m_overview.highlighted->id() : 0},
            {QStringLiteral("cards"), cards}};
}

QString TerminalWindow::scriptOverview(Pane* pane, const QString& operation, QVariantMap& result)
{
    if (operation == QLatin1String("open")) {
        if (!m_overview.open) {
            if (pane && pane != m_focused) revealPane(pane);
            if (!openOverview()) return QStringLiteral("The overview is not available");
        }
    } else if (operation == QLatin1String("close") || operation == QLatin1String("choose")) {
        if (!m_overview.open) return QStringLiteral("The overview is not open");
        closeOverview(operation == QLatin1String("choose") ? m_overview.highlighted.data() : nullptr);
    } else {
        return QStringLiteral("Overview operation must be open, close or choose");
    }
    result = overviewState();
    return {};
}

QString TerminalWindow::scriptOverviewMove(Pane*, Pane* target, int step, QVariantMap& result)
{
    if (!m_overview.open) return QStringLiteral("The overview is not open");
    const auto panes = overviewPanes();
    if (target) {
        const auto at = std::find(panes.begin(), panes.end(), target);
        if (at == panes.end()) return QStringLiteral("That pane is not in the overview");
        moveOverview(int(at - panes.begin()));
    } else if (step != 0) {
        const auto at = std::find(panes.begin(), panes.end(), m_overview.highlighted.data());
        const int current = at == panes.end() ? 0 : int(at - panes.begin());
        moveOverview(current + step, step > 0 ? 1 : -1);
    }
    result = overviewState();
    return {};
}

QString TerminalWindow::pulsePane(Pane* pane, int effect, int durationMs, int color)
{
    if (!pane || tabOf(pane) < 0) return QStringLiteral("Pane closed");
    if (!m_paneEffectsAvailable) return QStringLiteral("Shaders.paneEffects is off");
    m_panePulse.insert(pane, {std::chrono::steady_clock::now(), durationMs, effect, color});
    scheduleRender();
    return {};
}

void TerminalWindow::clearScriptOwner(const QString& owner)
{
    endScriptKeyMode(owner);
    m_shaderControls.clearOwner(owner);
    if (m_scriptDialog && m_scriptDialog->owner == owner) finishScriptDialog(false, {});
    std::erase_if(m_scriptAppearance, [&](const auto& item) { return item.owner == owner; });
    updateChrome(); scheduleRender();
}
}
