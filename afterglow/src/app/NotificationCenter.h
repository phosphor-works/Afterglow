// SPDX-FileCopyrightText: 2026 fuddlesworth
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../config/NotificationFilter.h"

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
#include <string>
#include <vector>

namespace Afterglow {

class Pane;

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

    /// Notifications that programs in one pane may show in @p windowMs
    /// (Notifications.rateWindow), @p count 0 for no limit
    /// (Notifications.rateLimit). Updates of a notification that is already
    /// shown do not count, nor do afterglow's own notices (showNotice: ctl
    /// and script notify among them). The notices of script command runs
    /// without a client (showLimitedNotice) count.
    void setRateLimit(int count, qint64 windowMs)
    {
        m_rateLimit = count;
        m_rateWindowMs = windowMs;
    }

    /// Notifications.filters: the rules of the notifications that programs
    /// send and that are not shown.
    void setFilters(std::vector<NotificationFilter> filters)
    {
        m_filters = std::move(filters);
    }

    /// An OSC 99 request from @p pane. The window has already applied the
    /// request's occasion (always / unfocused / invisible). Its icon goes in
    /// the 'g' cache first. Then Notifications.filters applies: a filtered
    /// update closes the notification that it would replace. Then the rate
    /// limit, which updates do not count against.
    void handle(Pane* pane, const AfterglowVt::NotificationRequest& request);
    /// A plain notification that a program in @p pane asked for (OSC 9,
    /// OSC 777, program status). Notifications.filters sees @p title as the
    /// program sent it; an empty one shows as the pane's title. Then it
    /// counts against the rate limit.
    void showSimple(Pane* pane, const QString& title, const QString& body, int urgency = 1);
    /// Afterglow's own notice about @p pane (which may be null): input
    /// rejected, a command finished, ctl and script notify, project and move
    /// results. Program output cannot use up its rate limit and hide it.
    void showNotice(Pane* pane, const QString& title, const QString& body, int urgency = 1);
    /// A notice that a script brings about, as often as it likes: the end
    /// of a command run that has no client. It counts against the rate
    /// limit of @p pane, or without a pane against one limit of its own.
    void showLimitedNotice(Pane* pane, const QString& title, const QString& body);
    /// The icons that m_iconCache keeps, the least recently used going first.
    static constexpr qsizetype MaxCachedIcons = 32;
    /// The bytes that m_iconCache keeps. It keeps the files as the program
    /// sent them (PNG, JPEG or GIF), not decoded images: eight of the
    /// largest that the terminal takes (4 MiB each).
    static constexpr std::size_t MaxCachedIconBytes = 32u * 1024 * 1024;

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
        std::string imageData; ///< decoded only when the call goes out: an update that waits may be replaced
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
    /// Counts a show in @p recent, the show times of one source. True when
    /// the source has used up the limit: the show is not counted then.
    bool overLimit(QList<qint64>& recent);
    void track(const Pane* pane);
    void forgetServer();
    void queryCapabilities();
    bool filtered(const NotificationFilter::Fields& fields) const;
    /// The icon of @p data, decoded once for a run of the same bytes.
    QImage decodedIcon(const std::string& data);

private Q_SLOTS:
    void onActionInvoked(uint id, const QString& action);
    void onClosed(uint id, uint reason);
    void onActivationToken(uint id, const QString& token);

private:
    QHash<uint, Entry> m_entries; ///< by D-Bus notification id
    QHash<Key, InFlight> m_inFlight;
    QHash<const Pane*, QList<qint64>> m_recent; ///< recent show times, for rate limiting
    QList<qint64> m_paneless; ///< recent shows of limited notices without a pane
    QSet<const Pane*> m_tracked; ///< panes whose destruction clears their state
    QElapsedTimer m_clock; ///< monotonic: a clock step must not silence a pane
    int m_rateLimit = 20; ///< until Application sets it from Notifications.rateLimit
    std::vector<NotificationFilter> m_filters;
    qint64 m_rateWindowMs = 10000; ///< until Application sets it from Notifications.rateWindow
    bool m_bodyMarkup = true; ///< the server reads markup in bodies (GetCapabilities)
    quint64 m_generation = 0;
    quint64 m_server = 0; ///< counts servers that went away: an id from an earlier one names nothing
    /// OSC 99 'g' ids and their bytes, decoded only for a notification that
    /// shows: one that a filter or the rate limit hides costs no decoding.
    QHash<QString, std::string> m_iconCache;
    QStringList m_iconCacheOrder;
    std::size_t m_iconCacheBytes = 0;
    std::string m_lastIconData; ///< the bytes of m_lastIcon
    QImage m_lastIcon;
};

} // namespace Afterglow
