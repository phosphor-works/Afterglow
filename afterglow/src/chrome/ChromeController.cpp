// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ChromeController.h"

namespace Afterglow {

void ChromeController::setTabs(const QStringList& titles, int current, int barHeight)
{
    if (titles == m_tabTitles && current == m_currentTab && barHeight == m_tabBarHeight) {
        return;
    }
    m_tabTitles = titles;
    m_currentTab = current;
    m_tabBarHeight = barHeight;
    Q_EMIT tabsChanged();
}

void ChromeController::setTheme(const QColor& background, const QColor& foreground, const QColor& accent)
{
    if (background == m_background && foreground == m_foreground && accent == m_accent) {
        return;
    }
    m_background = background;
    m_foreground = foreground;
    m_accent = accent;
    Q_EMIT themeChanged();
}

void ChromeController::setActions(const QVariantList& actions)
{
    if (actions == m_actions) {
        return;
    }
    m_actions = actions;
    Q_EMIT actionsChanged();
}

void ChromeController::setPaletteOpen(bool open)
{
    if (open == m_paletteOpen) {
        return;
    }
    m_paletteOpen = open;
    if (open && m_searchOpen) {
        m_searchOpen = false;
        Q_EMIT searchOpenChanged();
    }
    Q_EMIT paletteOpenChanged();
}

void ChromeController::setSearchOpen(bool open)
{
    if (open == m_searchOpen) {
        return;
    }
    m_searchOpen = open;
    if (open && m_paletteOpen) {
        m_paletteOpen = false;
        Q_EMIT paletteOpenChanged();
    }
    Q_EMIT searchOpenChanged();
}

void ChromeController::setSearchStatus(const QString& status)
{
    if (status == m_searchStatus) {
        return;
    }
    m_searchStatus = status;
    Q_EMIT searchStatusChanged();
}

void ChromeController::setPaneTitles(const QVariantList& titles)
{
    if (titles == m_paneTitles) {
        return;
    }
    m_paneTitles = titles;
    Q_EMIT paneTitlesChanged();
}

void ChromeController::askPaste(const QString& preview, int lines)
{
    m_pasteConfirmOpen = true;
    m_pastePreview = preview;
    m_pasteLines = lines;
    Q_EMIT pasteConfirmChanged();
}

void ChromeController::answerPaste(bool paste)
{
    if (!m_pasteConfirmOpen) {
        return;
    }
    m_pasteConfirmOpen = false;
    Q_EMIT pasteConfirmChanged();
    Q_EMIT pasteAnswered(paste);
}

void ChromeController::runAction(int id)
{
    setPaletteOpen(false);
    Q_EMIT actionRequested(id);
}

} // namespace Afterglow
