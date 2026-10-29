// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QObject>
#include <QStringList>
#include <QVariantList>

namespace Afterglow {

/// The state and requests the window chrome (tab bar, command palette,
/// search bar) binds to. Owned by a TerminalWindow, read by QML.
class ChromeController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList tabTitles READ tabTitles NOTIFY tabsChanged)
    Q_PROPERTY(int currentTab READ currentTab NOTIFY tabsChanged)
    Q_PROPERTY(int tabBarHeight READ tabBarHeight NOTIFY tabsChanged)
    Q_PROPERTY(QColor background READ background NOTIFY themeChanged)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY themeChanged)
    Q_PROPERTY(QColor accent READ accent NOTIFY themeChanged)
    Q_PROPERTY(QVariantList actions READ actions NOTIFY actionsChanged)
    Q_PROPERTY(bool paletteOpen READ paletteOpen WRITE setPaletteOpen NOTIFY paletteOpenChanged)
    Q_PROPERTY(bool searchOpen READ searchOpen WRITE setSearchOpen NOTIFY searchOpenChanged)
    Q_PROPERTY(QString searchStatus READ searchStatus NOTIFY searchStatusChanged)
    Q_PROPERTY(bool pasteConfirmOpen READ pasteConfirmOpen NOTIFY pasteConfirmChanged)
    Q_PROPERTY(QString pastePreview READ pastePreview NOTIFY pasteConfirmChanged)
    Q_PROPERTY(int pasteLines READ pasteLines NOTIFY pasteConfirmChanged)

public:
    using QObject::QObject;

    QStringList tabTitles() const { return m_tabTitles; }
    int currentTab() const { return m_currentTab; }
    int tabBarHeight() const { return m_tabBarHeight; }
    void setTabs(const QStringList& titles, int current, int barHeight);

    QColor background() const { return m_background; }
    QColor foreground() const { return m_foreground; }
    QColor accent() const { return m_accent; }
    void setTheme(const QColor& background, const QColor& foreground, const QColor& accent);

    /// [{ id: int, title: string, shortcut: string }]
    QVariantList actions() const { return m_actions; }
    void setActions(const QVariantList& actions);

    bool paletteOpen() const { return m_paletteOpen; }
    void setPaletteOpen(bool open);
    bool searchOpen() const { return m_searchOpen; }
    void setSearchOpen(bool open);
    QString searchStatus() const { return m_searchStatus; }
    void setSearchStatus(const QString& status);

    bool pasteConfirmOpen() const { return m_pasteConfirmOpen; }
    QString pastePreview() const { return m_pastePreview; }
    int pasteLines() const { return m_pasteLines; }
    /// Asks whether to paste text a shell would run line by line.
    void askPaste(const QString& preview, int lines);

    /// True while the chrome takes keyboard input instead of the terminal.
    bool wantsKeyboard() const { return m_paletteOpen || m_searchOpen || m_pasteConfirmOpen; }

    Q_INVOKABLE void activateTab(int index) { Q_EMIT activateTabRequested(index); }
    Q_INVOKABLE void closeTab(int index) { Q_EMIT closeTabRequested(index); }
    Q_INVOKABLE void newTab() { Q_EMIT newTabRequested(); }
    Q_INVOKABLE void runAction(int id);
    /// @p direction: 0 to search again from the current match, 1 next, -1 previous.
    Q_INVOKABLE void search(const QString& query, int direction) { Q_EMIT searchRequested(query, direction); }
    Q_INVOKABLE void answerPaste(bool paste);

Q_SIGNALS:
    void tabsChanged();
    void themeChanged();
    void actionsChanged();
    void paletteOpenChanged();
    void searchOpenChanged();
    void searchStatusChanged();
    void pasteConfirmChanged();
    void pasteAnswered(bool paste);

    void activateTabRequested(int index);
    void closeTabRequested(int index);
    void newTabRequested();
    void actionRequested(int id);
    void searchRequested(const QString& query, int direction);

private:
    QStringList m_tabTitles;
    int m_currentTab = 0;
    int m_tabBarHeight = 0;
    QColor m_background{0x1c, 0x1c, 0x1c};
    QColor m_foreground{0xd8, 0xd8, 0xd8};
    QColor m_accent{0x6c, 0x9e, 0xf8};
    QVariantList m_actions;
    bool m_paletteOpen = false;
    bool m_searchOpen = false;
    QString m_searchStatus;
    bool m_pasteConfirmOpen = false;
    QString m_pastePreview;
    int m_pasteLines = 0;
};

} // namespace Afterglow
