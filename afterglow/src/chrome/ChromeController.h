// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QColor>
#include <QObject>
#include <QPointF>
#include <QStringList>
#include <QVariantList>

namespace Afterglow {

/// The state and requests the window chrome (tab bar, command palette,
/// search bar) binds to. Owned by a TerminalWindow, read by QML.
class ChromeController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList tabTitles READ tabTitles NOTIFY tabsChanged)
    Q_PROPERTY(QVariantList tabStatus READ tabStatus NOTIFY tabStatusChanged)
    Q_PROPERTY(QVariantList urlHints READ urlHints NOTIFY urlHintsChanged)
    Q_PROPERTY(QVariantMap unicodeInput READ unicodeInput NOTIFY unicodeInputChanged)
    Q_PROPERTY(QColor goodColor READ goodColor NOTIFY themeChanged)
    Q_PROPERTY(QColor warnColor READ warnColor NOTIFY themeChanged)
    Q_PROPERTY(QColor badColor READ badColor NOTIFY themeChanged)
    Q_PROPERTY(int currentTab READ currentTab NOTIFY tabsChanged)
    Q_PROPERTY(int tabBarHeight READ tabBarHeight NOTIFY tabsChanged)
    Q_PROPERTY(QColor background READ background NOTIFY themeChanged)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY themeChanged)
    Q_PROPERTY(QColor accent READ accent NOTIFY themeChanged)
    Q_PROPERTY(QVariantList actions READ actions NOTIFY actionsChanged)
    Q_PROPERTY(bool paletteOpen READ paletteOpen WRITE setPaletteOpen NOTIFY paletteOpenChanged)
    Q_PROPERTY(bool searchOpen READ searchOpen WRITE setSearchOpen NOTIFY searchOpenChanged)
    Q_PROPERTY(QString searchStatus READ searchStatus NOTIFY searchStatusChanged)
    Q_PROPERTY(QVariantList paneTitles READ paneTitles NOTIFY paneTitlesChanged)
    Q_PROPERTY(bool pasteConfirmOpen READ pasteConfirmOpen NOTIFY pasteConfirmChanged)
    Q_PROPERTY(QString pastePreview READ pastePreview NOTIFY pasteConfirmChanged)
    Q_PROPERTY(int pasteLines READ pasteLines NOTIFY pasteConfirmChanged)
    Q_PROPERTY(bool clipboardPromptOpen READ clipboardPromptOpen NOTIFY clipboardPromptChanged)
    Q_PROPERTY(QString clipboardPromptText READ clipboardPromptText NOTIFY clipboardPromptChanged)
    Q_PROPERTY(QString clipboardPromptName READ clipboardPromptName NOTIFY clipboardPromptChanged)
    Q_PROPERTY(QVariantMap contextMenu READ contextMenu NOTIFY contextMenuChanged)
    Q_PROPERTY(QVariantMap titlePrompt READ titlePrompt NOTIFY titlePromptChanged)

public:
    using QObject::QObject;

    QStringList tabTitles() const { return m_tabTitles; }
    int currentTab() const { return m_currentTab; }
    int tabBarHeight() const { return m_tabBarHeight; }
    void setTabs(const QStringList& titles, int current, int barHeight);
    /// Per tab: { state, progress, text } of its most pressing program status,
    /// or an empty map.
    QVariantList tabStatus() const { return m_tabStatus; }
    /// URL mode: [{ x, y (logical pixels), label, typed (characters matched) }].
    QVariantList urlHints() const { return m_urlHints; }
    void setUrlHints(const QVariantList& hints);
    /// Unicode input: { active, text (hex typed), preview, x, y }.
    QVariantMap unicodeInput() const { return m_unicodeInput; }
    void setUnicodeInput(const QVariantMap& state);
    void setTabStatus(const QVariantList& status);

    QColor background() const { return m_background; }
    QColor foreground() const { return m_foreground; }
    QColor accent() const { return m_accent; }
    void setTheme(const QColor& background, const QColor& foreground, const QColor& accent);
    QColor goodColor() const { return m_goodColor; }
    QColor warnColor() const { return m_warnColor; }
    QColor badColor() const { return m_badColor; }
    void setStatusColors(const QColor& good, const QColor& warn, const QColor& bad);

    /// [{ id: int, title: string, shortcut: string }]
    QVariantList actions() const { return m_actions; }
    void setActions(const QVariantList& actions);

    bool paletteOpen() const { return m_paletteOpen; }
    void setPaletteOpen(bool open);
    bool searchOpen() const { return m_searchOpen; }
    void setSearchOpen(bool open);
    QString searchStatus() const { return m_searchStatus; }
    void setSearchStatus(const QString& status);

    /// [{ x, y, width, height (logical pixels), title, focused }] per split.
    QVariantList paneTitles() const { return m_paneTitles; }
    void setPaneTitles(const QVariantList& titles);

    bool pasteConfirmOpen() const { return m_pasteConfirmOpen; }
    QString pastePreview() const { return m_pastePreview; }
    int pasteLines() const { return m_pasteLines; }
    /// Asks whether to paste text a shell would run line by line.
    void askPaste(const QString& preview, int lines);

    /// True while the chrome takes keyboard input instead of the terminal.
    bool clipboardPromptOpen() const { return m_clipboardPromptOpen; }
    QString clipboardPromptText() const { return m_clipboardPromptText; }
    /// The program's name when it can be remembered, else empty.
    QString clipboardPromptName() const { return m_clipboardPromptName; }
    /// Asks whether a program may read the clipboard.
    void askClipboard(const QString& text, const QString& name);
    /// The context menu: { open, x, y (logical pixels, where it was asked
    /// for), items: [{ id, title, shortcut, enabled } or { separator: true }] }.
    QVariantMap contextMenu() const { return m_contextMenu; }
    bool contextMenuOpen() const { return m_contextMenu.value(QStringLiteral("open")).toBool(); }
    void openContextMenu(const QPointF& at, const QVariantList& items);
    Q_INVOKABLE void closeContextMenu();
    /// Runs a menu item and closes the menu.
    Q_INVOKABLE void activateMenuItem(int id);

    /// Asks for a title: { open, heading, text (the current one) }.
    QVariantMap titlePrompt() const { return m_titlePrompt; }
    bool titlePromptOpen() const { return m_titlePrompt.value(QStringLiteral("open")).toBool(); }
    void askTitle(const QString& heading, const QString& current);
    /// Empty returns to the program's title.
    Q_INVOKABLE void answerTitle(const QString& title);
    Q_INVOKABLE void cancelTitle();

    bool wantsKeyboard() const
    {
        return m_paletteOpen || m_searchOpen || m_pasteConfirmOpen || m_clipboardPromptOpen || contextMenuOpen()
            || titlePromptOpen();
    }

    Q_INVOKABLE void activateTab(int index) { Q_EMIT activateTabRequested(index); }
    Q_INVOKABLE void closeTab(int index) { Q_EMIT closeTabRequested(index); }
    Q_INVOKABLE void newTab() { Q_EMIT newTabRequested(); }
    /// Moves the tab at @p from to @p to (the indexes as shown).
    Q_INVOKABLE void moveTab(int from, int to) { Q_EMIT moveTabRequested(from, to); }
    Q_INVOKABLE void renameTab(int index) { Q_EMIT renameTabRequested(index); }
    /// The tab's menu, at @p x, @p y (logical pixels).
    Q_INVOKABLE void tabMenu(int index, qreal x, qreal y) { Q_EMIT tabMenuRequested(index, QPointF(x, y)); }
    Q_INVOKABLE void runAction(int id);
    /// @p direction: 0 to search again from the current match, 1 next, -1 previous.
    Q_INVOKABLE void search(const QString& query, int direction) { Q_EMIT searchRequested(query, direction); }
    Q_INVOKABLE void answerPaste(bool paste);
    /// 0 deny, 1 allow once, 2 always allow this program.
    Q_INVOKABLE void answerClipboard(int answer);

Q_SIGNALS:
    void tabsChanged();
    void tabStatusChanged();
    void urlHintsChanged();
    void unicodeInputChanged();
    void clipboardPromptChanged();
    void clipboardAnswered(int answer);
    void themeChanged();
    void actionsChanged();
    void paletteOpenChanged();
    void searchOpenChanged();
    void searchStatusChanged();
    void pasteConfirmChanged();
    void paneTitlesChanged();
    void pasteAnswered(bool paste);
    void contextMenuChanged();
    void titlePromptChanged();
    void titleAnswered(const QString& title);
    void menuItemActivated(int id);

    void activateTabRequested(int index);
    void closeTabRequested(int index);
    void newTabRequested();
    void moveTabRequested(int from, int to);
    void renameTabRequested(int index);
    void tabMenuRequested(int index, const QPointF& at);
    void actionRequested(int id);
    void searchRequested(const QString& query, int direction);

private:
    QStringList m_tabTitles;
    QVariantList m_tabStatus;
    QVariantList m_urlHints;
    QVariantMap m_unicodeInput;
    bool m_clipboardPromptOpen = false;
    QString m_clipboardPromptText;
    QString m_clipboardPromptName;
    QColor m_goodColor{Qt::green};
    QColor m_warnColor{Qt::yellow};
    QColor m_badColor{Qt::red};
    int m_currentTab = 0;
    int m_tabBarHeight = 0;
    QColor m_background{0x1c, 0x1c, 0x1c};
    QColor m_foreground{0xd8, 0xd8, 0xd8};
    QColor m_accent{0x6c, 0x9e, 0xf8};
    QVariantList m_actions;
    bool m_paletteOpen = false;
    bool m_searchOpen = false;
    QString m_searchStatus;
    QVariantList m_paneTitles;
    bool m_pasteConfirmOpen = false;
    QString m_pastePreview;
    int m_pasteLines = 0;
    QVariantMap m_contextMenu;
    QVariantMap m_titlePrompt;
};

} // namespace Afterglow
