// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <AfterglowVt/Notifications.h>

#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QVariantMap>

#include <optional>

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

    /// Notifications that programs in one pane may show in RateWindowMs, 0
    /// for no limit (Notifications.rateLimit). Updates of a notification that
    /// is already shown do not count, nor do afterglow's own notices.
    void setRateLimit(int count) { m_rateLimit = count; }
    static constexpr qint64 RateWindowMs = 10000;

    /// An OSC 99 request from @p pane. The window has already applied the
    /// request's occasion (always / unfocused / invisible).
    void handle(Pane* pane, TerminalWindow* window, const AfterglowVt::NotificationRequest& request);
    /// A plain notification that a program in @p pane asked for (OSC 9,
    /// OSC 777, program status). It counts against the rate limit.
    void showSimple(Pane* pane, TerminalWindow* window, const QString& title, const QString& body, int urgency = 1);
    /// Afterglow's own notice about @p pane (which may be null): input
    /// rejected, a command finished, ctl and script notify, project and move
    /// results. Program output cannot use up its rate limit and hide it.
    void showNotice(Pane* pane, const QString& title, const QString& body, int urgency = 1);

private:
    NotificationCenter();

    struct Entry
    {
        QPointer<Pane> pane;
        const Pane* owner = nullptr; ///< identity only, still valid in the pane's destroyed handler
        quint64 generation = 0;
        QString clientId; ///< the program's id, empty for none
        bool focusOnActivate = true;
        bool reportActivation = false;
        bool reportClose = false;
        QString activationToken; ///< from the server, for the click that follows it
    };

    /// One Notify call.
    struct Request
    {
        Entry entry;
        QString title;
        QString body; ///< plain text
        QString icon;
        QStringList actions;
        QVariantMap hints;
        int timeout = -1;
        int expireMs = -1;
    };

    /// A Notify for a program's id whose reply has not arrived: what the
    /// program asked for meanwhile waits for the server's id.
    struct InFlight
    {
        std::optional<Request> next; ///< the newest update
        bool close = false;
    };
    using Key = QPair<const Pane*, QString>;

    void send(const Request& request, uint replaces);
    void close(uint id);
    uint idFor(const Pane* pane, const QString& clientId) const;
    bool rateLimited(const Pane* pane);
    void track(const Pane* pane);
    void forgetServer();

private Q_SLOTS:
    void onActionInvoked(uint id, const QString& action);
    void onClosed(uint id, uint reason);
    void onActivationToken(uint id, const QString& token);

private:
    QHash<uint, Entry> m_entries; ///< by D-Bus notification id
    QHash<Key, InFlight> m_inFlight;
    QHash<const Pane*, QList<qint64>> m_recent; ///< recent show times, for rate limiting
    QSet<const Pane*> m_tracked; ///< panes whose destruction clears their state
    QElapsedTimer m_clock; ///< monotonic: a clock step must not silence a pane
    int m_rateLimit = 20;
    bool m_bodyMarkup = true; ///< the server reads markup in bodies (GetCapabilities)
    quint64 m_generation = 0;
    QHash<QString, QImage> m_iconCache; ///< OSC 99 'g' ids
    QStringList m_iconCacheOrder;
};

} // namespace Afterglow
