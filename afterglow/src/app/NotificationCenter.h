// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowVt/Notifications.h>

#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QStringList>

namespace Afterglow {

class Pane;
class TerminalWindow;

/// Desktop notifications through org.freedesktop.Notifications, for every
/// window: kitty's OSC 99 (updates by id, buttons, click and close reports,
/// expiry, icons, urgency, sounds) and plain ones. Clicking a notification
/// brings its pane forward.
class NotificationCenter : public QObject
{
    Q_OBJECT

public:
    static NotificationCenter& instance();

    /// The OSC 99 capability list afterglow answers queries with.
    static std::string capabilities();

    /// An OSC 99 request from @p pane. The window has already applied the
    /// request's occasion (always / unfocused / invisible).
    void handle(Pane* pane, TerminalWindow* window, const AfterglowVt::NotificationRequest& request);
    /// A plain notification (OSC 9, OSC 777, program status) for @p pane.
    void showSimple(Pane* pane, TerminalWindow* window, const QString& title, const QString& body, int urgency = 1);

private:
    NotificationCenter();

    struct Entry
    {
        QPointer<Pane> pane;
        QPointer<TerminalWindow> window;
        QString clientId; ///< the program's id, empty for none
        bool focusOnActivate = true;
        bool reportActivation = false;
        bool reportClose = false;
        bool closeRequested = false;
    };

    void notify(const Entry& entry, uint replaces, const QString& title, const QString& body, const QString& icon,
                const QStringList& actions, const QVariantMap& hints, int timeout, int expireMs);
    void close(uint id);
    uint idFor(const Pane* pane, const QString& clientId) const;
    bool rateLimited(const Pane* pane);

private Q_SLOTS:
    void onActionInvoked(uint id, const QString& action);
    void onClosed(uint id, uint reason);
    void onActivationToken(uint id, const QString& token);

private:
    QHash<uint, Entry> m_entries; ///< by D-Bus notification id
    QHash<const Pane*, QList<qint64>> m_recent; ///< recent show times, for rate limiting
    QHash<QString, QImage> m_iconCache; ///< OSC 99 'g' ids
    QStringList m_iconCacheOrder;
};

} // namespace Afterglow
